/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_MENUS_PUSHIN_H
#define GAME_CLIENT_COMPONENTS_MENUS_PUSHIN_H

#include <base/color.h>

// Pushin client — Var List feature
// These helpers are implemented in menus_pushin.cpp and used by nameplates.cpp
// and players.cpp to apply in-game tint / nick color / prefix / emote overrides
// for players that the local user has marked as "war" or "team".

// Returns true if the given player name is in the war or team list.
// If true, sets OutColor to the configured war/team tint color (or white if
// nickname tinting is disabled), and copies the prefix string (if enabled) to
// pPrefixBuf. pPrefixBuf may be null.
bool PushinGetPlayerDisplayInfo(const char *pName, ColorRGBA &OutColor, char *pPrefixBuf, int PrefixBufSize);

// Returns true if the given player name is in the war or team list.
// If true, applies the configured skin tint (in place) to BodyColor / FeetColor
// / BloodColor when skin tinting is enabled, and sets OutEmote to EMOTE_ANGRY
// (war) or EMOTE_HAPPY (team).
bool PushinApplyTintToPlayerSkin(const char *pName, ColorRGBA &BodyColor, ColorRGBA &FeetColor, ColorRGBA &BloodColor, int &OutEmote);

#endif
