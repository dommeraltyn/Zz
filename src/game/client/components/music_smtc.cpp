/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "music_smtc.h"

#include <base/system.h>
#include <base/thread.h>
#include <base/time.h>
#include <base/math.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <thread>

#if defined(CONF_FAMILY_WINDOWS)

#include <base/windows.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#if !defined(NOBITMAP)
#define ZZ_SMTC_DEFINED_NOBITMAP
#define NOBITMAP
#endif
#define IStorage ZZSMTCWin32IStorage
#include <audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#undef IStorage
#if defined(ZZ_SMTC_DEFINED_NOBITMAP)
#undef ZZ_SMTC_DEFINED_NOBITMAP
#undef NOBITMAP
#endif

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;

namespace
{
	static constexpr int SMTC_ART_DIM = CMusicSMTC::STrackInfo::ARTWORK_DIM;

	enum class ECommand : int
	{
		NONE = 0,
		TOGGLE_PLAY_PAUSE,
		NEXT,
		PREV,
	};

	static void SetIfDifferent(std::string &Dst, const std::string &Src)
	{
		if(Dst != Src)
			Dst = Src;
	}

	static std::string Utf8FromHString(const winrt::hstring &Str)
	{
		auto Opt = windows_wide_to_utf8(Str.c_str());
		return Opt.value_or("");
	}

	class CSystemAudioAnalyzer
	{
	public:
		~CSystemAudioAnalyzer()
		{
			Shutdown();
		}

		void Shutdown()
		{
			if(m_pAudioClient)
				m_pAudioClient->Stop();
			m_pCaptureClient = nullptr;
			m_pAudioClient = nullptr;
			m_pDevice = nullptr;
			m_pEnumerator = nullptr;
			m_Initialized = false;
			m_SampleRate = 48000;
			m_Channels = 2;
			m_SampleFormat = ESampleFormat::NONE;
			m_LowState = 0.0f;
			m_MidState = 0.0f;
			m_AirState = 0.0f;
			m_Agc = 0.06f;
			for(float &Value : m_aOutLevels)
				Value = 0.0f;
		}

		bool Poll(std::array<float, CMusicSMTC::STrackInfo::VISUALIZER_BARS> &aOutLevels)
		{
			if(!EnsureInitialized())
			{
				aOutLevels = m_aOutLevels;
				return false;
			}

			UINT32 PacketLength = 0;
			HRESULT Hr = m_pCaptureClient->GetNextPacketSize(&PacketLength);
			if(FAILED(Hr))
			{
				HandleCaptureFailure(Hr);
				aOutLevels = m_aOutLevels;
				return false;
			}

			bool GotFrames = false;
			while(SUCCEEDED(Hr) && PacketLength > 0)
			{
				BYTE *pData = nullptr;
				UINT32 NumFrames = 0;
				DWORD Flags = 0;
				Hr = m_pCaptureClient->GetBuffer(&pData, &NumFrames, &Flags, nullptr, nullptr);
				if(FAILED(Hr))
				{
					HandleCaptureFailure(Hr);
					aOutLevels = m_aOutLevels;
					return false;
				}

				if(!(Flags & AUDCLNT_BUFFERFLAGS_SILENT) && pData != nullptr && NumFrames > 0)
				{
					ProcessFrames(pData, NumFrames);
					GotFrames = true;
				}

				m_pCaptureClient->ReleaseBuffer(NumFrames);
				Hr = m_pCaptureClient->GetNextPacketSize(&PacketLength);
			}

			if(!GotFrames)
			{
				for(float &Value : m_aOutLevels)
					Value *= 0.94f;
			}

			aOutLevels = m_aOutLevels;
			return GotFrames;
		}

	private:
		enum class ESampleFormat
		{
			NONE,
			FLOAT32,
			PCM16,
		};

		winrt::com_ptr<IMMDeviceEnumerator> m_pEnumerator;
		winrt::com_ptr<IMMDevice> m_pDevice;
		winrt::com_ptr<IAudioClient> m_pAudioClient;
		winrt::com_ptr<IAudioCaptureClient> m_pCaptureClient;
		bool m_Initialized = false;
		ESampleFormat m_SampleFormat = ESampleFormat::NONE;
		int m_SampleRate = 48000;
		int m_Channels = 2;
		float m_LowState = 0.0f;
		float m_MidState = 0.0f;
		float m_AirState = 0.0f;
		float m_Agc = 0.06f;
		std::array<float, CMusicSMTC::STrackInfo::VISUALIZER_BARS> m_aOutLevels{};

		void HandleCaptureFailure(HRESULT Hr)
		{
			if(Hr == AUDCLNT_E_DEVICE_INVALIDATED || Hr == AUDCLNT_E_SERVICE_NOT_RUNNING || Hr == AUDCLNT_E_RESOURCES_INVALIDATED)
				Shutdown();
		}

		bool DetectSampleFormat(const WAVEFORMATEX *pFormat)
		{
			if(!pFormat)
				return false;

			m_Channels = maximum(1, (int)pFormat->nChannels);
			m_SampleRate = maximum(8000, (int)pFormat->nSamplesPerSec);
			m_SampleFormat = ESampleFormat::NONE;

			if(pFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && pFormat->wBitsPerSample == 32)
				m_SampleFormat = ESampleFormat::FLOAT32;
			else if(pFormat->wFormatTag == WAVE_FORMAT_PCM && pFormat->wBitsPerSample == 16)
				m_SampleFormat = ESampleFormat::PCM16;
			else if(pFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pFormat->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
			{
				const auto *pExtensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(pFormat);
				if(pExtensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && pFormat->wBitsPerSample == 32)
					m_SampleFormat = ESampleFormat::FLOAT32;
				else if(pExtensible->SubFormat == KSDATAFORMAT_SUBTYPE_PCM && pFormat->wBitsPerSample == 16)
					m_SampleFormat = ESampleFormat::PCM16;
			}

			return m_SampleFormat != ESampleFormat::NONE;
		}

		bool EnsureInitialized()
		{
			if(m_Initialized && m_pAudioClient && m_pCaptureClient)
				return true;

			Shutdown();

			if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(m_pEnumerator.put()))))
				return false;
			if(FAILED(m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, m_pDevice.put())))
				return false;
			if(FAILED(m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(m_pAudioClient.put()))))
				return false;

			WAVEFORMATEX *pMixFormat = nullptr;
			if(FAILED(m_pAudioClient->GetMixFormat(&pMixFormat)))
				return false;

			const bool SupportedFormat = DetectSampleFormat(pMixFormat);
			const HRESULT InitHr = m_pAudioClient->Initialize(
				AUDCLNT_SHAREMODE_SHARED,
				AUDCLNT_STREAMFLAGS_LOOPBACK,
				0,
				0,
				pMixFormat,
				nullptr);
			CoTaskMemFree(pMixFormat);
			if(!SupportedFormat || FAILED(InitHr))
				return false;

			if(FAILED(m_pAudioClient->GetService(IID_PPV_ARGS(m_pCaptureClient.put()))))
				return false;
			if(FAILED(m_pAudioClient->Start()))
				return false;

			m_Initialized = true;
			return true;
		}

		float ReadMonoSample(const BYTE *pData, UINT32 FrameIndex) const
		{
			const int Channels = maximum(1, m_Channels);
			float Sum = 0.0f;
			if(m_SampleFormat == ESampleFormat::FLOAT32)
			{
				const auto *pSamples = reinterpret_cast<const float *>(pData);
				for(int Ch = 0; Ch < Channels; ++Ch)
					Sum += pSamples[FrameIndex * Channels + Ch];
				return Sum / Channels;
			}
			if(m_SampleFormat == ESampleFormat::PCM16)
			{
				const auto *pSamples = reinterpret_cast<const int16_t *>(pData);
				for(int Ch = 0; Ch < Channels; ++Ch)
					Sum += pSamples[FrameIndex * Channels + Ch] / 32768.0f;
				return Sum / Channels;
			}
			return 0.0f;
		}

		void ProcessFrames(const BYTE *pData, UINT32 NumFrames)
		{
			if(!pData || NumFrames == 0)
				return;

			const float Dt = 1.0f / maximum(8000.0f, (float)m_SampleRate);
			const float RC200 = 1.0f / (2.0f * pi * 200.0f);
			const float RC2000 = 1.0f / (2.0f * pi * 2000.0f);
			const float RC6000 = 1.0f / (2.0f * pi * 6000.0f);
			const float Alpha200 = Dt / (RC200 + Dt);
			const float Alpha2000 = Dt / (RC2000 + Dt);
			const float Alpha6000 = Dt / (RC6000 + Dt);

			float AccBass = 0.0f;
			float AccMid = 0.0f;
			float AccTreble = 0.0f;
			float AccAir = 0.0f;
			for(UINT32 Frame = 0; Frame < NumFrames; ++Frame)
			{
				const float X = ReadMonoSample(pData, Frame);
				m_LowState += Alpha200 * (X - m_LowState);
				const float HighPass200 = X - m_LowState;
				m_MidState += Alpha2000 * (HighPass200 - m_MidState);
				const float Treble = HighPass200 - m_MidState;
				m_AirState += Alpha6000 * (Treble - m_AirState);
				const float Air = Treble - m_AirState;

				AccBass += absolute(m_LowState);
				AccMid += absolute(m_MidState);
				AccTreble += absolute(Treble);
				AccAir += absolute(Air);
			}

			const float InvFrames = 1.0f / maximum(1.0f, (float)NumFrames);
			float RawBass = AccBass * InvFrames;
			float RawMid = AccMid * InvFrames;
			float RawTreble = AccTreble * InvFrames;
			float RawAir = AccAir * InvFrames;

			const float RawTotal = (RawBass + RawMid + RawTreble + RawAir) * 0.25f;
			m_Agc = m_Agc * 0.992f + RawTotal * 0.008f;
			const float NormBase = maximum(m_Agc, 0.01f);

			const float TargetBass = std::clamp(RawBass / (NormBase * 1.20f), 0.0f, 1.0f);
			const float TargetMid = std::clamp(RawMid / (NormBase * 1.12f), 0.0f, 1.0f);
			const float TargetTreble = std::clamp(RawTreble / (NormBase * 1.05f), 0.0f, 1.0f);
			const float TargetAir = std::clamp(RawAir / (NormBase * 0.92f), 0.0f, 1.0f);

			const float Rise = 0.32f;
			const float Fall = 0.12f;
			const float Targets[CMusicSMTC::STrackInfo::VISUALIZER_BARS] = {
				TargetBass,
				TargetMid,
				TargetTreble,
				TargetAir};
			for(int i = 0; i < CMusicSMTC::STrackInfo::VISUALIZER_BARS; ++i)
			{
				const float Speed = Targets[i] > m_aOutLevels[i] ? Rise : Fall;
				m_aOutLevels[i] = m_aOutLevels[i] + (Targets[i] - m_aOutLevels[i]) * Speed;
			}
		}
	};

	static bool DecodeArtworkAndAccent(
		const GlobalSystemMediaTransportControlsSessionMediaProperties &Props,
		std::array<uint8_t, SMTC_ART_DIM * SMTC_ART_DIM * 4> &aOutRgba,
		float &OutAccentR, float &OutAccentG, float &OutAccentB,
		std::string &OutArtworkKey)
	{
		try
		{
			auto Thumbnail = Props.Thumbnail();
			if(!Thumbnail)
				return false;

			auto Stream = Thumbnail.OpenReadAsync().get();
			if(!Stream || Stream.Size() == 0)
				return false;

			auto Decoder = BitmapDecoder::CreateAsync(Stream).get();
			if(!Decoder)
				return false;

			BitmapTransform Transform;
			Transform.ScaledWidth(SMTC_ART_DIM);
			Transform.ScaledHeight(SMTC_ART_DIM);

			auto PixelData = Decoder.GetPixelDataAsync(
				BitmapPixelFormat::Bgra8,
				BitmapAlphaMode::Ignore,
				Transform,
				ExifOrientationMode::IgnoreExifOrientation,
				ColorManagementMode::DoNotColorManage)
				.get();
			auto vPixelsBgra = PixelData.DetachPixelData();
			if(vPixelsBgra.size() < aOutRgba.size())
				return false;

			float SumR = 0.0f;
			float SumG = 0.0f;
			float SumB = 0.0f;
			float DominantR = 0.32f;
			float DominantG = 0.52f;
			float DominantB = 0.80f;
			float BestScore = -1.0f;
			const int PixelCount = SMTC_ART_DIM * SMTC_ART_DIM;
			for(int Pixel = 0; Pixel < PixelCount; ++Pixel)
			{
				const int Src = Pixel * 4;
				const uint8_t B = vPixelsBgra[Src + 0];
				const uint8_t G = vPixelsBgra[Src + 1];
				const uint8_t R = vPixelsBgra[Src + 2];

				aOutRgba[Src + 0] = R;
				aOutRgba[Src + 1] = G;
				aOutRgba[Src + 2] = B;
				aOutRgba[Src + 3] = 255;

				const float FR = R / 255.0f;
				const float FG = G / 255.0f;
				const float FB = B / 255.0f;
				SumR += FR;
				SumG += FG;
				SumB += FB;

				const float MaxC = maximum(FR, maximum(FG, FB));
				const float MinC = minimum(FR, minimum(FG, FB));
				const float Sat = MaxC - MinC;
				const float Luma = FR * 0.2126f + FG * 0.7152f + FB * 0.0722f;
				const float Score = Sat * (0.45f + 0.55f * Luma);
				if(Score > BestScore)
				{
					BestScore = Score;
					DominantR = FR;
					DominantG = FG;
					DominantB = FB;
				}
			}

			const float AvgR = SumR / PixelCount;
			const float AvgG = SumG / PixelCount;
			const float AvgB = SumB / PixelCount;
			OutAccentR = std::clamp(AvgR * 0.30f + DominantR * 0.70f, 0.0f, 1.0f);
			OutAccentG = std::clamp(AvgG * 0.30f + DominantG * 0.70f, 0.0f, 1.0f);
			OutAccentB = std::clamp(AvgB * 0.30f + DominantB * 0.70f, 0.0f, 1.0f);

			const float AccentLuma = OutAccentR * 0.2126f + OutAccentG * 0.7152f + OutAccentB * 0.0722f;
			if(AccentLuma < 0.25f)
			{
				const float Boost = 0.25f / maximum(AccentLuma, 0.01f);
				OutAccentR = std::clamp(OutAccentR * Boost, 0.0f, 1.0f);
				OutAccentG = std::clamp(OutAccentG * Boost, 0.0f, 1.0f);
				OutAccentB = std::clamp(OutAccentB * Boost, 0.0f, 1.0f);
			}

			char aKey[128];
			str_format(aKey, sizeof(aKey), "%u:%u:%lld:%u:%u:%u",
				(unsigned)Decoder.PixelWidth(),
				(unsigned)Decoder.PixelHeight(),
				(long long)Stream.Size(),
				(unsigned)aOutRgba[0], (unsigned)aOutRgba[1], (unsigned)aOutRgba[2]);
			OutArtworkKey = aKey;
			return true;
		}
		catch(...)
		{
			return false;
		}
	}
}

class CMusicSMTC::CWorker
{
public:
	CMusicSMTC *m_pSelf;
	std::atomic_int m_PendingCmd{(int)ECommand::NONE};
	std::array<uint8_t, SMTC_ART_DIM * SMTC_ART_DIM * 4> m_aCachedArtworkRgba{};
	std::string m_LastArtworkKey;
	std::string m_LastArtworkIdentity;
	bool m_HasCachedArtwork = false;
	uint64_t m_ArtworkRevision = 0;
	int64_t m_LastArtworkDecodeTick = 0;
	float m_AccentR = 0.34f;
	float m_AccentG = 0.53f;
	float m_AccentB = 0.79f;
	bool m_HasVisualizer = false;
	std::array<float, CMusicSMTC::STrackInfo::VISUALIZER_BARS> m_aVisualizer{};
	CSystemAudioAnalyzer m_AudioAnalyzer;
	int64_t m_LastSessionRefreshTick = 0;
	int64_t m_LastMediaPropsTick = 0;
	std::string m_LastTitle;
	std::string m_LastArtist;

	CWorker(CMusicSMTC *pSelf) : m_pSelf(pSelf) {}

	void Run()
	{
		// thread_init initializes COM, but we also need WinRT apartment.
		winrt::init_apartment(winrt::apartment_type::multi_threaded);

		GlobalSystemMediaTransportControlsSessionManager Manager{nullptr};
		GlobalSystemMediaTransportControlsSession Session{nullptr};

		while(!m_pSelf->m_StopRequested.load())
		{
			const int64_t Now = time_get();

			// Ensure manager
			if(!Manager)
			{
				try
				{
					Manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
				}
				catch(...)
				{
					UpdateUnavailable();
					std::this_thread::sleep_for(std::chrono::milliseconds(1000));
					continue;
				}
			}

			// Refresh current session periodically (and immediately when missing).
			const int64_t SessionRefreshInterval = time_freq() / 4; // 250ms
			if(!Session || m_LastSessionRefreshTick == 0 || Now - m_LastSessionRefreshTick >= SessionRefreshInterval)
			{
				m_LastSessionRefreshTick = Now;
				try
				{
					Session = Manager.GetCurrentSession();
				}
				catch(...)
				{
					Session = nullptr;
				}
			}

			// Execute pending command on the current session
			const ECommand Cmd = (ECommand)m_PendingCmd.exchange((int)ECommand::NONE);
			if(Cmd != ECommand::NONE)
			{
				TryExecute(Session, Cmd);
			}

			// Poll info
			Poll(Session, Now);

			std::this_thread::sleep_for(std::chrono::milliseconds(Session ? 16 : 120));
		}
		winrt::uninit_apartment();
	}

private:
	void UpdateUnavailable()
	{
		std::scoped_lock Lock(m_pSelf->m_Mutex);
		m_pSelf->m_TrackInfo = {};
		m_pSelf->m_TrackInfo.m_Available = false;
		m_pSelf->m_TrackInfo.m_Playing = false;
		if(m_HasCachedArtwork || !m_LastArtworkKey.empty())
		{
			m_HasCachedArtwork = false;
			m_LastArtworkKey.clear();
			m_LastArtworkIdentity.clear();
			m_LastArtworkDecodeTick = 0;
			++m_ArtworkRevision;
		}
		m_AccentR = 0.34f;
		m_AccentG = 0.53f;
		m_AccentB = 0.79f;
		m_pSelf->m_TrackInfo.m_HasArtwork = false;
		m_pSelf->m_TrackInfo.m_ArtworkRevision = m_ArtworkRevision;
		m_pSelf->m_TrackInfo.m_AccentR = m_AccentR;
		m_pSelf->m_TrackInfo.m_AccentG = m_AccentG;
		m_pSelf->m_TrackInfo.m_AccentB = m_AccentB;
		m_HasVisualizer = false;
		for(float &Value : m_aVisualizer)
			Value = 0.0f;
		m_pSelf->m_TrackInfo.m_HasVisualizer = false;
		for(float &Value : m_pSelf->m_TrackInfo.m_aVisualizer)
			Value = 0.0f;
		m_AudioAnalyzer.Shutdown();
		m_LastSessionRefreshTick = 0;
		m_LastMediaPropsTick = 0;
		m_LastTitle.clear();
		m_LastArtist.clear();
	}

	void TryExecute(const GlobalSystemMediaTransportControlsSession &Session, ECommand Cmd)
	{
		if(!Session)
			return;
		try
		{
			switch(Cmd)
			{
			case ECommand::TOGGLE_PLAY_PAUSE:
				Session.TryTogglePlayPauseAsync().get();
				break;
			case ECommand::NEXT:
				Session.TrySkipNextAsync().get();
				break;
			case ECommand::PREV:
				Session.TrySkipPreviousAsync().get();
				break;
			default:
				break;
			}
		}
		catch(...)
		{
			// ignore
		}
	}

	void Poll(const GlobalSystemMediaTransportControlsSession &Session, int64_t Now)
	{
		if(!Session)
		{
			UpdateUnavailable();
			return;
		}

		bool Playing = false;
		try
		{
			auto PlaybackInfo = Session.GetPlaybackInfo();
			if(PlaybackInfo)
			{
				const auto Status = PlaybackInfo.PlaybackStatus();
				Playing = Status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
			}
		}
		catch(...)
		{
			// keep default
		}

		std::string Title = m_LastTitle;
		std::string Artist = m_LastArtist;
		std::array<uint8_t, SMTC_ART_DIM * SMTC_ART_DIM * 4> aArtworkRgba{};
		std::string ArtworkKey;
		float AccentR = m_AccentR;
		float AccentG = m_AccentG;
		float AccentB = m_AccentB;
		bool HasArtwork = m_HasCachedArtwork;
		if(m_HasCachedArtwork)
		{
			aArtworkRgba = m_aCachedArtworkRgba;
			ArtworkKey = m_LastArtworkKey;
		}
		std::array<float, CMusicSMTC::STrackInfo::VISUALIZER_BARS> aVisualizer{};
		const bool HasVisualizerRaw = m_AudioAnalyzer.Poll(aVisualizer);
		const bool HasVisualizer = Playing && HasVisualizerRaw;
		const int64_t MediaPropsInterval = time_freq() / 4; // 250ms
		if(m_LastMediaPropsTick == 0 || Now - m_LastMediaPropsTick >= MediaPropsInterval)
		{
			m_LastMediaPropsTick = Now;
			try
			{
				auto Props = Session.TryGetMediaPropertiesAsync().get();
				Title = Utf8FromHString(Props.Title());
				Artist = Utf8FromHString(Props.Artist());
				m_LastTitle = Title;
				m_LastArtist = Artist;

				const std::string ArtworkIdentity = Title + "|" + Artist;
				bool NeedDecodeArtwork = true;
				if(ArtworkIdentity == m_LastArtworkIdentity)
				{
					const int64_t DecodeInterval = m_HasCachedArtwork ? 5 * time_freq() : time_freq();
					NeedDecodeArtwork = (m_LastArtworkDecodeTick == 0 || Now - m_LastArtworkDecodeTick >= DecodeInterval);
				}

				if(NeedDecodeArtwork)
				{
					m_LastArtworkDecodeTick = Now;
					m_LastArtworkIdentity = ArtworkIdentity;
					HasArtwork = DecodeArtworkAndAccent(Props, aArtworkRgba, AccentR, AccentG, AccentB, ArtworkKey);
				}
			}
			catch(...)
			{
				// keep previous metadata/artwork state
			}
		}

		{
			std::scoped_lock Lock(m_pSelf->m_Mutex);
			m_pSelf->m_TrackInfo.m_Available = true;
			m_pSelf->m_TrackInfo.m_Playing = Playing;
			SetIfDifferent(m_pSelf->m_TrackInfo.m_Title, Title);
			SetIfDifferent(m_pSelf->m_TrackInfo.m_Artist, Artist);

			if(HasArtwork)
			{
				const bool ArtworkChanged = !m_HasCachedArtwork || m_LastArtworkKey != ArtworkKey;
				if(ArtworkChanged)
				{
					m_HasCachedArtwork = true;
					m_LastArtworkKey = ArtworkKey;
					m_aCachedArtworkRgba = aArtworkRgba;
					m_AccentR = AccentR;
					m_AccentG = AccentG;
					m_AccentB = AccentB;
					++m_ArtworkRevision;
				}
			}
			else if(m_HasCachedArtwork || !m_LastArtworkKey.empty())
			{
				m_HasCachedArtwork = false;
				m_LastArtworkKey.clear();
				m_LastArtworkIdentity.clear();
				m_LastArtworkDecodeTick = 0;
				m_AccentR = 0.34f;
				m_AccentG = 0.53f;
				m_AccentB = 0.79f;
				++m_ArtworkRevision;
			}

			m_pSelf->m_TrackInfo.m_HasArtwork = m_HasCachedArtwork;
			m_pSelf->m_TrackInfo.m_ArtworkRevision = m_ArtworkRevision;
			m_pSelf->m_TrackInfo.m_AccentR = m_AccentR;
			m_pSelf->m_TrackInfo.m_AccentG = m_AccentG;
			m_pSelf->m_TrackInfo.m_AccentB = m_AccentB;
			m_HasVisualizer = HasVisualizer;
			if(HasVisualizer)
				m_aVisualizer = aVisualizer;
			else
			{
				for(float &Value : m_aVisualizer)
					Value *= 0.90f;
			}
			m_pSelf->m_TrackInfo.m_HasVisualizer = m_HasVisualizer;
			m_pSelf->m_TrackInfo.m_aVisualizer = m_aVisualizer;
			if(m_HasCachedArtwork)
				m_pSelf->m_TrackInfo.m_aArtworkRgba = m_aCachedArtworkRgba;
		}
	}
};

CMusicSMTC::CMusicSMTC() = default;
CMusicSMTC::~CMusicSMTC() = default;

void CMusicSMTC::OnInit()
{
	StartWorker();
}

void CMusicSMTC::OnShutdown()
{
	StopWorker();
}

void CMusicSMTC::OnUpdate()
{
	// no-op, worker updates state
}

CMusicSMTC::STrackInfo CMusicSMTC::GetTrackInfo() const
{
	std::scoped_lock Lock(m_Mutex);
	return m_TrackInfo;
}

void CMusicSMTC::TogglePlayPause()
{
	if(!m_pWorker)
		return;
	((CWorker *)m_pWorker)->m_PendingCmd.store((int)ECommand::TOGGLE_PLAY_PAUSE);
}

void CMusicSMTC::Next()
{
	if(!m_pWorker)
		return;
	((CWorker *)m_pWorker)->m_PendingCmd.store((int)ECommand::NEXT);
}

void CMusicSMTC::Previous()
{
	if(!m_pWorker)
		return;
	((CWorker *)m_pWorker)->m_PendingCmd.store((int)ECommand::PREV);
}

void CMusicSMTC::StartWorker()
{
	if(m_WorkerRunning.exchange(true))
		return;
	m_StopRequested.store(false);
	m_pWorker = new CWorker(this);
	m_pThread = thread_init(CMusicSMTC::ThreadMain, m_pWorker, "smtc");
}

void CMusicSMTC::StopWorker()
{
	m_StopRequested.store(true);
	if(m_pThread)
	{
		thread_wait(m_pThread);
		m_pThread = nullptr;
	}
	if(m_pWorker)
	{
		delete (CWorker *)m_pWorker;
		m_pWorker = nullptr;
	}
	m_WorkerRunning.store(false);
}

void CMusicSMTC::ThreadMain(void *pUser)
{
	CWorker *pWorker = (CWorker *)pUser;
	pWorker->Run();
}

#else

CMusicSMTC::CMusicSMTC() = default;
CMusicSMTC::~CMusicSMTC() = default;

void CMusicSMTC::OnInit() {}
void CMusicSMTC::OnShutdown() {}
void CMusicSMTC::OnUpdate() {}

CMusicSMTC::STrackInfo CMusicSMTC::GetTrackInfo() const { return {}; }
void CMusicSMTC::TogglePlayPause() {}
void CMusicSMTC::Next() {}
void CMusicSMTC::Previous() {}

void CMusicSMTC::StartWorker() {}
void CMusicSMTC::StopWorker() {}
void CMusicSMTC::ThreadMain(void *) {}

#endif
