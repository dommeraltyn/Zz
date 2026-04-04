#ifndef RCLIENT_MENUS_START_RCLIENT_H
#define RCLIENT_MENUS_START_RCLIENT_H

#include <array>

#include <game/client/component.h>
#include <game/client/ui_rect.h>

class CMenusStartRClient : public CComponentInterfaces
{
public:
	void RenderStartMenu(CUIRect MainView);

private:
	bool CheckHotKey(int Key) const;

	bool m_LockActive = false;
	float m_LockAnim = 0.0f;
	std::array<float, 4> m_aLockButtonHoverAnim{};
};

#endif
