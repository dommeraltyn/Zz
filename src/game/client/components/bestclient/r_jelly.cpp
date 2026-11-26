/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
#include "r_jelly.h"

#include <base/math.h>

#include <engine/shared/config.h>

#include <game/client/gameclient.h>

#include <algorithm>
#include <cmath>

std::unique_ptr<CRJelly> rJelly;

namespace
{
constexpr float MIN_DELTA_TIME = 1.0f / 240.0f;
constexpr float MAX_DELTA_TIME = 1.0f / 20.0f;

struct SJellySuperProfile
{
	float m_StrengthMul;
	float m_DurationMul;
	float m_DeformSpringMul;
	float m_DeformDampingMul;
	float m_CompressionSpringMul;
	float m_CompressionDampingMul;
	float m_WobbleFrequency;
	float m_WobbleAmplitude;
	float m_StopSpeedEnter;
	float m_StopSpeedExit;
	float m_StopDrop;
	float m_SpeedSquashMul;
	float m_ScaleMul;
};

vec2 NormalizeOr(vec2 Value, vec2 Fallback)
{
	const float ValueLength = length(Value);
	if(ValueLength > 0.0001f)
		return Value / ValueLength;

	const float FallbackLength = length(Fallback);
	if(FallbackLength > 0.0001f)
		return Fallback / FallbackLength;

	return vec2(0.0f, -1.0f);
}

float DurationScale()
{
	const float Duration = std::clamp(g_Config.m_ClZzJellyTeeDuration / 100.0f, 0.05f, 5.0f);
	// Keep a perceptible inertia even at medium values.
	return 0.45f + Duration * 1.40f;
}

void ClampDeform(JellyTee &Deform, bool SuperMode)
{
	if(SuperMode)
	{
		Deform.m_BodyScale.x = std::clamp(Deform.m_BodyScale.x, 0.82f, 1.48f);
		Deform.m_BodyScale.y = std::clamp(Deform.m_BodyScale.y, 0.52f, 1.42f);
		Deform.m_FeetScale.x = std::clamp(Deform.m_FeetScale.x, 0.86f, 1.30f);
		Deform.m_FeetScale.y = std::clamp(Deform.m_FeetScale.y, 0.48f, 1.20f);
	}
	else
	{
		Deform.m_BodyScale.x = std::clamp(Deform.m_BodyScale.x, 0.94f, 1.22f);
		Deform.m_BodyScale.y = std::clamp(Deform.m_BodyScale.y, 0.74f, 1.22f);
		Deform.m_FeetScale.x = std::clamp(Deform.m_FeetScale.x, 0.95f, 1.15f);
		Deform.m_FeetScale.y = std::clamp(Deform.m_FeetScale.y, 0.70f, 1.10f);
	}
}

SJellySuperProfile GetJellySuperProfile()
{
	const int Mode = std::clamp(g_Config.m_ClZzJellyTeeSuperMode, 0, 2);
	// 0 = Normal, 1 = Medium, 2 = Crazy
	static const SJellySuperProfile s_aProfiles[] = {
		{1.60f, 1.00f, 0.88f, 0.88f, 0.92f, 0.86f, 6.5f, 0.34f, 3.3f, 0.70f, 1.5f, 0.46f, 1.18f},
		{2.25f, 1.22f, 0.72f, 0.72f, 0.78f, 0.70f, 5.4f, 0.52f, 2.3f, 0.55f, 1.1f, 0.72f, 1.52f},
		{3.30f, 1.45f, 0.58f, 0.55f, 0.63f, 0.52f, 4.4f, 0.82f, 1.2f, 0.45f, 0.7f, 1.08f, 2.08f}};
	return s_aProfiles[Mode];
}
} // namespace

CRJelly::CRJelly(CGameClient *pClient) :
	m_pClient(pClient)
{
}

void CRJelly::Reset()
{
	for(auto &State : m_aStates)
		State = CState();
}

bool CRJelly::IsEnabledFor(int ClientId) const
{
	if(m_pClient == nullptr || !g_Config.m_ClZzJellyTee || ClientId < 0 || ClientId >= MAX_JELLY_CLIENTS)
		return false;

	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		if(ClientId == m_pClient->m_aLocalIds[Dummy])
			return true;
	}

	return g_Config.m_ClZzJellyTeeOthers != 0;
}

CRJelly::CState *CRJelly::StateFor(int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_JELLY_CLIENTS)
		return nullptr;
	return &m_aStates[ClientId];
}

JellyTee CRJelly::GetDeform(int ClientId, vec2 PrevVel, vec2 Vel, vec2 LookDir, bool InAir, bool WantOtherDir, float DeltaTime, vec2 ExtraDeformImpulse, float ExtraCompression)
{
	JellyTee Deform;
	const bool Disabled = !g_Config.m_ClZzJellyTee || g_Config.m_ClZzJellyTeeStrength <= 0;
	CState *pState = StateFor(ClientId);

	if(Disabled)
	{
		Reset();
		return Deform;
	}

	if(pState == nullptr)
		return Deform;

	if(!IsEnabledFor(ClientId))
		return Deform;

	pState->m_ClientId = ClientId;
	DeltaTime = std::clamp(DeltaTime, MIN_DELTA_TIME, MAX_DELTA_TIME);

	if(!pState->m_Initialized)
	{
		pState->m_PrevInputVel = Vel;
		pState->m_Initialized = true;
	}

	const bool SuperMode = g_Config.m_ClZzJellyTeeSuper != 0;
	const float StrengthBase = g_Config.m_ClZzJellyTeeStrength / 100.0f;
	const SJellySuperProfile SuperProfile = GetJellySuperProfile();
	const float Strength = StrengthBase * (SuperMode ? SuperProfile.m_StrengthMul : 1.0f);
	const float Duration = DurationScale() * (SuperMode ? SuperProfile.m_DurationMul : 1.0f);

	const vec2 PreviousVel = pState->m_PrevInputVel;
	const vec2 DeltaVel = Vel - PreviousVel;
	const bool DirectionFlipX = (Vel.x > 0.0f && PreviousVel.x < 0.0f) || (Vel.x < 0.0f && PreviousVel.x > 0.0f);
	const float LandingImpact = (InAir ? 0.0f : std::clamp((PrevVel.y - Vel.y) / 13.0f, 0.0f, 1.8f)) * Strength;
	const float StopImpulse = std::clamp((length(PreviousVel) - length(Vel)) / 4.8f, 0.0f, 1.3f) * Strength;
	const float TurnImpulse = std::clamp(absolute(DeltaVel.x) / 5.8f, 0.0f, DirectionFlipX ? 1.2f : 0.8f) * Strength;
	const float SideImpulse = std::clamp(absolute(DeltaVel.x) / 5.2f, 0.0f, DirectionFlipX ? 1.15f : 0.75f) * Strength;
	const float PrevSpeed = length(PreviousVel);
	const float CurSpeed = length(Vel);
	const bool TriggerStopWobble = SuperMode &&
		PrevSpeed > SuperProfile.m_StopSpeedEnter &&
		CurSpeed < SuperProfile.m_StopSpeedExit &&
		(PrevSpeed - CurSpeed) > SuperProfile.m_StopDrop;
	if(TriggerStopWobble)
		pState->m_StopWobbleTime = 1.0f;
	pState->m_StopWobbleTime = maximum(0.0f, pState->m_StopWobbleTime - DeltaTime);
	pState->m_StopWobblePhase += DeltaTime * (SuperMode ? SuperProfile.m_WobbleFrequency : 12.0f);

	vec2 MotionBasis = NormalizeOr(Vel, vec2(LookDir.x, -0.25f));
	vec2 TargetDeform(
		std::clamp(-DeltaVel.x / 6.0f, -0.9f, 0.9f) * Strength,
		std::clamp(-DeltaVel.y / 10.0f, -1.0f, 1.0f) * Strength);
	TargetDeform.x += std::clamp(-DeltaVel.x / 5.0f, -0.5f, 0.5f) * SideImpulse;
	TargetDeform += vec2(std::clamp(-Vel.x / 22.0f, -0.28f, 0.28f), 0.0f) * StopImpulse;
	if(DirectionFlipX || WantOtherDir)
		TargetDeform.x += std::clamp(-Vel.x / 17.0f, -0.28f, 0.28f) * Strength;
	TargetDeform += ExtraDeformImpulse * Strength;

	const float DeformSpring = ((SuperMode ? 3.25f : 6.0f) * (SuperMode ? SuperProfile.m_DeformSpringMul : 1.0f)) / Duration;
	const float DeformDamping = ((SuperMode ? 0.95f : 1.9f) * (SuperMode ? SuperProfile.m_DeformDampingMul : 1.0f)) / maximum(Duration, 0.35f);
	pState->m_DeformVelocity += (TargetDeform - pState->m_Deform) * DeformSpring * DeltaTime;
	pState->m_DeformVelocity *= 1.0f / (1.0f + DeformDamping * DeltaTime);
	pState->m_Deform += pState->m_DeformVelocity * DeltaTime;

	const float TargetCompression = LandingImpact * 1.25f + StopImpulse * 0.55f + TurnImpulse * 0.30f - std::clamp(-Vel.y / 24.0f, 0.0f, 0.25f) + std::clamp(ExtraCompression, -0.8f, 2.2f) * Strength;
	const float CompressionSpring = ((SuperMode ? 2.9f : 4.2f) * (SuperMode ? SuperProfile.m_CompressionSpringMul : 1.0f)) / Duration;
	const float CompressionDamping = ((SuperMode ? 0.85f : 1.7f) * (SuperMode ? SuperProfile.m_CompressionDampingMul : 1.0f)) / maximum(Duration, 0.35f);
	pState->m_CompressionVelocity += (TargetCompression - pState->m_Compression) * CompressionSpring * DeltaTime;
	pState->m_CompressionVelocity *= 1.0f / (1.0f + CompressionDamping * DeltaTime);
	pState->m_Compression += pState->m_CompressionVelocity * DeltaTime;

	if(pState->m_StopWobbleTime > 0.0f)
	{
		const float WobbleFade = pState->m_StopWobbleTime;
		const float WobbleAmp = (SuperMode ? SuperProfile.m_WobbleAmplitude : 0.12f) * Strength * WobbleFade;
		pState->m_Deform.x += std::sin(pState->m_StopWobblePhase) * WobbleAmp;
		pState->m_Deform.y += std::cos(pState->m_StopWobblePhase * 0.78f) * WobbleAmp * 0.76f;
	}

	const vec2 DeformDirection = NormalizeOr(pState->m_Deform, MotionBasis);
	const float DeformAmount = std::clamp(length(pState->m_Deform), 0.0f, 1.10f);
	const float HorizontalStretch = absolute(DeformDirection.x) * DeformAmount;
	const float VerticalStretch = absolute(DeformDirection.y) * DeformAmount;
	const float LandingSquash = std::clamp(pState->m_Compression, 0.0f, 1.2f);
	const float AirStretch = std::clamp(-pState->m_Compression, 0.0f, 0.5f);
	const float SpeedForward = absolute(dot(NormalizeOr(Vel, vec2(1.0f, 0.0f)), NormalizeOr(LookDir, vec2(1.0f, 0.0f)))) * length(Vel);
	const float SpeedSquash = SuperMode ? std::clamp(SpeedForward / 18.0f, 0.0f, 1.0f) * SuperProfile.m_SpeedSquashMul : 0.0f;

	const float SuperMul = SuperMode ? SuperProfile.m_ScaleMul : 1.0f;
	Deform.m_BodyScale.x += (HorizontalStretch * 0.14f + LandingSquash * 0.24f + VerticalStretch * 0.03f) * SuperMul;
	Deform.m_BodyScale.y += (VerticalStretch * 0.15f + AirStretch * 0.22f - HorizontalStretch * 0.05f - LandingSquash * 0.34f - SpeedSquash * 0.28f) * SuperMul;

	Deform.m_FeetScale.x += (LandingSquash * 0.15f + HorizontalStretch * 0.03f + SpeedSquash * 0.20f) * SuperMul;
	Deform.m_FeetScale.y += (AirStretch * 0.05f - LandingSquash * 0.24f - HorizontalStretch * 0.01f - SpeedSquash * 0.18f) * SuperMul;

	const float MoveLean = std::clamp(Vel.x / 11.0f, -1.0f, 1.0f) * 0.055f * (InAir ? 0.70f : 1.0f);
	const float CounterLean = SuperMode ? std::clamp(-Vel.x / 9.0f, -1.0f, 1.0f) * 0.105f : 0.0f;
	Deform.m_BodyAngle = std::clamp(-pState->m_Deform.x * 0.12f - pState->m_DeformVelocity.x * 0.008f + DeformDirection.x * VerticalStretch * 0.02f + MoveLean + CounterLean, -0.27f, 0.27f);
	Deform.m_FeetAngle = std::clamp(-Deform.m_BodyAngle * 0.30f + pState->m_Deform.x * 0.025f + MoveLean * 0.25f - CounterLean * 0.22f, -0.14f, 0.14f);
	ClampDeform(Deform, SuperMode);

	pState->m_PrevInputVel = Vel;
	return Deform;
}
