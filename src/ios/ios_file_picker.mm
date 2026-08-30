/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "ios_file_picker.h"

#include <base/log.h>

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <PhotosUI/PhotosUI.h>

#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// Threading model
//
// The whole game (menu rendering, SDL event pump) runs on the iOS main
// thread; the main run loop is only serviced in tiny bursts from SDL's
// UIKit_PumpEvents. UIKit therefore works, but every delegate callback
// arrives inside one of those bursts, i.e. in the middle of SDL_PollEvent -
// a very unsafe place to run game code (handing the picked file to the game
// directly from the document picker delegate crashed the app).
//
// This implementation keeps all work away from that context:
//  1. The pickers are presented on a dedicated overlay UIWindow from its own
//     root view controller. The overlay window is made key while the pickers
//     are on screen and the alert -> picker transition is non-animated and
//     runs in the dismissal completion, which makes the presentation
//     deterministic (no "presentation while dismissing" race that silently
//     swallowed the Photos picker before).
//  2. Image conversion (decoding, downscaling, PNG encoding, file copying)
//     runs on a background GCD queue.
//  3. The finished result is pushed into a mutex protected queue which the
//     game drains via IosProcessPendingImportCallbacks() from CMenus::Render
//     - a safe point in the frame where all the other menu button handlers
//     run as well.
//
// This file is compiled with -fobjc-arc (see CMakeLists.txt).

// ------------------------------------------------------------------
// Pending import queue (filled from any thread, drained on the main thread)
// ------------------------------------------------------------------

struct SIosPendingImport
{
	std::function<void(const std::string &)> m_Callback;
	std::string m_Path;
};

static std::mutex s_PendingImportsMutex;
static std::vector<SIosPendingImport> s_apPendingImports;

static void IosQueueResult(const std::function<void(const std::string &)> &Callback, const std::string &Path)
{
	std::lock_guard<std::mutex> Lock(s_PendingImportsMutex);
	s_apPendingImports.push_back(SIosPendingImport{Callback, Path});
}

void IosProcessPendingImportCallbacks()
{
	std::vector<SIosPendingImport> apImports;
	{
		std::lock_guard<std::mutex> Lock(s_PendingImportsMutex);
		apImports.swap(s_apPendingImports);
	}
	for(const SIosPendingImport &Import : apImports)
	{
		Import.m_Callback(Import.m_Path);
	}
}

// ------------------------------------------------------------------
// Picker session state (main thread only)
// ------------------------------------------------------------------

static UIWindow *s_pGameWindow = nil;
static UIWindow *s_pOverlayWindow = nil;
static bool s_ImportActive = false;

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
// Camera photos can be 12-48 MP; decoding and re-encoding those at full size
// allocates hundreds of megabytes and gets the app killed by the OS.
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

// Runs on a background queue. The document picker (used with asCopy:YES)
// hands us a plain readable copy of the picked file inside our tmp
// directory - no security-scoped resource bookkeeping is needed.
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

// Runs on a background queue. Converts an image that came from the Photos
// picker (UIImage, possibly HEIC encoded and huge) into a downscaled PNG.
static std::string IosConvertImportedImage(UIImage *pImage)
{
	pImage = IosDownscaleImage(pImage, 2048.0);
	return IosWriteImageAsPng(pImage, "photo");
}

static void IosTeardownOverlay()
{
	if(s_pOverlayWindow != nil)
	{
		s_pOverlayWindow.hidden = YES;
		s_pOverlayWindow.rootViewController = nil;
		s_pOverlayWindow = nil;
	}
	if(s_pGameWindow != nil)
	{
		[s_pGameWindow makeKeyWindow];
		s_pGameWindow = nil;
	}
}

// Main thread. Called exactly once when the picker flow ends (picked,
// cancelled or failed). Closes the overlay window and starts the background
// conversion. The game callback is delivered later, from
// IosProcessPendingImportCallbacks(). Forward declared so the picker
// delegates below can call it.
static void IosFinishImport(NSURL *pUrl, UIImage *pImage, const std::function<void(const std::string &)> &Callback);

// ------------------------------------------------------------------
// Files app picker (UIDocumentPickerViewController)
// ------------------------------------------------------------------

@interface CIosFilePickerDelegate : NSObject <UIDocumentPickerDelegate>
{
@public
	std::function<void(const std::string &)> m_Callback;
}
@end

@implementation CIosFilePickerDelegate

- (void)documentPicker:(UIDocumentPickerViewController *)pController didPickDocumentsAtURLs:(NSArray<NSURL *> *)pUrls
{
	(void)pController;
	log_info("ios", "Files picker: %d document(s) selected", (int)pUrls.count);
	NSURL *pUrl = pUrls.count > 0 ? pUrls[0] : nil;
	// Copy the callback into a local so it stays alive even if the static
	// delegate reference is cleared while the blocks below are running.
	std::function<void(const std::string &)> Callback = m_Callback;
	UIViewController *pPresenter = pController.presentingViewController;
	void (^pFinish)(void) = ^
	{
		IosFinishImport(pUrl, nil, Callback);
	};
	if(pPresenter != nil)
	{
		[pPresenter dismissViewControllerAnimated:NO completion:pFinish];
	}
	else
	{
		// Already dismissed (e.g. interactive swipe-to-cancel).
		pFinish();
	}
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)pController
{
	log_info("ios", "Files picker: cancelled");
	std::function<void(const std::string &)> Callback = m_Callback;
	UIViewController *pPresenter = pController.presentingViewController;
	void (^pFinish)(void) = ^
	{
		IosFinishImport(nil, nil, Callback);
	};
	if(pPresenter != nil)
	{
		[pPresenter dismissViewControllerAnimated:NO completion:pFinish];
	}
	else
	{
		pFinish();
	}
}

@end

// ------------------------------------------------------------------
// Photos gallery picker (PHPickerViewController)
// ------------------------------------------------------------------

API_AVAILABLE(ios(14.0))
@interface CIosPhotoPickerDelegate : NSObject <PHPickerViewControllerDelegate>
{
@public
	std::function<void(const std::string &)> m_Callback;
}
@end

@implementation CIosPhotoPickerDelegate

- (void)picker:(PHPickerViewController *)pPicker didFinishPickingWithResults:(NSArray<PHPickerResult *> *)pResults
{
	log_info("ios", "Photos picker: %d result(s)", (int)pResults.count);
	std::function<void(const std::string &)> Callback = m_Callback;
	UIViewController *pPresenter = pPicker.presentingViewController;
	void (^pFinish)(void) = ^
	{
		PHPickerResult *pResult = pResults.count > 0 ? pResults[0] : nil;
		if(pResult == nil)
		{
			// Cancel button or swipe-to-dismiss: empty results.
			IosFinishImport(nil, nil, Callback);
			return;
		}
		// The image data loads asynchronously; the completion handler may
		// be called on any thread, so marshal back to the main thread
		// before touching UIKit (IosFinishImport tears down a UIWindow).
		[pResult.itemProvider loadObjectOfClass:[UIImage class] completionHandler:^(id<NSSecureCoding> _Nullable pObject, NSError *_Nullable pError)
		{
			UIImage *pImage = (UIImage *)pObject;
			if(pImage == nil)
			{
				log_error("ios", "Photos picker: could not load image (%s)",
					pError != nil ? pError.localizedDescription.UTF8String : "unknown error");
			}
			dispatch_async(dispatch_get_main_queue(), ^
			{
				IosFinishImport(nil, pImage, Callback);
			});
		}];
	};
	if(pPresenter != nil)
	{
		[pPresenter dismissViewControllerAnimated:NO completion:pFinish];
	}
	else
	{
		pFinish();
	}
}

@end

// Keeps the delegates (and thus the game callbacks) alive while the pickers
// are on screen. Only one import flow can be active at a time.
static CIosFilePickerDelegate *s_pFilePickerDelegate = nil;
static CIosPhotoPickerDelegate *s_pPhotoPickerDelegate API_AVAILABLE(ios(14.0)) = nil;

static void IosFinishImport(NSURL *pUrl, UIImage *pImage, const std::function<void(const std::string &)> &Callback)
{
	IosTeardownOverlay();
	s_pFilePickerDelegate = nil;
	if(@available(iOS 14.0, *))
	{
		s_pPhotoPickerDelegate = nil;
	}
	s_ImportActive = false;

	if(pUrl == nil && pImage == nil)
	{
		// Cancelled: deliver an empty path so the game callback can return
		// early.
		IosQueueResult(Callback, std::string());
		return;
	}

	// Convert the image on a background queue so neither the main thread nor
	// the SDL event pump is blocked.
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^
	{
		std::string Path;
		if(pUrl != nil)
		{
			Path = IosConvertImportedFile(pUrl);
		}
		else
		{
			Path = IosConvertImportedImage(pImage);
		}
		if(Path.empty())
		{
			log_error("ios", "Import failed: could not prepare the image");
		}
		IosQueueResult(Callback, Path);
	});
}

// ------------------------------------------------------------------
// Picker presentation
// ------------------------------------------------------------------

API_AVAILABLE(ios(14.0))
static void IosPresentFilesPicker(UIViewController *pRoot, const std::function<void(const std::string &)> &Callback)
{
	// asCopy:YES gives us a plain readable copy of the file inside our tmp
	// directory - no security-scoped resource handling required.
	NSArray<UTType *> *apContentTypes = @[ UTTypeImage ];
	UIDocumentPickerViewController *pPicker =
		[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:apContentTypes asCopy:YES];
	pPicker.allowsMultipleSelection = NO;
	pPicker.modalPresentationStyle = UIModalPresentationFormSheet;

	s_pFilePickerDelegate = [[CIosFilePickerDelegate alloc] init];
	s_pFilePickerDelegate->m_Callback = Callback;
	pPicker.delegate = s_pFilePickerDelegate;
	log_info("ios", "Presenting Files picker");
	[pRoot presentViewController:pPicker animated:NO completion:nil];
}

API_AVAILABLE(ios(14.0))
static void IosPresentPhotosPicker(UIViewController *pRoot, const std::function<void(const std::string &)> &Callback)
{
	PHPickerConfiguration *pConfig = [[PHPickerConfiguration alloc] init];
	pConfig.filter = [PHPickerFilter imagesFilter];
	pConfig.selectionLimit = 1;
	PHPickerViewController *pPicker = [[PHPickerViewController alloc] initWithConfiguration:pConfig];
	pPicker.modalPresentationStyle = UIModalPresentationPageSheet;

	s_pPhotoPickerDelegate = [[CIosPhotoPickerDelegate alloc] init];
	s_pPhotoPickerDelegate->m_Callback = Callback;
	pPicker.delegate = s_pPhotoPickerDelegate;
	log_info("ios", "Presenting Photos picker");
	[pRoot presentViewController:pPicker animated:NO completion:nil];
}

// Creates a transparent window above the game window. The pickers are
// presented from its root view controller, which keeps them independent
// from whatever presentation state the game window is in. While the window
// is key and nothing is presented on it, its non-interactive root view lets
// touches fall through to the game window below.
static UIWindow *IosCreateOverlayWindow(UIWindow *pGameWindow)
{
	UIWindow *pOverlay = nil;
	if(@available(iOS 13.0, *))
	{
		if(pGameWindow.windowScene != nil)
		{
			pOverlay = [[UIWindow alloc] initWithWindowScene:pGameWindow.windowScene];
		}
	}
	if(pOverlay == nil)
	{
		pOverlay = [[UIWindow alloc] initWithFrame:pGameWindow.bounds];
	}
	pOverlay.windowLevel = UIWindowLevelNormal + 1.0;
	pOverlay.backgroundColor = [UIColor clearColor];

	UIViewController *pRoot = [[UIViewController alloc] init];
	pRoot.view.backgroundColor = [UIColor clearColor];
	pRoot.view.userInteractionEnabled = NO;
	pOverlay.rootViewController = pRoot;
	[pOverlay makeKeyAndVisible];
	return pOverlay;
}

void IosPickImageFile(std::function<void(const std::string &Path)> Callback)
{
	dispatch_async(dispatch_get_main_queue(), ^
	{
		if(!@available(iOS 14.0, *))
		{
			log_error("ios", "Importing images requires iOS 14 or newer");
			IosQueueResult(Callback, std::string());
			return;
		}

		if(s_ImportActive || s_pOverlayWindow != nil)
		{
			log_error("ios", "An import picker is already open, ignoring request");
			IosQueueResult(Callback, std::string());
			return;
		}

		UIWindow *pGameWindow = IosFindGameWindow();
		if(pGameWindow == nil)
		{
			log_error("ios", "Import failed: no game window found");
			IosQueueResult(Callback, std::string());
			return;
		}

		UIWindow *pOverlay = IosCreateOverlayWindow(pGameWindow);
		if(pOverlay == nil)
		{
			log_error("ios", "Import failed: could not create the picker window");
			IosQueueResult(Callback, std::string());
			return;
		}
		s_pGameWindow = pGameWindow;
		s_pOverlayWindow = pOverlay;
		s_ImportActive = true;

		UIViewController *pRoot = pOverlay.rootViewController;
		UIAlertController *pSheet =
			[UIAlertController alertControllerWithTitle:@"Import image"
				message:nil
				preferredStyle:UIAlertControllerStyleActionSheet];
		// iPad requires a source view for action sheets, otherwise
		// presenting them crashes.
		pSheet.popoverPresentationController.sourceView = pRoot.view;
		pSheet.popoverPresentationController.sourceRect =
			CGRectMake(pRoot.view.bounds.size.width / 2.0, pRoot.view.bounds.size.height / 2.0, 1.0, 1.0);

		[pSheet addAction:[UIAlertAction actionWithTitle:@"Choose from Photos"
			style:UIAlertActionStyleDefault
			handler:^(UIAlertAction *pAction)
		{
			(void)pAction;
			// Present the picker only after the action sheet has fully
			// dismissed, otherwise UIKit may silently reject the
			// presentation (this is what made the Photos picker appear
			// dead before).
			[pRoot dismissViewControllerAnimated:NO completion:^
			{
				IosPresentPhotosPicker(pRoot, Callback);
			}];
		}]];
		[pSheet addAction:[UIAlertAction actionWithTitle:@"Choose from Files"
			style:UIAlertActionStyleDefault
			handler:^(UIAlertAction *pAction)
		{
			(void)pAction;
			[pRoot dismissViewControllerAnimated:NO completion:^
			{
				IosPresentFilesPicker(pRoot, Callback);
			}];
		}]];
		[pSheet addAction:[UIAlertAction actionWithTitle:@"Cancel"
			style:UIAlertActionStyleCancel
			handler:^(UIAlertAction *pAction)
		{
			(void)pAction;
			[pRoot dismissViewControllerAnimated:NO completion:^
			{
				IosFinishImport(nil, nil, Callback);
			}];
		}]];
		log_info("ios", "Presenting import source selection");
		[pRoot presentViewController:pSheet animated:NO completion:nil];
	});
}
