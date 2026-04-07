/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"

#include <base/math.h>
#include <base/system.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/gameclient.h>
#include <game/collision.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

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

namespace
{
constexpr int ZZ_BLATANT_AVOID_BASE_TICKS = 14;
constexpr int ZZ_BLATANT_AVOID_MIN_TICKS = 8;
constexpr int ZZ_BLATANT_AVOID_MAX_TICKS = 30;
constexpr int ZZ_BLATANT_AVOID_VALIDATE_EXTRA_TICKS = 10;
constexpr int ZZ_BLATANT_AVOID_TRIGGER_TICKS = 4;
constexpr int ZZ_BLATANT_AVOID_PANIC_TICKS = 2;
constexpr int ZZ_BLATANT_AVOID_ONE_TILE_TICKS = 1;
constexpr int ZZ_BLATANT_AVOID_MAX_CANDIDATES = 24;
constexpr float ZZ_BLATANT_AVOID_MIN_HOOK_DIST = 24.0f;
constexpr float ZZ_BLATANT_AVOID_PANIC_MIN_HOOK_DIST = 8.0f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_MIN_HOOK_DIST = 4.0f;
constexpr float ZZ_BLATANT_AVOID_DUPLICATE_DIST = 18.0f;
constexpr float ZZ_BLATANT_AVOID_TRIGGER_DIST = 40.0f;
constexpr float ZZ_BLATANT_AVOID_TRIGGER_MAX_DIST = 72.0f;
constexpr float ZZ_BLATANT_AVOID_PANIC_DIST = 24.0f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_DIST = 44.0f;
constexpr float ZZ_BLATANT_AVOID_CURSOR_DANGER_SOFT_DOT = 0.20f;
constexpr float ZZ_BLATANT_AVOID_CURSOR_DANGER_HARD_DOT = 0.60f;
constexpr float ZZ_BLATANT_AVOID_PANIC_CURSOR_DANGER_DOT = 0.45f;
constexpr float ZZ_BLATANT_AVOID_DIRECTION_SCORE = 96.0f;
constexpr float ZZ_BLATANT_AVOID_CURSOR_INPUT_SCORE = 1.15f;
constexpr float ZZ_BLATANT_AVOID_CURSOR_HOOK_SCORE = 84.0f;
constexpr float ZZ_BLATANT_AVOID_CURSOR_RAY_SCORE = 0.35f;
constexpr float ZZ_BLATANT_AVOID_CURSOR_PROGRESS_SCORE = 0.34f;
constexpr float ZZ_BLATANT_AVOID_PANIC_CURSOR_PROGRESS_SCORE = 0.50f;
constexpr float ZZ_BLATANT_AVOID_CURSOR_SIDE_SCORE = 0.05f;
constexpr float ZZ_BLATANT_AVOID_ESCAPE_PROGRESS_SCORE = 0.11f;
constexpr float ZZ_BLATANT_AVOID_PANIC_ESCAPE_PROGRESS_SCORE = 0.18f;
constexpr float ZZ_BLATANT_AVOID_BACKTRACK_SCORE = 1.45f;
constexpr float ZZ_BLATANT_AVOID_CORRIDOR_AXIS_SCORE = 0.18f;
constexpr float ZZ_BLATANT_AVOID_MANUAL_DIRECTION_SCORE = 0.18f;
constexpr float ZZ_BLATANT_AVOID_MANUAL_JUMP_SCORE = 0.32f;
constexpr float ZZ_BLATANT_AVOID_AUTO_JUMP_SCORE = 0.9f;
constexpr float ZZ_BLATANT_AVOID_FALLBACK_DIST_IMPROVEMENT = 12.0f;
constexpr float ZZ_BLATANT_AVOID_HOOK_MOVE_SCORE_FACTOR = 0.7f;
constexpr int ZZ_BLATANT_AVOID_HOOK_INPUT_VARIANTS = 3;
constexpr int ZZ_BLATANT_AVOID_PANIC_HOOK_INPUT_VARIANTS = 6;
constexpr float ZZ_BLATANT_AVOID_PANIC_RELEASE_SCORE = -0.9f;
constexpr float ZZ_BLATANT_AVOID_REHOOK_DIR_SCORE = 96.0f;
constexpr float ZZ_BLATANT_AVOID_REHOOK_PROGRESS_SCORE = 0.18f;
constexpr int ZZ_BLATANT_AVOID_REHOOK_MAX_CANDIDATES = 40;
constexpr int ZZ_BLATANT_AVOID_TAKEOVER_TICKS = 4;
constexpr int ZZ_BLATANT_AVOID_TAKEOVER_MAX_CANDIDATES = 56;
constexpr int ZZ_BLATANT_AVOID_TAKEOVER_HOOK_INPUT_VARIANTS = 8;
constexpr float ZZ_BLATANT_AVOID_TAKEOVER_RELEASE_SCORE = -1.2f;
constexpr int ZZ_BLATANT_AVOID_ONE_TILE_MAX_CANDIDATES = 96;
constexpr int ZZ_BLATANT_AVOID_ONE_TILE_HOOK_INPUT_VARIANTS = 14;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_RELEASE_SCORE = -2.2f;
constexpr float ZZ_BLATANT_AVOID_FALL_RESCUE_SPEED = 7.0f;
constexpr float ZZ_BLATANT_AVOID_FALL_TRAVEL_SCORE = 0.16f;
constexpr float ZZ_BLATANT_AVOID_REHOOK_LOWER_BLOCK_SCORE = 0.18f;
constexpr float ZZ_BLATANT_AVOID_HOOK_STABILITY_SCORE = 10.0f;
constexpr float ZZ_BLATANT_AVOID_NEW_HOOK_SAFE_PENALTY = 6.0f;
constexpr float ZZ_BLATANT_AVOID_HOOK_REPLACE_MARGIN = 14.0f;
constexpr float ZZ_BLATANT_AVOID_KEEP_HOOK_MARGIN = 6.0f;
constexpr float ZZ_BLATANT_AVOID_KEEP_HOOK_SAFE_BONUS = 2.6f;
constexpr float ZZ_BLATANT_AVOID_HOOK_CURSOR_VERTICAL_SCORE = 0.12f;
constexpr float ZZ_BLATANT_AVOID_HOOK_CURSOR_LOCK_PENALTY = 18.0f;
constexpr float ZZ_BLATANT_AVOID_HOOK_REVERSE_PENALTY = 24.0f;
constexpr int ZZ_BLATANT_AVOID_HOOK_COMMIT_TICKS = 6;
constexpr float ZZ_BLATANT_AVOID_SAFE_CURSOR_BACKTRACK_DIST = 6.0f;
constexpr float ZZ_BLATANT_AVOID_SAFE_CURSOR_ESCAPE_DIST = 18.0f;
constexpr float ZZ_BLATANT_AVOID_SAFE_CURSOR_HOOK_MIN_DOT = 0.12f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_CURSOR_PROGRESS_SCORE = 0.46f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_BACKTRACK_SCORE = 1.15f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_SAFE_CURSOR_BACKTRACK_DIST = 8.0f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_SAFE_CURSOR_ESCAPE_DIST = 12.0f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_HOOK_CURSOR_MIN_DOT = -0.08f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_OPPOSITE_HOOK_ESCAPE_DOT = 0.55f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_OPPOSITE_CURSOR_PROGRESS = -3.0f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_REHOOK_ESCAPE_DOT = 0.82f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_REHOOK_ESCAPE_PROGRESS = 14.0f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_REHOOK_REVERSE_MARGIN = 0.18f;
constexpr float ZZ_BLATANT_AVOID_ONE_TILE_VERTICAL_HOOK_MARGIN = 8.0f;
constexpr int ZZ_BLATANT_AVOID_LAST_JUMP_CHECK_TICKS = 22;
constexpr int ZZ_BLATANT_AVOID_JUMP_CHECK_TICKS = 5;
constexpr int ZZ_BLATANT_AVOID_PHASE_TICKS = 4;
constexpr float ZZ_BLATANT_AVOID_FOLLOWUP_SCORE_FACTOR = 0.55f;
constexpr int ZZ_BLATANT_AVOID_PHASE_BUDGET_SAFE = 2;
constexpr int ZZ_BLATANT_AVOID_PHASE_BUDGET_TAKEOVER = 6;
constexpr int ZZ_BLATANT_AVOID_PHASE_DANGER_TICKS = 6;

struct SZzAvoidHookCandidate
{
	vec2 m_HookPos;
	float m_Score;

	bool operator<(const SZzAvoidHookCandidate &Other) const
	{
		return m_Score < Other.m_Score;
	}
};

struct SZzAvoidInputCandidate
{
	CNetObj_PlayerInput m_Input;
	float m_Score;

	bool operator<(const SZzAvoidInputCandidate &Other) const
	{
		return m_Score < Other.m_Score;
	}
};

struct SZzAvoidFallback
{
	bool m_Valid = false;
	CNetObj_PlayerInput m_Input{};
	float m_Score = 0.0f;
	int m_DangerTick = -1;
	float m_DangerDist = 0.0f;
};

bool ZzAvoidIsFrozenState(const CCharacter *pCharacter)
{
	return !pCharacter || pCharacter->m_FreezeTime > 0 || pCharacter->Core()->m_FreezeEnd != 0 || pCharacter->Core()->m_IsInFreeze || pCharacter->Core()->m_DeepFrozen;
}

bool ZzAvoidIsDangerTile(int Tile)
{
	return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE || Tile == TILE_DEATH;
}

vec2 ZzAvoidNormalizedOrZero(vec2 Dir)
{
	const float Len = length(Dir);
	if(Len < 0.001f)
		return vec2(0.0f, 0.0f);
	return Dir / Len;
}

float ZzAvoidCursorWeight(vec2 CursorDir, vec2 DangerDir)
{
	if(length(CursorDir) < 0.001f)
		return 0.0f;

	const float CursorDangerDot = dot(CursorDir, DangerDir);
	if(CursorDangerDot <= ZZ_BLATANT_AVOID_CURSOR_DANGER_SOFT_DOT)
		return 1.0f;
	if(CursorDangerDot >= ZZ_BLATANT_AVOID_CURSOR_DANGER_HARD_DOT)
		return 0.18f;

	const float T = (CursorDangerDot - ZZ_BLATANT_AVOID_CURSOR_DANGER_SOFT_DOT) / (ZZ_BLATANT_AVOID_CURSOR_DANGER_HARD_DOT - ZZ_BLATANT_AVOID_CURSOR_DANGER_SOFT_DOT);
	return mix(1.0f, 0.18f, T);
}

vec2 ZzAvoidPreferredCursorDir(vec2 CursorDir, vec2 DangerDir)
{
	if(length(CursorDir) < 0.001f)
		return vec2(0.0f, 0.0f);

	const float CursorDangerDot = maximum(0.0f, dot(CursorDir, DangerDir));
	if(CursorDangerDot <= ZZ_BLATANT_AVOID_CURSOR_DANGER_SOFT_DOT)
		return CursorDir;

	const vec2 SideDir = CursorDir - DangerDir * CursorDangerDot;
	return ZzAvoidNormalizedOrZero(SideDir);
}

bool ZzAvoidIsDangerIndex(const CCollision *pCollision, int Index)
{
	if(!pCollision || Index < 0)
		return false;

	if(ZzAvoidIsDangerTile(pCollision->GetTileIndex(Index)) ||
		ZzAvoidIsDangerTile(pCollision->GetFrontTileIndex(Index)) ||
		ZzAvoidIsDangerTile(pCollision->GetSwitchType(Index)))
	{
		return true;
	}

	return pCollision->IsTeleport(Index) != 0 ||
		pCollision->IsEvilTeleport(Index) != 0 ||
		pCollision->IsCheckTeleport(Index) ||
		pCollision->IsCheckEvilTeleport(Index);
}

bool ZzAvoidProbeDanger(const CCollision *pCollision, vec2 Pos, vec2 Offset)
{
	if(!pCollision)
		return false;

	for(const int Index : pCollision->GetMapIndices(Pos, Pos + Offset))
	{
		if(ZzAvoidIsDangerIndex(pCollision, Index))
			return true;
	}

	return false;
}

bool ZzAvoidFindDangerOnPath(const CCollision *pCollision, vec2 PrevPos, vec2 CurrentPos, vec2 *pDangerPos = nullptr)
{
	if(!pCollision)
		return false;

	for(const int Index : pCollision->GetMapIndices(PrevPos, CurrentPos))
	{
		if(ZzAvoidIsDangerIndex(pCollision, Index))
		{
			if(pDangerPos)
				*pDangerPos = pCollision->GetPos(Index);
			return true;
		}
	}

	return false;
}

float ZzAvoidInputScore(int Direction, bool Jump, vec2 DangerDir, vec2 CursorDir, float CursorWeight, int BaseDirection, bool BaseJump)
{
	vec2 MoveDir((float)Direction, Jump ? -1.0f : 0.0f);
	const float MoveLen = length(MoveDir);
	float Score = Jump ? ZZ_BLATANT_AVOID_AUTO_JUMP_SCORE : 0.0f;

	if(MoveLen < 0.001f)
	{
		Score += 0.25f;
	}
	else
	{
		const vec2 MoveDirNormalized = MoveDir / MoveLen;
		Score += 1.0f - dot(MoveDirNormalized, -DangerDir);
		if(length(CursorDir) > 0.001f)
			Score -= dot(MoveDirNormalized, CursorDir) * ZZ_BLATANT_AVOID_CURSOR_INPUT_SCORE;
	}

	if(Direction != BaseDirection)
		Score += ZZ_BLATANT_AVOID_MANUAL_DIRECTION_SCORE;
	if(Jump != BaseJump)
		Score += ZZ_BLATANT_AVOID_MANUAL_JUMP_SCORE;

	return Score;
}

float ZzAvoidHookScore(vec2 StartPos, vec2 HookPos, vec2 DangerDir, vec2 CursorDir, float CursorWeight)
{
	const float HookDist = distance(StartPos, HookPos);
	const vec2 HookDir = normalize(HookPos - StartPos);

	float Score = HookDist - dot(HookDir, -DangerDir) * ZZ_BLATANT_AVOID_DIRECTION_SCORE;
	if(length(CursorDir) > 0.001f)
	{
		Score -= dot(HookDir, CursorDir) * ZZ_BLATANT_AVOID_CURSOR_HOOK_SCORE;
		const vec2 ToHook = HookPos - StartPos;
		const float CursorProjection = maximum(0.0f, dot(ToHook, CursorDir));
		Score += distance(ToHook, CursorDir * CursorProjection) * ZZ_BLATANT_AVOID_CURSOR_RAY_SCORE;
	}
	if(dot(HookDir, DangerDir) > 0.97f)
		Score += ZZ_BLATANT_AVOID_DIRECTION_SCORE;

	return Score;
}

bool ZzAvoidIsFallbackImprovement(int BaseDangerTick, float BaseDangerDist, int CandidateDangerTick, float CandidateDangerDist)
{
	return CandidateDangerTick > BaseDangerTick ||
		(CandidateDangerTick == BaseDangerTick && CandidateDangerDist > BaseDangerDist + ZZ_BLATANT_AVOID_FALLBACK_DIST_IMPROVEMENT);
}

bool ZzAvoidIsBetterFallback(int CandidateDangerTick, float CandidateDangerDist, float CandidateScore, const SZzAvoidFallback &BestFallback)
{
	if(!BestFallback.m_Valid)
		return true;
	if(CandidateDangerTick != BestFallback.m_DangerTick)
		return CandidateDangerTick > BestFallback.m_DangerTick;
	if(std::abs(CandidateDangerDist - BestFallback.m_DangerDist) > 0.001f)
		return CandidateDangerDist > BestFallback.m_DangerDist;
	return CandidateScore < BestFallback.m_Score;
}

bool SimulateZzBlatantAvoid(CGameWorld *pSourceWorld, int ClientId, const CNetObj_PlayerInput &BaseInput, int Ticks, const vec2 *pHookPos, int *pFirstDangerTick = nullptr, vec2 *pFirstDangerPos = nullptr, vec2 *pFinalPos = nullptr)
{
	CGameWorld FutureWorld;
	FutureWorld.CopyWorld(pSourceWorld);
	const CCollision *pCollision = FutureWorld.Collision();

	for(int Tick = 0; Tick < Ticks; Tick++)
	{
		CCharacter *pCharacter = FutureWorld.GetCharacterById(ClientId);
		if(ZzAvoidIsFrozenState(pCharacter))
		{
			if(pFirstDangerTick)
				*pFirstDangerTick = Tick;
			if(pFirstDangerPos)
				*pFirstDangerPos = pCharacter ? pCharacter->GetPos() : vec2(0.0f, 0.0f);
			if(pFinalPos)
				*pFinalPos = pCharacter ? pCharacter->GetPos() : vec2(0.0f, 0.0f);
			return true;
		}

		const vec2 PreviousPos = pCharacter->GetPos();
		if(ZzAvoidFindDangerOnPath(pCollision, PreviousPos, PreviousPos, pFirstDangerPos))
		{
			if(pFirstDangerTick)
				*pFirstDangerTick = Tick;
			if(pFinalPos)
				*pFinalPos = PreviousPos;
			return true;
		}

		CNetObj_PlayerInput Input = BaseInput;
		if(pHookPos)
		{
			const vec2 Aim = *pHookPos - pCharacter->GetPos();
			Input.m_Hook = 1;
			Input.m_TargetX = round_to_int(Aim.x);
			Input.m_TargetY = round_to_int(Aim.y);
			if(Input.m_TargetX == 0 && Input.m_TargetY == 0)
				Input.m_TargetX = 1;
		}

		pCharacter->OnDirectInput(&Input);
		FutureWorld.m_GameTick++;
		pCharacter->OnPredictedInput(&Input);
		FutureWorld.Tick();

		pCharacter = FutureWorld.GetCharacterById(ClientId);
		if(ZzAvoidIsFrozenState(pCharacter))
		{
			if(pFirstDangerTick)
				*pFirstDangerTick = Tick;
			if(pFirstDangerPos)
				*pFirstDangerPos = pCharacter ? pCharacter->GetPos() : vec2(0.0f, 0.0f);
			if(pFinalPos)
				*pFinalPos = pCharacter ? pCharacter->GetPos() : PreviousPos;
			return true;
		}
		if(!pCharacter)
			break;
		if(ZzAvoidFindDangerOnPath(pCollision, PreviousPos, pCharacter->GetPos(), pFirstDangerPos))
		{
			if(pFirstDangerTick)
				*pFirstDangerTick = Tick;
			if(pFinalPos)
				*pFinalPos = pCharacter->GetPos();
			return true;
		}
	}

	if(pFinalPos)
	{
		CCharacter *pCharacter = FutureWorld.GetCharacterById(ClientId);
		*pFinalPos = pCharacter ? pCharacter->GetPos() : vec2(0.0f, 0.0f);
	}

	return false;
}

bool BuildZzAvoidFutureWorld(CGameWorld *pSourceWorld, int ClientId, const CNetObj_PlayerInput &BaseInput, int Ticks, const vec2 *pHookPos, CGameWorld *pOutWorld, vec2 *pFinalPos = nullptr)
{
	if(!pSourceWorld || !pOutWorld || Ticks < 0)
		return false;

	CGameWorld FutureWorld;
	FutureWorld.CopyWorld(pSourceWorld);
	const CCollision *pCollision = FutureWorld.Collision();

	for(int Tick = 0; Tick < Ticks; Tick++)
	{
		CCharacter *pCharacter = FutureWorld.GetCharacterById(ClientId);
		if(ZzAvoidIsFrozenState(pCharacter))
			return false;

		const vec2 PreviousPos = pCharacter->GetPos();
		if(ZzAvoidFindDangerOnPath(pCollision, PreviousPos, PreviousPos))
			return false;

		CNetObj_PlayerInput Input = BaseInput;
		if(pHookPos)
		{
			const vec2 Aim = *pHookPos - pCharacter->GetPos();
			Input.m_Hook = 1;
			Input.m_TargetX = round_to_int(Aim.x);
			Input.m_TargetY = round_to_int(Aim.y);
			if(Input.m_TargetX == 0 && Input.m_TargetY == 0)
				Input.m_TargetX = 1;
		}

		pCharacter->OnDirectInput(&Input);
		FutureWorld.m_GameTick++;
		pCharacter->OnPredictedInput(&Input);
		FutureWorld.Tick();

		pCharacter = FutureWorld.GetCharacterById(ClientId);
		if(ZzAvoidIsFrozenState(pCharacter))
			return false;
		if(!pCharacter)
			return false;
		if(ZzAvoidFindDangerOnPath(pCollision, PreviousPos, pCharacter->GetPos()))
			return false;
	}

	pOutWorld->CopyWorld(&FutureWorld);
	if(pFinalPos)
	{
		CCharacter *pCharacter = FutureWorld.GetCharacterById(ClientId);
		*pFinalPos = pCharacter ? pCharacter->GetPos() : vec2(0.0f, 0.0f);
	}
	return true;
}

bool TryApplyZzBlatantAvoid(CGameClient *pGameClient, const CNetObj_PlayerInput &BaseInput, CNetObj_PlayerInput &OutputInput)
{
	if(!g_Config.m_ClZzAvoidEnabled || g_Config.m_ClZzAvoidMode != 1 || pGameClient->m_Snap.m_LocalClientId < 0 || !pGameClient->Collision())
		return false;

	CGameWorld *pSourceWorld = pGameClient->Predict() ? &pGameClient->m_PredictedWorld : &pGameClient->m_GameWorld;
	CCharacter *pCharacter = pSourceWorld->GetCharacterById(pGameClient->m_Snap.m_LocalClientId);
	if(!pCharacter || ZzAvoidIsFrozenState(pCharacter))
		return false;

	const int LookAheadTicks = std::clamp(ZZ_BLATANT_AVOID_BASE_TICKS + round_to_int(length(pCharacter->Core()->m_Vel) / 4.0f), ZZ_BLATANT_AVOID_BASE_TICKS, ZZ_BLATANT_AVOID_MAX_TICKS);
	const vec2 StartPos = pCharacter->GetPos();
	const bool Grounded = pCharacter->IsGrounded();
	const int AvailableJumps = pCharacter->Core()->m_Jumps;
	const int JumpedFlags = pCharacter->Core()->m_Jumped;
	const bool HasAirJumpAvailable = !Grounded && !(JumpedFlags & 2);
	const bool LastJumpState = (Grounded && AvailableJumps == 1) || (HasAirJumpAvailable && AvailableJumps <= 2);
	const float CeilingProbeDist = pCharacter->GetProximityRadius() + 24.0f;
	const bool DangerAboveImmediate = ZzAvoidProbeDanger(pGameClient->Collision(), StartPos, vec2(0.0f, -CeilingProbeDist));

	if(BaseInput.m_Jump != 0 && LastJumpState)
	{
		CNetObj_PlayerInput NoJumpInput = BaseInput;
		NoJumpInput.m_Jump = 0;

		int JumpDangerTick = -1;
		int NoJumpDangerTick = -1;
		vec2 JumpDangerPos(0.0f, 0.0f);
		vec2 NoJumpDangerPos(0.0f, 0.0f);
		const int LastJumpCheckTicks = minimum(ZZ_BLATANT_AVOID_LAST_JUMP_CHECK_TICKS, maximum(LookAheadTicks, ZZ_BLATANT_AVOID_JUMP_CHECK_TICKS + 4));
		const bool JumpDanger = SimulateZzBlatantAvoid(pSourceWorld, pGameClient->m_Snap.m_LocalClientId, BaseInput, LastJumpCheckTicks, nullptr, &JumpDangerTick, &JumpDangerPos);
		const bool NoJumpDanger = SimulateZzBlatantAvoid(pSourceWorld, pGameClient->m_Snap.m_LocalClientId, NoJumpInput, LastJumpCheckTicks, nullptr, &NoJumpDangerTick, &NoJumpDangerPos);
		const float JumpDangerDist = distance(StartPos, JumpDangerPos);
		const float NoJumpDangerDist = distance(StartPos, NoJumpDangerPos);

		if(JumpDanger && (!NoJumpDanger || ZzAvoidIsFallbackImprovement(JumpDangerTick, JumpDangerDist, NoJumpDangerTick, NoJumpDangerDist)))
		{
			OutputInput = NoJumpInput;
			return true;
		}
	}

	if(BaseInput.m_Jump != 0 && DangerAboveImmediate)
	{
		CNetObj_PlayerInput NoJumpInput = BaseInput;
		NoJumpInput.m_Jump = 0;

		int JumpDangerTick = -1;
		int NoJumpDangerTick = -1;
		vec2 JumpDangerPos(0.0f, 0.0f);
		vec2 NoJumpDangerPos(0.0f, 0.0f);
		const int CeilingJumpCheckTicks = minimum(ZZ_BLATANT_AVOID_LAST_JUMP_CHECK_TICKS, maximum(LookAheadTicks, ZZ_BLATANT_AVOID_JUMP_CHECK_TICKS + 6));
		const bool JumpDanger = SimulateZzBlatantAvoid(pSourceWorld, pGameClient->m_Snap.m_LocalClientId, BaseInput, CeilingJumpCheckTicks, nullptr, &JumpDangerTick, &JumpDangerPos);
		const bool NoJumpDanger = SimulateZzBlatantAvoid(pSourceWorld, pGameClient->m_Snap.m_LocalClientId, NoJumpInput, CeilingJumpCheckTicks, nullptr, &NoJumpDangerTick, &NoJumpDangerPos);
		const float JumpDangerDist = distance(StartPos, JumpDangerPos);
		const float NoJumpDangerDist = distance(StartPos, NoJumpDangerPos);

		if(JumpDanger && (!NoJumpDanger || ZzAvoidIsFallbackImprovement(JumpDangerTick, JumpDangerDist, NoJumpDangerTick, NoJumpDangerDist)))
		{
			OutputInput = NoJumpInput;
			return true;
		}
	}

	int DangerTick = -1;
	vec2 FirstDangerPos(0.0f, 0.0f);
	if(!SimulateZzBlatantAvoid(pSourceWorld, pGameClient->m_Snap.m_LocalClientId, BaseInput, LookAheadTicks, nullptr, &DangerTick, &FirstDangerPos))
		return false;

	vec2 DangerDir = pCharacter->Core()->m_Vel;
	if(length(DangerDir) < 0.001f)
		DangerDir = FirstDangerPos - pCharacter->GetPos();
	const float DangerLen = length(DangerDir);
	if(DangerLen < 0.001f)
		return false;
	DangerDir /= DangerLen;
	const vec2 CursorDir = ZzAvoidNormalizedOrZero(vec2((float)BaseInput.m_TargetX, (float)BaseInput.m_TargetY));
	const float BaseDangerDist = distance(StartPos, FirstDangerPos);
	const bool OneTileMode = DangerTick <= ZZ_BLATANT_AVOID_ONE_TILE_TICKS || BaseDangerDist <= ZZ_BLATANT_AVOID_ONE_TILE_DIST;
	const bool HasCurrentHook = BaseInput.m_Hook != 0 && (pCharacter->Core()->m_HookState == HOOK_FLYING || pCharacter->Core()->m_HookState == HOOK_GRABBED) &&
		distance(StartPos, pCharacter->Core()->m_HookPos) > 1.0f;
	const vec2 CurrentHookDir = HasCurrentHook ? normalize(pCharacter->Core()->m_HookPos - StartPos) : vec2(0.0f, 0.0f);
	const bool CurrentHookIntoDanger = HasCurrentHook && dot(CurrentHookDir, DangerDir) > 0.2f;
	const float TriggerDist = minimum(ZZ_BLATANT_AVOID_TRIGGER_MAX_DIST, maximum(ZZ_BLATANT_AVOID_TRIGGER_DIST, length(pCharacter->Core()->m_Vel) * 1.75f));
	if(DangerTick > ZZ_BLATANT_AVOID_TRIGGER_TICKS && distance(StartPos, FirstDangerPos) > TriggerDist)
		return false;

	const int SimTicks = std::clamp(DangerTick + ZZ_BLATANT_AVOID_VALIDATE_EXTRA_TICKS, ZZ_BLATANT_AVOID_MIN_TICKS, ZZ_BLATANT_AVOID_MAX_TICKS);
	CNetObj_PlayerInput WorkingInput = BaseInput;
	bool HasSafeCandidate = false;
	CNetObj_PlayerInput BestSafeInput = BaseInput;
	float BestSafeScore = 0.0f;
	bool BestSafeUsesNewHook = false;
	bool BestSafeKeepsCurrentHook = false;
	SZzAvoidFallback BestFallback;

	auto TryStoreSafeCandidate = [&](const CNetObj_PlayerInput &FinalInput, float CandidateScore, bool UsesNewHook, bool KeepsCurrentHook) {
		if(!HasSafeCandidate)
		{
			HasSafeCandidate = true;
			BestSafeInput = FinalInput;
			BestSafeScore = CandidateScore;
			BestSafeUsesNewHook = UsesNewHook;
			BestSafeKeepsCurrentHook = KeepsCurrentHook;
			return true;
		}

		if(OneTileMode && !CurrentHookIntoDanger)
		{
			if(!UsesNewHook && BestSafeUsesNewHook && CandidateScore <= BestSafeScore + 6.0f)
			{
				BestSafeInput = FinalInput;
				BestSafeScore = CandidateScore;
				BestSafeUsesNewHook = false;
				BestSafeKeepsCurrentHook = KeepsCurrentHook;
				return true;
			}
			if(KeepsCurrentHook && !BestSafeKeepsCurrentHook && CandidateScore <= BestSafeScore + 4.0f)
			{
				BestSafeInput = FinalInput;
				BestSafeScore = CandidateScore;
				BestSafeUsesNewHook = UsesNewHook;
				BestSafeKeepsCurrentHook = true;
				return true;
			}
			if(!KeepsCurrentHook && BestSafeKeepsCurrentHook && CandidateScore >= BestSafeScore - 4.0f)
				return false;
		}

		if(CandidateScore < BestSafeScore)
		{
			BestSafeInput = FinalInput;
			BestSafeScore = CandidateScore;
			BestSafeUsesNewHook = UsesNewHook;
			BestSafeKeepsCurrentHook = KeepsCurrentHook;
			return true;
		}
		return false;
	};

	auto RegisterCandidate = [&](const CNetObj_PlayerInput &CandidateInput, float CandidateScore, const vec2 *pHookPos = nullptr) {
		int CandidateDangerTick = -1;
		vec2 CandidateDangerPos(0.0f, 0.0f);
		vec2 CandidateFinalPos = StartPos;
		const bool HasDanger = SimulateZzBlatantAvoid(pSourceWorld, pGameClient->m_Snap.m_LocalClientId, CandidateInput, SimTicks, pHookPos, &CandidateDangerTick, &CandidateDangerPos, &CandidateFinalPos);

		CNetObj_PlayerInput FinalInput = CandidateInput;
		if(pHookPos)
		{
			FinalInput.m_Hook = 1;
			const vec2 Aim = *pHookPos - StartPos;
			FinalInput.m_TargetX = round_to_int(Aim.x);
			FinalInput.m_TargetY = round_to_int(Aim.y);
			if(FinalInput.m_TargetX == 0 && FinalInput.m_TargetY == 0)
				FinalInput.m_TargetX = 1;
		}
		const bool UsesNewHook = pHookPos != nullptr;
		const bool KeepsCurrentHook = !UsesNewHook && HasCurrentHook && CandidateInput.m_Hook != 0;
		float AdjustedScore = CandidateScore;
		if(OneTileMode)
		{
			const vec2 Travel = CandidateFinalPos - StartPos;
			const float EscapeProgress = dot(Travel, -DangerDir);
			AdjustedScore -= EscapeProgress * (ZZ_BLATANT_AVOID_ESCAPE_PROGRESS_SCORE * 1.7f);

			if(length(CursorDir) > 0.001f)
			{
				const float CursorProgress = dot(Travel, CursorDir);
				AdjustedScore -= CursorProgress * (ZZ_BLATANT_AVOID_ONE_TILE_CURSOR_PROGRESS_SCORE * 0.85f);
				if(CursorProgress < 0.0f && EscapeProgress < 20.0f)
					AdjustedScore += -CursorProgress * ZZ_BLATANT_AVOID_ONE_TILE_BACKTRACK_SCORE;
			}

			if(UsesNewHook && !CurrentHookIntoDanger)
				AdjustedScore += ZZ_BLATANT_AVOID_NEW_HOOK_SAFE_PENALTY * 0.35f;
			if(KeepsCurrentHook && !CurrentHookIntoDanger)
				AdjustedScore -= ZZ_BLATANT_AVOID_KEEP_HOOK_SAFE_BONUS * 0.55f;
		}

		if(!HasDanger)
		{
			TryStoreSafeCandidate(FinalInput, AdjustedScore, UsesNewHook, KeepsCurrentHook);
			return;
		}

		const float CandidateDangerDist = distance(StartPos, CandidateDangerPos);
		if(!ZzAvoidIsFallbackImprovement(DangerTick, BaseDangerDist, CandidateDangerTick, CandidateDangerDist))
			return;

		if(ZzAvoidIsBetterFallback(CandidateDangerTick, CandidateDangerDist, AdjustedScore, BestFallback))
		{
			BestFallback.m_Valid = true;
			BestFallback.m_Input = FinalInput;
			BestFallback.m_Score = AdjustedScore;
			BestFallback.m_DangerTick = CandidateDangerTick;
			BestFallback.m_DangerDist = CandidateDangerDist;
		}
	};

	if(BaseInput.m_Hook != 0)
	{
		RegisterCandidate(BaseInput, CurrentHookIntoDanger ? 0.0f : -0.35f);
		CNetObj_PlayerInput ReleaseInput = BaseInput;
		ReleaseInput.m_Hook = 0;
		RegisterCandidate(ReleaseInput, OneTileMode ? -0.35f : -0.25f);
		if(CurrentHookIntoDanger)
			WorkingInput = ReleaseInput;
	}

	std::vector<SZzAvoidInputCandidate> vInputCandidates;
	vInputCandidates.reserve(6);

	auto AddInputCandidate = [&](int Direction, int Jump)
	{
		CNetObj_PlayerInput Candidate = WorkingInput;
		Candidate.m_Direction = Direction;
		Candidate.m_Jump = Jump;

		if(Jump != 0)
		{
			CNetObj_PlayerInput NonJumpCandidate = Candidate;
			NonJumpCandidate.m_Jump = 0;
			int JumpDangerTick = -1;
			int NonJumpDangerTick = -1;
			vec2 DummyPos(0.0f, 0.0f);
			const int JumpCheckTicks = LastJumpState ? SimTicks : minimum(SimTicks, maximum(ZZ_BLATANT_AVOID_JUMP_CHECK_TICKS, DangerTick + 2));
			const bool JumpDanger = SimulateZzBlatantAvoid(pSourceWorld, pGameClient->m_Snap.m_LocalClientId, Candidate, JumpCheckTicks, nullptr, &JumpDangerTick, &DummyPos);
			const bool NonJumpDanger = SimulateZzBlatantAvoid(pSourceWorld, pGameClient->m_Snap.m_LocalClientId, NonJumpCandidate, JumpCheckTicks, nullptr, &NonJumpDangerTick, &DummyPos);
			if(JumpDanger && (LastJumpState || !NonJumpDanger || JumpDangerTick <= NonJumpDangerTick + 1))
				return;
		}

		if(Candidate.m_Direction == WorkingInput.m_Direction && Candidate.m_Jump == WorkingInput.m_Jump)
			return;

		for(const SZzAvoidInputCandidate &Existing : vInputCandidates)
		{
			if(Existing.m_Input.m_Direction == Candidate.m_Direction && Existing.m_Input.m_Jump == Candidate.m_Jump)
				return;
		}

		vInputCandidates.push_back({Candidate, ZzAvoidInputScore(Direction, Jump != 0, DangerDir, CursorDir, 1.0f, BaseInput.m_Direction, BaseInput.m_Jump != 0)});
	};

	AddInputCandidate(0, 0);
	AddInputCandidate(-1, 0);
	AddInputCandidate(1, 0);
	const bool ShouldTryJump = BaseInput.m_Jump != 0 || pCharacter->Core()->m_Vel.y > 4.0f || DangerDir.y > 0.25f;
	if(ShouldTryJump)
	{
		AddInputCandidate(0, 1);
		AddInputCandidate(-1, 1);
		AddInputCandidate(1, 1);
	}

	std::sort(vInputCandidates.begin(), vInputCandidates.end());

	for(const SZzAvoidInputCandidate &Candidate : vInputCandidates)
		RegisterCandidate(Candidate.m_Input, Candidate.m_Score);

	if(HasSafeCandidate && OneTileMode && (!BestSafeUsesNewHook || BestSafeKeepsCurrentHook))
	{
		OutputInput = BestSafeInput;
		return true;
	}

	const float HookLength = pCharacter->GetTuning(pCharacter->GetOverriddenTuneZone())->m_HookLength;
	const int TileRadius = round_to_int(std::ceil((HookLength + 32.0f) / 32.0f));
	const int CenterX = round_to_int(StartPos.x) / 32;
	const int CenterY = round_to_int(StartPos.y) / 32;
	const int Width = pGameClient->Collision()->GetWidth();
	const int Height = pGameClient->Collision()->GetHeight();

	std::vector<SZzAvoidHookCandidate> vHookCandidates;
	vHookCandidates.reserve((TileRadius * 2 + 1) * (TileRadius * 2 + 1));

	for(int y = maximum(0, CenterY - TileRadius); y <= minimum(Height - 1, CenterY + TileRadius); y++)
	{
		for(int x = maximum(0, CenterX - TileRadius); x <= minimum(Width - 1, CenterX + TileRadius); x++)
		{
			const vec2 TilePos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
			if(distance(StartPos, TilePos) > HookLength + 32.0f)
				continue;

			vec2 HookPos;
			vec2 BeforeCollision;
			const int Hit = pGameClient->Collision()->IntersectLineTeleHook(StartPos, TilePos, &HookPos, &BeforeCollision);
			if(Hit == 0 || Hit == TILE_NOHOOK)
				continue;

			const float HookDist = distance(StartPos, HookPos);
			const float MinHookDist = OneTileMode ? ZZ_BLATANT_AVOID_ONE_TILE_MIN_HOOK_DIST : ZZ_BLATANT_AVOID_MIN_HOOK_DIST;
			if(HookDist < MinHookDist || HookDist > HookLength + 4.0f)
				continue;

			const vec2 ToHookDir = normalize(HookPos - StartPos);
			if(OneTileMode && length(CursorDir) > 0.001f)
			{
				const float HookCursorDot = dot(ToHookDir, CursorDir);
				const float HookEscapeDot = dot(ToHookDir, -DangerDir);
				if(HookCursorDot < -0.22f && HookEscapeDot < 0.45f)
					continue;
				if(CursorDir.y < -0.15f && HookPos.y > StartPos.y + 10.0f && HookEscapeDot < 0.58f)
					continue;
				if(CursorDir.y > 0.15f && HookPos.y < StartPos.y - 10.0f && HookEscapeDot < 0.58f)
					continue;
			}

			vHookCandidates.push_back({HookPos, ZzAvoidHookScore(StartPos, HookPos, DangerDir, CursorDir, 1.0f)});
		}
	}

	const bool AllowHookSearch = !Grounded || OneTileMode || CurrentHookIntoDanger || BaseInput.m_Hook != 0;
	if(AllowHookSearch && !vHookCandidates.empty())
	{
		std::sort(vHookCandidates.begin(), vHookCandidates.end());

		std::vector<vec2> vAcceptedHookPositions;
		const int MaxHookCandidates = OneTileMode ? 48 : ZZ_BLATANT_AVOID_MAX_CANDIDATES;
		vAcceptedHookPositions.reserve(MaxHookCandidates);

		for(const SZzAvoidHookCandidate &Candidate : vHookCandidates)
		{
			bool Duplicate = false;
			for(const vec2 &Existing : vAcceptedHookPositions)
			{
				if(distance(Existing, Candidate.m_HookPos) < ZZ_BLATANT_AVOID_DUPLICATE_DIST)
				{
					Duplicate = true;
					break;
				}
			}
			if(Duplicate)
				continue;

			vAcceptedHookPositions.push_back(Candidate.m_HookPos);
			if((int)vAcceptedHookPositions.size() >= MaxHookCandidates)
				break;

			RegisterCandidate(WorkingInput, Candidate.m_Score, &Candidate.m_HookPos);
			if(OneTileMode)
			{
				int HookVariants = 0;
				for(const SZzAvoidInputCandidate &InputCandidate : vInputCandidates)
				{
					CNetObj_PlayerInput HookMoveInput = InputCandidate.m_Input;
					HookMoveInput.m_Hook = 0;
					RegisterCandidate(HookMoveInput, Candidate.m_Score + InputCandidate.m_Score * 0.7f, &Candidate.m_HookPos);
					HookVariants++;
					if(HookVariants >= 4)
						break;
				}
			}
		}
	}

	if(HasSafeCandidate)
	{
		OutputInput = BestSafeInput;
		return true;
	}

	if(BestFallback.m_Valid)
	{
		OutputInput = BestFallback.m_Input;
		return true;
	}

	return false;
}
}

CControls::CControls()
{
	mem_zero(&m_aInputData, sizeof(m_aInputData));
	mem_zero(&m_aLastData, sizeof(m_aLastData));
	mem_zero(&m_aInputHookSpam, sizeof(m_aInputHookSpam));
	std::fill(std::begin(m_aMousePos), std::end(m_aMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMousePosOnAction), std::end(m_aMousePosOnAction), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aTargetPos), std::end(m_aTargetPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMouseInputType), std::end(m_aMouseInputType), EMouseInputType::ABSOLUTE);
}

void CControls::OnReset()
{
	ResetInput(0);
	ResetInput(1);
	m_DummyAutoUnfreeze.Reset();

	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;

	m_LastSendTime = 0;
	m_BotFireReleasePending = false;
	m_ZzAutoUnfreezeOverridesInput = false;
	m_UnfreezeFound = false;
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
	m_aInputHookSpam[Dummy] = 0;
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
		static CInputState s_State = {this, {&m_aInputHookSpam[0], &m_aInputHookSpam[1]}};
		Console()->Register("+zz_hookspam", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Activate hook spam while key is held");
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
		static CInputToggleOnce s_Toggle = {&g_Config.m_ClZzCompanionEnabled, false};
		static CInputToggle s_ToggleLegacy = {&g_Config.m_ClZzCompanionEnabled};
		Console()->Register("+zz_companion_toggle", "", CFGFLAG_CLIENT, ConKeyToggleOnce, &s_Toggle, "ZZetClient: toggle companion");
		Console()->Register("zz_companion_toggle", "", CFGFLAG_CLIENT, ConKeyToggle, &s_ToggleLegacy, "ZZetClient: toggle companion (legacy)");
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
	if(g_Config.m_ClZzCompanionToggleKey >= KEY_FIRST && g_Config.m_ClZzCompanionToggleKey < KEY_LAST)
		GameClient()->m_Binds.Bind(g_Config.m_ClZzCompanionToggleKey, "+zz_companion_toggle", false, g_Config.m_ClZzCompanionToggleMod);
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
	m_ZzAutoUnfreezeOverridesInput = false;

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
	CNetObj_PlayerInput InputToSend = m_aInputData[g_Config.m_ClDummy];

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
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;
		InputToSend = m_aInputData[g_Config.m_ClDummy];

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
		const bool LocalSpectating = LocalId >= 0 && LocalId < MAX_CLIENTS && GameClient()->m_aClients[LocalId].m_Spec;
		const bool LocalPaused = LocalId >= 0 && LocalId < MAX_CLIENTS && GameClient()->m_aClients[LocalId].m_Paused;
		const bool GamePaused = GameClient()->m_Snap.m_pGameInfoObj && (GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED);
		const bool EscapePressed = Input()->KeyPress(KEY_ESCAPE);
		const bool FakeLagBlocked = EscapePressed || GameClient()->m_Menus.IsActive() || GameClient()->m_Snap.m_SpecInfo.m_Active || LocalSpectating || LocalPaused || GamePaused;
		if(g_Config.m_ClZzFakeLag)
		{
			// Hard stop fake-lag carryover before any fake-lag input is injected.
			if(FakeLagBlocked)
			{
				s_aFakeLagTick[DummyIndex] = 0;
				m_aInputData[DummyIndex].m_Direction = 0;
			}
			else
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
					// Very aggressive fake-lag: alternate every tick for the fastest A/D spam.
					m_aInputData[DummyIndex].m_Direction = (Tick & 1) ? -BaseDir : BaseDir;
				}
				else
				{
					if(DriftMode)
					{
						m_aInputData[DummyIndex].m_Direction = (Tick & 1) ? 1 : -1;
					}
					else
					{
						// Standing fake-lag: also alternate immediately without pauses.
						m_aInputData[DummyIndex].m_Direction = (Tick & 1) ? 1 : -1;
					}
				}
			}
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

		DoSelfUnfreeze();

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

		InputToSend = m_aInputData[g_Config.m_ClDummy];
		if(g_Config.m_ClZzAvoidEnabled && g_Config.m_ClZzAvoidMode == 1)
		{
			CNetObj_PlayerInput BotBaseInput = m_aInputData[g_Config.m_ClDummy];
			InputToSend = BotBaseInput;
			TryApplyZzBlatantAvoid(GameClient(), BotBaseInput, InputToSend);
		}

		// check if we need to send input
		Send = Send || InputToSend.m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
		Send = Send || InputToSend.m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
		Send = Send || InputToSend.m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
		Send = Send || InputToSend.m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
		Send = Send || InputToSend.m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
		Send = Send || InputToSend.m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
		Send = Send || InputToSend.m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
		Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz

		// ZZetClient: while hammer assist is actively trying to hit (hammer + fire held), send at tickrate.
		if(g_Config.m_ClZzHammerAssistEnabled && GameClient()->m_Snap.m_pLocalCharacter &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_HAMMER && (InputToSend.m_Fire & 1))
		{
			const int TickSpeed = maximum(1, Client()->GameTickSpeed());
			Send = Send || time_get() > m_LastSendTime + time_freq() / TickSpeed;
		}
		// Keep hook aimbot responsive on every game tick instead of only the default 25 Hz send cadence.
		const bool HookTryingToAttach = GameClient()->m_Snap.m_pLocalCharacter &&
			(GameClient()->m_Snap.m_pLocalCharacter->m_HookState != HOOK_FLYING &&
				GameClient()->m_Snap.m_pLocalCharacter->m_HookState != HOOK_GRABBED);
		if(g_Config.m_ClZzAimbotEnabled && InputToSend.m_Hook && HookTryingToAttach && !m_ZzAutoUnfreezeOverridesInput)
		{
			const int TickSpeed = maximum(1, Client()->GameTickSpeed());
			Send = Send || time_get() > m_LastSendTime + time_freq() / TickSpeed;
		}
		Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (InputToSend.m_Direction || InputToSend.m_Jump || InputToSend.m_Hook));

	}

	const CNetObj_PlayerInput OldLastData = m_aLastData[g_Config.m_ClDummy];

	// copy and return size
	m_aLastData[g_Config.m_ClDummy] = InputToSend;

	if(!Send)
		return 0;

	if(g_Config.m_ClZzFngDebug)
	{
		static int64_t s_LastSendDbg = 0;
		const int64_t Now = time_get();
		if(s_LastSendDbg == 0 || Now - s_LastSendDbg >= time_freq())
		{
			s_LastSendDbg = Now;
			const CNetObj_PlayerInput &Cur = InputToSend;
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
	mem_copy(pData, &InputToSend, sizeof(m_aInputData[0]));
	return sizeof(m_aInputData[0]);
}

void CControls::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	static bool s_WasAutoUnfreezeEnabled = false;
	if(g_Config.m_ClZzAutoUnfreeze && !s_WasAutoUnfreezeEnabled)
		g_Config.m_ClZzAutoUnfreezeAutoFire = 1;
	s_WasAutoUnfreezeEnabled = g_Config.m_ClZzAutoUnfreeze != 0;

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

	RenderCompanionPath();
	RenderUnfreezePredict();
}

void CControls::DoSelfUnfreeze()
{
	const int DummyIndex = g_Config.m_ClDummy;
	const CNetObj_PlayerInput PrevMainInput = m_aInputData[DummyIndex];
	const CNetObj_PlayerInput PrevDummyInput = m_aInputData[!DummyIndex];
	const CNetObj_PlayerInput PrevDummyNetInput = GameClient()->m_DummyInput;

	GameClient()->RunDmdBot(
		m_aInputData[DummyIndex],
		m_aAmmoCount,
		m_LastUnfreezeShot,
		m_UnfreezeFound,
		m_UnfreezePos,
		m_UnfreezeHit,
		m_UnfreezeFuture,
		m_BotFireReleasePending);
	GameClient()->RunDmdHookSpam(m_aInputData[DummyIndex], m_aInputHookSpam[DummyIndex] != 0);
	Balance();
	m_DummyAutoUnfreeze.Tick(GameClient(), DummyIndex, m_aInputData[!DummyIndex], GameClient()->m_DummyInput);

	m_ZzAutoUnfreezeOverridesInput =
		mem_comp(&PrevMainInput, &m_aInputData[DummyIndex], sizeof(CNetObj_PlayerInput)) != 0 ||
		mem_comp(&PrevDummyInput, &m_aInputData[!DummyIndex], sizeof(CNetObj_PlayerInput)) != 0 ||
		mem_comp(&PrevDummyNetInput, &GameClient()->m_DummyInput, sizeof(CNetObj_PlayerInput)) != 0;
}

void CControls::RenderUnfreezePredict()
{
	if(!m_UnfreezeFound)
		return;

	Graphics()->TextureClear();
	Graphics()->LinesBegin();
	Graphics()->SetColor(0.0f, 1.0f, 0.0f, 1.0f);
	IGraphics::CLineItem L1(m_UnfreezePos.x, m_UnfreezePos.y, m_UnfreezeHit.x, m_UnfreezeHit.y);
	Graphics()->LinesDraw(&L1, 1);

	Graphics()->SetColor(1.0f, 0.0f, 0.0f, 1.0f);
	IGraphics::CLineItem L2(m_UnfreezeHit.x, m_UnfreezeHit.y, m_UnfreezeFuture.x, m_UnfreezeFuture.y);
	Graphics()->LinesDraw(&L2, 1);
	Graphics()->LinesEnd();

	if(time_get() > m_LastUnfreezeShot + time_freq() * 0.6f)
		m_UnfreezeFound = false;
}

void CControls::RenderCompanionPath()
{
	if(!g_Config.m_ClZzCompanionEnabled || !GameClient()->Client()->DummyConnected())
		return;

	const int PathLen = GameClient()->CompanionPathLen();
	if(PathLen < 2)
		return;

	Graphics()->TextureClear();
	Graphics()->LinesBegin();
	for(int i = 1; i < PathLen; i++)
	{
		const vec2 &From = GameClient()->CompanionPathPoint(i - 1);
		const vec2 &To = GameClient()->CompanionPathPoint(i);
		if(i == PathLen - 1)
			Graphics()->SetColor(1.0f, 0.85f, 0.15f, 0.95f);
		else
			Graphics()->SetColor(0.15f, 0.95f, 1.0f, 0.9f);
		IGraphics::CLineItem Line(From.x, From.y, To.x, To.y);
		Graphics()->LinesDraw(&Line, 1);
	}
	Graphics()->LinesEnd();
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

