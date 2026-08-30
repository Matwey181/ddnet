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
 * Presents the native iOS document picker (Files app, iCloud Drive, and any
 * third-party providers like a Photos-export extension) filtered to image
 * files, so the user can pick a texture/skin PNG without needing a
 * jailbreak or a file manager app that can reach the sandboxed app
 * directory directly.
 *
 * The picker is presented asynchronously on the main thread. `Callback` is
 * invoked on the main thread once the user picks a file or cancels.
 *
 * IMPORTANT: `Callback` is invoked while the picked file's security-scoped
 * resource access is still active. If `Path` is non-empty, the callback
 * MUST synchronously read or copy the file's contents before returning -
 * the path is not guaranteed to remain readable afterwards.
 *
 * On cancel or failure, `Path` is empty and an error is logged for failures.
 * Calling this while a picker is already being presented has no effect
 * (the new request is ignored) to avoid stacking modal pickers.
 */
void IosPickImageFile(std::function<void(const std::string &Path)> Callback);

#endif // IOS_IOS_FILE_PICKER_H
