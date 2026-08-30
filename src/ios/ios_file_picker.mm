/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "ios_file_picker.h"

#include <base/log.h>

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <PhotosUI/PhotosUI.h>

// All UIKit work is dispatched to the main thread. The game shares the main
// thread with UIKit through SDL's event pump, so presenting view controllers
// works even though the game loop is running on the main thread as well.
// This file is compiled with -fobjc-arc (see CMakeLists.txt).

static UIViewController *IosRootViewController()
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
	return pKeyWindow.rootViewController;
}

// Writes the image into the app's temporary directory as a PNG file and
// returns its file URL. The game's image loader only supports PNG, so every
// import path funnels through here.
static NSURL *IosWriteImageAsPng(UIImage *pImage, NSString *pBaseName)
{
	const long long TimeMs = (long long)([[NSDate date] timeIntervalSince1970] * 1000.0);
	NSString *pFileName = [NSString stringWithFormat:@"%@_%lld.png", pBaseName, TimeMs];
	NSString *pPath = [NSTemporaryDirectory() stringByAppendingPathComponent:pFileName];
	NSData *pData = UIImagePNGRepresentation(pImage);
	if(pData == nil || ![pData writeToFile:pPath atomically:YES])
	{
		log_error("ios", "Failed to write imported image to '%s'", pPath.UTF8String);
		return nil;
	}
	return [NSURL fileURLWithPath:pPath];
}

// The document picker (used with asCopy:YES) hands us a plain readable copy
// of the picked file inside our tmp directory - no security-scoped resource
// bookkeeping is needed. PNG files are passed through unchanged, every other
// image format that iOS can decode gets re-encoded as PNG.
static NSURL *IosPreparePickedFile(NSURL *pUrl)
{
	NSString *pPath = pUrl.path;
	NSString *pExt = [[pPath pathExtension] lowercaseString];
	if([pExt isEqualToString:@"png"])
	{
		return pUrl;
	}
	UIImage *pImage = [UIImage imageWithContentsOfFile:pPath];
	if(pImage == nil)
	{
		log_error("ios", "Failed to decode picked file '%s' as an image", pPath.UTF8String);
		return nil;
	}
	NSString *pBaseName = [[pPath lastPathComponent] stringByDeletingPathExtension];
	return IosWriteImageAsPng(pImage, pBaseName);
}

// ------------------------------------------------------------------
// Files app picker (UIDocumentPickerViewController)
// ------------------------------------------------------------------

@interface CIosFilePickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, copy) void (^Completion)(NSURL *pUrl);
@end

@implementation CIosFilePickerDelegate

- (void)documentPicker:(UIDocumentPickerViewController *)pController didPickDocumentsAtURLs:(NSArray<NSURL *> *)pUrls
{
	(void)pController;
	log_info("ios", "Files picker: %d document(s) selected", (int)pUrls.count);
	[pController dismissViewControllerAnimated:YES completion:nil];
	if(self.Completion != nil)
	{
		self.Completion(pUrls.count > 0 ? pUrls[0] : nil);
	}
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)pController
{
	(void)pController;
	log_info("ios", "Files picker: cancelled");
	[pController dismissViewControllerAnimated:YES completion:nil];
	if(self.Completion != nil)
	{
		self.Completion(nil);
	}
}

@end

// ------------------------------------------------------------------
// Photos gallery picker (PHPickerViewController)
// ------------------------------------------------------------------

API_AVAILABLE(ios(14.0))
@interface CIosPhotoPickerDelegate : NSObject <PHPickerViewControllerDelegate>
@property(nonatomic, copy) void (^Completion)(NSURL *pUrl);
@end

@implementation CIosPhotoPickerDelegate

- (void)picker:(PHPickerViewController *)pPicker didFinishPickingWithResults:(NSArray<PHPickerResult *> *)pResults
{
	log_info("ios", "Photos picker: %d result(s)", (int)pResults.count);
	[pPicker dismissViewControllerAnimated:YES completion:nil];

	PHPickerResult *pResult = pResults.count > 0 ? pResults[0] : nil;
	if(pResult == nil)
	{
		if(self.Completion != nil)
		{
			self.Completion(nil);
		}
		return;
	}

	// The image data loads asynchronously; the block keeps this delegate
	// alive until the end (PHPickerViewController only holds a weak delegate
	// reference).
	[pResult.itemProvider loadObjectOfClass:[UIImage class] completionHandler:^(id<NSSecureCoding> _Nullable pObject, NSError *_Nullable pError)
	{
		dispatch_async(dispatch_get_main_queue(), ^
		{
			NSURL *pPngUrl = nil;
			UIImage *pImage = (UIImage *)pObject;
			if(pImage == nil)
			{
				const char *pErrorText = pError != nil ? pError.localizedDescription.UTF8String : "unknown error";
				log_error("ios", "Photos picker: could not load image (%s)", pErrorText);
			}
			else
			{
				pPngUrl = IosWriteImageAsPng(pImage, @"photo");
			}
			if(self.Completion != nil)
			{
				self.Completion(pPngUrl);
			}
		});
	}];
}

@end

// Keeps the delegates (and thus the completion blocks, which own the
// std::function) alive while the pickers are on screen. Only one picker can
// be open at a time.
static CIosFilePickerDelegate *s_pFilePickerDelegate = nil;
static CIosPhotoPickerDelegate *s_pPhotoPickerDelegate API_AVAILABLE(ios(14.0)) = nil;

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
	s_pFilePickerDelegate.Completion = ^(NSURL *pUrl)
	{
		NSURL *pPngUrl = nil;
		if(pUrl != nil)
		{
			pPngUrl = IosPreparePickedFile(pUrl);
		}
		s_pFilePickerDelegate = nil;
		if(pPngUrl != nil)
		{
			Callback(std::string(pPngUrl.path.UTF8String));
		}
		else
		{
			Callback(std::string());
		}
	};
	pPicker.delegate = s_pFilePickerDelegate;
	log_info("ios", "Presenting Files picker");
	[pRoot presentViewController:pPicker animated:YES completion:nil];
}

API_AVAILABLE(ios(14.0))
static void IosPresentPhotosPicker(UIViewController *pRoot, const std::function<void(const std::string &)> &Callback)
{
	PHPickerConfiguration *pConfig = [[PHPickerConfiguration alloc] init];
	pConfig.filter = [PHPickerFilter imagesFilter];
	pConfig.selectionLimit = 1;
	PHPickerViewController *pPicker = [[PHPickerViewController alloc] initWithConfiguration:pConfig];
	pPicker.modalPresentationStyle = UIModalPresentationFormSheet;

	s_pPhotoPickerDelegate = [[CIosPhotoPickerDelegate alloc] init];
	s_pPhotoPickerDelegate.Completion = ^(NSURL *pUrl)
	{
		s_pPhotoPickerDelegate = nil;
		if(pUrl != nil)
		{
			Callback(std::string(pUrl.path.UTF8String));
		}
		else
		{
			Callback(std::string());
		}
	};
	pPicker.delegate = s_pPhotoPickerDelegate;
	log_info("ios", "Presenting Photos picker");
	[pRoot presentViewController:pPicker animated:YES completion:nil];
}

void IosPickImageFile(std::function<void(const std::string &Path)> Callback)
{
	dispatch_async(dispatch_get_main_queue(), ^
	{
		if(@available(iOS 14.0, *))
		{
			if(s_pFilePickerDelegate != nil || s_pPhotoPickerDelegate != nil)
			{
				log_error("ios", "An import picker is already open, ignoring request");
				return;
			}

			UIViewController *pRoot = IosRootViewController();
			if(pRoot == nil)
			{
				log_error("ios", "Import failed: no root view controller");
				Callback(std::string());
				return;
			}

			UIAlertController *pSheet =
				[UIAlertController alertControllerWithTitle:@"Import image"
													 message:nil
											  preferredStyle:UIAlertControllerStyleActionSheet];
			[pSheet addAction:[UIAlertAction actionWithTitle:@"Choose from Photos"
														style:UIAlertActionStyleDefault
													  handler:^(UIAlertAction *pAction)
			{
				(void)pAction;
				IosPresentPhotosPicker(pRoot, Callback);
			}]];
			[pSheet addAction:[UIAlertAction actionWithTitle:@"Choose from Files"
														style:UIAlertActionStyleDefault
													  handler:^(UIAlertAction *pAction)
			{
				(void)pAction;
				IosPresentFilesPicker(pRoot, Callback);
			}]];
			[pSheet addAction:[UIAlertAction actionWithTitle:@"Cancel"
														style:UIAlertActionStyleCancel
													  handler:^(UIAlertAction *pAction)
			{
				(void)pAction;
				Callback(std::string());
			}]];
			log_info("ios", "Presenting import source selection");
			[pRoot presentViewController:pSheet animated:YES completion:nil];
		}
		else
		{
			log_error("ios", "Importing images requires iOS 14 or newer");
			Callback(std::string());
		}
	});
}
