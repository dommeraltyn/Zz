#ifndef GAME_CLIENT_COMPONENTS_ZZ_ASYNC_PROXY_BOTS_H
#define GAME_CLIENT_COMPONENTS_ZZ_ASYNC_PROXY_BOTS_H

#include <base/net.h>
#include <base/hash.h>
#include <base/types.h>
#include <base/vmath.h>

#include <engine/shared/network.h>
#include <engine/shared/protocol.h>
#include <engine/shared/uuid_manager.h>
#include <generated/protocol.h>

#include <memory>
#include <optional>
#include <string>
#include <deque>
#include <unordered_map>
#include <vector>

class CGameClient;
class CMsgPacker;

class CAsyncProxyBotScript
{
public:
	virtual ~CAsyncProxyBotScript() = default;
	virtual void Tick(CGameClient *pGameClient, int BotId) = 0;
};

class CCompanionMirrorBotScript final : public CAsyncProxyBotScript
{
public:
	void Tick(CGameClient *pGameClient, int BotId) override;
};

class CAsyncProxyBotManager
{
public:
	struct SStats
	{
		int m_Total = 0;
		int m_Connecting = 0;
		int m_Ingame = 0;
		int m_Error = 0;
	};

	void Tick(CGameClient *pGameClient);
	void StopAll();

	void StartFromConfig(CGameClient *pGameClient);
	void QueueChatAll(const char *pText);
	void StartAsciiBroadcast(const char *pText, int Cycles, int CooldownSeconds);
	void StopAsciiBroadcast();
	void QueueEmoticonAll(int Emoticon);
	void KillAllIngame();
	void NotifyLocalVoteCommand(int SourceClientId, int Vote);
	void VoteAll(int Vote);
	void ApplyLiveSettings(CGameClient *pGameClient);
	void NotifyLocalChatCommand(CGameClient *pGameClient, const char *pLine);

	bool IsRunning() const { return m_Running; }
	const SStats &Stats() const { return m_Stats; }
	const char *StatusText() const { return m_aStatus; }

private:
	struct SProxyEntry
	{
		NETADDR m_Addr = {};
		char m_aUsername[64] = "";
		char m_aPassword[64] = "";
		char m_aRaw[192] = "";
	};

	enum EBotState
	{
		BOT_OFFLINE = 0,
		BOT_CONNECTING,
		BOT_DOWNLOADING_MAP,
		BOT_WAIT_READYTOENTER,
		BOT_INGAME,
		BOT_ERROR
	};

	struct SBotSession
	{
		int m_Id = 0;
		EBotState m_State = BOT_OFFLINE;
		CNetClient m_NetClient;
		NETADDR m_ServerAddr = {};
		SProxyEntry m_Proxy = {};
		CUuid m_ConnectionId = UUID_ZEROED;
		char m_aName[MAX_NAME_LENGTH] = "";
		char m_aClan[MAX_CLAN_LENGTH] = "";
		int m_Country = -1;
		char m_aSkin[MAX_SKIN_LENGTH] = "";
		int m_UseCustomColor = 0;
		int m_ColorBody = 0;
		int m_ColorFeet = 0;
		char m_aError[128] = "";
		char m_aPendingChat[256] = "";
		bool m_Sixup = false;
		bool m_NoReconnect = false;
		bool m_TransportOpened = false;

		int64_t m_NextReconnectAt = 0;
		int m_Reconnects = 0;
		bool m_InfoSent = false;
		bool m_StartInfoSent = false;
		bool m_ReadySent = false;
		bool m_EnterGameSent = false;
		bool m_ConReadySeen = false;
		bool m_FastReadySent = false;
		int64_t m_ReadySentAt = 0;
		int64_t m_NextStartInfoRetryAt = 0;
		int64_t m_NextEnterRetryAt = 0;

		int m_MapCrc = 0;
		int m_MapChunk = 0;
		int m_MapBytes = 0;
		int m_MapSize = 0;
		int m_MapDownloadChunkSize = 900;
		int m_MapDownloadChunksPerRequest = 16;
		bool m_HasMapDetails = false;
		char m_aMapDetailsName[256] = "";
		int m_MapDetailsCrc = 0;
		int m_MapDetailsSize = 0;
		SHA256_DIGEST m_MapDetailsSha256 = {};
		int m_LastAckedSnapshot = 0;
		int m_InputTick = 0;
		int64_t m_NextInputAt = 0;
		int64_t m_ConnectStartedAt = 0;
		int64_t m_NextDiagAt = 0;
		int64_t m_NextNetPollAt = 0;
		int m_MirrorLastSourceAttackTick = -1;
		int m_MirrorLastSourceWeapon = -1;
		int m_KnownClientId = -1;
		int m_AiRole = 0; // 0 idle/support, 1 rescue, 2 hunter
		int m_AiTargetClientId = -1;
		int m_AiCommitUntilTick = 0;
		int m_AiFireCount = 0;
		int m_TowerHammerPrepTick = -1;
		int m_TripleFlyTargetClientId = -1;
		int m_TripleFlyLastHookedClientId = -1;
		int m_TripleFlyNextRehookTick = 0;
		int m_TripleFlyNextNoHookSearchTick = 0;
		bool m_TripleFlyPulsePending = false;
		float m_TripleFlyLastVelY = 0.0f;
		bool m_TripleFlyHasLastVelY = false;
		CNetObj_PlayerInput m_AiPlannedInput = {};
		bool m_AiHasPlannedInput = false;
		int m_AiLastSimTick = -1;
		int m_AiLastSimRole = -1;
		int m_AiLastSimTarget = -1;
		vec2 m_AiLastGoalPos = vec2(0.0f, 0.0f);
	};

	struct SAiTargetInfo
	{
		int m_ClientId = -1;
		vec2 m_Pos = vec2(0.0f, 0.0f);
		bool m_Frozen = false;
	};

	bool ParseProxyList(const char *pRaw, std::vector<SProxyEntry> &vOut, CGameClient *pGameClient);
	bool ParseSingleProxy(const char *pRaw, SProxyEntry &Out);
	bool ResolveServerAddress(CGameClient *pGameClient, int LookupTypes, NETADDR &Out);
	bool HasSavedMap(CGameClient *pGameClient, const char *pMapName, int MapCrc, const std::optional<SHA256_DIGEST> &MapSha256);
	void ScheduleSessions(CGameClient *pGameClient);
	void SpawnSession(CGameClient *pGameClient, const SProxyEntry &Proxy, int WaveIndex);
	void TickSession(CGameClient *pGameClient, SBotSession &Bot);
	void ReconnectSession(CGameClient *pGameClient, SBotSession &Bot);
	void DisconnectSession(SBotSession &Bot, const char *pReason, bool ErrorState);
	void SendPacket(SBotSession &Bot, CMsgPacker &Msg, int Flags);
	void SendClientInfo(CGameClient *pGameClient, SBotSession &Bot);
	void SendStartInfo(CGameClient *pGameClient, SBotSession &Bot);
	void SendChangeInfo(SBotSession &Bot);
	void SendMapRequest(SBotSession &Bot);
	void SendEnterGame(SBotSession &Bot);
	void SendEmoticon(SBotSession &Bot, int Emoticon);
	void SendChat(SBotSession &Bot, const char *pText);
	void SendKill(SBotSession &Bot);
	void SendInput(CGameClient *pGameClient, SBotSession &Bot);
	void BuildBotAppearance(CGameClient *pGameClient, SBotSession &Bot, int WaveIndex);
	void BuildInputTemplate(CGameClient *pGameClient, SBotSession &Bot, CNetObj_PlayerInput &Input);
	void ApplyTripleFlyHook(CGameClient *pGameClient, SBotSession &Bot, CNetObj_PlayerInput &Input);
	void BuildCompanionInput(CGameClient *pGameClient, SBotSession &Bot, CNetObj_PlayerInput &Input);
	void UpdateCompanionPlanner(CGameClient *pGameClient);
	void HandleChunk(CGameClient *pGameClient, SBotSession &Bot, const CNetChunk &Chunk);
	void RefreshStats();
	void ClearAsciiRuntimeState();
	void ArmFastReconnect(CGameClient *pGameClient, SBotSession &Bot, const char *pReason);

private:
	bool m_Running = false;
	bool m_QueuedStart = false;
	int64_t m_NextWaveAt = 0;
	int m_NextSpawnIndex = 0;
	int m_BatchSize = 4;
	int m_BatchDelayMs = 250;
	int m_TotalTarget = 0;
	NETADDR m_ServerAddr = {};
	int64_t m_NextCompanionPlanAt = 0;
	int64_t m_NextSynchronizedInputAt = 0;
	int m_CompanionComeUntilTick = 0;
	bool m_TowerAnchorLocked = false;
	vec2 m_TowerAnchorPos = vec2(0.0f, 0.0f);
	int m_TowerAnchorTick = -1;
	int64_t m_NextAsciiActionAt = 0;
	int m_AsciiCyclesLeft = 0;
	bool m_AsciiInfiniteCycles = false;
	int m_AsciiLineIndex = 0;
	int m_AsciiCooldownSeconds = 5;
	int m_AsciiPendingAckSessionId = -1;
	int64_t m_AsciiPendingAckSince = 0;
	std::string m_AsciiPendingAckText;
	std::vector<int> m_vAsciiPlanSessionIds;
	std::vector<std::string> m_vAsciiLines;
	std::unordered_map<int, int64_t> m_AsciiCooldownUntilBySessionId;
	int m_LocalVoteCommandChoice = 0;
	int m_LocalVoteCommandSourceId = -1;
	int m_LocalVoteCommandSerial = 0;
	int m_LocalVoteCommandHandledSerial = 0;

	SStats m_Stats = {};
	char m_aStatus[256] = "Idle";

	std::vector<SProxyEntry> m_vSpawnQueue;
	std::deque<SBotSession> m_vSessions;
	std::unordered_map<std::string, bool> m_SavedMapCache;
	std::unique_ptr<CAsyncProxyBotScript> m_pScript = std::make_unique<CCompanionMirrorBotScript>();
};

#endif
