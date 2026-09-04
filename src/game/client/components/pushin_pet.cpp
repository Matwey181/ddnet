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

        // --- Player position and emote ---
        const vec2 PlayerPos = GameClient()->m_aClients[LocalId].m_RenderPos;
        // Emote: read from the character snapshot (CNetObj_Character.m_Emote).
        int PlayerEmote = EMOTE_NORMAL;
        if(GameClient()->m_Snap.m_aCharacters[LocalId].m_Active)
                PlayerEmote = GameClient()->m_Snap.m_aCharacters[LocalId].m_Cur.m_Emote;
        // Look direction: from player to their aim target.
        const vec2 AimTarget = GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy];
        const vec2 PlayerToAim = AimTarget - PlayerPos;

        // --- Target pet position ---
        vec2 TargetPos;
        const float OffsetX = (float)g_Config.m_PushinPetOffsetX;
        const float OffsetY = (float)g_Config.m_PushinPetOffsetY;

        if(g_Config.m_PushinPetMode == 0) // flying
        {
                TargetPos = PlayerPos + vec2(OffsetX, OffsetY);
                // Bobbing: sinusoidal up-down motion.
                if(g_Config.m_PushinPetBob)
                {
                        m_BobPhase += Client()->RenderFrameTime() * 3.0f; // ~3 rad/s
                        TargetPos.y += std::sin(m_BobPhase) * (float)g_Config.m_PushinPetBobAmount;
                }
        }
        else // walking (mode 1)
        {
                // Walk on the ground: place the pet at the player's feet, offset
                // horizontally behind the player based on facing direction.
                const float Dir = (PlayerToAim.x >= 0.0f) ? 1.0f : -1.0f;
                TargetPos = PlayerPos + vec2(-Dir * std::abs(OffsetX) * 0.5f, 0.0f);
        }

        // --- Smooth position with configurable delay ---
        if(!m_Init)
        {
                m_PetPos = TargetPos;
                m_Init = true;
        }
        else
        {
                // Exponential smoothing. Delay is in centiseconds (30 = 0.3s).
                // Higher delay = slower follow. Convert to a smoothing factor:
                // factor = 1 - exp(-dt / delay). Clamp delay to avoid div-by-zero.
                const float DelaySec = std::max(0.01f, (float)g_Config.m_PushinPetDelay / 100.0f);
                const float Dt = Client()->RenderFrameTime();
                const float Factor = 1.0f - std::exp(-Dt / DelaySec);
                m_PetPos = mix(m_PetPos, TargetPos, Factor);
        }

        // --- Build the tee render info ---
        const CSkin *pSkin = GameClient()->m_Skins.Find(g_Config.m_PushinPetSkin);
        if(pSkin == nullptr)
                pSkin = GameClient()->m_Skins.Find("default");
        if(pSkin == nullptr)
                return;

        CTeeRenderInfo Info;
        Info.Apply(pSkin);
        // Size: percentage of the default player render size (which is 64).
        Info.m_Size = 64.0f * ((float)g_Config.m_PushinPetSize / 100.0f);

        // --- Animation state ---
        const CAnimState *pState;
        int Emote = EMOTE_NORMAL;
        vec2 Dir(1.0f, 0.0f);

        if(g_Config.m_PushinPetMode == 0) // flying
        {
                pState = CAnimState::GetIdle();
                // Emote: copy player's emote if enabled.
                if(g_Config.m_PushinPetEmote)
                        Emote = PlayerEmote;
                // Look direction: pet looks at the same point the player aims at.
                if(g_Config.m_PushinPetLook)
                {
                        const vec2 PetToAim = AimTarget - m_PetPos;
                        if(length(PetToAim) > 0.001f)
                                Dir = normalize(PetToAim);
                }
        }
        else // walking
        {
                // Use idle animation — full walk animation requires WalkTime
                // which is computed in the player render path and not easily
                // accessible here. The pet still follows on the ground.
                pState = CAnimState::GetIdle();
                // Emote copy.
                if(g_Config.m_PushinPetEmote)
                        Emote = PlayerEmote;
                // Look direction.
                if(g_Config.m_PushinPetLook)
                {
                        const vec2 PetToAim = AimTarget - m_PetPos;
                        if(length(PetToAim) > 0.001f)
                                Dir = normalize(PetToAim);
                }
        }

        RenderTools()->RenderTee(pState, &Info, Emote, Dir, m_PetPos);
}
