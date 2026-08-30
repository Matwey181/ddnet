/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef IOS_IOS_FILE_PICKER_H
#define IOS_IOS_FILE_PICKER_H

#include <base/detect.h>
#if !defined(CONF_PLATFORM_IOS)
#error "This header should only be included when compiling for iOS"
#endif

#include <functional>
#include <string>

/**
 * Shows a system dialog that lets the player import a texture image either
 * from the Photos gallery (PHPickerViewController) or from the Files app
 * (UIDocumentPickerViewController, incl. iCloud Drive and third-party file
 * providers).
 *
 * The pickers are presented on a dedicated overlay UIWindow, the image
 * conversion runs on a background GCD queue and the callback is NOT invoked
 * directly from the UIKit delegates. Instead the finished result is queued
 * and delivered by IosProcessPendingImportCallbacks(), which the game has
 * to call every frame from a safe point (CMenus::Render). This is required
 * because the whole game runs on the iOS main thread and UIKit delegate
 * callbacks arrive inside the SDL event pump, where calling into the game
 * is unsafe (the previous implementation crashed on file selection).
 *
 * The callback receives the path of a readable PNG file inside the app's
 * temporary directory:
 *  - PNG files are passed through as an exact copy (no decoding, no memory
 *    spike, regardless of file size).
 *  - Any other image format that iOS can decode (JPEG, HEIC, ...) is
 *    re-encoded as PNG and downscaled to at most 2048x2048 pixels, because
 *    the game's image loader only supports PNG.
 *
 * On cancel or failure the path is empty and details are logged. Requires
 * iOS 14 or newer; on older systems the callback is delivered with an empty
 * path.
 */
void IosPickImageFile(std::function<void(const std::string &Path)> Callback);

/**
 * Delivers the results of finished image imports. Must be called every
 * frame from the game's menu rendering on the main thread (e.g. from
 * CMenus::Render), which is a safe place to run the import callbacks.
 */
void IosProcessPendingImportCallbacks();

#endif // IOS_IOS_FILE_PICKER_H
