#ifndef GAME_CLIENT_COMPONENTS_PUSHIN_PET_H
#define GAME_CLIENT_COMPONENTS_PUSHIN_PET_H

#include <game/client/component.h>

// Pushin client — Pet feature.
//
// A small tee that follows the local player. Two modes:
//   0 = flying: pet floats above-left of the player, bobs up/down, looks
//       at the same point the player is aiming at, copies the player's
//       emote. Position is smoothed with a configurable delay.
//   1 = walking: pet walks on the ground behind the player, plays the
//       walk/idle animation, copies the player's emote and look
//       direction. Same smoothing delay.
//
// All parameters (skin, size, offset, delay, bob, emote copy, look copy)
// are configurable via g_Config.m_PushinPet* variables.
class CPushinPet : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	void OnRender() override;

private:
	// Smoothed pet position (interpolated toward the target with a delay).
	vec2 m_PetPos = vec2(0.0f, 0.0f);
	bool m_Init = false;
	// Bob phase for flying mode.
	float m_BobPhase = 0.0f;
};

#endif
