/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com                */
#ifndef GAME_CLIENT_COMPONENTS_AUTOFINISH_H
#define GAME_CLIENT_COMPONENTS_AUTOFINISH_H

#include <base/vmath.h>

#include <engine/console.h>

#include <game/client/component.h>
#include <game/gamecore.h>

#include <queue>
#include <unordered_map>
#include <vector>

/**
 * sh1zooo client: auto-finish bot.
 *
 * Plans the fastest route from the current position of the local player to
 * the finish tiles of the map and then walks the route automatically by
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
 * entered. The cost of a route is its duration in ticks, so the found
 * route is the fastest one the search space allows.
 *
 * The search is time-sliced over several frames so the game keeps
 * rendering while the bot is planning. The planned route is rendered as a
 * colored strip that smoothly reveals itself; color, thickness, opacity,
 * visibility and the automatic zoom-out are configurable.
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
		bool m_Goal = false;
		float m_G = 0.0f; // cost so far in ticks
		int m_Parent = -1;
		int m_Ticks = 5; // ticks of the action that led here
		SPlanAction m_Action;
		unsigned char m_Type = 0; // ENodeType
	};

public:
	CAutoFinish();

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
	int64_t m_NextPeriodicReplan = 0;
	int64_t m_NextZoomCorrect = 0;
	int64_t m_NextReanchor = 0;
	int64_t m_LastUpdateTime = 0;
	int m_StuckCount = 0;
	float m_PlanTickF = 0.0f; // ticks elapsed inside the current plan action
	int m_PlanIdx = 0;
	int m_PlanTotalTicks = 0;
	bool m_ReplanAfterFreeze = false;
	float m_OldZoom = 1.0f;
	bool m_OldZoomValid = false;
	int64_t m_LastRenderTime = 0;

	// --- path ---
	std::vector<SPathNode> m_vPath;
	float m_TotalPathLen = 0.0f;
	float m_RevealProgress = 0.0f; // 0..1, controls how much of the strip is drawn
	float m_RevealStart = 0.0f; // progress value the reveal restarts at after replans

	// --- status message shown above the HUD ---
	char m_aStatusMessage[128] = {};
	float m_StatusMessageTime = 0.0f; // remaining seconds
	char m_aPlanError[128] = {};

	// --- A* search (time-sliced across frames) ---
	CWorldCore m_SearchWorld;
	CCharacterCore m_SearchCore;
	bool m_SearchActive = false;
	bool m_SearchAllowFreeze = false;
	bool m_SearchCoarse = false;
	int m_SearchMacro = 5; // ticks per macro step
	int m_SearchAttempt = 0; // 0..3, attempt ladder
	int m_SearchExpansions = 0;
	int m_SearchMaxExpansions = 0;
	int m_SearchMaxNodes = 0;
	bool m_SearchOpenExhausted = false;
	bool m_SearchBudgetHit = false;
	int m_GoalNode = -1;
	int m_SearchTeam = 0;
	float m_SearchStartRelaxed = 1e30f;
	float m_SearchBestH = 1e30f;
	std::vector<SSearchNode> m_vSearchNodes;
	std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>,
		std::greater<std::pair<float, int>>>
		m_OpenHeap;
	std::unordered_map<uint64_t, float> m_BestG;

	// relaxed per-tile lower bound of the time to the finish (cached per map)
	std::vector<float> m_vRelaxedTicks;
	bool m_RelaxedValid = false;

	void Activate();
	void Deactivate(EDeactivateReason Reason, const char *pMessage = nullptr);
	void ClearBotInput();
	void SetStatusMessage(const char *pMessage, float Seconds);
	void RequestReplan(bool QuickReveal);

	// search
	void StartNextSearchAttempt();
	void StartSearch(bool AllowFreeze, bool Coarse);
	bool SearchStep(); // expands nodes until the slice budget is used, returns true when the attempt ended
	void FinishSearchAttempt();
	void ExpandNode(int NodeIdx);
	int CollectHookAnchors(const vec2 &Pos, vec2 *pAnchors, int MaxAnchors);
	bool SimulateAction(const SSearchNode &From, const SPlanAction &Action, int MaxTicks, SSearchNode &Out);
	void HandleSimTiles(const vec2 &Prev, const vec2 &Pos, bool &Death, bool &FreezeHit, bool &UnfreezeHit,
		bool &DeepFreezeHit, bool &Goal, bool &Tele, bool &EvilTele, vec2 &TelePos, int &JumpRefill, int &JumpsSet);
	void UpdateTuningZone(const vec2 &Pos);
	void PushSuccessor(int ParentIdx, const SSearchNode &State);
	void BuildRelaxedTicks();
	float RelaxedTicksAt(const vec2 &Pos) const;
	uint64_t StateKey(const SSearchNode &Node) const;
	void RestoreSearchCore(const SSearchNode &Node);
	void BuildPlanFromSearch(int GoalNode);
	void ClearSearchData();

	void ApplyAutoZoom(bool Immediate);
	void RenderPathStrip();
	void RenderStatus();
};

#endif
