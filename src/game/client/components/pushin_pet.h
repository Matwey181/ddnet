#ifndef GAME_CLIENT_COMPONENTS_PUSHIN_PET_H
#define GAME_CLIENT_COMPONENTS_PUSHIN_PET_H

#include <game/client/animstate.h>
#include <game/client/component.h>

// Pushin client — Pet feature.
//
// A small tee that follows the local player. Two modes:
//   0 = flying: pet floats above-left of the player, bobs up/down, looks
//       at the same point the player is aiming at, copies the player's
//       emote. Position is smoothed with a configurable delay.
//   1 = walking: pet walks on the ground behind the player, plays the
//       walk/idle/air animation (same as CPlayers), copies the player's
//       emote and look direction. Same smoothing delay.
//
// The look direction (where the pet's eyes point) is smoothed separately
// with a 0.1s delay so the pet turns its head smoothly instead of snapping
// when the player spins.
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
        // Pet velocity for walking-mode physics.
        vec2 m_PetVel = vec2(0.0f, 0.0f);
        bool m_Init = false;
        // Smoothed look direction (0.1s delay, prevents snap on fast turns).
        vec2 m_LookDir = vec2(1.0f, 0.0f);
        bool m_LookInit = false;
        // Bob phase for flying mode.
        float m_BobPhase = 0.0f;
        // Reusable walk animation state (walking mode only).
        CAnimState m_WalkState;
        // Last frame's pet X position — used to detect if the pet is moving
        // (for walk vs idle animation).
        float m_LastPetX = 0.0f;
        // Jump counter for double jump (2 = full, 1 = one air jump left, 0 = none).
        int m_JumpsLeft = 2;
        // Hook state: 0=idle, 2=grabbed (pulling toward hook point).
        int m_HookState = 0;
        // Hook target position (where the hook grabbed).
        vec2 m_HookPos = vec2(0.0f, 0.0f);
        // Hook active timer (seconds). When 0, hook releases.
        float m_HookTimer = 0.0f;
        // Hook cooldown (seconds). Prevents re-hooking immediately after release.
        float m_HookCooldown = 0.0f;
        // Stuck timer — if the pet hasn't moved for a while, it tries to hook.
        float m_StuckTimer = 0.0f;
};

#endif
