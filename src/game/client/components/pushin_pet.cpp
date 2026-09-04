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

	// --- Player data ---
	const vec2 PlayerPos = GameClient()->m_aClients[LocalId].m_RenderPos;
	int PlayerEmote = EMOTE_NORMAL;
	if(GameClient()->m_Snap.m_aCharacters[LocalId].m_Active)
		PlayerEmote = GameClient()->m_Snap.m_aCharacters[LocalId].m_Cur.m_Emote;
	const vec2 AimTarget = GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy];
	const CCharacterCore &PlayerCore = GameClient()->m_aClients[LocalId].m_Predicted;
	const float VelX = PlayerCore.m_Vel.x;
	const bool PlayerMoving = std::abs(VelX) > 1.0f;
	const bool PlayerInAir = !Collision()->IsOnGround(PlayerPos, CCharacterCore::PhysicalSize());

	// Player facing direction
	const float PlayerDir = (AimTarget.x >= PlayerPos.x) ? 1.0f : -1.0f;

	// --- Target pet position ---
	vec2 TargetPos;
	const float OffsetX = (float)g_Config.m_PushinPetOffsetX;
	const float OffsetY = (float)g_Config.m_PushinPetOffsetY;
	const float PetSize = 64.0f * ((float)g_Config.m_PushinPetSize / 100.0f);

	if(g_Config.m_PushinPetMode == 0) // flying
	{
		TargetPos = PlayerPos + vec2(OffsetX, OffsetY);
		if(g_Config.m_PushinPetBob)
		{
			m_BobPhase += Client()->RenderFrameTime() * 3.0f;
			TargetPos.y += std::sin(m_BobPhase) * (float)g_Config.m_PushinPetBobAmount;
		}
	}
	else // walking — pet is on the ground, behind the player
	{
		// Place pet at the same Y as the player (both are body-center).
		// Offset horizontally behind the player based on facing direction.
		// The pet walks on the same surface as the player.
		TargetPos = vec2(PlayerPos.x - PlayerDir * std::abs(OffsetX), PlayerPos.y);
	}

	// --- Smooth position ---
	// For flying: use the full configurable delay.
	// For walking: use a much smaller delay (1/5 of configured) so the pet
	// stays glued to the ground and doesn't float. The pet should feel like
	// it's walking right behind you, not drifting.
	if(!m_Init)
	{
		m_PetPos = TargetPos;
		m_Init = true;
	}
	else
	{
		float DelaySec = std::max(0.01f, (float)g_Config.m_PushinPetDelay / 100.0f);
		if(g_Config.m_PushinPetMode == 1) // walking — 5x faster follow
			DelaySec *= 0.2f;
		const float Dt = Client()->RenderFrameTime();
		const float Factor = 1.0f - std::exp(-Dt / DelaySec);
		m_PetPos = mix(m_PetPos, TargetPos, Factor);
	}

	// --- Smooth look direction (0.1s) ---
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
	Info.m_Size = PetSize;

	// --- Animation ---
	int Emote = EMOTE_NORMAL;
	if(g_Config.m_PushinPetEmote)
		Emote = PlayerEmote;
	vec2 Dir = g_Config.m_PushinPetLook ? m_LookDir : vec2(PlayerDir, 0.0f);

	const CAnimState *pState;
	if(g_Config.m_PushinPetMode == 0) // flying — idle
	{
		pState = CAnimState::GetIdle();
	}
	else // walking — full walk animation like CPlayers
	{
		// Check if the PET is on the ground (not the player).
		const bool PetInAir = !Collision()->IsOnGround(m_PetPos, PetSize);
		const bool PetStationary = std::abs(m_PetPos.x - m_LastPetX) < 0.5f;
		m_LastPetX = m_PetPos.x;

		// Walk time based on pet's x position (same formula as CPlayers).
		float WalkTime = std::fmod(m_PetPos.x, 100.0f) / 100.0f;
		if(WalkTime < 0.0f)
			WalkTime += 1.0f;

		m_WalkState.Set(&g_pData->m_aAnimations[ANIM_BASE], 0.0f);
		if(PetInAir)
			m_WalkState.Add(&g_pData->m_aAnimations[ANIM_INAIR], 0.0f, 1.0f);
		else if(PetStationary)
			m_WalkState.Add(&g_pData->m_aAnimations[ANIM_IDLE], 0.0f, 1.0f);
		else
			m_WalkState.Add(&g_pData->m_aAnimations[ANIM_WALK], WalkTime, 1.0f);
		pState = &m_WalkState;
	}

	RenderTools()->RenderTee(pState, &Info, Emote, Dir, m_PetPos);
}
