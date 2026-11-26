#include "zz_async_proxy_bots.h"

#include <base/log.h>
#include <base/bytes.h>
#include <base/hash.h>
#include <base/hash_ctxt.h>
#include <base/io.h>
#include <base/math.h>
#include <base/system.h>
#include <base/vmath.h>

#include <engine/client/checksum.h>
#include <engine/message.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol_ex.h>
#include <engine/storage.h>

#include <game/client/gameclient.h>
#include <game/client/prediction/gameworld.h>
#include <game/client/prediction/entities/character.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>
#include <game/version.h>
#include <generated/protocolglue.h>
#include <generated/protocol.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

using namespace std::chrono_literals;

namespace
{
bool IsWhitespaceOnly(const char *pText, size_t Length)
{
	for(size_t i = 0; i < Length; ++i)
	{
		const char c = pText[i];
		if(c != ' ' && c != '\t' && c != '\r' && c != '\n')
			return false;
	}
	return true;
}

const char *BotStateName(int State)
{
	switch(State)
	{
	case 0: return "OFFLINE";
	case 1: return "CONNECTING";
	case 2: return "MAP_DOWNLOAD";
	case 3: return "WAIT_ENTER";
	case 4: return "INGAME";
	case 5: return "ERROR";
	default: return "UNKNOWN";
	}
}

void TraceBot(CGameClient *pGameClient, int BotId, int State, const char *pText)
{
	if(!pGameClient || !g_Config.m_ClZzAsyncBotsVerbose)
		return;
	char aMsg[384];
	str_format(aMsg, sizeof(aMsg), "bot#%d [%s] %s", BotId, BotStateName(State), pText ? pText : "");
	pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz_async_bots", aMsg);
}

bool RepackBotMsg(const CMsgPacker &Msg, bool Sixup, CPacker &Out)
{
	int MsgId = Msg.m_MsgId;
	Out.Reset();

	if(Sixup && !Msg.m_NoTranslate)
	{
		if(Msg.m_System)
		{
			if(MsgId >= OFFSET_UUID)
				;
			else if(MsgId == NETMSG_INFO || MsgId == NETMSG_REQUEST_MAP_DATA)
				;
			else if(MsgId == NETMSG_READY)
				MsgId = protocol7::NETMSG_READY;
			else if(MsgId == NETMSG_RCON_CMD)
				MsgId = protocol7::NETMSG_RCON_CMD;
			else if(MsgId == NETMSG_ENTERGAME)
				MsgId = protocol7::NETMSG_ENTERGAME;
			else if(MsgId == NETMSG_INPUT)
				MsgId = protocol7::NETMSG_INPUT;
			else if(MsgId == NETMSG_RCON_AUTH)
				MsgId = protocol7::NETMSG_RCON_AUTH;
			else if(MsgId == NETMSG_PING)
				MsgId = protocol7::NETMSG_PING;
			else
				return false;
		}
		else
		{
			if(MsgId >= 0 && MsgId < OFFSET_UUID)
				MsgId = Msg_SixToSeven(MsgId);
			if(MsgId < 0)
				return false;
		}
	}

	if(Msg.m_MsgId < OFFSET_UUID)
	{
		Out.AddInt((MsgId << 1) | (Msg.m_System ? 1 : 0));
	}
	else
	{
		Out.AddInt(Msg.m_System ? 1 : 0);
		g_UuidManager.PackUuid(Msg.m_MsgId, &Out);
	}
	Out.AddRaw(Msg.Data(), Msg.Size());
	return !Out.Error();
}

void BuildPseudoRandomLetters(char *pOut, int OutSize, int Seed)
{
	if(!pOut || OutSize <= 0)
		return;
	uint32_t State = (uint32_t)Seed * 1103515245u + 12345u;
	const int MaxLen = maximum(1, OutSize - 1);
	const int Len = minimum(8, MaxLen);
	for(int i = 0; i < Len; i++)
	{
		State = State * 1664525u + 1013904223u;
		pOut[i] = 'a' + (State % 26);
	}
	pOut[Len] = '\0';
}

bool FindNearestTripleFlyTarget(CGameClient *pGameClient, int SourceClientId, const vec2 &SourcePos, const vec2 &AimDir, float MaxFovDeg, bool RequireNotAbove, bool RequireHookReach, float HookLength, int &OutTargetId, vec2 &OutDelta)
{
	if(!pGameClient || SourceClientId < 0 || SourceClientId >= MAX_CLIENTS)
		return false;
	if(RequireHookReach && !pGameClient->Collision())
		return false;
	CCharacter *pSourcePredChar = pGameClient->m_RegularPredictedWorld.GetCharacterById(SourceClientId);

	const vec2 AimNorm = length(AimDir) > 0.001f ? normalize(AimDir) : vec2(0.0f, 1.0f);
	const float HalfFovRad = std::clamp(MaxFovDeg, 1.0f, 360.0f) * (pi / 180.0f) * 0.5f;
	const float MinDot = cosf(HalfFovRad);

	bool Found = false;
	float BestDist = 1e9f;
	for(int Cid = 0; Cid < MAX_CLIENTS; ++Cid)
	{
		if(Cid == SourceClientId || !pGameClient->m_Snap.m_aCharacters[Cid].m_Active)
			continue;
		if(pGameClient->m_aClients[Cid].m_Team == TEAM_SPECTATORS)
			continue;
		if(pSourcePredChar && !pGameClient->m_RegularPredictedWorld.m_Teams.CanCollide(SourceClientId, Cid))
			continue;

		const vec2 TargetPos((float)pGameClient->m_Snap.m_aCharacters[Cid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[Cid].m_Cur.m_Y);
		if(RequireNotAbove && TargetPos.y < SourcePos.y - 1.0f)
			continue;

		const vec2 Delta = TargetPos - SourcePos;
		const float Dist = length(Delta);
		if(Dist < 1.0f)
			continue;
		if(RequireHookReach)
		{
			if(Dist > HookLength + CCharacterCore::PhysicalSize() * 0.8f)
				continue;
			vec2 HitPos;
			if(pGameClient->Collision()->IntersectLine(SourcePos, TargetPos, &HitPos, nullptr) != 0)
				continue;
		}
		const vec2 Dir = Delta * (1.0f / Dist);
		if(dot(AimNorm, Dir) < MinDot)
			continue;

		if(Dist < BestDist)
		{
			BestDist = Dist;
			OutTargetId = Cid;
			OutDelta = Delta;
			Found = true;
		}
	}

	return Found;
}

bool FindSpecificTripleFlyTarget(CGameClient *pGameClient, int SourceClientId, int TargetClientId, const vec2 &SourcePos, const vec2 &AimDir, float MaxFovDeg, bool RequireNotAbove, bool RequireHookReach, float HookLength, vec2 &OutDelta)
{
	if(!pGameClient || SourceClientId < 0 || SourceClientId >= MAX_CLIENTS || TargetClientId < 0 || TargetClientId >= MAX_CLIENTS)
		return false;
	if(TargetClientId == SourceClientId || !pGameClient->m_Snap.m_aCharacters[TargetClientId].m_Active)
		return false;
	if(RequireHookReach && !pGameClient->Collision())
		return false;
	if(pGameClient->m_aClients[TargetClientId].m_Team == TEAM_SPECTATORS)
		return false;
	CCharacter *pSourcePredChar = pGameClient->m_RegularPredictedWorld.GetCharacterById(SourceClientId);
	if(pSourcePredChar && !pGameClient->m_RegularPredictedWorld.m_Teams.CanCollide(SourceClientId, TargetClientId))
		return false;

	const vec2 AimNorm = length(AimDir) > 0.001f ? normalize(AimDir) : vec2(0.0f, 1.0f);
	const float HalfFovRad = std::clamp(MaxFovDeg, 1.0f, 360.0f) * (pi / 180.0f) * 0.5f;
	const float MinDot = cosf(HalfFovRad);

	const vec2 TargetPos((float)pGameClient->m_Snap.m_aCharacters[TargetClientId].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[TargetClientId].m_Cur.m_Y);
	if(RequireNotAbove && TargetPos.y < SourcePos.y - 1.0f)
		return false;

	const vec2 Delta = TargetPos - SourcePos;
	const float Dist = length(Delta);
	if(Dist < 1.0f)
		return false;
	if(RequireHookReach)
	{
		if(Dist > HookLength + CCharacterCore::PhysicalSize() * 0.8f)
			return false;
		vec2 HitPos;
		if(pGameClient->Collision()->IntersectLine(SourcePos, TargetPos, &HitPos, nullptr) != 0)
			return false;
	}
	const vec2 Dir = Delta * (1.0f / Dist);
	if(dot(AimNorm, Dir) < MinDot)
		return false;
	OutDelta = Delta;
	return true;
}
} // namespace

void CCompanionMirrorBotScript::Tick(CGameClient *pGameClient, int BotId)
{
	(void)pGameClient;
	(void)BotId;
}

namespace
{
bool IsFrozenClient(CGameClient *pGameClient, int ClientId, int GameTick)
{
	if(!pGameClient || ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	if(!pGameClient->m_Snap.m_aCharacters[ClientId].m_Active)
		return false;
	if(pGameClient->m_aClients[ClientId].m_FreezeEnd > GameTick)
		return true;
	const int TileIndex = pGameClient->Collision()->GetPureMapIndex(vec2(
		(float)pGameClient->m_Snap.m_aCharacters[ClientId].m_Cur.m_X,
		(float)pGameClient->m_Snap.m_aCharacters[ClientId].m_Cur.m_Y));
	if(TileIndex >= 0)
	{
		const int Tile = pGameClient->Collision()->GetTileIndex(TileIndex);
		const int Front = pGameClient->Collision()->GetFrontTileIndex(TileIndex);
		return Tile == TILE_FREEZE || Tile == TILE_DEATH || Front == TILE_FREEZE || Front == TILE_DEATH;
	}
	return false;
}

bool IsFreezeLikePos(CGameClient *pGameClient, vec2 Pos)
{
	if(!pGameClient)
		return false;
	const int Index = pGameClient->Collision()->GetPureMapIndex(Pos);
	if(Index < 0)
		return false;
	const int Tile = pGameClient->Collision()->GetTileIndex(Index);
	const int Front = pGameClient->Collision()->GetFrontTileIndex(Index);
	const int Switch = pGameClient->Collision()->GetSwitchType(Index);
	return Tile == TILE_FREEZE || Tile == TILE_DEATH || Front == TILE_FREEZE || Front == TILE_DEATH ||
		Switch == TILE_FREEZE || Switch == TILE_DEATH;
}

bool IsFreezeLikeSegment(CGameClient *pGameClient, vec2 From, vec2 To)
{
	if(!pGameClient)
		return false;
	const int Steps = 10;
	for(int i = 0; i <= Steps; i++)
	{
		const float T = (float)i / (float)Steps;
		const vec2 P = mix(From, To, T);
		if(IsFreezeLikePos(pGameClient, P))
			return true;
	}
	return false;
}

int PickSingleIpType(int TypeMask, int PreferredMask)
{
	const bool Has4 = (TypeMask & NETTYPE_IPV4) != 0;
	const bool Has6 = (TypeMask & NETTYPE_IPV6) != 0;
	const bool Prefer4 = (PreferredMask & NETTYPE_IPV4) != 0;
	const bool Prefer6 = (PreferredMask & NETTYPE_IPV6) != 0;
	if(Has4 && Has6)
	{
		if(Prefer4 && !Prefer6)
			return NETTYPE_IPV4;
		if(Prefer6 && !Prefer4)
			return NETTYPE_IPV6;
		return NETTYPE_IPV4;
	}
	if(Has4)
		return NETTYPE_IPV4;
	if(Has6)
		return NETTYPE_IPV6;
	return 0;
}

int FindSaferLateralDir(CGameClient *pGameClient, vec2 BotPos, vec2 TargetPos)
{
	if(!pGameClient || !pGameClient->Collision())
		return 0;
	const int CandidateDirs[3] = {0, (TargetPos.x >= BotPos.x) ? 1 : -1, (TargetPos.x >= BotPos.x) ? -1 : 1};
	float BestScore = 1e9f;
	int BestDir = 0;
	for(int Dir : CandidateDirs)
	{
		vec2 Probe = BotPos + vec2((float)Dir * 32.0f, 0.0f);
		float Score = distance(Probe, TargetPos) * 0.35f;
		if(IsFreezeLikePos(pGameClient, Probe))
			Score += 2200.0f;
		if(IsFreezeLikeSegment(pGameClient, BotPos, Probe))
			Score += 1500.0f;
		if(pGameClient->Collision()->IntersectLine(BotPos, Probe, nullptr, nullptr))
			Score += 700.0f;
		if(Score < BestScore)
		{
			BestScore = Score;
			BestDir = Dir;
		}
	}
	return BestDir;
}

struct STowerSimEval
{
	bool m_Ok = false;
	float m_Score = 1e9f;
	float m_WorstDist = 0.0f;
	vec2 m_EndPos = vec2(0.0f, 0.0f);
	vec2 m_EndVel = vec2(0.0f, 0.0f);
	bool m_HookedPlayer = false;
	bool m_HookedGround = false;
};

struct SAsyncCompanionSimEval
{
	bool m_Ok = false;
	vec2 m_EndPos = vec2(0.0f, 0.0f);
	vec2 m_EndVel = vec2(0.0f, 0.0f);
	bool m_EverFreezeLike = false;
	int m_FirstFreezeTick = -1;
	int m_LastFreezeTick = -1;
	bool m_EndFreezeLike = false;
	float m_MinDistToTarget = 1e9f;
	bool m_HookedNonFrozenFriendly = false;
	bool m_HookedTarget = false;
	float m_EndDistToTarget = 1e9f;
	float m_ProgressToTarget = 0.0f;
};

bool SimulateAsyncCompanionCandidate(CGameClient *pGameClient, const CGameWorld *pBaseWorld, int BotCid, int TargetCid, const std::array<bool, MAX_CLIENTS> &FriendlyMask, const CNetObj_PlayerInput &In, vec2 TargetPos, int Ticks, SAsyncCompanionSimEval &Out)
{
	Out = {};
	if(!pGameClient || !pBaseWorld || BotCid < 0 || BotCid >= MAX_CLIENTS)
		return false;

	// Reuse a single working world object across all candidate simulations.
	static CGameWorld s_SimWorkWorld;
	s_SimWorkWorld.CopyWorld(const_cast<CGameWorld *>(pBaseWorld));
	CGameWorld &SimWorld = s_SimWorkWorld;

	CCharacter *pBot = SimWorld.GetCharacterById(BotCid);
	if(!pBot)
		return false;
	const vec2 StartPos = pBot->GetPos();
	CNetObj_PlayerInput Input = In;
	if(Input.m_TargetX == 0 && Input.m_TargetY == 0)
		Input.m_TargetY = -1;
	for(int t = 0; t < Ticks; t++)
	{
		pBot = SimWorld.GetCharacterById(BotCid);
		if(!pBot)
			return false;
		pBot->OnDirectInput(&Input);
		SimWorld.m_GameTick++;
		pBot->OnPredictedInput(&Input);
		SimWorld.Tick();
		pBot = SimWorld.GetCharacterById(BotCid);
		if(!pBot)
			return false;
		const vec2 Pos = pBot->GetPos();
		Out.m_MinDistToTarget = minimum(Out.m_MinDistToTarget, distance(Pos, TargetPos));
		const int Hooked = pBot->Core()->HookedPlayer();
		if(Hooked >= 0 && Hooked < MAX_CLIENTS)
		{
			if(Hooked == TargetCid)
				Out.m_HookedTarget = true;
			if(FriendlyMask[Hooked])
			{
				CCharacter *pHooked = SimWorld.GetCharacterById(Hooked);
				const bool HookedFrozen = pHooked && (pHooked->m_FreezeTime > 0 || pHooked->Core()->m_IsInFreeze || pHooked->Core()->m_DeepFrozen || pHooked->Core()->m_FreezeEnd != 0);
				if(!HookedFrozen)
					Out.m_HookedNonFrozenFriendly = true;
			}
		}
		const bool FreezeLike = IsFreezeLikePos(pGameClient, Pos);
		if(FreezeLike)
		{
			Out.m_EverFreezeLike = true;
			if(Out.m_FirstFreezeTick < 0)
				Out.m_FirstFreezeTick = t;
			Out.m_LastFreezeTick = t;
		}
		if(t == Ticks - 1)
			Out.m_EndFreezeLike = FreezeLike;
	}
	pBot = SimWorld.GetCharacterById(BotCid);
	if(!pBot)
		return false;
	Out.m_EndPos = pBot->GetPos();
	Out.m_EndVel = pBot->Core()->m_Vel;
	Out.m_EndDistToTarget = distance(Out.m_EndPos, TargetPos);
	Out.m_ProgressToTarget = distance(StartPos, TargetPos) - Out.m_EndDistToTarget;
	Out.m_Ok = true;
	return true;
}

bool SimulateTowerCandidate(CGameClient *pGameClient, int BotCid, int SupportCid, const CNetObj_PlayerInput &In, vec2 TargetPos, int Ticks, STowerSimEval &Out)
{
	Out = {};
	if(!pGameClient || BotCid < 0 || BotCid >= MAX_CLIENTS)
		return false;

	CGameWorld SimWorld;
	SimWorld.CopyWorld(&pGameClient->m_RegularPredictedWorld);
	CCharacter *pBot = SimWorld.GetCharacterById(BotCid);
	if(!pBot)
		return false;

	CNetObj_PlayerInput Input = In;
	if(Input.m_TargetX == 0 && Input.m_TargetY == 0)
		Input.m_TargetY = -1;
	float WorstDist = 0.0f;
	for(int t = 0; t < Ticks; t++)
	{
		pBot = SimWorld.GetCharacterById(BotCid);
		if(!pBot)
			return false;
		pBot->OnDirectInput(&Input);
		SimWorld.m_GameTick++;
		pBot->OnPredictedInput(&Input);
		SimWorld.Tick();
		pBot = SimWorld.GetCharacterById(BotCid);
		if(!pBot)
			return false;

		const float Dist = distance(pBot->GetPos(), TargetPos);
		WorstDist = maximum(WorstDist, Dist);
		if(pBot->Core()->HookedPlayer() >= 0)
			Out.m_HookedPlayer = true;
		if(pBot->Core()->m_HookState == HOOK_GRABBED && pBot->Core()->HookedPlayer() == -1)
			Out.m_HookedGround = true;
	}

	pBot = SimWorld.GetCharacterById(BotCid);
	if(!pBot)
		return false;
	CCharacter *pSupport = SupportCid >= 0 ? SimWorld.GetCharacterById(SupportCid) : nullptr;

	Out.m_EndPos = pBot->GetPos();
	Out.m_EndVel = pBot->Core()->m_Vel;
	Out.m_WorstDist = WorstDist;
	const float EndDist = distance(Out.m_EndPos, TargetPos);
	const float VelPenalty = length(Out.m_EndVel) * 6.0f;
	const float XPenalty = fabsf(Out.m_EndPos.x - TargetPos.x) * 1.3f;
	const float YPenalty = fabsf(Out.m_EndPos.y - TargetPos.y) * 0.7f;
	float SupportPenalty = 0.0f;
	if(pSupport)
	{
		const float HeadX = fabsf(pSupport->GetPos().x - Out.m_EndPos.x);
		const float HeadY = fabsf((pSupport->GetPos().y - 28.0f) - Out.m_EndPos.y);
		SupportPenalty = HeadX * 1.8f + HeadY * 0.8f;
	}
	Out.m_Score = EndDist * 3.0f + XPenalty + YPenalty + VelPenalty + SupportPenalty + WorstDist * 0.12f;
	Out.m_Ok = true;
	return true;
}

void BuildTowerHoldInput(CGameClient *pGameClient, int BotCid, vec2 TargetPos, CNetObj_PlayerInput &Out)
{
	mem_zero(&Out, sizeof(Out));
	Out.m_PlayerFlags = PLAYERFLAG_PLAYING;
	Out.m_TargetX = 1;
	if(!pGameClient || BotCid < 0 || BotCid >= MAX_CLIENTS || !pGameClient->m_Snap.m_aCharacters[BotCid].m_Active)
		return;

	const vec2 Pos((float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_Y);
	const vec2 Vel((float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_VelX / 256.0f * 50.0f,
		(float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_VelY / 256.0f * 50.0f);
	const float ErrX = TargetPos.x - Pos.x;
	const float Kp = (float)std::clamp(g_Config.m_ClZzAsyncBotsTowerHoldKpX, 1, 80) / 100.0f;
	const float Kd = (float)std::clamp(g_Config.m_ClZzAsyncBotsTowerHoldKdX, 1, 200) / 100.0f;
	const float Deadzone = (float)std::clamp(g_Config.m_ClZzAsyncBotsTowerHoldDeadzoneX, 0, 10);
	const float U = Kp * ErrX - Kd * Vel.x;
	if(fabsf(ErrX) <= Deadzone && fabsf(Vel.x) < 0.22f)
		Out.m_Direction = 0;
	else
		Out.m_Direction = U > 0.35f ? 1 : (U < -0.35f ? -1 : 0);

	Out.m_TargetX = (int)std::clamp((int)round_to_int(U * 110.0f), -150, 150);
	Out.m_TargetY = -90;
	Out.m_Hook = 0;
	Out.m_Jump = 0;
}

vec2 ComputeComeWaypoint(CGameClient *pGameClient, vec2 BotPos, vec2 TargetPos)
{
	if(!pGameClient || !pGameClient->Collision())
		return TargetPos;
	if(pGameClient->Collision()->IntersectLine(BotPos, TargetPos, nullptr, nullptr) == 0 &&
		!IsFreezeLikeSegment(pGameClient, BotPos, TargetPos))
	{
		return TargetPos;
	}

	vec2 To = TargetPos - BotPos;
	if(length(To) < 0.001f)
		return TargetPos;
	const vec2 Forward = normalize(To);
	const vec2 Side(-Forward.y, Forward.x);
	const float Step = 96.0f;
	const float SideScale[9] = {0.0f, 0.35f, -0.35f, 0.70f, -0.70f, 1.05f, -1.05f, 1.40f, -1.40f};

	float BestScore = 1e9f;
	vec2 Best = BotPos + Forward * Step;
	for(float S : SideScale)
	{
		vec2 Candidate = BotPos + Forward * Step + Side * (S * Step);
		float Score = distance(Candidate, TargetPos);
		if(IsFreezeLikePos(pGameClient, Candidate))
			Score += 380.0f; // still risky in /come, but not forbidden
		if(IsFreezeLikeSegment(pGameClient, BotPos, Candidate))
			Score += 290.0f;
		if(pGameClient->Collision()->IntersectLine(BotPos, Candidate, nullptr, nullptr) != 0)
			Score += 180.0f;
		if(Score < BestScore)
		{
			BestScore = Score;
			Best = Candidate;
		}
	}
	return Best;
}

void SetInputAim(CNetObj_PlayerInput &Input, vec2 AimDir, float Scale = 380.0f)
{
	if(length(AimDir) < 0.001f)
		AimDir = vec2(1.0f, 0.0f);
	else
		AimDir = normalize(AimDir);
	Input.m_TargetX = (int)round_to_int(AimDir.x * Scale);
	Input.m_TargetY = (int)round_to_int(AimDir.y * Scale);
	if(Input.m_TargetX == 0 && Input.m_TargetY == 0)
		Input.m_TargetX = 1;
}

float AsyncCompanionHookLength(CGameClient *pGameClient, int BotCid)
{
	if(!pGameClient || BotCid < 0 || BotCid >= MAX_CLIENTS)
		return 380.0f;
	float HookLen = maximum(1.0f, (float)pGameClient->m_aClients[BotCid].m_Predicted.m_Tuning.m_HookLength);
	if(CCharacter *pBotChar = pGameClient->m_RegularPredictedWorld.GetCharacterById(BotCid))
	{
		const CTuningParams *pTune = pBotChar->GetTuning(pBotChar->GetOverriddenTuneZone());
		if(pTune)
			HookLen = maximum(1.0f, (float)pTune->m_HookLength);
	}
	return HookLen;
}

bool BuildRescueLikeCompanionInput(CGameClient *pGameClient, int Tick, int BotCid, int VictimCid, bool ComeMode, CNetObj_PlayerInput &Out)
{
	if(!pGameClient || !pGameClient->Collision())
		return false;
	if(BotCid < 0 || BotCid >= MAX_CLIENTS || VictimCid < 0 || VictimCid >= MAX_CLIENTS)
		return false;
	if(!pGameClient->m_Snap.m_aCharacters[BotCid].m_Active || !pGameClient->m_Snap.m_aCharacters[VictimCid].m_Active)
		return false;

	const vec2 BotPos((float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_Y);
	const vec2 VictimPos((float)pGameClient->m_Snap.m_aCharacters[VictimCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[VictimCid].m_Cur.m_Y);
	const vec2 Delta = VictimPos - BotPos;
	const float Dist = length(Delta);
	const vec2 BotVel((float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_VelX / 256.0f * 50.0f,
		(float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_VelY / 256.0f * 50.0f);
	const bool VictimFrozen = IsFrozenClient(pGameClient, VictimCid, Tick);
	const bool BotSafeNow = !IsFreezeLikePos(pGameClient, BotPos);
	const bool PathRisk = IsFreezeLikeSegment(pGameClient, BotPos, VictimPos);
	const bool Grounded = pGameClient->Collision()->CheckPoint(BotPos + vec2(0.0f, 18.0f));
	const bool HasAirJump = (pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_Jumped & 2) == 0;
	const bool HookingVictimNow = pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_HookedPlayer == VictimCid;
	const bool CanHammerNow = Dist <= 78.0f;
	const bool LineBlocked = pGameClient->Collision()->IntersectLine(BotPos, VictimPos, nullptr, nullptr) != 0;
	const float HookLen = AsyncCompanionHookLength(pGameClient, BotCid);
	bool CanHookNow = Dist > 42.0f && Dist < HookLen && !LineBlocked;
	if(!ComeMode && PathRisk && !VictimFrozen)
		CanHookNow = false;
	const bool PreferHammer = BotSafeNow && CanHammerNow;

	mem_zero(&Out, sizeof(Out));
	Out.m_PlayerFlags = PLAYERFLAG_PLAYING;
	Out.m_WantedWeapon = WEAPON_HAMMER + 1;
	SetInputAim(Out, Delta);

	const int DirToVictim = fabsf(Delta.x) > 10.0f ? (Delta.x > 0.0f ? 1 : -1) : 0;
	const int AwayFromVictim = fabsf(Delta.x) > 10.0f ? (Delta.x > 0.0f ? -1 : 1) : 0;
	int RescueDir = VictimFrozen ? AwayFromVictim : DirToVictim;
	if(RescueDir == 0)
		RescueDir = DirToVictim;

	// Companion-like immediate rescue priority: hammer first when close and safe.
	if(CanHammerNow)
	{
		Out.m_Direction = DirToVictim;
		Out.m_Fire = 1;
		const bool NeedLift = Delta.y < -12.0f || (fabsf(Delta.x) < 34.0f && Delta.y < 10.0f);
		Out.m_Jump = NeedLift && (Grounded || HasAirJump) ? 1 : 0;
		return true;
	}

	// Finisher: if we already hold victim with hook and got close enough, release hook and hammer now.
	if(HookingVictimNow && Dist <= 118.0f)
	{
		Out.m_Direction = DirToVictim;
		Out.m_Hook = 0;
		Out.m_Fire = 1;
		const bool NeedLift = Delta.y < -8.0f || VictimPos.y > BotPos.y + 6.0f;
		Out.m_Jump = NeedLift && (Grounded || HasAirJump) ? 1 : 0;
		return true;
	}

	// Companion-like pull/drag phase when not in direct hammer range.
	if((HookingVictimNow || CanHookNow) && !PreferHammer)
	{
		Out.m_Direction = RescueDir;
		Out.m_Hook = 1;
		const bool NeedJump = Delta.y < -10.0f || (!BotSafeNow && Delta.y < 0.0f) || (fabsf(BotVel.x) < 0.25f && Dist > 72.0f);
		Out.m_Jump = NeedJump && (Grounded || HasAirJump) ? 1 : 0;
		return true;
	}

	// Companion-like approach/positioning around victim.
	Out.m_Direction = VictimFrozen ? DirToVictim : RescueDir;
	const bool Stuck = fabsf(BotVel.x) < 0.2f && Dist > 84.0f && Out.m_Direction != 0;
	const bool NeedHeight = Delta.y < -22.0f || (VictimFrozen && Delta.y < -8.0f);
	Out.m_Jump = (NeedHeight || Stuck) && (Grounded || HasAirJump) ? 1 : 0;
	if(HookingVictimNow)
		Out.m_Hook = 0;

	// Outside /come keep rescuer safe like regular companion.
	if(!ComeMode)
	{
		const vec2 ForwardProbe = BotPos + vec2((float)Out.m_Direction * 28.0f, 0.0f);
		if(IsFreezeLikePos(pGameClient, ForwardProbe) || PathRisk)
		{
			Out.m_Hook = 0;
			if(IsFreezeLikePos(pGameClient, ForwardProbe))
			{
				Out.m_Direction = 0;
				Out.m_Jump = 0;
			}
		}
	}

	return true;
}
} // namespace

bool CAsyncProxyBotManager::ParseSingleProxy(const char *pRaw, SProxyEntry &Out)
{
	if(!pRaw || pRaw[0] == '\0')
		return false;

	char aProxy[192];
	str_copy(aProxy, pRaw, sizeof(aProxy));
	char *pTrim = str_skip_whitespaces(aProxy);
	str_utf8_trim_right(pTrim);
	if(pTrim[0] == '\0')
		return false;

	char *pNoScheme = pTrim;
	if(str_startswith_nocase(pNoScheme, "socks5://"))
		pNoScheme += 9;

	char aUser[64] = "";
	char aPass[64] = "";
	const char *pHostPart = pNoScheme;
	char *pAt = std::strchr(pNoScheme, '@');
	if(pAt)
	{
		*pAt = '\0';
		pHostPart = pAt + 1;
		char *pColon = std::strchr(pNoScheme, ':');
		if(pColon)
		{
			*pColon = '\0';
			str_copy(aUser, pNoScheme, sizeof(aUser));
			str_copy(aPass, pColon + 1, sizeof(aPass));
		}
		else
		{
			str_copy(aUser, pNoScheme, sizeof(aUser));
		}
	}

	NETADDR Addr;
	if(net_addr_from_str(&Addr, pHostPart) != 0)
	{
		char aHost[128] = "";
		int Port = 1080;
		const char *pColon = str_rchr(pHostPart, ':');
		if(pColon && str_find(pHostPart, ":") == pColon)
		{
			const int HostLen = minimum((int)(pColon - pHostPart), (int)sizeof(aHost) - 1);
			mem_copy(aHost, pHostPart, HostLen);
			aHost[HostLen] = '\0';
			const int ParsedPort = str_toint(pColon + 1);
			if(ParsedPort > 0 && ParsedPort <= 65535)
				Port = ParsedPort;
		}
		else
		{
			str_copy(aHost, pHostPart, sizeof(aHost));
		}

		if(aHost[0] == '\0' || net_host_lookup(aHost, &Addr, NETTYPE_IPV4 | NETTYPE_IPV6) != 0)
			return false;
		Addr.port = Port;
	}

	if(Addr.port == 0)
		Addr.port = 1080;

	Out.m_Addr = Addr;
	str_copy(Out.m_aUsername, aUser, sizeof(Out.m_aUsername));
	str_copy(Out.m_aPassword, aPass, sizeof(Out.m_aPassword));
	str_copy(Out.m_aRaw, pRaw, sizeof(Out.m_aRaw));
	return true;
}

bool CAsyncProxyBotManager::ParseProxyList(const char *pRaw, std::vector<SProxyEntry> &vOut, CGameClient *pGameClient)
{
	vOut.clear();
	if(!pRaw || pRaw[0] == '\0')
		return false;

	char aBuf[4096];
	str_copy(aBuf, pRaw, sizeof(aBuf));

	char *p = aBuf;
	while(*p)
	{
		while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',' || *p == ';')
			++p;
		if(*p == '\0')
			break;

		char *pEnd = p;
		while(*pEnd && *pEnd != '\n' && *pEnd != '\r' && *pEnd != ',' && *pEnd != ';')
			++pEnd;
		const char Saved = *pEnd;
		*pEnd = '\0';

		SProxyEntry Entry;
		if(ParseSingleProxy(p, Entry))
			vOut.push_back(Entry);
		else if(pGameClient)
		{
			char aMsg[256];
			str_format(aMsg, sizeof(aMsg), "async bots: bad proxy '%s'", p);
			pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz_async_bots", aMsg);
		}

		*pEnd = Saved;
		p = pEnd;
	}
	return !vOut.empty();
}

bool CAsyncProxyBotManager::HasSavedMap(CGameClient *pGameClient, const char *pMapName, int MapCrc, const std::optional<SHA256_DIGEST> &MapSha256)
{
	if(!pGameClient || !pMapName || pMapName[0] == '\0')
		return false;

	char aFilename[512];
	char aFound[IO_MAX_PATH_LENGTH];

	// maps/<name>.map
	str_format(aFilename, sizeof(aFilename), "%s.map", pMapName);
	if(pGameClient->Storage()->FindFile(aFilename, "maps", IStorage::TYPE_ALL, aFound, sizeof(aFound)))
		return true;

	// downloadedmaps/<name>_<sha>.map
	if(MapSha256.has_value())
	{
		char aSha256[SHA256_MAXSTRSIZE];
		sha256_str(MapSha256.value(), aSha256, sizeof(aSha256));
		str_format(aFilename, sizeof(aFilename), "%s_%s.map", pMapName, aSha256);
		if(pGameClient->Storage()->FindFile(aFilename, "downloadedmaps", IStorage::TYPE_ALL, aFound, sizeof(aFound)))
			return true;
	}

	// downloadedmaps/<name>_<crc>.map
	str_format(aFilename, sizeof(aFilename), "%s_%08x.map", pMapName, MapCrc);
	if(pGameClient->Storage()->FindFile(aFilename, "downloadedmaps", IStorage::TYPE_ALL, aFound, sizeof(aFound)))
		return true;

	// Legacy fallback: downloadedmaps/<name>.map
	str_format(aFilename, sizeof(aFilename), "%s.map", pMapName);
	if(pGameClient->Storage()->FindFile(aFilename, "downloadedmaps", IStorage::TYPE_ALL, aFound, sizeof(aFound)))
		return true;
	return false;
}

bool CAsyncProxyBotManager::ResolveServerAddress(CGameClient *pGameClient, int LookupTypes, NETADDR &Out)
{
	mem_zero(&Out, sizeof(Out));
	if(!pGameClient || !pGameClient->Client())
		return false;

	NETADDR Candidate = pGameClient->Client()->ServerAddress();

	// Normalize type flags for UDP SOCKS5 transport.
	const bool HasIpv4 = (Candidate.type & NETTYPE_IPV4) != 0;
	const bool HasIpv6 = (Candidate.type & NETTYPE_IPV6) != 0;
	const bool HasWs4 = (Candidate.type & NETTYPE_WEBSOCKET_IPV4) != 0;
	const bool HasWs6 = (Candidate.type & NETTYPE_WEBSOCKET_IPV6) != 0;

	int SanitizedType = 0;
	if(HasIpv4 || HasWs4)
		SanitizedType |= NETTYPE_IPV4;
	if(HasIpv6 || HasWs6)
		SanitizedType |= NETTYPE_IPV6;
	Candidate.type = SanitizedType;

	if((Candidate.type & (NETTYPE_IPV4 | NETTYPE_IPV6)) != 0 && Candidate.port != 0)
	{
		Out = Candidate;
		return true;
	}

	// Fallback: resolve from original connect string.
	const char *pConnectRaw = pGameClient->Client()->ConnectAddressString();
	if(!pConnectRaw || pConnectRaw[0] == '\0')
		return false;

	char aBuf[1024];
	str_copy(aBuf, pConnectRaw, sizeof(aBuf));

	char *p = aBuf;
	while(*p)
	{
		while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')
			++p;
		if(*p == '\0')
			break;

		char *pEnd = p;
		while(*pEnd && *pEnd != ',')
			++pEnd;
		const char Saved = *pEnd;
		*pEnd = '\0';

		str_utf8_trim_right(p);
		char *pToken = str_skip_whitespaces(p);
		if(pToken[0] != '\0')
		{
			NETADDR UrlAddr;
			mem_zero(&UrlAddr, sizeof(UrlAddr));
			char aHost[256];
			mem_zero(aHost, sizeof(aHost));
			const int UrlParseResult = net_addr_from_url(&UrlAddr, pToken, aHost, sizeof(aHost));
			if(UrlParseResult > 0)
				str_copy(aHost, pToken, sizeof(aHost));

			if(aHost[0] != '\0')
			{
				NETADDR Resolved;
				mem_zero(&Resolved, sizeof(Resolved));
				const int QueryTypes = (LookupTypes & (NETTYPE_IPV4 | NETTYPE_IPV6)) != 0 ? LookupTypes : (NETTYPE_IPV4 | NETTYPE_IPV6);
				if(net_host_lookup(aHost, &Resolved, QueryTypes) == 0)
				{
					Resolved.port = UrlAddr.port != 0 ? UrlAddr.port : (Candidate.port != 0 ? Candidate.port : 8303);
					Resolved.type &= (NETTYPE_IPV4 | NETTYPE_IPV6);

					if((Resolved.type & (NETTYPE_IPV4 | NETTYPE_IPV6)) != 0)
					{
						Out = Resolved;
						return true;
					}
				}
			}
		}

		*pEnd = Saved;
		p = pEnd;
	}

	return false;
}

void CAsyncProxyBotManager::SendPacket(SBotSession &Bot, CMsgPacker &Msg, int Flags)
{
	CPacker Packer;
	if(!RepackBotMsg(Msg, Bot.m_Sixup, Packer))
		return;

	CNetChunk Packet;
	mem_zero(&Packet, sizeof(Packet));
	Packet.m_ClientId = 0;
	Packet.m_Address = Bot.m_ServerAddr;
	Packet.m_DataSize = Packer.Size();
	Packet.m_pData = Packer.Data();
	if(Flags & MSGFLAG_VITAL)
		Packet.m_Flags |= NETSENDFLAG_VITAL;
	if(Flags & MSGFLAG_FLUSH)
		Packet.m_Flags |= NETSENDFLAG_FLUSH;
	Bot.m_NetClient.Send(&Packet);
}

void CAsyncProxyBotManager::SendClientInfo(CGameClient *pGameClient, SBotSession &Bot)
{
	int IdentityMsg = 0;
	switch(g_Config.m_ClZzNetIdentity)
	{
	case 1: IdentityMsg = NETMSG_IAM_QXD; break;
	case 2: IdentityMsg = NETMSG_IAMTATER; break;
	case 3: IdentityMsg = NETMSG_IAM_CACTUS; break;
	case 4: IdentityMsg = NETMSG_IAM_AIODOB; break;
	case 5: IdentityMsg = NETMSG_IAM_FEX; break;
	case 6: IdentityMsg = NETMSG_IAM_STA; break;
	case 7: IdentityMsg = NETMSG_IAM_PULSE; break;
	case 8: IdentityMsg = NETMSG_IAM_SCLIENT; break;
	case 9: IdentityMsg = NETMSG_IAM_CHILLERBOT; break;
	case 10: IdentityMsg = NETMSG_IAM_KOSHKA; break;
	case 11: IdentityMsg = NETMSG_IAM_RUSHIE; break;
	case 12: IdentityMsg = NETMSG_IAM_DUCKCLIENT; break;
	case 13: IdentityMsg = NETMSG_IAM_AXIS; break;
	case 14: IdentityMsg = NETMSG_IAM_KAIZO; break;
	case 15: IdentityMsg = NETMSG_IAM_JSCLIENT; break;
	case 16: IdentityMsg = NETMSG_IAM_NOFIS; break;
	default: IdentityMsg = 0; break; // ddnet mode: no IAM message
	}

	if(IdentityMsg != 0)
	{
		CMsgPacker MsgIdentity(IdentityMsg, true);
		MsgIdentity.AddString(pGameClient->DDNetVersionStr());
		SendPacket(Bot, MsgIdentity, MSGFLAG_VITAL);
	}

	// Legacy TClient identity packet (disabled by default):
	// CMsgPacker MsgTClient(NETMSG_IAMTATER, true);
	// MsgTClient.AddString(TCLIENT_VERSION " built on " __DATE__ ", " __TIME__);
	// SendPacket(Bot, MsgTClient, MSGFLAG_VITAL);

	// Keep connection id stable across server-requested reconnects for stricter servers.
	if(Bot.m_ConnectionId == UUID_ZEROED)
		Bot.m_ConnectionId = RandomUuid();

	CMsgPacker MsgVer(NETMSG_CLIENTVER, true);
	MsgVer.AddRaw(&Bot.m_ConnectionId, sizeof(Bot.m_ConnectionId));
	MsgVer.AddInt(pGameClient->DDNetVersion());
	MsgVer.AddString(pGameClient->DDNetVersionStr());
	SendPacket(Bot, MsgVer, MSGFLAG_VITAL);

	CMsgPacker MsgInfo(NETMSG_INFO, true);
	if(Bot.m_Sixup)
	{
		MsgInfo.AddString(GAME_NETVERSION7);
		MsgInfo.AddString(g_Config.m_Password);
		MsgInfo.AddInt(pGameClient->ClientVersion7());
	}
	else
	{
		MsgInfo.AddString(pGameClient->NetVersion());
		MsgInfo.AddString(g_Config.m_Password);
	}
	SendPacket(Bot, MsgInfo, MSGFLAG_VITAL | MSGFLAG_FLUSH);
}

void CAsyncProxyBotManager::SendStartInfo(CGameClient *pGameClient, SBotSession &Bot)
{
	(void)pGameClient;
	CNetMsg_Cl_StartInfo Msg;
	Msg.m_pName = Bot.m_aName;
	Msg.m_pClan = Bot.m_aClan;
	Msg.m_Country = Bot.m_Country;
	Msg.m_pSkin = Bot.m_aSkin;
	Msg.m_UseCustomColor = Bot.m_UseCustomColor;
	Msg.m_ColorBody = Bot.m_ColorBody;
	Msg.m_ColorFeet = Bot.m_ColorFeet;
	CMsgPacker Packer(&Msg);
	Msg.Pack(&Packer);
	SendPacket(Bot, Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
}

void CAsyncProxyBotManager::SendChangeInfo(SBotSession &Bot)
{
	CNetMsg_Cl_ChangeInfo Msg;
	Msg.m_pName = Bot.m_aName;
	Msg.m_pClan = Bot.m_aClan;
	Msg.m_Country = Bot.m_Country;
	Msg.m_pSkin = Bot.m_aSkin;
	Msg.m_UseCustomColor = Bot.m_UseCustomColor;
	Msg.m_ColorBody = Bot.m_ColorBody;
	Msg.m_ColorFeet = Bot.m_ColorFeet;
	CMsgPacker Packer(&Msg);
	Msg.Pack(&Packer);
	SendPacket(Bot, Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
}

void CAsyncProxyBotManager::SendMapRequest(SBotSession &Bot)
{
	CMsgPacker Msg(Bot.m_Sixup ? protocol7::NETMSG_REQUEST_MAP_DATA : NETMSG_REQUEST_MAP_DATA, true, Bot.m_Sixup);
	if(!Bot.m_Sixup)
		Msg.AddInt(Bot.m_MapChunk);
	SendPacket(Bot, Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH);
}

void CAsyncProxyBotManager::SendEnterGame(SBotSession &Bot)
{
	CMsgPacker Msg(NETMSG_ENTERGAME, true);
	SendPacket(Bot, Msg, MSGFLAG_VITAL | MSGFLAG_FLUSH);
	Bot.m_EnterGameSent = true;
}

void CAsyncProxyBotManager::SendEmoticon(SBotSession &Bot, int Emoticon)
{
	CNetMsg_Cl_Emoticon Msg;
	Msg.m_Emoticon = Emoticon;
	CMsgPacker Packer(&Msg);
	Msg.Pack(&Packer);
	SendPacket(Bot, Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
}

void CAsyncProxyBotManager::SendChat(SBotSession &Bot, const char *pText)
{
	if(!pText || pText[0] == '\0')
		return;

	CNetMsg_Cl_Say Msg;
	Msg.m_Team = 0;
	Msg.m_pMessage = pText;
	CMsgPacker Packer(&Msg);
	Msg.Pack(&Packer);
	SendPacket(Bot, Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
}

void CAsyncProxyBotManager::SendKill(SBotSession &Bot)
{
	CNetMsg_Cl_Kill Msg;
	CMsgPacker Packer(&Msg);
	Msg.Pack(&Packer);
	SendPacket(Bot, Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
}

void CAsyncProxyBotManager::BuildInputTemplate(CGameClient *pGameClient, SBotSession &Bot, CNetObj_PlayerInput &Input)
{
	if(g_Config.m_ClZzAsyncBotsCompanionMode || g_Config.m_ClZzAsyncBotsTowerEnable)
	{
		BuildCompanionInput(pGameClient, Bot, Input);
		ApplyTripleFlyHook(pGameClient, Bot, Input);
		return;
	}

	mem_zero(&Input, sizeof(Input));
	if(!pGameClient)
	{
		Input.m_TargetX = 1;
		Input.m_PlayerFlags = PLAYERFLAG_PLAYING;
		return;
	}

	bool MirroredFromInput = false;
	bool MirroredFromLocalDirect = false;
	if(g_Config.m_ClZzAsyncBotsMirrorEnable && g_Config.m_ClZzAsyncBotsMirrorClientId >= 0 && g_Config.m_ClZzAsyncBotsMirrorClientId < MAX_CLIENTS)
	{
		const int SourceId = g_Config.m_ClZzAsyncBotsMirrorClientId;
		int SourceLocalTee = -1;
		if(SourceId == pGameClient->m_aLocalIds[0])
			SourceLocalTee = 0;
		else if(SourceId == pGameClient->m_aLocalIds[1])
			SourceLocalTee = 1;

		if(SourceLocalTee >= 0 && SourceLocalTee < NUM_DUMMIES)
		{
			Input = pGameClient->m_Controls.m_aInputData[SourceLocalTee];
			MirroredFromInput = true;
			MirroredFromLocalDirect = true;
			if(Input.m_WantedWeapon < 0 || Input.m_WantedWeapon > NUM_WEAPONS)
				Input.m_WantedWeapon = 0;
		}
		else
		{
			const auto &SrcChar = pGameClient->m_Snap.m_aCharacters[SourceId];
			if(SrcChar.m_Active)
			{
				Input.m_Direction = std::clamp(SrcChar.m_Cur.m_Direction, -1, 1);
				const int Jumped = SrcChar.m_Cur.m_Jumped;
				Input.m_Jump = (Jumped == 1 || Jumped == 3) ? 1 : 0;
				Input.m_Hook = SrcChar.m_Cur.m_HookState != HOOK_IDLE ? 1 : 0;
				if(Bot.m_MirrorLastSourceAttackTick != SrcChar.m_Cur.m_AttackTick)
				{
					Input.m_Fire = 1;
					Bot.m_MirrorLastSourceAttackTick = SrcChar.m_Cur.m_AttackTick;
				}
				else
				{
					Input.m_Fire = 0;
				}
				if(Bot.m_MirrorLastSourceWeapon != SrcChar.m_Cur.m_Weapon)
				{
					if(SrcChar.m_Cur.m_Weapon >= 0 && SrcChar.m_Cur.m_Weapon < NUM_WEAPONS)
						Input.m_WantedWeapon = SrcChar.m_Cur.m_Weapon + 1;
					else
						Input.m_WantedWeapon = 0;
					Bot.m_MirrorLastSourceWeapon = SrcChar.m_Cur.m_Weapon;
				}
				const float Angle = (float)SrcChar.m_Cur.m_Angle / 256.0f;
				Input.m_TargetX = (int)round_to_int(cosf(Angle) * 360.0f);
				Input.m_TargetY = (int)round_to_int(sinf(Angle) * 360.0f);
				MirroredFromInput = true;
			}
			else
			{
				Input.m_TargetX = 1;
			}
		}

		if(g_Config.m_ClZzAsyncBotsMirrorAimOurCursor)
		{
			int CursorLocalTee = SourceLocalTee;
			int CursorLocalCid = SourceId;
			if(CursorLocalTee < 0 || CursorLocalTee >= NUM_DUMMIES || CursorLocalCid < 0 || CursorLocalCid >= MAX_CLIENTS ||
				!pGameClient->m_Snap.m_aCharacters[CursorLocalCid].m_Active)
			{
				CursorLocalTee = -1;
				CursorLocalCid = -1;
				for(int Tee = 0; Tee < NUM_DUMMIES; ++Tee)
				{
					const int LocalCid = pGameClient->m_aLocalIds[Tee];
					if(LocalCid >= 0 && LocalCid < MAX_CLIENTS && pGameClient->m_Snap.m_aCharacters[LocalCid].m_Active)
					{
						CursorLocalTee = Tee;
						CursorLocalCid = LocalCid;
						break;
					}
				}
			}

			int BotCid = Bot.m_KnownClientId;
			const bool CachedBotValid = BotCid >= 0 && BotCid < MAX_CLIENTS && pGameClient->m_Snap.m_aCharacters[BotCid].m_Active &&
				str_comp(pGameClient->m_aClients[BotCid].m_aName, Bot.m_aName) == 0;
			if(!CachedBotValid)
			{
				BotCid = -1;
				for(int Cid = 0; Cid < MAX_CLIENTS; ++Cid)
				{
					if(!pGameClient->m_Snap.m_aCharacters[Cid].m_Active)
						continue;
					if(str_comp(pGameClient->m_aClients[Cid].m_aName, Bot.m_aName) == 0)
					{
						BotCid = Cid;
						break;
					}
				}
				Bot.m_KnownClientId = BotCid;
			}

			if(CursorLocalTee >= 0 && CursorLocalTee < NUM_DUMMIES &&
				CursorLocalCid >= 0 && CursorLocalCid < MAX_CLIENTS &&
				BotCid >= 0 && BotCid < MAX_CLIENTS &&
				pGameClient->m_Snap.m_aCharacters[CursorLocalCid].m_Active &&
				pGameClient->m_Snap.m_aCharacters[BotCid].m_Active)
			{
				const vec2 LocalPos((float)pGameClient->m_Snap.m_aCharacters[CursorLocalCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[CursorLocalCid].m_Cur.m_Y);
				const vec2 BotPos((float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_Y);
				vec2 WorldCursorPos = pGameClient->m_Controls.m_aTargetPos[CursorLocalTee];
				if(length(WorldCursorPos) < 0.001f)
					WorldCursorPos = LocalPos + pGameClient->m_Controls.m_aMousePos[CursorLocalTee] * pGameClient->m_Camera.m_Zoom;
				const vec2 AimPos = WorldCursorPos - BotPos;
				Input.m_TargetX = round_to_int(AimPos.x);
				Input.m_TargetY = round_to_int(AimPos.y);
			}
		}
	}
	else
	{
		// Keep bots idle unless explicit mirror mode is enabled.
		Input.m_TargetX = 1;
	}

	if(Input.m_TargetX == 0 && Input.m_TargetY == 0)
		Input.m_TargetX = 1;

	Input.m_PlayerFlags |= PLAYERFLAG_PLAYING;
	if(!MirroredFromInput || !MirroredFromLocalDirect)
		Input.m_PlayerFlags &= ~(PLAYERFLAG_CHATTING | PLAYERFLAG_IN_MENU | PLAYERFLAG_SCOREBOARD | PLAYERFLAG_SPEC_CAM);

	ApplyTripleFlyHook(pGameClient, Bot, Input);
}

void CAsyncProxyBotManager::ApplyTripleFlyHook(CGameClient *pGameClient, SBotSession &Bot, CNetObj_PlayerInput &Input)
{
	if(!g_Config.m_ClZzAsyncBotsTripleFly || !pGameClient)
	{
		Bot.m_TripleFlyTargetClientId = -1;
		Bot.m_TripleFlyLastHookedClientId = -1;
		Bot.m_TripleFlyNextRehookTick = 0;
		Bot.m_TripleFlyNextNoHookSearchTick = 0;
		Bot.m_TripleFlyPulsePending = false;
		Bot.m_TripleFlyHasLastVelY = false;
		return;
	}

	int BotCid = Bot.m_KnownClientId;
	const bool CachedValid = BotCid >= 0 && BotCid < MAX_CLIENTS && pGameClient->m_Snap.m_aCharacters[BotCid].m_Active &&
		str_comp(pGameClient->m_aClients[BotCid].m_aName, Bot.m_aName) == 0;
	if(!CachedValid)
	{
		BotCid = -1;
		for(int Cid = 0; Cid < MAX_CLIENTS; ++Cid)
		{
			if(!pGameClient->m_Snap.m_aCharacters[Cid].m_Active)
				continue;
			if(str_comp(pGameClient->m_aClients[Cid].m_aName, Bot.m_aName) == 0)
			{
				BotCid = Cid;
				break;
			}
		}
		Bot.m_KnownClientId = BotCid;
	}
	if(BotCid < 0 || BotCid >= MAX_CLIENTS || !pGameClient->m_Snap.m_aCharacters[BotCid].m_Active)
	{
		Bot.m_TripleFlyTargetClientId = -1;
		Bot.m_TripleFlyLastHookedClientId = -1;
		Bot.m_TripleFlyNextRehookTick = 0;
		Bot.m_TripleFlyNextNoHookSearchTick = 0;
		Bot.m_TripleFlyPulsePending = false;
		Bot.m_TripleFlyHasLastVelY = false;
		Input.m_Hook = 0;
		return;
	}

	const vec2 BotPos((float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_Y);
	float HookLength = maximum(1.0f, (float)pGameClient->m_aClients[BotCid].m_Predicted.m_Tuning.m_HookLength);
	float HookDuration = 1.9f; // default fallback when server tune info is unavailable
	if(pGameClient->m_aClients[BotCid].m_Predicted.m_Tuning.m_HookDuration > 0.05f)
		HookDuration = (float)pGameClient->m_aClients[BotCid].m_Predicted.m_Tuning.m_HookDuration;
	if(CCharacter *pBotPredChar = pGameClient->m_RegularPredictedWorld.GetCharacterById(BotCid))
	{
		const CTuningParams *pTune = pBotPredChar->GetTuning(pBotPredChar->GetOverriddenTuneZone());
		if(pTune)
		{
			HookLength = maximum(1.0f, (float)pTune->m_HookLength);
			if(pTune->m_HookDuration > 0.05f)
				HookDuration = (float)pTune->m_HookDuration;
		}
	}
	HookDuration = maximum(0.05f, HookDuration);
	vec2 Aim((float)Input.m_TargetX, (float)Input.m_TargetY);
	if(length(Aim) < 0.001f)
		Aim = vec2(0.0f, 1.0f);

	const int GameTick = maximum(1, pGameClient->Client()->GameTick(g_Config.m_ClDummy));
	const int TickSpeed = maximum(1, pGameClient->Client()->GameTickSpeed());
	const float CurVelY = (float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_VelY / 256.0f * 50.0f;
	bool VerticalSpeedChangedHard = false;
	const float SharpVelYThreshold = (float)std::clamp(g_Config.m_ClZzAsyncBotsTripleFlyVelYThreshold, 10, 2000) / 100.0f;
	if(Bot.m_TripleFlyHasLastVelY)
		VerticalSpeedChangedHard = fabsf(CurVelY - Bot.m_TripleFlyLastVelY) > SharpVelYThreshold;
	Bot.m_TripleFlyLastVelY = CurVelY;
	Bot.m_TripleFlyHasLastVelY = true;

	const int HookState = pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_HookState;
	const int HookedPlayer = pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_HookedPlayer;
	const bool HookIsFlying = HookState == HOOK_FLYING;
	const bool HookIsGrabbedPlayer = HookState == HOOK_GRABBED && HookedPlayer >= 0;
	if(HookIsGrabbedPlayer)
		Bot.m_TripleFlyLastHookedClientId = HookedPlayer;
	const bool HookMissing = !HookIsGrabbedPlayer && !HookIsFlying;
	const int NoHookSearchTicks = 5;
	const bool HasStoredTarget = Bot.m_TripleFlyTargetClientId >= 0;
	const bool SearchDueNoHook = !HookMissing || !HasStoredTarget || Bot.m_TripleFlyNextNoHookSearchTick <= 0 || GameTick >= Bot.m_TripleFlyNextNoHookSearchTick;

	int TargetCid = -1;
	vec2 TargetDelta(0.0f, 0.0f);
	bool FoundTarget = false;
	const float ConfigFov = (float)g_Config.m_ClZzAsyncBotsTripleFlyFov;
	const vec2 DownAim(0.0f, 1.0f);
	if(SearchDueNoHook)
	{
		const int PreferredTargetCid = Bot.m_TripleFlyLastHookedClientId;
		if(PreferredTargetCid >= 0 && PreferredTargetCid != BotCid)
		{
			if(VerticalSpeedChangedHard)
			{
				FoundTarget =
					FindSpecificTripleFlyTarget(pGameClient, BotCid, PreferredTargetCid, BotPos, DownAim, 180.0f, true, true, HookLength, TargetDelta) ||
					FindSpecificTripleFlyTarget(pGameClient, BotCid, PreferredTargetCid, BotPos, DownAim, maximum(ConfigFov, 150.0f), true, true, HookLength, TargetDelta) ||
					FindSpecificTripleFlyTarget(pGameClient, BotCid, PreferredTargetCid, BotPos, Aim, ConfigFov, true, true, HookLength, TargetDelta);
			}
			else
			{
				FoundTarget = FindSpecificTripleFlyTarget(pGameClient, BotCid, PreferredTargetCid, BotPos, Aim, ConfigFov, true, true, HookLength, TargetDelta);
			}
			if(FoundTarget)
				TargetCid = PreferredTargetCid;
		}

		if(VerticalSpeedChangedHard)
		{
			FoundTarget = FoundTarget ||
				FindNearestTripleFlyTarget(pGameClient, BotCid, BotPos, DownAim, 180.0f, true, true, HookLength, TargetCid, TargetDelta) ||
				FindNearestTripleFlyTarget(pGameClient, BotCid, BotPos, DownAim, maximum(ConfigFov, 150.0f), true, true, HookLength, TargetCid, TargetDelta) ||
				FindNearestTripleFlyTarget(pGameClient, BotCid, BotPos, Aim, ConfigFov, true, true, HookLength, TargetCid, TargetDelta);
		}
		else
		{
			FoundTarget = FoundTarget ||
				FindNearestTripleFlyTarget(pGameClient, BotCid, BotPos, Aim, ConfigFov, true, true, HookLength, TargetCid, TargetDelta) ||
				FindNearestTripleFlyTarget(pGameClient, BotCid, BotPos, DownAim, maximum(ConfigFov, 150.0f), true, true, HookLength, TargetCid, TargetDelta) ||
				FindNearestTripleFlyTarget(pGameClient, BotCid, BotPos, DownAim, 180.0f, true, true, HookLength, TargetCid, TargetDelta);
		}
		if(HookMissing)
			Bot.m_TripleFlyNextNoHookSearchTick = GameTick + NoHookSearchTicks;
	}
	else
	{
		const int StoredCid = Bot.m_TripleFlyTargetClientId;
		if(StoredCid >= 0 && StoredCid < MAX_CLIENTS && StoredCid != BotCid)
		{
			if(VerticalSpeedChangedHard)
			{
				FoundTarget =
					FindSpecificTripleFlyTarget(pGameClient, BotCid, StoredCid, BotPos, DownAim, 180.0f, true, true, HookLength, TargetDelta) ||
					FindSpecificTripleFlyTarget(pGameClient, BotCid, StoredCid, BotPos, DownAim, maximum(ConfigFov, 150.0f), true, true, HookLength, TargetDelta) ||
					FindSpecificTripleFlyTarget(pGameClient, BotCid, StoredCid, BotPos, Aim, ConfigFov, true, true, HookLength, TargetDelta);
			}
			else
			{
				FoundTarget = FindSpecificTripleFlyTarget(pGameClient, BotCid, StoredCid, BotPos, Aim, ConfigFov, true, true, HookLength, TargetDelta);
			}
			if(FoundTarget)
				TargetCid = StoredCid;
		}
	}

	if(!FoundTarget)
	{
		Bot.m_TripleFlyTargetClientId = -1;
		Bot.m_TripleFlyNextRehookTick = 0;
		Bot.m_TripleFlyPulsePending = false;
		if(HookMissing)
		{
			if(SearchDueNoHook)
				Bot.m_TripleFlyNextNoHookSearchTick = GameTick + NoHookSearchTicks;
			else
				Bot.m_TripleFlyNextNoHookSearchTick = GameTick;
		}
		Input.m_Hook = 0;
		return;
	}

	const int HoldTicks = maximum(1, (int)round_to_int((HookDuration - 0.05f) * (float)TickSpeed));
	if(Bot.m_TripleFlyTargetClientId != TargetCid || Bot.m_TripleFlyNextRehookTick <= 0)
		Bot.m_TripleFlyNextRehookTick = GameTick + HoldTicks;

	Bot.m_TripleFlyTargetClientId = TargetCid;
	Input.m_TargetX = (int)round_to_int(TargetDelta.x);
	Input.m_TargetY = (int)round_to_int(TargetDelta.y);
	if(Input.m_TargetX == 0 && Input.m_TargetY == 0)
		Input.m_TargetY = 1;

	const float TargetDist = length(TargetDelta);
	const float KeepHookDist = (float)std::clamp(g_Config.m_ClZzAsyncBotsTripleFlyKeepHookDist, 1, 200);
	if(TargetDist < KeepHookDist)
	{
		Input.m_Hook = 0;
		Bot.m_TripleFlyPulsePending = true;
		return;
	}
	const bool NeedImmediateRehookPulse = HookMissing && !VerticalSpeedChangedHard;
	if(HookMissing && VerticalSpeedChangedHard)
	{
		Input.m_Hook = 1; // on sharp vertical change, relaunch immediately when hook is missing
		Bot.m_TripleFlyPulsePending = false;
		Bot.m_TripleFlyNextRehookTick = GameTick + HoldTicks;
		return;
	}
	if(NeedImmediateRehookPulse)
	{
		if(!Bot.m_TripleFlyPulsePending)
		{
			Input.m_Hook = 0;
			Bot.m_TripleFlyPulsePending = true;
			return;
		}

		Input.m_Hook = 1;
		Bot.m_TripleFlyPulsePending = false;
		Bot.m_TripleFlyNextRehookTick = GameTick + HoldTicks;
		return;
	}
	Bot.m_TripleFlyPulsePending = false;

	if(GameTick >= Bot.m_TripleFlyNextRehookTick)
	{
		Input.m_Hook = 0;
		Bot.m_TripleFlyPulsePending = true;
		Bot.m_TripleFlyNextRehookTick = GameTick + HoldTicks;
	}
	else
	{
		Input.m_Hook = 1;
	}
}

void CAsyncProxyBotManager::BuildBotAppearance(CGameClient *pGameClient, SBotSession &Bot, int WaveIndex)
{
	char aBase[MAX_NAME_LENGTH];
	str_copy(aBase, g_Config.m_ClZzAsyncBotsBaseName[0] ? g_Config.m_ClZzAsyncBotsBaseName : "ZZBot", sizeof(aBase));
	if(g_Config.m_ClZzAsyncBotsNameMode == 1)
	{
		char aLetters[16];
		BuildPseudoRandomLetters(aLetters, sizeof(aLetters), WaveIndex + 1);
		str_copy(Bot.m_aName, aLetters, sizeof(Bot.m_aName));
	}
	else
	{
		str_format(Bot.m_aName, sizeof(Bot.m_aName), "%s%d", aBase, WaveIndex + 1);
	}

	str_copy(Bot.m_aClan, g_Config.m_ClZzAsyncBotsClan[0] ? g_Config.m_ClZzAsyncBotsClan : g_Config.m_PlayerClan, sizeof(Bot.m_aClan));
	Bot.m_Country = g_Config.m_ClZzAsyncBotsCountry;
	str_copy(Bot.m_aSkin, g_Config.m_ClZzAsyncBotsSkinName[0] ? g_Config.m_ClZzAsyncBotsSkinName : g_Config.m_ClPlayerSkin, sizeof(Bot.m_aSkin));
	Bot.m_UseCustomColor = g_Config.m_ClZzAsyncBotsSkinUseCustomColor;
	Bot.m_ColorBody = g_Config.m_ClZzAsyncBotsSkinColorBody;
	Bot.m_ColorFeet = g_Config.m_ClZzAsyncBotsSkinColorFeet;

	const int IdentityCopyId = g_Config.m_ClZzAsyncBotsIdentityCopyClientId;
	const bool IdentityCopySourceValid = pGameClient && IdentityCopyId >= 0 && IdentityCopyId < MAX_CLIENTS &&
		pGameClient->m_Snap.m_apPlayerInfos[IdentityCopyId] != nullptr;
	if(g_Config.m_ClZzAsyncBotsIdentityCustom == 0 && IdentityCopySourceValid)
	{
		const auto &Src = pGameClient->m_aClients[IdentityCopyId];
		if(g_Config.m_ClZzAsyncBotsIdentityCopyName && Src.m_aName[0] != '\0')
			str_copy(Bot.m_aName, Src.m_aName, sizeof(Bot.m_aName));
		if(g_Config.m_ClZzAsyncBotsIdentityCopyClan && Src.m_aClan[0] != '\0')
			str_copy(Bot.m_aClan, Src.m_aClan, sizeof(Bot.m_aClan));
		if(g_Config.m_ClZzAsyncBotsIdentityCopyCountry)
			Bot.m_Country = Src.m_Country;
		if(g_Config.m_ClZzAsyncBotsIdentityCopySkin && Src.m_aSkinName[0] != '\0')
			str_copy(Bot.m_aSkin, Src.m_aSkinName, sizeof(Bot.m_aSkin));
		if(g_Config.m_ClZzAsyncBotsIdentityCopyColors)
		{
			Bot.m_UseCustomColor = Src.m_UseCustomColor;
			Bot.m_ColorBody = Src.m_ColorBody;
			Bot.m_ColorFeet = Src.m_ColorFeet;
		}
	}

	if(g_Config.m_ClZzAsyncBotsIdentityCustom != 0 && g_Config.m_ClZzAsyncBotsSkinMode == 1 && pGameClient)
	{
		const int CopyId = g_Config.m_ClZzAsyncBotsSkinCopyClientId;
		if(CopyId >= 0 && CopyId < MAX_CLIENTS)
		{
			const auto &Src = pGameClient->m_aClients[CopyId];
			if(Src.m_aSkinName[0] != '\0')
				str_copy(Bot.m_aSkin, Src.m_aSkinName, sizeof(Bot.m_aSkin));
			if(Src.m_aClan[0] != '\0')
				str_copy(Bot.m_aClan, Src.m_aClan, sizeof(Bot.m_aClan));
			Bot.m_Country = Src.m_Country;
			Bot.m_UseCustomColor = Src.m_UseCustomColor;
			Bot.m_ColorBody = Src.m_ColorBody;
			Bot.m_ColorFeet = Src.m_ColorFeet;
		}
	}
}

void CAsyncProxyBotManager::BuildCompanionInput(CGameClient *pGameClient, SBotSession &Bot, CNetObj_PlayerInput &Input)
{
	if(!pGameClient || !Bot.m_AiHasPlannedInput)
	{
		mem_zero(&Input, sizeof(Input));
		Input.m_TargetX = 1;
		Input.m_PlayerFlags = PLAYERFLAG_PLAYING;
		return;
	}
	Input = Bot.m_AiPlannedInput;
	if(Input.m_TargetX == 0 && Input.m_TargetY == 0)
		Input.m_TargetX = 1;
	Input.m_PlayerFlags |= PLAYERFLAG_PLAYING;
	Input.m_PlayerFlags &= ~(PLAYERFLAG_CHATTING | PLAYERFLAG_IN_MENU | PLAYERFLAG_SCOREBOARD | PLAYERFLAG_SPEC_CAM);
}

void CAsyncProxyBotManager::NotifyLocalChatCommand(CGameClient *pGameClient, const char *pLine)
{
	if(!pGameClient || !pLine || pLine[0] == '\0')
		return;
	const char *pCmd = str_skip_whitespaces_const(pLine);
	if(str_startswith_nocase(pCmd, "/come"))
	{
		m_CompanionComeUntilTick = pGameClient->Client()->GameTick(g_Config.m_ClDummy) + SERVER_TICK_SPEED * 10;
		m_NextCompanionPlanAt = 0;
	}
	else if(str_startswith_nocase(pCmd, "/stay"))
	{
		m_CompanionComeUntilTick = 0;
		m_NextCompanionPlanAt = 0;
		m_TowerAnchorLocked = false;
		m_TowerAnchorTick = -1;
	}
}

void CAsyncProxyBotManager::UpdateCompanionPlanner(CGameClient *pGameClient)
{
	if(!pGameClient || !pGameClient->Client())
		return;
	if(!pGameClient->Collision())
		return;

	const int Tick = pGameClient->Client()->GameTick(g_Config.m_ClDummy);
	if(Tick > m_CompanionComeUntilTick && g_Config.m_ClZzAsyncBotsCompanionComeMode == 0)
		m_CompanionComeUntilTick = 0;
	const bool ComeMode = g_Config.m_ClZzAsyncBotsCompanionComeMode != 0 || Tick < m_CompanionComeUntilTick;

	if(!g_Config.m_ClZzAsyncBotsCompanionMode && !g_Config.m_ClZzAsyncBotsTowerEnable)
	{
		m_TowerAnchorLocked = false;
		m_TowerAnchorTick = -1;
		for(SBotSession &Bot : m_vSessions)
		{
			Bot.m_AiHasPlannedInput = false;
			Bot.m_AiRole = 0;
			Bot.m_AiTargetClientId = -1;
		}
		return;
	}

	const int LocalMainCidEarly = pGameClient->m_aLocalIds[0];
	const int LocalDummyCidEarly = pGameClient->m_aLocalIds[1];
	bool UrgentRescue = false;
	if(LocalMainCidEarly >= 0 && LocalMainCidEarly < MAX_CLIENTS && IsFrozenClient(pGameClient, LocalMainCidEarly, Tick))
		UrgentRescue = true;
	if(LocalDummyCidEarly >= 0 && LocalDummyCidEarly < MAX_CLIENTS && IsFrozenClient(pGameClient, LocalDummyCidEarly, Tick))
		UrgentRescue = true;
	if(!UrgentRescue)
	{
		for(const SBotSession &Bot : m_vSessions)
		{
			if(Bot.m_State != BOT_INGAME || Bot.m_KnownClientId < 0 || Bot.m_KnownClientId >= MAX_CLIENTS)
				continue;
			if(IsFrozenClient(pGameClient, Bot.m_KnownClientId, Tick))
			{
				UrgentRescue = true;
				break;
			}
		}
	}

	const int64_t Now = time_get();
	if(!UrgentRescue && m_NextCompanionPlanAt != 0 && Now < m_NextCompanionPlanAt)
		return;
	int PlanMs = std::clamp(g_Config.m_ClZzAsyncBotsCompanionPlanMs, 15, 500);
	if(UrgentRescue)
		PlanMs = minimum(PlanMs, 10);
	else if(ComeMode)
		PlanMs = minimum(PlanMs, 20);
	m_NextCompanionPlanAt = Now + (int64_t)PlanMs * time_freq() / 1000;

	std::array<int, MAX_CLIENTS> UsedClient{};
	std::array<int, MAX_CLIENTS> TargetOccupancy{};
	UsedClient.fill(-1);
	TargetOccupancy.fill(0);

	std::vector<int> vActiveBotIdx;
	vActiveBotIdx.reserve(m_vSessions.size());

	// Resolve visible client ids for bots from the main snapshot.
	for(int i = 0; i < (int)m_vSessions.size(); i++)
	{
		SBotSession &Bot = m_vSessions[i];
		Bot.m_AiHasPlannedInput = false;
		Bot.m_AiRole = 0;
		Bot.m_AiTargetClientId = -1;
		if(Bot.m_State != BOT_INGAME)
			continue;

		if(Bot.m_KnownClientId >= 0 && Bot.m_KnownClientId < MAX_CLIENTS &&
			pGameClient->m_Snap.m_apPlayerInfos[Bot.m_KnownClientId] &&
			str_comp(pGameClient->m_aClients[Bot.m_KnownClientId].m_aName, Bot.m_aName) == 0)
		{
			UsedClient[Bot.m_KnownClientId] = i;
			vActiveBotIdx.push_back(i);
			continue;
		}

		Bot.m_KnownClientId = -1;
		for(int cid = 0; cid < MAX_CLIENTS; cid++)
		{
			if(!pGameClient->m_Snap.m_apPlayerInfos[cid] || UsedClient[cid] != -1)
				continue;
			if(str_comp(pGameClient->m_aClients[cid].m_aName, Bot.m_aName) == 0)
			{
				Bot.m_KnownClientId = cid;
				UsedClient[cid] = i;
				break;
			}
		}
		if(Bot.m_KnownClientId >= 0)
			vActiveBotIdx.push_back(i);
	}

	if(vActiveBotIdx.empty())
		return;

	if(g_Config.m_ClZzAsyncBotsTowerEnable)
	{
		const int LocalMainCid = pGameClient->m_aLocalIds[0];
		if(LocalMainCid >= 0 && LocalMainCid < MAX_CLIENTS && pGameClient->m_Snap.m_aCharacters[LocalMainCid].m_Active)
		{
			m_TowerAnchorPos = vec2((float)pGameClient->m_Snap.m_aCharacters[LocalMainCid].m_Cur.m_X,
				(float)pGameClient->m_Snap.m_aCharacters[LocalMainCid].m_Cur.m_Y);
			m_TowerAnchorLocked = true;
			m_TowerAnchorTick = Tick;
		}
		if(!m_TowerAnchorLocked)
			return;
		const float MainX = m_TowerAnchorPos.x;
		const float MainY = m_TowerAnchorPos.y;
		const float HammerRange = 76.0f;
		const int HammerDelayTicks = 10;

		// 16x16 behavior removed by request: force classic tower mode.
		g_Config.m_ClZzAsyncBotsTowerMode = 0;

		int TargetAboveCid = -1;
		float BestAboveXDist = 1e9f;
		for(int Cid = 0; Cid < MAX_CLIENTS; Cid++)
		{
			if(!pGameClient->m_Snap.m_aCharacters[Cid].m_Active)
				continue;
			const vec2 P((float)pGameClient->m_Snap.m_aCharacters[Cid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[Cid].m_Cur.m_Y);
			if(P.y >= MainY - 1.0f)
				continue;
			const float Xd = fabsf(P.x - MainX);
			if(Xd < BestAboveXDist)
			{
				BestAboveXDist = Xd;
				TargetAboveCid = Cid;
			}
		}

		for(int Bj : vActiveBotIdx)
		{
			SBotSession &Bot = m_vSessions[Bj];
			Bot.m_AiHasPlannedInput = false;
			Bot.m_AiRole = 0;
			Bot.m_AiTargetClientId = -1;
			CNetObj_PlayerInput Out = {};
			Out.m_PlayerFlags = PLAYERFLAG_PLAYING;
			Out.m_TargetX = 1;
			const int BotCid = Bot.m_KnownClientId;
			if(BotCid < 0 || !pGameClient->m_Snap.m_aCharacters[BotCid].m_Active)
			{
				Bot.m_AiPlannedInput = Out;
				Bot.m_AiHasPlannedInput = true;
				continue;
			}
			const vec2 BotPos((float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_Y);
			// BalanceBot-like horizontal alignment to Main X.
			const float XErr = MainX - BotPos.x;
			Out.m_Direction = XErr > 1.0f ? 1 : (XErr < -1.0f ? -1 : 0);
			Out.m_TargetX = (int)std::clamp((int)round_to_int(XErr), -220, 220);
			Out.m_TargetY = -80;
			Out.m_Hook = 0;
			Out.m_Jump = 0;

			// Jump/hammer sequence:
			// If bot is not above Main and cannot hammer target-above directly, prep jump on nearby bots,
			// then fire hammer after 10 ticks.
			const bool BotNotAboveMain = BotPos.y >= MainY - 1.0f;
			bool CanHammerAboveNow = false;
			if(TargetAboveCid >= 0 && pGameClient->m_Snap.m_aCharacters[TargetAboveCid].m_Active)
			{
				const vec2 T((float)pGameClient->m_Snap.m_aCharacters[TargetAboveCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[TargetAboveCid].m_Cur.m_Y);
				CanHammerAboveNow = distance(BotPos, T) <= HammerRange;
				Out.m_TargetX = (int)std::clamp((int)round_to_int(T.x - BotPos.x), -220, 220);
				Out.m_TargetY = (int)std::clamp((int)round_to_int(T.y - BotPos.y), -220, 220);
			}
			if(BotNotAboveMain && TargetAboveCid >= 0 && !CanHammerAboveNow)
			{
				bool AnyInZone = false;
				for(int OtherBj : vActiveBotIdx)
				{
					const int OtherCid = m_vSessions[OtherBj].m_KnownClientId;
					if(OtherCid < 0 || !pGameClient->m_Snap.m_aCharacters[OtherCid].m_Active)
						continue;
					const vec2 OP((float)pGameClient->m_Snap.m_aCharacters[OtherCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[OtherCid].m_Cur.m_Y);
					if(distance(OP, BotPos) <= HammerRange)
						AnyInZone = true;
				}
				if(AnyInZone)
				{
					Out.m_Jump = 1;
					if(Bot.m_TowerHammerPrepTick < 0)
						Bot.m_TowerHammerPrepTick = Tick;
					if(Tick - Bot.m_TowerHammerPrepTick >= HammerDelayTicks)
					{
						Bot.m_AiFireCount = ((Bot.m_AiFireCount + 2) | 1) & INPUT_STATE_MASK;
						Out.m_Fire = Bot.m_AiFireCount;
						Bot.m_TowerHammerPrepTick = -1;
					}
				}
				else
					Bot.m_TowerHammerPrepTick = -1;
			}
			Bot.m_AiPlannedInput = Out;
			Bot.m_AiHasPlannedInput = true;
		}
		return;
	}

	const int LocalMainCid = pGameClient->m_aLocalIds[0];
	const int LocalDummyCid = pGameClient->m_aLocalIds[1];

	const float RescueMaxDist = (float)std::clamp(g_Config.m_ClZzAsyncBotsCompanionRescueTiles, 4, 40) * 32.0f;
	const float ChaseMaxDist = (float)std::clamp(g_Config.m_ClZzAsyncBotsCompanionChaseTiles, 8, 100) * 32.0f;
	const float FollowStartDist = (float)std::clamp(g_Config.m_ClZzAsyncBotsCompanionFollowTiles, 5, 200) * 32.0f;
	const float FollowSpacing = (float)std::clamp(g_Config.m_ClZzAsyncBotsCompanionSpacingTiles, 1, 20) * 32.0f;
	const int AggroPct = std::clamp(g_Config.m_ClZzAsyncBotsCompanionAggro, 0, 100);
	const float AggroMul = 0.5f + (float)AggroPct / 100.0f;
	const bool MainFrozen = LocalMainCid >= 0 && LocalMainCid < MAX_CLIENTS &&
		pGameClient->m_Snap.m_aCharacters[LocalMainCid].m_Active &&
		IsFrozenClient(pGameClient, LocalMainCid, Tick);
	const bool AllowAggro = AggroPct > 0 && !MainFrozen; // block/rescue priority over attack

	auto IsManagedBotClient = [&](int Cid) {
		if(Cid < 0 || Cid >= MAX_CLIENTS)
			return false;
		if(UsedClient[Cid] != -1)
			return true;
		const char *pName = pGameClient->m_aClients[Cid].m_aName;
		const char *pClan = pGameClient->m_aClients[Cid].m_aClan;
		const char *pSkin = pGameClient->m_aClients[Cid].m_aSkinName;
		for(const SBotSession &KnownBot : m_vSessions)
		{
			if(KnownBot.m_State != BOT_INGAME)
				continue;
			if(KnownBot.m_KnownClientId == Cid)
				return true;
			if(KnownBot.m_aName[0] != '\0' && pName[0] != '\0' && str_comp(KnownBot.m_aName, pName) == 0)
				return true;
			if(KnownBot.m_aClan[0] != '\0' && pClan[0] != '\0' && str_comp(KnownBot.m_aClan, pClan) == 0 &&
				KnownBot.m_aSkin[0] != '\0' && pSkin[0] != '\0' && str_comp(KnownBot.m_aSkin, pSkin) == 0)
				return true;
		}
		return false;
	};

	std::vector<SAiTargetInfo> vEnemies;
	vEnemies.reserve(MAX_CLIENTS);
	if(AllowAggro)
	{
		for(int cid = 0; cid < MAX_CLIENTS; cid++)
		{
			if(!pGameClient->m_Snap.m_apPlayerInfos[cid] || !pGameClient->m_Snap.m_aCharacters[cid].m_Active)
				continue;
			if(cid == LocalMainCid || cid == LocalDummyCid)
				continue; // never attack local players
			if(IsManagedBotClient(cid))
				continue; // never attack managed bots
			SAiTargetInfo Info;
			Info.m_ClientId = cid;
			Info.m_Pos = vec2((float)pGameClient->m_Snap.m_aCharacters[cid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[cid].m_Cur.m_Y);
			Info.m_Frozen = IsFrozenClient(pGameClient, cid, Tick);
			vEnemies.push_back(Info);
		}
	}
	std::array<int, MAX_CLIENTS> RescueHelperCount{};
	RescueHelperCount.fill(0);
	std::array<int, MAX_CLIENTS> RescueTakenBy{};
	RescueTakenBy.fill(-1);
	auto TryAssignRescuer = [&](int VictimCid, int PriorityBoost, int MaxHelpers) {
		if(VictimCid < 0 || VictimCid >= MAX_CLIENTS)
			return;
		if(RescueHelperCount[VictimCid] >= maximum(1, MaxHelpers))
			return;
		if(!pGameClient->m_Snap.m_aCharacters[VictimCid].m_Active || !IsFrozenClient(pGameClient, VictimCid, Tick))
			return;

		const vec2 VictimPos((float)pGameClient->m_Snap.m_aCharacters[VictimCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[VictimCid].m_Cur.m_Y);
		float BestScore = 1e9f;
		int BestBotIndex = -1;
		for(int Bj : vActiveBotIdx)
		{
			SBotSession &Helper = m_vSessions[Bj];
			const int HelperCid = Helper.m_KnownClientId;
			if(HelperCid < 0 || !pGameClient->m_Snap.m_aCharacters[HelperCid].m_Active || IsFrozenClient(pGameClient, HelperCid, Tick))
				continue;
			if(Helper.m_AiRole == 1)
				continue;
			const vec2 HelperPos((float)pGameClient->m_Snap.m_aCharacters[HelperCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[HelperCid].m_Cur.m_Y);
			const float Dist = distance(HelperPos, VictimPos);
			if(Dist > RescueMaxDist)
				continue;
			const float BusyPenalty = Helper.m_AiRole == 2 ? 120.0f : 0.0f;
			const float FreezeRiskPenalty = (!ComeMode && IsFreezeLikeSegment(pGameClient, HelperPos, VictimPos)) ? 650.0f : 0.0f;
			const float Score = Dist + BusyPenalty + FreezeRiskPenalty - (float)PriorityBoost;
			if(Score < BestScore)
			{
				BestScore = Score;
				BestBotIndex = Bj;
			}
		}

		if(BestBotIndex >= 0)
		{
			SBotSession &Rescuer = m_vSessions[BestBotIndex];
			RescueTakenBy[VictimCid] = BestBotIndex;
			RescueHelperCount[VictimCid]++;
			Rescuer.m_AiRole = 1;
			Rescuer.m_AiTargetClientId = VictimCid;
			Rescuer.m_AiCommitUntilTick = Tick + 18;
		}
	};

	// Rescue priority: local main player first (up to 5 closest helpers), then others.
	TryAssignRescuer(LocalMainCid, 5000, MainFrozen ? 5 : 1);
	if(MainFrozen)
	{
		TryAssignRescuer(LocalMainCid, 4400, 5);
		TryAssignRescuer(LocalMainCid, 4200, 5);
		TryAssignRescuer(LocalMainCid, 4000, 5);
		TryAssignRescuer(LocalMainCid, 3800, 5);
	}
	TryAssignRescuer(LocalDummyCid, 1200, 1);
	for(int Bi : vActiveBotIdx)
	{
		const int VictimCid = m_vSessions[Bi].m_KnownClientId;
		TryAssignRescuer(VictimCid, MainFrozen ? -200 : 0, 1);
	}
	bool HasRescueAssignments = false;
	for(int Bi : vActiveBotIdx)
	{
		if(m_vSessions[Bi].m_AiRole == 1 && m_vSessions[Bi].m_AiTargetClientId >= 0)
		{
			HasRescueAssignments = true;
			break;
		}
	}

	// Hunter assignment with occupancy penalties (fully disabled at aggression=0).
	if(AllowAggro && !HasRescueAssignments)
	{
		for(int Bi : vActiveBotIdx)
		{
			SBotSession &Bot = m_vSessions[Bi];
			const int BotCid = Bot.m_KnownClientId;
			if(Bot.m_AiRole == 1 && Bot.m_AiTargetClientId >= 0)
				continue;
			if(BotCid < 0 || !pGameClient->m_Snap.m_aCharacters[BotCid].m_Active)
				continue;

			const vec2 BotPos((float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_Y);
			float BestScore = 1e9f;
			int BestTarget = -1;
			for(const SAiTargetInfo &Enemy : vEnemies)
			{
				const vec2 Delta = Enemy.m_Pos - BotPos;
				const float Dist = length(Delta);
				if(!ComeMode && Dist > ChaseMaxDist)
					continue;
				float Score = Dist + TargetOccupancy[Enemy.m_ClientId] * 220.0f;
				const vec2 Vel((float)pGameClient->m_Snap.m_aCharacters[Enemy.m_ClientId].m_Cur.m_VelX / 256.0f * 50.0f,
					(float)pGameClient->m_Snap.m_aCharacters[Enemy.m_ClientId].m_Cur.m_VelY / 256.0f * 50.0f);
				const vec2 Pred = Enemy.m_Pos + Vel * 0.30f;
				if(IsFreezeLikePos(pGameClient, Pred))
					Score -= 220.0f * AggroMul;
				if(IsFreezeLikePos(pGameClient, Enemy.m_Pos))
					Score -= 140.0f * AggroMul;
				if(IsFreezeLikeSegment(pGameClient, Enemy.m_Pos, Pred))
					Score -= 140.0f * AggroMul;
				if(Enemy.m_Frozen)
					Score += 180.0f;
				if(ComeMode)
					Score -= 0.26f * Dist;
				else if(IsFreezeLikeSegment(pGameClient, BotPos, Enemy.m_Pos))
					Score += 720.0f; // much stronger freeze fear outside /come
				if(Score < BestScore)
				{
					BestScore = Score;
					BestTarget = Enemy.m_ClientId;
				}
			}

			Bot.m_AiRole = BestTarget >= 0 ? 2 : Bot.m_AiRole;
			Bot.m_AiTargetClientId = BestTarget >= 0 ? BestTarget : Bot.m_AiTargetClientId;
			if(BestTarget >= 0)
				TargetOccupancy[BestTarget]++;
		}
	}

	// Build planned input for every active bot.
	CGameWorld SimBaseWorld;
	SimBaseWorld.CopyWorld(&pGameClient->m_RegularPredictedWorld);

	std::array<bool, MAX_CLIENTS> FriendlyMask{};
	FriendlyMask.fill(false);
	if(LocalMainCid >= 0 && LocalMainCid < MAX_CLIENTS)
		FriendlyMask[LocalMainCid] = true;
	if(LocalDummyCid >= 0 && LocalDummyCid < MAX_CLIENTS)
		FriendlyMask[LocalDummyCid] = true;
	for(int Bj : vActiveBotIdx)
	{
		const int Cid = m_vSessions[Bj].m_KnownClientId;
		if(Cid >= 0 && Cid < MAX_CLIENTS)
			FriendlyMask[Cid] = true;
	}
	int SimBudgetTotal = std::clamp(8 + (int)vActiveBotIdx.size() * 2, 8, 64);
	int SimBudgetRescue = std::clamp(4 + (int)vActiveBotIdx.size(), 4, 40);
	int SimBudgetFight = std::clamp(3 + (int)vActiveBotIdx.size() / 2, 3, 20);
	int SimBudgetFollow = std::clamp(1 + (int)vActiveBotIdx.size() / 6, 1, 8);

	for(int Bi : vActiveBotIdx)
	{
		SBotSession &Bot = m_vSessions[Bi];
		const int BotCid = Bot.m_KnownClientId;
		if(BotCid < 0 || !pGameClient->m_Snap.m_aCharacters[BotCid].m_Active)
			continue;

		CNetObj_PlayerInput Out;
		mem_zero(&Out, sizeof(Out));
		Out.m_PlayerFlags = PLAYERFLAG_PLAYING;
		Out.m_TargetX = 1;

		vec2 BotPos((float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_Y);
		vec2 TargetPos = BotPos;
		bool HasTarget = false;

		if(Bot.m_AiTargetClientId >= 0 && Bot.m_AiTargetClientId < MAX_CLIENTS &&
			pGameClient->m_Snap.m_aCharacters[Bot.m_AiTargetClientId].m_Active)
		{
			TargetPos = vec2((float)pGameClient->m_Snap.m_aCharacters[Bot.m_AiTargetClientId].m_Cur.m_X,
				(float)pGameClient->m_Snap.m_aCharacters[Bot.m_AiTargetClientId].m_Cur.m_Y);
			HasTarget = true;
		}
		else
		{
			// Companion-style fallback: follow local main player when no rescue/hunt target.
			const int FollowCid = (LocalMainCid >= 0 && LocalMainCid < MAX_CLIENTS && pGameClient->m_Snap.m_aCharacters[LocalMainCid].m_Active) ? LocalMainCid :
				((LocalDummyCid >= 0 && LocalDummyCid < MAX_CLIENTS && pGameClient->m_Snap.m_aCharacters[LocalDummyCid].m_Active) ? LocalDummyCid : -1);
			if(FollowCid >= 0 && FollowCid != BotCid)
			{
				const vec2 OwnerPos((float)pGameClient->m_Snap.m_aCharacters[FollowCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[FollowCid].m_Cur.m_Y);
				const float OwnerDist = distance(BotPos, OwnerPos);
				if(ComeMode || OwnerDist > FollowStartDist)
				{
					TargetPos = OwnerPos;
					HasTarget = true;
				}
			}
		}

		if(HasTarget)
		{
			vec2 NavTarget = TargetPos;
			if(ComeMode)
				NavTarget = ComputeComeWaypoint(pGameClient, BotPos, TargetPos);
			vec2 Delta = NavTarget - BotPos;
			const float Dist = length(Delta);
			const vec2 BotVel((float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_VelX / 256.0f * 50.0f,
				(float)pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_VelY / 256.0f * 50.0f);
			const bool Grounded = pGameClient->Collision()->CheckPoint(BotPos + vec2(0.0f, 18.0f));
			const bool HasAirJump = (pGameClient->m_Snap.m_aCharacters[BotCid].m_Cur.m_Jumped & 2) == 0;
			Out.m_Direction = Delta.x > 10.0f ? 1 : (Delta.x < -10.0f ? -1 : 0);
			Out.m_TargetX = (int)std::clamp((int)Delta.x, -380, 380);
			Out.m_TargetY = (int)std::clamp((int)Delta.y, -380, 380);
			const bool Stuck = fabsf(BotVel.x) < 0.20f && Dist > 96.0f && Out.m_Direction != 0;
			const bool NeedJump = Delta.y < -20.0f || (std::abs(Delta.x) < 42.0f && Delta.y < -6.0f) || Stuck;
			Out.m_Jump = NeedJump && (Grounded || HasAirJump) ? 1 : 0;
			const bool WantHook = Dist > 56.0f && Dist < (ComeMode ? 420.0f : 350.0f);
			Out.m_Hook = WantHook ? 1 : 0;
			if(!ComeMode)
			{
				const vec2 ForwardProbe = BotPos + vec2((float)Out.m_Direction * 28.0f, 0.0f);
				if(IsFreezeLikePos(pGameClient, ForwardProbe) || IsFreezeLikeSegment(pGameClient, BotPos, TargetPos))
				{
					// Companion-safe behavior outside /come.
					Out.m_Hook = 0;
					Out.m_Jump = 0;
					Out.m_Direction = FindSaferLateralDir(pGameClient, BotPos, TargetPos);
				}
			}

			if(Bot.m_AiRole == 2) // hunter
			{
				Out.m_WantedWeapon = 1; // hammer
				const vec2 Vel((float)pGameClient->m_Snap.m_aCharacters[Bot.m_AiTargetClientId].m_Cur.m_VelX / 256.0f * 50.0f,
					(float)pGameClient->m_Snap.m_aCharacters[Bot.m_AiTargetClientId].m_Cur.m_VelY / 256.0f * 50.0f);
				const vec2 Pred = TargetPos + Vel * 0.35f;
				const bool FreezeFinish = IsFreezeLikePos(pGameClient, Pred) || IsFreezeLikeSegment(pGameClient, TargetPos, Pred);
				if(Dist < (ComeMode ? 180.0f : 128.0f) || (FreezeFinish && Dist < 240.0f))
					Out.m_Fire = 1;
				if(FreezeFinish && Dist < (ComeMode ? 340.0f : 260.0f))
					Out.m_Hook = 1;
				if(Dist < 110.0f && (Delta.y < -8.0f || std::abs(Delta.x) < 44.0f) && (Grounded || HasAirJump))
					Out.m_Jump = 1;
			}
			else if(Bot.m_AiRole == 1) // rescuer
			{
				if(!BuildRescueLikeCompanionInput(pGameClient, Tick, BotCid, Bot.m_AiTargetClientId, ComeMode, Out))
				{
					Out.m_WantedWeapon = 1;
					if(Dist < 76.0f)
						Out.m_Fire = 1;
				}
			}
			else
			{
				// Follow mode: keep spacing around owner instead of standing still.
				const int FollowCid = (LocalMainCid >= 0 && LocalMainCid < MAX_CLIENTS && pGameClient->m_Snap.m_aCharacters[LocalMainCid].m_Active) ? LocalMainCid :
					((LocalDummyCid >= 0 && LocalDummyCid < MAX_CLIENTS && pGameClient->m_Snap.m_aCharacters[LocalDummyCid].m_Active) ? LocalDummyCid : -1);
				if(FollowCid >= 0 && FollowCid != BotCid)
				{
					const vec2 OwnerPos((float)pGameClient->m_Snap.m_aCharacters[FollowCid].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[FollowCid].m_Cur.m_Y);
					const vec2 ToOwner = OwnerPos - BotPos;
					const float OwnerDist = length(ToOwner);
					if(!ComeMode && OwnerDist < FollowSpacing)
						Out.m_Direction = 0;
				}
			}
		}

		// Optimized companion-like candidate simulation:
		// - Use simulation budget primarily for rescue/critical combat.
		// - Follow/non-critical uses lightweight heuristic most ticks.
		{
			static int s_SimBudgetCursor = 0;
			const bool RescueRole = Bot.m_AiRole == 1;
			const bool HunterRole = Bot.m_AiRole == 2;
			const bool CriticalRole = RescueRole || HunterRole;
			const int SimTicks = RescueRole ? 14 : (HunterRole ? 10 : 6);

			const int ActiveCount = (int)vActiveBotIdx.size();
			const int BudgetSlot = s_SimBudgetCursor++ % maximum(1, ActiveCount);
			const bool BudgetChosen = ((Bi % maximum(1, ActiveCount)) == BudgetSlot);
			const bool BudgetRoleAllowed = RescueRole ? (SimBudgetRescue > 0) : (HunterRole ? (SimBudgetFight > 0) : (SimBudgetFollow > 0));
			const bool BudgetAllowedNow = SimBudgetTotal > 0 && BudgetRoleAllowed;

			// Skip expensive simulation for non-critical roles unless it's this bot's budget turn.
			if(!CriticalRole && (!BudgetChosen || !BudgetAllowedNow))
			{
				Bot.m_AiPlannedInput = Out;
				Bot.m_AiHasPlannedInput = true;
				continue;
			}

			// Reuse previous decision briefly when context is stable to avoid resim spam.
			const bool StableContext = Bot.m_AiLastSimRole == Bot.m_AiRole &&
				Bot.m_AiLastSimTarget == Bot.m_AiTargetClientId &&
				distance(Bot.m_AiLastGoalPos, HasTarget ? TargetPos : BotPos) < 24.0f &&
				(Tick - Bot.m_AiLastSimTick) <= (CriticalRole ? 2 : 4);
			if(StableContext && (!BudgetChosen || !BudgetAllowedNow))
			{
				Bot.m_AiPlannedInput = Out;
				Bot.m_AiHasPlannedInput = true;
				continue;
			}
			if(!BudgetAllowedNow)
			{
				Bot.m_AiPlannedInput = Out;
				Bot.m_AiHasPlannedInput = true;
				continue;
			}

			std::vector<CNetObj_PlayerInput> vCand;
			vCand.reserve(6);
			vCand.push_back(Out);
			CNetObj_PlayerInput C = Out;
			C.m_Direction = 0;
			C.m_Hook = 0;
			vCand.push_back(C);
			C = Out;
			C.m_Jump = 0;
			vCand.push_back(C);
			if(CriticalRole)
			{
				C = Out;
				C.m_Direction = Out.m_Direction == 0 ? 0 : -Out.m_Direction;
				vCand.push_back(C);
			}
			if(HunterRole)
			{
				C = Out;
				C.m_Hook = 0;
				vCand.push_back(C);
			}
			if(RescueRole && Bot.m_AiTargetClientId >= 0 && Bot.m_AiTargetClientId < MAX_CLIENTS && pGameClient->m_Snap.m_aCharacters[Bot.m_AiTargetClientId].m_Active)
			{
				const vec2 VictimPos((float)pGameClient->m_Snap.m_aCharacters[Bot.m_AiTargetClientId].m_Cur.m_X, (float)pGameClient->m_Snap.m_aCharacters[Bot.m_AiTargetClientId].m_Cur.m_Y);
				const vec2 D = VictimPos - BotPos;
				const int DirTo = D.x > 8.0f ? 1 : (D.x < -8.0f ? -1 : 0);
				auto AddRescue = [&](int Dir, int Jump, int Hook, int Fire, vec2 Aim) {
					CNetObj_PlayerInput R = {};
					R.m_PlayerFlags = PLAYERFLAG_PLAYING;
					R.m_Direction = Dir;
					R.m_Jump = Jump;
					R.m_Hook = Hook;
					R.m_Fire = Fire;
					R.m_WantedWeapon = WEAPON_HAMMER + 1;
					R.m_TargetX = (int)std::clamp((int)round_to_int(Aim.x), -380, 380);
					R.m_TargetY = (int)std::clamp((int)round_to_int(Aim.y), -380, 380);
					if(R.m_TargetX == 0 && R.m_TargetY == 0)
						R.m_TargetY = -1;
					vCand.push_back(R);
				};
				// hammer-first close rescue
				AddRescue(DirTo, 0, 0, 1, D);
				// hook drag rescue
				AddRescue(DirTo, 0, 1, 0, D);
				// jump-hook if victim above
				if(D.y < -6.0f)
					AddRescue(DirTo, 1, 1, 0, D);
				// safe approach without hook
				AddRescue(DirTo, D.y < -10.0f ? 1 : 0, 0, 0, D);
			}

			float BestScore = 1e18f;
			CNetObj_PlayerInput BestIn = Out;
			const vec2 GoalPos = HasTarget ? TargetPos : BotPos;
			int SimsUsed = 0;
			const int RoleBudget = RescueRole ? SimBudgetRescue : (HunterRole ? SimBudgetFight : SimBudgetFollow);
			const int RoleCap = std::clamp(RoleBudget, 1, RescueRole ? 8 : (HunterRole ? 6 : 2));
			for(const auto &Cand : vCand)
			{
				if(SimBudgetTotal <= 0 || (RescueRole && SimBudgetRescue <= 0) || (HunterRole && SimBudgetFight <= 0) || (!CriticalRole && SimBudgetFollow <= 0))
					break;
				if(SimsUsed >= RoleCap)
					break;
				SAsyncCompanionSimEval Eval;
				if(!SimulateAsyncCompanionCandidate(pGameClient, &SimBaseWorld, BotCid, Bot.m_AiTargetClientId, FriendlyMask, Cand, GoalPos, SimTicks, Eval) || !Eval.m_Ok)
					continue;
				++SimsUsed;
				--SimBudgetTotal;
				if(RescueRole)
					--SimBudgetRescue;
				else if(HunterRole)
					--SimBudgetFight;
				else
					--SimBudgetFollow;
				if(Eval.m_HookedNonFrozenFriendly)
					continue; // reject candidate hooking main/bot that is not frozen
				float Score = distance(Eval.m_EndPos, GoalPos) * 1.8f + fabsf(Eval.m_EndVel.x) * 90.0f + fabsf(Eval.m_EndVel.y) * 75.0f;
				const float Progress = distance(BotPos, GoalPos) - distance(Eval.m_EndPos, GoalPos);
				Score -= Progress * 4.2f;
				if(Eval.m_HookedTarget)
					Score -= 120.0f;
				if(Cand.m_Hook != 0 && !Eval.m_HookedTarget)
					Score += 260.0f;
				if(Cand.m_Jump != 0 && fabsf(Eval.m_EndVel.y) > 5.0f)
					Score += 180.0f;
				if(distance(Eval.m_EndPos, GoalPos) < 42.0f)
					Score -= 140.0f;
				// Strong freeze fear for rescue/hunt.
				if(CriticalRole)
				{
					// Hard reject dangerous candidates in rescue/block.
					if(Eval.m_EverFreezeLike || Eval.m_EndFreezeLike)
						continue;
					if(RescueRole)
					{
						Score -= Eval.m_ProgressToTarget * 8.0f;
						Score += Eval.m_EndDistToTarget * 1.4f;
						if(Cand.m_Fire != 0 && Eval.m_EndDistToTarget < 84.0f)
							Score -= 220.0f;
						if(Cand.m_Hook != 0 && !Eval.m_HookedTarget)
							Score += 420.0f; // avoid empty hook spam in rescue
					}
					else
					{
						// Block/fight: favor controlled close pressure.
						Score += Eval.m_EndDistToTarget * 0.9f;
						if(Cand.m_Hook != 0 && Eval.m_HookedTarget)
							Score -= 120.0f;
					}
				}
				else
				{
					// Follow mode: allow temporary freeze-like if recovered within 50 ticks and ended safe.
					if(Eval.m_EverFreezeLike)
					{
						const bool RecoveredBy50 = Eval.m_LastFreezeTick >= 0 && Eval.m_LastFreezeTick < 50;
						if(!RecoveredBy50 || Eval.m_EndFreezeLike)
							continue;
						Score += 900.0f;
					}
				}
				if(Score < BestScore)
				{
					BestScore = Score;
					BestIn = Cand;
				}
			}
			Out = BestIn;
			Bot.m_AiLastSimTick = Tick;
			Bot.m_AiLastSimRole = Bot.m_AiRole;
			Bot.m_AiLastSimTarget = Bot.m_AiTargetClientId;
			Bot.m_AiLastGoalPos = GoalPos;
		}

		// INPUT fire is a counter-like state; pulse it so hammer attacks keep triggering.
		if(Out.m_Fire != 0)
		{
			Bot.m_AiFireCount = ((Bot.m_AiFireCount + 2) | 1) & INPUT_STATE_MASK;
			Out.m_Fire = Bot.m_AiFireCount;
		}
		else
		{
			Out.m_Fire = 0;
		}

		Bot.m_AiPlannedInput = Out;
		Bot.m_AiHasPlannedInput = true;
	}

	if(g_Config.m_ClZzAsyncBotsCompanionDebug && g_Config.m_ClZzAsyncBotsVerbose)
	{
		char aMsg[256];
		str_format(aMsg, sizeof(aMsg), "companion plan bots=%d enemies=%d come=%d", (int)vActiveBotIdx.size(), (int)vEnemies.size(), ComeMode ? 1 : 0);
		pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz_async_bots", aMsg);
	}
}

void CAsyncProxyBotManager::SendInput(CGameClient *pGameClient, SBotSession &Bot)
{
	CNetObj_PlayerInput Input;
	BuildInputTemplate(pGameClient, Bot, Input);

	if(Bot.m_InputTick <= 0)
		Bot.m_InputTick = maximum(1, pGameClient ? pGameClient->Client()->GameTick(g_Config.m_ClDummy) : 1);
	else
		Bot.m_InputTick++;

	const int Size = sizeof(CNetObj_PlayerInput);
	CMsgPacker Msg(NETMSG_INPUT, true);
	Msg.AddInt(Bot.m_LastAckedSnapshot);
	Msg.AddInt(Bot.m_InputTick);
	Msg.AddInt(Size);
	const int *pData = reinterpret_cast<const int *>(&Input);
	for(int k = 0; k < Size / 4; k++)
		Msg.AddInt(pData[k]);
	SendPacket(Bot, Msg, MSGFLAG_FLUSH);
}

void CAsyncProxyBotManager::DisconnectSession(SBotSession &Bot, const char *pReason, bool ErrorState)
{
	Bot.m_NetClient.Disconnect(pReason);
	Bot.m_NetClient.Close();
	Bot.m_TransportOpened = false;
	Bot.m_State = ErrorState ? BOT_ERROR : BOT_OFFLINE;
	if(pReason)
		str_copy(Bot.m_aError, pReason, sizeof(Bot.m_aError));
}

void CAsyncProxyBotManager::ReconnectSession(CGameClient *pGameClient, SBotSession &Bot)
{
	Bot.m_Proxy.m_Addr.type &= (NETTYPE_IPV4 | NETTYPE_IPV6);
	Bot.m_ServerAddr.type &= (NETTYPE_IPV4 | NETTYPE_IPV6);
	// SOCKS5 expects a single concrete target family (IPv4 or IPv6), not a combined mask.
	const int ProxyType = PickSingleIpType(Bot.m_Proxy.m_Addr.type, Bot.m_Proxy.m_Addr.type);
	if(ProxyType == NETTYPE_IPV4 || ProxyType == NETTYPE_IPV6)
		Bot.m_Proxy.m_Addr.type = ProxyType;
	else
		Bot.m_Proxy.m_Addr.type = NETTYPE_IPV4;
	const int ServerType = PickSingleIpType(Bot.m_ServerAddr.type, Bot.m_Proxy.m_Addr.type);
	if(ServerType == NETTYPE_IPV4 || ServerType == NETTYPE_IPV6)
		Bot.m_ServerAddr.type = ServerType;
	else
		Bot.m_ServerAddr.type = Bot.m_Proxy.m_Addr.type;
	if((Bot.m_ServerAddr.type & (NETTYPE_IPV4 | NETTYPE_IPV6)) == 0 || Bot.m_ServerAddr.port == 0)
	{
		DisconnectSession(Bot, "invalid reconnect target", true);
		return;
	}

	Bot.m_NetClient.Disconnect("reconnect");
	Bot.m_NetClient.ResetErrorString();
	if(!Bot.m_TransportOpened)
	{
		NETADDR BindAddr;
		mem_zero(&BindAddr, sizeof(BindAddr));
		BindAddr.type = (Bot.m_Proxy.m_Addr.type & NETTYPE_IPV6) ? NETTYPE_IPV6 : NETTYPE_IPV4;
		if(!Bot.m_NetClient.Open(BindAddr))
		{
			DisconnectSession(Bot, "open socket failed", true);
			return;
		}
		Bot.m_TransportOpened = true;
	}

	Bot.m_NetClient.SetProxyOverride(true, &Bot.m_Proxy.m_Addr, Bot.m_Proxy.m_aUsername, Bot.m_Proxy.m_aPassword);
	const bool Sixup = pGameClient && pGameClient->Client() && pGameClient->Client()->IsSixup();
	if(Sixup)
		Bot.m_NetClient.Connect7(&Bot.m_ServerAddr, 1);
	else
		Bot.m_NetClient.Connect(&Bot.m_ServerAddr, 1);
	Bot.m_State = BOT_CONNECTING;
	Bot.m_InfoSent = false;
	Bot.m_StartInfoSent = false;
	Bot.m_ReadySent = false;
	Bot.m_EnterGameSent = false;
	Bot.m_ConReadySeen = false;
	Bot.m_FastReadySent = false;
	Bot.m_ReadySentAt = 0;
	Bot.m_Sixup = Sixup;
	Bot.m_NextStartInfoRetryAt = 0;
	Bot.m_NextEnterRetryAt = 0;
	Bot.m_MapChunk = 0;
	Bot.m_MapCrc = 0;
	Bot.m_MapBytes = 0;
	Bot.m_MapSize = 0;
	Bot.m_MapDownloadChunkSize = 900;
	Bot.m_MapDownloadChunksPerRequest = 16;
	Bot.m_HasMapDetails = false;
	Bot.m_aMapDetailsName[0] = '\0';
	Bot.m_MapDetailsCrc = 0;
	Bot.m_MapDetailsSize = 0;
	mem_zero(&Bot.m_MapDetailsSha256, sizeof(Bot.m_MapDetailsSha256));
	Bot.m_LastAckedSnapshot = 0;
	Bot.m_InputTick = 0;
	Bot.m_NextInputAt = 0;
	Bot.m_AiFireCount = 0;
	Bot.m_TripleFlyTargetClientId = -1;
	Bot.m_TripleFlyLastHookedClientId = -1;
	Bot.m_TripleFlyNextRehookTick = 0;
	Bot.m_TripleFlyNextNoHookSearchTick = 0;
	Bot.m_TripleFlyPulsePending = false;
	Bot.m_TripleFlyHasLastVelY = false;
	Bot.m_TripleFlyLastVelY = 0.0f;
	Bot.m_NextNetPollAt = 0;
	Bot.m_MirrorLastSourceAttackTick = -1;
	Bot.m_MirrorLastSourceWeapon = -1;
	Bot.m_ConnectStartedAt = time_get();
	Bot.m_NextDiagAt = Bot.m_ConnectStartedAt + time_freq();
	Bot.m_NextReconnectAt = 0;
	Bot.m_aError[0] = '\0';

	if(pGameClient && (g_Config.m_Debug || g_Config.m_ClZzAsyncBotsVerbose))
	{
		char aMsg[320];
		str_format(aMsg, sizeof(aMsg), "connect via %s", Bot.m_Proxy.m_aRaw);
		TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aMsg);
	}
}

void CAsyncProxyBotManager::HandleChunk(CGameClient *pGameClient, SBotSession &Bot, const CNetChunk &Chunk)
{
	CUnpacker Unpacker;
	Unpacker.Reset((unsigned char *)Chunk.m_pData, Chunk.m_DataSize);

	bool System = false;
	int MsgId = 0;
	CUuid Uuid;
	CMsgPacker Packer(NETMSG_EX, true);
	const int UnpackResult = UnpackMessageId(&MsgId, &System, &Uuid, &Unpacker, &Packer);
	(void)Uuid;
	if(UnpackResult == UNPACKMESSAGE_ERROR)
		return;
	if(UnpackResult == UNPACKMESSAGE_ANSWER)
		SendPacket(Bot, Packer, MSGFLAG_VITAL);

	if(Bot.m_Sixup)
	{
		if(System)
		{
			switch(MsgId)
			{
			case protocol7::NETMSG_INFO: MsgId = NETMSG_INFO; break;
			case protocol7::NETMSG_MAP_CHANGE: MsgId = NETMSG_MAP_CHANGE; break;
			case protocol7::NETMSG_MAP_DATA: MsgId = NETMSG_MAP_DATA; break;
			case protocol7::NETMSG_CON_READY: MsgId = NETMSG_CON_READY; break;
			case protocol7::NETMSG_SNAP: MsgId = NETMSG_SNAP; break;
			case protocol7::NETMSG_SNAPEMPTY: MsgId = NETMSG_SNAPEMPTY; break;
			case protocol7::NETMSG_SNAPSINGLE: MsgId = NETMSG_SNAPSINGLE; break;
			case protocol7::NETMSG_SNAPSMALL: MsgId = NETMSG_SNAPSMALL; break;
			case protocol7::NETMSG_PING: MsgId = NETMSG_PING; break;
			case protocol7::NETMSG_PING_REPLY: MsgId = NETMSG_PING_REPLY; break;
			default: break;
			}
		}
		else if(MsgId >= 0 && MsgId < OFFSET_UUID)
		{
			MsgId = Msg_SevenToSix(MsgId);
			if(MsgId < 0)
				return;
		}
	}

	if(g_Config.m_ClZzAsyncBotsVerbose && Bot.m_State != BOT_INGAME)
	{
		const bool QuietMapDataInWait = System && MsgId == NETMSG_MAP_DATA && Bot.m_State == BOT_WAIT_READYTOENTER;
		if(!QuietMapDataInWait)
		{
			char aMsg[128];
			str_format(aMsg, sizeof(aMsg), "recv %s msg=%d size=%d", System ? "sys" : "game", MsgId, Chunk.m_DataSize);
			TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aMsg);
		}
	}

	if(System)
	{
		if(MsgId == NETMSG_CHECKSUM_REQUEST)
		{
			CUuid *pUuid = (CUuid *)Unpacker.GetRaw(sizeof(*pUuid));
			if(Unpacker.Error() || !pUuid)
			{
				CMsgPacker MsgErr(NETMSG_CHECKSUM_ERROR, true);
				CUuid Zero = UUID_ZEROED;
				MsgErr.AddRaw(&Zero, sizeof(Zero));
				MsgErr.AddInt(1);
				SendPacket(Bot, MsgErr, MSGFLAG_VITAL);
				return;
			}
			const int Start = Unpacker.GetInt();
			const int Length = Unpacker.GetInt();
			if(Unpacker.Error())
			{
				CMsgPacker MsgErr(NETMSG_CHECKSUM_ERROR, true);
				MsgErr.AddRaw(pUuid, sizeof(*pUuid));
				MsgErr.AddInt(1);
				SendPacket(Bot, MsgErr, MSGFLAG_VITAL);
				return;
			}
			if(Start < 0 || Length < 0 || Start > std::numeric_limits<int>::max() - Length)
			{
				CMsgPacker MsgErr(NETMSG_CHECKSUM_ERROR, true);
				MsgErr.AddRaw(pUuid, sizeof(*pUuid));
				MsgErr.AddInt(2);
				SendPacket(Bot, MsgErr, MSGFLAG_VITAL);
				return;
			}
			const int End = Start + Length;

#ifndef DDNET_CHECKSUM_SALT
// salt@ddnet.tw: db877f2b-2ddb-3ba6-9f67-a6d169ec671d
#define DDNET_CHECKSUM_SALT \
	{ \
		{ \
			0xdb, 0x87, 0x7f, 0x2b, 0x2d, 0xdb, 0x3b, 0xa6, \
				0x9f, 0x67, 0xa6, 0xd1, 0x69, 0xec, 0x67, 0x1d, \
		} \
	}
#endif

			CChecksumData Data = {};
			CChecksumData *pSource = pGameClient->Client()->ChecksumData();
			if(!pSource)
			{
				CMsgPacker MsgErr(NETMSG_CHECKSUM_ERROR, true);
				MsgErr.AddRaw(pUuid, sizeof(*pUuid));
				MsgErr.AddInt(3);
				SendPacket(Bot, MsgErr, MSGFLAG_VITAL);
				return;
			}
			Data = *pSource;
			if(Start <= (int)sizeof(CChecksumData))
			{
				mem_zero(&Data.m_Config, sizeof(Data.m_Config));
#define CHECKSUM_RECORD(Flags) (((Flags) & CFGFLAG_CLIENT) == 0 || ((Flags) & CFGFLAG_INSENSITIVE) != 0)
#define MACRO_CONFIG_INT(Name, ScriptName, Def, Min, Max, Flags, Desc) \
	if(CHECKSUM_RECORD(Flags)) \
	{ \
		Data.m_Config.m_##Name = g_Config.m_##Name; \
	}
#define MACRO_CONFIG_COL(Name, ScriptName, Def, Flags, Desc) \
	if(CHECKSUM_RECORD(Flags)) \
	{ \
		Data.m_Config.m_##Name = g_Config.m_##Name; \
	}
#define MACRO_CONFIG_STR(Name, ScriptName, Len, Def, Flags, Desc) \
	if(CHECKSUM_RECORD(Flags)) \
	{ \
		str_copy(Data.m_Config.m_##Name, g_Config.m_##Name, sizeof(Data.m_Config.m_##Name)); \
	}
#include <engine/shared/config_variables.h>
#undef CHECKSUM_RECORD
#undef MACRO_CONFIG_INT
#undef MACRO_CONFIG_COL
#undef MACRO_CONFIG_STR
			}

			const int ChecksumBytesEnd = minimum(End, (int)sizeof(CChecksumData));
			const int FileStart = maximum(Start, (int)sizeof(CChecksumData));
			unsigned char aStartBytes[sizeof(int32_t)];
			unsigned char aEndBytes[sizeof(int32_t)];
			uint_to_bytes_be(aStartBytes, Start);
			uint_to_bytes_be(aEndBytes, End);

			static IOHANDLE s_OwnExecutable = nullptr;
			static int64_t s_OwnExecutableSize = 0;
			if(End > (int)sizeof(CChecksumData))
			{
				if(s_OwnExecutableSize == 0)
				{
					s_OwnExecutable = io_current_exe();
					s_OwnExecutableSize = s_OwnExecutable ? io_length(s_OwnExecutable) : -1;
				}
				if(s_OwnExecutableSize < 0)
				{
					CMsgPacker MsgErr(NETMSG_CHECKSUM_ERROR, true);
					MsgErr.AddRaw(pUuid, sizeof(*pUuid));
					MsgErr.AddInt(3);
					SendPacket(Bot, MsgErr, MSGFLAG_VITAL);
					return;
				}
				if(End - (int)sizeof(CChecksumData) > s_OwnExecutableSize)
				{
					CMsgPacker MsgErr(NETMSG_CHECKSUM_ERROR, true);
					MsgErr.AddRaw(pUuid, sizeof(*pUuid));
					MsgErr.AddInt(4);
					SendPacket(Bot, MsgErr, MSGFLAG_VITAL);
					return;
				}
			}

			SHA256_CTX Sha256Ctxt;
			sha256_init(&Sha256Ctxt);
			CUuid Salt = DDNET_CHECKSUM_SALT;
			sha256_update(&Sha256Ctxt, &Salt, sizeof(Salt));
			sha256_update(&Sha256Ctxt, pUuid, sizeof(*pUuid));
			sha256_update(&Sha256Ctxt, aStartBytes, sizeof(aStartBytes));
			sha256_update(&Sha256Ctxt, aEndBytes, sizeof(aEndBytes));
			if(Start < (int)sizeof(CChecksumData))
				sha256_update(&Sha256Ctxt, ((const unsigned char *)&Data) + Start, ChecksumBytesEnd - Start);
			if(End > (int)sizeof(CChecksumData))
			{
				unsigned char aBuf[2048];
				if(io_seek(s_OwnExecutable, FileStart - (int)sizeof(CChecksumData), IOSEEK_START))
				{
					CMsgPacker MsgErr(NETMSG_CHECKSUM_ERROR, true);
					MsgErr.AddRaw(pUuid, sizeof(*pUuid));
					MsgErr.AddInt(5);
					SendPacket(Bot, MsgErr, MSGFLAG_VITAL);
					return;
				}
				for(int i = FileStart; i < End; i += (int)sizeof(aBuf))
				{
					const int Read = io_read(s_OwnExecutable, aBuf, minimum((int)sizeof(aBuf), End - i));
					sha256_update(&Sha256Ctxt, aBuf, Read);
				}
			}
			SHA256_DIGEST Sha256 = sha256_finish(&Sha256Ctxt);
			CMsgPacker MsgResp(NETMSG_CHECKSUM_RESPONSE, true);
			MsgResp.AddRaw(pUuid, sizeof(*pUuid));
			MsgResp.AddRaw(&Sha256, sizeof(Sha256));
			SendPacket(Bot, MsgResp, MSGFLAG_VITAL);
			return;
		}
		else if(MsgId == NETMSG_TATER_CHECKSUM_REQUEST)
		{
#ifndef TCLIENT_CHECKSUM_SALT
// salt@sjrc6.github.io: 26e65800-d8d9-3e8f-8d53-acdd1461f0a9
#define TCLIENT_CHECKSUM_SALT \
	{ \
		{ \
			0x26, 0xe6, 0x58, 0x00, 0xd8, 0xd9, 0x3e, 0x8f, \
				0x8d, 0x53, 0xac, 0xdd, 0x14, 0x61, 0xf0, 0xa9, \
		} \
	}
#endif
			CUuid *pUuid = (CUuid *)Unpacker.GetRaw(sizeof(*pUuid));
			if(Unpacker.Error() || !pUuid)
				return;
			SHA256_CTX Sha256Ctxt;
			sha256_init(&Sha256Ctxt);
			CUuid Salt = TCLIENT_CHECKSUM_SALT;
			sha256_update(&Sha256Ctxt, &Salt, sizeof(Salt));
			sha256_update(&Sha256Ctxt, pUuid, sizeof(*pUuid));
			SHA256_DIGEST Sha256 = sha256_finish(&Sha256Ctxt);
			CMsgPacker CSMsg(NETMSG_TATER_CHECKSUM_RESPONSE, true);
			CSMsg.AddRaw(pUuid, sizeof(*pUuid));
			CSMsg.AddRaw(&Sha256, sizeof(Sha256));
			SendPacket(Bot, CSMsg, MSGFLAG_VITAL);
			return;
		}
		else if(MsgId == NETMSG_RECONNECT)
		{
			TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "server requested RECONNECT");
			ReconnectSession(pGameClient, Bot);
			return;
		}
		else if(MsgId == NETMSG_REDIRECT)
		{
			const int RedirectPort = Unpacker.GetInt();
			if(Unpacker.Error() || RedirectPort <= 0 || RedirectPort > 65535)
			{
				DisconnectSession(Bot, "invalid redirect", true);
				return;
			}
			Bot.m_ServerAddr.port = RedirectPort;
			if(g_Config.m_ClZzAsyncBotsVerbose)
			{
				char aMsg[128];
				str_format(aMsg, sizeof(aMsg), "server requested REDIRECT port=%d", RedirectPort);
				TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aMsg);
			}
			ReconnectSession(pGameClient, Bot);
			return;
		}
		else if(MsgId == NETMSG_MAP_DETAILS)
		{
			const char *pMapName = Unpacker.GetString(CUnpacker::SANITIZE_CC | CUnpacker::SKIP_START_WHITESPACES);
			SHA256_DIGEST *pMapSha256 = (SHA256_DIGEST *)Unpacker.GetRaw(sizeof(*pMapSha256));
			const int MapCrc = Unpacker.GetInt();
			const int MapSize = Unpacker.GetInt();
			if(Unpacker.Error() || !pMapSha256 || !pMapName || pMapName[0] == '\0')
			{
				Bot.m_HasMapDetails = false;
				return;
			}
			Bot.m_HasMapDetails = true;
			str_copy(Bot.m_aMapDetailsName, pMapName, sizeof(Bot.m_aMapDetailsName));
			Bot.m_MapDetailsCrc = MapCrc;
			Bot.m_MapDetailsSize = MapSize;
			Bot.m_MapDetailsSha256 = *pMapSha256;
			if(g_Config.m_Debug)
			{
				char aSha[SHA256_MAXSTRSIZE];
				char aMsg[320];
				sha256_str(Bot.m_MapDetailsSha256, aSha, sizeof(aSha));
				str_format(aMsg, sizeof(aMsg), "MAP_DETAILS map='%s' crc=%d size=%d sha=%s", Bot.m_aMapDetailsName, Bot.m_MapDetailsCrc, Bot.m_MapDetailsSize, aSha);
				TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aMsg);
			}
			return;
		}
		else if(MsgId == NETMSG_MAP_CHANGE)
		{
			const char *pMapName = Unpacker.GetString(CUnpacker::SANITIZE_CC | CUnpacker::SKIP_START_WHITESPACES);
			const int NewMapCrc = Unpacker.GetInt();
			const int NewMapSize = Unpacker.GetInt();
			int NewChunksPerRequest = Bot.m_MapDownloadChunksPerRequest;
			int NewChunkSize = Bot.m_MapDownloadChunkSize;
			if(Bot.m_Sixup)
			{
				NewChunksPerRequest = Unpacker.GetInt();
				NewChunkSize = Unpacker.GetInt();
				if(NewChunksPerRequest <= 0)
					NewChunksPerRequest = 16;
				if(NewChunkSize <= 0)
					NewChunkSize = 900;
			}
			if(Unpacker.Error() || NewMapSize < 0 || NewMapSize > 1024 * 1024 * 1024 || !str_valid_filename(pMapName))
			{
				DisconnectSession(Bot, "invalid map-change", true);
				return;
			}
			if(Bot.m_State == BOT_DOWNLOADING_MAP && Bot.m_MapCrc == NewMapCrc && Bot.m_MapSize == NewMapSize)
			{
				if(g_Config.m_ClZzAsyncBotsVerbose)
				{
					char aDup[192];
					str_format(aDup, sizeof(aDup), "duplicate MAP_CHANGE ignored (crc=%d size=%d bytes=%d chunk=%d)", NewMapCrc, NewMapSize, Bot.m_MapBytes, Bot.m_MapChunk);
					TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aDup);
				}
				return;
			}
			Bot.m_MapCrc = NewMapCrc;
			Bot.m_MapSize = NewMapSize;
			Bot.m_MapDownloadChunksPerRequest = NewChunksPerRequest;
			Bot.m_MapDownloadChunkSize = NewChunkSize;
			Bot.m_MapChunk = 0;
			Bot.m_MapBytes = 0;
			Bot.m_LastAckedSnapshot = 0;
			Bot.m_InputTick = 0;
			Bot.m_NextInputAt = 0;
			Bot.m_ReadySent = false;
			Bot.m_EnterGameSent = false;
			Bot.m_ConReadySeen = false;
			Bot.m_NextEnterRetryAt = 0;

			const bool SkipMapDownload = g_Config.m_ClZzAsyncBotsSkipMapDownload != 0;
			const bool UseSavedMaps = g_Config.m_ClZzAsyncBotsUseSavedMaps != 0;
			std::optional<SHA256_DIGEST> MapSha256 = std::nullopt;
			if(Bot.m_HasMapDetails &&
				str_comp(Bot.m_aMapDetailsName, pMapName) == 0 &&
				Bot.m_MapDetailsCrc == Bot.m_MapCrc &&
				Bot.m_MapDetailsSize == Bot.m_MapSize)
			{
				MapSha256 = Bot.m_MapDetailsSha256;
			}
			bool HasCachedMap = false;
			if(!SkipMapDownload && UseSavedMaps && pMapName && pMapName[0] != '\0')
			{
				char aCacheKey[512];
				if(MapSha256.has_value())
				{
					char aSha256[SHA256_MAXSTRSIZE];
					sha256_str(MapSha256.value(), aSha256, sizeof(aSha256));
					str_format(aCacheKey, sizeof(aCacheKey), "%s|%08x|%s", pMapName, Bot.m_MapCrc, aSha256);
				}
				else
				{
					str_format(aCacheKey, sizeof(aCacheKey), "%s|%08x", pMapName, Bot.m_MapCrc);
				}
				const auto It = m_SavedMapCache.find(aCacheKey);
				if(It != m_SavedMapCache.end())
					HasCachedMap = It->second;
				else
				{
					HasCachedMap = HasSavedMap(pGameClient, pMapName, Bot.m_MapCrc, MapSha256);
					m_SavedMapCache.emplace(aCacheKey, HasCachedMap);
				}
			}
			if(pGameClient && g_Config.m_Debug)
			{
				char aDbg[320];
				str_format(aDbg, sizeof(aDbg), "bot #%d map '%s' size=%d crc=%d chunk_size=%d chunks_req=%d skip=%d cached=%d", Bot.m_Id, pMapName ? pMapName : "", Bot.m_MapSize, Bot.m_MapCrc, Bot.m_MapDownloadChunkSize, Bot.m_MapDownloadChunksPerRequest, SkipMapDownload ? 1 : 0, HasCachedMap ? 1 : 0);
				pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "zz_async_bots", aDbg);
			}
			if(SkipMapDownload || HasCachedMap)
			{
				CMsgPacker MsgReady(NETMSG_READY, true);
				SendPacket(Bot, MsgReady, MSGFLAG_VITAL | MSGFLAG_FLUSH);
				Bot.m_ReadySent = true;
				Bot.m_ReadySentAt = time_get();
				Bot.m_State = BOT_WAIT_READYTOENTER;
				TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "MAP_CHANGE -> sent READY");
			}
			else
			{
				Bot.m_State = BOT_DOWNLOADING_MAP;
				SendMapRequest(Bot);
				if(g_Config.m_ClZzAsyncBotsVerbose)
				{
					char aReq[192];
					str_format(aReq, sizeof(aReq), "MAP_CHANGE -> request MAP_DATA want_chunk=%d bytes=%d/%d", Bot.m_MapChunk, Bot.m_MapBytes, Bot.m_MapSize);
					TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aReq);
				}
			}
		}
		else if(MsgId == NETMSG_MAP_DATA)
		{
			if(Bot.m_Sixup)
			{
				// Match main client behavior for 0.7 map download flow:
				// no Last/CRC/Chunk fields in translated NETMSG_MAP_DATA, count chunk bytes locally.
				const int Remaining = maximum(0, Bot.m_MapSize - Bot.m_MapBytes);
				const int Size = minimum(Bot.m_MapDownloadChunkSize, Remaining);
				if(Size <= 0)
				{
					CMsgPacker MsgReady(NETMSG_READY, true);
					SendPacket(Bot, MsgReady, MSGFLAG_VITAL | MSGFLAG_FLUSH);
					Bot.m_ReadySent = true;
					Bot.m_ReadySentAt = time_get();
					Bot.m_State = BOT_WAIT_READYTOENTER;
					TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "MAP_DATA(7) done -> sent READY");
					return;
				}
				(void)Unpacker.GetRaw(Size);
				if(Unpacker.Error())
				{
					DisconnectSession(Bot, "invalid map-data(7)", true);
					return;
				}

				Bot.m_MapBytes += Size;
				Bot.m_MapChunk++;
				const bool Last = Bot.m_MapBytes >= Bot.m_MapSize;
				if(Last)
				{
					CMsgPacker MsgReady(NETMSG_READY, true);
					SendPacket(Bot, MsgReady, MSGFLAG_VITAL | MSGFLAG_FLUSH);
					Bot.m_ReadySent = true;
					Bot.m_ReadySentAt = time_get();
					Bot.m_State = BOT_WAIT_READYTOENTER;
					TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "MAP_DATA(7) done -> sent READY");
				}
				else if(Bot.m_MapChunk % Bot.m_MapDownloadChunksPerRequest == 0)
				{
					// In sixup, request the next batch, not every single chunk.
					SendMapRequest(Bot);
					if(g_Config.m_ClZzAsyncBotsVerbose)
					{
						char aReq[192];
						str_format(aReq, sizeof(aReq), "request MAP_DATA(7) want_chunk=%d bytes=%d/%d", Bot.m_MapChunk, Bot.m_MapBytes, Bot.m_MapSize);
						TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aReq);
					}
				}
				return;
			}

			const int Last = Unpacker.GetInt();
			const int MapCrc = Unpacker.GetInt();
			const int ChunkIndex = Unpacker.GetInt();
			const int Size = Unpacker.GetInt();
			// Hard bounds first to avoid unsafe raw reads on malformed packets.
			if(Unpacker.Error() || Size <= 0 || Size > Chunk.m_DataSize)
			{
				DisconnectSession(Bot, "invalid map-data", true);
				return;
			}
			(void)Unpacker.GetRaw(Size);
			if(Unpacker.Error())
			{
				DisconnectSession(Bot, "invalid map-data", true);
				return;
			}

			// Be tolerant to duplicate/out-of-order chunks to avoid reconnect storms on flaky proxies.
			if(MapCrc != Bot.m_MapCrc)
			{
				// Match stock client behavior: ignore mismatched map-data packets.
				return;
			}
			if(ChunkIndex != Bot.m_MapChunk)
			{
				// Match stock client behavior: ignore out-of-order/duplicate chunks.
				return;
			}

			const int64_t NewBytes = (int64_t)Bot.m_MapBytes + (int64_t)Size;
			if(Bot.m_MapSize > 0 && NewBytes > (int64_t)Bot.m_MapSize + 65536)
			{
				DisconnectSession(Bot, "map-data overflow", true);
				return;
			}
			Bot.m_MapBytes = (int)minimum<int64_t>(NewBytes, std::numeric_limits<int>::max());
			if(Last)
			{
				CMsgPacker MsgReady(NETMSG_READY, true);
				SendPacket(Bot, MsgReady, MSGFLAG_VITAL | MSGFLAG_FLUSH);
				Bot.m_ReadySent = true;
				Bot.m_ReadySentAt = time_get();
				Bot.m_State = BOT_WAIT_READYTOENTER;
				TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "MAP_DATA done -> sent READY");
			}
			else
			{
				Bot.m_MapChunk++;
				SendMapRequest(Bot);
				if(g_Config.m_ClZzAsyncBotsVerbose)
				{
					char aReq[192];
					str_format(aReq, sizeof(aReq), "request MAP_DATA want_chunk=%d bytes=%d/%d", Bot.m_MapChunk, Bot.m_MapBytes, Bot.m_MapSize);
					TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aReq);
				}
			}
		}
		else if(MsgId == NETMSG_CON_READY)
		{
			Bot.m_ConReadySeen = true;
			if(!Bot.m_StartInfoSent)
			{
				SendStartInfo(pGameClient, Bot);
				Bot.m_StartInfoSent = true;
				TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "CON_READY -> sent STARTINFO");
			}
			if(!Bot.m_EnterGameSent)
			{
				SendEnterGame(Bot);
				Bot.m_State = BOT_WAIT_READYTOENTER;
				Bot.m_NextEnterRetryAt = time_get() + (int64_t)std::clamp(g_Config.m_ClZzAsyncBotsEnterRetryMs, 100, 10000) * time_freq() / 1000;
				TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "CON_READY -> sent ENTERGAME");
			}
		}
		else if(MsgId == NETMSG_PING)
		{
			CMsgPacker MsgPong(NETMSG_PING_REPLY, true);
			SendPacket(Bot, MsgPong, MSGFLAG_FLUSH | MSGFLAG_VITAL);
		}
		else if(MsgId == NETMSG_PINGEX)
		{
			CUuid *pId = (CUuid *)Unpacker.GetRaw(sizeof(CUuid));
			if(!Unpacker.Error())
			{
				CMsgPacker MsgPong(NETMSG_PONGEX, true);
				MsgPong.AddRaw(pId, sizeof(*pId));
				SendPacket(Bot, MsgPong, MSGFLAG_FLUSH | MSGFLAG_VITAL);
			}
		}
		else if(MsgId == NETMSG_RCON_LINE)
		{
			const char *pLine = Unpacker.GetString(CUnpacker::SANITIZE_CC);
			if(!Unpacker.Error() && pLine && pLine[0] != '\0' && g_Config.m_ClZzAsyncBotsVerbose)
			{
				char aMsg[384];
				str_format(aMsg, sizeof(aMsg), "server: %s", pLine);
				TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aMsg);
			}
		}
		else if(MsgId == NETMSG_SNAP || MsgId == NETMSG_SNAPSINGLE || MsgId == NETMSG_SNAPEMPTY || MsgId == NETMSG_SNAPSMALL)
		{
			const int GameTick = Unpacker.GetInt();
			if(!Unpacker.Error() && GameTick >= MIN_TICK && GameTick < MAX_TICK)
			{
				Bot.m_LastAckedSnapshot = GameTick;
				if(Bot.m_State != BOT_INGAME)
				{
					Bot.m_State = BOT_INGAME;
					TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "SNAP received -> INGAME");
					if(Bot.m_aPendingChat[0] != '\0')
					{
						SendChat(Bot, Bot.m_aPendingChat);
						Bot.m_aPendingChat[0] = '\0';
					}
					ArmFastReconnect(pGameClient, Bot, "SNAP");
				}
			}
		}
		return;
	}

	if(MsgId == NETMSGTYPE_SV_CHAT)
	{
		const int Team = Unpacker.GetInt();
		const int ClientId = Unpacker.GetInt();
		const char *pMessage = Unpacker.GetString(CUnpacker::SANITIZE_CC);
		(void)Team;
		if(!Unpacker.Error() && pMessage && pMessage[0] != '\0')
		{
			if(m_AsciiPendingAckSessionId == Bot.m_Id && str_comp(pMessage, m_AsciiPendingAckText.c_str()) == 0)
			{
				m_AsciiPendingAckSessionId = -1;
				m_AsciiPendingAckSince = 0;
				m_AsciiPendingAckText.clear();
				m_NextAsciiActionAt = 0;
			}
			if(g_Config.m_ClZzAsyncBotsMirrorEnable && ClientId == g_Config.m_ClZzAsyncBotsMirrorClientId && Bot.m_State == BOT_INGAME)
				SendChat(Bot, pMessage);
		}
	}
	else if(MsgId == NETMSGTYPE_SV_EMOTICON && g_Config.m_ClZzAsyncBotsMirrorEnable)
	{
		const int ClientId = Unpacker.GetInt();
		const int Emoticon = Unpacker.GetInt();
		if(!Unpacker.Error() && ClientId == g_Config.m_ClZzAsyncBotsMirrorClientId && Bot.m_State == BOT_INGAME)
			SendEmoticon(Bot, Emoticon);
	}
	else if(MsgId == NETMSGTYPE_SV_KILLMSG && g_Config.m_ClZzAsyncBotsMirrorEnable)
	{
		(void)Unpacker.GetInt(); // killer
		const int Victim = Unpacker.GetInt();
		(void)Unpacker.GetInt(); // weapon
		(void)Unpacker.GetInt(); // mode special
		if(!Unpacker.Error() && Victim == g_Config.m_ClZzAsyncBotsMirrorClientId && Bot.m_State == BOT_INGAME)
			SendKill(Bot);
	}

	if(MsgId == NETMSGTYPE_SV_READYTOENTER)
	{
		if(!Bot.m_EnterGameSent)
			SendEnterGame(Bot);
		Bot.m_State = BOT_INGAME;
		TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "SV_READYTOENTER -> INGAME");
		if(Bot.m_aPendingChat[0] != '\0')
		{
			SendChat(Bot, Bot.m_aPendingChat);
			Bot.m_aPendingChat[0] = '\0';
		}
		ArmFastReconnect(pGameClient, Bot, "SV_READYTOENTER");
	}
}

void CAsyncProxyBotManager::TickSession(CGameClient *pGameClient, SBotSession &Bot)
{
	const int64_t Now = time_get();
	if(Bot.m_State == BOT_ERROR)
	{
		return;
	}

	// Reduce frame spikes: connecting sessions are polled at a lower frequency.
	if(Bot.m_State != BOT_INGAME && Bot.m_NextNetPollAt != 0 && Now < Bot.m_NextNetPollAt)
		return;

	Bot.m_NetClient.Update();

	if(Bot.m_NetClient.State() == NETSTATE_ONLINE && !Bot.m_InfoSent)
	{
		// Must be sent right after transport is online (same order as regular client handshake).
		SendClientInfo(pGameClient, Bot);
		Bot.m_InfoSent = true;
		TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "transport ONLINE -> sent CLIENTVER+INFO");
	}

	if(Bot.m_NetClient.State() == NETSTATE_OFFLINE)
	{
		const char *pErr = Bot.m_NetClient.ErrorString();
		if(!pErr || pErr[0] == '\0')
			pErr = "disconnected by server/proxy";
		if(g_Config.m_ClZzAsyncBotsVerbose)
		{
			char aMsg[320];
			str_format(aMsg, sizeof(aMsg), "transport OFFLINE err='%s'", pErr);
			TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aMsg);
		}
		DisconnectSession(Bot, pErr, true);
		return;
	}

	if(Bot.m_State == BOT_CONNECTING && Bot.m_NetClient.State() == NETSTATE_CONNECTING && !Bot.m_InfoSent)
	{
		const int TimeoutMs = std::clamp(g_Config.m_ClZzAsyncBotsConnectTimeoutMs, 1000, 15000);
		const int64_t AliveMs = (Now - Bot.m_ConnectStartedAt) * 1000 / time_freq();
		if(AliveMs >= TimeoutMs)
		{
			DisconnectSession(Bot, "proxy/connect timeout", true);
			if(g_Config.m_ClZzAsyncBotsVerbose)
				TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "timeout -> ERROR");
			return;
		}
	}
	else if(Bot.m_State != BOT_INGAME && Bot.m_NetClient.State() == NETSTATE_ONLINE)
	{
		// Server accepted transport but did not finish game handshake in time.
		const int HandshakeTimeoutMs = std::clamp(g_Config.m_ClZzAsyncBotsConnectTimeoutMs * 3, 3000, 30000);
		const int64_t AliveMs = (Now - Bot.m_ConnectStartedAt) * 1000 / time_freq();
		if(AliveMs >= HandshakeTimeoutMs && Bot.m_LastAckedSnapshot <= 0)
		{
			DisconnectSession(Bot, "server handshake/enter timeout", true);
			if(g_Config.m_ClZzAsyncBotsVerbose)
				TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "handshake timeout -> ERROR");
			return;
		}
	}

	CNetChunk Chunk;
	SECURITY_TOKEN ResponseToken = NET_SECURITY_TOKEN_UNKNOWN;
	const bool Sixup = pGameClient && pGameClient->Client() && pGameClient->Client()->IsSixup();
	int RecvBudget = Bot.m_State == BOT_INGAME ? 24 : 8;
	while(RecvBudget-- > 0 && Bot.m_NetClient.Recv(&Chunk, &ResponseToken, Sixup))
		HandleChunk(pGameClient, Bot, Chunk);

	if(Bot.m_State == BOT_INGAME)
	{
		if(g_Config.m_ClZzAsyncBotsFastReconnect && Bot.m_NextReconnectAt != 0 && Now >= Bot.m_NextReconnectAt)
		{
			Bot.m_NextReconnectAt = 0;
			ReconnectSession(pGameClient, Bot);
			return;
		}
	}
	else if(Bot.m_State == BOT_WAIT_READYTOENTER && Bot.m_EnterGameSent)
	{
		if(Bot.m_NextEnterRetryAt == 0 || Now >= Bot.m_NextEnterRetryAt)
		{
			SendEnterGame(Bot);
			Bot.m_NextEnterRetryAt = Now + (int64_t)std::clamp(g_Config.m_ClZzAsyncBotsEnterRetryMs, 100, 10000) * time_freq() / 1000;
			TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "retry ENTERGAME");
		}
	}

	if(g_Config.m_ClZzAsyncBotsVerbose && Bot.m_State != BOT_INGAME && Now >= Bot.m_NextDiagAt)
	{
		const int64_t AliveMs = (Now - Bot.m_ConnectStartedAt) * 1000 / time_freq();
		char aDiag[320];
		str_format(aDiag, sizeof(aDiag), "diag wait=%lldms netstate=%d info=%d conready=%d startinfo=%d ready=%d enter=%d ack=%d", AliveMs, Bot.m_NetClient.State(), Bot.m_InfoSent ? 1 : 0, Bot.m_ConReadySeen ? 1 : 0, Bot.m_StartInfoSent ? 1 : 0, Bot.m_ReadySent ? 1 : 0, Bot.m_EnterGameSent ? 1 : 0, Bot.m_LastAckedSnapshot);
		TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aDiag);
		Bot.m_NextDiagAt = Now + time_freq();
	}

	if(m_pScript && Bot.m_State == BOT_INGAME)
		m_pScript->Tick(pGameClient, Bot.m_Id);

	if(Bot.m_State != BOT_INGAME)
		Bot.m_NextNetPollAt = Now + time_freq() / 100; // ~10 ms polling for non-ingame sessions
	else
		Bot.m_NextNetPollAt = 0;
}

void CAsyncProxyBotManager::SpawnSession(CGameClient *pGameClient, const SProxyEntry &Proxy, int WaveIndex)
{
	m_vSessions.emplace_back();
	SBotSession &Bot = m_vSessions.back();
	Bot.m_Id = (int)m_vSessions.size();
	Bot.m_Proxy = Proxy;
	Bot.m_ConnectionId = RandomUuid();

	int LookupTypes = NETTYPE_IPV4 | NETTYPE_IPV6;
	const bool ProxyIsV4 = (Proxy.m_Addr.type & NETTYPE_IPV4) != 0;
	const bool ProxyIsV6 = (Proxy.m_Addr.type & NETTYPE_IPV6) != 0;
	if(ProxyIsV4 && !ProxyIsV6)
		LookupTypes = NETTYPE_IPV4;
	else if(ProxyIsV6 && !ProxyIsV4)
		LookupTypes = NETTYPE_IPV6;

	if(!ResolveServerAddress(pGameClient, LookupTypes, Bot.m_ServerAddr))
	{
		Bot.m_State = BOT_ERROR;
		str_copy(Bot.m_aError, "resolve server address failed", sizeof(Bot.m_aError));
		if(pGameClient)
			TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "failed to resolve server address for proxy");
		return;
	}
	const int SingleType = PickSingleIpType(Bot.m_ServerAddr.type, LookupTypes);
	if(SingleType == 0)
	{
		Bot.m_State = BOT_ERROR;
		str_copy(Bot.m_aError, "resolved address has no IPv4/IPv6 type", sizeof(Bot.m_aError));
		if(pGameClient)
			TraceBot(pGameClient, Bot.m_Id, Bot.m_State, "resolve returned unsupported type");
		return;
	}
	Bot.m_ServerAddr.type = SingleType;
	if(pGameClient && g_Config.m_ClZzAsyncBotsVerbose)
	{
		char aAddr[NETADDR_MAXSTRSIZE];
		net_addr_str(&Bot.m_ServerAddr, aAddr, sizeof(aAddr), true);
		char aMsg[320];
		str_format(aMsg, sizeof(aMsg), "resolved target=%s type=%d", aAddr, Bot.m_ServerAddr.type);
		TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aMsg);
	}

	BuildBotAppearance(pGameClient, Bot, WaveIndex);

	ReconnectSession(pGameClient, Bot);
}

void CAsyncProxyBotManager::ScheduleSessions(CGameClient *pGameClient)
{
	if(m_NextSpawnIndex >= (int)m_vSpawnQueue.size())
		return;

	const int64_t Now = time_get();
	if(Now < m_NextWaveAt)
		return;

	// Spawn one bot per update to avoid UI freeze from many blocking SOCKS5 connects in one frame.
	const int Index = m_NextSpawnIndex++;
	SpawnSession(pGameClient, m_vSpawnQueue[Index], Index);

	// Keep batch logic: after every batch size bots, wait configured delay; otherwise continue quickly.
	if(m_BatchSize <= 1 || (m_NextSpawnIndex % m_BatchSize) == 0)
		m_NextWaveAt = Now + (int64_t)m_BatchDelayMs * time_freq() / 1000;
	else
		m_NextWaveAt = Now + time_freq() / 200; // ~5ms pacing
}

void CAsyncProxyBotManager::RefreshStats()
{
	m_Stats = {};
	m_Stats.m_Total = (int)m_vSessions.size();
	for(const SBotSession &Bot : m_vSessions)
	{
		if(Bot.m_State == BOT_CONNECTING || Bot.m_State == BOT_DOWNLOADING_MAP || Bot.m_State == BOT_WAIT_READYTOENTER)
			m_Stats.m_Connecting++;
		else if(Bot.m_State == BOT_INGAME)
			m_Stats.m_Ingame++;
		else if(Bot.m_State == BOT_ERROR)
			m_Stats.m_Error++;
	}
	str_format(
		m_aStatus,
		sizeof(m_aStatus),
		"Total: %d | Connecting: %d | Ingame: %d | Error: %d",
		m_Stats.m_Total,
		m_Stats.m_Connecting,
		m_Stats.m_Ingame,
		m_Stats.m_Error);
}

void CAsyncProxyBotManager::StartFromConfig(CGameClient *pGameClient)
{
	if(!pGameClient || !pGameClient->Client() || pGameClient->Client()->State() != IClient::STATE_ONLINE)
	{
		str_copy(m_aStatus, "Not connected to server", sizeof(m_aStatus));
		return;
	}

	if(m_Running || !m_vSessions.empty() || !m_vSpawnQueue.empty())
		StopAll();

	std::vector<SProxyEntry> vProxies;
	if(!ParseProxyList(g_Config.m_ClZzAsyncBotsProxies, vProxies, pGameClient))
	{
		str_copy(m_aStatus, "Proxy list is empty or invalid", sizeof(m_aStatus));
		return;
	}

	const int PerProxy = std::clamp(g_Config.m_ClZzAsyncBotsPerProxy, 1, 64);
	const int MaxTotal = std::clamp(g_Config.m_ClZzAsyncBotsMaxTotal, 1, 2048);
	m_BatchSize = std::clamp(g_Config.m_ClZzAsyncBotsBatchSize, 1, 128);
	m_BatchDelayMs = std::clamp(g_Config.m_ClZzAsyncBotsBatchDelayMs, 0, 5000);

	m_vSpawnQueue.clear();
	m_vSpawnQueue.reserve(vProxies.size() * PerProxy);
	for(const SProxyEntry &Proxy : vProxies)
	{
		for(int i = 0; i < PerProxy; i++)
		{
			if((int)m_vSpawnQueue.size() >= MaxTotal)
				break;
			m_vSpawnQueue.push_back(Proxy);
		}
		if((int)m_vSpawnQueue.size() >= MaxTotal)
			break;
	}

	if(m_vSpawnQueue.empty())
	{
		str_copy(m_aStatus, "Nothing to spawn after limits", sizeof(m_aStatus));
		return;
	}

	m_ServerAddr = pGameClient->Client()->ServerAddress();
	m_NextSpawnIndex = 0;
	m_NextWaveAt = 0;
	m_Running = true;
	m_QueuedStart = false;
	m_TotalTarget = (int)m_vSpawnQueue.size();
	str_format(m_aStatus, sizeof(m_aStatus), "Spawning %d bots", m_TotalTarget);
	if(pGameClient && g_Config.m_ClZzAsyncBotsVerbose)
	{
		char aMsg[256];
		str_format(aMsg, sizeof(aMsg), "start target=%d proxies=%d per_proxy=%d batch=%d delay=%dms", m_TotalTarget, (int)vProxies.size(), PerProxy, m_BatchSize, m_BatchDelayMs);
		pGameClient->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz_async_bots", aMsg);
	}
}

void CAsyncProxyBotManager::StopAll()
{
	for(SBotSession &Bot : m_vSessions)
	{
		Bot.m_NetClient.Disconnect("bot-manager-stop");
		Bot.m_NetClient.Close();
	}
	m_vSessions.clear();
	m_vSpawnQueue.clear();
	m_SavedMapCache.clear();
	m_Running = false;
	m_QueuedStart = false;
	m_NextSpawnIndex = 0;
	m_NextWaveAt = 0;
	ClearAsciiRuntimeState();
	str_copy(m_aStatus, "Stopped", sizeof(m_aStatus));
	RefreshStats();
}

void CAsyncProxyBotManager::QueueChatAll(const char *pText)
{
	if(!pText || pText[0] == '\0')
		return;
	for(SBotSession &Bot : m_vSessions)
	{
		if(Bot.m_State == BOT_INGAME)
			SendChat(Bot, pText);
		else
			str_copy(Bot.m_aPendingChat, pText, sizeof(Bot.m_aPendingChat));
	}
}

void CAsyncProxyBotManager::StartAsciiBroadcast(const char *pText, int Cycles, int CooldownSeconds)
{
	ClearAsciiRuntimeState();
	m_AsciiInfiniteCycles = Cycles == 0;
	m_AsciiCyclesLeft = maximum(0, Cycles);
	m_AsciiCooldownSeconds = std::clamp(CooldownSeconds, 1, 60);
	if(!pText || pText[0] == '\0' || (!m_AsciiInfiniteCycles && m_AsciiCyclesLeft <= 0))
		return;

	const char *pLine = pText;
	const char *p = pText;
	while(true)
	{
		if(*p == '\n' || *p == '\0')
		{
			const char *pLineEnd = p;
			if(pLineEnd > pLine && pLineEnd[-1] == '\r')
				--pLineEnd;
			const size_t LineLength = pLineEnd - pLine;
			if(LineLength > 0 && !IsWhitespaceOnly(pLine, LineLength))
				m_vAsciiLines.emplace_back(pLine, LineLength);
			if(*p == '\0')
				break;
			pLine = p + 1;
		}
		++p;
	}
	if(m_vAsciiLines.empty())
		return;
	m_NextAsciiActionAt = 0;
	if(m_AsciiInfiniteCycles)
		str_format(m_aStatus, sizeof(m_aStatus), "ASCII armed: %d line(s), infinite cycles", (int)m_vAsciiLines.size());
	else
		str_format(m_aStatus, sizeof(m_aStatus), "ASCII armed: %d line(s), %d cycle(s)", (int)m_vAsciiLines.size(), m_AsciiCyclesLeft);
}

void CAsyncProxyBotManager::StopAsciiBroadcast()
{
	ClearAsciiRuntimeState();
	str_copy(m_aStatus, "ASCII stopped", sizeof(m_aStatus));
}

void CAsyncProxyBotManager::ClearAsciiRuntimeState()
{
	m_vAsciiLines.clear();
	m_vAsciiPlanSessionIds.clear();
	m_AsciiCooldownUntilBySessionId.clear();
	m_AsciiPendingAckText.clear();
	m_AsciiPendingAckSessionId = -1;
	m_AsciiPendingAckSince = 0;
	m_AsciiLineIndex = 0;
	m_AsciiCyclesLeft = 0;
	m_AsciiInfiniteCycles = false;
	m_NextAsciiActionAt = 0;
}

void CAsyncProxyBotManager::QueueEmoticonAll(int Emoticon)
{
	for(SBotSession &Bot : m_vSessions)
	{
		if(Bot.m_State == BOT_INGAME)
			SendEmoticon(Bot, Emoticon);
	}
}

void CAsyncProxyBotManager::ArmFastReconnect(CGameClient *pGameClient, SBotSession &Bot, const char *pReason)
{
	if(!g_Config.m_ClZzAsyncBotsFastReconnect || Bot.m_State != BOT_INGAME || Bot.m_NextReconnectAt != 0)
		return;

	const int DelayMs = std::clamp(g_Config.m_ClZzAsyncBotsFastReconnectDelayMs, 0, 3000);
	Bot.m_NextReconnectAt = time_get() + (int64_t)DelayMs * time_freq() / 1000;
	if(g_Config.m_ClZzAsyncBotsVerbose)
	{
		char aMsg[192];
		str_format(aMsg, sizeof(aMsg), "fast reconnect armed by %s in %dms", pReason ? pReason : "ingame", DelayMs);
		TraceBot(pGameClient, Bot.m_Id, Bot.m_State, aMsg);
	}
}

void CAsyncProxyBotManager::KillAllIngame()
{
	for(SBotSession &Bot : m_vSessions)
	{
		if(Bot.m_State == BOT_INGAME)
			SendKill(Bot);
	}
}

void CAsyncProxyBotManager::VoteAll(int Vote)
{
	const int ClampedVote = std::clamp(Vote, -1, 1);
	for(SBotSession &Bot : m_vSessions)
	{
		if(Bot.m_State != BOT_INGAME)
			continue;
		CNetMsg_Cl_Vote Msg;
		Msg.m_Vote = ClampedVote;
		CMsgPacker Packer(&Msg);
		Msg.Pack(&Packer);
		SendPacket(Bot, Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
	}
}

void CAsyncProxyBotManager::NotifyLocalVoteCommand(int SourceClientId, int Vote)
{
	m_LocalVoteCommandSourceId = SourceClientId;
	m_LocalVoteCommandChoice = std::clamp(Vote, -1, 1);
	++m_LocalVoteCommandSerial;
}

void CAsyncProxyBotManager::ApplyLiveSettings(CGameClient *pGameClient)
{
	int WaveIndex = 0;
	for(SBotSession &Bot : m_vSessions)
	{
		BuildBotAppearance(pGameClient, Bot, WaveIndex++);
		if(Bot.m_State == BOT_INGAME || Bot.m_StartInfoSent)
			SendChangeInfo(Bot);
	}
}

void CAsyncProxyBotManager::Tick(CGameClient *pGameClient)
{
	const int64_t Now = time_get();
	if(!m_Running)
	{
		m_NextSynchronizedInputAt = 0;
		m_LocalVoteCommandChoice = 0;
		m_LocalVoteCommandSourceId = -1;
		m_LocalVoteCommandHandledSerial = m_LocalVoteCommandSerial;
		RefreshStats();
		return;
	}

	ScheduleSessions(pGameClient);
	UpdateCompanionPlanner(pGameClient);
	for(SBotSession &Bot : m_vSessions)
		TickSession(pGameClient, Bot);

	if(g_Config.m_ClZzAsyncBotsMirrorEnable && pGameClient)
	{
		if(m_LocalVoteCommandHandledSerial != m_LocalVoteCommandSerial &&
			m_LocalVoteCommandChoice != 0 &&
			m_LocalVoteCommandSourceId == g_Config.m_ClZzAsyncBotsMirrorClientId)
		{
			VoteAll(m_LocalVoteCommandChoice);
			m_LocalVoteCommandHandledSerial = m_LocalVoteCommandSerial;
		}
	}

	if(pGameClient)
	{
		int InputRate = std::clamp(g_Config.m_ClZzAsyncBotsInputRate, 1, 100);
		if(g_Config.m_ClZzAsyncBotsCompanionMode)
			InputRate = maximum(InputRate, 50);
		const int64_t InputStep = maximum<int64_t>(1, time_freq() / InputRate);
		if(m_NextSynchronizedInputAt == 0 || Now >= m_NextSynchronizedInputAt)
		{
			for(SBotSession &Bot : m_vSessions)
			{
				if(Bot.m_State == BOT_INGAME)
					SendInput(pGameClient, Bot);
			}
			m_NextSynchronizedInputAt = Now + InputStep;
		}
	}

	if(!m_vAsciiLines.empty() && (m_AsciiInfiniteCycles || m_AsciiCyclesLeft > 0))
	{
		const int64_t Now = time_get();
		const int64_t FallbackTimeout = 8 * time_freq();
		const int64_t RetryDelay = time_freq() / 4;
		const int64_t CycleDelay = time_freq();

		if(m_AsciiPendingAckSessionId >= 0)
		{
			if(m_AsciiPendingAckSince != 0 && Now - m_AsciiPendingAckSince < FallbackTimeout)
			{
				RefreshStats();
				if(m_NextSpawnIndex >= (int)m_vSpawnQueue.size() && m_Stats.m_Ingame == 0 && m_Stats.m_Connecting == 0)
					str_copy(m_aStatus, "All sessions ended", sizeof(m_aStatus));
				return;
			}
			m_AsciiPendingAckSessionId = -1;
			m_AsciiPendingAckSince = 0;
			m_AsciiPendingAckText.clear();
		}

		if(m_AsciiLineIndex >= (int)m_vAsciiLines.size())
		{
			m_AsciiLineIndex = 0;
			if(!m_AsciiInfiniteCycles)
				--m_AsciiCyclesLeft;
			if(m_AsciiInfiniteCycles || m_AsciiCyclesLeft > 0)
			{
				m_NextAsciiActionAt = Now + CycleDelay;
				str_format(m_aStatus, sizeof(m_aStatus), "ASCII cycle sent (%d line(s))", (int)m_vAsciiLines.size());
			}
			else
			{
				str_copy(m_aStatus, "ASCII finished", sizeof(m_aStatus));
			}
			RefreshStats();
			if(m_NextSpawnIndex >= (int)m_vSpawnQueue.size() && m_Stats.m_Ingame == 0 && m_Stats.m_Connecting == 0)
				str_copy(m_aStatus, "All sessions ended", sizeof(m_aStatus));
			return;
		}

		if(m_NextAsciiActionAt != 0 && Now < m_NextAsciiActionAt)
		{
			RefreshStats();
			if(m_NextSpawnIndex >= (int)m_vSpawnQueue.size() && m_Stats.m_Ingame == 0 && m_Stats.m_Connecting == 0)
				str_copy(m_aStatus, "All sessions ended", sizeof(m_aStatus));
			return;
		}

		std::vector<SBotSession *> vAvailable;
		for(SBotSession &Bot : m_vSessions)
		{
			if(Bot.m_State != BOT_INGAME)
				continue;
			const auto It = m_AsciiCooldownUntilBySessionId.find(Bot.m_Id);
			if(It != m_AsciiCooldownUntilBySessionId.end() && Now < It->second)
				continue;
			vAvailable.push_back(&Bot);
		}

		if(vAvailable.empty())
		{
			str_format(m_aStatus, sizeof(m_aStatus), "ASCII waiting: no bot without cooldown for line %d/%d", m_AsciiLineIndex + 1, (int)m_vAsciiLines.size());
			m_NextAsciiActionAt = Now + RetryDelay;
			RefreshStats();
			if(m_NextSpawnIndex >= (int)m_vSpawnQueue.size() && m_Stats.m_Ingame == 0 && m_Stats.m_Connecting == 0)
				str_copy(m_aStatus, "All sessions ended", sizeof(m_aStatus));
			return;
		}

		SBotSession *pSender = vAvailable.front();
		const int SentLineIndex = m_AsciiLineIndex;
		SendChat(*pSender, m_vAsciiLines[m_AsciiLineIndex].c_str());
		m_AsciiCooldownUntilBySessionId[pSender->m_Id] = Now + (int64_t)m_AsciiCooldownSeconds * time_freq();
		m_AsciiPendingAckSessionId = pSender->m_Id;
		m_AsciiPendingAckSince = Now;
		m_AsciiPendingAckText = m_vAsciiLines[m_AsciiLineIndex];
		str_format(m_aStatus, sizeof(m_aStatus), "ASCII line %d/%d sent, waiting for server echo", SentLineIndex + 1, (int)m_vAsciiLines.size());
		++m_AsciiLineIndex;
		m_NextAsciiActionAt = 0;
	}
	RefreshStats();

	if(m_NextSpawnIndex >= (int)m_vSpawnQueue.size() && m_Stats.m_Ingame == 0 && m_Stats.m_Connecting == 0)
		str_copy(m_aStatus, "All sessions ended", sizeof(m_aStatus));
}
