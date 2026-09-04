/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

// Pushin client — Pet feature.
// See pushin_pet.h for the full description.

#include "pushin_pet.h"

#include <base/math.h>
#include <engine/shared/config.h>
#include <game/client/animstate.h>
#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/client/skin.h>
#include <generated/client_data.h>

void CPushinPet::OnRender()
{
	if(!g_Config.m_PushinPetEnabled)
		return;
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	if(LocalId < 0 || LocalId >= MAX_CLIENTS)
		return;
	if(!GameClient()->m_aClients[LocalId].m_Active)
		return;

	// --- Player position and emote ---
	const vec2 PlayerPos = GameClient()->m_aClients[LocalId].m_RenderPos;
	int PlayerEmote = EMOTE_NORMAL;
	if(GameClient()->m_Snap.m_aCharacters[LocalId].m_Active)
		PlayerEmote = GameClient()->m_Snap.m_aCharacters[LocalId].m_Cur.m_Emote;
	const vec2 AimTarget = GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy];

	// Player velocity for walk animation
	const CCharacterCore &PlayerCore = GameClient()->m_aClients[LocalId].m_Predicted;
	const float VelX = PlayerCore.m_Vel.x;
	const bool PlayerMoving = std::abs(VelX) > 1.0f;
	const bool PlayerInAir = !PlayerCore.m_Grounded;

	// --- Target pet position ---
	vec2 TargetPos;
	const float OffsetX = (float)g_Config.m_PushinPetOffsetX;
	const float OffsetY = (float)g_Config.m_PushinPetOffsetY;

	if(g_Config.m_PushinPetMode == 0) // flying
	{
		TargetPos = PlayerPos + vec2(OffsetX, OffsetY);
		if(g_Config.m_PushinPetBob)
		{
			m_BobPhase += Client()->RenderFrameTime() * 3.0f;
			TargetPos.y += std::sin(m_BobPhase) * (float)g_Config.m_PushinPetBobAmount;
		}
	}
	else // walking
	{
		// Place pet on the ground at the same Y as player feet.
		// Player position is at the body center; feet are ~16px below.
		// Pet is smaller, so its center is offset up by half its size.
		const float PetSize = 64.0f * ((float)g_Config.m_PushinPetSize / 100.0f);
		const float Dir = (AimTarget.x >= PlayerPos.x) ? 1.0f : -1.0f;
		// Walk behind the player: opposite of facing direction.
		TargetPos = vec2(PlayerPos.x - Dir * std::abs(OffsetX), PlayerPos.y);
	}

	// --- Smooth position with configurable delay ---
	if(!m_Init)
	{
		m_PetPos = TargetPos;
		m_Init = true;
	}
	else
	{
		const float DelaySec = std::max(0.01f, (float)g_Config.m_PushinPetDelay / 100.0f);
		const float Dt = Client()->RenderFrameTime();
		const float Factor = 1.0f - std::exp(-Dt / DelaySec);
		m_PetPos = mix(m_PetPos, TargetPos, Factor);
	}

	// --- Smooth look direction with separate delay (default 0.1s) ---
	// The look delay is a fraction of the position delay — faster, so the
	// pet turns its head smoothly but still follows quickly.
	const vec2 PetToAimRaw = AimTarget - m_PetPos;
	vec2 TargetDir(1.0f, 0.0f);
	if(length(PetToAimRaw) > 0.001f)
		TargetDir = normalize(PetToAimRaw);

	if(!m_LookInit)
	{
		m_LookDir = TargetDir;
		m_LookInit = true;
	}
	else
	{
		// Look delay: 0.1s default = 10 centiseconds.
		// Use a fixed 0.1s smoothing for the look direction (not configurable
		// separately to keep the UI simple — the position delay already gives
		// the "laggy follow" feel, and 0.1s look smoothing prevents snap).
		const float LookDelay = 0.1f;
		const float Dt = Client()->RenderFrameTime();
		const float Factor = 1.0f - std::exp(-Dt / LookDelay);
		m_LookDir = normalize(mix(m_LookDir, TargetDir, Factor));
	}

	// --- Build the tee render info ---
	const CSkin *pSkin = GameClient()->m_Skins.Find(g_Config.m_PushinPetSkin);
	if(pSkin == nullptr)
		pSkin = GameClient()->m_Skins.Find("default");
	if(pSkin == nullptr)
		return;

	CTeeRenderInfo Info;
	Info.Apply(pSkin);
	Info.m_Size = 64.0f * ((float)g_Config.m_PushinPetSize / 100.0f);

	// --- Animation state ---
	// For walking mode, build a proper walk animation like CPlayers does.
	int Emote = EMOTE_NORMAL;
	vec2 Dir = m_LookDir;
	if(g_Config.m_PushinPetLook)
		Dir = m_LookDir;
	else
		Dir = vec2(1.0f, 0.0f);

	if(g_Config.m_PushinPetEmote)
		Emote = PlayerEmote;

	const CAnimState *pState;
	if(g_Config.m_PushinPetMode == 0) // flying — idle
	{
		pState = CAnimState::GetIdle();
	}
	else // walking — build walk/idle/air animation
	{
		// Replicate the CPlayers animation logic.
		const bool Stationary = !PlayerMoving;
		const bool InAir = PlayerInAir;
		// Walk time based on pet position (not player) so the feet animate.
		float WalkTime = std::fmod(m_PetPos.x, 100.0f) / 100.0f;
		if(WalkTime < 0.0f)
			WalkTime += 1.0f;

		m_WalkState.Set(&g_pData->m_aAnimations[ANIM_BASE], 0.0f);
		if(InAir)
			m_WalkState.Add(&g_pData->m_aAnimations[ANIM_INAIR], 0.0f, 1.0f);
		else if(Stationary)
			m_WalkState.Add(&g_pData->m_aAnimations[ANIM_IDLE], 0.0f, 1.0f);
		else
			m_WalkState.Add(&g_pData->m_aAnimations[ANIM_WALK], WalkTime, 1.0f);
		pState = &m_WalkState;
	}

	RenderTools()->RenderTee(pState, &Info, Emote, Dir, m_PetPos);
}
