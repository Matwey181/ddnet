/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
// Pushin client — Var List feature (player war/team marking)
#include <base/log.h>
#include <base/math.h>
#include <base/system.h>

#include <engine/font_icons.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/client/skin.h>
#include <game/client/ui.h>
#include <game/client/ui_listbox.h>
#include <game/localization.h>

#include "menus.h"
#include "menus_pushin.h"
#include "skins.h"

#include <vector>
#include <string>

using namespace FontIcon;

namespace {
// Status enum used internally by the Var List UI
enum EPushinVarStatus {
        PUSHIN_VAR_NONE = 0,
        PUSHIN_VAR_WAR,
        PUSHIN_VAR_TEAM,
};

// Parse comma-separated string into a vector of names.
std::vector<std::string> ParsePushinList(const char *pList)
{
        std::vector<std::string> vNames;
        if(!pList)
                return vNames;
        const char *pStart = pList;
        while(true)
        {
                const char *pEnd = strchr(pStart, ',');
                std::string Name;
                if(pEnd == nullptr)
                        Name = std::string(pStart);
                else
                        Name = std::string(pStart, pEnd - pStart);
                size_t s = Name.find_first_not_of(" \t");
                size_t e = Name.find_last_not_of(" \t");
                if(s != std::string::npos && e != std::string::npos && e >= s)
                        vNames.push_back(Name.substr(s, e - s + 1));
                if(pEnd == nullptr)
                        break;
                pStart = pEnd + 1;
        }
        return vNames;
}

void WritePushinList(char *pDst, int DstSize, const std::vector<std::string> &vNames)
{
        if(DstSize <= 0)
                return;
        pDst[0] = '\0';
        int Off = 0;
        for(size_t i = 0; i < vNames.size(); i++)
        {
                if(i > 0)
                {
                        if(Off + 1 >= DstSize)
                                break;
                        pDst[Off++] = ',';
                        pDst[Off] = '\0';
                }
                const std::string &N = vNames[i];
                int Remaining = DstSize - Off;
                if(Remaining <= 1)
                        break;
                str_copy(pDst + Off, N.c_str(), Remaining);
                Off += str_length(pDst + Off);
        }
}

EPushinVarStatus GetPushinStatus(const char *pName)
{
        if(!pName || pName[0] == '\0')
                return PUSHIN_VAR_NONE;
        for(const auto &N : ParsePushinList(g_Config.m_PushinVarWarList))
                if(str_comp_nocase(N.c_str(), pName) == 0)
                        return PUSHIN_VAR_WAR;
        for(const auto &N : ParsePushinList(g_Config.m_PushinVarTeamList))
                if(str_comp_nocase(N.c_str(), pName) == 0)
                        return PUSHIN_VAR_TEAM;
        return PUSHIN_VAR_NONE;
}

void PushinRemoveFromList(char *pList, const char *pName)
{
        std::vector<std::string> v = ParsePushinList(pList);
        std::vector<std::string> vOut;
        for(const auto &N : v)
                if(str_comp_nocase(N.c_str(), pName) != 0)
                        vOut.push_back(N);
        WritePushinList(pList, 1024, vOut);
}

void PushinAddToList(char *pList, const char *pName)
{
        std::vector<std::string> v = ParsePushinList(pList);
        for(const auto &N : v)
                if(str_comp_nocase(N.c_str(), pName) == 0)
                        return;
        v.push_back(std::string(pName));
        WritePushinList(pList, 1024, v);
}

void SetPushinStatus(const char *pName, EPushinVarStatus Status)
{
        if(!pName || pName[0] == '\0')
                return;
        PushinRemoveFromList(g_Config.m_PushinVarWarList, pName);
        PushinRemoveFromList(g_Config.m_PushinVarTeamList, pName);
        if(Status == PUSHIN_VAR_WAR)
                PushinAddToList(g_Config.m_PushinVarWarList, pName);
        else if(Status == PUSHIN_VAR_TEAM)
                PushinAddToList(g_Config.m_PushinVarTeamList, pName);
}

ColorRGBA PushinConfigColorToRGBA(unsigned HslaPacked)
{
        return color_cast<ColorRGBA>(ColorHSLA(HslaPacked));
}

ColorRGBA PushinApplyTint(ColorRGBA Base, ColorRGBA Tint)
{
        float Pct = (float)g_Config.m_PushinVarTintSkinPercent / 100.0f;
        if(Pct < 0.0f)
                Pct = 0.0f;
        if(Pct > 1.0f)
                Pct = 1.0f;
        return ColorRGBA(
                Base.r * (1.0f - Pct) + Tint.r * Pct,
                Base.g * (1.0f - Pct) + Tint.g * Pct,
                Base.b * (1.0f - Pct) + Tint.b * Pct,
                Base.a);
}

// Look up skin by name and build a CTeeRenderInfo ready for rendering.
// Returns false if the default skin could not be found either.
bool BuildTeeRenderInfo(CGameClient *pGameClient, const char *pSkinName, int UseCustomColor, int ColorBody, int ColorFeet, float Size, CTeeRenderInfo &OutInfo)
{
        const CSkin *pDefaultSkin = pGameClient->m_Skins.Find("default");
        const CSkins::CSkinContainer *pContainer = pGameClient->m_Skins.FindContainerOrNullptr(pSkinName == nullptr || pSkinName[0] == '\0' ? "default" : pSkinName);
        const CSkin *pSkin = (pContainer != nullptr && pContainer->Skin() != nullptr) ? pContainer->Skin().get() : pDefaultSkin;
        if(pSkin == nullptr)
                pSkin = pDefaultSkin;
        if(pSkin == nullptr)
                return false;
        OutInfo.Apply(pSkin);
        OutInfo.ApplyColors(UseCustomColor != 0, ColorBody, ColorFeet);
        OutInfo.m_Size = Size;
        return true;
}

const char *PushinStatusLabel(EPushinVarStatus Status)
{
        switch(Status)
        {
        case PUSHIN_VAR_WAR:
                return g_Config.m_PushinVarPrefix ? g_Config.m_PushinVarWarPrefix : "war";
        case PUSHIN_VAR_TEAM:
                return g_Config.m_PushinVarPrefix ? g_Config.m_PushinVarTeamPrefix : "team";
        default:
                return "none";
        }
}
} // anonymous namespace

// =============================================================
//  Public API used by nameplates.cpp and players.cpp
// =============================================================
bool PushinGetPlayerDisplayInfo(const char *pName, ColorRGBA &OutColor, char *pPrefixBuf, int PrefixBufSize)
{
        EPushinVarStatus Status = GetPushinStatus(pName);
        if(Status == PUSHIN_VAR_NONE)
        {
                if(pPrefixBuf && PrefixBufSize > 0)
                        pPrefixBuf[0] = '\0';
                return false;
        }
        ColorRGBA Tint = (Status == PUSHIN_VAR_WAR) ? PushinConfigColorToRGBA(g_Config.m_PushinVarWarColor) : PushinConfigColorToRGBA(g_Config.m_PushinVarTeamColor);
        if(g_Config.m_PushinVarTintNick)
                OutColor = Tint;
        else
                OutColor = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
        if(pPrefixBuf && PrefixBufSize > 0)
        {
                if(g_Config.m_PushinVarPrefix)
                {
                        const char *pPfx = (Status == PUSHIN_VAR_WAR) ? g_Config.m_PushinVarWarPrefix : g_Config.m_PushinVarTeamPrefix;
                        str_copy(pPrefixBuf, pPfx, PrefixBufSize);
                }
                else
                {
                        pPrefixBuf[0] = '\0';
                }
        }
        return true;
}

bool PushinApplyTintToPlayerSkin(const char *pName, ColorRGBA &BodyColor, ColorRGBA &FeetColor, ColorRGBA &BloodColor, int &OutEmote)
{
        EPushinVarStatus Status = GetPushinStatus(pName);
        if(Status == PUSHIN_VAR_NONE)
                return false;
        if(g_Config.m_PushinVarTintSkin)
        {
                ColorRGBA Tint = (Status == PUSHIN_VAR_WAR) ? PushinConfigColorToRGBA(g_Config.m_PushinVarWarColor) : PushinConfigColorToRGBA(g_Config.m_PushinVarTeamColor);
                BodyColor = PushinApplyTint(BodyColor, Tint);
                FeetColor = PushinApplyTint(FeetColor, Tint);
                BloodColor = PushinApplyTint(BloodColor, Tint);
        }
        if(Status == PUSHIN_VAR_WAR)
                OutEmote = EMOTE_ANGRY;
        else
                OutEmote = EMOTE_HAPPY;
        return true;
}

// =============================================================
//  Settings UI
// =============================================================
void CMenus::RenderSettingsPushin(CUIRect MainView)
{
        // Section header
        CUIRect Label;
        MainView.HSplitTop(20.0f, &Label, &MainView);
        Ui()->DoLabel(&Label, Localize("Pushin client"), 20.0f, TEXTALIGN_ML);
        MainView.HSplitTop(5.0f, nullptr, &MainView);

        // A single dark row "Вар лист" with a dropdown arrow.
        CUIRect Row;
        MainView.HSplitTop(28.0f, &Row, &MainView);
        Row.VSplitRight(20.0f, &Row, nullptr);
        {
                ColorRGBA Dark(0.0f, 0.0f, 0.0f, 0.35f);
                Row.Draw(Dark, IGraphics::CORNER_ALL, 4.0f);
        }
        CUIRect Arrow, Text;
        Row.VSplitRight(Row.h, &Text, &Arrow);
        Text.VSplitLeft(10.0f, nullptr, &Text);
        Text.Margin(4.0f, &Text);
        Ui()->DoLabel(&Text, Localize("Var list"), 14.0f, TEXTALIGN_ML);

        {
                const char *pArrow = m_PushinVarListExpanded ? CHEVRON_DOWN : CHEVRON_RIGHT;
                TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
                TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT);
                Arrow.Margin(4.0f, &Arrow);
                Ui()->DoLabel(&Arrow, pArrow, 14.0f, TEXTALIGN_MC);
                TextRender()->SetRenderFlags(0);
                TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
        }
        static CButtonContainer s_ToggleBtn;
        if(Ui()->DoButtonLogic(&s_ToggleBtn, 0, &Row, BUTTONFLAG_LEFT))
        {
                m_PushinVarListExpanded = !m_PushinVarListExpanded;
        }

        MainView.HSplitTop(10.0f, nullptr, &MainView);

        if(m_PushinVarListExpanded)
        {
                RenderPushinVarList(MainView);
        }
}

void CMenus::RenderPushinVarList(CUIRect MainView)
{
        // Helper: render a single tee preview into the given rect.
        // Applies war/team tint and emote override based on Status, using the
        // configured tint strength and color.
        auto &&RenderTeePreview = [&](const CUIRect &Rect, const char *pSkinName, int UseCustomColor, int ColorBody, int ColorFeet,
                                       EPushinVarStatus Status, float Size) -> void {
                CTeeRenderInfo Info;
                if(!BuildTeeRenderInfo(GameClient(), pSkinName, UseCustomColor, ColorBody, ColorFeet, Size, Info))
                        return;
                int Emote = EMOTE_NORMAL;
                if(Status != PUSHIN_VAR_NONE)
                {
                        ColorRGBA Tint = (Status == PUSHIN_VAR_WAR) ? PushinConfigColorToRGBA(g_Config.m_PushinVarWarColor) : PushinConfigColorToRGBA(g_Config.m_PushinVarTeamColor);
                        if(g_Config.m_PushinVarTintSkin)
                        {
                                Info.m_ColorBody = PushinApplyTint(Info.m_ColorBody, Tint);
                                Info.m_ColorFeet = PushinApplyTint(Info.m_ColorFeet, Tint);
                                Info.m_BloodColor = PushinApplyTint(Info.m_BloodColor, Tint);
                        }
                        if(Status == PUSHIN_VAR_WAR)
                                Emote = EMOTE_ANGRY;
                        else if(Status == PUSHIN_VAR_TEAM)
                                Emote = EMOTE_HAPPY;
                }
                vec2 OffsetToMid;
                CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &Info, OffsetToMid);
                const vec2 Pos = vec2(Rect.x + Rect.w / 2.0f, Rect.y + Rect.h / 2.0f + OffsetToMid.y);
                // Tee looks at the cursor across the whole screen (not just inside the
                // preview rect), matching the in-game behavior where tees track the mouse
                // position globally.
                vec2 Dir = vec2(Ui()->MouseX() - Pos.x, Ui()->MouseY() - Pos.y);
                if(length(Dir) > 0.001f)
                        Dir = normalize(Dir);
                else
                        Dir = vec2(1.0f, 0.0f);
                RenderTools()->RenderTee(CAnimState::GetIdle(), &Info, Emote, Dir, Pos);
        };

        // Layout: top half = customization + 3 list columns, bottom = preview
        CUIRect Upper, Preview;
        MainView.HSplitBottom(180.0f, &Upper, &Preview);
        Upper.HSplitBottom(10.0f, &Upper, nullptr);

        CUIRect LeftPanel, RightPanel;
        Upper.VSplitLeft(380.0f, &LeftPanel, &RightPanel);
        LeftPanel.VSplitRight(10.0f, &LeftPanel, nullptr);

        // ---------------- Customization panel ----------------
        CUIRect Label, Button;
        LeftPanel.HSplitTop(20.0f, &Label, &LeftPanel);
        Ui()->DoLabel(&Label, Localize("Customization"), 16.0f, TEXTALIGN_ML);
        LeftPanel.HSplitTop(5.0f, nullptr, &LeftPanel);

        const float LineSize = 20.0f;
        const float ColorPickerLineSize = 25.0f;
        const float ColorPickerLabelSize = 13.0f;
        const float ColorPickerLineSpacing = 5.0f;

        static CButtonContainer s_WarColorReset, s_TeamColorReset;
        ColorRGBA RedDefault(1.0f, 0.0f, 0.0f, 1.0f);
        ColorRGBA GreenDefault(0.0f, 1.0f, 0.0f, 1.0f);
        DoLine_ColorPicker(&s_WarColorReset, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftPanel, Localize("War color"), &g_Config.m_PushinVarWarColor, RedDefault, false);
        DoLine_ColorPicker(&s_TeamColorReset, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftPanel, Localize("Team color"), &g_Config.m_PushinVarTeamColor, GreenDefault, false);

        LeftPanel.HSplitTop(LineSize, &Button, &LeftPanel);
        if(DoButton_CheckBox(&g_Config.m_PushinVarTintSkin, Localize("Tint player skin"), g_Config.m_PushinVarTintSkin, &Button))
                g_Config.m_PushinVarTintSkin ^= 1;

        LeftPanel.HSplitTop(LineSize, &Button, &LeftPanel);
        if(g_Config.m_PushinVarTintSkin)
        {
                LeftPanel.HSplitTop(LineSize * 1.5f, &Button, &LeftPanel);
                Ui()->DoScrollbarOption(&g_Config.m_PushinVarTintSkinPercent, &g_Config.m_PushinVarTintSkinPercent, &Button, Localize("Tint strength"), 0, 100, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_MULTILINE, "%");
        }
        else
        {
                LeftPanel.HSplitTop(LineSize, nullptr, &LeftPanel);
        }

        LeftPanel.HSplitTop(LineSize, &Button, &LeftPanel);
        if(DoButton_CheckBox(&g_Config.m_PushinVarTintNick, Localize("Tint player nickname"), g_Config.m_PushinVarTintNick, &Button))
                g_Config.m_PushinVarTintNick ^= 1;

        LeftPanel.HSplitTop(LineSize, &Button, &LeftPanel);
        if(DoButton_CheckBox(&g_Config.m_PushinVarPrefix, Localize("Show prefix on nickname"), g_Config.m_PushinVarPrefix, &Button))
                g_Config.m_PushinVarPrefix ^= 1;

        static CLineInputBuffered<16> s_WarPrefixInput;
        static CLineInputBuffered<16> s_TeamPrefixInput;
        s_WarPrefixInput.SetBuffer(g_Config.m_PushinVarWarPrefix, sizeof(g_Config.m_PushinVarWarPrefix));
        s_TeamPrefixInput.SetBuffer(g_Config.m_PushinVarTeamPrefix, sizeof(g_Config.m_PushinVarTeamPrefix));

        LeftPanel.HSplitTop(5.0f, nullptr, &LeftPanel);
        LeftPanel.HSplitTop(20.0f, &Button, &LeftPanel);
        {
                CUIRect Lbl;
                Button.VSplitLeft(120.0f, &Lbl, &Button);
                Ui()->DoLabel(&Lbl, Localize("War prefix"), 12.0f, TEXTALIGN_ML);
                Ui()->DoEditBox(&s_WarPrefixInput, &Button, 12.0f);
        }
        LeftPanel.HSplitTop(5.0f, nullptr, &LeftPanel);
        LeftPanel.HSplitTop(20.0f, &Button, &LeftPanel);
        {
                CUIRect Lbl;
                Button.VSplitLeft(120.0f, &Lbl, &Button);
                Ui()->DoLabel(&Lbl, Localize("Team prefix"), 12.0f, TEXTALIGN_ML);
                Ui()->DoEditBox(&s_TeamPrefixInput, &Button, 12.0f);
        }

        // ---------------- Right panel: 3 lists side by side ----------------
        CUIRect ColPlayers, ColWar, ColTeam;
        RightPanel.VSplitMid(&ColPlayers, &ColWar, 5.0f);
        ColWar.VSplitMid(&ColWar, &ColTeam, 5.0f);

        const char *pWarLabel = g_Config.m_PushinVarPrefix ? g_Config.m_PushinVarWarPrefix : "war";
        const char *pTeamLabel = g_Config.m_PushinVarPrefix ? g_Config.m_PushinVarTeamPrefix : "team";

        // Build the player entries list (visible players when in-game, otherwise just the
        // combined names from war/team lists so the user can still see and manage them).
        std::vector<std::pair<std::string, int>> vAllPlayers;
        bool InGame = (Client()->State() == IClient::STATE_ONLINE);
        if(InGame)
        {
                for(int i = 0; i < MAX_CLIENTS; i++)
                {
                        const CGameClient::CClientData &C = GameClient()->m_aClients[i];
                        if(!C.m_Active)
                                continue;
                        vAllPlayers.push_back(std::make_pair(std::string(C.m_aName), i));
                }
        }
        else
        {
                for(const auto &N : ParsePushinList(g_Config.m_PushinVarWarList))
                        vAllPlayers.push_back(std::make_pair(N, -1));
                for(const auto &N : ParsePushinList(g_Config.m_PushinVarTeamList))
                        vAllPlayers.push_back(std::make_pair(N, -1));
        }

        std::vector<std::pair<std::string, int>> vWarPlayers;
        std::vector<std::pair<std::string, int>> vTeamPlayers;
        for(const auto &N : ParsePushinList(g_Config.m_PushinVarWarList))
                vWarPlayers.push_back(std::make_pair(N, -1));
        for(const auto &N : ParsePushinList(g_Config.m_PushinVarTeamList))
                vTeamPlayers.push_back(std::make_pair(N, -1));

        // Static unique IDs for listbox items
        static int s_aColItemIds[3][256];

        // Currently selected player name (across all columns). Selected by tapping
        // a player row; tapping a column title moves the selected player there.
        // Stored as a member (m_PushinSelectedName) so it survives across frames.
        auto &&DrawColumn = [&](CUIRect &Col, const char *pTitle, const std::vector<std::pair<std::string, int>> &vEntries, int ColKind) -> void {
                CUIRect Title, List;
                Col.HSplitTop(20.0f, &Title, &Col);
                ColorRGBA TitleColor = ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f);
                if(ColKind == 1)
                {
                        ColorRGBA WarC = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_PushinVarWarColor));
                        TitleColor = ColorRGBA(WarC.r * 0.5f, WarC.g * 0.5f, WarC.b * 0.5f, 0.75f);
                }
                else if(ColKind == 2)
                {
                        ColorRGBA TeamC = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_PushinVarTeamColor));
                        TitleColor = ColorRGBA(TeamC.r * 0.5f, TeamC.g * 0.5f, TeamC.b * 0.5f, 0.75f);
                }
                Title.Draw(TitleColor, IGraphics::CORNER_T, 4.0f);
                Title.Margin(4.0f, &Title);
                Ui()->DoLabel(&Title, pTitle, 13.0f, TEXTALIGN_MC);

                Col.HSplitTop(5.0f, nullptr, &List);
                static CListBox s_ListBoxes[3];
                CListBox &LB = s_ListBoxes[ColKind];
                LB.DoStart(36.0f, (int)vEntries.size(), 1, 6, -1, &List, true, IGraphics::CORNER_B);
                for(size_t i = 0; i < vEntries.size() && i < 256; i++)
                {
                        const std::string &Name = vEntries[i].first;
                        int ClientId = vEntries[i].second;
                        const CListboxItem Item = LB.DoNextItem(&s_aColItemIds[ColKind][i], false);
                        if(!Item.m_Visible)
                                continue;
                        CUIRect RowR = Item.m_Rect;
                        RowR.Margin(2.0f, &RowR);

                        // Detect tap on this row via raw mouse hover + click (avoid calling
                        // DoButtonLogic twice, which conflicts with the listbox's own click
                        // handling and causes the selection to flicker / disappear).
                        if(Ui()->MouseHovered(&RowR) && Ui()->MouseButtonClicked(0))
                        {
                                str_copy(m_PushinSelectedName, Name.c_str(), sizeof(m_PushinSelectedName));
                        }

                        // Highlight the selected row.
                        if(m_PushinSelectedName[0] != '\0' && str_comp_nocase(m_PushinSelectedName, Name.c_str()) == 0)
                        {
                                ColorRGBA SelColor(1.0f, 1.0f, 1.0f, 0.25f);
                                RowR.Draw(SelColor, IGraphics::CORNER_ALL, 4.0f);
                        }

                        CUIRect TeeRect, NameRect;
                        RowR.VSplitLeft(RowR.h, &TeeRect, &NameRect);
                        NameRect.VSplitLeft(6.0f, nullptr, &NameRect);

                        const CGameClient::CClientData *pCli = (ClientId >= 0 && ClientId < MAX_CLIENTS) ? &GameClient()->m_aClients[ClientId] : nullptr;
                        const char *pSkinName = pCli ? pCli->m_aSkinName : g_Config.m_ClPlayerSkin;
                        int UseCustom = pCli ? pCli->m_UseCustomColor : g_Config.m_ClPlayerUseCustomColor;
                        int ColorBody = pCli ? pCli->m_ColorBody : g_Config.m_ClPlayerColorBody;
                        int ColorFeet = pCli ? pCli->m_ColorFeet : g_Config.m_ClPlayerColorFeet;

                        EPushinVarStatus S = (ColKind == 1) ? PUSHIN_VAR_WAR : (ColKind == 2) ? PUSHIN_VAR_TEAM : GetPushinStatus(Name.c_str());
                        RenderTeePreview(TeeRect, pSkinName, UseCustom, ColorBody, ColorFeet, S, 24.0f);

                        ColorRGBA NameColor(1.0f, 1.0f, 1.0f, 1.0f);
                        if(S != PUSHIN_VAR_NONE && g_Config.m_PushinVarTintNick)
                                NameColor = (S == PUSHIN_VAR_WAR) ? PushinConfigColorToRGBA(g_Config.m_PushinVarWarColor) : PushinConfigColorToRGBA(g_Config.m_PushinVarTeamColor);

                        char aDisplay[MAX_NAME_LENGTH + 24];
                        aDisplay[0] = '\0';
                        if(g_Config.m_PushinVarPrefix && S != PUSHIN_VAR_NONE)
                        {
                                const char *pPfx = (S == PUSHIN_VAR_WAR) ? g_Config.m_PushinVarWarPrefix : g_Config.m_PushinVarTeamPrefix;
                                str_format(aDisplay, sizeof(aDisplay), "[%s] %s", pPfx, Name.c_str());
                        }
                        else
                        {
                                str_copy(aDisplay, Name.c_str(), sizeof(aDisplay));
                        }

                        TextRender()->TextColor(NameColor);
                        Ui()->DoLabel(&NameRect, aDisplay, 11.0f, TEXTALIGN_ML);
                        TextRender()->TextColor(TextRender()->DefaultTextColor());

                        if(ColKind == 0)
                        {
                                CUIRect StatusRect;
                                NameRect.VSplitRight(60.0f, &NameRect, &StatusRect);
                                StatusRect.Margin(4.0f, &StatusRect);
                                const char *pStatusText = PushinStatusLabel(S);
                                ColorRGBA StatusColor(0.7f, 0.7f, 0.7f, 1.0f);
                                if(S == PUSHIN_VAR_WAR)
                                        StatusColor = PushinConfigColorToRGBA(g_Config.m_PushinVarWarColor);
                                else if(S == PUSHIN_VAR_TEAM)
                                        StatusColor = PushinConfigColorToRGBA(g_Config.m_PushinVarTeamColor);
                                TextRender()->TextColor(StatusColor);
                                Ui()->DoLabel(&StatusRect, pStatusText, 10.0f, TEXTALIGN_MR);
                                TextRender()->TextColor(TextRender()->DefaultTextColor());
                        }
                }
                LB.DoEnd();
        };

        DrawColumn(ColPlayers, Localize("Players"), vAllPlayers, 0);
        DrawColumn(ColWar, pWarLabel, vWarPlayers, 1);
        DrawColumn(ColTeam, pTeamLabel, vTeamPlayers, 2);

        // Drop-zone click detection on column titles. Tapping a title moves the
        // currently selected player into that column (war/team/none). Uses raw
        // mouse hover + click for the same reason as row selection above.
        auto &&TitleClick = [&](CUIRect &Col, int TargetKind) -> bool {
                CUIRect Title;
                Col.HSplitTop(20.0f, &Title, nullptr);
                return Ui()->MouseHovered(&Title) && Ui()->MouseButtonClicked(0);
        };
        if(m_PushinSelectedName[0] != '\0')
        {
                if(TitleClick(ColWar, 1))
                {
                        SetPushinStatus(m_PushinSelectedName, PUSHIN_VAR_WAR);
                        m_PushinSelectedName[0] = '\0';
                }
                else if(TitleClick(ColTeam, 2))
                {
                        SetPushinStatus(m_PushinSelectedName, PUSHIN_VAR_TEAM);
                        m_PushinSelectedName[0] = '\0';
                }
                else if(TitleClick(ColPlayers, 0))
                {
                        SetPushinStatus(m_PushinSelectedName, PUSHIN_VAR_NONE);
                        m_PushinSelectedName[0] = '\0';
                }
        }

        // Show the currently selected player near the top of the customization panel,
        // so the user knows what's currently being dragged.
        if(m_PushinSelectedName[0] != '\0')
        {
                CUIRect SelLabel;
                LeftPanel.HSplitTop(20.0f, &SelLabel, &LeftPanel);
                char aSelText[160];
                str_format(aSelText, sizeof(aSelText), Localize("Selected: %s — tap a column title to move"), m_PushinSelectedName);
                TextRender()->TextColor(PushinConfigColorToRGBA(g_Config.m_PushinVarWarColor));
                Ui()->DoLabel(&SelLabel, aSelText, 11.0f, TEXTALIGN_ML);
                TextRender()->TextColor(TextRender()->DefaultTextColor());
        }

        // ---------------- Preview block ----------------
        Preview.HSplitTop(20.0f, &Label, &Preview);
        Ui()->DoLabel(&Label, Localize("Preview"), 16.0f, TEXTALIGN_ML);
        Preview.HSplitTop(5.0f, nullptr, &Preview);

        Preview.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 6.0f);
        Preview.Margin(10.0f, &Preview);

        CUIRect TeeArea, ButtonsArea, NickArea;
        Preview.VSplitLeft(140.0f, &TeeArea, &ButtonsArea);
        TeeArea.HSplitTop(20.0f, &NickArea, &TeeArea);

        const char *pLocalName = Client()->PlayerName();
        static int s_PreviewStatus = 0; // 0=none, 1=war, 2=team
        EPushinVarStatus PrevStatus = (s_PreviewStatus == 1) ? PUSHIN_VAR_WAR : (s_PreviewStatus == 2) ? PUSHIN_VAR_TEAM : PUSHIN_VAR_NONE;
        char aLocalDisplay[128];
        aLocalDisplay[0] = '\0';
        if(g_Config.m_PushinVarPrefix && PrevStatus != PUSHIN_VAR_NONE)
        {
                const char *pPfx = (PrevStatus == PUSHIN_VAR_WAR) ? g_Config.m_PushinVarWarPrefix : g_Config.m_PushinVarTeamPrefix;
                str_format(aLocalDisplay, sizeof(aLocalDisplay), "[%s] %s", pPfx, pLocalName);
        }
        else
        {
                str_copy(aLocalDisplay, pLocalName, sizeof(aLocalDisplay));
        }
        ColorRGBA LocalNameColor(1.0f, 1.0f, 1.0f, 1.0f);
        if(g_Config.m_PushinVarTintNick && PrevStatus != PUSHIN_VAR_NONE)
                LocalNameColor = (PrevStatus == PUSHIN_VAR_WAR) ? PushinConfigColorToRGBA(g_Config.m_PushinVarWarColor) : PushinConfigColorToRGBA(g_Config.m_PushinVarTeamColor);
        TextRender()->TextColor(LocalNameColor);
        Ui()->DoLabel(&NickArea, aLocalDisplay, 13.0f, TEXTALIGN_MC);
        TextRender()->TextColor(TextRender()->DefaultTextColor());

        RenderTeePreview(TeeArea, g_Config.m_ClPlayerSkin, g_Config.m_ClPlayerUseCustomColor, g_Config.m_ClPlayerColorBody, g_Config.m_ClPlayerColorFeet, PrevStatus, 60.0f);

        ButtonsArea.VSplitMid(&Button, &ButtonsArea, 10.0f);
        Button.Margin(10.0f, &Button);
        ColorRGBA WarBtnColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_PushinVarWarColor));
        {
                static CButtonContainer s_WarPreviewBtn;
                ColorRGBA BtnFill = WarBtnColor;
                BtnFill.a = 0.35f;
                Button.Draw(BtnFill, IGraphics::CORNER_ALL, 6.0f);
                ColorRGBA Outline = WarBtnColor;
                Outline.a = 0.9f;
                TextRender()->TextColor(Outline);
                char aLabel[32];
                str_format(aLabel, sizeof(aLabel), "%s", g_Config.m_PushinVarPrefix ? g_Config.m_PushinVarWarPrefix : "war");
                Ui()->DoLabel(&Button, aLabel, 18.0f, TEXTALIGN_MC);
                TextRender()->TextColor(TextRender()->DefaultTextColor());
                if(Ui()->DoButtonLogic(&s_WarPreviewBtn, 0, &Button, BUTTONFLAG_LEFT))
                {
                        s_PreviewStatus = 1;
                }
        }
        Button = ButtonsArea;
        Button.Margin(10.0f, &Button);
        ColorRGBA TeamBtnColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_PushinVarTeamColor));
        {
                static CButtonContainer s_TeamPreviewBtn;
                ColorRGBA BtnFill = TeamBtnColor;
                BtnFill.a = 0.35f;
                Button.Draw(BtnFill, IGraphics::CORNER_ALL, 6.0f);
                ColorRGBA Outline = TeamBtnColor;
                Outline.a = 0.9f;
                TextRender()->TextColor(Outline);
                char aLabel[32];
                str_format(aLabel, sizeof(aLabel), "%s", g_Config.m_PushinVarPrefix ? g_Config.m_PushinVarTeamPrefix : "team");
                Ui()->DoLabel(&Button, aLabel, 18.0f, TEXTALIGN_MC);
                TextRender()->TextColor(TextRender()->DefaultTextColor());
                if(Ui()->DoButtonLogic(&s_TeamPreviewBtn, 0, &Button, BUTTONFLAG_LEFT))
                {
                        s_PreviewStatus = 2;
                }
        }
}
