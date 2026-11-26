#include "zz_tas.h"

#include <base/color.h>
#include <base/io.h>
#include <base/math.h>
#include <base/str.h>
#include <base/system.h>

#include <engine/console.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>
#include <engine/storage.h>

#include <generated/client_data.h>
#include <game/gamecore.h>
#include <game/client/animstate.h>
#include <game/client/components/chat.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/render.h>
#include <game/client/ui.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr const char *gs_pTasMagic = "PSTATAS4";
constexpr const char *gs_pTasMagicLegacyMulti = "PSTATAS3";
constexpr const char *gs_pTasMagicLegacy = "PSTATAS2";
constexpr float gs_TasStartTolerance = 0.5f;

struct STasScreenPixelGrid
{
	float m_ScreenX0;
	float m_ScreenY0;
	float m_PixelSizeX;
	float m_PixelSizeY;
};

int GetTasStepAmount()
{
	return g_Config.m_ClZzTasTickControlStep ? 1 : maximum(1, g_Config.m_ClZzTasTickControlTicks);
}

int DummyMaskBit(int Dummy)
{
	return 1 << (Dummy != 0 ? 1 : 0);
}

void GetReplayNameStem(char *pOut, int OutSize)
{
	str_copy(pOut, g_Config.m_ClZzTasReplayName[0] != '\0' ? g_Config.m_ClZzTasReplayName : "my_tas", OutSize);
	if(const char *pSuffix = str_endswith(pOut, ".tas"); pSuffix != nullptr)
		pOut[pSuffix - pOut] = '\0';
}

STasScreenPixelGrid GetTasScreenPixelGrid(const IGraphics *pGraphics)
{
	float ScreenX0, ScreenY0, ScreenX1, ScreenY1;
	pGraphics->GetScreen(&ScreenX0, &ScreenY0, &ScreenX1, &ScreenY1);

	return {
		ScreenX0,
		ScreenY0,
		(ScreenX1 - ScreenX0) / maximum(1, pGraphics->ScreenWidth()),
		(ScreenY1 - ScreenY0) / maximum(1, pGraphics->ScreenHeight())};
}

vec2 SnapTasPoint(const STasScreenPixelGrid &Grid, vec2 Pos)
{
	return vec2(
		Grid.m_ScreenX0 + roundf((Pos.x - Grid.m_ScreenX0) / Grid.m_PixelSizeX) * Grid.m_PixelSizeX,
		Grid.m_ScreenY0 + roundf((Pos.y - Grid.m_ScreenY0) / Grid.m_PixelSizeY) * Grid.m_PixelSizeY);
}

float SnapTasSpan(float Span, float PixelSize)
{
	return maximum(PixelSize, roundf(Span / PixelSize) * PixelSize);
}

void RenderTasPoints(IGraphics *pGraphics, IGraphics::CTextureHandle Texture, const std::vector<vec2> &vPoints, ColorRGBA Color, float Size)
{
	if(vPoints.empty() || Color.a <= 0.0f)
		return;

	const STasScreenPixelGrid Grid = GetTasScreenPixelGrid(pGraphics);
	const float SizeX = SnapTasSpan(Size, Grid.m_PixelSizeX);
	const float SizeY = SnapTasSpan(Size, Grid.m_PixelSizeY);

	if(Texture.IsValid())
		pGraphics->TextureSet(Texture);
	else
		pGraphics->TextureClear();

	pGraphics->QuadsBegin();
	pGraphics->SetColor(Color);
	for(const vec2 &Point : vPoints)
	{
		const vec2 TopLeft = SnapTasPoint(Grid, Point - vec2(SizeX * 0.5f, SizeY * 0.5f));
		const IGraphics::CQuadItem Quad(TopLeft.x, TopLeft.y, SizeX, SizeY);
		pGraphics->QuadsDrawTL(&Quad, 1);
	}
	pGraphics->QuadsEnd();
}

void RenderTasLines(IGraphics *pGraphics, const std::vector<IGraphics::CLineItem> &vSegments, ColorRGBA Color, float Thickness, float Zoom)
{
	if(vSegments.empty() || Color.a <= 0.0f)
		return;

	const STasScreenPixelGrid Grid = GetTasScreenPixelGrid(pGraphics);
	const float PixelSize = maximum(Grid.m_PixelSizeX, Grid.m_PixelSizeY);
	const float LineWidth = SnapTasSpan(maximum(PixelSize, (0.8f + (Thickness - 1.0f) * 0.5f) * Zoom), PixelSize);

	pGraphics->TextureClear();
	std::vector<IGraphics::CFreeformItem> vQuads;
	vQuads.reserve(vSegments.size());
	for(const auto &Segment : vSegments)
	{
		const vec2 Start = SnapTasPoint(Grid, vec2(Segment.m_X0, Segment.m_Y0));
		const vec2 End = SnapTasPoint(Grid, vec2(Segment.m_X1, Segment.m_Y1));
		const vec2 Diff = End - Start;
		if(length(Diff) < 0.001f)
			continue;
		const vec2 Perp = normalize(vec2(Diff.y, -Diff.x)) * (LineWidth * 0.5f);
		vQuads.emplace_back(
			End.x - Perp.x, End.y - Perp.y,
			End.x + Perp.x, End.y + Perp.y,
			Start.x - Perp.x, Start.y - Perp.y,
			Start.x + Perp.x, Start.y + Perp.y);
	}
	if(vQuads.empty())
		return;
	pGraphics->QuadsBegin();
	pGraphics->SetColor(Color);
	pGraphics->QuadsDrawFreeform(vQuads.data(), vQuads.size());
	pGraphics->QuadsEnd();
}
}

void CZzTas::OnConsoleInit()
{
	Console()->Register("zz_tas_record_start", "?r[name]", CFGFLAG_CLIENT, ConTasRecordStart, this, "Start TAS recording");
	Console()->Register("zz_tas_record_stop", "", CFGFLAG_CLIENT, ConTasRecordStop, this, "Stop TAS recording");
	Console()->Register("zz_tas_play_start", "?r[name]", CFGFLAG_CLIENT, ConTasPlayStart, this, "Start TAS playback");
	Console()->Register("zz_tas_play_stop", "", CFGFLAG_CLIENT, ConTasPlayStop, this, "Stop TAS playback");
	Console()->Register("zz_tas_save", "?r[name]", CFGFLAG_CLIENT, ConTasSave, this, "Save TAS replay");
	Console()->Register("zz_tas_load", "?r[name]", CFGFLAG_CLIENT, ConTasLoad, this, "Load TAS replay");
	Console()->Register("zz_tas_unload", "", CFGFLAG_CLIENT, ConTasUnload, this, "Unload/clear active TAS replay from memory");

	// Shalawa command compatibility aliases.
	Console()->Register("shalawa_tas_record_start", "?r[name]", CFGFLAG_CLIENT, ConTasRecordStart, this, "Shalawa alias: start TAS recording");
	Console()->Register("shalawa_tas_record_stop", "", CFGFLAG_CLIENT, ConTasRecordStop, this, "Shalawa alias: stop TAS recording");
	Console()->Register("shalawa_tas_play_start", "?r[name]", CFGFLAG_CLIENT, ConTasPlayStart, this, "Shalawa alias: start TAS playback");
	Console()->Register("shalawa_tas_play_stop", "", CFGFLAG_CLIENT, ConTasPlayStop, this, "Shalawa alias: stop TAS playback");
	Console()->Register("shalawa_tas_save", "?r[name]", CFGFLAG_CLIENT, ConTasSave, this, "Shalawa alias: save TAS replay");
	Console()->Register("shalawa_tas_load", "?r[name]", CFGFLAG_CLIENT, ConTasLoad, this, "Shalawa alias: load TAS replay");
	Console()->Register("shalawa_tas_unload", "", CFGFLAG_CLIENT, ConTasUnload, this, "Shalawa alias: unload/clear TAS replay");
}

void CZzTas::ProcessFinalSnapInput(CNetObj_PlayerInput &Input, bool Dummy)
{
	if(m_Recording && m_WorldInitialized)
		ObserveFinalSnapInput(Input, Dummy);

	if(!m_Playing)
		return;

	const int PlayerFlags = Input.m_PlayerFlags;
	CNetObj_PlayerInput PlaybackInput;
	const bool PlaybackDummy = Dummy;
	if(!m_vFrames.empty())
	{
		const SFrame &Frame = m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)];
		if(m_LegacySingleDummyFile)
		{
			(void)Frame;
		}
	}
	if(!GetPlaybackInput(PlaybackInput, PlaybackDummy))
	{
		// Missing dummy in current playback frame: keep original live input for this tee.
		return;
	}

	Input = PlaybackInput;
	Input.m_PlayerFlags = PlayerFlags;
}

void CZzTas::OnReset()
{
	m_vFrames.clear();
	m_vRecordedWorldStates.clear();
	m_PlayCursor = 0;
	m_LastRecordTasTick = -1;
	m_LastAutoSave = time_get();
	m_Recording = false;
	m_Playing = false;
	m_Paused = false;
	m_WasFrozenInWorld = false;
	m_HasInitialWorldState = false;
	m_LegacySingleDummyFile = false;
	m_Accumulator = 0.0;
	m_LastSeekTime = 0;
	m_SmoothIntra = 0.0f;
	m_PlaybackStepRateOverride = 0;
	m_LegacyPlaybackDummy = 0;
	m_aFxLastAttackTick.fill(-1);
	m_aFxLastFrozen.fill(false);
	m_SuppressTasEffects = false;
	ResetObservedInputs();
	g_Config.m_ClZzTasInit = 0;
	str_copy(m_aLoadedName, "", sizeof(m_aLoadedName));
	RefreshFileList();
	ShutdownWorld();
}

void CZzTas::ResetObservedInputs()
{
	m_aHaveObservedInput.fill(false);
	m_aHaveFinalInput.fill(false);
	m_aLatchedJump.fill(false);
	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		mem_zero(&m_aObservedInput[Dummy], sizeof(m_aObservedInput[Dummy]));
		mem_zero(&m_aFinalInput[Dummy], sizeof(m_aFinalInput[Dummy]));
		m_aObservedMouse[Dummy] = vec2(0.0f, 0.0f);
		m_aFinalMouse[Dummy] = vec2(0.0f, 0.0f);
		m_aObservedPos[Dummy] = vec2(0.0f, 0.0f);
	}
}

void CZzTas::SyncPlaybackControlDummy()
{
	if(!m_Playing || m_vFrames.empty() || m_LegacySingleDummyFile)
		return;

	const SFrame &Frame = m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)];
	const int ControlDummy = DisplayDummyForFrame(Frame);
	g_Config.m_ClDummy = ControlDummy != 0 ? 1 : 0;
}

float CZzTas::GetStepRate() const
{
	if(m_Playing && m_PlaybackStepRateOverride > 0)
		return (float)m_PlaybackStepRateOverride;
	return (float)std::clamp(g_Config.m_ClZzTasTicksPerSecond, 1, 50);
}

int CZzTas::CurrentDummyIndex() const
{
	return g_Config.m_ClDummy != 0 ? 1 : 0;
}

bool CZzTas::FrameHasDummy(const SFrame &Frame, int Dummy) const
{
	return (Frame.m_DummyMask & DummyMaskBit(Dummy)) != 0;
}

int CZzTas::FirstDummyInMask(int DummyMask) const
{
	if(DummyMask & DummyMaskBit(0))
		return 0;
	if(DummyMask & DummyMaskBit(1))
		return 1;
	return 0;
}

int CZzTas::DisplayDummyForFrame(const SFrame &Frame) const
{
	if(m_LegacySingleDummyFile)
		return m_LegacyPlaybackDummy;

	if(FrameHasDummy(Frame, Frame.m_ControlDummy))
		return Frame.m_ControlDummy;
	return FirstDummyInMask(Frame.m_DummyMask);
}

vec2 CZzTas::CurrentDummyPos(int Dummy) const
{
	const int LocalId = GameClient()->m_aLocalIds[Dummy];
	if(LocalId >= 0 && LocalId < MAX_CLIENTS)
	{
		if(const CCharacter *pLocalChar = GameClient()->m_RegularPredictedWorld.GetCharacterById(LocalId))
			return pLocalChar->GetPos();
		return GameClient()->m_aClients[LocalId].m_RegularPredicted.m_Pos;
	}
	return GameClient()->m_LocalCharacterPos;
}

int CZzTas::RecordedDummyMask() const
{
	int DummyMask = 0;
	for(const SFrame &Frame : m_vFrames)
		DummyMask |= Frame.m_DummyMask;
	return DummyMask;
}

void CZzTas::BuildCurrentTasInputs(TInputArray &aInputs, TVec2Array &aMice) const
{
	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		aInputs[Dummy] = GameClient()->m_Controls.m_aInputData[Dummy];
		aMice[Dummy] = vec2((float)aInputs[Dummy].m_TargetX, (float)aInputs[Dummy].m_TargetY);
		if(length(aMice[Dummy]) < 0.001f)
			aMice[Dummy] = vec2(1.0f, 0.0f);

		if(m_aHaveObservedInput[Dummy])
		{
			aInputs[Dummy] = m_aObservedInput[Dummy];
			aMice[Dummy] = m_aObservedMouse[Dummy];
		}
		if(m_aLatchedJump[Dummy])
			aInputs[Dummy].m_Jump = 1;
	}
}

void CZzTas::BuildPlaybackTasInputs(const SFrame &Frame, TInputArray &aInputs, TVec2Array &aMice) const
{
	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		CNetObj_PlayerInput Neutral = {};
		Neutral.m_TargetX = GameClient()->m_Controls.m_aInputData[Dummy].m_TargetX;
		Neutral.m_TargetY = GameClient()->m_Controls.m_aInputData[Dummy].m_TargetY;
		if(!Neutral.m_TargetX && !Neutral.m_TargetY)
			Neutral.m_TargetX = 1;
		aInputs[Dummy] = Neutral;
		aMice[Dummy] = vec2((float)Neutral.m_TargetX, (float)Neutral.m_TargetY);
		if(length(aMice[Dummy]) < 0.001f)
			aMice[Dummy] = vec2(1.0f, 0.0f);
	}

	if(m_LegacySingleDummyFile)
	{
		aInputs[m_LegacyPlaybackDummy] = Frame.m_aInput[0];
		aMice[m_LegacyPlaybackDummy] = Frame.m_aMouse[0];
		return;
	}

	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		if(!FrameHasDummy(Frame, Dummy))
			continue;
		aInputs[Dummy] = Frame.m_aInput[Dummy];
		aMice[Dummy] = Frame.m_aMouse[Dummy];
	}
}

CZzTas::SFrame CZzTas::CaptureFrameFromWorld(int TasTick, const TInputArray &aInputs, const TVec2Array &aMice) const
{
	SFrame Frame{};
	Frame.m_Tick = TasTick;
	Frame.m_ControlDummy = CurrentDummyIndex();
	CGameWorld *pSourceWorld = const_cast<CGameWorld *>(&GameClient()->m_RegularPredictedWorld);
	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		const int LocalId = GameClient()->m_aLocalIds[Dummy];
		if(LocalId < 0)
			continue;

		Frame.m_DummyMask |= DummyMaskBit(Dummy);
		Frame.m_aInput[Dummy] = aInputs[Dummy];
		Frame.m_aMouse[Dummy] = aMice[Dummy];
		Frame.m_aPos[Dummy] = CurrentDummyPos(Dummy);
		if(pSourceWorld != nullptr)
		{
			if(const CCharacter *pRecordedChar = pSourceWorld->GetCharacterById(LocalId))
				Frame.m_aPos[Dummy] = pRecordedChar->GetPos();
		}
	}
	if(!FrameHasDummy(Frame, Frame.m_ControlDummy))
		Frame.m_ControlDummy = FirstDummyInMask(Frame.m_DummyMask);
	return Frame;
}

bool CZzTas::BuildRecordedWorldState(SRecordedWorldState &State) const
{
	CGameWorld *pSourceWorld = const_cast<CGameWorld *>(&GameClient()->m_RegularPredictedWorld);
	if(pSourceWorld == nullptr)
		return false;

	State = SRecordedWorldState{};
	State.m_GameTick = pSourceWorld->m_GameTick;
	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		const int LocalId = GameClient()->m_aLocalIds[Dummy];
		if(LocalId < 0)
			continue;

		CCharacter *pRecordedChar = pSourceWorld->GetCharacterById(LocalId);
		if(pRecordedChar == nullptr)
			continue;

		State.m_DummyMask |= DummyMaskBit(Dummy);
		State.m_aCore[Dummy] = pRecordedChar->GetCore();
		State.m_aPos[Dummy] = pRecordedChar->GetPos();
		State.m_aPrevPos[Dummy] = pRecordedChar->m_PrevPos;
		State.m_aFreezeTime[Dummy] = pRecordedChar->m_FreezeTime;
		State.m_aReloadTimer[Dummy] = pRecordedChar->GetReloadTimer();
		State.m_aAttackTick[Dummy] = pRecordedChar->GetAttackTick();
	}
	return State.m_DummyMask != 0;
}

void CZzTas::CaptureRecordedWorldState()
{
	SRecordedWorldState State{};
	if(BuildRecordedWorldState(State))
		m_vRecordedWorldStates.push_back(State);
}

void CZzTas::ApplyRecordedWorldState(CGameWorld *pWorld, const SRecordedWorldState &State) const
{
	if(pWorld == nullptr)
		return;

	pWorld->m_GameTick = State.m_GameTick;
	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		if((State.m_DummyMask & DummyMaskBit(Dummy)) == 0)
			continue;

		const int LocalId = GameClient()->m_aLocalIds[Dummy];
		if(LocalId < 0)
			continue;

		CCharacter *pChar = pWorld->GetCharacterById(LocalId);
		if(pChar == nullptr)
			continue;

		pChar->SetCore(State.m_aCore[Dummy]);
		pChar->SetCoreWorld(pWorld);
		pChar->m_Pos = State.m_aPos[Dummy];
		pChar->m_PrevPos = State.m_aPrevPos[Dummy];
		pChar->m_FreezeTime = State.m_aFreezeTime[Dummy];
		pChar->SetReloadTimer(State.m_aReloadTimer[Dummy]);
		pChar->SetAttackTick(State.m_aAttackTick[Dummy]);
	}
}

void CZzTas::RestoreRecordedWorldState(const SRecordedWorldState &State)
{
	ApplyRecordedWorldState(m_pWorld, State);
}

bool CZzTas::HasPlaybackWorldStates() const
{
	// Playback should follow the resimulated TAS world driven by recorded inputs.
	// Recorded world snapshots are still useful while editing a recording, but
	// using them during playback can freeze or diverge when the stored states no
	// longer match the recorded per-dummy inputs.
	return !m_Playing && !m_vFrames.empty() && m_vRecordedWorldStates.size() == m_vFrames.size();
}

void CZzTas::RestorePlaybackCursorState()
{
	if(!m_WorldInitialized || m_pWorld == nullptr || m_vFrames.empty() || !HasPlaybackWorldStates())
		return;

	const int Index = std::clamp(m_PlayCursor, 0, (int)m_vRecordedWorldStates.size() - 1);
	RestoreRecordedWorldState(m_vRecordedWorldStates[Index]);
	if(m_pWorldPredicted != nullptr)
	{
		const int NextIndex = minimum(Index + 1, (int)m_vRecordedWorldStates.size() - 1);
		m_pWorldPredicted->CopyWorld(m_pWorld);
		if(NextIndex != Index)
			ApplyRecordedWorldState(m_pWorldPredicted, m_vRecordedWorldStates[NextIndex]);
		else
			m_pWorldPredicted->m_GameTick = m_pWorld->m_GameTick;
	}
}

vec2 CZzTas::GetStartPosition() const
{
	if(m_vFrames.empty())
		return vec2(0.0f, 0.0f);

	const SFrame &Frame = m_vFrames.front();
	if(m_LegacySingleDummyFile)
		return Frame.m_aPos[0];
	return Frame.m_aPos[DisplayDummyForFrame(Frame)];
}

float CZzTas::GetDistanceToStartPosition() const
{
	if(m_vFrames.empty())
		return 0.0f;

	const SFrame &Frame = m_vFrames.front();
	if(m_LegacySingleDummyFile)
	{
		const int Dummy = std::clamp(m_LegacyPlaybackDummy, 0, NUM_DUMMIES - 1);
		return distance(CurrentDummyPos(Dummy), Frame.m_aPos[0]);
	}

	float MaxDistance = 0.0f;
	bool HasAnyDummy = false;
	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		if(!FrameHasDummy(Frame, Dummy))
			continue;

		HasAnyDummy = true;
		MaxDistance = maximum(MaxDistance, distance(CurrentDummyPos(Dummy), Frame.m_aPos[Dummy]));
	}

	if(!HasAnyDummy)
	{
		const int Dummy = DisplayDummyForFrame(Frame);
		return distance(CurrentDummyPos(Dummy), Frame.m_aPos[Dummy]);
	}

	return MaxDistance;
}

bool CZzTas::IsAtStartPosition() const
{
	return !m_vFrames.empty() && GetDistanceToStartPosition() <= StartPositionTolerance();
}

float CZzTas::StartPositionTolerance() const
{
	return gs_TasStartTolerance;
}

void CZzTas::OnStateChange(int NewState, int OldState)
{
	(void)OldState;
	if(NewState != IClient::STATE_ONLINE)
	{
		g_Config.m_ClZzTasRecordReplay = 0;
		g_Config.m_ClZzTasLoadReplay = 0;
		g_Config.m_ClZzTasPause = 0;
		g_Config.m_ClZzTasRespawn = 0;
		g_Config.m_ClZzTasInit = 0;
		OnReset();
	}
}

void CZzTas::EnsureWorldInitialized()
{
	if(m_WorldInitialized)
		return;

	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	m_pWorld = new CGameWorld();
	m_pWorld->Init(Collision(), GameClient()->m_GameWorld.TuningList(), nullptr);
	m_pWorld->CopyWorld(&GameClient()->m_RegularPredictedWorld);
	m_pWorld->m_GameTick = GameClient()->m_RegularPredictedWorld.m_GameTick;

	m_pWorldPredicted = new CGameWorld();
	m_pWorldPredicted->Init(Collision(), GameClient()->m_GameWorld.TuningList(), nullptr);
	m_pWorldPredicted->CopyWorld(m_pWorld);
	m_pWorldPredicted->m_GameTick = m_pWorld->m_GameTick + 1;

	m_WorldInitialized = true;
}

void CZzTas::ShutdownWorld()
{
	if(m_pWorld != nullptr)
	{
		delete m_pWorld;
		m_pWorld = nullptr;
	}
	if(m_pWorldPredicted != nullptr)
	{
		delete m_pWorldPredicted;
		m_pWorldPredicted = nullptr;
	}
	m_WorldInitialized = false;
}

void CZzTas::ResetWorldFromPredicted()
{
	ShutdownWorld();
	EnsureWorldInitialized();
}

bool CZzTas::HasLocalCharactersInWorld(CGameWorld *pWorld) const
{
	if(pWorld == nullptr)
		return false;

	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		const int LocalId = GameClient()->m_aLocalIds[Dummy];
		if(LocalId < 0)
			continue;

		if(pWorld->GetCharacterById(LocalId) != nullptr)
			return true;
	}

	return false;
}

void CZzTas::SyncWorldFromPredictedIfNeeded()
{
	if((!m_Recording && !m_Playing) || !m_WorldInitialized)
		return;
	if(m_Playing)
		return;
	if(HasLocalCharactersInWorld(m_pWorld))
		return;

	ResetWorldFromPredicted();
}

void CZzTas::StartRecording()
{
	g_Config.m_ClZzTasInit = 0;
	ShutdownWorld();
	m_vFrames.clear();
	m_vRecordedWorldStates.clear();
	m_PlayCursor = 0;
	m_LastRecordTasTick = -1;
	m_LastAutoSave = time_get();
	m_Accumulator = 0.0;
	m_LastSeekTime = 0;
	ResetObservedInputs();
	m_WasFrozenInWorld = false;
	m_HasInitialWorldState = false;
	m_LegacySingleDummyFile = false;
	m_aFxLastAttackTick.fill(-1);
	m_aFxLastFrozen.fill(false);
	m_SuppressTasEffects = false;
	m_Recording = true;
	m_Playing = false;
	g_Config.m_ClZzTasPause = 0;
	m_Paused = false;
	g_Config.m_ClZzTasInit = 1;
	g_Config.m_ClZzTasLoadReplay = 0;
	ResetWorldFromPredicted();
	m_HasInitialWorldState = BuildRecordedWorldState(m_InitialWorldState);
}

void CZzTas::StopRecording()
{
	m_Recording = false;
	if(!m_Playing)
		ShutdownWorld();
}

bool CZzTas::FindReplayPath(char *pBuf, int BufSize, bool ExactStemOnly) const
{
	pBuf[0] = '\0';
	if(!EnsureFolder())
		return false;
	char aWantedStem[128];
	GetReplayNameStem(aWantedStem, sizeof(aWantedStem));

	namespace fs = std::filesystem;
	char aRoot[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, TAS_FOLDER, aRoot, sizeof(aRoot));
	const fs::path Root = fs::path(aRoot);
	if(!fs::exists(Root))
		return false;

	std::error_code ErrorCode;
	for(const auto &Entry : fs::recursive_directory_iterator(Root, ErrorCode))
	{
		if(ErrorCode)
			break;
		if(!Entry.is_regular_file() || Entry.path().extension() != ".tas")
			continue;

		const std::string Stem = Entry.path().stem().string();
		const std::string Relative = fs::relative(Entry.path(), Root, ErrorCode).generic_string();
		if((ExactStemOnly && Stem == aWantedStem) ||
			(!ExactStemOnly && (Stem == aWantedStem || Relative == g_Config.m_ClZzTasReplayName)))
		{
			str_copy(pBuf, Entry.path().string().c_str(), BufSize);
			return true;
		}
	}

	return false;
}

bool CZzTas::GetReplaySavePath(char *pBuf, int BufSize, bool AutoSave) const
{
	pBuf[0] = '\0';
	if(!EnsureFolder())
		return false;

	namespace fs = std::filesystem;
	char aRoot[IO_MAX_PATH_LENGTH];
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, TAS_FOLDER, aRoot, sizeof(aRoot));
	fs::path ReplayDir = fs::path(aRoot);
	if(AutoSave)
		ReplayDir /= "auto";
	std::error_code ErrorCode;
	fs::create_directories(ReplayDir, ErrorCode);

	char aName[128];
	char aStem[128];
	GetReplayNameStem(aStem, sizeof(aStem));
	if(AutoSave)
	{
		char aTimestamp[64];
		str_timestamp_format(aTimestamp, sizeof(aTimestamp), "%Y%m%d_%H%M%S");
		str_format(aName, sizeof(aName), "%s_%s.tas", aStem, aTimestamp);
	}
	else
		str_format(aName, sizeof(aName), "%s.tas", aStem);

	str_copy(pBuf, (ReplayDir / aName).string().c_str(), BufSize);
	return true;
}

bool CZzTas::SaveReplayToPath(const char *pPath) const
{
	std::ofstream File(pPath, std::ios::out | std::ios::trunc);
	if(!File.is_open())
		return false;

	File << gs_pTasMagic << "\n";
	File << "frames " << m_vFrames.size() << "\n";
	for(const SFrame &Frame : m_vFrames)
	{
		File << Frame.m_Tick << ' ' << Frame.m_DummyMask << ' ' << Frame.m_ControlDummy;
		for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
		{
			const CNetObj_PlayerInput &Input = Frame.m_aInput[Dummy];
			File
				<< ' ' << Input.m_Direction
				<< ' ' << Input.m_TargetX
				<< ' ' << Input.m_TargetY
				<< ' ' << Input.m_Jump
				<< ' ' << Input.m_Fire
				<< ' ' << Input.m_Hook
				<< ' ' << Input.m_PlayerFlags
				<< ' ' << Input.m_WantedWeapon
				<< ' ' << Input.m_NextWeapon
				<< ' ' << Input.m_PrevWeapon
				<< ' ' << Frame.m_aMouse[Dummy].x
				<< ' ' << Frame.m_aMouse[Dummy].y
				<< ' ' << Frame.m_aPos[Dummy].x
				<< ' ' << Frame.m_aPos[Dummy].y;
		}
		File << "\n";
	}
	return true;
}

bool CZzTas::LoadReplayFromPath(const char *pPath)
{
	std::ifstream File(pPath);
	if(!File.is_open())
		return false;

	std::string Line;
	if(!std::getline(File, Line))
		return false;
	const bool LegacyFormat = Line == gs_pTasMagicLegacy;
	const bool LegacyMultiFormat = Line == gs_pTasMagicLegacyMulti;
	if(Line != gs_pTasMagic && !LegacyMultiFormat && !LegacyFormat)
		return false;
	if(!std::getline(File, Line))
		return false;

	std::vector<SFrame> vFrames;
	while(std::getline(File, Line))
	{
		if(Line.empty())
			continue;
		SFrame Frame;
		std::istringstream Stream(Line);
		Stream >> Frame.m_Tick;
		if(LegacyFormat)
		{
			Frame.m_DummyMask = DummyMaskBit(0);
			Frame.m_ControlDummy = 0;
			Stream
				>> Frame.m_aInput[0].m_Direction
				>> Frame.m_aInput[0].m_TargetX
				>> Frame.m_aInput[0].m_TargetY
				>> Frame.m_aInput[0].m_Jump
				>> Frame.m_aInput[0].m_Fire
				>> Frame.m_aInput[0].m_Hook
				>> Frame.m_aInput[0].m_PlayerFlags
				>> Frame.m_aInput[0].m_WantedWeapon
				>> Frame.m_aInput[0].m_NextWeapon
				>> Frame.m_aInput[0].m_PrevWeapon
				>> Frame.m_aMouse[0].x
				>> Frame.m_aMouse[0].y
				>> Frame.m_aPos[0].x
				>> Frame.m_aPos[0].y;
		}
		else
		{
			Stream >> Frame.m_DummyMask;
			if(!LegacyMultiFormat)
				Stream >> Frame.m_ControlDummy;
			for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
			{
				Stream
					>> Frame.m_aInput[Dummy].m_Direction
					>> Frame.m_aInput[Dummy].m_TargetX
					>> Frame.m_aInput[Dummy].m_TargetY
					>> Frame.m_aInput[Dummy].m_Jump
					>> Frame.m_aInput[Dummy].m_Fire
					>> Frame.m_aInput[Dummy].m_Hook
					>> Frame.m_aInput[Dummy].m_PlayerFlags
					>> Frame.m_aInput[Dummy].m_WantedWeapon
					>> Frame.m_aInput[Dummy].m_NextWeapon
					>> Frame.m_aInput[Dummy].m_PrevWeapon
					>> Frame.m_aMouse[Dummy].x
					>> Frame.m_aMouse[Dummy].y
					>> Frame.m_aPos[Dummy].x
					>> Frame.m_aPos[Dummy].y;
			}
			if(LegacyMultiFormat)
				Frame.m_ControlDummy = FirstDummyInMask(Frame.m_DummyMask);
		}
		if(Stream.fail())
			return false;
		if(!FrameHasDummy(Frame, Frame.m_ControlDummy))
			Frame.m_ControlDummy = FirstDummyInMask(Frame.m_DummyMask);
		vFrames.push_back(Frame);
	}

	m_vFrames = std::move(vFrames);
	m_vRecordedWorldStates.clear();
	m_HasInitialWorldState = false;
	m_LegacySingleDummyFile = LegacyFormat;
	m_LegacyPlaybackDummy = 0;
	m_PlayCursor = 0;
	return !m_vFrames.empty();
}

bool CZzTas::StartPlayback()
{
	if(m_vFrames.empty())
	{
		char aPath[IO_MAX_PATH_LENGTH];
		if(!FindReplayPath(aPath, sizeof(aPath), false) || !LoadReplayFromPath(aPath))
		{
			PushTasWarning("TAS", "Replay not found or failed to load.");
			g_Config.m_ClZzTasLoadReplay = 0;
			return false;
		}
	}

	if(m_LegacySingleDummyFile)
		m_LegacyPlaybackDummy = GameClient()->m_aLocalIds[CurrentDummyIndex()] >= 0 ? CurrentDummyIndex() : 0;

	const int RequiredDummyMask = m_LegacySingleDummyFile ? DummyMaskBit(m_LegacyPlaybackDummy) : RecordedDummyMask();
	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		if((RequiredDummyMask & DummyMaskBit(Dummy)) == 0)
			continue;

		const int LocalId = GameClient()->m_aLocalIds[Dummy];
		if(LocalId < 0)
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "[TAS ERROR] Replay needs dummy %d connected.", Dummy + 1);
			GameClient()->Echo(aBuf);
			Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "zz_tas", aBuf, ColorRGBA(1.0f, 0.2f, 0.2f, 1.0f));
			g_Config.m_ClZzTasLoadReplay = 0;
			return false;
		}
	}

	if(!IsAtStartPosition())
	{
		PushTasWarning("TAS", "Stand on the recorded start position before playback.");
		g_Config.m_ClZzTasLoadReplay = 0;
		return false;
	}

	m_PlayCursor = std::clamp(m_PlayCursor, 0, maximum(0, (int)m_vFrames.size() - 1));
	m_Accumulator = 0.0;
	m_LastSeekTime = 0;
	m_PlaybackOriginalDummy = g_Config.m_ClDummy;
	g_Config.m_ClZzTasInit = 1;
	m_Playing = true;
	m_Recording = false;
	g_Config.m_ClZzTasPause = 0;
	m_Paused = false;
	m_PlaybackStepRateOverride = 50;
	g_Config.m_ClZzTasRecordReplay = 0;
	m_aFxLastAttackTick.fill(-1);
	m_aFxLastFrozen.fill(false);
	m_SuppressTasEffects = false;
	ResetObservedInputs();
	RestorePlaybackStartState();
	SyncPlaybackControlDummy();
	return true;
}

void CZzTas::StopPlayback()
{
	m_Playing = false;
	m_PlaybackStepRateOverride = 0;
	g_Config.m_ClZzTasPause = 0;
	m_Paused = false;
	g_Config.m_ClDummy = m_PlaybackOriginalDummy;
	ResetObservedInputs();
	if(!m_Recording)
		ShutdownWorld();
}

void CZzTas::ClearReplay(bool KillPlayer)
{
	m_vFrames.clear();
	m_vRecordedWorldStates.clear();
	m_HasInitialWorldState = false;
	m_PlayCursor = 0;
	m_LastRecordTasTick = -1;
	m_Recording = false;
	m_Playing = false;
	m_Paused = false;
	m_LegacySingleDummyFile = false;
	m_LegacyPlaybackDummy = 0;
	g_Config.m_ClZzTasRecordReplay = 0;
	g_Config.m_ClZzTasLoadReplay = 0;
	g_Config.m_ClZzTasPause = 0;
	g_Config.m_ClZzTasInit = 0;
	ShutdownWorld();
	if(KillPlayer && Client()->State() == IClient::STATE_ONLINE)
		GameClient()->m_Chat.SendChat(0, "/kill");
}

void CZzTas::RestorePlaybackStartState()
{
	ResetWorldFromPredicted();
	if(!m_WorldInitialized || m_vFrames.empty())
	{
		m_PlayCursor = 0;
		return;
	}

	m_PlayCursor = 0;
	if(HasPlaybackWorldStates())
	{
		RestorePlaybackCursorState();
		m_SmoothIntra = 0.0f;
		return;
	}

	if(m_HasInitialWorldState)
		RestoreRecordedWorldState(m_InitialWorldState);

	TInputArray aInputs{};
	TVec2Array aMice{};
	BuildPlaybackTasInputs(m_vFrames[0], aInputs, aMice);
	TickWorld(aInputs, aMice);
}

void CZzTas::StepPlayback(int Direction, int Amount)
{
	if(m_vFrames.empty())
		return;
	m_PlayCursor = std::clamp(m_PlayCursor + Direction * maximum(1, Amount), 0, (int)m_vFrames.size() - 1);
	SyncPlaybackControlDummy();
	if(HasPlaybackWorldStates())
	{
		RestorePlaybackCursorState();
		m_SmoothIntra = 0.0f;
	}
	else
		ResimulatePlaybackWorld();
}

void CZzTas::AdvancePlaybackOneStep()
{
	if(m_vFrames.empty())
		return;
	if(m_PlayCursor + 1 < (int)m_vFrames.size())
	{
		++m_PlayCursor;
		SyncPlaybackControlDummy();
	}
	else if(g_Config.m_ClZzTasAutoReplay)
	{
		RestorePlaybackStartState();
		SyncPlaybackControlDummy();
	}
	else
	{
		StopPlayback();
		g_Config.m_ClZzTasLoadReplay = 0;
	}
}

void CZzTas::TickWorld(const TInputArray &aInputs, const TVec2Array &aMice)
{
	if(!m_WorldInitialized || m_pWorld == nullptr)
		return;

	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		const int LocalId = GameClient()->m_aLocalIds[Dummy];
		if(LocalId < 0)
			continue;

		CCharacter *pChar = m_pWorld->GetCharacterById(LocalId);
		if(pChar == nullptr)
			continue;

		CNetObj_PlayerInput AppliedInput = aInputs[Dummy];
		AppliedInput.m_TargetX = (int)aMice[Dummy].x;
		AppliedInput.m_TargetY = (int)aMice[Dummy].y;
		if(!AppliedInput.m_TargetX && !AppliedInput.m_TargetY)
			AppliedInput.m_TargetX = 1;

		pChar->OnDirectInput(&AppliedInput);
		pChar->OnDirectInput(&AppliedInput);
		pChar->OnPredictedInput(&AppliedInput);
	}

	m_pWorld->Tick();
	m_pWorld->m_GameTick++;

	if(!m_SuppressTasEffects)
	{
		for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
		{
			const int LocalId = GameClient()->m_aLocalIds[Dummy];
			if(LocalId < 0 || LocalId >= MAX_CLIENTS)
				continue;

			CCharacter *pFxChar = m_pWorld->GetCharacterById(LocalId);
			if(pFxChar == nullptr || pFxChar->Core() == nullptr)
				continue;

			const int AttackTick = pFxChar->GetAttackTick();
			if(m_aFxLastAttackTick[Dummy] >= 0 && AttackTick != m_aFxLastAttackTick[Dummy] && pFxChar->GetActiveWeapon() == WEAPON_HAMMER)
				GameClient()->m_Effects.HammerHit(pFxChar->GetPos(), 1.0f, 1.0f, LocalId);
			m_aFxLastAttackTick[Dummy] = AttackTick;

			const bool FrozenNow = pFxChar->m_FreezeTime > 0 || pFxChar->Core()->m_DeepFrozen || pFxChar->Core()->m_LiveFrozen;
			if(FrozenNow && !m_aFxLastFrozen[Dummy])
				GameClient()->m_Effects.FreezingFlakes(pFxChar->GetPos(), vec2(36.0f, 36.0f), 1.0f);
			m_aFxLastFrozen[Dummy] = FrozenNow;
		}
	}

	UpdatePredictedWorld(aInputs, aMice);
}

void CZzTas::UpdatePredictedWorld(const TInputArray &aInputs, const TVec2Array &aMice)
{
	if(!m_WorldInitialized || m_pWorld == nullptr || m_pWorldPredicted == nullptr)
		return;

	m_pWorldPredicted->CopyWorld(m_pWorld);
	m_pWorldPredicted->m_GameTick = m_pWorld->m_GameTick + 1;

	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		const int LocalId = GameClient()->m_aLocalIds[Dummy];
		if(LocalId < 0)
			continue;

		if(CCharacter *pChar = m_pWorldPredicted->GetCharacterById(LocalId))
		{
			CNetObj_PlayerInput PredInput = aInputs[Dummy];
			PredInput.m_TargetX = (int)aMice[Dummy].x;
			PredInput.m_TargetY = (int)aMice[Dummy].y;
			if(!PredInput.m_TargetX && !PredInput.m_TargetY)
				PredInput.m_TargetX = 1;
			pChar->OnDirectInput(&PredInput);
			pChar->OnDirectInput(&PredInput);
			pChar->OnPredictedInput(&PredInput);
		}
	}

	m_pWorldPredicted->Tick();
}

void CZzTas::ResimulatePlaybackWorld()
{
	if(!m_Playing || m_vFrames.empty())
		return;

	if(HasPlaybackWorldStates())
	{
		RestorePlaybackCursorState();
		m_SmoothIntra = 0.0f;
		return;
	}

	const int TargetCursor = std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1);
	const bool PrevSuppress = m_SuppressTasEffects;
	m_SuppressTasEffects = true;
	RestorePlaybackStartState();
	if(!m_WorldInitialized)
	{
		m_SuppressTasEffects = PrevSuppress;
		return;
	}

	const int LastIndex = TargetCursor;
	for(int i = 1; i <= LastIndex; ++i)
	{
		TInputArray aInputs{};
		TVec2Array aMice{};
		BuildPlaybackTasInputs(m_vFrames[i], aInputs, aMice);
		TickWorld(aInputs, aMice);
	}

	m_PlayCursor = TargetCursor;
	SyncPlaybackControlDummy();
	m_SmoothIntra = 0.0f;
	m_SuppressTasEffects = PrevSuppress;
}

bool CZzTas::IsLocalFrozenInWorld() const
{
	if(!m_WorldInitialized || m_pWorld == nullptr)
		return false;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId < 0)
		return false;

	const CCharacter *pChar = m_pWorld->GetCharacterById(LocalId);
	if(pChar == nullptr)
		return false;

	return pChar->m_FreezeTime > 0 || pChar->Core()->m_DeepFrozen || pChar->Core()->m_LiveFrozen;
}

void CZzTas::RewindRecording(int Amount)
{
	if(!m_Recording || m_vFrames.empty())
		return;

	for(int Step = 0; Step < maximum(1, Amount) && !m_vFrames.empty(); ++Step)
	{
		m_vFrames.pop_back();
		if(!m_vRecordedWorldStates.empty())
			m_vRecordedWorldStates.pop_back();
	}

	if(m_vFrames.empty() || m_vRecordedWorldStates.empty())
	{
		m_PlayCursor = 0;
		m_LastRecordTasTick = -1;
		ResetWorldFromPredicted();
		return;
	}

	m_PlayCursor = (int)m_vFrames.size() - 1;
	const SFrame &LastFrame = m_vFrames.back();
	m_LastRecordTasTick = LastFrame.m_Tick;
	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		m_aHaveObservedInput[Dummy] = FrameHasDummy(LastFrame, Dummy);
		m_aObservedInput[Dummy] = LastFrame.m_aInput[Dummy];
		m_aObservedMouse[Dummy] = LastFrame.m_aMouse[Dummy];
		m_aObservedPos[Dummy] = LastFrame.m_aPos[Dummy];
		m_aLatchedJump[Dummy] = LastFrame.m_aInput[Dummy].m_Jump != 0;
	}

	RestoreRecordedWorldState(m_vRecordedWorldStates.back());
	TInputArray aInputs{};
	TVec2Array aMice{};
	BuildCurrentTasInputs(aInputs, aMice);
	UpdatePredictedWorld(aInputs, aMice);
}

void CZzTas::StepForwardRecording()
{
	if(!m_Recording)
		return;

	TInputArray aInputs{};
	TVec2Array aMice{};
	BuildCurrentTasInputs(aInputs, aMice);
	TickWorld(aInputs, aMice);

	const int TasTick = m_pWorld != nullptr ? m_pWorld->m_GameTick : (m_LastRecordTasTick + 1);
	if(TasTick == m_LastRecordTasTick)
		return;

	CaptureRecordedWorldState();
	m_vFrames.push_back(CaptureFrameFromWorld(TasTick, aInputs, aMice));
	m_LastRecordTasTick = TasTick;
	m_PlayCursor = (int)m_vFrames.size() - 1;
	m_aLatchedJump.fill(false);
}

void CZzTas::AutoForwardRecording()
{
	if(!m_Recording || !IsLocalFrozenInWorld())
		return;

	TInputArray aInputs{};
	TVec2Array aMice{};
	BuildCurrentTasInputs(aInputs, aMice);

	const int MaxTicks = maximum(1, g_Config.m_ClZzTasTickControlTicks) * 16;
	for(int i = 0; i < MaxTicks && IsLocalFrozenInWorld(); ++i)
	{
		TickWorld(aInputs, aMice);

		const int TasTick = m_pWorld != nullptr ? m_pWorld->m_GameTick : (m_LastRecordTasTick + 1);
		CaptureRecordedWorldState();
		m_vFrames.push_back(CaptureFrameFromWorld(TasTick, aInputs, aMice));
		m_LastRecordTasTick = TasTick;
		m_PlayCursor = (int)m_vFrames.size() - 1;
		m_aLatchedJump.fill(false);
	}
}

void CZzTas::OnUpdate()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	if(g_Config.m_ClZzTasFakeAim)
	{
		if(!m_vFrames.empty())
		{
			for(size_t i = 0; i < m_vFrames.size(); ++i)
			{
				SFrame &Frame = m_vFrames[i];
				if(Frame.m_DummyMask == 0)
					continue;

				const int Dummy = DisplayDummyForFrame(Frame);
				CNetObj_PlayerInput &Input = Frame.m_aInput[Dummy];
				vec2 &Mouse = Frame.m_aMouse[Dummy];
				const bool EssentialAim = Input.m_Fire != 0 || Input.m_Hook != 0;
				if(EssentialAim)
					continue;

				switch(g_Config.m_ClZzTasFakeAimMode)
				{
				case 0: // robot aim
					if(i > 0)
					{
						const SFrame &PrevFrame = m_vFrames[i - 1];
						const int PrevDummy = FrameHasDummy(PrevFrame, Dummy) ? Dummy : DisplayDummyForFrame(PrevFrame);
						Mouse = PrevFrame.m_aMouse[PrevDummy];
					}
					break;
				case 1: // spinbot
				{
					const float Angle = (float)i * 0.28f;
					Mouse = vec2(cosf(Angle), sinf(Angle)) * 380.0f;
					break;
				}
				case 2: // random
				{
					const float Angle = random_float() * pi * 2.0f;
					Mouse = vec2(cosf(Angle), sinf(Angle)) * (220.0f + random_float() * 220.0f);
					break;
				}
				case 3: // smooth
					if(i > 0)
					{
						const SFrame &PrevFrame = m_vFrames[i - 1];
						const int PrevDummy = FrameHasDummy(PrevFrame, Dummy) ? Dummy : DisplayDummyForFrame(PrevFrame);
						Mouse = mix(PrevFrame.m_aMouse[PrevDummy], Mouse, 0.2f);
					}
					break;
				default:
					break;
				}
				Input.m_TargetX = (int)Mouse.x;
				Input.m_TargetY = (int)Mouse.y;
				if(!Input.m_TargetX && !Input.m_TargetY)
					Input.m_TargetX = 1;
			}
		}
		g_Config.m_ClZzTasFakeAim = 0;
	}

	if(g_Config.m_ClZzTasRespawn)
	{
		ClearReplay(true);
		g_Config.m_ClZzTasRespawn = 0;
	}

	if(g_Config.m_ClZzTasInit && !m_WorldInitialized && Client()->State() == IClient::STATE_ONLINE)
		ResetWorldFromPredicted();
	else if(!g_Config.m_ClZzTasInit && m_WorldInitialized && !m_Recording && !m_Playing)
		ShutdownWorld();

	if(g_Config.m_ClZzTasRecordReplay && !m_Recording)
		StartRecording();
	else if(!g_Config.m_ClZzTasRecordReplay && m_Recording)
		StopRecording();

	if(g_Config.m_ClZzTasLoadReplay && !m_Playing)
		StartPlayback();
	else if(!g_Config.m_ClZzTasLoadReplay && m_Playing)
		StopPlayback();

	if(m_Playing)
		SyncPlaybackControlDummy();

	SyncWorldFromPredictedIfNeeded();
	m_Paused = g_Config.m_ClZzTasPause != 0;
}

void CZzTas::ObserveInput(const CNetObj_PlayerInput &Input, vec2 Pos, bool Dummy)
{
	const int DummyIndex = Dummy != 0 ? 1 : 0;
	m_aHaveObservedInput[DummyIndex] = true;
	m_aObservedInput[DummyIndex] = Input;
	if(Input.m_Jump != 0)
		m_aLatchedJump[DummyIndex] = true;
	m_aObservedMouse[DummyIndex] = vec2((float)Input.m_TargetX, (float)Input.m_TargetY);
	if(length(m_aObservedMouse[DummyIndex]) < 0.001f)
		m_aObservedMouse[DummyIndex] = vec2(1.0f, 0.0f);
	m_aObservedPos[DummyIndex] = Pos;
}

void CZzTas::ObserveFinalSnapInput(const CNetObj_PlayerInput &Input, bool Dummy)
{
	const int ControlDummy = g_Config.m_ClDummy != 0 ? 1 : 0;
	const int DummyIndex = Dummy ? (ControlDummy ^ 1) : ControlDummy;
	m_aHaveFinalInput[DummyIndex] = true;
	m_aFinalInput[DummyIndex] = Input;
	m_aFinalMouse[DummyIndex] = vec2((float)Input.m_TargetX, (float)Input.m_TargetY);
	if(length(m_aFinalMouse[DummyIndex]) < 0.001f)
		m_aFinalMouse[DummyIndex] = vec2(1.0f, 0.0f);
	if(Input.m_Jump != 0)
		m_aLatchedJump[DummyIndex] = true;
}

bool CZzTas::GetPlaybackInput(CNetObj_PlayerInput &Out) const
{
	return GetPlaybackInput(Out, GetDisplayDummyIndex() != 0);
}

bool CZzTas::GetPlaybackInput(CNetObj_PlayerInput &Out, bool Dummy) const
{
	if(!m_Playing || m_vFrames.empty())
		return false;

	const int DummyIndex = Dummy != 0 ? 1 : 0;
	const SFrame &Frame = m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)];
	if(m_LegacySingleDummyFile)
	{
		if(DummyIndex != m_LegacyPlaybackDummy)
			return false;
		Out = Frame.m_aInput[0];
		return true;
	}
	if(FrameHasDummy(Frame, DummyIndex))
	{
		Out = Frame.m_aInput[DummyIndex];
		return true;
	}
	return false;
}

bool CZzTas::HasRenderMouseOverride() const
{
	return m_WorldInitialized && m_Recording;
}

int CZzTas::GetDisplayDummyIndex() const
{
	if(m_Playing && !m_vFrames.empty())
	{
		const SFrame &Frame = m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)];
		return DisplayDummyForFrame(Frame);
	}
	return CurrentDummyIndex();
}

vec2 CZzTas::GetRenderMousePos() const
{
	if(!HasRenderMouseOverride())
		return vec2(0.0f, 0.0f);
	return GetRenderMousePos(GetDisplayDummyIndex() != 0);
}

vec2 CZzTas::GetRenderMousePos(bool Dummy) const
{
	const int DummyIndex = Dummy != 0 ? 1 : 0;
	if(m_Recording)
	{
		const vec2 Mouse = m_aHaveObservedInput[DummyIndex] ? m_aObservedMouse[DummyIndex] : GameClient()->m_Controls.m_aMousePos[DummyIndex];
		return length(Mouse) < 0.001f ? vec2(1.0f, 0.0f) : Mouse;
	}
	if(m_Playing && !m_vFrames.empty())
	{
		const SFrame &Frame = m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)];
		if(m_LegacySingleDummyFile)
		{
			if(DummyIndex == m_LegacyPlaybackDummy)
				return Frame.m_aMouse[0];
		}
		else if(FrameHasDummy(Frame, DummyIndex))
		{
			return Frame.m_aMouse[DummyIndex];
		}
	}
	return m_aObservedMouse[DummyIndex];
}

bool CZzTas::IsWorldActive() const
{
	return (m_Recording || m_Playing) && m_WorldInitialized;
}

vec2 CZzTas::GetInterpolatedPlayerPos() const
{
	if(!IsWorldActive())
		return GameClient()->m_LocalCharacterPos;

	const int LocalId = GameClient()->m_aLocalIds[GetDisplayDummyIndex()];
	if(LocalId < 0 || m_pWorld == nullptr)
		return GameClient()->m_LocalCharacterPos;

	CCharacter *pCommitted = m_pWorld->GetCharacterById(LocalId);
	if(pCommitted == nullptr)
		return GameClient()->m_LocalCharacterPos;

	CCharacter *pPredicted = m_pWorldPredicted != nullptr ? m_pWorldPredicted->GetCharacterById(LocalId) : nullptr;
	const vec2 PrevPos = pCommitted->GetPos();
	const vec2 CurPos = pPredicted != nullptr ? pPredicted->GetPos() : PrevPos;
	return mix(PrevPos, CurPos, std::clamp(m_SmoothIntra, 0.0f, 1.0f));
}

void CZzTas::PushTasWarning(const char *pTitle, const char *pMessage) const
{
	Client()->AddWarning(SWarning(pTitle, pMessage));
}

void CZzTas::SaveReplay()
{
	if(m_vFrames.empty())
	{
		PushTasWarning("TAS", "Replay buffer is empty.");
		return;
	}

	char aPath[IO_MAX_PATH_LENGTH];
	if(!GetReplaySavePath(aPath, sizeof(aPath), false) || !SaveReplayToPath(aPath))
	{
		PushTasWarning("TAS", "Failed to save replay.");
		return;
	}

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "Saved replay to %s", aPath);
	PushTasWarning("TAS", aBuf);
}

void CZzTas::LoadSelectedReplay()
{
	char aPath[IO_MAX_PATH_LENGTH];
	if(!FindReplayPath(aPath, sizeof(aPath), false))
	{
		PushTasWarning("TAS", "No replay selected.");
		return;
	}

	if(m_Playing)
	{
		m_Playing = false;
		g_Config.m_ClZzTasLoadReplay = 0;
	}
	m_Recording = false;
	g_Config.m_ClZzTasRecordReplay = 0;
	g_Config.m_ClZzTasPause = 0;
	m_Paused = false;

	if(!LoadReplayFromPath(aPath))
	{
		PushTasWarning("TAS", "Failed to load replay.");
		return;
	}

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "Loaded replay %s", aPath);
	PushTasWarning("TAS", aBuf);
}

void CZzTas::UnloadReplay()
{
	const bool HadData = !m_vFrames.empty() || m_Recording || m_Playing;
	StopPlayback();
	StopRecording();
	ClearReplay(false);
	g_Config.m_ClZzTasLoadReplay = 0;
	g_Config.m_ClZzTasRecordReplay = 0;
	g_Config.m_ClZzTasPause = 0;
	if(HadData)
		PushTasWarning("TAS", "Replay unloaded from memory.");
	else
		PushTasWarning("TAS", "No replay loaded.");
}

void CZzTas::ValidateReplay()
{
	if(!m_vFrames.empty())
	{
		PushTasWarning("TAS", "Replay buffer is valid.");
		return;
	}

	char aPath[IO_MAX_PATH_LENGTH];
	if(!FindReplayPath(aPath, sizeof(aPath), false))
	{
		PushTasWarning("TAS", "No replay selected.");
		return;
	}

	if(LoadReplayFromPath(aPath))
		PushTasWarning("TAS", "Replay file looks valid.");
	else
		PushTasWarning("TAS", "Replay file failed to validate.");
}

void CZzTas::ReportReplayTime()
{
	const int Frames = (int)m_vFrames.size();
	if(Frames <= 0)
	{
		PushTasWarning("TAS", "Replay buffer is empty.");
		return;
	}

	const float Seconds = Frames / GetStepRate();
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "%d frames, %.2fs", Frames, Seconds);
	PushTasWarning("TAS", aBuf);
}

void CZzTas::RemoveUseless()
{
	if(m_vFrames.size() < 2)
	{
		PushTasWarning("TAS", "Nothing to optimize.");
		return;
	}

	std::vector<SFrame> vOptimized;
	vOptimized.reserve(m_vFrames.size());
	vOptimized.push_back(m_vFrames.front());
	for(size_t i = 1; i < m_vFrames.size(); ++i)
	{
		const SFrame &PrevFrame = m_vFrames[i - 1];
		const SFrame &Frame = m_vFrames[i];
		bool SameFrame = PrevFrame.m_DummyMask == Frame.m_DummyMask && PrevFrame.m_ControlDummy == Frame.m_ControlDummy;
		for(int Dummy = 0; Dummy < NUM_DUMMIES && SameFrame; ++Dummy)
		{
			if(!FrameHasDummy(Frame, Dummy))
				continue;
			if(mem_comp(&PrevFrame.m_aInput[Dummy], &Frame.m_aInput[Dummy], sizeof(CNetObj_PlayerInput)) != 0)
				SameFrame = false;
			else if(distance(PrevFrame.m_aMouse[Dummy], Frame.m_aMouse[Dummy]) >= 0.01f)
				SameFrame = false;
		}

		if(!SameFrame)
			vOptimized.push_back(m_vFrames[i]);
	}

	const int Removed = (int)m_vFrames.size() - (int)vOptimized.size();
	m_vFrames = std::move(vOptimized);
	m_PlayCursor = std::clamp(m_PlayCursor, 0, maximum(0, (int)m_vFrames.size() - 1));

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "Removed %d repeated frames.", maximum(0, Removed));
	PushTasWarning("TAS", aBuf);
}

void CZzTas::SyncNow()
{
	PushTasWarning("TAS", "Replay Vault sync is not wired yet.");
}

void CZzTas::RenderStartEndPos()
{
	if(!g_Config.m_ClZzTasDrawStartEndPos || m_vFrames.empty() || m_Playing)
		return;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId < 0)
		return;

	CTeeRenderInfo TeeInfo = GameClient()->m_aClients[LocalId].m_RenderInfo;
	TeeInfo.m_Size = 64.0f;
	const CAnimState *pIdleState = CAnimState::GetIdle();

	CTeeRenderInfo StartInfo = TeeInfo;
	StartInfo.m_CustomColoredSkin = true;
	StartInfo.m_ColorBody = ColorRGBA(0.15f, 1.0f, 0.35f, 1.0f);
	StartInfo.m_ColorFeet = ColorRGBA(0.15f, 1.0f, 0.35f, 1.0f);
	const SFrame &StartFrame = m_vFrames.front();
	const int StartDummy = DisplayDummyForFrame(StartFrame);
	GameClient()->RenderTools()->RenderTee(pIdleState, &StartInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), StartFrame.m_aPos[StartDummy]);

	if(!m_Recording && m_vFrames.size() > 1)
	{
		CTeeRenderInfo EndInfo = TeeInfo;
		EndInfo.m_CustomColoredSkin = true;
		EndInfo.m_ColorBody = ColorRGBA(1.0f, 0.2f, 0.95f, 1.0f);
		EndInfo.m_ColorFeet = ColorRGBA(1.0f, 0.2f, 0.95f, 1.0f);
		const SFrame &EndFrame = m_vFrames.back();
		const int EndDummy = DisplayDummyForFrame(EndFrame);
		GameClient()->RenderTools()->RenderTee(pIdleState, &EndInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), EndFrame.m_aPos[EndDummy]);
	}
}

void CZzTas::RenderReplayPath()
{
	if(!g_Config.m_ClZzTasDrawPath || m_vFrames.size() < 2 || !m_Playing || m_Recording)
		return;

	// Frame positions can be misleading when playback is driven from recorded
	// per-dummy inputs instead of stored snapshot positions.
	if(!HasPlaybackWorldStates())
		return;

	const ColorRGBA PathColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClZzTasDrawPathColor));
	int StartIndex = 0;
	int EndIndex = (int)m_vFrames.size() - 1;
	if(g_Config.m_ClZzTasDrawPathSegmented)
	{
		const int Pivot = std::clamp(m_PlayCursor, 0, EndIndex);
		StartIndex = Pivot;
		EndIndex = minimum((int)m_vFrames.size() - 1, Pivot + 180);
	}

	Graphics()->TextureClear();
	if(g_Config.m_ClZzTasDrawPathMode == 0)
	{
		std::vector<vec2> vPoints;
		vPoints.reserve(EndIndex - StartIndex + 1);
		for(int i = StartIndex; i <= EndIndex; ++i)
			if(i % 2 == 0)
			{
				const SFrame &Frame = m_vFrames[i];
				const int Dummy = DisplayDummyForFrame(Frame);
				vPoints.push_back(Frame.m_aPos[Dummy]);
			}
		const auto DotTexture = GameClient()->m_ParticlesSkin.m_aSpriteParticles[SPRITE_PART_BALL - SPRITE_PART_SLICE];
		RenderTasPoints(Graphics(), DotTexture, vPoints, PathColor, 4.0f);
	}
	else
	{
		std::vector<IGraphics::CLineItem> vSegments;
		vSegments.reserve(EndIndex - StartIndex);
		for(int i = StartIndex; i < EndIndex; ++i)
		{
			const SFrame &Frame1 = m_vFrames[i];
			const SFrame &Frame2 = m_vFrames[i + 1];
			const int Dummy1 = DisplayDummyForFrame(Frame1);
			const int Dummy2 = DisplayDummyForFrame(Frame2);
			const vec2 Pos1 = Frame1.m_aPos[Dummy1];
			const vec2 Pos2 = Frame2.m_aPos[Dummy2];
			vSegments.emplace_back(Pos1.x, Pos1.y, Pos2.x, Pos2.y);
		}
		RenderTasLines(Graphics(), vSegments, PathColor, 1.35f, GameClient()->m_Camera.m_Zoom);
	}
}

void CZzTas::RenderPredictionPath()
{
	if(!g_Config.m_ClZzTasDrawPredictionPath || !m_WorldInitialized || m_pWorld == nullptr || m_Playing)
		return;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId < 0)
		return;
	CCharacter *pChar = m_pWorld->GetCharacterById(LocalId);
	if(pChar == nullptr)
		return;

	CGameWorld TempWorld;
	TempWorld.Init(Collision(), GameClient()->m_GameWorld.TuningList(), nullptr);
	TempWorld.CopyWorld(m_pWorld);
	CCharacter *pTempChar = TempWorld.GetCharacterById(LocalId);
	if(pTempChar == nullptr)
		return;

	TInputArray aInputs{};
	TVec2Array aMice{};
	BuildCurrentTasInputs(aInputs, aMice);
	const CNetObj_PlayerInput PredictInput = aInputs[CurrentDummyIndex()];
	std::vector<vec2> vPath;
	const int Ticks = 30;
	vPath.reserve(Ticks + 1);
	for(int t = 0; t < Ticks; ++t)
	{
		vPath.push_back(pTempChar->GetPos());
		pTempChar->OnDirectInput(&PredictInput);
		pTempChar->OnDirectInput(&PredictInput);
		pTempChar->OnPredictedInput(&PredictInput);
		TempWorld.Tick();
		TempWorld.m_GameTick++;
	}
	vPath.push_back(pTempChar->GetPos());

	const ColorRGBA LocalColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClZzTasDrawPredictionPathColor));
	const ColorRGBA FrozenColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClZzTasDrawPredictionPathColorFrozen));
	Graphics()->TextureClear();
	if(g_Config.m_ClZzTasDrawPredictionPathMode == 0)
	{
		std::vector<vec2> vLocalDots;
		std::vector<vec2> vFrozenDots;
		for(const vec2 &Pos : vPath)
		{
			bool Frozen = false;
			if(Collision() != nullptr)
			{
				const int Index = Collision()->GetPureMapIndex(Pos);
				const int Tile = Collision()->GetTileIndex(Index);
				const int FrontTile = Collision()->GetFrontTileIndex(Index);
				Frozen = Tile == TILE_FREEZE || FrontTile == TILE_FREEZE || Tile == TILE_DFREEZE || FrontTile == TILE_DFREEZE || Tile == TILE_LFREEZE || FrontTile == TILE_LFREEZE;
			}
			(Frozen ? vFrozenDots : vLocalDots).push_back(Pos);
		}
		const auto DotTexture = GameClient()->m_ParticlesSkin.m_aSpriteParticles[SPRITE_PART_BALL - SPRITE_PART_SLICE];
		RenderTasPoints(Graphics(), DotTexture, vLocalDots, LocalColor, 3.5f);
		RenderTasPoints(Graphics(), DotTexture, vFrozenDots, FrozenColor, 3.5f);
	}
	else
	{
		std::vector<IGraphics::CLineItem> vLocalSegments;
		std::vector<IGraphics::CLineItem> vFrozenSegments;
		for(size_t i = 1; i < vPath.size(); ++i)
		{
			bool Frozen = false;
			if(Collision() != nullptr)
			{
				const int Index = Collision()->GetPureMapIndex(vPath[i - 1]);
				const int Tile = Collision()->GetTileIndex(Index);
				const int FrontTile = Collision()->GetFrontTileIndex(Index);
				Frozen = Tile == TILE_FREEZE || FrontTile == TILE_FREEZE || Tile == TILE_DFREEZE || FrontTile == TILE_DFREEZE || Tile == TILE_LFREEZE || FrontTile == TILE_LFREEZE;
			}
			(Frozen ? vFrozenSegments : vLocalSegments).emplace_back(vPath[i - 1].x, vPath[i - 1].y, vPath[i].x, vPath[i].y);
		}
		RenderTasLines(Graphics(), vLocalSegments, LocalColor, 1.2f, GameClient()->m_Camera.m_Zoom);
		RenderTasLines(Graphics(), vFrozenSegments, FrozenColor, 1.2f, GameClient()->m_Camera.m_Zoom);
	}
}

void CZzTas::RenderTasWorld()
{
	if(m_Playing && !m_Recording)
	{
		const float Intra = std::clamp(Client()->PredIntraGameTick(g_Config.m_ClDummy), 0.0f, 1.0f);
		for(int DummyIdx = 0; DummyIdx < NUM_DUMMIES; ++DummyIdx)
		{
			const int LocalId = GameClient()->m_aLocalIds[DummyIdx];
			if(LocalId < 0 || LocalId >= MAX_CLIENTS)
				continue;

			const CGameClient::CClientData &ClientData = GameClient()->m_aClients[LocalId];
			CNetObj_Character PrevObj = ClientData.m_RenderPrev;
			CNetObj_Character CurObj = ClientData.m_RenderCur;
			const bool HaveRenderObjects = PrevObj.m_Tick >= 0 || CurObj.m_Tick >= 0;
			if(!HaveRenderObjects)
			{
				CCharacter *pPrevChar = GameClient()->m_PrevRegularPredictedWorld.GetCharacterById(LocalId);
				CCharacter *pCurChar = GameClient()->m_RegularPredictedWorld.GetCharacterById(LocalId);
				if(pPrevChar == nullptr)
					pPrevChar = pCurChar;
				if(pCurChar == nullptr)
					pCurChar = pPrevChar;
				if(pPrevChar == nullptr || pCurChar == nullptr)
					continue;

				const CCharacterCore *pPrevCore = pPrevChar->Core();
				const CCharacterCore *pCurCore = pCurChar->Core();
				if(pPrevCore == nullptr || pCurCore == nullptr)
					continue;

				vec2 Aim = vec2(1.0f, 0.0f);
				CNetObj_PlayerInput PlaybackInput{};
				if(GetPlaybackInput(PlaybackInput, DummyIdx != 0))
				{
					Aim = vec2((float)PlaybackInput.m_TargetX, (float)PlaybackInput.m_TargetY);
					if(length(Aim) < 0.001f)
						Aim = vec2(1.0f, 0.0f);
				}
				const vec2 AimDir = normalize(Aim);

				auto FillPlaybackRenderObj = [&](CNetObj_Character &Obj, const CCharacter *pChar, const CCharacterCore *pCore) {
					Obj = CNetObj_Character{};
					Obj.m_Tick = Client()->PredGameTick(g_Config.m_ClDummy);
					Obj.m_X = (int)pChar->GetPos().x;
					Obj.m_Y = (int)pChar->GetPos().y;
					Obj.m_VelX = (int)(pCore->m_Vel.x * 256.0f);
					Obj.m_VelY = (int)(pCore->m_Vel.y * 256.0f);
					Obj.m_Angle = (int)(angle(AimDir) * 256.0f);
					Obj.m_Direction = pCore->m_Direction;
					Obj.m_Jumped = pCore->m_Jumped;
					Obj.m_HookState = pCore->m_HookState;
					Obj.m_HookX = (int)pCore->m_HookPos.x;
					Obj.m_HookY = (int)pCore->m_HookPos.y;
					Obj.m_HookedPlayer = -1;
					Obj.m_AttackTick = pChar->GetAttackTick();
					Obj.m_Weapon = pCore->m_ActiveWeapon;
					Obj.m_Emote = (pCore->m_FreezeEnd != 0 || pCore->m_DeepFrozen || pCore->m_LiveFrozen) ? EMOTE_PAIN : EMOTE_NORMAL;
					Obj.m_PlayerFlags = PLAYERFLAG_PLAYING;
				};

				FillPlaybackRenderObj(PrevObj, pPrevChar, pPrevCore);
				FillPlaybackRenderObj(CurObj, pCurChar, pCurCore);
			}

			CTeeRenderInfo TeeInfo = ClientData.m_RenderInfo;
			TeeInfo.m_Size = 64.0f;
			TeeInfo.m_TeeRenderFlags = 0;
			if(ClientData.m_Predicted.m_FreezeEnd != 0)
				TeeInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN | TEE_NO_WEAPON;
			else if(ClientData.m_Predicted.m_LiveFrozen)
				TeeInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN;
			if(ClientData.m_Predicted.m_Invincible)
				TeeInfo.m_TeeRenderFlags |= TEE_EFFECT_SPARKLE;
			TeeInfo.m_ColorBody.a = 1.0f;
			TeeInfo.m_ColorFeet.a = 1.0f;

			GameClient()->m_Players.RenderExternalHook(&PrevObj, &CurObj, &TeeInfo, LocalId, Intra);
			GameClient()->m_Players.RenderExternalPlayer(&PrevObj, &CurObj, &TeeInfo, LocalId, Intra);
		}
		return;
	}

	if(!m_Recording || !m_WorldInitialized || m_pWorld == nullptr)
		return;

	const float Intra = std::clamp(m_SmoothIntra, 0.0f, 1.0f);
	const int TasClientId = -3;
	const SFrame *pCurrentFrame = m_vFrames.empty() ? nullptr : &m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)];
	const SFrame *pNextFrame = m_vFrames.empty() ? nullptr : &m_vFrames[minimum(std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1) + 1, (int)m_vFrames.size() - 1)];
	const bool UseFramePositionsOnly = false;
	for(int DummyIdx = 0; DummyIdx < NUM_DUMMIES; ++DummyIdx)
	{
		const int LocalId = GameClient()->m_aLocalIds[DummyIdx];
		if(LocalId < 0)
			continue;

		CCharacter *pCommitted = m_pWorld->GetCharacterById(LocalId);
		CCharacter *pPredicted = m_pWorldPredicted != nullptr ? m_pWorldPredicted->GetCharacterById(LocalId) : nullptr;
		const CCharacterCore *pPrevCore = pCommitted != nullptr ? pCommitted->Core() : nullptr;
		const CCharacterCore *pCurCore = pPredicted != nullptr ? pPredicted->Core() : pPrevCore;
		const vec2 MousePos = GetRenderMousePos(DummyIdx != 0);
		const vec2 AimDir = normalize(length(MousePos) > 0.001f ? MousePos : vec2(1.0f, 0.0f));
		vec2 RenderPos = m_aHaveObservedInput[DummyIdx] ? m_aObservedPos[DummyIdx] : CurrentDummyPos(DummyIdx);

		if(pCurrentFrame != nullptr)
		{
			if(m_LegacySingleDummyFile)
			{
				if(DummyIdx == m_LegacyPlaybackDummy)
					RenderPos = pCurrentFrame->m_aPos[0];
			}
			else if(FrameHasDummy(*pCurrentFrame, DummyIdx))
			{
				RenderPos = pCurrentFrame->m_aPos[DummyIdx];
			}
		}

		if(UseFramePositionsOnly)
		{
			vec2 NextPos = RenderPos;
			if(pNextFrame != nullptr)
			{
				if(m_LegacySingleDummyFile)
				{
					if(DummyIdx == m_LegacyPlaybackDummy)
						NextPos = pNextFrame->m_aPos[0];
				}
				else if(FrameHasDummy(*pNextFrame, DummyIdx))
				{
					NextPos = pNextFrame->m_aPos[DummyIdx];
				}
			}
			RenderPos = mix(RenderPos, NextPos, Intra);
		}
		else if(pCommitted != nullptr)
		{
			const vec2 PrevPos = pCommitted->GetPos();
			const vec2 CurPos = pPredicted != nullptr ? pPredicted->GetPos() : PrevPos;
			RenderPos = mix(PrevPos, CurPos, Intra);
		}

		CNetObj_Character PrevObj{};
		CNetObj_Character CurObj{};
		if(pPrevCore != nullptr)
		{
			PrevObj.m_X = (int)pCommitted->GetPos().x;
			PrevObj.m_Y = (int)pCommitted->GetPos().y;
			PrevObj.m_VelX = (int)(pPrevCore->m_Vel.x * 256.0f);
			PrevObj.m_VelY = (int)(pPrevCore->m_Vel.y * 256.0f);
			PrevObj.m_Angle = (int)(angle(AimDir) * 256.0f);
			PrevObj.m_Direction = pPrevCore->m_Direction;
			PrevObj.m_Jumped = pPrevCore->m_Jumped;
			PrevObj.m_HookState = pPrevCore->m_HookState;
			PrevObj.m_HookX = (int)pPrevCore->m_HookPos.x;
			PrevObj.m_HookY = (int)pPrevCore->m_HookPos.y;
			PrevObj.m_HookedPlayer = -1;
			PrevObj.m_AttackTick = pCommitted->GetAttackTick();
			PrevObj.m_Weapon = pPrevCore->m_ActiveWeapon;
			PrevObj.m_Emote = (pPrevCore->m_FreezeEnd != 0 || pPrevCore->m_DeepFrozen || pPrevCore->m_LiveFrozen) ? EMOTE_PAIN : EMOTE_NORMAL;

			CurObj = PrevObj;
			if(pPredicted != nullptr && pCurCore != nullptr)
			{
				CurObj.m_X = (int)pPredicted->GetPos().x;
				CurObj.m_Y = (int)pPredicted->GetPos().y;
				CurObj.m_VelX = (int)(pCurCore->m_Vel.x * 256.0f);
				CurObj.m_VelY = (int)(pCurCore->m_Vel.y * 256.0f);
				CurObj.m_Direction = pCurCore->m_Direction;
				CurObj.m_Jumped = pCurCore->m_Jumped;
				CurObj.m_HookState = pCurCore->m_HookState;
				CurObj.m_HookX = (int)pCurCore->m_HookPos.x;
				CurObj.m_HookY = (int)pCurCore->m_HookPos.y;
				CurObj.m_HookedPlayer = -1;
				CurObj.m_AttackTick = pPredicted->GetAttackTick();
				CurObj.m_Weapon = pCurCore->m_ActiveWeapon;
				CurObj.m_Emote = (pCurCore->m_FreezeEnd != 0 || pCurCore->m_DeepFrozen || pCurCore->m_LiveFrozen) ? EMOTE_PAIN : EMOTE_NORMAL;
			}
		}

		CTeeRenderInfo TeeInfo = GameClient()->m_aClients[LocalId].m_RenderInfo;
		TeeInfo.m_Size = 64.0f;
		TeeInfo.m_TeeRenderFlags = 0;
		if(pCurCore != nullptr && (pCurCore->m_FreezeEnd != 0 || pCurCore->m_DeepFrozen))
			TeeInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN | TEE_NO_WEAPON;
		else if(pCurCore != nullptr && pCurCore->m_LiveFrozen)
			TeeInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN;
		TeeInfo.m_ColorBody.a = 1.0f;
		TeeInfo.m_ColorFeet.a = 1.0f;

		if(UseFramePositionsOnly)
		{
			const int Emote = (TeeInfo.m_TeeRenderFlags & TEE_EFFECT_FROZEN) != 0 ? EMOTE_PAIN : EMOTE_NORMAL;
			GameClient()->RenderTools()->RenderTee(CAnimState::GetIdle(), &TeeInfo, Emote, AimDir, RenderPos, 1.0f);
		}
		else if(pPrevCore != nullptr)
			GameClient()->m_Players.RenderExternalHook(&PrevObj, &CurObj, &TeeInfo, TasClientId, Intra);
		if(!UseFramePositionsOnly && pPrevCore != nullptr)
			GameClient()->m_Players.RenderExternalPlayer(&PrevObj, &CurObj, &TeeInfo, TasClientId, Intra);
		else if(!UseFramePositionsOnly)
		{
			const int Emote = (TeeInfo.m_TeeRenderFlags & TEE_EFFECT_FROZEN) != 0 ? EMOTE_PAIN : EMOTE_NORMAL;
			GameClient()->RenderTools()->RenderTee(CAnimState::GetIdle(), &TeeInfo, Emote, AimDir, RenderPos, 1.0f);
		}
	}
}

void CZzTas::RenderWorldCharacters()
{
	RenderTasWorld();
}

void CZzTas::RenderHud()
{
	if(!g_Config.m_ClZzTasHud || GameClient()->m_Menus.IsActive())
		return;
	if(!m_Recording && !m_Playing)
		return;

	const float Width = 300.0f * Graphics()->ScreenAspect();
	const float Height = 300.0f;
	Graphics()->MapScreen(0, 0, Width, Height);

	const float X = Width - 95.0f;
	const float Y = 94.0f;
	const float BoxW = 88.0f;
	const float BoxH = 64.0f;
	CUIRect HudRect = {X, Y, BoxW, BoxH};
	HudRect.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.34f), IGraphics::CORNER_ALL, 3.0f);

	const int TickValue = m_Playing ? maximum(0, m_PlayCursor) : (int)m_vFrames.size();
	const float Seconds = m_vFrames.empty() ? 0.0f : TickValue / maximum(1.0f, GetStepRate());
	char aLine1[64];
	str_format(aLine1, sizeof(aLine1), "%s  %.2fs", m_Recording ? "REC" : "PLAY", Seconds);

	int HudDummy = CurrentDummyIndex();
	if(m_Playing && !m_vFrames.empty())
	{
		const SFrame &Frame = m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)];
		HudDummy = DisplayDummyForFrame(Frame);
	}

	const CNetObj_PlayerInput *pHudInput = nullptr;
	CNetObj_PlayerInput HudInput{};
	if(m_Playing && GetPlaybackInput(HudInput, HudDummy != 0))
		pHudInput = &HudInput;
	else if(m_Recording)
	{
		TInputArray aInputs{};
		TVec2Array aMice{};
		BuildCurrentTasInputs(aInputs, aMice);
		HudInput = aInputs[HudDummy];
		pHudInput = &HudInput;
	}

	const int LocalId = GameClient()->m_aLocalIds[HudDummy];
	CCharacter *pTasChar = (m_WorldInitialized && m_pWorld != nullptr && LocalId >= 0) ? m_pWorld->GetCharacterById(LocalId) : nullptr;
	int Cooldown = 0;
	const char *pWeaponName = "HAM";
	if(pTasChar != nullptr)
	{
		static const char *s_apWeaponNames[] = {"HAM", "GUN", "SG", "GR", "LAS", "NIN"};
		const int Weapon = std::clamp(pTasChar->GetActiveWeapon(), 0, NUM_WEAPONS - 1);
		pWeaponName = s_apWeaponNames[Weapon];
		const int Zone = pTasChar->GetOverriddenTuneZone();
		const CTuningParams *pTuning = &GameClient()->m_GameWorld.TuningList()[std::clamp(Zone, 0, TuneZone::NUM - 1)];
		if(pTuning != nullptr)
		{
			const int FireDelayTicks = maximum(0, round_to_int(pTuning->GetWeaponFireDelay(Weapon) * Client()->GameTickSpeed()));
			Cooldown = maximum(0, pTasChar->GetAttackTick() + FireDelayTicks - (m_pWorld != nullptr ? m_pWorld->m_GameTick : Client()->GameTick(HudDummy)));
		}
	}

	char aLine2[64];
	str_format(aLine2, sizeof(aLine2), "%s CD:%d", pWeaponName, Cooldown);

	char aLine3[96];
	str_format(
		aLine3,
		sizeof(aLine3),
		"D:%d J:%d H:%d F:%d",
		pHudInput != nullptr ? pHudInput->m_Direction : 0,
		pHudInput != nullptr && pHudInput->m_Jump ? 1 : 0,
		pHudInput != nullptr && pHudInput->m_Hook ? 1 : 0,
		pHudInput != nullptr && pHudInput->m_Fire ? 1 : 0);

	TextRender()->TextColor(m_Recording ? ColorRGBA(1.0f, 0.35f, 0.35f, 1.0f) : ColorRGBA(0.65f, 0.9f, 1.0f, 1.0f));
	TextRender()->Text(X + 4.0f, Y + 4.0f, 5.0f, aLine1, -1.0f);
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.95f);
	TextRender()->Text(X + 4.0f, Y + 16.0f, 4.8f, aLine2, -1.0f);
	TextRender()->Text(X + 4.0f, Y + 27.0f, 4.6f, aLine3, -1.0f);
	if(pTasChar != nullptr)
	{
		char aLine4[96];
		str_format(aLine4, sizeof(aLine4), "G:%d FR:%d", pTasChar->IsGrounded() ? 1 : 0, pTasChar->m_FreezeTime);
		TextRender()->Text(X + 4.0f, Y + 38.0f, 4.6f, aLine4, -1.0f);
	}
	if(m_Paused)
		TextRender()->Text(X + 4.0f, Y + 50.0f, 4.6f, "Paused", -1.0f);
	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CZzTas::OnRender()
{
	if(!g_Config.m_ClZzTasEnable)
		return;

	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if((m_Recording || m_Playing) && !m_WorldInitialized)
		EnsureWorldInitialized();
	SyncWorldFromPredictedIfNeeded();

	if((m_Recording || m_Playing) && m_WorldInitialized)
	{
		const float Delta = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.1f);
		const bool Rewinding = g_Config.m_ClZzTasRewind != 0;
		const bool Forwarding = g_Config.m_ClZzTasForward != 0;
		bool DidSeek = false;

		if(Rewinding)
		{
			const float SeekRate = maximum(1.0f, m_Playing ? GetStepRate() : (float)std::clamp(g_Config.m_ClZzTasTicksPerSecond, 1, 50));
			m_Accumulator -= Delta * SeekRate;
			while(m_Accumulator <= 0.0)
			{
				m_Accumulator += 1.0;
				if(m_Playing)
					StepPlayback(-1, 1);
				else
					RewindRecording(1);
			}
			DidSeek = true;
			if(g_Config.m_ClZzTasTickControlPause)
				g_Config.m_ClZzTasPause = 1;
		}
		else if(Forwarding)
		{
			const float SeekRate = 50.0f;
			m_Accumulator += Delta * SeekRate;
			while(m_Accumulator >= 1.0)
			{
				m_Accumulator -= 1.0;
				if(m_Playing)
					StepPlayback(1, 1);
				else
					StepForwardRecording();
			}
			DidSeek = true;
			if(g_Config.m_ClZzTasTickControlPause)
				g_Config.m_ClZzTasPause = 1;
		}

		if(!DidSeek)
			m_Accumulator += Delta * GetStepRate();

		while(!DidSeek && !m_Paused && m_Accumulator >= 1.0)
		{
			m_Accumulator -= 1.0;
			if(m_Recording)
			{
				TInputArray aInputs{};
				TVec2Array aMice{};
				BuildCurrentTasInputs(aInputs, aMice);
				TickWorld(aInputs, aMice);
				const int TasTick = m_pWorld != nullptr ? m_pWorld->m_GameTick : (m_LastRecordTasTick + 1);
				if(TasTick != m_LastRecordTasTick)
				{
					CaptureRecordedWorldState();
					m_vFrames.push_back(CaptureFrameFromWorld(TasTick, aInputs, aMice));
					m_LastRecordTasTick = TasTick;
					m_aLatchedJump.fill(false);
					const int64_t AutoSaveInterval = (int64_t)maximum(1, g_Config.m_ClZzTasAutoSaveInterval) * time_freq();
					if(g_Config.m_ClZzTasAutoSave && time_get() > m_LastAutoSave + AutoSaveInterval)
					{
						char aPath[IO_MAX_PATH_LENGTH];
						if(GetReplaySavePath(aPath, sizeof(aPath), true))
							SaveReplayToPath(aPath);
						m_LastAutoSave = time_get();
					}
				}
			}
			else if(m_Playing && !m_vFrames.empty())
			{
				if(!HasPlaybackWorldStates())
				{
					const int NextIndex = std::clamp(m_PlayCursor + 1, 0, (int)m_vFrames.size() - 1);
					const SFrame &Frame = m_vFrames[NextIndex];
					TInputArray aInputs{};
					TVec2Array aMice{};
					BuildPlaybackTasInputs(Frame, aInputs, aMice);
					TickWorld(aInputs, aMice);
				}
				AdvancePlaybackOneStep();
				if(m_Playing && HasPlaybackWorldStates())
					RestorePlaybackCursorState();
				if(!m_Playing)
					break;
			}
		}
		m_SmoothIntra = std::clamp((float)m_Accumulator, 0.0f, 1.0f);
		if(m_Recording)
		{
			const bool FrozenNow = IsLocalFrozenInWorld();
			if(FrozenNow && !m_WasFrozenInWorld)
			{
				if(g_Config.m_ClZzTasAutoRewind)
				{
					RewindRecording(GetTasStepAmount());
					if(g_Config.m_ClZzTasTickControlPause)
						g_Config.m_ClZzTasPause = 1;
				}
				else if(g_Config.m_ClZzTasAutoForward)
				{
					AutoForwardRecording();
					if(g_Config.m_ClZzTasTickControlPause)
						g_Config.m_ClZzTasPause = 1;
				}
			}
			m_WasFrozenInWorld = IsLocalFrozenInWorld();
		}
		else
		{
			m_WasFrozenInWorld = false;
		}

		if(m_Recording)
		{
			TInputArray aInputs{};
			TVec2Array aMice{};
			BuildCurrentTasInputs(aInputs, aMice);
			UpdatePredictedWorld(aInputs, aMice);
		}
		else if(m_Playing && !m_vFrames.empty())
		{
			if(HasPlaybackWorldStates())
				RestorePlaybackCursorState();
			else
			{
				const SFrame &Frame = m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)];
				TInputArray aInputs{};
				TVec2Array aMice{};
				BuildPlaybackTasInputs(Frame, aInputs, aMice);
				UpdatePredictedWorld(aInputs, aMice);
			}
		}
	}

	Graphics()->MapScreenToInterface(GameClient()->m_Camera.m_Center.x, GameClient()->m_Camera.m_Center.y, GameClient()->m_Camera.m_Zoom);
	RenderTasWorld();
	RenderStartEndPos();
	RenderReplayPath();
	RenderPredictionPath();
	RenderHud();
}

bool CZzTas::EnsureFolder() const
{
	if(Storage()->FolderExists(TAS_FOLDER, IStorage::TYPE_SAVE))
		return true;
	return Storage()->CreateFolder(TAS_FOLDER, IStorage::TYPE_SAVE);
}

bool CZzTas::ResolveFilename(const char *pName, char *pOutPath, int OutSize) const
{
	if(!pName || !pName[0])
		return false;

	char aSafe[IO_MAX_PATH_LENGTH];
	str_copy(aSafe, pName, sizeof(aSafe));
	for(char *p = aSafe; *p; ++p)
	{
		if(*p == '/' || *p == '\\' || *p == ':')
			*p = '_';
	}

	if(!str_endswith(aSafe, ".tas"))
		str_append(aSafe, ".tas", sizeof(aSafe));

	char aRel[IO_MAX_PATH_LENGTH];
	str_format(aRel, sizeof(aRel), "%s/%s", TAS_FOLDER, aSafe);
	Storage()->GetCompletePath(IStorage::TYPE_SAVE, aRel, pOutPath, OutSize);
	return true;
}

int CZzTas::TasListFetchCallback(const CFsFileInfo *pInfo, int IsDir, int StorageType, void *pUser)
{
	(void)StorageType;
	CZzTas *pSelf = static_cast<CZzTas *>(pUser);
	if(IsDir || !pInfo || !pInfo->m_pName || !str_endswith(pInfo->m_pName, ".tas"))
		return 0;

	STasFileItem Item{};
	str_copy(Item.m_aFilename, pInfo->m_pName, sizeof(Item.m_aFilename));
	Item.m_IsDir = false;
	Item.m_TimeModified = pInfo->m_TimeModified;
	pSelf->m_vFiles.push_back(Item);
	return 0;
}

void CZzTas::RefreshFileList()
{
	m_vFiles.clear();
	if(!EnsureFolder())
		return;
	Storage()->ListDirectoryInfo(IStorage::TYPE_SAVE, TAS_FOLDER, TasListFetchCallback, this);
	std::sort(m_vFiles.begin(), m_vFiles.end(), [](const STasFileItem &A, const STasFileItem &B) {
		return str_comp(A.m_aFilename, B.m_aFilename) < 0;
	});
}

bool CZzTas::StartRecord(const char *pName)
{
	if(pName && pName[0] != '\0')
	{
		str_copy(g_Config.m_ClZzTasReplayName, pName, sizeof(g_Config.m_ClZzTasReplayName));
		str_copy(g_Config.m_ClZzTasLastFile, pName, sizeof(g_Config.m_ClZzTasLastFile));
	}
	g_Config.m_ClZzTasRecordReplay = 0;
	StartRecording();
	return true;
}

void CZzTas::StopRecord()
{
	StopRecording();
}

bool CZzTas::StartPlayback(const char *pName)
{
	if(pName && pName[0] != '\0')
	{
		str_copy(g_Config.m_ClZzTasReplayName, pName, sizeof(g_Config.m_ClZzTasReplayName));
		str_copy(g_Config.m_ClZzTasLastFile, pName, sizeof(g_Config.m_ClZzTasLastFile));
	}
	return StartPlayback();
}

bool CZzTas::Save(const char *pName)
{
	if(pName && pName[0] != '\0')
	{
		str_copy(g_Config.m_ClZzTasReplayName, pName, sizeof(g_Config.m_ClZzTasReplayName));
		str_copy(g_Config.m_ClZzTasLastFile, pName, sizeof(g_Config.m_ClZzTasLastFile));
	}
	char aPath[IO_MAX_PATH_LENGTH];
	if(!GetReplaySavePath(aPath, sizeof(aPath), false))
		return false;
	const bool Ok = SaveReplayToPath(aPath);
	if(Ok)
	{
		str_copy(m_aLoadedName, g_Config.m_ClZzTasReplayName, sizeof(m_aLoadedName));
		RefreshFileList();
	}
	return Ok;
}

bool CZzTas::Load(const char *pName)
{
	if(pName && pName[0] != '\0')
	{
		str_copy(g_Config.m_ClZzTasReplayName, pName, sizeof(g_Config.m_ClZzTasReplayName));
		str_copy(g_Config.m_ClZzTasLastFile, pName, sizeof(g_Config.m_ClZzTasLastFile));
	}
	char aPath[IO_MAX_PATH_LENGTH];
	if(!FindReplayPath(aPath, sizeof(aPath), false))
		return false;
	const bool Ok = LoadReplayFromPath(aPath);
	if(Ok)
		str_copy(m_aLoadedName, g_Config.m_ClZzTasReplayName, sizeof(m_aLoadedName));
	return Ok;
}

void CZzTas::ConTasRecordStart(IConsole::IResult *pResult, void *pUserData)
{
	CZzTas *pSelf = static_cast<CZzTas *>(pUserData);
	const char *pName = pResult->NumArguments() > 0 ? pResult->GetString(0) : "";
	pSelf->StartRecord(pName);
}

void CZzTas::ConTasRecordStop(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CZzTas *>(pUserData)->StopRecord();
}

void CZzTas::ConTasPlayStart(IConsole::IResult *pResult, void *pUserData)
{
	CZzTas *pSelf = static_cast<CZzTas *>(pUserData);
	const char *pName = pResult->NumArguments() > 0 ? pResult->GetString(0) : "";
	pSelf->StartPlayback(pName);
}

void CZzTas::ConTasPlayStop(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CZzTas *>(pUserData)->StopPlayback();
}

void CZzTas::ConTasSave(IConsole::IResult *pResult, void *pUserData)
{
	CZzTas *pSelf = static_cast<CZzTas *>(pUserData);
	const char *pName = pResult->NumArguments() > 0 ? pResult->GetString(0) : "";
	pSelf->Save(pName);
}

void CZzTas::ConTasLoad(IConsole::IResult *pResult, void *pUserData)
{
	CZzTas *pSelf = static_cast<CZzTas *>(pUserData);
	const char *pName = pResult->NumArguments() > 0 ? pResult->GetString(0) : "";
	pSelf->Load(pName);
}

void CZzTas::ConTasUnload(IConsole::IResult *pResult, void *pUserData)
{
	(void)pResult;
	static_cast<CZzTas *>(pUserData)->UnloadReplay();
}


