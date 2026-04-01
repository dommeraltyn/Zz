/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"

#include <base/math.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/collision.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace
{
bool ZzIsFrozenClient(CGameClient *pGameClient, int ClientId, int DummyIndex)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	if(pGameClient->m_aClients[ClientId].m_DeepFrozen || pGameClient->m_aClients[ClientId].m_LiveFrozen)
		return true;
	if(pGameClient->m_aClients[ClientId].m_FreezeEnd > pGameClient->Client()->GameTick(DummyIndex))
		return true;
	return pGameClient->m_aClients[ClientId].m_Predicted.m_IsInFreeze != 0;
}

bool ZzIsLaserEnemy(CGameClient *pGameClient, int LocalId, int ClientId, int DummyIndex)
{
	if(ClientId == LocalId || ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	if(!pGameClient->m_Snap.m_aCharacters[ClientId].m_Active)
		return false;
	if(ZzIsFrozenClient(pGameClient, ClientId, DummyIndex))
		return false;

	const int OtherTeam = pGameClient->m_aClients[ClientId].m_Team;
	if(OtherTeam == TEAM_SPECTATORS)
		return false;

	if(!pGameClient->IsTeamPlay())
		return true;

	const int LocalTeam = pGameClient->m_aClients[LocalId].m_Team;
	if((LocalTeam == TEAM_RED || LocalTeam == TEAM_BLUE) && (OtherTeam == TEAM_RED || OtherTeam == TEAM_BLUE) && OtherTeam == LocalTeam)
		return false;
	return true;
}

bool ZzConsumeRelease(CNetObj_PlayerInput &Input, bool &ReleasePending, int &InjectedFire)
{
	if(ReleasePending && (Input.m_Fire & 1) != 0 && Input.m_Fire == InjectedFire)
	{
		Input.m_Fire = (Input.m_Fire + 1) & INPUT_STATE_MASK;
		ReleasePending = false;
		InjectedFire = -1;
		return true;
	}
	return false;
}

bool ZzInjectTapFire(CNetObj_PlayerInput &Input, bool &ReleasePending, int &InjectedFire)
{
	if(ReleasePending || (Input.m_Fire & 1) != 0)
		return false;
	Input.m_Fire = (Input.m_Fire + 1) & INPUT_STATE_MASK;
	ReleasePending = true;
	InjectedFire = Input.m_Fire;
	return true;
}

bool ZzRayHitsEnemyOnCrosshair(CGameClient *pGameClient, int DummyIndex, const vec2 &AimDir, float Reach)
{
	int LocalId = pGameClient->m_aLocalIds[DummyIndex];
	if(LocalId < 0)
		LocalId = pGameClient->m_Snap.m_LocalClientId;
	if(LocalId < 0)
		return false;

	const vec2 LocalPos = pGameClient->m_aClients[LocalId].m_RegularPredicted.m_Pos;
	const vec2 To = LocalPos + normalize(AimDir) * Reach;
	float BestT = Reach + 1.0f;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!ZzIsLaserEnemy(pGameClient, LocalId, i, DummyIndex))
			continue;

		const vec2 aTargets[] = {
			pGameClient->m_aClients[i].m_RenderPos,
			vec2((float)pGameClient->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[i].m_Cur.m_Y),
		};

		for(const vec2 &TargetPos : aTargets)
		{
			vec2 Closest;
			if(!closest_point_on_line(LocalPos, To, TargetPos, Closest))
				continue;
			const float Along = distance(LocalPos, Closest);
			if(Along >= BestT)
				continue;
			if(distance(TargetPos, Closest) > CCharacterCore::PhysicalSize() * 0.72f)
				continue;
			vec2 HitPos;
			if(pGameClient->Collision()->IntersectLine(LocalPos, Closest, &HitPos, nullptr) != 0)
				continue;
			BestT = Along;
		}
	}

	return BestT <= Reach;
}

std::optional<vec2> ZzFindDirectLaserAim(CGameClient *pGameClient, CNetObj_PlayerInput *pInputData, int DummyIndex)
{
	int LocalId = pGameClient->m_aLocalIds[DummyIndex];
	if(LocalId < 0)
		LocalId = pGameClient->m_Snap.m_LocalClientId;
	if(LocalId < 0)
		return std::nullopt;

	const vec2 LocalPos = pGameClient->m_aClients[LocalId].m_RegularPredicted.m_Pos;
	vec2 Aim((float)pInputData[DummyIndex].m_TargetX, (float)pInputData[DummyIndex].m_TargetY);
	if(length(Aim) <= 0.001f)
		Aim = vec2(1.0f, 0.0f);
	const vec2 CurAim = normalize(Aim);
	const float Reach = std::max((float)pGameClient->m_aClients[LocalId].m_Predicted.m_Tuning.m_LaserReach, 800.0f);
	const float TargetRadius = CCharacterCore::PhysicalSize() * 0.68f;
	static const vec2 s_aAimOffsets[] = {
		vec2(1.0f, 0.0f),
		vec2(-1.0f, 0.0f),
		vec2(0.0f, 1.0f),
		vec2(0.0f, -1.0f),
		normalize(vec2(1.0f, 1.0f)),
		normalize(vec2(-1.0f, 1.0f)),
		normalize(vec2(1.0f, -1.0f)),
		normalize(vec2(-1.0f, -1.0f)),
	};

	float BestScore = 1e18f;
	vec2 BestDir = CurAim;
	bool Found = false;
	auto TryAimPos = [&](const vec2 &AimPos) {
		vec2 To = AimPos - LocalPos;
		const float Dist = length(To);
		if(Dist < 0.001f || Dist > Reach)
			return;
		To *= 1.0f / Dist;
		vec2 HitPos;
		if(pGameClient->Collision()->IntersectLine(LocalPos, AimPos, &HitPos, nullptr) != 0)
			return;
		const float Angle = acosf(std::clamp(dot(CurAim, To), -1.0f, 1.0f));
		const float Score = Angle * 2000.0f + Dist * 0.25f;
		if(Score < BestScore)
		{
			BestScore = Score;
			BestDir = To;
			Found = true;
		}
	};

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!ZzIsLaserEnemy(pGameClient, LocalId, i, DummyIndex))
			continue;
		const vec2 RenderTargetPos = pGameClient->m_aClients[i].m_RenderPos;
		const vec2 SnapTargetPos = vec2((float)pGameClient->m_Snap.m_aCharacters[i].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[i].m_Cur.m_Y);
		TryAimPos(RenderTargetPos);
		TryAimPos(SnapTargetPos);
		for(const vec2 &Off : s_aAimOffsets)
		{
			TryAimPos(RenderTargetPos + Off * TargetRadius);
			TryAimPos(SnapTargetPos + Off * TargetRadius);
		}
	}

	if(!Found)
		return std::nullopt;
	return BestDir;
}

bool ZzSegmentHitsEnemy(CGameClient *pGameClient, int DummyIndex, int LocalId, const vec2 &From, const vec2 &To)
{
	const vec2 Ray = To - From;
	const float RayLen2 = dot(Ray, Ray);
	if(RayLen2 < 0.001f)
		return false;
	const float RayLen = sqrtf(RayLen2);
	const vec2 Dir = Ray * (1.0f / RayLen);
	const float Radius2 = CCharacterCore::PhysicalSize() * CCharacterCore::PhysicalSize();
	float BestT = RayLen + 1.0f;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!ZzIsLaserEnemy(pGameClient, LocalId, i, DummyIndex))
			continue;
		const vec2 P = pGameClient->m_aClients[i].m_RegularPredicted.m_Pos;
		const vec2 d = P - From;
		const float t = dot(d, Dir);
		if(t < 0.0f || t > RayLen || t >= BestT)
			continue;
		const vec2 Closest = From + Dir * t;
		const vec2 dp = P - Closest;
		if(dot(dp, dp) <= Radius2)
			BestT = t;
	}

	return BestT <= RayLen;
}

bool ZzRayHitsEnemyWithBounces(CGameClient *pGameClient, int DummyIndex, int LocalId, vec2 Pos, vec2 Dir, float Reach, float BounceCost, int MaxBounces)
{
	float Energy = Reach;
	int Bounces = 0;
	while(Energy > 0.0f)
	{
		vec2 ColTile;
		vec2 To = Pos + Dir * Energy;
		const int Res = pGameClient->Collision()->IntersectLineTeleWeapon(Pos, To, &ColTile, &To);
		if(ZzSegmentHitsEnemy(pGameClient, DummyIndex, LocalId, Pos, To))
			return true;
		if(!Res || Bounces >= MaxBounces)
			break;
		const vec2 From = Pos;
		Pos = To;
		vec2 TempPos = Pos;
		vec2 TempDir = Dir * 4.0f;
		pGameClient->Collision()->MovePoint(&TempPos, &TempDir, 1.0f, nullptr);
		Pos = TempPos;
		if(length(TempDir) <= 0.001f)
			break;
		Dir = normalize(TempDir);
		Energy -= distance(From, Pos) + BounceCost;
		Bounces++;
	}
	return false;
}
}

CControls::CControls()
{
	mem_zero(&m_aLastData, sizeof(m_aLastData));
	std::fill(std::begin(m_aMousePos), std::end(m_aMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMousePosOnAction), std::end(m_aMousePosOnAction), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aTargetPos), std::end(m_aTargetPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMouseInputType), std::end(m_aMouseInputType), EMouseInputType::ABSOLUTE);
}

void CControls::OnReset()
{
	ResetInput(0);
	ResetInput(1);

	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;

	m_LastSendTime = 0;
}

void CControls::ResetInput(int Dummy)
{
	m_aLastData[Dummy].m_Direction = 0;
	// simulate releasing the fire button
	if((m_aLastData[Dummy].m_Fire & 1) != 0)
		m_aLastData[Dummy].m_Fire++;
	m_aLastData[Dummy].m_Fire &= INPUT_STATE_MASK;
	m_aLastData[Dummy].m_Jump = 0;
	m_aInputData[Dummy] = m_aLastData[Dummy];

	m_aInputDirectionLeft[Dummy] = 0;
	m_aInputDirectionRight[Dummy] = 0;
	m_aSaveInBlock[Dummy] = 0;
}

void CControls::OnPlayerDeath()
{
	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;
}

struct CInputState
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
};

void CControls::ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if(pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
		return;

	*pState->m_apVariables[g_Config.m_ClDummy] = pResult->GetInteger(0);
}

void CControls::ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
	if(((*pVariable) & 1) != pResult->GetInteger(0))
		(*pVariable)++;
	*pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
	int m_Value;
};

struct CInputToggle
{
	int *m_pVariable;
};

static void ConKeyToggle(IConsole::IResult *pResult, void *pUserData)
{
	CInputToggle *pToggle = (CInputToggle *)pUserData;
	if(pResult->GetInteger(0))
		*pToggle->m_pVariable ^= 1;
}

struct CInputToggleOnce
{
	int *m_pVariable;
	bool m_WasDown;
};

static void ConKeyToggleOnce(IConsole::IResult *pResult, void *pUserData)
{
	CInputToggleOnce *pToggle = (CInputToggleOnce *)pUserData;
	const bool Down = pResult->GetInteger(0) != 0;
	if(Down && !pToggle->m_WasDown)
		*pToggle->m_pVariable ^= 1;
	pToggle->m_WasDown = Down;
}

struct CFentBotWaypointKey
{
	CControls *m_pControls;
	CGameClient *m_pGameClient;
	bool m_WasDown;
};

static void ConKeyFentBotWaypoint(IConsole::IResult *pResult, void *pUserData)
{
	CFentBotWaypointKey *pKey = (CFentBotWaypointKey *)pUserData;
	const bool Down = pResult->GetInteger(0) != 0;
	if(Down && !pKey->m_WasDown)
	{
		CGameClient *pGameClient = pKey->m_pGameClient;
		if(pGameClient->FentBotHasWaypoint())
			pGameClient->FentBotClearWaypoint();
		else
		{
			if(pGameClient->m_Snap.m_pLocalCharacter)
			{
				const int LocalId = pGameClient->m_Snap.m_LocalClientId;
				const vec2 LocalPos = pGameClient->m_aClients[LocalId].m_RegularPredicted.m_Pos;
				vec2 Aim((float)pGameClient->m_Controls.m_aInputData[g_Config.m_ClDummy].m_TargetX, (float)pGameClient->m_Controls.m_aInputData[g_Config.m_ClDummy].m_TargetY);
				if(length(Aim) < 0.001f)
					Aim = vec2(1.0f, 0.0f);
				Aim = normalize(Aim);
				vec2 HitPos;
				pGameClient->Collision()->IntersectLine(LocalPos, LocalPos + Aim * 2000.0f, &HitPos, nullptr);
				pGameClient->FentBotSetWaypoint(HitPos);
			}
		}
	}
	pKey->m_WasDown = Down;
}

void CControls::ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	if(pResult->GetInteger(0))
	{
		*pSet->m_apVariables[g_Config.m_ClDummy] = pSet->m_Value;
	}
}

void CControls::ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	ConKeyInputCounter(pResult, pSet);
	pSet->m_pControls->m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
	// game commands
	{
		static CInputState s_State = {this, {&m_aInputDirectionLeft[0], &m_aInputDirectionLeft[1]}};
		Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
	}
	{
		static CInputState s_State = {this, {&m_aInputDirectionRight[0], &m_aInputDirectionRight[1]}};
		Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Jump, &m_aInputData[1].m_Jump}};
		Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Hook, &m_aInputData[1].m_Hook}};
		Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
	}
	{
		static CInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire}};
		Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyInputCounter, &s_State, "Fire");
	}
	{
		static CInputState s_State = {this, {&m_aShowHookColl[0], &m_aShowHookColl[1]}};
		Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 1};
		Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 2};
		Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 3};
		Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 4};
		Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 5};
		Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_NextWeapon, &m_aInputData[1].m_NextWeapon}, 0};
		Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_PrevWeapon, &m_aInputData[1].m_PrevWeapon}, 0};
		Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
	}

	// ZZetClient
	{
		static CInputToggleOnce s_Toggle = {&g_Config.m_ClZzAimbotEnabled, false};
		static CInputToggle s_ToggleLegacy = {&g_Config.m_ClZzAimbotEnabled};
		// Note: for +commands, the console injects the stroke argument before parsing,
		// so the params string must be empty.
		Console()->Register("+zz_aimbot_toggle", "", CFGFLAG_CLIENT, ConKeyToggleOnce, &s_Toggle, "ZZetClient: toggle aimbot");
		Console()->Register("zz_aimbot_toggle", "", CFGFLAG_CLIENT, ConKeyToggle, &s_ToggleLegacy, "ZZetClient: toggle aimbot (legacy)");
	}
	{
		static CInputToggleOnce s_Toggle = {&g_Config.m_ClZzAvoidEnabled, false};
		static CInputToggle s_ToggleLegacy = {&g_Config.m_ClZzAvoidEnabled};
		Console()->Register("+zz_avoid_toggle", "", CFGFLAG_CLIENT, ConKeyToggleOnce, &s_Toggle, "ZZetClient: toggle avoid freeze");
		Console()->Register("zz_avoid_toggle", "", CFGFLAG_CLIENT, ConKeyToggle, &s_ToggleLegacy, "ZZetClient: toggle avoid freeze (legacy)");
	}
	{
		static CInputToggleOnce s_Toggle = {&g_Config.m_ClZzBalanceBot, false};
		static CInputToggle s_ToggleLegacy = {&g_Config.m_ClZzBalanceBot};
		Console()->Register("+zz_balance_bot_toggle", "", CFGFLAG_CLIENT, ConKeyToggleOnce, &s_Toggle, "ZZetClient: toggle balance bot");
		Console()->Register("zz_balance_bot_toggle", "", CFGFLAG_CLIENT, ConKeyToggle, &s_ToggleLegacy, "ZZetClient: toggle balance bot (legacy)");
	}
	{
		static CInputToggleOnce s_Toggle = {&g_Config.m_ClZzFakeLag, false};
		static CInputToggle s_ToggleLegacy = {&g_Config.m_ClZzFakeLag};
		Console()->Register("+zz_fake_lag_toggle", "", CFGFLAG_CLIENT, ConKeyToggleOnce, &s_Toggle, "ZZetClient: toggle fake lag");
		Console()->Register("zz_fake_lag_toggle", "", CFGFLAG_CLIENT, ConKeyToggle, &s_ToggleLegacy, "ZZetClient: toggle fake lag (legacy)");
	}
	{
		static CFentBotWaypointKey s_Key = {this, GameClient(), false};
		Console()->Register("+zz_fentbot_waypoint", "", CFGFLAG_CLIENT, ConKeyFentBotWaypoint, &s_Key, "ZZetClient: set/clear FentBot waypoint");
	}
	{
		static CInputState s_State = {this, {&m_aSaveInBlock[0], &m_aSaveInBlock[1]}};
		Console()->Register("+zz_tile_aimbot", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "ZZetClient: hook a hookable tile in your FOV (hold)");
		Console()->Register("+zz_save_in_block", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "ZZetClient: hook a hookable tile in your FOV (hold) (legacy alias)");
	}

	// Re-apply saved keybind (if any).
	if(g_Config.m_ClZzAimbotKey >= KEY_FIRST && g_Config.m_ClZzAimbotKey < KEY_LAST)
		GameClient()->m_Binds.Bind(g_Config.m_ClZzAimbotKey, "+zz_aimbot_toggle", false, g_Config.m_ClZzAimbotMod);
	if(g_Config.m_ClZzAvoidToggleKey >= KEY_FIRST && g_Config.m_ClZzAvoidToggleKey < KEY_LAST)
		GameClient()->m_Binds.Bind(g_Config.m_ClZzAvoidToggleKey, "+zz_avoid_toggle", false, g_Config.m_ClZzAvoidToggleMod);
	if(g_Config.m_ClZzBalanceBotKey >= KEY_FIRST && g_Config.m_ClZzBalanceBotKey < KEY_LAST)
		GameClient()->m_Binds.Bind(g_Config.m_ClZzBalanceBotKey, "+zz_balance_bot_toggle", false, g_Config.m_ClZzBalanceBotMod);
	if(g_Config.m_ClZzFakeLagKey >= KEY_FIRST && g_Config.m_ClZzFakeLagKey < KEY_LAST)
		GameClient()->m_Binds.Bind(g_Config.m_ClZzFakeLagKey, "+zz_fake_lag_toggle", false, g_Config.m_ClZzFakeLagMod);
	if(g_Config.m_ClZzFentBotWaypointKey >= KEY_FIRST && g_Config.m_ClZzFentBotWaypointKey < KEY_LAST)
		GameClient()->m_Binds.Bind(g_Config.m_ClZzFentBotWaypointKey, "+zz_fentbot_waypoint", false, g_Config.m_ClZzFentBotWaypointMod);
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
	if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
		if(g_Config.m_ClAutoswitchWeapons)
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = pMsg->m_Weapon + 1;
		// We don't really know ammo count, until we'll switch to that weapon, but any non-zero count will suffice here
		m_aAmmoCount[maximum(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
	}
}

void CControls::Balance()
{
	if(!g_Config.m_ClZzBalanceBot)
		return;

	const int Dummy = g_Config.m_ClDummy;
	if(m_aInputData[Dummy].m_Direction != 0)
		return;

	CGameClient *pGameClient = GameClient();
	const int LocalId = pGameClient->m_aLocalIds[Dummy];
	if(LocalId < 0 || !pGameClient->m_aClients[LocalId].m_Active || !pGameClient->m_Snap.m_aCharacters[LocalId].m_Active)
		return;

	const vec2 LocalPos = pGameClient->m_aClients[LocalId].m_Predicted.m_Pos + pGameClient->m_aClients[LocalId].m_Predicted.m_Vel;
	int TargetId = -1;
	float BestRange = 401.0f;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == LocalId || !pGameClient->m_aClients[i].m_Active || !pGameClient->m_Snap.m_aCharacters[i].m_Active)
			continue;

		const vec2 PlayerPos = pGameClient->m_aClients[i].m_Predicted.m_Pos;
		if(LocalPos.y == PlayerPos.y)
			continue;

		const float Dist = distance(PlayerPos, LocalPos);
		if(Dist < BestRange)
		{
			BestRange = Dist;
			TargetId = i;
		}
	}

	if(TargetId < 0)
		return;

	const vec2 TargetPos = pGameClient->m_aClients[TargetId].m_Predicted.m_Pos;
	if(LocalPos.x - TargetPos.x > 1.0f)
		m_aInputData[Dummy].m_Direction = -1;
	else if(TargetPos.x - LocalPos.x > 1.0f)
		m_aInputData[Dummy].m_Direction = 1;
}

int CControls::SnapInput(int *pData)
{
	// update player state
	if(GameClient()->m_Chat.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_CHATTING;
	else if(GameClient()->m_Menus.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_IN_MENU;
	else
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_PLAYING;

	if(GameClient()->m_Scoreboard.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Controls.m_aShowHookColl[g_Config.m_ClDummy])
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_AIM;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

	switch(m_aMouseInputType[g_Config.m_ClDummy])
	{
	case CControls::EMouseInputType::AUTOMATED:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE;
		break;
	case CControls::EMouseInputType::ABSOLUTE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE | PLAYERFLAG_INPUT_MANUAL;
		break;
	case CControls::EMouseInputType::RELATIVE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_MANUAL;
		break;
	}

	// TClient
	if(g_Config.m_TcHideChatBubbles && Client()->RconAuthed())
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags &= ~PLAYERFLAG_CHATTING;

	if(g_Config.m_TcNameplatePingCircle)
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	bool Send = m_aLastData[g_Config.m_ClDummy].m_PlayerFlags != m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	m_aLastData[g_Config.m_ClDummy].m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	// we freeze the input if chat or menu is activated
	if(!(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
	{
		if(!GameClient()->m_GameInfo.m_BugDDRaceInput)
			ResetInput(g_Config.m_ClDummy);

		mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

		// set the target anyway though so that we can keep seeing our surroundings,
		// even if chat or menu are activated
		vec2 Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		// send once a second just to be sure
		Send = Send || time_get() > m_LastSendTime + time_freq();
	}
	else
	{
		// TClient
		vec2 Pos;
		if(g_Config.m_ClSubTickAiming && m_aMousePosOnAction[g_Config.m_ClDummy] != vec2(0.0f, 0.0f))
		{
			Pos = GameClient()->m_Controls.m_aMousePosOnAction[g_Config.m_ClDummy];
			m_aMousePosOnAction[g_Config.m_ClDummy] = vec2(0.0f, 0.0f);
		}
		else
			Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];

		m_FastInputHookAction = false;
		m_FastInputFireAction = false;

		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		// set direction
		m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
		if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
		if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = 1;
		Balance();
		static int s_aFakeLagTick[NUM_DUMMIES] = {0, 0};
		const int DummyIndex = g_Config.m_ClDummy;
		const int LocalId = GameClient()->m_Snap.m_LocalClientId;
		const bool LocalPaused = LocalId >= 0 && LocalId < MAX_CLIENTS && GameClient()->m_aClients[LocalId].m_Paused;
		const bool GamePaused = GameClient()->m_Snap.m_pGameInfoObj && (GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED);
		const bool EscapePressed = Input()->KeyPress(KEY_ESCAPE);
		const bool FakeLagBlocked = EscapePressed || GameClient()->m_Menus.IsActive() || GameClient()->m_Snap.m_SpecInfo.m_Active || LocalPaused || GamePaused;
		if(g_Config.m_ClZzFakeLag && !FakeLagBlocked)
		{
			const int BaseDir = m_aInputData[DummyIndex].m_Direction;
			const bool DriftMode = g_Config.m_ClZzFakeLagDrift != 0;
			bool OnGround = false;
			if(GameClient()->m_Snap.m_pLocalCharacter)
			{
				const vec2 ProbePos = GameClient()->m_LocalCharacterPos + vec2(0.0f, CCharacterCore::PhysicalSize() + 6.0f);
				OnGround = GameClient()->Collision()->CheckPoint(ProbePos);
			}
			const int Tick = s_aFakeLagTick[DummyIndex]++;
			if(BaseDir != 0)
			{
				// Preserve movement most of the time and inject occasional opposite ticks.
				// In air we inject less often to avoid breaking control.
				const int Phase = OnGround ? (Tick % 4) : (Tick % 6);
				const bool InjectOpposite = OnGround ? (Phase == 1) : (Phase == 2);
				m_aInputData[DummyIndex].m_Direction = InjectOpposite ? -BaseDir : BaseDir;
			}
			else
			{
				if(DriftMode)
				{
					m_aInputData[DummyIndex].m_Direction = (Tick & 1) ? 1 : -1;
				}
				else
				{
					// Standing still: short left burst, pause, short right burst, pause.
					// Keeps fake-lag wobble while staying close to the same coordinates.
					const int StandPhase = Tick % 8;
					if(StandPhase < 2)
						m_aInputData[DummyIndex].m_Direction = -1;
					else if(StandPhase < 4)
						m_aInputData[DummyIndex].m_Direction = 0;
					else if(StandPhase < 6)
						m_aInputData[DummyIndex].m_Direction = 1;
					else
						m_aInputData[DummyIndex].m_Direction = 0;
				}
			}
		}
		else if(g_Config.m_ClZzFakeLag)
		{
			// Hard stop fake-lag carryover when menu/spec/pause is entered.
			s_aFakeLagTick[DummyIndex] = 0;
			m_aInputData[DummyIndex].m_Direction = 0;
		}

		static bool s_aTriggerLaserReleasePending[NUM_DUMMIES] = {false, false};
		static int s_aTriggerLaserInjectedFire[NUM_DUMMIES] = {-1, -1};
		static bool s_aAutoLaserReleasePending[NUM_DUMMIES] = {false, false};
		static int s_aAutoLaserInjectedFire[NUM_DUMMIES] = {-1, -1};
		static int s_aAutoLaserAimTicks[NUM_DUMMIES] = {0, 0};
		static vec2 s_aAutoLaserPlannedAim[NUM_DUMMIES] = {vec2(1.0f, 0.0f), vec2(1.0f, 0.0f)};

		const bool CanUseLaserLogic = DummyIndex == 0 && GameClient()->m_Snap.m_pLocalCharacter && !GameClient()->m_Snap.m_SpecInfo.m_Active;
		const bool LaserSelected = CanUseLaserLogic && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_LASER;

		ZzConsumeRelease(m_aInputData[DummyIndex], s_aTriggerLaserReleasePending[DummyIndex], s_aTriggerLaserInjectedFire[DummyIndex]);
		ZzConsumeRelease(m_aInputData[DummyIndex], s_aAutoLaserReleasePending[DummyIndex], s_aAutoLaserInjectedFire[DummyIndex]);

		if(g_Config.m_ClZzFngTriggerLaser && LaserSelected)
		{
			int LocalId = GameClient()->m_aLocalIds[DummyIndex];
			if(LocalId < 0)
				LocalId = GameClient()->m_Snap.m_LocalClientId;
			if(LocalId >= 0)
			{
				vec2 Aim((float)m_aInputData[DummyIndex].m_TargetX, (float)m_aInputData[DummyIndex].m_TargetY);
				if(length(Aim) > 0.001f)
				{
					const float Reach = std::max((float)GameClient()->m_aClients[LocalId].m_Predicted.m_Tuning.m_LaserReach, 800.0f);
					if(ZzRayHitsEnemyOnCrosshair(GameClient(), DummyIndex, normalize(Aim), Reach))
						ZzInjectTapFire(m_aInputData[DummyIndex], s_aTriggerLaserReleasePending[DummyIndex], s_aTriggerLaserInjectedFire[DummyIndex]);
				}
			}
		}
		else
		{
			s_aTriggerLaserReleasePending[DummyIndex] = false;
			s_aTriggerLaserInjectedFire[DummyIndex] = -1;
		}

		if(g_Config.m_ClZzFngAutoLaser && LaserSelected)
		{
			int LocalId = GameClient()->m_aLocalIds[DummyIndex];
			if(LocalId < 0)
				LocalId = GameClient()->m_Snap.m_LocalClientId;
			if(LocalId >= 0)
			{
				vec2 Aim((float)m_aInputData[DummyIndex].m_TargetX, (float)m_aInputData[DummyIndex].m_TargetY);
				if(length(Aim) <= 0.001f)
					Aim = vec2(1.0f, 0.0f);
				const vec2 CurAim = normalize(Aim);
				const vec2 LocalPos = GameClient()->m_aClients[LocalId].m_RegularPredicted.m_Pos;
				const float Reach = std::max((float)GameClient()->m_aClients[LocalId].m_Predicted.m_Tuning.m_LaserReach, 800.0f);
				const float BounceCost = (float)GameClient()->m_aClients[LocalId].m_Predicted.m_Tuning.m_LaserBounceCost;

				bool Found = false;
				vec2 BestDir = CurAim;
				if(const auto DirectAim = ZzFindDirectLaserAim(GameClient(), m_aInputData, DummyIndex))
				{
					BestDir = *DirectAim;
					Found = true;
				}
				else if(g_Config.m_ClZzFngAutoLaserBounce)
				{
					const int RaysPerTick = std::clamp(g_Config.m_ClZzFngAutoLaserRaysPerTick, 1, 64);
					const int MaxBounces = std::clamp(g_Config.m_ClZzFngAutoLaserMaxBounces, 0, 8);
					static int s_ScanStep = 0;
					const float Step = 2.0f * pi / 128.0f;
					float BestScore = 1e18f;
					for(int r = 0; r < RaysPerTick; r++)
					{
						const int Idx = (s_ScanStep + r) % 128;
						const float A = (float)Idx * Step;
						const vec2 Dir = vec2(cosf(A), sinf(A));
						if(!ZzRayHitsEnemyWithBounces(GameClient(), DummyIndex, LocalId, LocalPos, Dir, Reach, BounceCost, MaxBounces))
							continue;
						const float Angle = acosf(std::clamp(dot(CurAim, Dir), -1.0f, 1.0f));
						if(Angle < BestScore)
						{
							BestScore = Angle;
							BestDir = Dir;
							Found = true;
						}
					}
					s_ScanStep = (s_ScanStep + RaysPerTick) % 128;
				}

				if(Found)
				{
					s_aAutoLaserPlannedAim[DummyIndex] = BestDir;
					s_aAutoLaserAimTicks[DummyIndex] = 1;
				}
				else
				{
					s_aAutoLaserAimTicks[DummyIndex] = 0;
				}

				if(s_aAutoLaserAimTicks[DummyIndex] > 0)
				{
					vec2 SmoothAim = CurAim + (normalize(s_aAutoLaserPlannedAim[DummyIndex]) - CurAim) * 0.82f;
					if(length(SmoothAim) > 0.001f)
						SmoothAim = normalize(SmoothAim);
					else
						SmoothAim = CurAim;
					m_aInputData[DummyIndex].m_TargetX = (int)round_to_int(SmoothAim.x * 1000.0f);
					m_aInputData[DummyIndex].m_TargetY = (int)round_to_int(SmoothAim.y * 1000.0f);
					if(!m_aInputData[DummyIndex].m_TargetX && !m_aInputData[DummyIndex].m_TargetY)
						m_aInputData[DummyIndex].m_TargetX = 1;
					s_aAutoLaserAimTicks[DummyIndex]--;
					if(s_aAutoLaserAimTicks[DummyIndex] <= 0)
						ZzInjectTapFire(m_aInputData[DummyIndex], s_aAutoLaserReleasePending[DummyIndex], s_aAutoLaserInjectedFire[DummyIndex]);
				}
			}
		}
		else
		{
			s_aAutoLaserReleasePending[DummyIndex] = false;
			s_aAutoLaserInjectedFire[DummyIndex] = -1;
			s_aAutoLaserAimTicks[DummyIndex] = 0;
		}

		// dummy copy moves
		if(g_Config.m_ClDummyCopyMoves)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;

			// Don't copy any input to dummy when spectating others
			if(!GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
			{
				pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
				pDummyInput->m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
				pDummyInput->m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
				pDummyInput->m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;
				pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
				pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
				pDummyInput->m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;

				if(!g_Config.m_ClDummyControl)
					pDummyInput->m_Fire += m_aInputData[g_Config.m_ClDummy].m_Fire - m_aLastData[g_Config.m_ClDummy].m_Fire;

				pDummyInput->m_NextWeapon += m_aInputData[g_Config.m_ClDummy].m_NextWeapon - m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
				pDummyInput->m_PrevWeapon += m_aInputData[g_Config.m_ClDummy].m_PrevWeapon - m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
			}

			m_aInputData[!g_Config.m_ClDummy] = *pDummyInput;
		}

		if(g_Config.m_ClDummyControl)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;
			pDummyInput->m_Jump = g_Config.m_ClDummyJump;

			if(g_Config.m_ClDummyFire)
				pDummyInput->m_Fire = g_Config.m_ClDummyFire;
			else if((pDummyInput->m_Fire & 1) != 0)
				pDummyInput->m_Fire++;

			pDummyInput->m_Hook = g_Config.m_ClDummyHook;
		}

		// stress testing
		if(g_Config.m_DbgStress)
		{
			float t = Client()->LocalTime();
			mem_zero(&m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

			m_aInputData[g_Config.m_ClDummy].m_Direction = ((int)t / 2) & 1;
			m_aInputData[g_Config.m_ClDummy].m_Jump = ((int)t);
			m_aInputData[g_Config.m_ClDummy].m_Fire = ((int)(t * 10));
			m_aInputData[g_Config.m_ClDummy].m_Hook = ((int)(t * 2)) & 1;
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = ((int)t) % NUM_WEAPONS;
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)(std::sin(t * 3) * 100.0f);
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)(std::cos(t * 3) * 100.0f);
		}

		// check if we need to send input
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
		Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz

		// ZZetClient: while hammer assist is actively trying to hit (hammer + fire held), send at tickrate.
		if(g_Config.m_ClZzHammerAssistEnabled && GameClient()->m_Snap.m_pLocalCharacter &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_HAMMER && (m_aInputData[g_Config.m_ClDummy].m_Fire & 1))
		{
			const int TickSpeed = maximum(1, Client()->GameTickSpeed());
			Send = Send || time_get() > m_LastSendTime + time_freq() / TickSpeed;
		}
		Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));
	}

	const CNetObj_PlayerInput OldLastData = m_aLastData[g_Config.m_ClDummy];

	// copy and return size
	m_aLastData[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];

	if(!Send)
		return 0;

	if(g_Config.m_ClZzFngDebug)
	{
		static int64_t s_LastSendDbg = 0;
		const int64_t Now = time_get();
		if(s_LastSendDbg == 0 || Now - s_LastSendDbg >= time_freq())
		{
			s_LastSendDbg = Now;
			const CNetObj_PlayerInput &Cur = m_aInputData[g_Config.m_ClDummy];
			int Changed = 0;
			if(Cur.m_Direction != OldLastData.m_Direction) Changed |= 1 << 0;
			if(Cur.m_Jump != OldLastData.m_Jump) Changed |= 1 << 1;
			if(Cur.m_Fire != OldLastData.m_Fire) Changed |= 1 << 2;
			if(Cur.m_Hook != OldLastData.m_Hook) Changed |= 1 << 3;
			if(Cur.m_WantedWeapon != OldLastData.m_WantedWeapon) Changed |= 1 << 4;
			if(Cur.m_NextWeapon != OldLastData.m_NextWeapon) Changed |= 1 << 5;
			if(Cur.m_PrevWeapon != OldLastData.m_PrevWeapon) Changed |= 1 << 6;
			if(Cur.m_TargetX != OldLastData.m_TargetX || Cur.m_TargetY != OldLastData.m_TargetY) Changed |= 1 << 7;

			int Reason = 0;
			if(Changed != 0) Reason |= 1 << 0;
			if(time_get() > m_LastSendTime + time_freq() / 25) Reason |= 1 << 1;
			if(g_Config.m_ClZzHammerAssistEnabled && GameClient()->m_Snap.m_pLocalCharacter &&
				GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_HAMMER && (Cur.m_Fire & 1))
				Reason |= 1 << 2;
			if(GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (Cur.m_Direction || Cur.m_Jump || Cur.m_Hook))
				Reason |= 1 << 3;

			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "Tick=%d Size=%d ReasonMask=0x%x ChangedMask=0x%x Fire=%d Hook=%d Dir=%d Jump=%d Aim=(%d,%d)", Client()->GameTick(g_Config.m_ClDummy), (int)sizeof(m_aInputData[0]), Reason, Changed, Cur.m_Fire, Cur.m_Hook, Cur.m_Direction, Cur.m_Jump, Cur.m_TargetX, Cur.m_TargetY);
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz/fng_send", aBuf);
		}
	}

	m_LastSendTime = time_get();
	mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
	return sizeof(m_aInputData[0]);
}

void CControls::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->m_GameInfo.m_UnlimitedAmmo && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// Keep track of ammo count, we know weapon ammo only when we switch to that weapon, this is tracked on server and protocol does not track that
		m_aAmmoCount[maximum(0, GameClient()->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount;
		// Autoswitch weapon if we're out of ammo
		if(m_aInputData[g_Config.m_ClDummy].m_Fire % 2 != 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		{
			int Weapon;
			for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
			{
				if(Weapon == GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
					continue;
				if(m_aAmmoCount[Weapon] > 0)
					break;
			}
			if(Weapon != GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = Weapon + 1;
		}
	}

	// update target pos
	if(GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		// make sure to compensate for smooth dyncam to ensure the cursor stays still in world space if zoomed
		vec2 DyncamOffsetDelta = GameClient()->m_Camera.m_DyncamTargetCameraOffset - GameClient()->m_Camera.m_aDyncamCurrentCameraOffset[g_Config.m_ClDummy];
		float Zoom = GameClient()->m_Camera.m_Zoom;
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_LocalCharacterPos + m_aMousePos[g_Config.m_ClDummy] - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
	}
	else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
	{
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_Snap.m_SpecInfo.m_Position + m_aMousePos[g_Config.m_ClDummy];
	}
	else
	{
		m_aTargetPos[g_Config.m_ClDummy] = m_aMousePos[g_Config.m_ClDummy];
	}
}

bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(GameClient()->m_Snap.m_pGameInfoObj && (GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return false;

	if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		vec2 AbsoluteDirection;
		if(Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
		{
			m_aMousePos[g_Config.m_ClDummy] = AbsoluteDirection * GetMaxMouseDistance();
			GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::ABSOLUTE;
		}
		return true;
	}

	float Factor = 1.0f;
	if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
	{
		Factor = g_Config.m_ClDyncamMousesens / 100.0f;
	}
	else
	{
		switch(CursorType)
		{
		case IInput::CURSOR_MOUSE:
			Factor = g_Config.m_InpMousesens / 100.0f;
			break;
		case IInput::CURSOR_JOYSTICK:
			Factor = g_Config.m_InpControllerSens / 100.0f;
			break;
		default:
			dbg_assert_failed("CControls::OnCursorMove CursorType %d", (int)CursorType);
		}
	}

	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
		Factor *= GameClient()->m_Camera.m_Zoom;

	m_aMousePos[g_Config.m_ClDummy] += vec2(x, y) * Factor;
	GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::RELATIVE;
	ClampMousePos();
	return true;
}

void CControls::ClampMousePos()
{
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
	{
		m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
		m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
	}
	else
	{
		const float MouseMin = GetMinMouseDistance();
		const bool ChatActive = GameClient()->m_Chat.IsActive();
		const float MouseMax = GetMaxMouseDistance() * (ChatActive ? 2.5f : 1.0f);

		float MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance < 0.001f)
		{
			m_aMousePos[g_Config.m_ClDummy].x = 0.001f;
			m_aMousePos[g_Config.m_ClDummy].y = 0;
			MouseDistance = 0.001f;
		}
		if(MouseDistance < MouseMin)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMin;
		MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance > MouseMax)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMax;

		if(g_Config.m_TcLimitMouseToScreen)
		{
			float Width, Height;
			Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), 1.0f, &Width, &Height);
			Height /= 2.0f;
			Width /= 2.0f;
			if(ChatActive)
			{
				Height *= 2.5f;
				Width *= 2.5f;
			}
			if(g_Config.m_TcLimitMouseToScreen == 2)
				Width = Height;
			m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -Height, Height);
			m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -Width, Width);
		}
	}
}

float CControls::GetMinMouseDistance() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
	float CameraMaxDistance = 200.0f;
	float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
	return minimum((FollowFactor != 0 ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance), MaxDistance);
}

bool CControls::CheckNewInput()
{
	bool NewInput[2] = {};
	for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
	{
		CNetObj_PlayerInput TestInput = m_aInputData[Dummy];
		if(Dummy == g_Config.m_ClDummy)
		{
			TestInput.m_Direction = 0;
			if(m_aInputDirectionLeft[Dummy] && !m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = -1;
			if(!m_aInputDirectionLeft[Dummy] && m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = 1;
		}

		if(m_aFastInput[Dummy].m_Direction != TestInput.m_Direction)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Hook != TestInput.m_Hook)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Jump != TestInput.m_Jump)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_NextWeapon != TestInput.m_NextWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_PrevWeapon != TestInput.m_PrevWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_WantedWeapon != TestInput.m_WantedWeapon)
			NewInput[Dummy] = true;

		bool SetMousePos = false;
		// We need to be careful about how we manage the mouse position to avoid mispredicted hooks and fires
		// on the first tick that they activate before we know what mouse position we actually sent to the server
		if(Dummy == g_Config.m_ClDummy)
		{
			if(m_aFastInput[Dummy].m_Hook == 0 && TestInput.m_Hook == 1)
			{
				m_FastInputHookAction = true;
				SetMousePos = true;
			}
			if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire && TestInput.m_Fire % 2 == 1)
			{
				m_FastInputFireAction = true;
				SetMousePos = true;
			}
			if(!m_FastInputHookAction && !m_FastInputFireAction)
			{
				SetMousePos = true;
			}
		}

		if(SetMousePos)
		{
			TestInput.m_TargetX = (int)m_aMousePos[Dummy].x;
			TestInput.m_TargetY = (int)m_aMousePos[Dummy].y;
		}
		else
		{
			TestInput.m_TargetX = m_aFastInput[Dummy].m_TargetX;
			TestInput.m_TargetY = m_aFastInput[Dummy].m_TargetY;
		}

		m_aFastInput[Dummy] = TestInput;
	}

	if(NewInput[0] || NewInput[1])
		return true;
	else
		return false;
}
