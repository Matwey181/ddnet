/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

// Pushin client — Pet feature.
//
// Walking mode AI with BFS tile-based pathfinding:
//   1. Every ~0.2s, run a BFS on the tile grid from the pet's tile to
//      the player's tile. BFS treats solid tiles as impassable and can
//      move in 8 directions (including diagonals for jump arcs).
//   2. The first waypoint on the BFS path is the pet's immediate target.
//      The pet runs toward it, jumps over walls, and drops off edges.
//   3. If BFS fails (no path within 200 tiles), fall back to direct
//      chase + jump + hook.
//   4. If stuck for >1.5s, teleport to the player.

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

struct SPetHistoryEntry
{
        vec2 m_Pos;
        bool m_Grounded;
        bool m_Frozen;
};
static SPetHistoryEntry s_aHistory[PET_HISTORY_SIZE];
static int s_HistoryHead = 0;
static int s_HistoryCount = 0;

// BFS pathfinding state — recompute every 0.2s.
constexpr int BFS_MAX_NODES = 20000;
constexpr float BFS_RECOMPUTE_INTERVAL = 0.2f;

struct SBfsNode
{
        int16_t x, y;
        int16_t parentIdx; // index into the node array, -1 = root
};

// Convert world position to tile coordinates.
static inline void WorldToTile(vec2 Pos, int &Tx, int &Ty)
{
        Tx = (int)(Pos.x / TILE_SIZE);
        Ty = (int)(Pos.y / TILE_SIZE);
}

// Convert tile to world center.
static inline vec2 TileToWorld(int Tx, int Ty)
{
        return vec2(Tx * TILE_SIZE + TILE_SIZE / 2.0f, Ty * TILE_SIZE + TILE_SIZE / 2.0f);
}

// Check if a tile is passable (not solid).
static inline bool TilePassable(class CCollision *pCol, int Tx, int Ty)
{
        if(Tx < 0 || Ty < 0)
                return false;
        if(Tx >= pCol->GetWidth() || Ty >= pCol->GetHeight())
                return false;
        return !pCol->IsSolid(Tx, Ty);
}

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

        const bool PlayerGrounded = Collision()->IsOnGround(PlayerPos, CCharacterCore::PhysicalSize());
        bool PlayerFrozen = false;
        if(GameClient()->m_Snap.m_aCharacters[LocalId].m_HasExtendedData)
                PlayerFrozen = GameClient()->m_Snap.m_aCharacters[LocalId].m_ExtendedData.m_FreezeEnd != 0;
        if(GameClient()->m_aClients[LocalId].m_Predicted.m_FreezeEnd != 0)
                PlayerFrozen = true;

        // --- Record history ---
        s_aHistory[s_HistoryHead].m_Pos = PlayerPos;
        s_aHistory[s_HistoryHead].m_Grounded = PlayerGrounded;
        s_aHistory[s_HistoryHead].m_Frozen = PlayerFrozen;
        s_HistoryHead = (s_HistoryHead + 1) % PET_HISTORY_SIZE;
        if(s_HistoryCount < PET_HISTORY_SIZE)
                s_HistoryCount++;

        const float Dt = Client()->RenderFrameTime();
        const float PetSize = 64.0f * ((float)g_Config.m_PushinPetSize / 100.0f);
        const float PetCollideSize = std::max(14.0f, PetSize * 0.44f);

        // --- Get delayed state ---
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
        else // ===== WALKING (BFS pathfinding + smart physics) =====
        {
                if(!m_Init)
                {
                        m_PetPos = PlayerPos;
                        m_PetVel = vec2(0.0f, 0.0f);
                        m_JumpsLeft = 2;
                        m_HookState = 0;
                        m_HookPos = m_PetPos;
                        m_BfsTimer = 0.0f;
                        m_StuckTimer = 0.0f;
                        m_LastPetX = m_PetPos.x;
                        m_Init = true;
                }

                // ================================================================
                // BFS PATHFINDING — recompute every 0.2s
                // ================================================================
                m_BfsTimer -= Dt;
                if(m_BfsTimer <= 0.0f)
                {
                        m_BfsTimer = BFS_RECOMPUTE_INTERVAL;

                        int PetTx, PetTy, TargTx, TargTy;
                        WorldToTile(m_PetPos, PetTx, PetTy);
                        WorldToTile(TargetPos, TargTx, TargTy);

                        // BFS on the tile grid. 8-directional movement.
                        // Solid tiles are impassable. The BFS finds the shortest
                        // tile path from the pet to the target.
                        static SBfsNode aNodes[BFS_MAX_NODES];
                        int NodeCount = 0;
                        aNodes[NodeCount] = {(int16_t)PetTx, (int16_t)PetTy, -1};
                        NodeCount++;

                        // Visited set — use a simple hash. Grid can be up to ~1000x1000
                        // so we use a relative coordinate offset.
                        // Use a bitset for the visited area around the pet.
                        constexpr int BFS_RADIUS = 80; // 80 tiles in each direction
                        constexpr int BFS_SIZE = BFS_RADIUS * 2 + 1;
                        static bool aVisited[BFS_SIZE * BFS_SIZE];
                        for(int i = 0; i < BFS_SIZE * BFS_SIZE; i++) aVisited[i] = false;

                        auto VisitedIdx = [](int x, int y) -> int {
                                int rx = x + BFS_RADIUS;
                                int ry = y + BFS_RADIUS;
                                if(rx < 0 || ry < 0 || rx >= BFS_SIZE || ry >= BFS_SIZE)
                                        return -1;
                                return ry * BFS_SIZE + rx;
                        };

                        auto SetVisited = [&](int x, int y) {
                                int idx = VisitedIdx(x, y);
                                if(idx >= 0)
                                        aVisited[idx] = true;
                        };

                        auto IsVisited = [&](int x, int y) -> bool {
                                int idx = VisitedIdx(x, y);
                                return idx >= 0 && aVisited[idx];
                        };

                        SetVisited(PetTx, PetTy);

                        std::queue<int> Q;
                        Q.push(0); // index of root node

                        bool PathFound = false;
                        int GoalNodeIdx = -1;

                        // 8 directions: N, S, E, W, NE, NW, SE, SW
                        constexpr int dx[] = {0, 0, 1, -1, 1, -1, 1, -1};
                        constexpr int dy[] = {-1, 1, 0, 0, -1, -1, 1, 1};

                        int Iterations = 0;
                        while(!Q.empty() && NodeCount < BFS_MAX_NODES && Iterations < BFS_MAX_NODES)
                        {
                                Iterations++;
                                int CurIdx = Q.front();
                                Q.pop();
                                SBfsNode &Cur = aNodes[CurIdx];

                                // Reached the target tile?
                                if(Cur.x == TargTx && Cur.y == TargTy)
                                {
                                        PathFound = true;
                                        GoalNodeIdx = CurIdx;
                                        break;
                                }

                                for(int d = 0; d < 8; d++)
                                {
                                        int nx = Cur.x + dx[d];
                                        int ny = Cur.y + dy[d];

                                        if(IsVisited(nx, ny))
                                                continue;

                                        // Check passability: the tile itself must be non-solid.
                                        if(!TilePassable(Collision(), nx, ny))
                                        {
                                                SetVisited(nx, ny);
                                                continue;
                                        }

                                        // For diagonal moves, both adjacent tiles must be passable
                                        // (no cutting through corners).
                                        if(d >= 4)
                                        {
                                                if(!TilePassable(Collision(), Cur.x + dx[d], Cur.y) ||
                                                   !TilePassable(Collision(), Cur.x, Cur.y + dy[d]))
                                                {
                                                        SetVisited(nx, ny);
                                                        continue;
                                                }
                                        }

                                        SetVisited(nx, ny);
                                        aNodes[NodeCount] = {(int16_t)nx, (int16_t)ny, (int16_t)CurIdx};
                                        Q.push(NodeCount);
                                        NodeCount++;
                                }
                        }

                        // Extract the first waypoint (the step AFTER the pet's current
                        // tile — this is where the pet should move next).
                        m_HasBfsPath = false;
                        if(PathFound && GoalNodeIdx > 0)
                        {
                                // Walk back from goal to find the first step (child of root).
                                int TraceIdx = GoalNodeIdx;
                                while(aNodes[TraceIdx].parentIdx > 0)
                                        TraceIdx = aNodes[TraceIdx].parentIdx;

                                // TraceIdx is now the child of the root — this is the
                                // first step toward the target.
                                m_BfsWaypoint = TileToWorld(aNodes[TraceIdx].x, aNodes[TraceIdx].y);
                                m_HasBfsPath = true;
                        }
                }

                // ================================================================
                // MOVEMENT — navigate toward the BFS waypoint
                // ================================================================
                const bool OnGround = Collision()->IsOnGround(m_PetPos, PetCollideSize);
                if(OnGround)
                        m_JumpsLeft = 2;

                // Choose the target: BFS waypoint if available, else direct target.
                vec2 NavTarget = m_HasBfsPath ? m_BfsWaypoint : TargetPos;
                const vec2 ToNav = NavTarget - m_PetPos;
                const vec2 ToTarget = TargetPos - m_PetPos;
                const float NavDistX = std::abs(ToNav.x);
                const float TargetDistX = std::abs(ToTarget.x);
                const float TargetDistY = ToTarget.y; // positive = target below
                const float TargetDist = length(ToTarget);
                const float NavDir = (ToNav.x > 0.0f) ? 1.0f : (ToNav.x < 0.0f ? -1.0f : 0.0f);

                // --- Horizontal movement ---
                const float MinDist = 40.0f;
                const float RunSpeed = 600.0f;

                // If close to the player (not just the waypoint), slow down.
                if(TargetDistX > MinDist + 10.0f)
                        m_PetVel.x = NavDir * RunSpeed;
                else if(TargetDistX < MinDist - 10.0f)
                        m_PetVel.x = -NavDir * RunSpeed * 0.5f;
                else
                        m_PetVel.x *= 0.5f;

                // --- Gravity ---
                m_PetVel.y += 900.0f * Dt;

                // ================================================================
                // JUMPING
                // ================================================================
                bool WantDrop = (TargetDistY > 30.0f && TargetDist > MinDist); // target below

                if(TargetDist > MinDist && m_JumpsLeft > 0 && !WantDrop)
                {
                        bool ShouldJump = false;

                        // REPLAY: player was airborne → jump.
                        if(OnGround && !TargetGrounded)
                                ShouldJump = true;

                        // Wall ahead.
                        if(OnGround)
                        {
                                const vec2 CheckPos = m_PetPos + vec2(NavDir * PetCollideSize * 0.7f, 0.0f);
                                if(Collision()->CheckPoint(CheckPos))
                                        ShouldJump = true;
                        }

                        // Target above.
                        if(OnGround && TargetDistY < -25.0f)
                                ShouldJump = true;

                        // Stuck.
                        if(OnGround && TargetDist > 80.0f && std::abs(m_PetVel.x) < 80.0f)
                                ShouldJump = true;

                        // Double jump in air.
                        if(!OnGround && m_JumpsLeft >= 1 && TargetDistY < -30.0f && m_PetVel.y > -50.0f && m_PetVel.y < 200.0f)
                                ShouldJump = true;

                        if(ShouldJump)
                        {
                                bool WasAirJump = !OnGround;
                                m_PetVel.y = -550.0f;
                                m_JumpsLeft--;
                                if(WasAirJump)
                                        GameClient()->m_Effects.AirJump(m_PetPos, 1.0f, 0.7f);
                        }
                }

                // ================================================================
                // HOOK
                // ================================================================
                bool ShouldHook = false;

                if(TargetDistY < -60.0f && TargetDist > 100.0f && m_JumpsLeft == 0)
                        ShouldHook = true;

                // Stuck detection — track X movement.
                if(std::abs(m_PetPos.x - m_LastPetX) < 1.0f && OnGround && TargetDist > 60.0f)
                        m_StuckTimer += Dt;
                else
                        m_StuckTimer = 0.0f;
                m_LastPetX = m_PetPos.x;

                if(m_StuckTimer > 0.3f)
                        ShouldHook = true;

                // TELEPORT: if stuck for >1.5s, teleport to the player.
                if(m_StuckTimer > 1.5f)
                {
                        m_PetPos = PlayerPos;
                        m_PetVel = vec2(0.0f, 0.0f);
                        m_StuckTimer = 0.0f;
                        m_HookState = 0;
                        m_JumpsLeft = 2;
                        ShouldHook = false;
                }

                if(ShouldHook && m_HookState == 0 && m_HookCooldown <= 0.0f)
                {
                        // Try 5 angles: direct, high-left, high-right, straight up, diagonal.
                        const vec2 HookDirs[] = {
                                normalize(ToTarget),
                                normalize(vec2(-1.0f, -1.0f)),
                                normalize(vec2(1.0f, -1.0f)),
                                vec2(0.0f, -1.0f),
                                normalize(vec2(ToTarget.x * 0.3f, ToTarget.y - 100.0f))
                        };

                        for(int attempt = 0; attempt < 5 && m_HookState == 0; attempt++)
                        {
                                vec2 HookEnd = m_PetPos + HookDirs[attempt] * 500.0f;
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

                // --- Move ---
                bool Grounded = false;
                vec2 VelPerFrame = m_PetVel * Dt;
                Collision()->MoveBox(&m_PetPos, &VelPerFrame, vec2(PetCollideSize, PetCollideSize), vec2(0.0f, 0.0f), &Grounded);
                m_PetVel = VelPerFrame / std::max(Dt, 0.001f);
                if(Grounded && m_PetVel.y > 0.0f)
                        m_PetVel.y = 0.0f;

                // --- Render hook ---
                if(m_HookState == 2)
                {
                        const float HookDist = length(m_HookPos - m_PetPos);
                        const vec2 ChainDir = (HookDist > 0.1f) ? normalize(m_PetPos - m_HookPos) : vec2(1.0f, 0.0f);
                        const float HookAngle = angle(ChainDir) + pi;

                        Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHookHead);
                        Graphics()->QuadsSetRotation(HookAngle);
                        Graphics()->QuadsBegin();
                        Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
                        IGraphics::CQuadItem HeadQuad(m_HookPos.x - 8.0f, m_HookPos.y - 8.0f, 16.0f, 16.0f);
                        Graphics()->QuadsDrawTL(&HeadQuad, 1);
                        Graphics()->QuadsEnd();

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

        // --- Smooth look direction ---
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

        // --- Build tee render info ---
        const CSkin *pSkin = GameClient()->m_Skins.Find(g_Config.m_PushinPetSkin);
        if(pSkin == nullptr)
                pSkin = GameClient()->m_Skins.Find("default");
        if(pSkin == nullptr)
                return;

        CTeeRenderInfo Info;
        Info.Apply(pSkin);
        Info.m_Size = PetSize;

        // --- Freeze skin ---
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
        if(g_Config.m_PushinPetMode == 0)
        {
                pState = CAnimState::GetIdle();
        }
        else
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
