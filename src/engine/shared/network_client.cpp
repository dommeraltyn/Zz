/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "network.h"

#include <base/log.h>
#include <base/system.h>
#include <base/types.h>

#include <engine/shared/config.h>
#include <engine/shared/protocol7.h>

#include <algorithm>

namespace
{
bool ParseProxyAddress(const char *pAddress, int LookupTypes, NETADDR *pOut)
{
	if(!pAddress || pAddress[0] == '\0')
		return false;

	char aAddress[256];
	str_copy(aAddress, pAddress, sizeof(aAddress));
	char *pTrimmed = str_skip_whitespaces(aAddress);
	str_utf8_trim_right(pTrimmed);
	if(pTrimmed[0] == '\0')
		return false;

	NETADDR Addr;
	if(net_addr_from_str(&Addr, pTrimmed) == 0)
	{
		*pOut = Addr;
		return true;
	}

	char aHost[256] = "";
	unsigned short Port = 0;

	const int UrlParseResult = net_addr_from_url(&Addr, pTrimmed, aHost, sizeof(aHost));
	if(UrlParseResult == 0)
	{
		*pOut = Addr;
		return true;
	}

	if(pTrimmed[0] == '[')
	{
		const char *pClose = str_find(pTrimmed, "]");
		if(pClose)
		{
			const int HostLen = std::min((int)(pClose - pTrimmed - 1), (int)sizeof(aHost) - 1);
			if(HostLen > 0)
			{
				mem_copy(aHost, pTrimmed + 1, HostLen);
				aHost[HostLen] = '\0';
			}
			if(pClose[1] == ':' && pClose[2] != '\0')
			{
				const int ParsedPort = str_toint(pClose + 2);
				if(ParsedPort > 0 && ParsedPort <= 65535)
					Port = (unsigned short)ParsedPort;
			}
		}
	}
	else
	{
		const char *pLastColon = str_rchr(pTrimmed, ':');
		const char *pFirstColon = str_find(pTrimmed, ":");
		if(pLastColon && pFirstColon == pLastColon)
		{
			const int HostLen = std::min((int)(pLastColon - pTrimmed), (int)sizeof(aHost) - 1);
			if(HostLen > 0)
			{
				mem_copy(aHost, pTrimmed, HostLen);
				aHost[HostLen] = '\0';
				const int ParsedPort = str_toint(pLastColon + 1);
				if(ParsedPort > 0 && ParsedPort <= 65535)
					Port = (unsigned short)ParsedPort;
			}
		}
		else
		{
			str_copy(aHost, pTrimmed, sizeof(aHost));
		}
	}

	if(aHost[0] == '\0')
		return false;
	if(net_host_lookup(aHost, &Addr, LookupTypes) != 0)
		return false;
	if(Port != 0)
		Addr.port = Port;
	*pOut = Addr;
	return true;
}

bool ConfigureSocks5Proxy(CNetClient *pNetClient)
{
	const bool UseOverride = pNetClient->HasProxyOverride();
	const bool ProxyEnabled = UseOverride ? pNetClient->ProxyOverrideEnabled() : (g_Config.m_ClZzProxyEnabled && g_Config.m_ClZzProxyAddress[0] != '\0');
	if(!ProxyEnabled)
	{
		net_udp_set_proxy(pNetClient->m_Socket, nullptr, nullptr, nullptr);
		return true;
	}

	NETADDR ProxyAddr;
	if(UseOverride)
	{
		ProxyAddr = pNetClient->ProxyOverrideAddr();
	}
	else
	{
		const int LookupTypesRaw = pNetClient->NetType() & (NETTYPE_IPV4 | NETTYPE_IPV6);
		const int LookupTypes = LookupTypesRaw == 0 ? (NETTYPE_IPV4 | NETTYPE_IPV6) : LookupTypesRaw;
		if(!ParseProxyAddress(g_Config.m_ClZzProxyAddress, LookupTypes, &ProxyAddr))
		{
			log_error("net", "could not resolve SOCKS5 proxy address '%s'", g_Config.m_ClZzProxyAddress);
			net_udp_set_proxy(pNetClient->m_Socket, nullptr, nullptr, nullptr);
			return false;
		}
	}

	if(ProxyAddr.port == 0)
		ProxyAddr.port = 1080;

	const char *pUser = UseOverride ? pNetClient->ProxyOverrideUser() : g_Config.m_ClZzProxyUsername;
	const char *pPass = UseOverride ? pNetClient->ProxyOverridePassword() : g_Config.m_ClZzProxyPassword;
	if(net_udp_set_proxy(pNetClient->m_Socket, &ProxyAddr, pUser, pPass) != 0)
	{
		if(UseOverride)
		{
			char aAddr[NETADDR_MAXSTRSIZE];
			net_addr_str(&ProxyAddr, aAddr, sizeof(aAddr), true);
			log_error("net", "failed to initialize SOCKS5 proxy '%s'", aAddr);
		}
		else
			log_error("net", "failed to initialize SOCKS5 proxy '%s'", g_Config.m_ClZzProxyAddress);
		net_udp_set_proxy(pNetClient->m_Socket, nullptr, nullptr, nullptr);
		return false;
	}
	return true;
}

bool IsRecoverableNetError(const char *pError)
{
	if(!pError || pError[0] == '\0')
		return false;
	return str_find_nocase(pError, "timeout") != nullptr ||
		str_find_nocase(pError, "too weak connection") != nullptr;
}

} // namespace

bool CNetClient::Open(NETADDR BindAddr)
{
	// open socket
	NETSOCKET Socket;
	Socket = net_udp_create(BindAddr);
	if(!Socket)
		return false;
	Close();
	// clean it
	*this = CNetClient{};

	// init
	m_Socket = Socket;
	m_pStun = new CStun(m_Socket);
	m_Connection.Init(m_Socket, false);
	m_TokenCache.Init(m_Socket);

	return true;
}

void CNetClient::Close()
{
	if(!m_Socket)
	{
		return;
	}
	if(m_pStun)
	{
		delete m_pStun;
		m_pStun = nullptr;
	}
	net_udp_close(m_Socket);
	m_Socket = nullptr;
}

void CNetClient::Disconnect(const char *pReason)
{
	m_Connection.Disconnect(pReason);
}

void CNetClient::Update()
{
	const bool ProxyActive = HasProxyOverride() ? ProxyOverrideEnabled() : g_Config.m_ClZzProxyEnabled;
	if(ProxyActive && m_Connection.State() == CNetConnection::EState::ONLINE)
	{
		const int64_t Now = time_get();
		const int64_t Silence = Now - m_Connection.LastRecvTime();
		if(Silence > time_freq())
		{
			// Keep the current in-game session alive while transport proxy is being repaired.
			m_Connection.KeepAliveForTransportStall();
			m_ProxyStallActive = true;
			m_Connection.SignalResend();
			m_Connection.Flush();

			// Re-associate UDP relay only when stall persists, with backoff to avoid log/request spam.
			if(Silence > 3 * time_freq() &&
				(m_LastProxyRepairAttempt == 0 || Now - m_LastProxyRepairAttempt > (int64_t)m_ProxyRepairBackoffSeconds * time_freq()))
			{
				m_LastProxyRepairAttempt = Now;
				if(ConfigureSocks5Proxy(this))
					m_ProxyRepairBackoffSeconds = std::min(m_ProxyRepairBackoffSeconds * 2, 20);
			}
		}
		else
		{
			m_ProxyStallActive = false;
			m_ProxyRepairBackoffSeconds = 3;
		}
	}
	else
	{
		m_ProxyStallActive = false;
		m_ProxyRepairBackoffSeconds = 3;
	}

	m_Connection.Update();
	if(m_Connection.State() == CNetConnection::EState::ERROR)
	{
		const bool RecoverableProxyError = ProxyActive && IsRecoverableNetError(m_Connection.ErrorString());
		if(RecoverableProxyError)
		{
			// Do not reconnect/reset: recover the existing session and continue traffic.
			const int64_t Now = time_get();
			if(m_LastProxyRepairAttempt == 0 || Now - m_LastProxyRepairAttempt > (int64_t)m_ProxyRepairBackoffSeconds * time_freq())
			{
				m_LastProxyRepairAttempt = Now;
				ConfigureSocks5Proxy(this);
				m_ProxyRepairBackoffSeconds = std::min(m_ProxyRepairBackoffSeconds * 2, 20);
			}
			if(m_Connection.RecoverFromTimeoutError())
			{
				m_ProxyRepairBackoffSeconds = 3;
				m_Connection.SignalResend();
				m_Connection.Flush();
			}
		}
		else
			Disconnect(m_Connection.ErrorString());
	}
	m_pStun->Update();
	m_TokenCache.Update();
}

void CNetClient::Connect(const NETADDR *pAddr, int NumAddrs)
{
	m_ProxyStallActive = false;
	m_LastProxyRepairAttempt = 0;
	m_ProxyRepairBackoffSeconds = 3;
	ConfigureSocks5Proxy(this);
	m_Connection.Connect(pAddr, NumAddrs);
}

void CNetClient::Connect7(const NETADDR *pAddr, int NumAddrs)
{
	m_ProxyStallActive = false;
	m_LastProxyRepairAttempt = 0;
	m_ProxyRepairBackoffSeconds = 3;
	ConfigureSocks5Proxy(this);
	m_Connection.Connect7(pAddr, NumAddrs);
}

void CNetClient::SetProxyOverride(bool Enabled, const NETADDR *pAddr, const char *pUsername, const char *pPassword)
{
	m_ProxyOverrideSet = true;
	m_ProxyOverrideEnabled = Enabled && pAddr != nullptr;
	if(pAddr)
		m_ProxyOverrideAddr = *pAddr;
	else
		mem_zero(&m_ProxyOverrideAddr, sizeof(m_ProxyOverrideAddr));
	str_copy(m_aProxyOverrideUser, pUsername ? pUsername : "", sizeof(m_aProxyOverrideUser));
	str_copy(m_aProxyOverridePassword, pPassword ? pPassword : "", sizeof(m_aProxyOverridePassword));
}

void CNetClient::ClearProxyOverride()
{
	m_ProxyOverrideSet = false;
	m_ProxyOverrideEnabled = false;
	mem_zero(&m_ProxyOverrideAddr, sizeof(m_ProxyOverrideAddr));
	m_aProxyOverrideUser[0] = '\0';
	m_aProxyOverridePassword[0] = '\0';
}

void CNetClient::ResetErrorString()
{
	m_Connection.ResetErrorString();
}

int CNetClient::Recv(CNetChunk *pChunk, SECURITY_TOKEN *pResponseToken, bool Sixup)
{
	while(true)
	{
		// Unpack next chunk from stored packet if available
		if(m_PacketChunkUnpacker.UnpackNextChunk(pChunk))
			return 1;

		// TODO: empty the recvinfo
		NETADDR Addr;
		unsigned char *pData;
		int Bytes = net_udp_recv(m_Socket, &Addr, &pData);

		// no more packets for now
		if(Bytes <= 0)
			break;

		if(m_pStun->OnPacket(Addr, pData, Bytes))
		{
			continue;
		}

		SECURITY_TOKEN Token;
		*pResponseToken = NET_SECURITY_TOKEN_UNKNOWN;
		if(CNetBase::UnpackPacket(pData, Bytes, &m_RecvBuffer, Sixup, &Token, pResponseToken) == 0)
		{
			if(Sixup)
			{
				Addr.type |= NETTYPE_TW7;
			}
			if(m_RecvBuffer.m_Flags & NET_PACKETFLAG_CONNLESS)
			{
				pChunk->m_Flags = NETSENDFLAG_CONNLESS;
				pChunk->m_ClientId = -1;
				pChunk->m_Address = Addr;
				pChunk->m_DataSize = m_RecvBuffer.m_DataSize;
				pChunk->m_pData = m_RecvBuffer.m_aChunkData;
				if(m_RecvBuffer.m_Flags & NET_PACKETFLAG_EXTENDED)
				{
					pChunk->m_Flags |= NETSENDFLAG_EXTENDED;
					mem_copy(pChunk->m_aExtraData, m_RecvBuffer.m_aExtraData, sizeof(pChunk->m_aExtraData));
				}
				return 1;
			}
			else
			{
				const bool Control = (m_RecvBuffer.m_Flags & NET_PACKETFLAG_CONTROL) != 0;
				if(Sixup &&
					Control &&
					m_RecvBuffer.m_DataSize >= 1 + (int)sizeof(SECURITY_TOKEN) &&
					m_RecvBuffer.m_aChunkData[0] == protocol7::NET_CTRLMSG_TOKEN)
				{
					m_TokenCache.AddToken(&Addr, *pResponseToken);
				}
				if(m_Connection.State() != CNetConnection::EState::OFFLINE &&
					m_Connection.State() != CNetConnection::EState::ERROR &&
					m_Connection.Feed(&m_RecvBuffer, &Addr, Token, *pResponseToken))
				{
					if(!Control &&
						m_RecvBuffer.m_DataSize > 0 &&
						m_RecvBuffer.m_NumChunks > 0)
					{
						m_PacketChunkUnpacker.FeedPacket(Addr, m_RecvBuffer, &m_Connection, 0);
					}
				}
			}
		}
	}
	return 0;
}

int CNetClient::Send(CNetChunk *pChunk)
{
	if(pChunk->m_DataSize >= NET_MAX_PAYLOAD)
	{
		dbg_msg("netclient", "chunk payload too big. %d. dropping chunk", pChunk->m_DataSize);
		return -1;
	}

	if(pChunk->m_Flags & NETSENDFLAG_CONNLESS)
	{
		// send connectionless packet
		if(pChunk->m_Address.type & NETTYPE_TW7)
		{
			m_TokenCache.SendPacketConnless(pChunk);
		}
		else
		{
			CNetBase::SendPacketConnless(m_Socket, &pChunk->m_Address, pChunk->m_pData, pChunk->m_DataSize,
				pChunk->m_Flags & NETSENDFLAG_EXTENDED, pChunk->m_aExtraData);
		}
	}
	else
	{
		int Flags = 0;
		dbg_assert(pChunk->m_ClientId == 0, "erroneous client id");

		if(pChunk->m_Flags & NETSENDFLAG_VITAL)
			Flags = NET_CHUNKFLAG_VITAL;

		m_Connection.QueueChunk(Flags, pChunk->m_DataSize, pChunk->m_pData);

		if(pChunk->m_Flags & NETSENDFLAG_FLUSH)
			m_Connection.Flush();
	}
	return 0;
}

int CNetClient::State()
{
	if(m_Connection.State() == CNetConnection::EState::ONLINE)
		return NETSTATE_ONLINE;
	if(m_Connection.State() == CNetConnection::EState::OFFLINE)
		return NETSTATE_OFFLINE;
	return NETSTATE_CONNECTING;
}

int CNetClient::Flush()
{
	return m_Connection.Flush();
}

bool CNetClient::GotProblems(int64_t MaxLatency) const
{
	return time_get() - m_Connection.LastRecvTime() > MaxLatency;
}

const char *CNetClient::ErrorString() const
{
	return m_Connection.ErrorString();
}

void CNetClient::FeedStunServer(NETADDR StunServer)
{
	m_pStun->FeedStunServer(StunServer);
}

void CNetClient::RefreshStun()
{
	m_pStun->Refresh();
}

CONNECTIVITY CNetClient::GetConnectivity(int NetType, NETADDR *pGlobalAddr)
{
	return m_pStun->GetConnectivity(NetType, pGlobalAddr);
}
