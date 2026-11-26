#ifndef GAME_CLIENT_COMPONENTS_ZZ_TAS_H
#define GAME_CLIENT_COMPONENTS_ZZ_TAS_H

#include <generated/protocol.h>

#include <engine/client/enums.h>
#include <engine/console.h>

#include <game/client/component.h>
#include <game/client/prediction/gameworld.h>

#include <base/system.h>
#include <base/vmath.h>

#include <array>
#include <vector>

class CZzTas : public CComponent
{
public:
	struct SFrame
	{
		int m_Tick = 0;
		int m_DummyMask = 0;
		int m_ControlDummy = 0;
		std::array<CNetObj_PlayerInput, NUM_DUMMIES> m_aInput{};
		std::array<vec2, NUM_DUMMIES> m_aMouse{};
		std::array<vec2, NUM_DUMMIES> m_aPos{};
	};

	struct SRecordedWorldState
	{
		int m_GameTick = 0;
		int m_DummyMask = 0;
		std::array<CCharacterCore, NUM_DUMMIES> m_aCore{};
		std::array<vec2, NUM_DUMMIES> m_aPos{};
		std::array<vec2, NUM_DUMMIES> m_aPrevPos{};
		std::array<int, NUM_DUMMIES> m_aFreezeTime{};
		std::array<int, NUM_DUMMIES> m_aReloadTimer{};
		std::array<int, NUM_DUMMIES> m_aAttackTick{};
	};

	using TInputArray = std::array<CNetObj_PlayerInput, NUM_DUMMIES>;
	using TVec2Array = std::array<vec2, NUM_DUMMIES>;

	CZzTas() = default;
	int Sizeof() const override { return sizeof(*this); }

	void OnConsoleInit() override;
	void OnReset() override;
	void OnStateChange(int NewState, int OldState) override;
	void OnUpdate() override;
	void OnRender() override;
	void ProcessFinalSnapInput(CNetObj_PlayerInput &Input, bool Dummy);

	void ObserveInput(const CNetObj_PlayerInput &Input, vec2 Pos, bool Dummy);
	bool GetPlaybackInput(CNetObj_PlayerInput &Out) const;
	bool GetPlaybackInput(CNetObj_PlayerInput &Out, bool Dummy) const;
	bool HasRenderMouseOverride() const;
	int GetDisplayDummyIndex() const;
	vec2 GetRenderMousePos() const;
	vec2 GetRenderMousePos(bool Dummy) const;
	bool IsWorldActive() const;
	bool IsWorldInitialized() const { return m_WorldInitialized; }
	vec2 GetInterpolatedPlayerPos() const;

	void SaveReplay();
	void LoadSelectedReplay();
	void UnloadReplay();
	void ValidateReplay();
	void ReportReplayTime();
	void RemoveUseless();
	void SyncNow();
	bool StartRecord(const char *pName);
	void StopRecord();
	bool StartPlayback(const char *pName);
	void StopPlayback();
	bool Save(const char *pName);
	bool Load(const char *pName);
	void RefreshFileList();

	bool IsRecording() const { return m_Recording; }
	bool IsPlaying() const { return m_Playing; }
	bool IsPaused() const { return m_Paused; }
	int NumFrames() const { return (int)m_vFrames.size(); }
	int PlaybackCursor() const { return m_PlayCursor; }
	bool HasStartPosition() const { return !m_vFrames.empty(); }
	vec2 GetStartPosition() const;
	float GetDistanceToStartPosition() const;
	bool IsAtStartPosition() const;
	float StartPositionTolerance() const;
	float GetStepRate() const;
	void RenderWorldCharacters();
	const char *LoadedName() const { return m_aLoadedName; }

	struct STasFileItem
	{
		char m_aFilename[IO_MAX_PATH_LENGTH];
		bool m_IsDir;
		time_t m_TimeModified;
	};

	const std::vector<STasFileItem> &Files() const { return m_vFiles; }

private:
	static constexpr const char *TAS_FOLDER = "ZZTas";

	std::vector<SFrame> m_vFrames;
	std::vector<STasFileItem> m_vFiles;
	std::vector<SRecordedWorldState> m_vRecordedWorldStates;
	SRecordedWorldState m_InitialWorldState{};
	CGameWorld *m_pWorld = nullptr;
	CGameWorld *m_pWorldPredicted = nullptr;
	int m_PlayCursor = 0;
	int m_LastRecordTasTick = -1;
	int64_t m_LastAutoSave = 0;
	bool m_Recording = false;
	bool m_Playing = false;
	bool m_Paused = false;
	bool m_WorldInitialized = false;
	bool m_WasFrozenInWorld = false;
	bool m_HasInitialWorldState = false;
	bool m_LegacySingleDummyFile = false;
	double m_Accumulator = 0.0;
	int64_t m_LastSeekTime = 0;
	float m_SmoothIntra = 0.0f;
	int m_PlaybackStepRateOverride = 0;
	int m_LegacyPlaybackDummy = 0;
	int m_PlaybackOriginalDummy = 0;
	std::array<bool, NUM_DUMMIES> m_aHaveObservedInput{};
	std::array<bool, NUM_DUMMIES> m_aHaveFinalInput{};
	std::array<bool, NUM_DUMMIES> m_aLatchedJump{};
	std::array<CNetObj_PlayerInput, NUM_DUMMIES> m_aObservedInput{};
	std::array<CNetObj_PlayerInput, NUM_DUMMIES> m_aFinalInput{};
	std::array<vec2, NUM_DUMMIES> m_aObservedMouse{};
	std::array<vec2, NUM_DUMMIES> m_aFinalMouse{};
	std::array<vec2, NUM_DUMMIES> m_aObservedPos{};
	std::array<int, NUM_DUMMIES> m_aFxLastAttackTick{};
	std::array<bool, NUM_DUMMIES> m_aFxLastFrozen{};
	bool m_SuppressTasEffects = false;
	char m_aLoadedName[IO_MAX_PATH_LENGTH] = "";

	void ClearReplay(bool KillPlayer);
	void StartRecording();
	void StopRecording();
	bool StartPlayback();
	void StepPlayback(int Direction, int Amount);
	void AdvancePlaybackOneStep();
	void EnsureWorldInitialized();
	void ShutdownWorld();
	void ResetWorldFromPredicted();
	bool HasLocalCharactersInWorld(CGameWorld *pWorld) const;
	void SyncWorldFromPredictedIfNeeded();
	void RestorePlaybackStartState();
	void ResimulatePlaybackWorld();
	void RewindRecording(int Amount);
	void StepForwardRecording();
	void AutoForwardRecording();
	bool IsLocalFrozenInWorld() const;
	void TickWorld(const TInputArray &aInputs, const TVec2Array &aMice);
	void UpdatePredictedWorld(const TInputArray &aInputs, const TVec2Array &aMice);
	bool SaveReplayToPath(const char *pPath) const;
	bool LoadReplayFromPath(const char *pPath);
	bool FindReplayPath(char *pBuf, int BufSize, bool ExactStemOnly) const;
	bool GetReplaySavePath(char *pBuf, int BufSize, bool AutoSave) const;
	void PushTasWarning(const char *pTitle, const char *pMessage) const;
	void RenderTasWorld();
	void RenderStartEndPos();
	void RenderReplayPath();
	void RenderPredictionPath();
	void RenderHud();
	void BuildCurrentTasInputs(TInputArray &aInputs, TVec2Array &aMice) const;
	void BuildPlaybackTasInputs(const SFrame &Frame, TInputArray &aInputs, TVec2Array &aMice) const;
	SFrame CaptureFrameFromWorld(int TasTick, const TInputArray &aInputs, const TVec2Array &aMice) const;
	bool BuildRecordedWorldState(SRecordedWorldState &State) const;
	void CaptureRecordedWorldState();
	void ApplyRecordedWorldState(CGameWorld *pWorld, const SRecordedWorldState &State) const;
	void RestoreRecordedWorldState(const SRecordedWorldState &State);
	bool HasPlaybackWorldStates() const;
	void RestorePlaybackCursorState();
	void ResetObservedInputs();
	void ObserveFinalSnapInput(const CNetObj_PlayerInput &Input, bool Dummy);
	void SyncPlaybackControlDummy();
	int CurrentDummyIndex() const;
	bool FrameHasDummy(const SFrame &Frame, int Dummy) const;
	int FirstDummyInMask(int DummyMask) const;
	int DisplayDummyForFrame(const SFrame &Frame) const;
	vec2 CurrentDummyPos(int Dummy) const;
	int RecordedDummyMask() const;

	static void ConTasRecordStart(IConsole::IResult *pResult, void *pUserData);
	static void ConTasRecordStop(IConsole::IResult *pResult, void *pUserData);
	static void ConTasPlayStart(IConsole::IResult *pResult, void *pUserData);
	static void ConTasPlayStop(IConsole::IResult *pResult, void *pUserData);
	static void ConTasSave(IConsole::IResult *pResult, void *pUserData);
	static void ConTasLoad(IConsole::IResult *pResult, void *pUserData);
	static void ConTasUnload(IConsole::IResult *pResult, void *pUserData);
	static int TasListFetchCallback(const CFsFileInfo *pInfo, int IsDir, int StorageType, void *pUser);

	bool EnsureFolder() const;
	bool ResolveFilename(const char *pName, char *pOutPath, int OutSize) const;
};

#endif

