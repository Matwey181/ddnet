/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com                */
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
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	constexpr float CELL = 32.0f;
	constexpr float POS_QUANT = 8.0f;
	constexpr float VEL_QUANT = 2.0f;
	constexpr float HOOK_POS_QUANT = 8.0f;
	constexpr float HEUR_VMAX = 32.0f; // px/tick, generous upper speed bound for the heuristic
	constexpr float CORRIDOR_SLACK_TICKS = 160.0f;
	constexpr int MACRO_FINE = 5;
	constexpr int MACRO_COARSE = 8;
	constexpr int HOOK_FIRE_TICKS = 8; // hook flight budget before giving up
	constexpr int MAX_NODES_FINE = 12000;
	constexpr int MAX_NODES_COARSE = 20000;
	constexpr int MAX_EXPANSIONS_FINE = 5000;
	constexpr int MAX_EXPANSIONS_COARSE = 9000;
	constexpr int MAX_ANCHORS = 6;
	constexpr float NODE_REACH_PX = 46.0f;
	// a node below our feet by this much is a fall node: it can only be claimed
	// by actually falling, never by walking near its x on solid ground
	constexpr float FEET_DROP_GUARD_PX = 15.0f;
	// a node above our feet while standing needs a jump: same rule
	constexpr float NODE_ABOVE_GUARD_PX = 8.0f;
	constexpr float NODE_PASS_PX = 220.0f; // max distance at which a passed node counts
	constexpr float DEVIATION_REPLAN_PX = 340.0f;
	constexpr float AIM_MAX_PX = 900.0f;
	constexpr int STUCK_SECONDS = 5;
	constexpr int MAX_STUCK_REPLANS = 3;
	constexpr int MAX_PLAN_RETRIES = 2;
	constexpr int PERIODIC_REPLAN_SECONDS = 15;
	constexpr int MAX_SEARCH_ATTEMPTS = 4;
	// segment planning: the search aims ROUTE_LOOKAHEAD_POINTS along the global
	// route instead of the whole map, which keeps it small and fast
	constexpr int ROUTE_LOOKAHEAD_POINTS = 32;
	constexpr int ROUTE_MAX_POINTS = 600;
	constexpr float SUBGOAL_REACH_PX = 52.0f;
	constexpr int SEARCH_ATTEMPT_SECONDS = 6;
	constexpr float DRIFT_RESTART_PX = 140.0f;
	constexpr int MAX_DRIFT_RESTARTS = 2;
	constexpr float WAIT_LAND_SECONDS = 1.5f;
	// partial-route fallback: a candidate must buy this much progress over its search start
	constexpr float MIN_PARTIAL_GAIN_TICKS = 15.0f;
	// consecutive partial routes must improve on the previous one by this much
	constexpr float MIN_PARTIAL_IMPROVE_TICKS = 5.0f;
	constexpr int MAX_PARTIAL_STAGNANT = 3;
	constexpr float REVEAL_SECONDS = 1.1f;
}

CAutoFinish::CAutoFinish()
{
}

void CAutoFinish::OnConsoleInit()
{
	Console()->Register("autofinish", "?i[enabled]", CFGFLAG_CLIENT, ConAutoFinish, this, "Toggle the auto-finish bot (simulates the real physics and walks the fastest route to the finish)");
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
	m_PlanIdx = 0;
	m_PlanTickF = 0.0f;
	m_PlanTotalTicks = 0;
	m_TotalPathLen = 0.0f;
	m_RevealProgress = 0.0f;
	m_RunState = 0;
	m_PlanUsedFreeze = false;
	m_PlanRetries = 0;
	m_NextPlanTryTime = 0;
	m_StuckCount = 0;
	m_ReplanAfterFreeze = false;
	m_RelaxedValid = false;
	m_vRoute.clear();
	m_RouteIdx = 0;
	m_RouteReachedFinish = false;
	m_PlanEndsAtFinish = false;
	m_WaitLandUntil = 0;
	m_DriftRestarts = 0;
	ClearSearchData();
	if(m_Active)
	{
		ClearBotInput();
		RequestReplan(true);
	}
}

void CAutoFinish::OnMapLoad()
{
	m_RelaxedValid = false;
	m_vRoute.clear();
	m_RouteIdx = 0;
	m_RouteReachedFinish = false;
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
	m_PlanIdx = 0;
	m_PlanTickF = 0.0f;
	m_SearchAttempt = 0;
	m_PlanEndsAtFinish = false;
	m_WaitLandUntil = 0;
	m_PartialPlan = false;
	m_HasPartialCandidate = false;
	m_BestPartialH = 1e30f;
	m_vPartialPath.clear();
	m_vPartialPath.shrink_to_fit();
	m_RevealStart = QuickReveal ? std::max(0.4f, m_RevealProgress * 0.7f) : 0.3f;
	ClearSearchData();
}

void CAutoFinish::ClearSearchData()
{
	m_SearchActive = false;
	m_GoalNode = -1;
	m_SearchExpansions = 0;
	m_SearchOpenExhausted = false;
	m_SearchBudgetHit = false;
	m_SearchStartRelaxed = 1e30f;
	m_SearchBestH = 1e30f;
	m_SearchBestNode = -1;
	m_vSearchNodes.clear();
	m_vSearchNodes.shrink_to_fit();
	m_OpenHeap = std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>,
		std::greater<std::pair<float, int>>>();
	m_BestG.clear();
}

void CAutoFinish::Activate()
{
	m_Active = true;
	m_RunState = 0;
	m_PlanUsedFreeze = false;
	m_PlanRetries = 0;
	m_StuckCount = 0;
	m_PlanIdx = 0;
	m_PlanTickF = 0.0f;
	m_PlanTotalTicks = 0;
	m_ReplanAfterFreeze = false;
	m_NextPlanTryTime = 0;
	m_SearchAttempt = 0;
	m_aPlanError[0] = 0;
	m_vPath.clear();
	m_TotalPathLen = 0.0f;
	m_RevealProgress = 0.0f;
	m_RevealStart = 0.0f;
	m_PartialPlan = false;
	m_HasPartialCandidate = false;
	m_BestPartialH = 1e30f;
	m_LastPartialTargetH = 1e30f;
	m_PartialStagnant = 0;
	m_vPartialPath.clear();
	m_vPartialPath.shrink_to_fit();
	m_LastProgressTime = time_get();
	m_LastUpdateTime = m_LastProgressTime;
	m_NextPeriodicReplan = m_LastProgressTime + (int64_t)time_freq() * PERIODIC_REPLAN_SECONDS;
	m_PlanEndsAtFinish = false;
	m_WaitLandUntil = 0;
	m_DriftRestarts = 0;
	m_InitialRelaxed = 1e30f;
	m_vRoute.clear();
	m_RouteIdx = 0;
	m_RouteReachedFinish = false;
	ClearSearchData();
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
	ClearSearchData();
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

// ==== main loop ===========================================================

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
	if(m_LastUpdateTime == 0)
	{
		m_LastUpdateTime = Now;
	}
	const float DtTicks = std::clamp((float)((double)(Now - m_LastUpdateTime) / (double)Freq) * 50.0f, 0.0f, 10.0f);
	m_LastUpdateTime = Now;

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

	CControls &Controls = GameClient()->m_Controls;
	const int D = g_Config.m_ClDummy;

	// ---- planning (time-sliced so the game keeps rendering) ----------------
	if(m_RunState != 1)
	{
		if(!m_SearchActive)
		{
			StartNextSearchAttempt();
		}
		if(m_SearchActive)
		{
			// a small slice of this frame for the search: segments are small, so
			// the game stays smooth - the old ~8 ms slices starved the netcode
			// and caused the "connection problems" lags
			const int64_t SliceStart = time_get();
			const int64_t SliceBudget = Freq / 400;
			while(m_SearchActive && time_get() - SliceStart < SliceBudget)
			{
				if(SearchStep())
				{
					break;
				}
			}
		}
		if(m_RunState != 1)
		{
			if(m_RunState != 2)
			{
				m_LastProgressTime = Now; // planning is not being stuck
			}
			// keep walking towards the next route point while the next segment is
			// being planned instead of freezing in place; mid-air we stay passive
			// (no air control, just fall) so we cannot invalidate the seed
			if(m_RunState == 0 && !GameClient()->m_Chat.IsActive() &&
				!GameClient()->m_Menus.IsActive())
			{
				vec2 SteerPos = GameClient()->m_PredictedChar.m_Pos;
				const CNetObj_Character &SteerSnap = GameClient()->m_Snap.m_aCharacters[LocalId].m_Cur;
				if(distance(SteerPos, vec2(SteerSnap.m_X, SteerSnap.m_Y)) > 500.0f)
				{
					SteerPos = vec2(SteerSnap.m_X, SteerSnap.m_Y);
				}
				const bool SteerGround = Collision()->TestBox(
					SteerPos + vec2(0.0f, 3.0f), CCharacterCore::PhysicalSizeVec2());
				int SteerDir = 0;
				if(SteerGround && (int)m_vRoute.size() > m_RouteIdx + 1)
				{
					// aim a few points ahead; across a teleporter the next point is
					// far away, so walk onto the current one (the teleporter) instead
					const int Ni = m_RouteIdx + 1;
					const vec2 Near = distance(m_vRoute[Ni], m_vRoute[m_RouteIdx]) > 120.0f ?
								  m_vRoute[m_RouteIdx] :
								  m_vRoute[std::min(Ni + 2, (int)m_vRoute.size() - 1)];
					if(Near.x > SteerPos.x + 8.0f)
					{
						SteerDir = 1;
					}
					else if(Near.x < SteerPos.x - 8.0f)
					{
						SteerDir = -1;
					}
					// never steer into death or freeze tiles
					if(SteerDir != 0)
					{
						const vec2 Probe = SteerPos + vec2((float)SteerDir * 40.0f, 0.0f);
						const int ProbeIdx = Collision()->GetMapIndex(Probe);
						const int ProbeDownIdx = Collision()->GetMapIndex(Probe + vec2(0.0f, 28.0f));
						const int T = Collision()->GetTileIndex(ProbeIdx);
						const int FT = Collision()->GetFrontTileIndex(ProbeIdx);
						const int TD = Collision()->GetTileIndex(ProbeDownIdx);
						const int FTD = Collision()->GetFrontTileIndex(ProbeDownIdx);
						if(T == TILE_DEATH || FT == TILE_DEATH || TD == TILE_DEATH || FTD == TILE_DEATH ||
							T == TILE_FREEZE || FT == TILE_FREEZE || TD == TILE_FREEZE || FTD == TILE_FREEZE)
						{
							SteerDir = 0;
						}
					}
				}
				Controls.m_aInputDirectionLeft[D] = SteerDir < 0 ? 1 : 0;
				Controls.m_aInputDirectionRight[D] = SteerDir > 0 ? 1 : 0;
				Controls.m_aInputData[D].m_Jump = 0;
				Controls.m_aInputData[D].m_Hook = 0;
			}
			else
			{
				ClearBotInput();
			}
			return;
		}
	}

	// the game freezes the input while chatting or in menus: wait silently
	if(GameClient()->m_Chat.IsActive() || GameClient()->m_Menus.IsActive())
	{
		m_LastProgressTime = Now;
		m_LastUpdateTime = Now;
		return;
	}

	// current player state (prefer prediction, fall back to the snapshot)
	vec2 Pos = GameClient()->m_PredictedChar.m_Pos;
	const CNetObj_Character &SnapChar = GameClient()->m_Snap.m_aCharacters[LocalId].m_Cur;
	const vec2 SnapPos(SnapChar.m_X, SnapChar.m_Y);
	if(distance(Pos, SnapPos) > 500.0f)
	{
		Pos = SnapPos;
	}

	// standing on solid ground? (the tee hitbox is 28x28, feet 14px below the
	// center; probe a box a few pixels lower than the current position)
	const bool OnGround = Collision()->TestBox(Pos + vec2(0.0f, 3.0f), CCharacterCore::PhysicalSizeVec2());

	// frozen players cannot move: hold still, wait it out and replan afterwards
	const bool Frozen = GameClient()->m_aClients[LocalId].m_DeepFrozen ||
			    GameClient()->m_aClients[LocalId].m_FreezeEnd > Client()->GameTick(D);
	if(Frozen)
	{
		Controls.m_aInputDirectionLeft[D] = 0;
		Controls.m_aInputDirectionRight[D] = 0;
		Controls.m_aInputData[D].m_Jump = 0;
		Controls.m_aInputData[D].m_Hook = 0;
		m_ReplanAfterFreeze = true;
		m_LastProgressTime = Now; // don't accumulate stuck time while frozen
		return;
	}
	if(m_ReplanAfterFreeze)
	{
		m_ReplanAfterFreeze = false;
		RequestReplan(true);
		return;
	}

	// ---- trajectory bookkeeping --------------------------------------------
	if(m_vPath.empty())
	{
		Deactivate(REASON_FAILED, "AUTO-FINISH FAILED: empty plan");
		return;
	}

	// advance along the plan: a node is done when we touch it or when we have
	// PASSED it along the direction of travel (running, falling or flying
	// past it) - never merely because its tick budget ran out while we were
	// only near it: switching to the next point before reaching the current
	// one is exactly what derailed the replay. A node above our feet needs a
	// jump and a node below our feet needs a fall: both are claimed only
	// while airborne.
	while(m_PlanIdx < (int)m_vPath.size())
	{
		const SPathNode &Node = m_vPath[m_PlanIdx];
		const vec2 From = m_PlanIdx > 0 ? m_vPath[m_PlanIdx - 1].m_Pos : Pos;
		vec2 Travel = Node.m_Pos - From;
		const float TravelLen = length(Travel);
		bool Done = distance(Pos, Node.m_Pos) < NODE_REACH_PX;
		if(!Done && TravelLen > 1.0f)
		{
			const vec2 TravelDir = Travel / TravelLen;
			Done = dot(Pos - Node.m_Pos, TravelDir) > 0.0f &&
			       distance(Pos, Node.m_Pos) < NODE_PASS_PX;
		}
		if(Done && OnGround &&
			(Node.m_Pos.y < Pos.y - NODE_ABOVE_GUARD_PX || Node.m_Pos.y > Pos.y + FEET_DROP_GUARD_PX))
		{
			Done = false; // claim it only once we are airborne
		}
		if(!Done)
		{
			break;
		}
		m_PlanIdx++;
		m_PlanTickF = 0.0f;
		m_LastProgressTime = Now;
	}

	if(m_PlanIdx >= (int)m_vPath.size())
	{
		if(m_PlanEndsAtFinish)
		{
			SetStatusMessage("AUTO-FINISH: MAP COMPLETED", 6.0f);
			Deactivate(REASON_FINISHED);
			return;
		}
		if(!OnGround)
		{
			// the segment ended mid-air (fall or swing): finish the landing first.
			// Re-planning from a stale falling position is what made the bot fire
			// surprise hooks in the middle of a clean drop
			if(m_WaitLandUntil == 0)
			{
				m_WaitLandUntil = Now + (int64_t)(Freq * WAIT_LAND_SECONDS);
				ClearBotInput();
				return;
			}
			if(Now < m_WaitLandUntil)
			{
				ClearBotInput();
				return;
			}
			m_WaitLandUntil = 0;
		}
		if(m_PartialPlan)
		{
			// walked as far as the planner could see: plan again from here
			SetStatusMessage("AUTO-FINISH: partial route walked, planning further", 3.0f);
		}
		RequestReplan(true);
		return;
	}

	// progress along the global route (used for steering while planning and
	// for the dim guide line that shows the whole way to the finish)
	UpdateRouteProgress(Pos);

	// periodic deviation check: if we drifted far from EVERY upcoming point
	// of the route, replan from the current position. This no longer jumps
	// the plan index to the closest node: on switchback routes the upcoming
	// leg runs physically close to the current one and the jump made the bot
	// skip whole legs of the plan. Advancing is done by the loop above only.
	if(Now > m_NextReanchor)
	{
		m_NextReanchor = Now + Freq / 5;
		float BestD = 1e9f;
		const int To = std::min((int)m_vPath.size() - 1, m_PlanIdx + 16);
		for(int i = m_PlanIdx; i <= To; i++)
		{
			BestD = std::min(BestD, distance(Pos, m_vPath[i].m_Pos));
		}
		if(BestD > DEVIATION_REPLAN_PX)
		{
			RequestReplan(true);
			return;
		}
	}

	const SPathNode &Target = m_vPath[m_PlanIdx];

	// periodic replanning keeps the route fresh; never mid-air: a plan built
	// from a falling position goes stale immediately (surprise hooks, again)
	if(Now > m_NextPeriodicReplan)
	{
		if(!OnGround)
		{
			m_NextPeriodicReplan = Now + Freq; // retry once we have landed
		}
		else
		{
			m_NextPeriodicReplan = Now + Freq * PERIODIC_REPLAN_SECONDS;
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

	// ---- execute the planned action -----------------------------------------
	m_PlanTickF += DtTicks;

	vec2 AimRel = Target.m_Pos - Pos;
	const float AimLen = length(AimRel);
	if(AimLen > AIM_MAX_PX)
	{
		AimRel = AimRel * (AIM_MAX_PX / AimLen);
	}
	else if(AimLen < 1.0f)
	{
		AimRel = vec2(1.0f, 0.0f);
	}

	const vec2 ActFrom = m_PlanIdx > 0 ? m_vPath[m_PlanIdx - 1].m_Pos : Pos;
	bool WantHook = false;
	if(Target.m_HookMode != 0)
	{
		// keep holding a hook that is already flying, attached or retracting:
		// releasing mid-flight would retract it and dropping mid-drag breaks
		// the swing the planner counted on
		const int HookState = GameClient()->m_PredictedChar.m_HookState;
		if(HookState == HOOK_FLYING || HookState == HOOK_GRABBED ||
			HookState == HOOK_RETRACT_START || HookState == HOOK_RETRACT_END)
		{
			WantHook = true;
		}
		else if(m_PlanTickF < (float)Target.m_Ticks * 3.0f + 25.0f)
		{
			// fire only when the anchor is really within the hook length and
			// visible from where we actually are: the plan fired it from a spot
			// we may lag behind, and a hook that cannot reach just spams
			// fire/retract at walls we never get
			int Zone = 0;
			if(GameClient()->m_PredictedWorld.m_WorldConfig.m_UseTuneZones)
			{
				Zone = Collision()->IsTune(Collision()->GetMapIndex(Pos));
			}
			Zone = std::clamp(Zone, 0, TuneZone::NUM - 1);
			const float HookLen = GameClient()->GetTuning(Zone)->m_HookLength;
			vec2 HitPos;
			const int Hit = Collision()->IntersectLineTeleHook(
				Pos, Target.m_HookTarget, &HitPos, nullptr, nullptr);
			// in the air be stricter: firing from a lagging position at an
			// anchor we can barely reach is the surprise mid-air hook spam
			const float RangeMul = OnGround ? 0.95f : 0.85f;
			const bool NearActionStart = OnGround || distance(Pos, ActFrom) < NODE_PASS_PX;
			if(distance(Pos, Target.m_HookTarget) < HookLen * RangeMul &&
				Hit == TILE_SOLID && distance(HitPos, Target.m_HookTarget) < 30.0f &&
				NearActionStart)
			{
				WantHook = true;
			}
		}
		if(WantHook)
		{
			vec2 HookAim = Target.m_HookTarget - Pos;
			const float HookAimLen = length(HookAim);
			if(HookAimLen > AIM_MAX_PX)
			{
				HookAim = HookAim * (AIM_MAX_PX / HookAimLen);
			}
			else if(HookAimLen < 1.0f)
			{
				HookAim = vec2(1.0f, 0.0f);
			}
			AimRel = HookAim;
		}
	}

	int Dir = Target.m_Dir;
	if(OnGround && Target.m_Pos.y > Pos.y + FEET_DROP_GUARD_PX &&
		m_PlanTickF > (float)Target.m_Ticks * 0.9f)
	{
		// the plan expects us to be falling already: head for the drop point
		// instead of replaying an air-control direction on the ground. No
		// deadzone here: stepping exactly onto the drop point means falling
		// off the edge, which is the whole point
		Dir = Target.m_Pos.x >= Pos.x ? 1 : -1;
	}
	else if(OnGround &&
		((Target.m_HookMode == 1 && !WantHook) ||
			(Target.m_Jump && distance(Pos, ActFrom) > NODE_REACH_PX * 1.5f)))
	{
		// this action starts from a spot we have not reached: the hook anchor
		// is out of range from here or the jump needs its take-off point.
		// Close in on the action's starting point first
		Dir = ActFrom.x > Pos.x + 4.0f ? 1 : (ActFrom.x < Pos.x - 4.0f ? -1 : 0);
	}
	Controls.m_aInputDirectionLeft[D] = Dir < 0 ? 1 : 0;
	Controls.m_aInputDirectionRight[D] = Dir > 0 ? 1 : 0;
	Controls.m_aMousePos[D] = AimRel;
	const float JumpPhase = Target.m_Ticks > 0 ? std::fmod(m_PlanTickF, (float)Target.m_Ticks) : 999.0f;
	// press jumps only from near the point the action starts from (the
	// previous path point), not from wherever we happen to lag behind
	const bool JumpReady = !Target.m_Jump || distance(Pos, ActFrom) < NODE_REACH_PX * 1.5f;
	Controls.m_aInputData[D].m_Jump = (Target.m_Jump && JumpReady && JumpPhase < 1.6f) ? 1 : 0;
	Controls.m_aInputData[D].m_Hook = WantHook ? 1 : 0;
}

// ==== search: setup and driver ===========================================

void CAutoFinish::StartNextSearchAttempt()
{
	// attempt ladder: fine/avoid-freeze -> fine/freeze -> coarse/avoid-freeze -> coarse/freeze
	switch(m_SearchAttempt)
	{
	case 0:
		StartSearch(false, false);
		break;
	case 1:
		StartSearch(true, false);
		break;
	case 2:
		StartSearch(false, true);
		break;
	default:
		StartSearch(true, true);
		break;
	}
}

void CAutoFinish::StartSearch(bool AllowFreeze, bool Coarse)
{
	m_SearchActive = true;
	m_SearchAllowFreeze = AllowFreeze;
	m_SearchCoarse = Coarse;
	m_SearchMacro = Coarse ? MACRO_COARSE : MACRO_FINE;
	m_SearchMaxNodes = Coarse ? MAX_NODES_COARSE : MAX_NODES_FINE;
	m_SearchMaxExpansions = Coarse ? MAX_EXPANSIONS_COARSE : MAX_EXPANSIONS_FINE;
	m_SearchOpenExhausted = false;
	m_SearchBudgetHit = false;
	m_SearchExpansions = 0;
	m_GoalNode = -1;
	m_SearchBestNode = 0; // the start node is the initial best

	m_vSearchNodes.clear();
	m_OpenHeap = std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>,
		std::greater<std::pair<float, int>>>();
	m_BestG.clear();

	BuildRelaxedTicks();
	m_SearchStartWall = time_get();

	// our own world core: an empty character list (no player interactions,
	// the search stays deterministic) with a snapshot of the switcher states
	m_SearchWorld = CWorldCore();
	m_SearchWorld.m_pPrng = nullptr;
	if(!GameClient()->PredSwitchers().empty())
	{
		m_SearchWorld.m_vSwitchers = GameClient()->PredSwitchers();
	}
	else if(!GameClient()->Switchers().empty())
	{
		m_SearchWorld.m_vSwitchers = GameClient()->Switchers();
	}

	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	m_SearchTeam = std::clamp(GameClient()->m_PredictedWorld.Teams()->Team(LocalId), 0, NUM_DDRACE_TEAMS - 1);
	m_SearchCore.Init(&m_SearchWorld, Collision(), GameClient()->m_PredictedWorld.Teams());
	m_SearchCore.m_Id = LocalId;
	mem_zero(&m_SearchCore.m_Input, sizeof(m_SearchCore.m_Input));

	// start from the predicted character state, fall back to the snapshot
	const CCharacterCore &Pred = GameClient()->m_PredictedChar;
	vec2 Pos = Pred.m_Pos;
	vec2 Vel = Pred.m_Vel;
	const CNetObj_Character &SnapChar = GameClient()->m_Snap.m_aCharacters[LocalId].m_Cur;
	const vec2 SnapPos(SnapChar.m_X, SnapChar.m_Y);
	if(distance(Pos, SnapPos) > 500.0f)
	{
		Pos = SnapPos;
		Vel = vec2(SnapChar.m_VelX / 256.0f, SnapChar.m_VelY / 256.0f);
	}

	// the segment sub-goal: a point ROUTE_LOOKAHEAD_POINTS along the global
	// route to the finish; the search aims for it instead of the whole map
	BuildRouteFrom(Pos);
	if((int)m_vRoute.size() >= 2)
	{
		m_SearchSubGoal = m_vRoute[std::min(m_RouteIdx + ROUTE_LOOKAHEAD_POINTS, (int)m_vRoute.size() - 1)];
		m_SearchSubGoalValid = true;
	}
	else
	{
		m_SearchSubGoalValid = false;
	}

	SSearchNode Start;
	Start.m_Pos = Pos;
	Start.m_Vel = Vel;
	Start.m_HookState = Pred.m_HookState == HOOK_GRABBED ? HOOK_GRABBED : HOOK_IDLE;
	Start.m_HookPos = Start.m_HookState == HOOK_GRABBED ? Pred.m_HookPos : Pos;
	Start.m_Jumped = Pred.m_Jumped;
	Start.m_JumpedTotal = Pred.m_JumpedTotal;
	Start.m_Jumps = Pred.m_Jumps;
	Start.m_EndlessJump = Pred.m_EndlessJump;
	Start.m_FrozenTicks = 0;
	Start.m_Goal = false;
	Start.m_G = 0.0f;
	Start.m_Parent = -1;
	Start.m_Ticks = 0;
	Start.m_Type = NODE_WALK;

	// super and invincible flags survive on the core for the whole search
	m_SearchCore.m_Super = Pred.m_Super;
	m_SearchCore.m_Invincible = Pred.m_Invincible;

	m_SearchStartRelaxed = RelaxedTicksAt(Start.m_Pos);
	if(m_SearchStartRelaxed < m_InitialRelaxed)
	{
		m_InitialRelaxed = m_SearchStartRelaxed; // reference for the route percentage
	}
	m_SearchBestH = m_SearchStartRelaxed;
	if(m_SearchStartRelaxed > 1e29f)
	{
		str_copy(m_aPlanError, "the start position cannot reach any finish tile");
		m_SearchOpenExhausted = true; // nothing to search
		FinishSearchAttempt();
		return;
	}

	// starting on a finish tile already?
	{
		bool Death, FreezeHit, UnfreezeHit, DeepFreezeHit, Goal, Tele, EvilTele;
		vec2 TelePos;
		int JumpRefill, JumpsSet;
		RestoreSearchCore(Start);
		HandleSimTiles(Pos, Pos, Death, FreezeHit, UnfreezeHit, DeepFreezeHit, Goal, Tele, EvilTele, TelePos, JumpRefill, JumpsSet);
		Start.m_Goal = Goal;
	}

	m_BestG[StateKey(Start)] = 0.0f;
	m_vSearchNodes.push_back(Start);
	m_OpenHeap.push({RelaxedTicksAt(Start.m_Pos), 0});
}

bool CAutoFinish::SearchStep()
{
	if(!m_SearchActive)
	{
		return true;
	}
	// wall-clock cap for one attempt: without it a hopeless segment could
	// eat tens of seconds of search time slice by slice (the old lags)
	if(m_SearchStartWall != 0 && time_get() - m_SearchStartWall > (int64_t)time_freq() * SEARCH_ATTEMPT_SECONDS)
	{
		m_SearchBudgetHit = true;
		FinishSearchAttempt();
		return true;
	}
	if(m_OpenHeap.empty())
	{
		m_SearchOpenExhausted = true;
		FinishSearchAttempt();
		return true;
	}
	if(m_SearchExpansions >= m_SearchMaxExpansions ||
		(int)m_vSearchNodes.size() >= m_SearchMaxNodes ||
		m_SearchBudgetHit)
	{
		m_SearchBudgetHit = true;
		FinishSearchAttempt();
		return true;
	}

	const int NodeIdx = m_OpenHeap.top().second;
	m_OpenHeap.pop();

	const SSearchNode &Node = m_vSearchNodes[NodeIdx];
	const auto It = m_BestG.find(StateKey(Node));
	if(It != m_BestG.end() && Node.m_G > It->second + 0.05f)
	{
		return false; // stale heap entry, a cheaper path to this state exists
	}

	if(Node.m_Goal || Node.m_SubGoalHit)
	{
		m_GoalNode = NodeIdx;
		FinishSearchAttempt();
		return true;
	}

	m_SearchExpansions++;
	const float h = RelaxedTicksAt(Node.m_Pos);
	if(h < m_SearchBestH)
	{
		m_SearchBestH = h;
		m_SearchBestNode = NodeIdx;
	}

	ExpandNode(NodeIdx);
	return false;
}

void CAutoFinish::FinishSearchAttempt()
{
	m_SearchActive = false;

	if(m_GoalNode >= 0)
	{
		const bool FinishHit = m_vSearchNodes[m_GoalNode].m_Goal;
		BuildPlanFromSearch(m_GoalNode);
		if(m_vPath.empty())
		{
			SetStatusMessage("AUTO-FINISH: MAP COMPLETED", 6.0f);
			Deactivate(REASON_FINISHED);
			return;
		}
		m_PlanUsedFreeze = m_SearchAllowFreeze;
		m_PlanEndsAtFinish = FinishHit;
		m_RunState = 1;
		m_PlanIdx = 0;
		m_PlanTickF = 0.0f;
		m_LastProgressTime = time_get();
		m_LastUpdateTime = m_LastProgressTime;
		m_NextPeriodicReplan = m_LastProgressTime + (int64_t)time_freq() * PERIODIC_REPLAN_SECONDS;
		m_NextReanchor = 0;
		m_RevealProgress = m_RevealStart;
		m_PlanRetries = 0;
		m_StuckCount = 0;
		m_DriftRestarts = 0;
		m_PartialPlan = false;
		m_HasPartialCandidate = false;
		m_BestPartialH = 1e30f;
		m_LastPartialTargetH = 1e30f;
		m_PartialStagnant = 0;
		m_vPartialPath.clear();
		m_vPartialPath.shrink_to_fit();
		if(m_PlanUsedFreeze)
		{
			SetStatusMessage("AUTO-FINISH: the route unavoidably passes freeze tiles", 4.0f);
		}
		ClearSearchData();
		return;
	}

	// this attempt found nothing: remember how far it got as a partial route
	if(m_SearchBestNode > 0 && m_SearchBestH < m_SearchStartRelaxed - MIN_PARTIAL_GAIN_TICKS)
	{
		// keep the route that is currently being rendered while planning
		std::vector<SPathNode> vOldPath = m_vPath;
		const int OldTotalTicks = m_PlanTotalTicks;
		const float OldPathLen = m_TotalPathLen;
		BuildPlanFromSearch(m_SearchBestNode);
		if(!m_vPath.empty() && (!m_HasPartialCandidate || m_SearchBestH < m_BestPartialH - 0.05f))
		{
			m_HasPartialCandidate = true;
			m_BestPartialH = m_SearchBestH;
			m_vPartialPath = m_vPath;
			m_PartialTotalTicks = m_PlanTotalTicks;
			m_PartialPathLen = m_TotalPathLen;
			m_PartialUsedFreeze = m_SearchAllowFreeze;
		}
		m_vPath = std::move(vOldPath);
		m_PlanTotalTicks = OldTotalTicks;
		m_TotalPathLen = OldPathLen;
	}

	// the seed went stale while we were planning (we kept walking, fell or
	// teleported): restart from where we actually are instead of executing a
	// plan computed for a ghost position - that is where the surprise mid-air
	// hooks came from
	if(m_DriftRestarts < MAX_DRIFT_RESTARTS)
	{
		vec2 CurPos = GameClient()->m_PredictedChar.m_Pos;
		const int LocalId = GameClient()->m_Snap.m_LocalClientId;
		if(LocalId >= 0 && !m_vSearchNodes.empty())
		{
			const CNetObj_Character &CurSnap = GameClient()->m_Snap.m_aCharacters[LocalId].m_Cur;
			if(distance(CurPos, vec2(CurSnap.m_X, CurSnap.m_Y)) > 500.0f)
			{
				CurPos = vec2(CurSnap.m_X, CurSnap.m_Y);
			}
			if(distance(CurPos, m_vSearchNodes[0].m_Pos) > DRIFT_RESTART_PX)
			{
				m_DriftRestarts++;
				RequestReplan(false);
				return;
			}
		}
	}

	// this attempt found nothing, try the next one
	m_SearchAttempt++;
	if(m_SearchAttempt >= MAX_SEARCH_ATTEMPTS)
	{
		// no complete route anywhere: walk the best partial route and
		// plan again from where it ends
		if(m_HasPartialCandidate)
		{
			const bool Improving = m_BestPartialH < m_LastPartialTargetH - MIN_PARTIAL_IMPROVE_TICKS;
			m_PartialStagnant = Improving ? 0 : m_PartialStagnant + 1;
			m_LastPartialTargetH = m_BestPartialH;
			if(m_PartialStagnant < MAX_PARTIAL_STAGNANT)
			{
				m_vPath = std::move(m_vPartialPath);
				m_PlanTotalTicks = m_PartialTotalTicks;
				m_TotalPathLen = m_PartialPathLen;
				m_PlanUsedFreeze = m_PartialUsedFreeze;
				m_PartialPlan = true;
				m_HasPartialCandidate = false;
				m_BestPartialH = 1e30f;
				m_vPartialPath.clear();
				m_vPartialPath.shrink_to_fit();
				m_RunState = 1;
				m_PlanIdx = 0;
				m_PlanTickF = 0.0f;
				m_SearchAttempt = 0;
				m_PlanRetries = 0;
				m_StuckCount = 0;
				m_LastProgressTime = time_get();
				m_LastUpdateTime = m_LastProgressTime;
				m_NextPeriodicReplan = m_LastProgressTime + (int64_t)time_freq() * PERIODIC_REPLAN_SECONDS;
				m_NextReanchor = 0;
				m_RevealProgress = m_RevealStart;
				SetStatusMessage(m_PlanUsedFreeze ?
							 "AUTO-FINISH: no full route, walking the partial one (freeze ahead)" :
							 "AUTO-FINISH: no full route, walking the partial one",
					4.0f);
				ClearSearchData();
				return;
			}
			m_HasPartialCandidate = false;
			m_vPartialPath.clear();
			m_vPartialPath.shrink_to_fit();
			str_copy(m_aPlanError, "partial routes stopped making progress");
		}
		m_PlanRetries++;
		if(m_PlanRetries >= MAX_PLAN_RETRIES)
		{
			char aMsg[192];
			str_format(aMsg, sizeof(aMsg), "AUTO-FINISH FAILED: %s",
				m_aPlanError[0] != 0 ? m_aPlanError : "no route to the finish found");
			SetStatusMessage(aMsg, 6.0f);
			Deactivate(REASON_FAILED, aMsg);
			return;
		}
		m_RunState = 2;
		m_NextPlanTryTime = time_get() + time_freq() * 3;
		m_SearchAttempt = 0;
		return;
	}
	// the next attempt is started by OnUpdate
}

// ==== search: expansion ===================================================

void CAutoFinish::ExpandNode(int NodeIdx)
{
	const SSearchNode From = m_vSearchNodes[NodeIdx]; // copy: the vector may grow while pushing

	if(From.m_FrozenTicks > 0)
	{
		// frozen: wait it out
		SPlanAction Action;
		SSearchNode Out;
		if(SimulateAction(From, Action, m_SearchMacro, Out))
		{
			PushSuccessor(NodeIdx, Out);
		}
		return;
	}

	// walk, fall, release the hook
	for(int Dir = -1; Dir <= 1; Dir++)
	{
		SPlanAction Action;
		Action.m_Dir = Dir;
		SSearchNode Out;
		if(SimulateAction(From, Action, m_SearchMacro, Out))
		{
			PushSuccessor(NodeIdx, Out);
		}
	}

	// jumps
	for(int Dir = -1; Dir <= 1; Dir++)
	{
		SPlanAction Action;
		Action.m_Dir = Dir;
		Action.m_Jump = true;
		SSearchNode Out;
		if(SimulateAction(From, Action, m_SearchMacro, Out))
		{
			PushSuccessor(NodeIdx, Out);
		}
	}

	// fire the hook at nearby walls
	if(g_Config.m_ClAutoFinishHook && From.m_HookState != HOOK_GRABBED)
	{
		vec2 aAnchors[MAX_ANCHORS];
		const int NumAnchors = CollectHookAnchors(From.m_Pos, aAnchors, MAX_ANCHORS);
		for(int i = 0; i < NumAnchors; i++)
		{
			SPlanAction Action;
			Action.m_HookMode = 1;
			Action.m_HookTarget = aAnchors[i];
			SSearchNode Out;
			if(SimulateAction(From, Action, m_SearchMacro + HOOK_FIRE_TICKS, Out))
			{
				PushSuccessor(NodeIdx, Out);
			}
		}
	}

	// keep holding an attached hook and steer the swing
	if(From.m_HookState == HOOK_GRABBED)
	{
		for(int Dir = -1; Dir <= 1; Dir++)
		{
			SPlanAction Action;
			Action.m_Dir = Dir;
			Action.m_HookMode = 2;
			Action.m_HookTarget = From.m_HookPos;
			SSearchNode Out;
			if(SimulateAction(From, Action, m_SearchMacro, Out))
			{
				PushSuccessor(NodeIdx, Out);
			}
		}
	}
}

int CAutoFinish::CollectHookAnchors(const vec2 &Pos, vec2 *pAnchors, int MaxAnchors)
{
	UpdateTuningZone(Pos);
	const int HookLength = (int)m_SearchCore.m_Tuning.m_HookLength;
	const int Range = std::min((int)(HookLength / CELL) + 1, 16);
	if(Range < 2 || HookLength < 60)
	{
		return 0;
	}

	CCollision *pCol = Collision();
	const int W = pCol->GetWidth();
	const int H = pCol->GetHeight();
	const CTile *pTiles = pCol->GameLayer();
	if(W <= 0 || H <= 0 || !pTiles)
	{
		return 0;
	}

	const int Cx = (int)std::floor(Pos.x / CELL);
	const int Cy = (int)std::floor(Pos.y / CELL);

	// collect the best solid tile centers first (cheap), raycast later
	struct SCandidate
	{
		vec2 m_Center;
		float m_Score;
	};
	SCandidate aCands[MAX_ANCHORS * 3];
	int NumCands = 0;
	for(int Dy = -Range; Dy <= Range; Dy++)
	{
		for(int Dx = -Range; Dx <= Range; Dx++)
		{
			const int Tx = Cx + Dx;
			const int Ty = Cy + Dy;
			if(Tx < 0 || Ty < 0 || Tx >= W || Ty >= H)
			{
				continue;
			}
			if(pTiles[Ty * W + Tx].m_Index != TILE_SOLID)
			{
				continue;
			}
			const vec2 Center(Tx * CELL + 16.0f, Ty * CELL + 16.0f);
			const float d = distance(Pos, Center);
			if(d < 34.0f || d > HookLength - 8.0f)
			{
				continue;
			}
			const float Score = RelaxedTicksAt(Center) + d * 0.02f;
			if(NumCands < (int)(sizeof(aCands) / sizeof(aCands[0])))
			{
				aCands[NumCands].m_Center = Center;
				aCands[NumCands].m_Score = Score;
				NumCands++;
			}
			else
			{
				// replace the worst candidate if we are better
				int Worst = 0;
				for(int k = 1; k < NumCands; k++)
				{
					if(aCands[k].m_Score > aCands[Worst].m_Score)
					{
						Worst = k;
					}
				}
				if(Score < aCands[Worst].m_Score)
				{
					aCands[Worst].m_Center = Center;
					aCands[Worst].m_Score = Score;
				}
			}
		}
	}

	// sort candidates by score
	for(int i = 1; i < NumCands; i++)
	{
		SCandidate Key = aCands[i];
		int j = i - 1;
		while(j >= 0 && aCands[j].m_Score > Key.m_Score)
		{
			aCands[j + 1] = aCands[j];
			j--;
		}
		aCands[j + 1] = Key;
	}

	int Count = 0;
	for(int i = 0; i < NumCands && Count < MaxAnchors; i++)
	{
		const vec2 &Center = aCands[i].m_Center;
		vec2 Dir = Center - Pos;
		const float Len = length(Dir);
		if(Len < 1.0f)
		{
			continue;
		}
		Dir /= Len;
		vec2 HitPos;
		const int Hit = pCol->IntersectLineTeleHook(Pos, Center + Dir * 40.0f, &HitPos, nullptr, nullptr);
		if(Hit != TILE_SOLID)
		{
			continue; // blocked (unhookable) or nothing to grab
		}
		if(distance(Pos, HitPos) > HookLength - 4.0f)
		{
			continue;
		}
		// skip anchors that are too close to already accepted ones
		bool Duplicate = false;
		for(int k = 0; k < Count; k++)
		{
			if(distance(HitPos, pAnchors[k]) < 24.0f)
			{
				Duplicate = true;
				break;
			}
		}
		if(!Duplicate)
		{
			pAnchors[Count++] = HitPos;
		}
	}
	return Count;
}

// ==== search: physical simulation =========================================

void CAutoFinish::RestoreSearchCore(const SSearchNode &Node)
{
	m_SearchCore.m_Pos = Node.m_Pos;
	m_SearchCore.m_Vel = Node.m_Vel;
	m_SearchCore.m_HookState = Node.m_HookState;
	m_SearchCore.m_HookPos = Node.m_HookPos;
	if(Node.m_HookState == HOOK_GRABBED)
	{
		vec2 Dir = Node.m_HookPos - Node.m_Pos;
		const float Len = length(Dir);
		m_SearchCore.m_HookDir = Len > 0.001f ? Dir / Len : vec2(0.0f, -1.0f);
	}
	else
	{
		m_SearchCore.m_HookDir = vec2(0.0f, 0.0f);
	}
	m_SearchCore.m_HookTick = 0;
	m_SearchCore.m_NewHook = false;
	m_SearchCore.m_HookTeleBase = m_SearchCore.m_Pos;
	m_SearchCore.SetHookedPlayer(-1);
	m_SearchCore.m_Jumped = Node.m_Jumped;
	m_SearchCore.m_JumpedTotal = Node.m_JumpedTotal;
	m_SearchCore.m_Jumps = Node.m_Jumps;
	m_SearchCore.m_EndlessJump = Node.m_EndlessJump;
	m_SearchCore.m_TriggeredEvents = 0;
	m_SearchCore.m_LeftWall = true;
	m_SearchCore.m_Colliding = 0;
}

void CAutoFinish::UpdateTuningZone(const vec2 &Pos)
{
	int Zone = 0;
	if(GameClient()->m_PredictedWorld.m_WorldConfig.m_UseTuneZones)
	{
		Zone = Collision()->IsTune(Collision()->GetMapIndex(Pos));
	}
	Zone = std::clamp(Zone, 0, TuneZone::NUM - 1);
	m_SearchCore.m_Tuning = *GameClient()->GetTuning(Zone);
}

bool CAutoFinish::SimulateAction(const SSearchNode &From, const SPlanAction &Action, int MaxTicks, SSearchNode &Out)
{
	RestoreSearchCore(From);

	int FrozenTicks = From.m_FrozenTicks;
	bool GaveUpHook = false;
	bool Teleported = false;
	bool GoalHit = false;
	int Ticks = 0;

	for(int T = 0; T < MaxTicks; T++)
	{
		// ---- the input of this tick ----
		CNetObj_PlayerInput &Input = m_SearchCore.m_Input;
		Input.m_Direction = 0;
		Input.m_Jump = 0;
		Input.m_Hook = 0;
		Input.m_Fire = 0;
		Input.m_WantedWeapon = 0;
		Input.m_NextWeapon = 0;
		Input.m_PrevWeapon = 0;

		if(FrozenTicks > 0)
		{
			// frozen: no input at all, gravity and friction keep acting
			FrozenTicks--;
		}
		else
		{
			Input.m_Direction = Action.m_Dir;
			Input.m_Jump = (T == 0 && Action.m_Jump) ? 1 : 0;
			if(Action.m_HookMode == 2)
			{
				Input.m_Hook = 1;
			}
			else if(Action.m_HookMode == 1)
			{
				if(m_SearchCore.m_HookState == HOOK_GRABBED)
				{
					Input.m_Hook = 1; // attached, keep dragging
				}
				else if(!GaveUpHook && T < HOOK_FIRE_TICKS)
				{
					Input.m_Hook = 1; // still flying
				}
				else
				{
					GaveUpHook = true; // missed, stop pressing
				}
			}
		}

		// aim
		vec2 Rel = vec2(1.0f, 0.0f);
		if(Action.m_HookMode != 0)
		{
			Rel = Action.m_HookTarget - m_SearchCore.m_Pos;
		}
		if(length(Rel) < 1.0f)
		{
			Rel = vec2(1.0f, 0.0f);
		}
		Input.m_TargetX = round_to_int(Rel.x);
		Input.m_TargetY = round_to_int(Rel.y);

		// ---- the physics tick ----
		UpdateTuningZone(m_SearchCore.m_Pos);
		m_SearchCore.Tick(true, true);

		// speedup tiles at the current position
		{
			const int SpeedIndex = Collision()->GetMapIndex(m_SearchCore.m_Pos);
			if(Collision()->IsSpeedup(SpeedIndex))
			{
				vec2 Direction;
				int Force, MaxSpeed, Type;
				Collision()->GetSpeedup(SpeedIndex, &Direction, &Force, &MaxSpeed, &Type);
				if(Force != 0)
				{
					if(Type == TILE_SPEED_BOOST)
					{
						if(MaxSpeed == 0)
						{
							MaxSpeed = 100;
						}
						const float CurrentDirectionalSpeed = dot(Direction, m_SearchCore.m_Vel);
						const float TempMaxSpeed = MaxSpeed / 5.0f;
						if(CurrentDirectionalSpeed + Force > TempMaxSpeed)
						{
							m_SearchCore.m_Vel += Direction * (TempMaxSpeed - CurrentDirectionalSpeed);
						}
						else
						{
							m_SearchCore.m_Vel += Direction * (float)Force;
						}
					}
					else
					{
						if(Force == 255 && MaxSpeed != 0)
						{
							m_SearchCore.m_Vel = Direction * (MaxSpeed / 5.0f);
						}
						else
						{
							m_SearchCore.m_Vel += Direction * (float)Force;
						}
					}
				}
			}
		}

		// ---- the movement ----
		const vec2 PrevPos = m_SearchCore.m_Pos;
		m_SearchCore.Move();
		m_SearchCore.Quantize();

		// ---- tiles crossed by this movement ----
		bool Death, FreezeHit, UnfreezeHit, DeepFreezeHit, Goal, Tele, EvilTele;
		vec2 TelePos;
		int JumpRefill, JumpsSet;
		HandleSimTiles(PrevPos, m_SearchCore.m_Pos, Death, FreezeHit, UnfreezeHit, DeepFreezeHit, Goal, Tele, EvilTele, TelePos, JumpRefill, JumpsSet);

		Ticks++;

		if(Death || DeepFreezeHit)
		{
			return false; // never risk dying or deep/live-freezing
		}
		if(FreezeHit && !m_SearchCore.m_Super && !m_SearchCore.m_Invincible)
		{
			if(!m_SearchAllowFreeze)
			{
				return false;
			}
			if(FrozenTicks == 0)
			{
				FrozenTicks = std::max(1, g_Config.m_SvFreezeDelay) * 50;
			}
		}
		if(UnfreezeHit)
		{
			FrozenTicks = 0;
		}
		if(JumpRefill)
		{
			m_SearchCore.m_Jumped = 0;
			m_SearchCore.m_JumpedTotal = 0;
		}
		if(JumpsSet != -999 && JumpsSet != m_SearchCore.m_Jumps)
		{
			m_SearchCore.m_Jumps = JumpsSet;
		}
		if(Goal)
		{
			GoalHit = true;
			break;
		}
		if(Tele)
		{
			m_SearchCore.m_Pos = TelePos;
			if(EvilTele)
			{
				m_SearchCore.m_Vel = vec2(0.0f, 0.0f);
			}
			m_SearchCore.m_HookState = HOOK_IDLE;
			m_SearchCore.m_HookPos = m_SearchCore.m_Pos;
			m_SearchCore.m_HookDir = vec2(0.0f, 0.0f);
			m_SearchCore.SetHookedPlayer(-1);
			Teleported = true;
			break;
		}
	}

	// ---- snapshot the resulting state ----
	Out.m_Pos = m_SearchCore.m_Pos;
	Out.m_Vel = m_SearchCore.m_Vel;
	Out.m_HookState = m_SearchCore.m_HookState == HOOK_GRABBED ? HOOK_GRABBED : HOOK_IDLE;
	Out.m_HookPos = Out.m_HookState == HOOK_GRABBED ? m_SearchCore.m_HookPos : Out.m_Pos;
	Out.m_Jumped = m_SearchCore.m_Jumped;
	Out.m_JumpedTotal = m_SearchCore.m_JumpedTotal;
	Out.m_Jumps = m_SearchCore.m_Jumps;
	Out.m_EndlessJump = m_SearchCore.m_EndlessJump;
	Out.m_FrozenTicks = FrozenTicks;
	Out.m_Goal = GoalHit;
	Out.m_G = From.m_G + Ticks;
	Out.m_Parent = -1;
	Out.m_Ticks = Ticks;
	Out.m_Action = Action;
	if(Teleported)
	{
		Out.m_Type = NODE_TELE;
	}
	else if(Action.m_HookMode != 0)
	{
		Out.m_Type = NODE_HOOK;
	}
	else if(Action.m_Jump)
	{
		Out.m_Type = NODE_JUMP;
	}
	else
	{
		Out.m_Type = NODE_WALK;
	}
	return true;
}

void CAutoFinish::HandleSimTiles(const vec2 &Prev, const vec2 &Pos, bool &Death, bool &FreezeHit, bool &UnfreezeHit,
	bool &DeepFreezeHit, bool &Goal, bool &Tele, bool &EvilTele, vec2 &TelePos, int &JumpRefill, int &JumpsSet)
{
	Death = false;
	FreezeHit = false;
	UnfreezeHit = false;
	DeepFreezeHit = false;
	Goal = false;
	Tele = false;
	EvilTele = false;
	JumpRefill = 0;
	JumpsSet = -999;
	TelePos = vec2(0.0f, 0.0f);

	CCollision *pCol = Collision();
	const int W = pCol->GetWidth();
	const int H = pCol->GetHeight();
	if(W <= 0 || H <= 0)
	{
		return;
	}

	const float d = distance(Prev, Pos);
	const int Steps = std::max(1, (int)(d / 8.0f) + 1);
	int LastCell = -1;
	for(int i = 0; i <= Steps; i++)
	{
		const float a = (float)i / (float)Steps;
		const vec2 P = mix(Prev, Pos, a);
		const int Nx = std::clamp((int)std::floor(P.x / CELL), 0, W - 1);
		const int Ny = std::clamp((int)std::floor(P.y / CELL), 0, H - 1);
		const int Cell = Ny * W + Nx;
		if(Cell == LastCell)
		{
			continue;
		}
		LastCell = Cell;

		const int Tile = pCol->GetTileIndex(Cell);
		const int FTile = pCol->GetFrontTileIndex(Cell);
		const int STile = pCol->GetSwitchType(Cell);

		if(Tile == TILE_DEATH || FTile == TILE_DEATH)
		{
			Death = true;
		}
		if(Tile == TILE_FINISH || FTile == TILE_FINISH)
		{
			Goal = true;
		}
		if(Tile == TILE_UNFREEZE || FTile == TILE_UNFREEZE)
		{
			UnfreezeHit = true;
		}
		if(Tile == TILE_REFILL_JUMPS || FTile == TILE_REFILL_JUMPS)
		{
			JumpRefill = 1;
		}

		if(!m_SearchCore.m_Super && !m_SearchCore.m_Invincible)
		{
			if(Tile == TILE_FREEZE || FTile == TILE_FREEZE)
			{
				FreezeHit = true;
			}
			if(Tile == TILE_DFREEZE || FTile == TILE_DFREEZE)
			{
				DeepFreezeHit = true;
			}
			if(Tile == TILE_LFREEZE || FTile == TILE_LFREEZE)
			{
				DeepFreezeHit = true; // live freeze is treated as unusable
			}
			if(STile == TILE_FREEZE || STile == TILE_DFREEZE)
			{
				const int Number = pCol->GetSwitchNumber(Cell);
				bool Active = Number <= 0;
				if(Number > 0 && Number < (int)m_SearchWorld.m_vSwitchers.size())
				{
					Active = m_SearchWorld.m_vSwitchers[Number].m_aStatus[m_SearchTeam];
				}
				if(Active)
				{
					if(STile == TILE_FREEZE)
					{
						FreezeHit = true;
					}
					else
					{
						DeepFreezeHit = true;
					}
				}
			}
		}

		if(STile == TILE_JUMP)
		{
			int NewJumps = pCol->GetSwitchDelay(Cell);
			if(NewJumps == 255)
			{
				NewJumps = -1;
			}
			JumpsSet = NewJumps;
		}

		// teleporters
		const int z = pCol->IsTeleport(Cell);
		if(z && !pCol->TeleOuts(z - 1).empty())
		{
			Tele = true;
			EvilTele = false;
			TelePos = pCol->TeleOuts(z - 1)[0];
		}
		else
		{
			const int e = pCol->IsEvilTeleport(Cell);
			if(e && !pCol->TeleOuts(e - 1).empty())
			{
				Tele = true;
				EvilTele = true;
				TelePos = pCol->TeleOuts(e - 1)[0];
			}
		}
	}
}

// ==== search: states and heuristic =======================================

void CAutoFinish::PushSuccessor(int ParentIdx, const SSearchNode &State)
{
	if(State.m_Ticks <= 0)
	{
		return;
	}

	const float hFin = RelaxedTicksAt(State.m_Pos);
	if(hFin > 1e29f)
	{
		return; // this cell can never reach a finish
	}
	if(hFin > m_SearchStartRelaxed + CORRIDOR_SLACK_TICKS)
	{
		return; // too far off the corridor towards the finish
	}
	// priority heuristic: time to the finish, or - when a segment sub-goal
	// is set - the straight-line time to it, whichever is tighter
	float h = hFin;
	if(m_SearchSubGoalValid)
	{
		h = std::min(h, distance(State.m_Pos, m_SearchSubGoal) / HEUR_VMAX);
	}

	const float g = State.m_G;
	const uint64_t Key = StateKey(State);
	const auto It = m_BestG.find(Key);
	if(It != m_BestG.end() && It->second <= g + 0.05f)
	{
		return; // we already know a route that is at least as fast to this state
	}
	m_BestG[Key] = g;

	if((int)m_vSearchNodes.size() >= m_SearchMaxNodes)
	{
		m_SearchBudgetHit = true;
		return;
	}

	SSearchNode Node = State;
	Node.m_G = g;
	Node.m_Parent = ParentIdx;
	Node.m_SubGoalHit = m_SearchSubGoalValid &&
			    distance(Node.m_Pos, m_SearchSubGoal) < SUBGOAL_REACH_PX;
	m_vSearchNodes.push_back(Node);
	m_OpenHeap.push({g + h, (int)m_vSearchNodes.size() - 1});

	if(hFin < m_SearchBestH)
	{
		m_SearchBestH = hFin;
		m_SearchBestNode = (int)m_vSearchNodes.size() - 1;
	}
}

uint64_t CAutoFinish::StateKey(const SSearchNode &Node) const
{
	const int Qx = (int)std::round(Node.m_Pos.x / POS_QUANT);
	const int Qy = (int)std::round(Node.m_Pos.y / POS_QUANT);
	const int Vx = (int)std::round(Node.m_Vel.x / VEL_QUANT);
	const int Vy = (int)std::round(Node.m_Vel.y / VEL_QUANT);

	uint64_t Key = 1469598103934665603ULL;
	const auto Mix = [&Key](int Value) {
		Key ^= (uint32_t)Value;
		Key *= 1099511628211ULL;
	};
	Mix(Qx);
	Mix(Qy);
	Mix(Vx);
	Mix(Vy);
	Mix(Node.m_HookState == HOOK_GRABBED ? 1 : 0);
	if(Node.m_HookState == HOOK_GRABBED)
	{
		Mix((int)std::round(Node.m_HookPos.x / HOOK_POS_QUANT));
		Mix((int)std::round(Node.m_HookPos.y / HOOK_POS_QUANT));
	}
	Mix(Node.m_Jumped);
	Mix(Node.m_JumpedTotal);
	Mix(Node.m_Jumps);
	Mix(Node.m_EndlessJump ? 1 : 0);
	Mix(Node.m_FrozenTicks / 5);
	return Key;
}

void CAutoFinish::BuildRelaxedTicks()
{
	if(m_RelaxedValid)
	{
		return;
	}
	m_RelaxedValid = true;

	CCollision *pCol = Collision();
	const int W = pCol->GetWidth();
	const int H = pCol->GetHeight();
	const CTile *pTiles = pCol->GameLayer();
	const CTile *pFront = pCol->FrontLayer();
	if(W <= 0 || H <= 0 || !pTiles)
	{
		return;
	}

	const int NumCells = W * H;
	m_vRelaxedTicks.assign(NumCells, 1e30f);
	m_vRouteParent.assign(NumCells, -1);

	std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>,
		std::greater<std::pair<float, int>>>
		Heap;

	// sources: all finish tiles
	for(int i = 0; i < NumCells; i++)
	{
		if(pTiles[i].m_Index == TILE_FINISH || (pFront && pFront[i].m_Index == TILE_FINISH))
		{
			m_vRelaxedTicks[i] = 0.0f;
			Heap.push({0.0f, i});
		}
	}
	if(Heap.empty())
	{
		str_copy(m_aPlanError, "this map has no finish tile");
		return;
	}

	// teleporter links, both directions (relaxation for the lower bound)
	std::vector<int> vTeleFirst(NumCells, -1);
	std::vector<int> vTeleTo;
	std::vector<int> vTeleNext;
	const CTeleTile *pTele = pCol->TeleLayer();
	if(pTele)
	{
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
					const int OutCell = Oy * W + Ox;
					// in -> out
					vTeleTo.push_back(OutCell);
					vTeleNext.push_back(vTeleFirst[InCell]);
					vTeleFirst[InCell] = (int)vTeleTo.size() - 1;
					// out -> in
					vTeleTo.push_back(InCell);
					vTeleNext.push_back(vTeleFirst[OutCell]);
					vTeleFirst[OutCell] = (int)vTeleTo.size() - 1;
				}
			}
		}
	}

	// Dijkstra from the finish tiles over the relaxed grid
	while(!Heap.empty())
	{
		const float Dist = Heap.top().first;
		const int Idx = Heap.top().second;
		Heap.pop();
		if(Dist > m_vRelaxedTicks[Idx] + 1e-4f)
		{
			continue;
		}
		const int x = Idx % W;
		const int y = Idx / W;

		for(int Dy = -1; Dy <= 1; Dy++)
		{
			for(int Dx = -1; Dx <= 1; Dx++)
			{
				if(Dx == 0 && Dy == 0)
				{
					continue;
				}
				const int Nx = x + Dx;
				const int Ny = y + Dy;
				if(Nx < 0 || Ny < 0 || Nx >= W || Ny >= H)
				{
					continue;
				}
				const int NIdx = Ny * W + Nx;
				const int Raw = pTiles[NIdx].m_Index;
				if(Raw == TILE_SOLID || Raw == TILE_NOHOOK)
				{
					continue;
				}
				const float Cost = std::sqrt((float)(Dx * Dx + Dy * Dy)) * CELL / HEUR_VMAX;
				const float NewDist = Dist + Cost;
				if(NewDist < m_vRelaxedTicks[NIdx] - 1e-4f)
				{
					m_vRelaxedTicks[NIdx] = NewDist;
					m_vRouteParent[NIdx] = Idx;
					Heap.push({NewDist, NIdx});
				}
			}
		}

		for(int Ei = vTeleFirst[Idx]; Ei != -1; Ei = vTeleNext[Ei])
		{
			const int OIdx = vTeleTo[Ei];
			const float NewDist = Dist + 1.0f;
			if(NewDist < m_vRelaxedTicks[OIdx] - 1e-4f)
			{
				m_vRelaxedTicks[OIdx] = NewDist;
				m_vRouteParent[OIdx] = Idx;
				Heap.push({NewDist, OIdx});
			}
		}
	}
}

float CAutoFinish::RelaxedTicksAt(const vec2 &Pos) const
{
	if(m_vRelaxedTicks.empty())
	{
		return 1e30f;
	}
	CCollision *pCol = Collision();
	const int W = pCol->GetWidth();
	const int H = pCol->GetHeight();
	const int x = std::clamp((int)std::floor(Pos.x / CELL), 0, W - 1);
	const int y = std::clamp((int)std::floor(Pos.y / CELL), 0, H - 1);
	return m_vRelaxedTicks[y * W + x];
}

void CAutoFinish::BuildRouteFrom(const vec2 &Pos)
{
	m_vRoute.clear();
	m_RouteIdx = 0;
	m_RouteReachedFinish = false;
	if(m_vRelaxedTicks.empty() || m_vRouteParent.empty())
	{
		return;
	}
	CCollision *pCol = Collision();
	const int W = pCol->GetWidth();
	const int H = pCol->GetHeight();
	if(W <= 0 || H <= 0)
	{
		return;
	}
	int Cell = std::clamp((int)std::floor(Pos.y / CELL), 0, H - 1) * W +
		   std::clamp((int)std::floor(Pos.x / CELL), 0, W - 1);
	// walk the relaxed gradient downhill towards the finish; teleporter
	// links jump across the map, which is exactly what the route should do
	while((int)m_vRoute.size() < ROUTE_MAX_POINTS)
	{
		const int x = Cell % W;
		const int y = Cell / W;
		m_vRoute.push_back(vec2(x * CELL + 16.0f, y * CELL + 16.0f));
		if(m_vRelaxedTicks[Cell] <= 0.0f)
		{
			m_RouteReachedFinish = true;
			break; // a finish tile
		}
		const int Parent = m_vRouteParent[Cell];
		if(Parent < 0 || Parent >= W * H || Parent == Cell)
		{
			break;
		}
		Cell = Parent;
	}
}

void CAutoFinish::UpdateRouteProgress(const vec2 &Pos)
{
	const int To = std::min((int)m_vRoute.size() - 1, m_RouteIdx + 24);
	for(int i = m_RouteIdx + 1; i <= To; i++)
	{
		if(distance(Pos, m_vRoute[i]) < 48.0f)
		{
			m_RouteIdx = i;
		}
	}
}

void CAutoFinish::BuildPlanFromSearch(int GoalNode)
{
	std::vector<int> vChain;
	for(int i = GoalNode; i != -1; i = m_vSearchNodes[i].m_Parent)
	{
		vChain.push_back(i);
	}
	std::reverse(vChain.begin(), vChain.end());

	m_vPath.clear();
	m_PlanTotalTicks = 0;
	m_TotalPathLen = 0.0f;
	vec2 Prev = m_vSearchNodes[vChain[0]].m_Pos;
	for(size_t i = 1; i < vChain.size(); i++)
	{
		const SSearchNode &N = m_vSearchNodes[vChain[i]];
		SPathNode P;
		P.m_Pos = N.m_Pos;
		P.m_Dir = N.m_Action.m_Dir;
		P.m_Jump = N.m_Action.m_Jump;
		P.m_HookMode = N.m_Action.m_HookMode;
		P.m_HookTarget = N.m_Action.m_HookTarget;
		P.m_Ticks = N.m_Ticks;
		P.m_Type = N.m_Type;
		m_vPath.push_back(P);
		m_PlanTotalTicks += N.m_Ticks;
		m_TotalPathLen += distance(Prev, N.m_Pos);
		Prev = N.m_Pos;
	}
}

// ==== rendering ===========================================================

void CAutoFinish::RenderPathStrip()
{
	if(m_vPath.size() < 2 && m_vRoute.size() < 2)
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

	// the global route to the finish as a dim guide line: one continuous path
	// to the end of the map, even while the local segment is being re-planned
	if((int)m_vRoute.size() >= m_RouteIdx + 2)
	{
		std::vector<IGraphics::CFreeformItem> vGuide;
		const float GuideHalf = CoreWidth * 0.30f;
		vec2 Prev = m_vRoute[m_RouteIdx];
		for(int i = m_RouteIdx + 1; i < (int)m_vRoute.size(); i++)
		{
			const vec2 P1 = m_vRoute[i];
			const float Len = distance(Prev, P1);
			if(Len > 1.0f && Len < 200.0f) // do not draw across teleporter jumps
			{
				const vec2 Dir = (P1 - Prev) / Len;
				const vec2 Perp(-Dir.y, Dir.x);
				vGuide.emplace_back(
					Prev + Perp * GuideHalf, P1 + Perp * GuideHalf,
					P1 - Perp * GuideHalf, Prev - Perp * GuideHalf);
			}
			Prev = P1;
		}
		if(!vGuide.empty())
		{
			Graphics()->QuadsBegin();
			Graphics()->SetColor(CoreColor.r, CoreColor.g, CoreColor.b, Alpha * 0.30f);
			Graphics()->QuadsDrawFreeform(vGuide.data(), (int)vGuide.size());
			Graphics()->QuadsEnd();
		}
	}

	const float RevealLen = m_RevealProgress * m_TotalPathLen;

	// collect the visible segments up to the reveal length
	std::vector<std::pair<vec2, vec2>> vSegs;
	float Acc = 0.0f;
	vec2 Head = m_vPath.empty() ?
			    m_vRoute[std::clamp(m_RouteIdx, 0, (int)m_vRoute.size() - 1)] :
			    m_vPath[0].m_Pos;
	vec2 Prev = Head;
	for(size_t i = 1; i < m_vPath.size() && Acc < RevealLen; i++)
	{
		const vec2 P0 = Prev;
		vec2 P1 = m_vPath[i].m_Pos;
		float SegLen = distance(P0, P1);
		Prev = P1;
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

	// pulsing diamond at the finish (the route end when we have a full route)
	{
		const bool HasFinish = m_RouteReachedFinish || m_vPath.size() >= 2;
		const vec2 F = (m_RouteReachedFinish && !m_vRoute.empty()) ?
				       m_vRoute.back() :
				       (m_vPath.empty() ? vec2(0.0f, 0.0f) : m_vPath.back().m_Pos);
		if(HasFinish)
		{
			const float S = 9.0f + 4.0f * (0.5f + 0.5f * std::sin((double)LocalTime() * 5.0f));
			Graphics()->QuadsBegin();
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha * 0.9f);
			IGraphics::CFreeformItem Diamond(
				F.x, F.y - S, F.x + S, F.y,
				F.x, F.y + S, F.x - S, F.y);
			Graphics()->QuadsDrawFreeform(&Diamond, 1);
			Graphics()->QuadsEnd();
		}
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
	else if(m_RunState == 0 || m_RunState == 2)
	{
		if(m_RunState == 2)
		{
			str_copy(aLabel, "AUTO-FINISH · RETRYING...");
		}
		else
		{
			float Progress = 0.0f;
			if(m_SearchStartRelaxed < 1e29f && m_SearchStartRelaxed > 1.0f)
			{
				Progress = 1.0f - m_SearchBestH / m_SearchStartRelaxed;
			}
			if(m_SearchMaxExpansions > 0)
			{
				const float ByExpansions = (float)m_SearchExpansions / (float)m_SearchMaxExpansions;
				Progress = std::max(Progress, ByExpansions * 0.7f);
			}
			Progress = std::clamp(Progress, 0.0f, 0.99f);
			str_format(aLabel, sizeof(aLabel), "AUTO-FINISH · PLANNING (%d/4) · %d%%",
				std::clamp(m_SearchAttempt + 1, 1, MAX_SEARCH_ATTEMPTS), (int)(Progress * 100.0f));
		}
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
			float RoutePct = 0.0f;
			if(m_InitialRelaxed < 1e29f && m_InitialRelaxed > 1.0f)
			{
				RoutePct = std::clamp(1.0f - RelaxedTicksAt(GameClient()->m_PredictedChar.m_Pos) / m_InitialRelaxed, 0.0f, 0.99f);
			}
			str_format(aLabel, sizeof(aLabel), "AUTO-FINISH%s · ROUTE %d%% · %d/%d · ~%.1fs%s",
				m_PartialPlan ? " · PARTIAL" : "",
				(int)(RoutePct * 100.0f),
				std::min(m_PlanIdx + 1, (int)m_vPath.size()), (int)m_vPath.size(),
				m_PlanTotalTicks / 50.0f,
				m_PlanUsedFreeze ? " · freeze ahead" : "");
		}
	}

	const float W = 360.0f;
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
