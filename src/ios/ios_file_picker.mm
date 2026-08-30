/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "ios_file_picker.h"

#include <base/log.h>

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <PhotosUI/PhotosUI.h>

// ---------------------------------------------------------------------------
// How this works
//
// DDNet runs its whole game loop on the iOS main thread (SDL runs SDL_main
// inside application:didFinishLaunching) and never hands the main run loop
// back to the system - the run loop is only serviced in tiny two
// microsecond bursts from SDL's UIKit_PumpEvents, deep inside SDL_PollEvent.
//
// That is enough for plain local UIKit controls (the source-selection alert
// worked fine), but it is NOT enough for system pickers: their interfaces
// are driven by other iOS processes and talk to the host app over XPC,
// which requires a properly serviced main run loop. With the old approach
// the Photos picker showed no confirm button and ignored Cancel, and
// picking a file in the Files picker crashed the app inside the XPC
// completion machinery.
//
// So while an import dialog is on screen we run the real main run loop
// ourselves - the classic nested "modal" run loop. IosPickImageFile blocks
// the calling game frame until the user is done, which is fine: it is
// called from a settings-menu button handler, the dialog covers the
// screen and no game code can run in parallel on the same thread. When the
// dialog finishes, the image is converted and the callback is invoked
// directly - in exactly the same (game frame) context in which every other
// menu button handler runs.
//
// The Photos picker is UIImagePickerController (fully in-process UIKit,
// battle tested since the first iPhone) instead of the remote
// PHPickerViewController, the Files picker stays UIDocumentPickerViewController.
//
// This file is compiled with -fobjc-arc (see CMakeLists.txt).

// ------------------------------------------------------------------
// Modal session state (main thread only)
// ------------------------------------------------------------------

static const NSTimeInterval IosModalTimeoutSeconds = 10 * 60;

enum EIosImportChoice
{
	IOS_IMPORT_CHOICE_NONE,
	IOS_IMPORT_CHOICE_PHOTOS,
	IOS_IMPORT_CHOICE_FILES,
	IOS_IMPORT_CHOICE_CANCEL,
};

static BOOL s_ModalDone = NO;
static int s_ImportChoice = IOS_IMPORT_CHOICE_NONE;
static NSURL *s_pPickedUrl = nil;
static UIImage *s_pPickedImage = nil;

// Runs the real main run loop until s_ModalDone is set. Returns NO if the
// safety timeout expired (in that case everything still on screen is
// dismissed so the game stays usable).
static BOOL IosRunMainRunLoopUntilDone(UIViewController *pRoot)
{
	s_ModalDone = NO;
	const NSTimeInterval Deadline = [NSDate timeIntervalSinceReferenceDate] + IosModalTimeoutSeconds;
	while(!s_ModalDone)
	{
		if([NSDate timeIntervalSinceReferenceDate] >= Deadline)
		{
			log_error("ios", "Import dialog timed out");
			[pRoot dismissViewControllerAnimated:NO completion:nil];
			return NO;
		}
		// One pass through the run loop: processes every pending source
		// (touches, XPC replies, main queue blocks) and blocks for at
		// most a second before the condition is re-checked.
		[[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
			beforeDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
	}
	// Give UIKit a moment to finish in-flight transitions.
	[[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
		beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
	return YES;
}

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

static UIWindow *IosFindGameWindow()
{
	UIWindow *pKeyWindow = nil;
	for(UIScene *pScene in UIApplication.sharedApplication.connectedScenes)
	{
		if(![pScene isKindOfClass:[UIWindowScene class]])
			continue;
		UIWindowScene *pWindowScene = (UIWindowScene *)pScene;
		for(UIWindow *pWindow in pWindowScene.windows)
		{
			if(pWindow.isKeyWindow)
			{
				pKeyWindow = pWindow;
				break;
			}
		}
		if(pKeyWindow)
			break;
	}
	// Fallback for older iOS versions / edge cases where no window is
	// reported as "key" yet.
	if(!pKeyWindow && UIApplication.sharedApplication.windows.count > 0)
	{
		pKeyWindow = UIApplication.sharedApplication.windows[0];
	}
	return pKeyWindow;
}

// Downscales the image so its longest side is at most MaxDimension pixels.
// Camera photos can be 12-48 MP; re-encoding those at full size allocates
// hundreds of megabytes, so imports are capped at a sane size.
static UIImage *IosDownscaleImage(UIImage *pImage, CGFloat MaxDimension)
{
	const CGFloat Width = pImage.size.width;
	const CGFloat Height = pImage.size.height;
	if(Width <= MaxDimension && Height <= MaxDimension)
	{
		return pImage;
	}
	const CGFloat Scale = MaxDimension / (Width > Height ? Width : Height);
	const CGSize NewSize = CGSizeMake(floor(Width * Scale), floor(Height * Scale));
	UIGraphicsImageRendererFormat *pFormat = [[UIGraphicsImageRendererFormat alloc] init];
	pFormat.scale = 1;
	UIGraphicsImageRenderer *pRenderer = [[UIGraphicsImageRenderer alloc] initWithSize:NewSize format:pFormat];
	UIImage *pScaled = [pRenderer imageWithActions:^(UIGraphicsImageRendererContext *pContext)
	{
		(void)pContext;
		[pImage drawInRect:CGRectMake(0, 0, NewSize.width, NewSize.height)];
	}];
	log_info("ios", "Downscaled imported image from %dx%d to %dx%d",
		(int)Width, (int)Height, (int)NewSize.width, (int)NewSize.height);
	return pScaled;
}

// Writes the image into the app's temporary directory as a PNG file and
// returns its path. The game's image loader only supports PNG, so every
// import path that needs re-encoding funnels through here.
static std::string IosWriteImageAsPng(UIImage *pImage, const char *pBaseName)
{
	const long long TimeMs = (long long)([[NSDate date] timeIntervalSince1970] * 1000.0);
	NSString *pFileName = [NSString stringWithFormat:@"%s_%lld.png", pBaseName, TimeMs];
	NSString *pPath = [NSTemporaryDirectory() stringByAppendingPathComponent:pFileName];
	NSData *pData = UIImagePNGRepresentation(pImage);
	if(pData == nil || ![pData writeToFile:pPath atomically:YES])
	{
		log_error("ios", "Failed to write imported image to '%s'", pPath.UTF8String);
		return std::string();
	}
	return std::string(pPath.UTF8String);
}

// Copies an already-PNG file into our own temporary directory without
// decoding it: a plain file copy, no memory spike regardless of file size.
static std::string IosCopyPngFile(NSURL *pUrl)
{
	const long long TimeMs = (long long)([[NSDate date] timeIntervalSince1970] * 1000.0);
	NSString *pDestPath = [NSTemporaryDirectory() stringByAppendingPathComponent:
		[NSString stringWithFormat:@"import_%lld.png", TimeMs]];
	NSError *pError = nil;
	if(![[NSFileManager defaultManager] copyItemAtURL:pUrl toURL:[NSURL fileURLWithPath:pDestPath] error:&pError])
	{
		log_error("ios", "Failed to copy picked file: %s",
			pError != nil ? pError.localizedDescription.UTF8String : "unknown error");
		return std::string();
	}
	return std::string(pDestPath.UTF8String);
}

// Converts a file that came from the Files picker. The document picker
// (used with asCopy:YES) hands us a plain readable copy of the picked file
// inside our tmp directory - no security-scoped resource bookkeeping is
// needed.
static std::string IosConvertImportedFile(NSURL *pUrl)
{
	NSString *pPath = pUrl.path;
	NSString *pExt = [[pPath pathExtension] lowercaseString];
	if([pExt isEqualToString:@"png"])
	{
		return IosCopyPngFile(pUrl);
	}
	UIImage *pImage = [UIImage imageWithContentsOfFile:pPath];
	if(pImage == nil)
	{
		log_error("ios", "Failed to decode picked file '%s' as an image", pPath.UTF8String);
		return std::string();
	}
	pImage = IosDownscaleImage(pImage, 2048.0);
	return IosWriteImageAsPng(pImage, "import");
}

// Converts an image that came from the Photos picker (UIImage, possibly
// huge) into a downscaled PNG.
static std::string IosConvertImportedImage(UIImage *pImage)
{
	pImage = IosDownscaleImage(pImage, 2048.0);
	return IosWriteImageAsPng(pImage, "photo");
}

// ------------------------------------------------------------------
// Files app picker (UIDocumentPickerViewController)
// ------------------------------------------------------------------

@interface CIosFilePickerDelegate : NSObject <UIDocumentPickerDelegate>
@end

@implementation CIosFilePickerDelegate

// The delegate callbacks run inside our own run loop, i.e. in a completely
// normal UIKit context - dismissing the picker from here is the standard
// pattern. They only record the result and end the modal loop; the actual
// import happens after the loop, in the game frame.

- (void)documentPicker:(UIDocumentPickerViewController *)pController didPickDocumentsAtURLs:(NSArray<NSURL *> *)pUrls
{
	log_info("ios", "Files picker: %d document(s) selected", (int)pUrls.count);
	s_pPickedUrl = pUrls.count > 0 ? pUrls[0] : nil;
	[pController dismissViewControllerAnimated:NO completion:nil];
	s_ModalDone = YES;
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)pController
{
	log_info("ios", "Files picker: cancelled");
	s_pPickedUrl = nil;
	[pController dismissViewControllerAnimated:NO completion:nil];
	s_ModalDone = YES;
}

@end

// ------------------------------------------------------------------
// Photos gallery picker (UIImagePickerController, fully in-process)
// ------------------------------------------------------------------

@interface CIosPhotoPickerDelegate : NSObject <UIImagePickerControllerDelegate, UINavigationControllerDelegate>
@end

@implementation CIosPhotoPickerDelegate

- (void)imagePickerController:(UIImagePickerController *)pController didFinishPickingMediaWithInfo:(NSDictionary<UIImagePickerControllerInfoKey, id> *)pInfo
{
	log_info("ios", "Photos picker: image picked");
	s_pPickedImage = pInfo[UIImagePickerControllerOriginalImage];
	[pController dismissViewControllerAnimated:NO completion:nil];
	s_ModalDone = YES;
}

- (void)imagePickerControllerDidCancel:(UIImagePickerController *)pController
{
	log_info("ios", "Photos picker: cancelled");
	s_pPickedImage = nil;
	[pController dismissViewControllerAnimated:NO completion:nil];
	s_ModalDone = YES;
}

@end

// Keeps the delegates alive while the pickers are on screen (the pickers
// hold their delegates weakly).
static CIosFilePickerDelegate *s_pFilePickerDelegate = nil;
static CIosPhotoPickerDelegate *s_pPhotoPickerDelegate = nil;

// ------------------------------------------------------------------
// Import flow
// ------------------------------------------------------------------

void IosPickImageFile(std::function<void(const std::string &Path)> Callback)
{
	// Must be called from the game frame on the main thread - the whole
	// flow is synchronous and delivers the callback in the caller's
	// context.
	if(![NSThread isMainThread])
	{
		log_error("ios", "IosPickImageFile must be called on the main thread");
		Callback(std::string());
		return;
	}

	if(!@available(iOS 14.0, *))
	{
		log_error("ios", "Importing images requires iOS 14 or newer");
		Callback(std::string());
		return;
	}

	UIWindow *pGameWindow = IosFindGameWindow();
	UIViewController *pRoot = pGameWindow.rootViewController;
	if(pRoot == nil)
	{
		log_error("ios", "Import failed: no root view controller");
		Callback(std::string());
		return;
	}

	s_pPickedUrl = nil;
	s_pPickedImage = nil;
	s_ImportChoice = IOS_IMPORT_CHOICE_NONE;

	// ------------------------------------------------------------------
	// Step 1: ask where to import from.
	// ------------------------------------------------------------------
	UIAlertController *pSheet =
		[UIAlertController alertControllerWithTitle:@"Import image"
			message:nil
			preferredStyle:UIAlertControllerStyleActionSheet];
	[pSheet addAction:[UIAlertAction actionWithTitle:@"Choose from Photos"
		style:UIAlertActionStyleDefault
		handler:^(UIAlertAction *pAction)
	{
		(void)pAction;
		s_ImportChoice = IOS_IMPORT_CHOICE_PHOTOS;
		[pRoot dismissViewControllerAnimated:NO completion:nil];
		s_ModalDone = YES;
	}]];
	[pSheet addAction:[UIAlertAction actionWithTitle:@"Choose from Files"
		style:UIAlertActionStyleDefault
		handler:^(UIAlertAction *pAction)
	{
		(void)pAction;
		s_ImportChoice = IOS_IMPORT_CHOICE_FILES;
		[pRoot dismissViewControllerAnimated:NO completion:nil];
		s_ModalDone = YES;
	}]];
	[pSheet addAction:[UIAlertAction actionWithTitle:@"Cancel"
		style:UIAlertActionStyleCancel
		handler:^(UIAlertAction *pAction)
	{
		(void)pAction;
		s_ImportChoice = IOS_IMPORT_CHOICE_CANCEL;
		[pRoot dismissViewControllerAnimated:NO completion:nil];
		s_ModalDone = YES;
	}]];
	log_info("ios", "Presenting import source selection");
	[pRoot presentViewController:pSheet animated:NO completion:nil];
	IosRunMainRunLoopUntilDone(pRoot);

	if(s_ImportChoice != IOS_IMPORT_CHOICE_PHOTOS && s_ImportChoice != IOS_IMPORT_CHOICE_FILES)
	{
		// Cancelled or timed out.
		log_info("ios", "Import cancelled");
		Callback(std::string());
		return;
	}

	// Let the alert dismissal fully finish before presenting the picker.
	[[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
		beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];

	// ------------------------------------------------------------------
	// Step 2: the picker itself.
	// ------------------------------------------------------------------
	s_pPickedUrl = nil;
	s_pPickedImage = nil;
	if(s_ImportChoice == IOS_IMPORT_CHOICE_PHOTOS)
	{
		UIImagePickerController *pPicker = [[UIImagePickerController alloc] init];
		pPicker.sourceType = UIImagePickerControllerSourceTypePhotoLibrary;
		pPicker.mediaTypes = @[ UTTypeImage.identifier ];
		pPicker.modalPresentationStyle = UIModalPresentationFullScreen;
		s_pPhotoPickerDelegate = [[CIosPhotoPickerDelegate alloc] init];
		pPicker.delegate = s_pPhotoPickerDelegate;
		log_info("ios", "Presenting Photos picker");
		[pRoot presentViewController:pPicker animated:NO completion:nil];
	}
	else
	{
		// asCopy:YES gives us a plain readable copy of the file inside
		// our tmp directory - no security-scoped resource handling
		// required.
		NSArray<UTType *> *apContentTypes = @[ UTTypeImage ];
		UIDocumentPickerViewController *pPicker =
			[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:apContentTypes asCopy:YES];
		pPicker.allowsMultipleSelection = NO;
		pPicker.modalPresentationStyle = UIModalPresentationFormSheet;
		s_pFilePickerDelegate = [[CIosFilePickerDelegate alloc] init];
		pPicker.delegate = s_pFilePickerDelegate;
		log_info("ios", "Presenting Files picker");
		[pRoot presentViewController:pPicker animated:NO completion:nil];
	}

	IosRunMainRunLoopUntilDone(pRoot);

	// Belt and suspenders: make sure nothing stays on screen even if the
	// loop ended through the timeout (the delegates dismiss their pickers
	// themselves on the normal paths).
	[pRoot dismissViewControllerAnimated:NO completion:nil];
	[[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
		beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];

	s_pFilePickerDelegate = nil;
	s_pPhotoPickerDelegate = nil;

	// ------------------------------------------------------------------
	// Step 3: convert the result and deliver it - we are back in the
	// game frame, in the same context as every other menu button
	// handler.
	// ------------------------------------------------------------------
	std::string Path;
	if(s_pPickedUrl != nil)
	{
		Path = IosConvertImportedFile(s_pPickedUrl);
	}
	else if(s_pPickedImage != nil)
	{
		Path = IosConvertImportedImage(s_pPickedImage);
	}
	else
	{
		log_info("ios", "Import cancelled");
	}
	s_pPickedUrl = nil;
	s_pPickedImage = nil;

	if(!Path.empty())
	{
		log_info("ios", "Import ready: '%s'", Path.c_str());
	}
	Callback(Path);
}
