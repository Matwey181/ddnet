/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

// Pushin client — Pet feature.
//
// Walking mode: the pet replays the player's recorded path (position +
// grounded state + freeze state) with a configurable delay. It uses
// real physics (gravity, collision, MoveBox) and can jump, double-jump,
// and hook to reach platforms. When the player is frozen, the pet's
// skin changes to x_ninja like the player's.

#include "pushin_pet.h"

#include <base/math.h>
#include <engine/shared/config.h>
#include <game/client/animstate.h>
#include <game/client/components/controls.h>
#include <game/client/components/effects.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/client/skin.h>
#include <generated/client_data.h>

// History buffer: stores player state for path replay.
constexpr int PET_HISTORY_SIZE = 180; // ~3s at 60fps
struct SPetHistoryEntry
{
	vec2 m_Pos;
	bool m_Grounded;
	bool m_Frozen;
};
static SPetHistoryEntry s_aHistory[PET_HISTORY_SIZE];
static int s_HistoryHead = 0;
static int s_HistoryCount = 0;

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

	const vec2 PlayerPos = GameClient()->m_aClients[LocalId].m_RenderPos;
	int PlayerEmote = EMOTE_NORMAL;
	if(GameClient()->m_Snap.m_aCharacters[LocalId].m_Active)
		PlayerEmote = GameClient()->m_Snap.m_aCharacters[LocalId].m_Cur.m_Emote;
	const vec2 AimTarget = GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy];
	const float PlayerDir = (AimTarget.x >= PlayerPos.x) ? 1.0f : -1.0f;

	// Player grounded + freeze state
	const bool PlayerGrounded = Collision()->IsOnGround(PlayerPos, CCharacterCore::PhysicalSize());
	bool PlayerFrozen = false;
	if(GameClient()->m_Snap.m_aCharacters[LocalId].m_HasExtendedData)
		PlayerFrozen = GameClient()->m_Snap.m_aCharacters[LocalId].m_ExtendedData.m_FreezeEnd != 0;
	if(GameClient()->m_aClients[LocalId].m_Predicted.m_FreezeEnd != 0)
		PlayerFrozen = true;

	// --- Record into history ---
	s_aHistory[s_HistoryHead].m_Pos = PlayerPos;
	s_aHistory[s_HistoryHead].m_Grounded = PlayerGrounded;
	s_aHistory[s_HistoryHead].m_Frozen = PlayerFrozen;
	s_HistoryHead = (s_HistoryHead + 1) % PET_HISTORY_SIZE;
	if(s_HistoryCount < PET_HISTORY_SIZE)
		s_HistoryCount++;

	const float Dt = Client()->RenderFrameTime();
	const float PetSize = 64.0f * ((float)g_Config.m_PushinPetSize / 100.0f);
	const float PetCollideSize = std::max(14.0f, PetSize * 0.44f);

	// --- Get delayed state from history ---
	const int DelayFrames = std::clamp((int)((float)g_Config.m_PushinPetDelay / 100.0f * 60.0f), 0, PET_HISTORY_SIZE - 1);
	vec2 TargetPos = PlayerPos;
	bool TargetGrounded = PlayerGrounded;
	bool TargetFrozen = PlayerFrozen;
	if(s_HistoryCount > DelayFrames)
	{
		const int Idx = (s_HistoryHead - 1 - DelayFrames + PET_HISTORY_SIZE) % PET_HISTORY_SIZE;
		TargetPos = s_aHistory[Idx].m_Pos;
		TargetGrounded = s_aHistory[Idx].m_Grounded;
		TargetFrozen = s_aHistory[Idx].m_Frozen;
	}

	if(g_Config.m_PushinPetMode == 0) // ===== FLYING =====
	{
		vec2 FlyTarget = TargetPos + vec2((float)g_Config.m_PushinPetOffsetX, (float)g_Config.m_PushinPetOffsetY);
		if(g_Config.m_PushinPetBob)
		{
			m_BobPhase += Dt * 3.0f;
			FlyTarget.y += std::sin(m_BobPhase) * (float)g_Config.m_PushinPetBobAmount;
		}
		if(!m_Init)
		{
			m_PetPos = FlyTarget;
			m_Init = true;
		}
		else
		{
			const float DelaySec = std::max(0.01f, (float)g_Config.m_PushinPetDelay / 100.0f);
			const float Factor = 1.0f - std::exp(-Dt / DelaySec);
			m_PetPos = mix(m_PetPos, FlyTarget, Factor);
		}
	}
	else // ===== WALKING (smart physics) =====
	{
		if(!m_Init)
		{
			m_PetPos = PlayerPos;
			m_PetVel = vec2(0.0f, 0.0f);
			m_JumpsLeft = 2;
			m_HookState = 0;
			m_HookPos = m_PetPos;
			m_Init = true;
		}

		const vec2 ToTarget = TargetPos - m_PetPos;
		const float DistX = std::abs(ToTarget.x);
		const float Dist = length(ToTarget);
		const float TargetDir = (ToTarget.x > 0.0f) ? 1.0f : (ToTarget.x < 0.0f ? -1.0f : 0.0f);
		const bool OnGround = Collision()->IsOnGround(m_PetPos, PetCollideSize);

		if(OnGround)
			m_JumpsLeft = 2;

		// --- Horizontal movement ---
		const float MinDist = 40.0f;
		const float RunSpeed = 600.0f;
		if(DistX > MinDist + 10.0f)
			m_PetVel.x = TargetDir * RunSpeed;
		else if(DistX < MinDist - 10.0f)
			m_PetVel.x = -TargetDir * RunSpeed * 0.5f;
		else
			m_PetVel.x *= 0.5f;

		// --- Gravity ---
		m_PetVel.y += 900.0f * Dt;

		// --- SMART JUMP (replay player jumps + obstacle detection) ---
		if(Dist > MinDist && m_JumpsLeft > 0)
		{
			bool ShouldJump = false;

			// REPLAY: if the player was NOT grounded at this point in
			// history (i.e. they jumped), the pet should also jump.
			if(OnGround && !TargetGrounded)
				ShouldJump = true;

			// Wall ahead — trace at body center height.
			if(OnGround && DistX > 5.0f)
			{
				const vec2 CheckPos = m_PetPos + vec2(TargetDir * PetCollideSize * 0.7f, 0.0f);
				if(Collision()->CheckPoint(CheckPos))
					ShouldJump = true;
			}

			// Target is above.
			if(OnGround && ToTarget.y < -25.0f)
				ShouldJump = true;

			// Stuck: not moving but target is far.
			if(OnGround && Dist > 80.0f && std::abs(m_PetVel.x) < 80.0f)
				ShouldJump = true;

			// Double jump: in air, target above, near apex.
			if(!OnGround && m_JumpsLeft >= 1 && ToTarget.y < -30.0f && m_PetVel.y > -50.0f && m_PetVel.y < 200.0f)
				ShouldJump = true;

			if(ShouldJump)
			{
				bool WasAirJump = !OnGround; // track for particle effect
				m_PetVel.y = -550.0f;
				m_JumpsLeft--;

				// Air jump particles — same effect as the player.
				if(WasAirJump)
				{
					GameClient()->m_Effects.AirJump(m_PetPos, 1.0f, 0.7f);
				}
			}
		}

		// --- SMART HOOK ---
		bool ShouldHook = false;

		// Target high above and out of jumps.
		if(ToTarget.y < -60.0f && Dist > 100.0f && m_JumpsLeft == 0)
			ShouldHook = true;

		// Stuck for too long.
		m_StuckTimer = (Dist > 80.0f && std::abs(m_PetVel.x) < 30.0f && OnGround) ? m_StuckTimer + Dt : 0.0f;
		if(m_StuckTimer > 0.5f)
			ShouldHook = true;

		if(ShouldHook && m_HookState == 0 && m_HookCooldown <= 0.0f)
		{
			// Try 3 angles.
			for(int attempt = 0; attempt < 3 && m_HookState == 0; attempt++)
			{
				vec2 HookDir;
				if(attempt == 0)
					HookDir = normalize(ToTarget);
				else if(attempt == 1)
					HookDir = normalize(vec2(ToTarget.x * 0.3f, ToTarget.y - 100.0f));
				else
					HookDir = vec2(0.0f, -1.0f);

				vec2 HookEnd = m_PetPos + HookDir * 500.0f;
				vec2 OutCol, OutBeforeCol;
				int Hit = Collision()->IntersectLine(m_PetPos, HookEnd, &OutCol, &OutBeforeCol);
				if(Hit != 0 && OutCol.y < m_PetPos.y - 5.0f)
				{
					m_HookState = 2;
					m_HookPos = OutCol;
					m_HookTimer = 0.8f;
					m_HookCooldown = 1.5f;
				}
			}
		}

		// --- Apply hook pull ---
		if(m_HookState == 2 && m_HookTimer > 0.0f)
		{
			m_HookTimer -= Dt;
			vec2 ToHook = m_HookPos - m_PetPos;
			float HookDist = length(ToHook);
			if(HookDist > 10.0f)
			{
				vec2 HookDir = normalize(ToHook);
				m_PetVel.x += HookDir.x * 1200.0f * Dt;
				m_PetVel.y += HookDir.y * 1200.0f * Dt;
			}
			if(m_HookTimer <= 0.0f || HookDist < 20.0f)
			{
				m_HookState = 0;
				m_HookCooldown = 1.0f;
			}
		}
		m_HookCooldown -= Dt;

		// --- Clamp velocity ---
		m_PetVel.x = std::clamp(m_PetVel.x, -800.0f, 800.0f);
		m_PetVel.y = std::clamp(m_PetVel.y, -800.0f, 800.0f);

		// --- Move with collision ---
		bool Grounded = false;
		vec2 VelPerFrame = m_PetVel * Dt;
		Collision()->MoveBox(&m_PetPos, &VelPerFrame, vec2(PetCollideSize, PetCollideSize), vec2(0.0f, 0.0f), &Grounded);
		m_PetVel = VelPerFrame / std::max(Dt, 0.001f);
		if(Grounded && m_PetVel.y > 0.0f)
			m_PetVel.y = 0.0f;

		// --- Render hook with game textures ---
		if(m_HookState == 2)
		{
			const float HookDist = length(m_HookPos - m_PetPos);
			// Chain direction: from hook point to pet (same as player code).
			const vec2 ChainDir = (HookDist > 0.1f) ? normalize(m_PetPos - m_HookPos) : vec2(1.0f, 0.0f);
			const float HookAngle = angle(ChainDir) + pi;

			// Hook head
			Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHookHead);
			Graphics()->QuadsSetRotation(HookAngle);
			Graphics()->QuadsBegin();
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
			IGraphics::CQuadItem HeadQuad(m_HookPos.x - 8.0f, m_HookPos.y - 8.0f, 16.0f, 16.0f);
			Graphics()->QuadsDrawTL(&HeadQuad, 1);
			Graphics()->QuadsEnd();

			// Chain
			Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHookChain);
			Graphics()->QuadsSetRotation(HookAngle);
			Graphics()->QuadsBegin();
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
			for(float f = 12.0f; f < HookDist; f += 12.0f)
			{
				vec2 p = m_HookPos + ChainDir * f;
				IGraphics::CQuadItem Seg(p.x - 6.0f, p.y - 6.0f, 12.0f, 12.0f);
				Graphics()->QuadsDrawTL(&Seg, 1);
			}
			Graphics()->QuadsEnd();
			Graphics()->QuadsSetRotation(0.0f);
		}
	}

	// --- Smooth look direction (0.1s) ---
	const vec2 PetToAimRaw = AimTarget - m_PetPos;
	vec2 TargetDir2(1.0f, 0.0f);
	if(length(PetToAimRaw) > 0.001f)
		TargetDir2 = normalize(PetToAimRaw);
	if(!m_LookInit)
	{
		m_LookDir = TargetDir2;
		m_LookInit = true;
	}
	else
	{
		const float Factor = 1.0f - std::exp(-Dt / 0.1f);
		m_LookDir = normalize(mix(m_LookDir, TargetDir2, Factor));
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

	// --- Freeze: if the player was frozen (at the delayed point), switch
	// the pet's skin to x_ninja like the game does for frozen players. ---
	if(g_Config.m_PushinPetMode == 1 && TargetFrozen)
	{
		const CSkin *pNinjaSkin = GameClient()->m_Skins.Find("x_ninja");
		if(pNinjaSkin != nullptr)
		{
			Info.Apply(pNinjaSkin);
			Info.m_ColorBody = ColorRGBA(1.0f, 1.0f, 1.0f);
			Info.m_ColorFeet = ColorRGBA(1.0f, 1.0f, 1.0f);
			Info.m_TeeRenderFlags |= TEE_EFFECT_FROZEN | TEE_NO_WEAPON;
		}
	}

	// --- Animation ---
	int Emote = EMOTE_NORMAL;
	if(g_Config.m_PushinPetEmote)
		Emote = PlayerEmote;
	vec2 Dir = g_Config.m_PushinPetLook ? m_LookDir : vec2(PlayerDir, 0.0f);

	if(g_Config.m_PushinPetMode == 1 && m_HookState == 2)
	{
		vec2 HookLook = m_HookPos - m_PetPos;
		if(length(HookLook) > 0.001f)
			Dir = normalize(HookLook);
	}

	const CAnimState *pState;
	if(g_Config.m_PushinPetMode == 0) // flying
	{
		pState = CAnimState::GetIdle();
	}
	else // walking
	{
		const bool OnGround = Collision()->IsOnGround(m_PetPos, PetCollideSize);
		const bool Moving = std::abs(m_PetVel.x) > 50.0f;
		float WalkTime = std::fmod(m_PetPos.x, 100.0f) / 100.0f;
		if(WalkTime < 0.0f)
			WalkTime += 1.0f;

		m_WalkState.Set(&g_pData->m_aAnimations[ANIM_BASE], 0.0f);
		if(!OnGround)
			m_WalkState.Add(&g_pData->m_aAnimations[ANIM_INAIR], 0.0f, 1.0f);
		else if(!Moving)
			m_WalkState.Add(&g_pData->m_aAnimations[ANIM_IDLE], 0.0f, 1.0f);
		else
			m_WalkState.Add(&g_pData->m_aAnimations[ANIM_WALK], WalkTime, 1.0f);
		pState = &m_WalkState;
	}

	RenderTools()->RenderTee(pState, &Info, Emote, Dir, m_PetPos);
}
