#include "dummy_auto_unfreeze.h"

#include "../controls.h"

#include <base/math.h>
#include <base/vmath.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <engine/client.h>
#include <engine/shared/config.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/collision.h>
#include <game/mapitems.h>

namespace
{
struct CLaserLine
{
	vec2 m_From;
	vec2 m_To;
};

constexpr int MAX_PREDICTED_TICKS = 300;
constexpr float TEE_RADIUS = 30.0f;
constexpr float AIM_STEP = 0.04f;

bool IntersectCircleLineNearestT(vec2 CirclePos, float Radius, vec2 LineStart, vec2 LineEnd, float *pOutT)
{
	const vec2 Delta = LineEnd - LineStart;
	const vec2 Relative = LineStart - CirclePos;
	const float A = dot(Delta, Delta);
	if(A < 0.00001f)
	{
		return false;
	}

	const float B = 2.0f * dot(Relative, Delta);
	const float C = dot(Relative, Relative) - Radius * Radius;
	float Discriminant = B * B - 4.0f * A * C;
	if(Discriminant < 0.0f)
	{
		return false;
	}

	Discriminant = sqrtf(Discriminant);
	const float T1 = (-B - Discriminant) / (2.0f * A);
	const float T2 = (-B + Discriminant) / (2.0f * A);

	float BestT = 2.0f;
	if(T1 >= 0.0f && T1 <= 1.0f)
	{
		BestT = T1;
	}
	if(T2 >= 0.0f && T2 <= 1.0f)
	{
		BestT = minimum(BestT, T2);
	}
	if(BestT > 1.0f)
	{
		return false;
	}

	if(pOutT)
	{
		*pOutT = BestT;
	}
	return true;
}

bool IsFreezeLaserableTile(int Tile)
{
	return Tile == TILE_FREEZE || Tile == TILE_LFREEZE;
}

bool IsPositionOnFreezeTile(CGameClient *pGameClient, vec2 Pos)
{
	const int Tile = pGameClient->Collision()->GetTileIndex(pGameClient->Collision()->GetPureMapIndex(Pos.x, Pos.y));
	return IsFreezeLaserableTile(Tile);
}

bool IsFrozenTarget(CGameClient *pGameClient, const CCharacter *pTargetChar, int TargetId)
{
	if(TargetId < 0 || TargetId >= MAX_CLIENTS)
	{
		return false;
	}

	if((pTargetChar && pTargetChar->Core()->m_DeepFrozen) || pGameClient->m_aClients[TargetId].m_DeepFrozen)
	{
		return false;
	}

	const vec2 Pos = pGameClient->m_aClients[TargetId].m_Predicted.m_Pos;
	const vec2 Vel = pGameClient->m_aClients[TargetId].m_Predicted.m_Vel;
	const int CurrentTile = pGameClient->Collision()->GetTileIndex(pGameClient->Collision()->GetPureMapIndex(Pos.x, Pos.y));
	const int NextTile = pGameClient->Collision()->GetTileIndex(pGameClient->Collision()->GetPureMapIndex(Pos.x + Vel.x, Pos.y + Vel.y));
	const bool OnFreezeTile = IsFreezeLaserableTile(CurrentTile) || IsFreezeLaserableTile(NextTile);

	return (pTargetChar && pTargetChar->m_FreezeTime > 0) || pGameClient->m_aClients[TargetId].m_LiveFrozen || OnFreezeTile;
}

int FindFirstExitTick(CGameClient *pGameClient, const vec2 *pPredictedPos, int PredictedTicks)
{
	for(int i = 0; i < PredictedTicks; ++i)
	{
		if(!IsPositionOnFreezeTile(pGameClient, pPredictedPos[i]))
		{
			return i;
		}
	}

	return -1;
}

void PredictTargetPositions(CGameClient *pGameClient, int TargetId, vec2 *pPosArray, int PredictedTicks)
{
	vec2 Pos = pGameClient->m_aClients[TargetId].m_Predicted.m_Pos;
	vec2 Vel = pGameClient->m_aClients[TargetId].m_Predicted.m_Vel;
	const CTuningParams &Tuning = pGameClient->m_aClients[TargetId].m_Predicted.m_Tuning;

	for(int i = 0; i < PredictedTicks; ++i)
	{
		Vel.y += Tuning.m_Gravity;
		Vel.x *= Tuning.m_AirFriction;
		Vel.y *= Tuning.m_AirFriction;

		vec2 NextPosX = Pos + vec2(Vel.x, 0.0f);
		vec2 ColPosX = NextPosX;
		if(pGameClient->Collision()->IntersectLine(Pos, NextPosX, nullptr, &ColPosX))
		{
			Vel.x = 0.0f;
			Pos = ColPosX;
		}
		else
		{
			Pos = NextPosX;
		}

		vec2 NextPosY = Pos + vec2(0.0f, Vel.y);
		vec2 ColPosY = NextPosY;
		if(pGameClient->Collision()->IntersectLine(Pos, NextPosY, nullptr, &ColPosY))
		{
			if(Vel.y > 0.0f)
			{
				Vel.x = 0.0f;
			}
			else
			{
				Vel.y = 0.0f;
			}
			Pos = ColPosY;
		}
		else
		{
			Pos = NextPosY;
		}

		pPosArray[i] = Pos;
	}
}

std::vector<CLaserLine> BuildLaserLines(CGameClient *pGameClient, int HelperId, vec2 StartPos, vec2 Direction)
{
	std::vector<CLaserLine> vLines;
	const CTuningParams &Tuning = pGameClient->m_aClients[HelperId].m_Predicted.m_Tuning;
	const int MaxBounces = std::clamp((int)Tuning.m_LaserBounceNum, 0, 16);
	float DistanceLeft = Tuning.m_LaserReach;
	vec2 CurrentDir = normalize(Direction);
	vec2 LastPos = StartPos;
	constexpr float MinDistance = 0.0001f;

	vLines.reserve(MaxBounces + 1);

	for(int i = 0; i <= MaxBounces && DistanceLeft > MinDistance; ++i)
	{
		vec2 ColPos;
		vec2 BeforeColPos;
		vec2 NextPos = LastPos + CurrentDir * DistanceLeft;

		int TeleNr = 0;
		if(pGameClient->Collision()->IntersectLineTeleWeapon(LastPos, NextPos, &ColPos, &BeforeColPos, &TeleNr))
		{
			vec2 MovePointPos = ColPos - (ColPos - BeforeColPos);
			vec2 MoveDir = ColPos - MovePointPos;
			pGameClient->Collision()->MovePoint(&MovePointPos, &MoveDir, 1.0f, nullptr);

			NextPos = BeforeColPos;
			if(length(MoveDir) > MinDistance)
			{
				CurrentDir = normalize(MoveDir);
			}
		}

		vLines.push_back({LastPos, NextPos});

		const float TraveledDist = distance(LastPos, NextPos);
		if(TraveledDist <= MinDistance)
		{
			break;
		}

		DistanceLeft -= TraveledDist + Tuning.m_LaserBounceCost;
		LastPos = NextPos;
	}

	return vLines;
}

bool SegmentHitsTargetFirst(CGameClient *pGameClient, int ShooterId, int TargetId, vec2 From, vec2 To)
{
	float BestT = 2.0f;
	int BestId = -1;

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(i == ShooterId || !pGameClient->m_aClients[i].m_Active || !pGameClient->m_Snap.m_aCharacters[i].m_Active)
		{
			continue;
		}

		float HitT;
		if(!IntersectCircleLineNearestT(pGameClient->m_aClients[i].m_Predicted.m_Pos, TEE_RADIUS, From, To, &HitT))
		{
			continue;
		}

		if(HitT < BestT)
		{
			BestT = HitT;
			BestId = i;
		}
	}

	return BestId == TargetId;
}

vec2 FindDummyAutoUnfreezeAim(CGameClient *pGameClient, int HelperId, int TargetId)
{
	const vec2 ShooterPos = pGameClient->m_aClients[HelperId].m_Predicted.m_Pos;
	const vec2 TargetPos = pGameClient->m_aClients[TargetId].m_Predicted.m_Pos;
	if(distance(ShooterPos, TargetPos) < 0.001f)
	{
		return vec2(0.0f, 0.0f);
	}

	vec2 aPredictedPos[MAX_PREDICTED_TICKS];
	PredictTargetPositions(pGameClient, TargetId, aPredictedPos, MAX_PREDICTED_TICKS);
	const int ExitTick = FindFirstExitTick(pGameClient, aPredictedPos, MAX_PREDICTED_TICKS);
	if(ExitTick < 0)
	{
		return vec2(0.0f, 0.0f);
	}

	const CTuningParams &Tuning = pGameClient->m_aClients[HelperId].m_Predicted.m_Tuning;
	const float BounceDelayTicks = Tuning.m_LaserBounceDelay * (pGameClient->Client()->GameTickSpeed() / 1000.0f);
	const float PingTicks = static_cast<float>(pGameClient->Client()->GetPredictionTime()) / 100.0f;
	const float BaseAngle = angle(TargetPos - ShooterPos);
	const int MaxSteps = (int)std::ceil(pi / AIM_STEP);
	vec2 BestAim(0.0f, 0.0f);
	int BestImpactTick = MAX_PREDICTED_TICKS + 1;

	auto TryAngle = [&](float ScanAngle) {
		const vec2 Direction = direction(ScanAngle);
		const std::vector<CLaserLine> vLines = BuildLaserLines(pGameClient, HelperId, ShooterPos, Direction);
		if(vLines.empty())
		{
			return;
		}

		for(int i = 0; i < (int)vLines.size(); ++i)
		{
			const int PredTick = std::clamp((int)std::lround(i * BounceDelayTicks - PingTicks), 0, MAX_PREDICTED_TICKS - 1);
			if(PredTick < ExitTick || IsPositionOnFreezeTile(pGameClient, aPredictedPos[PredTick]))
			{
				continue;
			}
			if(!SegmentHitsTargetFirst(pGameClient, HelperId, TargetId, vLines[i].m_From, vLines[i].m_To))
			{
				continue;
			}
			if(!IntersectCircleLineNearestT(aPredictedPos[PredTick], TEE_RADIUS, vLines[i].m_From, vLines[i].m_To, nullptr))
			{
				continue;
			}

			if(PredTick < BestImpactTick)
			{
				BestImpactTick = PredTick;
				BestAim = vLines[0].m_To - vLines[0].m_From;
			}
		}
	};

	for(int i = 0; i <= MaxSteps; ++i)
	{
		const float Offset = i * AIM_STEP;
		TryAngle(BaseAngle + Offset);

		if(i == 0)
		{
			continue;
		}

		TryAngle(BaseAngle - Offset);
	}

	return BestAim;
}
} 

namespace FlyHelper
{
void CDummyAutoUnfreeze::Reset()
{
	for(auto &State : m_aState)
	{
		State = SState{};
	}
}

void CDummyAutoUnfreeze::Tick(CGameClient *pGameClient, int ControlIndex, CNetObj_PlayerInput &HelperInput, CNetObj_PlayerInput &DummyInput)
{
	if(ControlIndex < 0 || ControlIndex >= NUM_DUMMIES)
	{
		return;
	}

	SState &State = m_aState[ControlIndex];
	const auto RestoreInput = [&]() {
		if(State.m_Overriding)
		{
			HelperInput = State.m_SavedInput;
			DummyInput = State.m_SavedInput;
		}
		State = SState{};
	};

	if(!pGameClient || !g_Config.m_ClZzDummyAutoUnfreeze || g_Config.m_ClDummyHammer || !pGameClient->Client()->DummyConnected() ||
		pGameClient->Client()->State() != IClient::STATE_ONLINE || pGameClient->m_Snap.m_SpecInfo.m_Active)
	{
		RestoreInput();
		return;
	}

	const int HelperIndex = !ControlIndex;
	const int TargetId = pGameClient->m_aLocalIds[ControlIndex];
	const int HelperId = pGameClient->m_aLocalIds[HelperIndex];
	if(TargetId < 0 || TargetId >= MAX_CLIENTS || HelperId < 0 || HelperId >= MAX_CLIENTS ||
		!pGameClient->m_aClients[TargetId].m_Active || !pGameClient->m_aClients[HelperId].m_Active ||
		!pGameClient->m_Snap.m_aCharacters[TargetId].m_Active || !pGameClient->m_Snap.m_aCharacters[HelperId].m_Active)
	{
		RestoreInput();
		return;
	}

	CCharacter *pTargetChar = pGameClient->m_PredictedWorld.GetCharacterById(TargetId);
	CCharacter *pHelperChar = pGameClient->m_PredictedWorld.GetCharacterById(HelperId);
	const bool HelperFrozen = (pHelperChar && (pHelperChar->m_FreezeTime > 0 || pHelperChar->Core()->m_DeepFrozen)) ||
		pGameClient->m_aClients[HelperId].m_DeepFrozen || pGameClient->m_aClients[HelperId].m_LiveFrozen;
	const bool HelperHasLaser = (pHelperChar && pHelperChar->GetWeaponGot(WEAPON_LASER)) ||
		pGameClient->m_aClients[HelperId].m_Predicted.m_ActiveWeapon == WEAPON_LASER;
	const bool LaserHitsDisabled = pGameClient->m_aClients[HelperId].m_LaserHitDisabled;
	if(HelperFrozen || !HelperHasLaser || LaserHitsDisabled || !IsFrozenTarget(pGameClient, pTargetChar, TargetId))
	{
		RestoreInput();
		return;
	}

	if(!State.m_Overriding)
	{
		State.m_SavedInput = DummyInput;
		State.m_Overriding = true;
	}

	CNetObj_PlayerInput NextInput = DummyInput;
	if(State.m_FireReleasePending)
	{
		if(NextInput.m_Fire & 1)
		{
			NextInput.m_Fire++;
		}
		State.m_FireReleasePending = false;
	}

	NextInput.m_Direction = 0;
	NextInput.m_Jump = 0;
	NextInput.m_Hook = 0;
	NextInput.m_WantedWeapon = WEAPON_LASER + 1;

	const vec2 FallbackAim = pGameClient->m_aClients[TargetId].m_Predicted.m_Pos - pGameClient->m_aClients[HelperId].m_Predicted.m_Pos;
	const vec2 AimDir = FindDummyAutoUnfreezeAim(pGameClient, HelperId, TargetId);
	const vec2 FinalAim = length(AimDir) > 0.001f ? AimDir : FallbackAim;
	NextInput.m_TargetX = (int)FinalAim.x;
	NextInput.m_TargetY = (int)FinalAim.y;

	const int64_t Now = time_get();
	const bool CanShoot = length(AimDir) > 0.001f && Now >= State.m_LastShotTime + (int64_t)(time_freq() * 0.12f);
	if(CanShoot && !(NextInput.m_Fire & 1))
	{
		NextInput.m_Fire++;
		State.m_FireReleasePending = true;
		State.m_LastShotTime = Now;
	}

	HelperInput = NextInput;
	DummyInput = NextInput;
}
} 
 // сделано флайхелпером )(00()0)
