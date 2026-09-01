/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_AUTOFINISH_H
#define GAME_CLIENT_COMPONENTS_AUTOFINISH_H

#include <base/vmath.h>

#include <engine/console.h>

#include <game/client/component.h>

#include <vector>

/**
 * sh1zooo client: auto-finish bot.
 *
 * Plans the shortest route from the current position of the local player to
 * the finish tiles of the map with an A* search over the tile grid and then
 * walks the route automatically by injecting movement input every tick. The
 * search models the DDRace movement physics (walk speed, gravity, jump
 * impulse, hook range and drag speed, teleporters) and avoids freeze tiles
 * unless no route exists without them. The planned route is rendered as a
 * colored strip that smoothly reveals itself; color, thickness, opacity,
 * visibility and the automatic zoom-out are configurable.
 *
 * Toggle with the "autofinish" console command, the sh1zooo client settings
 * tab or a touch button bound to the "autofinish" command.
 */
class CAutoFinish : public CComponent
{
public:
	// How a path node is reached from its predecessor.
	enum ENodeType
	{
		NODE_WALK = 0, // walking or falling
		NODE_JUMP = 1, // reached with a jump
		NODE_HOOK = 2, // reached by hooking to a wall
		NODE_TELE = 3, // reached through a teleporter
	};

	// Why the bot was deactivated.
	enum EDeactivateReason
	{
		REASON_DISABLED = 0,
		REASON_FINISHED = 1,
		REASON_FAILED = 2,
	};

	struct SPathNode
	{
		vec2 m_Pos; // world position of the tile center
		unsigned char m_Type; // ENodeType
		vec2 m_HookPos; // world position of the hook attach point (NODE_HOOK only)
	};

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

	// --- state ---
	bool m_Active = false;
	int m_RunState = 0; // 0 = planning, 1 = running, 2 = failed (retrying)
	bool m_PlanUsedFreeze = false; // the current route passes through freeze tiles
	int m_PlanRetries = 0;
	int64_t m_NextPlanTryTime = 0; // time_get() based, 0 = as soon as possible
	int64_t m_LastProgressTime = 0;
	int64_t m_NextPeriodicReplan = 0;
	int64_t m_NextZoomCorrect = 0;
	int m_StuckCount = 0;
	int m_BlockedTicks = 0; // ticks pressing a direction without moving
	int m_AirJumpCooldown = 0;
	int m_JumpCooldown = 0;
	int m_JumpHoldTicks = 0;
	int m_HookNodeTicks = 0; // ticks spent on the current hook node
	float m_OldZoom = 1.0f;
	bool m_OldZoomValid = false;
	int64_t m_LastRenderTime = 0;

	// --- path ---
	std::vector<SPathNode> m_vPath;
	int m_NextNode = 0;
	float m_TotalPathLen = 0.0f;
	float m_RevealProgress = 0.0f; // 0..1, controls how much of the strip is drawn
	float m_RevealStart = 0.0f; // progress value the reveal restarts at after replans

	// --- status message shown above the HUD ---
	char m_aStatusMessage[128] = {};
	float m_StatusMessageTime = 0.0f; // remaining seconds
	char m_aPlanError[128] = {};

	// --- A* scratch buffers (reused between plans) ---
	std::vector<float> m_vGScore;
	std::vector<int> m_vFromCell;
	std::vector<unsigned char> m_vEdgeType;
	std::vector<int> m_vHookAttachCell;
	std::vector<unsigned char> m_vClosed;
	// teleporter edges as per-cell linked lists
	std::vector<int> m_vTeleFirst;
	std::vector<int> m_vTeleTo;
	std::vector<int> m_vTeleNext;

	void Activate();
	void Deactivate(EDeactivateReason Reason, const char *pMessage = nullptr);
	void ClearBotInput();
	void SetStatusMessage(const char *pMessage, float Seconds);
	void RequestReplan(bool QuickReveal);
	bool PlanPath(const vec2 &StartPos, bool AllowFreeze);
	void BuildTeleportEdges();
	void ApplyAutoZoom(bool Immediate);
	void RenderPathStrip();
	void RenderStatus();
};

#endif
