#include "menus_start_rclient.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <string>

#include <base/math.h>
#include <base/system.h>

#include <engine/font_icons.h>
#include <engine/graphics.h>
#include <engine/image.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>
#include <game/client/ui.h>
#include <game/localization.h>
#include <game/version.h>

namespace
{
	ColorRGBA MixColors(const ColorRGBA &A, const ColorRGBA &B, float T)
	{
		const float K = std::clamp(T, 0.0f, 1.0f);
		return ColorRGBA(
			A.r + (B.r - A.r) * K,
			A.g + (B.g - A.g) * K,
			A.b + (B.b - A.b) * K,
			A.a + (B.a - A.a) * K);
	}

	float SmoothTowards(float Current, float Target, float Dt, float Speed)
	{
		const float K = std::clamp(Dt * Speed, 0.0f, 1.0f);
		return Current + (Target - Current) * K;
	}

	std::string CutText(const std::string &Text, int MaxChars)
	{
		if((int)Text.size() <= MaxChars)
			return Text;
		std::string Out = Text.substr(0, MaxChars);
		Out.append("...");
		return Out;
	}
}

void CMenusStartRClient::RenderStartMenu(CUIRect MainView)
{
	(void)MainView;
	GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_START);

	const CUIRect View = *Ui()->Screen();
	const auto Ease = [](float Progress) {
		return Progress * Progress * (3.0f - 2.0f * Progress);
	};
	const auto Lerp = [](float A, float B, float T) {
		return A + (B - A) * T;
	};

	// Keep themed menu background visible; add only a soft dark overlay.
	View.Draw(ColorRGBA(0.03f, 0.03f, 0.07f, 0.20f), IGraphics::CORNER_NONE, 0.0f);
	Graphics()->TextureClear();

	m_LockAnim = std::clamp(m_LockAnim + Client()->RenderFrameTime() * (m_LockActive ? 6.0f : -6.0f), 0.0f, 1.0f);
	const float LockProgress = Ease(m_LockAnim);
	const float LockAlpha = 1.0f - LockProgress;
	const float GuiAlpha = LockProgress;

	time_t Now = ::time(nullptr);
	tm LocalTm{};
#if defined(CONF_FAMILY_WINDOWS)
	localtime_s(&LocalTm, &Now);
#else
	LocalTm = *localtime(&Now);
#endif

	static const char *s_apWeekdays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
	static const char *s_apMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
	const int WDay = std::clamp(LocalTm.tm_wday, 0, 6);
	const int Mon = std::clamp(LocalTm.tm_mon, 0, 11);

	char aDate[128];
	char aTime[16];
	str_format(aDate, sizeof(aDate), "%s, %d %s", s_apWeekdays[WDay], LocalTm.tm_mday, s_apMonths[Mon]);
	str_format(aTime, sizeof(aTime), "%02d:%02d", LocalTm.tm_hour, LocalTm.tm_min);

	// Keep time/date safely above menu buttons in unlocked state.
	const float UnlockedTimeY = View.y + View.h * 0.31f;
	const float TimeOffsetY = Lerp(View.y + View.h * 0.29f, UnlockedTimeY, LockProgress);
	const float DateSize = Lerp(24.0f, 30.0f, LockProgress);
	const float TimeSize = Lerp(70.0f, 86.0f, LockProgress);
	const float DateGap = Lerp(40.0f, 50.0f, LockProgress);
	CUIRect DateRect = {View.x, TimeOffsetY - DateGap, View.w, DateSize + 2.0f};
	CUIRect TimeRect = {View.x, TimeOffsetY, View.w, TimeSize + 4.0f};

	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
	Ui()->DoLabel(&DateRect, aDate, DateSize, TEXTALIGN_MC);
	Ui()->DoLabel(&TimeRect, aTime, TimeSize, TEXTALIGN_MC);
	TextRender()->TextColor(TextRender()->DefaultTextColor());

	if(LockAlpha > 0.01f)
	{
		CUIRect HintRect = {View.x, View.y + View.h - 28.0f, View.w, 16.0f};
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.65f * LockAlpha);
		Ui()->DoLabel(&HintRect, "Click to unlock", 14.0f, TEXTALIGN_MC);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}

	if(LockProgress > 0.0f)
	{
		CUIRect Bar = {View.x + View.w / 2.0f - 36.0f, View.y + View.h - 8.0f - 3.0f * LockProgress, 72.0f, 3.0f};
		Bar.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f * LockProgress), IGraphics::CORNER_ALL, 1.5f);
	}

	static CButtonContainer s_FullscreenUnlock;
	if(!m_LockActive && Ui()->DoButtonLogic(&s_FullscreenUnlock, 0, &View, BUTTONFLAG_LEFT))
		m_LockActive = true;

	int NewPage = -1;
	bool WantQuit = false;

	if(GuiAlpha > 0.01f)
	{
		CUIRect Menu;
		const float VMargin = View.w / 2.0f - 190.0f;
		View.VMargin(VMargin, &Menu);
		Menu.HSplitBottom(130.0f, &Menu, nullptr);

		const float Rounding = 10.0f;
		const float ButtonH = 40.0f;
		const float Spacing = 6.0f;
		ColorRGBA ButtonColor(0.0f, 0.0f, 0.0f, (0.25f + 0.10f * GuiAlpha) * GuiAlpha);
		ColorRGBA QuitColor(0.15f, 0.02f, 0.02f, (0.20f + 0.10f * GuiAlpha) * GuiAlpha);

		CUIRect Button;
		Menu.HSplitBottom(ButtonH, &Menu, &Button);
		static CButtonContainer s_QuitButton;
		if(GameClient()->m_Menus.DoButton_Menu(&s_QuitButton, Localize("Quit"), 0, &Button, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, Rounding, 0.5f, QuitColor) || (GuiAlpha > 0.95f && CheckHotKey(KEY_Q)))
			WantQuit = true;

		Menu.HSplitBottom(Spacing, &Menu, nullptr);
		Menu.HSplitBottom(ButtonH, &Menu, &Button);
		static CButtonContainer s_SettingsButton;
		if(GameClient()->m_Menus.DoButton_Menu(&s_SettingsButton, Localize("Settings"), 0, &Button, BUTTONFLAG_LEFT, g_Config.m_ClShowStartMenuImages ? "settings" : nullptr, IGraphics::CORNER_ALL, Rounding, 0.5f, ButtonColor) || (GuiAlpha > 0.95f && CheckHotKey(KEY_S)))
			NewPage = CMenus::PAGE_SETTINGS;

		Menu.HSplitBottom(Spacing, &Menu, nullptr);
		Menu.HSplitBottom(ButtonH, &Menu, &Button);
		static CButtonContainer s_DemoButton;
		if(GameClient()->m_Menus.DoButton_Menu(&s_DemoButton, Localize("Demos"), 0, &Button, BUTTONFLAG_LEFT, g_Config.m_ClShowStartMenuImages ? "demos" : nullptr, IGraphics::CORNER_ALL, Rounding, 0.5f, ButtonColor) || (GuiAlpha > 0.95f && CheckHotKey(KEY_D)))
			NewPage = CMenus::PAGE_DEMOS;

		Menu.HSplitBottom(Spacing, &Menu, nullptr);
		Menu.HSplitBottom(ButtonH, &Menu, &Button);
		static CButtonContainer s_PlayButton;
		if(GameClient()->m_Menus.DoButton_Menu(&s_PlayButton, Localize("Play", "Start menu"), 0, &Button, BUTTONFLAG_LEFT, g_Config.m_ClShowStartMenuImages ? "play_game" : nullptr, IGraphics::CORNER_ALL, Rounding, 0.5f, ButtonColor) || (GuiAlpha > 0.95f && (Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER) || CheckHotKey(KEY_P))))
			NewPage = g_Config.m_UiPage >= CMenus::PAGE_INTERNET && g_Config.m_UiPage <= CMenus::PAGE_FAVORITE_COMMUNITY_5 ? g_Config.m_UiPage : CMenus::PAGE_INTERNET;
	}

	if(GuiAlpha > 0.01f)
	{
		char aWatermark[128];
		str_format(aWatermark, sizeof(aWatermark), "ZZClient Free %s", UI_CLIENT_RELEASE_VERSION);
		const float FontSize = 8.2f;
		const float TextW = TextRender()->TextWidth(FontSize, aWatermark, -1, -1.0f);
		const float BoxW = TextW + 14.0f;
		const float BoxH = 16.0f;
		const float X = View.x + View.w - BoxW - 10.0f;
		const float Y = View.y + 10.0f;
		const float Alpha = std::clamp(GuiAlpha, 0.0f, 1.0f);
		Graphics()->DrawRect(X, Y, BoxW, BoxH, ColorRGBA(0.0f, 0.0f, 0.0f, 0.36f * Alpha), IGraphics::CORNER_ALL, 5.0f);
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.95f * Alpha);
		TextRender()->Text(X + 7.0f, Y + (BoxH - FontSize) * 0.5f - 0.1f, FontSize, aWatermark, -1.0f);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}

	const float PlayerAlpha = std::clamp((GuiAlpha - 0.30f) / 0.70f, 0.0f, 1.0f);
	if(PlayerAlpha > 0.01f)
	{
		const auto Track = GameClient()->m_MusicSMTC.GetTrackInfo();
		const bool Playing = Track.m_Available && Track.m_Playing && (!Track.m_Title.empty() || !Track.m_Artist.empty());
		if(Playing)
		{
			static IGraphics::CTextureHandle s_MenuArtworkTexture;
			static uint64_t s_MenuArtworkRevision = 0;
			static std::array<float, CMusicSMTC::STrackInfo::VISUALIZER_BARS> s_aBars{};
			static float s_PlayerExpand = 0.0f;
			static CButtonContainer s_PrevButton;
			static CButtonContainer s_NextButton;

			if(Track.m_ArtworkRevision != s_MenuArtworkRevision)
			{
				if(s_MenuArtworkTexture.IsValid())
					Graphics()->UnloadTexture(&s_MenuArtworkTexture);
				s_MenuArtworkRevision = Track.m_ArtworkRevision;
				if(Track.m_HasArtwork)
				{
					CImageInfo Image;
					Image.m_Width = CMusicSMTC::STrackInfo::ARTWORK_DIM;
					Image.m_Height = CMusicSMTC::STrackInfo::ARTWORK_DIM;
					Image.m_Format = CImageInfo::FORMAT_RGBA;
					Image.m_pData = static_cast<uint8_t *>(malloc(Track.m_aArtworkRgba.size()));
					if(Image.m_pData != nullptr)
					{
						mem_copy(Image.m_pData, Track.m_aArtworkRgba.data(), Track.m_aArtworkRgba.size());
						s_MenuArtworkTexture = Graphics()->LoadTextureRawMove(Image, 0, "zz_menu_smtc_art");
					}
				}
			}

			const std::string Title = CutText(Track.m_Title, 30);
			const std::string Artist = CutText(Track.m_Artist, 34);
			const float TopSize = 11.0f;
			const float BottomSize = 9.0f;
			const float TopW = TextRender()->TextWidth(TopSize, Title.c_str(), -1, -1.0f);
			const float BottomW = TextRender()->TextWidth(BottomSize, Artist.c_str(), -1, -1.0f);
			const float TextW = maximum(TopW, BottomW);
			const float PaddingX = 8.0f;
			const float ArtSize = 24.0f;
			const float Spacing = 5.0f;
			const float BarW = 2.4f;
			const float BarGap = 2.0f;
			const float BarsW = CMusicSMTC::STrackInfo::VISUALIZER_BARS * BarW + (CMusicSMTC::STrackInfo::VISUALIZER_BARS - 1) * BarGap;
			const float BtnSize = 13.5f;
			const float BtnGap = 4.0f;
			const float HoverExtraW = BtnSize * 2.0f + BtnGap + 10.0f;
			const float BaseW = PaddingX + ArtSize + Spacing + TextW + Spacing + BarsW + PaddingX;
			const float BoxH = 39.0f;
			const float Dt = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.1f);
			const float Y = View.y + 16.0f;
			const float HitW = BaseW + HoverExtraW;
			CUIRect HitRect = {View.x + View.w * 0.5f - HitW * 0.5f, Y, HitW, BoxH};
			const bool Hovered = Ui()->MouseHovered(&HitRect) || Ui()->CheckActiveItem(&s_PrevButton) || Ui()->CheckActiveItem(&s_NextButton);
			s_PlayerExpand = SmoothTowards(s_PlayerExpand, Hovered ? 1.0f : 0.0f, Dt, 9.0f);
			const float BoxW = BaseW + HoverExtraW * s_PlayerExpand;
			const float X = View.x + View.w * 0.5f - BoxW * 0.5f;

			ColorRGBA Accent(std::clamp(Track.m_AccentR, 0.0f, 1.0f), std::clamp(Track.m_AccentG, 0.0f, 1.0f), std::clamp(Track.m_AccentB, 0.0f, 1.0f), 1.0f);
			const ColorRGBA Accent2 = MixColors(Accent, ColorRGBA(0.11f, 0.15f, 0.22f, 1.0f), 0.35f);
			const float Alpha = std::clamp(PlayerAlpha, 0.0f, 1.0f);

			Graphics()->DrawRect4(
				X, Y, BoxW, BoxH,
				MixColors(Accent, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), 0.22f).WithAlpha(0.86f * Alpha),
				MixColors(Accent2, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), 0.18f).WithAlpha(0.86f * Alpha),
				MixColors(Accent, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), 0.22f).WithAlpha(0.86f * Alpha),
				MixColors(Accent2, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), 0.18f).WithAlpha(0.86f * Alpha),
				IGraphics::CORNER_ALL, BoxH * 0.40f);
			Graphics()->DrawRect(X + 0.8f, Y + 0.8f, BoxW - 1.6f, BoxH - 1.6f, ColorRGBA(0.06f, 0.06f, 0.08f, 0.70f * Alpha), IGraphics::CORNER_ALL, maximum(0.0f, BoxH * 0.38f - 0.4f));

			const float ArtX = X + PaddingX;
			const float ArtY = Y + (BoxH - ArtSize) * 0.5f;
			Graphics()->DrawRect(ArtX, ArtY, ArtSize, ArtSize, ColorRGBA(0.12f, 0.13f, 0.17f, 0.80f * Alpha), IGraphics::CORNER_ALL, 3.0f);
			if(Track.m_HasArtwork && s_MenuArtworkTexture.IsValid())
			{
				Graphics()->WrapClamp();
				Graphics()->TextureSet(s_MenuArtworkTexture);
				Graphics()->QuadsBegin();
				Graphics()->SetColor(1.0f, 1.0f, 1.0f, Alpha);
				const float Inset = 0.45f;
				IGraphics::CQuadItem Quad(ArtX + Inset, ArtY + Inset, ArtSize - Inset * 2.0f, ArtSize - Inset * 2.0f);
				Graphics()->QuadsDrawTL(&Quad, 1);
				Graphics()->QuadsEnd();
				Graphics()->WrapNormal();
			}
			else
			{
				TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
				TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.95f * Alpha);
				TextRender()->Text(ArtX + 3.8f, ArtY + 3.0f, 7.0f, FontIcon::MUSIC, -1.0f);
				TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
			}

			const float TextX = ArtX + ArtSize + Spacing + 0.3f;
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.98f * Alpha);
			TextRender()->Text(TextX, Y + 7.8f, TopSize, Title.c_str(), -1.0f);
			TextRender()->TextColor(0.85f, 0.87f, 0.92f, 0.93f * Alpha);
			TextRender()->Text(TextX, Y + 22.0f, BottomSize, Artist.c_str(), -1.0f);
			TextRender()->TextColor(TextRender()->DefaultTextColor());

			const float ControlsW = HoverExtraW * s_PlayerExpand;
			const float BarsX = X + BoxW - PaddingX - BarsW - ControlsW;
			const float BarsBaseY = Y + BoxH - 6.2f;
			const float MaxH = 18.0f;
			for(int i = 0; i < CMusicSMTC::STrackInfo::VISUALIZER_BARS; i++)
			{
				const float Level = Track.m_HasVisualizer ? std::clamp(Track.m_aVisualizer[i], 0.0f, 1.0f) : 0.0f;
				s_aBars[i] = SmoothTowards(s_aBars[i], Level, Dt, 20.0f);
				const float H = 2.4f + s_aBars[i] * (MaxH - 2.4f);
				const float BX = BarsX + i * (BarW + BarGap);
				const float BY = BarsBaseY - H;
				const ColorRGBA BarColor = MixColors(Accent, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f), 0.34f + i * 0.08f).WithAlpha(0.95f * Alpha);
				Graphics()->DrawRect(BX, BY, BarW, H, BarColor, IGraphics::CORNER_T, 0.8f);
			}

			if(s_PlayerExpand > 0.02f)
			{
				const float BtnY = Y + (BoxH - BtnSize) * 0.5f;
				const float ControlsRight = X + BoxW - PaddingX;
				const float NextX = ControlsRight - BtnSize;
				const float PrevX = NextX - BtnGap - BtnSize;
				CUIRect PrevRect = {PrevX, BtnY, BtnSize, BtnSize};
				CUIRect NextRect = {NextX, BtnY, BtnSize, BtnSize};
				const float BtnAlpha = Alpha * s_PlayerExpand;
				Graphics()->DrawRect(PrevRect.x, PrevRect.y, PrevRect.w, PrevRect.h, ColorRGBA(0.12f, 0.14f, 0.18f, 0.78f * BtnAlpha), IGraphics::CORNER_ALL, 2.4f);
				Graphics()->DrawRect(NextRect.x, NextRect.y, NextRect.w, NextRect.h, ColorRGBA(0.12f, 0.14f, 0.18f, 0.78f * BtnAlpha), IGraphics::CORNER_ALL, 2.4f);

				if(Ui()->DoButtonLogic(&s_PrevButton, 0, &PrevRect, BUTTONFLAG_LEFT))
					GameClient()->m_MusicSMTC.Previous();
				if(Ui()->DoButtonLogic(&s_NextButton, 0, &NextRect, BUTTONFLAG_LEFT))
					GameClient()->m_MusicSMTC.Next();

				TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
				TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.96f * BtnAlpha);
				Ui()->DoLabel(&PrevRect, FontIcon::BACKWARD_STEP, 8.2f, TEXTALIGN_MC);
				Ui()->DoLabel(&NextRect, FontIcon::FORWARD_STEP, 8.2f, TEXTALIGN_MC);
				TextRender()->TextColor(TextRender()->DefaultTextColor());
				TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
			}
		}
	}

	if(WantQuit)
	{
		if(GameClient()->Editor()->HasUnsavedData() || (GameClient()->CurrentRaceTime() / 60 >= g_Config.m_ClConfirmQuitTime && g_Config.m_ClConfirmQuitTime >= 0))
			GameClient()->m_Menus.ShowQuitPopup();
		else
			Client()->Quit();
	}

	if(NewPage != -1)
	{
		GameClient()->m_Menus.SetShowStart(false);
		GameClient()->m_Menus.SetMenuPage(NewPage);
	}
}

bool CMenusStartRClient::CheckHotKey(int Key) const
{
	return !Input()->ShiftIsPressed() && !Input()->ModifierIsPressed() && !Input()->AltIsPressed() &&
	       Input()->KeyPress(Key) &&
	       !GameClient()->m_GameConsole.IsActive();
}
