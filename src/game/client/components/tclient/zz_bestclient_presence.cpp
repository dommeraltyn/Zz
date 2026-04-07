#include "zz_bestclient_presence.h"

#include <base/hash_ctxt.h>
#include <base/logger.h>
#include <base/system.h>

#include <engine/console.h>
#include <engine/shared/config.h>
#include <engine/shared/json.h>

#include <game/client/gameclient.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace
{
constexpr const char *LOG_SCOPE = "zz-bc-presence";
constexpr const char *OLD_TOKEN_URL = "http://150.241.70.188:8779/token.json";
constexpr const char *NEW_TOKEN_URL = "https://150.241.70.188:8779/token.json";
constexpr const char *USERS_URL = "https://150.241.70.188:8779/users.json";
constexpr int DEFAULT_INDICATOR_PORT = 8778;
constexpr int TOKEN_REFRESH_SECONDS = 60;
constexpr int USERS_REFRESH_SECONDS = 30;
constexpr int HEARTBEAT_SECONDS = 5;

constexpr uint32_t PROTOCOL_MAGIC = 0x42434931u; // BCI1
constexpr uint8_t PROTOCOL_VERSION = 1;

enum EPacketType : uint8_t
{
	PACKET_JOIN = 1,
	PACKET_HEARTBEAT = 2,
	PACKET_LEAVE = 3,
};

void WriteU8(std::vector<uint8_t> &vOut, uint8_t Value)
{
	vOut.push_back(Value);
}

void WriteU16(std::vector<uint8_t> &vOut, uint16_t Value)
{
	vOut.push_back((uint8_t)((Value >> 8) & 0xff));
	vOut.push_back((uint8_t)(Value & 0xff));
}

void WriteS16(std::vector<uint8_t> &vOut, int16_t Value)
{
	WriteU16(vOut, (uint16_t)Value);
}

void WriteU64(std::vector<uint8_t> &vOut, uint64_t Value)
{
	for(int Shift = 56; Shift >= 0; Shift -= 8)
		vOut.push_back((uint8_t)((Value >> Shift) & 0xff));
}

void WriteRaw(std::vector<uint8_t> &vOut, const void *pData, int DataSize)
{
	if(!pData || DataSize <= 0)
		return;
	const auto *pBytes = static_cast<const uint8_t *>(pData);
	vOut.insert(vOut.end(), pBytes, pBytes + DataSize);
}

void WriteString(std::vector<uint8_t> &vOut, const char *pValue)
{
	const int Length = pValue ? str_length(pValue) : 0;
	WriteU16(vOut, (uint16_t)Length);
	if(Length > 0)
		WriteRaw(vOut, pValue, Length);
}

void WriteUuid(std::vector<uint8_t> &vOut, CUuid Uuid)
{
	WriteRaw(vOut, Uuid.m_aData, (int)sizeof(Uuid.m_aData));
}

void WriteHeader(std::vector<uint8_t> &vOut, EPacketType Type)
{
	vOut.push_back((uint8_t)((PROTOCOL_MAGIC >> 24) & 0xff));
	vOut.push_back((uint8_t)((PROTOCOL_MAGIC >> 16) & 0xff));
	vOut.push_back((uint8_t)((PROTOCOL_MAGIC >> 8) & 0xff));
	vOut.push_back((uint8_t)(PROTOCOL_MAGIC & 0xff));
	WriteU8(vOut, (uint8_t)Type);
	WriteU8(vOut, PROTOCOL_VERSION);
}

void AppendProof(std::vector<uint8_t> &vPacket, const char *pSharedToken)
{
	SHA256_CTX Sha256;
	sha256_init(&Sha256);
	if(pSharedToken && pSharedToken[0] != '\0')
		sha256_update(&Sha256, pSharedToken, str_length(pSharedToken));
	if(!vPacket.empty())
		sha256_update(&Sha256, vPacket.data(), (int)vPacket.size());
	const SHA256_DIGEST Digest = sha256_finish(&Sha256);
	WriteRaw(vPacket, Digest.data, (int)sizeof(Digest.data));
}

bool ParseAddress(const char *pAddress, int DefaultPort, NETADDR &Out)
{
	if(!pAddress || pAddress[0] == '\0')
		return false;
	if(net_addr_from_url(&Out, pAddress, nullptr, 0) == 0 || net_addr_from_str(&Out, pAddress) == 0)
	{
		if(Out.port == 0)
			Out.port = DefaultPort;
		return true;
	}
	if(net_host_lookup(pAddress, &Out, NETTYPE_ALL) == 0)
	{
		if(Out.port == 0)
			Out.port = DefaultPort;
		return true;
	}
	return false;
}

bool IsBlockedIndicatorAddress(const NETADDR &Addr)
{
	return net_addr_is_local(&Addr);
}

bool IsSpaceCharacter(char C)
{
	return std::isspace((unsigned char)C) != 0;
}

std::string NormalizeConfiguredNickname(const char *pRawNickname)
{
	if(!pRawNickname)
		return "";

	const char *pBegin = pRawNickname;
	const char *pEnd = pRawNickname + str_length(pRawNickname);
	while(pBegin < pEnd && IsSpaceCharacter(*pBegin))
		++pBegin;
	while(pEnd > pBegin && IsSpaceCharacter(*(pEnd - 1)))
		--pEnd;

	if(pEnd - pBegin >= 2 && *pBegin == '"' && *(pEnd - 1) == '"')
	{
		++pBegin;
		--pEnd;
		while(pBegin < pEnd && IsSpaceCharacter(*pBegin))
			++pBegin;
		while(pEnd > pBegin && IsSpaceCharacter(*(pEnd - 1)))
			--pEnd;
	}

	return std::string(pBegin, pEnd);
}

const char *GetStringField(const json_value &Json, const char *pField)
{
	const json_value &Field = Json[pField];
	return Field.type == json_string ? Field.u.string.ptr : nullptr;
}
}

CZzBestClientPresence::CZzBestClientPresence()
{
	OnReset();
}

void CZzBestClientPresence::OnInit()
{
	if(m_ClientInstanceId == UUID_ZEROED)
		m_ClientInstanceId = RandomUuid();
	if(g_Config.m_ClZzBestclientTokenUrl[0] == '\0' || str_comp(g_Config.m_ClZzBestclientTokenUrl, OLD_TOKEN_URL) == 0)
	{
		str_copy(g_Config.m_ClZzBestclientTokenUrl, NEW_TOKEN_URL, sizeof(g_Config.m_ClZzBestclientTokenUrl));
		DebugLog("upgraded token url to https default");
	}
	DebugLog("initialized");
}

void CZzBestClientPresence::OnReset()
{
	StopPresence(false);
	ResetTokenState();
}

void CZzBestClientPresence::OnShutdown()
{
	DebugLog("shutdown");
	StopPresence(true);
	ResetTokenState();
}

void CZzBestClientPresence::OnStateChange(int NewState, int OldState)
{
	(void)OldState;
	if(NewState == IClient::STATE_OFFLINE)
	{
		DebugLog("state offline -> leave/unregister");
		StopPresence(true);
		ResetTokenState();
	}
	else if(NewState == IClient::STATE_ONLINE)
	{
		DebugLog("state online -> refresh token/users");
		if(g_Config.m_ClZzBestclientRegister)
			RefreshToken(false);
		RefreshUsers(false);
	}
}

void CZzBestClientPresence::OnUpdate()
{
	const bool Enabled = g_Config.m_ClZzBestclientRegister != 0;
	const bool Online = Client()->State() == IClient::STATE_ONLINE;
	if(!g_Config.m_HttpAllowInsecure)
	{
		g_Config.m_HttpAllowInsecure = 1;
		if(!m_ForcedAllowInsecure)
		{
			DebugLog("forced http_allow_insecure=1 for BestClient token bootstrap");
			m_ForcedAllowInsecure = true;
		}
	}
	if(m_pUsersTask && m_pUsersTask->State() == EHttpState::DONE)
	{
		DebugLog("users request done http_status=%d", m_pUsersTask->StatusCode());
		FinishUsersRefresh();
		ResetUsersTask();
	}
	else if(m_pUsersTask && (m_pUsersTask->State() == EHttpState::ERROR || m_pUsersTask->State() == EHttpState::ABORTED))
	{
		DebugLog("users request failed state=%d", (int)m_pUsersTask->State());
		ResetUsersTask();
	}

	const int64_t Now = time_get();
	if(!m_pUsersTask && (m_LastUsersRefreshTick == 0 || Now - m_LastUsersRefreshTick >= USERS_REFRESH_SECONDS * time_freq()))
		RefreshUsers(false);

	if(!Enabled || !Online)
	{
		if(m_Socket || !m_RegisteredClientIds.empty() || m_pTokenTask || m_aWebSharedToken[0] != '\0')
		{
			DebugLog("disabled/offline -> leave/unregister");
			StopPresence(true);
			ResetTokenState();
		}
		return;
	}

	if(m_pTokenTask && m_pTokenTask->State() == EHttpState::DONE)
	{
		DebugLog("token request done http_status=%d", m_pTokenTask->StatusCode());
		FinishTokenRefresh();
		ResetTokenTask();
	}
	else if(m_pTokenTask && (m_pTokenTask->State() == EHttpState::ERROR || m_pTokenTask->State() == EHttpState::ABORTED))
	{
		DebugLog("token request failed state=%d", (int)m_pTokenTask->State());
		char aOldEffectiveToken[sizeof(m_aWebSharedToken)];
		str_copy(aOldEffectiveToken, EffectiveSharedToken(), sizeof(aOldEffectiveToken));
		m_aWebSharedToken[0] = '\0';
		if(str_comp(aOldEffectiveToken, EffectiveSharedToken()) != 0)
			StopPresence(true);
		ResetTokenTask();
	}
	if(!m_pTokenTask && (m_LastTokenRefreshTick == 0 || Now - m_LastTokenRefreshTick >= TOKEN_REFRESH_SECONDS * time_freq()))
		RefreshToken(false);

	if(EffectiveSharedToken()[0] == '\0')
		return;

	UpdatePresence();
}

void CZzBestClientPresence::RefreshToken(bool Force)
{
	if(!g_Config.m_HttpAllowInsecure)
	{
		g_Config.m_HttpAllowInsecure = 1;
		if(!m_ForcedAllowInsecure)
		{
			DebugLog("forced http_allow_insecure=1 before token refresh");
			m_ForcedAllowInsecure = true;
		}
	}

	if(g_Config.m_ClZzBestclientTokenUrl[0] == '\0')
	{
		DebugLog("token refresh skipped: token url empty");
		return;
	}
	if(m_pTokenTask && !m_pTokenTask->Done())
	{
		if(!Force)
		{
			DebugLog("token refresh skipped: request in progress");
			return;
		}
		ResetTokenTask();
	}

	m_pTokenTask = HttpGet(g_Config.m_ClZzBestclientTokenUrl);
	m_pTokenTask->Timeout(CTimeout{10000, 0, 500, 5});
	m_pTokenTask->IpResolve(IPRESOLVE::V4);
	m_pTokenTask->VerifyPeer(false);
	m_pTokenTask->LogProgress(HTTPLOG::FAILURE);
	m_LastTokenRefreshTick = time_get();
	DebugLog("token refresh start url=%s", g_Config.m_ClZzBestclientTokenUrl);
	Http()->Run(m_pTokenTask);
}

void CZzBestClientPresence::FinishTokenRefresh()
{
	if(!m_pTokenTask)
		return;

	json_value *pJson = m_pTokenTask->ResultJson();
	char aOldEffectiveToken[sizeof(m_aWebSharedToken)];
	str_copy(aOldEffectiveToken, EffectiveSharedToken(), sizeof(aOldEffectiveToken));
	m_aWebSharedToken[0] = '\0';

	if(pJson)
	{
		const char *pToken = nullptr;
		if(pJson->type == json_object)
		{
			const json_value &Token = (*pJson)["token"];
			if(Token.type == json_string)
				pToken = Token.u.string.ptr;
		}
		if(pToken && pToken[0] != '\0')
			str_copy(m_aWebSharedToken, pToken, sizeof(m_aWebSharedToken));
		json_value_free(pJson);
	}
	else
	{
		DebugLog("token refresh completed but JSON parse failed");
	}

	if(m_aWebSharedToken[0] != '\0')
		DebugLog("token refresh success, web token length=%d", str_length(m_aWebSharedToken));
	else
		DebugLog("token refresh empty, fallback token length=%d", str_length(g_Config.m_ClZzBestclientSharedToken));

	if(str_comp(aOldEffectiveToken, EffectiveSharedToken()) != 0)
	{
		DebugLog("effective token changed -> reset presence");
		StopPresence(true);
	}
}

void CZzBestClientPresence::ResetTokenTask()
{
	if(m_pTokenTask)
	{
		m_pTokenTask->Abort();
		m_pTokenTask = nullptr;
	}
}

void CZzBestClientPresence::RefreshUsers(bool Force)
{
	if(m_pUsersTask && !m_pUsersTask->Done())
	{
		if(!Force)
		{
			DebugLog("users refresh skipped: request in progress");
			return;
		}
		ResetUsersTask();
	}

	m_pUsersTask = HttpGet(USERS_URL);
	m_pUsersTask->Timeout(CTimeout{10000, 0, 500, 5});
	m_pUsersTask->IpResolve(IPRESOLVE::V4);
	m_pUsersTask->VerifyPeer(false);
	m_pUsersTask->LogProgress(HTTPLOG::FAILURE);
	m_LastUsersRefreshTick = time_get();
	DebugLog("users refresh start url=%s", USERS_URL);
	Http()->Run(m_pUsersTask);
}

void CZzBestClientPresence::FinishUsersRefresh()
{
	if(!m_pUsersTask)
		return;

	json_value *pJson = m_pUsersTask->ResultJson();
	if(!pJson)
	{
		DebugLog("users refresh completed but JSON parse failed");
		return;
	}

	std::unordered_map<std::string, std::unordered_set<std::string>> Parsed;
	const auto AddPlayer = [&](const char *pServerAddress, const char *pName) {
		if(!pServerAddress || !pName || pServerAddress[0] == '\0' || pName[0] == '\0')
			return;
		std::string NormalizedAddress;
		if(!NormalizeServerAddress(pServerAddress, NormalizedAddress))
			return;
		Parsed[NormalizedAddress].insert(pName);
	};

	std::function<void(const char *, const json_value &)> ParsePlayersValue = [&](const char *pServerAddress, const json_value &Players) {
		if(!pServerAddress || pServerAddress[0] == '\0')
			return;
		if(Players.type == json_array)
		{
			for(unsigned int i = 0; i < Players.u.array.length; ++i)
			{
				const json_value &Player = *Players.u.array.values[i];
				if(Player.type == json_string)
					AddPlayer(pServerAddress, Player.u.string.ptr);
				else if(Player.type == json_object)
				{
					if(const char *pName = GetStringField(Player, "name"))
						AddPlayer(pServerAddress, pName);
					else if(const char *pName = GetStringField(Player, "player_name"))
						AddPlayer(pServerAddress, pName);
				}
			}
		}
		else if(Players.type == json_object)
		{
			if(const char *pName = GetStringField(Players, "name"))
				AddPlayer(pServerAddress, pName);
			if(const char *pName = GetStringField(Players, "player_name"))
				AddPlayer(pServerAddress, pName);
			for(unsigned int i = 0; i < Players.u.object.length; ++i)
			{
				const auto &Entry = Players.u.object.values[i];
				if(Entry.value->type == json_object || Entry.value->type == json_array)
					ParsePlayersValue(pServerAddress, *Entry.value);
				else if(Entry.value->type == json_string)
					AddPlayer(pServerAddress, Entry.value->u.string.ptr);
			}
		}
	};

	if(pJson->type == json_array)
	{
		for(unsigned int i = 0; i < pJson->u.array.length; ++i)
		{
			const json_value &Entry = *pJson->u.array.values[i];
			if(Entry.type != json_object)
				continue;
			const char *pServerAddress = GetStringField(Entry, "server_address");
			if(!pServerAddress)
				pServerAddress = GetStringField(Entry, "address");
			if(!pServerAddress)
				pServerAddress = GetStringField(Entry, "server");
			if(!pServerAddress)
				continue;
			const json_value &Players = Entry["players"];
			if(Players.type != json_none)
				ParsePlayersValue(pServerAddress, Players);
			if(const char *pName = GetStringField(Entry, "name"))
				AddPlayer(pServerAddress, pName);
			if(const char *pName = GetStringField(Entry, "player_name"))
				AddPlayer(pServerAddress, pName);
		}
	}
	else if(pJson->type == json_object)
	{
		for(unsigned int i = 0; i < pJson->u.object.length; ++i)
		{
			const auto &Entry = pJson->u.object.values[i];
			if(Entry.value->type == json_object || Entry.value->type == json_array)
				ParsePlayersValue(Entry.name, *Entry.value);
		}
	}

	json_value_free(pJson);
	m_BestClientPlayersByServer = std::move(Parsed);
	DebugLog("users refresh success, servers=%d", (int)m_BestClientPlayersByServer.size());
}

void CZzBestClientPresence::ResetUsersTask()
{
	if(m_pUsersTask)
	{
		m_pUsersTask->Abort();
		m_pUsersTask = nullptr;
	}
}

void CZzBestClientPresence::ResetTokenState()
{
	ResetTokenTask();
	ResetUsersTask();
	m_aWebSharedToken[0] = '\0';
}

void CZzBestClientPresence::OpenPresenceSocket()
{
	if(m_Socket || g_Config.m_ClZzBestclientIndicatorServerAddress[0] == '\0')
		return;

	if(!ParseAddress(g_Config.m_ClZzBestclientIndicatorServerAddress, DEFAULT_INDICATOR_PORT, m_IndicatorServerAddr))
	{
		DebugLog("presence socket open failed: parse '%s'", g_Config.m_ClZzBestclientIndicatorServerAddress);
		return;
	}
	if(IsBlockedIndicatorAddress(m_IndicatorServerAddr))
	{
		DebugLog("presence socket open blocked: local address");
		return;
	}

	NETADDR Bind = NETADDR_ZEROED;
	Bind.type = NETTYPE_ALL;
	Bind.port = 0;
	m_Socket = net_udp_create(Bind);
	if(!m_Socket)
	{
		DebugLog("presence socket open failed: net_udp_create null");
		return;
	}

	net_set_non_blocking(m_Socket);
	m_HasIndicatorServerAddr = true;
	str_copy(m_aLastIndicatorServerAddress, g_Config.m_ClZzBestclientIndicatorServerAddress, sizeof(m_aLastIndicatorServerAddress));
	DebugLog("presence socket opened indicator=%s", m_aLastIndicatorServerAddress);
}

void CZzBestClientPresence::ClosePresenceSocket()
{
	if(m_Socket)
	{
		DebugLog("closing presence socket");
		net_udp_close(m_Socket);
		m_Socket = nullptr;
	}
	m_HasIndicatorServerAddr = false;
	m_aLastIndicatorServerAddress[0] = '\0';
}

void CZzBestClientPresence::StopPresence(bool SendLeavePackets)
{
	if(SendLeavePackets)
		SendLeaveForAll();
	ClosePresenceSocket();
	ResetPresenceState();
}

void CZzBestClientPresence::ResetPresenceState()
{
	m_LastHeartbeatTick = 0;
	m_LastSocketOpenAttemptTick = 0;
	m_RegisteredClientIds.clear();
	m_MissingConfiguredNicknames.clear();
	m_aCurrentGameServerAddress[0] = '\0';
	m_aLastGameServerAddress[0] = '\0';
}

void CZzBestClientPresence::EnsurePresenceSocket()
{
	if(m_Socket)
		return;

	const bool ServerChanged = m_aLastIndicatorServerAddress[0] != '\0' &&
		str_comp(m_aLastIndicatorServerAddress, g_Config.m_ClZzBestclientIndicatorServerAddress) != 0;
	if(ServerChanged)
		StopPresence(true);

	const int64_t Now = time_get();
	if(m_LastSocketOpenAttemptTick != 0 && Now - m_LastSocketOpenAttemptTick < time_freq())
		return;
	m_LastSocketOpenAttemptTick = Now;
	OpenPresenceSocket();
}

const char *CZzBestClientPresence::CurrentGameServerAddress()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return "";
	if(IsBlockedIndicatorAddress(Client()->ServerAddress()))
		return "";

	net_addr_str(&Client()->ServerAddress(), m_aCurrentGameServerAddress, sizeof(m_aCurrentGameServerAddress), true);
	return m_aCurrentGameServerAddress;
}

bool CZzBestClientPresence::NormalizeServerAddress(const char *pAddress, std::string &OutAddress)
{
	OutAddress.clear();
	if(!pAddress || pAddress[0] == '\0')
		return false;

	char aToken[MAX_SERVER_ADDRESSES * NETADDR_MAXSTRSIZE];
	const char *pBegin = pAddress;
	while(*pBegin != '\0' && str_isspace(*pBegin))
		++pBegin;
	int Length = 0;
	while(pBegin[Length] != '\0' && pBegin[Length] != ',')
		++Length;
	while(Length > 0 && str_isspace(pBegin[Length - 1]))
		--Length;
	str_truncate(aToken, sizeof(aToken), pBegin, Length);
	if(aToken[0] == '\0')
		return false;

	NETADDR Addr;
	char aNormalized[NETADDR_MAXSTRSIZE];
	if(net_addr_from_url(&Addr, aToken, nullptr, 0) == 0 || net_addr_from_str(&Addr, aToken) == 0)
	{
		net_addr_str(&Addr, aNormalized, sizeof(aNormalized), true);
		OutAddress = aNormalized;
		return true;
	}

	OutAddress = aToken;
	return true;
}

std::string CZzBestClientPresence::CurrentGameServerAddressNormalized() const
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return {};
	if(IsBlockedIndicatorAddress(Client()->ServerAddress()))
		return {};

	char aAddress[NETADDR_MAXSTRSIZE];
	net_addr_str(&Client()->ServerAddress(), aAddress, sizeof(aAddress), true);
	std::string OutAddress;
	if(!NormalizeServerAddress(aAddress, OutAddress))
		return {};
	return OutAddress;
}

bool CZzBestClientPresence::IsBestClientPlayerOnCurrentServer(const char *pPlayerName) const
{
	if(!pPlayerName || pPlayerName[0] == '\0')
		return false;
	const std::string Server = CurrentGameServerAddressNormalized();
	if(Server.empty())
		return false;
	const auto It = m_BestClientPlayersByServer.find(Server);
	if(It == m_BestClientPlayersByServer.end())
		return false;
	return It->second.find(pPlayerName) != It->second.end();
}

bool CZzBestClientPresence::IsBestClientPlayerOnServer(const CServerInfo *pServerInfo, const char *pPlayerName) const
{
	if(!pServerInfo || !pPlayerName || pPlayerName[0] == '\0')
		return false;
	for(int AddrIndex = 0; AddrIndex < pServerInfo->m_NumAddresses; AddrIndex++)
	{
		char aAddress[NETADDR_MAXSTRSIZE];
		net_addr_str(&pServerInfo->m_aAddresses[AddrIndex], aAddress, sizeof(aAddress), true);
		std::string Normalized;
		if(!NormalizeServerAddress(aAddress, Normalized))
			continue;
		const auto It = m_BestClientPlayersByServer.find(Normalized);
		if(It != m_BestClientPlayersByServer.end() && It->second.find(pPlayerName) != It->second.end())
			return true;
	}
	return false;
}

int CZzBestClientPresence::CountBestClientPlayersOnServer(const CServerInfo *pServerInfo) const
{
	if(!pServerInfo)
		return 0;
	std::unordered_set<std::string> UniquePlayers;
	for(int AddrIndex = 0; AddrIndex < pServerInfo->m_NumAddresses; AddrIndex++)
	{
		char aAddress[NETADDR_MAXSTRSIZE];
		net_addr_str(&pServerInfo->m_aAddresses[AddrIndex], aAddress, sizeof(aAddress), true);
		std::string Normalized;
		if(!NormalizeServerAddress(aAddress, Normalized))
			continue;
		const auto It = m_BestClientPlayersByServer.find(Normalized);
		if(It == m_BestClientPlayersByServer.end())
			continue;
		for(const std::string &Name : It->second)
			UniquePlayers.insert(Name);
	}
	return (int)UniquePlayers.size();
}

const char *CZzBestClientPresence::PlayerNameForClient(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return "";
	if(GameClient()->m_aClients[ClientId].m_Active && GameClient()->m_aClients[ClientId].m_aName[0] != '\0')
		return GameClient()->m_aClients[ClientId].m_aName;
	if(ClientId == GameClient()->m_aLocalIds[IClient::CONN_MAIN])
		return Client()->PlayerName();
	if(ClientId == GameClient()->m_aLocalIds[IClient::CONN_DUMMY])
		return Client()->DummyName();
	return "";
}

const char *CZzBestClientPresence::EffectiveSharedToken() const
{
	if(m_aWebSharedToken[0] != '\0')
		return m_aWebSharedToken;
	return g_Config.m_ClZzBestclientSharedToken;
}

void CZzBestClientPresence::SendPresencePacket(int ClientId, int PacketType)
{
	if(!m_Socket || !m_HasIndicatorServerAddr || ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	const char *pSharedToken = EffectiveSharedToken();
	if(!pSharedToken || pSharedToken[0] == '\0')
		return;

	std::vector<uint8_t> vPacket;
	vPacket.reserve(256);
	WriteHeader(vPacket, (EPacketType)PacketType);
	WriteUuid(vPacket, m_ClientInstanceId);
	WriteUuid(vPacket, RandomUuid());
	WriteU64(vPacket, (uint64_t)time_timestamp());
	WriteString(vPacket, CurrentGameServerAddress());
	WriteString(vPacket, PlayerNameForClient(ClientId));
	WriteS16(vPacket, (int16_t)ClientId);
	AppendProof(vPacket, pSharedToken);

	net_udp_send(m_Socket, &m_IndicatorServerAddr, vPacket.data(), (int)vPacket.size());

	const char *pType = PacketType == PACKET_JOIN ? "join" :
		PacketType == PACKET_HEARTBEAT ? "heartbeat" :
		PacketType == PACKET_LEAVE ? "leave" :
		"unknown";
	DebugLog("sent %s client_id=%d name='%s' game_server=%s",
		pType, ClientId, PlayerNameForClient(ClientId), CurrentGameServerAddress());
}

void CZzBestClientPresence::SendLeaveForAll()
{
	if(!m_Socket || !m_HasIndicatorServerAddr)
		return;
	DebugLog("sending leave for %d registered local clients", (int)m_RegisteredClientIds.size());
	for(const int ClientId : m_RegisteredClientIds)
		SendPresencePacket(ClientId, PACKET_LEAVE);
}

int CZzBestClientPresence::FindClientIdByName(const char *pName) const
{
	if(!pName || pName[0] == '\0')
		return -1;

	int FallbackCaseInsensitive = -1;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!GameClient()->m_aClients[ClientId].m_Active)
			continue;
		if(str_comp(GameClient()->m_aClients[ClientId].m_aName, pName) == 0)
			return ClientId;
		if(FallbackCaseInsensitive < 0 && str_utf8_comp_nocase(GameClient()->m_aClients[ClientId].m_aName, pName) == 0)
			FallbackCaseInsensitive = ClientId;
	}

	return FallbackCaseInsensitive;
}

std::vector<std::string> CZzBestClientPresence::ParseConfiguredNicknames() const
{
	std::vector<std::string> vNicknames;
	if(g_Config.m_ClZzBestclientRegisterNicks[0] == '\0')
		return vNicknames;

	constexpr int TOKEN_BUFFER_SIZE = 256;
	char aToken[TOKEN_BUFFER_SIZE];
	const char *pToken = g_Config.m_ClZzBestclientRegisterNicks;
	while((pToken = str_next_token(pToken, ",", aToken, sizeof(aToken))))
	{
		std::string Nickname = NormalizeConfiguredNickname(aToken);
		if(Nickname.empty())
			continue;
		if(std::find(vNicknames.begin(), vNicknames.end(), Nickname) == vNicknames.end())
			vNicknames.push_back(Nickname);
	}

	return vNicknames;
}

void CZzBestClientPresence::SyncLocalRegistrations()
{
	if(!m_Socket || !m_HasIndicatorServerAddr)
		return;

	std::vector<int> vDesiredClientIds;
	vDesiredClientIds.reserve(MAX_CLIENTS);

	const auto AddDesiredClientId = [this, &vDesiredClientIds](int ClientId) {
		if(ClientId < 0 || ClientId >= MAX_CLIENTS)
			return;
		if(!GameClient()->m_aClients[ClientId].m_Active)
			return;
		if(std::find(vDesiredClientIds.begin(), vDesiredClientIds.end(), ClientId) == vDesiredClientIds.end())
			vDesiredClientIds.push_back(ClientId);
	};

	// Keep original behavior: always register local main + dummy.
	std::array<int, 2> aLocalIds = {
		GameClient()->m_aLocalIds[IClient::CONN_MAIN],
		GameClient()->m_aLocalIds[IClient::CONN_DUMMY]};
	for(const int LocalId : aLocalIds)
		AddDesiredClientId(LocalId);

	// Separate feature: register all players visible in snapshot.
	if(g_Config.m_ClZzBestclientRegisterAll)
	{
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
			AddDesiredClientId(ClientId);
	}

	// Separate feature: register configured nicknames.
	if(g_Config.m_ClZzBestclientRegisterNicksEnabled)
	{
		const std::vector<std::string> vConfiguredNicknames = ParseConfiguredNicknames();
		std::unordered_set<std::string> DesiredNicknameSet(vConfiguredNicknames.begin(), vConfiguredNicknames.end());
		for(auto It = m_MissingConfiguredNicknames.begin(); It != m_MissingConfiguredNicknames.end();)
		{
			if(DesiredNicknameSet.find(*It) == DesiredNicknameSet.end())
				It = m_MissingConfiguredNicknames.erase(It);
			else
				++It;
		}

		for(const std::string &Nickname : vConfiguredNicknames)
		{
			const int ClientId = FindClientIdByName(Nickname.c_str());
			if(ClientId >= 0)
			{
				AddDesiredClientId(ClientId);
				m_MissingConfiguredNicknames.erase(Nickname);
				continue;
			}

			if(m_MissingConfiguredNicknames.insert(Nickname).second)
			{
				PrintConsoleMessage("nickname '%s' not found in current snapshot", Nickname.c_str());
				DebugLog("configured nickname '%s' not found in current snapshot", Nickname.c_str());
			}
		}
	}
	else
	{
		m_MissingConfiguredNicknames.clear();
	}

	for(const int ClientId : vDesiredClientIds)
	{
		if(m_RegisteredClientIds.find(ClientId) == m_RegisteredClientIds.end())
		{
			SendPresencePacket(ClientId, PACKET_JOIN);
			m_RegisteredClientIds.insert(ClientId);
		}
	}

	for(auto It = m_RegisteredClientIds.begin(); It != m_RegisteredClientIds.end();)
	{
		const bool Keep = std::find(vDesiredClientIds.begin(), vDesiredClientIds.end(), *It) != vDesiredClientIds.end();
		if(!Keep)
		{
			SendPresencePacket(*It, PACKET_LEAVE);
			It = m_RegisteredClientIds.erase(It);
		}
		else
		{
			++It;
		}
	}
}

void CZzBestClientPresence::PrintConsoleMessage(const char *pFormat, ...) const
{
	char aBuf[1024];
	va_list Args;
	va_start(Args, pFormat);
	str_format_v(aBuf, sizeof(aBuf), pFormat, Args);
	va_end(Args);
	Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, LOG_SCOPE, aBuf);
}

void CZzBestClientPresence::UpdatePresence()
{
	EnsurePresenceSocket();
	if(!m_Socket || !m_HasIndicatorServerAddr)
		return;

	const char *pCurrentGameServer = CurrentGameServerAddress();
	if(pCurrentGameServer[0] == '\0')
	{
		SendLeaveForAll();
		m_RegisteredClientIds.clear();
		m_LastHeartbeatTick = 0;
		m_aCurrentGameServerAddress[0] = '\0';
		m_aLastGameServerAddress[0] = '\0';
		DebugLog("skip presence: current game server empty");
		return;
	}

	if(str_comp(m_aLastGameServerAddress, pCurrentGameServer) != 0)
	{
		if(!m_RegisteredClientIds.empty())
		{
			DebugLog("game server changed old=%s new=%s -> leave old registrations", m_aLastGameServerAddress, pCurrentGameServer);
			SendLeaveForAll();
		}
		str_copy(m_aLastGameServerAddress, pCurrentGameServer, sizeof(m_aLastGameServerAddress));
		m_RegisteredClientIds.clear();
		m_LastHeartbeatTick = 0;
	}

	SyncLocalRegistrations();

	const int64_t Now = time_get();
	if(m_LastHeartbeatTick == 0 || Now - m_LastHeartbeatTick > HEARTBEAT_SECONDS * time_freq())
	{
		for(const int ClientId : m_RegisteredClientIds)
			SendPresencePacket(ClientId, PACKET_HEARTBEAT);
		m_LastHeartbeatTick = Now;
		DebugLog("heartbeat tick for %d local clients", (int)m_RegisteredClientIds.size());
	}
}

void CZzBestClientPresence::DebugLog(const char *pFormat, ...) const
{
	char aBuf[1024];
	va_list Args;
	va_start(Args, pFormat);
	str_format_v(aBuf, sizeof(aBuf), pFormat, Args);
	va_end(Args);
	log_info(LOG_SCOPE, "%s", aBuf);
}

