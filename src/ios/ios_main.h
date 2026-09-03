/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef IOS_IOS_MAIN_H
#define IOS_IOS_MAIN_H

#include <base/detect.h>
#if !defined(CONF_PLATFORM_IOS)
#error "This header should only be included when compiling for iOS"
#endif

/**
 * Initializes iOS specific runtime settings.
 *
 * This changes the current working directory to the app bundle so the data
 * files can be read directly from the packaged app.
 *
 * @return `nullptr` on success, error message on failure.
 */
const char *InitIOS();

#endif // IOS_IOS_MAIN_H
