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
 * from the Photos gallery (UIImagePickerController) or from the Files app
 * (UIDocumentPickerViewController, incl. iCloud Drive and third-party file
 * providers).
 *
 * This function is SYNCHRONOUS and must be called on the main thread from
 * the game frame (e.g. a menu button handler). While the dialogs are on
 * screen the real main run loop is run in a nested "modal" loop, because
 * the game never services the run loop properly itself (SDL only pumps it
 * in tiny bursts) and the system pickers need a properly serviced run loop
 * to work at all. The calling game frame blocks until the user is done.
 *
 * The callback is invoked directly (in the caller's context) with the path
 * of a readable PNG file inside the app's temporary directory:
 *  - PNG files are passed through as an exact copy (no decoding, no memory
 *    spike, regardless of file size).
 *  - Any other image format that iOS can decode (JPEG, HEIC, ...) is
 *    re-encoded as PNG and downscaled to at most 2048x2048 pixels, because
 *    the game's image loader only supports PNG.
 *
 * On cancel or failure the path is empty and details are logged. Requires
 * iOS 14 or newer; on older systems the callback is invoked immediately
 * with an empty path.
 */
void IosPickImageFile(std::function<void(const std::string &Path)> Callback);

#endif // IOS_IOS_FILE_PICKER_H
