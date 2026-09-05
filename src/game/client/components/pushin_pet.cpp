/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */

// Pushin client — Pet feature.
//
// Walking mode AI: state machine + BFS pathfinding + smooth physics.
//
// States:
//   CHASE  — run toward the BFS waypoint, jump over walls, drop off edges
//   CLIMB  — target is above, use jumps + hook to climb up
//   DROP   — target is below, walk to edge and fall off
//   STUCK  — hasn't moved for 0.5s, try jump then hook then teleport
//
// Movement uses smooth acceleration (not snap) to avoid jerky motion.
// BFS runs every 0.3s and provides a waypoint. The pet steers toward
// the waypoint with proportional control.

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

#include <queue>

constexpr int PET_HISTORY_SIZE = 180;
constexpr float TILE_SIZE = 32.0f;
constexpr float BFS_RECOMPUTE_INTERVAL = 0.3f;
constexpr int BFS_RADIUS = 60;
constexpr int BFS_SIZE = BFS_RADIUS * 2 + 1;
constexpr int BFS_MAX_NODES = 15000;

struct SPetHistoryEntry { vec2 m_Pos; bool m_Grounded; bool m_Frozen; };
static SPetHistoryEntry s_aHistory[PET_HISTORY_SIZE];
static int s_HistoryHead = 0, s_HistoryCount = 0;

// BFS node pool
struct SBfsNode { int16_t x, y, parent; };
static SBfsNode aBfsNodes[BFS_MAX_NODES];
static bool aBfsVisited[BFS_SIZE * BFS_SIZE];

static inline void WorldToTile(vec2 Pos, int &Tx, int &Ty) { Tx = (int)(Pos.x / TILE_SIZE); Ty = (int)(Pos.y / TILE_SIZE); }
static inline vec2 TileToWorld(int Tx, int Ty) { return vec2(Tx * TILE_SIZE + TILE_SIZE/2.0f, Ty * TILE_SIZE + TILE_SIZE/2.0f); }
static inline bool TilePassable(class CCollision *pCol, int Tx, int Ty) {
	if(Tx < 0 || Ty < 0 || Tx >= pCol->GetWidth() || Ty >= pCol->GetHeight()) return false;
	return !pCol->IsSolid(Tx, Ty);
}

// Pet AI states
enum EPetState { PET_CHASE = 0, PET_CLIMB, PET_DROP, PET_STUCK };

void CPushinPet::OnRender()
{
	if(!g_Config.m_PushinPetEnabled) return;
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK) return;
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	if(LocalId < 0 || LocalId >= MAX_CLIENTS) return;
	if(!GameClient()->m_aClients[LocalId].m_Active) return;

	const vec2 PlayerPos = GameClient()->m_aClients[LocalId].m_RenderPos;
	int PlayerEmote = EMOTE_NORMAL;
	if(GameClient()->m_Snap.m_aCharacters[LocalId].m_Active)
		PlayerEmote = GameClient()->m_Snap.m_aCharacters[LocalId].m_Cur.m_Emote;
	const vec2 AimTarget = GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy];
	const float PlayerDir = (AimTarget.x >= PlayerPos.x) ? 1.0f : -1.0f;
	const bool PlayerGrounded = Collision()->IsOnGround(PlayerPos, CCharacterCore::PhysicalSize());
	bool PlayerFrozen = false;
	if(GameClient()->m_Snap.m_aCharacters[LocalId].m_HasExtendedData)
		PlayerFrozen = GameClient()->m_Snap.m_aCharacters[LocalId].m_ExtendedData.m_FreezeEnd != 0;
	if(GameClient()->m_aClients[LocalId].m_Predicted.m_FreezeEnd != 0) PlayerFrozen = true;

	s_aHistory[s_HistoryHead] = {PlayerPos, PlayerGrounded, PlayerFrozen};
	s_HistoryHead = (s_HistoryHead + 1) % PET_HISTORY_SIZE;
	if(s_HistoryCount < PET_HISTORY_SIZE) s_HistoryCount++;

	const float Dt = Client()->RenderFrameTime();
	const float PetSize = 64.0f * ((float)g_Config.m_PushinPetSize / 100.0f);
	const float PetCollideSize = std::max(14.0f, PetSize * 0.44f);

	const int DelayFrames = std::clamp((int)((float)g_Config.m_PushinPetDelay / 100.0f * 60.0f), 0, PET_HISTORY_SIZE - 1);
	vec2 TargetPos = PlayerPos; bool TargetGrounded = PlayerGrounded; bool TargetFrozen = PlayerFrozen;
	if(s_HistoryCount > DelayFrames) {
		const int Idx = (s_HistoryHead - 1 - DelayFrames + PET_HISTORY_SIZE) % PET_HISTORY_SIZE;
		TargetPos = s_aHistory[Idx].m_Pos; TargetGrounded = s_aHistory[Idx].m_Grounded; TargetFrozen = s_aHistory[Idx].m_Frozen;
	}

	if(g_Config.m_PushinPetMode == 0) // FLYING
	{
		vec2 FlyTarget = TargetPos + vec2((float)g_Config.m_PushinPetOffsetX, (float)g_Config.m_PushinPetOffsetY);
		if(g_Config.m_PushinPetBob) { m_BobPhase += Dt * 3.0f; FlyTarget.y += std::sin(m_BobPhase) * (float)g_Config.m_PushinPetBobAmount; }
		if(!m_Init) { m_PetPos = FlyTarget; m_Init = true; }
		else { const float DelaySec = std::max(0.01f, (float)g_Config.m_PushinPetDelay / 100.0f);
			m_PetPos = mix(m_PetPos, FlyTarget, 1.0f - std::exp(-Dt / DelaySec)); }
	}
	else // WALKING — state machine + BFS + smooth physics
	{
		if(!m_Init) { m_PetPos = PlayerPos; m_PetVel = vec2(0,0); m_JumpsLeft = 2; m_HookState = 0; m_HookPos = m_PetPos;
			m_BfsTimer = 0; m_StuckTimer = 0; m_LastPetX = m_PetPos.x; m_PetState = PET_CHASE; m_Init = true; }

		const vec2 ToTarget = TargetPos - m_PetPos;
		const float DistX = std::abs(ToTarget.x);
		const float DistY = ToTarget.y;
		const float Dist = length(ToTarget);
		const float MoveDir = (ToTarget.x > 0.0f) ? 1.0f : (ToTarget.x < 0.0f ? -1.0f : 0.0f);
		const bool OnGround = Collision()->IsOnGround(m_PetPos, PetCollideSize);
		if(OnGround) m_JumpsLeft = 2;

		// === BFS pathfinding (every 0.3s) ===
		m_BfsTimer -= Dt;
		if(m_BfsTimer <= 0.0f) {
			m_BfsTimer = BFS_RECOMPUTE_INTERVAL;
			int PetTx, PetTy, TargTx, TargTy;
			WorldToTile(m_PetPos, PetTx, PetTy);
			WorldToTile(TargetPos, TargTx, TargTy);
			int NodeCount = 0;
			aBfsNodes[NodeCount++] = {(int16_t)PetTx, (int16_t)PetTy, -1};
			for(int i = 0; i < BFS_SIZE * BFS_SIZE; i++) aBfsVisited[i] = false;
			auto VIdx = [](int x, int y) -> int { int rx=x+BFS_RADIUS, ry=y+BFS_RADIUS; if(rx<0||ry<0||rx>=BFS_SIZE||ry>=BFS_SIZE) return -1; return ry*BFS_SIZE+rx; };
			auto SetV = [&](int x,int y){ int i=VIdx(x,y); if(i>=0) aBfsVisited[i]=true; };
			auto IsV = [&](int x,int y)->bool{ int i=VIdx(x,y); return i>=0&&aBfsVisited[i]; };
			SetV(PetTx, PetTy);
			std::queue<int> Q; Q.push(0);
			bool Found = false; int GoalIdx = -1;
			constexpr int dx[] = {0,0,1,-1,1,-1,1,-1};
			constexpr int dy[] = {-1,1,0,0,-1,-1,1,1};
			int Iters = 0;
			while(!Q.empty() && NodeCount < BFS_MAX_NODES && Iters < BFS_MAX_NODES) {
				Iters++;
				int ci = Q.front(); Q.pop();
				if(aBfsNodes[ci].x == TargTx && aBfsNodes[ci].y == TargTy) { Found = true; GoalIdx = ci; break; }
				for(int d = 0; d < 8; d++) {
					int nx = aBfsNodes[ci].x + dx[d], ny = aBfsNodes[ci].y + dy[d];
					if(IsV(nx, ny)) continue;
					if(!TilePassable(Collision(), nx, ny)) { SetV(nx,ny); continue; }
					if(d >= 4 && (!TilePassable(Collision(), aBfsNodes[ci].x+dx[d], aBfsNodes[ci].y) || !TilePassable(Collision(), aBfsNodes[ci].x, aBfsNodes[ci].y+dy[d]))) { SetV(nx,ny); continue; }
					SetV(nx,ny);
					aBfsNodes[NodeCount] = {(int16_t)nx, (int16_t)ny, (int16_t)ci};
					Q.push(NodeCount); NodeCount++;
				}
			}
			m_HasBfsPath = false;
			if(Found && GoalIdx > 0) {
				int t = GoalIdx;
				while(aBfsNodes[t].parent > 0) t = aBfsNodes[t].parent;
				m_BfsWaypoint = TileToWorld(aBfsNodes[t].x, aBfsNodes[t].y);
				m_HasBfsPath = true;
			}
		}

		// === STATE MACHINE ===
		// Decide state based on terrain and target position.
		vec2 NavTarget = m_HasBfsPath ? m_BfsWaypoint : TargetPos;
		const vec2 ToNav = NavTarget - m_PetPos;
		const float NavDir = (ToNav.x > 0.0f) ? 1.0f : (ToNav.x < 0.0f ? -1.0f : 0.0f);

		// Wall check
		const vec2 WallCheck = m_PetPos + vec2(MoveDir * PetCollideSize * 0.7f, 0.0f);
		const bool WallAhead = Collision()->CheckPoint(WallCheck);

		// Stuck check
		if(std::abs(m_PetPos.x - m_LastPetX) < 1.0f && OnGround && Dist > 60.0f)
			m_StuckTimer += Dt;
		else
			m_StuckTimer = std::max(0.0f, m_StuckTimer - Dt * 2.0f);
		m_LastPetX = m_PetPos.x;

		// State selection
		if(m_StuckTimer > 0.5f)
			m_PetState = PET_STUCK;
		else if(DistY > 40.0f && OnGround && Dist > 50.0f)
			m_PetState = PET_DROP;
		else if(DistY < -30.0f && Dist > 50.0f)
			m_PetState = PET_CLIMB;
		else
			m_PetState = PET_CHASE;

		// === PHYSICS — smooth acceleration ===
		const float MinDist = 40.0f;
		const float MaxSpeed = 600.0f;
		const float Accel = 8000.0f * Dt; // how fast to reach max speed
		const float Decel = 6000.0f * Dt; // how fast to slow down

		// Desired horizontal velocity
		float DesiredVelX = 0.0f;
		if(DistX > MinDist + 10.0f)
			DesiredVelX = NavDir * MaxSpeed;
		else if(DistX < MinDist - 10.0f)
			DesiredVelX = -NavDir * MaxSpeed * 0.5f;
		else
			DesiredVelX = 0.0f;

		// DROP state: always run toward target, even off edges
		if(m_PetState == PET_DROP)
			DesiredVelX = MoveDir * MaxSpeed;

		// Smoothly approach desired velocity (no snapping)
		if(std::abs(m_PetVel.x - DesiredVelX) < Accel)
			m_PetVel.x = DesiredVelX;
		else if(m_PetVel.x < DesiredVelX)
			m_PetVel.x += Accel;
		else
			m_PetVel.x -= Decel;

		// Gravity
		m_PetVel.y += 900.0f * Dt;

		// === JUMPING ===
		bool WantJump = false;
		bool WantDrop = (m_PetState == PET_DROP);

		if(Dist > MinDist && m_JumpsLeft > 0 && !WantDrop) {
			// CHASE: jump over walls, replay player jumps
			if(m_PetState == PET_CHASE) {
				if(OnGround && WallAhead) WantJump = true;
				if(OnGround && !TargetGrounded) WantJump = true; // replay
				if(OnGround && DistY < -25.0f) WantJump = true;
			}
			// CLIMB: jump + double jump to reach higher ground
			if(m_PetState == PET_CLIMB) {
				if(OnGround) WantJump = true;
				if(!OnGround && m_JumpsLeft >= 1 && DistY < -30.0f && m_PetVel.y > -50.0f && m_PetVel.y < 200.0f) WantJump = true;
			}
			// STUCK: jump to unstick
			if(m_PetState == PET_STUCK && OnGround) WantJump = true;
		}

		if(WantJump) {
			bool WasAir = !OnGround;
			m_PetVel.y = -550.0f;
			m_JumpsLeft--;
			if(WasAir) GameClient()->m_Effects.AirJump(m_PetPos, 1.0f, 0.7f);
		}

		// === HOOK ===
		bool WantHook = false;
		if(m_PetState == PET_CLIMB && m_JumpsLeft == 0 && DistY < -60.0f && Dist > 100.0f) WantHook = true;
		if(m_PetState == PET_STUCK && m_StuckTimer > 0.3f) WantHook = true;

		// TELEPORT: stuck >1.5s → teleport
		if(m_StuckTimer > 1.5f) {
			m_PetPos = PlayerPos;
			m_PetVel = vec2(0, 0);
			m_StuckTimer = 0;
			m_HookState = 0;
			m_JumpsLeft = 2;
			WantHook = false;
		}

		if(WantHook && m_HookState == 0 && m_HookCooldown <= 0.0f) {
			const vec2 HookDirs[] = {
				normalize(ToTarget),
				normalize(vec2(-1.0f, -1.0f)),
				normalize(vec2(1.0f, -1.0f)),
				vec2(0.0f, -1.0f),
				normalize(vec2(ToTarget.x * 0.3f, ToTarget.y - 100.0f))
			};
			for(int a = 0; a < 5 && m_HookState == 0; a++) {
				vec2 End = m_PetPos + HookDirs[a] * 500.0f;
				vec2 Out, OutBefore;
				if(Collision()->IntersectLine(m_PetPos, End, &Out, &OutBefore) != 0 && Out.y < m_PetPos.y - 5.0f) {
					m_HookState = 2; m_HookPos = Out; m_HookTimer = 0.8f; m_HookCooldown = 1.5f;
				}
			}
		}

		// Hook pull
		if(m_HookState == 2 && m_HookTimer > 0.0f) {
			m_HookTimer -= Dt;
			vec2 ToHook = m_HookPos - m_PetPos;
			float HDist = length(ToHook);
			if(HDist > 10.0f) { vec2 HD = normalize(ToHook); m_PetVel.x += HD.x * 1200.0f * Dt; m_PetVel.y += HD.y * 1200.0f * Dt; }
			if(m_HookTimer <= 0.0f || HDist < 20.0f) { m_HookState = 0; m_HookCooldown = 1.0f; }
		}
		m_HookCooldown -= Dt;

		// Clamp
		m_PetVel.x = std::clamp(m_PetVel.x, -800.0f, 800.0f);
		m_PetVel.y = std::clamp(m_PetVel.y, -800.0f, 800.0f);

		// Move
		bool Grounded = false;
		vec2 VPF = m_PetVel * Dt;
		Collision()->MoveBox(&m_PetPos, &VPF, vec2(PetCollideSize, PetCollideSize), vec2(0,0), &Grounded);
		m_PetVel = VPF / std::max(Dt, 0.001f);
		if(Grounded && m_PetVel.y > 0.0f) m_PetVel.y = 0.0f;

		// === Render hook ===
		if(m_HookState == 2) {
			float HDist = length(m_HookPos - m_PetPos);
			vec2 CD = (HDist > 0.1f) ? normalize(m_PetPos - m_HookPos) : vec2(1,0);
			float HA = angle(CD) + pi;
			Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHookHead);
			Graphics()->QuadsSetRotation(HA);
			Graphics()->QuadsBegin(); Graphics()->SetColor(1,1,1,1);
			IGraphics::CQuadItem HQ(m_HookPos.x-8, m_HookPos.y-8, 16, 16);
			Graphics()->QuadsDrawTL(&HQ, 1); Graphics()->QuadsEnd();
			Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHookChain);
			Graphics()->QuadsSetRotation(HA);
			Graphics()->QuadsBegin(); Graphics()->SetColor(1,1,1,1);
			for(float f = 12.0f; f < HDist; f += 12.0f) {
				vec2 p = m_HookPos + CD * f;
				IGraphics::CQuadItem SQ(p.x-6, p.y-6, 12, 12);
				Graphics()->QuadsDrawTL(&SQ, 1);
			}
			Graphics()->QuadsEnd(); Graphics()->QuadsSetRotation(0.0f);
		}
	}

	// === Look direction ===
	const vec2 PetToAim = AimTarget - m_PetPos;
	vec2 TDir2(1,0);
	if(length(PetToAim) > 0.001f) TDir2 = normalize(PetToAim);
	if(!m_LookInit) { m_LookDir = TDir2; m_LookInit = true; }
	else m_LookDir = normalize(mix(m_LookDir, TDir2, 1.0f - std::exp(-Dt / 0.1f)));

	// === Skin + render info ===
	const CSkin *pSkin = GameClient()->m_Skins.Find(g_Config.m_PushinPetSkin);
	if(!pSkin) pSkin = GameClient()->m_Skins.Find("default");
	if(!pSkin) return;
	CTeeRenderInfo Info; Info.Apply(pSkin); Info.m_Size = PetSize;

	if(g_Config.m_PushinPetMode == 1 && TargetFrozen) {
		const CSkin *pN = GameClient()->m_Skins.Find("x_ninja");
		if(pN) { Info.Apply(pN); Info.m_ColorBody = ColorRGBA(1,1,1); Info.m_ColorFeet = ColorRGBA(1,1,1);
			Info.m_TeeRenderFlags |= TEE_EFFECT_FROZEN | TEE_NO_WEAPON; }
	}

	int Emote = EMOTE_NORMAL;
	if(g_Config.m_PushinPetEmote) Emote = PlayerEmote;
	vec2 Dir = g_Config.m_PushinPetLook ? m_LookDir : vec2(PlayerDir, 0);
	if(g_Config.m_PushinPetMode == 1 && m_HookState == 2) {
		vec2 HL = m_HookPos - m_PetPos;
		if(length(HL) > 0.001f) Dir = normalize(HL);
	}

	const CAnimState *pState;
	if(g_Config.m_PushinPetMode == 0) pState = CAnimState::GetIdle();
	else {
		bool OG = Collision()->IsOnGround(m_PetPos, PetCollideSize);
		bool Mov = std::abs(m_PetVel.x) > 50.0f;
		float WT = std::fmod(m_PetPos.x, 100.0f) / 100.0f;
		if(WT < 0.0f) WT += 1.0f;
		m_WalkState.Set(&g_pData->m_aAnimations[ANIM_BASE], 0.0f);
		if(!OG) m_WalkState.Add(&g_pData->m_aAnimations[ANIM_INAIR], 0.0f, 1.0f);
		else if(!Mov) m_WalkState.Add(&g_pData->m_aAnimations[ANIM_IDLE], 0.0f, 1.0f);
		else m_WalkState.Add(&g_pData->m_aAnimations[ANIM_WALK], WT, 1.0f);
		pState = &m_WalkState;
	}
	RenderTools()->RenderTee(pState, &Info, Emote, Dir, m_PetPos);
}
