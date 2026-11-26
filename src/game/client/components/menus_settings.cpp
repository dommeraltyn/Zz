/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "countryflags.h"
#include "menus.h"
#include "skins.h"

#include <base/log.h>
#include <base/math.h>
#include <base/system.h>

#include <engine/font_icons.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/localization.h>
#include <engine/shared/protocol7.h>
#include <engine/storage.h>
#include <engine/textrender.h>
#include <engine/updater.h>

#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/components/chat.h>
#include <game/client/components/menu_background.h>
#include <game/client/components/sounds.h>
#include <game/client/gameclient.h>
#include <game/client/skin.h>
#include <game/client/ui.h>
#include <game/client/ui_listbox.h>
#include <game/client/ui_scrollregion.h>
#include <game/localization.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

using namespace std::chrono_literals;

void CMenus::RenderSettingsGeneral(CUIRect MainView)
{
	char aBuf[128 + IO_MAX_PATH_LENGTH];
	CUIRect Label, Button, Left, Right, Game, ClientSettings;
	MainView.HSplitTop(150.0f, &Game, &ClientSettings);

	// game
	{
		// headline
		Game.HSplitTop(30.0f, &Label, &Game);
		Ui()->DoLabel(&Label, Localize("Game"), 20.0f, TEXTALIGN_ML);
		Game.HSplitTop(5.0f, nullptr, &Game);
		Game.VSplitMid(&Left, nullptr, 20.0f);

		// dynamic camera
		Left.HSplitTop(20.0f, &Button, &Left);
		const bool IsDyncam = g_Config.m_ClDyncam || g_Config.m_ClMouseFollowfactor > 0;
		if(DoButton_CheckBox(&g_Config.m_ClDyncam, Localize("Dynamic Camera"), IsDyncam, &Button))
		{
			if(IsDyncam)
			{
				g_Config.m_ClDyncam = 0;
				g_Config.m_ClMouseFollowfactor = 0;
			}
			else
			{
				g_Config.m_ClDyncam = 1;
			}
		}

		// smooth dynamic camera
		Left.HSplitTop(5.0f, nullptr, &Left);
		Left.HSplitTop(20.0f, &Button, &Left);
		if(g_Config.m_ClDyncam)
		{
			if(DoButton_CheckBox(&g_Config.m_ClDyncamSmoothness, Localize("Smooth Dynamic Camera"), g_Config.m_ClDyncamSmoothness, &Button))
			{
				if(g_Config.m_ClDyncamSmoothness)
				{
					g_Config.m_ClDyncamSmoothness = 0;
				}
				else
				{
					g_Config.m_ClDyncamSmoothness = 50;
					g_Config.m_ClDyncamStabilizing = 50;
				}
			}
		}

		// weapon pickup
		Left.HSplitTop(5.0f, nullptr, &Left);
		Left.HSplitTop(20.0f, &Button, &Left);
		if(DoButton_CheckBox(&g_Config.m_ClAutoswitchWeapons, Localize("Switch weapon on pickup"), g_Config.m_ClAutoswitchWeapons, &Button))
			g_Config.m_ClAutoswitchWeapons ^= 1;

		// weapon out of ammo autoswitch
		Left.HSplitTop(5.0f, nullptr, &Left);
		Left.HSplitTop(20.0f, &Button, &Left);
		if(DoButton_CheckBox(&g_Config.m_ClAutoswitchWeaponsOutOfAmmo, Localize("Switch weapon when out of ammo"), g_Config.m_ClAutoswitchWeaponsOutOfAmmo, &Button))
			g_Config.m_ClAutoswitchWeaponsOutOfAmmo ^= 1;
	}

	// client
	{
		// headline
		ClientSettings.HSplitTop(30.0f, &Label, &ClientSettings);
		Ui()->DoLabel(&Label, Localize("Client"), 20.0f, TEXTALIGN_ML);
		ClientSettings.HSplitTop(5.0f, nullptr, &ClientSettings);
		ClientSettings.VSplitMid(&Left, &Right, 20.0f);

		// skip main menu
		Left.HSplitTop(20.0f, &Button, &Left);
		if(DoButton_CheckBox(&g_Config.m_ClSkipStartMenu, Localize("Skip the main menu"), g_Config.m_ClSkipStartMenu, &Button))
			g_Config.m_ClSkipStartMenu ^= 1;

		Left.HSplitTop(10.0f, nullptr, &Left);
		Left.HSplitTop(20.0f, &Button, &Left);
		str_copy(aBuf, " ");
		str_append(aBuf, Localize("Hz", "Hertz"));
		Ui()->DoScrollbarOption(&g_Config.m_ClRefreshRate, &g_Config.m_ClRefreshRate, &Button, Localize("Update Rate"), 10, 1000, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_INFINITE | CUi::SCROLLBAR_OPTION_NOCLAMPVALUE | CUi::SCROLLBAR_OPTION_DELAYUPDATE, aBuf);
		Left.HSplitTop(5.0f, nullptr, &Left);
		Left.HSplitTop(20.0f, &Button, &Left);
		static int s_LowerRefreshRate;
		if(DoButton_CheckBox(&s_LowerRefreshRate, Localize("Save power by lowering update rate (higher input latency)"), g_Config.m_ClRefreshRate <= 480 && g_Config.m_ClRefreshRate != 0, &Button))
			g_Config.m_ClRefreshRate = g_Config.m_ClRefreshRate > 480 || g_Config.m_ClRefreshRate == 0 ? 480 : 0;

		CUIRect SettingsButton;
		Left.HSplitBottom(20.0f, &Left, &SettingsButton);
		Left.HSplitBottom(5.0f, &Left, nullptr);
		static CButtonContainer s_SettingsButtonId;
		if(DoButton_Menu(&s_SettingsButtonId, Localize("Settings file"), 0, &SettingsButton))
		{
			Storage()->GetCompletePath(IStorage::TYPE_SAVE, s_aConfigDomains[ConfigDomain::DDNET].m_aConfigPath, aBuf, sizeof(aBuf));
			Client()->ViewFile(aBuf);
		}
		GameClient()->m_Tooltips.DoToolTip(&s_SettingsButtonId, &SettingsButton, Localize("Open the settings file"));

		CUIRect SavesButton;
		Left.HSplitBottom(20.0f, &Left, &SavesButton);
		Left.HSplitBottom(5.0f, &Left, nullptr);
		static CButtonContainer s_SavesButtonId;
		if(DoButton_Menu(&s_SavesButtonId, Localize("Saves file"), 0, &SavesButton))
		{
			Storage()->GetCompletePath(IStorage::TYPE_SAVE, SAVES_FILE, aBuf, sizeof(aBuf));
			Client()->ViewFile(aBuf);
		}
		GameClient()->m_Tooltips.DoToolTip(&s_SavesButtonId, &SavesButton, Localize("Open the saves file"));

		CUIRect ConfigButton;
		Left.HSplitBottom(20.0f, &Left, &ConfigButton);
		Left.HSplitBottom(5.0f, &Left, nullptr);
		static CButtonContainer s_ConfigButtonId;
		if(DoButton_Menu(&s_ConfigButtonId, Localize("Config directory"), 0, &ConfigButton))
		{
			Storage()->GetCompletePath(IStorage::TYPE_SAVE, "", aBuf, sizeof(aBuf));
			Client()->ViewFile(aBuf);
		}
		GameClient()->m_Tooltips.DoToolTip(&s_ConfigButtonId, &ConfigButton, Localize("Open the directory that contains the configuration and user files"));

		CUIRect DirectoryButton;
		Left.HSplitBottom(20.0f, &Left, &DirectoryButton);
		Left.HSplitBottom(5.0f, &Left, nullptr);
		static CButtonContainer s_ThemesButtonId;
		if(DoButton_Menu(&s_ThemesButtonId, Localize("Themes directory"), 0, &DirectoryButton))
		{
			Storage()->GetCompletePath(IStorage::TYPE_SAVE, "themes", aBuf, sizeof(aBuf));
			Storage()->CreateFolder("themes", IStorage::TYPE_SAVE);
			Client()->ViewFile(aBuf);
		}
		GameClient()->m_Tooltips.DoToolTip(&s_ThemesButtonId, &DirectoryButton, Localize("Open the directory to add custom themes"));

		Left.HSplitTop(20.0f, nullptr, &Left);
		RenderThemeSelection(Left);

		// auto demo settings
		{
			Right.HSplitTop(40.0f, nullptr, &Right);
			Right.HSplitTop(20.0f, &Button, &Right);
			if(DoButton_CheckBox(&g_Config.m_ClAutoDemoRecord, Localize("Automatically record demos"), g_Config.m_ClAutoDemoRecord, &Button))
				g_Config.m_ClAutoDemoRecord ^= 1;

			Right.HSplitTop(2 * 20.0f, &Button, &Right);
			if(g_Config.m_ClAutoDemoRecord)
				Ui()->DoScrollbarOption(&g_Config.m_ClAutoDemoMax, &g_Config.m_ClAutoDemoMax, &Button, Localize("Max demos"), 1, 1000, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_INFINITE | CUi::SCROLLBAR_OPTION_MULTILINE);

			Right.HSplitTop(10.0f, nullptr, &Right);
			Right.HSplitTop(20.0f, &Button, &Right);
			if(DoButton_CheckBox(&g_Config.m_ClAutoScreenshot, Localize("Automatically take game over screenshot"), g_Config.m_ClAutoScreenshot, &Button))
				g_Config.m_ClAutoScreenshot ^= 1;

			Right.HSplitTop(2 * 20.0f, &Button, &Right);
			if(g_Config.m_ClAutoScreenshot)
				Ui()->DoScrollbarOption(&g_Config.m_ClAutoScreenshotMax, &g_Config.m_ClAutoScreenshotMax, &Button, Localize("Max Screenshots"), 1, 1000, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_INFINITE | CUi::SCROLLBAR_OPTION_MULTILINE);
		}

		// auto statboard screenshot
		{
			Right.HSplitTop(10.0f, nullptr, &Right);
			Right.HSplitTop(20.0f, &Button, &Right);
			if(DoButton_CheckBox(&g_Config.m_ClAutoStatboardScreenshot, Localize("Automatically take statboard screenshot"), g_Config.m_ClAutoStatboardScreenshot, &Button))
			{
				g_Config.m_ClAutoStatboardScreenshot ^= 1;
			}

			Right.HSplitTop(2 * 20.0f, &Button, &Right);
			if(g_Config.m_ClAutoStatboardScreenshot)
				Ui()->DoScrollbarOption(&g_Config.m_ClAutoStatboardScreenshotMax, &g_Config.m_ClAutoStatboardScreenshotMax, &Button, Localize("Max Screenshots"), 1, 1000, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_INFINITE | CUi::SCROLLBAR_OPTION_MULTILINE);
		}

		// auto statboard csv
		{
			Right.HSplitTop(10.0f, nullptr, &Right);
			Right.HSplitTop(20.0f, &Button, &Right);
			if(DoButton_CheckBox(&g_Config.m_ClAutoCSV, Localize("Automatically create statboard csv"), g_Config.m_ClAutoCSV, &Button))
			{
				g_Config.m_ClAutoCSV ^= 1;
			}

			Right.HSplitTop(2 * 20.0f, &Button, &Right);
			if(g_Config.m_ClAutoCSV)
				Ui()->DoScrollbarOption(&g_Config.m_ClAutoCSVMax, &g_Config.m_ClAutoCSVMax, &Button, Localize("Max CSVs"), 1, 1000, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_INFINITE | CUi::SCROLLBAR_OPTION_MULTILINE);
		}
	}
}

namespace
{
	CBindSlot FindBindSlotForCommand(CBinds *pBinds, const char *pCommand)
	{
		for(int Modifier = KeyModifier::NONE; Modifier < KeyModifier::COMBINATION_COUNT; Modifier++)
		{
			for(int Key = KEY_FIRST; Key < KEY_LAST; Key++)
			{
				if(str_comp(pBinds->Get(Key, Modifier), pCommand) == 0)
					return CBindSlot(Key, Modifier);
			}
		}
		return EMPTY_BIND_SLOT;
	}

	void UnbindCommand(CBinds *pBinds, const char *pCommand)
	{
		for(int Modifier = KeyModifier::NONE; Modifier < KeyModifier::COMBINATION_COUNT; Modifier++)
		{
			for(int Key = KEY_FIRST; Key < KEY_LAST; Key++)
			{
				if(str_comp(pBinds->Get(Key, Modifier), pCommand) == 0)
					pBinds->Bind(Key, "", false, Modifier);
			}
		}
	}
	} // namespace

void CMenus::RenderSettingsZZetClient(CUIRect MainView)
{
	enum
	{
		ZZ_TAB_AIMBOT = 0,
		ZZ_TAB_GORES,
		ZZ_TAB_AVOID,
		ZZ_TAB_MISC,
		ZZ_TAB_FLY,
		ZZ_TAB_FNG,
		ZZ_TAB_VISUALS,
		ZZ_TAB_BESTCLIENT,
		ZZ_TAB_TAS,
		ZZ_NUM_TABS
	};

	static int s_SubTab = 0;
	static CButtonContainer s_aSubTabs[ZZ_NUM_TABS] = {};
	static int64_t s_SubTabAnimStart = 0;
	static int s_SubTabAnimFrom = 0;
	static int s_SubTabAnimTo = 0;

	CUIRect TabBar, Button;
	MainView.HSplitTop(24.0f, &TabBar, &MainView);
	const float TabWidth = TabBar.w / (float)ZZ_NUM_TABS;
	const char *apTabNames[] = {
		Localize("Aimbot"),
		Localize("Gores"),
		Localize("Avoid"),
		Localize("Misc"),
		Localize("Fly"),
		Localize("FNG"),
		Localize("Visuals"),
		Localize("Others"),
		"TAS"};
	if(s_SubTab >= ZZ_NUM_TABS)
		s_SubTab = 0;
	for(int i = 0; i < ZZ_NUM_TABS; i++)
	{
		TabBar.VSplitLeft(TabWidth, &Button, &TabBar);
		const int Corners = i == 0 ? IGraphics::CORNER_L : i == ZZ_NUM_TABS - 1 ? IGraphics::CORNER_R : IGraphics::CORNER_NONE;
		if(DoButton_MenuTab(&s_aSubTabs[i], apTabNames[i], s_SubTab == i, &Button, Corners, nullptr, nullptr, nullptr, nullptr, 4.0f))
		{
			s_SubTabAnimFrom = s_SubTab;
			s_SubTabAnimTo = i;
			s_SubTabAnimStart = time_get();
			s_SubTab = i;
		}
	}
	MainView.HSplitTop(10.0f, nullptr, &MainView);

	CUIRect Label;
	MainView.HSplitTop(30.0f, &Label, &MainView);
	Ui()->DoLabel(&Label, Localize("ZZetClient Settings"), 20.0f, TEXTALIGN_ML);
	MainView.HSplitTop(10.0f, nullptr, &MainView);

	const int64_t Now = time_get();
	const float AnimDuration = 0.16f;
	const float AnimDurationTicks = AnimDuration * (float)time_freq();
	float AnimT = 1.0f;
	if(s_SubTabAnimStart != 0 && AnimDurationTicks > 0.0f)
	{
		AnimT = std::clamp((float)(Now - s_SubTabAnimStart) / AnimDurationTicks, 0.0f, 1.0f);
		if(AnimT >= 1.0f)
			s_SubTabAnimStart = 0;
	}
	const float Ease = AnimT * AnimT * (3.0f - 2.0f * AnimT);
	const float FadeAlpha = 0.72f + 0.28f * Ease;
	const float SlideDir = (float)((s_SubTabAnimTo > s_SubTabAnimFrom) - (s_SubTabAnimTo < s_SubTabAnimFrom));
	MainView.x += (1.0f - Ease) * SlideDir * 22.0f;
	ColorRGBA OldTextColor = TextRender()->DefaultTextColor();
	TextRender()->TextColor(OldTextColor.WithMultipliedAlpha(FadeAlpha));

	CScrollRegion *pFeatureScrollRegion = nullptr;
	// ZZClient card migration plan:
	// 1) Use a single BeginCard -> content -> EndCard lifecycle for every card.
	// 2) EndCard measures real used content by delta of the mutable inner rect.
	// 3) Final card height = used content + margins + bottom pad + CardHeightAdjust, clamped.
	// 4) Migrate all ZZ tabs/cards (Aimbot/Gores/Avoid/Misc/Fly/FNG/Visuals/Others/Bots/TAS).
	// 5) Verify no legacy fixed-height helper calls remain and scroll/layout stay intact.
	const float CardHeightAdjust = 6.0f;
	const float CardMinHeight = 40.0f;
	const float CardCorner = 10.0f;
	const float CardInnerMargin = 10.0f;
	const float CardGap = 10.0f;
	const float CardBottomPad = 4.0f;
	struct SCardCtx
	{
		bool m_Active = false;
		CUIRect *m_pView = nullptr;
		CUIRect *m_pInner = nullptr;
		CUIRect m_PanelReserved;
		CUIRect m_InnerStart;
		float m_ReservedHeight = 0.0f;
	};
	SCardCtx CardCtx;
	auto EndCard = [&]() {
		if(!CardCtx.m_Active)
			return;
		const float UsedInner = maximum(0.0f, CardCtx.m_InnerStart.h - CardCtx.m_pInner->h);
		const float AutoHeight = UsedInner + CardInnerMargin * 2.0f + CardBottomPad;
		const float FinalHeight = maximum(CardMinHeight, AutoHeight + CardHeightAdjust);
		const float Delta = CardCtx.m_ReservedHeight - FinalHeight;
		if(Delta > 0.0f)
		{
			CardCtx.m_pView->y -= Delta;
			CardCtx.m_pView->h += Delta;
		}
		CardCtx.m_Active = false;
	};
	auto BeginCard = [&](CUIRect &View, float HeightHint, CUIRect &Inner) {
		EndCard();
		CUIRect Panel;
		const float ReservedHeight = maximum(CardMinHeight, HeightHint + CardHeightAdjust);
		View.HSplitTop(ReservedHeight, &Panel, &View);
		if(pFeatureScrollRegion)
			pFeatureScrollRegion->AddRect(Panel);
		Panel.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, CardCorner);
		Panel.Margin(CardInnerMargin, &Inner);
		View.HSplitTop(CardGap, nullptr, &View);
		CardCtx.m_Active = true;
		CardCtx.m_pView = &View;
		CardCtx.m_pInner = &Inner;
		CardCtx.m_PanelReserved = Panel;
		CardCtx.m_InnerStart = Inner;
		CardCtx.m_ReservedHeight = ReservedHeight;
	};
	auto DoPanelHeader = [&](CUIRect &Inner, const char *pTitle) {
		CUIRect Header;
		Inner.HSplitTop(16.0f, &Header, &Inner);
		Ui()->DoLabel(&Header, pTitle, 16.0f, TEXTALIGN_ML);
		Inner.HSplitTop(4.0f, nullptr, &Inner);
	};
	const auto UpdateRevealPhase = [&](float &Phase, bool Expanded) {
		const float Target = Expanded ? 1.0f : 0.0f;
		if(g_Config.m_BcAnimations)
		{
			const float Dt = maximum(Client()->RenderFrameTime(), 0.0f);
			const float Speed = 10.0f;
			if(Phase < Target)
				Phase = minimum(Target, Phase + Dt * Speed);
			else
				Phase = maximum(Target, Phase - Dt * Speed);
		}
		else
			Phase = Target;
	};

	if(s_SubTab == ZZ_TAB_AIMBOT)
	{
		const float LineSize = 20.0f;
		static CScrollRegion s_AimbotScrollRegion;
		static vec2 s_AimbotScrollOffset(0.0f, 0.0f);
		CScrollRegionParams ScrollParams;
		ScrollParams.m_ScrollUnit = 28.0f;
		s_AimbotScrollRegion.Begin(&MainView, &s_AimbotScrollOffset, &ScrollParams);
		CUIRect ScrollView = MainView;
		ScrollView.y += s_AimbotScrollOffset.y;
		pFeatureScrollRegion = &s_AimbotScrollRegion;

		CUIRect Left, Right;
		ScrollView.VSplitMid(&Left, &Right, 20.0f);

		// Left: Aimbot
		{
			CUIRect Inner;
			static float s_AimbotGhostAuraPhase = 0.0f;
			const bool GhostAuraEnabled = g_Config.m_ClZzAimbotGhostAura != 0;
			UpdateRevealPhase(s_AimbotGhostAuraPhase, GhostAuraEnabled);
			BeginCard(Left, 278.0f + 288.0f * s_AimbotGhostAuraPhase, Inner);
			DoPanelHeader(Inner, Localize("Aimbot"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAimbotEnabled, Localize("Enable aimbot"), &g_Config.m_ClZzAimbotEnabled, &Inner, LineSize);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Ui()->DoScrollbarOption(&g_Config.m_ClZzAimbotFov, &g_Config.m_ClZzAimbotFov, &Button, Localize("FOV"), 1, 360);
			static CUi::SDropDownState s_AimbotModeState;
			static CScrollRegion s_AimbotModeScroll;
			s_AimbotModeState.m_SelectionPopupContext.m_pScrollRegion = &s_AimbotModeScroll;
			const char *apAimbotModes[] = {Localize("Crosshair"), Localize("Nearest player")};
			Inner.HSplitTop(LineSize, &Button, &Inner);
			g_Config.m_ClZzAimbotTargetMode = Ui()->DoDropDown(&Button, g_Config.m_ClZzAimbotTargetMode, apAimbotModes, 2, s_AimbotModeState);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Choose how target is selected"), 14.0f, TEXTALIGN_ML);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAimbotIgnoreFriends, Localize("Ignore friends"), &g_Config.m_ClZzAimbotIgnoreFriends, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAimbotSilent, Localize("Silent aim"), &g_Config.m_ClZzAimbotSilent, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAimbotShowFov, Localize("show FOV"), &g_Config.m_ClZzAimbotShowFov, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAimbotGlowTarget, Localize("Glow target"), &g_Config.m_ClZzAimbotGlowTarget, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAimbotGhostAura, Localize("Target ghost aura"), &g_Config.m_ClZzAimbotGhostAura, &Inner, LineSize);

			const float GhostExtra = 288.0f * s_AimbotGhostAuraPhase;
			if(GhostExtra > 0.0f)
			{
				CUIRect Expand;
				Inner.HSplitTop(GhostExtra, &Expand, &Inner);
				static CUi::SDropDownState s_GhostAxisState;
				static CScrollRegion s_GhostAxisScroll;
				static CUi::SDropDownState s_GhostDirectionState;
				static CScrollRegion s_GhostDirectionScroll;
				static CUi::SDropDownState s_GhostAxis2State;
				static CScrollRegion s_GhostAxis2Scroll;
				static CUi::SDropDownState s_GhostDirection2State;
				static CScrollRegion s_GhostDirection2Scroll;
				s_GhostAxisState.m_SelectionPopupContext.m_pScrollRegion = &s_GhostAxisScroll;
				s_GhostDirectionState.m_SelectionPopupContext.m_pScrollRegion = &s_GhostDirectionScroll;
				s_GhostAxis2State.m_SelectionPopupContext.m_pScrollRegion = &s_GhostAxis2Scroll;
				s_GhostDirection2State.m_SelectionPopupContext.m_pScrollRegion = &s_GhostDirection2Scroll;
				const char *apAxis[] = {Localize("X axis"), Localize("Y axis"), Localize("Z axis")};
				const char *apDirections[] = {Localize("Clockwise"), Localize("Counter-clockwise")};
				Expand.HSplitTop(LineSize, &Button, &Expand);
				g_Config.m_ClZzAimbotGhostAuraAxis = Ui()->DoDropDown(&Button, g_Config.m_ClZzAimbotGhostAuraAxis - 1, apAxis, 3, s_GhostAxisState) + 1;
				Expand.HSplitTop(LineSize, &Button, &Expand);
				g_Config.m_ClZzAimbotGhostAuraDirection = Ui()->DoDropDown(&Button, g_Config.m_ClZzAimbotGhostAuraDirection - 1, apDirections, 2, s_GhostDirectionState) + 1;
				Expand.HSplitTop(LineSize, &Button, &Expand);
				g_Config.m_ClZzAimbotGhostAuraAxis2 = Ui()->DoDropDown(&Button, g_Config.m_ClZzAimbotGhostAuraAxis2 - 1, apAxis, 3, s_GhostAxis2State) + 1;
				Expand.HSplitTop(LineSize, &Button, &Expand);
				g_Config.m_ClZzAimbotGhostAuraDirection2 = Ui()->DoDropDown(&Button, g_Config.m_ClZzAimbotGhostAuraDirection2 - 1, apDirections, 2, s_GhostDirection2State) + 1;
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAimbotGhostAuraRadius, &g_Config.m_ClZzAimbotGhostAuraRadius, &Button, Localize("Ghost radius"), 8, 200);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAimbotGhostAuraStretch, &g_Config.m_ClZzAimbotGhostAuraStretch, &Button, Localize("Ring 1 stretch"), 50, 250);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAimbotGhostAuraSize, &g_Config.m_ClZzAimbotGhostAuraSize, &Button, Localize("Line thickness"), 50, 250);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAimbotGhostAuraSpeed, &g_Config.m_ClZzAimbotGhostAuraSpeed, &Button, Localize("Ghost speed"), 10, 300);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAimbotGhostAuraStretch2, &g_Config.m_ClZzAimbotGhostAuraStretch2, &Button, Localize("Ring 2 stretch"), 50, 250);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAimbotGhostAuraAlpha, &g_Config.m_ClZzAimbotGhostAuraAlpha, &Button, Localize("Ghost alpha"), 1, 100);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAimbotGhostAuraUseTargetBodyColor, Localize("Use target body color"), &g_Config.m_ClZzAimbotGhostAuraUseTargetBodyColor, &Expand, LineSize);
				if(!g_Config.m_ClZzAimbotGhostAuraUseTargetBodyColor)
				{
					static CButtonContainer s_GhostAuraColorResetId;
					DoLine_ColorPicker(&s_GhostAuraColorResetId, 25.0f, 13.0f, 4.0f, &Expand, Localize("Ghost color"), &g_Config.m_ClZzAimbotGhostAuraColor, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), false);
				}
			}

			Inner.HSplitTop(5.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Aimbot key"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CButtonContainer s_AimbotKeyReader;
			static CButtonContainer s_AimbotKeyClear;
			CBindSlot CurrentBindAimbot((int)g_Config.m_ClZzAimbotKey, (int)g_Config.m_ClZzAimbotMod);
			const auto ResAimbot = GameClient()->m_KeyBinder.DoKeyReader(&s_AimbotKeyReader, &s_AimbotKeyClear, &Button, CurrentBindAimbot, false);
			if(!ResAimbot.m_Aborted && (ResAimbot.m_Bind.m_Key != CurrentBindAimbot.m_Key || ResAimbot.m_Bind.m_ModifierMask != CurrentBindAimbot.m_ModifierMask))
			{
				if(CurrentBindAimbot.m_Key >= KEY_FIRST && CurrentBindAimbot.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(CurrentBindAimbot.m_Key, "", false, CurrentBindAimbot.m_ModifierMask);
				g_Config.m_ClZzAimbotKey = ResAimbot.m_Bind.m_Key;
				g_Config.m_ClZzAimbotMod = ResAimbot.m_Bind.m_ModifierMask;
				if(ResAimbot.m_Bind.m_Key >= KEY_FIRST && ResAimbot.m_Bind.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(ResAimbot.m_Bind.m_Key, "+zz_aimbot_toggle", false, ResAimbot.m_Bind.m_ModifierMask);
			}
		}
		{
			CUIRect Inner;
			BeginCard(Left, 196.0f, Inner);
			DoPanelHeader(Inner, Localize("Tile aimbot"));
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Tile aimbot key"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CButtonContainer s_SaveInBlockKeyReader;
			static CButtonContainer s_SaveInBlockKeyClear;
			CBindSlot CurrentBindSave((int)g_Config.m_ClZzSaveInBlockKey, (int)g_Config.m_ClZzSaveInBlockMod);
			const auto ResSave = GameClient()->m_KeyBinder.DoKeyReader(&s_SaveInBlockKeyReader, &s_SaveInBlockKeyClear, &Button, CurrentBindSave, false);
			if(!ResSave.m_Aborted && (ResSave.m_Bind.m_Key != CurrentBindSave.m_Key || ResSave.m_Bind.m_ModifierMask != CurrentBindSave.m_ModifierMask))
			{
				if(CurrentBindSave.m_Key >= KEY_FIRST && CurrentBindSave.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(CurrentBindSave.m_Key, "", false, CurrentBindSave.m_ModifierMask);
				g_Config.m_ClZzSaveInBlockKey = ResSave.m_Bind.m_Key;
				g_Config.m_ClZzSaveInBlockMod = ResSave.m_Bind.m_ModifierMask;
				if(ResSave.m_Bind.m_Key >= KEY_FIRST && ResSave.m_Bind.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(ResSave.m_Bind.m_Key, "+zz_tile_aimbot", false, ResSave.m_Bind.m_ModifierMask);
			}
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzSaveInBlockSilent, Localize("Silent"), &g_Config.m_ClZzSaveInBlockSilent, &Inner, LineSize);
			Inner.HSplitTop(10.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Hold key to hook nearest visible hookable tile."), 14.0f, TEXTALIGN_ML);
		}

		// Right: Auto-hit + Fly helper
		{
			CUIRect Inner;
			BeginCard(Right, 136.0f, Inner);
			DoPanelHeader(Inner, Localize("Auto-hit"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzHammerAssistEnabled, Localize("Enable auto-hit"), &g_Config.m_ClZzHammerAssistEnabled, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzHammerAssistIgnoreFriends, Localize("Ignore friends"), &g_Config.m_ClZzHammerAssistIgnoreFriends, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzHammerAssistSilent, Localize("Silent"), &g_Config.m_ClZzHammerAssistSilent, &Inner, LineSize);
			Inner.HSplitTop(5.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Hits enemy while hammer is held."), 14.0f, TEXTALIGN_ML);
		}
		{
			CUIRect Inner;
			BeginCard(Right, 186.0f, Inner);
			DoPanelHeader(Inner, Localize("AutoHook"));
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("AutoHook key"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CButtonContainer s_AutoHookPlayersKeyReader;
			static CButtonContainer s_AutoHookPlayersKeyClear;
			CBindSlot CurrentBindAutoHook((int)g_Config.m_ClZzAimbotAutoHookPlayersKey, (int)g_Config.m_ClZzAimbotAutoHookPlayersMod);
			const auto ResAutoHook = GameClient()->m_KeyBinder.DoKeyReader(&s_AutoHookPlayersKeyReader, &s_AutoHookPlayersKeyClear, &Button, CurrentBindAutoHook, false);
			if(!ResAutoHook.m_Aborted && (ResAutoHook.m_Bind.m_Key != CurrentBindAutoHook.m_Key || ResAutoHook.m_Bind.m_ModifierMask != CurrentBindAutoHook.m_ModifierMask))
			{
				if(CurrentBindAutoHook.m_Key >= KEY_FIRST && CurrentBindAutoHook.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(CurrentBindAutoHook.m_Key, "", false, CurrentBindAutoHook.m_ModifierMask);
				g_Config.m_ClZzAimbotAutoHookPlayersKey = ResAutoHook.m_Bind.m_Key;
				g_Config.m_ClZzAimbotAutoHookPlayersMod = ResAutoHook.m_Bind.m_ModifierMask;
				if(ResAutoHook.m_Bind.m_Key >= KEY_FIRST && ResAutoHook.m_Bind.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(ResAutoHook.m_Bind.m_Key, "+zz_auto_hook_players", false, ResAutoHook.m_Bind.m_ModifierMask);
			}
			Inner.HSplitTop(5.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Hooks target when it enters FOV."), 14.0f, TEXTALIGN_ML);
		}

		EndCard();
		s_AimbotScrollRegion.End();
		pFeatureScrollRegion = nullptr;
	}
	else if(s_SubTab == ZZ_TAB_GORES)
	{
		const float LineSize = 20.0f;
		CUIRect Left, Right;
		MainView.VSplitMid(&Left, &Right, 20.0f);

		// Left: Hammer-gun
		{
			CUIRect Inner;
			BeginCard(Left, 136.0f, Inner);
			DoPanelHeader(Inner, Localize("Hammer-gun"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzHammerGun, Localize("Enable hammer-gun"), &g_Config.m_ClZzHammerGun, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzHammerGunAutoDisableExtraWeapons, Localize("Disable on extra weapons"), &g_Config.m_ClZzHammerGunAutoDisableExtraWeapons, &Inner, LineSize);
			Inner.HSplitTop(5.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Keeps gun selected, replaces shot with hammer hit."), 14.0f, TEXTALIGN_ML);
		}

		(void)Right;
	}
	else if(s_SubTab == ZZ_TAB_AVOID)
	{
		const float LineSize = 20.0f;
		CUIRect Left, Right;
		MainView.VSplitMid(&Left, &Right, 20.0f);

		{
			CUIRect Inner;
			BeginCard(Left, 191.0f, Inner);
			DoPanelHeader(Inner, Localize("Avoid"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAvoidEnabled, Localize("Enable avoid"), &g_Config.m_ClZzAvoidEnabled, &Inner, LineSize);
			static CUi::SDropDownState s_AvoidModeState;
			static CScrollRegion s_AvoidModeScroll;
			s_AvoidModeState.m_SelectionPopupContext.m_pScrollRegion = &s_AvoidModeScroll;
			const char *apAvoidModes[] = {Localize("Classic"), Localize("Blatant")};
			Inner.HSplitTop(LineSize, &Button, &Inner);
			g_Config.m_ClZzAvoidMode = Ui()->DoDropDown(&Button, g_Config.m_ClZzAvoidMode, apAvoidModes, 2, s_AvoidModeState);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Choose avoid logic"), 14.0f, TEXTALIGN_ML);
			if(g_Config.m_ClZzAvoidMode == 1)
			{
				Inner.HSplitTop(LineSize, &Button, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAvoidPredTicks, &g_Config.m_ClZzAvoidPredTicks, &Button, Localize("Blatant pred ticks"), 1, 30);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAvoidJump, Localize("Blatant jump"), &g_Config.m_ClZzAvoidJump, &Inner, LineSize);
				Inner.HSplitTop(LineSize * 2.0f, &Label, &Inner);
			}
		}

		{
			CUIRect Inner;
			BeginCard(Right, 141.0f, Inner);
			DoPanelHeader(Inner, Localize("Bind"));
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Avoid key"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CButtonContainer s_AvoidKeyReader;
			static CButtonContainer s_AvoidKeyClear;
			CBindSlot CurrentBindAvoid((int)g_Config.m_ClZzAvoidToggleKey, (int)g_Config.m_ClZzAvoidToggleMod);
			const auto ResAvoid = GameClient()->m_KeyBinder.DoKeyReader(&s_AvoidKeyReader, &s_AvoidKeyClear, &Button, CurrentBindAvoid, false);
			if(!ResAvoid.m_Aborted && (ResAvoid.m_Bind.m_Key != CurrentBindAvoid.m_Key || ResAvoid.m_Bind.m_ModifierMask != CurrentBindAvoid.m_ModifierMask))
			{
				if(CurrentBindAvoid.m_Key >= KEY_FIRST && CurrentBindAvoid.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(CurrentBindAvoid.m_Key, "", false, CurrentBindAvoid.m_ModifierMask);
				g_Config.m_ClZzAvoidToggleKey = ResAvoid.m_Bind.m_Key;
				g_Config.m_ClZzAvoidToggleMod = ResAvoid.m_Bind.m_ModifierMask;
				if(ResAvoid.m_Bind.m_Key >= KEY_FIRST && ResAvoid.m_Bind.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(ResAvoid.m_Bind.m_Key, "+zz_avoid_toggle", false, ResAvoid.m_Bind.m_ModifierMask);
			}
			Inner.HSplitTop(8.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize * 1.8f, &Label, &Inner);
		}
	}
	else if(s_SubTab == ZZ_TAB_MISC)
	{
		const float LineSize = 20.0f;
		static CScrollRegion s_MiscScrollRegion;
		static vec2 s_MiscScrollOffset(0.0f, 0.0f);
		CScrollRegionParams ScrollParams;
		ScrollParams.m_ScrollUnit = 28.0f;
		s_MiscScrollRegion.Begin(&MainView, &s_MiscScrollOffset, &ScrollParams);
		CUIRect ScrollView = MainView;
		ScrollView.y += s_MiscScrollOffset.y;

		CUIRect LeftView, RightView;
		ScrollView.VSplitMid(&LeftView, &RightView, 20.0f);
		pFeatureScrollRegion = &s_MiscScrollRegion;

		// Left: independent misc features
		{
			CUIRect Inner;
			BeginCard(LeftView, 98.0f, Inner);
			DoPanelHeader(Inner, Localize("Fast weapons"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFastHammer, Localize("Fast hammer"), &g_Config.m_ClZzFastHammer, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFastGun, Localize("Fast gun"), &g_Config.m_ClZzFastGun, &Inner, LineSize);
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize * 1.6f, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("При зажатом fire: 1 тик нажат, 1 тик отпущен (по текущему оружию)."), 14.0f, TEXTALIGN_ML);
		}
		{
			CUIRect Inner;
			BeginCard(LeftView, 78.0f, Inner);
			DoPanelHeader(Inner, Localize("Auto aled"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAutoAled, Localize("Auto aled enabled?"), &g_Config.m_ClZzAutoAled, &Inner, LineSize);
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize * 1.6f, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Автоматически бьет хаммером, когда нужно сделать Aled."), 14.0f, TEXTALIGN_ML);
		}
		{
			CUIRect Inner;
			BeginCard(LeftView, 198.0f, Inner);
			DoPanelHeader(Inner, Localize("Auto unfreeze"));
			if(DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAutoUnfreeze, Localize("Auto-unfreeze"), &g_Config.m_ClZzAutoUnfreeze, &Inner, LineSize))
			{
				if(g_Config.m_ClZzAutoUnfreeze)
					g_Config.m_ClZzAutoUnfreezeAutoFire = 1;
			}
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Ui()->DoScrollbarOption(&g_Config.m_ClZzAutoUnfreezeFov, &g_Config.m_ClZzAutoUnfreezeFov, &Button, Localize("FOV"), 1, 360);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAutoUnfreezeAutoFire, Localize("Auto-fire"), &g_Config.m_ClZzAutoUnfreezeAutoFire, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzDummyAutoUnfreeze, Localize("Dummy auto-unfreeze"), &g_Config.m_ClZzDummyAutoUnfreeze, &Inner, LineSize);
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Auto-unfreeze bind"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			constexpr const char *AutoUnfreezeToggleCommand = "toggle zz_auto_unfreeze 0 1";
			CBindSlot AutoUnfreezeBind = FindBindSlotForCommand(&GameClient()->m_Binds, AutoUnfreezeToggleCommand);
			static CButtonContainer s_AutoUnfreezeBindButton;
			static CButtonContainer s_AutoUnfreezeBindClearButton;
			const CKeyBinder::CKeyReaderResult AutoUnfreezeBindResult = GameClient()->m_KeyBinder.DoKeyReader(
				&s_AutoUnfreezeBindButton, &s_AutoUnfreezeBindClearButton, &Button, AutoUnfreezeBind, false);
			if(!AutoUnfreezeBindResult.m_Aborted && AutoUnfreezeBindResult.m_Bind != AutoUnfreezeBind)
			{
				UnbindCommand(&GameClient()->m_Binds, AutoUnfreezeToggleCommand);
				if(AutoUnfreezeBindResult.m_Bind.m_Key != KEY_UNKNOWN)
					GameClient()->m_Binds.Bind(AutoUnfreezeBindResult.m_Bind.m_Key, AutoUnfreezeToggleCommand, false, AutoUnfreezeBindResult.m_Bind.m_ModifierMask);
			}
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize * 2.1f, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Автоматически делает unfreeze себя с лазером."), 14.0f, TEXTALIGN_ML);
		}
		{
			CUIRect Inner;
			BeginCard(LeftView, 122.0f, Inner);
			DoPanelHeader(Inner, Localize("Balance bot"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzBalanceBot, Localize("Balance bot"), &g_Config.m_ClZzBalanceBot, &Inner, LineSize);
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Balance bot keybind"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CButtonContainer s_BalanceBotKeyReader;
			static CButtonContainer s_BalanceBotKeyClear;
			CBindSlot CurrentBindBalance((int)g_Config.m_ClZzBalanceBotKey, (int)g_Config.m_ClZzBalanceBotMod);
			const auto ResBalance = GameClient()->m_KeyBinder.DoKeyReader(&s_BalanceBotKeyReader, &s_BalanceBotKeyClear, &Button, CurrentBindBalance, false);
			if(!ResBalance.m_Aborted && (ResBalance.m_Bind.m_Key != CurrentBindBalance.m_Key || ResBalance.m_Bind.m_ModifierMask != CurrentBindBalance.m_ModifierMask))
			{
				if(CurrentBindBalance.m_Key >= KEY_FIRST && CurrentBindBalance.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(CurrentBindBalance.m_Key, "", false, CurrentBindBalance.m_ModifierMask);
				g_Config.m_ClZzBalanceBotKey = ResBalance.m_Bind.m_Key;
				g_Config.m_ClZzBalanceBotMod = ResBalance.m_Bind.m_ModifierMask;
				if(ResBalance.m_Bind.m_Key >= KEY_FIRST && ResBalance.m_Bind.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(ResBalance.m_Bind.m_Key, "+zz_balance_bot_toggle", false, ResBalance.m_Bind.m_ModifierMask);
			}
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize * 1.6f, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Балансирует на голове таргета."), 14.0f, TEXTALIGN_ML);
		}
		{
			CUIRect Inner;
			BeginCard(LeftView, 340.0f, Inner);
			DoPanelHeader(Inner, Localize("Companion"));
			if(DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzCompanionEnabled, Localize("Companion"), &g_Config.m_ClZzCompanionEnabled, &Inner, LineSize))
			{
				if(g_Config.m_ClZzCompanionEnabled)
					g_Config.m_ClZzCompanionPyInfer = 0;
			}
			static CUi::SDropDownState s_CompanionModeState;
			static CScrollRegion s_CompanionModeScroll;
			s_CompanionModeState.m_SelectionPopupContext.m_pScrollRegion = &s_CompanionModeScroll;
			const char *apCompanionModes[] = {Localize("PvP"), Localize("Gores")};
			Inner.HSplitTop(LineSize, &Button, &Inner);
			g_Config.m_ClZzCompanionMode = Ui()->DoDropDown(&Button, g_Config.m_ClZzCompanionMode, apCompanionModes, 2, s_CompanionModeState);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Choose companion mode"), 14.0f, TEXTALIGN_ML);

			if(g_Config.m_ClZzCompanionMode == 0)
			{
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzCompanionFightMode, Localize("Агрессивный мод"), &g_Config.m_ClZzCompanionFightMode, &Inner, LineSize);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzCompanionNoAggroTeam, Localize("Не бить тиммейтов и спасать их"), &g_Config.m_ClZzCompanionNoAggroTeam, &Inner, LineSize);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzCompanionHitOnlyWars, Localize("Бить только варов"), &g_Config.m_ClZzCompanionHitOnlyWars, &Inner, LineSize);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzCompanionRescueOnlyFrozen, Localize("Спасать только после фриза"), &g_Config.m_ClZzCompanionRescueOnlyFrozen, &Inner, LineSize);
				Inner.HSplitTop(4.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Button, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzCompanionFollowTiles, &g_Config.m_ClZzCompanionFollowTiles, &Button, Localize("Follow tiles"), 5, 200);
				Inner.HSplitTop(LineSize, &Button, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzCompanionSpacingTiles, &g_Config.m_ClZzCompanionSpacingTiles, &Button, Localize("Spacing (tiles)"), 1, 20);
			}
			else
			{
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzCompanionGoresRescueAll, Localize("Спасать всех"), &g_Config.m_ClZzCompanionGoresRescueAll, &Inner, LineSize);
				Inner.HSplitTop(4.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Button, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzCompanionGoresMinTiles, &g_Config.m_ClZzCompanionGoresMinTiles, &Button, Localize("Min distance (tiles)"), 3, 20);
				Inner.HSplitTop(LineSize, &Button, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzCompanionGoresMaxTiles, &g_Config.m_ClZzCompanionGoresMaxTiles, &Button, Localize("Max distance (tiles)"), 4, 30);
				if(g_Config.m_ClZzCompanionGoresMaxTiles < g_Config.m_ClZzCompanionGoresMinTiles)
					g_Config.m_ClZzCompanionGoresMaxTiles = g_Config.m_ClZzCompanionGoresMinTiles;
			}

			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(6.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Companion toggle keybind"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CButtonContainer s_CompanionKeyReader;
			static CButtonContainer s_CompanionKeyClear;
			CBindSlot CurrentBindCompanion((int)g_Config.m_ClZzCompanionToggleKey, (int)g_Config.m_ClZzCompanionToggleMod);
			const auto ResCompanion = GameClient()->m_KeyBinder.DoKeyReader(&s_CompanionKeyReader, &s_CompanionKeyClear, &Button, CurrentBindCompanion, false);
			if(!ResCompanion.m_Aborted && (ResCompanion.m_Bind.m_Key != CurrentBindCompanion.m_Key || ResCompanion.m_Bind.m_ModifierMask != CurrentBindCompanion.m_ModifierMask))
			{
				constexpr const char *CompanionToggleCommand = "+zz_companion_toggle";
				UnbindCommand(&GameClient()->m_Binds, CompanionToggleCommand);
				g_Config.m_ClZzCompanionToggleKey = ResCompanion.m_Bind.m_Key;
				g_Config.m_ClZzCompanionToggleMod = ResCompanion.m_Bind.m_ModifierMask;
				if(ResCompanion.m_Bind.m_Key >= KEY_FIRST && ResCompanion.m_Bind.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(ResCompanion.m_Bind.m_Key, CompanionToggleCommand, false, ResCompanion.m_Bind.m_ModifierMask);
			}
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize * 1.6f, &Label, &Inner);
			if(g_Config.m_ClZzCompanionMode == 0)
				Ui()->DoLabel(&Label, Localize("PvP: самостоятельно бьет игроков и спасает вас."), 14.0f, TEXTALIGN_ML);
			else
				Ui()->DoLabel(&Label, Localize("Gores: держит 4-12 тайлов, спасает игроков и проходит узкие туннели."), 14.0f, TEXTALIGN_ML);
		}
		{
			CUIRect Inner;
			BeginCard(LeftView, 160.0f, Inner);
			DoPanelHeader(Inner, Localize("Fake lag"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFakeLag, Localize("Fake lag"), &g_Config.m_ClZzFakeLag, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFakeLagDrift, Localize("Drift mode"), &g_Config.m_ClZzFakeLagDrift, &Inner, LineSize);
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Fake lag keybind"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CButtonContainer s_FakeLagKeyReader;
			static CButtonContainer s_FakeLagKeyClear;
			CBindSlot CurrentBindFakeLag((int)g_Config.m_ClZzFakeLagKey, (int)g_Config.m_ClZzFakeLagMod);
			const auto ResFakeLag = GameClient()->m_KeyBinder.DoKeyReader(&s_FakeLagKeyReader, &s_FakeLagKeyClear, &Button, CurrentBindFakeLag, false);
			if(!ResFakeLag.m_Aborted && (ResFakeLag.m_Bind.m_Key != CurrentBindFakeLag.m_Key || ResFakeLag.m_Bind.m_ModifierMask != CurrentBindFakeLag.m_ModifierMask))
			{
				if(CurrentBindFakeLag.m_Key >= KEY_FIRST && CurrentBindFakeLag.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(CurrentBindFakeLag.m_Key, "", false, CurrentBindFakeLag.m_ModifierMask);
				g_Config.m_ClZzFakeLagKey = ResFakeLag.m_Bind.m_Key;
				g_Config.m_ClZzFakeLagMod = ResFakeLag.m_Bind.m_ModifierMask;
				if(ResFakeLag.m_Bind.m_Key >= KEY_FIRST && ResFakeLag.m_Bind.m_Key < KEY_LAST)
					GameClient()->m_Binds.Bind(ResFakeLag.m_Bind.m_Key, "+zz_fake_lag_toggle", false, ResFakeLag.m_Bind.m_ModifierMask);
			}
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize * 2.0f, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Создает лаги для игроков с antiping через переключение A/D."), 14.0f, TEXTALIGN_ML);
		}
		{
			CUIRect Inner;
			BeginCard(RightView, 192.0f, Inner);
			DoPanelHeader(Inner, Localize("Fake aim"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFakeAimEnabled, Localize("Fake aim"), &g_Config.m_ClZzFakeAimEnabled, &Inner, LineSize);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Ui()->DoScrollbarOption(&g_Config.m_ClZzFakeAimMode, &g_Config.m_ClZzFakeAimMode, &Button, Localize("Fake aim mode"), 1, 3);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("1 = spin, 2 = robot, 3 = random"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Ui()->DoScrollbarOption(&g_Config.m_ClZzFakeAimSpinSpeed, &g_Config.m_ClZzFakeAimSpinSpeed, &Button, Localize("Spin speed"), 1, 7200);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFakeAimShowAim, Localize("Show aim"), &g_Config.m_ClZzFakeAimShowAim, &Inner, LineSize);
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize * 2.0f, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Replaces real aim with fake aim."), 14.0f, TEXTALIGN_ML);
		}

			{
				CUIRect Inner;
				BeginCard(RightView, 76.0f, Inner);
				DoPanelHeader(Inner, Localize("Force zoom"));
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzForceZoom, Localize("Force zoom on restricted servers"), &g_Config.m_ClZzForceZoom, &Inner, LineSize);
				Inner.HSplitTop(4.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, Localize("Allows changing zoom even when server blocks zoom."), 14.0f, TEXTALIGN_ML);
			}
			{
				CUIRect Inner;
				BeginCard(RightView, 76.0f, Inner);
				DoPanelHeader(Inner, Localize("Async Proxy Bots"));
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, Localize("Moved to Others -> Bots."), 14.0f, TEXTALIGN_ML);
			}
			EndCard();
			pFeatureScrollRegion = nullptr;
			s_MiscScrollRegion.End();
		}
	else if(s_SubTab == ZZ_TAB_FLY)
	{
		const float LineSize = 20.0f;
		CUIRect Inner;
		BeginCard(MainView, 110.0f, Inner);
		DoPanelHeader(Inner, Localize("Fly Helper"));
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFlyHelperEnabled, Localize("Enable auto-pseudo"), &g_Config.m_ClZzFlyHelperEnabled, &Inner, LineSize);
		Inner.HSplitTop(10.0f, nullptr, &Inner);
		Inner.HSplitTop(LineSize, &Label, &Inner);
		Ui()->DoLabel(&Label, Localize("Requires pseudo fly bind."), 14.0f, TEXTALIGN_ML);

		BeginCard(MainView, 188.0f, Inner);
		DoPanelHeader(Inner, Localize("Auto Triplefly"));
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFlyAutoTriplefly, Localize("Auto triplefly (copy moves)"), &g_Config.m_ClZzFlyAutoTriplefly, &Inner, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFlyAutoTripleflySmartHook, Localize("Smart hook"), &g_Config.m_ClZzFlyAutoTripleflySmartHook, &Inner, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFlyAutoTripleflyNoTargetNoHook, Localize("No target -> no hook"), &g_Config.m_ClZzFlyAutoTripleflyNoTargetNoHook, &Inner, LineSize);
		Inner.HSplitTop(LineSize, &Button, &Inner);
		Ui()->DoScrollbarOption(&g_Config.m_ClZzFlyAutoTripleflyFov, &g_Config.m_ClZzFlyAutoTripleflyFov, &Button, Localize("Triplefly FOV"), 5, 180);
		Inner.HSplitTop(4.0f, nullptr, &Inner);
		Inner.HSplitTop(LineSize, &Label, &Inner);
		Ui()->DoLabel(&Label, Localize("Auto triplefly keybind"), 14.0f, TEXTALIGN_ML);
		Inner.HSplitTop(LineSize, &Button, &Inner);
		static CButtonContainer s_FlyAutoTripleKeyReader;
		static CButtonContainer s_FlyAutoTripleKeyClear;
		CBindSlot CurrentBindFlyTriple((int)g_Config.m_ClZzFlyAutoTripleflyToggleKey, (int)g_Config.m_ClZzFlyAutoTripleflyToggleMod);
		const auto ResFlyTriple = GameClient()->m_KeyBinder.DoKeyReader(&s_FlyAutoTripleKeyReader, &s_FlyAutoTripleKeyClear, &Button, CurrentBindFlyTriple, false);
		if(!ResFlyTriple.m_Aborted && (ResFlyTriple.m_Bind.m_Key != CurrentBindFlyTriple.m_Key || ResFlyTriple.m_Bind.m_ModifierMask != CurrentBindFlyTriple.m_ModifierMask))
		{
			if(CurrentBindFlyTriple.m_Key >= KEY_FIRST && CurrentBindFlyTriple.m_Key < KEY_LAST)
				GameClient()->m_Binds.Bind(CurrentBindFlyTriple.m_Key, "", false, CurrentBindFlyTriple.m_ModifierMask);
			g_Config.m_ClZzFlyAutoTripleflyToggleKey = ResFlyTriple.m_Bind.m_Key;
			g_Config.m_ClZzFlyAutoTripleflyToggleMod = ResFlyTriple.m_Bind.m_ModifierMask;
			if(ResFlyTriple.m_Bind.m_Key >= KEY_FIRST && ResFlyTriple.m_Bind.m_Key < KEY_LAST)
				GameClient()->m_Binds.Bind(ResFlyTriple.m_Bind.m_Key, "+zz_fly_auto_triplefly_toggle", false, ResFlyTriple.m_Bind.m_ModifierMask);
		}
		Inner.HSplitTop(10.0f, nullptr, &Inner);
		Inner.HSplitTop(LineSize * 2.0f, &Label, &Inner);
		Ui()->DoLabel(&Label, Localize("When you hold hook with copy moves, dummy auto-aims nearest target in FOV."), 14.0f, TEXTALIGN_ML);
	}
	else if(s_SubTab == 99)
	{
		const float LineSize = 20.0f;
		const float MarginSmall = 5.0f;
		CUIRect Left, Right;
		MainView.VSplitMid(&Left, &Right, 20.0f);

		// Left: quick mode + KoG policy behavior.
		{
			CUIRect Inner;
			BeginCard(Left, 290.0f, Inner);
			int DerivedAiQuickMode = 0;
			if(g_Config.m_ClZzKogAiEnabled)
				DerivedAiQuickMode = 5;
			else if(g_Config.m_ClZzCompanionEnabled)
				DerivedAiQuickMode = 4;
			else if(g_Config.m_ClZzFentBotEnabled && g_Config.m_ClZzFentBotAiInfer)
				DerivedAiQuickMode = 3;
			else if(g_Config.m_ClZzFentBotEnabled && g_Config.m_ClZzFentBotPyInfer)
				DerivedAiQuickMode = 2;
			else if(g_Config.m_ClZzFentBotEnabled)
				DerivedAiQuickMode = 1;

			static int s_AiQuickModeUiAi = 0;
			if(!Ui()->MouseButton(0))
				s_AiQuickModeUiAi = DerivedAiQuickMode;
			Inner.HSplitTop(LineSize, &Button, &Inner);
			if(Ui()->DoScrollbarOption(&s_AiQuickModeUiAi, &s_AiQuickModeUiAi, &Button, Localize("AI quick mode"), 0, 5))
			{
				char aCmd[64];
				str_format(aCmd, sizeof(aCmd), "zz_ai_mode %d", s_AiQuickModeUiAi);
				Console()->ExecuteLine(aCmd, IConsole::CLIENT_ID_UNSPECIFIED);
			}
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("0=off 1=fentbot 2=python 3=bc 4=companion 5=kog"), 14.0f, TEXTALIGN_ML);

			Inner.HSplitTop(8.0f, nullptr, &Inner);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzKogAiEnabled, Localize("KoG AI enabled?"), &g_Config.m_ClZzKogAiEnabled, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzKogAiLearning, Localize("Online learning"), &g_Config.m_ClZzKogAiLearning, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzKogAiLog, Localize("Verbose logs"), &g_Config.m_ClZzKogAiLog, &Inner, LineSize);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Ui()->DoScrollbarOption(&g_Config.m_ClZzKogAiDifficulty, &g_Config.m_ClZzKogAiDifficulty, &Button, Localize("Difficulty"), 1, 3);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Ui()->DoScrollbarOption(&g_Config.m_ClZzKogAiReactionTime, &g_Config.m_ClZzKogAiReactionTime, &Button, Localize("Reaction ms"), 20, 500);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Ui()->DoScrollbarOption(&g_Config.m_ClZzKogAiAggression, &g_Config.m_ClZzKogAiAggression, &Button, Localize("Aggression"), 0, 100);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Ui()->DoScrollbarOption(&g_Config.m_ClZzKogAiStyle, &g_Config.m_ClZzKogAiStyle, &Button, Localize("Style"), 0, 2);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Style: 0=aggressive 1=careful 2=balanced"), 14.0f, TEXTALIGN_ML);
		}

		// Right: model paths + commands.
		{
			CUIRect Inner;
			BeginCard(Right, 290.0f, Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("KoG model file (relative to save dir)"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CLineInput s_KogModelPath;
			s_KogModelPath.SetBuffer(g_Config.m_ClZzKogAiModel, sizeof(g_Config.m_ClZzKogAiModel));
			s_KogModelPath.SetEmptyText("kog_ai/model.bin");
			Ui()->DoEditBox(&s_KogModelPath, &Button, 12.0f);

			Inner.HSplitTop(8.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("FentBot model file (relative to save dir)"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CLineInput s_FentModelPathAi;
			s_FentModelPathAi.SetBuffer(g_Config.m_ClZzFentBotAiModel, sizeof(g_Config.m_ClZzFentBotAiModel));
			s_FentModelPathAi.SetEmptyText("fentbot/model.bin");
			Ui()->DoEditBox(&s_FentModelPathAi, &Button, 12.0f);

			Inner.HSplitTop(8.0f, nullptr, &Inner);
			CUIRect Row;
			Inner.HSplitTop(LineSize, &Row, &Inner);
			static CButtonContainer s_KogStatus;
			if(DoButtonLineSize_Menu(&s_KogStatus, Localize("KoG: status"), 0, &Row, LineSize, false, 0, IGraphics::CORNER_ALL, 5.0f, 0.0f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f)))
				Console()->ExecuteLine("zz_kog_ai_status", IConsole::CLIENT_ID_UNSPECIFIED);

			Inner.HSplitTop(MarginSmall, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Row, &Inner);
			static CButtonContainer s_KogLoad;
			if(DoButtonLineSize_Menu(&s_KogLoad, Localize("KoG: load model"), 0, &Row, LineSize, false, 0, IGraphics::CORNER_ALL, 5.0f, 0.0f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f)))
			{
				char aCmd[512];
				str_format(aCmd, sizeof(aCmd), "zz_kog_ai_load_model %s", g_Config.m_ClZzKogAiModel);
				Console()->ExecuteLine(aCmd, IConsole::CLIENT_ID_UNSPECIFIED);
			}

			Inner.HSplitTop(MarginSmall, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Row, &Inner);
			static CButtonContainer s_KogSave;
			if(DoButtonLineSize_Menu(&s_KogSave, Localize("KoG: save model"), 0, &Row, LineSize, false, 0, IGraphics::CORNER_ALL, 5.0f, 0.0f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f)))
			{
				char aCmd[512];
				str_format(aCmd, sizeof(aCmd), "zz_kog_ai_save_model %s", g_Config.m_ClZzKogAiModel);
				Console()->ExecuteLine(aCmd, IConsole::CLIENT_ID_UNSPECIFIED);
			}

			Inner.HSplitTop(MarginSmall, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Row, &Inner);
			static CButtonContainer s_KogExport;
			if(DoButtonLineSize_Menu(&s_KogExport, Localize("KoG: export dataset"), 0, &Row, LineSize, false, 0, IGraphics::CORNER_ALL, 5.0f, 0.0f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f)))
				Console()->ExecuteLine("zz_kog_ai_export_dataset kog_ai/dataset.bin", IConsole::CLIENT_ID_UNSPECIFIED);

			Inner.HSplitTop(MarginSmall, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Row, &Inner);
			static CButtonContainer s_KogTrainOffline;
			if(DoButtonLineSize_Menu(&s_KogTrainOffline, Localize("KoG: train offline"), 0, &Row, LineSize, false, 0, IGraphics::CORNER_ALL, 5.0f, 0.0f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f)))
				Console()->ExecuteLine("zz_kog_ai_train_offline kog_ai/dataset.bin 12", IConsole::CLIENT_ID_UNSPECIFIED);

			Inner.HSplitTop(MarginSmall, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Row, &Inner);
			static CButtonContainer s_FentLoadAi;
			if(DoButtonLineSize_Menu(&s_FentLoadAi, Localize("FentBot: load model"), 0, &Row, LineSize, false, 0, IGraphics::CORNER_ALL, 5.0f, 0.0f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f)))
			{
				char aCmd[512];
				str_format(aCmd, sizeof(aCmd), "zz_fentbot_load_model %s", g_Config.m_ClZzFentBotAiModel);
				Console()->ExecuteLine(aCmd, IConsole::CLIENT_ID_UNSPECIFIED);
			}
		}
	}
	else if(s_SubTab == ZZ_TAB_FNG)
	{
		const float LineSize = 20.0f;
		CUIRect Inner;
		BeginCard(MainView, 216.0f, Inner);
		DoPanelHeader(Inner, Localize("FNG"));
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFngTriggerLaser, Localize("Laser triggerbot"), &g_Config.m_ClZzFngTriggerLaser, &Inner, LineSize);
		Inner.HSplitTop(5.0f, nullptr, &Inner);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFngAutoLaser, Localize("Auto-laser (360 scan)"), &g_Config.m_ClZzFngAutoLaser, &Inner, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFngAutoLaserBounce, Localize("Allow bounce"), &g_Config.m_ClZzFngAutoLaserBounce, &Inner, LineSize);
		Inner.HSplitTop(LineSize, &Button, &Inner);
		Ui()->DoScrollbarOption(&g_Config.m_ClZzFngAutoLaserRaysPerTick, &g_Config.m_ClZzFngAutoLaserRaysPerTick, &Button, Localize("Rays per tick"), 1, 64);
		Inner.HSplitTop(LineSize, &Button, &Inner);
		Ui()->DoScrollbarOption(&g_Config.m_ClZzFngAutoLaserMaxBounces, &g_Config.m_ClZzFngAutoLaserMaxBounces, &Button, Localize("Max bounces"), 0, 8);
		Inner.HSplitTop(5.0f, nullptr, &Inner);
		Inner.HSplitTop(LineSize, &Label, &Inner);
		Ui()->DoLabel(&Label, Localize("Scans around and fires if it can hit enemy."), 14.0f, TEXTALIGN_ML);
	}
	else if(s_SubTab == ZZ_TAB_VISUALS)
	{
		const float LineSize = 20.0f;
		static CScrollRegion s_VisualsScrollRegion;
		static vec2 s_VisualsScrollOffset(0.0f, 0.0f);
		CScrollRegionParams VisualsScrollParams;
		VisualsScrollParams.m_ScrollUnit = 28.0f;
		s_VisualsScrollRegion.Begin(&MainView, &s_VisualsScrollOffset, &VisualsScrollParams);
		CUIRect ScrollView = MainView;
		ScrollView.y += s_VisualsScrollOffset.y;
		pFeatureScrollRegion = &s_VisualsScrollRegion;

		CUIRect Left, Right;
		ScrollView.VSplitMid(&Left, &Right, 20.0f);
		static CButtonContainer s_IslandColorResetId;
		static CButtonContainer s_WatermarkColorResetId;
		CUIRect Inner;
		BeginCard(Left, 350.0f, Inner);
		DoPanelHeader(Inner, Localize("Visuals"));
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_BcAnimations, Localize("UI reveal animations"), &g_Config.m_BcAnimations, &Inner, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(
			&g_Config.m_ClZzWatermarkEnabled,
			Localize("Show ZZClient watermark"),
			&g_Config.m_ClZzWatermarkEnabled,
			&Inner,
			LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(
			&g_Config.m_ClZzIslandRainbowEnabled,
			Localize("Rainbow island border"),
			&g_Config.m_ClZzIslandRainbowEnabled,
			&Inner,
			LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(
			&g_Config.m_ClZzWatermarkRainbowEnabled,
			Localize("Rainbow watermark border"),
			&g_Config.m_ClZzWatermarkRainbowEnabled,
			&Inner,
			LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(
			&g_Config.m_ClZzIslandColorApplyWhenPlayerActive,
			Localize("Force island color with player"),
			&g_Config.m_ClZzIslandColorApplyWhenPlayerActive,
			&Inner,
			LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(
			&g_Config.m_ClZzWatermarkColorApplyWhenPlayerActive,
			Localize("Force watermark color with player"),
			&g_Config.m_ClZzWatermarkColorApplyWhenPlayerActive,
			&Inner,
			LineSize);
		Inner.HSplitTop(LineSize, &Button, &Inner);
		static CButtonContainer s_HudEditModeButton;
		if(DoButton_Menu(
			   &s_HudEditModeButton,
			   g_Config.m_ClZzHudEditMode ? Localize("Exit HUD edit mode (save)") : Localize("Enter HUD edit mode"),
			   0,
			   &Button,
			   BUTTONFLAG_LEFT,
			   nullptr,
			   IGraphics::CORNER_ALL,
			   5.0f,
			   0.0f,
			   g_Config.m_ClZzHudEditMode ? ColorRGBA(0.43f, 0.24f, 0.23f, 0.93f) : ColorRGBA(0.15f, 0.31f, 0.46f, 0.93f)))
		{
			g_Config.m_ClZzHudEditMode ^= 1;
			if(!g_Config.m_ClZzHudEditMode)
				(void)ConfigManager()->Save();
		}
		Inner.HSplitTop(4.0f, nullptr, &Inner);
		Inner.HSplitTop(LineSize, &Label, &Inner);
		Ui()->DoLabel(&Label, Localize("In edit mode: drag widgets, ESC exits and saves."), 13.0f, TEXTALIGN_ML);
		Inner.HSplitTop(4.0f, nullptr, &Inner);
		Inner.HSplitTop(LineSize, &Button, &Inner);
		static CButtonContainer s_ResetHudLayoutButton;
		if(DoButton_Menu(&s_ResetHudLayoutButton, Localize("Reset HUD layout"), 0, &Button))
		{
			g_Config.m_ClZzHudArraylistX = 980;
			g_Config.m_ClZzHudArraylistY = 500;
			g_Config.m_ClZzHudNotifyX = 980;
			g_Config.m_ClZzHudNotifyY = 80;
			g_Config.m_ClZzHudIslandX = 500;
			g_Config.m_ClZzHudIslandY = 20;
			g_Config.m_ClZzHudWatermarkX = 980;
			g_Config.m_ClZzHudWatermarkY = 20;
		}
		DoLine_ColorPicker(
			&s_IslandColorResetId,
			25.0f,
			13.0f,
			4.0f,
			&Inner,
			Localize("Island color"),
			&g_Config.m_ClZzIslandColor,
			ColorRGBA(0.32f, 0.72f, 1.0f, 1.0f),
			false);
		DoLine_ColorPicker(
			&s_WatermarkColorResetId,
			25.0f,
			13.0f,
			4.0f,
			&Inner,
			Localize("Watermark color"),
			&g_Config.m_ClZzWatermarkColor,
			ColorRGBA(0.32f, 0.72f, 1.0f, 1.0f),
			false);

		static float s_JellyPhase = 0.0f;
		static float s_3dPhase = 0.0f;
		static float s_MagicPhase = 0.0f;
		static float s_OrbitPhase = 0.0f;
		static float s_HammerHitParticlesPhase = 0.0f;
		static float s_CameraDriftPhase = 0.0f;
		static float s_DynamicFovPhase = 0.0f;

		const bool JellyEnabled = g_Config.m_ClZzJellyTee != 0;
		const bool Particles3dEnabled = g_Config.m_ClZz3dParticles != 0;
		const bool MagicEnabled = g_Config.m_ClZzMagicParticles != 0;
		const bool OrbitEnabled = g_Config.m_ClZzOrbitAura != 0;
		const bool HammerHitParticlesEnabled = g_Config.m_ClZzHammerHitParticles != 0;
		const bool CameraDriftEnabled = g_Config.m_ClZzCameraDrift != 0;
		const bool DynamicFovEnabled = g_Config.m_ClZzDynamicFov != 0;
		UpdateRevealPhase(s_JellyPhase, JellyEnabled);
		UpdateRevealPhase(s_3dPhase, Particles3dEnabled);
		UpdateRevealPhase(s_MagicPhase, MagicEnabled);
		UpdateRevealPhase(s_OrbitPhase, OrbitEnabled);
		UpdateRevealPhase(s_HammerHitParticlesPhase, HammerHitParticlesEnabled);
		UpdateRevealPhase(s_CameraDriftPhase, CameraDriftEnabled);
		UpdateRevealPhase(s_DynamicFovPhase, DynamicFovEnabled);

		{
			CUIRect Expand;
			BeginCard(Right, 88.0f + 102.0f * s_CameraDriftPhase, Inner);
			DoPanelHeader(Inner, Localize("Camera Drift"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzCameraDrift, Localize("Enable camera drift"), &g_Config.m_ClZzCameraDrift, &Inner, LineSize);
			const float Extra = 102.0f * s_CameraDriftPhase;
			if(Extra > 0.0f)
			{
				Inner.HSplitTop(Extra, &Expand, &Inner);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzCameraDriftAmount, &g_Config.m_ClZzCameraDriftAmount, &Button, Localize("Camera drift amount"), 1, 200);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzCameraDriftSmoothness, &g_Config.m_ClZzCameraDriftSmoothness, &Button, Localize("Camera drift smoothness"), 1, 20);

				CUIRect DirectionRow;
				Expand.HSplitTop(LineSize, &DirectionRow, &Expand);
				CUIRect DirectionForward;
				CUIRect DirectionBackward;
				DirectionRow.VSplitMid(&DirectionForward, &DirectionBackward, 5.0f);
				static CButtonContainer s_CameraDriftForwardButton;
				static CButtonContainer s_CameraDriftBackwardButton;
				if(DoButton_CheckBox(&s_CameraDriftForwardButton, Localize("Forward"), !g_Config.m_ClZzCameraDriftReverse, &DirectionForward))
					g_Config.m_ClZzCameraDriftReverse = 0;
				if(DoButton_CheckBox(&s_CameraDriftBackwardButton, Localize("Backward"), g_Config.m_ClZzCameraDriftReverse, &DirectionBackward))
					g_Config.m_ClZzCameraDriftReverse = 1;
			}
		}

		{
			CUIRect Expand;
			BeginCard(Right, 88.0f + 64.0f * s_DynamicFovPhase, Inner);
			DoPanelHeader(Inner, Localize("Dynamic FOV"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzDynamicFov, Localize("Enable dynamic FOV"), &g_Config.m_ClZzDynamicFov, &Inner, LineSize);
			const float Extra = 64.0f * s_DynamicFovPhase;
			if(Extra > 0.0f)
			{
				Inner.HSplitTop(Extra, &Expand, &Inner);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzDynamicFovAmount, &g_Config.m_ClZzDynamicFovAmount, &Button, Localize("Dynamic FOV amount"), 1, 200);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzDynamicFovSmoothness, &g_Config.m_ClZzDynamicFovSmoothness, &Button, Localize("Dynamic FOV smoothness"), 1, 20);
			}
		}

		{
			CUIRect Expand;
			BeginCard(Right, 88.0f + 112.0f * s_JellyPhase, Inner);
			DoPanelHeader(Inner, Localize("Jelly Tee"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzJellyTee, Localize("Enable jelly tee"), &g_Config.m_ClZzJellyTee, &Inner, LineSize);
			const float Extra = 112.0f * s_JellyPhase;
			if(Extra > 0.0f)
			{
				Inner.HSplitTop(Extra, &Expand, &Inner);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzJellyTeeOthers, Localize("Apply to others"), &g_Config.m_ClZzJellyTeeOthers, &Expand, LineSize);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzJellyTeeSuper, Localize("Super jelly tee"), &g_Config.m_ClZzJellyTeeSuper, &Expand, LineSize);
				if(g_Config.m_ClZzJellyTeeSuper)
				{
					static CUi::SDropDownState s_JellySuperModeState;
					static CScrollRegion s_JellySuperModeScroll;
					s_JellySuperModeState.m_SelectionPopupContext.m_pScrollRegion = &s_JellySuperModeScroll;
					const char *apJellySuperModes[] = {Localize("Normal"), Localize("Medium"), Localize("Crazy")};
					Expand.HSplitTop(LineSize, &Button, &Expand);
					g_Config.m_ClZzJellyTeeSuperMode = Ui()->DoDropDown(&Button, std::clamp(g_Config.m_ClZzJellyTeeSuperMode, 0, 2), apJellySuperModes, 3, s_JellySuperModeState);
				}
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzJellyTeeStrength, &g_Config.m_ClZzJellyTeeStrength, &Button, Localize("Strength"), 0, 1000);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzJellyTeeDuration, &g_Config.m_ClZzJellyTeeDuration, &Button, Localize("Duration"), 1, 500);
			}
		}

		{
			CUIRect Expand;
			BeginCard(Right, 88.0f + 140.0f * s_3dPhase, Inner);
			DoPanelHeader(Inner, Localize("3D Particles"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZz3dParticles, Localize("Enable 3D particles"), &g_Config.m_ClZz3dParticles, &Inner, LineSize);
			const float Extra = 140.0f * s_3dPhase;
			if(Extra > 0.0f)
			{
				static CUi::SDropDownState s_3dTypeState;
				static CScrollRegion s_3dTypeScroll;
				s_3dTypeState.m_SelectionPopupContext.m_pScrollRegion = &s_3dTypeScroll;
				const char *ap3dTypes[] = {Localize("Cube"), Localize("Heart"), Localize("Mixed")};
				Inner.HSplitTop(Extra, &Expand, &Inner);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				g_Config.m_ClZz3dParticlesType = Ui()->DoDropDown(&Button, g_Config.m_ClZz3dParticlesType - 1, ap3dTypes, 3, s_3dTypeState) + 1;
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZz3dParticlesCount, &g_Config.m_ClZz3dParticlesCount, &Button, Localize("Count"), 1, 200);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZz3dParticlesSpeed, &g_Config.m_ClZz3dParticlesSpeed, &Button, Localize("Speed"), 1, 500);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZz3dParticlesDepth, &g_Config.m_ClZz3dParticlesDepth, &Button, Localize("Depth"), 10, 1000);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZz3dParticlesAlpha, &g_Config.m_ClZz3dParticlesAlpha, &Button, Localize("Alpha"), 1, 100);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZz3dParticlesGlow, Localize("Glow"), &g_Config.m_ClZz3dParticlesGlow, &Expand, LineSize);
			}
		}

		{
			CUIRect Expand;
			BeginCard(Right, 88.0f + 120.0f * s_MagicPhase, Inner);
			DoPanelHeader(Inner, Localize("Magic Particles"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzMagicParticles, Localize("Enable magic particles"), &g_Config.m_ClZzMagicParticles, &Inner, LineSize);
			const float Extra = 120.0f * s_MagicPhase;
			if(Extra > 0.0f)
			{
				static CUi::SDropDownState s_MagicTypeState;
				static CScrollRegion s_MagicTypeScroll;
				s_MagicTypeState.m_SelectionPopupContext.m_pScrollRegion = &s_MagicTypeScroll;
				const char *apMagicTypes[] = {Localize("Slice"), Localize("Ball"), Localize("Smoke"), Localize("Shell")};
				Inner.HSplitTop(Extra, &Expand, &Inner);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				g_Config.m_ClZzMagicParticlesType = Ui()->DoDropDown(&Button, g_Config.m_ClZzMagicParticlesType - 1, apMagicTypes, 4, s_MagicTypeState) + 1;
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzMagicParticlesCount, &g_Config.m_ClZzMagicParticlesCount, &Button, Localize("Count"), 1, 100);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzMagicParticlesRadius, &g_Config.m_ClZzMagicParticlesRadius, &Button, Localize("Radius"), 1, 1000);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzMagicParticlesSize, &g_Config.m_ClZzMagicParticlesSize, &Button, Localize("Size"), 1, 50);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzMagicParticlesAlphaDelay, &g_Config.m_ClZzMagicParticlesAlphaDelay, &Button, Localize("Alpha delay"), 1, 100);
			}
		}

		{
			CUIRect Expand;
			BeginCard(Right, 88.0f + 120.0f * s_OrbitPhase, Inner);
			DoPanelHeader(Inner, Localize("Orbit Aura"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzOrbitAura, Localize("Enable orbit aura"), &g_Config.m_ClZzOrbitAura, &Inner, LineSize);
			const float Extra = 120.0f * s_OrbitPhase;
			if(Extra > 0.0f)
			{
				Inner.HSplitTop(Extra, &Expand, &Inner);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzOrbitAuraRadius, &g_Config.m_ClZzOrbitAuraRadius, &Button, Localize("Radius"), 8, 200);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzOrbitAuraParticles, &g_Config.m_ClZzOrbitAuraParticles, &Button, Localize("Particles"), 2, 120);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzOrbitAuraAlpha, &g_Config.m_ClZzOrbitAuraAlpha, &Button, Localize("Alpha"), 0, 100);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzOrbitAuraSpeed, &g_Config.m_ClZzOrbitAuraSpeed, &Button, Localize("Speed"), 10, 200);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzOrbitAuraIdle, Localize("Only when idle"), &g_Config.m_ClZzOrbitAuraIdle, &Expand, LineSize);
				if(g_Config.m_ClZzOrbitAuraIdle)
				{
					Expand.HSplitTop(LineSize, &Button, &Expand);
					Ui()->DoScrollbarOption(&g_Config.m_ClZzOrbitAuraIdleTimer, &g_Config.m_ClZzOrbitAuraIdleTimer, &Button, Localize("Idle timer"), 1, 30);
				}
			}
		}

		{
			CUIRect Expand;
			BeginCard(Right, 88.0f + 182.0f * s_HammerHitParticlesPhase, Inner);
			DoPanelHeader(Inner, Localize("Hammer Hit Particles"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzHammerHitParticles, Localize("Enable hammer hit particles"), &g_Config.m_ClZzHammerHitParticles, &Inner, LineSize);
			const float Extra = 182.0f * s_HammerHitParticlesPhase;
			if(Extra > 0.0f)
			{
				static CUi::SDropDownState s_HammerHitTypeState;
				static CScrollRegion s_HammerHitTypeScroll;
				s_HammerHitTypeState.m_SelectionPopupContext.m_pScrollRegion = &s_HammerHitTypeScroll;
				const char *apHammerHitTypes[] = {Localize("Cubes"), Localize("Stars"), Localize("Mixed")};
				Inner.HSplitTop(Extra, &Expand, &Inner);
				DoButton_CheckBoxAutoVMarginAndSet(
					&g_Config.m_ClZzHammerHitParticlesHideDefaultHitEffect,
					Localize("Hide default hit effect"),
					&g_Config.m_ClZzHammerHitParticlesHideDefaultHitEffect,
					&Expand,
					LineSize);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				const int HammerTypeSel = std::clamp(g_Config.m_ClZzHammerHitParticlesType - 1, 0, 2);
				g_Config.m_ClZzHammerHitParticlesType = Ui()->DoDropDown(&Button, HammerTypeSel, apHammerHitTypes, 3, s_HammerHitTypeState) + 1;
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzHammerHitParticlesCount, &g_Config.m_ClZzHammerHitParticlesCount, &Button, Localize("Count"), 1, 80);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzHammerHitParticlesSize, &g_Config.m_ClZzHammerHitParticlesSize, &Button, Localize("Size"), 2, 36);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzHammerHitParticlesSpeed, &g_Config.m_ClZzHammerHitParticlesSpeed, &Button, Localize("Speed"), 20, 1200);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzHammerHitParticlesLifetime, &g_Config.m_ClZzHammerHitParticlesLifetime, &Button, Localize("Lifetime ms"), 100, 2500);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzHammerHitParticlesAlpha, &g_Config.m_ClZzHammerHitParticlesAlpha, &Button, Localize("Alpha"), 1, 100);
				static CUi::SDropDownState s_HammerHitColorModeState;
				static CScrollRegion s_HammerHitColorModeScroll;
				s_HammerHitColorModeState.m_SelectionPopupContext.m_pScrollRegion = &s_HammerHitColorModeScroll;
				const char *apHammerColorModes[] = {Localize("Custom"), Localize("Rainbow")};
				Expand.HSplitTop(LineSize, &Button, &Expand);
				const int HammerColorModeSel = std::clamp(g_Config.m_ClZzHammerHitParticlesColorMode - 1, 0, 1);
				g_Config.m_ClZzHammerHitParticlesColorMode = Ui()->DoDropDown(&Button, HammerColorModeSel, apHammerColorModes, 2, s_HammerHitColorModeState) + 1;
				if(g_Config.m_ClZzHammerHitParticlesColorMode == 1)
				{
					static CButtonContainer s_HammerHitParticlesColorResetId;
					DoLine_ColorPicker(&s_HammerHitParticlesColorResetId, 25.0f, 13.0f, 4.0f, &Expand, Localize("Particle color"), &g_Config.m_ClZzHammerHitParticlesColor, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), false);
				}
			}
		}
		EndCard();
		s_VisualsScrollRegion.End();
		pFeatureScrollRegion = nullptr;
	}
	else if(s_SubTab == ZZ_TAB_BESTCLIENT)
	{
		const float LineSize = 20.0f;
		static int s_BestClientSubTab = 0;
		static CButtonContainer s_aBestClientSubTabs[4] = {};

		CUIRect InnerTabBar, InnerTabButton;
		MainView.HSplitTop(22.0f, &InnerTabBar, &MainView);
		const float InnerTabWidth = InnerTabBar.w / 4.0f;
		const char *apInnerTabNames[] = {
			Localize("BestClient"),
			Localize("Voice Chat"),
			Localize("Network"),
			Localize("Bots")};
		for(int i = 0; i < 4; i++)
		{
			InnerTabBar.VSplitLeft(InnerTabWidth, &InnerTabButton, &InnerTabBar);
			const int Corners = i == 0 ? IGraphics::CORNER_L : i == 3 ? IGraphics::CORNER_R : IGraphics::CORNER_NONE;
			if(DoButton_MenuTab(&s_aBestClientSubTabs[i], apInnerTabNames[i], s_BestClientSubTab == i, &InnerTabButton, Corners, nullptr, nullptr, nullptr, nullptr, 4.0f))
				s_BestClientSubTab = i;
		}
		MainView.HSplitTop(10.0f, nullptr, &MainView);

		if(s_BestClientSubTab == 0)
		{
			CUIRect Inner;
			BeginCard(MainView, 350.0f, Inner);
			DoPanelHeader(Inner, Localize("BestClient"));
			DoButton_CheckBoxAutoVMarginAndSet(
				&g_Config.m_ClZzBestclientRegister,
				"Регистрировать игрока как пользователь бестклиента",
				&g_Config.m_ClZzBestclientRegister,
				&Inner,
				LineSize);

			DoButton_CheckBoxAutoVMarginAndSet(
				&g_Config.m_ClZzBestclientRegisterNicksEnabled,
				Localize("Регистрировать игроков по никам"),
				&g_Config.m_ClZzBestclientRegisterNicksEnabled,
				&Inner,
				LineSize);

			Inner.HSplitTop(8.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Ники для регистрации метки BestClient (через запятую)"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CLineInput s_BestClientRegisterNicks;
			s_BestClientRegisterNicks.SetBuffer(g_Config.m_ClZzBestclientRegisterNicks, sizeof(g_Config.m_ClZzBestclientRegisterNicks));
			s_BestClientRegisterNicks.SetEmptyText("\"nick1\", \"nick2\"");
			Ui()->DoEditBox(&s_BestClientRegisterNicks, &Button, 12.0f);

			Inner.HSplitTop(8.0f, nullptr, &Inner);
			DoButton_CheckBoxAutoVMarginAndSet(
				&g_Config.m_ClZzBestclientRegisterAll,
				Localize("Регистрировать всех игроков сервера для метки BestClient"),
				&g_Config.m_ClZzBestclientRegisterAll,
				&Inner,
				LineSize);

			Inner.HSplitTop(8.0f, nullptr, &Inner);
			DoButton_CheckBoxAutoVMarginAndSet(
				&g_Config.m_ClZzBestclientShowInTab,
				Localize("Показывать метки BestClient в TAB"),
				&g_Config.m_ClZzBestclientShowInTab,
				&Inner,
				LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(
				&g_Config.m_ClZzBestclientShowInBrowser,
				Localize("Показывать метки BestClient в браузере"),
				&g_Config.m_ClZzBestclientShowInBrowser,
				&Inner,
				LineSize);

			Inner.HSplitTop(8.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("При включении: token.json -> join -> heartbeat -> leave при дисконнекте."), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, g_Config.m_ClZzBestclientIndicatorServerAddress, 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, g_Config.m_ClZzBestclientTokenUrl, 14.0f, TEXTALIGN_ML);
		}
		else if(s_BestClientSubTab == 1)
		{
			CUIRect Inner;
			BeginCard(MainView, maximum(140.0f, MainView.h - 4.0f), Inner);
			DoPanelHeader(Inner, Localize("Voice Chat"));

			CUIRect Row;
			Inner.HSplitTop(20.0f, &Row, &Inner);
			Ui()->DoLabel(&Row, Localize("Voice chat panel and settings"), 14.0f, TEXTALIGN_ML);

			Inner.HSplitTop(6.0f, nullptr, &Inner);
			Inner.HSplitTop(18.0f, &Row, &Inner);
			Ui()->DoLabel(&Row, Localize("Open voice panel using bind below (toggle_voice_panel)."), 12.0f, TEXTALIGN_ML);

			Inner.HSplitTop(8.0f, nullptr, &Inner);
			Inner.HSplitTop(120.0f, &Row, &Inner);
			GameClient()->m_VoiceChat.RenderMenuControlBinds(Row);
		}
		else if(s_BestClientSubTab == 2)
		{
			CUIRect Inner;
			BeginCard(MainView, 520.0f, Inner);
			DoPanelHeader(Inner, Localize("Network"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzProxyEnabled, Localize("Enable proxy"), &g_Config.m_ClZzProxyEnabled, &Inner, LineSize);

			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Proxy address (ip:port)"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CLineInput s_ProxyAddressInput;
			s_ProxyAddressInput.SetBuffer(g_Config.m_ClZzProxyAddress, sizeof(g_Config.m_ClZzProxyAddress));
			s_ProxyAddressInput.SetEmptyText("127.0.0.1:1080");
			Ui()->DoEditBox(&s_ProxyAddressInput, &Button, 12.0f);

			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Proxy login (optional)"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CLineInput s_ProxyUsernameInput;
			s_ProxyUsernameInput.SetBuffer(g_Config.m_ClZzProxyUsername, sizeof(g_Config.m_ClZzProxyUsername));
			s_ProxyUsernameInput.SetEmptyText("username");
			Ui()->DoEditBox(&s_ProxyUsernameInput, &Button, 12.0f);

			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("Proxy password (optional)"), 14.0f, TEXTALIGN_ML);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			static CLineInput s_ProxyPasswordInput;
			s_ProxyPasswordInput.SetBuffer(g_Config.m_ClZzProxyPassword, sizeof(g_Config.m_ClZzProxyPassword));
			s_ProxyPasswordInput.SetEmptyText("password");
			Ui()->DoEditBox(&s_ProxyPasswordInput, &Button, 12.0f);

			Inner.HSplitTop(8.0f, nullptr, &Inner);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzFakePing, Localize("Fake ping"), &g_Config.m_ClZzFakePing, &Inner, LineSize);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Ui()->DoScrollbarOption(&g_Config.m_ClZzFakePingValue, &g_Config.m_ClZzFakePingValue, &Button, Localize("Fake ping ms"), 0, 3000);

			Inner.HSplitTop(6.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize * 2.0f, &Label, &Inner);
			Ui()->DoLabel(&Label, Localize("SOCKS5 proxy auto-restores after short drops. Fake ping delays outgoing traffic."), 14.0f, TEXTALIGN_ML);

			Inner.HSplitTop(10.0f, nullptr, &Inner);
			DoPanelHeader(Inner, Localize("Client spoofing"));
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Button.VSplitMid(&Label, &Button);
			Ui()->DoLabel(&Label, Localize("Version Number:"), 14.0f, TEXTALIGN_ML);
			static char s_aVerBuf[32];
			static bool s_VerInitialized = false;
			if(!s_VerInitialized)
			{
				str_format(s_aVerBuf, sizeof(s_aVerBuf), "%d", g_Config.m_ClZzNetVersionNum);
				s_VerInitialized = true;
			}
			static CLineInput s_Ver(s_aVerBuf, sizeof(s_aVerBuf));
			Ui()->DoEditBox(&s_Ver, &Button, 12.0f);
			g_Config.m_ClZzNetVersionNum = str_toint(s_aVerBuf);

			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Button.VSplitMid(&Label, &Button);
			Ui()->DoLabel(&Label, Localize("Version String:"), 14.0f, TEXTALIGN_ML);
			static CLineInput s_VerStr(g_Config.m_ClZzNetVersionName, sizeof(g_Config.m_ClZzNetVersionName));
			Ui()->DoEditBox(&s_VerStr, &Button, 12.0f);

			Inner.HSplitTop(4.0f, nullptr, &Inner);
			{
				static std::vector<const char *> s_ClientDropDownNames = {
					"DDNet", "TClient", "EClient", "Cactus", "ChillerBot", "Pulse Client", "FeX Client", "S-Client", "StA Client",
					"Koshka Client", "Best Client", "Aiodob Client", "Nofis Client", "Rushie", "Duck Client", "Axis Client",
					"Kaizo Client", "JS Client", "Nofis Client"};
				static CUi::SDropDownState s_ClientDropDownState;
				static CScrollRegion s_ClientDropDownScrollRegion;
				s_ClientDropDownState.m_SelectionPopupContext.m_pScrollRegion = &s_ClientDropDownScrollRegion;
				CUIRect ClientDropDownRect;
				Inner.HSplitTop(LineSize, &ClientDropDownRect, &Inner);
				ClientDropDownRect.VSplitLeft(130.0f, &Label, &ClientDropDownRect);
				Ui()->DoLabel(&Label, Localize("Spoof Client:"), 14.0f, TEXTALIGN_ML);
				const int ClientSelectedNew = Ui()->DoDropDown(&ClientDropDownRect, g_Config.m_ClZzNetIdentity, s_ClientDropDownNames.data(), s_ClientDropDownNames.size(), s_ClientDropDownState);
				if(g_Config.m_ClZzNetIdentity != ClientSelectedNew)
					g_Config.m_ClZzNetIdentity = ClientSelectedNew;
			}

			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Button.VSplitMid(&Label, &Button);
			Ui()->DoLabel(&Label, Localize("Git Revision:"), 14.0f, TEXTALIGN_ML);
			static CLineInput s_GitRev(g_Config.m_ClZzNetGitRev, sizeof(g_Config.m_ClZzNetGitRev));
			Ui()->DoEditBox(&s_GitRev, &Button, 12.0f);

			Inner.HSplitTop(4.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Button, &Inner);
			CUIRect BtnL, BtnR;
			Button.VSplitMid(&BtnL, &BtnR, 4.0f);
			static CButtonContainer s_DoVerStrBtn;
			if(DoButton_Menu(&s_DoVerStrBtn, Localize("Auto complete Verstr"), 0, &BtnL))
			{
				const int Vernum = g_Config.m_ClZzNetVersionNum;
				const int Major = Vernum / 1000;
				const int Remainder = Vernum % 1000;
				const int Patch = Remainder % 10;
				const int Minor = Remainder / 10;
				char aNewVerStr[32];
				if(Patch != 0)
					str_format(aNewVerStr, sizeof(aNewVerStr), "%d.%d.%d", Major, Minor, Patch);
				else
					str_format(aNewVerStr, sizeof(aNewVerStr), "%d.%d", Major, Minor);
				str_copy(g_Config.m_ClZzNetVersionName, aNewVerStr, sizeof(g_Config.m_ClZzNetVersionName));
				s_VerStr.SetBuffer(g_Config.m_ClZzNetVersionName, sizeof(g_Config.m_ClZzNetVersionName));
			}
			static CButtonContainer s_DoVerNumBtn;
			if(DoButton_Menu(&s_DoVerNumBtn, Localize("Auto complete Vernum"), 0, &BtnR))
			{
				const char *pSrc = g_Config.m_ClZzNetVersionName;
				int Major = 0, Minor = 0, Patch = 0;
				if(sscanf(pSrc, "%d.%d.%d", &Major, &Minor, &Patch) >= 2)
				{
					const int NewVernum = Major * 1000 + Minor * 10 + Patch;
					g_Config.m_ClZzNetVersionNum = NewVernum;
					str_format(s_aVerBuf, sizeof(s_aVerBuf), "%d", NewVernum);
				}
			}
		}
		else
		{
			static CScrollRegion s_BotsScrollRegion;
			static vec2 s_BotsScrollOffset(0.0f, 0.0f);
			CScrollRegionParams ScrollParams;
			ScrollParams.m_ScrollUnit = 28.0f;
			s_BotsScrollRegion.Begin(&MainView, &s_BotsScrollOffset, &ScrollParams);
			CUIRect ScrollView = MainView;
			ScrollView.y += s_BotsScrollOffset.y;
			pFeatureScrollRegion = &s_BotsScrollRegion;

			static float s_BotsMirrorPhase = 0.0f;
			static float s_BotsSkinCopyPhase = 0.0f;
			static float s_BotsNameBasePhase = 0.0f;
			static float s_BotsCustomColorPhase = 0.0f;
			static float s_BotsIdentityCustomPhase = 0.0f;
			static int s_BotsSubTab = 0; // 0 Main, 1 Control, 2 Others
			static CButtonContainer s_aBotsSubTabs[3];
			UpdateRevealPhase(s_BotsMirrorPhase, g_Config.m_ClZzAsyncBotsMirrorEnable != 0);
			UpdateRevealPhase(s_BotsSkinCopyPhase, g_Config.m_ClZzAsyncBotsSkinMode == 1);
			UpdateRevealPhase(s_BotsNameBasePhase, g_Config.m_ClZzAsyncBotsNameMode == 0);
			UpdateRevealPhase(s_BotsCustomColorPhase, g_Config.m_ClZzAsyncBotsSkinUseCustomColor != 0);
			UpdateRevealPhase(s_BotsIdentityCustomPhase, g_Config.m_ClZzAsyncBotsIdentityCustom != 0);
			const auto FormatAsyncBotSummary = [&](char *pBuf, int BufSize) {
				const auto &Stats = GameClient()->AsyncProxyBots().Stats();
				str_format(pBuf, BufSize, "Bots: %d / Online: %d / Error: %d", Stats.m_Total, Stats.m_Ingame, Stats.m_Error);
			};
			CUIRect BotsTabs;
			ScrollView.HSplitTop(24.0f, &BotsTabs, &ScrollView);
			const float BotsTabW = BotsTabs.w / 3.0f;
			const char *apBotsTabNames[] = {Localize("Main"), Localize("Control"), Localize("Others")};
			for(int i = 0; i < 3; i++)
			{
				CUIRect TabBtn;
				BotsTabs.VSplitLeft(BotsTabW, &TabBtn, &BotsTabs);
				const int Corners = i == 0 ? IGraphics::CORNER_L : (i == 2 ? IGraphics::CORNER_R : IGraphics::CORNER_NONE);
				if(DoButton_MenuTab(&s_aBotsSubTabs[i], apBotsTabNames[i], s_BotsSubTab == i, &TabBtn, Corners, nullptr, nullptr, nullptr, nullptr, 4.0f))
					s_BotsSubTab = i;
			}
			ScrollView.HSplitTop(8.0f, nullptr, &ScrollView);
			CUIRect ScrollLeft, ScrollRight;
			ScrollView.VSplitMid(&ScrollLeft, &ScrollRight, 8.0f);

			{

			if(s_BotsSubTab == 0)
			{
				CUIRect Inner;
				char aBotSummary[96];
				FormatAsyncBotSummary(aBotSummary, sizeof(aBotSummary));
				const float Extra = 308.0f;
				BeginCard(ScrollLeft, 152.0f + Extra, Inner);
				DoPanelHeader(Inner, Localize("Connection"));
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsAutoConnect, Localize("Auto-start after connect"), &g_Config.m_ClZzAsyncBotsAutoConnect, &Inner, LineSize);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsFastReconnect, Localize("Fast reconnect"), &g_Config.m_ClZzAsyncBotsFastReconnect, &Inner, LineSize);
				Inner.HSplitTop(LineSize, &Button, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsFastReconnectDelayMs, &g_Config.m_ClZzAsyncBotsFastReconnectDelayMs, &Button, Localize("Fast reconnect delay (ms)"), 0, 3000);
				Inner.HSplitTop(8.0f, nullptr, &Inner);
				CUIRect Row;
				Inner.HSplitTop(LineSize, &Row, &Inner);
				CUIRect BtnConnect, BtnDisconnect;
				Row.VSplitMid(&BtnConnect, &BtnDisconnect, 5.0f);
				static CButtonContainer s_AsyncBotsConnectMain;
				static CButtonContainer s_AsyncBotsDisconnectMain;
				if(DoButton_Menu(&s_AsyncBotsConnectMain, Localize("Connect"), 0, &BtnConnect))
					GameClient()->AsyncProxyBots().StartFromConfig(GameClient());
				if(DoButton_Menu(&s_AsyncBotsDisconnectMain, Localize("Disconnect"), 0, &BtnDisconnect))
					GameClient()->AsyncProxyBots().StopAll();
				Inner.HSplitTop(6.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, aBotSummary, 14.0f, TEXTALIGN_ML);
				Inner.HSplitTop(4.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, GameClient()->AsyncProxyBots().StatusText(), 14.0f, TEXTALIGN_ML);
				CUIRect Expand;
				Inner.HSplitTop(Extra, &Expand, &Inner);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsSkipMapDownload, Localize("Skip map download"), &g_Config.m_ClZzAsyncBotsSkipMapDownload, &Expand, LineSize);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsUseSavedMaps, Localize("Use saved maps first"), &g_Config.m_ClZzAsyncBotsUseSavedMaps, &Expand, LineSize);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsVerbose, Localize("Verbose diagnostics"), &g_Config.m_ClZzAsyncBotsVerbose, &Expand, LineSize);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsPerProxy, &g_Config.m_ClZzAsyncBotsPerProxy, &Button, Localize("Bots per proxy"), 1, 64);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsMaxTotal, &g_Config.m_ClZzAsyncBotsMaxTotal, &Button, Localize("Max total bots"), 1, 2048);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsInputRate, &g_Config.m_ClZzAsyncBotsInputRate, &Button, Localize("Input rate (pps)"), 1, 100);
				Expand.HSplitTop(LineSize, &Button, &Expand);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsConnectTimeoutMs, &g_Config.m_ClZzAsyncBotsConnectTimeoutMs, &Button, Localize("Connect timeout (ms)"), 1000, 15000);
			}

			if(s_BotsSubTab == 0 || s_BotsSubTab == 1)
			{
				CUIRect Inner;
				BeginCard(ScrollRight, 96.0f, Inner);
				DoPanelHeader(Inner, Localize("Proxy Pool"));
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, Localize("SOCKS5 proxies (comma/newline/semicolon)"), 14.0f, TEXTALIGN_ML);
				Inner.HSplitTop(LineSize, &Button, &Inner);
				static CLineInput s_AsyncBotsProxies2;
				s_AsyncBotsProxies2.SetBuffer(g_Config.m_ClZzAsyncBotsProxies, sizeof(g_Config.m_ClZzAsyncBotsProxies));
				s_AsyncBotsProxies2.SetEmptyText("socks5://user:pass@1.2.3.4:1080, 5.6.7.8:1080");
				Ui()->DoEditBox(&s_AsyncBotsProxies2, &Button, 12.0f);
			}

			if(s_BotsSubTab == 0 || s_BotsSubTab == 1)
			{
				CUIRect Inner;
				const float CustomIdentityPhase = s_BotsIdentityCustomPhase;
				const float CopyIdentityPhase = 1.0f - CustomIdentityPhase;
				const float NameExtra = 52.0f * s_BotsNameBasePhase * CustomIdentityPhase;
				const float SkinExtra = 52.0f * CustomIdentityPhase;
				const float ColorExtra = 58.0f * s_BotsCustomColorPhase * CustomIdentityPhase;
				const float CopyExtra = 170.0f * CopyIdentityPhase;
				BeginCard(ScrollLeft, 280.0f + NameExtra + SkinExtra + ColorExtra + CopyExtra, Inner);
				DoPanelHeader(Inner, Localize("Identity"));

				auto IsSignedNumber = [](const char *pText) -> bool {
					if(!pText || pText[0] == '\0')
						return false;
					const char *p = pText;
					if(*p == '-')
						++p;
					if(*p == '\0')
						return false;
					for(; *p; ++p)
					{
						if(*p < '0' || *p > '9')
							return false;
					}
					return true;
				};

				auto IsClientPresent = [&](int ClientId) -> bool {
					return ClientId >= 0 && ClientId < MAX_CLIENTS && GameClient()->m_Snap.m_apPlayerInfos[ClientId] != nullptr;
				};

				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsIdentityCustom, Localize("Custom identity"), &g_Config.m_ClZzAsyncBotsIdentityCustom, &Inner, LineSize);

				static CLineInput s_IdentityCopyIdInput;
				static char s_aIdentityCopyIdBuf[16] = "-1";
				static bool s_IdentityCopyIdInit = false;
				if(!s_IdentityCopyIdInit)
				{
					str_format(s_aIdentityCopyIdBuf, sizeof(s_aIdentityCopyIdBuf), "%d", g_Config.m_ClZzAsyncBotsIdentityCopyClientId);
					s_IdentityCopyIdInit = true;
				}
				s_IdentityCopyIdInput.SetBuffer(s_aIdentityCopyIdBuf, sizeof(s_aIdentityCopyIdBuf));
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, Localize("Identity source client id"), 14.0f, TEXTALIGN_ML);
				Inner.HSplitTop(LineSize, &Button, &Inner);
				Ui()->DoEditBox(&s_IdentityCopyIdInput, &Button, 12.0f);

				bool IdentityCopyIdValid = IsSignedNumber(s_aIdentityCopyIdBuf);
				if(IdentityCopyIdValid)
				{
					const int Value = str_toint(s_aIdentityCopyIdBuf);
					if(Value >= -1 && Value <= 200)
						g_Config.m_ClZzAsyncBotsIdentityCopyClientId = Value;
					else
						IdentityCopyIdValid = false;
				}
				if(!IdentityCopyIdValid)
				{
					Inner.HSplitTop(LineSize, &Label, &Inner);
					Ui()->DoLabel(&Label, Localize("Client id must be a number in range -1..200"), 14.0f, TEXTALIGN_ML);
				}
				else
				{
					const int SourceId = g_Config.m_ClZzAsyncBotsIdentityCopyClientId;
					if(SourceId >= MAX_CLIENTS)
					{
						Inner.HSplitTop(LineSize, &Label, &Inner);
						Ui()->DoLabel(&Label, Localize("Client id is above current protocol limit, cannot copy now"), 14.0f, TEXTALIGN_ML);
					}
					else if(SourceId >= 0 && !IsClientPresent(SourceId))
					{
						Inner.HSplitTop(LineSize, &Label, &Inner);
						Ui()->DoLabel(&Label, Localize("Client id is not currently present on this server"), 14.0f, TEXTALIGN_ML);
					}
				}

				static CUi::SDropDownState s_AsyncBotsNameModeState;
				static CScrollRegion s_AsyncBotsNameModeScroll;
				s_AsyncBotsNameModeState.m_SelectionPopupContext.m_pScrollRegion = &s_AsyncBotsNameModeScroll;
				const char *apNameModes[] = {Localize("Base + index"), Localize("Pseudo-random letters")};
				if(CustomIdentityPhase > 0.0f)
				{
					Inner.HSplitTop(LineSize, &Button, &Inner);
					g_Config.m_ClZzAsyncBotsNameMode = Ui()->DoDropDown(&Button, g_Config.m_ClZzAsyncBotsNameMode, apNameModes, 2, s_AsyncBotsNameModeState);
					if(NameExtra > 0.0f)
					{
						CUIRect Expand;
						Inner.HSplitTop(NameExtra, &Expand, &Inner);
						Expand.HSplitTop(LineSize, &Label, &Expand);
						Ui()->DoLabel(&Label, Localize("Bot base name"), 14.0f, TEXTALIGN_ML);
						Expand.HSplitTop(LineSize, &Button, &Expand);
						static CLineInput s_AsyncBotsBaseName2;
						s_AsyncBotsBaseName2.SetBuffer(g_Config.m_ClZzAsyncBotsBaseName, sizeof(g_Config.m_ClZzAsyncBotsBaseName));
						s_AsyncBotsBaseName2.SetEmptyText("ZZBot");
						Ui()->DoEditBox(&s_AsyncBotsBaseName2, &Button, 12.0f);
					}

					static CUi::SDropDownState s_AsyncBotsSkinModeState;
					static CScrollRegion s_AsyncBotsSkinModeScroll;
					s_AsyncBotsSkinModeState.m_SelectionPopupContext.m_pScrollRegion = &s_AsyncBotsSkinModeScroll;
					const char *apSkinModes[] = {Localize("Fixed skin"), Localize("Copy client skin")};
					Inner.HSplitTop(LineSize, &Button, &Inner);
					g_Config.m_ClZzAsyncBotsSkinMode = Ui()->DoDropDown(&Button, g_Config.m_ClZzAsyncBotsSkinMode, apSkinModes, 2, s_AsyncBotsSkinModeState);

					CUIRect SkinExpand;
					Inner.HSplitTop(SkinExtra, &SkinExpand, &Inner);
					if(g_Config.m_ClZzAsyncBotsSkinMode == 0)
					{
						SkinExpand.HSplitTop(LineSize, &Label, &SkinExpand);
						Ui()->DoLabel(&Label, Localize("Bot fixed skin"), 14.0f, TEXTALIGN_ML);
						SkinExpand.HSplitTop(LineSize, &Button, &SkinExpand);
						static CLineInput s_AsyncBotsSkinName;
						s_AsyncBotsSkinName.SetBuffer(g_Config.m_ClZzAsyncBotsSkinName, sizeof(g_Config.m_ClZzAsyncBotsSkinName));
						s_AsyncBotsSkinName.SetEmptyText("default");
						Ui()->DoEditBox(&s_AsyncBotsSkinName, &Button, 12.0f);
					}
					else
					{
						static CLineInput s_SkinCopyIdInput;
						static char s_aSkinCopyIdBuf[16] = "-1";
						static bool s_SkinCopyIdInit = false;
						if(!s_SkinCopyIdInit)
						{
							str_format(s_aSkinCopyIdBuf, sizeof(s_aSkinCopyIdBuf), "%d", g_Config.m_ClZzAsyncBotsSkinCopyClientId);
							s_SkinCopyIdInit = true;
						}
						s_SkinCopyIdInput.SetBuffer(s_aSkinCopyIdBuf, sizeof(s_aSkinCopyIdBuf));
						SkinExpand.HSplitTop(LineSize, &Label, &SkinExpand);
						Ui()->DoLabel(&Label, Localize("Copy skin+colors+clan from client id"), 14.0f, TEXTALIGN_ML);
						SkinExpand.HSplitTop(LineSize, &Button, &SkinExpand);
						Ui()->DoEditBox(&s_SkinCopyIdInput, &Button, 12.0f);
						bool SkinCopyIdValid = IsSignedNumber(s_aSkinCopyIdBuf);
						if(SkinCopyIdValid)
						{
							const int Value = str_toint(s_aSkinCopyIdBuf);
							if(Value >= -1 && Value <= 200)
								g_Config.m_ClZzAsyncBotsSkinCopyClientId = Value;
							else
								SkinCopyIdValid = false;
						}
						if(!SkinCopyIdValid)
						{
							SkinExpand.HSplitTop(LineSize, &Label, &SkinExpand);
							Ui()->DoLabel(&Label, Localize("Client id must be a number in range -1..200"), 14.0f, TEXTALIGN_ML);
						}
					}

					Inner.HSplitTop(LineSize, &Label, &Inner);
					Ui()->DoLabel(&Label, Localize("Bot clan"), 14.0f, TEXTALIGN_ML);
					Inner.HSplitTop(LineSize, &Button, &Inner);
					static CLineInput s_AsyncBotsClan;
					s_AsyncBotsClan.SetBuffer(g_Config.m_ClZzAsyncBotsClan, sizeof(g_Config.m_ClZzAsyncBotsClan));
					Ui()->DoEditBox(&s_AsyncBotsClan, &Button, 12.0f);

					Inner.HSplitTop(LineSize, &Label, &Inner);
					Ui()->DoLabel(&Label, Localize("Bot country code"), 14.0f, TEXTALIGN_ML);
					Inner.HSplitTop(LineSize, &Button, &Inner);
					static CLineInput s_AsyncBotsCountryInput;
					static char s_aAsyncBotsCountryBuf[16] = "-1";
					static bool s_AsyncBotsCountryInit = false;
					if(!s_AsyncBotsCountryInit)
					{
						str_format(s_aAsyncBotsCountryBuf, sizeof(s_aAsyncBotsCountryBuf), "%d", g_Config.m_ClZzAsyncBotsCountry);
						s_AsyncBotsCountryInit = true;
					}
					s_AsyncBotsCountryInput.SetBuffer(s_aAsyncBotsCountryBuf, sizeof(s_aAsyncBotsCountryBuf));
					Ui()->DoEditBox(&s_AsyncBotsCountryInput, &Button, 12.0f);
					if(IsSignedNumber(s_aAsyncBotsCountryBuf))
					{
						const int Value = str_toint(s_aAsyncBotsCountryBuf);
						if(Value >= -999 && Value <= 1000)
							g_Config.m_ClZzAsyncBotsCountry = Value;
					}

					DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsSkinUseCustomColor, Localize("Use custom skin colors"), &g_Config.m_ClZzAsyncBotsSkinUseCustomColor, &Inner, LineSize);
					if(ColorExtra > 0.0f)
					{
						static CButtonContainer s_AsyncBotsBodyColorResetId;
						static CButtonContainer s_AsyncBotsFeetColorResetId;
						DoLine_ColorPicker(&s_AsyncBotsBodyColorResetId, 25.0f, 13.0f, 4.0f, &Inner, Localize("Skin body color"), &g_Config.m_ClZzAsyncBotsSkinColorBody, color_cast<ColorRGBA>(ColorHSLA(65408, true)), false);
						DoLine_ColorPicker(&s_AsyncBotsFeetColorResetId, 25.0f, 13.0f, 4.0f, &Inner, Localize("Skin feet color"), &g_Config.m_ClZzAsyncBotsSkinColorFeet, color_cast<ColorRGBA>(ColorHSLA(65408, true)), false);
					}
				}
				else
				{
					DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsIdentityCopyName, Localize("Copy name"), &g_Config.m_ClZzAsyncBotsIdentityCopyName, &Inner, LineSize);
					DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsIdentityCopySkin, Localize("Copy skin"), &g_Config.m_ClZzAsyncBotsIdentityCopySkin, &Inner, LineSize);
					DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsIdentityCopyClan, Localize("Copy clan"), &g_Config.m_ClZzAsyncBotsIdentityCopyClan, &Inner, LineSize);
					DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsIdentityCopyCountry, Localize("Copy country"), &g_Config.m_ClZzAsyncBotsIdentityCopyCountry, &Inner, LineSize);
					DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsIdentityCopyColors, Localize("Copy colors"), &g_Config.m_ClZzAsyncBotsIdentityCopyColors, &Inner, LineSize);
				}
			}

			if(s_BotsSubTab == 0 || s_BotsSubTab == 1)
			{
				CUIRect Inner;
				const float Extra = 78.0f * s_BotsMirrorPhase;
				BeginCard(ScrollRight, 78.0f + Extra, Inner);
				DoPanelHeader(Inner, Localize("Mirror"));
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsMirrorEnable, Localize("Mirror source client (input/chat/emote)"), &g_Config.m_ClZzAsyncBotsMirrorEnable, &Inner, LineSize);
				if(Extra > 0.0f)
				{
					CUIRect Expand;
					static CLineInput s_MirrorIdInput;
					static char s_aMirrorIdBuf[16] = "-1";
					static bool s_MirrorIdInit = false;
					if(!s_MirrorIdInit)
					{
						str_format(s_aMirrorIdBuf, sizeof(s_aMirrorIdBuf), "%d", g_Config.m_ClZzAsyncBotsMirrorClientId);
						s_MirrorIdInit = true;
					}
					s_MirrorIdInput.SetBuffer(s_aMirrorIdBuf, sizeof(s_aMirrorIdBuf));
					Inner.HSplitTop(Extra, &Expand, &Inner);
					Expand.HSplitTop(LineSize, &Label, &Expand);
					Ui()->DoLabel(&Label, Localize("Mirror source client id"), 14.0f, TEXTALIGN_ML);
					Expand.HSplitTop(LineSize, &Button, &Expand);
					Ui()->DoEditBox(&s_MirrorIdInput, &Button, 12.0f);
					DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsMirrorAimOurCursor, Localize("Aim our cursor"), &g_Config.m_ClZzAsyncBotsMirrorAimOurCursor, &Expand, LineSize);
					bool MirrorIdValid = true;
					const char *p = s_aMirrorIdBuf;
					if(!p || p[0] == '\0')
						MirrorIdValid = false;
					if(MirrorIdValid && *p == '-')
						++p;
					if(MirrorIdValid && *p == '\0')
						MirrorIdValid = false;
					for(; MirrorIdValid && *p; ++p)
					{
						if(*p < '0' || *p > '9')
							MirrorIdValid = false;
					}
					if(MirrorIdValid)
					{
						const int Value = str_toint(s_aMirrorIdBuf);
						if(Value >= -1 && Value <= 200)
							g_Config.m_ClZzAsyncBotsMirrorClientId = Value;
						else
							MirrorIdValid = false;
					}
					if(!MirrorIdValid)
					{
						Expand.HSplitTop(LineSize, &Label, &Expand);
						Ui()->DoLabel(&Label, Localize("Client id must be a number in range -1..200"), 14.0f, TEXTALIGN_ML);
					}
					else
					{
						const int MirrorId = g_Config.m_ClZzAsyncBotsMirrorClientId;
						if(MirrorId >= MAX_CLIENTS)
						{
							Expand.HSplitTop(LineSize, &Label, &Expand);
							Ui()->DoLabel(&Label, Localize("Client id is above current protocol limit, cannot mirror now"), 14.0f, TEXTALIGN_ML);
						}
						else if(MirrorId >= 0 && !GameClient()->m_Snap.m_apPlayerInfos[MirrorId])
						{
							Expand.HSplitTop(LineSize, &Label, &Expand);
							Ui()->DoLabel(&Label, Localize("Client id is not currently present on this server"), 14.0f, TEXTALIGN_ML);
						}
						else if(g_Config.m_ClZzAsyncBotsMirrorAimOurCursor && GameClient()->m_aLocalIds[0] < 0 && GameClient()->m_aLocalIds[1] < 0)
						{
							Expand.HSplitTop(LineSize, &Label, &Expand);
							Ui()->DoLabel(&Label, Localize("Aim our cursor needs a local main or dummy tee in-game"), 14.0f, TEXTALIGN_ML);
						}
					}
				}
			}

			if(s_BotsSubTab == 2)
			{
				CUIRect Inner;
				BeginCard(ScrollLeft, 230.0f, Inner);
				DoPanelHeader(Inner, Localize("Dummy Companion"));
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsCompanionMode, Localize("Enable companion swarm AI"), &g_Config.m_ClZzAsyncBotsCompanionMode, &Inner, LineSize);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsCompanionComeMode, Localize("Come mode (fast risky paths)"), &g_Config.m_ClZzAsyncBotsCompanionComeMode, &Inner, LineSize);
				CUIRect Row;
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsCompanionRescueTiles, &g_Config.m_ClZzAsyncBotsCompanionRescueTiles, &Row, Localize("Rescue radius (tiles)"), 4, 40);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsCompanionChaseTiles, &g_Config.m_ClZzAsyncBotsCompanionChaseTiles, &Row, Localize("Max chase distance (tiles)"), 8, 100);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsCompanionFollowTiles, &g_Config.m_ClZzAsyncBotsCompanionFollowTiles, &Row, Localize("Follow main if farther than (tiles)"), 5, 200);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsCompanionSpacingTiles, &g_Config.m_ClZzAsyncBotsCompanionSpacingTiles, &Row, Localize("Preferred spacing to main (tiles)"), 1, 20);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsCompanionAggro, &g_Config.m_ClZzAsyncBotsCompanionAggro, &Row, Localize("Aggression (%)"), 0, 100);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsCompanionPlanMs, &g_Config.m_ClZzAsyncBotsCompanionPlanMs, &Row, Localize("Planner interval (ms)"), 15, 500);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsCompanionDebug, Localize("Companion debug"), &g_Config.m_ClZzAsyncBotsCompanionDebug, &Inner, LineSize);
			}

			if(s_BotsSubTab == 2)
			{
				CUIRect Inner;
				BeginCard(ScrollRight, 680.0f, Inner);
				DoPanelHeader(Inner, Localize("Tower Planner"));
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsTowerEnable, Localize("Enable tower planner"), &g_Config.m_ClZzAsyncBotsTowerEnable, &Inner, LineSize);
				CUIRect Row;
				g_Config.m_ClZzAsyncBotsTowerMode = 0;
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoLabel(&Row, Localize("Mode: Classic tower (16x16 removed)"), 13.0f, TEXTALIGN_ML);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerMaxBots, &g_Config.m_ClZzAsyncBotsTowerMaxBots, &Row, Localize("Max bots in structure"), 1, 128);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerCellSize, &g_Config.m_ClZzAsyncBotsTowerCellSize, &Row, Localize("Grid cell size"), 16, 64);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerSettleDist, &g_Config.m_ClZzAsyncBotsTowerSettleDist, &Row, Localize("Settle radius"), 4, 64);
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsTowerBoost, Localize("Allow boost approach"), &g_Config.m_ClZzAsyncBotsTowerBoost, &Inner, LineSize);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerMpcHorizonTicks, &g_Config.m_ClZzAsyncBotsTowerMpcHorizonTicks, &Row, Localize("MPC horizon ticks"), 6, 60);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerMpcCandidates, &g_Config.m_ClZzAsyncBotsTowerMpcCandidates, &Row, Localize("MPC candidates"), 8, 128);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerMpcWPosX, &g_Config.m_ClZzAsyncBotsTowerMpcWPosX, &Row, Localize("MPC W pos X"), 1, 100000);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerMpcWPosY, &g_Config.m_ClZzAsyncBotsTowerMpcWPosY, &Row, Localize("MPC W pos Y"), 1, 100000);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerMpcWVelX, &g_Config.m_ClZzAsyncBotsTowerMpcWVelX, &Row, Localize("MPC W vel X"), 1, 100000);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerMpcWVelY, &g_Config.m_ClZzAsyncBotsTowerMpcWVelY, &Row, Localize("MPC W vel Y"), 1, 100000);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerMpcPenaltyFallRisk, &g_Config.m_ClZzAsyncBotsTowerMpcPenaltyFallRisk, &Row, Localize("MPC fall risk penalty"), 0, 200000);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerMpcHookPlayerPenaltyTower, &g_Config.m_ClZzAsyncBotsTowerMpcHookPlayerPenaltyTower, &Row, Localize("Hook player penalty (tower)"), 0, 500000);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerMpcHookPlayerPenaltyGrid, &g_Config.m_ClZzAsyncBotsTowerMpcHookPlayerPenaltyGrid, &Row, Localize("Hook player penalty (16x16)"), 0, 500000);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerMpcBonusHookBlock, &g_Config.m_ClZzAsyncBotsTowerMpcBonusHookBlock, &Row, Localize("Hook block bonus"), 0, 20000);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerFinalSimTicks, &g_Config.m_ClZzAsyncBotsTowerFinalSimTicks, &Row, Localize("Final hold sim ticks"), 6, 20);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerHoldKpX, &g_Config.m_ClZzAsyncBotsTowerHoldKpX, &Row, Localize("Hold Kp X (x100)"), 1, 80);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerHoldKdX, &g_Config.m_ClZzAsyncBotsTowerHoldKdX, &Row, Localize("Hold Kd X (x100)"), 1, 200);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTowerHoldDeadzoneX, &g_Config.m_ClZzAsyncBotsTowerHoldDeadzoneX, &Row, Localize("Hold deadzone X"), 0, 10);

				// 16x16 grid editor removed by request.
			}

			if(s_BotsSubTab == 2)
			{
				static bool s_BotFlyExpanded = true;
				static float s_BotFlyPhase = 1.0f;
				static CButtonContainer s_BotFlyExpandButton;
				UpdateRevealPhase(s_BotFlyPhase, s_BotFlyExpanded);
				CUIRect Inner;
				CUIRect Row;
				const float CollapsedHeight = 30.0f;
				const float ExpandedExtra = 138.0f;
				const float PanelHeight = CollapsedHeight + ExpandedExtra * s_BotFlyPhase;
				BeginCard(ScrollLeft, PanelHeight, Inner);

				CUIRect Header;
				Inner.HSplitTop(18.0f, &Header, &Inner);
				CUIRect HeaderLabel, HeaderButton;
				Header.VSplitRight(92.0f, &HeaderLabel, &HeaderButton);
				Ui()->DoLabel(&HeaderLabel, Localize("Bot Fly (Triplefly)"), 16.0f, TEXTALIGN_ML);
				if(DoButton_Menu(&s_BotFlyExpandButton, s_BotFlyExpanded ? Localize("Collapse") : Localize("Expand"), 0, &HeaderButton))
					s_BotFlyExpanded = !s_BotFlyExpanded;
				Inner.HSplitTop(4.0f, nullptr, &Inner);

				if(s_BotFlyPhase > 0.001f)
				{
					CUIRect Expand;
					Inner.HSplitTop(ExpandedExtra * s_BotFlyPhase, &Expand, &Inner);
					DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzAsyncBotsTripleFly, Localize("Enable bot fly (triplefly)"), &g_Config.m_ClZzAsyncBotsTripleFly, &Expand, LineSize);
					Expand.HSplitTop(LineSize, &Row, &Expand);
					Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTripleFlyFov, &g_Config.m_ClZzAsyncBotsTripleFlyFov, &Row, Localize("Hook FOV"), 5, 360);
					Expand.HSplitTop(LineSize, &Row, &Expand);
					Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTripleFlyKeepHookDist, &g_Config.m_ClZzAsyncBotsTripleFlyKeepHookDist, &Row, Localize("Keep hook distance"), 1, 200);
					Expand.HSplitTop(LineSize, &Row, &Expand);
					Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsTripleFlyVelYThreshold, &g_Config.m_ClZzAsyncBotsTripleFlyVelYThreshold, &Row, Localize("Sharp velY threshold x100"), 10, 2000);
					Expand.HSplitTop(8.0f, nullptr, &Expand);
					Expand.HSplitTop(LineSize * 2.0f, &Label, &Expand);
					Ui()->DoLabel(&Label, Localize("Bots hook nearest target not above them, hold hook, and auto rehook by 0->1 pulse."), 14.0f, TEXTALIGN_ML);
				}
			}

			if(s_BotsSubTab == 1)
			{
				CUIRect Inner;
				char aBotSummary[96];
				FormatAsyncBotSummary(aBotSummary, sizeof(aBotSummary));
				const auto CountRealLines = [](const char *pText) {
					int Lines = 1;
					if(!pText || pText[0] == '\0')
						return Lines;
					for(const char *p = pText; *p; ++p)
					{
						if(*p == '\n')
							++Lines;
					}
					return Lines;
				};

				const int RealAsciiLines = CountRealLines(g_Config.m_ClZzAsyncBotsAsciiCard);
				const int VisibleAsciiLines = std::clamp(RealAsciiLines, 2, 10);
				const float AsciiInputH = LineSize * VisibleAsciiLines + 4.0f;
				BeginCard(ScrollLeft, 262.0f + AsciiInputH, Inner);
				DoPanelHeader(Inner, Localize("Control"));
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, Localize("Bots chat broadcast"), 14.0f, TEXTALIGN_ML);
				Inner.HSplitTop(LineSize, &Button, &Inner);
				static CLineInput s_AsyncBotsChat2;
				s_AsyncBotsChat2.SetBuffer(g_Config.m_ClZzAsyncBotsChat, sizeof(g_Config.m_ClZzAsyncBotsChat));
				s_AsyncBotsChat2.SetEmptyText("hello");
				Ui()->DoEditBox(&s_AsyncBotsChat2, &Button, 12.0f);
				Inner.HSplitTop(8.0f, nullptr, &Inner);
				CUIRect Row;
				Inner.HSplitTop(LineSize, &Row, &Inner);
				CUIRect BtnChat, Unused;
				Row.VSplitLeft(Row.w * 0.5f, &BtnChat, &Unused);
				static CButtonContainer s_AsyncBotsChatSend2;
				if(DoButton_Menu(&s_AsyncBotsChatSend2, Localize("Send chat"), 0, &BtnChat))
					GameClient()->AsyncProxyBots().QueueChatAll(g_Config.m_ClZzAsyncBotsChat);
				Inner.HSplitTop(8.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, Localize("ASCII card"), 14.0f, TEXTALIGN_ML);
				Inner.HSplitTop(AsciiInputH, &Button, &Inner);
				static CLineInput s_AsyncBotsAsciiCard;
				s_AsyncBotsAsciiCard.SetBuffer(g_Config.m_ClZzAsyncBotsAsciiCard, sizeof(g_Config.m_ClZzAsyncBotsAsciiCard));
				s_AsyncBotsAsciiCard.SetEmptyText("line 1\nline 2\nline 3");
				Ui()->DoEditBoxMultiline(&s_AsyncBotsAsciiCard, &Button, 12.0f);
				Inner.HSplitTop(8.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, Localize("Put each message on a new line and bots send them one by one."), 12.0f, TEXTALIGN_ML);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsAsciiCycles, &g_Config.m_ClZzAsyncBotsAsciiCycles, &Row, Localize("ASCII cycles (0 = infinite)"), 0, 100);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzAsyncBotsAsciiCooldown, &g_Config.m_ClZzAsyncBotsAsciiCooldown, &Row, Localize("ASCII cooldown (s)"), 1, 60);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				CUIRect BtnAsciiStart, BtnAsciiStop;
				Row.VSplitMid(&BtnAsciiStart, &BtnAsciiStop, 5.0f);
				static CButtonContainer s_AsyncBotsAsciiStart, s_AsyncBotsAsciiStop;
				if(DoButton_Menu(&s_AsyncBotsAsciiStart, Localize("Start ASCII"), 0, &BtnAsciiStart))
					GameClient()->AsyncProxyBots().StartAsciiBroadcast(g_Config.m_ClZzAsyncBotsAsciiCard, g_Config.m_ClZzAsyncBotsAsciiCycles, g_Config.m_ClZzAsyncBotsAsciiCooldown);
				if(DoButton_Menu(&s_AsyncBotsAsciiStop, Localize("Stop ASCII"), 0, &BtnAsciiStop))
					GameClient()->AsyncProxyBots().StopAsciiBroadcast();
				Inner.HSplitTop(12.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, aBotSummary, 14.0f, TEXTALIGN_ML);
				Inner.HSplitTop(4.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, GameClient()->AsyncProxyBots().StatusText(), 14.0f, TEXTALIGN_ML);
				Inner.HSplitTop(8.0f, nullptr, &Inner);
			}

			if(s_BotsSubTab == 1)
			{
				CUIRect Inner;
				char aBotSummary[96];
				FormatAsyncBotSummary(aBotSummary, sizeof(aBotSummary));
				BeginCard(ScrollRight, 126.0f, Inner);
				DoPanelHeader(Inner, Localize("Bots"));
				CUIRect Row;
				Inner.HSplitTop(LineSize, &Row, &Inner);
				CUIRect BtnConnect, BtnDisconnect;
				Row.VSplitMid(&BtnConnect, &BtnDisconnect, 5.0f);
				static CButtonContainer s_AsyncBotsConnectControl;
				static CButtonContainer s_AsyncBotsDisconnectControl;
				if(DoButton_Menu(&s_AsyncBotsConnectControl, Localize("Connect"), 0, &BtnConnect))
					GameClient()->AsyncProxyBots().StartFromConfig(GameClient());
				if(DoButton_Menu(&s_AsyncBotsDisconnectControl, Localize("Disconnect"), 0, &BtnDisconnect))
					GameClient()->AsyncProxyBots().StopAll();
				Inner.HSplitTop(8.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Row, &Inner);
				CUIRect BtnApply, BtnStop;
				Row.VSplitMid(&BtnApply, &BtnStop, 5.0f);
				static CButtonContainer s_AsyncBotsApply3;
				static CButtonContainer s_AsyncBotsStop3;
				if(DoButton_Menu(&s_AsyncBotsApply3, Localize("Apply live"), 0, &BtnApply))
					GameClient()->AsyncProxyBots().ApplyLiveSettings(GameClient());
				if(DoButton_Menu(&s_AsyncBotsStop3, Localize("Stop bots"), 0, &BtnStop))
					GameClient()->AsyncProxyBots().StopAll();
				Inner.HSplitTop(8.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, aBotSummary, 14.0f, TEXTALIGN_ML);
				Inner.HSplitTop(4.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, GameClient()->AsyncProxyBots().StatusText(), 14.0f, TEXTALIGN_ML);
			}

			if(s_BotsSubTab == 1)
			{
				CUIRect Inner;
				BeginCard(ScrollRight, 64.0f, Inner);
				DoPanelHeader(Inner, Localize("Controls"));
				CUIRect Row;
				Inner.HSplitTop(LineSize, &Row, &Inner);
				CUIRect BtnKill, BtnVoteYes, BtnVoteNo;
				Row.VSplitLeft(Row.w / 3.0f, &BtnKill, &Row);
				Row.VSplitLeft(Row.w / 2.0f, &BtnVoteYes, &BtnVoteNo);
				static CButtonContainer s_AsyncBotsKillAll2;
				static CButtonContainer s_AsyncBotsVoteYes2;
				static CButtonContainer s_AsyncBotsVoteNo2;
				if(DoButton_Menu(&s_AsyncBotsKillAll2, Localize("Kill bots"), 0, &BtnKill))
					GameClient()->AsyncProxyBots().KillAllIngame();
				if(DoButton_Menu(&s_AsyncBotsVoteYes2, Localize("Vote yes"), 0, &BtnVoteYes))
					GameClient()->AsyncProxyBots().VoteAll(1);
				if(DoButton_Menu(&s_AsyncBotsVoteNo2, Localize("Vote no"), 0, &BtnVoteNo))
					GameClient()->AsyncProxyBots().VoteAll(-1);
			}

			}

			EndCard();
			s_BotsScrollRegion.End();
			pFeatureScrollRegion = nullptr;
		}
	}
	else if(s_SubTab == ZZ_TAB_TAS)
	{
		const float LineSize = 20.0f;
		const float SectionGap = 16.0f;
		static CLineInput s_ReplayNameInput;
		static CButtonContainer s_PauseButton;
		static CButtonContainer s_PlaybackButton;
		static CButtonContainer s_RecordButton;
		static CButtonContainer s_ClearReplayButton;
		static CButtonContainer s_SaveReplayInline;
		static CButtonContainer s_ReloadReplayInline;
		static CButtonContainer s_ValidateReplay;
		static CButtonContainer s_ReplayTime;
		static CButtonContainer s_RemoveUseless;
		static CButtonContainer s_AutoSyncButton;
		static CButtonContainer s_SyncNowButton;
		static CButtonContainer s_AddTasFakeAimButton;
		static CButtonContainer s_TasPathColorReset;
		static CButtonContainer s_TasPredictionLocalReset;
		static CButtonContainer s_TasPredictionFrozenReset;
		static CButtonContainer s_TasPredictionOtherReset;
		static CUi::SDropDownState s_SavedReplayState;
		static CUi::SDropDownState s_TasFakeAimState;
		static CUi::SDropDownState s_TasPathModeState;
		static CUi::SDropDownState s_TasPredictionPathModeState;
		static CScrollRegion s_SavedReplayScroll;
		static CScrollRegion s_TasFakeAimScroll;
		static CScrollRegion s_TasPathModeScroll;
		static CScrollRegion s_TasPredictionPathModeScroll;
		static int s_SelectedReplay = 0;
		static CScrollRegion s_TasScrollRegion;
		static vec2 s_TasScrollOffset(0.0f, 0.0f);

		auto DoDoubleButtonRow = [&](CUIRect &View, CButtonContainer &LeftId, const char *pLeftLabel, CButtonContainer &RightId, const char *pRightLabel, bool *pLeftPressed = nullptr, bool *pRightPressed = nullptr, ColorRGBA LeftColor = ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), ColorRGBA RightColor = ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f)) {
			(void)LeftColor;
			(void)RightColor;
			CUIRect Row, LeftButton, RightButton;
			View.HSplitTop(LineSize, &Row, &View);
			Row.VSplitMid(&LeftButton, &RightButton, 5.0f);
			const bool LeftPressed = DoButton_Menu(&LeftId, pLeftLabel, 0, &LeftButton) != 0;
			const bool RightPressed = DoButton_Menu(&RightId, pRightLabel, 0, &RightButton) != 0;
			if(pLeftPressed)
				*pLeftPressed = LeftPressed;
			if(pRightPressed)
				*pRightPressed = RightPressed;
		};

		CScrollRegionParams TasScrollParams;
		TasScrollParams.m_ScrollUnit = 28.0f;
		s_TasScrollRegion.Begin(&MainView, &s_TasScrollOffset, &TasScrollParams);
		CUIRect ScrollView = MainView;
		ScrollView.y += s_TasScrollOffset.y;
		pFeatureScrollRegion = &s_TasScrollRegion;

		CUIRect Left, Right;
		ScrollView.VSplitMid(&Left, &Right, 20.0f);

		{
			CUIRect Inner;
			BeginCard(Left, 230.0f, Inner);
			DoPanelHeader(Inner, Localize("Main"));
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Ui()->DoScrollbarOption(&g_Config.m_ClZzTasTicksPerSecond, &g_Config.m_ClZzTasTicksPerSecond, &Button, "TPS", 1, 50);
			Inner.HSplitTop(LineSize, nullptr, &Inner);
			bool PausePressed = false;
			bool PlaybackPressed = false;
			DoDoubleButtonRow(Inner, s_PauseButton, g_Config.m_ClZzTasPause ? "Resume" : "Pause", s_PlaybackButton, g_Config.m_ClZzTasLoadReplay ? "Stop Playback" : "Playback Replay", &PausePressed, &PlaybackPressed, ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), g_Config.m_ClZzTasLoadReplay ? ColorRGBA(0.8f, 0.1f, 0.1f, 0.85f) : ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f));
			if(PausePressed)
				g_Config.m_ClZzTasPause ^= 1;
			if(PlaybackPressed)
				g_Config.m_ClZzTasLoadReplay ^= 1;
			Inner.HSplitTop(6.0f, nullptr, &Inner);
			bool RecordPressed = false;
			bool ClearPressed = false;
			DoDoubleButtonRow(Inner, s_RecordButton, g_Config.m_ClZzTasRecordReplay ? "Stop Recording" : "Record Replay", s_ClearReplayButton, "Clear Replay", &RecordPressed, &ClearPressed, g_Config.m_ClZzTasRecordReplay ? ColorRGBA(0.8f, 0.1f, 0.1f, 0.85f) : ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f));
			if(RecordPressed)
				g_Config.m_ClZzTasRecordReplay ^= 1;
			if(ClearPressed)
				g_Config.m_ClZzTasRespawn ^= 1;

			char aStatus[256];
			str_format(aStatus, sizeof(aStatus), "State: rec=%s play=%s frames=%d",
				GameClient()->m_ZzTas.IsRecording() ? "on" : "off",
				GameClient()->m_ZzTas.IsPlaying() ? "on" : "off",
				GameClient()->m_ZzTas.NumFrames());
			Inner.HSplitTop(8.0f, nullptr, &Inner);
			Inner.HSplitTop(LineSize, &Label, &Inner);
			Ui()->DoLabel(&Label, aStatus, 14.0f, TEXTALIGN_ML);
			if(GameClient()->m_ZzTas.HasStartPosition())
			{
				const vec2 StartPos = GameClient()->m_ZzTas.GetStartPosition();
				const float Dist = GameClient()->m_ZzTas.GetDistanceToStartPosition();
				char aStartInfo[256];
				str_format(aStartInfo, sizeof(aStartInfo), "Start: %.2f %.2f | dist=%.2f (need <= %.2f)",
					StartPos.x, StartPos.y, Dist, GameClient()->m_ZzTas.StartPositionTolerance());
				Inner.HSplitTop(4.0f, nullptr, &Inner);
				Inner.HSplitTop(LineSize, &Label, &Inner);
				Ui()->DoLabel(&Label, aStartInfo, 14.0f, TEXTALIGN_ML);
			}
		}

		Left.HSplitTop(SectionGap, nullptr, &Left);
		{
			CUIRect Inner;
			BeginCard(Left, 170.0f, Inner);
			DoPanelHeader(Inner, Localize("Settings"));
			static const struct
			{
				const char *m_pLabel;
				int *m_pValue;
			} s_aTasChecks[] = {
				{Localize("Enable Sound"), &g_Config.m_ClZzTasUseSound},
				{Localize("Enable Effects"), &g_Config.m_ClZzTasShowEffects},
				{Localize("Show Real Aim"), &g_Config.m_ClZzTasShowAim},
				{Localize("Predict Players"), &g_Config.m_ClZzTasPlayerPrediction},
				{Localize("Auto Replay"), &g_Config.m_ClZzTasAutoReplay},
				{Localize("Auto Save Replay"), &g_Config.m_ClZzTasAutoSave},
			};
			for(const auto &Entry : s_aTasChecks)
				DoButton_CheckBoxAutoVMarginAndSet(Entry.m_pValue, Entry.m_pLabel, Entry.m_pValue, &Inner, LineSize);
			if(g_Config.m_ClZzTasAutoSave)
			{
				Inner.HSplitTop(LineSize, &Button, &Inner);
				Ui()->DoScrollbarOption(&g_Config.m_ClZzTasAutoSaveInterval, &g_Config.m_ClZzTasAutoSaveInterval, &Button, Localize("Interval"), 1, 300, &CUi::ms_LinearScrollbarScale, 0, "s");
			}
		}

		Left.HSplitTop(SectionGap, nullptr, &Left);
		{
			CUIRect Inner;
			BeginCard(Left, 170.0f, Inner);
			DoPanelHeader(Inner, Localize("Hotkeys"));
			static CButtonContainer s_RecordReader;
			static CButtonContainer s_RecordClear;
			static CButtonContainer s_LoadReader;
			static CButtonContainer s_LoadClear;
			static CButtonContainer s_ClearReader;
			static CButtonContainer s_ClearClear;
			static CButtonContainer s_PauseReader;
			static CButtonContainer s_PauseClear;
			static CButtonContainer s_RewindReader;
			static CButtonContainer s_RewindClear;
			static CButtonContainer s_ForwardReader;
			static CButtonContainer s_ForwardClear;
			DoLine_KeyReader(Inner, s_RecordReader, s_RecordClear, "Record", "toggle zz_tas_record_replay 1 0");
			DoLine_KeyReader(Inner, s_LoadReader, s_LoadClear, "Load", "toggle zz_tas_load_replay 1 0");
			DoLine_KeyReader(Inner, s_ClearReader, s_ClearClear, "Clear", "+toggle zz_tas_respawn 1 0");
			DoLine_KeyReader(Inner, s_PauseReader, s_PauseClear, "Pause", "toggle zz_tas_pause 0 1");
			DoLine_KeyReader(Inner, s_RewindReader, s_RewindClear, "Rewind", "+toggle zz_tas_rewind 1 0");
			DoLine_KeyReader(Inner, s_ForwardReader, s_ForwardClear, "Forward", "+toggle zz_tas_forward 1 0");
		}

		{
			CUIRect Inner;
			BeginCard(Right, 210.0f, Inner);
			DoPanelHeader(Inner, Localize("Replay"));

			s_ReplayNameInput.SetBuffer(g_Config.m_ClZzTasReplayName, sizeof(g_Config.m_ClZzTasReplayName));
			s_ReplayNameInput.SetEmptyText("my_tas");

			CUIRect ReplayNameRow, ReplayNameBox, ReplaySaveButton, ReplayReloadButton;
			Inner.HSplitTop(LineSize, &ReplayNameRow, &Inner);
			ReplayNameRow.VSplitRight(70.0f, &ReplayNameRow, &ReplayReloadButton);
			ReplayNameRow.VSplitRight(5.0f, &ReplayNameRow, nullptr);
			ReplayNameRow.VSplitRight(90.0f, &ReplayNameRow, &ReplaySaveButton);
			ReplayNameBox = ReplayNameRow;
			Ui()->DoEditBox(&s_ReplayNameInput, &ReplayNameBox, 12.0f);
			if(DoButton_Menu(&s_SaveReplayInline, "Save", 0, &ReplaySaveButton))
				GameClient()->m_ZzTas.Save(g_Config.m_ClZzTasReplayName);
			if(DoButton_Menu(&s_ReloadReplayInline, "Reload", 0, &ReplayReloadButton))
				GameClient()->m_ZzTas.RefreshFileList();

			Inner.HSplitTop(6.0f, nullptr, &Inner);
			const auto &vFiles = GameClient()->m_ZzTas.Files();
			std::vector<const char *> vReplayNames;
			vReplayNames.reserve(vFiles.size());
			for(const auto &File : vFiles)
				vReplayNames.push_back(File.m_aFilename);
			if(vReplayNames.empty())
				vReplayNames.push_back("No replays found");
			s_SelectedReplay = std::clamp(s_SelectedReplay, 0, (int)vReplayNames.size() - 1);
			s_SavedReplayState.m_SelectionPopupContext.m_pScrollRegion = &s_SavedReplayScroll;
			CUIRect ReplayDropDownRow;
			Inner.HSplitTop(LineSize, &ReplayDropDownRow, &Inner);
			const int NewSelectedReplay = Ui()->DoDropDown(&ReplayDropDownRow, s_SelectedReplay, vReplayNames.data(), vReplayNames.size(), s_SavedReplayState);
			if(NewSelectedReplay != s_SelectedReplay)
			{
				s_SelectedReplay = NewSelectedReplay;
				if(!vFiles.empty())
				{
					str_copy(g_Config.m_ClZzTasLastFile, vFiles[s_SelectedReplay].m_aFilename, sizeof(g_Config.m_ClZzTasLastFile));
					str_copy(g_Config.m_ClZzTasReplayName, vFiles[s_SelectedReplay].m_aFilename, sizeof(g_Config.m_ClZzTasReplayName));
					GameClient()->m_ZzTas.Load(g_Config.m_ClZzTasReplayName);
				}
			}

			Inner.HSplitTop(6.0f, nullptr, &Inner);
			bool ValidatePressed = false;
			bool ReplayTimePressed = false;
			DoDoubleButtonRow(Inner, s_ValidateReplay, "Validate Replay", s_ReplayTime, "Get Replay Time", &ValidatePressed, &ReplayTimePressed);
			if(ValidatePressed)
				GameClient()->m_ZzTas.ValidateReplay();
			if(ReplayTimePressed)
				GameClient()->m_ZzTas.ReportReplayTime();
			Inner.HSplitTop(6.0f, nullptr, &Inner);
			CUIRect RemoveRow;
			Inner.HSplitTop(LineSize, &RemoveRow, &Inner);
			if(DoButton_Menu(&s_RemoveUseless, "Remove Useless", 0, &RemoveRow))
				GameClient()->m_ZzTas.RemoveUseless();
		}

		Right.HSplitTop(SectionGap, nullptr, &Right);
		{
			CUIRect Inner;
			BeginCard(Right, 68.0f, Inner);
			DoPanelHeader(Inner, Localize("Replay Vault"));
			bool AutoSyncPressed = false;
			bool SyncNowPressed = false;
			DoDoubleButtonRow(Inner, s_AutoSyncButton, g_Config.m_ClZzTasReplayVaultAutoSync ? "Auto Sync" : "Enable Auto Sync", s_SyncNowButton, "Sync Now", &AutoSyncPressed, &SyncNowPressed, ColorRGBA(0.2f, 0.65f, 0.2f, 0.85f), ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f));
			if(AutoSyncPressed)
				g_Config.m_ClZzTasReplayVaultAutoSync ^= 1;
			if(SyncNowPressed)
				GameClient()->m_ZzTas.SyncNow();
		}

		Right.HSplitTop(SectionGap, nullptr, &Right);
		{
			CUIRect Inner;
			BeginCard(Right, 114.0f, Inner);
			DoPanelHeader(Inner, Localize("Tools"));
			Inner.HSplitTop(LineSize, &Button, &Inner);
			Ui()->DoScrollbarOption(&g_Config.m_ClZzTasTickControlTicks, &g_Config.m_ClZzTasTickControlTicks, &Button, Localize("Ticks"), 1, 128);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzTasAutoRewind, Localize("Auto Rewind"), &g_Config.m_ClZzTasAutoRewind, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzTasAutoForward, Localize("Auto Forward"), &g_Config.m_ClZzTasAutoForward, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzTasTickControlPause, Localize("Auto Pause"), &g_Config.m_ClZzTasTickControlPause, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzTasTickControlStep, Localize("Step"), &g_Config.m_ClZzTasTickControlStep, &Inner, LineSize);
		}

		Right.HSplitTop(SectionGap, nullptr, &Right);
		{
			CUIRect Inner;
			BeginCard(Right, 84.0f, Inner);
			DoPanelHeader(Inner, Localize("Fake Aim"));
			static std::array<const char *, 4> s_aTasFakeAimModes = {Localize("Robot Aim"), Localize("Spinbot"), Localize("Random"), Localize("Smooth")};
			s_TasFakeAimState.m_SelectionPopupContext.m_pScrollRegion = &s_TasFakeAimScroll;
			Inner.HSplitTop(LineSize, &Button, &Inner);
			const int NewFakeAimMode = Ui()->DoDropDown(&Button, g_Config.m_ClZzTasFakeAimMode, s_aTasFakeAimModes.data(), s_aTasFakeAimModes.size(), s_TasFakeAimState);
			if(NewFakeAimMode != g_Config.m_ClZzTasFakeAimMode)
				g_Config.m_ClZzTasFakeAimMode = NewFakeAimMode;
			Inner.HSplitTop(6.0f, nullptr, &Inner);
			CUIRect AddFakeAimRow;
			Inner.HSplitTop(LineSize, &AddFakeAimRow, &Inner);
			if(DoButton_Menu(&s_AddTasFakeAimButton, "Add fake aim", 0, &AddFakeAimRow))
				g_Config.m_ClZzTasFakeAim = 1;
		}

		Right.HSplitTop(SectionGap, nullptr, &Right);
		{
			CUIRect Inner;
			BeginCard(Right, 170.0f, Inner);
			DoPanelHeader(Inner, Localize("Visuals"));
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzTasDrawStartEndPos, Localize("Draw start/end pos"), &g_Config.m_ClZzTasDrawStartEndPos, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzTasDrawPath, Localize("Draw replay path"), &g_Config.m_ClZzTasDrawPath, &Inner, LineSize);
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzTasDrawPredictionPath, Localize("Draw prediction path"), &g_Config.m_ClZzTasDrawPredictionPath, &Inner, LineSize);
			static std::array<const char *, 2> s_aTasPathModes = {Localize("Dotted"), Localize("Full Line")};
			if(g_Config.m_ClZzTasDrawPath)
			{
				DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClZzTasDrawPathSegmented, Localize("Segmented"), &g_Config.m_ClZzTasDrawPathSegmented, &Inner, LineSize);
				s_TasPathModeState.m_SelectionPopupContext.m_pScrollRegion = &s_TasPathModeScroll;
				Inner.HSplitTop(LineSize, &Button, &Inner);
				const int NewMode = Ui()->DoDropDown(&Button, g_Config.m_ClZzTasDrawPathMode, s_aTasPathModes.data(), s_aTasPathModes.size(), s_TasPathModeState);
				if(NewMode != g_Config.m_ClZzTasDrawPathMode)
					g_Config.m_ClZzTasDrawPathMode = NewMode;
				DoLine_ColorPicker(&s_TasPathColorReset, 25.0f, 13.0f, 5.0f, &Inner, Localize("Color"), &g_Config.m_ClZzTasDrawPathColor, ColorRGBA(0.0f, 1.0f, 0.0f), false);
			}
			if(g_Config.m_ClZzTasDrawPredictionPath)
			{
				s_TasPredictionPathModeState.m_SelectionPopupContext.m_pScrollRegion = &s_TasPredictionPathModeScroll;
				Inner.HSplitTop(LineSize, &Button, &Inner);
				const int NewMode = Ui()->DoDropDown(&Button, g_Config.m_ClZzTasDrawPredictionPathMode, s_aTasPathModes.data(), s_aTasPathModes.size(), s_TasPredictionPathModeState);
				if(NewMode != g_Config.m_ClZzTasDrawPredictionPathMode)
					g_Config.m_ClZzTasDrawPredictionPathMode = NewMode;
				DoLine_ColorPicker(&s_TasPredictionLocalReset, 25.0f, 13.0f, 5.0f, &Inner, Localize("Local Color"), &g_Config.m_ClZzTasDrawPredictionPathColor, ColorRGBA(0.0f, 1.0f, 0.0f), false);
				DoLine_ColorPicker(&s_TasPredictionFrozenReset, 25.0f, 13.0f, 5.0f, &Inner, Localize("Frozen Color"), &g_Config.m_ClZzTasDrawPredictionPathColorFrozen, ColorRGBA(0.0f, 1.0f, 1.0f), false);
			}
		}
		EndCard();
		s_TasScrollRegion.End();
		pFeatureScrollRegion = nullptr;
	}
	EndCard();
	TextRender()->TextColor(OldTextColor);
}

void CMenus::SetNeedSendInfo()
{
	if(m_Dummy)
		m_NeedSendDummyinfo = true;
	else
		m_NeedSendinfo = true;
}

void CMenus::RenderSettingsPlayer(CUIRect MainView)
{
	CUIRect TabBar, PlayerTab, DummyTab, ChangeInfo, QuickSearch;
	MainView.HSplitTop(20.0f, &TabBar, &MainView);
	TabBar.VSplitMid(&TabBar, &ChangeInfo, 20.f);
	TabBar.VSplitMid(&PlayerTab, &DummyTab);
	MainView.HSplitTop(10.0f, nullptr, &MainView);

	static CButtonContainer s_PlayerTabButton;
	if(DoButton_MenuTab(&s_PlayerTabButton, Localize("Player"), !m_Dummy, &PlayerTab, IGraphics::CORNER_L, nullptr, nullptr, nullptr, nullptr, 4.0f))
	{
		m_Dummy = false;
	}

	static CButtonContainer s_DummyTabButton;
	if(DoButton_MenuTab(&s_DummyTabButton, Localize("Dummy"), m_Dummy, &DummyTab, IGraphics::CORNER_R, nullptr, nullptr, nullptr, nullptr, 4.0f))
	{
		m_Dummy = true;
	}

	if(Client()->State() == IClient::STATE_ONLINE &&
		GameClient()->m_aNextChangeInfo[m_Dummy] > Client()->GameTick(m_Dummy))
	{
		char aChangeInfo[128], aTimeLeft[32];
		str_format(aTimeLeft, sizeof(aTimeLeft), Localize("%ds left"), (GameClient()->m_aNextChangeInfo[m_Dummy] - Client()->GameTick(m_Dummy) + Client()->GameTickSpeed() - 1) / Client()->GameTickSpeed());
		str_format(aChangeInfo, sizeof(aChangeInfo), "%s: %s", Localize("Player info change cooldown"), aTimeLeft);
		Ui()->DoLabel(&ChangeInfo, aChangeInfo, 10.f, TEXTALIGN_ML);
	}

	static CLineInput s_NameInput;
	static CLineInput s_ClanInput;

	int *pCountry;
	if(!m_Dummy)
	{
		pCountry = &g_Config.m_PlayerCountry;
		s_NameInput.SetBuffer(g_Config.m_PlayerName, sizeof(g_Config.m_PlayerName));
		s_NameInput.SetEmptyText(Client()->PlayerName());
		s_ClanInput.SetBuffer(g_Config.m_PlayerClan, sizeof(g_Config.m_PlayerClan));
	}
	else
	{
		pCountry = &g_Config.m_ClDummyCountry;
		s_NameInput.SetBuffer(g_Config.m_ClDummyName, sizeof(g_Config.m_ClDummyName));
		s_NameInput.SetEmptyText(Client()->DummyName());
		s_ClanInput.SetBuffer(g_Config.m_ClDummyClan, sizeof(g_Config.m_ClDummyClan));
	}

	// player name
	CUIRect Button, Label;
	MainView.HSplitTop(20.0f, &Button, &MainView);
	Button.VSplitLeft(80.0f, &Label, &Button);
	Button.VSplitLeft(150.0f, &Button, nullptr);
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "%s:", Localize("Name"));
	Ui()->DoLabel(&Label, aBuf, 14.0f, TEXTALIGN_ML);
	if(Ui()->DoEditBox(&s_NameInput, &Button, 14.0f))
	{
		SetNeedSendInfo();
	}

	// player clan
	MainView.HSplitTop(5.0f, nullptr, &MainView);
	MainView.HSplitTop(20.0f, &Button, &MainView);
	Button.VSplitLeft(80.0f, &Label, &Button);
	Button.VSplitLeft(150.0f, &Button, nullptr);
	str_format(aBuf, sizeof(aBuf), "%s:", Localize("Clan"));
	Ui()->DoLabel(&Label, aBuf, 14.0f, TEXTALIGN_ML);
	if(Ui()->DoEditBox(&s_ClanInput, &Button, 14.0f))
	{
		SetNeedSendInfo();
	}

	// country flag selector
	static CLineInputBuffered<25> s_FlagFilterInput;

	std::vector<const CCountryFlags::CCountryFlag *> vpFilteredFlags;
	for(size_t i = 0; i < GameClient()->m_CountryFlags.Num(); ++i)
	{
		const CCountryFlags::CCountryFlag &Entry = GameClient()->m_CountryFlags.GetByIndex(i);
		if(!str_find_nocase(Entry.m_aCountryCodeString, s_FlagFilterInput.GetString()))
			continue;
		vpFilteredFlags.push_back(&Entry);
	}

	MainView.HSplitTop(10.0f, nullptr, &MainView);
	MainView.HSplitBottom(20.0f, &MainView, &QuickSearch);
	MainView.HSplitBottom(5.0f, &MainView, nullptr);
	QuickSearch.VSplitLeft(220.0f, &QuickSearch, nullptr);

	int SelectedOld = -1;
	static CListBox s_ListBox;
	s_ListBox.DoStart(48.0f, vpFilteredFlags.size(), 10, 3, SelectedOld, &MainView);

	for(size_t i = 0; i < vpFilteredFlags.size(); i++)
	{
		const CCountryFlags::CCountryFlag *pEntry = vpFilteredFlags[i];

		if(pEntry->m_CountryCode == *pCountry)
			SelectedOld = i;

		const CListboxItem Item = s_ListBox.DoNextItem(&pEntry->m_CountryCode, SelectedOld >= 0 && (size_t)SelectedOld == i);
		if(!Item.m_Visible)
			continue;

		CUIRect FlagRect;
		Item.m_Rect.Margin(5.0f, &FlagRect);
		FlagRect.HSplitBottom(12.0f, &FlagRect, &Label);
		Label.HSplitTop(2.0f, nullptr, &Label);
		const float OldWidth = FlagRect.w;
		FlagRect.w = FlagRect.h * 2;
		FlagRect.x += (OldWidth - FlagRect.w) / 2.0f;
		GameClient()->m_CountryFlags.Render(pEntry->m_CountryCode, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), FlagRect.x, FlagRect.y, FlagRect.w, FlagRect.h);

		if(pEntry->m_Texture.IsValid())
		{
			Ui()->DoLabel(&Label, pEntry->m_aCountryCodeString, 10.0f, TEXTALIGN_MC);
		}
	}

	const int NewSelected = s_ListBox.DoEnd();
	if(SelectedOld != NewSelected)
	{
		*pCountry = vpFilteredFlags[NewSelected]->m_CountryCode;
		SetNeedSendInfo();
	}

	Ui()->DoEditBox_Search(&s_FlagFilterInput, &QuickSearch, 14.0f, !Ui()->IsPopupOpen() && !GameClient()->m_GameConsole.IsActive());
}

void CMenus::RenderSettingsTee(CUIRect MainView)
{
	CUIRect TabBar, PlayerTab, DummyTab, ChangeInfo;
	MainView.HSplitTop(20.0f, &TabBar, &MainView);
	TabBar.VSplitMid(&TabBar, &ChangeInfo, 20.f);
	TabBar.VSplitMid(&PlayerTab, &DummyTab);
	MainView.HSplitTop(10.0f, nullptr, &MainView);

	static CButtonContainer s_PlayerTabButton;
	if(DoButton_MenuTab(&s_PlayerTabButton, Localize("Player"), !m_Dummy, &PlayerTab, IGraphics::CORNER_L, nullptr, nullptr, nullptr, nullptr, 4.0f))
	{
		m_Dummy = false;
		m_SkinListScrollToSelected = true;
	}

	static CButtonContainer s_DummyTabButton;
	if(DoButton_MenuTab(&s_DummyTabButton, Localize("Dummy"), m_Dummy, &DummyTab, IGraphics::CORNER_R, nullptr, nullptr, nullptr, nullptr, 4.0f))
	{
		m_Dummy = true;
		m_SkinListScrollToSelected = true;
	}

	if(Client()->State() == IClient::STATE_ONLINE &&
		GameClient()->m_aNextChangeInfo[m_Dummy] > Client()->GameTick(m_Dummy))
	{
		char aChangeInfo[128], aTimeLeft[32];
		str_format(aTimeLeft, sizeof(aTimeLeft), Localize("%ds left"), (GameClient()->m_aNextChangeInfo[m_Dummy] - Client()->GameTick(m_Dummy) + Client()->GameTickSpeed() - 1) / Client()->GameTickSpeed());
		str_format(aChangeInfo, sizeof(aChangeInfo), "%s: %s", Localize("Player info change cooldown"), aTimeLeft);
		Ui()->DoLabel(&ChangeInfo, aChangeInfo, 10.f, TEXTALIGN_ML);
	}

	if(g_Config.m_Debug)
	{
		const CSkins::CSkinLoadingStats Stats = GameClient()->m_Skins.LoadingStats();
		char aStats[256];
		str_format(aStats, sizeof(aStats), "unloaded: %" PRIzu ", pending: %" PRIzu ", loading: %" PRIzu ",\nloaded: %" PRIzu ", error: %" PRIzu ", notfound: %" PRIzu,
			Stats.m_NumUnloaded, Stats.m_NumPending, Stats.m_NumLoading, Stats.m_NumLoaded, Stats.m_NumError, Stats.m_NumNotFound);
		Ui()->DoLabel(&ChangeInfo, aStats, 9.0f, TEXTALIGN_MR);
	}

	char *pSkinName;
	size_t SkinNameSize;
	int *pUseCustomColor;
	unsigned *pColorBody;
	unsigned *pColorFeet;
	int *pEmote;
	if(!m_Dummy)
	{
		pSkinName = g_Config.m_ClPlayerSkin;
		SkinNameSize = sizeof(g_Config.m_ClPlayerSkin);
		pUseCustomColor = &g_Config.m_ClPlayerUseCustomColor;
		pColorBody = &g_Config.m_ClPlayerColorBody;
		pColorFeet = &g_Config.m_ClPlayerColorFeet;
		pEmote = &g_Config.m_ClPlayerDefaultEyes;
	}
	else
	{
		pSkinName = g_Config.m_ClDummySkin;
		SkinNameSize = sizeof(g_Config.m_ClDummySkin);
		pUseCustomColor = &g_Config.m_ClDummyUseCustomColor;
		pColorBody = &g_Config.m_ClDummyColorBody;
		pColorFeet = &g_Config.m_ClDummyColorFeet;
		pEmote = &g_Config.m_ClDummyDefaultEyes;
	}

	const float EyeButtonSize = 40.0f;
	const bool RenderEyesBelow = MainView.w < 750.0f;
	CUIRect YourSkin, Checkboxes, SkinPrefix, Eyes, Button, Label;
	MainView.HSplitTop(90.0f, &YourSkin, &MainView);
	if(RenderEyesBelow)
	{
		YourSkin.VSplitLeft(MainView.w * 0.45f, &YourSkin, &Checkboxes);
		Checkboxes.VSplitLeft(MainView.w * 0.35f, &Checkboxes, &SkinPrefix);
		MainView.HSplitTop(5.0f, nullptr, &MainView);
		MainView.HSplitTop(EyeButtonSize, &Eyes, &MainView);
		Eyes.VSplitRight(EyeButtonSize * (float)NUM_EMOTES + 5.0f * (float)(NUM_EMOTES - 1), nullptr, &Eyes);
	}
	else
	{
		YourSkin.VSplitRight(3 * EyeButtonSize + 2 * 5.0f, &YourSkin, &Eyes);
		const float RemainderWidth = YourSkin.w;
		YourSkin.VSplitLeft(RemainderWidth * 0.4f, &YourSkin, &Checkboxes);
		Checkboxes.VSplitLeft(RemainderWidth * 0.35f, &Checkboxes, &SkinPrefix);
		SkinPrefix.VSplitRight(20.0f, &SkinPrefix, nullptr);
	}
	YourSkin.VSplitRight(20.0f, &YourSkin, nullptr);
	Checkboxes.VSplitRight(20.0f, &Checkboxes, nullptr);

	// Checkboxes
	bool ShouldRefresh = false;
	Checkboxes.HSplitTop(20.0f, &Button, &Checkboxes);
	if(DoButton_CheckBox(&g_Config.m_ClDownloadSkins, Localize("Download skins"), g_Config.m_ClDownloadSkins, &Button))
	{
		g_Config.m_ClDownloadSkins ^= 1;
		ShouldRefresh = true;
	}

	Checkboxes.HSplitTop(20.0f, &Button, &Checkboxes);
	if(DoButton_CheckBox(&g_Config.m_ClDownloadCommunitySkins, Localize("Download community skins"), g_Config.m_ClDownloadCommunitySkins, &Button))
	{
		g_Config.m_ClDownloadCommunitySkins ^= 1;
		ShouldRefresh = true;
	}

	Checkboxes.HSplitTop(20.0f, &Button, &Checkboxes);
	if(DoButton_CheckBox(&g_Config.m_ClVanillaSkinsOnly, Localize("Vanilla skins only"), g_Config.m_ClVanillaSkinsOnly, &Button))
	{
		g_Config.m_ClVanillaSkinsOnly ^= 1;
		ShouldRefresh = true;
	}

	Checkboxes.HSplitTop(20.0f, &Button, &Checkboxes);
	if(DoButton_CheckBox(&g_Config.m_ClFatSkins, Localize("Fat skins (DDFat)"), g_Config.m_ClFatSkins, &Button))
	{
		g_Config.m_ClFatSkins ^= 1;
	}

	// Skin prefix
	{
		SkinPrefix.HSplitTop(20.0f, &Label, &SkinPrefix);
		Ui()->DoLabel(&Label, Localize("Skin prefix"), 14.0f, TEXTALIGN_ML);

		SkinPrefix.HSplitTop(20.0f, &Button, &SkinPrefix);
		static CLineInput s_SkinPrefixInput(g_Config.m_ClSkinPrefix, sizeof(g_Config.m_ClSkinPrefix));
		if(Ui()->DoClearableEditBox(&s_SkinPrefixInput, &Button, 14.0f))
		{
			ShouldRefresh = true;
		}

		SkinPrefix.HSplitTop(2.0f, nullptr, &SkinPrefix);

		static const char *s_apSkinPrefixes[] = {"kitty", "santa"};
		static CButtonContainer s_aPrefixButtons[std::size(s_apSkinPrefixes)];
		for(size_t i = 0; i < std::size(s_apSkinPrefixes); i++)
		{
			SkinPrefix.HSplitTop(20.0f, &Button, &SkinPrefix);
			Button.HMargin(2.0f, &Button);
			if(DoButton_Menu(&s_aPrefixButtons[i], s_apSkinPrefixes[i], 0, &Button))
			{
				str_copy(g_Config.m_ClSkinPrefix, s_apSkinPrefixes[i]);
				ShouldRefresh = true;
			}
		}
	}
	CUIRect RandomColorsButton;

	// Player skin area
	CUIRect CustomColorsButton, RandomSkinButton;
	YourSkin.HSplitTop(20.0f, &Label, &YourSkin);
	YourSkin.HSplitBottom(20.0f, &YourSkin, &CustomColorsButton);

	CustomColorsButton.VSplitRight(30.0f, &CustomColorsButton, &RandomSkinButton);
	CustomColorsButton.VSplitRight(3.0f, &CustomColorsButton, 0);

	CustomColorsButton.VSplitRight(110.0f, &CustomColorsButton, &RandomColorsButton);

	CustomColorsButton.VSplitRight(5.0f, &CustomColorsButton, nullptr);
	YourSkin.VSplitLeft(65.0f, &YourSkin, &Button);
	Button.VSplitLeft(5.0f, nullptr, &Button);
	Button.HMargin((Button.h - 20.0f) / 2.0f, &Button);

	char aBuf[128 + IO_MAX_PATH_LENGTH];
	str_format(aBuf, sizeof(aBuf), "%s:", Localize("Your skin"));
	Ui()->DoLabel(&Label, aBuf, 14.0f, TEXTALIGN_ML);

	CSkins::CSkinList &SkinList = GameClient()->m_Skins.SkinList();
	const CSkin *pDefaultSkin = GameClient()->m_Skins.Find("default");
	const CSkins::CSkinContainer *pOwnSkinContainer = GameClient()->m_Skins.FindContainerOrNullptr(pSkinName[0] == '\0' ? "default" : pSkinName);
	if(pOwnSkinContainer != nullptr && pOwnSkinContainer->IsSpecial())
	{
		pOwnSkinContainer = nullptr; // Special skins cannot be selected, show as missing due to invalid name
	}

	CTeeRenderInfo OwnSkinInfo;
	OwnSkinInfo.Apply(pOwnSkinContainer == nullptr || pOwnSkinContainer->Skin() == nullptr ? pDefaultSkin : pOwnSkinContainer->Skin().get());
	OwnSkinInfo.ApplyColors(*pUseCustomColor, *pColorBody, *pColorFeet);
	OwnSkinInfo.m_Size = 50.0f;

	// Tee
	{
		vec2 OffsetToMid;
		CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &OwnSkinInfo, OffsetToMid);
		const vec2 TeeRenderPos = vec2(YourSkin.x + YourSkin.w / 2.0f, YourSkin.y + YourSkin.h / 2.0f + OffsetToMid.y);
		// tee looking towards cursor, and it is happy when you touch it
		const vec2 DeltaPosition = Ui()->MousePos() - TeeRenderPos;
		const float Distance = length(DeltaPosition);
		const float InteractionDistance = 20.0f;
		const vec2 TeeDirection = Distance < InteractionDistance ? normalize(vec2(DeltaPosition.x, maximum(DeltaPosition.y, 0.5f))) : normalize(DeltaPosition);
		const int TeeEmote = Distance < InteractionDistance ? EMOTE_HAPPY : *pEmote;
		RenderTools()->RenderTee(CAnimState::GetIdle(), &OwnSkinInfo, TeeEmote, TeeDirection, TeeRenderPos);
	}

	// Skin loading status
	const auto &&RenderSkinStatus = [&](CUIRect Parent, const CSkins::CSkinContainer *pSkinContainer, const void *pStatusTooltipId) {
		if(pSkinContainer != nullptr && pSkinContainer->State() == CSkins::CSkinContainer::EState::LOADED)
		{
			return;
		}

		CUIRect StatusIcon;
		Parent.HSplitTop(20.0f, &StatusIcon, nullptr);
		StatusIcon.VSplitLeft(20.0f, &StatusIcon, nullptr);

		if(pSkinContainer != nullptr &&
			(pSkinContainer->State() == CSkins::CSkinContainer::EState::UNLOADED ||
				pSkinContainer->State() == CSkins::CSkinContainer::EState::PENDING ||
				pSkinContainer->State() == CSkins::CSkinContainer::EState::LOADING))
		{
			Ui()->RenderProgressSpinner(StatusIcon.Center(), 5.0f);
		}
		else
		{
			TextRender()->TextColor(ColorRGBA(1.0f, 0.0f, 0.0f, 1.0f));
			TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
			TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT | ETextRenderFlags::TEXT_RENDER_FLAG_NO_OVERSIZE);
			Ui()->DoLabel(&StatusIcon, pSkinContainer == nullptr || pSkinContainer->State() == CSkins::CSkinContainer::EState::ERROR ? FontIcon::TRIANGLE_EXCLAMATION : FontIcon::QUESTION, 12.0f, TEXTALIGN_MC);
			TextRender()->SetRenderFlags(0);
			TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
			TextRender()->TextColor(TextRender()->DefaultTextColor());
			Ui()->DoButtonLogic(pStatusTooltipId, 0, &StatusIcon, BUTTONFLAG_NONE);
			const char *pErrorTooltip;
			if(pSkinContainer == nullptr)
			{
				pErrorTooltip = Localize("This skin name cannot be used.");
			}
			else if(pSkinContainer->State() == CSkins::CSkinContainer::EState::ERROR)
			{
				pErrorTooltip = Localize("Skin could not be loaded due to an error. Check the local console for details.");
			}
			else
			{
				pErrorTooltip = Localize("Skin could not be found.");
			}
			GameClient()->m_Tooltips.DoToolTip(pStatusTooltipId, &StatusIcon, pErrorTooltip);
		}
	};
	static char s_StatusTooltipId;
	RenderSkinStatus(YourSkin, pOwnSkinContainer, &s_StatusTooltipId);

	// Skin name
	static CLineInput s_SkinInput;
	s_SkinInput.SetBuffer(pSkinName, SkinNameSize);
	s_SkinInput.SetEmptyText("default");
	if(Ui()->DoClearableEditBox(&s_SkinInput, &Button, 14.0f))
	{
		SetNeedSendInfo();
		m_SkinListScrollToSelected = true;
		SkinList.ForceRefresh();
	}

	// Random skin button
	static CButtonContainer s_RandomSkinButton;
	static const char *s_apDice[] = {FontIcon::DICE_ONE, FontIcon::DICE_TWO, FontIcon::DICE_THREE, FontIcon::DICE_FOUR, FontIcon::DICE_FIVE, FontIcon::DICE_SIX};
	static int s_CurrentDie = rand() % std::size(s_apDice);
	TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
	TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT | ETextRenderFlags::TEXT_RENDER_FLAG_NO_OVERSIZE);
	if(DoButton_Menu(&s_RandomSkinButton, s_apDice[s_CurrentDie], 0, &RandomSkinButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 5.0f, -0.2f))
	{
		GameClient()->m_Skins.RandomizeSkin(m_Dummy);
		SetNeedSendInfo();
		m_SkinListScrollToSelected = true;
		s_CurrentDie = rand() % std::size(s_apDice);
	}
	TextRender()->SetRenderFlags(0);
	TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
	GameClient()->m_Tooltips.DoToolTip(&s_RandomSkinButton, &RandomSkinButton, Localize("Create a random skin"));

	static CButtonContainer s_RandomizeColors;
	if(*pUseCustomColor)
	{
		// RandomColorsButton.VSplitLeft(120.0f, &RandomColorsButton, 0);
		if(DoButton_Menu(&s_RandomizeColors, "Random Colors", 0, &RandomColorsButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 5.0f, 0.0f, ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f)))
		{
			if(m_Dummy)
			{
				g_Config.m_ClDummyColorBody = ColorHSLA((std::rand() % 100) / 100.0f, (std::rand() % 100) / 100.0f, (std::rand() % 100) / 100.0f, 1).Pack(false);
				g_Config.m_ClDummyColorFeet = ColorHSLA((std::rand() % 100) / 100.0f, (std::rand() % 100) / 100.0f, (std::rand() % 100) / 100.0f, 1).Pack(false);
			}
			else
			{
				g_Config.m_ClPlayerColorBody = ColorHSLA((std::rand() % 100) / 100.0f, (std::rand() % 100) / 100.0f, (std::rand() % 100) / 100.0f, 1).Pack(false);
				g_Config.m_ClPlayerColorFeet = ColorHSLA((std::rand() % 100) / 100.0f, (std::rand() % 100) / 100.0f, (std::rand() % 100) / 100.0f, 1).Pack(false);
			}
			SetNeedSendInfo();
		}
	}
	MainView.HSplitTop(5.0f, 0, &MainView);

	// Custom colors button
	if(DoButton_CheckBox(pUseCustomColor, Localize("Custom colors"), *pUseCustomColor, &CustomColorsButton))
	{
		*pUseCustomColor = *pUseCustomColor ? 0 : 1;
		SetNeedSendInfo();
	}

	// Default eyes
	{
		CTeeRenderInfo EyeSkinInfo = OwnSkinInfo;
		EyeSkinInfo.m_Size = EyeButtonSize;
		vec2 OffsetToMid;
		CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &EyeSkinInfo, OffsetToMid);

		CUIRect EyesRow;
		Eyes.HSplitTop(EyeButtonSize, &EyesRow, &Eyes);
		static CButtonContainer s_aEyeButtons[NUM_EMOTES];
		for(int CurrentEyeEmote = 0; CurrentEyeEmote < NUM_EMOTES; CurrentEyeEmote++)
		{
			EyesRow.VSplitLeft(EyeButtonSize, &Button, &EyesRow);
			EyesRow.VSplitLeft(5.0f, nullptr, &EyesRow);
			if(!RenderEyesBelow && (CurrentEyeEmote + 1) % 3 == 0)
			{
				Eyes.HSplitTop(5.0f, nullptr, &Eyes);
				Eyes.HSplitTop(EyeButtonSize, &EyesRow, &Eyes);
			}

			const ColorRGBA EyeButtonColor = ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f + (*pEmote == CurrentEyeEmote ? 0.25f : 0.0f));
			if(DoButton_Menu(&s_aEyeButtons[CurrentEyeEmote], "", 0, &Button, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 5.0f, 0.0f, EyeButtonColor))
			{
				*pEmote = CurrentEyeEmote;
				if((int)m_Dummy == g_Config.m_ClDummy)
					GameClient()->m_Emoticon.EyeEmote(CurrentEyeEmote);
			}
			GameClient()->m_Tooltips.DoToolTip(&s_aEyeButtons[CurrentEyeEmote], &Button, Localize("Choose default eyes when joining a server"));
			RenderTools()->RenderTee(CAnimState::GetIdle(), &EyeSkinInfo, CurrentEyeEmote, vec2(1.0f, 0.0f), vec2(Button.x + Button.w / 2.0f, Button.y + Button.h / 2.0f + OffsetToMid.y));
		}
	}

	// Custom color pickers
	MainView.HSplitTop(5.0f, nullptr, &MainView);
	if(*pUseCustomColor)
	{
		CUIRect CustomColors;
		MainView.HSplitTop(95.0f, &CustomColors, &MainView);
		CUIRect aRects[2];
		CustomColors.VSplitMid(&aRects[0], &aRects[1], 20.0f);

		unsigned *apColors[] = {pColorBody, pColorFeet};
		const char *apParts[] = {Localize("Body"), Localize("Feet")};

		for(int i = 0; i < 2; i++)
		{
			aRects[i].HSplitTop(20.0f, &Label, &aRects[i]);
			Ui()->DoLabel(&Label, apParts[i], 14.0f, TEXTALIGN_ML);
			if(RenderHslaScrollbars(&aRects[i], apColors[i], false, ColorHSLA::DARKEST_LGT))
			{
				SetNeedSendInfo();
			}
		}
	}
	MainView.HSplitTop(5.0f, nullptr, &MainView);

	// Layout bottom controls and use remainder for skin selector
	CUIRect QuickSearch, DatabaseButton, DirectoryButton, RefreshButton;
	MainView.HSplitBottom(20.0f, &MainView, &QuickSearch);
	MainView.HSplitBottom(5.0f, &MainView, nullptr);
	QuickSearch.VSplitLeft(220.0f, &QuickSearch, &DatabaseButton);
	DatabaseButton.VSplitLeft(10.0f, nullptr, &DatabaseButton);
	DatabaseButton.VSplitLeft(150.0f, &DatabaseButton, &DirectoryButton);
	DirectoryButton.VSplitRight(175.0f, nullptr, &DirectoryButton);
	DirectoryButton.VSplitRight(25.0f, &DirectoryButton, &RefreshButton);
	DirectoryButton.VSplitRight(10.0f, &DirectoryButton, nullptr);

	// Skin selector
	static CListBox s_ListBox;
	std::vector<CSkins::CSkinListEntry> &vSkinList = SkinList.Skins();
	int OldSelected = -1;
	s_ListBox.DoStart(50.0f, vSkinList.size(), 4, 2, OldSelected, &MainView);
	for(size_t i = 0; i < vSkinList.size(); ++i)
	{
		CSkins::CSkinListEntry &SkinListEntry = vSkinList[i];
		const CSkins::CSkinContainer *pSkinContainer = vSkinList[i].SkinContainer();

		if(!m_Dummy ? SkinListEntry.IsSelectedMain() : SkinListEntry.IsSelectedDummy())
		{
			OldSelected = i;
			if(m_SkinListScrollToSelected)
			{
				s_ListBox.ScrollToSelected();
				m_SkinListScrollToSelected = false;
			}
		}

		const CListboxItem Item = s_ListBox.DoNextItem(SkinListEntry.ListItemId(), OldSelected >= 0 && (size_t)OldSelected == i);
		if(!Item.m_Visible)
		{
			continue;
		}

		SkinListEntry.RequestLoad();
		const CSkin *pSkin = pSkinContainer->State() == CSkins::CSkinContainer::EState::LOADED ? pSkinContainer->Skin().get() : pDefaultSkin;

		Item.m_Rect.VSplitLeft(60.0f, &Button, &Label);

		{
			CTeeRenderInfo Info = OwnSkinInfo;
			Info.Apply(pSkin);
			vec2 OffsetToMid;
			CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &Info, OffsetToMid);
			const vec2 TeeRenderPos = vec2(Button.x + Button.w / 2.0f, Button.y + Button.h / 2 + OffsetToMid.y);
			RenderTools()->RenderTee(CAnimState::GetIdle(), &Info, *pEmote, vec2(1.0f, 0.0f), TeeRenderPos);
		}

		{
			SLabelProperties Props;
			Props.m_MaxWidth = Label.w - 5.0f;
			const auto &NameMatch = SkinListEntry.NameMatch();
			if(NameMatch.has_value())
			{
				const auto [MatchStart, MatchLength] = NameMatch.value();
				Props.m_vColorSplits.emplace_back(MatchStart, MatchLength, ColorRGBA(0.4f, 0.4f, 1.0f, 1.0f));
			}
			Ui()->DoLabel(&Label, pSkinContainer->Name(), 12.0f, TEXTALIGN_ML, Props);
		}

		if(g_Config.m_Debug)
		{
			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			Graphics()->SetColor(*pUseCustomColor ? color_cast<ColorRGBA>(ColorHSLA(*pColorBody).UnclampLighting(ColorHSLA::DARKEST_LGT)) : pSkin->m_BloodColor);
			IGraphics::CQuadItem QuadItem(Label.x, Label.y, 12.0f, 12.0f);
			Graphics()->QuadsDrawTL(&QuadItem, 1);
			Graphics()->QuadsEnd();
		}

		// render skin favorite icon
		{
			CUIRect FavIcon;
			Item.m_Rect.HSplitTop(20.0f, &FavIcon, nullptr);
			FavIcon.VSplitRight(20.0f, nullptr, &FavIcon);
			if(DoButton_Favorite(SkinListEntry.FavoriteButtonId(), SkinListEntry.ListItemId(), SkinListEntry.IsFavorite(), &FavIcon))
			{
				if(SkinListEntry.IsFavorite())
				{
					GameClient()->m_Skins.RemoveFavorite(pSkinContainer->Name());
				}
				else
				{
					GameClient()->m_Skins.AddFavorite(pSkinContainer->Name());
				}
			}
		}

		RenderSkinStatus(Item.m_Rect, pSkinContainer, SkinListEntry.ErrorTooltipId());
	}

	const int NewSelected = s_ListBox.DoEnd();
	if(OldSelected != NewSelected)
	{
		str_copy(pSkinName, vSkinList[NewSelected].SkinContainer()->Name(), SkinNameSize);
		SkinList.ForceRefresh();
		SetNeedSendInfo();
	}

	static CLineInput s_SkinFilterInput(g_Config.m_ClSkinFilterString, sizeof(g_Config.m_ClSkinFilterString));
	if(SkinList.UnfilteredCount() > 0 && vSkinList.empty())
	{
		CUIRect FilterLabel, ResetButton;
		MainView.HMargin((MainView.h - (16.0f + 18.0f + 8.0f)) / 2.0f, &FilterLabel);
		FilterLabel.HSplitTop(16.0f, &FilterLabel, &ResetButton);
		ResetButton.HSplitTop(8.0f, nullptr, &ResetButton);
		ResetButton.VMargin((ResetButton.w - 200.0f) / 2.0f, &ResetButton);
		Ui()->DoLabel(&FilterLabel, Localize("No skins match your filter criteria"), 16.0f, TEXTALIGN_MC);
		static CButtonContainer s_ResetButton;
		if(DoButton_Menu(&s_ResetButton, Localize("Reset filter"), 0, &ResetButton))
		{
			s_SkinFilterInput.Clear();
			SkinList.ForceRefresh();
		}
	}

	if(Ui()->DoEditBox_Search(&s_SkinFilterInput, &QuickSearch, 14.0f, !Ui()->IsPopupOpen() && !GameClient()->m_GameConsole.IsActive()))
	{
		SkinList.ForceRefresh();
	}

	static CButtonContainer s_SkinDatabaseButton;
	if(DoButton_Menu(&s_SkinDatabaseButton, Localize("Skin Database"), 0, &DatabaseButton))
	{
		Client()->ViewLink("https://ddnet.org/skins/");
	}

	static CButtonContainer s_DirectoryButton;
	if(DoButton_Menu(&s_DirectoryButton, Localize("Skins directory"), 0, &DirectoryButton))
	{
		Storage()->GetCompletePath(IStorage::TYPE_SAVE, "skins", aBuf, sizeof(aBuf));
		Storage()->CreateFolder("skins", IStorage::TYPE_SAVE);
		Client()->ViewFile(aBuf);
	}
	GameClient()->m_Tooltips.DoToolTip(&s_DirectoryButton, &DirectoryButton, Localize("Open the directory to add custom skins"));

	TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
	TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT | ETextRenderFlags::TEXT_RENDER_FLAG_NO_OVERSIZE);
	static CButtonContainer s_SkinRefreshButton;
	if(DoButton_Menu(&s_SkinRefreshButton, FontIcon::ARROW_ROTATE_RIGHT, 0, &RefreshButton) || Input()->KeyPress(KEY_F5) || (Input()->KeyPress(KEY_R) && Input()->ModifierIsPressed()))
	{
		ShouldRefresh = true;
	}
	TextRender()->SetRenderFlags(0);
	TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);

	if(ShouldRefresh)
	{
		GameClient()->RefreshSkins(CSkinDescriptor::FLAG_SIX);
	}
}

void CMenus::RenderSettingsGraphics(CUIRect MainView)
{
	CUIRect Button;
	char aBuf[128];
	bool CheckSettings = false;

	static const int MAX_RESOLUTIONS = 256;
	static CVideoMode s_aModes[MAX_RESOLUTIONS];
	static int s_NumNodes = Graphics()->GetVideoModes(s_aModes, MAX_RESOLUTIONS, g_Config.m_GfxScreen);
	static int s_GfxFsaaSamples = g_Config.m_GfxFsaaSamples;
	static bool s_GfxBackendChanged = false;
	static bool s_GfxGpuChanged = false;

	static int s_InitDisplayAllVideoModes = g_Config.m_GfxDisplayAllVideoModes;

	static bool s_WasInit = false;
	static bool s_ModesReload = false;
	if(!s_WasInit)
	{
		s_WasInit = true;

		Graphics()->AddWindowPropChangeListener([]() {
			s_ModesReload = true;
		});
	}

	if(s_ModesReload || g_Config.m_GfxDisplayAllVideoModes != s_InitDisplayAllVideoModes)
	{
		s_NumNodes = Graphics()->GetVideoModes(s_aModes, MAX_RESOLUTIONS, g_Config.m_GfxScreen);
		s_ModesReload = false;
		s_InitDisplayAllVideoModes = g_Config.m_GfxDisplayAllVideoModes;
	}

	CUIRect ModeList, ModeLabel;
	MainView.VSplitLeft(350.0f, &MainView, &ModeList);
	ModeList.HSplitTop(24.0f, &ModeLabel, &ModeList);
	MainView.VSplitLeft(340.0f, &MainView, nullptr);

	// display mode list
	static CListBox s_ListBox;
	static const float sc_RowHeightResList = 22.0f;
	static const float sc_FontSizeResListHeader = 12.0f;
	static const float sc_FontSizeResList = 10.0f;

	{
		int G = std::gcd(g_Config.m_GfxScreenWidth, g_Config.m_GfxScreenHeight);
		str_format(aBuf, sizeof(aBuf), "%s: %dx%d @%dhz %d bit (%d:%d)", Localize("Current"), (int)(g_Config.m_GfxScreenWidth * Graphics()->ScreenHiDPIScale()), (int)(g_Config.m_GfxScreenHeight * Graphics()->ScreenHiDPIScale()), g_Config.m_GfxScreenRefreshRate, g_Config.m_GfxColorDepth, g_Config.m_GfxScreenWidth / G, g_Config.m_GfxScreenHeight / G);
		Ui()->DoLabel(&ModeLabel, aBuf, sc_FontSizeResListHeader, TEXTALIGN_MC);
	}

	int SelectedOld = -1;
	s_ListBox.SetActive(!Ui()->IsPopupOpen());
	s_ListBox.DoStart(sc_RowHeightResList, s_NumNodes, 1, 3, SelectedOld, &ModeList);

	for(int i = 0; i < s_NumNodes; ++i)
	{
		const int Depth = s_aModes[i].m_Red + s_aModes[i].m_Green + s_aModes[i].m_Blue > 16 ? 24 : 16;
		if(g_Config.m_GfxColorDepth == Depth &&
			g_Config.m_GfxScreenWidth == s_aModes[i].m_WindowWidth &&
			g_Config.m_GfxScreenHeight == s_aModes[i].m_WindowHeight &&
			g_Config.m_GfxScreenRefreshRate == s_aModes[i].m_RefreshRate)
		{
			SelectedOld = i;
		}

		const CListboxItem Item = s_ListBox.DoNextItem(&s_aModes[i], SelectedOld == i);
		if(!Item.m_Visible)
			continue;

		int G = std::gcd(s_aModes[i].m_WindowWidth, s_aModes[i].m_WindowHeight);
		str_format(aBuf, sizeof(aBuf), " %dx%d @%dhz %d bit (%d:%d)", s_aModes[i].m_CanvasWidth, s_aModes[i].m_CanvasHeight, s_aModes[i].m_RefreshRate, Depth, s_aModes[i].m_WindowWidth / G, s_aModes[i].m_WindowHeight / G);
		Ui()->DoLabel(&Item.m_Rect, aBuf, sc_FontSizeResList, TEXTALIGN_ML);
	}

	const int NewSelected = s_ListBox.DoEnd();
	if(SelectedOld != NewSelected)
	{
		const int Depth = s_aModes[NewSelected].m_Red + s_aModes[NewSelected].m_Green + s_aModes[NewSelected].m_Blue > 16 ? 24 : 16;
		g_Config.m_GfxColorDepth = Depth;
		g_Config.m_GfxScreenWidth = s_aModes[NewSelected].m_WindowWidth;
		g_Config.m_GfxScreenHeight = s_aModes[NewSelected].m_WindowHeight;
		g_Config.m_GfxScreenRefreshRate = s_aModes[NewSelected].m_RefreshRate;
		Graphics()->ResizeToScreen();
	}

	// switches
	CUIRect WindowModeDropDown;
	MainView.HSplitTop(20.0f, &WindowModeDropDown, &MainView);

	const char *apWindowModes[] = {Localize("Windowed"), Localize("Windowed borderless"), Localize("Windowed fullscreen"), Localize("Desktop fullscreen"), Localize("Fullscreen")};
	static const int s_NumWindowMode = std::size(apWindowModes);

	const int OldWindowMode = (g_Config.m_GfxFullscreen ? (g_Config.m_GfxFullscreen == 1 ? 4 : (g_Config.m_GfxFullscreen == 2 ? 3 : 2)) : (g_Config.m_GfxBorderless ? 1 : 0));

	static CUi::SDropDownState s_WindowModeDropDownState;
	static CScrollRegion s_WindowModeDropDownScrollRegion;
	s_WindowModeDropDownState.m_SelectionPopupContext.m_pScrollRegion = &s_WindowModeDropDownScrollRegion;
	const int NewWindowMode = Ui()->DoDropDown(&WindowModeDropDown, OldWindowMode, apWindowModes, s_NumWindowMode, s_WindowModeDropDownState);
	if(OldWindowMode != NewWindowMode)
	{
		if(NewWindowMode == 0)
			Graphics()->SetWindowParams(0, false);
		else if(NewWindowMode == 1)
			Graphics()->SetWindowParams(0, true);
		else if(NewWindowMode == 2)
			Graphics()->SetWindowParams(3, false);
		else if(NewWindowMode == 3)
			Graphics()->SetWindowParams(2, false);
		else if(NewWindowMode == 4)
			Graphics()->SetWindowParams(1, false);
	}

	if(Graphics()->GetNumScreens() > 1)
	{
		CUIRect ScreenDropDown;
		MainView.HSplitTop(2.0f, nullptr, &MainView);
		MainView.HSplitTop(20.0f, &ScreenDropDown, &MainView);

		const int NumScreens = Graphics()->GetNumScreens();
		static std::vector<std::string> s_vScreenNames;
		static std::vector<const char *> s_vpScreenNames;
		s_vScreenNames.resize(NumScreens);
		s_vpScreenNames.resize(NumScreens);

		for(int i = 0; i < NumScreens; ++i)
		{
			str_format(aBuf, sizeof(aBuf), "%s %d: %s", Localize("Screen"), i, Graphics()->GetScreenName(i));
			s_vScreenNames[i] = aBuf;
			s_vpScreenNames[i] = s_vScreenNames[i].c_str();
		}

		static CUi::SDropDownState s_ScreenDropDownState;
		static CScrollRegion s_ScreenDropDownScrollRegion;
		s_ScreenDropDownState.m_SelectionPopupContext.m_pScrollRegion = &s_ScreenDropDownScrollRegion;
		const int NewScreen = Ui()->DoDropDown(&ScreenDropDown, g_Config.m_GfxScreen, s_vpScreenNames.data(), s_vpScreenNames.size(), s_ScreenDropDownState);
		if(NewScreen != g_Config.m_GfxScreen)
			Graphics()->SwitchWindowScreen(NewScreen, true);
	}

	MainView.HSplitTop(2.0f, nullptr, &MainView);
	MainView.HSplitTop(20.0f, &Button, &MainView);
	str_format(aBuf, sizeof(aBuf), "%s (%s)", Localize("V-Sync"), Localize("may cause delay"));
	if(DoButton_CheckBox(&g_Config.m_GfxVsync, aBuf, g_Config.m_GfxVsync, &Button))
	{
		Graphics()->SetVSync(!g_Config.m_GfxVsync);
	}

	bool MultiSamplingChanged = false;
	MainView.HSplitTop(20.0f, &Button, &MainView);
	str_format(aBuf, sizeof(aBuf), "%s (%s)", Localize("FSAA samples"), Localize("may cause delay"));
	int GfxFsaaSamplesMouseButton = DoButton_CheckBox_Number(&g_Config.m_GfxFsaaSamples, aBuf, g_Config.m_GfxFsaaSamples, &Button);
	int CurFSAA = g_Config.m_GfxFsaaSamples == 0 ? 1 : g_Config.m_GfxFsaaSamples;
	if(GfxFsaaSamplesMouseButton == 1) // inc
	{
		g_Config.m_GfxFsaaSamples = std::pow(2, (int)std::log2(CurFSAA) + 1);
		if(g_Config.m_GfxFsaaSamples > 64)
			g_Config.m_GfxFsaaSamples = 0;
		MultiSamplingChanged = true;
	}
	else if(GfxFsaaSamplesMouseButton == 2) // dec
	{
		if(CurFSAA == 1)
			g_Config.m_GfxFsaaSamples = 64;
		else if(CurFSAA == 2)
			g_Config.m_GfxFsaaSamples = 0;
		else
			g_Config.m_GfxFsaaSamples = std::pow(2, (int)std::log2(CurFSAA) - 1);
		MultiSamplingChanged = true;
	}

	uint32_t MultiSamplingCountBackend = 0;
	if(MultiSamplingChanged)
	{
		if(Graphics()->SetMultiSampling(g_Config.m_GfxFsaaSamples, MultiSamplingCountBackend))
		{
			// try again with 0 if mouse click was increasing multi sampling
			// else just accept the current value as is
			if((uint32_t)g_Config.m_GfxFsaaSamples > MultiSamplingCountBackend && GfxFsaaSamplesMouseButton == 1)
				Graphics()->SetMultiSampling(0, MultiSamplingCountBackend);
			g_Config.m_GfxFsaaSamples = (int)MultiSamplingCountBackend;
		}
		else
		{
			CheckSettings = true;
		}
	}

	MainView.HSplitTop(20.0f, &Button, &MainView);
	if(DoButton_CheckBox(&g_Config.m_GfxHighDetail, Localize("High Detail"), g_Config.m_GfxHighDetail, &Button))
		g_Config.m_GfxHighDetail ^= 1;
	GameClient()->m_Tooltips.DoToolTip(&g_Config.m_GfxHighDetail, &Button, Localize("Allows maps to render with more detail"));

	MainView.HSplitTop(20.0f, &Button, &MainView);
	if(DoButton_CheckBox(&g_Config.m_ClShowfps, Localize("Show FPS"), g_Config.m_ClShowfps, &Button))
		g_Config.m_ClShowfps ^= 1;
	GameClient()->m_Tooltips.DoToolTip(&g_Config.m_ClShowfps, &Button, Localize("Renders your frame rate in the top right"));

	MainView.HSplitTop(20.0f, &Button, &MainView);
	str_copy(aBuf, " ");
	str_append(aBuf, Localize("Hz", "Hertz"));
	Ui()->DoScrollbarOption(&g_Config.m_GfxRefreshRate, &g_Config.m_GfxRefreshRate, &Button, Localize("Refresh Rate"), 10, 1000, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_INFINITE | CUi::SCROLLBAR_OPTION_NOCLAMPVALUE | CUi::SCROLLBAR_OPTION_DELAYUPDATE, aBuf);

	MainView.HSplitTop(2.0f, nullptr, &MainView);
	static CButtonContainer s_UiColorResetId;
	DoLine_ColorPicker(&s_UiColorResetId, 25.0f, 13.0f, 2.0f, &MainView, Localize("UI Color"), &g_Config.m_UiColor, color_cast<ColorRGBA>(ColorHSLA(0xE4A046AFU, true)), false, nullptr, true);

	// Backend list
	struct SMenuBackendInfo
	{
		int m_Major = 0;
		int m_Minor = 0;
		int m_Patch = 0;
		const char *m_pBackendName = "";
		bool m_Found = false;
	};
	std::array<std::array<SMenuBackendInfo, EGraphicsDriverAgeType::GRAPHICS_DRIVER_AGE_TYPE_COUNT>, EBackendType::BACKEND_TYPE_COUNT> aaSupportedBackends{};
	uint32_t FoundBackendCount = 0;
	for(uint32_t i = 0; i < BACKEND_TYPE_COUNT; ++i)
	{
		if(EBackendType(i) == BACKEND_TYPE_AUTO)
			continue;
		for(uint32_t n = 0; n < GRAPHICS_DRIVER_AGE_TYPE_COUNT; ++n)
		{
			auto &Info = aaSupportedBackends[i][n];
			if(Graphics()->GetDriverVersion(EGraphicsDriverAgeType(n), Info.m_Major, Info.m_Minor, Info.m_Patch, Info.m_pBackendName, EBackendType(i)))
			{
				// don't count blocked opengl drivers
				if(EBackendType(i) != BACKEND_TYPE_OPENGL || EGraphicsDriverAgeType(n) == GRAPHICS_DRIVER_AGE_TYPE_LEGACY || g_Config.m_GfxDriverIsBlocked == 0)
				{
					Info.m_Found = true;
					++FoundBackendCount;
				}
			}
		}
	}

	if(FoundBackendCount > 1)
	{
		CUIRect Text, BackendDropDown;
		MainView.HSplitTop(10.0f, nullptr, &MainView);
		MainView.HSplitTop(20.0f, &Text, &MainView);
		MainView.HSplitTop(2.0f, nullptr, &MainView);
		MainView.HSplitTop(20.0f, &BackendDropDown, &MainView);
		Ui()->DoLabel(&Text, Localize("Renderer"), 16.0f, TEXTALIGN_MC);

		static std::vector<std::string> s_vBackendIdNames;
		static std::vector<const char *> s_vpBackendIdNamesCStr;
		static std::vector<SMenuBackendInfo> s_vBackendInfos;

		size_t BackendCount = FoundBackendCount + 1;
		s_vBackendIdNames.resize(BackendCount);
		s_vpBackendIdNamesCStr.resize(BackendCount);
		s_vBackendInfos.resize(BackendCount);

		char aTmpBackendName[256];

		auto IsInfoDefault = [](const SMenuBackendInfo &CheckInfo) {
			return str_comp_nocase(CheckInfo.m_pBackendName, DefaultConfig::GfxBackend) == 0 && CheckInfo.m_Major == DefaultConfig::GfxGLMajor && CheckInfo.m_Minor == DefaultConfig::GfxGLMinor && CheckInfo.m_Patch == DefaultConfig::GfxGLPatch;
		};

		int SelectedOldBackend = -1;
		uint32_t CurCounter = 0;
		for(uint32_t i = 0; i < BACKEND_TYPE_COUNT; ++i)
		{
			for(uint32_t n = 0; n < GRAPHICS_DRIVER_AGE_TYPE_COUNT; ++n)
			{
				auto &Info = aaSupportedBackends[i][n];
				if(Info.m_Found)
				{
					bool IsDefault = IsInfoDefault(Info);
					str_format(aTmpBackendName, sizeof(aTmpBackendName), "%s (%d.%d.%d)%s%s", Info.m_pBackendName, Info.m_Major, Info.m_Minor, Info.m_Patch, IsDefault ? " - " : "", IsDefault ? Localize("default") : "");
					s_vBackendIdNames[CurCounter] = aTmpBackendName;
					s_vpBackendIdNamesCStr[CurCounter] = s_vBackendIdNames[CurCounter].c_str();
					if(str_comp_nocase(Info.m_pBackendName, g_Config.m_GfxBackend) == 0 && g_Config.m_GfxGLMajor == Info.m_Major && g_Config.m_GfxGLMinor == Info.m_Minor && g_Config.m_GfxGLPatch == Info.m_Patch)
					{
						SelectedOldBackend = CurCounter;
					}

					s_vBackendInfos[CurCounter] = Info;
					++CurCounter;
				}
			}
		}

		if(SelectedOldBackend != -1)
		{
			// no custom selected
			BackendCount -= 1;
		}
		else
		{
			// custom selected one
			str_format(aTmpBackendName, sizeof(aTmpBackendName), "%s (%s %d.%d.%d)", Localize("custom"), g_Config.m_GfxBackend, g_Config.m_GfxGLMajor, g_Config.m_GfxGLMinor, g_Config.m_GfxGLPatch);
			s_vBackendIdNames[CurCounter] = aTmpBackendName;
			s_vpBackendIdNamesCStr[CurCounter] = s_vBackendIdNames[CurCounter].c_str();
			SelectedOldBackend = CurCounter;

			s_vBackendInfos[CurCounter].m_pBackendName = "custom";
			s_vBackendInfos[CurCounter].m_Major = g_Config.m_GfxGLMajor;
			s_vBackendInfos[CurCounter].m_Minor = g_Config.m_GfxGLMinor;
			s_vBackendInfos[CurCounter].m_Patch = g_Config.m_GfxGLPatch;
		}

		static int s_SelectedOldBackend = -1;
		if(s_SelectedOldBackend == -1)
			s_SelectedOldBackend = SelectedOldBackend;

		static CUi::SDropDownState s_BackendDropDownState;
		static CScrollRegion s_BackendDropDownScrollRegion;
		s_BackendDropDownState.m_SelectionPopupContext.m_pScrollRegion = &s_BackendDropDownScrollRegion;
		const int NewBackend = Ui()->DoDropDown(&BackendDropDown, SelectedOldBackend, s_vpBackendIdNamesCStr.data(), BackendCount, s_BackendDropDownState);
		if(SelectedOldBackend != NewBackend)
		{
			str_copy(g_Config.m_GfxBackend, s_vBackendInfos[NewBackend].m_pBackendName);
			g_Config.m_GfxGLMajor = s_vBackendInfos[NewBackend].m_Major;
			g_Config.m_GfxGLMinor = s_vBackendInfos[NewBackend].m_Minor;
			g_Config.m_GfxGLPatch = s_vBackendInfos[NewBackend].m_Patch;

			CheckSettings = true;
			s_GfxBackendChanged = s_SelectedOldBackend != NewBackend;
		}
	}

	// GPU list
	const auto &GpuList = Graphics()->GetGpus();
	if(GpuList.m_vGpus.size() > 1)
	{
		CUIRect Text, GpuDropDown;
		MainView.HSplitTop(10.0f, nullptr, &MainView);
		MainView.HSplitTop(20.0f, &Text, &MainView);
		MainView.HSplitTop(2.0f, nullptr, &MainView);
		MainView.HSplitTop(20.0f, &GpuDropDown, &MainView);
		Ui()->DoLabel(&Text, Localize("Graphics card"), 16.0f, TEXTALIGN_MC);

		static std::vector<const char *> s_vpGpuIdNames;

		size_t GpuCount = GpuList.m_vGpus.size() + 1;
		s_vpGpuIdNames.resize(GpuCount);

		char aCurDeviceName[256 + 4];

		int OldSelectedGpu = -1;
		for(size_t i = 0; i < GpuCount; ++i)
		{
			if(i == 0)
			{
				str_format(aCurDeviceName, sizeof(aCurDeviceName), "%s (%s)", Localize("auto"), GpuList.m_AutoGpu.m_aName);
				s_vpGpuIdNames[i] = aCurDeviceName;
				if(str_comp("auto", g_Config.m_GfxGpuName) == 0)
				{
					OldSelectedGpu = 0;
				}
			}
			else
			{
				s_vpGpuIdNames[i] = GpuList.m_vGpus[i - 1].m_aName;
				if(str_comp(GpuList.m_vGpus[i - 1].m_aName, g_Config.m_GfxGpuName) == 0)
				{
					OldSelectedGpu = i;
				}
			}
		}

		static int s_OldSelectedGpu = -1;
		if(s_OldSelectedGpu == -1)
			s_OldSelectedGpu = OldSelectedGpu;

		static CUi::SDropDownState s_GpuDropDownState;
		static CScrollRegion s_GpuDropDownScrollRegion;
		s_GpuDropDownState.m_SelectionPopupContext.m_pScrollRegion = &s_GpuDropDownScrollRegion;
		const int NewGpu = Ui()->DoDropDown(&GpuDropDown, OldSelectedGpu, s_vpGpuIdNames.data(), GpuCount, s_GpuDropDownState);
		if(OldSelectedGpu != NewGpu)
		{
			if(NewGpu == 0)
				str_copy(g_Config.m_GfxGpuName, "auto");
			else
				str_copy(g_Config.m_GfxGpuName, GpuList.m_vGpus[NewGpu - 1].m_aName);
			CheckSettings = true;
			s_GfxGpuChanged = NewGpu != s_OldSelectedGpu;
		}
	}

	// check if the new settings require a restart
	if(CheckSettings)
	{
		m_NeedRestartGraphics = !(s_GfxFsaaSamples == g_Config.m_GfxFsaaSamples &&
					  !s_GfxBackendChanged &&
					  !s_GfxGpuChanged);
	}
}

void CMenus::RenderSettingsSound(CUIRect MainView)
{
	static int s_SndEnable = g_Config.m_SndEnable;

	CUIRect Button;
	MainView.HSplitTop(20.0f, &Button, &MainView);
	if(DoButton_CheckBox(&g_Config.m_SndEnable, Localize("Use sounds"), g_Config.m_SndEnable, &Button))
	{
		g_Config.m_SndEnable ^= 1;
		UpdateMusicState();
		m_NeedRestartSound = g_Config.m_SndEnable && !s_SndEnable;
	}

	if(!g_Config.m_SndEnable)
		return;

	MainView.HSplitTop(20.0f, &Button, &MainView);
	if(DoButton_CheckBox(&g_Config.m_SndMusic, Localize("Play background music"), g_Config.m_SndMusic, &Button))
	{
		g_Config.m_SndMusic ^= 1;
		UpdateMusicState();
	}

	MainView.HSplitTop(20.0f, &Button, &MainView);
	if(DoButton_CheckBox(&g_Config.m_SndNonactiveMute, Localize("Mute when not active"), g_Config.m_SndNonactiveMute, &Button))
		g_Config.m_SndNonactiveMute ^= 1;

	MainView.HSplitTop(20.0f, &Button, &MainView);
	if(DoButton_CheckBox(&g_Config.m_SndGame, Localize("Enable game sounds"), g_Config.m_SndGame, &Button))
		g_Config.m_SndGame ^= 1;

	MainView.HSplitTop(20.0f, &Button, &MainView);
	if(DoButton_CheckBox(&g_Config.m_SndGun, Localize("Enable gun sound"), g_Config.m_SndGun, &Button))
		g_Config.m_SndGun ^= 1;

	MainView.HSplitTop(20.0f, &Button, &MainView);
	if(DoButton_CheckBox(&g_Config.m_SndLongPain, Localize("Enable long pain sound (used when shooting in freeze)"), g_Config.m_SndLongPain, &Button))
		g_Config.m_SndLongPain ^= 1;

	MainView.HSplitTop(20.0f, &Button, &MainView);
	if(DoButton_CheckBox(&g_Config.m_SndServerMessage, Localize("Enable server message sound"), g_Config.m_SndServerMessage, &Button))
		g_Config.m_SndServerMessage ^= 1;

	MainView.HSplitTop(20.0f, &Button, &MainView);
	if(DoButton_CheckBox(&g_Config.m_SndChat, Localize("Enable regular chat sound"), g_Config.m_SndChat, &Button))
		g_Config.m_SndChat ^= 1;

	MainView.HSplitTop(20.0f, &Button, &MainView);
	if(DoButton_CheckBox(&g_Config.m_SndTeamChat, Localize("Enable team chat sound"), g_Config.m_SndTeamChat, &Button))
		g_Config.m_SndTeamChat ^= 1;

	MainView.HSplitTop(20.0f, &Button, &MainView);
	if(DoButton_CheckBox(&g_Config.m_SndHighlight, Localize("Enable highlighted chat sound"), g_Config.m_SndHighlight, &Button))
		g_Config.m_SndHighlight ^= 1;

	// volume slider
	{
		MainView.HSplitTop(5.0f, nullptr, &MainView);
		MainView.HSplitTop(20.0f, &Button, &MainView);
		Ui()->DoScrollbarOption(&g_Config.m_SndVolume, &g_Config.m_SndVolume, &Button, Localize("Sound volume"), 0, 100, &CUi::ms_LogarithmicScrollbarScale, 0u, "%");
	}

	// volume slider game sounds
	{
		MainView.HSplitTop(5.0f, nullptr, &MainView);
		MainView.HSplitTop(20.0f, &Button, &MainView);
		Ui()->DoScrollbarOption(&g_Config.m_SndGameVolume, &g_Config.m_SndGameVolume, &Button, Localize("Game sound volume"), 0, 100, &CUi::ms_LogarithmicScrollbarScale, 0u, "%");
	}

	// volume slider gui sounds
	{
		MainView.HSplitTop(5.0f, nullptr, &MainView);
		MainView.HSplitTop(20.0f, &Button, &MainView);
		Ui()->DoScrollbarOption(&g_Config.m_SndChatVolume, &g_Config.m_SndChatVolume, &Button, Localize("Chat sound volume"), 0, 100, &CUi::ms_LogarithmicScrollbarScale, 0u, "%");
	}

	// volume slider map sounds
	{
		MainView.HSplitTop(5.0f, nullptr, &MainView);
		MainView.HSplitTop(20.0f, &Button, &MainView);
		Ui()->DoScrollbarOption(&g_Config.m_SndMapVolume, &g_Config.m_SndMapVolume, &Button, Localize("Map sound volume"), 0, 100, &CUi::ms_LogarithmicScrollbarScale, 0u, "%");
	}

	// volume slider background music
	{
		MainView.HSplitTop(5.0f, nullptr, &MainView);
		MainView.HSplitTop(20.0f, &Button, &MainView);
		Ui()->DoScrollbarOption(&g_Config.m_SndBackgroundMusicVolume, &g_Config.m_SndBackgroundMusicVolume, &Button, Localize("Background music volume"), 0, 100, &CUi::ms_LogarithmicScrollbarScale, 0u, "%");
	}
}

void CMenus::RenderLanguageSettings(CUIRect MainView)
{
	const float CreditsFontSize = 14.0f;
	const float CreditsMargin = 10.0f;

	CUIRect List, CreditsScroll;
	MainView.HSplitBottom(4.0f * CreditsFontSize + 2.0f * CreditsMargin + CScrollRegion::HEIGHT_MAGIC_FIX, &List, &CreditsScroll);
	List.HSplitBottom(5.0f, &List, nullptr);

	RenderLanguageSelection(List);

	CreditsScroll.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f), IGraphics::CORNER_ALL, 5.0f);

	static CScrollRegion s_CreditsScrollRegion;
	vec2 ScrollOffset(0.0f, 0.0f);
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollUnit = CreditsFontSize;
	s_CreditsScrollRegion.Begin(&CreditsScroll, &ScrollOffset, &ScrollParams);
	CreditsScroll.y += ScrollOffset.y;

	CTextCursor Cursor;
	Cursor.m_FontSize = CreditsFontSize;
	Cursor.m_LineWidth = CreditsScroll.w - 2.0f * CreditsMargin;

	const unsigned OldRenderFlags = TextRender()->GetRenderFlags();
	TextRender()->SetRenderFlags(OldRenderFlags | TEXT_RENDER_FLAG_ONE_TIME_USE);
	STextContainerIndex CreditsTextContainer;
	TextRender()->CreateTextContainer(CreditsTextContainer, &Cursor, Localize("English translation by the DDNet Team", "Translation credits: Add your own name here when you update translations"));
	TextRender()->SetRenderFlags(OldRenderFlags);
	if(CreditsTextContainer.Valid())
	{
		CUIRect CreditsLabel;
		CreditsScroll.HSplitTop(TextRender()->GetBoundingBoxTextContainer(CreditsTextContainer).m_H + 2.0f * CreditsMargin, &CreditsLabel, &CreditsScroll);
		s_CreditsScrollRegion.AddRect(CreditsLabel);
		CreditsLabel.Margin(CreditsMargin, &CreditsLabel);
		TextRender()->RenderTextContainer(CreditsTextContainer, TextRender()->DefaultTextColor(), TextRender()->DefaultTextOutlineColor(), CreditsLabel.x, CreditsLabel.y);
		TextRender()->DeleteTextContainer(CreditsTextContainer);
	}

	s_CreditsScrollRegion.End();
}

bool CMenus::RenderLanguageSelection(CUIRect MainView)
{
	static int s_SelectedLanguage = -2; // -2 = unloaded, -1 = unset
	static CListBox s_ListBox;

	if(s_SelectedLanguage == -2)
	{
		s_SelectedLanguage = -1;
		for(size_t i = 0; i < g_Localization.Languages().size(); i++)
		{
			if(str_comp(g_Localization.Languages()[i].m_Filename.c_str(), g_Config.m_ClLanguagefile) == 0)
			{
				s_SelectedLanguage = i;
				s_ListBox.ScrollToSelected();
				break;
			}
		}
	}

	const int SelectedOld = s_SelectedLanguage;

	s_ListBox.DoStart(24.0f, g_Localization.Languages().size(), 1, 3, s_SelectedLanguage, &MainView);

	for(const auto &Language : g_Localization.Languages())
	{
		const CListboxItem Item = s_ListBox.DoNextItem(&Language.m_Name, s_SelectedLanguage != -1 && !str_comp(g_Localization.Languages()[s_SelectedLanguage].m_Name.c_str(), Language.m_Name.c_str()));
		if(!Item.m_Visible)
			continue;

		CUIRect FlagRect, Label;
		Item.m_Rect.VSplitLeft(Item.m_Rect.h * 2.0f, &FlagRect, &Label);
		FlagRect.VMargin(6.0f, &FlagRect);
		FlagRect.HMargin(3.0f, &FlagRect);
		GameClient()->m_CountryFlags.Render(Language.m_CountryCode, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), FlagRect.x, FlagRect.y, FlagRect.w, FlagRect.h);

		Ui()->DoLabel(&Label, Language.m_Name.c_str(), 16.0f, TEXTALIGN_ML);
	}

	s_SelectedLanguage = s_ListBox.DoEnd();

	if(SelectedOld != s_SelectedLanguage)
	{
		str_copy(g_Config.m_ClLanguagefile, g_Localization.Languages()[s_SelectedLanguage].m_Filename.c_str());
		GameClient()->OnLanguageChange();
	}

	return s_ListBox.WasItemActivated();
}

void CMenus::RenderSettings(CUIRect MainView)
{
	// render background
	CUIRect Button, TabBar, RestartBar;
	MainView.VSplitRight(120.0f, &MainView, &TabBar);
	MainView.Draw(ms_ColorTabbarActive, IGraphics::CORNER_B, 10.0f);
	MainView.Margin(20.0f, &MainView);

	const bool NeedRestart = m_NeedRestartGraphics || m_NeedRestartSound || m_NeedRestartUpdate;
	if(NeedRestart)
	{
		MainView.HSplitBottom(20.0f, &MainView, &RestartBar);
		MainView.HSplitBottom(10.0f, &MainView, nullptr);
	}

	TabBar.HSplitTop(50.0f, &Button, &TabBar);
	Button.Draw(ms_ColorTabbarActive, IGraphics::CORNER_BR, 10.0f);

	static const int s_aMainPages[] = {
		SETTINGS_LANGUAGE,
		SETTINGS_GENERAL,
		SETTINGS_PLAYER,
		SETTINGS_TEE,
		SETTINGS_APPEARANCE,
		SETTINGS_CONTROLS,
		SETTINGS_GRAPHICS,
		SETTINGS_SOUND,
		SETTINGS_DDNET,
		SETTINGS_ASSETS,
		SETTINGS_TCLIENT,
		SETTINGS_PROFILES,
		SETTINGS_ZZETCLIENT,
	};
	static const char *s_apMainTabs[] = {
		Localize("Language"),
		Localize("General"),
		Localize("Player"),
		Localize("Tee"),
		Localize("Appearance"),
		Localize("Controls"),
		Localize("Graphics"),
		Localize("Sound"),
		Localize("DDNet"),
		Localize("Assets"),
		Localize("TClient"),
		Localize("Profiles"),
		Localize("ZZetClient"),
	};
	static CButtonContainer s_aTabButtons[(int)std::size(s_aMainPages)];
	bool MainPageSelected = false;
	for(int i = 0; i < (int)std::size(s_aMainPages); i++)
	{
		TabBar.HSplitTop(10.0f, nullptr, &TabBar);
		TabBar.HSplitTop(26.0f, &Button, &TabBar);
		const int Page = s_aMainPages[i];
		if(g_Config.m_UiSettingsPage == Page)
			MainPageSelected = true;
		if(DoButton_MenuTab(&s_aTabButtons[i], s_apMainTabs[i], g_Config.m_UiSettingsPage == Page, &Button, IGraphics::CORNER_R, &m_aAnimatorsSettingsTab[Page]))
		{
			g_Config.m_UiSettingsPage = Page;
			MainPageSelected = true;
		}
	}
	if(!MainPageSelected)
		g_Config.m_UiSettingsPage = SETTINGS_GENERAL;

	if(g_Config.m_UiSettingsPage == SETTINGS_LANGUAGE)
	{
		RenderLanguageSettings(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_GENERAL)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_GENERAL);
		RenderSettingsGeneral(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_PLAYER)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_PLAYER);
		RenderSettingsPlayer(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_TEE)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_TEE);
		if(Client()->IsSixup())
			RenderSettingsTee7(MainView);
		else
			RenderSettingsTee(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_APPEARANCE)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_APPEARANCE);
		RenderSettingsAppearance(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_CONTROLS)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_CONTROLS);
		m_MenusSettingsControls.Render(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_GRAPHICS)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_GRAPHICS);
		RenderSettingsGraphics(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_SOUND)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_SOUND);
		RenderSettingsSound(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_DDNET)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_DDNET);
		RenderSettingsDDNet(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_ASSETS)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_ASSETS);
		RenderSettingsCustom(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_TCLIENT)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_RESERVED0);
		RenderSettingsTClient(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_PROFILES)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_RESERVED0);
		RenderSettingsTClientProfiles(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_CONFIGS)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_RESERVED0);
		RenderSettingsTClientConfigs(MainView);
	}
	else if(g_Config.m_UiSettingsPage == SETTINGS_ZZETCLIENT)
	{
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_RESERVED1);
		RenderSettingsZZetClient(MainView);
	}
	else
	{
		dbg_assert_failed("ui_settings_page invalid");
	}

	if(NeedRestart)
	{
		CUIRect RestartWarning, RestartButton;
		RestartBar.VSplitRight(125.0f, &RestartWarning, &RestartButton);
		RestartWarning.VSplitRight(10.0f, &RestartWarning, nullptr);
		if(m_NeedRestartUpdate)
		{
			TextRender()->TextColor(1.0f, 0.4f, 0.4f, 1.0f);
			Ui()->DoLabel(&RestartWarning, Localize("DDNet Client needs to be restarted to complete update!"), 14.0f, TEXTALIGN_ML);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		}
		else
		{
			Ui()->DoLabel(&RestartWarning, Localize("You must restart the game for all settings to take effect."), 14.0f, TEXTALIGN_ML);
		}

		static CButtonContainer s_RestartButton;
		if(DoButton_Menu(&s_RestartButton, Localize("Restart"), 0, &RestartButton))
		{
			if(Client()->State() == IClient::STATE_ONLINE || GameClient()->Editor()->HasUnsavedData())
			{
				m_Popup = POPUP_RESTART;
			}
			else
			{
				Client()->Restart();
			}
		}
	}
}

bool CMenus::RenderHslaScrollbars(CUIRect *pRect, unsigned int *pColor, bool Alpha, float DarkestLight)
{
	const unsigned PrevPackedColor = *pColor;
	ColorHSLA Color(*pColor, Alpha);
	const ColorHSLA OriginalColor = Color;
	const char *apLabels[] = {Localize("Hue"), Localize("Sat."), Localize("Lht."), Localize("Alpha")};
	const float SizePerEntry = 20.0f;
	const float MarginPerEntry = 5.0f;
	const float PreviewMargin = 2.5f;
	const float PreviewHeight = 40.0f + 2 * PreviewMargin;
	const float OffY = (SizePerEntry + MarginPerEntry) * (3 + (Alpha ? 1 : 0)) - PreviewHeight;

	CUIRect Preview;
	pRect->VSplitLeft(PreviewHeight, &Preview, pRect);
	Preview.HSplitTop(OffY / 2.0f, nullptr, &Preview);
	Preview.HSplitTop(PreviewHeight, &Preview, nullptr);

	Preview.Draw(ColorRGBA(0.15f, 0.15f, 0.15f, 1.0f), IGraphics::CORNER_ALL, 4.0f + PreviewMargin);
	Preview.Margin(PreviewMargin, &Preview);
	Preview.Draw(color_cast<ColorRGBA>(Color.UnclampLighting(DarkestLight)), IGraphics::CORNER_ALL, 4.0f + PreviewMargin);

	auto &&RenderHueRect = [&](CUIRect *pColorRect) {
		float CurXOff = pColorRect->x;
		const float SizeColor = pColorRect->w / 6;

		// red to yellow
		{
			IGraphics::CColorVertex aColorVertices[] = {
				IGraphics::CColorVertex(0, 1, 0, 0, 1),
				IGraphics::CColorVertex(1, 1, 1, 0, 1),
				IGraphics::CColorVertex(2, 1, 0, 0, 1),
				IGraphics::CColorVertex(3, 1, 1, 0, 1)};
			Graphics()->SetColorVertex(aColorVertices, std::size(aColorVertices));

			IGraphics::CFreeformItem Freeform(
				CurXOff, pColorRect->y,
				CurXOff + SizeColor, pColorRect->y,
				CurXOff, pColorRect->y + pColorRect->h,
				CurXOff + SizeColor, pColorRect->y + pColorRect->h);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}

		// yellow to green
		CurXOff += SizeColor;
		{
			IGraphics::CColorVertex aColorVertices[] = {
				IGraphics::CColorVertex(0, 1, 1, 0, 1),
				IGraphics::CColorVertex(1, 0, 1, 0, 1),
				IGraphics::CColorVertex(2, 1, 1, 0, 1),
				IGraphics::CColorVertex(3, 0, 1, 0, 1)};
			Graphics()->SetColorVertex(aColorVertices, std::size(aColorVertices));

			IGraphics::CFreeformItem Freeform(
				CurXOff, pColorRect->y,
				CurXOff + SizeColor, pColorRect->y,
				CurXOff, pColorRect->y + pColorRect->h,
				CurXOff + SizeColor, pColorRect->y + pColorRect->h);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}

		CurXOff += SizeColor;
		// green to turquoise
		{
			IGraphics::CColorVertex aColorVertices[] = {
				IGraphics::CColorVertex(0, 0, 1, 0, 1),
				IGraphics::CColorVertex(1, 0, 1, 1, 1),
				IGraphics::CColorVertex(2, 0, 1, 0, 1),
				IGraphics::CColorVertex(3, 0, 1, 1, 1)};
			Graphics()->SetColorVertex(aColorVertices, std::size(aColorVertices));

			IGraphics::CFreeformItem Freeform(
				CurXOff, pColorRect->y,
				CurXOff + SizeColor, pColorRect->y,
				CurXOff, pColorRect->y + pColorRect->h,
				CurXOff + SizeColor, pColorRect->y + pColorRect->h);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}

		CurXOff += SizeColor;
		// turquoise to blue
		{
			IGraphics::CColorVertex aColorVertices[] = {
				IGraphics::CColorVertex(0, 0, 1, 1, 1),
				IGraphics::CColorVertex(1, 0, 0, 1, 1),
				IGraphics::CColorVertex(2, 0, 1, 1, 1),
				IGraphics::CColorVertex(3, 0, 0, 1, 1)};
			Graphics()->SetColorVertex(aColorVertices, std::size(aColorVertices));

			IGraphics::CFreeformItem Freeform(
				CurXOff, pColorRect->y,
				CurXOff + SizeColor, pColorRect->y,
				CurXOff, pColorRect->y + pColorRect->h,
				CurXOff + SizeColor, pColorRect->y + pColorRect->h);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}

		CurXOff += SizeColor;
		// blue to purple
		{
			IGraphics::CColorVertex aColorVertices[] = {
				IGraphics::CColorVertex(0, 0, 0, 1, 1),
				IGraphics::CColorVertex(1, 1, 0, 1, 1),
				IGraphics::CColorVertex(2, 0, 0, 1, 1),
				IGraphics::CColorVertex(3, 1, 0, 1, 1)};
			Graphics()->SetColorVertex(aColorVertices, std::size(aColorVertices));

			IGraphics::CFreeformItem Freeform(
				CurXOff, pColorRect->y,
				CurXOff + SizeColor, pColorRect->y,
				CurXOff, pColorRect->y + pColorRect->h,
				CurXOff + SizeColor, pColorRect->y + pColorRect->h);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}

		CurXOff += SizeColor;
		// purple to red
		{
			IGraphics::CColorVertex aColorVertices[] = {
				IGraphics::CColorVertex(0, 1, 0, 1, 1),
				IGraphics::CColorVertex(1, 1, 0, 0, 1),
				IGraphics::CColorVertex(2, 1, 0, 1, 1),
				IGraphics::CColorVertex(3, 1, 0, 0, 1)};
			Graphics()->SetColorVertex(aColorVertices, std::size(aColorVertices));

			IGraphics::CFreeformItem Freeform(
				CurXOff, pColorRect->y,
				CurXOff + SizeColor, pColorRect->y,
				CurXOff, pColorRect->y + pColorRect->h,
				CurXOff + SizeColor, pColorRect->y + pColorRect->h);
			Graphics()->QuadsDrawFreeform(&Freeform, 1);
		}
	};

	auto &&RenderSaturationRect = [&](CUIRect *pColorRect, const ColorRGBA &CurColor) {
		ColorHSLA LeftColor = color_cast<ColorHSLA>(CurColor);
		ColorHSLA RightColor = color_cast<ColorHSLA>(CurColor);

		LeftColor.s = 0.0f;
		RightColor.s = 1.0f;

		const ColorRGBA LeftColorRGBA = color_cast<ColorRGBA>(LeftColor);
		const ColorRGBA RightColorRGBA = color_cast<ColorRGBA>(RightColor);

		Graphics()->SetColor4(LeftColorRGBA, RightColorRGBA, RightColorRGBA, LeftColorRGBA);

		IGraphics::CFreeformItem Freeform(
			pColorRect->x, pColorRect->y,
			pColorRect->x + pColorRect->w, pColorRect->y,
			pColorRect->x, pColorRect->y + pColorRect->h,
			pColorRect->x + pColorRect->w, pColorRect->y + pColorRect->h);
		Graphics()->QuadsDrawFreeform(&Freeform, 1);
	};

	auto &&RenderLightingRect = [&](CUIRect *pColorRect, const ColorRGBA &CurColor) {
		ColorHSLA LeftColor = color_cast<ColorHSLA>(CurColor);
		ColorHSLA RightColor = color_cast<ColorHSLA>(CurColor);

		LeftColor.l = DarkestLight;
		RightColor.l = 1.0f;

		const ColorRGBA LeftColorRGBA = color_cast<ColorRGBA>(LeftColor);
		const ColorRGBA RightColorRGBA = color_cast<ColorRGBA>(RightColor);

		Graphics()->SetColor4(LeftColorRGBA, RightColorRGBA, RightColorRGBA, LeftColorRGBA);

		IGraphics::CFreeformItem Freeform(
			pColorRect->x, pColorRect->y,
			pColorRect->x + pColorRect->w, pColorRect->y,
			pColorRect->x, pColorRect->y + pColorRect->h,
			pColorRect->x + pColorRect->w, pColorRect->y + pColorRect->h);
		Graphics()->QuadsDrawFreeform(&Freeform, 1);
	};

	auto &&RenderAlphaRect = [&](CUIRect *pColorRect, const ColorRGBA &CurColorFull) {
		const ColorRGBA LeftColorRGBA = color_cast<ColorRGBA>(color_cast<ColorHSLA>(CurColorFull).WithAlpha(0.0f));
		const ColorRGBA RightColorRGBA = color_cast<ColorRGBA>(color_cast<ColorHSLA>(CurColorFull).WithAlpha(1.0f));

		Graphics()->SetColor4(LeftColorRGBA, RightColorRGBA, RightColorRGBA, LeftColorRGBA);

		IGraphics::CFreeformItem Freeform(
			pColorRect->x, pColorRect->y,
			pColorRect->x + pColorRect->w, pColorRect->y,
			pColorRect->x, pColorRect->y + pColorRect->h,
			pColorRect->x + pColorRect->w, pColorRect->y + pColorRect->h);
		Graphics()->QuadsDrawFreeform(&Freeform, 1);
	};

	for(int i = 0; i < 3 + Alpha; i++)
	{
		CUIRect Button, Label;
		pRect->HSplitTop(SizePerEntry, &Button, pRect);
		pRect->HSplitTop(MarginPerEntry, nullptr, pRect);
		Button.VSplitLeft(140.0f, &Label, &Button);
		Label.VMargin(10.0f, &Label);

		Button.Draw(ColorRGBA(0.15f, 0.15f, 0.15f, 1.0f), IGraphics::CORNER_ALL, 1.0f);

		CUIRect Rail;
		Button.Margin(2.0f, &Rail);

		char aBuf[32];

		// Hue
		if(i == 0)
			str_format(aBuf, sizeof(aBuf), "%s: %.1fВ° (%03d)", apLabels[i], Color[i] * 360.0f, round_to_int(Color[i] * 255.0f));
		// Lht
		else if(i == 2)
		{
			// handle internal light clamping, see `UnclampLighting`
			float Lht = DarkestLight + Color[i] * (1.0f - DarkestLight);
			str_format(aBuf, sizeof(aBuf), "%s: %.1f%% (%03d)", apLabels[i], Lht * 100.0f, round_to_int(Color[i] * 255.0f));
		}
		// Sat and Alpha
		else
			str_format(aBuf, sizeof(aBuf), "%s: %.1f%% (%03d)", apLabels[i], Color[i] * 100.0f, round_to_int(Color[i] * 255.0f));
		Ui()->DoLabel(&Label, aBuf, 12.0f, TEXTALIGN_ML);

		ColorRGBA HandleColor;
		Graphics()->TextureClear();
		Graphics()->TrianglesBegin();
		if(i == 0)
		{
			RenderHueRect(&Rail);
			HandleColor = color_cast<ColorRGBA>(ColorHSLA(Color.h, 1.0f, 0.5f, 1.0f));
		}
		else if(i == 1)
		{
			RenderSaturationRect(&Rail, color_cast<ColorRGBA>(ColorHSLA(Color.h, 1.0f, 0.5f, 1.0f)));
			HandleColor = color_cast<ColorRGBA>(ColorHSLA(Color.h, Color.s, 0.5f, 1.0f));
		}
		else if(i == 2)
		{
			RenderLightingRect(&Rail, color_cast<ColorRGBA>(ColorHSLA(Color.h, Color.s, 0.5f, 1.0f)));
			HandleColor = color_cast<ColorRGBA>(ColorHSLA(Color.h, Color.s, Color.l, 1.0f).UnclampLighting(DarkestLight));
		}
		else if(i == 3)
		{
			RenderAlphaRect(&Rail, color_cast<ColorRGBA>(ColorHSLA(Color.h, Color.s, Color.l, 1.0f).UnclampLighting(DarkestLight)));
			HandleColor = color_cast<ColorRGBA>(Color.UnclampLighting(DarkestLight));
		}
		Graphics()->TrianglesEnd();

		Color[i] = Ui()->DoScrollbarH(&((char *)pColor)[i], &Button, Color[i], &HandleColor);
	}

	if(OriginalColor != Color)
	{
		*pColor = Color.Pack(Alpha);
	}
	return PrevPackedColor != *pColor;
}

enum
{
	APPEARANCE_TAB_HUD = 0,
	APPEARANCE_TAB_CHAT = 1,
	APPEARANCE_TAB_NAME_PLATE = 2,
	APPEARANCE_TAB_HOOK_COLLISION = 3,
	APPEARANCE_TAB_INFO_MESSAGES = 4,
	APPEARANCE_TAB_LASER = 5,
	NUMBER_OF_APPEARANCE_TABS = 6,
};

void CMenus::RenderSettingsAppearance(CUIRect MainView)
{
	char aBuf[128];
	static int s_CurTab = 0;

	CUIRect TabBar, LeftView, RightView, Button;

	MainView.HSplitTop(20.0f, &TabBar, &MainView);
	const float TabWidth = TabBar.w / (float)NUMBER_OF_APPEARANCE_TABS;
	static CButtonContainer s_aPageTabs[NUMBER_OF_APPEARANCE_TABS] = {};
	const char *apTabNames[NUMBER_OF_APPEARANCE_TABS] = {
		Localize("HUD"),
		Localize("Chat"),
		Localize("Name Plate"),
		Localize("Hook Collisions"),
		Localize("Info Messages"),
		Localize("Laser")};

	for(int Tab = APPEARANCE_TAB_HUD; Tab < NUMBER_OF_APPEARANCE_TABS; ++Tab)
	{
		TabBar.VSplitLeft(TabWidth, &Button, &TabBar);
		const int Corners = Tab == APPEARANCE_TAB_HUD ? IGraphics::CORNER_L : (Tab == NUMBER_OF_APPEARANCE_TABS - 1 ? IGraphics::CORNER_R : IGraphics::CORNER_NONE);
		if(DoButton_MenuTab(&s_aPageTabs[Tab], apTabNames[Tab], s_CurTab == Tab, &Button, Corners, nullptr, nullptr, nullptr, nullptr, 4.0f))
		{
			s_CurTab = Tab;
		}
	}

	MainView.HSplitTop(10.0f, nullptr, &MainView);

	const float LineSize = 20.0f;
	const float ColorPickerLineSize = 25.0f;
	const float HeadlineFontSize = 20.0f;
	const float HeadlineHeight = 30.0f;
	const float MarginSmall = 5.0f;
	const float MarginBetweenViews = 20.0f;

	const float ColorPickerLabelSize = 13.0f;
	const float ColorPickerLineSpacing = 5.0f;

	if(s_CurTab == APPEARANCE_TAB_HUD)
	{
		MainView.VSplitMid(&LeftView, &RightView, MarginBetweenViews);

		// ***** HUD ***** //
		Ui()->DoLabel_AutoLineSize(Localize("HUD"), HeadlineFontSize,
			TEXTALIGN_ML, &LeftView, HeadlineHeight);
		LeftView.HSplitTop(MarginSmall, nullptr, &LeftView);

		// Switch of the entire HUD
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowhud, Localize("Show ingame HUD"), &g_Config.m_ClShowhud, &LeftView, LineSize);

		// Switches of the various normal HUD elements
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowhudHealthAmmo, Localize("Show health, shields and ammo"), &g_Config.m_ClShowhudHealthAmmo, &LeftView, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowhudScore, Localize("Show score"), &g_Config.m_ClShowhudScore, &LeftView, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowLocalTimeAlways, Localize("Show local time always"), &g_Config.m_ClShowLocalTimeAlways, &LeftView, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClSpecCursor, Localize("Show spectator cursor"), &g_Config.m_ClSpecCursor, &LeftView, LineSize);

		// Settings of the HUD element for votes
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowVotesAfterVoting, Localize("Show votes window after voting"), &g_Config.m_ClShowVotesAfterVoting, &LeftView, LineSize);

		// ***** Scoreboard ***** //
		LeftView.HSplitTop(MarginBetweenViews, nullptr, &LeftView);
		Ui()->DoLabel_AutoLineSize(Localize("Scoreboard"), HeadlineFontSize,
			TEXTALIGN_ML, &LeftView, HeadlineHeight);
		LeftView.HSplitTop(MarginSmall, nullptr, &LeftView);

		ColorRGBA GreenDefault(0.78f, 1.0f, 0.8f, 1.0f);
		static CButtonContainer s_AuthedColor, s_SameClanColor;
		DoLine_ColorPicker(&s_AuthedColor, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Authed name color in scoreboard"), &g_Config.m_ClAuthedPlayerColor, GreenDefault, false);
		DoLine_ColorPicker(&s_SameClanColor, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Same clan color in scoreboard"), &g_Config.m_ClSameClanColor, GreenDefault, false);

		// ***** DDRace HUD ***** //
		Ui()->DoLabel_AutoLineSize(Localize("DDRace HUD"), HeadlineFontSize,
			TEXTALIGN_ML, &RightView, HeadlineHeight);
		RightView.HSplitTop(MarginSmall, nullptr, &RightView);

		// Switches of various DDRace HUD elements
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowIds, Localize("Show client IDs (scoreboard, chat, spectator)"), &g_Config.m_ClShowIds, &RightView, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowhudDDRace, Localize("Show DDRace HUD"), &g_Config.m_ClShowhudDDRace, &RightView, LineSize);
		if(g_Config.m_ClShowhudDDRace)
		{
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowhudJumpsIndicator, Localize("Show jumps indicator"), &g_Config.m_ClShowhudJumpsIndicator, &RightView, LineSize);
		}
		else
		{
			RightView.HSplitTop(LineSize, nullptr, &RightView); // Create empty space for hidden option
		}

		// Eye with a number of spectators
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowhudSpectatorCount, Localize("Show number of spectators"), &g_Config.m_ClShowhudSpectatorCount, &RightView, LineSize);

		// Switch for dummy actions display
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowhudDummyActions, Localize("Show dummy actions"), &g_Config.m_ClShowhudDummyActions, &RightView, LineSize);

		// Player movement information display settings
		RightView.HSplitTop(MarginSmall, nullptr, &RightView); // TClient
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowhudPlayerPosition, Localize("Show player position"), &g_Config.m_ClShowhudPlayerPosition, &RightView, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowhudPlayerSpeed, Localize("Show player speed"), &g_Config.m_ClShowhudPlayerSpeed, &RightView, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowhudPlayerAngle, Localize("Show player target angle"), &g_Config.m_ClShowhudPlayerAngle, &RightView, LineSize);

		// Dummy movement information display settings
		RightView.HSplitTop(MarginSmall, nullptr, &RightView); // TClient
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcShowhudDummyPosition, TCLocalize("Show dummy position"), &g_Config.m_TcShowhudDummyPosition, &RightView, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcShowhudDummySpeed, TCLocalize("Show dummy speed"), &g_Config.m_TcShowhudDummySpeed, &RightView, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_TcShowhudDummyAngle, TCLocalize("Show dummy target angle"), &g_Config.m_TcShowhudDummyAngle, &RightView, LineSize);

		// Freeze bar settings
		RightView.HSplitTop(MarginSmall, nullptr, &RightView); // TClient
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowFreezeBars, Localize("Show freeze bars"), &g_Config.m_ClShowFreezeBars, &RightView, LineSize);
		RightView.HSplitTop(LineSize * 2.0f, &Button, &RightView);
		if(g_Config.m_ClShowFreezeBars)
		{
			Ui()->DoScrollbarOption(&g_Config.m_ClFreezeBarsAlphaInsideFreeze, &g_Config.m_ClFreezeBarsAlphaInsideFreeze, &Button, Localize("Opacity of freeze bars inside freeze"), 0, 100, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_MULTILINE, "%");
		}
	}
	else if(s_CurTab == APPEARANCE_TAB_CHAT)
	{
		CChat &Chat = GameClient()->m_Chat;
		CUIRect TopView, PreviewView;
		MainView.HSplitBottom(220.0f, &TopView, &PreviewView);
		TopView.HSplitBottom(MarginBetweenViews, &TopView, nullptr);
		TopView.VSplitMid(&LeftView, &RightView, MarginBetweenViews);

		// ***** Chat ***** //
		Ui()->DoLabel_AutoLineSize(Localize("Chat"), HeadlineFontSize,
			TEXTALIGN_ML, &LeftView, HeadlineHeight);
		LeftView.HSplitTop(MarginSmall, nullptr, &LeftView);

		// General chat settings
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(DoButton_CheckBox(&g_Config.m_ClShowChat, Localize("Show chat"), g_Config.m_ClShowChat, &Button))
		{
			g_Config.m_ClShowChat = g_Config.m_ClShowChat ? 0 : 1;
		}
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(g_Config.m_ClShowChat)
		{
			static int s_ShowChat = 0;
			if(DoButton_CheckBox(&s_ShowChat, Localize("Always show chat"), g_Config.m_ClShowChat == 2, &Button))
				g_Config.m_ClShowChat = g_Config.m_ClShowChat != 2 ? 2 : 1;
		}

		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClChatTeamColors, Localize("Show names in chat in team colors"), &g_Config.m_ClChatTeamColors, &LeftView, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowChatFriends, Localize("Show only chat messages from friends"), &g_Config.m_ClShowChatFriends, &LeftView, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClShowChatTeamMembersOnly, Localize("Show only chat messages from team members"), &g_Config.m_ClShowChatTeamMembersOnly, &LeftView, LineSize);

		if(DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClChatOld, Localize("Use old chat style"), &g_Config.m_ClChatOld, &LeftView, LineSize))
			GameClient()->m_Chat.RebuildChat();

		// DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClCensorChat, Localize("Censor profanity"), &g_Config.m_ClCensorChat, &LeftView, LineSize);

		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(Ui()->DoScrollbarOption(&g_Config.m_ClChatFontSize, &g_Config.m_ClChatFontSize, &Button, Localize("Chat font size"), 10, 100))
		{
			Chat.EnsureCoherentWidth();
			Chat.RebuildChat();
		}

		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(Ui()->DoScrollbarOption(&g_Config.m_ClChatWidth, &g_Config.m_ClChatWidth, &Button, Localize("Chat width"), 120, 400))
		{
			Chat.EnsureCoherentFontSize();
			Chat.RebuildChat();
		}

		static CButtonContainer s_BackgroundColor;
		DoLine_ColorPicker(&s_BackgroundColor, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Chat background color"), &g_Config.m_ClChatBackgroundColor, color_cast<ColorRGBA>(ColorHSLA(DefaultConfig::ClChatBackgroundColor, true)), false, nullptr, true);

		// ***** Messages ***** //
		Ui()->DoLabel_AutoLineSize(Localize("Messages"), HeadlineFontSize,
			TEXTALIGN_ML, &RightView, HeadlineHeight);
		RightView.HSplitTop(MarginSmall, nullptr, &RightView);

		// Message Colors and extra settings
		static CButtonContainer s_SystemMessageColor;
		DoLine_ColorPicker(&s_SystemMessageColor, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &RightView, Localize("System message"), &g_Config.m_ClMessageSystemColor, ColorRGBA(1.0f, 1.0f, 0.5f), true, &g_Config.m_ClShowChatSystem);
		static CButtonContainer s_HighlightedMessageColor;
		DoLine_ColorPicker(&s_HighlightedMessageColor, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &RightView, Localize("Highlighted message"), &g_Config.m_ClMessageHighlightColor, ColorRGBA(1.0f, 0.5f, 0.5f));
		static CButtonContainer s_TeamMessageColor;
		DoLine_ColorPicker(&s_TeamMessageColor, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &RightView, Localize("Team message"), &g_Config.m_ClMessageTeamColor, ColorRGBA(0.65f, 1.0f, 0.65f));
		static CButtonContainer s_FriendMessageColor;
		DoLine_ColorPicker(&s_FriendMessageColor, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &RightView, Localize("Friend message"), &g_Config.m_ClMessageFriendColor, ColorRGBA(1.0f, 0.137f, 0.137f), true, &g_Config.m_ClMessageFriend);
		static CButtonContainer s_NormalMessageColor;
		DoLine_ColorPicker(&s_NormalMessageColor, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &RightView, Localize("Normal message"), &g_Config.m_ClMessageColor, ColorRGBA(1.0f, 1.0f, 1.0f));

		str_format(aBuf, sizeof(aBuf), "%s (echo)", Localize("Client message"));
		static CButtonContainer s_ClientMessageColor;
		// TClient
		DoLine_ColorPicker(&s_ClientMessageColor, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &RightView, aBuf, &g_Config.m_ClMessageClientColor, ColorRGBA(0.5f, 0.78f, 1.0f), true, &g_Config.m_TcShowChatClient);

		// ***** Chat Preview ***** //
		Ui()->DoLabel_AutoLineSize(Localize("Preview"), HeadlineFontSize,
			TEXTALIGN_ML, &PreviewView, HeadlineHeight);
		PreviewView.HSplitTop(MarginSmall, nullptr, &PreviewView);

		// Use the rest of the view for preview
		PreviewView.Draw(ColorRGBA(1, 1, 1, 0.1f), IGraphics::CORNER_ALL, 5.0f);
		PreviewView.Margin(MarginSmall, &PreviewView);

		ColorRGBA SystemColor = color_cast<ColorRGBA, ColorHSLA>(ColorHSLA(g_Config.m_ClMessageSystemColor));
		ColorRGBA HighlightedColor = color_cast<ColorRGBA, ColorHSLA>(ColorHSLA(g_Config.m_ClMessageHighlightColor));
		ColorRGBA TeamColor = color_cast<ColorRGBA, ColorHSLA>(ColorHSLA(g_Config.m_ClMessageTeamColor));
		ColorRGBA FriendColor = color_cast<ColorRGBA, ColorHSLA>(ColorHSLA(g_Config.m_ClMessageFriendColor));
		ColorRGBA NormalColor = color_cast<ColorRGBA, ColorHSLA>(ColorHSLA(g_Config.m_ClMessageColor));
		ColorRGBA ClientColor = color_cast<ColorRGBA, ColorHSLA>(ColorHSLA(g_Config.m_ClMessageClientColor));
		ColorRGBA DefaultNameColor(0.8f, 0.8f, 0.8f, 1.0f);

		const float RealFontSize = Chat.FontSize() * 2;
		const float RealMsgPaddingX = (!g_Config.m_ClChatOld ? Chat.MessagePaddingX() : 0) * 2;
		const float RealMsgPaddingY = (!g_Config.m_ClChatOld ? Chat.MessagePaddingY() : 0) * 2;
		const float RealMsgPaddingTee = (!g_Config.m_ClChatOld ? Chat.MessageTeeSize() + CChat::MESSAGE_TEE_PADDING_RIGHT : 0) * 2;
		const float RealOffsetY = RealFontSize + RealMsgPaddingY;

		const float X = RealMsgPaddingX / 2.0f + PreviewView.x;
		float Y = PreviewView.y;
		float LineWidth = g_Config.m_ClChatWidth * 2 - (RealMsgPaddingX * 1.5f) - RealMsgPaddingTee;

		str_copy(aBuf, Client()->PlayerName());

		const CAnimState *pIdleState = CAnimState::GetIdle();
		const float RealTeeSize = Chat.MessageTeeSize() * 2;
		const float RealTeeSizeHalved = Chat.MessageTeeSize();
		constexpr float TWSkinUnreliableOffset = -0.25f;
		const float OffsetTeeY = RealTeeSizeHalved;
		const float FullHeightMinusTee = RealOffsetY - RealTeeSize;

		struct SPreviewLine
		{
			int m_ClientId;
			bool m_Team;
			char m_aName[64];
			char m_aText[256];
			bool m_Friend;
			bool m_Player;
			bool m_Client;
			bool m_Highlighted;
			int m_TimesRepeated;

			CTeeRenderInfo m_RenderInfo;
		};

		static std::vector<SPreviewLine> s_vLines;

		enum ELineFlag
		{
			FLAG_TEAM = 1 << 0,
			FLAG_FRIEND = 1 << 1,
			FLAG_HIGHLIGHT = 1 << 2,
			FLAG_CLIENT = 1 << 3
		};
		enum
		{
			PREVIEW_SYS,
			PREVIEW_HIGHLIGHT,
			PREVIEW_TEAM,
			PREVIEW_FRIEND,
			PREVIEW_SPAMMER,
			PREVIEW_CLIENT
		};
		auto &&SetPreviewLine = [](int Index, int ClientId, const char *pName, const char *pText, int Flag, int Repeats) {
			SPreviewLine *pLine;
			if((int)s_vLines.size() <= Index)
			{
				s_vLines.emplace_back();
				pLine = &s_vLines.back();
			}
			else
			{
				pLine = &s_vLines[Index];
			}
			pLine->m_ClientId = ClientId;
			pLine->m_Team = Flag & FLAG_TEAM;
			pLine->m_Friend = Flag & FLAG_FRIEND;
			pLine->m_Player = ClientId >= 0;
			pLine->m_Highlighted = Flag & FLAG_HIGHLIGHT;
			pLine->m_Client = Flag & FLAG_CLIENT;
			pLine->m_TimesRepeated = Repeats;
			str_copy(pLine->m_aName, pName);
			str_copy(pLine->m_aText, pText);
		};
		auto &&SetLineSkin = [RealTeeSize](int Index, const CSkin *pSkin) {
			if(Index >= (int)s_vLines.size())
				return;
			s_vLines[Index].m_RenderInfo.m_Size = RealTeeSize;
			s_vLines[Index].m_RenderInfo.Apply(pSkin);
		};

		auto &&RenderPreview = [&](int LineIndex, int x, int y, bool Render = true) {
			if(LineIndex >= (int)s_vLines.size())
				return vec2(0, 0);
			CTextCursor LocalCursor;
			LocalCursor.SetPosition(vec2(x, y));
			LocalCursor.m_FontSize = RealFontSize;
			LocalCursor.m_Flags = Render ? TEXTFLAG_RENDER : 0;
			LocalCursor.m_LineWidth = LineWidth;
			const auto &Line = s_vLines[LineIndex];

			char aClientId[16] = "";
			if(g_Config.m_ClShowIds && Line.m_ClientId >= 0 && Line.m_aName[0] != '\0')
			{
				GameClient()->FormatClientId(Line.m_ClientId, aClientId, EClientIdFormat::INDENT_FORCE);
			}

			char aCount[12];
			if(Line.m_ClientId < 0)
				str_format(aCount, sizeof(aCount), "[%d] ", Line.m_TimesRepeated + 1);
			else
				str_format(aCount, sizeof(aCount), " [%d]", Line.m_TimesRepeated + 1);

			if(Line.m_Player)
			{
				LocalCursor.m_X += RealMsgPaddingTee;

				if(Line.m_Friend && g_Config.m_ClMessageFriend)
				{
					if(Render)
						TextRender()->TextColor(FriendColor);
					TextRender()->TextEx(&LocalCursor, "в™Ґ ", -1);
				}
			}

			ColorRGBA NameColor;
			if(Line.m_Team)
				NameColor = CalculateNameColor(color_cast<ColorHSLA>(TeamColor));
			else if(Line.m_Player)
				NameColor = DefaultNameColor;
			else if(Line.m_Client)
				NameColor = ClientColor;
			else
				NameColor = SystemColor;

			if(Render)
				TextRender()->TextColor(NameColor);

			TextRender()->TextEx(&LocalCursor, aClientId);
			TextRender()->TextEx(&LocalCursor, Line.m_aName);

			if(Line.m_TimesRepeated > 0)
			{
				if(Render)
					TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.3f);
				TextRender()->TextEx(&LocalCursor, aCount, -1);
			}

			if(Line.m_ClientId >= 0 && Line.m_aName[0] != '\0')
			{
				if(Render)
					TextRender()->TextColor(NameColor);
				TextRender()->TextEx(&LocalCursor, ": ", -1);
			}

			CTextCursor AppendCursor = LocalCursor;
			AppendCursor.m_LongestLineWidth = 0.0f;
			if(!g_Config.m_ClChatOld)
			{
				AppendCursor.m_StartX = LocalCursor.m_X;
				AppendCursor.m_LineWidth -= LocalCursor.m_LongestLineWidth;
			}

			if(Render)
			{
				if(Line.m_Highlighted)
					TextRender()->TextColor(HighlightedColor);
				else if(Line.m_Team)
					TextRender()->TextColor(TeamColor);
				else if(Line.m_Player)
					TextRender()->TextColor(NormalColor);
			}

			TextRender()->TextEx(&AppendCursor, Line.m_aText, -1);
			if(Render)
				TextRender()->TextColor(TextRender()->DefaultTextColor());

			return vec2{LocalCursor.m_LongestLineWidth + AppendCursor.m_LongestLineWidth, AppendCursor.Height() + RealMsgPaddingY};
		};

		// Set preview lines
		{
			char aLineBuilder[128];

			str_format(aLineBuilder, sizeof(aLineBuilder), "'%s' entered and joined the game", aBuf);
			SetPreviewLine(PREVIEW_SYS, -1, "*** ", aLineBuilder, 0, 0);

			str_format(aLineBuilder, sizeof(aLineBuilder), "Hey, how are you %s?", aBuf);
			SetPreviewLine(PREVIEW_HIGHLIGHT, 7, "Random Tee", aLineBuilder, FLAG_HIGHLIGHT, 0);

			SetPreviewLine(PREVIEW_TEAM, 11, "Your Teammate", "Let's speedrun this!", FLAG_TEAM, 0);
			SetPreviewLine(PREVIEW_FRIEND, 8, "Friend", "Hello there", FLAG_FRIEND, 0);
			SetPreviewLine(PREVIEW_SPAMMER, 9, "Spammer", "Hey fools, I'm spamming here!", 0, 5);
			SetPreviewLine(PREVIEW_CLIENT, -1, "вЂ” ", "Echo command executed", FLAG_CLIENT, 0);
		}

		SetLineSkin(1, GameClient()->m_Skins.Find("pinky"));
		SetLineSkin(2, GameClient()->m_Skins.Find("default"));
		SetLineSkin(3, GameClient()->m_Skins.Find("cammostripes"));
		SetLineSkin(4, GameClient()->m_Skins.Find("beast"));

		// Backgrounds first
		if(!g_Config.m_ClChatOld)
		{
			Graphics()->TextureClear();
			Graphics()->QuadsBegin();
			Graphics()->SetColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClChatBackgroundColor, true)));

			float TempY = Y;
			const float RealBackgroundRounding = Chat.MessageRounding() * 2.0f;

			auto &&RenderMessageBackground = [&](int LineIndex) {
				auto Size = RenderPreview(LineIndex, 0, 0, false);
				Graphics()->DrawRectExt(X - RealMsgPaddingX / 2.0f, TempY - RealMsgPaddingY / 2.0f, Size.x + RealMsgPaddingX * 1.5f, Size.y, RealBackgroundRounding, IGraphics::CORNER_ALL);
				return Size.y;
			};

			if(g_Config.m_ClShowChatSystem)
			{
				TempY += RenderMessageBackground(PREVIEW_SYS);
			}

			if(!g_Config.m_ClShowChatFriends)
			{
				if(!g_Config.m_ClShowChatTeamMembersOnly)
					TempY += RenderMessageBackground(PREVIEW_HIGHLIGHT);
				TempY += RenderMessageBackground(PREVIEW_TEAM);
			}

			if(!g_Config.m_ClShowChatTeamMembersOnly)
				TempY += RenderMessageBackground(PREVIEW_FRIEND);

			if(!g_Config.m_ClShowChatFriends && !g_Config.m_ClShowChatTeamMembersOnly)
			{
				TempY += RenderMessageBackground(PREVIEW_SPAMMER);
			}

			if(g_Config.m_TcShowChatClient)
			{
				TempY += RenderMessageBackground(PREVIEW_CLIENT);
			}

			Graphics()->QuadsEnd();
		}

		// System
		if(g_Config.m_ClShowChatSystem)
		{
			Y += RenderPreview(PREVIEW_SYS, X, Y).y;
		}

		if(!g_Config.m_ClShowChatFriends)
		{
			// Highlighted
			if(!g_Config.m_ClChatOld && !g_Config.m_ClShowChatTeamMembersOnly)
				RenderTools()->RenderTee(pIdleState, &s_vLines[PREVIEW_HIGHLIGHT].m_RenderInfo, EMOTE_NORMAL, vec2(1, 0.1f), vec2(X + RealTeeSizeHalved, Y + OffsetTeeY + FullHeightMinusTee / 2.0f + TWSkinUnreliableOffset));
			if(!g_Config.m_ClShowChatTeamMembersOnly)
				Y += RenderPreview(PREVIEW_HIGHLIGHT, X, Y).y;

			// Team
			if(!g_Config.m_ClChatOld)
				RenderTools()->RenderTee(pIdleState, &s_vLines[PREVIEW_TEAM].m_RenderInfo, EMOTE_NORMAL, vec2(1, 0.1f), vec2(X + RealTeeSizeHalved, Y + OffsetTeeY + FullHeightMinusTee / 2.0f + TWSkinUnreliableOffset));
			Y += RenderPreview(PREVIEW_TEAM, X, Y).y;
		}

		// Friend
		if(!g_Config.m_ClChatOld && !g_Config.m_ClShowChatTeamMembersOnly)
			RenderTools()->RenderTee(pIdleState, &s_vLines[PREVIEW_FRIEND].m_RenderInfo, EMOTE_NORMAL, vec2(1, 0.1f), vec2(X + RealTeeSizeHalved, Y + OffsetTeeY + FullHeightMinusTee / 2.0f + TWSkinUnreliableOffset));
		if(!g_Config.m_ClShowChatTeamMembersOnly)
			Y += RenderPreview(PREVIEW_FRIEND, X, Y).y;

		// Normal
		if(!g_Config.m_ClShowChatFriends && !g_Config.m_ClShowChatTeamMembersOnly)
		{
			if(!g_Config.m_ClChatOld)
				RenderTools()->RenderTee(pIdleState, &s_vLines[PREVIEW_SPAMMER].m_RenderInfo, EMOTE_NORMAL, vec2(1, 0.1f), vec2(X + RealTeeSizeHalved, Y + OffsetTeeY + FullHeightMinusTee / 2.0f + TWSkinUnreliableOffset));
			Y += RenderPreview(PREVIEW_SPAMMER, X, Y).y;
		}
		// Client
		if(g_Config.m_TcShowChatClient)
		{
			RenderPreview(PREVIEW_CLIENT, X, Y);
		}

		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}
	else if(s_CurTab == APPEARANCE_TAB_NAME_PLATE)
	{
		MainView.VSplitMid(&LeftView, &RightView, MarginBetweenViews);

		// ***** Name Plate ***** //
		Ui()->DoLabel_AutoLineSize(Localize("Name Plate"), HeadlineFontSize,
			TEXTALIGN_ML, &LeftView, HeadlineHeight);
		LeftView.HSplitTop(MarginSmall, nullptr, &LeftView);

		// General name plate settings
		{
			int Pressed = (g_Config.m_ClNamePlates ? 2 : 0) + (g_Config.m_ClNamePlatesOwn ? 1 : 0);
			if(DoLine_RadioMenu(LeftView, Localize("Show name plates"),
				   m_vButtonContainersNamePlateShow,
				   {Localize("None", "Show name plates"), Localize("Own", "Show name plates"), Localize("Others", "Show name plates"), Localize("All", "Show name plates")},
				   {0, 1, 2, 3},
				   Pressed))
			{
				g_Config.m_ClNamePlates = Pressed & 2 ? 1 : 0;
				g_Config.m_ClNamePlatesOwn = Pressed & 1 ? 1 : 0;
			}
		}
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		Ui()->DoScrollbarOption(&g_Config.m_ClNamePlatesSize, &g_Config.m_ClNamePlatesSize, &Button, Localize("Name plates size"), -50, 100);
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		Ui()->DoScrollbarOption(&g_Config.m_ClNamePlatesOffset, &g_Config.m_ClNamePlatesOffset, &Button, Localize("Name plates offset"), 10, 50);

		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClNamePlatesClan, Localize("Show clan above name plates"), &g_Config.m_ClNamePlatesClan, &LeftView, LineSize);
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(g_Config.m_ClNamePlatesClan)
			Ui()->DoScrollbarOption(&g_Config.m_ClNamePlatesClanSize, &g_Config.m_ClNamePlatesClanSize, &Button, Localize("Clan plates size"), -50, 100);

		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClNamePlatesTeamcolors, Localize("Use team colors for name plates"), &g_Config.m_ClNamePlatesTeamcolors, &LeftView, LineSize);
		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClNamePlatesFriendMark, Localize("Show friend icon in name plates"), &g_Config.m_ClNamePlatesFriendMark, &LeftView, LineSize);

		DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClNamePlatesIds, Localize("Show client IDs in name plates"), &g_Config.m_ClNamePlatesIds, &LeftView, LineSize);
		if(g_Config.m_ClNamePlatesIds > 0)
			DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_ClNamePlatesIdsSeparateLine, Localize("Show client IDs on a separate line"), &g_Config.m_ClNamePlatesIdsSeparateLine, &LeftView, LineSize);
		else
			LeftView.HSplitTop(LineSize, nullptr, &LeftView);
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(g_Config.m_ClNamePlatesIds > 0 && g_Config.m_ClNamePlatesIdsSeparateLine > 0)
			Ui()->DoScrollbarOption(&g_Config.m_ClNamePlatesIdsSize, &g_Config.m_ClNamePlatesIdsSize, &Button, Localize("Client IDs size"), -50, 100);

		// ***** Hook Strength ***** //
		LeftView.HSplitTop(MarginBetweenViews, nullptr, &LeftView);
		Ui()->DoLabel_AutoLineSize(Localize("Hook Strength"), HeadlineFontSize,
			TEXTALIGN_ML, &LeftView, HeadlineHeight);
		LeftView.HSplitTop(MarginSmall, nullptr, &LeftView);

		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(DoButton_CheckBox(&g_Config.m_ClNamePlatesStrong, Localize("Show hook strength icon indicator"), g_Config.m_ClNamePlatesStrong, &Button))
		{
			g_Config.m_ClNamePlatesStrong = g_Config.m_ClNamePlatesStrong ? 0 : 1;
		}
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(g_Config.m_ClNamePlatesStrong)
		{
			static int s_NamePlatesStrong = 0;
			if(DoButton_CheckBox(&s_NamePlatesStrong, Localize("Show hook strength number indicator"), g_Config.m_ClNamePlatesStrong == 2, &Button))
				g_Config.m_ClNamePlatesStrong = g_Config.m_ClNamePlatesStrong != 2 ? 2 : 1;
		}

		LeftView.HSplitTop(LineSize * 2.0f, &Button, &LeftView);
		if(g_Config.m_ClNamePlatesStrong)
		{
			Ui()->DoScrollbarOption(&g_Config.m_ClNamePlatesStrongSize, &g_Config.m_ClNamePlatesStrongSize, &Button, Localize("Size of hook strength icon and number indicator"), -50, 100, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_MULTILINE);
		}

		// ***** Key Presses ***** //
		LeftView.HSplitTop(MarginBetweenViews, nullptr, &LeftView);
		Ui()->DoLabel_AutoLineSize(Localize("Key Presses"), HeadlineFontSize,
			TEXTALIGN_ML, &LeftView, HeadlineHeight);
		LeftView.HSplitTop(MarginSmall, nullptr, &LeftView);

		DoLine_RadioMenu(LeftView, Localize("Show players' key presses"),
			m_vButtonContainersNamePlateKeyPresses,
			{Localize("None", "Show players' key presses"), Localize("Own", "Show players' key presses"), Localize("Others", "Show players' key presses"), Localize("All", "Show players' key presses")},
			{0, 3, 1, 2},
			g_Config.m_ClShowDirection);

		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(g_Config.m_ClShowDirection > 0)
			Ui()->DoScrollbarOption(&g_Config.m_ClDirectionSize, &g_Config.m_ClDirectionSize, &Button, Localize("Size of key press icons"), -50, 100);

		// ***** Name Plate Preview ***** //
		Ui()->DoLabel_AutoLineSize(Localize("Preview"), HeadlineFontSize,
			TEXTALIGN_ML, &RightView, HeadlineHeight);
		RightView.HSplitTop(2.0f * MarginSmall, nullptr, &RightView);

		// ***** Name Plate Dummy Preview ***** //
		RightView.HSplitBottom(LineSize, &RightView, &Button);
		if(DoButton_CheckBox(&m_DummyNamePlatePreview, g_Config.m_ClDummy ? Localize("Preview player's name plate") : Localize("Preview dummy's name plate"), m_DummyNamePlatePreview, &Button))
			m_DummyNamePlatePreview = !m_DummyNamePlatePreview;

		int Dummy = g_Config.m_ClDummy != (m_DummyNamePlatePreview ? 1 : 0);

		const vec2 Position = RightView.Center();

		GameClient()->m_NamePlates.RenderNamePlatePreview(Position, Dummy);
	}
	else if(s_CurTab == APPEARANCE_TAB_HOOK_COLLISION)
	{
		MainView.VSplitMid(&LeftView, &RightView, MarginBetweenViews);

		// ***** Hookline ***** //
		Ui()->DoLabel_AutoLineSize(Localize("Hook collision line"), HeadlineFontSize,
			TEXTALIGN_ML, &LeftView, HeadlineHeight);
		LeftView.HSplitTop(MarginSmall, nullptr, &LeftView);

		// General hookline settings
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(DoButton_CheckBox(&g_Config.m_ClShowHookCollOwn, Localize("Show own player's hook collision line"), g_Config.m_ClShowHookCollOwn, &Button))
		{
			g_Config.m_ClShowHookCollOwn = g_Config.m_ClShowHookCollOwn ? 0 : 1;
		}
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(g_Config.m_ClShowHookCollOwn)
		{
			static int s_ShowHookCollOwn = 0;
			if(DoButton_CheckBox(&s_ShowHookCollOwn, Localize("Always show own player's hook collision line"), g_Config.m_ClShowHookCollOwn == 2, &Button))
				g_Config.m_ClShowHookCollOwn = g_Config.m_ClShowHookCollOwn != 2 ? 2 : 1;
		}

		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(DoButton_CheckBox(&g_Config.m_ClShowHookCollOther, Localize("Show other players' hook collision lines"), g_Config.m_ClShowHookCollOther, &Button))
		{
			g_Config.m_ClShowHookCollOther = g_Config.m_ClShowHookCollOther >= 1 ? 0 : 1;
		}
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(g_Config.m_ClShowHookCollOther)
		{
			static int s_ShowHookCollOther = 0;
			if(DoButton_CheckBox(&s_ShowHookCollOther, Localize("Always show other players' hook collision lines"), g_Config.m_ClShowHookCollOther == 2, &Button))
				g_Config.m_ClShowHookCollOther = g_Config.m_ClShowHookCollOther != 2 ? 2 : 1;
		}

		LeftView.HSplitTop(LineSize * 2.0f, &Button, &LeftView);
		Ui()->DoScrollbarOption(&g_Config.m_ClHookCollSize, &g_Config.m_ClHookCollSize, &Button, Localize("Width of your own hook collision line"), 0, 20, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_MULTILINE);

		LeftView.HSplitTop(LineSize * 2.0f, &Button, &LeftView);
		Ui()->DoScrollbarOption(&g_Config.m_ClHookCollSizeOther, &g_Config.m_ClHookCollSizeOther, &Button, Localize("Width of others' hook collision line"), 0, 20, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_MULTILINE);

		LeftView.HSplitTop(LineSize * 2.0f, &Button, &LeftView);
		Ui()->DoScrollbarOption(&g_Config.m_ClHookCollAlpha, &g_Config.m_ClHookCollAlpha, &Button, Localize("Hook collision line opacity"), 0, 100, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_MULTILINE, "%");

		static CButtonContainer s_HookCollNoCollResetId, s_HookCollHookableCollResetId, s_HookCollTeeCollResetId, s_HookCollTipColorResetId;
		static int s_HookCollToolTip;

		Ui()->DoLabel_AutoLineSize(Localize("Colors of the hook collision line:"), 13.0f,
			TEXTALIGN_ML, &LeftView, HeadlineHeight);

		Ui()->DoButtonLogic(&s_HookCollToolTip, 0, &LeftView, BUTTONFLAG_NONE); // Just for the tooltip, result ignored
		GameClient()->m_Tooltips.DoToolTip(&s_HookCollToolTip, &LeftView, Localize("Your movements are not taken into account when calculating the line colors"));
		DoLine_ColorPicker(&s_HookCollNoCollResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("When nothing is hookable", "Hook collision line color"), &g_Config.m_ClHookCollColorNoColl, ColorRGBA(1.0f, 0.0f, 0.0f, 1.0f), false);
		DoLine_ColorPicker(&s_HookCollHookableCollResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("When something is hookable", "Hook collision line color"), &g_Config.m_ClHookCollColorHookableColl, ColorRGBA(130.0f / 255.0f, 232.0f / 255.0f, 160.0f / 255.0f, 1.0f), false);
		DoLine_ColorPicker(&s_HookCollTeeCollResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("When a Tee is hookable", "Hook collision line color"), &g_Config.m_ClHookCollColorTeeColl, ColorRGBA(1.0f, 1.0f, 0.0f, 1.0f), false);
		DoLine_ColorPicker(&s_HookCollTipColorResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Hook collision line tip", "Hook collision line color"), &g_Config.m_ClHookCollTipColor, ColorRGBA(1.0f, 1.0f, 0.0f, 0.5f), false, nullptr, true);

		// ***** Hook collisions preview ***** //
		Ui()->DoLabel_AutoLineSize(Localize("Preview"), HeadlineFontSize,
			TEXTALIGN_ML, &RightView, HeadlineHeight);
		RightView.HSplitTop(2 * MarginSmall, nullptr, &RightView);

		auto DoHookCollision = [this](const vec2 &Pos, const float &Length, const int &Size, const ColorRGBA &Color, const ColorRGBA &TipColor, const bool &Invert) {
			ColorRGBA ColorModified = Color;
			ColorRGBA TipColorModified = TipColor;
			if(Invert)
				ColorModified = color_invert(ColorModified);
			ColorModified = ColorModified.WithAlpha((float)g_Config.m_ClHookCollAlpha / 100);
			TipColorModified = TipColor.WithMultipliedAlpha((float)g_Config.m_ClHookCollAlpha / 100);
			Graphics()->TextureClear();
			if(Size > 0)
			{
				Graphics()->QuadsBegin();
				Graphics()->SetColor(ColorModified);
				float LineWidth = 0.5f + (float)(Size - 1) * 0.25f;
				IGraphics::CQuadItem QuadItem(Pos.x, Pos.y - LineWidth, Length, LineWidth * 2.f);
				Graphics()->QuadsDrawTL(&QuadItem, 1);
				if(TipColor.a > 0.0f)
				{
					Graphics()->SetColor(TipColorModified);
					IGraphics::CQuadItem TipQuadItem(Pos.x + Length, Pos.y - LineWidth, 15.f, LineWidth * 2.f);
					Graphics()->QuadsDrawTL(&TipQuadItem, 1);
				}
				Graphics()->QuadsEnd();
			}
			else
			{
				Graphics()->LinesBegin();
				Graphics()->SetColor(ColorModified);
				IGraphics::CLineItem LineItem(Pos.x, Pos.y, Pos.x + Length, Pos.y);
				Graphics()->LinesDraw(&LineItem, 1);
				if(TipColor.a > 0.0f)
				{
					Graphics()->SetColor(TipColorModified);
					IGraphics::CLineItem TipLineItem(Pos.x + Length, Pos.y, Pos.x + Length + 15.f, Pos.y);
					Graphics()->LinesDraw(&TipLineItem, 1);
				}
				Graphics()->LinesEnd();
			}
		};

		CTeeRenderInfo OwnSkinInfo;
		OwnSkinInfo.Apply(GameClient()->m_Skins.Find(g_Config.m_ClPlayerSkin));
		OwnSkinInfo.ApplyColors(g_Config.m_ClPlayerUseCustomColor, g_Config.m_ClPlayerColorBody, g_Config.m_ClPlayerColorFeet);
		OwnSkinInfo.m_Size = 50.0f;

		CTeeRenderInfo DummySkinInfo;
		DummySkinInfo.Apply(GameClient()->m_Skins.Find(g_Config.m_ClDummySkin));
		DummySkinInfo.ApplyColors(g_Config.m_ClDummyUseCustomColor, g_Config.m_ClDummyColorBody, g_Config.m_ClDummyColorFeet);
		DummySkinInfo.m_Size = 50.0f;

		vec2 TeeRenderPos, DummyRenderPos;

		const float LineLength = 150.f;
		const float LeftMargin = 30.f;

		const int TileScale = 32.0f;

		// Toggled via checkbox later, inverts some previews
		static bool s_HookCollPressed = false;

		CUIRect PreviewColl;

		// ***** Unhookable Tile Preview *****
		CUIRect PreviewNoColl;
		RightView.HSplitTop(50.0f, &PreviewNoColl, &RightView);
		RightView.HSplitTop(4 * MarginSmall, nullptr, &RightView);
		TeeRenderPos = vec2(PreviewNoColl.x + LeftMargin, PreviewNoColl.y + PreviewNoColl.h / 2.0f);
		DoHookCollision(TeeRenderPos, PreviewNoColl.w - LineLength, g_Config.m_ClHookCollSize, color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollColorNoColl)), ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f), s_HookCollPressed);
		RenderTools()->RenderTee(CAnimState::GetIdle(), &OwnSkinInfo, 0, vec2(1.0f, 0.0f), TeeRenderPos);

		CUIRect NoHookTileRect;
		PreviewNoColl.VSplitRight(LineLength, &PreviewNoColl, &NoHookTileRect);
		NoHookTileRect.VSplitLeft(50.0f, &NoHookTileRect, nullptr);
		NoHookTileRect.Margin(10.0f, &NoHookTileRect);

		// Render unhookable tile
		Graphics()->TextureClear();
		Graphics()->TextureSet(GameClient()->m_MapImages.GetEntities(MAP_IMAGE_ENTITY_LAYER_TYPE_ALL_EXCEPT_SWITCH));
		Graphics()->BlendNormal();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		RenderMap()->RenderTile(NoHookTileRect.x, NoHookTileRect.y, TILE_NOHOOK, TileScale, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));

		// ***** Hookable Tile Preview *****
		RightView.HSplitTop(50.0f, &PreviewColl, &RightView);
		RightView.HSplitTop(4 * MarginSmall, nullptr, &RightView);
		TeeRenderPos = vec2(PreviewColl.x + LeftMargin, PreviewColl.y + PreviewColl.h / 2.0f);
		DoHookCollision(TeeRenderPos, PreviewColl.w - LineLength, g_Config.m_ClHookCollSize, color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollColorHookableColl)), ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f), s_HookCollPressed);
		RenderTools()->RenderTee(CAnimState::GetIdle(), &OwnSkinInfo, 0, vec2(1.0f, 0.0f), TeeRenderPos);

		CUIRect HookTileRect;
		PreviewColl.VSplitRight(LineLength, &PreviewColl, &HookTileRect);
		HookTileRect.VSplitLeft(50.0f, &HookTileRect, nullptr);
		HookTileRect.Margin(10.0f, &HookTileRect);

		// Render hookable tile
		Graphics()->TextureClear();
		Graphics()->TextureSet(GameClient()->m_MapImages.GetEntities(MAP_IMAGE_ENTITY_LAYER_TYPE_ALL_EXCEPT_SWITCH));
		Graphics()->BlendNormal();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		RenderMap()->RenderTile(HookTileRect.x, HookTileRect.y, TILE_SOLID, TileScale, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));

		// ***** Hook Dummy Preview *****
		RightView.HSplitTop(50.0f, &PreviewColl, &RightView);
		RightView.HSplitTop(4 * MarginSmall, nullptr, &RightView);
		TeeRenderPos = vec2(PreviewColl.x + LeftMargin, PreviewColl.y + PreviewColl.h / 2.0f);
		DummyRenderPos = vec2(PreviewColl.x + PreviewColl.w - LineLength - 5.f + LeftMargin, PreviewColl.y + PreviewColl.h / 2.0f);
		DoHookCollision(TeeRenderPos, PreviewColl.w - LineLength - 15.f, g_Config.m_ClHookCollSize, color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollColorTeeColl)), ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f), s_HookCollPressed);
		RenderTools()->RenderTee(CAnimState::GetIdle(), &DummySkinInfo, 0, vec2(1.0f, 0.0f), DummyRenderPos);
		RenderTools()->RenderTee(CAnimState::GetIdle(), &OwnSkinInfo, 0, vec2(1.0f, 0.0f), TeeRenderPos);

		// ***** Hook Dummy Reverse Preview *****
		RightView.HSplitTop(50.0f, &PreviewColl, &RightView);
		RightView.HSplitTop(4 * MarginSmall, nullptr, &RightView);
		TeeRenderPos = vec2(PreviewColl.x + LeftMargin, PreviewColl.y + PreviewColl.h / 2.0f);
		DummyRenderPos = vec2(PreviewColl.x + PreviewColl.w - LineLength - 5.f + LeftMargin, PreviewColl.y + PreviewColl.h / 2.0f);
		DoHookCollision(TeeRenderPos, PreviewColl.w - LineLength - 15.f, g_Config.m_ClHookCollSizeOther, color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollColorTeeColl)), ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f), false);
		RenderTools()->RenderTee(CAnimState::GetIdle(), &OwnSkinInfo, 0, vec2(1.0f, 0.0f), DummyRenderPos);
		RenderTools()->RenderTee(CAnimState::GetIdle(), &DummySkinInfo, 0, vec2(1.0f, 0.0f), TeeRenderPos);

		// ***** Hook Line Tip Preview *****
		RightView.HSplitTop(50.0f, &PreviewColl, &RightView);
		RightView.HSplitTop(4 * MarginSmall, nullptr, &RightView);
		TeeRenderPos = vec2(PreviewColl.x + LeftMargin, PreviewColl.y + PreviewColl.h / 2.0f);
		DoHookCollision(TeeRenderPos, PreviewColl.w - LineLength - 15.f, g_Config.m_ClHookCollSize, color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollColorNoColl)), color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClHookCollTipColor, true)), s_HookCollPressed);
		RenderTools()->RenderTee(CAnimState::GetIdle(), &OwnSkinInfo, 0, vec2(1.0f, 0.0f), TeeRenderPos);

		// ***** Preview +hookcoll pressed toggle *****
		RightView.HSplitTop(LineSize, &Button, &RightView);
		if(DoButton_CheckBox(&s_HookCollPressed, Localize("Preview 'Hook collisions' being pressed"), s_HookCollPressed, &Button))
			s_HookCollPressed = !s_HookCollPressed;
	}
	else if(s_CurTab == APPEARANCE_TAB_INFO_MESSAGES)
	{
		MainView.VSplitMid(&LeftView, &RightView, MarginBetweenViews);

		// ***** Info Messages ***** //
		Ui()->DoLabel_AutoLineSize(Localize("Info Messages"), HeadlineFontSize,
			TEXTALIGN_ML, &LeftView, HeadlineHeight);
		LeftView.HSplitTop(MarginSmall, nullptr, &LeftView);

		// General info messages settings
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(DoButton_CheckBox(&g_Config.m_ClShowKillMessages, Localize("Show kill messages"), g_Config.m_ClShowKillMessages, &Button))
		{
			g_Config.m_ClShowKillMessages ^= 1;
		}

		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(DoButton_CheckBox(&g_Config.m_ClShowFinishMessages, Localize("Show finish messages"), g_Config.m_ClShowFinishMessages, &Button))
		{
			g_Config.m_ClShowFinishMessages ^= 1;
		}

		static CButtonContainer s_KillMessageNormalColorId, s_KillMessageHighlightColorId;
		DoLine_ColorPicker(&s_KillMessageNormalColorId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Normal Color"), &g_Config.m_ClKillMessageNormalColor, ColorRGBA(1.0f, 1.0f, 1.0f), false);
		DoLine_ColorPicker(&s_KillMessageHighlightColorId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Highlight Color"), &g_Config.m_ClKillMessageHighlightColor, ColorRGBA(1.0f, 1.0f, 1.0f), false);
	}
	else if(s_CurTab == APPEARANCE_TAB_LASER)
	{
		MainView.VSplitMid(&LeftView, &RightView, MarginBetweenViews);

		// ***** Weapons ***** //
		Ui()->DoLabel_AutoLineSize(Localize("Weapons"), HeadlineFontSize,
			TEXTALIGN_ML, &LeftView, HeadlineHeight);
		LeftView.HSplitTop(MarginSmall, nullptr, &LeftView);

		// General weapon laser settings
		static CButtonContainer s_LaserRifleOutResetId, s_LaserRifleInResetId, s_LaserShotgunOutResetId, s_LaserShotgunInResetId;

		ColorHSLA LaserRifleOutlineColor = DoLine_ColorPicker(&s_LaserRifleOutResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Rifle Laser Outline Color"), &g_Config.m_ClLaserRifleOutlineColor, ColorRGBA(0.074402f, 0.074402f, 0.247166f, 1.0f), false);
		ColorHSLA LaserRifleInnerColor = DoLine_ColorPicker(&s_LaserRifleInResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Rifle Laser Inner Color"), &g_Config.m_ClLaserRifleInnerColor, ColorRGBA(0.498039f, 0.498039f, 1.0f, 1.0f), false);
		ColorHSLA LaserShotgunOutlineColor = DoLine_ColorPicker(&s_LaserShotgunOutResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Shotgun Laser Outline Color"), &g_Config.m_ClLaserShotgunOutlineColor, ColorRGBA(0.125490f, 0.098039f, 0.043137f, 1.0f), false);
		ColorHSLA LaserShotgunInnerColor = DoLine_ColorPicker(&s_LaserShotgunInResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Shotgun Laser Inner Color"), &g_Config.m_ClLaserShotgunInnerColor, ColorRGBA(0.570588f, 0.417647f, 0.252941f, 1.0f), false);

		// ***** Entities ***** //
		LeftView.HSplitTop(10.0f, nullptr, &LeftView);
		Ui()->DoLabel_AutoLineSize(Localize("Entities"), HeadlineFontSize,
			TEXTALIGN_ML, &LeftView, HeadlineHeight);
		LeftView.HSplitTop(MarginSmall, nullptr, &LeftView);

		// General entity laser settings
		static CButtonContainer s_LaserDoorOutResetId, s_LaserDoorInResetId, s_LaserFreezeOutResetId, s_LaserFreezeInResetId, s_LaserDraggerOutResetId, s_LaserDraggerInResetId;

		ColorHSLA LaserDoorOutlineColor = DoLine_ColorPicker(&s_LaserDoorOutResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Door Laser Outline Color"), &g_Config.m_ClLaserDoorOutlineColor, ColorRGBA(0.0f, 0.131372f, 0.096078f, 1.0f), false);
		ColorHSLA LaserDoorInnerColor = DoLine_ColorPicker(&s_LaserDoorInResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Door Laser Inner Color"), &g_Config.m_ClLaserDoorInnerColor, ColorRGBA(0.262745f, 0.760784f, 0.639215f, 1.0f), false);
		ColorHSLA LaserFreezeOutlineColor = DoLine_ColorPicker(&s_LaserFreezeOutResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Freeze Laser Outline Color"), &g_Config.m_ClLaserFreezeOutlineColor, ColorRGBA(0.131372f, 0.123529f, 0.182352f, 1.0f), false);
		ColorHSLA LaserFreezeInnerColor = DoLine_ColorPicker(&s_LaserFreezeInResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Freeze Laser Inner Color"), &g_Config.m_ClLaserFreezeInnerColor, ColorRGBA(0.482352f, 0.443137f, 0.564705f, 1.0f), false);
		ColorHSLA LaserDraggerOutlineColor = DoLine_ColorPicker(&s_LaserDraggerOutResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Dragger Outline Color"), &g_Config.m_ClLaserDraggerOutlineColor, ColorRGBA(0.1640625f, 0.015625f, 0.015625f, 1.0f), false);
		ColorHSLA LaserDraggerInnerColor = DoLine_ColorPicker(&s_LaserDraggerInResetId, ColorPickerLineSize, ColorPickerLabelSize, ColorPickerLineSpacing, &LeftView, Localize("Dragger Inner Color"), &g_Config.m_ClLaserDraggerInnerColor, ColorRGBA(.8666666f, .3725490f, .3725490f, 1.0f), false);

		static CButtonContainer s_AllToRifleResetId, s_AllToDefaultResetId;

		LeftView.HSplitTop(4 * MarginSmall, nullptr, &LeftView);
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(DoButton_Menu(&s_AllToRifleResetId, Localize("Set all to Rifle"), 0, &Button))
		{
			g_Config.m_ClLaserShotgunOutlineColor = g_Config.m_ClLaserRifleOutlineColor;
			g_Config.m_ClLaserShotgunInnerColor = g_Config.m_ClLaserRifleInnerColor;
			g_Config.m_ClLaserDoorOutlineColor = g_Config.m_ClLaserRifleOutlineColor;
			g_Config.m_ClLaserDoorInnerColor = g_Config.m_ClLaserRifleInnerColor;
			g_Config.m_ClLaserFreezeOutlineColor = g_Config.m_ClLaserRifleOutlineColor;
			g_Config.m_ClLaserFreezeInnerColor = g_Config.m_ClLaserRifleInnerColor;
			g_Config.m_ClLaserDraggerOutlineColor = g_Config.m_ClLaserRifleOutlineColor;
			g_Config.m_ClLaserDraggerInnerColor = g_Config.m_ClLaserRifleInnerColor;
		}

		// values taken from the CL commands
		LeftView.HSplitTop(2 * MarginSmall, nullptr, &LeftView);
		LeftView.HSplitTop(LineSize, &Button, &LeftView);
		if(DoButton_Menu(&s_AllToDefaultResetId, Localize("Reset to defaults"), 0, &Button))
		{
			g_Config.m_ClLaserRifleOutlineColor = 11176233;
			g_Config.m_ClLaserRifleInnerColor = 11206591;
			g_Config.m_ClLaserShotgunOutlineColor = 1866773;
			g_Config.m_ClLaserShotgunInnerColor = 1467241;
			g_Config.m_ClLaserDoorOutlineColor = 7667473;
			g_Config.m_ClLaserDoorInnerColor = 7701379;
			g_Config.m_ClLaserFreezeOutlineColor = 11613223;
			g_Config.m_ClLaserFreezeInnerColor = 12001153;
			g_Config.m_ClLaserDraggerOutlineColor = 57618;
			g_Config.m_ClLaserDraggerInnerColor = 42398;
		}

		// ***** Laser Preview ***** //
		Ui()->DoLabel_AutoLineSize(Localize("Preview"), HeadlineFontSize,
			TEXTALIGN_ML, &RightView, HeadlineHeight);
		RightView.HSplitTop(MarginSmall, nullptr, &RightView);

		const float LaserPreviewHeight = 60.0f;
		CUIRect LaserPreview;
		RightView.HSplitTop(LaserPreviewHeight, &LaserPreview, &RightView);
		RightView.HSplitTop(2 * MarginSmall, nullptr, &RightView);
		DoLaserPreview(&LaserPreview, LaserRifleOutlineColor, LaserRifleInnerColor, LASERTYPE_RIFLE);

		RightView.HSplitTop(LaserPreviewHeight, &LaserPreview, &RightView);
		RightView.HSplitTop(2 * MarginSmall, nullptr, &RightView);
		DoLaserPreview(&LaserPreview, LaserShotgunOutlineColor, LaserShotgunInnerColor, LASERTYPE_SHOTGUN);

		RightView.HSplitTop(LaserPreviewHeight, &LaserPreview, &RightView);
		RightView.HSplitTop(2 * MarginSmall, nullptr, &RightView);
		DoLaserPreview(&LaserPreview, LaserDoorOutlineColor, LaserDoorInnerColor, LASERTYPE_DOOR);

		RightView.HSplitTop(LaserPreviewHeight, &LaserPreview, &RightView);
		RightView.HSplitTop(2 * MarginSmall, nullptr, &RightView);
		DoLaserPreview(&LaserPreview, LaserFreezeOutlineColor, LaserFreezeInnerColor, LASERTYPE_FREEZE);

		RightView.HSplitTop(LaserPreviewHeight, &LaserPreview, &RightView);
		RightView.HSplitTop(2 * MarginSmall, nullptr, &RightView);
		DoLaserPreview(&LaserPreview, LaserDraggerOutlineColor, LaserDraggerInnerColor, LASERTYPE_DRAGGER);
	}
}

void CMenus::RenderSettingsDDNet(CUIRect MainView)
{
	CUIRect Button, Left, Right, LeftLeft, Label;

#if defined(CONF_AUTOUPDATE)
	CUIRect UpdaterRect;
	MainView.HSplitBottom(20.0f, &MainView, &UpdaterRect);
	MainView.HSplitBottom(5.0f, &MainView, nullptr);
#endif

	// demo
	CUIRect Demo;
	MainView.HSplitTop(110.0f, &Demo, &MainView);
	Demo.HSplitTop(30.0f, &Label, &Demo);
	Ui()->DoLabel(&Label, Localize("Demo"), 20.0f, TEXTALIGN_ML);
	Label.VSplitMid(nullptr, &Label, 20.0f);
	Ui()->DoLabel(&Label, Localize("Ghost"), 20.0f, TEXTALIGN_ML);

	Demo.HSplitTop(5.0f, nullptr, &Demo);
	Demo.VSplitMid(&Left, &Right, 20.0f);

	Left.HSplitTop(20.0f, &Button, &Left);
	if(DoButton_CheckBox(&g_Config.m_ClAutoRaceRecord, Localize("Save the best demo of each race"), g_Config.m_ClAutoRaceRecord, &Button))
	{
		g_Config.m_ClAutoRaceRecord ^= 1;
	}

	Left.HSplitTop(20.0f, &Button, &Left);
	if(DoButton_CheckBox(&g_Config.m_ClReplays, Localize("Enable replays"), g_Config.m_ClReplays, &Button))
	{
		g_Config.m_ClReplays ^= 1;
		if(Client()->State() == IClient::STATE_ONLINE)
		{
			Client()->DemoRecorder_UpdateReplayRecorder();
		}
	}

	Left.HSplitTop(20.0f, &Button, &Left);
	if(g_Config.m_ClReplays)
		Ui()->DoScrollbarOption(&g_Config.m_ClReplayLength, &g_Config.m_ClReplayLength, &Button, Localize("Default length"), 10, 600, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_NOCLAMPVALUE);

	Right.HSplitTop(20.0f, &Button, &Right);
	if(DoButton_CheckBox(&g_Config.m_ClRaceGhost, Localize("Enable ghost"), g_Config.m_ClRaceGhost, &Button))
	{
		g_Config.m_ClRaceGhost ^= 1;
	}
	GameClient()->m_Tooltips.DoToolTip(&g_Config.m_ClRaceGhost, &Button, Localize("When you cross the start line, show a ghost tee replicating the movements of your best time"));

	if(g_Config.m_ClRaceGhost)
	{
		Right.HSplitTop(20.0f, &Button, &Right);
		Button.VSplitMid(&LeftLeft, &Button);
		if(DoButton_CheckBox(&g_Config.m_ClRaceShowGhost, Localize("Show ghost"), g_Config.m_ClRaceShowGhost, &LeftLeft))
		{
			g_Config.m_ClRaceShowGhost ^= 1;
		}
		Ui()->DoScrollbarOption(&g_Config.m_ClRaceGhostAlpha, &g_Config.m_ClRaceGhostAlpha, &Button, Localize("Opacity"), 0, 100, &CUi::ms_LinearScrollbarScale, 0u, "%");

		Right.HSplitTop(20.0f, &Button, &Right);
		if(DoButton_CheckBox(&g_Config.m_ClRaceSaveGhost, Localize("Save ghost"), g_Config.m_ClRaceSaveGhost, &Button))
		{
			g_Config.m_ClRaceSaveGhost ^= 1;
		}

		if(g_Config.m_ClRaceSaveGhost)
		{
			Right.HSplitTop(20.0f, &Button, &Right);
			if(DoButton_CheckBox(&g_Config.m_ClRaceGhostSaveBest, Localize("Only save improvements"), g_Config.m_ClRaceGhostSaveBest, &Button))
			{
				g_Config.m_ClRaceGhostSaveBest ^= 1;
			}
		}
	}

	// gameplay
	CUIRect Gameplay;
	MainView.HSplitTop(170.0f, &Gameplay, &MainView);
	Gameplay.HSplitTop(30.0f, &Label, &Gameplay);
	Ui()->DoLabel(&Label, Localize("Gameplay"), 20.0f, TEXTALIGN_ML);
	Gameplay.HSplitTop(5.0f, nullptr, &Gameplay);
	Gameplay.VSplitMid(&Left, &Right, 20.0f);

	Left.HSplitTop(20.0f, &Button, &Left);
	Ui()->DoScrollbarOption(&g_Config.m_ClOverlayEntities, &g_Config.m_ClOverlayEntities, &Button, Localize("Overlay entities"), 0, 100);

	Left.HSplitTop(20.0f, &Button, &Left);
	Button.VSplitMid(&LeftLeft, &Button);

	if(DoButton_CheckBox(&g_Config.m_ClTextEntities, Localize("Show text entities"), g_Config.m_ClTextEntities, &LeftLeft))
		g_Config.m_ClTextEntities ^= 1;

	if(g_Config.m_ClTextEntities)
	{
		if(Ui()->DoScrollbarOption(&g_Config.m_ClTextEntitiesSize, &g_Config.m_ClTextEntitiesSize, &Button, Localize("Size"), 20, 100, &CUi::ms_LinearScrollbarScale, CUi::SCROLLBAR_OPTION_DELAYUPDATE))
			GameClient()->m_MapImages.SetTextureScale(g_Config.m_ClTextEntitiesSize);
	}

	Left.HSplitTop(20.0f, &Button, &Left);
	Button.VSplitMid(&LeftLeft, &Button);

	if(DoButton_CheckBox(&g_Config.m_ClShowOthers, Localize("Show others"), g_Config.m_ClShowOthers == SHOW_OTHERS_ON, &LeftLeft))
		g_Config.m_ClShowOthers = g_Config.m_ClShowOthers != SHOW_OTHERS_ON ? SHOW_OTHERS_ON : SHOW_OTHERS_OFF;

	Ui()->DoScrollbarOption(&g_Config.m_ClShowOthersAlpha, &g_Config.m_ClShowOthersAlpha, &Button, Localize("Opacity"), 0, 100, &CUi::ms_LinearScrollbarScale, 0u, "%");

	GameClient()->m_Tooltips.DoToolTip(&g_Config.m_ClShowOthersAlpha, &Button, Localize("Adjust the opacity of entities belonging to other teams, such as tees and name plates"));

	Left.HSplitTop(20.0f, &Button, &Left);
	static int s_ShowOwnTeamId = 0;
	if(DoButton_CheckBox(&s_ShowOwnTeamId, Localize("Show others (own team only)"), g_Config.m_ClShowOthers == SHOW_OTHERS_ONLY_TEAM, &Button))
	{
		g_Config.m_ClShowOthers = g_Config.m_ClShowOthers != SHOW_OTHERS_ONLY_TEAM ? SHOW_OTHERS_ONLY_TEAM : SHOW_OTHERS_OFF;
	}

	Left.HSplitTop(20.0f, &Button, &Left);
	if(DoButton_CheckBox(&g_Config.m_ClShowQuads, Localize("Show background quads"), g_Config.m_ClShowQuads, &Button))
	{
		g_Config.m_ClShowQuads ^= 1;
	}
	GameClient()->m_Tooltips.DoToolTip(&g_Config.m_ClShowQuads, &Button, Localize("Quads are used for background decoration"));

	Left.HSplitTop(20.0f, &Button, &Left);
	if(Ui()->DoScrollbarOption(&g_Config.m_ClDefaultZoom, &g_Config.m_ClDefaultZoom, &Button, Localize("Default zoom"), 0, 20))
		GameClient()->m_Camera.SetZoom(CCamera::ZoomStepsToValue(g_Config.m_ClDefaultZoom - 10), g_Config.m_ClSmoothZoomTime, true);

	Right.HSplitTop(20.0f, &Button, &Right);
	Ui()->DoScrollbarOption(&g_Config.m_ClPredictionMargin, &g_Config.m_ClPredictionMargin, &Button, Localize("Prediction margin"), 1, 300);

	Right.HSplitTop(20.0f, &Button, &Right);
	if(DoButton_CheckBox(&g_Config.m_ClPredictEvents, Localize("Predict events (experimental)"), g_Config.m_ClPredictEvents, &Button))
	{
		g_Config.m_ClPredictEvents ^= 1;
	}

	Right.HSplitTop(20.0f, &Button, &Right);
	if(DoButton_CheckBox(&g_Config.m_ClAntiPing, Localize("AntiPing"), g_Config.m_ClAntiPing, &Button))
	{
		g_Config.m_ClAntiPing ^= 1;
	}
	GameClient()->m_Tooltips.DoToolTip(&g_Config.m_ClAntiPing, &Button, Localize("Tries to predict other entities to give a feel of low latency"));

	if(g_Config.m_ClAntiPing)
	{
		Right.HSplitTop(20.0f, &Button, &Right);
		if(DoButton_CheckBox(&g_Config.m_ClAntiPingPlayers, Localize("AntiPing: predict other players"), g_Config.m_ClAntiPingPlayers, &Button))
		{
			g_Config.m_ClAntiPingPlayers ^= 1;
		}

		Right.HSplitTop(20.0f, &Button, &Right);
		if(DoButton_CheckBox(&g_Config.m_ClAntiPingWeapons, Localize("AntiPing: predict weapons"), g_Config.m_ClAntiPingWeapons, &Button))
		{
			g_Config.m_ClAntiPingWeapons ^= 1;
		}

		Right.HSplitTop(20.0f, &Button, &Right);
		if(DoButton_CheckBox(&g_Config.m_ClAntiPingGrenade, Localize("AntiPing: predict grenade paths"), g_Config.m_ClAntiPingGrenade, &Button))
		{
			g_Config.m_ClAntiPingGrenade ^= 1;
		}
	}

	CUIRect Background, Miscellaneous;
	MainView.VSplitMid(&Background, &Miscellaneous, 20.0f);

	// background
	Background.HSplitTop(30.0f, &Label, &Background);
	Background.HSplitTop(5.0f, nullptr, &Background);
	Ui()->DoLabel(&Label, Localize("Background"), 20.0f, TEXTALIGN_ML);

	ColorRGBA GreyDefault(0.5f, 0.5f, 0.5f, 1);

	static CButtonContainer s_ResetId1;
	DoLine_ColorPicker(&s_ResetId1, 25.0f, 13.0f, 5.0f, &Background, Localize("Regular background color"), &g_Config.m_ClBackgroundColor, GreyDefault, false);

	static CButtonContainer s_ResetId2;
	DoLine_ColorPicker(&s_ResetId2, 25.0f, 13.0f, 5.0f, &Background, Localize("Entities background color"), &g_Config.m_ClBackgroundEntitiesColor, GreyDefault, false);

	CUIRect EditBox, ReloadButton;
	Background.HSplitTop(20.0f, &Label, &Background);
	Background.HSplitTop(2.0f, nullptr, &Background);
	Label.VSplitLeft(100.0f, &Label, &EditBox);
	EditBox.VSplitRight(60.0f, &EditBox, &Button);
	Button.VSplitMid(&ReloadButton, &Button, 5.0f);
	EditBox.VSplitRight(5.0f, &EditBox, nullptr);

	Ui()->DoLabel(&Label, Localize("Map"), 14.0f, TEXTALIGN_ML);

	static CLineInput s_BackgroundEntitiesInput(g_Config.m_ClBackgroundEntities, sizeof(g_Config.m_ClBackgroundEntities));
	Ui()->DoEditBox(&s_BackgroundEntitiesInput, &EditBox, 14.0f);

	static CButtonContainer s_BackgroundEntitiesMapPicker, s_BackgroundEntitiesReload;

	if(Ui()->DoButton_FontIcon(&s_BackgroundEntitiesReload, FontIcon::ARROW_ROTATE_RIGHT, 0, &ReloadButton, BUTTONFLAG_LEFT))
	{
		GameClient()->m_Background.LoadBackground();
	}

	if(Ui()->DoButton_FontIcon(&s_BackgroundEntitiesMapPicker, FontIcon::FOLDER, 0, &Button, BUTTONFLAG_LEFT))
	{
		static SPopupMenuId s_PopupMapPickerId;
		static CPopupMapPickerContext s_PopupMapPickerContext;
		s_PopupMapPickerContext.m_pMenus = this;
		s_PopupMapPickerContext.MapListPopulate();
		Ui()->DoPopupMenu(&s_PopupMapPickerId, Ui()->MouseX(), Ui()->MouseY(), 300.0f, 250.0f, &s_PopupMapPickerContext, PopupMapPicker);
	}

	Background.HSplitTop(20.0f, &Button, &Background);
	const bool UseCurrentMap = str_comp(g_Config.m_ClBackgroundEntities, CURRENT_MAP) == 0;
	static int s_UseCurrentMapId = 0;
	if(DoButton_CheckBox(&s_UseCurrentMapId, Localize("Use current map as background"), UseCurrentMap, &Button))
	{
		if(UseCurrentMap)
			g_Config.m_ClBackgroundEntities[0] = '\0';
		else
			str_copy(g_Config.m_ClBackgroundEntities, CURRENT_MAP);
		GameClient()->m_Background.LoadBackground();
	}

	Background.HSplitTop(20.0f, &Button, &Background);
	if(DoButton_CheckBox(&g_Config.m_ClBackgroundShowTilesLayers, Localize("Show tiles layers from BG map"), g_Config.m_ClBackgroundShowTilesLayers, &Button))
		g_Config.m_ClBackgroundShowTilesLayers ^= 1;

	// miscellaneous
	Miscellaneous.HSplitTop(30.0f, &Label, &Miscellaneous);
	Miscellaneous.HSplitTop(5.0f, nullptr, &Miscellaneous);

	Ui()->DoLabel(&Label, Localize("Miscellaneous"), 20.0f, TEXTALIGN_ML);

	static CButtonContainer s_ButtonTimeout;
	Miscellaneous.HSplitTop(20.0f, &Button, &Miscellaneous);
	if(DoButton_Menu(&s_ButtonTimeout, Localize("New random timeout code"), 0, &Button))
	{
		Client()->GenerateTimeoutSeed();
	}

	Miscellaneous.HSplitTop(5.0f, nullptr, &Miscellaneous);
	Miscellaneous.HSplitTop(20.0f, &Label, &Miscellaneous);
	Miscellaneous.HSplitTop(2.0f, nullptr, &Miscellaneous);
	Ui()->DoLabel(&Label, Localize("Run on join"), 14.0f, TEXTALIGN_ML);
	Miscellaneous.HSplitTop(20.0f, &Button, &Miscellaneous);
	static CLineInput s_RunOnJoinInput(g_Config.m_ClRunOnJoin, sizeof(g_Config.m_ClRunOnJoin));
	s_RunOnJoinInput.SetEmptyText(Localize("Chat command (e.g. showall 1)"));
	Ui()->DoEditBox(&s_RunOnJoinInput, &Button, 14.0f);

#if defined(CONF_FAMILY_WINDOWS)
	static CButtonContainer s_ButtonUnregisterShell;
	Miscellaneous.HSplitTop(10.0f, nullptr, &Miscellaneous);
	Miscellaneous.HSplitTop(20.0f, &Button, &Miscellaneous);
	if(DoButton_Menu(&s_ButtonUnregisterShell, Localize("Unregister protocol and file extensions"), 0, &Button))
	{
		Client()->ShellUnregister();
	}
#endif

	// Updater
#if defined(CONF_AUTOUPDATE)
	{
		const bool NeedUpdate = GameClient()->m_TClient.NeedUpdate();
		IUpdater::EUpdaterState State = Updater()->GetCurrentState();

		// Update Button
		char aBuf[256];
		if(NeedUpdate && State <= IUpdater::CLEAN)
		{
			str_format(aBuf, sizeof(aBuf), Localize("TClient %s is available:"), GameClient()->m_TClient.m_aVersionStr);
			UpdaterRect.VSplitLeft(TextRender()->TextWidth(14.0f, aBuf, -1, -1.0f) + 10.0f, &UpdaterRect, &Button);
			Button.VSplitLeft(100.0f, &Button, nullptr);
			static CButtonContainer s_ButtonUpdate;
			if(DoButton_Menu(&s_ButtonUpdate, Localize("Update now"), 0, &Button))
			{
				Updater()->InitiateUpdate();
			}
		}
		else if(State >= IUpdater::GETTING_MANIFEST && State < IUpdater::NEED_RESTART)
			str_copy(aBuf, Localize("UpdatingвЂ¦"));
		else if(State == IUpdater::NEED_RESTART)
		{
			str_copy(aBuf, Localize("TClient Client updated!"));
			m_NeedRestartUpdate = true;
		}
		else
		{
			str_copy(aBuf, Localize("No updates available"));
			UpdaterRect.VSplitLeft(TextRender()->TextWidth(14.0f, aBuf, -1, -1.0f) + 10.0f, &UpdaterRect, &Button);
			Button.VSplitLeft(100.0f, &Button, nullptr);
			static CButtonContainer s_ButtonUpdate;
			if(DoButton_Menu(&s_ButtonUpdate, Localize("Check now"), 0, &Button))
			{
				GameClient()->m_TClient.FetchTClientInfo();
			}
		}
		Ui()->DoLabel(&UpdaterRect, aBuf, 14.0f, TEXTALIGN_ML);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	}
#endif
}

CUi::EPopupMenuFunctionResult CMenus::PopupMapPicker(void *pContext, CUIRect View, bool Active)
{
	CPopupMapPickerContext *pPopupContext = static_cast<CPopupMapPickerContext *>(pContext);
	CMenus *pMenus = pPopupContext->m_pMenus;

	static CListBox s_ListBox;
	s_ListBox.SetActive(Active);
	s_ListBox.DoStart(20.0f, pPopupContext->m_vMaps.size(), 1, 3, -1, &View, false);

	int MapIndex = 0;
	for(auto &Map : pPopupContext->m_vMaps)
	{
		MapIndex++;
		const CListboxItem Item = s_ListBox.DoNextItem(&Map, MapIndex == pPopupContext->m_Selection);
		if(!Item.m_Visible)
			continue;

		CUIRect Label, Icon;
		Item.m_Rect.VSplitLeft(20.0f, &Icon, &Label);

		char aLabelText[IO_MAX_PATH_LENGTH];
		str_copy(aLabelText, Map.m_aFilename);
		if(Map.m_IsDirectory)
			str_append(aLabelText, "/", sizeof(aLabelText));

		const char *pIconType;
		if(!Map.m_IsDirectory)
		{
			pIconType = FontIcon::MAP;
		}
		else
		{
			if(!str_comp(Map.m_aFilename, ".."))
				pIconType = FontIcon::FOLDER_TREE;
			else
				pIconType = FontIcon::FOLDER;
		}

		pMenus->TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		pMenus->TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING);
		pMenus->Ui()->DoLabel(&Icon, pIconType, 12.0f, TEXTALIGN_ML);
		pMenus->TextRender()->SetRenderFlags(0);
		pMenus->TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);

		pMenus->Ui()->DoLabel(&Label, aLabelText, 10.0f, TEXTALIGN_ML);
	}

	const int NewSelected = s_ListBox.DoEnd();
	pPopupContext->m_Selection = NewSelected >= 0 ? NewSelected : -1;
	if(s_ListBox.WasItemSelected() || s_ListBox.WasItemActivated())
	{
		const CMapListItem &SelectedItem = pPopupContext->m_vMaps[pPopupContext->m_Selection];

		if(SelectedItem.m_IsDirectory)
		{
			if(!str_comp(SelectedItem.m_aFilename, ".."))
			{
				fs_parent_dir(pPopupContext->m_aCurrentMapFolder);
			}
			else
			{
				str_append(pPopupContext->m_aCurrentMapFolder, "/", sizeof(pPopupContext->m_aCurrentMapFolder));
				str_append(pPopupContext->m_aCurrentMapFolder, SelectedItem.m_aFilename, sizeof(pPopupContext->m_aCurrentMapFolder));
			}
			pPopupContext->MapListPopulate();
		}
		else
		{
			str_format(g_Config.m_ClBackgroundEntities, sizeof(g_Config.m_ClBackgroundEntities), "%s/%s", pPopupContext->m_aCurrentMapFolder, SelectedItem.m_aFilename);
			pMenus->GameClient()->m_Background.LoadBackground();
			return CUi::POPUP_CLOSE_CURRENT;
		}
	}

	return CUi::POPUP_KEEP_OPEN;
}

void CMenus::CPopupMapPickerContext::MapListPopulate()
{
	m_vMaps.clear();
	char aTemp[IO_MAX_PATH_LENGTH];
	str_format(aTemp, sizeof(aTemp), "maps/%s", m_aCurrentMapFolder);
	m_pMenus->Storage()->ListDirectoryInfo(IStorage::TYPE_ALL, aTemp, MapListFetchCallback, this);
	std::stable_sort(m_vMaps.begin(), m_vMaps.end(), CompareFilenameAscending);
	m_Selection = -1;
}

int CMenus::CPopupMapPickerContext::MapListFetchCallback(const CFsFileInfo *pInfo, int IsDir, int StorageType, void *pUser)
{
	CPopupMapPickerContext *pRealUser = (CPopupMapPickerContext *)pUser;
	if((!IsDir && !str_endswith(pInfo->m_pName, ".map")) || !str_comp(pInfo->m_pName, ".") || (!str_comp(pInfo->m_pName, "..") && (!str_comp(pRealUser->m_aCurrentMapFolder, ""))))
		return 0;

	CMapListItem Item;
	str_copy(Item.m_aFilename, pInfo->m_pName);
	Item.m_IsDirectory = IsDir;

	pRealUser->m_vMaps.emplace_back(Item);

	return 0;
}


