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
 * from the Photos gallery or from the Files app (incl. iCloud Drive and
 * third-party file providers).
 *
 * The callback is invoked exactly once on the main thread with the path of a
 * readable PNG file inside the app's temporary directory:
 *  - PNG files are passed through unchanged (the picker copies them into
 *    the tmp directory, no security-scoped resource handling required).
 *  - Any other image format that iOS can decode (JPEG, HEIC, ...) is
 *    re-encoded as PNG, because the game's image loader only supports PNG.
 *
 * On cancel or failure the path is empty and details are logged. Requires
 * iOS 14 or newer; on older systems the callback is invoked immediately
 * with an empty path.
 */
void IosPickImageFile(std::function<void(const std::string &Path)> Callback);

#endif // IOS_IOS_FILE_PICKER_H
