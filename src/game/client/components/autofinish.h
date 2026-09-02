/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com                */
#ifndef GAME_CLIENT_COMPONENTS_AUTOFINISH_H
#define GAME_CLIENT_COMPONENTS_AUTOFINISH_H

#include <base/vmath.h>

#include <engine/console.h>

#include <game/client/component.h>
#include <game/gamecore.h>
#include <game/teamscore.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

class CCollision;

/**
 * sh1zooo client: auto-finish bot.
 *
 * Plans the route from the current position of the local player to the
 * finish tiles of the map and then walks the route automatically by
 * injecting movement input every tick.
 *
 * The planner is an A* search over simulated game states: every candidate
 * action (walk left/right, jump, fire the hook at a wall, hold the hook,
 * release it) is simulated with the real CCharacterCore physics, the same
 * code the game uses for client-side prediction. The search therefore
 * models the complete DDRace movement: ground and air control, gravity,
 * jump impulses, the hook flight, attach and drag physics, teleporters,
 * speedup tiles, tune zones, stoppers and switcher states. Freeze tiles
 * are avoided unless no route exists without them; death tiles are never
 * entered.
 *
 * The whole search ladder runs on a dedicated background thread with a
 * generous node budget, so it plans ONE CONTINUOUS ROUTE TO THE FINISH
 * instead of short segments, and the game thread is never blocked by the
 * search (the old frame-sliced searches starved the netcode and caused
 * "connection problems" lags). Everything the search needs about the live
 * game is captured in a seed snapshot before the thread starts, so the
 * planner only ever reads the static map data plus that snapshot.
 *
 * While planning the tee stands still, which keeps the seed valid; when a
 * full route is ready the bot walks it end to end, re-planning only when
 * it gets pushed off the route. The planned route is rendered as a colored
 * strip (culled to the camera view, so even a full-map route stays cheap)
 * that smoothly reveals itself; color, thickness, opacity, visibility and
 * the automatic zoom-out are configurable.
 *
 * Toggle with the "autofinish" console command, the sh1zooo client settings
 * tab or a touch button bound to the "autofinish" command.
 */
class CAutoFinish : public CComponent
{
	// How a trajectory point was reached from its predecessor.
	enum ENodeType
	{
		NODE_WALK = 0, // walking or falling
		NODE_JUMP = 1, // reached with a jump
		NODE_HOOK = 2, // reached with the hook
		NODE_TELE = 3, // reached through a teleporter
	};

	// Why the bot was deactivated.
	enum EDeactivateReason
	{
		REASON_DISABLED = 0,
		REASON_FINISHED = 1,
		REASON_FAILED = 2,
	};

	// What the bot does during one macro step of the plan.
	struct SPlanAction
	{
		int m_Dir = 0; // -1, 0 or +1
		bool m_Jump = false; // press jump on the first tick
		int m_HookMode = 0; // 0 = no hook, 1 = fire and hold at m_HookTarget, 2 = keep holding
		vec2 m_HookTarget = vec2(0.0f, 0.0f); // world position to aim at
	};

	// One point of the planned trajectory. The action stored in the node is
	// the action that leads from the previous point to this one.
	struct SPathNode
	{
		vec2 m_Pos = vec2(0.0f, 0.0f);
		int m_Dir = 0;
		bool m_Jump = false;
		int m_HookMode = 0;
		vec2 m_HookTarget = vec2(0.0f, 0.0f);
		int m_Ticks = 5; // how long the action takes
		unsigned char m_Type = 0; // ENodeType, for rendering
	};

	// One node of the A* search: a full physical state snapshot.
	struct SSearchNode
	{
		vec2 m_Pos = vec2(0.0f, 0.0f);
		vec2 m_Vel = vec2(0.0f, 0.0f);
		vec2 m_HookPos = vec2(0.0f, 0.0f);
		int m_HookState = HOOK_IDLE;
		int m_Jumped = 0;
		int m_JumpedTotal = 0;
		int m_Jumps = 2;
		bool m_EndlessJump = false;
		int m_FrozenTicks = 0;
		bool m_Goal = false; // reached a finish tile
		float m_G = 0.0f; // cost so far in ticks
		int m_Parent = -1;
		int m_Ticks = 5; // ticks of the action that led here
		SPlanAction m_Action;
		unsigned char m_Type = 0; // ENodeType
	};

	// Everything the background planner needs to know about the live game.
	// Captured on the game thread before the search starts, so the planner
	// thread never touches live (mutating) game state - only the static map
	// data and this snapshot.
	struct SPlannerSeed
	{
		vec2 m_Pos = vec2(0.0f, 0.0f);
		vec2 m_Vel = vec2(0.0f, 0.0f);
		int m_HookState = HOOK_IDLE;
		vec2 m_HookPos = vec2(0.0f, 0.0f);
		int m_Jumped = 0;
		int m_JumpedTotal = 0;
		int m_Jumps = 2;
		bool m_EndlessJump = false;
		bool m_Super = false;
		bool m_Invincible = false;
		int m_Team = 0;
		CTeamsCore m_Teams;
		bool m_UseTuneZones = false;
		CTuningParams m_aTunings[TuneZone::NUM];
		std::vector<SSwitchers> m_vSwitchers;
		CCollision *m_pCollision = nullptr;
		bool m_HookAllowed = true;
		int m_FreezeDelay = 3;
	};

	// What kind of plan the background planner produced.
	enum EPlannerResult
	{
		PLAN_NONE = 0,
		PLAN_FULL = 1, // complete route to a finish tile
		PLAN_PARTIAL = 2, // best partial route (emergency fallback)
		PLAN_FAILED = 3, // nothing usable found
		PLAN_ABORTED = 4, // cancelled (replan or shutdown)
	};

	// The plan the background planner hands back to the game thread.
	struct SPlannerResult
	{
		int m_Kind = PLAN_NONE;
		bool m_UsedFreeze = false;
		std::vector<SPathNode> m_vPath;
		int m_TotalTicks = 0;
		float m_PathLen = 0.0f;
		std::vector<vec2> m_vRoute; // guide line to the finish
		bool m_RouteReachedFinish = false;
		float m_StartRelaxed = 1e30f;
		std::vector<float> m_vRelaxedTicks; // main-thread copy for the status
		float m_BestPartialH = 1e30f;
		char m_aError[128] = {};
	};

public:
	CAutoFinish();
	~CAutoFinish() override;

	int Sizeof() const override { return sizeof(*this); }
	void OnConsoleInit() override;
	void OnReset() override;
	void OnMapLoad() override;
	void OnUpdate() override;
	void OnRender() override;

	bool IsActive() const { return m_Active; }

private:
	static void ConAutoFinish(IConsole::IResult *pResult, void *pUserData);

	// --- bot state ---
	bool m_Active = false;
	int m_RunState = 0; // 0 = planning, 1 = running, 2 = failed (waiting for retry)
	bool m_PlanUsedFreeze = false;
	int m_PlanRetries = 0;
	int64_t m_NextPlanTryTime = 0; // time_get() based, 0 = as soon as possible
	int64_t m_LastProgressTime = 0;
	int64_t m_NextZoomCorrect = 0;
	int64_t m_NextReanchor = 0;
	int64_t m_LastUpdateTime = 0;
	int64_t m_PlanAirWaitStart = 0; // waiting to land before (re)planning starts
	int m_StuckCount = 0;
	float m_PlanTickF = 0.0f; // ticks elapsed inside the current plan action
	int m_PlanIdx = 0;
	int m_PlanTotalTicks = 0;
	bool m_ReplanAfterFreeze = false;
	float m_OldZoom = 1.0f;
	bool m_OldZoomValid = false;
	int64_t m_LastRenderTime = 0;

	// --- path (game thread) ---
	std::vector<SPathNode> m_vPath;
	float m_TotalPathLen = 0.0f;
	float m_RevealProgress = 0.0f; // 0..1, controls how much of the strip is drawn
	float m_RevealStart = 0.0f; // progress value the reveal restarts at after replans

	// --- partial-route fallback policy (game thread) ---
	bool m_PartialPlan = false; // the executed route ends short of the finish
	float m_LastPartialTargetH = 1e30f; // relaxed ticks at the end of the last walked partial route
	int m_PartialStagnant = 0; // consecutive partial cycles without real progress

	// --- global route to the finish (game thread copies) ---
	std::vector<vec2> m_vRoute; // tile centers from here to the finish (guide line)
	int m_RouteIdx = 0; // how far along the global route we are
	bool m_RouteReachedFinish = false; // the route reached a finish tile
	float m_InitialRelaxed = 1e30f; // relaxed ticks at the first plan, for the route percentage
	bool m_PlanEndsAtFinish = false; // the current plan ends on a finish tile
	int64_t m_WaitLandUntil = 0; // plan exhausted mid-air: wait for the landing

	// --- status message shown above the HUD ---
	char m_aStatusMessage[128] = {};
	float m_StatusMessageTime = 0.0f; // remaining seconds

	// --- background planner thread ---
	std::thread m_PlannerThread;
	std::mutex m_PlannerMutex;
	std::condition_variable m_PlannerCond;
	bool m_PlannerThreadRunning = false; // guarded by m_PlannerMutex
	bool m_PlannerWakeup = false; // guarded by m_PlannerMutex: start planning
	bool m_PlannerQuit = false; // guarded by m_PlannerMutex: exit the thread
	std::atomic<bool> m_PlannerAbort{false}; // cancel the running search
	std::atomic<int> m_PlannerPhase{0}; // 0 idle, 1 searching, 2 result ready
	std::atomic<int> m_PlannerAttempt{0}; // progress for the HUD
	std::atomic<int> m_PlannerProgress{0}; // 0..100 for the HUD
	SPlannerSeed m_PlannerSeed; // guarded by m_PlannerMutex
	SPlannerResult m_PlannerResult; // guarded by m_PlannerMutex
	vec2 m_SeedPos = vec2(0.0f, 0.0f); // game thread: drift guard against the seed
	bool m_SeedPosValid = false;
	int m_DriftRestarts = 0; // search restarts because the seed went stale

	// --- A* search (planner thread only while a search is running) ---
	CWorldCore m_SearchWorld;
	CCharacterCore m_SearchCore;
	SPlannerSeed m_ActiveSeed; // the seed the current search runs with
	bool m_SearchActive = false;
	bool m_SearchAllowFreeze = false;
	bool m_SearchCoarse = false;
	int64_t m_SearchStartWall = 0; // wall-clock start of the current attempt
	int m_SearchMacro = 5; // ticks per macro step
	int m_SearchAttempt = 0; // 0..3, attempt ladder
	int m_SearchExpansions = 0;
	int m_SearchMaxExpansions = 0;
	int m_SearchMaxNodes = 0;
	bool m_SearchOpenExhausted = false;
	bool m_SearchBudgetHit = false;
	bool m_SearchAborted = false;
	int m_GoalNode = -1;
	int m_SearchTeam = 0;
	float m_SearchStartRelaxed = 1e30f;
	float m_SearchBestH = 1e30f;
	int m_SearchBestNode = -1; // node with the smallest heuristic in the current attempt
	std::vector<SSearchNode> m_vSearchNodes;
	std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>,
		std::greater<std::pair<float, int>>>
		m_OpenHeap;
	std::unordered_map<uint64_t, float> m_BestG;

	// --- plan scratch (planner thread) ---
	std::vector<SPathNode> m_vPathWork;
	int m_PlanTotalTicksWork = 0;
	float m_TotalPathLenWork = 0.0f;

	// --- partial candidate of the current search ladder (planner thread) ---
	bool m_HasPartialCandidate = false;
	float m_BestPartialH = 1e30f;
	std::vector<SPathNode> m_vPartialPath;
	int m_PartialTotalTicks = 0;
	float m_PartialPathLen = 0.0f;
	bool m_PartialUsedFreeze = false;

	// --- relaxed per-tile lower bound of the time to the finish ---
	std::vector<float> m_vRelaxedTicks; // planner cache, rebuilt per map
	std::vector<int> m_vRouteParent; // per-cell predecessor on the relaxed grid
	bool m_RelaxedValid = false;
	std::vector<float> m_vRelaxedTicksMain; // game thread copy for the status display
	std::vector<vec2> m_vRouteWork; // planner scratch: guide route from the seed
	bool m_RouteReachedFinishWork = false;

	void Activate();
	void Deactivate(EDeactivateReason Reason, const char *pMessage = nullptr);
	void ClearBotInput();
	void SetStatusMessage(const char *pMessage, float Seconds);
	void RequestReplan(bool QuickReveal);
	void BuildRouteFrom(const vec2 &Pos); // planner: rebuild the guide route scratch
	void UpdateRouteProgress(const vec2 &Pos); // game thread: advance m_RouteIdx

	// planner thread lifecycle
	void StartPlanner(); // game thread: capture the seed and wake the planner
	void PlannerThread(); // planner: thread entry
	void RunPlanner(const SPlannerSeed &Seed, SPlannerResult &Result); // planner: full ladder
	void PublishPlannerProgress(); // planner: update the HUD progress atomics
	void AbortPlanner(bool Wait); // game thread: cancel the running search
	void StopPlannerThread(); // game thread: shutdown and join
	void ApplyPlannerResult(); // game thread: apply a finished plan
	void PlanFailed(const char *pError); // game thread: retry or give up

	// search (planner thread only)
	void StartSearch(const SPlannerSeed &Seed, bool AllowFreeze, bool Coarse);
	bool SearchStep(); // expands nodes; returns true when the attempt ended
	void FinishSearchAttempt();
	void ExpandNode(int NodeIdx);
	int CollectHookAnchors(const vec2 &Pos, vec2 *pAnchors, int MaxAnchors);
	bool SimulateAction(const SSearchNode &From, const SPlanAction &Action, int MaxTicks, SSearchNode &Out);
	void HandleSimTiles(const vec2 &Prev, const vec2 &Pos, bool &Death, bool &FreezeHit, bool &UnfreezeHit,
		bool &DeepFreezeHit, bool &Goal, bool &Tele, bool &EvilTele, vec2 &TelePos, int &JumpRefill, int &JumpsSet);
	void UpdateTuningZone(const vec2 &Pos);
	void PushSuccessor(int ParentIdx, const SSearchNode &State);
	int BuildRelaxedTicks(char *pError, size_t ErrorSize); // 0 = ok, 1 = failed, 2 = aborted
	float RelaxedTicksAt(const vec2 &Pos) const; // planner copy
	float RelaxedTicksMainAt(const vec2 &Pos) const; // game thread copy
	uint64_t StateKey(const SSearchNode &Node) const;
	void RestoreSearchCore(const SSearchNode &Node);
	void BuildPlanFromSearch(int GoalNode); // into the planner scratch
	void ClearSearchData();

	void ApplyAutoZoom(bool Immediate);
	void RenderPathStrip();
	void RenderStatus();
};

#endif
