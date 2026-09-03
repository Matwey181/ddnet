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

namespace {
constexpr int PUSHIN_STATUS_NONE = 0;
constexpr int PUSHIN_STATUS_VAR = 1;
constexpr int PUSHIN_STATUS_TEAM = 2;

// Per-player pushin status. Indexed by client id.
int s_aPushinStatus[MAX_CLIENTS] = {};

// Drag state: which client id is currently being dragged (-1 = none).
int s_PushinDragClientId = -1;
// True while the drag is active (mouse/touch held).
bool s_PushinDragging = false;
// Preview status: cycles none → team → var → none when clicking preview buttons.
int s_PushinPreviewStatus = PUSHIN_STATUS_NONE;

// Helper: get a color RGBA from a packed HSLA config variable.
ColorRGBA PushinColorToRGBA(unsigned int PackedHsla)
{
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

	const float Mix = std::clamp(TintPercent / 100.0f, 0.0f, 1.0f) * 0.4f; // cap at 40%
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
		Ui()->DoLabel(&Label, "вар settings", 16.0f, TEXTALIGN_ML);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		VarCol.HSplitTop(4.0f, nullptr, &VarCol);

		static CButtonContainer s_VarColorReset;
		DoLine_ColorPicker(&s_VarColorReset, ColorPickerLineSize, ColorPickerLabelSize,
			ColorPickerLineSpacing, &VarCol, "var color", &g_Config.m_PushinVarColor,
			ColorRGBA(0.9f, 0.2f, 0.2f, 1.0f), false, &g_Config.m_PushinVarTintSkin);

		VarCol.HSplitTop(LineSize, &Button, &VarCol);
		Ui()->DoScrollbarOption(&g_Config.m_PushinVarTintPercent, &g_Config.m_PushinVarTintPercent, &Button,
			"var tint strength", 0, 100, &CUi::ms_LinearScrollbarScale, 0u, "%");

		VarCol.HSplitTop(LineSize, &Button, &VarCol);
		if(DoButton_CheckBox(&g_Config.m_PushinVarColorNick, "color var nicknames", g_Config.m_PushinVarColorNick, &Button))
			g_Config.m_PushinVarColorNick ^= 1;

		VarCol.HSplitTop(LineSize, &Button, &VarCol);
		if(DoButton_CheckBox(&g_Config.m_PushinVarUsePrefix, "add prefix to var nicks", g_Config.m_PushinVarUsePrefix, &Button))
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
		Ui()->DoLabel(&Label, "тим settings", 16.0f, TEXTALIGN_ML);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		TeamCol.HSplitTop(4.0f, nullptr, &TeamCol);

		static CButtonContainer s_TeamColorReset;
		DoLine_ColorPicker(&s_TeamColorReset, ColorPickerLineSize, ColorPickerLabelSize,
			ColorPickerLineSpacing, &TeamCol, "team color", &g_Config.m_PushinTeamColor,
			ColorRGBA(0.2f, 0.9f, 0.4f, 1.0f), false, &g_Config.m_PushinTeamTintSkin);

		TeamCol.HSplitTop(LineSize, &Button, &TeamCol);
		Ui()->DoScrollbarOption(&g_Config.m_PushinTeamTintPercent, &g_Config.m_PushinTeamTintPercent, &Button,
			"team tint strength", 0, 100, &CUi::ms_LinearScrollbarScale, 0u, "%");

		TeamCol.HSplitTop(LineSize, &Button, &TeamCol);
		if(DoButton_CheckBox(&g_Config.m_PushinTeamColorNick, "color team nicknames", g_Config.m_PushinTeamColorNick, &Button))
			g_Config.m_PushinTeamColorNick ^= 1;

		TeamCol.HSplitTop(LineSize, &Button, &TeamCol);
		if(DoButton_CheckBox(&g_Config.m_PushinTeamUsePrefix, "add prefix to team nicks", g_Config.m_PushinTeamUsePrefix, &Button))
			g_Config.m_PushinTeamUsePrefix ^= 1;

		TeamCol.HSplitTop(LineSize, &Button, &TeamCol);
		static CLineInput s_TeamPrefixInput(g_Config.m_PushinTeamPrefix, sizeof(g_Config.m_PushinTeamPrefix));
		Ui()->DoClearableEditBox(&s_TeamPrefixInput, &Button, 12.0f);
	}
}

void CMenus::RenderPushinPlayerRow(const CUIRect &Row, int ClientId, const char *pDisplayName, int Status)
{
	// Background: highlight if this is the row being dragged.
	if(s_PushinDragClientId == ClientId && s_PushinDragging)
		Row.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.20f), IGraphics::CORNER_ALL, 4.0f);

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

	// Drag start: if mouse pressed inside this row, grab this client.
	if(Ui()->MouseInside(&Row) && Ui()->MouseButton(0) && !s_PushinDragging && s_PushinDragClientId < 0)
	{
		s_PushinDragClientId = ClientId;
		s_PushinDragging = true;
	}
}

int CMenus::RenderPushinColumn(CUIRect View, const char *pTitle, const std::vector<int> &vClientIds, int ColumnStatus)
{
	// Drop target highlight
	const bool IsDropTarget = s_PushinDragging && Ui()->MouseInside(&View);
	if(IsDropTarget)
		View.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.15f), IGraphics::CORNER_ALL, 6.0f);
	else
		View.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.3f), IGraphics::CORNER_ALL, 6.0f);
	View.Margin(4.0f, &View);

	// Title
	CUIRect TitleRect;
	View.HSplitTop(20.0f, &TitleRect, &View);
	Ui()->DoLabel(&TitleRect, pTitle, 14.0f, TEXTALIGN_MC);
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

		RenderPushinPlayerRow(Item.m_Rect, ClientId, aDisplayName, s_aPushinStatus[ClientId]);
	}

	ListBox.DoEnd();

	// Drop detection: if the drag just ended (mouse released) and the cursor
	// is inside this column, assign the dragged client to this column's status.
	int DroppedClientId = -1;
	if(s_PushinDragging && !Ui()->MouseButton(0) && s_PushinDragClientId >= 0 && Ui()->MouseInside(&View))
	{
		DroppedClientId = s_PushinDragClientId;
		s_aPushinStatus[DroppedClientId] = ColumnStatus;
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

	// Nick on top, tee below looking at cursor
	CUIRect NickRect, TeeRect;
	PreviewArea.HSplitTop(20.0f, &NickRect, &TeeRect);

	// Local player name with current preview status prefix
	char aDisplayName[64];
	const int LocalId = GameClient()->m_Snap.m_LocalClientId;
	const char *pLocalName = (LocalId >= 0) ? GameClient()->m_aClients[LocalId].m_aName : "player";
	FormatPushinName(aDisplayName, sizeof(aDisplayName), s_PushinPreviewStatus, pLocalName);

	const ColorRGBA NickColor = PushinNickColor(s_PushinPreviewStatus);
	TextRender()->TextColor(NickColor.r, NickColor.g, NickColor.b, NickColor.a);
	Ui()->DoLabel(&NickRect, aDisplayName, 14.0f, TEXTALIGN_MC);
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

	// Tee looking at cursor
	{
		const int LocalClientId = GameClient()->m_Snap.m_LocalClientId;
		if(LocalClientId >= 0)
		{
			const CGameClient::CClientData &Client = GameClient()->m_aClients[LocalClientId];
			CTeeRenderInfo Info = Client.m_RenderInfo;
			ApplyPushinTintToTee(Info, s_PushinPreviewStatus);
			Info.m_Size = 80.0f;

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
	Ui()->DoLabel(&Title, "Пушин клиент", 22.0f, TEXTALIGN_MC);

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

	// Three columns + mini-menu on the right
	CUIRect ColumnsRow, MiniMenuCol, PreviewArea;
	ColumnsAndPreview.HSplitTop(220.0f, &ColumnsRow, &PreviewArea);
	PreviewArea.HSplitTop(6.0f, nullptr, &PreviewArea);

	// Reserve 80px on the right for the mini-menu
	ColumnsRow.VSplitRight(80.0f, &ColumnsRow, &MiniMenuCol);
	MiniMenuCol.HMargin(20.0f, &MiniMenuCol);

	// Split the remaining width into 3 columns
	CUIRect ColPlayers, ColTeam, ColVar;
	const float Gap = 4.0f;
	ColumnsRow.VSplitMid(&ColPlayers, &ColTeam, Gap);
	CUIRect ColTeamLeft, ColVarRight;
	ColTeam.VSplitMid(&ColTeamLeft, &ColVarRight, Gap);
	ColTeam = ColTeamLeft;
	ColVar = ColVarRight;

	// Build the client lists per status
	std::vector<int> vAll, vTeam, vVar;
	const CGameClient *pGC = GameClient();
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(!pGC->m_aClients[i].m_Active)
			continue;
		const int Status = s_aPushinStatus[i];
		vAll.push_back(i);
		if(Status == PUSHIN_STATUS_TEAM)
			vTeam.push_back(i);
		else if(Status == PUSHIN_STATUS_VAR)
			vVar.push_back(i);
	}

	// Render the three columns
	(void)RenderPushinColumn(ColPlayers, "Players", vAll, PUSHIN_STATUS_NONE);
	(void)RenderPushinColumn(ColTeam, "Team", vTeam, PUSHIN_STATUS_TEAM);
	(void)RenderPushinColumn(ColVar, "Var", vVar, PUSHIN_STATUS_VAR);

	// Mini-menu (drop target)
	(void)RenderPushinMiniMenu(MiniMenuCol);

	// Update drag state: if mouse released, end the drag.
	const bool MouseDown = Ui()->MouseButton(0);
	if(!MouseDown && s_PushinDragging)
	{
		s_PushinDragging = false;
		s_PushinDragClientId = -1;
	}

	// Preview at the bottom
	RenderPushinPreview(PreviewArea);
}
// ---- end Pushin client ----
