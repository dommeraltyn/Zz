/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_MUSIC_SMTC_H
#define GAME_CLIENT_COMPONENTS_MUSIC_SMTC_H

#include <game/client/component.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>

class CMusicSMTC : public CComponent
{
public:
	struct STrackInfo
	{
		static constexpr int ARTWORK_DIM = 36;
		static constexpr int VISUALIZER_BARS = 4;

		bool m_Available = false;
		bool m_Playing = false;
		std::string m_Title;
		std::string m_Artist;
		bool m_HasArtwork = false;
		uint64_t m_ArtworkRevision = 0;
		std::array<uint8_t, ARTWORK_DIM * ARTWORK_DIM * 4> m_aArtworkRgba{};
		float m_AccentR = 0.34f;
		float m_AccentG = 0.53f;
		float m_AccentB = 0.79f;
		bool m_HasVisualizer = false;
		std::array<float, VISUALIZER_BARS> m_aVisualizer{};
	};

	CMusicSMTC();
	~CMusicSMTC() override;

	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;
	void OnShutdown() override;
	void OnUpdate() override;

	STrackInfo GetTrackInfo() const;

	void TogglePlayPause();
	void Next();
	void Previous();

private:
	class CWorker;

	static void ThreadMain(void *pUser);

	void StartWorker();
	void StopWorker();

	mutable std::mutex m_Mutex;
	STrackInfo m_TrackInfo;

	std::atomic_bool m_StopRequested{false};
	std::atomic_bool m_WorkerRunning{false};
	void *m_pThread = nullptr;
	void *m_pWorker = nullptr;
};

#endif // GAME_CLIENT_COMPONENTS_MUSIC_SMTC_H
