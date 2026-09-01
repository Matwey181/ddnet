/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "autofinish.h"

#include <base/color.h>
#include <base/math.h>
#include <base/system.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <game/client/components/camera.h>
#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/ui.h>
#include <game/client/ui_rect.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <utility>

namespace
{
	constexpr float CELL = 32.0f;
	constexpr float NODE_REACH_PX = 26.0f;
	constexpr float HOOK_NODE_REACH_PX = 34.0f;
	constexpr float TELE_NODE_REACH_PX = 30.0f;
	constexpr float AIM_MAX_PX = 700.0f;
	constexpr float DEVIATION_REPLAN_PX = 260.0f;
	constexpr int NODE_EXPAND_BUDGET = 200000;
	constexpr int HOOK_WINDOW_TILES = 8; // how far hook edges may reach
	constexpr int HOOK_MIN_SPAN_TILES = 2;
	constexpr int STUCK_SECONDS = 4;
	constexpr int MAX_STUCK_REPLANS = 3;
	constexpr int MAX_PLAN_RETRIES = 3;
	constexpr int PERIODIC_REPLAN_SECONDS = 12;
	constexpr int JUMP_TICKS_BETWEEN = 10;
	constexpr int HOOK_NODE_TIMEOUT_TICKS = 140; // 2.8s per hook node
	constexpr float REVEAL_SECONDS = 0.9f;
}

CAutoFinish::CAutoFinish()
{
}

void CAutoFinish::OnConsoleInit()
{
	Console()->Register("autofinish", "?i[enabled]", CFGFLAG_CLIENT, ConAutoFinish, this, "Toggle the auto-finish bot (plans the shortest route to the finish and walks it)");
}

void CAutoFinish::ConAutoFinish(IConsole::IResult *pResult, void *pUserData)
{
	CAutoFinish *pSelf = (CAutoFinish *)pUserData;
	if(pResult->NumArguments() > 0)
	{
		g_Config.m_ClAutoFinish = pResult->GetInteger(0) ? 1 : 0;
	}
	else
	{
		g_Config.m_ClAutoFinish = g_Config.m_ClAutoFinish ? 0 : 1;
	}
}

void CAutoFinish::OnReset()
{
	m_vPath.clear();
	m_NextNode = 0;
	m_TotalPathLen = 0.0f;
	m_RevealProgress = 0.0f;
	m_RunState = 0;
	m_PlanUsedFreeze = false;
	m_PlanRetries = 0;
	m_NextPlanTryTime = 0;
	m_StuckCount = 0;
	m_HookNodeTicks = 0;
	m_JumpHoldTicks = 0;
	m_BlockedTicks = 0;
	if(m_Active)
	{
		ClearBotInput();
		RequestReplan(true);
	}
}

void CAutoFinish::OnMapLoad()
{
	if(m_Active && !m_vPath.empty())
	{
		RequestReplan(false);
	}
}

void CAutoFinish::ClearBotInput()
{
	if(!GameClient())
	{
		return;
	}
	CControls &Controls = GameClient()->m_Controls;
	const int D = g_Config.m_ClDummy;
	Controls.m_aInputDirectionLeft[D] = 0;
	Controls.m_aInputDirectionRight[D] = 0;
	Controls.m_aInputData[D].m_Jump = 0;
	Controls.m_aInputData[D].m_Hook = 0;
}

void CAutoFinish::SetStatusMessage(const char *pMessage, float Seconds)
{
	str_copy(m_aStatusMessage, pMessage);
	m_StatusMessageTime = Seconds;
}

void CAutoFinish::RequestReplan(bool QuickReveal)
{
	m_RunState = 0;
	m_NextPlanTryTime = 0;
	m_HookNodeTicks = 0;
	m_JumpHoldTicks = 0;
	m_RevealStart = QuickReveal ? std::max(0.4f, m_RevealProgress * 0.7f) : 0.35f;
}

void CAutoFinish::Activate()
{
	m_Active = true;
	m_RunState = 0;
	m_PlanUsedFreeze = false;
	m_PlanRetries = 0;
	m_StuckCount = 0;
	m_BlockedTicks = 0;
	m_AirJumpCooldown = 0;
	m_JumpCooldown = 0;
	m_JumpHoldTicks = 0;
	m_HookNodeTicks = 0;
	m_NextPlanTryTime = 0;
	m_aPlanError[0] = 0;
	m_vPath.clear();
	m_NextNode = 0;
	m_TotalPathLen = 0.0f;
	m_RevealProgress = 0.0f;
	m_RevealStart = 0.0f;
	m_LastProgressTime = time_get();
	m_NextPeriodicReplan = time_get() + (int64_t)time_freq() * PERIODIC_REPLAN_SECONDS;
	if(!m_OldZoomValid)
	{
		m_OldZoom = GameClient()->m_Camera.m_Zoom;
		m_OldZoomValid = true;
	}
	ApplyAutoZoom(true);
}

void CAutoFinish::Deactivate(EDeactivateReason Reason, const char *pMessage)
{
	if(!m_Active)
	{
		return;
	}
	m_Active = false;
	ClearBotInput();
	g_Config.m_ClAutoFinish = 0;
	if(m_OldZoomValid)
	{
		GameClient()->m_Camera.SetZoom(m_OldZoom, 700, true);
		m_OldZoomValid = false;
	}
	if(Reason == REASON_FINISHED)
	{
		SetStatusMessage("AUTO-FINISH: MAP COMPLETED", 6.0f);
	}
	else if(Reason == REASON_DISABLED)
	{
		SetStatusMessage("AUTO-FINISH DISABLED", 2.0f);
	}
	else if(pMessage != nullptr)
	{
		SetStatusMessage(pMessage, 6.0f);
	}
}

void CAutoFinish::ApplyAutoZoom(bool Immediate)
{
	const int ZoomCfg = g_Config.m_ClAutoFinishZoom;
	if(ZoomCfg <= 0 || ZoomCfg >= 100)
	{
		return;
	}
	const float Target = std::clamp(ZoomCfg / 100.0f, 0.2f, 1.0f);
	CCamera &Cam = GameClient()->m_Camera;
	if(std::abs(Cam.m_Zoom - Target) > 0.04f)
	{
		Cam.SetZoom(Target, Immediate ? 250 : 700, true);
	}
}

void CAutoFinish::OnUpdate()
{
	// activation transitions, driven by the config (settings, console, binds)
	if(g_Config.m_ClAutoFinish && !m_Active)
	{
		Activate();
	}
	else if(!g_Config.m_ClAutoFinish && m_Active)
	{
		Deactivate(REASON_DISABLED);
	}

	if(!m_Active)
	{
		return;
	}

	const int64_t Now = time_get();
	const int64_t Freq = time_freq();

	// we need an online game with an alive local character
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	const bool InGame = Client()->State() == IClient::STATE_ONLINE &&
			    LocalId >= 0 &&
			    GameClient()->m_Snap.m_aCharacters[LocalId].m_Active &&
			    !GameClient()->m_Snap.m_SpecInfo.m_Active;
	if(!InGame)
	{
		if(m_RunState == 1)
		{
			m_RunState = 0; // replan once we are back in the game
		}
		ClearBotInput();
		return;
	}

	// waiting for the next plan retry
	if(m_RunState == 2 && Now < m_NextPlanTryTime)
	{
		ClearBotInput();
		return;
	}

	// ---- planning ----------------------------------------------------------
	if(m_RunState != 1)
	{
		const vec2 StartPos = GameClient()->m_PredictedChar.m_Pos;
		bool Found = PlanPath(StartPos, false);
		if(!Found)
		{
			Found = PlanPath(StartPos, true);
		}
		if(Found)
		{
			if(m_vPath.size() <= 1)
			{
				SetStatusMessage("AUTO-FINISH: MAP COMPLETED", 6.0f);
				Deactivate(REASON_FINISHED);
				return;
			}
			m_RunState = 1;
			m_NextNode = 1; // node 0 is the tile we are standing on
			m_LastProgressTime = Now;
			m_NextPeriodicReplan = Now + Freq * PERIODIC_REPLAN_SECONDS;
			m_RevealProgress = m_RevealStart;
			m_PlanRetries = 0;
			m_StuckCount = 0;
			if(m_PlanUsedFreeze)
			{
				SetStatusMessage("AUTO-FINISH: the route unavoidably passes freeze tiles", 4.0f);
			}
		}
		else
		{
			m_PlanRetries++;
			m_RunState = 2;
			m_NextPlanTryTime = Now + Freq * 4;
			if(m_PlanRetries >= MAX_PLAN_RETRIES)
			{
				char aMsg[192];
				str_format(aMsg, sizeof(aMsg), "AUTO-FINISH FAILED: %s", m_aPlanError[0] != 0 ? m_aPlanError : "no route to the finish found");
				SetStatusMessage(aMsg, 6.0f);
				Deactivate(REASON_FAILED, aMsg);
				return;
			}
		}
		if(m_RunState != 1)
		{
			return;
		}
	}

	CControls &Controls = GameClient()->m_Controls;
	const int D = g_Config.m_ClDummy;

	// the game freezes the input while chatting or in menus: wait silently
	if(GameClient()->m_Chat.IsActive() || GameClient()->m_Menus.IsActive())
	{
		m_LastProgressTime = Now;
		return;
	}

	// current player state (prefer prediction, fall back to the snapshot)
	vec2 Pos = GameClient()->m_PredictedChar.m_Pos;
	vec2 Vel = GameClient()->m_PredictedChar.m_Vel;
	const CNetObj_Character &SnapChar = GameClient()->m_Snap.m_aCharacters[LocalId].m_Cur;
	const vec2 SnapPos(SnapChar.m_X, SnapChar.m_Y);
	if(distance(Pos, SnapPos) > 500.0f)
	{
		Pos = SnapPos;
		Vel = vec2(SnapChar.m_VelX / 256.0f, SnapChar.m_VelY / 256.0f);
	}
	const bool Grounded = Collision()->IsOnGround(Pos, 28.0f);

	// frozen players cannot move: hold still and wait it out
	const bool Frozen = GameClient()->m_aClients[LocalId].m_DeepFrozen ||
			    GameClient()->m_aClients[LocalId].m_FreezeEnd > Client()->GameTick(D);
	if(Frozen)
	{
		Controls.m_aInputDirectionLeft[D] = 0;
		Controls.m_aInputDirectionRight[D] = 0;
		Controls.m_aInputData[D].m_Jump = 0;
		Controls.m_aInputData[D].m_Hook = 0;
		m_LastProgressTime = Now; // don't accumulate stuck time while frozen
		return;
	}

	// ---- node bookkeeping --------------------------------------------------
	while(m_NextNode < (int)m_vPath.size())
	{
		const SPathNode &Node = m_vPath[m_NextNode];
		const float Reach = Node.m_Type == NODE_HOOK ? HOOK_NODE_REACH_PX :
							       (Node.m_Type == NODE_TELE ? TELE_NODE_REACH_PX : NODE_REACH_PX);
		if(distance(Pos, Node.m_Pos) < Reach)
		{
			m_NextNode++;
			m_LastProgressTime = Now;
			m_StuckCount = 0;
			m_HookNodeTicks = 0;
			continue;
		}
		break;
	}
	// overshot the node without touching it: move on
	if(m_NextNode + 1 < (int)m_vPath.size() &&
		distance(Pos, m_vPath[m_NextNode + 1].m_Pos) + 14.0f < distance(Pos, m_vPath[m_NextNode].m_Pos))
	{
		m_NextNode++;
		m_LastProgressTime = Now;
		m_HookNodeTicks = 0;
	}

	if(m_NextNode >= (int)m_vPath.size())
	{
		SetStatusMessage("AUTO-FINISH: MAP COMPLETED", 6.0f);
		Deactivate(REASON_FINISHED);
		return;
	}

	const SPathNode &Target = m_vPath[m_NextNode];

	// periodic replanning keeps the route fresh
	if(Now > m_NextPeriodicReplan)
	{
		m_NextPeriodicReplan = Now + Freq * PERIODIC_REPLAN_SECONDS;
		RequestReplan(true);
		return;
	}

	// deviation from the route
	{
		float Best = 1e9f;
		const int From = std::max(0, m_NextNode - 3);
		const int To = std::min((int)m_vPath.size() - 1, m_NextNode + 6);
		for(int i = From; i <= To; i++)
		{
			Best = std::min(Best, distance(Pos, m_vPath[i].m_Pos));
		}
		if(Best > DEVIATION_REPLAN_PX)
		{
			RequestReplan(true);
			return;
		}
	}

	// stuck detection
	if(Now - m_LastProgressTime > Freq * STUCK_SECONDS)
	{
		m_StuckCount++;
		m_LastProgressTime = Now;
		if(m_StuckCount >= MAX_STUCK_REPLANS)
		{
			SetStatusMessage("AUTO-FINISH FAILED: stuck, giving up", 6.0f);
			Deactivate(REASON_FAILED);
			return;
		}
		RequestReplan(true);
		return;
	}

	// keep the auto zoom in place
	if(Now > m_NextZoomCorrect)
	{
		m_NextZoomCorrect = Now + Freq * 8;
		ApplyAutoZoom(false);
	}

	// ---- steering ----------------------------------------------------------
	const float Dx = Target.m_Pos.x - Pos.x;
	int Dir = 0;
	if(std::abs(Dx) > 12.0f)
	{
		Dir = Dx > 0.0f ? 1 : -1;
	}

	// aim at the hook attach point for hook nodes, at the node otherwise
	const vec2 AimWorld = Target.m_Type == NODE_HOOK ? Target.m_HookPos : Target.m_Pos;
	vec2 AimRel = AimWorld - Pos;
	const float AimLen = length(AimRel);
	if(AimLen > AIM_MAX_PX)
	{
		AimRel = AimRel * (AIM_MAX_PX / AimLen);
	}
	Controls.m_aMousePos[D] = AimRel;

	// ---- jump logic --------------------------------------------------------
	bool WantJump = false;
	if(Target.m_Type == NODE_JUMP && Grounded && std::abs(Dx) < 90.0f)
	{
		WantJump = true;
	}
	if(Target.m_Pos.y < Pos.y - 44.0f && std::abs(Dx) < 120.0f)
	{
		WantJump = true;
	}
	// pressing a direction but not moving: probably a small step, jump over it
	if(Dir != 0 && Grounded && std::abs(Vel.x) < 2.0f)
	{
		m_BlockedTicks++;
	}
	else
	{
		m_BlockedTicks = 0;
	}
	if(m_BlockedTicks > 10)
	{
		WantJump = true;
	}

	if(Grounded)
	{
		m_AirJumpCooldown = 0;
	}
	else if(m_AirJumpCooldown > 0)
	{
		m_AirJumpCooldown--;
	}
	if(WantJump && !Grounded)
	{
		if(m_AirJumpCooldown > 0)
		{
			WantJump = false;
		}
		else
		{
			m_AirJumpCooldown = 40; // single air-jump attempt, then wait
		}
	}
	if(WantJump && m_JumpCooldown <= 0)
	{
		m_JumpHoldTicks = 2;
		m_JumpCooldown = JUMP_TICKS_BETWEEN;
	}
	if(m_JumpCooldown > 0)
	{
		m_JumpCooldown--;
	}

	// ---- hook logic --------------------------------------------------------
	bool WantHook = false;
	if(Target.m_Type == NODE_HOOK)
	{
		m_HookNodeTicks++;
		if(m_HookNodeTicks > HOOK_NODE_TIMEOUT_TICKS)
		{
			// hooking did not work out: skip the node and let replanning handle it
			m_NextNode++;
			m_LastProgressTime = Now;
			m_HookNodeTicks = 0;
		}
		else if(distance(Pos, Target.m_HookPos) > 46.0f)
		{
			WantHook = true;
		}
	}
	else
	{
		m_HookNodeTicks = 0;
	}

	// ---- write the input ---------------------------------------------------
	Controls.m_aInputDirectionLeft[D] = Dir < 0 ? 1 : 0;
	Controls.m_aInputDirectionRight[D] = Dir > 0 ? 1 : 0;
	if(m_JumpHoldTicks > 0)
	{
		m_JumpHoldTicks--;
		Controls.m_aInputData[D].m_Jump = 1;
	}
	else
	{
		Controls.m_aInputData[D].m_Jump = 0;
	}
	Controls.m_aInputData[D].m_Hook = WantHook ? 1 : 0;
}

// ==== planning ============================================================

void CAutoFinish::BuildTeleportEdges()
{
	CCollision *pCol = Collision();
	const int W = pCol->GetWidth();
	const int H = pCol->GetHeight();
	m_vTeleFirst.assign((size_t)std::max(W, 1) * std::max(H, 1), -1);
	m_vTeleTo.clear();
	m_vTeleNext.clear();
	const CTeleTile *pTele = pCol->TeleLayer();
	if(!pTele || W <= 0 || H <= 0)
	{
		return;
	}
	for(int y = 0; y < H; y++)
	{
		for(int x = 0; x < W; x++)
		{
			const CTeleTile &Tele = pTele[y * W + x];
			if(Tele.m_Number < 1)
			{
				continue;
			}
			if(Tele.m_Type != TILE_TELEIN && Tele.m_Type != TILE_TELEINEVIL)
			{
				continue;
			}
			const std::vector<vec2> &vOuts = pCol->TeleOuts(Tele.m_Number - 1);
			for(const vec2 &Out : vOuts)
			{
				const int Ox = std::clamp((int)(Out.x / CELL), 0, W - 1);
				const int Oy = std::clamp((int)(Out.y / CELL), 0, H - 1);
				const int InCell = y * W + x;
				m_vTeleTo.push_back(Oy * W + Ox);
				m_vTeleNext.push_back(m_vTeleFirst[InCell]);
				m_vTeleFirst[InCell] = (int)m_vTeleTo.size() - 1;
			}
		}
	}
}

bool CAutoFinish::PlanPath(const vec2 &StartPos, bool AllowFreeze)
{
	m_vPath.clear();
	m_NextNode = 0;
	m_TotalPathLen = 0.0f;
	m_aPlanError[0] = 0;
	m_PlanUsedFreeze = false;

	CCollision *pCol = Collision();
	const int W = pCol->GetWidth();
	const int H = pCol->GetHeight();
	const CTile *pTiles = pCol->GameLayer();
	const CTile *pFront = pCol->FrontLayer();
	if(W <= 0 || H <= 0 || !pTiles)
	{
		str_copy(m_aPlanError, "no map collision data");
		return false;
	}
	const int NumCells = W * H;
	m_vGScore.assign(NumCells, 1e30f);
	m_vFromCell.assign(NumCells, -1);
	m_vEdgeType.assign(NumCells, (unsigned char)NODE_WALK);
	m_vHookAttachCell.assign(NumCells, -1);
	m_vClosed.assign(NumCells, 0);
	BuildTeleportEdges();

	auto TileAt = [&](int x, int y) -> int {
		if(x < 0 || y < 0 || x >= W || y >= H)
		{
			return TILE_SOLID;
		}
		return (int)pTiles[y * W + x].m_Index;
	};
	auto FrontAt = [&](int x, int y) -> int {
		if(!pFront || x < 0 || y < 0 || x >= W || y >= H)
		{
			return 0;
		}
		return (int)pFront[y * W + x].m_Index;
	};
	auto IsSolidCell = [&](int x, int y) -> bool {
		const int Raw = TileAt(x, y);
		return Raw == TILE_SOLID || Raw == TILE_NOHOOK;
	};
	// cells the tee may not enter
	auto CellBlocked = [&](int x, int y) -> bool {
		const int Raw = TileAt(x, y);
		const int Front = FrontAt(x, y);
		if(Raw == TILE_SOLID || Raw == TILE_NOHOOK)
		{
			return true;
		}
		if(Raw == TILE_DEATH || Front == TILE_DEATH)
		{
			return true;
		}
		if(Raw == TILE_STOP || Raw == TILE_STOPS || Raw == TILE_STOPA)
		{
			return true;
		}
		if(Raw == TILE_DFREEZE || Front == TILE_DFREEZE)
		{
			return true; // deep freeze is permanent
		}
		if(!AllowFreeze && (Raw == TILE_FREEZE || Front == TILE_FREEZE))
		{
			return true;
		}
		return false;
	};

	// ---- goals: all finish tiles ----
	int GoalMinX = W, GoalMaxX = -1, GoalMinY = H, GoalMaxY = -1;
	int NumGoals = 0;
	for(int y = 0; y < H; y++)
	{
		for(int x = 0; x < W; x++)
		{
			if(pTiles[y * W + x].m_Index == TILE_FINISH ||
				(pFront && pFront[y * W + x].m_Index == TILE_FINISH))
			{
				NumGoals++;
				GoalMinX = std::min(GoalMinX, x);
				GoalMaxX = std::max(GoalMaxX, x);
				GoalMinY = std::min(GoalMinY, y);
				GoalMaxY = std::max(GoalMaxY, y);
			}
		}
	}
	if(NumGoals == 0)
	{
		str_copy(m_aPlanError, "this map has no finish tile");
		return false;
	}

	// ---- physics from the current server tuning ----
	const CTuningParams &Tuning = GameClient()->m_aTuning[g_Config.m_ClDummy];
	const float GroundSpeed = std::max(1.0f, (float)Tuning.m_GroundControlSpeed);
	const float JumpImpulse = std::max(1.0f, (float)Tuning.m_GroundJumpImpulse);
	const float Grav = std::max(0.05f, (float)Tuning.m_Gravity);
	const float HookDrag = std::max(1.0f, (float)Tuning.m_HookDragSpeed);
	const float HookLength = std::max(60.0f, (float)Tuning.m_HookLength);
	const float JumpHeightPx = JumpImpulse * JumpImpulse / (2.0f * Grav);
	const int MaxJumpTiles = std::max(1, (int)(JumpHeightPx / CELL) - 1);
	const int HookTiles = (int)(HookLength / CELL);
	const float WalkCost = CELL / GroundSpeed;
	const float FallCost = 2.2f;
	const float JumpCostPerTile = 5.0f;
	const int HookReach = std::max(1, std::min(HookTiles - 1, HOOK_WINDOW_TILES));

	// admissible-ish heuristic: manhattan distance to the finish bounding box
	auto Heur = [&](int x, int y) -> float {
		const int Hx = std::max(std::max(GoalMinX - x, x - GoalMaxX), 0);
		const int Hy = std::max(std::max(GoalMinY - y, y - GoalMaxY), 0);
		return (Hx + Hy) * 1.8f;
	};

	const int Sx = std::clamp((int)std::floor(StartPos.x / CELL), 0, W - 1);
	const int Sy = std::clamp((int)std::floor(StartPos.y / CELL), 0, H - 1);
	const int StartCell = Sy * W + Sx;
	m_vGScore[StartCell] = 0.0f;
	m_vFromCell[StartCell] = -1;

	std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>,
		std::greater<std::pair<float, int>>>
		Heap;
	Heap.push({Heur(Sx, Sy), StartCell});

	int Expanded = 0;
	int GoalCell = -1;
	while(!Heap.empty())
	{
		const int Cell = Heap.top().second;
		Heap.pop();
		if(m_vClosed[Cell])
		{
			continue;
		}
		m_vClosed[Cell] = 1;
		if(++Expanded > NODE_EXPAND_BUDGET)
		{
			break;
		}
		const int Cx = Cell % W;
		const int Cy = Cell / W;
		if(TileAt(Cx, Cy) == TILE_FINISH || (pFront && pFront[Cell].m_Index == TILE_FINISH))
		{
			GoalCell = Cell;
			break;
		}

		auto TryEdge = [&](int Tx, int Ty, float Cost, unsigned char EType, int AttachCell) {
			if(Tx < 0 || Ty < 0 || Tx >= W || Ty >= H)
			{
				return;
			}
			if(EType != NODE_TELE && CellBlocked(Tx, Ty))
			{
				return;
			}
			const int Tc = Ty * W + Tx;
			if(m_vClosed[Tc])
			{
				return;
			}
			const float Ng = m_vGScore[Cell] + Cost;
			if(Ng < m_vGScore[Tc] - 0.0001f)
			{
				m_vGScore[Tc] = Ng;
				m_vFromCell[Tc] = Cell;
				m_vEdgeType[Tc] = EType;
				m_vHookAttachCell[Tc] = AttachCell;
				Heap.push({Ng + Heur(Tx, Ty), Tc});
			}
		};

		// walk and fall
		TryEdge(Cx - 1, Cy, WalkCost, NODE_WALK, -1);
		TryEdge(Cx + 1, Cy, WalkCost, NODE_WALK, -1);
		TryEdge(Cx, Cy + 1, FallCost, NODE_WALK, -1);
		TryEdge(Cx - 1, Cy + 1, FallCost * 1.15f, NODE_WALK, -1);
		TryEdge(Cx + 1, Cy + 1, FallCost * 1.15f, NODE_WALK, -1);

		// jumps from ground cells
		const bool GroundHere = Cy + 1 >= H || IsSolidCell(Cx, Cy + 1);
		if(GroundHere && Cy - 1 >= 0 && !CellBlocked(Cx, Cy - 1))
		{
			for(int j = 1; j <= MaxJumpTiles; j++)
			{
				const int Ty = Cy - j;
				if(Ty < 0)
				{
					break;
				}
				for(int Dj = -1; Dj <= 1; Dj++)
				{
					const int Tx = Cx + Dj;
					if(Tx < 0 || Tx >= W)
					{
						continue;
					}
					// the tee has to pass the diagonal cell while rising
					if(Dj != 0 && CellBlocked(Tx, Ty + 1))
					{
						continue;
					}
					TryEdge(Tx, Ty, JumpCostPerTile * j + (Dj != 0 ? 1.0f : 0.0f), NODE_JUMP, -1);
				}
			}
		}

		// hook edges: the hook attaches to a solid wall behind the target
		if(g_Config.m_ClAutoFinishHook && HookReach >= HOOK_MIN_SPAN_TILES)
		{
			for(int Dy = -HookReach; Dy <= HookReach; Dy++)
			{
				for(int Dx = -HookReach; Dx <= HookReach; Dx++)
				{
					if(std::max(std::abs(Dx), std::abs(Dy)) < HOOK_MIN_SPAN_TILES)
					{
						continue;
					}
					const int Tx = Cx + Dx;
					const int Ty = Cy + Dy;
					if(Tx < 0 || Ty < 0 || Tx >= W || Ty >= H)
					{
						continue;
					}
					if(Dx * Dx + Dy * Dy > HookReach * HookReach)
					{
						continue;
					}
					// the wall the hook grabs, one tile past the target
					const int Ax = Tx + (Dx > 0 ? 1 : (Dx < 0 ? -1 : 0));
					const int Ay = Ty + (Dy > 0 ? 1 : (Dy < 0 ? -1 : 0));
					if(TileAt(Ax, Ay) != TILE_SOLID)
					{
						continue;
					}
					// line of sight from the tee to the attach point
					const vec2 From(Cx * CELL + 16.0f, Cy * CELL + 16.0f);
					const vec2 To(Ax * CELL + 16.0f, Ay * CELL + 16.0f);
					const float Len = distance(From, To);
					if(Len > HookLength - 8.0f || Len < 1.0f)
					{
						continue;
					}
					bool Clear = true;
					const vec2 Dir = (To - From) / Len;
					for(float T = 12.0f; T < Len - 20.0f && Clear; T += 12.0f)
					{
						const vec2 P = From + Dir * T;
						if(pCol->IsSolid(round_to_int(P.x), round_to_int(P.y)))
						{
							Clear = false;
						}
					}
					if(!Clear)
					{
						continue;
					}
					TryEdge(Tx, Ty, Len / HookDrag + 5.0f, NODE_HOOK, Ay * W + Ax);
				}
			}
		}

		// teleporters
		for(int Ei = m_vTeleFirst[Cell]; Ei != -1; Ei = m_vTeleNext[Ei])
		{
			const int OutCell = m_vTeleTo[Ei];
			TryEdge(OutCell % W, OutCell / W, 0.1f, NODE_TELE, -1);
		}
	}

	if(GoalCell == -1)
	{
		str_copy(m_aPlanError, AllowFreeze ? "no route to the finish found" : "no route without freeze tiles");
		return false;
	}

	// ---- reconstruct the node chain ----
	for(int C = GoalCell; C != -1; C = m_vFromCell[C])
	{
		SPathNode Node;
		Node.m_Pos = vec2((C % W) * CELL + 16.0f, (C / W) * CELL + 16.0f);
		Node.m_Type = m_vEdgeType[C];
		Node.m_HookPos = Node.m_Pos;
		if(Node.m_Type == NODE_HOOK && m_vHookAttachCell[C] >= 0)
		{
			const int A = m_vHookAttachCell[C];
			Node.m_HookPos = vec2((A % W) * CELL + 16.0f, (A / W) * CELL + 16.0f);
		}
		m_vPath.push_back(Node);
	}
	std::reverse(m_vPath.begin(), m_vPath.end());
	// node 0 is the tile the player stands on, drop it
	if(m_vPath.size() > 1)
	{
		m_vPath.erase(m_vPath.begin());
	}

	m_TotalPathLen = 0.0f;
	for(size_t i = 1; i < m_vPath.size(); i++)
	{
		m_TotalPathLen += distance(m_vPath[i - 1].m_Pos, m_vPath[i].m_Pos);
	}
	m_PlanUsedFreeze = AllowFreeze;
	return true;
}

// ==== rendering ===========================================================

void CAutoFinish::RenderPathStrip()
{
	if(m_vPath.size() < 2)
	{
		return;
	}

	// world-space projection matching the game camera
	float aPoints[4];
	const CCamera &Cam = GameClient()->m_Camera;
	Graphics()->MapScreenToWorld(Cam.m_Center.x, Cam.m_Center.y, 100.0f, 100.0f, 100.0f,
		0, 0, Graphics()->ScreenAspect(), Cam.m_Zoom, aPoints);
	Graphics()->MapScreen(aPoints[0], aPoints[1], aPoints[2], aPoints[3]);

	const float Alpha = std::clamp(g_Config.m_ClAutoFinishAlpha, 0, 255) / 255.0f;
	const ColorRGBA CoreColor(
		std::clamp(g_Config.m_ClAutoFinishColorR, 0, 255) / 255.0f,
		std::clamp(g_Config.m_ClAutoFinishColorG, 0, 255) / 255.0f,
		std::clamp(g_Config.m_ClAutoFinishColorB, 0, 255) / 255.0f,
		Alpha);
	const float CoreWidth = (float)std::clamp(g_Config.m_ClAutoFinishWidth, 1, 20);
	const float RevealLen = m_RevealProgress * m_TotalPathLen;

	// collect the visible segments up to the reveal length
	std::vector<std::pair<vec2, vec2>> vSegs;
	float Acc = 0.0f;
	vec2 Head = m_vPath[0].m_Pos;
	for(size_t i = 1; i < m_vPath.size() && Acc < RevealLen; i++)
	{
		const vec2 P0 = m_vPath[i - 1].m_Pos;
		vec2 P1 = m_vPath[i].m_Pos;
		float SegLen = distance(P0, P1);
		if(SegLen < 0.01f)
		{
			continue;
		}
		if(Acc + SegLen > RevealLen)
		{
			const float T = (RevealLen - Acc) / SegLen;
			P1 = P0 + (P1 - P0) * T;
			SegLen = RevealLen - Acc;
		}
		Acc += SegLen;
		Head = P1;
		vSegs.emplace_back(P0, P1);
	}

	// draw a thick polyline as rotated freeform quads
	auto DrawStrip = [&](float Width, const ColorRGBA &Color) {
		if(vSegs.empty())
		{
			return;
		}
		const float Half = Width * 0.5f;
		std::vector<IGraphics::CFreeformItem> vItems;
		vItems.reserve(vSegs.size());
		for(const auto &Seg : vSegs)
		{
			const vec2 P0 = Seg.first;
			const vec2 P1 = Seg.second;
			const float Len = distance(P0, P1);
			if(Len < 0.01f)
			{
				continue;
			}
			const vec2 Dir = (P1 - P0) / Len;
			const vec2 Perp(-Dir.y, Dir.x);
			vItems.emplace_back(
				P0 + Perp * Half, P1 + Perp * Half,
				P1 - Perp * Half, P0 - Perp * Half);
		}
		if(vItems.empty())
		{
			return;
		}
		Graphics()->QuadsBegin();
		Graphics()->SetColor(Color);
		Graphics()->QuadsDrawFreeform(vItems.data(), (int)vItems.size());
		Graphics()->QuadsEnd();
	};

	// soft glow underlay + crisp core line
	DrawStrip(CoreWidth * 2.6f, ColorRGBA(CoreColor.r, CoreColor.g, CoreColor.b, Alpha * 0.22f));
	DrawStrip(CoreWidth, CoreColor);

	// glowing head of the reveal animation
	{
		const float Pulse = 5.0f + 3.5f * (0.5f + 0.5f * std::sin((double)LocalTime() * 7.0f));
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
		IGraphics::CQuadItem HeadItem(Head.x - Pulse, Head.y - Pulse, Pulse * 2.0f, Pulse * 2.0f);
		Graphics()->QuadsDraw(&HeadItem, 1);
		Graphics()->SetColor(CoreColor.r, CoreColor.g, CoreColor.b, Alpha * 0.6f);
		IGraphics::CQuadItem HaloItem(Head.x - Pulse * 2.2f, Head.y - Pulse * 2.2f, Pulse * 4.4f, Pulse * 4.4f);
		Graphics()->QuadsDraw(&HaloItem, 1);
		Graphics()->QuadsEnd();
	}

	// pulsing diamond at the finish
	{
		const vec2 &F = m_vPath.back().m_Pos;
		const float S = 9.0f + 4.0f * (0.5f + 0.5f * std::sin((double)LocalTime() * 5.0f));
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha * 0.9f);
		IGraphics::CFreeformItem Diamond(
			F.x, F.y - S, F.x + S, F.y,
			F.x, F.y + S, F.x - S, F.y);
		Graphics()->QuadsDrawFreeform(&Diamond, 1);
		Graphics()->QuadsEnd();
	}

	// small diamonds at teleporter nodes
	{
		Graphics()->QuadsBegin();
		Graphics()->SetColor(CoreColor.r, CoreColor.g, CoreColor.b, Alpha * 0.85f);
		std::vector<IGraphics::CFreeformItem> vItems;
		for(size_t i = 1; i < m_vPath.size(); i++)
		{
			if(m_vPath[i].m_Type != NODE_TELE)
			{
				continue;
			}
			const vec2 &P = m_vPath[i].m_Pos;
			const float S = 6.0f;
			vItems.emplace_back(P.x, P.y - S, P.x + S, P.y, P.x, P.y + S, P.x - S, P.y);
		}
		if(!vItems.empty())
		{
			Graphics()->QuadsDrawFreeform(vItems.data(), (int)vItems.size());
		}
		Graphics()->QuadsEnd();
	}
}

void CAutoFinish::RenderStatus()
{
	Graphics()->MapScreen(0.0f, 0.0f, Graphics()->ScreenWidth(), Graphics()->ScreenHeight());

	char aLabel[160];
	if(m_StatusMessageTime > 0.0f)
	{
		str_copy(aLabel, m_aStatusMessage);
	}
	else if(!m_Active)
	{
		return;
	}
	else if(m_RunState == 0 || m_vPath.empty())
	{
		str_copy(aLabel, "AUTO-FINISH · PLANNING ROUTE...");
	}
	else if(m_RunState == 2)
	{
		str_copy(aLabel, "AUTO-FINISH · RETRYING...");
	}
	else
	{
		const int LocalId = GameClient()->m_Snap.m_LocalClientId;
		const bool Frozen = LocalId >= 0 &&
				    (GameClient()->m_aClients[LocalId].m_DeepFrozen ||
					    GameClient()->m_aClients[LocalId].m_FreezeEnd > Client()->GameTick(g_Config.m_ClDummy));
		if(Frozen)
		{
			str_copy(aLabel, "AUTO-FINISH · FROZEN, WAITING");
		}
		else
		{
			str_format(aLabel, sizeof(aLabel), "AUTO-FINISH · %d/%d%s",
				m_NextNode, (int)m_vPath.size(),
				m_PlanUsedFreeze ? " · freeze ahead" : "");
		}
	}

	const float W = 330.0f;
	const float H = 24.0f;
	CUIRect Rect((Graphics()->ScreenWidth() - W) * 0.5f, 8.0f, W, H);
	Rect.Draw(ColorRGBA(0.06f, 0.09f, 0.11f, 0.6f), IGraphics::CORNER_ALL, 6.0f);
	TextRender()->TextColor(0.55f, 1.0f, 0.75f, 0.95f);
	Ui()->DoLabel(&Rect, aLabel, 11.0f, TEXTALIGN_MC);
	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CAutoFinish::OnRender()
{
	// frame-rate independent animation timers
	const int64_t Now = time_get();
	float Dt = 0.016f;
	if(m_LastRenderTime != 0)
	{
		Dt = std::clamp((float)((double)(Now - m_LastRenderTime) / (double)time_freq()), 0.0f, 0.1f);
	}
	m_LastRenderTime = Now;

	if(m_Active)
	{
		m_RevealProgress = std::min(1.0f, m_RevealProgress + Dt / REVEAL_SECONDS);
	}
	if(m_StatusMessageTime > 0.0f)
	{
		m_StatusMessageTime = std::max(0.0f, m_StatusMessageTime - Dt);
	}

	if(Client()->State() != IClient::STATE_ONLINE)
	{
		return;
	}
	if(!m_Active && m_StatusMessageTime <= 0.0f)
	{
		return;
	}

	if(m_Active && g_Config.m_ClAutoFinishShowPath)
	{
		RenderPathStrip();
	}
	RenderStatus();
}
