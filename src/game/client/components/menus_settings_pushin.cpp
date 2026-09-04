/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

// ============================================================================
// ⚠️  AGENT MEMORY NOTE — READ BEFORE EVERY EDIT  ⚠️
// ============================================================================
// This is the iOS-PORTED DDNet (Pioooooo/ddnet@ddnet-ios, mirrored to
// Matwey181/ddnet). The iOS build infrastructure is ALREADY WORKING.
// DO NOT touch CMakeLists.txt, detect.h, system.cpp, notifications.cpp,
// updater.cpp, macos/*.mm, gen_libs.sh, _build_common.sh, Info.plist.in,
// cmake/Find*.cmake, .github/workflows/*.yml, ddnet-libs/.
//
// This file implements the Pushin client "Var list" feature as a separate
// settings tab. See MEMORY.md at repo root for the full spec.
//
// All render helpers are declared as members of CMenus (see menus.h) so
// they can access protected CComponent interfaces (GameClient, TextRender,
// Graphics) and private CMenus UI helpers (DoLine_ColorPicker, DoButton_*).
// ============================================================================

#include "menus.h"
#include "skins.h"

#include <base/math.h>
#include <base/system.h>
#include <engine/client/client.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>
#include <game/client/animstate.h>
#include <game/client/components/menu_background.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/client/skin.h>
#include <game/client/ui_listbox.h>

// ----------------------------------------------------------------------------
// Pushin client — Var list
//
// Layout (when expanded):
//   ┌─────────────────────────────────────────────────────────────────┐
//   │  Settings panel: color pickers, toggles, sliders, prefix edits  │
//   ├──────────────┬──────────────┬──────────────┬───────────────────┤
//   │   Players    │     Team     │     Var      │ ← mini-menus on   │
//   │ (scrollable) │ (scrollable) │ (scrollable) │   the right:      │
//   │              │              │              │   [team]          │
//   │              │              │              │   [var]           │
//   ├──────────────┴──────────────┴──────────────┴───────────────────┤
//   │  Preview: tee looks at cursor, nick on top, 2 buttons [team]   │
//   │  [var] to flip the preview status                              │
//   └─────────────────────────────────────────────────────────────────┘
//
// Drag-and-drop: press and hold on a player row, drag it over the Team or
// Var column (or the mini-menu buttons), release. The player is assigned
// to that status. Drag back to Players to clear.
//
// Touch support: this port exposes Input()->TouchFingerStates() but the
// menu system already proxies touch through Ui()->MouseX/Y/MouseButton,
// so the same drag logic works for both mouse and touch.
// ----------------------------------------------------------------------------

// Per-player pushin status. Indexed by client id.
// Defined at file scope (not anonymous namespace) so the static accessor
// methods declared in menus.h can read it.
int s_aPushinStatus[MAX_CLIENTS] = {};

// Status codes — also used by the static accessors.
constexpr int PUSHIN_STATUS_NONE = 0;
constexpr int PUSHIN_STATUS_VAR = 1;
constexpr int PUSHIN_STATUS_TEAM = 2;

namespace {

// Drag state: which client id is currently being dragged (-1 = none).
int s_PushinDragClientId = -1;
// True while the drag is active (mouse/touch held).
bool s_PushinDragging = false;
// Preview status: cycles none → team → var → none when clicking preview buttons.
int s_PushinPreviewStatus = PUSHIN_STATUS_NONE;

// Double-click detection: last click time + client id, per row.
std::chrono::steady_clock::time_point s_PushinLastClickTime{};
int s_PushinLastClickClientId = -1;
constexpr auto PUSHIN_DOUBLE_CLICK_TIME = std::chrono::milliseconds(400);

// Mouse button state tracking (to detect "just pressed" vs "just released").
bool s_PushinMouseWasDown = false;

// Helper: get a color RGBA from a packed RGBA config variable.
// The config variable is stored as a packed RGBA unsigned int (0xRRGGBBAA)
// to match what DoLine_ColorPicker produces when Alpha=false — but since
// DoLine_ColorPicker actually uses HSLA, we convert HSLA→RGBA here.
ColorRGBA PushinColorToRGBA(unsigned int PackedHsla)
{
        // DoLine_ColorPicker stores HSLA packed as 0xHHSSLLAA.
        // ColorHSLA(PackedHsla, false) interprets the int as H/S/L/A bytes.
        const ColorHSLA Hsla(PackedHsla, false);
        return color_cast<ColorRGBA>(Hsla.UnclampLighting(ColorHSLA::DARKEST_LGT));
}

// Helper: format a player's display name with the configured prefix.
void FormatPushinName(char *pBuf, int BufSize, int Status, const char *pOriginalName)
{
        const char *pPrefix = nullptr;
        if(Status == PUSHIN_STATUS_VAR && g_Config.m_PushinVarUsePrefix)
                pPrefix = g_Config.m_PushinVarPrefix;
        else if(Status == PUSHIN_STATUS_TEAM && g_Config.m_PushinTeamUsePrefix)
                pPrefix = g_Config.m_PushinTeamPrefix;

        if(pPrefix && pPrefix[0] != '\0')
                str_format(pBuf, BufSize, "[%s] %s", pPrefix, pOriginalName);
        else
                str_copy(pBuf, pOriginalName, BufSize);
}

// Helper: get the configured nick color for a status.
ColorRGBA PushinNickColor(int Status)
{
        if(Status == PUSHIN_STATUS_VAR && g_Config.m_PushinVarColorNick)
                return PushinColorToRGBA(g_Config.m_PushinVarColor);
        if(Status == PUSHIN_STATUS_TEAM && g_Config.m_PushinTeamColorNick)
                return PushinColorToRGBA(g_Config.m_PushinTeamColor);
        return ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
}

// Helper: apply skin tint to a CTeeRenderInfo in place.
void ApplyPushinTintToTee(CTeeRenderInfo &Info, int Status)
{
        bool TintSkin = false;
        int TintPercent = 0;
        ColorRGBA TintColor(1.0f, 1.0f, 1.0f, 1.0f);

        if(Status == PUSHIN_STATUS_VAR && g_Config.m_PushinVarTintSkin)
        {
                TintSkin = true;
                TintPercent = g_Config.m_PushinVarTintPercent;
                TintColor = PushinColorToRGBA(g_Config.m_PushinVarColor);
        }
        else if(Status == PUSHIN_STATUS_TEAM && g_Config.m_PushinTeamTintSkin)
        {
                TintSkin = true;
                TintPercent = g_Config.m_PushinTeamTintPercent;
                TintColor = PushinColorToRGBA(g_Config.m_PushinTeamColor);
        }

        if(!TintSkin)
                return;

        // Mix the tint color into the skin color. The slider 0..100% maps
        // linearly to the mix factor — 100% means the skin becomes fully
        // the tint color, 50% is a 50/50 blend. No artificial cap.
        const float Mix = std::clamp(TintPercent / 100.0f, 0.0f, 1.0f);
        auto MixChannel = [&](float c, float t) { return c * (1.0f - Mix) + t * Mix; };
        Info.m_ColorBody = ColorRGBA(MixChannel(Info.m_ColorBody.r, TintColor.r),
                MixChannel(Info.m_ColorBody.g, TintColor.g),
                MixChannel(Info.m_ColorBody.b, TintColor.b),
                Info.m_ColorBody.a);
        Info.m_ColorFeet = ColorRGBA(MixChannel(Info.m_ColorFeet.r, TintColor.r),
                MixChannel(Info.m_ColorFeet.g, TintColor.g),
                MixChannel(Info.m_ColorFeet.b, TintColor.b),
                Info.m_ColorFeet.a);
}

// Helper: get the emote override for a status (var = angry, team = happy).
int PushinEmote(int Status)
{
        if(Status == PUSHIN_STATUS_VAR)
                return EMOTE_ANGRY;
        if(Status == PUSHIN_STATUS_TEAM)
                return EMOTE_HAPPY;
        return EMOTE_NORMAL;
}
} // namespace

// ============================================================================
// CMenus member implementations
// ============================================================================

bool CMenus::RenderPushinVarListRow(CUIRect Row)
{
        Row.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_ALL, 4.0f);
        Row.Margin(2.0f, &Row);

        CUIRect Label, Arrow;
        Row.VSplitRight(20.0f, &Label, &Arrow);
        Arrow.VSplitLeft(8.0f, nullptr, &Arrow);

        Ui()->DoLabel(&Label, "вар лист", 14.0f, TEXTALIGN_ML);
        Ui()->DoLabel(&Arrow, g_Config.m_PushinVarListExpanded ? "▼" : "▶", 14.0f, TEXTALIGN_MC);

        static CButtonContainer s_ToggleBtn;
        if(DoButton_Menu(&s_ToggleBtn, "", 0, &Row, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 4.0f, 0.0f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f)))
        {
                g_Config.m_PushinVarListExpanded ^= 1;
        }

        return g_Config.m_PushinVarListExpanded != 0;
}

void CMenus::RenderPushinSettingsPanel(CUIRect View)
{
        CUIRect VarCol, TeamCol;
        View.VSplitMid(&VarCol, &TeamCol, 8.0f);

        const float LineSize = 20.0f;
        const float ColorPickerLineSize = 25.0f;
        const float ColorPickerLabelSize = 13.0f;
        const float ColorPickerLineSpacing = 4.0f;

        // ---- Var column ----
        {
                CUIRect Label, Button;
                VarCol.HSplitTop(22.0f, &Label, &VarCol);
                const ColorRGBA VarRgba = PushinColorToRGBA(g_Config.m_PushinVarColor);
                TextRender()->TextColor(VarRgba.r, VarRgba.g, VarRgba.b, 1.0f);
                Ui()->DoLabel(&Label, "настройки варов", 16.0f, TEXTALIGN_ML);
                TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
                VarCol.HSplitTop(4.0f, nullptr, &VarCol);

                static CButtonContainer s_VarColorReset;
                // Pass &g_Config.m_PushinVarColor directly (cast to unsigned int*)
                // so the color picker popup writes back to the config variable
                // immediately — not to a local copy that gets discarded.
                unsigned int *pVarColorPtr = (unsigned int *)&g_Config.m_PushinVarColor;
                DoLine_ColorPicker(&s_VarColorReset, ColorPickerLineSize, ColorPickerLabelSize,
                        ColorPickerLineSpacing, &VarCol, "цвет варов", pVarColorPtr,
                        ColorRGBA(0.9f, 0.2f, 0.2f, 1.0f), false, &g_Config.m_PushinVarTintSkin);

                VarCol.HSplitTop(LineSize, &Button, &VarCol);
                Ui()->DoScrollbarOption(&g_Config.m_PushinVarTintPercent, &g_Config.m_PushinVarTintPercent, &Button,
                        "сила цвета варов", 0, 100, &CUi::ms_LinearScrollbarScale, 0u, "%");

                VarCol.HSplitTop(LineSize, &Button, &VarCol);
                if(DoButton_CheckBox(&g_Config.m_PushinVarColorNick, "красить ники варов", g_Config.m_PushinVarColorNick, &Button))
                        g_Config.m_PushinVarColorNick ^= 1;

                VarCol.HSplitTop(LineSize, &Button, &VarCol);
                if(DoButton_CheckBox(&g_Config.m_PushinVarUsePrefix, "префикс для варов", g_Config.m_PushinVarUsePrefix, &Button))
                        g_Config.m_PushinVarUsePrefix ^= 1;

                VarCol.HSplitTop(LineSize, &Button, &VarCol);
                static CLineInput s_VarPrefixInput(g_Config.m_PushinVarPrefix, sizeof(g_Config.m_PushinVarPrefix));
                Ui()->DoClearableEditBox(&s_VarPrefixInput, &Button, 12.0f);
        }

        // ---- Team column ----
        {
                CUIRect Label, Button;
                TeamCol.HSplitTop(22.0f, &Label, &TeamCol);
                const ColorRGBA TeamRgba = PushinColorToRGBA(g_Config.m_PushinTeamColor);
                TextRender()->TextColor(TeamRgba.r, TeamRgba.g, TeamRgba.b, 1.0f);
                Ui()->DoLabel(&Label, "настройки тимов", 16.0f, TEXTALIGN_ML);
                TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
                TeamCol.HSplitTop(4.0f, nullptr, &TeamCol);

                static CButtonContainer s_TeamColorReset;
                unsigned int *pTeamColorPtr = (unsigned int *)&g_Config.m_PushinTeamColor;
                DoLine_ColorPicker(&s_TeamColorReset, ColorPickerLineSize, ColorPickerLabelSize,
                        ColorPickerLineSpacing, &TeamCol, "цвет тимов", pTeamColorPtr,
                        ColorRGBA(0.2f, 0.9f, 0.4f, 1.0f), false, &g_Config.m_PushinTeamTintSkin);

                TeamCol.HSplitTop(LineSize, &Button, &TeamCol);
                Ui()->DoScrollbarOption(&g_Config.m_PushinTeamTintPercent, &g_Config.m_PushinTeamTintPercent, &Button,
                        "сила цвета тимов", 0, 100, &CUi::ms_LinearScrollbarScale, 0u, "%");

                TeamCol.HSplitTop(LineSize, &Button, &TeamCol);
                if(DoButton_CheckBox(&g_Config.m_PushinTeamColorNick, "красить ники тимов", g_Config.m_PushinTeamColorNick, &Button))
                        g_Config.m_PushinTeamColorNick ^= 1;

                TeamCol.HSplitTop(LineSize, &Button, &TeamCol);
                if(DoButton_CheckBox(&g_Config.m_PushinTeamUsePrefix, "префикс для тимов", g_Config.m_PushinTeamUsePrefix, &Button))
                        g_Config.m_PushinTeamUsePrefix ^= 1;

                TeamCol.HSplitTop(LineSize, &Button, &TeamCol);
                static CLineInput s_TeamPrefixInput(g_Config.m_PushinTeamPrefix, sizeof(g_Config.m_PushinTeamPrefix));
                Ui()->DoClearableEditBox(&s_TeamPrefixInput, &Button, 12.0f);
        }
}

void CMenus::RenderPushinPlayerRow(const CUIRect &Row, int ClientId, const char *pDisplayName, int Status, int ColumnStatus)
{
        // Background: highlight if this is the row being dragged. Skip drawing
        // the row in its source column while dragging (the row "follows" the
        // cursor — see the floating-row render in RenderSettingsPushin).
        if(s_PushinDragClientId == ClientId && s_PushinDragging)
        {
                Row.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.05f), IGraphics::CORNER_ALL, 4.0f);
                return;
        }

        CUIRect TeeBox, NameBox, StatusBox;
        Row.VSplitLeft(40.0f, &TeeBox, &NameBox);
        NameBox.VSplitRight(60.0f, &NameBox, &StatusBox);

        // Tee preview
        {
                const CGameClient::CClientData &Client = GameClient()->m_aClients[ClientId];
                CTeeRenderInfo Info = Client.m_RenderInfo;
                ApplyPushinTintToTee(Info, Status);
                Info.m_Size = 36.0f;
                vec2 OffsetToMid;
                CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &Info, OffsetToMid);
                const vec2 Pos(TeeBox.x + TeeBox.w / 2.0f, TeeBox.y + TeeBox.h / 2.0f + OffsetToMid.y);
                const int Emote = PushinEmote(Status);
                RenderTools()->RenderTee(CAnimState::GetIdle(), &Info, Emote, vec2(1.0f, 0.0f), Pos);
        }

        // Name (with prefix + color)
        {
                const ColorRGBA NickColor = PushinNickColor(Status);
                TextRender()->TextColor(NickColor.r, NickColor.g, NickColor.b, NickColor.a);
                SLabelProperties Props;
                Props.m_MaxWidth = NameBox.w - 4.0f;
                Ui()->DoLabel(&NameBox, pDisplayName, 12.0f, TEXTALIGN_ML, Props);
                TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
        }

        // Status text
        const char *pStatusText = (Status == PUSHIN_STATUS_VAR) ? "вар" :
                                  (Status == PUSHIN_STATUS_TEAM) ? "тим" : "нету";
        const ColorRGBA StatusColor = (Status == PUSHIN_STATUS_VAR) ? ColorRGBA(0.9f, 0.2f, 0.2f, 1.0f) :
                                      (Status == PUSHIN_STATUS_TEAM) ? ColorRGBA(0.2f, 0.9f, 0.4f, 1.0f) :
                                      ColorRGBA(0.6f, 0.6f, 0.6f, 1.0f);
        TextRender()->TextColor(StatusColor.r, StatusColor.g, StatusColor.b, StatusColor.a);
        Ui()->DoLabel(&StatusBox, pStatusText, 12.0f, TEXTALIGN_MR);
        TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

        // Click handling on this row:
        //   - Double-click ONLY in var/team column removes the player back to
        //     Players (status = none). Single click in the Players column does
        //     nothing — only drag starts when the cursor leaves the row.
        //   - press-and-hold + drag: start a drag when the cursor moves outside
        //     the row while the button is held.
        const bool MouseInsideRow = Ui()->MouseInside(&Row);
        const bool MouseJustClicked = MouseInsideRow && Ui()->MouseButtonClicked(0);
        const bool MouseHeld = Ui()->MouseButton(0);

        if(MouseJustClicked && !s_PushinDragging)
        {
                // Double-click detection: only meaningful inside var/team columns.
                if(ColumnStatus != PUSHIN_STATUS_NONE)
                {
                        const auto Now = std::chrono::steady_clock::now();
                        const bool IsDoubleClick = (s_PushinLastClickClientId == ClientId) &&
                                (Now - s_PushinLastClickTime < PUSHIN_DOUBLE_CLICK_TIME);
                        if(IsDoubleClick && Status != PUSHIN_STATUS_NONE)
                        {
                                s_aPushinStatus[ClientId] = PUSHIN_STATUS_NONE;
                                s_PushinLastClickClientId = -1;
                                return;
                        }
                        s_PushinLastClickClientId = ClientId;
                        s_PushinLastClickTime = std::chrono::steady_clock::now();
                }
                // Begin a potential drag — actual drag starts when cursor moves
                // outside the row while still held.
                s_PushinDragClientId = ClientId;
        }

        // Promote to "dragging" once the cursor leaves the row while the button
        // is still held. This prevents accidental assignment on a simple click.
        if(s_PushinDragClientId == ClientId && MouseHeld && !MouseInsideRow)
        {
                s_PushinDragging = true;
        }
}

int CMenus::RenderPushinColumn(CUIRect View, const char *pTitle, const std::vector<int> &vClientIds, int ColumnStatus)
{
        // Drop target highlight — only when actually dragging.
        const bool IsDropTarget = s_PushinDragging && Ui()->MouseInside(&View);
        if(IsDropTarget)
                View.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.15f), IGraphics::CORNER_ALL, 6.0f);
        else
                View.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.3f), IGraphics::CORNER_ALL, 6.0f);
        View.Margin(4.0f, &View);

        // Title — for var/team columns, use the configured prefix if enabled.
        char aTitleBuf[32];
        if(ColumnStatus == PUSHIN_STATUS_VAR)
        {
                const char *pPref = (g_Config.m_PushinVarUsePrefix && g_Config.m_PushinVarPrefix[0]) ? g_Config.m_PushinVarPrefix : "вар";
                str_format(aTitleBuf, sizeof(aTitleBuf), "%s", pPref);
        }
        else if(ColumnStatus == PUSHIN_STATUS_TEAM)
        {
                const char *pPref = (g_Config.m_PushinTeamUsePrefix && g_Config.m_PushinTeamPrefix[0]) ? g_Config.m_PushinTeamPrefix : "тим";
                str_format(aTitleBuf, sizeof(aTitleBuf), "%s", pPref);
        }
        else
        {
                str_format(aTitleBuf, sizeof(aTitleBuf), "%s", pTitle);
        }

        CUIRect TitleRect;
        View.HSplitTop(20.0f, &TitleRect, &View);
        Ui()->DoLabel(&TitleRect, aTitleBuf, 14.0f, TEXTALIGN_MC);
        View.HSplitTop(4.0f, nullptr, &View);

        // Scrollable list — one static CListBox per column index.
        static CListBox s_aListBoxes[3];
        const int ListBoxIdx = (ColumnStatus == PUSHIN_STATUS_NONE) ? 0 :
                               (ColumnStatus == PUSHIN_STATUS_TEAM) ? 1 : 2;
        CListBox &ListBox = s_aListBoxes[ListBoxIdx];
        ListBox.DoStart(40.0f, vClientIds.size(), 1, 4, -1, &View);

        for(size_t i = 0; i < vClientIds.size(); ++i)
        {
                const int ClientId = vClientIds[i];
                const CGameClient::CClientData &Client = GameClient()->m_aClients[ClientId];
                char aDisplayName[64];
                FormatPushinName(aDisplayName, sizeof(aDisplayName), s_aPushinStatus[ClientId], Client.m_aName);

                const CListboxItem Item = ListBox.DoNextItem(reinterpret_cast<const void *>((intptr_t)(ClientId + 1)), false);
                if(!Item.m_Visible)
                        continue;

                RenderPushinPlayerRow(Item.m_Rect, ClientId, aDisplayName, s_aPushinStatus[ClientId], ColumnStatus);
        }

        ListBox.DoEnd();

        // Drop detection: only when a real drag is in progress and the button
        // was just released. This prevents the "random player gets added" bug
        // where a simple click on a row in the Players column would trigger a
        // drop because the cursor happened to be inside the column on release.
        int DroppedClientId = -1;
        const bool MouseJustReleased = !Ui()->MouseButton(0) && s_PushinMouseWasDown;
        if(s_PushinDragging && MouseJustReleased && s_PushinDragClientId >= 0 && Ui()->MouseInside(&View))
        {
                DroppedClientId = s_PushinDragClientId;
                s_aPushinStatus[DroppedClientId] = ColumnStatus;
                // End the drag immediately so it doesn't also drop into another
                // column whose rect happens to overlap the cursor.
                s_PushinDragging = false;
                s_PushinDragClientId = -1;
        }
        return DroppedClientId;
}

int CMenus::RenderPushinMiniMenu(CUIRect View)
{
        View.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.3f), IGraphics::CORNER_ALL, 6.0f);
        View.Margin(4.0f, &View);

        CUIRect TeamBtn, VarBtn;
        View.HSplitMid(&TeamBtn, &VarBtn, 4.0f);

        const char *pTeamLabel = (g_Config.m_PushinTeamUsePrefix && g_Config.m_PushinTeamPrefix[0]) ? g_Config.m_PushinTeamPrefix : "тим";
        const char *pVarLabel = (g_Config.m_PushinVarUsePrefix && g_Config.m_PushinVarPrefix[0]) ? g_Config.m_PushinVarPrefix : "вар";

        static CButtonContainer s_TeamBtn;
        static CButtonContainer s_VarBtn;

        int DroppedClientId = -1;
        const bool WasDragging = s_PushinDragging;
        const bool MouseDown = Ui()->MouseButton(0);

        // Team button (top)
        const bool TeamHover = Ui()->MouseInside(&TeamBtn);
        if(TeamHover && s_PushinDragging)
                TeamBtn.Draw(ColorRGBA(0.2f, 0.9f, 0.4f, 0.5f), IGraphics::CORNER_ALL, 4.0f);
        if(DoButton_Menu(&s_TeamBtn, pTeamLabel, 0, &TeamBtn, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 4.0f, 0.0f,
                ColorRGBA(0.2f, 0.9f, 0.4f, 0.6f)))
        {
                // Click without drag: no-op (drop target only)
        }
        if(WasDragging && !MouseDown && s_PushinDragClientId >= 0 && TeamHover)
        {
                DroppedClientId = s_PushinDragClientId;
                s_aPushinStatus[DroppedClientId] = PUSHIN_STATUS_TEAM;
        }

        // Var button (bottom)
        const bool VarHover = Ui()->MouseInside(&VarBtn);
        if(VarHover && s_PushinDragging)
                VarBtn.Draw(ColorRGBA(0.9f, 0.2f, 0.2f, 0.5f), IGraphics::CORNER_ALL, 4.0f);
        if(DoButton_Menu(&s_VarBtn, pVarLabel, 0, &VarBtn, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 4.0f, 0.0f,
                ColorRGBA(0.9f, 0.2f, 0.2f, 0.6f)))
        {
                // Click without drag: no-op (drop target only)
        }
        if(WasDragging && !MouseDown && s_PushinDragClientId >= 0 && VarHover)
        {
                DroppedClientId = s_PushinDragClientId;
                s_aPushinStatus[DroppedClientId] = PUSHIN_STATUS_VAR;
        }

        return DroppedClientId;
}

void CMenus::RenderPushinPreview(CUIRect View)
{
        View.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.3f), IGraphics::CORNER_ALL, 6.0f);
        View.Margin(8.0f, &View);

        // Two buttons at the top
        CUIRect BtnRow, PreviewArea;
        View.HSplitTop(24.0f, &BtnRow, &PreviewArea);
        BtnRow.VMargin((BtnRow.w - 200.0f) / 2.0f, &BtnRow);
        CUIRect TeamBtn, VarBtn;
        BtnRow.VSplitMid(&TeamBtn, &VarBtn, 8.0f);

        const char *pTeamLabel = (g_Config.m_PushinTeamUsePrefix && g_Config.m_PushinTeamPrefix[0]) ? g_Config.m_PushinTeamPrefix : "тим";
        const char *pVarLabel = (g_Config.m_PushinVarUsePrefix && g_Config.m_PushinVarPrefix[0]) ? g_Config.m_PushinVarPrefix : "вар";

        static CButtonContainer s_PreviewTeamBtn;
        static CButtonContainer s_PreviewVarBtn;
        if(DoButton_Menu(&s_PreviewTeamBtn, pTeamLabel, s_PushinPreviewStatus == PUSHIN_STATUS_TEAM, &TeamBtn, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 4.0f, 0.0f, ColorRGBA(0.2f, 0.9f, 0.4f, 0.7f)))
                s_PushinPreviewStatus = (s_PushinPreviewStatus == PUSHIN_STATUS_TEAM) ? PUSHIN_STATUS_NONE : PUSHIN_STATUS_TEAM;
        if(DoButton_Menu(&s_PreviewVarBtn, pVarLabel, s_PushinPreviewStatus == PUSHIN_STATUS_VAR, &VarBtn, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 4.0f, 0.0f, ColorRGBA(0.9f, 0.2f, 0.2f, 0.7f)))
                s_PushinPreviewStatus = (s_PushinPreviewStatus == PUSHIN_STATUS_VAR) ? PUSHIN_STATUS_NONE : PUSHIN_STATUS_VAR;

        // Nick on top, tee below looking at cursor.
        // Nick gets a fixed 18px; the rest of PreviewArea goes to the tee.
        CUIRect NickRect, TeeRect;
        PreviewArea.HSplitTop(18.0f, &NickRect, &TeeRect);

        // Local player name with current preview status prefix
        char aDisplayName[64];
        const int LocalId = GameClient()->m_Snap.m_LocalClientId;
        const char *pLocalName = (LocalId >= 0) ? GameClient()->m_aClients[LocalId].m_aName : "player";
        FormatPushinName(aDisplayName, sizeof(aDisplayName), s_PushinPreviewStatus, pLocalName);

        const ColorRGBA NickColor = PushinNickColor(s_PushinPreviewStatus);
        TextRender()->TextColor(NickColor.r, NickColor.g, NickColor.b, NickColor.a);
        Ui()->DoLabel(&NickRect, aDisplayName, 14.0f, TEXTALIGN_MC);
        TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

        // Tee looking at cursor — size scales with available TeeRect height
        // so the whole tee (body + feet + eyes) always fits.
        {
                const int LocalClientId = GameClient()->m_Snap.m_LocalClientId;
                if(LocalClientId >= 0)
                {
                        const CGameClient::CClientData &Client = GameClient()->m_aClients[LocalClientId];
                        CTeeRenderInfo Info = Client.m_RenderInfo;
                        ApplyPushinTintToTee(Info, s_PushinPreviewStatus);
                        // Tee render size = min(rect width, rect height) * 0.9
                        // so the tee fills ~90% of the available preview area.
                        Info.m_Size = std::min(TeeRect.w, TeeRect.h) * 0.9f;

                        vec2 OffsetToMid;
                        CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &Info, OffsetToMid);
                        const vec2 TeePos(TeeRect.x + TeeRect.w / 2.0f, TeeRect.y + TeeRect.h / 2.0f + OffsetToMid.y);

                        // Direction: from tee to cursor
                        const vec2 CursorPos(Ui()->MouseX(), Ui()->MouseY());
                        vec2 Dir = normalize(CursorPos - TeePos);

                        const int Emote = PushinEmote(s_PushinPreviewStatus);
                        RenderTools()->RenderTee(CAnimState::GetIdle(), &Info, Emote, Dir, TeePos);
                }
        }
}

// ============================================================================
// Public entry point — called from menus_settings.cpp when the
// "Пушин клиент" tab is active.
// ============================================================================
void CMenus::RenderSettingsPushin(CUIRect MainView)
{
        // Title
        CUIRect Title, Body;
        MainView.HSplitTop(30.0f, &Title, &Body);
        Body.HSplitTop(8.0f, nullptr, &Body);
        Ui()->DoLabel(&Title, "пушин клиент", 22.0f, TEXTALIGN_MC);

        // Collapsible "вар лист" row
        CUIRect VarListRow, Rest;
        Body.HSplitTop(28.0f, &VarListRow, &Rest);
        Rest.HSplitTop(4.0f, nullptr, &Rest);
        const bool Expanded = RenderPushinVarListRow(VarListRow);

        if(!Expanded)
                return;

        // Settings panel at the top
        CUIRect SettingsView, ColumnsAndPreview;
        Rest.HSplitTop(180.0f, &SettingsView, &ColumnsAndPreview);
        ColumnsAndPreview.HSplitTop(6.0f, nullptr, &ColumnsAndPreview);
        RenderPushinSettingsPanel(SettingsView);

        // Three columns (no mini-menu — user removed it).
        // Columns row is 180px tall; the rest of the space goes to the
        // preview area so the tee fits entirely.
        CUIRect ColumnsRow, PreviewArea;
        ColumnsAndPreview.HSplitTop(180.0f, &ColumnsRow, &PreviewArea);
        PreviewArea.HSplitTop(6.0f, nullptr, &PreviewArea);

        // Split the width into 3 equal columns
        CUIRect ColPlayers, ColTeam, ColVar;
        const float Gap = 4.0f;
        ColumnsRow.VSplitMid(&ColPlayers, &ColTeam, Gap);
        CUIRect ColTeamLeft, ColVarRight;
        ColTeam.VSplitMid(&ColTeamLeft, &ColVarRight, Gap);
        ColTeam = ColTeamLeft;
        ColVar = ColVarRight;

        // Build the client lists per status.
        // The Players column shows ONLY players with status=none — once a
        // player is dragged into var/team, they disappear from the Players
        // list and only appear in their assigned column. Drag them back to
        // the Players column (or double-click them in var/team) to return.
        std::vector<int> vPlayers, vTeam, vVar;
        const CGameClient *pGC = GameClient();
        for(int i = 0; i < MAX_CLIENTS; ++i)
        {
                if(!pGC->m_aClients[i].m_Active)
                        continue;
                const int Status = s_aPushinStatus[i];
                if(Status == PUSHIN_STATUS_TEAM)
                        vTeam.push_back(i);
                else if(Status == PUSHIN_STATUS_VAR)
                        vVar.push_back(i);
                else
                        vPlayers.push_back(i);
        }

        // Render the three columns
        (void)RenderPushinColumn(ColPlayers, "игроки", vPlayers, PUSHIN_STATUS_NONE);
        (void)RenderPushinColumn(ColTeam, "тим", vTeam, PUSHIN_STATUS_TEAM);
        (void)RenderPushinColumn(ColVar, "вар", vVar, PUSHIN_STATUS_VAR);

        // Update drag state: if mouse released, end the drag and clear the
        // pending drag client id. Also track s_PushinMouseWasDown for the
        // "just released" detection in RenderPushinColumn.
        const bool MouseDown = Ui()->MouseButton(0);
        if(!MouseDown)
        {
                s_PushinDragging = false;
                s_PushinDragClientId = -1;
        }
        s_PushinMouseWasDown = MouseDown;

        // Floating dragged row: while dragging, render the player's row
        // following the cursor so the user sees what they're carrying.
        if(s_PushinDragging && s_PushinDragClientId >= 0)
        {
                const int DragId = s_PushinDragClientId;
                const CGameClient::CClientData &Client = GameClient()->m_aClients[DragId];
                char aDisplayName[64];
                FormatPushinName(aDisplayName, sizeof(aDisplayName), s_aPushinStatus[DragId], Client.m_aName);

                // Build a floating rect centered on the cursor.
                const float RowW = 220.0f;
                const float RowH = 40.0f;
                CUIRect Floating;
                Floating.x = Ui()->MouseX() - RowW / 2.0f;
                Floating.y = Ui()->MouseY() - RowH / 2.0f;
                Floating.w = RowW;
                Floating.h = RowH;
                Floating.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.75f), IGraphics::CORNER_ALL, 4.0f);

                // Inline tee + name + status (don't call RenderPushinPlayerRow
                // because it would early-return on the drag guard).
                CUIRect TeeBox, NameBox, StatusBox;
                Floating.VSplitLeft(40.0f, &TeeBox, &NameBox);
                NameBox.VSplitRight(60.0f, &NameBox, &StatusBox);

                {
                        CTeeRenderInfo Info = Client.m_RenderInfo;
                        ApplyPushinTintToTee(Info, s_aPushinStatus[DragId]);
                        Info.m_Size = 36.0f;
                        vec2 OffsetToMid;
                        CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &Info, OffsetToMid);
                        const vec2 Pos(TeeBox.x + TeeBox.w / 2.0f, TeeBox.y + TeeBox.h / 2.0f + OffsetToMid.y);
                        const int Emote = PushinEmote(s_aPushinStatus[DragId]);
                        RenderTools()->RenderTee(CAnimState::GetIdle(), &Info, Emote, vec2(1.0f, 0.0f), Pos);
                }
                {
                        const ColorRGBA NickColor = PushinNickColor(s_aPushinStatus[DragId]);
                        TextRender()->TextColor(NickColor.r, NickColor.g, NickColor.b, NickColor.a);
                        SLabelProperties Props;
                        Props.m_MaxWidth = NameBox.w - 4.0f;
                        Ui()->DoLabel(&NameBox, aDisplayName, 12.0f, TEXTALIGN_ML, Props);
                        TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
                }
                {
                        const int Status = s_aPushinStatus[DragId];
                        const char *pStatusText = (Status == PUSHIN_STATUS_VAR) ? "вар" :
                                                  (Status == PUSHIN_STATUS_TEAM) ? "тим" : "нету";
                        const ColorRGBA StatusColor = (Status == PUSHIN_STATUS_VAR) ? ColorRGBA(0.9f, 0.2f, 0.2f, 1.0f) :
                                                      (Status == PUSHIN_STATUS_TEAM) ? ColorRGBA(0.2f, 0.9f, 0.4f, 1.0f) :
                                                      ColorRGBA(0.6f, 0.6f, 0.6f, 1.0f);
                        TextRender()->TextColor(StatusColor.r, StatusColor.g, StatusColor.b, StatusColor.a);
                        Ui()->DoLabel(&StatusBox, pStatusText, 12.0f, TEXTALIGN_MR);
                        TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
                }
        }

        // Preview at the bottom
        RenderPushinPreview(PreviewArea);
}
// ============================================================================
// Static accessor methods — used by players.cpp, nameplates.cpp, scoreboard.cpp
// to apply the var/team tint+prefix+emote in-game.
// ============================================================================

int CMenus::GetPushinStatus(int ClientId)
{
        if(ClientId < 0 || ClientId >= MAX_CLIENTS)
                return PUSHIN_STATUS_NONE;
        return s_aPushinStatus[ClientId];
}

void CMenus::GetPushinDisplayName(char *pBuf, int BufSize, int ClientId, const char *pOriginalName)
{
        const int Status = GetPushinStatus(ClientId);
        const char *pPrefix = nullptr;
        if(Status == PUSHIN_STATUS_VAR && g_Config.m_PushinVarUsePrefix)
                pPrefix = g_Config.m_PushinVarPrefix;
        else if(Status == PUSHIN_STATUS_TEAM && g_Config.m_PushinTeamUsePrefix)
                pPrefix = g_Config.m_PushinTeamPrefix;

        if(pPrefix && pPrefix[0] != '\0')
                str_format(pBuf, BufSize, "[%s] %s", pPrefix, pOriginalName);
        else
                str_copy(pBuf, pOriginalName, BufSize);
}

ColorRGBA CMenus::GetPushinNickColor(int ClientId)
{
        const int Status = GetPushinStatus(ClientId);
        if(Status == PUSHIN_STATUS_VAR && g_Config.m_PushinVarColorNick)
                return PushinColorToRGBA(g_Config.m_PushinVarColor);
        if(Status == PUSHIN_STATUS_TEAM && g_Config.m_PushinTeamColorNick)
                return PushinColorToRGBA(g_Config.m_PushinTeamColor);
        return ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
}

void CMenus::ApplyPushinToRenderInfo(CTeeRenderInfo &Info, int ClientId)
{
        ApplyPushinTintToTee(Info, GetPushinStatus(ClientId));
}

int CMenus::GetPushinEmote(int ClientId)
{
        return PushinEmote(GetPushinStatus(ClientId));
}
// ---- end Pushin client ----
