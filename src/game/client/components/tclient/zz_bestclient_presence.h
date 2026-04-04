#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_ZZ_BESTCLIENT_PRESENCE_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_ZZ_BESTCLIENT_PRESENCE_H

#include <base/net.h>

#include <engine/shared/http.h>
#include <engine/shared/uuid_manager.h>

#include <game/client/component.h>

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class CZzBestClientPresence : public CComponent
{
public:
	CZzBestClientPresence();
	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;
	void OnReset() override;
	void OnUpdate() override;
	void OnStateChange(int NewState, int OldState) override;
	void OnShutdown() override;

private:
	NETSOCKET m_Socket = nullptr;
	NETADDR m_IndicatorServerAddr = NETADDR_ZEROED;
	bool m_HasIndicatorServerAddr = false;
	char m_aLastIndicatorServerAddress[256] = "";
	char m_aCurrentGameServerAddress[NETADDR_MAXSTRSIZE] = "";
	char m_aLastGameServerAddress[NETADDR_MAXSTRSIZE] = "";

	CUuid m_ClientInstanceId = UUID_ZEROED;
	std::unordered_set<int> m_RegisteredClientIds;

	std::shared_ptr<CHttpRequest> m_pTokenTask = nullptr;
	char m_aWebSharedToken[256] = "";
	int64_t m_LastHeartbeatTick = 0;
	int64_t m_LastTokenRefreshTick = 0;
	int64_t m_LastSocketOpenAttemptTick = 0;
	bool m_ForcedAllowInsecure = false;

	void RefreshToken(bool Force);
	void FinishTokenRefresh();
	void ResetTokenTask();
	void ResetTokenState();

	void EnsurePresenceSocket();
	void OpenPresenceSocket();
	void ClosePresenceSocket();
	void StopPresence(bool SendLeavePackets);
	void ResetPresenceState();

	void UpdatePresence();
	void SyncLocalRegistrations();
	void SendLeaveForAll();
	void SendPresencePacket(int ClientId, int PacketType);
	int FindClientIdByName(const char *pName) const;
	std::vector<std::string> ParseConfiguredNicknames() const;

	const char *CurrentGameServerAddress();
	const char *PlayerNameForClient(int ClientId) const;
	const char *EffectiveSharedToken() const;
	void PrintConsoleMessage(const char *pFormat, ...) const;
	void DebugLog(const char *pFormat, ...) const;

	std::unordered_set<std::string> m_MissingConfiguredNicknames;
};

#endif
