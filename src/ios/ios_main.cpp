/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "ios_main.h"

#include <base/detect.h>
#include <base/fs.h>
#include <base/log.h>

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_main.h>

#if defined(CONF_PLATFORM_IOS)
extern "C" int SDL_UIKitRunApp(int argc, char **argv, int (*mainFunction)(int, char **));
extern "C" int SDL_main(int argc, char **argv);

int main(int argc, char **argv)
{
	return SDL_UIKitRunApp(argc, argv, SDL_main);
}
#endif

const char *InitIOS()
{
	char *pBasePath = SDL_GetBasePath();
	if(!pBasePath)
	{
		return "Failed to determine the app base path.";
	}
	if(fs_chdir(pBasePath) != 0)
	{
		SDL_free(pBasePath);
		return "Failed to change current directory to the app bundle.";
	}
	log_info("ios", "Changed current directory to '%s'", pBasePath);
	SDL_free(pBasePath);

	if(!fs_is_dir("data"))
	{
		return "Missing data directory in app bundle. Ensure data is packaged into the app.";
	}

	return nullptr;
}
