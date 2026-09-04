/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

// Pushin client — Pet feature.
//
// The pet is a mini character with real physics: gravity, ground collision,
// wall collision, walking, jumping. It follows the local player with a
// configurable delay by chasing a "ghost" of the player's past position.
//
// Flying mode: the pet floats above the player (no physics, smooth interp).
// Walking mode: the pet is a physics body that runs on the ground, jumps
// over obstacles, and chases the player's delayed position.

#include "pushin_pet.h"

#include <base/math.h>
#include <engine/shared/config.h>
#include <game/client/animstate.h>
#include <game/client/components/controls.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/client/skin.h>
#include <generated/client_data.h>

#include <vector>

// History buffer: stores player positions for the last ~2 seconds so the
// pet can follow with a delay.
constexpr int PET_HISTORY_SIZE = 120; // ~2s at 60fps
static vec2 s_aPlayerHistory[PET_HISTORY_SIZE];
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

        // --- Record player position into history ---
        s_aPlayerHistory[s_HistoryHead] = PlayerPos;
        s_HistoryHead = (s_HistoryHead + 1) % PET_HISTORY_SIZE;
        if(s_HistoryCount < PET_HISTORY_SIZE)
                s_HistoryCount++;

        const float Dt = Client()->RenderFrameTime();
        const float PetSize = 64.0f * ((float)g_Config.m_PushinPetSize / 100.0f);
        // Pet collision size (proportional to render size, min 14).
        const float PetCollideSize = std::max(14.0f, PetSize * 0.44f);

        // --- Get delayed target position ---
        // Delay is in centiseconds (30 = 0.3s). Convert to frames at ~60fps.
        const int DelayFrames = std::clamp((int)((float)g_Config.m_PushinPetDelay / 100.0f * 60.0f), 0, PET_HISTORY_SIZE - 1);
        vec2 TargetPos;
        if(s_HistoryCount > DelayFrames)
        {
                const int Idx = (s_HistoryHead - 1 - DelayFrames + PET_HISTORY_SIZE) % PET_HISTORY_SIZE;
                TargetPos = s_aPlayerHistory[Idx];
        }
        else
        {
                TargetPos = PlayerPos;
        }

        if(g_Config.m_PushinPetMode == 0) // ===== FLYING =====
        {
                // Smooth interpolation toward target + offset.
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
        else // ===== WALKING (physics-based) =====
        {
                if(!m_Init)
                {
                        m_PetPos = TargetPos;
                        m_PetVel = vec2(0.0f, 0.0f);
                        m_JumpsLeft = 2;
                        m_HookState = 0;
                        m_HookPos = m_PetPos;
                        m_Init = true;
                }

                // --- AI: chase the delayed player position ---
                const vec2 ToTarget = TargetPos - m_PetPos;
                const float DistX = std::abs(ToTarget.x);
                const float DistY = std::abs(ToTarget.y);
                const float Dist = length(ToTarget);
                const float TargetDir = (ToTarget.x > 0.0f) ? 1.0f : (ToTarget.x < 0.0f ? -1.0f : 0.0f);

                const bool OnGround = Collision()->IsOnGround(m_PetPos, PetCollideSize);

                // --- Reset jumps when on ground ---
                if(OnGround)
                        m_JumpsLeft = 2;

                // --- Horizontal movement: keep a minimum distance ---
                const float MinDist = 40.0f;
                const float RunSpeed = 500.0f;
                if(DistX > MinDist + 10.0f)
                        m_PetVel.x = mix(m_PetVel.x, TargetDir * RunSpeed, 0.15f);
                else if(DistX < MinDist - 10.0f)
                        m_PetVel.x = mix(m_PetVel.x, -TargetDir * RunSpeed * 0.5f, 0.15f);
                else
                        m_PetVel.x *= 0.85f;

                // --- Gravity ---
                m_PetVel.y += 900.0f * Dt;

                // --- Jumping logic (with double jump) ---
                // Ground jump: m_JumpsLeft >= 1 (starts at 2, so first jump uses 1)
                // Air jump (double): m_JumpsLeft >= 1 after ground jump used one
                if(Dist > MinDist)
                {
                        bool ShouldJump = false;

                        // Wall ahead?
                        if(OnGround && DistX > 5.0f)
                        {
                                const vec2 CheckPos = m_PetPos + vec2(TargetDir * PetCollideSize * 0.7f, 0.0f);
                                if(Collision()->CheckPoint(CheckPos))
                                        ShouldJump = true;
                        }

                        // Target above us?
                        if(OnGround && ToTarget.y < -30.0f)
                                ShouldJump = true;

                        // Stuck on ground and target is far?
                        if(OnGround && Dist > 100.0f && std::abs(m_PetVel.x) < 50.0f)
                                ShouldJump = true;

                        // In air — use double jump if target is above.
                        // Key fix: m_JumpsLeft >= 1 (not >= 2) so the SECOND jump works.
                        if(!OnGround && m_JumpsLeft >= 1 && ToTarget.y < -40.0f && m_PetVel.y > -100.0f)
                                ShouldJump = true;

                        if(ShouldJump && m_JumpsLeft > 0)
                        {
                                m_PetVel.y = -550.0f;
                                m_JumpsLeft--;
                        }
                }

                // --- Hook logic ---
                // If the target is far above or the pet can't reach with jumps, hook.
                const bool ShouldHook = (ToTarget.y < -80.0f && Dist > 120.0f) ||
                                        (Dist > 200.0f && !OnGround && m_JumpsLeft == 0);
                if(ShouldHook && m_HookState == 0)
                {
                        vec2 HookDir = normalize(ToTarget);
                        vec2 HookEnd = m_PetPos + HookDir * 400.0f;
                        vec2 OutCol, OutBeforeCol;
                        int Hit = Collision()->IntersectLine(m_PetPos, HookEnd, &OutCol, &OutBeforeCol);
                        if(Hit != 0)
                        {
                                m_HookState = 2;
                                m_HookPos = OutCol;
                                m_HookTimer = 0.5f;
                        }
                }

                // --- Apply hook pull ---
                if(m_HookState == 2 && m_HookTimer > 0.0f)
                {
                        m_HookTimer -= Dt;
                        vec2 HookDir = m_HookPos - m_PetPos;
                        float HookDist = length(HookDir);
                        if(HookDist > 10.0f)
                        {
                                HookDir = normalize(HookDir);
                                m_PetVel.x += HookDir.x * 800.0f * Dt;
                                m_PetVel.y += HookDir.y * 800.0f * Dt;
                        }
                        if(m_HookTimer <= 0.0f || HookDist < 15.0f)
                                m_HookState = 0;
                }

                // --- Clamp velocity ---
                m_PetVel.x = std::clamp(m_PetVel.x, -700.0f, 700.0f);
                m_PetVel.y = std::clamp(m_PetVel.y, -700.0f, 700.0f);

                // --- Move with collision ---
                bool Grounded = false;
                vec2 VelPerFrame = m_PetVel * Dt;
                Collision()->MoveBox(&m_PetPos, &VelPerFrame, vec2(PetCollideSize, PetCollideSize), vec2(0.0f, 0.0f), &Grounded);
                m_PetVel = VelPerFrame / std::max(Dt, 0.001f);
                if(Grounded && m_PetVel.y > 0.0f)
                        m_PetVel.y = 0.0f;

                // --- Render the hook with game textures (hook chain + head) ---
                if(m_HookState == 2)
                {
                        const vec2 HookDir = m_HookPos - m_PetPos;
                        const float HookDist = length(HookDir);
                        const vec2 Dir = normalize(m_PetPos - m_HookPos); // chain direction
                        const float Angle = angle(Dir) + pi;

                        // Hook head
                        Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHookHead);
                        Graphics()->QuadsSetRotation(Angle);
                        Graphics()->QuadsBegin();
                        Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.9f);
                        IGraphics::CQuadItem HeadQuad(m_HookPos.x, m_HookPos.y, 16.0f, 16.0f);
                        Graphics()->QuadsDrawTL(&HeadQuad, 1);
                        Graphics()->QuadsEnd();

                        // Hook chain — draw chain segments every 24px like the player hook
                        Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteHookChain);
                        Graphics()->QuadsSetRotation(Angle);
                        Graphics()->QuadsBegin();
                        Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.9f);
                        std::vector<IGraphics::CQuadItem> aChainQuads;
                        for(float f = 24.0f; f < HookDist; f += 24.0f)
                        {
                                vec2 p = m_HookPos + Dir * f;
                                aChainQuads.emplace_back(p.x, p.y, 24.0f, 16.0f);
                        }
                        if(!aChainQuads.empty())
                                Graphics()->QuadsDrawTL(aChainQuads.data(), aChainQuads.size());
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

        // --- Animation ---
        int Emote = EMOTE_NORMAL;
        if(g_Config.m_PushinPetEmote)
                Emote = PlayerEmote;
        vec2 Dir = g_Config.m_PushinPetLook ? m_LookDir : vec2(PlayerDir, 0.0f);

        // When hooking, override look direction toward the hook target.
        if(g_Config.m_PushinPetMode == 1 && m_HookState == 2)
        {
                vec2 HookLook = m_HookPos - m_PetPos;
                if(length(HookLook) > 0.001f)
                        Dir = normalize(HookLook);
        }

        const CAnimState *pState;
        if(g_Config.m_PushinPetMode == 0) // flying — idle
        {
                pState = CAnimState::GetIdle();
        }
        else // walking — physics-based animation
        {
                const bool OnGround = Collision()->IsOnGround(m_PetPos, PetCollideSize);
                const bool Moving = std::abs(m_PetVel.x) > 50.0f;
                const float WalkSpeed = std::abs(m_PetVel.x) / 500.0f;

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
