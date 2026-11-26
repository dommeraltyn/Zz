#ifndef GAME_CLIENT_COMPONENTS_FLYHELPER_DUMMY_AUTO_UNFREEZE_H
#define GAME_CLIENT_COMPONENTS_FLYHELPER_DUMMY_AUTO_UNFREEZE_H

#include <engine/client/enums.h>
#include <generated/protocol.h>

#include <cstdint>

class CGameClient;

namespace FlyHelper
{
class CDummyAutoUnfreeze
{
public:
	void Reset();
	void Tick(CGameClient *pGameClient, int ControlIndex, CNetObj_PlayerInput &HelperInput, CNetObj_PlayerInput &DummyInput);

private:
	struct SState
	{
		bool m_Overriding = false;
		bool m_FireReleasePending = false;
		int64_t m_LastShotTime = 0;
		CNetObj_PlayerInput m_SavedInput{};
	};

	SState m_aState[NUM_DUMMIES];
};
} 

#endif

// сделано флайхелпером))999
