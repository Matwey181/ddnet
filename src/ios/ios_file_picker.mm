/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "ios_file_picker.h"

#include <base/log.h>

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@interface CIosFilePickerDelegate : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, copy) void (^Completion)(NSURL *pUrl);
@end

@implementation CIosFilePickerDelegate

- (void)documentPicker:(UIDocumentPickerViewController *)pController didPickDocumentsAtURLs:(NSArray<NSURL *> *)pUrls
{
	(void)pController;
	if(self.Completion)
	{
		self.Completion(pUrls.count > 0 ? pUrls[0] : nil);
	}
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)pController
{
	(void)pController;
	if(self.Completion)
	{
		self.Completion(nil);
	}
}

@end

// Keeps the delegate (and thus the completion block, which owns the
// std::function) alive for as long as the picker is on screen. There can
// only be one picker open at a time, so a single static instance is enough.
static CIosFilePickerDelegate *s_pPickerDelegate = nil;
static bool s_PickerOpen = false;

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

void IosPickImageFile(std::function<void(const std::string &Path)> Callback)
{
	if(s_PickerOpen)
	{
		log_error("ios", "File picker already open, ignoring request");
		return;
	}

	dispatch_async(dispatch_get_main_queue(), ^{
		UIViewController *pRoot = IosRootViewController();
		if(!pRoot)
		{
			log_error("ios", "Failed to present file picker: no root view controller");
			Callback(std::string());
			return;
		}

		NSArray<UTType *> *pContentTypes = @[ UTTypeImage ];
		UIDocumentPickerViewController *pPicker =
			[[UIDocumentPickerViewController alloc] initForOpeningContentTypes:pContentTypes];
		pPicker.allowsMultipleSelection = NO;
		pPicker.modalPresentationStyle = UIModalPresentationFormSheet;

		s_pPickerDelegate = [[CIosFilePickerDelegate alloc] init];
		s_pPickerDelegate.Completion = ^(NSURL *pUrl) {
			std::string Result;
			BOOL Accessing = NO;
			if(pUrl)
			{
				Accessing = [pUrl startAccessingSecurityScopedResource];
				Result = std::string(pUrl.path.UTF8String);
			}

			// Callback must synchronously read/copy the file: access to the
			// security-scoped resource ends right after this call returns.
			Callback(Result);

			if(Accessing)
			{
				[pUrl stopAccessingSecurityScopedResource];
			}
			s_pPickerDelegate = nil;
			s_PickerOpen = false;
		};
		pPicker.delegate = s_pPickerDelegate;

		s_PickerOpen = true;
		[pRoot presentViewController:pPicker animated:YES completion:nil];
	});
}
