/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "menus.h"

#include <base/color.h>
#include <base/log.h>
#include <base/math.h>
#include <base/str.h>
#include <base/system.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/client/updater.h>
#include <engine/config.h>
#include <engine/editor.h>
#include <engine/font_icons.h>
#include <engine/friends.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/graphics.h>
#include <engine/keys.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/shared/http.h>
#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>
#include <engine/shared/uuid_manager.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <generated/client_data.h>
#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/components/binds.h>
#include <game/client/components/console.h>
#include <game/client/components/key_binder.h>
#include <game/client/components/menu_background.h>
#include <game/client/components/sounds.h>
#include <game/client/gameclient.h>
#include <game/client/ui_listbox.h>
#include <game/localization.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace
{
constexpr const char *CLIENT_CHAT_HEADER_NAME = "X-Dobri-Client";
constexpr const char *CLIENT_CHAT_HEADER_VALUE = "dobri-client-chat-6c4f7f14b822";
constexpr const char *CLIENT_CHAT_AUTH_HEADER_NAME = "X-Dobri-Auth";
constexpr const char *CLIENT_CHAT_CONTACTS_FOLDER = "dobri_client_chat";
constexpr int CLIENT_CHAT_ROOM_LIMIT = 64;
constexpr int CLIENT_CHAT_DIRECT_LIMIT = 96;
constexpr int CLIENT_CHAT_MAX_VISIBLE_ITEMS = 12;
constexpr auto CLIENT_CHAT_ACTIVE_POLL_INTERVAL = 2s;
constexpr auto CLIENT_CHAT_BACKGROUND_POLL_INTERVAL = 5s;
constexpr auto CLIENT_CHAT_TOAST_DURATION = 6s;
constexpr CTimeout CLIENT_CHAT_TIMEOUT{4000, 15000, 500, 5};

ColorRGBA ClientChatAccentColor(bool UseCustomColor, int ColorValue, float Alpha = 0.35f)
{
	if(!UseCustomColor)
		return ColorRGBA(0.65f, 0.65f, 0.65f, Alpha * 0.6f);

	ColorRGBA Result = color_cast<ColorRGBA>(ColorHSLA((unsigned)ColorValue, true));
	Result.a = Alpha;
	return Result;
}

bool ClientChatContactsFilePath(char *pBuffer, size_t BufferSize, const char *pAccountId)
{
	if(pBuffer == nullptr || BufferSize == 0 || pAccountId == nullptr || pAccountId[0] == '\0')
		return false;

	str_format(pBuffer, BufferSize, "%s/direct_contacts_%s.json", CLIENT_CHAT_CONTACTS_FOLDER, pAccountId);
	return true;
}

bool ClientChatIsAccountId(const char *pUserId)
{
	return pUserId != nullptr && str_startswith(pUserId, "acct_") != nullptr;
}

bool ClientChatIdsEqual(const char *pLeft, const char *pRight);
bool ClientChatNamesEqual(const char *pLeft, const char *pRight);

bool ClientChatTextContainsMention(const char *pText, const char *pName)
{
	if(pText == nullptr || pText[0] == '\0' || pName == nullptr || pName[0] == '\0')
		return false;

	char aNeedle[128];
	str_format(aNeedle, sizeof(aNeedle), "@%s", pName);
	return str_utf8_find_nocase(pText, aNeedle) != nullptr;
}

void ClientChatFormatToastLine(char *pDst, int DstSize, const char *pText, int MaxChars)
{
	if(pDst == nullptr || DstSize <= 0)
		return;

	char aRaw[256];
	if(pText != nullptr && pText[0] != '\0')
		str_copy(aRaw, pText, sizeof(aRaw));
	else
		aRaw[0] = '\0';
	char aCompact[256];
	int OutIndex = 0;
	bool LastWasSpace = false;
	for(int Index = 0; aRaw[Index] != '\0' && OutIndex < (int)sizeof(aCompact) - 1; ++Index)
	{
		char c = aRaw[Index];
		if(c == '\r' || c == '\n' || c == '\t')
			c = ' ';
		if(c == ' ')
		{
			if(LastWasSpace)
				continue;
			LastWasSpace = true;
		}
		else
			LastWasSpace = false;
		aCompact[OutIndex++] = c;
	}
	aCompact[OutIndex] = '\0';
	str_utf8_trim_right(aCompact);

	if(MaxChars > 3 && str_length(aCompact) > MaxChars)
	{
		str_truncate(pDst, DstSize, aCompact, MaxChars - 3);
		str_append(pDst, "...", DstSize);
	}
	else
		str_copy(pDst, aCompact, DstSize);
}

bool ClientChatIsLocalMessage(const char *pSenderId, const char *pSenderInstanceId, const char *pSenderName, const char *pLocalId, const char *pLocalName, const char *pLocalInstanceId, bool HasAccountIdentity)
{
	if(ClientChatIdsEqual(pSenderId, pLocalId))
	{
		if(HasAccountIdentity && pLocalInstanceId != nullptr && pLocalInstanceId[0] != '\0' && pSenderInstanceId != nullptr && pSenderInstanceId[0] != '\0')
			return str_comp(pSenderInstanceId, pLocalInstanceId) == 0;
		return true;
	}
	if(pSenderId != nullptr && pSenderId[0] != '\0')
		return false;
	return ClientChatNamesEqual(pSenderName, pLocalName);
}

bool ClientChatIdsEqual(const char *pLeft, const char *pRight)
{
	return pLeft != nullptr && pRight != nullptr && pLeft[0] != '\0' && pRight[0] != '\0' && str_comp(pLeft, pRight) == 0;
}

bool ClientChatNamesEqual(const char *pLeft, const char *pRight)
{
	return pLeft != nullptr && pRight != nullptr && pLeft[0] != '\0' && pRight[0] != '\0' && str_utf8_comp_nocase(pLeft, pRight) == 0;
}

void ClientChatCopyOptional(char *pDst, int DstSize, const char *pSrc, const char *pFallback = "")
{
	if(pSrc != nullptr && pSrc[0] != '\0')
		str_copy(pDst, pSrc, DstSize);
	else
		str_copy(pDst, pFallback, DstSize);
}

void ClientChatTrimPreview(char *pDst, int DstSize, const char *pText)
{
	if(DstSize <= 0)
		return;
	ClientChatCopyOptional(pDst, DstSize, pText);
	if(str_length(pDst) > 72 && DstSize > 4)
	{
		pDst[DstSize - 4] = '.';
		pDst[DstSize - 3] = '.';
		pDst[DstSize - 2] = '.';
		pDst[DstSize - 1] = '\0';
	}
}

const char *ClientChatStatusIconFor(const char *pStatus, bool Online)
{
	if(!Online)
		return FontIcon::CIRCLE;
	if(str_comp_nocase(pStatus, "dnd") == 0)
		return FontIcon::BAN;
	if(str_comp_nocase(pStatus, "idle") == 0)
		return FontIcon::PAUSE;
	if(str_comp_nocase(pStatus, "invisible") == 0)
		return FontIcon::EYE_SLASH;
	return FontIcon::CIRCLE;
}

ColorRGBA ClientChatStatusColorFor(const char *pStatus, bool Online)
{
	if(!Online)
		return ColorRGBA(0.58f, 0.62f, 0.70f, 0.90f);
	if(str_comp_nocase(pStatus, "dnd") == 0)
		return ColorRGBA(0.86f, 0.30f, 0.34f, 0.95f);
	if(str_comp_nocase(pStatus, "idle") == 0)
		return ColorRGBA(0.93f, 0.72f, 0.22f, 0.95f);
	if(str_comp_nocase(pStatus, "invisible") == 0)
		return ColorRGBA(0.64f, 0.66f, 0.72f, 0.95f);
	return ColorRGBA(0.35f, 0.83f, 0.50f, 0.95f);
}
} // namespace

ColorRGBA CMenus::ms_GuiColor;
ColorRGBA CMenus::ms_ColorTabbarInactiveOutgame;
ColorRGBA CMenus::ms_ColorTabbarActiveOutgame;
ColorRGBA CMenus::ms_ColorTabbarHoverOutgame;
ColorRGBA CMenus::ms_ColorTabbarInactive;
ColorRGBA CMenus::ms_ColorTabbarActive = ColorRGBA(0, 0, 0, 0.5f);
ColorRGBA CMenus::ms_ColorTabbarHover;
ColorRGBA CMenus::ms_ColorTabbarInactiveIngame;
ColorRGBA CMenus::ms_ColorTabbarActiveIngame;
ColorRGBA CMenus::ms_ColorTabbarHoverIngame;

float CMenus::ms_ButtonHeight = 25.0f;
float CMenus::ms_ListheaderHeight = 17.0f;

CMenus::CMenus()
{
	m_Popup = POPUP_NONE;
	m_MenuPage = 0;
	m_GamePage = PAGE_GAME;

	m_NeedRestartGraphics = false;
	m_NeedRestartSound = false;
	m_NeedSendinfo = false;
	m_NeedSendDummyinfo = false;
	m_MenuActive = true;
	m_ShowStart = true;

	str_copy(m_aCurrentDemoFolder, "demos");
	m_DemolistStorageType = IStorage::TYPE_ALL;

	m_DemoPlayerState = DEMOPLAYER_NONE;
	m_Dummy = false;

	for(SUIAnimator &Animator : m_aAnimatorsSettingsTab)
	{
		Animator.m_YOffset = -2.5f;
		Animator.m_HOffset = 5.0f;
		Animator.m_WOffset = 5.0f;
		Animator.m_RepositionLabel = true;
	}

	for(SUIAnimator &Animator : m_aAnimatorsBigPage)
	{
		Animator.m_YOffset = -5.0f;
		Animator.m_HOffset = 5.0f;
	}

	for(SUIAnimator &Animator : m_aAnimatorsSmallPage)
	{
		Animator.m_YOffset = -2.5f;
		Animator.m_HOffset = 2.5f;
	}

	m_PasswordInput.SetBuffer(g_Config.m_Password, sizeof(g_Config.m_Password));
	m_PasswordInput.SetHidden(true);
	m_ClientChatInput.SetEmptyText("Write a message...");
	m_ClientChatAuthNameInput.SetEmptyText("Username");
	m_ClientChatAuthPasswordInput.SetEmptyText("Password");
	m_ClientChatAuthPasswordInput.SetHidden(true);
}

int CMenus::DoButton_Toggle(const void *pId, int Checked, const CUIRect *pRect, bool Active, const unsigned Flags)
{
	Graphics()->TextureSet(g_pData->m_aImages[IMAGE_GUIBUTTONS].m_Id);
	Graphics()->QuadsBegin();
	if(!Active)
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.5f);
	Graphics()->SelectSprite(Checked ? SPRITE_GUIBUTTON_ON : SPRITE_GUIBUTTON_OFF);
	IGraphics::CQuadItem QuadItem(pRect->x, pRect->y, pRect->w, pRect->h);
	Graphics()->QuadsDrawTL(&QuadItem, 1);
	if(Ui()->HotItem() == pId && Active)
	{
		Graphics()->SelectSprite(SPRITE_GUIBUTTON_HOVER);
		QuadItem = IGraphics::CQuadItem(pRect->x, pRect->y, pRect->w, pRect->h);
		Graphics()->QuadsDrawTL(&QuadItem, 1);
	}
	Graphics()->QuadsEnd();

	return Active ? Ui()->DoButtonLogic(pId, Checked, pRect, Flags) : 0;
}

int CMenus::DoButton_Menu(CButtonContainer *pButtonContainer, const char *pText, int Checked, const CUIRect *pRect, const unsigned Flags, const char *pImageName, int Corners, float Rounding, float FontFactor, ColorRGBA Color)
{
	CUIRect Text = *pRect;

	if(Checked)
		Color = ColorRGBA(0.6f, 0.6f, 0.6f, 0.5f);
	else // TClient, why was this not here? ig they never use "checked" anywhere important
		Color.a *= Ui()->ButtonColorMul(pButtonContainer);

	pRect->Draw(Color, Corners, Rounding);

	if(pImageName)
	{
		CUIRect Image;
		pRect->VSplitRight(pRect->h * 4.0f, &Text, &Image); // always correct ratio for image

		// render image
		const CMenuImage *pImage = FindMenuImage(pImageName);
		if(pImage)
		{
			Graphics()->TextureSet(Ui()->HotItem() == pButtonContainer ? pImage->m_OrgTexture : pImage->m_GreyTexture);
			Graphics()->WrapClamp();
			Graphics()->QuadsBegin();
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
			IGraphics::CQuadItem QuadItem(Image.x, Image.y, Image.w, Image.h);
			Graphics()->QuadsDrawTL(&QuadItem, 1);
			Graphics()->QuadsEnd();
			Graphics()->WrapNormal();
		}
	}

	Text.HMargin(pRect->h >= 20.0f ? 2.0f : 1.0f, &Text);
	Text.HMargin((Text.h * FontFactor) / 2.0f, &Text);
	Ui()->DoLabel(&Text, pText, Text.h * CUi::ms_FontmodHeight, TEXTALIGN_MC);

	return Ui()->DoButtonLogic(pButtonContainer, Checked, pRect, Flags);
}

int CMenus::DoButton_MenuTab(CButtonContainer *pButtonContainer, const char *pText, int Checked, const CUIRect *pRect, int Corners, SUIAnimator *pAnimator, const ColorRGBA *pDefaultColor, const ColorRGBA *pActiveColor, const ColorRGBA *pHoverColor, float EdgeRounding, const CCommunityIcon *pCommunityIcon)
{
	const bool MouseInside = Ui()->HotItem() == pButtonContainer;
	CUIRect Rect = *pRect;

	if(pAnimator != nullptr)
	{
		auto Time = time_get_nanoseconds();

		if(pAnimator->m_Time + 100ms < Time)
		{
			pAnimator->m_Value = pAnimator->m_Active ? 1 : 0;
			pAnimator->m_Time = Time;
		}

		pAnimator->m_Active = Checked || MouseInside;

		if(pAnimator->m_Active)
			pAnimator->m_Value = std::clamp<float>(pAnimator->m_Value + (Time - pAnimator->m_Time).count() / (double)std::chrono::nanoseconds(100ms).count(), 0, 1);
		else
			pAnimator->m_Value = std::clamp<float>(pAnimator->m_Value - (Time - pAnimator->m_Time).count() / (double)std::chrono::nanoseconds(100ms).count(), 0, 1);

		Rect.w += pAnimator->m_Value * pAnimator->m_WOffset;
		Rect.h += pAnimator->m_Value * pAnimator->m_HOffset;
		Rect.x += pAnimator->m_Value * pAnimator->m_XOffset;
		Rect.y += pAnimator->m_Value * pAnimator->m_YOffset;

		pAnimator->m_Time = Time;
	}

	if(Checked)
	{
		ColorRGBA ColorMenuTab = ms_ColorTabbarActive;
		if(pActiveColor)
			ColorMenuTab = *pActiveColor;

		Rect.Draw(ColorMenuTab, Corners, EdgeRounding);
	}
	else
	{
		if(MouseInside)
		{
			ColorRGBA HoverColorMenuTab = ms_ColorTabbarHover;
			if(pHoverColor)
				HoverColorMenuTab = *pHoverColor;

			Rect.Draw(HoverColorMenuTab, Corners, EdgeRounding);
		}
		else
		{
			ColorRGBA ColorMenuTab = ms_ColorTabbarInactive;
			if(pDefaultColor)
				ColorMenuTab = *pDefaultColor;

			Rect.Draw(ColorMenuTab, Corners, EdgeRounding);
		}
	}

	if(pAnimator != nullptr)
	{
		if(pAnimator->m_RepositionLabel)
		{
			Rect.x += Rect.w - pRect->w + Rect.x - pRect->x;
			Rect.y += Rect.h - pRect->h + Rect.y - pRect->y;
		}

		if(!pAnimator->m_ScaleLabel)
		{
			Rect.w = pRect->w;
			Rect.h = pRect->h;
		}
	}

	if(pCommunityIcon)
	{
		CUIRect CommunityIcon;
		Rect.Margin(2.0f, &CommunityIcon);
		m_CommunityIcons.Render(pCommunityIcon, CommunityIcon, true);
	}
	else
	{
		CUIRect Label;
		Rect.HMargin(2.0f, &Label);
		Ui()->DoLabel(&Label, pText, Label.h * CUi::ms_FontmodHeight, TEXTALIGN_MC);
	}

	return Ui()->DoButtonLogic(pButtonContainer, Checked, pRect, BUTTONFLAG_LEFT);
}

int CMenus::DoButton_GridHeader(const void *pId, const char *pText, int Checked, const CUIRect *pRect, int Align)
{
	if(Checked == 2)
		pRect->Draw(ColorRGBA(1, 0.98f, 0.5f, 0.55f), IGraphics::CORNER_T, 5.0f);
	else if(Checked)
		pRect->Draw(ColorRGBA(1, 1, 1, 0.5f), IGraphics::CORNER_T, 5.0f);

	CUIRect Temp;
	pRect->VMargin(5.0f, &Temp);
	Ui()->DoLabel(&Temp, pText, pRect->h * CUi::ms_FontmodHeight, Align);
	return Ui()->DoButtonLogic(pId, Checked, pRect, BUTTONFLAG_LEFT);
}

int CMenus::DoButton_Favorite(const void *pButtonId, const void *pParentId, bool Checked, const CUIRect *pRect)
{
	if(Checked || (pParentId != nullptr && Ui()->HotItem() == pParentId) || Ui()->HotItem() == pButtonId)
	{
		TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT | ETextRenderFlags::TEXT_RENDER_FLAG_NO_OVERSIZE);
		const float Alpha = Ui()->HotItem() == pButtonId ? 0.2f : 0.0f;
		TextRender()->TextColor(Checked ? ColorRGBA(1.0f, 0.85f, 0.3f, 0.8f + Alpha) : ColorRGBA(0.5f, 0.5f, 0.5f, 0.8f + Alpha));
		SLabelProperties Props;
		Props.m_MaxWidth = pRect->w;
		Ui()->DoLabel(pRect, FontIcon::STAR, 12.0f, TEXTALIGN_MC, Props);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
		TextRender()->SetRenderFlags(0);
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
	}
	return Ui()->DoButtonLogic(pButtonId, 0, pRect, BUTTONFLAG_LEFT);
}

int CMenus::DoButton_CheckBox_Common(const void *pId, const char *pText, const char *pBoxText, const CUIRect *pRect, const unsigned Flags)
{
	CUIRect Box, Label;
	pRect->VSplitLeft(pRect->h, &Box, &Label);
	Label.VSplitLeft(5.0f, nullptr, &Label);

	Box.Margin(2.0f, &Box);
	Box.Draw(ColorRGBA(1, 1, 1, 0.25f * Ui()->ButtonColorMul(pId)), IGraphics::CORNER_ALL, 3.0f);

	const bool Checkable = *pBoxText == 'X';
	if(Checkable)
	{
		TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_OVERSIZE | ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT);
		TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		Ui()->DoLabel(&Box, FontIcon::XMARK, Box.h * CUi::ms_FontmodHeight, TEXTALIGN_MC);
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
	}
	else
		Ui()->DoLabel(&Box, pBoxText, Box.h * CUi::ms_FontmodHeight, TEXTALIGN_MC);

	TextRender()->SetRenderFlags(0);
	Ui()->DoLabel(&Label, pText, Box.h * CUi::ms_FontmodHeight, TEXTALIGN_ML);

	return Ui()->DoButtonLogic(pId, 0, pRect, Flags);
}

void CMenus::DoLaserPreview(const CUIRect *pRect, const ColorHSLA LaserOutlineColor, const ColorHSLA LaserInnerColor, const int LaserType)
{
	CUIRect Section = *pRect;
	vec2 From = vec2(Section.x + 30.0f, Section.y + Section.h / 2.0f);
	vec2 Pos = vec2(Section.x + Section.w - 20.0f, Section.y + Section.h / 2.0f);

	const ColorRGBA OuterColor = color_cast<ColorRGBA>(ColorHSLA(LaserOutlineColor));
	const ColorRGBA InnerColor = color_cast<ColorRGBA>(ColorHSLA(LaserInnerColor));
	const float TicksHead = Client()->GlobalTime() * Client()->GameTickSpeed();

	// TicksBody = 4.0 for less laser width for weapon alignment
	GameClient()->m_Items.RenderLaser(From, Pos, OuterColor, InnerColor, 4.0f, TicksHead, LaserType);

	switch(LaserType)
	{
	case LASERTYPE_RIFLE:
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteWeaponLaser);
		Graphics()->SelectSprite(SPRITE_WEAPON_LASER_BODY);
		Graphics()->QuadsBegin();
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		Graphics()->DrawSprite(Section.x + 30.0f, Section.y + Section.h / 2.0f, 60.0f);
		Graphics()->QuadsEnd();
		break;
	case LASERTYPE_SHOTGUN:
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteWeaponShotgun);
		Graphics()->SelectSprite(SPRITE_WEAPON_SHOTGUN_BODY);
		Graphics()->QuadsBegin();
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		Graphics()->DrawSprite(Section.x + 30.0f, Section.y + Section.h / 2.0f, 60.0f);
		Graphics()->QuadsEnd();
		break;
	case LASERTYPE_DRAGGER:
	{
		CTeeRenderInfo TeeRenderInfo;
		TeeRenderInfo.Apply(GameClient()->m_Skins.Find(g_Config.m_ClPlayerSkin));
		TeeRenderInfo.ApplyColors(g_Config.m_ClPlayerUseCustomColor, g_Config.m_ClPlayerColorBody, g_Config.m_ClPlayerColorFeet);
		TeeRenderInfo.m_Size = 64.0f;
		RenderTools()->RenderTee(CAnimState::GetIdle(), &TeeRenderInfo, EMOTE_NORMAL, vec2(-1, 0), Pos);
		break;
	}
	case LASERTYPE_FREEZE:
	{
		CTeeRenderInfo TeeRenderInfo;
		if(g_Config.m_ClShowNinja)
			TeeRenderInfo.Apply(GameClient()->m_Skins.Find("x_ninja"));
		else
			TeeRenderInfo.Apply(GameClient()->m_Skins.Find(g_Config.m_ClPlayerSkin));
		TeeRenderInfo.m_TeeRenderFlags = TEE_EFFECT_FROZEN;
		TeeRenderInfo.m_Size = 64.0f;
		TeeRenderInfo.m_ColorBody = ColorRGBA(1, 1, 1);
		TeeRenderInfo.m_ColorFeet = ColorRGBA(1, 1, 1);
		RenderTools()->RenderTee(CAnimState::GetIdle(), &TeeRenderInfo, EMOTE_PAIN, vec2(1, 0), From);
		GameClient()->m_Effects.FreezingFlakes(From, vec2(32, 32), 1.0f);
		break;
	}
	default:
		GameClient()->m_Items.RenderLaser(From, From, OuterColor, InnerColor, 4.0f, TicksHead, LaserType);
	}
}

bool CMenus::DoLine_RadioMenu(CUIRect &View, const char *pLabel, std::vector<CButtonContainer> &vButtonContainers, const std::vector<const char *> &vLabels, const std::vector<int> &vValues, int &Value)
{
	dbg_assert(vButtonContainers.size() == vValues.size(), "vButtonContainers and vValues must have the same size");
	dbg_assert(vButtonContainers.size() == vLabels.size(), "vButtonContainers and vLabels must have the same size");
	const int N = vButtonContainers.size();
	const float Spacing = 2.0f;
	const float ButtonHeight = 20.0f;
	CUIRect Label, Buttons;
	View.HSplitTop(Spacing, nullptr, &View);
	View.HSplitTop(ButtonHeight, &Buttons, &View);
	Buttons.VSplitMid(&Label, &Buttons, 10.0f);
	Buttons.HMargin(2.0f, &Buttons);
	Ui()->DoLabel(&Label, pLabel, 13.0f, TEXTALIGN_ML);
	const float W = Buttons.w / N;
	bool Pressed = false;
	for(int i = 0; i < N; ++i)
	{
		CUIRect Button;
		Buttons.VSplitLeft(W, &Button, &Buttons);
		int Corner = IGraphics::CORNER_NONE;
		if(i == 0)
			Corner = IGraphics::CORNER_L;
		if(i == N - 1)
			Corner = IGraphics::CORNER_R;
		if(DoButton_Menu(&vButtonContainers[i], vLabels[i], vValues[i] == Value, &Button, BUTTONFLAG_LEFT, nullptr, Corner))
		{
			Pressed = true;
			Value = vValues[i];
		}
	}
	return Pressed;
}

ColorHSLA CMenus::DoLine_ColorPicker(CButtonContainer *pResetId, const float LineSize, const float LabelSize, const float BottomMargin, CUIRect *pMainRect, const char *pText, unsigned int *pColorValue, const ColorRGBA DefaultColor, bool CheckBoxSpacing, int *pCheckBoxValue, bool Alpha)
{
	CUIRect Section, ColorPickerButton, ResetButton, Label;

	pMainRect->HSplitTop(LineSize, &Section, pMainRect);
	pMainRect->HSplitTop(BottomMargin, nullptr, pMainRect);

	Section.VSplitRight(60.0f, &Section, &ResetButton);
	Section.VSplitRight(8.0f, &Section, nullptr);
	Section.VSplitRight(Section.h, &Section, &ColorPickerButton);
	Section.VSplitRight(8.0f, &Label, nullptr);

	if(pCheckBoxValue != nullptr)
	{
		Label.Margin(2.0f, &Label);
		if(DoButton_CheckBox(pCheckBoxValue, pText, *pCheckBoxValue, &Label))
			*pCheckBoxValue ^= 1;
	}
	else if(CheckBoxSpacing)
	{
		Label.VSplitLeft(Label.h + 5.0f, nullptr, &Label);
	}
	if(pCheckBoxValue == nullptr)
	{
		Ui()->DoLabel(&Label, pText, LabelSize, TEXTALIGN_ML);
	}

	const ColorHSLA PickedColor = DoButton_ColorPicker(&ColorPickerButton, pColorValue, Alpha);

	ResetButton.HMargin(2.0f, &ResetButton);
	if(DoButton_Menu(pResetId, Localize("Reset"), 0, &ResetButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 4.0f, 0.1f, ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f)))
	{
		*pColorValue = color_cast<ColorHSLA>(DefaultColor).Pack(Alpha);
	}

	return PickedColor;
}

ColorHSLA CMenus::DoButton_ColorPicker(const CUIRect *pRect, unsigned int *pHslaColor, bool Alpha)
{
	ColorHSLA HslaColor = ColorHSLA(*pHslaColor, Alpha);

	ColorRGBA Outline = ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f);
	Outline.a *= Ui()->ButtonColorMul(pHslaColor);

	CUIRect Rect;
	pRect->Margin(3.0f, &Rect);

	pRect->Draw(Outline, IGraphics::CORNER_ALL, 4.0f);
	Rect.Draw(color_cast<ColorRGBA>(HslaColor), IGraphics::CORNER_ALL, 4.0f);

	if(Ui()->DoButtonLogic(pHslaColor, 0, pRect, BUTTONFLAG_LEFT))
	{
		m_ColorPickerPopupContext.m_pHslaColor = pHslaColor;
		m_ColorPickerPopupContext.m_HslaColor = HslaColor;
		m_ColorPickerPopupContext.m_HsvaColor = color_cast<ColorHSVA>(HslaColor);
		m_ColorPickerPopupContext.m_RgbaColor = color_cast<ColorRGBA>(m_ColorPickerPopupContext.m_HsvaColor);
		m_ColorPickerPopupContext.m_Alpha = Alpha;
		Ui()->ShowPopupColorPicker(Ui()->MouseX(), Ui()->MouseY(), &m_ColorPickerPopupContext);
	}
	else if(Ui()->IsPopupOpen(&m_ColorPickerPopupContext) && m_ColorPickerPopupContext.m_pHslaColor == pHslaColor)
	{
		HslaColor = color_cast<ColorHSLA>(m_ColorPickerPopupContext.m_HsvaColor);
	}

	return HslaColor;
}

int CMenus::DoButton_CheckBoxAutoVMarginAndSet(const void *pId, const char *pText, int *pValue, CUIRect *pRect, float VMargin)
{
	CUIRect CheckBoxRect;
	pRect->HSplitTop(VMargin, &CheckBoxRect, pRect);

	int Logic = DoButton_CheckBox_Common(pId, pText, *pValue ? "X" : "", &CheckBoxRect, BUTTONFLAG_LEFT);

	if(Logic)
		*pValue ^= 1;

	return Logic;
}

int CMenus::DoButton_CheckBox(const void *pId, const char *pText, int Checked, const CUIRect *pRect)
{
	return DoButton_CheckBox_Common(pId, pText, Checked ? "X" : "", pRect, BUTTONFLAG_LEFT);
}

int CMenus::DoButton_CheckBox_Number(const void *pId, const char *pText, int Checked, const CUIRect *pRect)
{
	char aBuf[16];
	str_format(aBuf, sizeof(aBuf), "%d", Checked);
	return DoButton_CheckBox_Common(pId, pText, aBuf, pRect, BUTTONFLAG_LEFT | BUTTONFLAG_RIGHT);
}

void CMenus::RenderMenubar(CUIRect Box, IClient::EClientState ClientState)
{
	CUIRect Button;

	int NewPage = -1;
	int ActivePage = -1;
	if(ClientState == IClient::STATE_OFFLINE)
	{
		ActivePage = m_MenuPage;
	}
	else if(ClientState == IClient::STATE_ONLINE)
	{
		ActivePage = m_GamePage;
	}
	else
	{
		dbg_assert_failed("Client state %d is invalid for RenderMenubar", ClientState);
	}

	// First render buttons aligned from right side so remaining
	// width is known when rendering buttons from left side.
	TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
	TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT | ETextRenderFlags::TEXT_RENDER_FLAG_NO_OVERSIZE);

	Box.VSplitRight(33.0f, &Box, &Button);
	static CButtonContainer s_QuitButton;
	ColorRGBA QuitColor(1, 0, 0, 0.5f);
	if(DoButton_MenuTab(&s_QuitButton, FontIcon::POWER_OFF, 0, &Button, IGraphics::CORNER_T, &m_aAnimatorsSmallPage[SMALL_TAB_QUIT], nullptr, nullptr, &QuitColor, 10.0f))
	{
		if(GameClient()->Editor()->HasUnsavedData() || (GameClient()->CurrentRaceTime() / 60 >= g_Config.m_ClConfirmQuitTime && g_Config.m_ClConfirmQuitTime >= 0) || m_MenusIngameTouchControls.UnsavedChanges() || GameClient()->m_TouchControls.HasEditingChanges())
		{
			m_Popup = POPUP_QUIT;
		}
		else
		{
			Client()->Quit();
		}
	}
	GameClient()->m_Tooltips.DoToolTip(&s_QuitButton, &Button, Localize("Quit"));

	Box.VSplitRight(10.0f, &Box, nullptr);
	Box.VSplitRight(33.0f, &Box, &Button);
	static CButtonContainer s_SettingsButton;
	if(DoButton_MenuTab(&s_SettingsButton, FontIcon::GEAR, ActivePage == PAGE_SETTINGS, &Button, IGraphics::CORNER_T, &m_aAnimatorsSmallPage[SMALL_TAB_SETTINGS]))
	{
		NewPage = PAGE_SETTINGS;
	}
	GameClient()->m_Tooltips.DoToolTip(&s_SettingsButton, &Button, Localize("Settings"));

	Box.VSplitRight(10.0f, &Box, nullptr);
	Box.VSplitRight(33.0f, &Box, &Button);
	static CButtonContainer s_EditorButton;
	if(DoButton_MenuTab(&s_EditorButton, FontIcon::PEN_TO_SQUARE, 0, &Button, IGraphics::CORNER_T, &m_aAnimatorsSmallPage[SMALL_TAB_EDITOR]))
	{
		g_Config.m_ClEditor = 1;
	}
	GameClient()->m_Tooltips.DoToolTip(&s_EditorButton, &Button, Localize("Editor"));

	if(ClientState == IClient::STATE_OFFLINE)
	{
		Box.VSplitRight(10.0f, &Box, nullptr);
		Box.VSplitRight(33.0f, &Box, &Button);
		static CButtonContainer s_DemoButton;
		if(DoButton_MenuTab(&s_DemoButton, FontIcon::CLAPPERBOARD, ActivePage == PAGE_DEMOS, &Button, IGraphics::CORNER_T, &m_aAnimatorsSmallPage[SMALL_TAB_DEMOBUTTON]))
		{
			NewPage = PAGE_DEMOS;
		}
		GameClient()->m_Tooltips.DoToolTip(&s_DemoButton, &Button, Localize("Demos"));
		Box.VSplitRight(10.0f, &Box, nullptr);

		Box.VSplitLeft(33.0f, &Button, &Box);

		bool GotNewsOrUpdate = false;

#if defined(CONF_AUTOUPDATE)
		int State = Updater()->GetCurrentState();
		bool NeedUpdate = str_comp(Client()->LatestVersion(), "0");
		if(State == IUpdater::CLEAN && NeedUpdate)
		{
			GotNewsOrUpdate = true;
		}
#endif

		GotNewsOrUpdate |= (bool)g_Config.m_UiUnreadNews;

		ColorRGBA HomeButtonColorAlert(0, 1, 0, 0.25f);
		ColorRGBA HomeButtonColorAlertHover(0, 1, 0, 0.5f);
		ColorRGBA *pHomeButtonColor = nullptr;
		ColorRGBA *pHomeButtonColorHover = nullptr;

		const char *pHomeScreenButtonLabel = FontIcon::HOUSE;
		if(GotNewsOrUpdate)
		{
			pHomeScreenButtonLabel = FontIcon::NEWSPAPER;
			pHomeButtonColor = &HomeButtonColorAlert;
			pHomeButtonColorHover = &HomeButtonColorAlertHover;
		}

		static CButtonContainer s_StartButton;
		if(DoButton_MenuTab(&s_StartButton, pHomeScreenButtonLabel, false, &Button, IGraphics::CORNER_T, &m_aAnimatorsSmallPage[SMALL_TAB_HOME], pHomeButtonColor, pHomeButtonColor, pHomeButtonColorHover, 10.0f))
		{
			m_ShowStart = true;
		}
		GameClient()->m_Tooltips.DoToolTip(&s_StartButton, &Button, Localize("Main menu"));

		const float BrowserButtonWidth = 75.0f;
		Box.VSplitLeft(10.0f, nullptr, &Box);
		Box.VSplitLeft(BrowserButtonWidth, &Button, &Box);
		static CButtonContainer s_InternetButton;
		if(DoButton_MenuTab(&s_InternetButton, FontIcon::EARTH_AMERICAS, ActivePage == PAGE_INTERNET, &Button, IGraphics::CORNER_T, &m_aAnimatorsBigPage[BIG_TAB_INTERNET]))
		{
			NewPage = PAGE_INTERNET;
		}
		GameClient()->m_Tooltips.DoToolTip(&s_InternetButton, &Button, Localize("Internet"));

		Box.VSplitLeft(BrowserButtonWidth, &Button, &Box);
		static CButtonContainer s_LanButton;
		if(DoButton_MenuTab(&s_LanButton, FontIcon::NETWORK_WIRED, ActivePage == PAGE_LAN, &Button, IGraphics::CORNER_T, &m_aAnimatorsBigPage[BIG_TAB_LAN]))
		{
			NewPage = PAGE_LAN;
		}
		GameClient()->m_Tooltips.DoToolTip(&s_LanButton, &Button, Localize("LAN"));

		Box.VSplitLeft(BrowserButtonWidth, &Button, &Box);
		static CButtonContainer s_FavoritesButton;
		if(DoButton_MenuTab(&s_FavoritesButton, FontIcon::STAR, ActivePage == PAGE_FAVORITES, &Button, IGraphics::CORNER_T, &m_aAnimatorsBigPage[BIG_TAB_FAVORITES]))
		{
			NewPage = PAGE_FAVORITES;
		}
		GameClient()->m_Tooltips.DoToolTip(&s_FavoritesButton, &Button, Localize("Favorites"));

		int MaxPage = PAGE_FAVORITES + ServerBrowser()->FavoriteCommunities().size();
		if(
			!Ui()->IsPopupOpen() &&
			CLineInput::GetActiveInput() == nullptr &&
			(g_Config.m_UiPage >= PAGE_INTERNET && g_Config.m_UiPage <= MaxPage) &&
			(m_MenuPage >= PAGE_INTERNET && m_MenuPage <= PAGE_FAVORITE_COMMUNITY_5))
		{
			if(Input()->KeyPress(KEY_RIGHT))
			{
				NewPage = g_Config.m_UiPage + 1;
				if(NewPage > MaxPage)
					NewPage = PAGE_INTERNET;
			}
			if(Input()->KeyPress(KEY_LEFT))
			{
				NewPage = g_Config.m_UiPage - 1;
				if(NewPage < PAGE_INTERNET)
					NewPage = MaxPage;
			}
		}

		size_t FavoriteCommunityIndex = 0;
		static CButtonContainer s_aFavoriteCommunityButtons[5];
		static_assert(std::size(s_aFavoriteCommunityButtons) == (size_t)PAGE_FAVORITE_COMMUNITY_5 - PAGE_FAVORITE_COMMUNITY_1 + 1);
		static_assert(std::size(s_aFavoriteCommunityButtons) == (size_t)BIT_TAB_FAVORITE_COMMUNITY_5 - BIT_TAB_FAVORITE_COMMUNITY_1 + 1);
		static_assert(std::size(s_aFavoriteCommunityButtons) == (size_t)IServerBrowser::TYPE_FAVORITE_COMMUNITY_5 - IServerBrowser::TYPE_FAVORITE_COMMUNITY_1 + 1);
		for(const CCommunity *pCommunity : ServerBrowser()->FavoriteCommunities())
		{
			if(Box.w < BrowserButtonWidth)
				break;
			Box.VSplitLeft(BrowserButtonWidth, &Button, &Box);
			const int Page = PAGE_FAVORITE_COMMUNITY_1 + FavoriteCommunityIndex;
			if(DoButton_MenuTab(&s_aFavoriteCommunityButtons[FavoriteCommunityIndex], FontIcon::ELLIPSIS, ActivePage == Page, &Button, IGraphics::CORNER_T, &m_aAnimatorsBigPage[BIT_TAB_FAVORITE_COMMUNITY_1 + FavoriteCommunityIndex], nullptr, nullptr, nullptr, 10.0f, m_CommunityIcons.Find(pCommunity->Id())))
			{
				NewPage = Page;
			}
			GameClient()->m_Tooltips.DoToolTip(&s_aFavoriteCommunityButtons[FavoriteCommunityIndex], &Button, pCommunity->Name());

			++FavoriteCommunityIndex;
			if(FavoriteCommunityIndex >= std::size(s_aFavoriteCommunityButtons))
				break;
		}

		TextRender()->SetRenderFlags(0);
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
	}
	else
	{
		TextRender()->SetRenderFlags(0);
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);

		// online menus
		Box.VSplitLeft(90.0f, &Button, &Box);
		static CButtonContainer s_GameButton;
		if(DoButton_MenuTab(&s_GameButton, Localize("Game"), ActivePage == PAGE_GAME, &Button, IGraphics::CORNER_TL))
			NewPage = PAGE_GAME;

		Box.VSplitLeft(90.0f, &Button, &Box);
		static CButtonContainer s_PlayersButton;
		if(DoButton_MenuTab(&s_PlayersButton, Localize("Players"), ActivePage == PAGE_PLAYERS, &Button, IGraphics::CORNER_NONE))
			NewPage = PAGE_PLAYERS;

		Box.VSplitLeft(130.0f, &Button, &Box);
		static CButtonContainer s_ServerInfoButton;
		if(DoButton_MenuTab(&s_ServerInfoButton, Localize("Server info"), ActivePage == PAGE_SERVER_INFO, &Button, IGraphics::CORNER_NONE))
			NewPage = PAGE_SERVER_INFO;

		Box.VSplitLeft(90.0f, &Button, &Box);
		static CButtonContainer s_NetworkButton;
		if(DoButton_MenuTab(&s_NetworkButton, Localize("Browser"), ActivePage == PAGE_NETWORK, &Button, IGraphics::CORNER_NONE))
			NewPage = PAGE_NETWORK;

		if(GameClient()->m_GameInfo.m_Race)
		{
			Box.VSplitLeft(90.0f, &Button, &Box);
			static CButtonContainer s_GhostButton;
			if(DoButton_MenuTab(&s_GhostButton, Localize("Ghost"), ActivePage == PAGE_GHOST, &Button, IGraphics::CORNER_NONE))
				NewPage = PAGE_GHOST;
		}

		Box.VSplitLeft(100.0f, &Button, &Box);
		Box.VSplitLeft(4.0f, nullptr, &Box);
		static CButtonContainer s_CallVoteButton;
		if(DoButton_MenuTab(&s_CallVoteButton, Localize("Call vote"), ActivePage == PAGE_CALLVOTE, &Button, IGraphics::CORNER_TR))
		{
			NewPage = PAGE_CALLVOTE;
			m_ControlPageOpening = true;
		}

		if(Box.w >= 10.0f + 33.0f + 10.0f)
		{
			TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
			TextRender()->SetRenderFlags(ETextRenderFlags::TEXT_RENDER_FLAG_ONLY_ADVANCE_WIDTH | ETextRenderFlags::TEXT_RENDER_FLAG_NO_X_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_Y_BEARING | ETextRenderFlags::TEXT_RENDER_FLAG_NO_PIXEL_ALIGNMENT | ETextRenderFlags::TEXT_RENDER_FLAG_NO_OVERSIZE);

			Box.VSplitRight(10.0f, &Box, nullptr);
			Box.VSplitRight(33.0f, &Box, &Button);
			static CButtonContainer s_DemoButton;
			if(DoButton_MenuTab(&s_DemoButton, FontIcon::CLAPPERBOARD, ActivePage == PAGE_DEMOS, &Button, IGraphics::CORNER_T, &m_aAnimatorsSmallPage[SMALL_TAB_DEMOBUTTON]))
			{
				NewPage = PAGE_DEMOS;
			}
			GameClient()->m_Tooltips.DoToolTip(&s_DemoButton, &Button, Localize("Demos"));
			Box.VSplitRight(10.0f, &Box, nullptr);

			TextRender()->SetRenderFlags(0);
			TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
		}
	}

	if(NewPage != -1)
	{
		if(ClientState == IClient::STATE_OFFLINE)
			SetMenuPage(NewPage);
		else
			m_GamePage = NewPage;
	}
}

void CMenus::RenderLoading(const char *pCaption, const char *pContent, int IncreaseCounter)
{
	// TODO: not supported right now due to separate render thread

	const int CurLoadRenderCount = m_LoadingState.m_Current;
	m_LoadingState.m_Current += IncreaseCounter;
	dbg_assert(m_LoadingState.m_Current <= m_LoadingState.m_Total, "Invalid progress for RenderLoading");

	// make sure that we don't render for each little thing we load
	// because that will slow down loading if we have vsync
	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	if(Now - m_LoadingState.m_LastRender < std::chrono::nanoseconds(1s) / 60l)
		return;

	// need up date this here to get correct
	ms_GuiColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true));

	Ui()->MapScreen();

	if(GameClient()->m_MenuBackground.IsLoading())
	{
		// Avoid rendering while loading the menu background as this would otherwise
		// cause the regular menu background to be rendered for a few frames while
		// the menu background is not loaded yet.
		return;
	}
	if(!GameClient()->m_MenuBackground.Render())
	{
		RenderBackground();
	}

	m_LoadingState.m_LastRender = Now;

	CUIRect Box;
	Ui()->Screen()->Margin(160.0f, &Box);

	Graphics()->BlendNormal();
	Graphics()->TextureClear();
	Box.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f), IGraphics::CORNER_ALL, 15.0f);
	Box.Margin(20.0f, &Box);

	CUIRect Label;
	Box.HSplitTop(24.0f, &Label, &Box);
	Ui()->DoLabel(&Label, pCaption, 24.0f, TEXTALIGN_MC);

	Box.HSplitTop(20.0f, nullptr, &Box);
	Box.HSplitTop(24.0f, &Label, &Box);
	Ui()->DoLabel(&Label, pContent, 20.0f, TEXTALIGN_MC);

	if(m_LoadingState.m_Total > 0)
	{
		CUIRect ProgressBar;
		Box.HSplitBottom(30.0f, &Box, nullptr);
		Box.HSplitBottom(25.0f, &Box, &ProgressBar);
		ProgressBar.VMargin(20.0f, &ProgressBar);
		Ui()->RenderProgressBar(ProgressBar, CurLoadRenderCount / (float)m_LoadingState.m_Total);
	}

	Graphics()->SetColor(1.0, 1.0, 1.0, 1.0);

	Client()->UpdateAndSwap();
}

void CMenus::FinishLoading()
{
	m_LoadingState.m_Current = 0;
	m_LoadingState.m_Total = 0;
}

void CMenus::RenderNews(CUIRect MainView)
{
	GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_NEWS);

	g_Config.m_UiUnreadNews = false;

	MainView.Draw(ms_ColorTabbarActive, IGraphics::CORNER_B, 10.0f);

	MainView.HSplitTop(10.0f, nullptr, &MainView);
	MainView.VSplitLeft(15.0f, nullptr, &MainView);

	CUIRect Label;

	const char *pStr = Client()->News();
	char aLine[256];
	while((pStr = str_next_token(pStr, "\n", aLine, sizeof(aLine))))
	{
		const int Len = str_length(aLine);
		if(Len > 0 && aLine[0] == '|' && aLine[Len - 1] == '|')
		{
			MainView.HSplitTop(30.0f, &Label, &MainView);
			aLine[Len - 1] = '\0';
			Ui()->DoLabel(&Label, aLine + 1, 20.0f, TEXTALIGN_ML);
		}
		else
		{
			MainView.HSplitTop(20.0f, &Label, &MainView);
			Ui()->DoLabel(&Label, aLine, 15.f, TEXTALIGN_ML);
		}
	}
}

bool CMenus::IsClientChatPageActive() const
{
	const IClient::EClientState ClientState = Client()->State();
	if(ClientState == IClient::STATE_OFFLINE)
		return m_MenuPage == PAGE_CLIENT_CHAT;
	if(ClientState == IClient::STATE_ONLINE)
		return m_GamePage == PAGE_CLIENT_CHAT;
	return false;
}

void CMenus::ActivateClientChatRoom()
{
	m_aClientChatDirectTargetId[0] = '\0';
	m_aClientChatDirectTargetName[0] = '\0';
	str_copy(m_aClientChatDirectTargetSkin, "default", sizeof(m_aClientChatDirectTargetSkin));
	m_ClientChatDirectTargetUseCustomColor = false;
	m_ClientChatDirectTargetColorBody = 65408;
	m_ClientChatDirectTargetColorFeet = 65408;
}

void CMenus::ActivateClientChatDirect(const char *pUserId, const char *pName, const char *pSkin, bool UseCustomColor, int ColorBody, int ColorFeet)
{
	EnsureClientChatDirectContact(pUserId, pName, pSkin, UseCustomColor, ColorBody, ColorFeet);

	ClientChatCopyOptional(m_aClientChatDirectTargetId, sizeof(m_aClientChatDirectTargetId), pUserId);
	ClientChatCopyOptional(m_aClientChatDirectTargetName, sizeof(m_aClientChatDirectTargetName), pName);
	ClientChatCopyOptional(m_aClientChatDirectTargetSkin, sizeof(m_aClientChatDirectTargetSkin), pSkin, "default");
	m_ClientChatDirectTargetUseCustomColor = UseCustomColor;
	m_ClientChatDirectTargetColorBody = ColorBody;
	m_ClientChatDirectTargetColorFeet = ColorFeet;
	RefreshClientChatConversations();

	for(CClientChatConversation &Conversation : m_vClientChatConversations)
	{
		if((m_aClientChatDirectTargetId[0] != '\0' && ClientChatIdsEqual(Conversation.m_aUserId, m_aClientChatDirectTargetId)) ||
			(m_aClientChatDirectTargetId[0] == '\0' && ClientChatNamesEqual(Conversation.m_aName, m_aClientChatDirectTargetName)))
		{
			Conversation.m_UnreadCount = 0;
			ClientChatCopyOptional(m_aClientChatDirectTargetName, sizeof(m_aClientChatDirectTargetName), Conversation.m_aName, m_aClientChatDirectTargetName);
			ClientChatCopyOptional(m_aClientChatDirectTargetSkin, sizeof(m_aClientChatDirectTargetSkin), Conversation.m_aSkin, "default");
			if(m_aClientChatDirectTargetId[0] == '\0')
				ClientChatCopyOptional(m_aClientChatDirectTargetId, sizeof(m_aClientChatDirectTargetId), Conversation.m_aUserId);
			m_ClientChatDirectTargetUseCustomColor = Conversation.m_UseCustomColor;
			m_ClientChatDirectTargetColorBody = Conversation.m_ColorBody;
			m_ClientChatDirectTargetColorFeet = Conversation.m_ColorFeet;
			break;
		}
	}
}

CMenus::CClientChatDirectContact *CMenus::FindClientChatDirectContact(const char *pUserId, const char *pName)
{
	for(CClientChatDirectContact &Contact : m_vClientChatDirectContacts)
	{
		if((pUserId != nullptr && pUserId[0] != '\0' && Contact.m_aUserId[0] != '\0' && ClientChatIdsEqual(Contact.m_aUserId, pUserId)) ||
			((pUserId == nullptr || pUserId[0] == '\0' || Contact.m_aUserId[0] == '\0') && ClientChatNamesEqual(Contact.m_aName, pName)))
			return &Contact;
	}
	return nullptr;
}

const CMenus::CClientChatDirectContact *CMenus::FindClientChatDirectContact(const char *pUserId, const char *pName) const
{
	for(const CClientChatDirectContact &Contact : m_vClientChatDirectContacts)
	{
		if((pUserId != nullptr && pUserId[0] != '\0' && Contact.m_aUserId[0] != '\0' && ClientChatIdsEqual(Contact.m_aUserId, pUserId)) ||
			((pUserId == nullptr || pUserId[0] == '\0' || Contact.m_aUserId[0] == '\0') && ClientChatNamesEqual(Contact.m_aName, pName)))
			return &Contact;
	}
	return nullptr;
}

void CMenus::LoadClientChatDirectContacts()
{
	m_vClientChatDirectContacts.clear();

	char aContactsFile[IO_MAX_PATH_LENGTH];
	if(!ClientChatContactsFilePath(aContactsFile, sizeof(aContactsFile), m_aClientChatAccountId))
		return;

	char *pRawContacts = Storage()->ReadFileStr(aContactsFile, IStorage::TYPE_SAVE);
	if(pRawContacts == nullptr || pRawContacts[0] == '\0')
	{
		if(pRawContacts != nullptr)
			std::free(pRawContacts);
		return;
	}

	json_settings JsonSettings = {};
	char aError[json_error_max];
	json_value *pJson = json_parse_ex(&JsonSettings, pRawContacts, str_length(pRawContacts), aError);
	std::free(pRawContacts);

	if(pJson == nullptr || pJson->type != json_array)
	{
		if(pJson != nullptr)
			json_value_free(pJson);
		return;
	}

	const int NumContacts = json_array_length(pJson);
	for(int Index = 0; Index < NumContacts; ++Index)
	{
		const json_value *pEntry = json_array_get(pJson, Index);
		if(pEntry == nullptr || pEntry->type != json_object)
			continue;

		CClientChatDirectContact Contact;
		if(const json_value *pUserId = json_object_get(pEntry, "user_id"); pUserId != nullptr && pUserId->type == json_string)
			str_copy(Contact.m_aUserId, json_string_get(pUserId), sizeof(Contact.m_aUserId));
		if(const json_value *pName = json_object_get(pEntry, "name"); pName != nullptr && pName->type == json_string)
			str_copy(Contact.m_aName, json_string_get(pName), sizeof(Contact.m_aName));
		if(const json_value *pSkin = json_object_get(pEntry, "skin"); pSkin != nullptr && pSkin->type == json_string)
			str_copy(Contact.m_aSkin, json_string_get(pSkin), sizeof(Contact.m_aSkin));
		if(const json_value *pUseCustomColor = json_object_get(pEntry, "use_custom_color"); pUseCustomColor != nullptr && pUseCustomColor->type == json_boolean)
			Contact.m_UseCustomColor = json_boolean_get(pUseCustomColor);
		if(const json_value *pColorBody = json_object_get(pEntry, "color_body"); pColorBody != nullptr && pColorBody->type == json_integer)
			Contact.m_ColorBody = json_int_get(pColorBody);
		if(const json_value *pColorFeet = json_object_get(pEntry, "color_feet"); pColorFeet != nullptr && pColorFeet->type == json_integer)
			Contact.m_ColorFeet = json_int_get(pColorFeet);
		if(const json_value *pHiddenBeforeId = json_object_get(pEntry, "hidden_before_id"); pHiddenBeforeId != nullptr && pHiddenBeforeId->type == json_integer)
			Contact.m_HiddenBeforeId = json_int_get(pHiddenBeforeId);
		if(const json_value *pVisible = json_object_get(pEntry, "visible"); pVisible != nullptr && pVisible->type == json_boolean)
			Contact.m_Visible = json_boolean_get(pVisible);

		if(Contact.m_aName[0] == '\0' && Contact.m_aUserId[0] == '\0')
			continue;
		if(Contact.m_aSkin[0] == '\0')
			str_copy(Contact.m_aSkin, "default", sizeof(Contact.m_aSkin));
		if(m_aClientChatAccountId[0] != '\0' && Contact.m_aUserId[0] != '\0' && !ClientChatIsAccountId(Contact.m_aUserId))
			continue;

		if(CClientChatDirectContact *pExisting = FindClientChatDirectContact(Contact.m_aUserId, Contact.m_aName))
		{
			ClientChatCopyOptional(pExisting->m_aUserId, sizeof(pExisting->m_aUserId), Contact.m_aUserId, pExisting->m_aUserId);
			ClientChatCopyOptional(pExisting->m_aName, sizeof(pExisting->m_aName), Contact.m_aName, pExisting->m_aName);
			ClientChatCopyOptional(pExisting->m_aSkin, sizeof(pExisting->m_aSkin), Contact.m_aSkin, "default");
			pExisting->m_UseCustomColor = Contact.m_UseCustomColor;
			pExisting->m_ColorBody = Contact.m_ColorBody;
			pExisting->m_ColorFeet = Contact.m_ColorFeet;
			pExisting->m_HiddenBeforeId = maximum(pExisting->m_HiddenBeforeId, Contact.m_HiddenBeforeId);
			pExisting->m_Visible = pExisting->m_Visible || Contact.m_Visible;
			continue;
		}

		if(m_aClientChatAccountId[0] != '\0')
		{
			for(CClientChatDirectContact &Existing : m_vClientChatDirectContacts)
			{
				if(!ClientChatNamesEqual(Existing.m_aName, Contact.m_aName))
					continue;

				const bool ExistingAccountId = ClientChatIsAccountId(Existing.m_aUserId);
				const bool ContactAccountId = ClientChatIsAccountId(Contact.m_aUserId);
				if(ExistingAccountId == ContactAccountId)
					continue;

				CClientChatDirectContact &Preferred = ExistingAccountId ? Existing : Contact;
				const CClientChatDirectContact &Legacy = ExistingAccountId ? Contact : Existing;
				ClientChatCopyOptional(Preferred.m_aSkin, sizeof(Preferred.m_aSkin), Legacy.m_aSkin, Preferred.m_aSkin);
				Preferred.m_UseCustomColor = Legacy.m_UseCustomColor;
				Preferred.m_ColorBody = Legacy.m_ColorBody;
				Preferred.m_ColorFeet = Legacy.m_ColorFeet;
				Preferred.m_HiddenBeforeId = maximum(Preferred.m_HiddenBeforeId, Legacy.m_HiddenBeforeId);
				Preferred.m_Visible = Preferred.m_Visible || Legacy.m_Visible;
				if(!ExistingAccountId)
					Existing = Preferred;
				goto skip_push_contact;
			}
		}

		m_vClientChatDirectContacts.push_back(Contact);
skip_push_contact:
		;
	}

	json_value_free(pJson);
	SaveClientChatDirectContacts();
}

void CMenus::SaveClientChatDirectContacts() const
{
	char aContactsFile[IO_MAX_PATH_LENGTH];
	if(!ClientChatContactsFilePath(aContactsFile, sizeof(aContactsFile), m_aClientChatAccountId))
		return;

	Storage()->CreateFolder(CLIENT_CHAT_CONTACTS_FOLDER, IStorage::TYPE_SAVE);
	IOHANDLE File = Storage()->OpenFile(aContactsFile, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return;

	CJsonFileWriter Writer(File);
	Writer.BeginArray();
	for(const CClientChatDirectContact &Contact : m_vClientChatDirectContacts)
	{
		Writer.BeginObject();
		Writer.WriteAttribute("user_id");
		Writer.WriteStrValue(Contact.m_aUserId);
		Writer.WriteAttribute("name");
		Writer.WriteStrValue(Contact.m_aName);
		Writer.WriteAttribute("skin");
		Writer.WriteStrValue(Contact.m_aSkin);
		Writer.WriteAttribute("use_custom_color");
		Writer.WriteBoolValue(Contact.m_UseCustomColor);
		Writer.WriteAttribute("color_body");
		Writer.WriteIntValue(Contact.m_ColorBody);
		Writer.WriteAttribute("color_feet");
		Writer.WriteIntValue(Contact.m_ColorFeet);
		Writer.WriteAttribute("hidden_before_id");
		Writer.WriteIntValue(Contact.m_HiddenBeforeId);
		Writer.WriteAttribute("visible");
		Writer.WriteBoolValue(Contact.m_Visible);
		Writer.EndObject();
	}
	Writer.EndArray();
}

void CMenus::EnsureClientChatDirectContact(const char *pUserId, const char *pName, const char *pSkin, bool UseCustomColor, int ColorBody, int ColorFeet)
{
	if((pUserId == nullptr || pUserId[0] == '\0') && (pName == nullptr || pName[0] == '\0'))
		return;

	if(m_aClientChatAccountId[0] != '\0' && pUserId != nullptr && pUserId[0] != '\0' && !ClientChatIsAccountId(pUserId))
		return;

	CClientChatDirectContact *pContact = FindClientChatDirectContact(pUserId, pName);
	if(pContact == nullptr)
	{
		if(m_aClientChatAccountId[0] != '\0' && pName != nullptr && pName[0] != '\0')
		{
			for(CClientChatDirectContact &Contact : m_vClientChatDirectContacts)
			{
				if(!Contact.m_Visible || !ClientChatNamesEqual(Contact.m_aName, pName))
					continue;

				const bool ExistingAccountId = ClientChatIsAccountId(Contact.m_aUserId);
				const bool NewAccountId = ClientChatIsAccountId(pUserId);
				if(ExistingAccountId || !NewAccountId)
					continue;

				ClientChatCopyOptional(Contact.m_aUserId, sizeof(Contact.m_aUserId), pUserId, Contact.m_aUserId);
				ClientChatCopyOptional(Contact.m_aSkin, sizeof(Contact.m_aSkin), pSkin, "default");
				Contact.m_UseCustomColor = UseCustomColor;
				Contact.m_ColorBody = ColorBody;
				Contact.m_ColorFeet = ColorFeet;
				SaveClientChatDirectContacts();
				return;
			}
		}

		CClientChatDirectContact Contact;
		ClientChatCopyOptional(Contact.m_aUserId, sizeof(Contact.m_aUserId), pUserId);
		ClientChatCopyOptional(Contact.m_aName, sizeof(Contact.m_aName), pName, "unnamed");
		ClientChatCopyOptional(Contact.m_aSkin, sizeof(Contact.m_aSkin), pSkin, "default");
		Contact.m_UseCustomColor = UseCustomColor;
		Contact.m_ColorBody = ColorBody;
		Contact.m_ColorFeet = ColorFeet;
		Contact.m_HiddenBeforeId = m_ClientChatLastDirectId;
		Contact.m_Visible = true;
		m_vClientChatDirectContacts.push_back(Contact);
		SaveClientChatDirectContacts();
		return;
	}

	const bool WasVisible = pContact->m_Visible;
	ClientChatCopyOptional(pContact->m_aUserId, sizeof(pContact->m_aUserId), pUserId, pContact->m_aUserId);
	ClientChatCopyOptional(pContact->m_aName, sizeof(pContact->m_aName), pName, pContact->m_aName);
	ClientChatCopyOptional(pContact->m_aSkin, sizeof(pContact->m_aSkin), pSkin, "default");
	pContact->m_UseCustomColor = UseCustomColor;
	pContact->m_ColorBody = ColorBody;
	pContact->m_ColorFeet = ColorFeet;
	pContact->m_Visible = true;
	if(!WasVisible)
		pContact->m_HiddenBeforeId = maximum(pContact->m_HiddenBeforeId, m_ClientChatLastDirectId);
	SaveClientChatDirectContacts();
}

void CMenus::DeleteClientChatDirectContact(const char *pUserId, const char *pName)
{
	CClientChatDirectContact *pContact = FindClientChatDirectContact(pUserId, pName);
	if(pContact == nullptr)
		return;

	pContact->m_Visible = false;
	pContact->m_HiddenBeforeId = maximum(pContact->m_HiddenBeforeId, m_ClientChatLastDirectId);
	SaveClientChatDirectContacts();

	const bool Selected = (m_aClientChatDirectTargetId[0] != '\0' && pContact->m_aUserId[0] != '\0' && ClientChatIdsEqual(pContact->m_aUserId, m_aClientChatDirectTargetId)) ||
		(m_aClientChatDirectTargetName[0] != '\0' && ClientChatNamesEqual(pContact->m_aName, m_aClientChatDirectTargetName));
	if(Selected)
		ActivateClientChatRoom();

	RefreshClientChatConversations();
}

void CMenus::RefreshClientChatConversations()
{
	const char *pLocalNameRaw = Client()->PlayerName();
	const char *pLocalName = pLocalNameRaw != nullptr ? pLocalNameRaw : "";
	const char *pLocalId = m_aClientChatAccountId[0] != '\0' ? m_aClientChatAccountId : (m_aClientChatRuntimeIdentity[0] != '\0' ? m_aClientChatRuntimeIdentity : g_Config.m_ClClientChatIdentity);

	auto IsLocalMessage = [&](const CClientChatMessage &Message) {
		return ClientChatIsLocalMessage(Message.m_aSenderId, Message.m_aSenderInstanceId, Message.m_aName, pLocalId, pLocalName, m_aClientChatRuntimeIdentity, m_aClientChatAccountId[0] != '\0');
	};
	auto SameConversation = [](const CClientChatConversation &Conversation, const char *pUserId, const char *pName) {
		if(pUserId != nullptr && pUserId[0] != '\0' && Conversation.m_aUserId[0] != '\0')
			return ClientChatIdsEqual(Conversation.m_aUserId, pUserId);
		return ClientChatNamesEqual(Conversation.m_aName, pName);
	};

	auto FindVisibleContact = [&](const char *pUserId, const char *pName) -> const CClientChatDirectContact * {
		const CClientChatDirectContact *pContact = FindClientChatDirectContact(pUserId, pName);
		return pContact != nullptr && pContact->m_Visible ? pContact : nullptr;
	};

	std::vector<CClientChatConversation> vPrevious = m_vClientChatConversations;
	std::vector<CClientChatConversation> vConversations;

	auto FindConversation = [&](const char *pUserId, const char *pName) -> CClientChatConversation * {
		for(CClientChatConversation &Conversation : vConversations)
		{
			if(SameConversation(Conversation, pUserId, pName))
				return &Conversation;
		}
		return nullptr;
	};

	auto GetOrCreateConversation = [&](const char *pUserId, const char *pName) -> CClientChatConversation & {
		if(CClientChatConversation *pExisting = FindConversation(pUserId, pName))
			return *pExisting;

		CClientChatConversation Conversation;
		ClientChatCopyOptional(Conversation.m_aUserId, sizeof(Conversation.m_aUserId), pUserId);
		ClientChatCopyOptional(Conversation.m_aName, sizeof(Conversation.m_aName), pName, "unnamed");
		for(const CClientChatConversation &Previous : vPrevious)
		{
			if(SameConversation(Previous, pUserId, pName))
			{
				Conversation.m_UnreadCount = Previous.m_UnreadCount;
				ClientChatCopyOptional(Conversation.m_aSkin, sizeof(Conversation.m_aSkin), Previous.m_aSkin, "default");
				Conversation.m_UseCustomColor = Previous.m_UseCustomColor;
				Conversation.m_ColorBody = Previous.m_ColorBody;
				Conversation.m_ColorFeet = Previous.m_ColorFeet;
				ClientChatCopyOptional(Conversation.m_aPreview, sizeof(Conversation.m_aPreview), Previous.m_aPreview);
				Conversation.m_LastTimestamp = Previous.m_LastTimestamp;
				break;
			}
		}
		vConversations.push_back(Conversation);
		return vConversations.back();
	};

	auto TouchConversation = [&](CClientChatConversation &Conversation, const char *pUserId, const char *pName, const char *pSkin, bool UseCustomColor, int ColorBody, int ColorFeet, const char *pPreview, int Timestamp) {
		if(Conversation.m_aUserId[0] == '\0')
			ClientChatCopyOptional(Conversation.m_aUserId, sizeof(Conversation.m_aUserId), pUserId);
		if(Conversation.m_aName[0] == '\0' || ClientChatNamesEqual(Conversation.m_aName, "unnamed"))
			ClientChatCopyOptional(Conversation.m_aName, sizeof(Conversation.m_aName), pName, "unnamed");
		if(pSkin != nullptr && pSkin[0] != '\0')
			ClientChatCopyOptional(Conversation.m_aSkin, sizeof(Conversation.m_aSkin), pSkin, "default");
		Conversation.m_UseCustomColor = UseCustomColor;
		Conversation.m_ColorBody = ColorBody;
		Conversation.m_ColorFeet = ColorFeet;
		if(Timestamp >= Conversation.m_LastTimestamp)
		{
			Conversation.m_LastTimestamp = Timestamp;
			ClientChatTrimPreview(Conversation.m_aPreview, sizeof(Conversation.m_aPreview), pPreview);
		}
	};

	for(const CClientChatDirectContact &Contact : m_vClientChatDirectContacts)
	{
		if(!Contact.m_Visible || IsClientChatBlocked(Contact.m_aUserId, Contact.m_aName))
			continue;

		CClientChatConversation &Conversation = GetOrCreateConversation(Contact.m_aUserId, Contact.m_aName);
		TouchConversation(Conversation, Contact.m_aUserId, Contact.m_aName, Contact.m_aSkin, Contact.m_UseCustomColor, Contact.m_ColorBody, Contact.m_ColorFeet, "Private line", 0);
	}

	for(const CClientChatMessage &Message : m_vClientChatDirectMessages)
	{
		char aPeerId[64] = "";
		char aPeerName[64] = "";
		const bool LocalMessage = IsLocalMessage(Message);
		if(LocalMessage)
		{
			ClientChatCopyOptional(aPeerId, sizeof(aPeerId), Message.m_aTargetId);
			ClientChatCopyOptional(aPeerName, sizeof(aPeerName), Message.m_aTargetName, Message.m_aName);
		}
		else
		{
			ClientChatCopyOptional(aPeerId, sizeof(aPeerId), Message.m_aSenderId);
			ClientChatCopyOptional(aPeerName, sizeof(aPeerName), Message.m_aName, Message.m_aTargetName);
		}

		const CClientChatDirectContact *pContact = FindVisibleContact(aPeerId, aPeerName);
		if(pContact == nullptr || Message.m_Id <= pContact->m_HiddenBeforeId)
			continue;
		if(IsClientChatBlocked(pContact->m_aUserId, pContact->m_aName))
			continue;

		CClientChatConversation &Conversation = GetOrCreateConversation(pContact->m_aUserId, pContact->m_aName);
		char aPreview[96];
		if(LocalMessage)
			str_format(aPreview, sizeof(aPreview), "You: %s", Message.m_aText);
		else
			ClientChatCopyOptional(aPreview, sizeof(aPreview), Message.m_aText);

		const char *pSkin = LocalMessage ? pContact->m_aSkin : Message.m_aSkin;
		const bool UseCustomColor = LocalMessage ? pContact->m_UseCustomColor : Message.m_UseCustomColor;
		const int ColorBody = LocalMessage ? pContact->m_ColorBody : Message.m_ColorBody;
		const int ColorFeet = LocalMessage ? pContact->m_ColorFeet : Message.m_ColorFeet;
		TouchConversation(Conversation, pContact->m_aUserId, pContact->m_aName, pSkin, UseCustomColor, ColorBody, ColorFeet, aPreview, Message.m_Timestamp);
	}

	std::sort(vConversations.begin(), vConversations.end(), [](const CClientChatConversation &Left, const CClientChatConversation &Right) {
		if(Left.m_LastTimestamp != Right.m_LastTimestamp)
			return Left.m_LastTimestamp > Right.m_LastTimestamp;
		return str_utf8_comp_nocase(Left.m_aName, Right.m_aName) < 0;
	});

	m_vClientChatConversations = std::move(vConversations);
	bool SelectionStillExists = m_aClientChatDirectTargetId[0] == '\0' && m_aClientChatDirectTargetName[0] == '\0';
	for(const CClientChatConversation &Conversation : m_vClientChatConversations)
	{
		if((m_aClientChatDirectTargetId[0] != '\0' && ClientChatIdsEqual(Conversation.m_aUserId, m_aClientChatDirectTargetId)) ||
			(m_aClientChatDirectTargetId[0] == '\0' && m_aClientChatDirectTargetName[0] != '\0' && ClientChatNamesEqual(Conversation.m_aName, m_aClientChatDirectTargetName)))
		{
			ClientChatCopyOptional(m_aClientChatDirectTargetId, sizeof(m_aClientChatDirectTargetId), Conversation.m_aUserId, m_aClientChatDirectTargetId);
			ClientChatCopyOptional(m_aClientChatDirectTargetName, sizeof(m_aClientChatDirectTargetName), Conversation.m_aName, m_aClientChatDirectTargetName);
			ClientChatCopyOptional(m_aClientChatDirectTargetSkin, sizeof(m_aClientChatDirectTargetSkin), Conversation.m_aSkin, "default");
			m_ClientChatDirectTargetUseCustomColor = Conversation.m_UseCustomColor;
			m_ClientChatDirectTargetColorBody = Conversation.m_ColorBody;
			m_ClientChatDirectTargetColorFeet = Conversation.m_ColorFeet;
			SelectionStillExists = true;
			break;
		}
	}

	if(!SelectionStillExists)
		ActivateClientChatRoom();
}

void CMenus::StartClientChatFetch()
{
	if(m_pClientChatFetchTask || m_pClientChatAuthTask || g_Config.m_ClClientChatUrl[0] == '\0' || g_Config.m_ClClientChatAuthToken[0] == '\0')
		return;

	char aUrl[256];
	str_format(aUrl, sizeof(aUrl), "%s?room_limit=%d&direct_limit=%d", g_Config.m_ClClientChatUrl, CLIENT_CHAT_ROOM_LIMIT, CLIENT_CHAT_DIRECT_LIMIT);

	std::shared_ptr<CHttpRequest> pRequest = HttpGet(aUrl);
	pRequest->Timeout(CLIENT_CHAT_TIMEOUT);
	pRequest->LogProgress(HTTPLOG::FAILURE);
	pRequest->FailOnErrorStatus(false);
	pRequest->MaxResponseSize(512 * 1024);
	pRequest->HeaderString(CLIENT_CHAT_HEADER_NAME, CLIENT_CHAT_HEADER_VALUE);
	pRequest->HeaderString(CLIENT_CHAT_AUTH_HEADER_NAME, g_Config.m_ClClientChatAuthToken);
	Http()->Run(pRequest);

	m_pClientChatFetchTask = pRequest;
	m_ClientChatLastPollTime = time_get_nanoseconds();

	if(m_vClientChatRoomMessages.empty() && m_vClientChatDirectMessages.empty())
	{
		str_copy(m_aClientChatStatus, m_aClientChatAccountId[0] == '\0' ? "Checking saved session..." : Localize("Connecting to client chat..."), sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = false;
	}
}

void CMenus::HandleClientChatFetchCompletion(const std::shared_ptr<CHttpRequest> &pTask)
{
	if(!pTask)
		return;

	if(pTask->State() != EHttpState::DONE)
	{
		str_copy(m_aClientChatStatus, Localize("Couldn't reach the client chat server."), sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		return;
	}

	if(pTask->StatusCode() >= 400)
	{
		if(pTask->StatusCode() == 401 || pTask->StatusCode() == 403)
		{
			++m_ClientChatTransientAuthFailures;
			if(m_ClientChatTransientAuthFailures < 3 && g_Config.m_ClClientChatAuthToken[0] != '\0')
			{
				m_aClientChatAccountId[0] = '\0';
				str_format(m_aClientChatStatus, sizeof(m_aClientChatStatus), "Reconnecting chat session (%d/3)...", m_ClientChatTransientAuthFailures);
				m_ClientChatStatusError = false;
				m_ClientChatLastPollTime = 0ns;
			}
			else
			{
				ClearClientChatAuthState(true);
				str_copy(m_aClientChatStatus, "Session expired. Log in again.", sizeof(m_aClientChatStatus));
				m_ClientChatStatusError = true;
			}
			return;
		}

		str_format(m_aClientChatStatus, sizeof(m_aClientChatStatus), Localize("Client chat server returned %d."), pTask->StatusCode());
		m_ClientChatStatusError = true;
		return;
	}

	json_value *pJson = pTask->ResultJson();
	if(pJson == nullptr || pJson->type != json_object)
	{
		str_copy(m_aClientChatStatus, Localize("Client chat returned invalid data."), sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		if(pJson != nullptr)
			json_value_free(pJson);
		return;
	}

	if(const json_value *pOk = json_object_get(pJson, "ok"); pOk != nullptr && pOk->type == json_boolean && !json_boolean_get(pOk))
	{
		const char *pErrorText = "Client chat auth failed.";
		if(const json_value *pError = json_object_get(pJson, "error"); pError != nullptr && pError->type == json_string)
		{
			const char *pErrorCode = json_string_get(pError);
			if(str_comp(pErrorCode, "auth_required") == 0 || str_comp(pErrorCode, "invalid_session") == 0 || str_comp(pErrorCode, "session_expired") == 0)
			{
				++m_ClientChatTransientAuthFailures;
				if(m_ClientChatTransientAuthFailures < 3 && g_Config.m_ClClientChatAuthToken[0] != '\0')
				{
					m_aClientChatAccountId[0] = '\0';
					str_format(m_aClientChatStatus, sizeof(m_aClientChatStatus), "Reconnecting chat session (%d/3)...", m_ClientChatTransientAuthFailures);
					m_ClientChatStatusError = false;
					m_ClientChatLastPollTime = 0ns;
					json_value_free(pJson);
					return;
				}
				ClearClientChatAuthState(true);
				pErrorText = "Session expired. Log in again.";
			}
			else
				pErrorText = "Client chat server rejected the request.";
		}

		str_copy(m_aClientChatStatus, pErrorText, sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		json_value_free(pJson);
		return;
	}

	const json_value *pAccount = json_object_get(pJson, "account");
	if(pAccount == nullptr || pAccount->type != json_object)
	{
		str_copy(m_aClientChatStatus, "Client chat account data is missing.", sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		json_value_free(pJson);
		return;
	}

	char aPreviousAccountId[sizeof(m_aClientChatAccountId)];
	str_copy(aPreviousAccountId, m_aClientChatAccountId, sizeof(aPreviousAccountId));
	if(const json_value *pId = json_object_get(pAccount, "id"); pId != nullptr && pId->type == json_string)
		str_copy(m_aClientChatAccountId, json_string_get(pId), sizeof(m_aClientChatAccountId));
	if(const json_value *pUsername = json_object_get(pAccount, "username"); pUsername != nullptr && pUsername->type == json_string)
	{
		str_copy(m_aClientChatAccountUsername, json_string_get(pUsername), sizeof(m_aClientChatAccountUsername));
		str_copy(g_Config.m_ClClientChatAccountName, m_aClientChatAccountUsername, sizeof(g_Config.m_ClClientChatAccountName));
		if(m_ClientChatAuthNameInput.IsEmpty())
			m_ClientChatAuthNameInput.Set(m_aClientChatAccountUsername);
	}
	if(m_aClientChatAccountId[0] == '\0')
	{
		str_copy(m_aClientChatStatus, "Client chat account data is invalid.", sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		json_value_free(pJson);
		return;
	}
	if(!ClientChatIdsEqual(aPreviousAccountId, m_aClientChatAccountId))
		LoadClientChatDirectContacts();
	m_ClientChatTransientAuthFailures = 0;
	if(const json_value *pStatus = json_object_get(pAccount, "status"); pStatus != nullptr && pStatus->type == json_string)
		str_copy(m_aClientChatPresenceStatus, json_string_get(pStatus), sizeof(m_aClientChatPresenceStatus));
	if(const json_value *pStatusText = json_object_get(pAccount, "status_text"); pStatusText != nullptr && pStatusText->type == json_string)
		str_copy(m_aClientChatPresenceText, json_string_get(pStatusText), sizeof(m_aClientChatPresenceText));
	if(const json_value *pPrivacy = json_object_get(pAccount, "privacy"); pPrivacy != nullptr && pPrivacy->type == json_object)
	{
		if(const json_value *pWhoCanDm = json_object_get(pPrivacy, "who_can_dm"); pWhoCanDm != nullptr && pWhoCanDm->type == json_string)
			str_copy(m_aClientChatPrivacyWhoCanDm, json_string_get(pWhoCanDm), sizeof(m_aClientChatPrivacyWhoCanDm));
		if(const json_value *pWhoCanAddFriends = json_object_get(pPrivacy, "who_can_add_friends"); pWhoCanAddFriends != nullptr && pWhoCanAddFriends->type == json_string)
			str_copy(m_aClientChatPrivacyWhoCanAddFriends, json_string_get(pWhoCanAddFriends), sizeof(m_aClientChatPrivacyWhoCanAddFriends));
	}

	m_vClientChatFriends.clear();
	if(const json_value *pFriends = json_object_get(pJson, "friends"); pFriends != nullptr && pFriends->type == json_array)
	{
		const int Num = json_array_length(pFriends);
		m_vClientChatFriends.reserve(Num);
		for(int i = 0; i < Num; ++i)
		{
			const json_value *pFriend = json_array_get(pFriends, i);
			if(pFriend == nullptr || pFriend->type != json_object)
				continue;
			CClientChatFriend Friend;
			if(const json_value *pId = json_object_get(pFriend, "id"); pId != nullptr && pId->type == json_string)
				str_copy(Friend.m_aUserId, json_string_get(pId), sizeof(Friend.m_aUserId));
			if(const json_value *pName = json_object_get(pFriend, "username"); pName != nullptr && pName->type == json_string)
				str_copy(Friend.m_aName, json_string_get(pName), sizeof(Friend.m_aName));
			if(const json_value *pStatus = json_object_get(pFriend, "status"); pStatus != nullptr && pStatus->type == json_string)
				str_copy(Friend.m_aStatus, json_string_get(pStatus), sizeof(Friend.m_aStatus));
			if(const json_value *pStatusText = json_object_get(pFriend, "status_text"); pStatusText != nullptr && pStatusText->type == json_string)
				str_copy(Friend.m_aStatusText, json_string_get(pStatusText), sizeof(Friend.m_aStatusText));
			if(const json_value *pCurrentServer = json_object_get(pFriend, "current_server"); pCurrentServer != nullptr && pCurrentServer->type == json_string)
				str_copy(Friend.m_aCurrentServer, json_string_get(pCurrentServer), sizeof(Friend.m_aCurrentServer));
			if(const json_value *pOnline = json_object_get(pFriend, "is_online"); pOnline != nullptr && pOnline->type == json_boolean)
				Friend.m_Online = json_boolean_get(pOnline);
			if(const json_value *pCanJoin = json_object_get(pFriend, "can_join"); pCanJoin != nullptr && pCanJoin->type == json_boolean)
				Friend.m_CanJoin = json_boolean_get(pCanJoin);
			if(Friend.m_aName[0] != '\0')
				m_vClientChatFriends.push_back(Friend);
		}
	}

	m_vClientChatFriendRequestsIn.clear();
	if(const json_value *pRequests = json_object_get(pJson, "friend_requests_in"); pRequests != nullptr && pRequests->type == json_array)
	{
		const int Num = json_array_length(pRequests);
		m_vClientChatFriendRequestsIn.reserve(Num);
		for(int i = 0; i < Num; ++i)
		{
			const json_value *pRequest = json_array_get(pRequests, i);
			if(pRequest == nullptr || pRequest->type != json_object)
				continue;
			CClientChatFriendRequest Request;
			if(const json_value *pReqId = json_object_get(pRequest, "id"); pReqId != nullptr && pReqId->type == json_integer)
				Request.m_Id = json_int_get(pReqId);
			if(const json_value *pFromId = json_object_get(pRequest, "from_id"); pFromId != nullptr && pFromId->type == json_string)
				str_copy(Request.m_aUserId, json_string_get(pFromId), sizeof(Request.m_aUserId));
			if(const json_value *pFromName = json_object_get(pRequest, "from_username"); pFromName != nullptr && pFromName->type == json_string)
				str_copy(Request.m_aName, json_string_get(pFromName), sizeof(Request.m_aName));
			if(Request.m_Id > 0 && Request.m_aName[0] != '\0')
				m_vClientChatFriendRequestsIn.push_back(Request);
		}
	}

	m_vClientChatBlockedUsers.clear();
	if(const json_value *pBlocks = json_object_get(pJson, "blocks"); pBlocks != nullptr && pBlocks->type == json_array)
	{
		const int Num = json_array_length(pBlocks);
		m_vClientChatBlockedUsers.reserve(Num);
		for(int i = 0; i < Num; ++i)
		{
			const json_value *pBlock = json_array_get(pBlocks, i);
			if(pBlock == nullptr || pBlock->type != json_object)
				continue;
			CClientChatBlockedUser Blocked;
			if(const json_value *pBlockedId = json_object_get(pBlock, "user_id"); pBlockedId != nullptr && pBlockedId->type == json_string)
				str_copy(Blocked.m_aUserId, json_string_get(pBlockedId), sizeof(Blocked.m_aUserId));
			if(const json_value *pBlockedName = json_object_get(pBlock, "username"); pBlockedName != nullptr && pBlockedName->type == json_string)
				str_copy(Blocked.m_aName, json_string_get(pBlockedName), sizeof(Blocked.m_aName));
			if(Blocked.m_aName[0] != '\0' || Blocked.m_aUserId[0] != '\0')
				m_vClientChatBlockedUsers.push_back(Blocked);
		}
	}

	const json_value *pRoomMessages = json_object_get(pJson, "room_messages");
	if(pRoomMessages == nullptr)
		pRoomMessages = json_object_get(pJson, "messages");
	const json_value *pDirectMessages = json_object_get(pJson, "direct_messages");
	if((pRoomMessages != nullptr && pRoomMessages->type != json_array) || (pDirectMessages != nullptr && pDirectMessages->type != json_array))
	{
		str_copy(m_aClientChatStatus, Localize("Client chat returned invalid data."), sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		json_value_free(pJson);
		return;
	}

	const char *pLocalNameRaw = Client()->PlayerName();
	const char *pLocalName = pLocalNameRaw != nullptr ? pLocalNameRaw : "";
	const char *pLocalId = m_aClientChatAccountId[0] != '\0' ? m_aClientChatAccountId : (m_aClientChatRuntimeIdentity[0] != '\0' ? m_aClientChatRuntimeIdentity : g_Config.m_ClClientChatIdentity);

	auto IsLocalMessage = [&](const CClientChatMessage &Message) {
		return ClientChatIsLocalMessage(Message.m_aSenderId, Message.m_aSenderInstanceId, Message.m_aName, pLocalId, pLocalName, m_aClientChatRuntimeIdentity, m_aClientChatAccountId[0] != '\0');
	};

	auto ContainsLocalMention = [&](const char *pText) {
		return ClientChatTextContainsMention(pText, pLocalName) ||
			ClientChatTextContainsMention(pText, m_aClientChatAccountUsername);
	};

	auto ParseMessages = [&](const json_value *pArray, std::vector<CClientChatMessage> &vOut, bool DefaultDirect) {
		if(pArray == nullptr)
			return;

		const int NumMessages = json_array_length(pArray);
		vOut.reserve(NumMessages);
		for(int Index = 0; Index < NumMessages; ++Index)
		{
			const json_value *pMessage = json_array_get(pArray, Index);
			if(pMessage == nullptr || pMessage->type != json_object)
				continue;

			CClientChatMessage Message;
			Message.m_Direct = DefaultDirect;
			if(const json_value *pId = json_object_get(pMessage, "id"); pId != nullptr && pId->type == json_integer)
				Message.m_Id = json_int_get(pId);
			if(const json_value *pTimestamp = json_object_get(pMessage, "timestamp"); pTimestamp != nullptr && pTimestamp->type == json_integer)
				Message.m_Timestamp = json_int_get(pTimestamp);
			if(const json_value *pSenderId = json_object_get(pMessage, "sender_id"); pSenderId != nullptr && pSenderId->type == json_string)
				str_copy(Message.m_aSenderId, json_string_get(pSenderId), sizeof(Message.m_aSenderId));
			if(const json_value *pSenderInstanceId = json_object_get(pMessage, "sender_instance_id"); pSenderInstanceId != nullptr && pSenderInstanceId->type == json_string)
				str_copy(Message.m_aSenderInstanceId, json_string_get(pSenderInstanceId), sizeof(Message.m_aSenderInstanceId));
			if(const json_value *pTargetId = json_object_get(pMessage, "target_id"); pTargetId != nullptr && pTargetId->type == json_string)
				str_copy(Message.m_aTargetId, json_string_get(pTargetId), sizeof(Message.m_aTargetId));
			if(const json_value *pName = json_object_get(pMessage, "name"); pName != nullptr && pName->type == json_string)
				str_copy(Message.m_aName, json_string_get(pName), sizeof(Message.m_aName));
			if(const json_value *pTargetName = json_object_get(pMessage, "target_name"); pTargetName != nullptr && pTargetName->type == json_string)
				str_copy(Message.m_aTargetName, json_string_get(pTargetName), sizeof(Message.m_aTargetName));
			if(const json_value *pText = json_object_get(pMessage, "text"); pText != nullptr && pText->type == json_string)
				str_copy(Message.m_aText, json_string_get(pText), sizeof(Message.m_aText));
			if(const json_value *pSkin = json_object_get(pMessage, "skin"); pSkin != nullptr && pSkin->type == json_string)
				str_copy(Message.m_aSkin, json_string_get(pSkin), sizeof(Message.m_aSkin));
			if(const json_value *pUseCustomColor = json_object_get(pMessage, "use_custom_color"); pUseCustomColor != nullptr && pUseCustomColor->type == json_boolean)
				Message.m_UseCustomColor = json_boolean_get(pUseCustomColor);
			if(const json_value *pColorBody = json_object_get(pMessage, "color_body"); pColorBody != nullptr && pColorBody->type == json_integer)
				Message.m_ColorBody = json_int_get(pColorBody);
			if(const json_value *pColorFeet = json_object_get(pMessage, "color_feet"); pColorFeet != nullptr && pColorFeet->type == json_integer)
				Message.m_ColorFeet = json_int_get(pColorFeet);
			if(const json_value *pDirect = json_object_get(pMessage, "direct"); pDirect != nullptr && pDirect->type == json_boolean)
				Message.m_Direct = json_boolean_get(pDirect);
			if(const json_value *pChannel = json_object_get(pMessage, "channel"); pChannel != nullptr && pChannel->type == json_string)
				Message.m_Direct = str_comp(json_string_get(pChannel), "direct") == 0;

			if(Message.m_aName[0] == '\0')
				str_copy(Message.m_aName, "unnamed", sizeof(Message.m_aName));
			if(Message.m_aSkin[0] == '\0')
				str_copy(Message.m_aSkin, "default", sizeof(Message.m_aSkin));
			if(!IsLocalMessage(Message) && IsClientChatBlocked(Message.m_aSenderId, Message.m_aName))
				continue;
			Message.m_MentionsLocal = !Message.m_Direct && !IsLocalMessage(Message) && ContainsLocalMention(Message.m_aText);
			vOut.push_back(Message);
		}
	};

	int PreviousRoomId = m_ClientChatLastRoomId;
	int PreviousDirectId = m_ClientChatLastDirectId;

	std::vector<CClientChatMessage> vRoomMessages;
	std::vector<CClientChatMessage> vDirectMessages;
	ParseMessages(pRoomMessages, vRoomMessages, false);
	ParseMessages(pDirectMessages, vDirectMessages, true);

	m_vClientChatRoomMessages = std::move(vRoomMessages);
	m_vClientChatDirectMessages = std::move(vDirectMessages);
	for(const CClientChatMessage &Message : m_vClientChatDirectMessages)
	{
		if(Message.m_Id <= PreviousDirectId || IsLocalMessage(Message))
			continue;

		EnsureClientChatDirectContact(Message.m_aSenderId, Message.m_aName, Message.m_aSkin, Message.m_UseCustomColor, Message.m_ColorBody, Message.m_ColorFeet);
	}
	RefreshClientChatConversations();

	auto MessageMatchesSelectedDirect = [&](const CClientChatMessage &Message) {
		if(!Message.m_Direct || (m_aClientChatDirectTargetName[0] == '\0' && m_aClientChatDirectTargetId[0] == '\0'))
			return false;
		if(IsLocalMessage(Message))
		{
			if(m_aClientChatDirectTargetId[0] != '\0' && Message.m_aTargetId[0] != '\0')
				return ClientChatIdsEqual(Message.m_aTargetId, m_aClientChatDirectTargetId);
			return ClientChatNamesEqual(Message.m_aTargetName, m_aClientChatDirectTargetName);
		}
		if(m_aClientChatDirectTargetId[0] != '\0' && Message.m_aSenderId[0] != '\0')
			return ClientChatIdsEqual(Message.m_aSenderId, m_aClientChatDirectTargetId);
		return ClientChatNamesEqual(Message.m_aName, m_aClientChatDirectTargetName);
	};

	auto QueueToast = [&](const CClientChatMessage &Message, const char *pVisualTitle, const char *pNotifyTitle) {
		if(g_Config.m_ClClientChatNotifications == 0)
			return;
		CClientChatToast Toast;
		ClientChatCopyOptional(Toast.m_aTitle, sizeof(Toast.m_aTitle), pVisualTitle);
		ClientChatCopyOptional(Toast.m_aText, sizeof(Toast.m_aText), Message.m_aText);
		ClientChatCopyOptional(Toast.m_aName, sizeof(Toast.m_aName), Message.m_aName);
		ClientChatCopyOptional(Toast.m_aSkin, sizeof(Toast.m_aSkin), Message.m_aSkin, "default");
		Toast.m_UseCustomColor = Message.m_UseCustomColor;
		Toast.m_ColorBody = Message.m_ColorBody;
		Toast.m_ColorFeet = Message.m_ColorFeet;
		Toast.m_Direct = Message.m_Direct;
		m_vClientChatToastQueue.push_back(Toast);
		Client()->Notify(pNotifyTitle, Message.m_aText);
	};

	auto IncrementUnread = [&](const CClientChatMessage &Message) {
		for(CClientChatConversation &Conversation : m_vClientChatConversations)
		{
			if((Conversation.m_aUserId[0] != '\0' && Message.m_aSenderId[0] != '\0' && ClientChatIdsEqual(Conversation.m_aUserId, Message.m_aSenderId)) ||
				(Conversation.m_aUserId[0] == '\0' && ClientChatNamesEqual(Conversation.m_aName, Message.m_aName)))
			{
				Conversation.m_UnreadCount++;
				break;
			}
		}
	};

	const bool ViewingRoom = IsClientChatPageActive() && m_aClientChatDirectTargetName[0] == '\0' && m_aClientChatDirectTargetId[0] == '\0';
	if(m_ClientChatHasFetched)
	{
		for(const CClientChatMessage &Message : m_vClientChatRoomMessages)
		{
			if(Message.m_Id <= PreviousRoomId || IsLocalMessage(Message) || !Message.m_MentionsLocal || ViewingRoom)
				continue;

			char aNotifyTitle[96];
			str_format(aNotifyTitle, sizeof(aNotifyTitle), "Ping from %s", Message.m_aName);
			QueueToast(Message, "PING", aNotifyTitle);
		}

		for(const CClientChatMessage &Message : m_vClientChatDirectMessages)
		{
			if(Message.m_Id <= PreviousDirectId || IsLocalMessage(Message))
				continue;

			const CClientChatDirectContact *pContact = FindClientChatDirectContact(Message.m_aSenderId, Message.m_aName);
			if(pContact == nullptr || !pContact->m_Visible || Message.m_Id <= pContact->m_HiddenBeforeId)
				continue;

			const bool ViewingThisConversation = IsClientChatPageActive() && MessageMatchesSelectedDirect(Message);
			if(!ViewingThisConversation)
				IncrementUnread(Message);
			if(!ViewingThisConversation)
			{
				char aNotifyTitle[96];
				str_format(aNotifyTitle, sizeof(aNotifyTitle), "LS from %s", Message.m_aName);
				QueueToast(Message, "LS", aNotifyTitle);
			}
		}
	}

	m_ClientChatLastRoomId = 0;
	for(const CClientChatMessage &Message : m_vClientChatRoomMessages)
		m_ClientChatLastRoomId = maximum(m_ClientChatLastRoomId, Message.m_Id);
	m_ClientChatLastDirectId = 0;
	for(const CClientChatMessage &Message : m_vClientChatDirectMessages)
		m_ClientChatLastDirectId = maximum(m_ClientChatLastDirectId, Message.m_Id);

	if(IsClientChatPageActive() && (m_aClientChatDirectTargetName[0] != '\0' || m_aClientChatDirectTargetId[0] != '\0'))
	{
		for(CClientChatConversation &Conversation : m_vClientChatConversations)
		{
			if((m_aClientChatDirectTargetId[0] != '\0' && ClientChatIdsEqual(Conversation.m_aUserId, m_aClientChatDirectTargetId)) ||
				(m_aClientChatDirectTargetId[0] == '\0' && ClientChatNamesEqual(Conversation.m_aName, m_aClientChatDirectTargetName)))
			{
				Conversation.m_UnreadCount = 0;
				break;
			}
		}
	}

	if(m_vClientChatRoomMessages.empty() && m_vClientChatConversations.empty())
		str_copy(m_aClientChatStatus, Localize("Chat is empty. Send the first message."), sizeof(m_aClientChatStatus));
	else
		m_aClientChatStatus[0] = '\0';
	m_ClientChatStatusError = false;
	m_ClientChatHasFetched = true;

	json_value_free(pJson);
}

void CMenus::ClearClientChatAuthState(bool ClearSavedToken)
{
	if(ClearSavedToken)
		g_Config.m_ClClientChatAuthToken[0] = '\0';

	m_aClientChatAccountId[0] = '\0';
	m_aClientChatAccountUsername[0] = '\0';
	m_ClientChatTransientAuthFailures = 0;
	if(m_pClientChatActionTask)
		m_pClientChatActionTask->Abort();
	m_pClientChatActionTask.reset();
	m_ClientChatInput.Clear();
	m_ClientChatAuthPasswordInput.Clear();
	ActivateClientChatRoom();
	m_vClientChatDirectContacts.clear();
	m_vClientChatRoomMessages.clear();
	m_vClientChatDirectMessages.clear();
	m_vClientChatConversations.clear();
	m_vClientChatFriends.clear();
	m_vClientChatFriendRequestsIn.clear();
	m_vClientChatBlockedUsers.clear();
	m_vClientChatToastQueue.clear();
	m_ClientChatToast.m_Active = false;
	m_ClientChatHasFetched = false;
	m_ClientChatLastRoomId = 0;
	m_ClientChatLastDirectId = 0;
	m_ClientChatLastPollTime = 0ns;
	str_copy(m_aClientChatPresenceStatus, "online", sizeof(m_aClientChatPresenceStatus));
	m_aClientChatPresenceText[0] = '\0';
	str_copy(m_aClientChatPrivacyWhoCanDm, "everyone", sizeof(m_aClientChatPrivacyWhoCanDm));
	str_copy(m_aClientChatPrivacyWhoCanAddFriends, "everyone", sizeof(m_aClientChatPrivacyWhoCanAddFriends));
	m_aClientChatPresenceServer[0] = '\0';
	m_ClientChatPresenceLastSync = 0ns;
}

void CMenus::StartClientChatAuthRequest(EClientChatAuthAction Action)
{
	if(m_pClientChatAuthTask || m_pClientChatFetchTask || m_pClientChatSendTask || g_Config.m_ClClientChatUrl[0] == '\0')
		return;

	const char *pUsername = m_ClientChatAuthNameInput.GetString();
	const char *pPassword = m_ClientChatAuthPasswordInput.GetString();
	if((Action == CLIENT_CHAT_AUTH_LOGIN || Action == CLIENT_CHAT_AUTH_REGISTER) && (pUsername[0] == '\0' || pPassword[0] == '\0'))
	{
		str_copy(m_aClientChatStatus, "Fill in username and password.", sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		return;
	}

	CJsonStringWriter JsonWriter;
	JsonWriter.BeginObject();
	JsonWriter.WriteAttribute("action");
	if(Action == CLIENT_CHAT_AUTH_REGISTER)
		JsonWriter.WriteStrValue("register");
	else if(Action == CLIENT_CHAT_AUTH_LOGOUT)
		JsonWriter.WriteStrValue("logout");
	else
		JsonWriter.WriteStrValue("login");
	if(Action == CLIENT_CHAT_AUTH_LOGIN || Action == CLIENT_CHAT_AUTH_REGISTER)
	{
		JsonWriter.WriteAttribute("username");
		JsonWriter.WriteStrValue(pUsername);
		JsonWriter.WriteAttribute("password");
		JsonWriter.WriteStrValue(pPassword);
	}
	JsonWriter.EndObject();

	std::string JsonBody = JsonWriter.GetOutputString();
	std::shared_ptr<CHttpRequest> pRequest = HttpPostJson(g_Config.m_ClClientChatUrl, JsonBody.c_str());
	pRequest->Timeout(CLIENT_CHAT_TIMEOUT);
	pRequest->LogProgress(HTTPLOG::FAILURE);
	pRequest->FailOnErrorStatus(false);
	pRequest->MaxResponseSize(128 * 1024);
	pRequest->HeaderString(CLIENT_CHAT_HEADER_NAME, CLIENT_CHAT_HEADER_VALUE);
	if(Action == CLIENT_CHAT_AUTH_LOGOUT && g_Config.m_ClClientChatAuthToken[0] != '\0')
		pRequest->HeaderString(CLIENT_CHAT_AUTH_HEADER_NAME, g_Config.m_ClClientChatAuthToken);
	Http()->Run(pRequest);

	m_pClientChatAuthTask = pRequest;
	m_ClientChatAuthAction = Action;
	m_ClientChatStatusError = false;
	if(Action == CLIENT_CHAT_AUTH_REGISTER)
		str_copy(m_aClientChatStatus, "Creating chat account...", sizeof(m_aClientChatStatus));
	else if(Action == CLIENT_CHAT_AUTH_LOGOUT)
		str_copy(m_aClientChatStatus, "Signing out...", sizeof(m_aClientChatStatus));
	else
		str_copy(m_aClientChatStatus, "Signing in...", sizeof(m_aClientChatStatus));
}

void CMenus::HandleClientChatAuthCompletion(const std::shared_ptr<CHttpRequest> &pTask)
{
	if(!pTask)
		return;

	auto SetErrorFromCode = [&](const char *pErrorCode) {
		if(pErrorCode == nullptr || pErrorCode[0] == '\0')
		{
			str_copy(m_aClientChatStatus, "Client chat auth failed.", sizeof(m_aClientChatStatus));
			return;
		}

		if(str_comp(pErrorCode, "username_taken") == 0)
			str_copy(m_aClientChatStatus, "This username is already taken.", sizeof(m_aClientChatStatus));
		else if(str_comp(pErrorCode, "invalid_credentials") == 0)
			str_copy(m_aClientChatStatus, "Wrong username or password.", sizeof(m_aClientChatStatus));
		else if(str_comp(pErrorCode, "invalid_username") == 0)
			str_copy(m_aClientChatStatus, "Username must be 3-24 chars: letters, numbers, _ or -.", sizeof(m_aClientChatStatus));
		else if(str_comp(pErrorCode, "invalid_password") == 0)
			str_copy(m_aClientChatStatus, "Password must be at least 4 characters.", sizeof(m_aClientChatStatus));
		else if(str_comp(pErrorCode, "auth_required") == 0 || str_comp(pErrorCode, "invalid_session") == 0 || str_comp(pErrorCode, "session_expired") == 0)
			str_copy(m_aClientChatStatus, "Session expired. Log in again.", sizeof(m_aClientChatStatus));
		else
			str_copy(m_aClientChatStatus, "Client chat auth failed.", sizeof(m_aClientChatStatus));
	};

	if(pTask->State() != EHttpState::DONE)
	{
		str_copy(m_aClientChatStatus, "Couldn't reach the chat login server.", sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		m_ClientChatAuthAction = CLIENT_CHAT_AUTH_NONE;
		return;
	}

	json_value *pJson = pTask->ResultJson();
	const json_value *pError = pJson != nullptr && pJson->type == json_object ? json_object_get(pJson, "error") : nullptr;
	const char *pErrorCode = pError != nullptr && pError->type == json_string ? json_string_get(pError) : "";

	if(pTask->StatusCode() >= 400)
	{
		if(m_ClientChatAuthAction == CLIENT_CHAT_AUTH_LOGOUT)
			ClearClientChatAuthState(true);
		else if(str_comp(pErrorCode, "auth_required") == 0 || str_comp(pErrorCode, "invalid_session") == 0 || str_comp(pErrorCode, "session_expired") == 0)
			ClearClientChatAuthState(true);

		SetErrorFromCode(pErrorCode);
		m_ClientChatStatusError = true;
		m_ClientChatAuthAction = CLIENT_CHAT_AUTH_NONE;
		if(pJson != nullptr)
			json_value_free(pJson);
		return;
	}

	if(pJson == nullptr || pJson->type != json_object)
	{
		str_copy(m_aClientChatStatus, "Client chat auth returned invalid data.", sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		m_ClientChatAuthAction = CLIENT_CHAT_AUTH_NONE;
		if(pJson != nullptr)
			json_value_free(pJson);
		return;
	}

	if(const json_value *pOk = json_object_get(pJson, "ok"); pOk != nullptr && pOk->type == json_boolean && !json_boolean_get(pOk))
	{
		SetErrorFromCode(pErrorCode);
		m_ClientChatStatusError = true;
		m_ClientChatAuthAction = CLIENT_CHAT_AUTH_NONE;
		json_value_free(pJson);
		return;
	}

	if(m_ClientChatAuthAction == CLIENT_CHAT_AUTH_LOGOUT)
	{
		ClearClientChatAuthState(true);
		str_copy(m_aClientChatStatus, "Signed out from client chat.", sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = false;
		m_ClientChatAuthAction = CLIENT_CHAT_AUTH_NONE;
		json_value_free(pJson);
		return;
	}

	const json_value *pToken = json_object_get(pJson, "session_token");
	const json_value *pAccount = json_object_get(pJson, "account");
	if(pToken == nullptr || pToken->type != json_string || pAccount == nullptr || pAccount->type != json_object)
	{
		str_copy(m_aClientChatStatus, "Client chat auth returned incomplete data.", sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		m_ClientChatAuthAction = CLIENT_CHAT_AUTH_NONE;
		json_value_free(pJson);
		return;
	}

	const json_value *pAccountId = json_object_get(pAccount, "id");
	const json_value *pUsername = json_object_get(pAccount, "username");
	if(pAccountId == nullptr || pAccountId->type != json_string || pUsername == nullptr || pUsername->type != json_string)
	{
		str_copy(m_aClientChatStatus, "Client chat auth returned invalid account data.", sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		m_ClientChatAuthAction = CLIENT_CHAT_AUTH_NONE;
		json_value_free(pJson);
		return;
	}

	str_copy(g_Config.m_ClClientChatAuthToken, json_string_get(pToken), sizeof(g_Config.m_ClClientChatAuthToken));
	str_copy(m_aClientChatAccountId, json_string_get(pAccountId), sizeof(m_aClientChatAccountId));
	str_copy(m_aClientChatAccountUsername, json_string_get(pUsername), sizeof(m_aClientChatAccountUsername));
	str_copy(g_Config.m_ClClientChatAccountName, m_aClientChatAccountUsername, sizeof(g_Config.m_ClClientChatAccountName));
	m_ClientChatAuthNameInput.Set(m_aClientChatAccountUsername);
	m_ClientChatAuthPasswordInput.Clear();
	m_ClientChatTransientAuthFailures = 0;
	LoadClientChatDirectContacts();
	RefreshClientChatConversations();
	m_ClientChatLastPollTime = 0ns;
	m_ClientChatStatusError = false;
	if(m_ClientChatAuthAction == CLIENT_CHAT_AUTH_REGISTER)
		str_format(m_aClientChatStatus, sizeof(m_aClientChatStatus), "Account %s created.", m_aClientChatAccountUsername);
	else
		str_format(m_aClientChatStatus, sizeof(m_aClientChatStatus), "Signed in as %s.", m_aClientChatAccountUsername);
	m_ClientChatAuthAction = CLIENT_CHAT_AUTH_NONE;
	json_value_free(pJson);
	StartClientChatFetch();
}

void CMenus::SendClientChatMessage()
{
	if(m_pClientChatSendTask || g_Config.m_ClClientChatUrl[0] == '\0' || g_Config.m_ClClientChatAuthToken[0] == '\0' || m_aClientChatAccountId[0] == '\0')
		return;

	char aMessage[192];
	str_copy(aMessage, m_ClientChatInput.GetString(), sizeof(aMessage));
	char *pTrimmedMessage = str_skip_whitespaces(aMessage);
	str_utf8_trim_right(pTrimmedMessage);
	if(pTrimmedMessage[0] == '\0')
		return;

	const char *pPlayerName = Client()->PlayerName();
	if(pPlayerName == nullptr || pPlayerName[0] == '\0')
		pPlayerName = "unnamed";
	const bool DirectMessage = m_aClientChatDirectTargetName[0] != '\0' || m_aClientChatDirectTargetId[0] != '\0';
	if(DirectMessage && IsClientChatBlocked(m_aClientChatDirectTargetId, m_aClientChatDirectTargetName))
	{
		str_copy(m_aClientChatStatus, "Unblock this user before sending LS.", sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		return;
	}

	CJsonStringWriter JsonWriter;
	JsonWriter.BeginObject();
	JsonWriter.WriteAttribute("action");
	JsonWriter.WriteStrValue("send");
	JsonWriter.WriteAttribute("name");
	JsonWriter.WriteStrValue(pPlayerName);
	JsonWriter.WriteAttribute("sender_instance_id");
	JsonWriter.WriteStrValue(m_aClientChatRuntimeIdentity);
	JsonWriter.WriteAttribute("text");
	JsonWriter.WriteStrValue(pTrimmedMessage);
	JsonWriter.WriteAttribute("skin");
	JsonWriter.WriteStrValue(g_Config.m_ClPlayerSkin);
	JsonWriter.WriteAttribute("use_custom_color");
	JsonWriter.WriteBoolValue(g_Config.m_ClPlayerUseCustomColor != 0);
	JsonWriter.WriteAttribute("color_body");
	JsonWriter.WriteIntValue(g_Config.m_ClPlayerColorBody);
	JsonWriter.WriteAttribute("color_feet");
	JsonWriter.WriteIntValue(g_Config.m_ClPlayerColorFeet);
	JsonWriter.WriteAttribute("channel");
	JsonWriter.WriteStrValue(DirectMessage ? "direct" : "room");
	if(DirectMessage)
	{
		JsonWriter.WriteAttribute("target_id");
		JsonWriter.WriteStrValue(m_aClientChatDirectTargetId);
		JsonWriter.WriteAttribute("target_name");
		JsonWriter.WriteStrValue(m_aClientChatDirectTargetName);
	}
	JsonWriter.EndObject();

	std::string JsonBody = JsonWriter.GetOutputString();
	std::shared_ptr<CHttpRequest> pRequest = HttpPostJson(g_Config.m_ClClientChatUrl, JsonBody.c_str());
	pRequest->Timeout(CLIENT_CHAT_TIMEOUT);
	pRequest->LogProgress(HTTPLOG::FAILURE);
	pRequest->FailOnErrorStatus(false);
	pRequest->MaxResponseSize(256 * 1024);
	pRequest->HeaderString(CLIENT_CHAT_HEADER_NAME, CLIENT_CHAT_HEADER_VALUE);
	pRequest->HeaderString(CLIENT_CHAT_AUTH_HEADER_NAME, g_Config.m_ClClientChatAuthToken);
	Http()->Run(pRequest);

	m_pClientChatSendTask = pRequest;
	str_copy(m_aClientChatStatus, DirectMessage ? "Sending private message..." : Localize("Sending message..."), sizeof(m_aClientChatStatus));
	m_ClientChatStatusError = false;
}

void CMenus::HandleClientChatSendCompletion(const std::shared_ptr<CHttpRequest> &pTask)
{
	if(!pTask)
		return;

	if(pTask->State() != EHttpState::DONE)
	{
		str_copy(m_aClientChatStatus, Localize("Couldn't send the message."), sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		return;
	}

	if(pTask->StatusCode() >= 400)
	{
		if(pTask->StatusCode() == 401 || pTask->StatusCode() == 403)
		{
			++m_ClientChatTransientAuthFailures;
			if(m_ClientChatTransientAuthFailures < 3 && g_Config.m_ClClientChatAuthToken[0] != '\0')
			{
				m_aClientChatAccountId[0] = '\0';
				str_format(m_aClientChatStatus, sizeof(m_aClientChatStatus), "Reconnecting chat session (%d/3)...", m_ClientChatTransientAuthFailures);
				m_ClientChatStatusError = false;
				m_ClientChatLastPollTime = 0ns;
			}
			else
			{
				ClearClientChatAuthState(true);
				str_copy(m_aClientChatStatus, "Session expired. Log in again.", sizeof(m_aClientChatStatus));
				m_ClientChatStatusError = true;
			}
			return;
		}
		str_format(m_aClientChatStatus, sizeof(m_aClientChatStatus), Localize("Message rejected with %d."), pTask->StatusCode());
		m_ClientChatStatusError = true;
		return;
	}

	m_ClientChatInput.Clear();
	m_aClientChatStatus[0] = '\0';
	m_ClientChatStatusError = false;
	m_ClientChatTransientAuthFailures = 0;
	m_ClientChatLastPollTime = 0ns;
	StartClientChatFetch();
}

void CMenus::StartClientChatActionRequest(const std::string &JsonBody, const char *pPendingStatus)
{
	if(m_pClientChatActionTask || g_Config.m_ClClientChatUrl[0] == '\0' || g_Config.m_ClClientChatAuthToken[0] == '\0' || m_aClientChatAccountId[0] == '\0')
		return;

	std::shared_ptr<CHttpRequest> pRequest = HttpPostJson(g_Config.m_ClClientChatUrl, JsonBody.c_str());
	pRequest->Timeout(CLIENT_CHAT_TIMEOUT);
	pRequest->LogProgress(HTTPLOG::FAILURE);
	pRequest->FailOnErrorStatus(false);
	pRequest->MaxResponseSize(256 * 1024);
	pRequest->HeaderString(CLIENT_CHAT_HEADER_NAME, CLIENT_CHAT_HEADER_VALUE);
	pRequest->HeaderString(CLIENT_CHAT_AUTH_HEADER_NAME, g_Config.m_ClClientChatAuthToken);
	Http()->Run(pRequest);
	m_pClientChatActionTask = pRequest;
	if(pPendingStatus != nullptr)
		str_copy(m_aClientChatStatus, pPendingStatus, sizeof(m_aClientChatStatus));
	m_ClientChatStatusError = false;
}

void CMenus::HandleClientChatActionCompletion(const std::shared_ptr<CHttpRequest> &pTask)
{
	if(!pTask)
		return;

	if(pTask->State() != EHttpState::DONE)
	{
		str_copy(m_aClientChatStatus, "Chat action failed: network error.", sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		return;
	}

	if(pTask->StatusCode() >= 400)
	{
		str_format(m_aClientChatStatus, sizeof(m_aClientChatStatus), "Chat action rejected with %d.", pTask->StatusCode());
		m_ClientChatStatusError = true;
		return;
	}

	json_value *pJson = pTask->ResultJson();
	if(pJson == nullptr || pJson->type != json_object)
	{
		str_copy(m_aClientChatStatus, "Chat action returned invalid data.", sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		if(pJson != nullptr)
			json_value_free(pJson);
		return;
	}

	if(const json_value *pOk = json_object_get(pJson, "ok"); pOk != nullptr && pOk->type == json_boolean && !json_boolean_get(pOk))
	{
		const json_value *pError = json_object_get(pJson, "error");
		if(pError != nullptr && pError->type == json_string)
			str_format(m_aClientChatStatus, sizeof(m_aClientChatStatus), "Action failed: %s", json_string_get(pError));
		else
			str_copy(m_aClientChatStatus, "Chat action failed.", sizeof(m_aClientChatStatus));
		m_ClientChatStatusError = true;
		json_value_free(pJson);
		return;
	}

	m_aClientChatStatus[0] = '\0';
	m_ClientChatStatusError = false;
	m_ClientChatLastPollTime = 0ns;
	json_value_free(pJson);
	StartClientChatFetch();
}

bool CMenus::IsClientChatBlocked(const char *pUserId, const char *pName) const
{
	for(const CClientChatBlockedUser &Blocked : m_vClientChatBlockedUsers)
	{
		if(pUserId != nullptr && pUserId[0] != '\0' && Blocked.m_aUserId[0] != '\0' && ClientChatIdsEqual(Blocked.m_aUserId, pUserId))
			return true;
		if((pUserId == nullptr || pUserId[0] == '\0' || Blocked.m_aUserId[0] == '\0') && pName != nullptr && pName[0] != '\0' && ClientChatNamesEqual(Blocked.m_aName, pName))
			return true;
	}
	return false;
}

void CMenus::ToggleClientChatBlock(const char *pUserId, const char *pName, bool Block)
{
	if((pUserId == nullptr || pUserId[0] == '\0') && (pName == nullptr || pName[0] == '\0'))
		return;

	CJsonStringWriter JsonWriter;
	JsonWriter.BeginObject();
	JsonWriter.WriteAttribute("action");
	JsonWriter.WriteStrValue(Block ? "block_user" : "unblock_user");
	if(pUserId != nullptr && pUserId[0] != '\0')
	{
		JsonWriter.WriteAttribute("target_id");
		JsonWriter.WriteStrValue(pUserId);
	}
	if(pName != nullptr && pName[0] != '\0')
	{
		JsonWriter.WriteAttribute("target_name");
		JsonWriter.WriteStrValue(pName);
	}
	JsonWriter.EndObject();
	StartClientChatActionRequest(JsonWriter.GetOutputString(), Block ? "Blocking user..." : "Removing block...");
}

void CMenus::SendClientChatFriendRequest(const char *pUserId, const char *pName)
{
	if((pUserId == nullptr || pUserId[0] == '\0') && (pName == nullptr || pName[0] == '\0'))
		return;

	CJsonStringWriter JsonWriter;
	JsonWriter.BeginObject();
	JsonWriter.WriteAttribute("action");
	JsonWriter.WriteStrValue("friend_request");
	if(pUserId != nullptr && pUserId[0] != '\0')
	{
		JsonWriter.WriteAttribute("target_id");
		JsonWriter.WriteStrValue(pUserId);
	}
	if(pName != nullptr && pName[0] != '\0')
	{
		JsonWriter.WriteAttribute("target_name");
		JsonWriter.WriteStrValue(pName);
	}
	JsonWriter.EndObject();
	StartClientChatActionRequest(JsonWriter.GetOutputString(), "Sending friend request...");
}

void CMenus::SetClientChatPresenceStatus(const char *pStatus)
{
	if(pStatus == nullptr || pStatus[0] == '\0')
		return;

	CJsonStringWriter JsonWriter;
	JsonWriter.BeginObject();
	JsonWriter.WriteAttribute("action");
	JsonWriter.WriteStrValue("set_status");
	JsonWriter.WriteAttribute("status");
	JsonWriter.WriteStrValue(pStatus);
	JsonWriter.EndObject();
	StartClientChatActionRequest(JsonWriter.GetOutputString(), "Updating status...");
}

void CMenus::UpdateClientChat()
{
	if(m_ClientChatToast.m_Active && time_get_nanoseconds() >= m_ClientChatToast.m_ExpiresAt)
		m_ClientChatToast.m_Active = false;

	if(!m_ClientChatToast.m_Active && !m_vClientChatToastQueue.empty())
	{
		m_ClientChatToast = m_vClientChatToastQueue.front();
		m_vClientChatToastQueue.erase(m_vClientChatToastQueue.begin());
		m_ClientChatToast.m_Active = true;
		m_ClientChatToast.m_ExpiresAt = time_get_nanoseconds() + CLIENT_CHAT_TOAST_DURATION;
	}

	if(m_pClientChatAuthTask && m_pClientChatAuthTask->Done())
	{
		std::shared_ptr<CHttpRequest> pCompletedTask = m_pClientChatAuthTask;
		m_pClientChatAuthTask.reset();
		HandleClientChatAuthCompletion(pCompletedTask);
	}

	if(m_pClientChatSendTask && m_pClientChatSendTask->Done())
	{
		std::shared_ptr<CHttpRequest> pCompletedTask = m_pClientChatSendTask;
		m_pClientChatSendTask.reset();
		HandleClientChatSendCompletion(pCompletedTask);
	}

	if(m_pClientChatFetchTask && m_pClientChatFetchTask->Done())
	{
		std::shared_ptr<CHttpRequest> pCompletedTask = m_pClientChatFetchTask;
		m_pClientChatFetchTask.reset();
		HandleClientChatFetchCompletion(pCompletedTask);
	}

	if(m_pClientChatActionTask && m_pClientChatActionTask->Done())
	{
		std::shared_ptr<CHttpRequest> pCompletedTask = m_pClientChatActionTask;
		m_pClientChatActionTask.reset();
		HandleClientChatActionCompletion(pCompletedTask);
	}

	if(g_Config.m_ClClientChatUrl[0] == '\0')
		return;
	if(Client()->State() != IClient::STATE_OFFLINE && Client()->State() != IClient::STATE_ONLINE)
		return;

	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	const std::chrono::nanoseconds PollInterval = IsClientChatPageActive() ? CLIENT_CHAT_ACTIVE_POLL_INTERVAL : CLIENT_CHAT_BACKGROUND_POLL_INTERVAL;
	if(g_Config.m_ClClientChatAuthToken[0] != '\0' && !m_pClientChatAuthTask && !m_pClientChatFetchTask &&
		(m_aClientChatAccountId[0] == '\0' || m_ClientChatLastPollTime == 0ns || Now - m_ClientChatLastPollTime >= PollInterval))
		StartClientChatFetch();
	if(g_Config.m_ClClientChatAuthToken[0] == '\0' || m_aClientChatAccountId[0] == '\0')
		return;

	if(!m_pClientChatActionTask && !m_pClientChatAuthTask && !m_pClientChatSendTask)
	{
		char aCurrentServer[96] = "";
		if(Client()->State() == IClient::STATE_ONLINE)
			net_addr_str(&Client()->ServerAddress(), aCurrentServer, sizeof(aCurrentServer), true);
		if((m_ClientChatPresenceLastSync == 0ns || Now - m_ClientChatPresenceLastSync >= 8s || str_comp(aCurrentServer, m_aClientChatPresenceServer) != 0))
		{
			CJsonStringWriter JsonWriter;
			JsonWriter.BeginObject();
			JsonWriter.WriteAttribute("action");
			JsonWriter.WriteStrValue("set_presence_server");
			JsonWriter.WriteAttribute("server");
			JsonWriter.WriteStrValue(aCurrentServer);
			JsonWriter.EndObject();
			StartClientChatActionRequest(JsonWriter.GetOutputString(), nullptr);
			str_copy(m_aClientChatPresenceServer, aCurrentServer, sizeof(m_aClientChatPresenceServer));
			m_ClientChatPresenceLastSync = Now;
		}
	}

	if(IsClientChatPageActive() && (m_aClientChatDirectTargetName[0] != '\0' || m_aClientChatDirectTargetId[0] != '\0'))
	{
		for(CClientChatConversation &Conversation : m_vClientChatConversations)
		{
			if((m_aClientChatDirectTargetId[0] != '\0' && ClientChatIdsEqual(Conversation.m_aUserId, m_aClientChatDirectTargetId)) ||
				(m_aClientChatDirectTargetId[0] == '\0' && ClientChatNamesEqual(Conversation.m_aName, m_aClientChatDirectTargetName)))
			{
				Conversation.m_UnreadCount = 0;
				break;
			}
		}
	}
}

void CMenus::RenderClientChatToast()
{
	if(!m_ClientChatToast.m_Active || IsClientChatPageActive())
		return;

	if(time_get_nanoseconds() >= m_ClientChatToast.m_ExpiresAt)
	{
		m_ClientChatToast.m_Active = false;
		return;
	}

	const float ScreenHeight = 300.0f;
	const float ScreenWidth = ScreenHeight * Graphics()->ScreenAspect();
	Graphics()->MapScreen(0.0f, 0.0f, ScreenWidth, ScreenHeight);

	char aPreview[96];
	char aToastName[48];
	ClientChatFormatToastLine(aPreview, sizeof(aPreview), m_ClientChatToast.m_aText, 34);
	ClientChatFormatToastLine(aToastName, sizeof(aToastName), m_ClientChatToast.m_aName, 18);

	CUIRect Toast;
	Toast.w = 118.0f;
	Toast.h = 34.0f;
	Toast.x = ScreenWidth - Toast.w - 10.0f;
	Toast.y = ScreenHeight - Toast.h - 18.0f;
	Toast.Draw(ColorRGBA(0.02f, 0.03f, 0.06f, 0.94f), IGraphics::CORNER_ALL, 8.0f);

	CUIRect Accent;
	Toast.VSplitLeft(3.0f, &Accent, &Toast);
	Accent.Draw(ClientChatAccentColor(m_ClientChatToast.m_UseCustomColor, m_ClientChatToast.m_ColorBody, 0.9f), IGraphics::CORNER_L, 8.0f);
	Toast.Margin(6.0f, &Toast);

	CUIRect Avatar, Content;
	Toast.VSplitLeft(18.0f, &Avatar, &Content);
	Content.VSplitLeft(6.0f, nullptr, &Content);

	CTeeRenderInfo TeeInfo = GetTeeRenderInfo(vec2(16.0f, 16.0f), m_ClientChatToast.m_aSkin, m_ClientChatToast.m_UseCustomColor, m_ClientChatToast.m_ColorBody, m_ClientChatToast.m_ColorFeet);
	RenderTools()->RenderTee(CAnimState::GetIdle(), &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), vec2(Avatar.x + Avatar.w / 2.0f, Avatar.y + Avatar.h / 2.0f));

	CUIRect TopRow, TextRow, Badge, NameRow;
	Content.HSplitTop(11.0f, &TopRow, &Content);
	Content.HSplitTop(3.0f, nullptr, &Content);
	TextRow = Content;

	TopRow.VSplitLeft(20.0f, &Badge, &NameRow);
	NameRow.VSplitLeft(4.0f, nullptr, &NameRow);
	Badge.Draw(m_ClientChatToast.m_Direct ? ColorRGBA(ms_GuiColor.r, ms_GuiColor.g, ms_GuiColor.b, 0.30f) : ColorRGBA(0.93f, 0.70f, 0.16f, 0.30f), IGraphics::CORNER_ALL, 4.0f);
	TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.72f));
	Ui()->DoLabel(&Badge, m_ClientChatToast.m_aTitle, 6.3f, TEXTALIGN_MC);

	TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.95f));
	Ui()->DoLabel(&NameRow, aToastName, 7.2f, TEXTALIGN_ML, {.m_MaxWidth = NameRow.w});
	TextRender()->TextColor(ColorRGBA(0.84f, 0.90f, 1.0f, 0.74f));
	Ui()->DoLabel(&TextRow, aPreview, 6.6f, TEXTALIGN_ML, {.m_MaxWidth = TextRow.w});
	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CMenus::RenderClientChat(CUIRect MainView)
{
	if(Client()->State() == IClient::STATE_OFFLINE)
		GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_SETTINGS_RESERVED0);

	const bool DirectView = m_aClientChatDirectTargetName[0] != '\0' || m_aClientChatDirectTargetId[0] != '\0';
	const bool Authenticated = m_aClientChatAccountId[0] != '\0';
	const bool HasSavedSession = g_Config.m_ClClientChatAuthToken[0] != '\0';
	enum
	{
		CLIENT_CHAT_VIEW_MESSAGES = 0,
		CLIENT_CHAT_VIEW_FRIENDS = 1,
		CLIENT_CHAT_VIEW_SETTINGS = 2,
	};
	static int s_ClientChatView = CLIENT_CHAT_VIEW_MESSAGES;
	if(DirectView)
		s_ClientChatView = CLIENT_CHAT_VIEW_MESSAGES;
	const char *pLocalNameRaw = Client()->PlayerName();
	const char *pLocalName = pLocalNameRaw != nullptr ? pLocalNameRaw : "";
	const char *pLocalId = m_aClientChatAccountId[0] != '\0' ? m_aClientChatAccountId : (m_aClientChatRuntimeIdentity[0] != '\0' ? m_aClientChatRuntimeIdentity : g_Config.m_ClClientChatIdentity);
	const CClientChatDirectContact *pSelectedContact = DirectView ? FindClientChatDirectContact(m_aClientChatDirectTargetId, m_aClientChatDirectTargetName) : nullptr;
	const bool DirectBlocked = DirectView && IsClientChatBlocked(m_aClientChatDirectTargetId, m_aClientChatDirectTargetName);
	bool DirectFriend = false;
	const CClientChatFriend *pDirectFriendInfo = nullptr;
	if(DirectView)
	{
		for(const CClientChatFriend &Friend : m_vClientChatFriends)
		{
			if((m_aClientChatDirectTargetId[0] != '\0' && Friend.m_aUserId[0] != '\0' && ClientChatIdsEqual(Friend.m_aUserId, m_aClientChatDirectTargetId)) ||
				(m_aClientChatDirectTargetId[0] == '\0' && ClientChatNamesEqual(Friend.m_aName, m_aClientChatDirectTargetName)))
			{
				DirectFriend = true;
				pDirectFriendInfo = &Friend;
				break;
			}
		}
	}
	auto FindFriendByConversation = [&](const char *pUserId, const char *pName) -> const CClientChatFriend * {
		for(const CClientChatFriend &Friend : m_vClientChatFriends)
		{
			if((pUserId != nullptr && pUserId[0] != '\0' && Friend.m_aUserId[0] != '\0' && ClientChatIdsEqual(Friend.m_aUserId, pUserId)) ||
				((pUserId == nullptr || pUserId[0] == '\0') && pName != nullptr && pName[0] != '\0' && ClientChatNamesEqual(Friend.m_aName, pName)))
				return &Friend;
		}
		return nullptr;
	};
	auto IsLocalMessage = [&](const CClientChatMessage &Message) {
		return ClientChatIsLocalMessage(Message.m_aSenderId, Message.m_aSenderInstanceId, Message.m_aName, pLocalId, pLocalName, m_aClientChatRuntimeIdentity, m_aClientChatAccountId[0] != '\0');
	};
	auto MessageMatchesDirectTarget = [&](const CClientChatMessage &Message) {
		if(!Message.m_Direct || !DirectView)
			return false;
		if(pSelectedContact != nullptr && (!pSelectedContact->m_Visible || Message.m_Id <= pSelectedContact->m_HiddenBeforeId))
			return false;
		if(IsLocalMessage(Message))
		{
			if(m_aClientChatDirectTargetId[0] != '\0' && Message.m_aTargetId[0] != '\0')
				return ClientChatIdsEqual(Message.m_aTargetId, m_aClientChatDirectTargetId);
			return ClientChatNamesEqual(Message.m_aTargetName, m_aClientChatDirectTargetName);
		}
		if(m_aClientChatDirectTargetId[0] != '\0' && Message.m_aSenderId[0] != '\0')
			return ClientChatIdsEqual(Message.m_aSenderId, m_aClientChatDirectTargetId);
		return ClientChatNamesEqual(Message.m_aName, m_aClientChatDirectTargetName);
	};

	MainView.Draw(ColorRGBA(0.018f, 0.022f, 0.038f, 0.94f), IGraphics::CORNER_B, 14.0f);
	CUIRect AccentBar;
	MainView.HSplitTop(3.0f, &AccentBar, &MainView);
	AccentBar.Draw(ColorRGBA(ms_GuiColor.r, ms_GuiColor.g, ms_GuiColor.b, 0.62f), IGraphics::CORNER_T, 14.0f);
	MainView.Margin(12.0f, &MainView);

	if(!Authenticated)
	{
		CUIRect AuthCard;
		AuthCard.w = minimum(MainView.w, 360.0f);
		AuthCard.h = 210.0f;
		AuthCard.x = MainView.x + (MainView.w - AuthCard.w) / 2.0f;
		AuthCard.y = MainView.y + (MainView.h - AuthCard.h) / 2.0f;
		AuthCard.Draw(ColorRGBA(0.036f, 0.046f, 0.078f, 0.94f), IGraphics::CORNER_ALL, 14.0f);
		AuthCard.Margin(14.0f, &AuthCard);

		CUIRect TitleRow, SubRow, StatusRow, LabelRow, InputRow, ButtonsRow, FooterRow;
		AuthCard.HSplitTop(22.0f, &TitleRow, &AuthCard);
		AuthCard.HSplitTop(6.0f, nullptr, &AuthCard);
		AuthCard.HSplitTop(28.0f, &SubRow, &AuthCard);
		AuthCard.HSplitTop(8.0f, nullptr, &AuthCard);
		StatusRow = AuthCard;
		if(m_aClientChatStatus[0] != '\0')
		{
			AuthCard.HSplitTop(24.0f, &StatusRow, &AuthCard);
			AuthCard.HSplitTop(8.0f, nullptr, &AuthCard);
		}
		CUIRect UserLabel, UserInput, PassLabel, PassInput;
		AuthCard.HSplitTop(12.0f, &UserLabel, &AuthCard);
		AuthCard.HSplitTop(24.0f, &UserInput, &AuthCard);
		AuthCard.HSplitTop(8.0f, nullptr, &AuthCard);
		AuthCard.HSplitTop(12.0f, &PassLabel, &AuthCard);
		AuthCard.HSplitTop(24.0f, &PassInput, &AuthCard);
		AuthCard.HSplitTop(12.0f, nullptr, &AuthCard);
		AuthCard.HSplitTop(28.0f, &ButtonsRow, &AuthCard);
		FooterRow = AuthCard;

		Ui()->DoLabel(&TitleRow, "Client chat account", 17.0f, TEXTALIGN_ML, {.m_MaxWidth = TitleRow.w});
		TextRender()->TextColor(ColorRGBA(0.82f, 0.87f, 1.0f, 0.58f));
		Ui()->DoLabel(&SubRow, "Register once, then log in. Private messages are now tied to your account.", 10.0f, TEXTALIGN_TL, {.m_MaxWidth = SubRow.w});
		TextRender()->TextColor(TextRender()->DefaultTextColor());

		if(m_aClientChatStatus[0] != '\0')
		{
			StatusRow.Draw(m_ClientChatStatusError ? ColorRGBA(0.30f, 0.09f, 0.10f, 0.92f) : ColorRGBA(0.08f, 0.16f, 0.26f, 0.90f), IGraphics::CORNER_ALL, 8.0f);
			StatusRow.Margin(8.0f, &StatusRow);
			TextRender()->TextColor(m_ClientChatStatusError ? ColorRGBA(1.0f, 0.78f, 0.78f, 1.0f) : ColorRGBA(0.84f, 0.92f, 1.0f, 1.0f));
			Ui()->DoLabel(&StatusRow, m_aClientChatStatus, 9.6f, TEXTALIGN_ML, {.m_MaxWidth = StatusRow.w});
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		}

		TextRender()->TextColor(ColorRGBA(0.82f, 0.87f, 1.0f, 0.70f));
		Ui()->DoLabel(&UserLabel, "Username", 9.5f, TEXTALIGN_ML);
		Ui()->DoLabel(&PassLabel, "Password", 9.5f, TEXTALIGN_ML);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
		UserInput.Draw(ColorRGBA(0.02f, 0.03f, 0.05f, 0.70f), IGraphics::CORNER_ALL, 7.0f);
		PassInput.Draw(ColorRGBA(0.02f, 0.03f, 0.05f, 0.70f), IGraphics::CORNER_ALL, 7.0f);
		UserInput.Margin(6.0f, &UserInput);
		PassInput.Margin(6.0f, &PassInput);
		Ui()->DoEditBox(&m_ClientChatAuthNameInput, &UserInput, 10.5f);
		Ui()->DoEditBox(&m_ClientChatAuthPasswordInput, &PassInput, 10.5f);

		const bool AllowAuthButtons = !m_pClientChatAuthTask && !m_pClientChatFetchTask;
		CUIRect LoginButton, RegisterButton, ResetButton;
		ButtonsRow.VSplitMid(&LoginButton, &RegisterButton, 6.0f);
		static CButtonContainer s_LoginButton;
		static CButtonContainer s_RegisterButton;
		if((DoButton_Menu(&s_LoginButton, AllowAuthButtons ? "Log in" : "Please wait", 0, &LoginButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 8.0f, 0.0f, ColorRGBA(ms_GuiColor.r, ms_GuiColor.g, ms_GuiColor.b, 0.34f)) && AllowAuthButtons) ||
			(AllowAuthButtons && m_ClientChatAuthPasswordInput.IsActive() && Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER)))
			StartClientChatAuthRequest(CLIENT_CHAT_AUTH_LOGIN);
		if(DoButton_Menu(&s_RegisterButton, "Register", 0, &RegisterButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 8.0f, 0.0f, ColorRGBA(0.16f, 0.19f, 0.25f, 0.92f)) && AllowAuthButtons)
			StartClientChatAuthRequest(CLIENT_CHAT_AUTH_REGISTER);

		if(HasSavedSession)
		{
			FooterRow.HSplitTop(8.0f, nullptr, &FooterRow);
			FooterRow.HSplitTop(24.0f, &ResetButton, &FooterRow);
			static CButtonContainer s_ResetSessionButton;
			if(DoButton_Menu(&s_ResetSessionButton, "Use another account", 0, &ResetButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 8.0f, 0.0f, ColorRGBA(0.13f, 0.14f, 0.18f, 0.90f)) && !m_pClientChatAuthTask)
			{
				ClearClientChatAuthState(true);
				str_copy(m_aClientChatStatus, "Saved session cleared.", sizeof(m_aClientChatStatus));
				m_ClientChatStatusError = false;
			}
		}

		return;
	}

	CUIRect Header, Content, Composer;
	MainView.HSplitTop(52.0f, &Header, &MainView);
	MainView.HSplitTop(8.0f, nullptr, &MainView);
	MainView.HSplitBottom(62.0f, &Content, &Composer);

	Header.Draw(ColorRGBA(0.042f, 0.052f, 0.085f, 0.92f), IGraphics::CORNER_ALL, 12.0f);
	Header.Margin(10.0f, &Header);
	CUIRect HeaderLeft, HeaderRight;
	Header.VSplitRight(DirectView ? 380.0f : 288.0f, &HeaderLeft, &HeaderRight);
	CUIRect TitleRow, SubtitleRow;
	HeaderLeft.HSplitTop(19.0f, &TitleRow, &HeaderLeft);
	HeaderLeft.HSplitTop(4.0f, nullptr, &HeaderLeft);
	SubtitleRow = HeaderLeft;
	char aPanelTitle[128];
	if(DirectView)
		str_format(aPanelTitle, sizeof(aPanelTitle), "%s %s", FontIcon::COMMENT, m_aClientChatDirectTargetName);
	else
		str_format(aPanelTitle, sizeof(aPanelTitle), "%s %s", FontIcon::COMMENT, Localize("Client chat"));
	Ui()->DoLabel(&TitleRow, aPanelTitle, 16.0f, TEXTALIGN_ML, {.m_MaxWidth = TitleRow.w});
	char aSubtitle[160];
	if(DirectView)
		str_format(aSubtitle, sizeof(aSubtitle), "Private line with %s. Signed in as %s.", m_aClientChatDirectTargetName, m_aClientChatAccountUsername);
	else
		str_format(aSubtitle, sizeof(aSubtitle), "Signed in as %s. Public room for this client.", m_aClientChatAccountUsername);
	TextRender()->TextColor(ColorRGBA(0.82f, 0.87f, 1.0f, 0.56f));
	Ui()->DoLabel(&SubtitleRow, aSubtitle, 9.8f, TEXTALIGN_ML, {.m_MaxWidth = SubtitleRow.w});
	TextRender()->TextColor(TextRender()->DefaultTextColor());

	CUIRect RefreshButton, Badge, BackButton, DeleteButton, LogoutButton, StatusButton, FriendButton, BlockButton, JoinButton;
	DeleteButton = HeaderRight;
	DeleteButton.w = 0.0f;
	BackButton = HeaderRight;
	BackButton.w = 0.0f;
	LogoutButton = HeaderRight;
	LogoutButton.w = 0.0f;
	FriendButton = HeaderRight;
	FriendButton.w = 0.0f;
	BlockButton = HeaderRight;
	BlockButton.w = 0.0f;
	StatusButton = HeaderRight;
	StatusButton.w = 0.0f;
	JoinButton = HeaderRight;
	JoinButton.w = 0.0f;
	HeaderRight.VSplitRight(88.0f, &HeaderRight, &RefreshButton);
	HeaderRight.VSplitRight(8.0f, &HeaderRight, nullptr);
	HeaderRight.VSplitRight(92.0f, &HeaderRight, &StatusButton);
	HeaderRight.VSplitRight(8.0f, &HeaderRight, nullptr);
	HeaderRight.VSplitRight(88.0f, &HeaderRight, &LogoutButton);
	HeaderRight.VSplitRight(8.0f, &HeaderRight, nullptr);
	if(DirectView)
	{
		HeaderRight.VSplitRight(86.0f, &HeaderRight, &JoinButton);
		HeaderRight.VSplitRight(8.0f, &HeaderRight, nullptr);
		HeaderRight.VSplitRight(92.0f, &HeaderRight, &BlockButton);
		HeaderRight.VSplitRight(8.0f, &HeaderRight, nullptr);
		HeaderRight.VSplitRight(92.0f, &HeaderRight, &FriendButton);
		HeaderRight.VSplitRight(8.0f, &HeaderRight, nullptr);
		HeaderRight.VSplitRight(88.0f, &HeaderRight, &DeleteButton);
		HeaderRight.VSplitRight(8.0f, &HeaderRight, nullptr);
		HeaderRight.VSplitRight(72.0f, &HeaderRight, &BackButton);
		HeaderRight.VSplitRight(8.0f, &HeaderRight, nullptr);
	}
	Badge = HeaderRight;
	Badge.HMargin(12.0f, &Badge);
	Badge.Draw(DirectView ? ColorRGBA(ms_GuiColor.r, ms_GuiColor.g, ms_GuiColor.b, 0.18f) : ColorRGBA(0.82f, 0.87f, 1.0f, 0.11f), IGraphics::CORNER_ALL, 6.0f);
	Ui()->DoLabel(&Badge, DirectView ? "LS" : "room", 9.2f, TEXTALIGN_MC);
	if(DirectView)
	{
		static CButtonContainer s_BackToRoomButton;
		if(DoButton_Menu(&s_BackToRoomButton, "Room", 0, &BackButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 7.0f, 0.0f, ColorRGBA(0.12f, 0.16f, 0.22f, 0.96f)))
			ActivateClientChatRoom();

		static CButtonContainer s_DeleteDirectButton;
		if(DoButton_Menu(&s_DeleteDirectButton, "Delete LS", 0, &DeleteButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 7.0f, 0.0f, ColorRGBA(0.25f, 0.10f, 0.11f, 0.92f)))
			DeleteClientChatDirectContact(m_aClientChatDirectTargetId, m_aClientChatDirectTargetName);

		static CButtonContainer s_JoinFriendButton;
		const bool CanJoinFriendServer = pDirectFriendInfo != nullptr && pDirectFriendInfo->m_CanJoin && pDirectFriendInfo->m_aCurrentServer[0] != '\0';
		if(DoButton_Menu(&s_JoinFriendButton, CanJoinFriendServer ? "Join" : "-", 0, &JoinButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 7.0f, 0.0f, CanJoinFriendServer ? ColorRGBA(0.11f, 0.20f, 0.14f, 0.94f) : ColorRGBA(0.09f, 0.11f, 0.14f, 0.90f)))
		{
			if(CanJoinFriendServer)
			{
				char aCmd[160];
				str_format(aCmd, sizeof(aCmd), "connect %s", pDirectFriendInfo->m_aCurrentServer);
				Console()->ExecuteLine(aCmd, IConsole::CLIENT_ID_GAME);
				SetActive(false);
			}
		}

		static CButtonContainer s_FriendRequestButton;
		if(DoButton_Menu(&s_FriendRequestButton, DirectFriend ? "Friend" : "Add friend", 0, &FriendButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 7.0f, 0.0f, ColorRGBA(0.14f, 0.20f, 0.30f, 0.94f)) && !DirectFriend)
			SendClientChatFriendRequest(m_aClientChatDirectTargetId, m_aClientChatDirectTargetName);

		static CButtonContainer s_BlockUserButton;
		if(DoButton_Menu(&s_BlockUserButton, DirectBlocked ? "Unblock" : "Block", 0, &BlockButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 7.0f, 0.0f, DirectBlocked ? ColorRGBA(0.14f, 0.20f, 0.30f, 0.94f) : ColorRGBA(0.27f, 0.10f, 0.11f, 0.94f)))
			ToggleClientChatBlock(m_aClientChatDirectTargetId, m_aClientChatDirectTargetName, !DirectBlocked);
	}

	char aStatusLabel[32];
	const bool StatusOnline = str_comp_nocase(m_aClientChatPresenceStatus, "online") == 0;
	const bool StatusIdle = str_comp_nocase(m_aClientChatPresenceStatus, "idle") == 0;
	const bool StatusDnd = str_comp_nocase(m_aClientChatPresenceStatus, "dnd") == 0;
	if(StatusOnline)
		str_copy(aStatusLabel, "Online", sizeof(aStatusLabel));
	else if(StatusIdle)
		str_copy(aStatusLabel, "Idle", sizeof(aStatusLabel));
	else if(StatusDnd)
		str_copy(aStatusLabel, "DND", sizeof(aStatusLabel));
	else
		str_copy(aStatusLabel, "Invisible", sizeof(aStatusLabel));
	static CButtonContainer s_StatusButton;
	if(DoButton_Menu(&s_StatusButton, aStatusLabel, 0, &StatusButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 7.0f, 0.0f, ColorRGBA(0.12f, 0.16f, 0.23f, 0.96f)))
	{
		if(StatusOnline)
			SetClientChatPresenceStatus("idle");
		else if(StatusIdle)
			SetClientChatPresenceStatus("dnd");
		else if(StatusDnd)
			SetClientChatPresenceStatus("invisible");
		else
			SetClientChatPresenceStatus("online");
	}
	static CButtonContainer s_LogoutButton;
	if(DoButton_Menu(&s_LogoutButton, m_pClientChatAuthTask ? "..." : "Logout", 0, &LogoutButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 7.0f, 0.0f, ColorRGBA(0.14f, 0.14f, 0.18f, 0.94f)) &&
		!m_pClientChatAuthTask && !m_pClientChatSendTask && !m_pClientChatFetchTask && !m_pClientChatActionTask)
		StartClientChatAuthRequest(CLIENT_CHAT_AUTH_LOGOUT);
	static CButtonContainer s_RefreshButton;
	const char *pRefreshLabel = m_pClientChatFetchTask ? Localize("Refreshing") : Localize("Refresh");
	if(DoButton_Menu(&s_RefreshButton, pRefreshLabel, 0, &RefreshButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 7.0f, 0.0f, ColorRGBA(ms_GuiColor.r, ms_GuiColor.g, ms_GuiColor.b, 0.28f)) && !m_pClientChatFetchTask)
	{
		m_ClientChatLastPollTime = 0ns;
		StartClientChatFetch();
	}

	CUIRect Layout = Content;
	CUIRect IconRail, Sidebar, Conversation;
	Layout.VSplitLeft(52.0f, &IconRail, &Layout);
	Layout.VSplitLeft(8.0f, nullptr, &Layout);
	Layout.VSplitLeft(248.0f, &Sidebar, &Layout);
	Layout.VSplitLeft(10.0f, nullptr, &Layout);
	Conversation = Layout;

	IconRail.Draw(ColorRGBA(0.022f, 0.028f, 0.050f, 0.95f), IGraphics::CORNER_ALL, 12.0f);
	IconRail.Margin(6.0f, &IconRail);
	static CButtonContainer s_ViewMessagesIcon;
	static CButtonContainer s_ViewFriendsIcon;
	static CButtonContainer s_ViewSettingsIcon;
	static CButtonContainer s_ViewRoomIcon;
	CUIRect RailButton;
	IconRail.HSplitTop(36.0f, &RailButton, &IconRail);
	if(DoButton_MenuTab(&s_ViewMessagesIcon, FontIcon::COMMENT, !DirectView && s_ClientChatView == CLIENT_CHAT_VIEW_MESSAGES, &RailButton, IGraphics::CORNER_ALL, &m_aAnimatorsSmallPage[SMALL_TAB_SETTINGS]))
	{
		s_ClientChatView = CLIENT_CHAT_VIEW_MESSAGES;
		if(DirectView)
			ActivateClientChatRoom();
	}
	GameClient()->m_Tooltips.DoToolTip(&s_ViewMessagesIcon, &RailButton, Localize("Messages"));

	IconRail.HSplitTop(6.0f, nullptr, &IconRail);
	IconRail.HSplitTop(36.0f, &RailButton, &IconRail);
	if(DoButton_MenuTab(&s_ViewFriendsIcon, FontIcon::ICON_USERS, !DirectView && s_ClientChatView == CLIENT_CHAT_VIEW_FRIENDS, &RailButton, IGraphics::CORNER_ALL, &m_aAnimatorsSmallPage[SMALL_TAB_SETTINGS]))
	{
		s_ClientChatView = CLIENT_CHAT_VIEW_FRIENDS;
		if(DirectView)
			ActivateClientChatRoom();
	}
	GameClient()->m_Tooltips.DoToolTip(&s_ViewFriendsIcon, &RailButton, Localize("Friends"));

	IconRail.HSplitTop(6.0f, nullptr, &IconRail);
	IconRail.HSplitTop(36.0f, &RailButton, &IconRail);
	if(DoButton_MenuTab(&s_ViewSettingsIcon, FontIcon::GEAR, !DirectView && s_ClientChatView == CLIENT_CHAT_VIEW_SETTINGS, &RailButton, IGraphics::CORNER_ALL, &m_aAnimatorsSmallPage[SMALL_TAB_SETTINGS]))
	{
		s_ClientChatView = CLIENT_CHAT_VIEW_SETTINGS;
		if(DirectView)
			ActivateClientChatRoom();
	}
	GameClient()->m_Tooltips.DoToolTip(&s_ViewSettingsIcon, &RailButton, Localize("Settings"));

	IconRail.HSplitBottom(36.0f, &IconRail, &RailButton);
	if(DoButton_MenuTab(&s_ViewRoomIcon, "#", !DirectView, &RailButton, IGraphics::CORNER_ALL, &m_aAnimatorsSmallPage[SMALL_TAB_SETTINGS]))
		ActivateClientChatRoom();
	GameClient()->m_Tooltips.DoToolTip(&s_ViewRoomIcon, &RailButton, Localize("Public room"));

	Sidebar.Draw(ColorRGBA(0.032f, 0.042f, 0.071f, 0.94f), IGraphics::CORNER_ALL, 12.0f);
	Sidebar.Margin(10.0f, &Sidebar);
	CUIRect SidebarTitle, SidebarSubTitle, SidebarList;
	Sidebar.HSplitTop(15.0f, &SidebarTitle, &Sidebar);
	Sidebar.HSplitTop(2.0f, nullptr, &Sidebar);
	Sidebar.HSplitTop(13.0f, &SidebarSubTitle, &Sidebar);
	Sidebar.HSplitTop(7.0f, nullptr, &Sidebar);
	SidebarList = Sidebar;
	Ui()->DoLabel(&SidebarTitle, DirectView ? "Direct messages" : "Conversation list", 11.2f, TEXTALIGN_ML, {.m_MaxWidth = SidebarTitle.w});
	TextRender()->TextColor(ColorRGBA(0.82f, 0.87f, 1.0f, 0.58f));
	Ui()->DoLabel(&SidebarSubTitle, "VoiceChat-style hub layout", 8.8f, TEXTALIGN_ML, {.m_MaxWidth = SidebarSubTitle.w});
	TextRender()->TextColor(TextRender()->DefaultTextColor());

	if(!m_vClientChatFriendRequestsIn.empty())
	{
		CUIRect RequestsHead;
		SidebarList.HSplitTop(14.0f, &RequestsHead, &SidebarList);
		SidebarList.HSplitTop(4.0f, nullptr, &SidebarList);
		char aRequestsTitle[48];
		str_format(aRequestsTitle, sizeof(aRequestsTitle), "Requests (%d)", (int)m_vClientChatFriendRequestsIn.size());
		TextRender()->TextColor(ColorRGBA(0.84f, 0.90f, 1.0f, 0.68f));
		Ui()->DoLabel(&RequestsHead, aRequestsTitle, 9.2f, TEXTALIGN_ML, {.m_MaxWidth = RequestsHead.w});
		TextRender()->TextColor(TextRender()->DefaultTextColor());

		const int MaxRequests = minimum((int)m_vClientChatFriendRequestsIn.size(), 2);
		static std::array<CButtonContainer, 2> s_RequestAccept;
		static std::array<CButtonContainer, 2> s_RequestDecline;
		for(int i = 0; i < MaxRequests; ++i)
		{
			const CClientChatFriendRequest &Request = m_vClientChatFriendRequestsIn[i];
			CUIRect RequestRow;
			SidebarList.HSplitTop(24.0f, &RequestRow, &SidebarList);
			SidebarList.HSplitTop(4.0f, nullptr, &SidebarList);
			CUIRect NamePart, AcceptPart, DeclinePart;
			RequestRow.VSplitRight(22.0f, &RequestRow, &DeclinePart);
			RequestRow.VSplitRight(3.0f, &RequestRow, nullptr);
			RequestRow.VSplitRight(22.0f, &RequestRow, &AcceptPart);
			NamePart = RequestRow;
			NamePart.Draw(ColorRGBA(0.05f, 0.07f, 0.11f, 0.80f), IGraphics::CORNER_ALL, 6.0f);
			NamePart.Margin(5.0f, &NamePart);
			Ui()->DoLabel(&NamePart, Request.m_aName, 8.8f, TEXTALIGN_ML, {.m_MaxWidth = NamePart.w});

			if(DoButton_Menu(&s_RequestAccept[i], "+", 0, &AcceptPart, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 5.0f, 0.0f, ColorRGBA(0.12f, 0.24f, 0.16f, 0.96f)))
			{
				CJsonStringWriter JsonWriter;
				JsonWriter.BeginObject();
				JsonWriter.WriteAttribute("action");
				JsonWriter.WriteStrValue("friend_accept");
				JsonWriter.WriteAttribute("request_id");
				JsonWriter.WriteIntValue(Request.m_Id);
				JsonWriter.EndObject();
				StartClientChatActionRequest(JsonWriter.GetOutputString(), "Accepting friend request...");
			}
			if(DoButton_Menu(&s_RequestDecline[i], "-", 0, &DeclinePart, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 5.0f, 0.0f, ColorRGBA(0.23f, 0.10f, 0.10f, 0.96f)))
			{
				CJsonStringWriter JsonWriter;
				JsonWriter.BeginObject();
				JsonWriter.WriteAttribute("action");
				JsonWriter.WriteStrValue("friend_decline");
				JsonWriter.WriteAttribute("request_id");
				JsonWriter.WriteIntValue(Request.m_Id);
				JsonWriter.EndObject();
				StartClientChatActionRequest(JsonWriter.GetOutputString(), "Declining friend request...");
			}
		}
		SidebarList.HSplitTop(6.0f, nullptr, &SidebarList);
	}

	CUIRect PrivacyHead;
	SidebarList.HSplitTop(12.0f, &PrivacyHead, &SidebarList);
	SidebarList.HSplitTop(4.0f, nullptr, &SidebarList);
	TextRender()->TextColor(ColorRGBA(0.84f, 0.90f, 1.0f, 0.62f));
	Ui()->DoLabel(&PrivacyHead, "Privacy", 8.7f, TEXTALIGN_ML, {.m_MaxWidth = PrivacyHead.w});
	TextRender()->TextColor(TextRender()->DefaultTextColor());
	CUIRect DmPrivacyRow;
	SidebarList.HSplitTop(20.0f, &DmPrivacyRow, &SidebarList);
	SidebarList.HSplitTop(3.0f, nullptr, &SidebarList);
	CUIRect FriendPrivacyRow;
	SidebarList.HSplitTop(20.0f, &FriendPrivacyRow, &SidebarList);
	SidebarList.HSplitTop(6.0f, nullptr, &SidebarList);
	static CButtonContainer s_DmPrivacyButton;
	static CButtonContainer s_AddFriendsPrivacyButton;
	char aDmPrivacy[64];
	str_format(aDmPrivacy, sizeof(aDmPrivacy), "DM: %s", m_aClientChatPrivacyWhoCanDm);
	char aAddPrivacy[64];
	str_format(aAddPrivacy, sizeof(aAddPrivacy), "Add: %s", m_aClientChatPrivacyWhoCanAddFriends);
	if(DoButton_Menu(&s_DmPrivacyButton, aDmPrivacy, 0, &DmPrivacyRow, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 6.0f, 0.0f, ColorRGBA(0.08f, 0.12f, 0.18f, 0.92f)))
	{
		const char *pNext = "everyone";
		if(str_comp(m_aClientChatPrivacyWhoCanDm, "everyone") == 0)
			pNext = "friends";
		else if(str_comp(m_aClientChatPrivacyWhoCanDm, "friends") == 0)
			pNext = "no_one";
		CJsonStringWriter JsonWriter;
		JsonWriter.BeginObject();
		JsonWriter.WriteAttribute("action");
		JsonWriter.WriteStrValue("set_privacy");
		JsonWriter.WriteAttribute("who_can_dm");
		JsonWriter.WriteStrValue(pNext);
		JsonWriter.EndObject();
		StartClientChatActionRequest(JsonWriter.GetOutputString(), "Updating DM privacy...");
	}
	if(DoButton_Menu(&s_AddFriendsPrivacyButton, aAddPrivacy, 0, &FriendPrivacyRow, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 6.0f, 0.0f, ColorRGBA(0.08f, 0.12f, 0.18f, 0.92f)))
	{
		const char *pNext = "everyone";
		if(str_comp(m_aClientChatPrivacyWhoCanAddFriends, "everyone") == 0)
			pNext = "friends_of_friends";
		else if(str_comp(m_aClientChatPrivacyWhoCanAddFriends, "friends_of_friends") == 0)
			pNext = "no_one";
		CJsonStringWriter JsonWriter;
		JsonWriter.BeginObject();
		JsonWriter.WriteAttribute("action");
		JsonWriter.WriteStrValue("set_privacy");
		JsonWriter.WriteAttribute("who_can_add_friends");
		JsonWriter.WriteStrValue(pNext);
		JsonWriter.EndObject();
		StartClientChatActionRequest(JsonWriter.GetOutputString(), "Updating friend privacy...");
	}

	static CButtonContainer s_RoomButton;
	CUIRect RoomRow;
	SidebarList.HSplitTop(40.0f, &RoomRow, &SidebarList);
	SidebarList.HSplitTop(5.0f, nullptr, &SidebarList);
	const bool RoomSelected = !DirectView;
	RoomRow.Draw(RoomSelected ? ColorRGBA(0.10f, 0.17f, 0.28f, 0.96f) : ColorRGBA(0.055f, 0.068f, 0.10f, Ui()->MouseInside(&RoomRow) ? 0.90f : 0.76f), IGraphics::CORNER_ALL, 9.0f);
	if(Ui()->DoButtonLogic(&s_RoomButton, 0, &RoomRow, BUTTONFLAG_LEFT))
		ActivateClientChatRoom();
	CUIRect RoomIcon, RoomInfo;
	RoomRow.Margin(8.0f, &RoomRow);
	RoomRow.VSplitLeft(24.0f, &RoomIcon, &RoomInfo);
	RoomInfo.VSplitLeft(7.0f, nullptr, &RoomInfo);
	Ui()->DoLabel(&RoomIcon, "#", 15.0f, TEXTALIGN_MC);
	CUIRect RoomName, RoomPreview;
	RoomInfo.HSplitTop(14.0f, &RoomName, &RoomInfo);
	RoomInfo.HSplitTop(2.0f, nullptr, &RoomInfo);
	RoomPreview = RoomInfo;
	Ui()->DoLabel(&RoomName, "Public room", 11.0f, TEXTALIGN_ML, {.m_MaxWidth = RoomName.w});
	TextRender()->TextColor(ColorRGBA(0.82f, 0.87f, 1.0f, 0.54f));
	Ui()->DoLabel(&RoomPreview, "@nickname pings people", 8.8f, TEXTALIGN_ML, {.m_MaxWidth = RoomPreview.w});
	TextRender()->TextColor(TextRender()->DefaultTextColor());

	static std::array<CButtonContainer, CLIENT_CHAT_MAX_VISIBLE_ITEMS> s_ConversationButtons;
	const int VisibleConversations = minimum((int)m_vClientChatConversations.size(), CLIENT_CHAT_MAX_VISIBLE_ITEMS);
	for(int Index = 0; Index < VisibleConversations; ++Index)
	{
		const CClientChatConversation &ConversationData = m_vClientChatConversations[Index];
		CUIRect Row;
		SidebarList.HSplitTop(42.0f, &Row, &SidebarList);
		SidebarList.HSplitTop(5.0f, nullptr, &SidebarList);
		const bool Selected = DirectView && ((m_aClientChatDirectTargetId[0] != '\0' && ClientChatIdsEqual(ConversationData.m_aUserId, m_aClientChatDirectTargetId)) ||
												(m_aClientChatDirectTargetId[0] == '\0' && ClientChatNamesEqual(ConversationData.m_aName, m_aClientChatDirectTargetName)));
		Row.Draw(Selected ? ColorRGBA(0.12f, 0.19f, 0.31f, 0.97f) : ColorRGBA(0.055f, 0.07f, 0.11f, Ui()->MouseInside(&Row) ? 0.90f : 0.76f), IGraphics::CORNER_ALL, 9.0f);
		if(Ui()->DoButtonLogic(&s_ConversationButtons[Index], 0, &Row, BUTTONFLAG_LEFT))
			ActivateClientChatDirect(ConversationData.m_aUserId, ConversationData.m_aName, ConversationData.m_aSkin, ConversationData.m_UseCustomColor, ConversationData.m_ColorBody, ConversationData.m_ColorFeet);
		CUIRect Avatar, Info, UnreadBadge;
		Row.Margin(8.0f, &Row);
		Row.VSplitLeft(24.0f, &Avatar, &Info);
		Info.VSplitLeft(7.0f, nullptr, &Info);
		const CClientChatFriend *pFriendInfo = FindFriendByConversation(ConversationData.m_aUserId, ConversationData.m_aName);
		const bool VisibleOnline = pFriendInfo != nullptr ? pFriendInfo->m_Online : false;
		const char *pVisibleStatus = pFriendInfo != nullptr ? pFriendInfo->m_aStatus : "offline";
		CUIRect StatusIndicator;
		Info.VSplitRight(14.0f, &Info, &StatusIndicator);
		Info.VSplitRight(3.0f, &Info, nullptr);
		if(ConversationData.m_UnreadCount > 0)
		{
			Info.VSplitRight(18.0f, &Info, &UnreadBadge);
			UnreadBadge.HMargin(11.0f, &UnreadBadge);
			UnreadBadge.Draw(ColorRGBA(ms_GuiColor.r, ms_GuiColor.g, ms_GuiColor.b, 0.84f), IGraphics::CORNER_ALL, 6.0f);
			char aUnread[8];
			str_format(aUnread, sizeof(aUnread), "%d", ConversationData.m_UnreadCount);
			Ui()->DoLabel(&UnreadBadge, aUnread, 9.0f, TEXTALIGN_MC);
		}
		CTeeRenderInfo TeeInfo = GetTeeRenderInfo(vec2(20.0f, 20.0f), ConversationData.m_aSkin, ConversationData.m_UseCustomColor, ConversationData.m_ColorBody, ConversationData.m_ColorFeet);
		RenderTools()->RenderTee(CAnimState::GetIdle(), &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), vec2(Avatar.x + Avatar.w / 2.0f, Avatar.y + Avatar.h / 2.0f + 0.5f));
		TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
		TextRender()->TextColor(ClientChatStatusColorFor(pVisibleStatus, VisibleOnline));
		Ui()->DoLabel(&StatusIndicator, ClientChatStatusIconFor(pVisibleStatus, VisibleOnline), 8.5f, TEXTALIGN_MC);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
		TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
		CUIRect NameRow, PreviewRow;
		Info.HSplitTop(14.0f, &NameRow, &Info);
		Info.HSplitTop(2.0f, nullptr, &Info);
		PreviewRow = Info;
		Ui()->DoLabel(&NameRow, ConversationData.m_aName, 10.8f, TEXTALIGN_ML, {.m_MaxWidth = NameRow.w});
		TextRender()->TextColor(ColorRGBA(0.82f, 0.87f, 1.0f, 0.54f));
		Ui()->DoLabel(&PreviewRow, ConversationData.m_aPreview, 8.6f, TEXTALIGN_ML, {.m_MaxWidth = PreviewRow.w});
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}

	if(VisibleConversations == 0)
	{
		CUIRect EmptyDirects = SidebarList;
		EmptyDirects.Margin(6.0f, &EmptyDirects);
		TextRender()->TextColor(ColorRGBA(0.82f, 0.87f, 1.0f, 0.46f));
		Ui()->DoLabel(&EmptyDirects, "LS list is empty.\nPress LS on a room message.", 10.0f, TEXTALIGN_TL, {.m_MaxWidth = EmptyDirects.w});
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}

	Conversation.Draw(ColorRGBA(0.028f, 0.036f, 0.062f, 0.93f), IGraphics::CORNER_ALL, 12.0f);
	Conversation.Margin(10.0f, &Conversation);
	CUIRect ConversationHeader, MessageArea;
	Conversation.HSplitTop(42.0f, &ConversationHeader, &Conversation);
	Conversation.HSplitTop(8.0f, nullptr, &Conversation);
	MessageArea = Conversation;
	ConversationHeader.Draw(ColorRGBA(0.040f, 0.050f, 0.082f, 0.92f), IGraphics::CORNER_ALL, 10.0f);
	ConversationHeader.Margin(9.0f, &ConversationHeader);
	CUIRect HeaderAvatar, HeaderText, ActiveTitle, ActiveInfo;
	if(DirectView)
	{
		ConversationHeader.VSplitLeft(30.0f, &HeaderAvatar, &HeaderText);
		HeaderText.VSplitLeft(8.0f, nullptr, &HeaderText);
		CTeeRenderInfo HeaderTeeInfo = GetTeeRenderInfo(vec2(22.0f, 22.0f), m_aClientChatDirectTargetSkin, m_ClientChatDirectTargetUseCustomColor, m_ClientChatDirectTargetColorBody, m_ClientChatDirectTargetColorFeet);
		RenderTools()->RenderTee(CAnimState::GetIdle(), &HeaderTeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), vec2(HeaderAvatar.x + HeaderAvatar.w / 2.0f, HeaderAvatar.y + HeaderAvatar.h / 2.0f));
	}
	else
	{
		HeaderText = ConversationHeader;
	}
	HeaderText.HSplitTop(16.0f, &ActiveTitle, &HeaderText);
	HeaderText.HSplitTop(3.0f, nullptr, &HeaderText);
	ActiveInfo = HeaderText;
	Ui()->DoLabel(&ActiveTitle, DirectView ? m_aClientChatDirectTargetName : "Public room", 13.0f, TEXTALIGN_ML, {.m_MaxWidth = ActiveTitle.w});
	TextRender()->TextColor(ColorRGBA(0.82f, 0.87f, 1.0f, 0.58f));
	if(DirectView && pDirectFriendInfo != nullptr)
	{
		char aPresenceInfo[160];
		if(pDirectFriendInfo->m_aCurrentServer[0] != '\0' && pDirectFriendInfo->m_CanJoin)
			str_format(aPresenceInfo, sizeof(aPresenceInfo), "%s • playing on %s", pDirectFriendInfo->m_aStatus, pDirectFriendInfo->m_aCurrentServer);
		else
			str_format(aPresenceInfo, sizeof(aPresenceInfo), "%s", pDirectFriendInfo->m_aStatus);
		Ui()->DoLabel(&ActiveInfo, aPresenceInfo, 9.2f, TEXTALIGN_ML, {.m_MaxWidth = ActiveInfo.w});
	}
	else if(!DirectView && s_ClientChatView == CLIENT_CHAT_VIEW_FRIENDS)
		Ui()->DoLabel(&ActiveInfo, "Friends and incoming requests", 9.2f, TEXTALIGN_ML, {.m_MaxWidth = ActiveInfo.w});
	else if(!DirectView && s_ClientChatView == CLIENT_CHAT_VIEW_SETTINGS)
		Ui()->DoLabel(&ActiveInfo, "Integrated chat settings (VoiceChat style)", 9.2f, TEXTALIGN_ML, {.m_MaxWidth = ActiveInfo.w});
	else
		Ui()->DoLabel(&ActiveInfo, DirectView ? "Local LS dialog" : "Everybody using this client can read this room", 9.2f, TEXTALIGN_ML, {.m_MaxWidth = ActiveInfo.w});
	TextRender()->TextColor(TextRender()->DefaultTextColor());

	if(m_aClientChatStatus[0] != '\0')
	{
		CUIRect StatusRow;
		MessageArea.HSplitTop(24.0f, &StatusRow, &MessageArea);
		MessageArea.HSplitTop(8.0f, nullptr, &MessageArea);
		StatusRow.Draw(m_ClientChatStatusError ? ColorRGBA(0.28f, 0.08f, 0.08f, 0.92f) : ColorRGBA(0.08f, 0.16f, 0.26f, 0.90f), IGraphics::CORNER_ALL, 8.0f);
		StatusRow.Margin(8.0f, &StatusRow);
		TextRender()->TextColor(m_ClientChatStatusError ? ColorRGBA(1.0f, 0.74f, 0.74f, 1.0f) : ColorRGBA(0.80f, 0.90f, 1.0f, 1.0f));
		Ui()->DoLabel(&StatusRow, m_aClientChatStatus, 10.5f, TEXTALIGN_ML, {.m_MaxWidth = StatusRow.w});
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}

	const bool FriendsView = !DirectView && s_ClientChatView == CLIENT_CHAT_VIEW_FRIENDS;
	const bool SettingsView = !DirectView && s_ClientChatView == CLIENT_CHAT_VIEW_SETTINGS;
	if(FriendsView)
	{
		CUIRect List = MessageArea;
		const int MaxRows = minimum((int)m_vClientChatFriends.size(), CLIENT_CHAT_MAX_VISIBLE_ITEMS);
		if(MaxRows == 0)
		{
			CUIRect EmptyState = List;
			EmptyState.Draw(ColorRGBA(0.016f, 0.02f, 0.036f, 0.64f), IGraphics::CORNER_ALL, 10.0f);
			EmptyState.Margin(16.0f, &EmptyState);
			Ui()->DoLabel(&EmptyState, "No friends yet. Add players from room/LS actions.", 14.0f, TEXTALIGN_MC, {.m_MaxWidth = EmptyState.w});
		}
		else
		{
			static std::array<CButtonContainer, CLIENT_CHAT_MAX_VISIBLE_ITEMS> s_FriendOpenDm;
			static std::array<CButtonContainer, CLIENT_CHAT_MAX_VISIBLE_ITEMS> s_FriendJoin;
			static std::array<CButtonContainer, CLIENT_CHAT_MAX_VISIBLE_ITEMS> s_FriendRemove;
			for(int i = 0; i < MaxRows; ++i)
			{
				const CClientChatFriend &Friend = m_vClientChatFriends[i];
				CUIRect Row;
				List.HSplitTop(42.0f, &Row, &List);
				List.HSplitTop(6.0f, nullptr, &List);
				Row.Draw(ColorRGBA(0.05f, 0.07f, 0.11f, 0.90f), IGraphics::CORNER_ALL, 8.0f);
				Row.Margin(8.0f, &Row);
				CUIRect NamePart, DmButton, JoinButton, RemoveButton, StatusBadge;
				Row.VSplitRight(12.0f, &Row, &StatusBadge);
				Row.VSplitRight(3.0f, &Row, nullptr);
				Row.VSplitRight(74.0f, &Row, &RemoveButton);
				Row.VSplitRight(6.0f, &Row, nullptr);
				Row.VSplitRight(56.0f, &Row, &JoinButton);
				Row.VSplitRight(6.0f, &Row, nullptr);
				Row.VSplitRight(60.0f, &Row, &DmButton);
				NamePart = Row;
				char aFriendTitle[192];
				if(Friend.m_aCurrentServer[0] != '\0')
					str_format(aFriendTitle, sizeof(aFriendTitle), "%s • %s", Friend.m_aName, Friend.m_aCurrentServer);
				else
					str_format(aFriendTitle, sizeof(aFriendTitle), "%s", Friend.m_aName);
				Ui()->DoLabel(&NamePart, aFriendTitle, 10.4f, TEXTALIGN_ML, {.m_MaxWidth = NamePart.w});
				if(DoButton_Menu(&s_FriendOpenDm[i], "LS", 0, &DmButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 6.0f, 0.0f, ColorRGBA(0.12f, 0.18f, 0.26f, 0.95f)))
					ActivateClientChatDirect(Friend.m_aUserId, Friend.m_aName, "default", false, 65408, 65408);
				if(DoButton_Menu(&s_FriendJoin[i], Friend.m_CanJoin && Friend.m_aCurrentServer[0] != '\0' ? "Join" : "-", 0, &JoinButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 6.0f, 0.0f, Friend.m_CanJoin && Friend.m_aCurrentServer[0] != '\0' ? ColorRGBA(0.10f, 0.20f, 0.14f, 0.95f) : ColorRGBA(0.08f, 0.10f, 0.13f, 0.90f)))
				{
					if(Friend.m_CanJoin && Friend.m_aCurrentServer[0] != '\0')
					{
						char aCmd[160];
						str_format(aCmd, sizeof(aCmd), "connect %s", Friend.m_aCurrentServer);
						Console()->ExecuteLine(aCmd, IConsole::CLIENT_ID_GAME);
						SetActive(false);
					}
				}
				if(DoButton_Menu(&s_FriendRemove[i], "Remove", 0, &RemoveButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 6.0f, 0.0f, ColorRGBA(0.23f, 0.10f, 0.10f, 0.95f)))
				{
					CJsonStringWriter JsonWriter;
					JsonWriter.BeginObject();
					JsonWriter.WriteAttribute("action");
					JsonWriter.WriteStrValue("friend_remove");
					JsonWriter.WriteAttribute("target_id");
					JsonWriter.WriteStrValue(Friend.m_aUserId);
					JsonWriter.EndObject();
					StartClientChatActionRequest(JsonWriter.GetOutputString(), "Removing friend...");
				}
				TextRender()->SetFontPreset(EFontPreset::ICON_FONT);
				TextRender()->TextColor(ClientChatStatusColorFor(Friend.m_aStatus, Friend.m_Online));
				Ui()->DoLabel(&StatusBadge, ClientChatStatusIconFor(Friend.m_aStatus, Friend.m_Online), 8.5f, TEXTALIGN_MC);
				TextRender()->TextColor(TextRender()->DefaultTextColor());
				TextRender()->SetFontPreset(EFontPreset::DEFAULT_FONT);
			}
		}
	}
	else if(SettingsView)
	{
		CUIRect SettingsWrap = MessageArea;
		SettingsWrap.Draw(ColorRGBA(0.016f, 0.022f, 0.037f, 0.78f), IGraphics::CORNER_ALL, 10.0f);
		SettingsWrap.Margin(12.0f, &SettingsWrap);
		CUIRect Row;

		SettingsWrap.HSplitTop(22.0f, &Row, &SettingsWrap);
		Ui()->DoLabel(&Row, "Chat Notifications", 11.0f, TEXTALIGN_ML, {.m_MaxWidth = Row.w});
		SettingsWrap.HSplitTop(26.0f, &Row, &SettingsWrap);
		static CButtonContainer s_NotificationsToggle;
		if(DoButton_Menu(&s_NotificationsToggle, g_Config.m_ClClientChatNotifications ? "Enabled" : "Disabled", 0, &Row, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 7.0f, 0.0f, ColorRGBA(ms_GuiColor.r, ms_GuiColor.g, ms_GuiColor.b, 0.30f)))
			g_Config.m_ClClientChatNotifications ^= 1;

		SettingsWrap.HSplitTop(8.0f, nullptr, &SettingsWrap);
		SettingsWrap.HSplitTop(22.0f, &Row, &SettingsWrap);
		Ui()->DoLabel(&Row, "Chat Open Bind", 11.0f, TEXTALIGN_ML, {.m_MaxWidth = Row.w});
		SettingsWrap.HSplitTop(26.0f, &Row, &SettingsWrap);
		char aBind[64];
		GameClient()->m_Binds.GetKey("toggle_client_chat", aBind, sizeof(aBind));
		if(aBind[0] == '\0')
			str_copy(aBind, "Not set", sizeof(aBind));
		char aBindLine[96];
		str_format(aBindLine, sizeof(aBindLine), "toggle_client_chat: %s", aBind);
		Ui()->DoLabel(&Row, aBindLine, 10.0f, TEXTALIGN_ML, {.m_MaxWidth = Row.w});

		SettingsWrap.HSplitTop(8.0f, nullptr, &SettingsWrap);
		SettingsWrap.HSplitTop(22.0f, &Row, &SettingsWrap);
		Ui()->DoLabel(&Row, "Presence Status", 11.0f, TEXTALIGN_ML, {.m_MaxWidth = Row.w});
		SettingsWrap.HSplitTop(26.0f, &Row, &SettingsWrap);
		static CButtonContainer s_StatusCycleInSettings;
		char aStatusLabel[48];
		str_format(aStatusLabel, sizeof(aStatusLabel), "Now: %s", m_aClientChatPresenceStatus);
		if(DoButton_Menu(&s_StatusCycleInSettings, aStatusLabel, 0, &Row, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 7.0f, 0.0f, ColorRGBA(0.10f, 0.15f, 0.22f, 0.95f)))
		{
			if(str_comp_nocase(m_aClientChatPresenceStatus, "online") == 0)
				SetClientChatPresenceStatus("idle");
			else if(str_comp_nocase(m_aClientChatPresenceStatus, "idle") == 0)
				SetClientChatPresenceStatus("dnd");
			else if(str_comp_nocase(m_aClientChatPresenceStatus, "dnd") == 0)
				SetClientChatPresenceStatus("invisible");
			else
				SetClientChatPresenceStatus("online");
		}

		SettingsWrap.HSplitTop(8.0f, nullptr, &SettingsWrap);
		SettingsWrap.HSplitTop(22.0f, &Row, &SettingsWrap);
		Ui()->DoLabel(&Row, "Privacy", 11.0f, TEXTALIGN_ML, {.m_MaxWidth = Row.w});
		SettingsWrap.HSplitTop(26.0f, &Row, &SettingsWrap);
		static CButtonContainer s_DmPrivacyInSettings;
		char aDmPrivacy[64];
		str_format(aDmPrivacy, sizeof(aDmPrivacy), "Who can DM: %s", m_aClientChatPrivacyWhoCanDm);
		if(DoButton_Menu(&s_DmPrivacyInSettings, aDmPrivacy, 0, &Row, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 7.0f, 0.0f, ColorRGBA(0.10f, 0.15f, 0.22f, 0.95f)))
		{
			const char *pNext = "everyone";
			if(str_comp(m_aClientChatPrivacyWhoCanDm, "everyone") == 0)
				pNext = "friends";
			else if(str_comp(m_aClientChatPrivacyWhoCanDm, "friends") == 0)
				pNext = "no_one";
			CJsonStringWriter JsonWriter;
			JsonWriter.BeginObject();
			JsonWriter.WriteAttribute("action");
			JsonWriter.WriteStrValue("set_privacy");
			JsonWriter.WriteAttribute("who_can_dm");
			JsonWriter.WriteStrValue(pNext);
			JsonWriter.EndObject();
			StartClientChatActionRequest(JsonWriter.GetOutputString(), "Updating DM privacy...");
		}
		SettingsWrap.HSplitTop(4.0f, nullptr, &SettingsWrap);
		SettingsWrap.HSplitTop(26.0f, &Row, &SettingsWrap);
		static CButtonContainer s_AddFriendsPrivacyInSettings;
		char aAddPrivacy[72];
		str_format(aAddPrivacy, sizeof(aAddPrivacy), "Who can add friends: %s", m_aClientChatPrivacyWhoCanAddFriends);
		if(DoButton_Menu(&s_AddFriendsPrivacyInSettings, aAddPrivacy, 0, &Row, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 7.0f, 0.0f, ColorRGBA(0.10f, 0.15f, 0.22f, 0.95f)))
		{
			const char *pNext = "everyone";
			if(str_comp(m_aClientChatPrivacyWhoCanAddFriends, "everyone") == 0)
				pNext = "friends_of_friends";
			else if(str_comp(m_aClientChatPrivacyWhoCanAddFriends, "friends_of_friends") == 0)
				pNext = "no_one";
			CJsonStringWriter JsonWriter;
			JsonWriter.BeginObject();
			JsonWriter.WriteAttribute("action");
			JsonWriter.WriteStrValue("set_privacy");
			JsonWriter.WriteAttribute("who_can_add_friends");
			JsonWriter.WriteStrValue(pNext);
			JsonWriter.EndObject();
			StartClientChatActionRequest(JsonWriter.GetOutputString(), "Updating friend privacy...");
		}
	}
	else
	{
		std::vector<const CClientChatMessage *> vpMessages;
		if(DirectView)
		{
			for(const CClientChatMessage &Message : m_vClientChatDirectMessages)
				if(MessageMatchesDirectTarget(Message))
					vpMessages.push_back(&Message);
		}
		else
		{
			for(const CClientChatMessage &Message : m_vClientChatRoomMessages)
				vpMessages.push_back(&Message);
		}

		if(vpMessages.empty())
		{
			CUIRect EmptyState = MessageArea;
			EmptyState.Draw(ColorRGBA(0.016f, 0.02f, 0.036f, 0.64f), IGraphics::CORNER_ALL, 10.0f);
			EmptyState.Margin(16.0f, &EmptyState);
			Ui()->DoLabel(&EmptyState, DirectView ? "No messages in this LS yet." : "Room is quiet. Send the first message.", 14.0f, TEXTALIGN_MC, {.m_MaxWidth = EmptyState.w});
		}
		else
		{
			const float RowHeight = 56.0f;
			const float RowSpacing = 6.0f;
			const int VisibleMessages = maximum(1, minimum(minimum((int)vpMessages.size(), CLIENT_CHAT_MAX_VISIBLE_ITEMS), (int)((MessageArea.h + RowSpacing) / (RowHeight + RowSpacing))));
			const int StartIndex = maximum(0, (int)vpMessages.size() - VisibleMessages);
			static std::array<CButtonContainer, CLIENT_CHAT_MAX_VISIBLE_ITEMS> s_MessageActionButtons;
			for(int VisibleIndex = 0; VisibleIndex < VisibleMessages; ++VisibleIndex)
			{
				CUIRect Row;
				MessageArea.HSplitTop(RowHeight, &Row, &MessageArea);
				MessageArea.HSplitTop(RowSpacing, nullptr, &MessageArea);
				const CClientChatMessage &Message = *vpMessages[StartIndex + VisibleIndex];
				const bool LocalMessage = IsLocalMessage(Message);
				const float BubbleWidth = minimum(Row.w * 0.82f, Row.w - 18.0f);
				CUIRect Bubble;
				if(LocalMessage)
					Row.VSplitRight(BubbleWidth, nullptr, &Bubble);
				else
					Row.VSplitLeft(BubbleWidth, &Bubble, nullptr);
				const bool Hover = Ui()->MouseInside(&Bubble);
				ColorRGBA BubbleColor = LocalMessage ? ColorRGBA(ms_GuiColor.r * 0.40f, ms_GuiColor.g * 0.42f, ms_GuiColor.b * 0.46f, 0.36f) : ColorRGBA(0.055f, 0.07f, 0.10f, 0.95f);
				if(Message.m_Direct)
					BubbleColor = LocalMessage ? ColorRGBA(0.10f, 0.14f, 0.22f, 0.94f) : ColorRGBA(0.05f, 0.08f, 0.13f, 0.96f);
				if(Message.m_MentionsLocal)
					BubbleColor = ColorRGBA(0.24f, 0.16f, 0.05f, 0.92f);
				Bubble.Draw(BubbleColor, IGraphics::CORNER_ALL, 10.0f);
				CUIRect Accent;
				Bubble.VSplitLeft(4.0f, &Accent, &Bubble);
				Accent.Draw(ClientChatAccentColor(Message.m_UseCustomColor, Message.m_ColorBody, LocalMessage ? 0.74f : 0.58f), IGraphics::CORNER_L, 10.0f);
				Bubble.Margin(7.0f, &Bubble);
				CUIRect Avatar, TextArea;
				Bubble.VSplitLeft(28.0f, &Avatar, &TextArea);
				TextArea.VSplitLeft(6.0f, nullptr, &TextArea);
				CTeeRenderInfo TeeInfo = GetTeeRenderInfo(vec2(20.0f, 20.0f), Message.m_aSkin, Message.m_UseCustomColor, Message.m_ColorBody, Message.m_ColorFeet);
				RenderTools()->RenderTee(CAnimState::GetIdle(), &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), vec2(Avatar.x + Avatar.w / 2.0f, Avatar.y + Avatar.h / 2.0f + 1.0f));
				CUIRect MetaRow, TextRow;
				TextArea.HSplitTop(14.0f, &MetaRow, &TextArea);
				TextArea.HSplitTop(3.0f, nullptr, &TextArea);
				TextRow = TextArea;
				char aTimestamp[32] = "";
				if(Message.m_Timestamp > 0)
					str_timestamp_ex((time_t)Message.m_Timestamp, aTimestamp, sizeof(aTimestamp), "%H:%M");
				CUIRect ActionButton, TimeLabel, NameLabel;
				if(!LocalMessage && !DirectView)
				{
					MetaRow.VSplitRight(Hover ? 96.0f : 0.0f, &MetaRow, &ActionButton);
					if(Hover)
					{
						CUIRect LsButton, FriendAction, BlockAction;
						ActionButton.VSplitLeft(30.0f, &LsButton, &ActionButton);
						ActionButton.VSplitLeft(3.0f, nullptr, &ActionButton);
						ActionButton.VSplitLeft(30.0f, &FriendAction, &ActionButton);
						ActionButton.VSplitLeft(3.0f, nullptr, &ActionButton);
						BlockAction = ActionButton;
						if(DoButton_Menu(&s_MessageActionButtons[VisibleIndex], "LS", 0, &LsButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 6.0f, 0.0f, ColorRGBA(0.14f, 0.18f, 0.24f, 0.98f)))
							ActivateClientChatDirect(Message.m_aSenderId, Message.m_aName, Message.m_aSkin, Message.m_UseCustomColor, Message.m_ColorBody, Message.m_ColorFeet);
						static std::array<CButtonContainer, CLIENT_CHAT_MAX_VISIBLE_ITEMS> s_MessageFriendButtons;
						static std::array<CButtonContainer, CLIENT_CHAT_MAX_VISIBLE_ITEMS> s_MessageBlockButtons;
						if(DoButton_Menu(&s_MessageFriendButtons[VisibleIndex], "+", 0, &FriendAction, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 6.0f, 0.0f, ColorRGBA(0.12f, 0.20f, 0.30f, 0.98f)))
							SendClientChatFriendRequest(Message.m_aSenderId, Message.m_aName);
						if(DoButton_Menu(&s_MessageBlockButtons[VisibleIndex], "B", 0, &BlockAction, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 6.0f, 0.0f, ColorRGBA(0.27f, 0.10f, 0.11f, 0.98f)))
							ToggleClientChatBlock(Message.m_aSenderId, Message.m_aName, true);
					}
				}
				MetaRow.VSplitRight(42.0f, &NameLabel, &TimeLabel);
				Ui()->DoLabel(&NameLabel, LocalMessage ? "You" : Message.m_aName, 10.8f, TEXTALIGN_ML, {.m_MaxWidth = NameLabel.w});
				if(aTimestamp[0] != '\0')
				{
					TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.42f));
					Ui()->DoLabel(&TimeLabel, aTimestamp, 8.5f, TEXTALIGN_MR);
					TextRender()->TextColor(TextRender()->DefaultTextColor());
				}
				if(Message.m_MentionsLocal)
				{
					CUIRect PingBadge = MetaRow;
					PingBadge.VSplitRight(42.0f, nullptr, &PingBadge);
					PingBadge.HMargin(1.0f, &PingBadge);
					PingBadge.Draw(ColorRGBA(0.95f, 0.72f, 0.16f, 0.24f), IGraphics::CORNER_ALL, 6.0f);
					Ui()->DoLabel(&PingBadge, "@you", 8.2f, TEXTALIGN_MC);
				}
				else if(Message.m_Direct)
				{
					CUIRect DirectBadge = MetaRow;
					DirectBadge.VSplitRight(32.0f, nullptr, &DirectBadge);
					DirectBadge.HMargin(1.0f, &DirectBadge);
					DirectBadge.Draw(ColorRGBA(ms_GuiColor.r, ms_GuiColor.g, ms_GuiColor.b, 0.18f), IGraphics::CORNER_ALL, 6.0f);
					Ui()->DoLabel(&DirectBadge, "LS", 8.2f, TEXTALIGN_MC);
				}
				Ui()->DoLabel(&TextRow, Message.m_aText, 11.4f, TEXTALIGN_TL, {.m_MaxWidth = TextRow.w});
			}
		}
	}

	if(!SettingsView)
	{
		Composer.Draw(ColorRGBA(0.040f, 0.050f, 0.082f, 0.94f), IGraphics::CORNER_ALL, 12.0f);
		Composer.Margin(10.0f, &Composer);
		CUIRect HintRow, InputRow;
		Composer.HSplitTop(12.0f, &HintRow, &Composer);
		Composer.HSplitTop(7.0f, nullptr, &Composer);
		InputRow = Composer;
		char aHint[160];
		if(DirectView && DirectBlocked)
			str_format(aHint, sizeof(aHint), "You blocked %s. Unblock to send direct messages.", m_aClientChatDirectTargetName);
		else if(DirectView)
			str_format(aHint, sizeof(aHint), "This LS is visible only to you and %s.", m_aClientChatDirectTargetName);
		else
			str_copy(aHint, "Tip: use LS on a room message to pin someone into your private list.", sizeof(aHint));
		TextRender()->TextColor(ColorRGBA(0.82f, 0.87f, 1.0f, 0.55f));
		Ui()->DoLabel(&HintRow, aHint, 8.9f, TEXTALIGN_ML, {.m_MaxWidth = HintRow.w});
		TextRender()->TextColor(TextRender()->DefaultTextColor());
		CUIRect InputBox, SendButton;
		InputRow.VSplitRight(112.0f, &InputBox, &SendButton);
		InputBox.VSplitRight(8.0f, &InputBox, nullptr);
		Ui()->DoClearableEditBox(&m_ClientChatInput, &InputBox, 14.0f);
		static CButtonContainer s_SendButton;
		const bool AllowSend = !m_pClientChatSendTask && !DirectBlocked;
		const char *pSendLabel = DirectBlocked ? "Blocked" : (AllowSend ? (DirectView ? "Send LS" : Localize("Send")) : Localize("Sending"));
		if((DoButton_Menu(&s_SendButton, pSendLabel, 0, &SendButton, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 8.0f, 0.0f, ColorRGBA(ms_GuiColor.r, ms_GuiColor.g, ms_GuiColor.b, 0.36f)) && AllowSend) ||
			(AllowSend && m_ClientChatInput.IsActive() && Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER)))
			SendClientChatMessage();
	}
}

void CMenus::OnInterfacesInit(CGameClient *pClient)
{
	CComponentInterfaces::OnInterfacesInit(pClient);
	m_MenusIngameTouchControls.OnInterfacesInit(pClient);
	m_MenusSettingsControls.OnInterfacesInit(pClient);
	m_MenusStart.OnInterfacesInit(pClient);
	m_MenusStartRClient.OnInterfacesInit(pClient);
	m_CommunityIcons.OnInterfacesInit(pClient);
}

void CMenus::OnConsoleInit()
{
	Console()->Register("zz_clickgui_toggle", "", CFGFLAG_CLIENT, ConToggleZzClickGui, this, "Toggle ZZ ClickGUI overlay");
}

void CMenus::OnInit()
{
	if(g_Config.m_ClShowWelcome)
	{
		m_Popup = POPUP_LANGUAGE;
		m_CreateDefaultFavoriteCommunities = true;
	}

	if(g_Config.m_UiPage >= PAGE_FAVORITE_COMMUNITY_1 && g_Config.m_UiPage <= PAGE_FAVORITE_COMMUNITY_5 &&
		(size_t)(g_Config.m_UiPage - PAGE_FAVORITE_COMMUNITY_1) >= ServerBrowser()->FavoriteCommunities().size())
	{
		// Reset page to internet when there is no favorite community for this page.
		g_Config.m_UiPage = PAGE_INTERNET;
	}

	if(g_Config.m_ClSkipStartMenu)
	{
		m_ShowStart = false;
	}
	m_MenuPage = g_Config.m_UiPage;
	if(m_MenuPage == PAGE_CLIENT_CHAT)
	{
		m_MenuPage = PAGE_SETTINGS;
		g_Config.m_UiPage = PAGE_SETTINGS;
	}

	m_RefreshButton.Init(Ui(), -1);
	m_ConnectButton.Init(Ui(), -1);

	Console()->Chain("add_favorite", ConchainFavoritesUpdate, this);
	Console()->Chain("remove_favorite", ConchainFavoritesUpdate, this);
	Console()->Chain("add_friend", ConchainFriendlistUpdate, this);
	Console()->Chain("remove_friend", ConchainFriendlistUpdate, this);

	Console()->Chain("add_excluded_community", ConchainCommunitiesUpdate, this);
	Console()->Chain("remove_excluded_community", ConchainCommunitiesUpdate, this);
	Console()->Chain("add_excluded_country", ConchainCommunitiesUpdate, this);
	Console()->Chain("remove_excluded_country", ConchainCommunitiesUpdate, this);
	Console()->Chain("add_excluded_type", ConchainCommunitiesUpdate, this);
	Console()->Chain("remove_excluded_type", ConchainCommunitiesUpdate, this);

	Console()->Chain("ui_page", ConchainUiPageUpdate, this);

	Console()->Chain("snd_enable", ConchainUpdateMusicState, this);
	Console()->Chain("snd_enable_music", ConchainUpdateMusicState, this);
	Console()->Chain("cl_background_entities", ConchainBackgroundEntities, this);

	Console()->Chain("cl_assets_entities", ConchainAssetsEntities, this);
	Console()->Chain("cl_asset_game", ConchainAssetGame, this);
	Console()->Chain("cl_asset_emoticons", ConchainAssetEmoticons, this);
	Console()->Chain("cl_asset_particles", ConchainAssetParticles, this);
	Console()->Chain("cl_asset_hud", ConchainAssetHud, this);
	Console()->Chain("cl_asset_extras", ConchainAssetExtras, this);

	Console()->Chain("demo_play", ConchainDemoPlay, this);
	Console()->Chain("demo_speed", ConchainDemoSpeed, this);

	m_TextureBlob = Graphics()->LoadTexture("blob.png", IStorage::TYPE_ALL);
	m_BestClientIconTexture = Graphics()->LoadTexture("bestclient.png", IStorage::TYPE_ALL);

	// setup load amount
	m_LoadingState.m_Current = 0;
	m_LoadingState.m_Total = g_pData->m_NumImages + GameClient()->ComponentCount();
	if(!g_Config.m_ClThreadsoundloading)
		m_LoadingState.m_Total += g_pData->m_NumSounds;

	m_IsInit = true;

	// load menu images
	m_vMenuImages.clear();
	Storage()->ListDirectory(IStorage::TYPE_ALL, "menuimages", MenuImageScan, this);

	m_CommunityIcons.Load();

	// Quad for the direction arrows above the player
	m_DirectionQuadContainerIndex = Graphics()->CreateQuadContainer(false);
	Graphics()->QuadContainerAddSprite(m_DirectionQuadContainerIndex, 0.f, 0.f, 22.f);
	Graphics()->QuadContainerUpload(m_DirectionQuadContainerIndex);
}

void CMenus::ConchainBackgroundEntities(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CMenus *pSelf = (CMenus *)pUserData;
		if(str_comp(g_Config.m_ClBackgroundEntities, pSelf->GameClient()->m_Background.MapName()) != 0)
			pSelf->GameClient()->m_Background.LoadBackground();
	}
}

void CMenus::ConchainUpdateMusicState(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	auto *pSelf = (CMenus *)pUserData;
	if(pResult->NumArguments())
		pSelf->UpdateMusicState();
}

void CMenus::UpdateMusicState()
{
	const bool ShouldPlay = Client()->State() == IClient::STATE_OFFLINE && g_Config.m_SndEnable && g_Config.m_SndMusic;
	if(ShouldPlay && !GameClient()->m_Sounds.IsPlaying(SOUND_MENU))
		GameClient()->m_Sounds.Enqueue(CSounds::CHN_MUSIC, SOUND_MENU);
	else if(!ShouldPlay && GameClient()->m_Sounds.IsPlaying(SOUND_MENU))
		GameClient()->m_Sounds.Stop(SOUND_MENU);
}

void CMenus::PopupMessage(const char *pTitle, const char *pMessage, const char *pButtonLabel, int NextPopup, FPopupButtonCallback pfnButtonCallback)
{
	// reset active item
	Ui()->SetActiveItem(nullptr);

	str_copy(m_aPopupTitle, pTitle);
	str_copy(m_aPopupMessage, pMessage);
	str_copy(m_aPopupButtons[BUTTON_CONFIRM].m_aLabel, pButtonLabel);
	m_aPopupButtons[BUTTON_CONFIRM].m_NextPopup = NextPopup;
	m_aPopupButtons[BUTTON_CONFIRM].m_pfnCallback = pfnButtonCallback;
	m_Popup = POPUP_MESSAGE;
}

void CMenus::PopupConfirm(const char *pTitle, const char *pMessage, const char *pConfirmButtonLabel, const char *pCancelButtonLabel,
	FPopupButtonCallback pfnConfirmButtonCallback, int ConfirmNextPopup, FPopupButtonCallback pfnCancelButtonCallback, int CancelNextPopup)
{
	// reset active item
	Ui()->SetActiveItem(nullptr);

	str_copy(m_aPopupTitle, pTitle);
	str_copy(m_aPopupMessage, pMessage);
	str_copy(m_aPopupButtons[BUTTON_CONFIRM].m_aLabel, pConfirmButtonLabel);
	m_aPopupButtons[BUTTON_CONFIRM].m_NextPopup = ConfirmNextPopup;
	m_aPopupButtons[BUTTON_CONFIRM].m_pfnCallback = pfnConfirmButtonCallback;
	str_copy(m_aPopupButtons[BUTTON_CANCEL].m_aLabel, pCancelButtonLabel);
	m_aPopupButtons[BUTTON_CANCEL].m_NextPopup = CancelNextPopup;
	m_aPopupButtons[BUTTON_CANCEL].m_pfnCallback = pfnCancelButtonCallback;
	m_Popup = POPUP_CONFIRM;
}

void CMenus::PopupWarning(const char *pTopic, const char *pBody, const char *pButton, std::chrono::nanoseconds Duration)
{
	// no multiline support for console
	std::string BodyStr = pBody;
	std::replace(BodyStr.begin(), BodyStr.end(), '\n', ' ');
	log_warn("client", "%s: %s", pTopic, BodyStr.c_str());

	Ui()->SetActiveItem(nullptr);

	str_copy(m_aMessageTopic, pTopic);
	str_copy(m_aMessageBody, pBody);
	str_copy(m_aMessageButton, pButton);
	m_Popup = POPUP_WARNING;
	SetActive(true);

	m_PopupWarningDuration = Duration;
	m_PopupWarningLastTime = time_get_nanoseconds();
}

bool CMenus::CanDisplayWarning() const
{
	return m_Popup == POPUP_NONE;
}

void CMenus::Render()
{
	Ui()->MapScreen();
	Ui()->SetMouseSlow(false);

	static int s_Frame = 0;
	if(s_Frame == 0)
	{
		RefreshBrowserTab(true);
		s_Frame++;
	}
	else if(s_Frame == 1)
	{
		UpdateMusicState();
		s_Frame++;
	}
	else
	{
		m_CommunityIcons.Update();
	}

	// Initially add DDNet as favorite community and select its tab.
	// This must be delayed until the DDNet info is available.
	if(m_CreateDefaultFavoriteCommunities &&
		ServerBrowser()->DDNetInfoAvailable())
	{
		m_CreateDefaultFavoriteCommunities = false;
		if(ServerBrowser()->Community(IServerBrowser::COMMUNITY_DDNET) != nullptr)
		{
			ServerBrowser()->FavoriteCommunitiesFilter().Clear();
			ServerBrowser()->FavoriteCommunitiesFilter().Add(IServerBrowser::COMMUNITY_DDNET);
			SetMenuPage(PAGE_FAVORITE_COMMUNITY_1);
			ServerBrowser()->Refresh(IServerBrowser::TYPE_FAVORITE_COMMUNITY_1);
		}
	}
	if(m_JoinTutorial.m_Queued && m_Popup == POPUP_NONE)
	{
		const char *pAddr = ServerBrowser()->GetTutorialServer();
		if(pAddr)
		{
			Client()->Connect(pAddr);
		}
		else
		{
			m_Popup = POPUP_JOIN_TUTORIAL;
		}
		m_JoinTutorial.m_Queued = false;
	}

	// Determine the client state once before rendering because it can change
	// while rendering which causes frames with broken user interface.
	const IClient::EClientState ClientState = Client()->State();

	if(ClientState == IClient::STATE_ONLINE || ClientState == IClient::STATE_DEMOPLAYBACK)
	{
		ms_ColorTabbarInactive = ms_ColorTabbarInactiveIngame;
		ms_ColorTabbarActive = ms_ColorTabbarActiveIngame;
		ms_ColorTabbarHover = ms_ColorTabbarHoverIngame;
	}
	else
	{
		if(!GameClient()->m_MenuBackground.Render())
		{
			RenderBackground();
		}
		ms_ColorTabbarInactive = ms_ColorTabbarInactiveOutgame;
		ms_ColorTabbarActive = ms_ColorTabbarActiveOutgame;
		ms_ColorTabbarHover = ms_ColorTabbarHoverOutgame;
	}

	CUIRect Screen = *Ui()->Screen();
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK || m_Popup != POPUP_NONE)
	{
		Screen.Margin(10.0f, &Screen);
	}

	switch(ClientState)
	{
	case IClient::STATE_QUITTING:
	case IClient::STATE_RESTARTING:
		// Render nothing except menu background. This should not happen for more than one frame.
		return;

	case IClient::STATE_CONNECTING:
		RenderPopupConnecting(Screen);
		break;

	case IClient::STATE_LOADING:
		RenderPopupLoading(Screen);
		break;

	case IClient::STATE_OFFLINE:
		if(m_Popup != POPUP_NONE)
		{
			RenderPopupFullscreen(Screen);
		}
		else if(m_ShowStart)
		{
			m_MenusStartRClient.RenderStartMenu(Screen);
		}
		else
		{
			CUIRect TabBar, MainView;
			Screen.HSplitTop(24.0f, &TabBar, &MainView);

			if(m_MenuPage == PAGE_NEWS)
			{
				RenderNews(MainView);
			}
			else if(m_MenuPage >= PAGE_INTERNET && m_MenuPage <= PAGE_FAVORITE_COMMUNITY_5)
			{
				RenderServerbrowser(MainView);
			}
			else if(m_MenuPage == PAGE_DEMOS)
			{
				RenderDemoBrowser(MainView);
			}
			else if(m_MenuPage == PAGE_SETTINGS)
			{
				RenderSettings(MainView);
			}
			else
			{
				dbg_assert_failed("Invalid m_MenuPage: %d", m_MenuPage);
			}

			RenderMenubar(TabBar, ClientState);
		}
		break;

	case IClient::STATE_ONLINE:
		if(m_Popup != POPUP_NONE)
		{
			RenderPopupFullscreen(Screen);
		}
		else
		{
			CUIRect TabBar, MainView;
			Screen.HSplitTop(24.0f, &TabBar, &MainView);

			if(m_GamePage == PAGE_GAME)
			{
				RenderGame(MainView);
				RenderIngameHint();
			}
			else if(m_GamePage == PAGE_PLAYERS)
			{
				RenderPlayers(MainView);
			}
			else if(m_GamePage == PAGE_SERVER_INFO)
			{
				RenderServerInfo(MainView);
			}
			else if(m_GamePage == PAGE_NETWORK)
			{
				RenderInGameNetwork(MainView);
			}
			else if(m_GamePage == PAGE_GHOST)
			{
				RenderGhost(MainView);
			}
			else if(m_GamePage == PAGE_CALLVOTE)
			{
				RenderServerControl(MainView);
			}
			else if(m_GamePage == PAGE_DEMOS)
			{
				RenderDemoBrowser(MainView);
			}
			else if(m_GamePage == PAGE_SETTINGS)
			{
				RenderSettings(MainView);
			}
			else
			{
				dbg_assert_failed("Invalid m_GamePage: %d", m_GamePage);
			}

			RenderMenubar(TabBar, ClientState);
		}
		break;

	case IClient::STATE_DEMOPLAYBACK:
		if(m_Popup != POPUP_NONE)
		{
			RenderPopupFullscreen(Screen);
		}
		else
		{
			RenderDemoPlayer(Screen);
		}
		break;
	}

	Ui()->RenderPopupMenus();

	// Prevent UI elements from being hovered while a key reader is active
	if(GameClient()->m_KeyBinder.IsActive())
	{
		Ui()->SetHotItem(nullptr);
	}

	// Handle this escape hotkey after popup menus
	if(!m_ShowStart && ClientState == IClient::STATE_OFFLINE && Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
	{
		m_ShowStart = true;
	}
}

void CMenus::RenderPopupFullscreen(CUIRect Screen)
{
	char aBuf[1536];
	const char *pTitle = "";
	const char *pExtraText = "";
	const char *pButtonText = "";
	bool TopAlign = false;

	ColorRGBA BgColor = ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f);
	if(m_Popup == POPUP_MESSAGE || m_Popup == POPUP_CONFIRM)
	{
		pTitle = m_aPopupTitle;
		pExtraText = m_aPopupMessage;
		TopAlign = true;
	}
	else if(m_Popup == POPUP_DISCONNECTED)
	{
		pTitle = Localize("Disconnected");
		pExtraText = Client()->ErrorString();
		pButtonText = Localize("Ok");
		if(Client()->ReconnectTime() > 0)
		{
			str_format(aBuf, sizeof(aBuf), Localize("Reconnect in %d sec"), (int)((Client()->ReconnectTime() - time_get()) / time_freq()) + 1);
			pTitle = Client()->ErrorString();
			pExtraText = aBuf;
			pButtonText = Localize("Abort");
		}
	}
	else if(m_Popup == POPUP_RENAME_DEMO)
	{
		dbg_assert(m_DemolistSelectedIndex >= 0, "m_DemolistSelectedIndex invalid for POPUP_RENAME_DEMO");
		pTitle = m_vpFilteredDemos[m_DemolistSelectedIndex]->m_IsDir ? Localize("Rename folder") : Localize("Rename demo");
	}
#if defined(CONF_VIDEORECORDER)
	else if(m_Popup == POPUP_RENDER_DEMO)
	{
		pTitle = Localize("Render demo");
	}
	else if(m_Popup == POPUP_RENDER_DONE)
	{
		pTitle = Localize("Render complete");
	}
#endif
	else if(m_Popup == POPUP_PASSWORD)
	{
		pTitle = Localize("Password incorrect");
		pButtonText = Localize("Try again");
	}
	else if(m_Popup == POPUP_RESTART)
	{
		pTitle = Localize("Restart");
		pExtraText = Localize("Are you sure that you want to restart?");
	}
	else if(m_Popup == POPUP_QUIT)
	{
		pTitle = Localize("Quit");
		pExtraText = Localize("Are you sure that you want to quit?");
	}
	else if(m_Popup == POPUP_FIRST_LAUNCH)
	{
		pTitle = Localize("Welcome to DDNet");
		str_format(aBuf, sizeof(aBuf), "%s\n\n%s\n\n%s\n\n%s",
			Localize("DDraceNetwork is a cooperative online game where the goal is for you and your group of tees to reach the finish line of the map. As a newcomer you should start on Novice servers, which host the easiest maps. Consider the ping to choose a server close to you."),
			Localize("Use k key to kill (restart), q to pause and watch other players. See settings for other key binds."),
			Localize("It's recommended that you check the settings to adjust them to your liking before joining a server."),
			Localize("Please enter your nickname below."));
		pExtraText = aBuf;
		pButtonText = Localize("Ok");
		TopAlign = true;
	}
	else if(m_Popup == POPUP_JOIN_TUTORIAL)
	{
		pTitle = Localize("Joining Tutorial server");
	}
	else if(m_Popup == POPUP_POINTS)
	{
		pTitle = Localize("Existing Player");
		if(Client()->InfoState() == IClient::EInfoState::SUCCESS && Client()->Points() > 50)
		{
			str_format(aBuf, sizeof(aBuf), Localize("Your nickname '%s' is already used (%d points). Do you still want to use it?"), Client()->PlayerName(), Client()->Points());
			pExtraText = aBuf;
			TopAlign = true;
		}
		else
		{
			pExtraText = Localize("Checking for existing player with your name");
		}
	}
	else if(m_Popup == POPUP_WARNING)
	{
		BgColor = ColorRGBA(0.5f, 0.0f, 0.0f, 0.7f);
		pTitle = m_aMessageTopic;
		pExtraText = m_aMessageBody;
		pButtonText = m_aMessageButton;
		TopAlign = true;
	}
	else if(m_Popup == POPUP_SAVE_SKIN)
	{
		pTitle = Localize("Save skin");
		pExtraText = Localize("Are you sure you want to save your skin? If a skin with this name already exists, it will be replaced.");
	}

	CUIRect Box, Part;
	Box = Screen;
	if(m_Popup != POPUP_FIRST_LAUNCH)
	{
		Box.Margin(150.0f, &Box);
	}

	// Background
	Box.Draw(BgColor, IGraphics::CORNER_ALL, 15.0f);

	// Title
	{
		CUIRect Title;
		Box.HSplitTop(20.0f, nullptr, &Box);
		Box.HSplitTop(24.0f, &Title, &Box);
		Box.HSplitTop(20.0f, nullptr, &Box);
		Title.VMargin(20.0f, &Title);

		const float TitleFontSize = 24.0f;
		if(TextRender()->TextWidth(TitleFontSize, pTitle) > Title.w)
			Ui()->DoLabel(&Title, pTitle, TitleFontSize, TEXTALIGN_ML, {.m_MaxWidth = Title.w});
		else
			Ui()->DoLabel(&Title, pTitle, TitleFontSize, TEXTALIGN_MC);
	}

	// Extra text (optional)
	if(m_Popup != POPUP_JOIN_TUTORIAL)
	{
		CUIRect ExtraText;
		Box.HSplitTop(24.0f, &ExtraText, &Box);
		ExtraText.VMargin(20.0f, &ExtraText);
		if(pExtraText[0] != '\0')
		{
			const float ExtraTextFontSize = m_Popup == POPUP_FIRST_LAUNCH ? 16.0f : 20.0f;

			if(TopAlign)
				Ui()->DoLabel(&ExtraText, pExtraText, ExtraTextFontSize, TEXTALIGN_TL, {.m_MaxWidth = ExtraText.w});
			else if(TextRender()->TextWidth(ExtraTextFontSize, pExtraText) > ExtraText.w)
				Ui()->DoLabel(&ExtraText, pExtraText, ExtraTextFontSize, TEXTALIGN_ML, {.m_MaxWidth = ExtraText.w});
			else
				Ui()->DoLabel(&ExtraText, pExtraText, ExtraTextFontSize, TEXTALIGN_MC);
		}
	}

	if(m_Popup == POPUP_MESSAGE || m_Popup == POPUP_CONFIRM)
	{
		CUIRect ButtonBar;
		Box.HSplitBottom(20.0f, &Box, nullptr);
		Box.HSplitBottom(24.0f, &Box, &ButtonBar);
		ButtonBar.VMargin(100.0f, &ButtonBar);

		if(m_Popup == POPUP_MESSAGE)
		{
			static CButtonContainer s_ButtonConfirm;
			if(DoButton_Menu(&s_ButtonConfirm, m_aPopupButtons[BUTTON_CONFIRM].m_aLabel, 0, &ButtonBar) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE) || Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER))
			{
				m_Popup = m_aPopupButtons[BUTTON_CONFIRM].m_NextPopup;
				(this->*m_aPopupButtons[BUTTON_CONFIRM].m_pfnCallback)();
			}
		}
		else if(m_Popup == POPUP_CONFIRM)
		{
			CUIRect CancelButton, ConfirmButton;
			ButtonBar.VSplitMid(&CancelButton, &ConfirmButton, 40.0f);

			static CButtonContainer s_ButtonCancel;
			if(DoButton_Menu(&s_ButtonCancel, m_aPopupButtons[BUTTON_CANCEL].m_aLabel, 0, &CancelButton) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
			{
				m_Popup = m_aPopupButtons[BUTTON_CANCEL].m_NextPopup;
				(this->*m_aPopupButtons[BUTTON_CANCEL].m_pfnCallback)();
			}

			static CButtonContainer s_ButtonConfirm;
			if(DoButton_Menu(&s_ButtonConfirm, m_aPopupButtons[BUTTON_CONFIRM].m_aLabel, 0, &ConfirmButton) || Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER))
			{
				m_Popup = m_aPopupButtons[BUTTON_CONFIRM].m_NextPopup;
				(this->*m_aPopupButtons[BUTTON_CONFIRM].m_pfnCallback)();
			}
		}
	}
	else if(m_Popup == POPUP_QUIT || m_Popup == POPUP_RESTART)
	{
		CUIRect Yes, No;
		Box.HSplitBottom(20.f, &Box, &Part);
		Box.HSplitBottom(24.f, &Box, &Part);

		// additional info
		Box.VMargin(20.f, &Box);
		if(GameClient()->Editor()->HasUnsavedData())
		{
			str_format(aBuf, sizeof(aBuf), "%s\n\n%s", Localize("There's an unsaved map in the editor, you might want to save it."), Localize("Continue anyway?"));
			Ui()->DoLabel(&Box, aBuf, 20.0f, TEXTALIGN_ML, {.m_MaxWidth = Part.w - 20.0f});
		}
		else if(GameClient()->m_TouchControls.HasEditingChanges() || m_MenusIngameTouchControls.UnsavedChanges())
		{
			str_format(aBuf, sizeof(aBuf), "%s\n\n%s", Localize("There's an unsaved change in the touch controls editor, you might want to save it."), Localize("Continue anyway?"));
			Ui()->DoLabel(&Box, aBuf, 20.0f, TEXTALIGN_ML, {.m_MaxWidth = Part.w - 20.0f});
		}

		// buttons
		Part.VMargin(80.0f, &Part);
		Part.VSplitMid(&No, &Yes);
		Yes.VMargin(20.0f, &Yes);
		No.VMargin(20.0f, &No);

		static CButtonContainer s_ButtonAbort;
		if(DoButton_Menu(&s_ButtonAbort, Localize("No"), 0, &No) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
			m_Popup = POPUP_NONE;

		static CButtonContainer s_ButtonTryAgain;
		if(DoButton_Menu(&s_ButtonTryAgain, Localize("Yes"), 0, &Yes) || Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER))
		{
			if(m_Popup == POPUP_RESTART)
			{
				m_Popup = POPUP_NONE;
				Client()->Restart();
			}
			else
			{
				m_Popup = POPUP_NONE;
				Client()->Quit();
			}
		}
	}
	else if(m_Popup == POPUP_PASSWORD)
	{
		Box.HSplitBottom(20.0f, &Box, nullptr);
		Box.HSplitBottom(24.0f, &Box, &Part);
		Part.VMargin(100.0f, &Part);

		CUIRect TryAgain, Abort;
		Part.VSplitMid(&Abort, &TryAgain, 40.0f);

		static CButtonContainer s_ButtonAbort;
		if(DoButton_Menu(&s_ButtonAbort, Localize("Abort"), 0, &Abort) ||
			Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
		{
			m_Popup = POPUP_NONE;
		}

		char aAddr[NETADDR_MAXSTRSIZE];
		net_addr_str(&Client()->ServerAddress(), aAddr, sizeof(aAddr), true);

		static CButtonContainer s_ButtonTryAgain;
		if(DoButton_Menu(&s_ButtonTryAgain, Localize("Try again"), 0, &TryAgain) ||
			Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER))
		{
			Client()->Connect(aAddr, g_Config.m_Password);
		}

		Box.VMargin(60.0f, &Box);
		Box.HSplitBottom(32.0f, &Box, nullptr);
		Box.HSplitBottom(24.0f, &Box, &Part);

		CUIRect Label, TextBox;
		Part.VSplitLeft(100.0f, &Label, &TextBox);
		TextBox.VSplitLeft(20.0f, nullptr, &TextBox);
		Ui()->DoLabel(&Label, Localize("Password"), 18.0f, TEXTALIGN_ML);
		Ui()->DoClearableEditBox(&m_PasswordInput, &TextBox, 12.0f);

		Box.HSplitBottom(32.0f, &Box, nullptr);
		Box.HSplitBottom(24.0f, &Box, &Part);

		CUIRect Address;
		Part.VSplitLeft(100.0f, &Label, &Address);
		Address.VSplitLeft(20.0f, nullptr, &Address);
		Ui()->DoLabel(&Label, Localize("Address"), 18.0f, TEXTALIGN_ML);
		Ui()->DoLabel(&Address, aAddr, 18.0f, TEXTALIGN_ML);

		const CServerBrowser::CServerEntry *pEntry = ServerBrowser()->Find(Client()->ServerAddress());
		if(pEntry != nullptr && pEntry->m_GotInfo)
		{
			const CCommunity *pCommunity = ServerBrowser()->Community(pEntry->m_Info.m_aCommunityId);
			const CCommunityIcon *pIcon = pCommunity == nullptr ? nullptr : m_CommunityIcons.Find(pCommunity->Id());

			Box.HSplitBottom(32.0f, &Box, nullptr);
			Box.HSplitBottom(24.0f, &Box, &Part);

			CUIRect Name;
			Part.VSplitLeft(100.0f, &Label, &Name);
			Name.VSplitLeft(20.0f, nullptr, &Name);
			if(pIcon != nullptr)
			{
				CUIRect Icon;
				static char s_CommunityTooltipButtonId;
				Name.VSplitLeft(2.5f * Name.h, &Icon, &Name);
				m_CommunityIcons.Render(pIcon, Icon, true);
				Ui()->DoButtonLogic(&s_CommunityTooltipButtonId, 0, &Icon, BUTTONFLAG_NONE);
				GameClient()->m_Tooltips.DoToolTip(&s_CommunityTooltipButtonId, &Icon, pCommunity->Name());
			}

			Ui()->DoLabel(&Label, Localize("Name"), 18.0f, TEXTALIGN_ML);
			Ui()->DoLabel(&Name, pEntry->m_Info.m_aName, 18.0f, TEXTALIGN_ML);
		}
	}
	else if(m_Popup == POPUP_LANGUAGE)
	{
		CUIRect Button;
		Screen.Margin(150.0f, &Box);
		Box.HSplitTop(20.0f, nullptr, &Box);
		Box.HSplitBottom(20.0f, &Box, nullptr);
		Box.HSplitBottom(24.0f, &Box, &Button);
		Box.HSplitBottom(20.0f, &Box, nullptr);
		Box.VMargin(20.0f, &Box);
		const bool Activated = RenderLanguageSelection(Box);
		Button.VMargin(120.0f, &Button);

		static CButtonContainer s_Button;
		if(DoButton_Menu(&s_Button, Localize("Ok"), 0, &Button) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE) || Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER) || Activated)
			m_Popup = POPUP_FIRST_LAUNCH;
	}
	else if(m_Popup == POPUP_RENAME_DEMO)
	{
		CUIRect Label, TextBox, Ok, Abort;

		Box.HSplitBottom(20.f, &Box, &Part);
		Box.HSplitBottom(24.f, &Box, &Part);
		Part.VMargin(80.0f, &Part);

		Part.VSplitMid(&Abort, &Ok);

		Ok.VMargin(20.0f, &Ok);
		Abort.VMargin(20.0f, &Abort);

		static CButtonContainer s_ButtonAbort;
		if(DoButton_Menu(&s_ButtonAbort, Localize("Abort"), 0, &Abort) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
			m_Popup = POPUP_NONE;

		static CButtonContainer s_ButtonOk;
		if(DoButton_Menu(&s_ButtonOk, Localize("Ok"), 0, &Ok) || Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER))
		{
			m_Popup = POPUP_NONE;
			// rename demo
			char aBufOld[IO_MAX_PATH_LENGTH];
			str_format(aBufOld, sizeof(aBufOld), "%s/%s", m_aCurrentDemoFolder, m_vpFilteredDemos[m_DemolistSelectedIndex]->m_aFilename);
			char aBufNew[IO_MAX_PATH_LENGTH];
			str_format(aBufNew, sizeof(aBufNew), "%s/%s", m_aCurrentDemoFolder, m_DemoRenameInput.GetString());
			if(!m_vpFilteredDemos[m_DemolistSelectedIndex]->m_IsDir && !str_endswith(aBufNew, ".demo"))
				str_append(aBufNew, ".demo");

			if(str_comp(aBufOld, aBufNew) == 0)
			{
				// Nothing to rename, also same capitalization
			}
			else if(!str_valid_filename(m_DemoRenameInput.GetString()))
			{
				PopupMessage(Localize("Error"), Localize("This name cannot be used for files and folders"), Localize("Ok"), POPUP_RENAME_DEMO);
			}
			else if(str_utf8_comp_nocase(aBufOld, aBufNew) != 0 && // Allow renaming if it only changes capitalization to support case-insensitive filesystems
				Storage()->FileExists(aBufNew, m_vpFilteredDemos[m_DemolistSelectedIndex]->m_StorageType))
			{
				PopupMessage(Localize("Error"), Localize("A demo with this name already exists"), Localize("Ok"), POPUP_RENAME_DEMO);
			}
			else if(Storage()->FolderExists(aBufNew, m_vpFilteredDemos[m_DemolistSelectedIndex]->m_StorageType))
			{
				PopupMessage(Localize("Error"), Localize("A folder with this name already exists"), Localize("Ok"), POPUP_RENAME_DEMO);
			}
			else if(Storage()->RenameFile(aBufOld, aBufNew, m_vpFilteredDemos[m_DemolistSelectedIndex]->m_StorageType))
			{
				str_copy(m_aCurrentDemoSelectionName, m_DemoRenameInput.GetString());
				if(!m_vpFilteredDemos[m_DemolistSelectedIndex]->m_IsDir)
					fs_split_file_extension(m_DemoRenameInput.GetString(), m_aCurrentDemoSelectionName, sizeof(m_aCurrentDemoSelectionName));
				DemolistPopulate();
				DemolistOnUpdate(false);
			}
			else
			{
				PopupMessage(Localize("Error"), m_vpFilteredDemos[m_DemolistSelectedIndex]->m_IsDir ? Localize("Unable to rename the folder") : Localize("Unable to rename the demo"), Localize("Ok"), POPUP_RENAME_DEMO);
			}
		}

		Box.HSplitBottom(60.f, &Box, &Part);
		Box.HSplitBottom(24.f, &Box, &Part);

		Part.VSplitLeft(60.0f, nullptr, &Label);
		Label.VSplitLeft(120.0f, nullptr, &TextBox);
		TextBox.VSplitLeft(20.0f, nullptr, &TextBox);
		TextBox.VSplitRight(60.0f, &TextBox, nullptr);
		Ui()->DoLabel(&Label, Localize("New name:"), 18.0f, TEXTALIGN_ML);
		Ui()->DoEditBox(&m_DemoRenameInput, &TextBox, 12.0f);
	}
#if defined(CONF_VIDEORECORDER)
	else if(m_Popup == POPUP_RENDER_DEMO)
	{
		CUIRect Row, Ok, Abort;
		Box.VMargin(60.0f, &Box);
		Box.HMargin(20.0f, &Box);
		Box.HSplitBottom(24.0f, &Box, &Row);
		Box.HSplitBottom(40.0f, &Box, nullptr);
		Row.VMargin(40.0f, &Row);
		Row.VSplitMid(&Abort, &Ok, 40.0f);

		static CButtonContainer s_ButtonAbort;
		if(DoButton_Menu(&s_ButtonAbort, Localize("Abort"), 0, &Abort) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
		{
			m_DemoRenderInput.Clear();
			m_Popup = POPUP_NONE;
		}

		static CButtonContainer s_ButtonOk;
		if(DoButton_Menu(&s_ButtonOk, Localize("Ok"), 0, &Ok) || Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER))
		{
			m_Popup = POPUP_NONE;
			// render video
			char aVideoPath[IO_MAX_PATH_LENGTH];
			str_format(aVideoPath, sizeof(aVideoPath), "videos/%s", m_DemoRenderInput.GetString());
			if(!str_endswith(aVideoPath, ".mp4"))
				str_append(aVideoPath, ".mp4");

			if(!str_valid_filename(m_DemoRenderInput.GetString()))
			{
				PopupMessage(Localize("Error"), Localize("This name cannot be used for files and folders"), Localize("Ok"), POPUP_RENDER_DEMO);
			}
			else if(Storage()->FolderExists(aVideoPath, IStorage::TYPE_SAVE))
			{
				PopupMessage(Localize("Error"), Localize("A folder with this name already exists"), Localize("Ok"), POPUP_RENDER_DEMO);
			}
			else if(Storage()->FileExists(aVideoPath, IStorage::TYPE_SAVE))
			{
				char aMessage[128 + IO_MAX_PATH_LENGTH];
				str_format(aMessage, sizeof(aMessage), Localize("File '%s' already exists, do you want to overwrite it?"), m_DemoRenderInput.GetString());
				PopupConfirm(Localize("Replace video"), aMessage, Localize("Yes"), Localize("No"), &CMenus::PopupConfirmDemoReplaceVideo, POPUP_NONE, &CMenus::DefaultButtonCallback, POPUP_RENDER_DEMO);
			}
			else
			{
				PopupConfirmDemoReplaceVideo();
			}
		}

		CUIRect ShowChatCheckbox, UseSoundsCheckbox;
		Box.HSplitBottom(20.0f, &Box, &Row);
		Box.HSplitBottom(10.0f, &Box, nullptr);
		Row.VSplitMid(&ShowChatCheckbox, &UseSoundsCheckbox, 20.0f);

		if(DoButton_CheckBox(&g_Config.m_ClVideoShowChat, Localize("Show chat"), g_Config.m_ClVideoShowChat, &ShowChatCheckbox))
			g_Config.m_ClVideoShowChat ^= 1;

		if(DoButton_CheckBox(&g_Config.m_ClVideoSndEnable, Localize("Use sounds"), g_Config.m_ClVideoSndEnable, &UseSoundsCheckbox))
			g_Config.m_ClVideoSndEnable ^= 1;

		CUIRect ShowHudButton;
		Box.HSplitBottom(20.0f, &Box, &Row);
		Row.VSplitMid(&Row, &ShowHudButton, 20.0f);

		if(DoButton_CheckBox(&g_Config.m_ClVideoShowhud, Localize("Show ingame HUD"), g_Config.m_ClVideoShowhud, &ShowHudButton))
			g_Config.m_ClVideoShowhud ^= 1;

		// slowdown
		CUIRect SlowDownButton;
		Row.VSplitLeft(20.0f, &SlowDownButton, &Row);
		Row.VSplitLeft(5.0f, nullptr, &Row);
		static CButtonContainer s_SlowDownButton;
		if(Ui()->DoButton_FontIcon(&s_SlowDownButton, FontIcon::BACKWARD, 0, &SlowDownButton, BUTTONFLAG_LEFT))
			m_Speed = std::clamp(m_Speed - 1, 0, (int)(std::size(DEMO_SPEEDS) - 1));

		// paused
		CUIRect PausedButton;
		Row.VSplitLeft(20.0f, &PausedButton, &Row);
		Row.VSplitLeft(5.0f, nullptr, &Row);
		static CButtonContainer s_PausedButton;
		if(Ui()->DoButton_FontIcon(&s_PausedButton, FontIcon::PAUSE, 0, &PausedButton, BUTTONFLAG_LEFT))
			m_StartPaused ^= 1;

		// fastforward
		CUIRect FastForwardButton;
		Row.VSplitLeft(20.0f, &FastForwardButton, &Row);
		Row.VSplitLeft(8.0f, nullptr, &Row);
		static CButtonContainer s_FastForwardButton;
		if(Ui()->DoButton_FontIcon(&s_FastForwardButton, FontIcon::FORWARD, 0, &FastForwardButton, BUTTONFLAG_LEFT))
			m_Speed = std::clamp(m_Speed + 1, 0, (int)(std::size(DEMO_SPEEDS) - 1));

		// speed meter
		char aBuffer[128];
		const char *pPaused = m_StartPaused ? Localize("(paused)") : "";
		str_format(aBuffer, sizeof(aBuffer), "%s: ×%g %s", Localize("Speed"), DEMO_SPEEDS[m_Speed], pPaused);
		Ui()->DoLabel(&Row, aBuffer, 12.8f, TEXTALIGN_ML);
		Box.HSplitBottom(16.0f, &Box, nullptr);
		Box.HSplitBottom(24.0f, &Box, &Row);

		CUIRect Label, TextBox;
		Row.VSplitLeft(110.0f, &Label, &TextBox);
		TextBox.VSplitLeft(10.0f, nullptr, &TextBox);
		Ui()->DoLabel(&Label, Localize("Video name:"), 12.8f, TEXTALIGN_ML);
		Ui()->DoEditBox(&m_DemoRenderInput, &TextBox, 12.8f);

		// Warn about disconnect if online
		if(Client()->State() == IClient::STATE_ONLINE)
		{
			Box.HSplitBottom(10.0f, &Box, nullptr);
			Box.HSplitBottom(20.0f, &Box, &Row);
			SLabelProperties LabelProperties;
			LabelProperties.SetColor(ColorRGBA(1.0f, 0.0f, 0.0f));
			Ui()->DoLabel(&Row, Localize("You will be disconnected from the server."), 12.8f, TEXTALIGN_MC, LabelProperties);
		}
	}
	else if(m_Popup == POPUP_RENDER_DONE)
	{
		CUIRect Ok, OpenFolder;

		char aFilePath[IO_MAX_PATH_LENGTH];
		char aSaveFolder[IO_MAX_PATH_LENGTH];
		Storage()->GetCompletePath(IStorage::TYPE_SAVE, "videos", aSaveFolder, sizeof(aSaveFolder));
		str_format(aFilePath, sizeof(aFilePath), "%s/%s.mp4", aSaveFolder, m_DemoRenderInput.GetString());

		Box.HSplitBottom(20.f, &Box, &Part);
		Box.HSplitBottom(24.f, &Box, &Part);
		Part.VMargin(80.0f, &Part);

		Part.VSplitMid(&OpenFolder, &Ok);

		Ok.VMargin(20.0f, &Ok);
		OpenFolder.VMargin(20.0f, &OpenFolder);

		static CButtonContainer s_ButtonOpenFolder;
		if(DoButton_Menu(&s_ButtonOpenFolder, Localize("Videos directory"), 0, &OpenFolder))
		{
			Client()->ViewFile(aSaveFolder);
		}

		static CButtonContainer s_ButtonOk;
		if(DoButton_Menu(&s_ButtonOk, Localize("Ok"), 0, &Ok) || Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER))
		{
			m_Popup = POPUP_NONE;
			m_DemoRenderInput.Clear();
		}

		Box.HSplitBottom(160.f, &Box, &Part);
		Part.VMargin(20.0f, &Part);

		str_format(aBuf, sizeof(aBuf), Localize("Video was saved to '%s'"), aFilePath);

		SLabelProperties MessageProps;
		MessageProps.m_MaxWidth = (int)Part.w;
		Ui()->DoLabel(&Part, aBuf, 18.0f, TEXTALIGN_TL, MessageProps);
	}
#endif
	else if(m_Popup == POPUP_FIRST_LAUNCH)
	{
		CUIRect Label, TextBox, Skip, Join;

		Box.HSplitBottom(20.f, &Box, &Part);
		Box.HSplitBottom(24.f, &Box, &Part);
		Part.VMargin(80.0f, &Part);
		Part.VSplitMid(&Skip, &Join);
		Skip.VMargin(20.0f, &Skip);
		Join.VMargin(20.0f, &Join);

		static CButtonContainer s_JoinTutorialButton;
		if(DoButton_Menu(&s_JoinTutorialButton, Localize("Join Tutorial Server"), 0, &Join) || Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER))
		{
			Client()->RequestDDNetInfo();
			m_Popup = g_Config.m_BrIndicateFinished ? POPUP_POINTS : POPUP_NONE;
			JoinTutorial();
		}

		static CButtonContainer s_SkipTutorialButton;
		if(DoButton_Menu(&s_SkipTutorialButton, Localize("Skip Tutorial"), 0, &Skip) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
		{
			Client()->RequestDDNetInfo();
			m_Popup = g_Config.m_BrIndicateFinished ? POPUP_POINTS : POPUP_NONE;
		}

		Box.HSplitBottom(20.f, &Box, &Part);
		Box.HSplitBottom(24.f, &Box, &Part);

		Part.VSplitLeft(30.0f, nullptr, &Part);
		str_format(aBuf, sizeof(aBuf), "%s\n(%s)",
			Localize("Show DDNet map finishes in server browser"),
			Localize("transmits your player name to info.ddnet.org"));

		if(DoButton_CheckBox(&g_Config.m_BrIndicateFinished, aBuf, g_Config.m_BrIndicateFinished, &Part))
			g_Config.m_BrIndicateFinished ^= 1;

		Box.HSplitBottom(20.f, &Box, &Part);
		Box.HSplitBottom(24.f, &Box, &Part);

		Part.VSplitLeft(60.0f, nullptr, &Label);
		Label.VSplitLeft(100.0f, nullptr, &TextBox);
		TextBox.VSplitLeft(20.0f, nullptr, &TextBox);
		TextBox.VSplitRight(60.0f, &TextBox, nullptr);
		Ui()->DoLabel(&Label, Localize("Nickname"), 16.0f, TEXTALIGN_ML);
		static CLineInput s_PlayerNameInput(g_Config.m_PlayerName, sizeof(g_Config.m_PlayerName));
		s_PlayerNameInput.SetEmptyText(Client()->PlayerName());
		Ui()->DoEditBox(&s_PlayerNameInput, &TextBox, 12.0f);
	}
	else if(m_Popup == POPUP_JOIN_TUTORIAL)
	{
		CUIRect ButtonBar, StatusLabel, ProgressLabel, ProgressIndicator;
		Box.HSplitBottom(20.0f, &Box, nullptr);
		Box.HSplitBottom(24.0f, &Box, &ButtonBar);
		ButtonBar.VMargin(120.0f, &ButtonBar);
		Box.HSplitBottom(20.0f, &StatusLabel, nullptr);
		StatusLabel.VMargin(20.0f, &StatusLabel);
		StatusLabel.HSplitMid(&StatusLabel, &ProgressLabel);
		ProgressLabel.VSplitLeft(50.0f, &ProgressIndicator, &ProgressLabel);

		if(m_JoinTutorial.m_Status == CJoinTutorial::EStatus::REFRESHING)
		{
			if(ServerBrowser()->IsGettingServerlist() ||
				Client()->InfoState() == IClient::EInfoState::LOADING)
			{
				// Still refreshing
			}
			else if(ServerBrowser()->IsServerlistError() ||
				Client()->InfoState() == IClient::EInfoState::ERROR)
			{
				m_JoinTutorial.m_Status = CJoinTutorial::EStatus::SERVER_LIST_ERROR;
			}
			else
			{
				const char *pAddr = ServerBrowser()->GetTutorialServer();
				if(pAddr)
				{
					Client()->Connect(pAddr);
				}
				else
				{
					m_JoinTutorial.m_Status = CJoinTutorial::EStatus::NO_TUTORIAL_AVAILABLE;
				}
			}
		}

		const char *pStatusLabel = nullptr;
		switch(m_JoinTutorial.m_Status)
		{
		case CJoinTutorial::EStatus::REFRESHING:
			pStatusLabel = Localize("Getting server list from master server");
			break;
		case CJoinTutorial::EStatus::SERVER_LIST_ERROR:
			pStatusLabel = Localize("Could not get server list from master server");
			break;
		case CJoinTutorial::EStatus::NO_TUTORIAL_AVAILABLE:
			pStatusLabel = Localize("There are no Tutorial servers available");
			break;
		}
		if(pStatusLabel != nullptr)
		{
			Ui()->DoLabel(&StatusLabel, pStatusLabel, 20.0f, TEXTALIGN_ML);
		}

		const char *pProgressLabel = nullptr;
		bool ProgressDeterminate = true;
		const float LastStateChangeSeconds = std::chrono::duration_cast<std::chrono::duration<float>>(time_get_nanoseconds() - m_JoinTutorial.m_StateChange).count();
		constexpr float RefreshDelay = 5.0f;

		if(m_JoinTutorial.m_Status == CJoinTutorial::EStatus::REFRESHING)
		{
			pProgressLabel = Localize("Please wait…");
			ProgressDeterminate = false;
		}
		else if(!m_JoinTutorial.m_TryRefresh)
		{
			if(!m_JoinTutorial.m_TriedRefresh)
			{
				m_JoinTutorial.m_TryRefresh = true;
				m_JoinTutorial.m_StateChange = time_get_nanoseconds();
			}
			else if(m_JoinTutorial.m_LocalServerState == CJoinTutorial::ELocalServerState::NOT_TRIED)
			{
				m_JoinTutorial.m_LocalServerState = CJoinTutorial::ELocalServerState::TRY;
				m_JoinTutorial.m_StateChange = time_get_nanoseconds();
			}
		}

		if(m_JoinTutorial.m_TryRefresh)
		{
			if(LastStateChangeSeconds >= RefreshDelay)
			{
				// Activate internet tab before joining tutorial to make sure the server info
				// for the tutorial servers is available.
				GameClient()->m_Menus.SetMenuPage(CMenus::PAGE_INTERNET);
				GameClient()->m_Menus.RefreshBrowserTab(true);
				m_JoinTutorial.m_Status = CJoinTutorial::EStatus::REFRESHING;
				m_JoinTutorial.m_TryRefresh = false;
				m_JoinTutorial.m_TriedRefresh = true;
				m_JoinTutorial.m_StateChange = time_get_nanoseconds();
			}
			else
			{
				pProgressLabel = Localize("Retrying…");
			}
		}

		const auto &&ShowFinalErrorMessage = [&]() {
			PopupMessage(Localize("Error joining Tutorial server"), Localize("Could not find a Tutorial server. Check your internet connection."), Localize("Ok"));
		};
		const auto &&RunServer = [&]() {
			char aMotd[256];
			str_copy(aMotd, "sv_motd \"");
			char *pDst = aMotd + str_length(aMotd);
			str_escape(&pDst, Localize("You're playing on a local server because no online Tutorial server could be found.\n\nYour record will only be saved locally."), aMotd + sizeof(aMotd) - 1);
			str_append(aMotd, "\"");
			if(GameClient()->m_LocalServer.RunServer({"sv_register 0", "sv_map Tutorial", aMotd}))
			{
				m_JoinTutorial.m_LocalServerState = CJoinTutorial::ELocalServerState::WAITING_START;
				m_JoinTutorial.m_StateChange = time_get_nanoseconds();
			}
			else
			{
				ShowFinalErrorMessage();
			}
		};
		if(m_JoinTutorial.m_LocalServerState == CJoinTutorial::ELocalServerState::TRY)
		{
			if(LastStateChangeSeconds >= RefreshDelay)
			{
				if(GameClient()->m_LocalServer.IsServerRunning())
				{
					GameClient()->m_LocalServer.KillServer();
					m_JoinTutorial.m_LocalServerState = CJoinTutorial::ELocalServerState::WAITING_STOP;
					m_JoinTutorial.m_StateChange = time_get_nanoseconds();
				}
				else
				{
					RunServer();
				}
			}
			else
			{
				pProgressLabel = Localize("Could not find online Tutorial server.\nStarting and connecting to local server…");
			}
		}
		else if(m_JoinTutorial.m_LocalServerState == CJoinTutorial::ELocalServerState::WAITING_STOP)
		{
			if(LastStateChangeSeconds >= 5.0f)
			{
				ShowFinalErrorMessage();
			}
			else
			{
				if(!GameClient()->m_LocalServer.IsServerRunning())
				{
					RunServer();
				}

				pProgressLabel = Localize("Waiting for local server to stop…");
				ProgressDeterminate = false;
			}
		}
		else if(m_JoinTutorial.m_LocalServerState == CJoinTutorial::ELocalServerState::WAITING_START)
		{
			if(LastStateChangeSeconds >= 5.0f)
			{
				ShowFinalErrorMessage();
			}
			else
			{
				if(LastStateChangeSeconds >= 2.0f &&
					GameClient()->m_LocalServer.IsServerRunning())
				{
					Client()->Connect("localhost");
				}

				pProgressLabel = Localize("Waiting for local server to start…");
				ProgressDeterminate = false;
			}
		}

		if(pProgressLabel != nullptr)
		{
			Ui()->RenderProgressSpinner(ProgressIndicator.Center(), 12.0f, {.m_Progress = ProgressDeterminate ? (LastStateChangeSeconds / RefreshDelay) : -1.0f});
			Ui()->DoLabel(&ProgressLabel, pProgressLabel, 20.0f, TEXTALIGN_ML);
		}

		static CButtonContainer s_Button;
		if(DoButton_Menu(&s_Button, Localize("Cancel"), 0, &ButtonBar) ||
			Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE) ||
			Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER))
		{
			m_Popup = POPUP_NONE;
		}
	}
	else if(m_Popup == POPUP_POINTS)
	{
		Box.HSplitBottom(20.0f, &Box, nullptr);
		Box.HSplitBottom(24.0f, &Box, &Part);
		Part.VMargin(120.0f, &Part);

		if(Client()->InfoState() == IClient::EInfoState::SUCCESS && Client()->Points() > 50)
		{
			CUIRect Yes, No;
			Part.VSplitMid(&No, &Yes, 40.0f);
			static CButtonContainer s_ButtonNo;
			if(DoButton_Menu(&s_ButtonNo, Localize("No"), 0, &No) ||
				Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
			{
				m_Popup = POPUP_FIRST_LAUNCH;
			}

			static CButtonContainer s_ButtonYes;
			if(DoButton_Menu(&s_ButtonYes, Localize("Yes"), 0, &Yes) ||
				Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER))
			{
				m_Popup = POPUP_NONE;
			}
		}
		else
		{
			static CButtonContainer s_Button;
			if(DoButton_Menu(&s_Button, Localize("Cancel"), 0, &Part) ||
				Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE) ||
				Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER) ||
				Client()->InfoState() == IClient::EInfoState::SUCCESS)
			{
				m_Popup = POPUP_NONE;
			}
			if(Client()->InfoState() == IClient::EInfoState::ERROR)
			{
				PopupMessage(Localize("Error checking player name"), Localize("Could not check for existing player with your name. Check your internet connection."), Localize("Ok"));
			}
		}
	}
	else if(m_Popup == POPUP_WARNING)
	{
		Box.HSplitBottom(20.f, &Box, &Part);
		Box.HSplitBottom(24.f, &Box, &Part);
		Part.VMargin(120.0f, &Part);

		static CButtonContainer s_Button;
		if(DoButton_Menu(&s_Button, pButtonText, 0, &Part) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE) || Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER) || (m_PopupWarningDuration > 0s && time_get_nanoseconds() - m_PopupWarningLastTime >= m_PopupWarningDuration))
		{
			m_Popup = POPUP_NONE;
			SetActive(false);
		}
	}
	else if(m_Popup == POPUP_SAVE_SKIN)
	{
		CUIRect Label, TextBox, Yes, No;

		Box.HSplitBottom(20.f, &Box, &Part);
		Box.HSplitBottom(24.f, &Box, &Part);
		Part.VMargin(80.0f, &Part);

		Part.VSplitMid(&No, &Yes);

		Yes.VMargin(20.0f, &Yes);
		No.VMargin(20.0f, &No);

		static CButtonContainer s_ButtonNo;
		if(DoButton_Menu(&s_ButtonNo, Localize("No"), 0, &No) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
			m_Popup = POPUP_NONE;

		static CButtonContainer s_ButtonYes;
		if(DoButton_Menu(&s_ButtonYes, Localize("Yes"), m_SkinNameInput.IsEmpty() ? 1 : 0, &Yes) || Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER))
		{
			if(!str_valid_filename(m_SkinNameInput.GetString()))
			{
				PopupMessage(Localize("Error"), Localize("This name cannot be used for files and folders"), Localize("Ok"), POPUP_SAVE_SKIN);
			}
			else if(CSkins7::IsSpecialSkin(m_SkinNameInput.GetString()))
			{
				PopupMessage(Localize("Error"), Localize("Unable to save the skin with a reserved name"), Localize("Ok"), POPUP_SAVE_SKIN);
			}
			else if(!GameClient()->m_Skins7.SaveSkinfile(m_SkinNameInput.GetString(), m_Dummy))
			{
				PopupMessage(Localize("Error"), Localize("Unable to save the skin"), Localize("Ok"), POPUP_SAVE_SKIN);
			}
			else
			{
				m_Popup = POPUP_NONE;
				m_SkinList7LastRefreshTime = std::nullopt;
			}
		}

		Box.HSplitBottom(60.f, &Box, &Part);
		Box.HSplitBottom(24.f, &Box, &Part);

		Part.VMargin(60.0f, &Label);
		Label.VSplitLeft(100.0f, &Label, &TextBox);
		TextBox.VSplitLeft(20.0f, nullptr, &TextBox);
		Ui()->DoLabel(&Label, Localize("Name"), 18.0f, TEXTALIGN_ML);
		Ui()->DoClearableEditBox(&m_SkinNameInput, &TextBox, 12.0f);
	}
	else
	{
		Box.HSplitBottom(20.f, &Box, &Part);
		Box.HSplitBottom(24.f, &Box, &Part);
		Part.VMargin(120.0f, &Part);

		static CButtonContainer s_Button;
		if(DoButton_Menu(&s_Button, pButtonText, 0, &Part) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE) || Ui()->ConsumeHotkey(CUi::HOTKEY_ENTER))
		{
			if(m_Popup == POPUP_DISCONNECTED && Client()->ReconnectTime() > 0)
				Client()->SetReconnectTime(0);
			m_Popup = POPUP_NONE;
		}
	}

	if(m_Popup == POPUP_NONE)
		Ui()->SetActiveItem(nullptr);
}

void CMenus::RenderPopupConnecting(CUIRect Screen)
{
	const float FontSize = 20.0f;

	CUIRect Box, Label;
	Screen.Margin(150.0f, &Box);
	Box.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f), IGraphics::CORNER_ALL, 15.0f);
	Box.Margin(20.0f, &Box);

	Box.HSplitTop(24.0f, &Label, &Box);
	Ui()->DoLabel(&Label, Localize("Connecting to"), 24.0f, TEXTALIGN_MC);

	Box.HSplitTop(20.0f, nullptr, &Box);
	Box.HSplitTop(24.0f, &Label, &Box);
	SLabelProperties Props;
	Props.m_MaxWidth = Label.w;
	Props.m_EllipsisAtEnd = true;
	Ui()->DoLabel(&Label, Client()->ConnectAddressString(), FontSize, TEXTALIGN_MC, Props);

	if(time_get() - Client()->StateStartTime() > time_freq())
	{
		const char *pConnectivityLabel = "";
		switch(Client()->UdpConnectivity(Client()->ConnectNetTypes()))
		{
		case IClient::CONNECTIVITY_UNKNOWN:
			break;
		case IClient::CONNECTIVITY_CHECKING:
			pConnectivityLabel = Localize("Trying to determine UDP connectivity…");
			break;
		case IClient::CONNECTIVITY_UNREACHABLE:
			pConnectivityLabel = Localize("UDP seems to be filtered.");
			break;
		case IClient::CONNECTIVITY_DIFFERING_UDP_TCP_IP_ADDRESSES:
			pConnectivityLabel = Localize("UDP and TCP IP addresses seem to be different. Try disabling VPN, proxy or network accelerators.");
			break;
		case IClient::CONNECTIVITY_REACHABLE:
			pConnectivityLabel = Localize("No answer from server yet.");
			break;
		}
		if(pConnectivityLabel[0] != '\0')
		{
			Box.HSplitTop(20.0f, nullptr, &Box);
			Box.HSplitTop(24.0f, &Label, &Box);
			SLabelProperties ConnectivityLabelProps;
			ConnectivityLabelProps.m_MaxWidth = Label.w;
			if(TextRender()->TextWidth(FontSize, pConnectivityLabel) > Label.w)
				Ui()->DoLabel(&Label, pConnectivityLabel, FontSize, TEXTALIGN_ML, ConnectivityLabelProps);
			else
				Ui()->DoLabel(&Label, pConnectivityLabel, FontSize, TEXTALIGN_MC);
		}
	}

	CUIRect Button;
	Box.HSplitBottom(24.0f, &Box, &Button);
	Button.VMargin(100.0f, &Button);

	static CButtonContainer s_Button;
	if(DoButton_Menu(&s_Button, Localize("Abort"), 0, &Button) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
	{
		Client()->Disconnect();
		Ui()->SetActiveItem(nullptr);
		RefreshBrowserTab(true);
	}
}

void CMenus::RenderPopupLoading(CUIRect Screen)
{
	char aTitle[256];
	char aLabel1[128];
	char aLabel2[128];
	if(Client()->MapDownloadTotalsize() > 0)
	{
		const int64_t Now = time_get();
		if(Now - m_DownloadLastCheckTime >= time_freq())
		{
			if(m_DownloadLastCheckSize > Client()->MapDownloadAmount())
			{
				// map downloaded restarted
				m_DownloadLastCheckSize = 0;
			}

			// update download speed
			const float Diff = (Client()->MapDownloadAmount() - m_DownloadLastCheckSize) / ((int)((Now - m_DownloadLastCheckTime) / time_freq()));
			const float StartDiff = m_DownloadLastCheckSize - 0.0f;
			if(StartDiff + Diff > 0.0f)
				m_DownloadSpeed = (Diff / (StartDiff + Diff)) * (Diff / 1.0f) + (StartDiff / (Diff + StartDiff)) * m_DownloadSpeed;
			else
				m_DownloadSpeed = 0.0f;
			m_DownloadLastCheckTime = Now;
			m_DownloadLastCheckSize = Client()->MapDownloadAmount();
		}

		str_format(aTitle, sizeof(aTitle), "%s: %s", Localize("Downloading map"), Client()->MapDownloadName());

		str_format(aLabel1, sizeof(aLabel1), Localize("%d/%d KiB (%.1f KiB/s)"), Client()->MapDownloadAmount() / 1024, Client()->MapDownloadTotalsize() / 1024, m_DownloadSpeed / 1024.0f);

		const int SecondsLeft = maximum(1, m_DownloadSpeed > 0.0f ? static_cast<int>((Client()->MapDownloadTotalsize() - Client()->MapDownloadAmount()) / m_DownloadSpeed) : 1);
		const int MinutesLeft = SecondsLeft / 60;
		if(MinutesLeft > 0)
		{
			str_format(aLabel2, sizeof(aLabel2), MinutesLeft == 1 ? Localize("%i minute left") : Localize("%i minutes left"), MinutesLeft);
		}
		else
		{
			str_format(aLabel2, sizeof(aLabel2), SecondsLeft == 1 ? Localize("%i second left") : Localize("%i seconds left"), SecondsLeft);
		}
	}
	else
	{
		str_copy(aTitle, Localize("Connected"));
		switch(Client()->LoadingStateDetail())
		{
		case IClient::LOADING_STATE_DETAIL_INITIAL:
			str_copy(aLabel1, Localize("Getting game info"));
			break;
		case IClient::LOADING_STATE_DETAIL_LOADING_MAP:
			str_copy(aLabel1, Localize("Loading map file from storage"));
			break;
		case IClient::LOADING_STATE_DETAIL_LOADING_DEMO:
			str_copy(aLabel1, Localize("Loading demo file from storage"));
			break;
		case IClient::LOADING_STATE_DETAIL_SENDING_READY:
			str_copy(aLabel1, Localize("Requesting to join the game"));
			break;
		case IClient::LOADING_STATE_DETAIL_GETTING_READY:
			str_copy(aLabel1, Localize("Sending initial client info"));
			break;
		default:
			dbg_assert_failed("Invalid loading state %d for RenderPopupLoading", static_cast<int>(Client()->LoadingStateDetail()));
		}
		aLabel2[0] = '\0';
	}

	const float FontSize = 20.0f;

	CUIRect Box, Label;
	Screen.Margin(150.0f, &Box);
	Box.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f), IGraphics::CORNER_ALL, 15.0f);
	Box.Margin(20.0f, &Box);

	Box.HSplitTop(24.0f, &Label, &Box);
	Ui()->DoLabel(&Label, aTitle, 24.0f, TEXTALIGN_MC);

	Box.HSplitTop(20.0f, nullptr, &Box);
	Box.HSplitTop(24.0f, &Label, &Box);
	Ui()->DoLabel(&Label, aLabel1, FontSize, TEXTALIGN_MC);

	if(aLabel2[0] != '\0')
	{
		Box.HSplitTop(20.0f, nullptr, &Box);
		Box.HSplitTop(24.0f, &Label, &Box);
		SLabelProperties ExtraTextProps;
		ExtraTextProps.m_MaxWidth = Label.w;
		if(TextRender()->TextWidth(FontSize, aLabel2) > Label.w)
			Ui()->DoLabel(&Label, aLabel2, FontSize, TEXTALIGN_ML, ExtraTextProps);
		else
			Ui()->DoLabel(&Label, aLabel2, FontSize, TEXTALIGN_MC);
	}

	if(Client()->MapDownloadTotalsize() > 0)
	{
		CUIRect ProgressBar;
		Box.HSplitTop(20.0f, nullptr, &Box);
		Box.HSplitTop(24.0f, &ProgressBar, &Box);
		ProgressBar.VMargin(20.0f, &ProgressBar);
		Ui()->RenderProgressBar(ProgressBar, Client()->MapDownloadAmount() / (float)Client()->MapDownloadTotalsize());
	}

	CUIRect Button;
	Box.HSplitBottom(24.0f, &Box, &Button);
	Button.VMargin(100.0f, &Button);

	static CButtonContainer s_Button;
	if(DoButton_Menu(&s_Button, Localize("Abort"), 0, &Button) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
	{
		Client()->Disconnect();
		Ui()->SetActiveItem(nullptr);
		RefreshBrowserTab(true);
	}
}

#if defined(CONF_VIDEORECORDER)
void CMenus::PopupConfirmDemoReplaceVideo()
{
	char aBuf[IO_MAX_PATH_LENGTH];
	str_format(aBuf, sizeof(aBuf), "%s/%s.demo", m_aCurrentDemoFolder, m_aCurrentDemoSelectionName);
	char aVideoName[IO_MAX_PATH_LENGTH];
	str_copy(aVideoName, m_DemoRenderInput.GetString());
	const char *pError = Client()->DemoPlayer_Render(aBuf, m_DemolistStorageType, aVideoName, m_Speed, m_StartPaused);
	m_Speed = DEMO_SPEED_INDEX_DEFAULT;
	m_StartPaused = false;
	m_LastPauseChange = -1.0f;
	m_LastSpeedChange = -1.0f;
	if(pError)
	{
		m_DemoRenderInput.Clear();
		PopupMessage(Localize("Error loading demo"), pError, Localize("Ok"));
	}
}
#endif

void CMenus::RenderThemeSelection(CUIRect MainView)
{
	const std::vector<CTheme> &vThemes = GameClient()->m_MenuBackground.GetThemes();

	int SelectedTheme = -1;
	for(int i = 0; i < (int)vThemes.size(); i++)
	{
		if(str_comp(vThemes[i].m_Name.c_str(), g_Config.m_ClMenuMap) == 0)
		{
			SelectedTheme = i;
			break;
		}
	}
	const int OldSelected = SelectedTheme;

	static CListBox s_ListBox;
	s_ListBox.DoHeader(&MainView, Localize("Theme"), 20.0f);
	s_ListBox.DoStart(20.0f, vThemes.size(), 1, 3, SelectedTheme);

	for(int i = 0; i < (int)vThemes.size(); i++)
	{
		const CTheme &Theme = vThemes[i];
		const CListboxItem Item = s_ListBox.DoNextItem(&Theme.m_Name, i == SelectedTheme);

		if(!Item.m_Visible)
			continue;

		CUIRect Icon, Label;
		Item.m_Rect.VSplitLeft(Item.m_Rect.h * 2.0f, &Icon, &Label);

		// draw icon if it exists
		if(Theme.m_IconTexture.IsValid())
		{
			Icon.VMargin(6.0f, &Icon);
			Icon.HMargin(3.0f, &Icon);
			Graphics()->TextureSet(Theme.m_IconTexture);
			Graphics()->QuadsBegin();
			Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
			IGraphics::CQuadItem QuadItem(Icon.x, Icon.y, Icon.w, Icon.h);
			Graphics()->QuadsDrawTL(&QuadItem, 1);
			Graphics()->QuadsEnd();
		}

		char aName[128];
		if(Theme.m_Name.empty())
			str_copy(aName, "(none)");
		else if(str_comp(Theme.m_Name.c_str(), "auto") == 0)
			str_copy(aName, "(seasons)");
		else if(str_comp(Theme.m_Name.c_str(), "rand") == 0)
			str_copy(aName, "(random)");
		else if(Theme.m_HasDay && Theme.m_HasNight)
			str_copy(aName, Theme.m_Name.c_str());
		else if(Theme.m_HasDay && !Theme.m_HasNight)
			str_format(aName, sizeof(aName), "%s (day)", Theme.m_Name.c_str());
		else if(!Theme.m_HasDay && Theme.m_HasNight)
			str_format(aName, sizeof(aName), "%s (night)", Theme.m_Name.c_str());
		else // generic
			str_copy(aName, Theme.m_Name.c_str());

		Ui()->DoLabel(&Label, aName, 16.0f * CUi::ms_FontmodHeight, TEXTALIGN_ML);
	}

	SelectedTheme = s_ListBox.DoEnd();

	if(OldSelected != SelectedTheme)
	{
		const CTheme &Theme = vThemes[SelectedTheme];
		str_copy(g_Config.m_ClMenuMap, Theme.m_Name.c_str());
		GameClient()->m_MenuBackground.LoadMenuBackground(Theme.m_HasDay, Theme.m_HasNight);
	}
}

void CMenus::SetActive(bool Active)
{
	if(Active != m_MenuActive)
	{
		Ui()->SetHotItem(nullptr);
		Ui()->SetActiveItem(nullptr);
	}
	m_MenuActive = Active;
	if(!m_MenuActive)
	{
		if(m_NeedSendinfo)
		{
			GameClient()->SendInfo(false);
			m_NeedSendinfo = false;
		}

		if(m_NeedSendDummyinfo)
		{
			GameClient()->SendDummyInfo(false);
			m_NeedSendDummyinfo = false;
		}

		if(Client()->State() == IClient::STATE_ONLINE)
		{
			GameClient()->OnRelease();
		}
	}
	else if(Client()->State() == IClient::STATE_DEMOPLAYBACK)
	{
		GameClient()->OnRelease();
	}
}

void CMenus::OnReset()
{
}

void CMenus::OnShutdown()
{
	if(m_pClientChatFetchTask)
		m_pClientChatFetchTask->Abort();
	if(m_pClientChatSendTask)
		m_pClientChatSendTask->Abort();
	if(m_pClientChatActionTask)
		m_pClientChatActionTask->Abort();
	m_pClientChatFetchTask.reset();
	m_pClientChatSendTask.reset();
	m_pClientChatActionTask.reset();
	m_CommunityIcons.Shutdown();
}

namespace
{
float ZzClickGuiEaseOutBack(float T)
{
	T = std::clamp(T, 0.0f, 1.0f);
	const float C1 = 1.70158f;
	const float C3 = C1 + 1.0f;
	return 1.0f + C3 * powf(T - 1.0f, 3.0f) + C1 * powf(T - 1.0f, 2.0f);
}

float ZzClickGuiSmoothStep(float T)
{
	T = std::clamp(T, 0.0f, 1.0f);
	return T * T * (3.0f - 2.0f * T);
}
}

void CMenus::RenderZzClickGuiOverlay()
{
	Ui()->MapScreen();
	const float Anim = std::clamp(m_ZzClickGuiAnim, 0.0f, 1.0f);
	if(Anim <= 0.001f)
		return;
	const float ShowEase = ZzClickGuiEaseOutBack(Anim);
	const float AlphaEase = ZzClickGuiSmoothStep(Anim);
	const float ContentEase = ZzClickGuiSmoothStep(std::clamp((Anim - 0.22f) / 0.78f, 0.0f, 1.0f));

	CUIRect Screen = *Ui()->Screen();
	Graphics()->TextureClear();
	Graphics()->DrawRect(Screen.x, Screen.y, Screen.w, Screen.h, ColorRGBA(0.03f, 0.03f, 0.05f, 0.56f * AlphaEase), IGraphics::CORNER_ALL, 0.0f);

	CUIRect Window;
	Screen.Margin(10.0f, &Window);
	const float MaxW = maximum(250.0f, minimum(Window.w, Screen.w * 0.67f));
	const float MaxH = maximum(190.0f, minimum(Window.h, Screen.h * 0.78f));
	const float Scale = 0.88f + 0.12f * ShowEase;
	const float FinalX = Screen.x + (Screen.w - MaxW) * 0.5f;
	const float FinalY = Screen.y + (Screen.h - MaxH) * 0.5f;
	const float WinW = MaxW * Scale;
	const float WinH = MaxH * Scale;
	const float WinX = FinalX + (MaxW - WinW) * 0.5f;
	const float WinY = FinalY + (MaxH - WinH) * 0.5f + (1.0f - ShowEase) * 10.0f;
	Window.x = WinX;
	Window.y = WinY;
	Window.w = WinW;
	Window.h = WinH;

	Graphics()->DrawRect(Window.x - 1.0f, Window.y - 1.0f, Window.w + 2.0f, Window.h + 2.0f, ColorRGBA(0.16f, 0.18f, 0.22f, 0.72f * AlphaEase), IGraphics::CORNER_ALL, 13.0f);
	Window.Draw(ColorRGBA(0.055f, 0.055f, 0.063f, 0.98f * AlphaEase), IGraphics::CORNER_ALL, 12.0f);

	CUIRect Header, Content;
	Window.HSplitTop(25.0f, &Header, &Content);
	Header.Draw(ColorRGBA(0.094f, 0.094f, 0.106f, 0.96f * AlphaEase), IGraphics::CORNER_T, 12.0f);
	Graphics()->DrawRect(Header.x, Header.y + Header.h - 1.0f, Header.w, 1.0f, ColorRGBA(0.345f, 0.396f, 0.949f, 0.85f * AlphaEase), IGraphics::CORNER_NONE, 0.0f);

	CUIRect Sidebar, Main;
	Content.VSplitLeft(36.0f, &Sidebar, &Main);
	Sidebar.Draw(ColorRGBA(0.094f, 0.094f, 0.106f, (0.90f * AlphaEase) * ContentEase), IGraphics::CORNER_BL, 12.0f);
	Main.Margin(7.0f, &Main);
	Sidebar.Margin(5.0f, &Sidebar);

	CUIRect HeaderText = Header;
	HeaderText.VMargin(8.0f, &HeaderText);
	Ui()->DoLabel(&HeaderText, "ZZ ClickGUI", 7.5f, TEXTALIGN_ML, {.m_MaxWidth = HeaderText.w});

	CUIRect HeaderRight = HeaderText;
	HeaderRight.VSplitLeft(HeaderText.w - 58.0f, nullptr, &HeaderRight);
	Ui()->DoLabel(&HeaderRight, "[Insert/Esc]", 6.0f, TEXTALIGN_MR, {.m_MaxWidth = HeaderRight.w});

	CUIRect SideLabel = Sidebar;
	SideLabel.HSplitTop(16.0f, &SideLabel, &Sidebar);
	Ui()->DoLabel(&SideLabel, "ZZ", 9.5f, TEXTALIGN_MC, {.m_MaxWidth = SideLabel.w});

	CUIRect SideMode = Sidebar;
	SideMode.HSplitBottom(16.0f, &Sidebar, &SideMode);
	const bool HudEditActive = g_Config.m_ClZzHudEditMode != 0;
	static CButtonContainer s_HudEditModeToggleBtn;
	const char *pHudBtnText = HudEditActive ? "Exit" : "Edit";
	if(DoButton_Menu(&s_HudEditModeToggleBtn, pHudBtnText, 0, &SideMode, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_ALL, 4.0f, 0.0f,
		   HudEditActive ? ColorRGBA(0.58f, 0.23f, 0.36f, 0.92f * ContentEase) : ColorRGBA(0.345f, 0.396f, 0.949f, 0.92f * ContentEase)))
	{
		g_Config.m_ClZzHudEditMode = HudEditActive ? 0 : 1;
		if(!g_Config.m_ClZzHudEditMode)
			(void)ConfigManager()->Save();
	}

	if(ContentEase > 0.01f)
	{
		CUIRect AnimatedMain = Main;
		AnimatedMain.x += (1.0f - ContentEase) * 26.0f;
		RenderSettingsZZetClient(AnimatedMain);
	}
}

void CMenus::ConToggleZzClickGui(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	CMenus *pSelf = (CMenus *)pUserData;
	if(pSelf == nullptr || pSelf->Client()->State() != IClient::STATE_ONLINE)
		return;

	pSelf->m_ZzClickGuiActive ^= 1;
	pSelf->Ui()->SetHotItem(nullptr);
	pSelf->Ui()->SetActiveItem(nullptr);
	if(pSelf->m_ZzClickGuiActive && pSelf->m_MenuActive)
		pSelf->SetActive(false);
}

bool CMenus::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(!m_MenuActive && !m_ZzClickGuiActive && m_ZzClickGuiAnim <= 0.001f && !g_Config.m_ClZzHudEditMode)
		return false;

	Ui()->ConvertMouseMove(&x, &y, CursorType);
	Ui()->OnCursorMove(x, y);

	return true;
}

bool CMenus::OnInput(const IInput::CEvent &Event)
{
	if((Event.m_Flags & IInput::FLAG_PRESS) != 0 &&
		(Event.m_Key == KEY_INSERT || Event.m_Key == KEY_KP_0) &&
		Client()->State() == IClient::STATE_ONLINE)
	{
		ConToggleZzClickGui(nullptr, this);
		return true;
	}

	if(g_Config.m_ClZzHudEditMode && !m_ZzClickGuiActive && !m_MenuActive)
	{
		if((Event.m_Flags & IInput::FLAG_PRESS) != 0 && Event.m_Key == KEY_ESCAPE)
		{
			g_Config.m_ClZzHudEditMode = 0;
			(void)ConfigManager()->Save();
			return true;
		}

		Ui()->OnInput(Event);
		if(Event.m_Key == KEY_MOUSE_1 || Event.m_Key == KEY_MOUSE_2 || Event.m_Key == KEY_MOUSE_3 ||
			Event.m_Key == KEY_MOUSE_4 || Event.m_Key == KEY_MOUSE_5 ||
			Event.m_Key == KEY_MOUSE_WHEEL_UP || Event.m_Key == KEY_MOUSE_WHEEL_DOWN)
			return true;
	}

	if(m_ZzClickGuiActive || m_ZzClickGuiAnim > 0.001f)
	{
		if(m_ZzClickGuiActive && (Event.m_Flags & IInput::FLAG_PRESS) != 0 && Event.m_Key == KEY_ESCAPE)
		{
			m_ZzClickGuiActive = false;
			Ui()->SetHotItem(nullptr);
			Ui()->SetActiveItem(nullptr);
			return true;
		}

		if(m_ZzClickGuiActive)
			Ui()->OnInput(Event);
		return true;
	}

	// Escape key is always handled to activate/deactivate menu
	if((Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_ESCAPE) || IsActive())
	{
		Ui()->OnInput(Event);
		return true;
	}
	return false;
}

void CMenus::OnStateChange(int NewState, int OldState)
{
	// reset active item
	Ui()->SetActiveItem(nullptr);
	if(NewState != IClient::STATE_ONLINE)
	{
		m_ZzClickGuiActive = false;
		m_ZzClickGuiAnim = 0.0f;
	}

	if(OldState == IClient::STATE_ONLINE || OldState == IClient::STATE_OFFLINE)
		TextRender()->DeleteTextContainer(m_MotdTextContainerIndex);

	if(NewState == IClient::STATE_OFFLINE)
	{
		if(OldState >= IClient::STATE_ONLINE && NewState < IClient::STATE_QUITTING)
			UpdateMusicState();
		m_Popup = POPUP_NONE;
		if(Client()->ErrorString() && Client()->ErrorString()[0] != 0)
		{
			if(str_find(Client()->ErrorString(), "password"))
			{
				m_Popup = POPUP_PASSWORD;
				m_PasswordInput.SelectAll();
				Ui()->SetActiveItem(&m_PasswordInput);
			}
			else
				m_Popup = POPUP_DISCONNECTED;
		}
	}
	else if(NewState == IClient::STATE_LOADING)
	{
		m_DownloadLastCheckTime = time_get();
		m_DownloadLastCheckSize = 0;
		m_DownloadSpeed = 0.0f;
	}
	else if(NewState == IClient::STATE_ONLINE || NewState == IClient::STATE_DEMOPLAYBACK)
	{
		if(m_Popup != POPUP_WARNING)
		{
			m_Popup = POPUP_NONE;
			SetActive(false);
		}
	}
}

void CMenus::OnWindowResize()
{
	TextRender()->DeleteTextContainer(m_MotdTextContainerIndex);
}

void CMenus::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		SetActive(true);

	UpdateClientChat();

	if(Client()->State() == IClient::STATE_ONLINE && GameClient()->m_ServerMode == CGameClient::SERVERMODE_PUREMOD)
	{
		Client()->Disconnect();
		SetActive(true);
		PopupMessage(Localize("Disconnected"), Localize("The server is running a non-standard tuning on a pure game type."), Localize("Ok"));
	}

	const float ZzGuiDt = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.1f);
	const float ZzGuiTarget = m_ZzClickGuiActive ? 1.0f : 0.0f;
	m_ZzClickGuiAnim += (ZzGuiTarget - m_ZzClickGuiAnim) * minimum(1.0f, ZzGuiDt * 12.0f);
	if(std::abs(m_ZzClickGuiAnim - ZzGuiTarget) < 0.001f)
		m_ZzClickGuiAnim = ZzGuiTarget;
	const bool ZzGuiVisible = m_ZzClickGuiActive || m_ZzClickGuiAnim > 0.001f;

	if(!IsActive() && !ZzGuiVisible && !g_Config.m_ClZzHudEditMode)
	{
		if(Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
		{
			SetActive(true);
		}
		else if(Client()->State() != IClient::STATE_DEMOPLAYBACK)
		{
			Ui()->ClearHotkeys();
			return;
		}
	}

	Ui()->StartCheck();
	UpdateColors();

	Ui()->Update();

	if(g_Config.m_ClZzHudEditMode && !IsActive() && !ZzGuiVisible)
	{
		Ui()->FinishCheck();
		Ui()->ClearHotkeys();
		return;
	}

	if(ZzGuiVisible && !IsActive())
	{
		Ui()->MapScreen();
		RenderZzClickGuiOverlay();
		Ui()->RenderPopupMenus();
		RenderTools()->RenderCursor(Ui()->MousePos(), 24.0f);
		if(Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
			m_ZzClickGuiActive = false;
		Ui()->FinishCheck();
		Ui()->ClearHotkeys();
		return;
	}

	Render();

	if(IsActive())
	{
		RenderTools()->RenderCursor(Ui()->MousePos(), 24.0f);
	}

	// render debug information
	if(g_Config.m_Debug)
		Ui()->DebugRender(2.0f, Ui()->Screen()->h - 12.0f);

	if(Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
		SetActive(false);

	Ui()->FinishCheck();
	Ui()->ClearHotkeys();
}

void CMenus::UpdateColors()
{
	ms_GuiColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_UiColor, true));

	ms_ColorTabbarInactiveOutgame = ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f);
	ms_ColorTabbarActiveOutgame = ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f);
	ms_ColorTabbarHoverOutgame = ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f);

	const float ColorIngameScaleI = 0.5f;
	const float ColorIngameScaleA = 0.2f;

	ms_ColorTabbarInactiveIngame = ColorRGBA(
		ms_GuiColor.r * ColorIngameScaleI,
		ms_GuiColor.g * ColorIngameScaleI,
		ms_GuiColor.b * ColorIngameScaleI,
		ms_GuiColor.a * 0.8f);

	ms_ColorTabbarActiveIngame = ColorRGBA(
		ms_GuiColor.r * ColorIngameScaleA,
		ms_GuiColor.g * ColorIngameScaleA,
		ms_GuiColor.b * ColorIngameScaleA,
		ms_GuiColor.a);

	ms_ColorTabbarHoverIngame = ColorRGBA(1.0f, 1.0f, 1.0f, 0.75f);
}

void CMenus::RenderBackground()
{
	Graphics()->BlendNormal();

	const float ScreenHeight = 300.0f;
	const float ScreenWidth = ScreenHeight * Graphics()->ScreenAspect();
	Graphics()->MapScreen(0.0f, 0.0f, ScreenWidth, ScreenHeight);

	// render background color
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(ms_GuiColor.WithAlpha(1.0f));
	const IGraphics::CQuadItem BackgroundQuadItem = IGraphics::CQuadItem(0, 0, ScreenWidth, ScreenHeight);
	Graphics()->QuadsDrawTL(&BackgroundQuadItem, 1);
	Graphics()->QuadsEnd();

	// render the tiles
	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.045f);
	const float Size = 15.0f;
	const float OffsetTime = std::fmod(Client()->GlobalTime() * 0.15f, 2.0f);
	IGraphics::CQuadItem aCheckerItems[64];
	size_t NumCheckerItems = 0;
	const int NumItemsWidth = std::ceil(ScreenWidth / Size);
	const int NumItemsHeight = std::ceil(ScreenHeight / Size);
	for(int y = -2; y < NumItemsHeight; y++)
	{
		for(int x = 0; x < NumItemsWidth + 4; x += 2)
		{
			aCheckerItems[NumCheckerItems] = IGraphics::CQuadItem((x - 2 * OffsetTime + (y & 1)) * Size, (y + OffsetTime) * Size, Size, Size);
			NumCheckerItems++;
			if(NumCheckerItems == std::size(aCheckerItems))
			{
				Graphics()->QuadsDrawTL(aCheckerItems, NumCheckerItems);
				NumCheckerItems = 0;
			}
		}
	}
	if(NumCheckerItems != 0)
		Graphics()->QuadsDrawTL(aCheckerItems, NumCheckerItems);
	Graphics()->QuadsEnd();

	// render border fade
	Graphics()->TextureSet(m_TextureBlob);
	Graphics()->QuadsBegin();
	Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	const IGraphics::CQuadItem BlobQuadItem = IGraphics::CQuadItem(-100, -100, ScreenWidth + 200, ScreenHeight + 200);
	Graphics()->QuadsDrawTL(&BlobQuadItem, 1);
	Graphics()->QuadsEnd();

	// restore screen
	Ui()->MapScreen();
}

int CMenus::DoButton_CheckBox_Tristate(const void *pId, const char *pText, TRISTATE Checked, const CUIRect *pRect)
{
	switch(Checked)
	{
	case TRISTATE::NONE:
		return DoButton_CheckBox_Common(pId, pText, "", pRect, BUTTONFLAG_LEFT);
	case TRISTATE::SOME:
		return DoButton_CheckBox_Common(pId, pText, "O", pRect, BUTTONFLAG_LEFT);
	case TRISTATE::ALL:
		return DoButton_CheckBox_Common(pId, pText, "X", pRect, BUTTONFLAG_LEFT);
	default:
		dbg_assert_failed("Invalid tristate. Checked: %d", static_cast<int>(Checked));
	}
}

int CMenus::MenuImageScan(const char *pName, int IsDir, int DirType, void *pUser)
{
	const char *pExtension = ".png";
	CMenuImage MenuImage;
	CMenus *pSelf = static_cast<CMenus *>(pUser);
	if(IsDir || !str_endswith(pName, pExtension) || str_length(pName) - str_length(pExtension) >= (int)sizeof(MenuImage.m_aName))
		return 0;

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "menuimages/%s", pName);

	CImageInfo Info;
	if(!pSelf->Graphics()->LoadPng(Info, aPath, DirType))
	{
		char aError[IO_MAX_PATH_LENGTH + 64];
		str_format(aError, sizeof(aError), "Failed to load menu image from '%s'", aPath);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "menus", aError);
		return 0;
	}
	if(Info.m_Format != CImageInfo::FORMAT_RGBA)
	{
		Info.Free();
		char aError[IO_MAX_PATH_LENGTH + 64];
		str_format(aError, sizeof(aError), "Failed to load menu image from '%s': must be an RGBA image", aPath);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_ADDINFO, "menus", aError);
		return 0;
	}

	MenuImage.m_OrgTexture = pSelf->Graphics()->LoadTextureRaw(Info, 0, aPath);

	ConvertToGrayscale(Info);
	MenuImage.m_GreyTexture = pSelf->Graphics()->LoadTextureRawMove(Info, 0, aPath);

	str_truncate(MenuImage.m_aName, sizeof(MenuImage.m_aName), pName, str_length(pName) - str_length(pExtension));
	pSelf->m_vMenuImages.push_back(MenuImage);

	pSelf->RenderLoading(Localize("Loading DDNet Client"), Localize("Loading menu images"), 0);

	return 0;
}

const CMenus::CMenuImage *CMenus::FindMenuImage(const char *pName)
{
	for(auto &Image : m_vMenuImages)
		if(str_comp(Image.m_aName, pName) == 0)
			return &Image;
	return nullptr;
}

void CMenus::SetMenuPage(int NewPage)
{
	const int OldPage = m_MenuPage;
	m_MenuPage = NewPage;
	if(NewPage >= PAGE_INTERNET && NewPage <= PAGE_FAVORITE_COMMUNITY_5)
	{
		g_Config.m_UiPage = NewPage;
		bool ForceRefresh = false;
		if(m_ForceRefreshLanPage && NewPage == PAGE_LAN)
		{
			ForceRefresh = true;
			m_ForceRefreshLanPage = false;
		}
		if(OldPage != NewPage || ForceRefresh)
		{
			RefreshBrowserTab(ForceRefresh);
		}
	}
}

void CMenus::ConToggleClientChat(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	CMenus *pSelf = static_cast<CMenus *>(pUserData);
	if(pSelf == nullptr)
		return;

	if(pSelf->IsActive() && pSelf->IsClientChatPageActive())
	{
		pSelf->SetActive(false);
		return;
	}

	pSelf->SetActive(true);
	if(pSelf->Client()->State() == IClient::STATE_OFFLINE)
		pSelf->SetMenuPage(PAGE_CLIENT_CHAT);
	else
		pSelf->m_GamePage = PAGE_CLIENT_CHAT;
}

void CMenus::RefreshBrowserTab(bool Force)
{
	if(g_Config.m_UiPage == PAGE_INTERNET)
	{
		if(Force || ServerBrowser()->GetCurrentType() != IServerBrowser::TYPE_INTERNET)
		{
			if(Force || ServerBrowser()->GetCurrentType() == IServerBrowser::TYPE_LAN)
			{
				Client()->RequestDDNetInfo();
			}
			ServerBrowser()->Refresh(IServerBrowser::TYPE_INTERNET);
			UpdateCommunityCache(true);
		}
	}
	else if(g_Config.m_UiPage == PAGE_LAN)
	{
		if(Force || ServerBrowser()->GetCurrentType() != IServerBrowser::TYPE_LAN)
		{
			ServerBrowser()->Refresh(IServerBrowser::TYPE_LAN);
			UpdateCommunityCache(true);
		}
	}
	else if(g_Config.m_UiPage == PAGE_FAVORITES)
	{
		if(Force || ServerBrowser()->GetCurrentType() != IServerBrowser::TYPE_FAVORITES)
		{
			if(Force || ServerBrowser()->GetCurrentType() == IServerBrowser::TYPE_LAN)
			{
				Client()->RequestDDNetInfo();
			}
			ServerBrowser()->Refresh(IServerBrowser::TYPE_FAVORITES);
			UpdateCommunityCache(true);
		}
	}
	else if(g_Config.m_UiPage >= PAGE_FAVORITE_COMMUNITY_1 && g_Config.m_UiPage <= PAGE_FAVORITE_COMMUNITY_5)
	{
		const int BrowserType = g_Config.m_UiPage - PAGE_FAVORITE_COMMUNITY_1 + IServerBrowser::TYPE_FAVORITE_COMMUNITY_1;
		if(Force || ServerBrowser()->GetCurrentType() != BrowserType)
		{
			if(Force || ServerBrowser()->GetCurrentType() == IServerBrowser::TYPE_LAN)
			{
				Client()->RequestDDNetInfo();
			}
			ServerBrowser()->Refresh(BrowserType);
			UpdateCommunityCache(true);
		}
	}
}

void CMenus::ForceRefreshLanPage()
{
	m_ForceRefreshLanPage = true;
}

void CMenus::SetShowStart(bool ShowStart)
{
	m_ShowStart = ShowStart;
}

void CMenus::ShowQuitPopup()
{
	m_Popup = POPUP_QUIT;
}

void CMenus::JoinTutorial()
{
	m_JoinTutorial.m_Queued = true;
	m_JoinTutorial.m_Status = CJoinTutorial::EStatus::REFRESHING;
	m_JoinTutorial.m_TryRefresh = false;
	m_JoinTutorial.m_TriedRefresh = false;
	m_JoinTutorial.m_LocalServerState = CJoinTutorial::ELocalServerState::NOT_TRIED;
	m_JoinTutorial.m_StateChange = time_get_nanoseconds();
}
