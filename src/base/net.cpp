/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "net.h"

#include "dbg.h"
#include "log.h"
#include "mem.h"
#include "str.h"
#include "time.h"
#include "windows.h"

#include <algorithm>
#include <chrono>
#include <iterator> // std::size
#include <string_view>

#if defined(CONF_WEBSOCKETS)
#include <engine/shared/websockets.h>
#endif

#if defined(CONF_FAMILY_UNIX)
#include <sys/time.h> // timeval
#include <unistd.h> // close

// UNIX net includes
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#if defined(CONF_PLATFORM_SOLARIS)
#include <sys/filio.h> // FIONBIO
#endif

#include <cerrno>
#elif defined(CONF_FAMILY_WINDOWS)
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#error NOT IMPLEMENTED
#endif

static NETSTATS network_stats = {0};

#define VLEN 128
#define PACKETSIZE 1400
typedef struct
{
#ifdef CONF_PLATFORM_LINUX
	int pos;
	int size;
	struct mmsghdr msgs[VLEN];
	struct iovec iovecs[VLEN];
	char bufs[VLEN][PACKETSIZE];
	char sockaddrs[VLEN][128];
#else
	char buf[PACKETSIZE];
#endif
} NETSOCKET_BUFFER;

void net_buffer_init(NETSOCKET_BUFFER *buffer)
{
#if defined(CONF_PLATFORM_LINUX)
	buffer->pos = 0;
	buffer->size = 0;
	mem_zero(buffer->msgs, sizeof(buffer->msgs));
	mem_zero(buffer->iovecs, sizeof(buffer->iovecs));
	mem_zero(buffer->sockaddrs, sizeof(buffer->sockaddrs));
	for(int i = 0; i < VLEN; ++i)
	{
		buffer->iovecs[i].iov_base = buffer->bufs[i];
		buffer->iovecs[i].iov_len = PACKETSIZE;
		buffer->msgs[i].msg_hdr.msg_iov = &(buffer->iovecs[i]);
		buffer->msgs[i].msg_hdr.msg_iovlen = 1;
		buffer->msgs[i].msg_hdr.msg_name = &(buffer->sockaddrs[i]);
		buffer->msgs[i].msg_hdr.msg_namelen = sizeof(buffer->sockaddrs[i]);
	}
#endif
}

void net_buffer_reinit(NETSOCKET_BUFFER *buffer)
{
#if defined(CONF_PLATFORM_LINUX)
	for(int i = 0; i < VLEN; i++)
	{
		buffer->msgs[i].msg_hdr.msg_namelen = sizeof(buffer->sockaddrs[i]);
	}
#endif
}

void net_buffer_simple(NETSOCKET_BUFFER *buffer, char **buf, int *size)
{
#if defined(CONF_PLATFORM_LINUX)
	*buf = buffer->bufs[0];
	*size = sizeof(buffer->bufs[0]);
#else
	*buf = buffer->buf;
	*size = sizeof(buffer->buf);
#endif
}

struct NETSOCKET_INTERNAL
{
	int type;
	int ipv4sock;
	int ipv6sock;
	int web_ipv4sock;
	int web_ipv6sock;

	NETSOCKET_BUFFER buffer;
	bool socks5_proxy_enabled;
	NETADDR socks5_proxy_addr;
	NETADDR socks5_udp_relay_addr;
	NETSOCKET socks5_tcp_socket;
	char socks5_proxy_username[128];
	char socks5_proxy_password[128];
	int64_t socks5_last_reconnect_attempt;
	int64_t socks5_next_reconnect_after;
	int socks5_reconnect_fail_count;
	unsigned char socks5_udp_packet_buffer[PACKETSIZE + 64];
};
static NETSOCKET_INTERNAL invalid_socket = {NETTYPE_INVALID, -1, -1, -1, -1};

static bool IsZeroAddress(const NETADDR *pAddr)
{
	for(unsigned char Byte : pAddr->ip)
	{
		if(Byte != 0)
			return false;
	}
	return true;
}

static bool IsLoopbackAddress(const NETADDR *pAddr)
{
	if(pAddr->type & NETTYPE_IPV4)
		return pAddr->ip[0] == 127;
	if(pAddr->type & NETTYPE_IPV6)
	{
		for(int i = 0; i < 15; i++)
		{
			if(pAddr->ip[i] != 0)
				return false;
		}
		return pAddr->ip[15] == 1;
	}
	return false;
}

static void NetUdpDisableProxy(NETSOCKET pSock, bool ClearProxyConfig)
{
	if(pSock->socks5_tcp_socket)
	{
		net_tcp_close(pSock->socks5_tcp_socket);
		pSock->socks5_tcp_socket = nullptr;
	}
	pSock->socks5_proxy_enabled = false;
	pSock->socks5_last_reconnect_attempt = 0;
	pSock->socks5_next_reconnect_after = 0;
	pSock->socks5_reconnect_fail_count = 0;
	if(ClearProxyConfig)
	{
		mem_zero(&pSock->socks5_proxy_addr, sizeof(pSock->socks5_proxy_addr));
		pSock->socks5_proxy_addr.type = NETTYPE_INVALID;
		pSock->socks5_proxy_username[0] = '\0';
		pSock->socks5_proxy_password[0] = '\0';
	}
	pSock->socks5_udp_relay_addr.type = NETTYPE_INVALID;
}

static bool NetUdpTryRestoreProxy(NETSOCKET pSock);

static bool NetTcpSendAll(NETSOCKET pSock, const void *pData, int DataSize)
{
	const unsigned char *pBytes = (const unsigned char *)pData;
	int Sent = 0;
	while(Sent < DataSize)
	{
		const int Result = net_tcp_send(pSock, pBytes + Sent, DataSize - Sent);
		if(Result <= 0)
			return false;
		Sent += Result;
	}
	return true;
}

static bool NetTcpRecvAll(NETSOCKET pSock, void *pData, int DataSize)
{
	unsigned char *pBytes = (unsigned char *)pData;
	int Received = 0;
	while(Received < DataSize)
	{
		if(!net_socket_read_wait(pSock, std::chrono::milliseconds(120)))
			return false;
		const int Result = net_tcp_recv(pSock, pBytes + Received, DataSize - Received);
		if(Result <= 0)
			return false;
		Received += Result;
	}
	return true;
}

const NETADDR NETADDR_ZEROED = {NETTYPE_INVALID, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0};

static void netaddr_to_sockaddr_in(const NETADDR *src, sockaddr_in *dest)
{
	dbg_assert((src->type & NETTYPE_IPV4) != 0, "Invalid address type '%d' for netaddr_to_sockaddr_in", src->type);
	mem_zero(dest, sizeof(*dest));
	dest->sin_family = AF_INET;
	dest->sin_port = htons(src->port);
	mem_copy(&dest->sin_addr.s_addr, src->ip, 4);
}

static void netaddr_to_sockaddr_in6(const NETADDR *src, sockaddr_in6 *dest)
{
	dbg_assert((src->type & NETTYPE_IPV6) != 0, "Invalid address type '%d' for netaddr_to_sockaddr_in6", src->type);
	mem_zero(dest, sizeof(*dest));
	dest->sin6_family = AF_INET6;
	dest->sin6_port = htons(src->port);
	mem_copy(&dest->sin6_addr.s6_addr, src->ip, 16);
}

static void sockaddr_to_netaddr(const sockaddr *src, socklen_t src_len, NETADDR *dst)
{
	*dst = NETADDR_ZEROED;
	if(src->sa_family == AF_INET && src_len >= (socklen_t)sizeof(sockaddr_in))
	{
		const sockaddr_in *src_in = (const sockaddr_in *)src;
		dst->type = NETTYPE_IPV4;
		dst->port = htons(src_in->sin_port);
		static_assert(sizeof(dst->ip) >= sizeof(src_in->sin_addr.s_addr));
		mem_copy(dst->ip, &src_in->sin_addr.s_addr, sizeof(src_in->sin_addr.s_addr));
	}
	else if(src->sa_family == AF_INET6 && src_len >= (socklen_t)sizeof(sockaddr_in6))
	{
		const sockaddr_in6 *src_in6 = (const sockaddr_in6 *)src;
		dst->type = NETTYPE_IPV6;
		dst->port = htons(src_in6->sin6_port);
		static_assert(sizeof(dst->ip) >= sizeof(src_in6->sin6_addr.s6_addr));
		mem_copy(dst->ip, &src_in6->sin6_addr.s6_addr, sizeof(src_in6->sin6_addr.s6_addr));
	}
	else
	{
		log_warn("net", "Cannot convert sockaddr of family %d", src->sa_family);
	}
}

int net_addr_comp(const NETADDR *a, const NETADDR *b)
{
	int diff = a->type - b->type;
	if(diff != 0)
	{
		return diff;
	}
	diff = mem_comp(a->ip, b->ip, sizeof(a->ip));
	if(diff != 0)
	{
		return diff;
	}
	return a->port - b->port;
}

bool NETADDR::operator==(const NETADDR &other) const
{
	return net_addr_comp(this, &other) == 0;
}

bool NETADDR::operator!=(const NETADDR &other) const
{
	return net_addr_comp(this, &other) != 0;
}

bool NETADDR::operator<(const NETADDR &other) const
{
	return net_addr_comp(this, &other) < 0;
}

size_t std::hash<NETADDR>::operator()(const NETADDR &Addr) const noexcept
{
	size_t seed = std::hash<unsigned int>{}(Addr.type);
	seed ^= std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char *>(Addr.ip), sizeof(Addr.ip))) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
	seed ^= std::hash<unsigned short>{}(Addr.port) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
	return seed;
}

int net_addr_comp_noport(const NETADDR *a, const NETADDR *b)
{
	int diff = a->type - b->type;
	if(diff != 0)
	{
		return diff;
	}
	return mem_comp(a->ip, b->ip, sizeof(a->ip));
}

void net_addr_str_v6(const unsigned short ip[8], int port, char *buffer, int buffer_size)
{
	int longest_seq_len = 0;
	int longest_seq_start = -1;
	int w = 0;
	int i;
	{
		int seq_len = 0;
		int seq_start = -1;
		// Determine longest sequence of zeros.
		for(i = 0; i < 8 + 1; i++)
		{
			if(seq_start != -1)
			{
				if(i == 8 || ip[i] != 0)
				{
					if(longest_seq_len < seq_len)
					{
						longest_seq_len = seq_len;
						longest_seq_start = seq_start;
					}
					seq_len = 0;
					seq_start = -1;
				}
				else
				{
					seq_len += 1;
				}
			}
			else
			{
				if(i != 8 && ip[i] == 0)
				{
					seq_start = i;
					seq_len = 1;
				}
			}
		}
	}
	if(longest_seq_len <= 1)
	{
		longest_seq_len = 0;
		longest_seq_start = -1;
	}
	w += str_copy(buffer + w, "[", buffer_size - w);
	for(i = 0; i < 8; i++)
	{
		if(longest_seq_start <= i && i < longest_seq_start + longest_seq_len)
		{
			if(i == longest_seq_start)
			{
				w += str_copy(buffer + w, "::", buffer_size - w);
			}
		}
		else
		{
			const char *colon = (i == 0 || i == longest_seq_start + longest_seq_len) ? "" : ":";
			w += str_format(buffer + w, buffer_size - w, "%s%x", colon, ip[i]);
		}
	}
	w += str_copy(buffer + w, "]", buffer_size - w);
	if(port >= 0)
	{
		str_format(buffer + w, buffer_size - w, ":%d", port);
	}
}

void net_addr_str(const NETADDR *addr, char *string, int max_length, bool add_port)
{
	if((addr->type & (NETTYPE_IPV4 | NETTYPE_WEBSOCKET_IPV4)) != 0)
	{
		if(add_port)
		{
			str_format(string, max_length, "%d.%d.%d.%d:%d", addr->ip[0], addr->ip[1], addr->ip[2], addr->ip[3], addr->port);
		}
		else
		{
			str_format(string, max_length, "%d.%d.%d.%d", addr->ip[0], addr->ip[1], addr->ip[2], addr->ip[3]);
		}
	}
	else if((addr->type & (NETTYPE_IPV6 | NETTYPE_WEBSOCKET_IPV6)) != 0)
	{
		unsigned short ip[8];
		for(int i = 0; i < 8; i++)
		{
			ip[i] = (addr->ip[i * 2] << 8) | (addr->ip[i * 2 + 1]);
		}
		int port = add_port ? addr->port : -1;
		net_addr_str_v6(ip, port, string, max_length);
	}
	else
	{
		dbg_assert_failed("unknown NETADDR type %d", addr->type);
	}
}

static int parse_int(int *out, const char **str)
{
	int i = 0;
	*out = 0;
	if(!str_isnum(**str))
		return -1;

	i = **str - '0';
	(*str)++;

	while(true)
	{
		if(!str_isnum(**str))
		{
			*out = i;
			return 0;
		}

		i = (i * 10) + (**str - '0');
		(*str)++;
	}

	return 0;
}

static int parse_char(char c, const char **str)
{
	if(**str != c)
		return -1;
	(*str)++;
	return 0;
}

static int parse_uint8(unsigned char *out, const char **str)
{
	int i;
	if(parse_int(&i, str) != 0)
		return -1;
	if(i < 0 || i > 0xff)
		return -1;
	*out = i;
	return 0;
}

static int parse_uint16(unsigned short *out, const char **str)
{
	int i;
	if(parse_int(&i, str) != 0)
		return -1;
	if(i < 0 || i > 0xffff)
		return -1;
	*out = i;
	return 0;
}

int net_addr_from_url(NETADDR *addr, const char *string, char *host_buf, size_t host_buf_size)
{
	bool sixup = false;
	mem_zero(addr, sizeof(*addr));
	const char *str = str_startswith(string, "tw-0.6+udp://");
	if(!str && (str = str_startswith(string, "tw-0.7+udp://")))
	{
		addr->type |= NETTYPE_TW7;
		sixup = true;
	}
	if(!str)
		return 1;

	int length = str_length(str);
	int start = 0;
	int end = length;
	for(int i = 0; i < length; i++)
	{
		if(str[i] == '@')
		{
			if(start != 0)
			{
				// Two at signs.
				return true;
			}
			start = i + 1;
		}
		else if(str[i] == '/' || str[i] == '?' || str[i] == '#')
		{
			end = i;
			break;
		}
	}

	char host[128];
	str_truncate(host, sizeof(host), str + start, end - start);
	if(host_buf)
		str_copy(host_buf, host, host_buf_size);

	int failure = net_addr_from_str(addr, host);
	if(failure)
		return failure;

	if(sixup)
		addr->type |= NETTYPE_TW7;

	return failure;
}

bool net_addr_is_local(const NETADDR *addr)
{
	char addr_str[NETADDR_MAXSTRSIZE];
	net_addr_str(addr, addr_str, sizeof(addr_str), true);

	if(addr->ip[0] == 127 || addr->ip[0] == 10 || (addr->ip[0] == 192 && addr->ip[1] == 168) || (addr->ip[0] == 172 && (addr->ip[1] >= 16 && addr->ip[1] <= 31)))
		return true;

	if(str_startswith(addr_str, "[fe80:") || str_startswith(addr_str, "[::1"))
		return true;

	return false;
}

int net_addr_from_str(NETADDR *addr, const char *string)
{
	const char *str = string;
	mem_zero(addr, sizeof(NETADDR));

	if(str[0] == '[')
	{
		/* ipv6 */
		sockaddr_in6 sa6;
		char buf[128];
		int i;
		str++;
		for(i = 0; i < 127 && str[i] && str[i] != ']'; i++)
			buf[i] = str[i];
		buf[i] = 0;
		str += i;
#if defined(CONF_FAMILY_WINDOWS)
		{
			int size;
			sa6.sin6_family = AF_INET6;
			size = (int)sizeof(sa6);
			if(WSAStringToAddressA(buf, AF_INET6, nullptr, (sockaddr *)&sa6, &size) != 0)
				return -1;
		}
#else
		sa6.sin6_family = AF_INET6;

		if(inet_pton(AF_INET6, buf, &sa6.sin6_addr) != 1)
			return -1;
#endif
		sockaddr_to_netaddr((sockaddr *)&sa6, sizeof(sa6), addr);

		if(*str == ']')
		{
			str++;
			if(*str == ':')
			{
				str++;
				if(parse_uint16(&addr->port, &str))
					return -1;
			}
			else
			{
				addr->port = 0;
			}
		}
		else
			return -1;

		return 0;
	}
	else
	{
		/* ipv4 */
		if(parse_uint8(&addr->ip[0], &str))
			return -1;
		if(parse_char('.', &str))
			return -1;
		if(parse_uint8(&addr->ip[1], &str))
			return -1;
		if(parse_char('.', &str))
			return -1;
		if(parse_uint8(&addr->ip[2], &str))
			return -1;
		if(parse_char('.', &str))
			return -1;
		if(parse_uint8(&addr->ip[3], &str))
			return -1;
		if(*str == ':')
		{
			str++;
			if(parse_uint16(&addr->port, &str))
				return -1;
		}
		if(*str != '\0')
			return -1;

		addr->type = NETTYPE_IPV4;
	}

	return 0;
}

static int priv_net_extract(const char *hostname, char *host, int max_host, int *port)
{
	*port = 0;
	host[0] = 0;

	if(hostname[0] == '[')
	{
		// ipv6 mode
		int i;
		for(i = 1; i < max_host && hostname[i] && hostname[i] != ']'; i++)
			host[i - 1] = hostname[i];
		host[i - 1] = 0;
		if(hostname[i] != ']') // malformatted
			return -1;

		i++;
		if(hostname[i] == ':')
			*port = str_toint(hostname + i + 1);
	}
	else
	{
		// generic mode (ipv4, hostname etc)
		int i;
		for(i = 0; i < max_host - 1 && hostname[i] && hostname[i] != ':'; i++)
			host[i] = hostname[i];
		host[i] = 0;

		if(hostname[i] == ':')
			*port = str_toint(hostname + i + 1);
	}

	return 0;
}

static int net_host_lookup_fallback(const char *hostname, NETADDR *addr, int types, int port)
{
	if(str_comp_nocase(hostname, "localhost") == 0)
	{
		if(types == NETTYPE_IPV4)
		{
			dbg_assert(net_addr_from_str(addr, "127.0.0.1") == 0, "unreachable");
			addr->port = port;
			return 0;
		}
		else if(types == NETTYPE_IPV6)
		{
			dbg_assert(net_addr_from_str(addr, "[::1]") == 0, "unreachable");
			addr->port = port;
			return 0;
		}
		else
		{
			// TODO: return both IPv4 and IPv6 address
			dbg_assert(net_addr_from_str(addr, "127.0.0.1") == 0, "unreachable");
			addr->port = port;
			return 0;
		}
	}
	return -1;
}

static int net_host_lookup_impl(const char *hostname, NETADDR *addr, int types)
{
	char host[256];
	int port = 0;
	if(priv_net_extract(hostname, host, sizeof(host), &port))
		return -1;

	log_trace("host_lookup", "host='%s' port='%d' types='%d'", host, port, types);

	struct addrinfo hints;
	mem_zero(&hints, sizeof(hints));

	if(types == NETTYPE_IPV4)
		hints.ai_family = AF_INET;
	else if(types == NETTYPE_IPV6)
		hints.ai_family = AF_INET6;
	else
		hints.ai_family = AF_UNSPEC;

	struct addrinfo *result = nullptr;
	int e = getaddrinfo(host, nullptr, &hints, &result);
	if(!result)
	{
		return net_host_lookup_fallback(hostname, addr, types, port);
	}

	if(e != 0)
	{
		freeaddrinfo(result);
		return net_host_lookup_fallback(hostname, addr, types, port);
	}

	sockaddr_to_netaddr(result->ai_addr, result->ai_addrlen, addr);
	addr->port = port;
	freeaddrinfo(result);
	return 0;
}

int net_host_lookup(const char *hostname, NETADDR *addr, int types)
{
	const char *ws_hostname = str_startswith(hostname, "ws://");
	if(ws_hostname)
	{
		if((types & (NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_IPV6)) == 0)
		{
			return -1;
		}
		int result = net_host_lookup_impl(ws_hostname, addr, types & ~(NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_IPV6));
		if(result == 0)
		{
			if(addr->type == NETTYPE_IPV4)
			{
				addr->type = NETTYPE_WEBSOCKET_IPV4;
			}
			else if(addr->type == NETTYPE_IPV6)
			{
				addr->type = NETTYPE_WEBSOCKET_IPV6;
			}
		}
		return result;
	}
	return net_host_lookup_impl(hostname, addr, types & ~(NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_IPV6));
}

void net_init()
{
#if defined(CONF_FAMILY_WINDOWS)
	WSADATA wsa_data;
	dbg_assert(WSAStartup(MAKEWORD(1, 1), &wsa_data) == 0, "WSAStartup failure");
#endif
#if defined(CONF_WEBSOCKETS)
	websocket_init();
#endif
}

int net_errno()
{
#if defined(CONF_FAMILY_WINDOWS)
	return WSAGetLastError();
#else
	return errno;
#endif
}

std::string net_error_message()
{
	const int error = net_errno();
#if defined(CONF_FAMILY_WINDOWS)
	const std::string message = windows_format_system_message(error);
	return std::to_string(error) + " '" + message + "'";
#else
	return std::to_string(error) + " '" + strerror(error) + "'";
#endif
}

void net_stats(NETSTATS *stats_inout)
{
	*stats_inout = network_stats;
}

int net_socket_type(NETSOCKET sock)
{
	return sock->type;
}

static int net_set_blocking_impl(NETSOCKET sock, bool blocking)
{
	unsigned long mode = blocking ? 0 : 1;
	const char *mode_str = blocking ? "blocking" : "non-blocking";
	int sockets[] = {sock->ipv4sock, sock->ipv6sock};
	const char *socket_str[] = {"IPv4", "IPv6"};

	for(size_t i = 0; i < std::size(sockets); ++i)
	{
		if(sockets[i] >= 0)
		{
#if defined(CONF_FAMILY_WINDOWS)
			if(ioctlsocket(sockets[i], FIONBIO, (unsigned long *)&mode) != NO_ERROR)
			{
				log_error("net", "Setting %s mode for %s socket failed (%s)", socket_str[i], mode_str, net_error_message().c_str());
			}
#else
			if(ioctl(sockets[i], FIONBIO, (unsigned long *)&mode) == -1)
			{
				log_error("net", "Setting %s mode for %s socket failed (%s)", socket_str[i], mode_str, net_error_message().c_str());
			}
#endif
		}
	}

	return 0;
}

int net_set_non_blocking(NETSOCKET sock)
{
	return net_set_blocking_impl(sock, false);
}

int net_set_blocking(NETSOCKET sock)
{
	return net_set_blocking_impl(sock, true);
}

int net_would_block()
{
#if defined(CONF_FAMILY_WINDOWS)
	return net_errno() == WSAEWOULDBLOCK;
#else
	return net_errno() == EWOULDBLOCK;
#endif
}

int net_socket_read_wait(NETSOCKET sock, std::chrono::nanoseconds nanoseconds)
{
	const int64_t microseconds = std::chrono::duration_cast<std::chrono::microseconds>(nanoseconds).count();
	dbg_assert(microseconds >= 0, "Negative wait duration %" PRId64 " not allowed", microseconds);

	fd_set readfds;
	FD_ZERO(&readfds);

	int maxfd = -1;
	if(sock->ipv4sock >= 0)
	{
		FD_SET(sock->ipv4sock, &readfds);
		maxfd = sock->ipv4sock;
	}
	if(sock->ipv6sock >= 0)
	{
		FD_SET(sock->ipv6sock, &readfds);
		maxfd = std::max(maxfd, sock->ipv6sock);
	}
#if defined(CONF_WEBSOCKETS)
	if(sock->web_ipv4sock >= 0)
	{
		maxfd = std::max(maxfd, websocket_fd_set(sock->web_ipv4sock, &readfds));
	}
	if(sock->web_ipv6sock >= 0)
	{
		maxfd = std::max(maxfd, websocket_fd_set(sock->web_ipv6sock, &readfds));
	}
#endif
	if(maxfd < 0)
	{
		return 0;
	}

	struct timeval tv;
	tv.tv_sec = microseconds / 1000000;
	tv.tv_usec = microseconds % 1000000;
	// don't care about writefds and exceptfds
	select(maxfd + 1, &readfds, nullptr, nullptr, &tv);

	if(sock->ipv4sock >= 0 && FD_ISSET(sock->ipv4sock, &readfds))
	{
		return 1;
	}
	if(sock->ipv6sock >= 0 && FD_ISSET(sock->ipv6sock, &readfds))
	{
		return 1;
	}
#if defined(CONF_WEBSOCKETS)
	if(sock->web_ipv4sock >= 0 && websocket_fd_get(sock->web_ipv4sock, &readfds))
	{
		return 1;
	}
	if(sock->web_ipv6sock >= 0 && websocket_fd_get(sock->web_ipv6sock, &readfds))
	{
		return 1;
	}
#endif
	return 0;
}

static void priv_net_close_socket(int sock)
{
#if defined(CONF_FAMILY_WINDOWS)
	dbg_assert(closesocket(sock) == 0, "closesocket failure (%s)", net_error_message().c_str());
#else
	dbg_assert(close(sock) == 0, "close failure (%s)", net_error_message().c_str());
#endif
}

static void priv_net_close_all_sockets(NETSOCKET sock)
{
	if(sock->ipv4sock >= 0)
	{
		priv_net_close_socket(sock->ipv4sock);
		sock->ipv4sock = -1;
		sock->type &= ~NETTYPE_IPV4;
	}

#if defined(CONF_WEBSOCKETS)
	if(sock->web_ipv4sock >= 0)
	{
		websocket_destroy(sock->web_ipv4sock);
		sock->web_ipv4sock = -1;
		sock->type &= ~NETTYPE_WEBSOCKET_IPV4;
	}
#endif

	if(sock->ipv6sock >= 0)
	{
		priv_net_close_socket(sock->ipv6sock);
		sock->ipv6sock = -1;
		sock->type &= ~NETTYPE_IPV6;
	}

#if defined(CONF_WEBSOCKETS)
	if(sock->web_ipv6sock >= 0)
	{
		websocket_destroy(sock->web_ipv6sock);
		sock->web_ipv6sock = -1;
		sock->type &= ~NETTYPE_WEBSOCKET_IPV6;
	}
#endif

	free(sock);
}

static int priv_net_create_socket(int domain, int type, const NETADDR *bindaddr)
{
	int sock = socket(domain, type, 0);
	if(sock < 0)
	{
		log_error("net", "Failed to create socket with domain %d and type %d (%s)", domain, type, net_error_message().c_str());
		return -1;
	}

#if defined(CONF_FAMILY_UNIX)
	// On TCP sockets set SO_REUSEADDR to fix port rebind on restart
	if(domain == AF_INET && type == SOCK_STREAM)
	{
		int reuse_addr = 1;
		if(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse_addr, sizeof(reuse_addr)) != 0)
		{
			log_error("net", "Setting SO_REUSEADDR failed with domain %d and type %d (%s)", domain, type, net_error_message().c_str());
		}
	}
#elif defined(CONF_FAMILY_WINDOWS)
	{
		// Ensure exclusive use of address, otherwise it's possible on Windows to bind to the same address and port with another socket.
		// See https://learn.microsoft.com/en-us/windows/win32/winsock/using-so-reuseaddr-and-so-exclusiveaddruse (last update 06/14/2022)
		int exclusive_addr_use = 1;
		if(setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive_addr_use, sizeof(exclusive_addr_use)) != 0)
		{
			log_error("net", "Setting SO_EXCLUSIVEADDRUSE failed with domain %d and type %d (%s)", domain, type, net_error_message().c_str());
		}
	}
#endif

	// Set to IPv6-only if that's what we are creating, to ensure that dual-stack does not block the same IPv4 port.
#if defined(IPV6_V6ONLY)
	if(domain == AF_INET6)
	{
		int ipv6only = 1;
		if(setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&ipv6only, sizeof(ipv6only)) != 0)
		{
			log_error("net", "Setting IPV6_V6ONLY failed with domain %d and type %d (%s)", domain, type, net_error_message().c_str());
		}
	}
#endif

	sockaddr_storage addr;
	socklen_t addr_len;
	if(bindaddr->type == NETTYPE_IPV4)
	{
		netaddr_to_sockaddr_in(bindaddr, (sockaddr_in *)&addr);
		addr_len = sizeof(sockaddr_in);
	}
	else if(bindaddr->type == NETTYPE_IPV6)
	{
		netaddr_to_sockaddr_in6(bindaddr, (sockaddr_in6 *)&addr);
		addr_len = sizeof(sockaddr_in6);
	}
	else
	{
		dbg_assert_failed("socket type invalid: %d", type);
	}

	if(bind(sock, (sockaddr *)&addr, addr_len) != 0)
	{
		log_error("net", "Failed to bind socket with domain %d and type %d (%s)", domain, type, net_error_message().c_str());
		priv_net_close_socket(sock);
		return -1;
	}

	return sock;
}

NETSOCKET net_udp_create(NETADDR bindaddr)
{
	NETSOCKET sock = (NETSOCKET_INTERNAL *)malloc(sizeof(*sock));
	*sock = invalid_socket;

	if(bindaddr.type & NETTYPE_IPV4)
	{
		NETADDR bindaddr_ipv4 = bindaddr;
		bindaddr_ipv4.type = NETTYPE_IPV4;
		const int socket = priv_net_create_socket(AF_INET, SOCK_DGRAM, &bindaddr_ipv4);
		if(socket >= 0)
		{
			sock->type |= NETTYPE_IPV4;
			sock->ipv4sock = socket;

			// Set broadcast
			{
				int broadcast = 1;
				if(setsockopt(socket, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast, sizeof(broadcast)) != 0)
				{
					log_error("net", "Setting SO_BROADCAST on IPv4 failed (%s)", net_error_message().c_str());
				}
			}

			// Set DSCP/TOS
			{
				int iptos = 0x10; // IPTOS_LOWDELAY
				if(setsockopt(socket, IPPROTO_IP, IP_TOS, (const char *)&iptos, sizeof(iptos)) != 0)
				{
					log_error("net", "Setting IP_TOS on IPv4 failed (%s)", net_error_message().c_str());
				}
			}
		}
	}

#if defined(CONF_WEBSOCKETS)
	if(bindaddr.type & NETTYPE_WEBSOCKET_IPV4)
	{
		NETADDR bindaddr_websocket_ipv4 = bindaddr;
		bindaddr_websocket_ipv4.type = NETTYPE_WEBSOCKET_IPV4;
		const int socket = websocket_create(&bindaddr_websocket_ipv4);
		if(socket >= 0)
		{
			sock->type |= NETTYPE_WEBSOCKET_IPV4;
			sock->web_ipv4sock = socket;
		}
	}
#endif

	if(bindaddr.type & NETTYPE_IPV6)
	{
		NETADDR bindaddr_ipv6 = bindaddr;
		bindaddr_ipv6.type = NETTYPE_IPV6;
		const int socket = priv_net_create_socket(AF_INET6, SOCK_DGRAM, &bindaddr_ipv6);
		if(socket >= 0)
		{
			sock->type |= NETTYPE_IPV6;
			sock->ipv6sock = socket;

			// Set broadcast
			{
				int broadcast = 1;
				if(setsockopt(socket, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast, sizeof(broadcast)) != 0)
				{
					log_error("net", "Setting SO_BROADCAST on IPv6 failed (%s)", net_error_message().c_str());
				}
			}

			// Set DSCP/TOS
			// TODO: setting IP_TOS on ipv6 with setsockopt is not supported on Windows, see https://github.com/ddnet/ddnet/issues/7605
#if !defined(CONF_FAMILY_WINDOWS)
			{
				int iptos = 0x10; // IPTOS_LOWDELAY
				if(setsockopt(socket, IPPROTO_IP, IP_TOS, (const char *)&iptos, sizeof(iptos)) != 0)
				{
					log_error("net", "Setting IP_TOS on IPv6 failed (%s)", net_error_message().c_str());
				}
			}
#endif
		}
	}

#if defined(CONF_WEBSOCKETS)
	if(bindaddr.type & NETTYPE_WEBSOCKET_IPV6)
	{
		NETADDR bindaddr_websocket_ipv6 = bindaddr;
		bindaddr_websocket_ipv6.type = NETTYPE_WEBSOCKET_IPV6;
		const int socket = websocket_create(&bindaddr_websocket_ipv6);
		if(socket >= 0)
		{
			sock->type |= NETTYPE_WEBSOCKET_IPV6;
			sock->web_ipv6sock = socket;
		}
	}
#endif

	if(sock->type == NETTYPE_INVALID)
	{
		free(sock);
		sock = nullptr;
	}
	else
	{
		net_set_non_blocking(sock);
		net_buffer_init(&sock->buffer);
	}

	return sock;
}

int net_udp_send(NETSOCKET sock, const NETADDR *addr, const void *data, int size)
{
	if(sock->socks5_proxy_enabled)
	{
		unsigned char *pPacket = sock->socks5_udp_packet_buffer;
		int PacketSize = 0;
		pPacket[PacketSize++] = 0x00; // RSV
		pPacket[PacketSize++] = 0x00; // RSV
		pPacket[PacketSize++] = 0x00; // FRAG (must be 0, no fragmentation)

		NETADDR TargetAddr = *addr;
		if(TargetAddr.type & NETTYPE_WEBSOCKET_IPV4)
			TargetAddr.type = NETTYPE_IPV4;
		else if(TargetAddr.type & NETTYPE_WEBSOCKET_IPV6)
			TargetAddr.type = NETTYPE_IPV6;

		if(TargetAddr.type & NETTYPE_IPV4)
		{
			pPacket[PacketSize++] = 0x01; // ATYP IPv4
			mem_copy(pPacket + PacketSize, TargetAddr.ip, 4);
			PacketSize += 4;
		}
		else if(TargetAddr.type & NETTYPE_IPV6)
		{
			pPacket[PacketSize++] = 0x04; // ATYP IPv6
			mem_copy(pPacket + PacketSize, TargetAddr.ip, 16);
			PacketSize += 16;
		}
		else
		{
			log_error("net", "SOCKS5: unsupported target address type");
			return -1;
		}
		pPacket[PacketSize++] = (unsigned char)((TargetAddr.port >> 8) & 0xff);
		pPacket[PacketSize++] = (unsigned char)(TargetAddr.port & 0xff);

		const int MaxPayload = (int)sizeof(sock->socks5_udp_packet_buffer) - PacketSize;
		if(size > MaxPayload)
		{
			log_error("net", "SOCKS5: payload too large (%d > %d)", size, MaxPayload);
			return -1;
		}
		mem_copy(pPacket + PacketSize, data, size);
		PacketSize += size;

		int Result = -1;
		const NETADDR &RelayAddr = sock->socks5_udp_relay_addr;
		if((RelayAddr.type & NETTYPE_IPV4) != 0 && sock->ipv4sock >= 0)
		{
			sockaddr_in sa;
			netaddr_to_sockaddr_in(&RelayAddr, &sa);
			Result = sendto(sock->ipv4sock, (const char *)pPacket, PacketSize, 0, (sockaddr *)&sa, sizeof(sa));
		}
		else if((RelayAddr.type & NETTYPE_IPV6) != 0 && sock->ipv6sock >= 0)
		{
			sockaddr_in6 sa;
			netaddr_to_sockaddr_in6(&RelayAddr, &sa);
			Result = sendto(sock->ipv6sock, (const char *)pPacket, PacketSize, 0, (sockaddr *)&sa, sizeof(sa));
		}
		else
		{
			log_error("net", "SOCKS5: UDP relay type is not available on this socket");
			return -1;
		}
		if(Result < 0 && !net_would_block() && NetUdpTryRestoreProxy(sock))
		{
			const NETADDR &RetryRelayAddr = sock->socks5_udp_relay_addr;
			if((RetryRelayAddr.type & NETTYPE_IPV4) != 0 && sock->ipv4sock >= 0)
			{
				sockaddr_in sa;
				netaddr_to_sockaddr_in(&RetryRelayAddr, &sa);
				Result = sendto(sock->ipv4sock, (const char *)pPacket, PacketSize, 0, (sockaddr *)&sa, sizeof(sa));
			}
			else if((RetryRelayAddr.type & NETTYPE_IPV6) != 0 && sock->ipv6sock >= 0)
			{
				sockaddr_in6 sa;
				netaddr_to_sockaddr_in6(&RetryRelayAddr, &sa);
				Result = sendto(sock->ipv6sock, (const char *)pPacket, PacketSize, 0, (sockaddr *)&sa, sizeof(sa));
			}
		}

		network_stats.sent_bytes += size;
		network_stats.sent_packets++;
		return Result;
	}

	int d = -1;

	if(addr->type & NETTYPE_IPV4)
	{
		if(sock->ipv4sock >= 0)
		{
			sockaddr_in sa;
			if(addr->type & NETTYPE_LINK_BROADCAST)
			{
				mem_zero(&sa, sizeof(sa));
				sa.sin_port = htons(addr->port);
				sa.sin_family = AF_INET;
				sa.sin_addr.s_addr = INADDR_BROADCAST;
			}
			else
			{
				netaddr_to_sockaddr_in(addr, &sa);
			}

			d = sendto(sock->ipv4sock, (const char *)data, size, 0, (sockaddr *)&sa, sizeof(sa));
		}
		else
		{
			log_error("net", "Cannot send IPv4 traffic to this socket");
		}
	}

#if defined(CONF_WEBSOCKETS)
	if(addr->type & NETTYPE_WEBSOCKET_IPV4)
	{
		if(sock->web_ipv4sock >= 0)
		{
			if(addr->type & NETTYPE_LINK_BROADCAST)
			{
				log_error("net", "Cannot send broadcasts to Websocket IPv4");
			}
			else
			{
				d = websocket_send(sock->web_ipv4sock, (const unsigned char *)data, size, addr);
			}
		}
		else
		{
			log_error("net", "Cannot send Websocket IPv4 traffic to this socket");
		}
	}
#endif

	if(addr->type & NETTYPE_IPV6)
	{
		if(sock->ipv6sock >= 0)
		{
			sockaddr_in6 sa;
			if(addr->type & NETTYPE_LINK_BROADCAST)
			{
				mem_zero(&sa, sizeof(sa));
				sa.sin6_port = htons(addr->port);
				sa.sin6_family = AF_INET6;
				sa.sin6_addr.s6_addr[0] = 0xff; /* multicast */
				sa.sin6_addr.s6_addr[1] = 0x02; /* link local scope */
				sa.sin6_addr.s6_addr[15] = 1; /* all nodes */
			}
			else
			{
				netaddr_to_sockaddr_in6(addr, &sa);
			}

			d = sendto(sock->ipv6sock, (const char *)data, size, 0, (sockaddr *)&sa, sizeof(sa));
		}
		else
		{
			log_error("net", "Cannot send IPv6 traffic to this socket");
		}
	}

#if defined(CONF_WEBSOCKETS)
	if(addr->type & NETTYPE_WEBSOCKET_IPV6)
	{
		if(sock->web_ipv6sock >= 0)
		{
			if(addr->type & NETTYPE_LINK_BROADCAST)
			{
				log_error("net", "Cannot send broadcasts to Websocket IPv6");
			}
			else
			{
				d = websocket_send(sock->web_ipv6sock, (const unsigned char *)data, size, addr);
			}
		}
		else
		{
			log_error("net", "Cannot send Websocket IPv6 traffic to this socket");
		}
	}
#endif

	network_stats.sent_bytes += size;
	network_stats.sent_packets++;
	return d;
}

int net_udp_recv(NETSOCKET sock, NETADDR *addr, unsigned char **data)
{
	if(sock->socks5_proxy_enabled)
	{
		while(true)
		{
			int Bytes = 0;
			sockaddr_storage RecvAddr;
			socklen_t FromLen = sizeof(RecvAddr);
			if(sock->ipv4sock >= 0)
			{
				Bytes = recvfrom(sock->ipv4sock, (char *)sock->socks5_udp_packet_buffer, sizeof(sock->socks5_udp_packet_buffer), 0, (sockaddr *)&RecvAddr, &FromLen);
			}
			else if(sock->ipv6sock >= 0)
			{
				Bytes = recvfrom(sock->ipv6sock, (char *)sock->socks5_udp_packet_buffer, sizeof(sock->socks5_udp_packet_buffer), 0, (sockaddr *)&RecvAddr, &FromLen);
			}
			else
			{
				return -1;
			}

			if(Bytes <= 0)
			{
				if(Bytes < 0 && !net_would_block())
				{
					NetUdpTryRestoreProxy(sock);
				}
				return 0;
			}

			const unsigned char *pRaw = sock->socks5_udp_packet_buffer;
			if(Bytes < 4 || pRaw[0] != 0x00 || pRaw[1] != 0x00 || pRaw[2] != 0x00)
				continue;

			int Offset = 3;
			unsigned char Atyp = pRaw[Offset++];
			NETADDR ParsedAddr = NETADDR_ZEROED;
			if(Atyp == 0x01)
			{
				if(Bytes < Offset + 4 + 2)
					continue;
				ParsedAddr.type = NETTYPE_IPV4;
				mem_copy(ParsedAddr.ip, pRaw + Offset, 4);
				Offset += 4;
			}
			else if(Atyp == 0x04)
			{
				if(Bytes < Offset + 16 + 2)
					continue;
				ParsedAddr.type = NETTYPE_IPV6;
				mem_copy(ParsedAddr.ip, pRaw + Offset, 16);
				Offset += 16;
			}
			else if(Atyp == 0x03)
			{
				if(Bytes < Offset + 1)
					continue;
				const int DomainLen = (int)pRaw[Offset++];
				if(Bytes < Offset + DomainLen + 2)
					continue;
				Offset += DomainLen;
				// Domain ATYP is uncommon for UDP reply; skip address and keep only payload.
				ParsedAddr.type = NETTYPE_INVALID;
			}
			else
				continue;

			ParsedAddr.port = (((unsigned short)pRaw[Offset]) << 8) | (unsigned short)pRaw[Offset + 1];
			Offset += 2;
			const int PayloadSize = Bytes - Offset;
			if(PayloadSize <= 0)
				continue;

			mem_move(sock->socks5_udp_packet_buffer, sock->socks5_udp_packet_buffer + Offset, PayloadSize);
			*addr = ParsedAddr;
			*data = sock->socks5_udp_packet_buffer;
			network_stats.recv_bytes += PayloadSize;
			network_stats.recv_packets++;
			return PayloadSize;
		}
	}

	static const auto &&update_stats = [](int bytes) {
		network_stats.recv_bytes += bytes;
		network_stats.recv_packets++;
	};

	int bytes = 0;
#if defined(CONF_PLATFORM_LINUX)
	if(sock->ipv4sock >= 0)
	{
		if(sock->buffer.pos >= sock->buffer.size)
		{
			net_buffer_reinit(&sock->buffer);
			sock->buffer.size = recvmmsg(sock->ipv4sock, sock->buffer.msgs, VLEN, 0, NULL);
			sock->buffer.pos = 0;
		}
	}

	if(sock->ipv6sock >= 0)
	{
		if(sock->buffer.pos >= sock->buffer.size)
		{
			net_buffer_reinit(&sock->buffer);
			sock->buffer.size = recvmmsg(sock->ipv6sock, sock->buffer.msgs, VLEN, 0, NULL);
			sock->buffer.pos = 0;
		}
	}

	if(sock->buffer.pos < sock->buffer.size)
	{
		sockaddr_to_netaddr((sockaddr *)&(sock->buffer.sockaddrs[sock->buffer.pos]), sizeof(sock->buffer.sockaddrs[sock->buffer.pos]), addr);
		bytes = sock->buffer.msgs[sock->buffer.pos].msg_len;
		*data = (unsigned char *)sock->buffer.bufs[sock->buffer.pos];
		sock->buffer.pos++;
		update_stats(bytes);
		return bytes;
	}
#else
	if(sock->ipv4sock >= 0)
	{
		sockaddr_storage recv_addr;
		socklen_t fromlen = sizeof(recv_addr);
		bytes = recvfrom(sock->ipv4sock, sock->buffer.buf, sizeof(sock->buffer.buf), 0, (sockaddr *)&recv_addr, &fromlen);
		*data = (unsigned char *)sock->buffer.buf;
		if(bytes > 0)
		{
			sockaddr_to_netaddr((sockaddr *)&recv_addr, fromlen, addr);
			update_stats(bytes);
			return bytes;
		}
	}

	if(sock->ipv6sock >= 0)
	{
		sockaddr_storage recv_addr;
		socklen_t fromlen = sizeof(recv_addr);
		bytes = recvfrom(sock->ipv6sock, sock->buffer.buf, sizeof(sock->buffer.buf), 0, (sockaddr *)&recv_addr, &fromlen);
		*data = (unsigned char *)sock->buffer.buf;
		if(bytes > 0)
		{
			sockaddr_to_netaddr((sockaddr *)&recv_addr, fromlen, addr);
			update_stats(bytes);
			return bytes;
		}
	}
#endif

#if defined(CONF_WEBSOCKETS)
	if(sock->web_ipv4sock >= 0)
	{
		char *buf;
		int size;
		net_buffer_simple(&sock->buffer, &buf, &size);
		bytes = websocket_recv(sock->web_ipv4sock, (unsigned char *)buf, size, addr);
		*data = (unsigned char *)buf;
		if(bytes > 0)
		{
			update_stats(bytes);
			return bytes;
		}
	}

	if(sock->web_ipv6sock >= 0)
	{
		char *buf;
		int size;
		net_buffer_simple(&sock->buffer, &buf, &size);
		bytes = websocket_recv(sock->web_ipv6sock, (unsigned char *)buf, size, addr);
		*data = (unsigned char *)buf;
		if(bytes > 0)
		{
			update_stats(bytes);
			return bytes;
		}
	}
#endif

	return bytes < 0 ? -1 : 0;
}

int net_udp_set_proxy(NETSOCKET sock, const NETADDR *proxy_addr, const char *proxy_username, const char *proxy_password)
{
	if(!sock)
		return -1;

	NetUdpDisableProxy(sock, proxy_addr == nullptr);
	if(proxy_addr == nullptr)
		return 0;

	if((proxy_addr->type & (NETTYPE_IPV4 | NETTYPE_IPV6)) == 0)
	{
		log_error("net", "SOCKS5 proxy requires IPv4 or IPv6 address");
		return -1;
	}

	NETADDR BindAddr = NETADDR_ZEROED;
	BindAddr.type = (proxy_addr->type & NETTYPE_IPV6) ? NETTYPE_IPV6 : NETTYPE_IPV4;
	NETSOCKET pTcpSock = net_tcp_create(BindAddr);
	if(!pTcpSock)
	{
		log_error("net", "SOCKS5: failed to create TCP control socket");
		return -1;
	}

	if(net_tcp_connect(pTcpSock, proxy_addr) != 0)
	{
		log_error("net", "SOCKS5: connect failed (%s)", net_error_message().c_str());
		net_tcp_close(pTcpSock);
		return -1;
	}

	const bool HasAuth = proxy_username && proxy_username[0] != '\0';
	const unsigned char aHelloNoAuth[] = {0x05, 0x01, 0x00};
	const unsigned char aHelloWithAuth[] = {0x05, 0x02, 0x00, 0x02};
	unsigned char aHelloReply[2];
	const unsigned char *pHello = HasAuth ? aHelloWithAuth : aHelloNoAuth;
	const int HelloSize = HasAuth ? (int)sizeof(aHelloWithAuth) : (int)sizeof(aHelloNoAuth);
	if(!NetTcpSendAll(pTcpSock, pHello, HelloSize) || !NetTcpRecvAll(pTcpSock, aHelloReply, sizeof(aHelloReply)))
	{
		log_error("net", "SOCKS5: hello handshake failed");
		net_tcp_close(pTcpSock);
		return -1;
	}
	if(aHelloReply[0] != 0x05)
	{
		log_error("net", "SOCKS5: invalid hello response");
		net_tcp_close(pTcpSock);
		return -1;
	}
	if(aHelloReply[1] == 0x02)
	{
		const int UserLen = str_length(proxy_username);
		const int PassLen = proxy_password ? str_length(proxy_password) : 0;
		if(UserLen <= 0 || UserLen > 255 || PassLen > 255)
		{
			log_error("net", "SOCKS5: invalid username/password length");
			net_tcp_close(pTcpSock);
			return -1;
		}
		unsigned char aAuthReq[3 + 255 + 255];
		int AuthReqSize = 0;
		aAuthReq[AuthReqSize++] = 0x01; // auth version
		aAuthReq[AuthReqSize++] = (unsigned char)UserLen;
		mem_copy(aAuthReq + AuthReqSize, proxy_username, UserLen);
		AuthReqSize += UserLen;
		aAuthReq[AuthReqSize++] = (unsigned char)PassLen;
		if(PassLen > 0)
		{
			mem_copy(aAuthReq + AuthReqSize, proxy_password, PassLen);
			AuthReqSize += PassLen;
		}
		unsigned char aAuthReply[2];
		if(!NetTcpSendAll(pTcpSock, aAuthReq, AuthReqSize) || !NetTcpRecvAll(pTcpSock, aAuthReply, sizeof(aAuthReply)))
		{
			log_error("net", "SOCKS5: auth exchange failed");
			net_tcp_close(pTcpSock);
			return -1;
		}
		if(aAuthReply[0] != 0x01 || aAuthReply[1] != 0x00)
		{
			log_error("net", "SOCKS5: auth rejected");
			net_tcp_close(pTcpSock);
			return -1;
		}
	}
	else if(aHelloReply[1] != 0x00)
	{
		log_error("net", "SOCKS5: unsupported auth method");
		net_tcp_close(pTcpSock);
		return -1;
	}

	unsigned char aAssocReq[4 + 16 + 2];
	int AssocReqSize = 0;
	aAssocReq[AssocReqSize++] = 0x05; // VER
	aAssocReq[AssocReqSize++] = 0x03; // CMD UDP ASSOCIATE
	aAssocReq[AssocReqSize++] = 0x00; // RSV
	if(proxy_addr->type & NETTYPE_IPV6)
	{
		aAssocReq[AssocReqSize++] = 0x04; // ATYP IPv6
		mem_zero(aAssocReq + AssocReqSize, 16);
		AssocReqSize += 16;
	}
	else
	{
		aAssocReq[AssocReqSize++] = 0x01; // ATYP IPv4
		mem_zero(aAssocReq + AssocReqSize, 4);
		AssocReqSize += 4;
	}
	aAssocReq[AssocReqSize++] = 0x00; // PORT hi
	aAssocReq[AssocReqSize++] = 0x00; // PORT lo

	if(!NetTcpSendAll(pTcpSock, aAssocReq, AssocReqSize))
	{
		log_error("net", "SOCKS5: UDP ASSOCIATE send failed");
		net_tcp_close(pTcpSock);
		return -1;
	}

	unsigned char aAssocReplyHead[4];
	if(!NetTcpRecvAll(pTcpSock, aAssocReplyHead, sizeof(aAssocReplyHead)))
	{
		log_error("net", "SOCKS5: UDP ASSOCIATE reply failed");
		net_tcp_close(pTcpSock);
		return -1;
	}
	if(aAssocReplyHead[0] != 0x05 || aAssocReplyHead[1] != 0x00)
	{
		log_error("net", "SOCKS5: UDP ASSOCIATE rejected (code=%d)", (int)aAssocReplyHead[1]);
		net_tcp_close(pTcpSock);
		return -1;
	}

	NETADDR UdpRelay = NETADDR_ZEROED;
	const unsigned char Atyp = aAssocReplyHead[3];
	if(Atyp == 0x01)
	{
		unsigned char aTmp[6];
		if(!NetTcpRecvAll(pTcpSock, aTmp, sizeof(aTmp)))
		{
			log_error("net", "SOCKS5: failed to read IPv4 UDP relay");
			net_tcp_close(pTcpSock);
			return -1;
		}
		UdpRelay.type = NETTYPE_IPV4;
		mem_copy(UdpRelay.ip, aTmp, 4);
		UdpRelay.port = (((unsigned short)aTmp[4]) << 8) | (unsigned short)aTmp[5];
	}
	else if(Atyp == 0x04)
	{
		unsigned char aTmp[18];
		if(!NetTcpRecvAll(pTcpSock, aTmp, sizeof(aTmp)))
		{
			log_error("net", "SOCKS5: failed to read IPv6 UDP relay");
			net_tcp_close(pTcpSock);
			return -1;
		}
		UdpRelay.type = NETTYPE_IPV6;
		mem_copy(UdpRelay.ip, aTmp, 16);
		UdpRelay.port = (((unsigned short)aTmp[16]) << 8) | (unsigned short)aTmp[17];
	}
	else if(Atyp == 0x03)
	{
		unsigned char DomainLen = 0;
		if(!NetTcpRecvAll(pTcpSock, &DomainLen, 1))
		{
			log_error("net", "SOCKS5: failed to read domain length");
			net_tcp_close(pTcpSock);
			return -1;
		}
		unsigned char aDomainBuf[256 + 2];
		if(DomainLen > 0 && !NetTcpRecvAll(pTcpSock, aDomainBuf, DomainLen + 2))
		{
			log_error("net", "SOCKS5: failed to read domain reply body");
			net_tcp_close(pTcpSock);
			return -1;
		}
		// For domain replies, use proxy address with returned port.
		UdpRelay = *proxy_addr;
		UdpRelay.type = (proxy_addr->type & NETTYPE_IPV6) ? NETTYPE_IPV6 : NETTYPE_IPV4;
		UdpRelay.port = (((unsigned short)aDomainBuf[DomainLen]) << 8) | (unsigned short)aDomainBuf[DomainLen + 1];
	}
	else
	{
		log_error("net", "SOCKS5: unsupported UDP relay ATYP=%d", (int)Atyp);
		net_tcp_close(pTcpSock);
		return -1;
	}

	if(UdpRelay.port == 0)
	{
		log_error("net", "SOCKS5: invalid UDP relay port");
		net_tcp_close(pTcpSock);
		return -1;
	}
	if(IsZeroAddress(&UdpRelay))
	{
		UdpRelay = *proxy_addr;
		UdpRelay.type = (proxy_addr->type & NETTYPE_IPV6) ? NETTYPE_IPV6 : NETTYPE_IPV4;
	}
	else if(IsLoopbackAddress(&UdpRelay) && !IsLoopbackAddress(proxy_addr))
	{
		// Some remote SOCKS5 servers incorrectly return a loopback relay.
		// Keep the announced UDP relay port, but route to the configured proxy host.
		const unsigned short RelayPort = UdpRelay.port;
		UdpRelay = *proxy_addr;
		UdpRelay.type = (proxy_addr->type & NETTYPE_IPV6) ? NETTYPE_IPV6 : NETTYPE_IPV4;
		UdpRelay.port = RelayPort;
	}

	sock->socks5_tcp_socket = pTcpSock;
	sock->socks5_proxy_enabled = true;
	sock->socks5_proxy_addr = *proxy_addr;
	sock->socks5_udp_relay_addr = UdpRelay;
	str_copy(sock->socks5_proxy_username, proxy_username ? proxy_username : "", sizeof(sock->socks5_proxy_username));
	str_copy(sock->socks5_proxy_password, proxy_password ? proxy_password : "", sizeof(sock->socks5_proxy_password));

	char aProxyAddr[NETADDR_MAXSTRSIZE];
	char aRelayAddr[NETADDR_MAXSTRSIZE];
	net_addr_str(proxy_addr, aProxyAddr, sizeof(aProxyAddr), true);
	net_addr_str(&UdpRelay, aRelayAddr, sizeof(aRelayAddr), true);
	log_debug("net", "SOCKS5 proxy enabled: control=%s udp_relay=%s", aProxyAddr, aRelayAddr);
	return 0;
}

static bool NetUdpTryRestoreProxy(NETSOCKET pSock)
{
	if(pSock->socks5_proxy_addr.type == NETTYPE_INVALID)
		return false;

	const int64_t Now = time_get();
	if(pSock->socks5_next_reconnect_after != 0 && Now < pSock->socks5_next_reconnect_after)
		return false;
	if(pSock->socks5_last_reconnect_attempt != 0 && Now - pSock->socks5_last_reconnect_attempt < time_freq() / 2)
		return false;
	pSock->socks5_last_reconnect_attempt = Now;

	const int Res = net_udp_set_proxy(pSock, &pSock->socks5_proxy_addr, pSock->socks5_proxy_username, pSock->socks5_proxy_password);
	if(Res == 0)
	{
		pSock->socks5_reconnect_fail_count = 0;
		pSock->socks5_next_reconnect_after = 0;
		log_info("net", "SOCKS5 proxy auto-restored");
	}
	else
	{
		pSock->socks5_reconnect_fail_count = std::min(pSock->socks5_reconnect_fail_count + 1, 8);
		const int BackoffSeconds = std::min(1 << std::min(pSock->socks5_reconnect_fail_count, 4), 12);
		pSock->socks5_next_reconnect_after = Now + (int64_t)BackoffSeconds * time_freq();
	}
	return Res == 0;
}

void net_udp_close(NETSOCKET sock)
{
	NetUdpDisableProxy(sock, true);
	priv_net_close_all_sockets(sock);
}

NETSOCKET net_tcp_create(NETADDR bindaddr)
{
	NETSOCKET sock = (NETSOCKET_INTERNAL *)malloc(sizeof(*sock));
	*sock = invalid_socket;

	if(bindaddr.type & NETTYPE_IPV4)
	{
		NETADDR bindaddr_ipv4 = bindaddr;
		bindaddr_ipv4.type = NETTYPE_IPV4;
		const int socket4 = priv_net_create_socket(AF_INET, SOCK_STREAM, &bindaddr_ipv4);
		if(socket4 >= 0)
		{
			sock->type |= NETTYPE_IPV4;
			sock->ipv4sock = socket4;
		}
	}

	if(bindaddr.type & NETTYPE_IPV6)
	{
		NETADDR bindaddr_ipv6 = bindaddr;
		bindaddr_ipv6.type = NETTYPE_IPV6;
		const int socket6 = priv_net_create_socket(AF_INET6, SOCK_STREAM, &bindaddr_ipv6);
		if(socket6 >= 0)
		{
			sock->type |= NETTYPE_IPV6;
			sock->ipv6sock = socket6;
		}
	}

	if(sock->type == NETTYPE_INVALID)
	{
		free(sock);
		sock = nullptr;
	}

	return sock;
}

int net_tcp_listen(NETSOCKET sock, int backlog)
{
	int err = -1;
	if(sock->ipv4sock >= 0)
	{
		err = listen(sock->ipv4sock, backlog);
	}
	if(sock->ipv6sock >= 0)
	{
		err = listen(sock->ipv6sock, backlog);
	}
	return err;
}

int net_tcp_accept(NETSOCKET sock, NETSOCKET *new_sock, NETADDR *a)
{
	*new_sock = nullptr;

	if(sock->ipv4sock >= 0)
	{
		sockaddr_storage addr;
		socklen_t sockaddr_len = sizeof(addr);

		int s = accept(sock->ipv4sock, (sockaddr *)&addr, &sockaddr_len);
		if(s != -1)
		{
			sockaddr_to_netaddr((sockaddr *)&addr, sockaddr_len, a);

			*new_sock = (NETSOCKET_INTERNAL *)malloc(sizeof(**new_sock));
			**new_sock = invalid_socket;
			(*new_sock)->type = NETTYPE_IPV4;
			(*new_sock)->ipv4sock = s;
			return s;
		}
	}

	if(sock->ipv6sock >= 0)
	{
		sockaddr_storage addr;
		socklen_t sockaddr_len = sizeof(addr);

		int s = accept(sock->ipv6sock, (sockaddr *)&addr, &sockaddr_len);
		if(s != -1)
		{
			*new_sock = (NETSOCKET_INTERNAL *)malloc(sizeof(**new_sock));
			**new_sock = invalid_socket;
			sockaddr_to_netaddr((sockaddr *)&addr, sockaddr_len, a);
			(*new_sock)->type = NETTYPE_IPV6;
			(*new_sock)->ipv6sock = s;
			return s;
		}
	}

	return -1;
}

int net_tcp_connect(NETSOCKET sock, const NETADDR *a)
{
	if(a->type & NETTYPE_IPV4)
	{
		if(sock->ipv4sock < 0)
			return -2;
		sockaddr_in addr;
		netaddr_to_sockaddr_in(a, &addr);
		return connect(sock->ipv4sock, (sockaddr *)&addr, sizeof(addr));
	}

	if(a->type & NETTYPE_IPV6)
	{
		if(sock->ipv6sock < 0)
			return -2;
		sockaddr_in6 addr;
		netaddr_to_sockaddr_in6(a, &addr);
		return connect(sock->ipv6sock, (sockaddr *)&addr, sizeof(addr));
	}

	return -1;
}

int net_tcp_connect_non_blocking(NETSOCKET sock, NETADDR bindaddr)
{
	net_set_non_blocking(sock);
	int res = net_tcp_connect(sock, &bindaddr);
	net_set_blocking(sock);
	return res;
}

int net_tcp_send(NETSOCKET sock, const void *data, int size)
{
	int bytes = -1;

	if(sock->ipv4sock >= 0)
	{
		bytes = send(sock->ipv4sock, (const char *)data, size, 0);
	}
	if(sock->ipv6sock >= 0)
	{
		bytes = send(sock->ipv6sock, (const char *)data, size, 0);
	}

	return bytes;
}

int net_tcp_recv(NETSOCKET sock, void *data, int maxsize)
{
	int bytes = -1;

	if(sock->ipv4sock >= 0)
	{
		bytes = recv(sock->ipv4sock, (char *)data, maxsize, 0);
	}
	if(sock->ipv6sock >= 0)
	{
		bytes = recv(sock->ipv6sock, (char *)data, maxsize, 0);
	}

	return bytes;
}

void net_tcp_close(NETSOCKET sock)
{
	priv_net_close_all_sockets(sock);
}

#if defined(CONF_FAMILY_UNIX)
UNIXSOCKET net_unix_create_unnamed()
{
	return socket(AF_UNIX, SOCK_DGRAM, 0);
}

int net_unix_send(UNIXSOCKET sock, UNIXSOCKETADDR *addr, void *data, int size)
{
	return sendto(sock, data, size, 0, (sockaddr *)addr, sizeof(*addr));
}

void net_unix_set_addr(UNIXSOCKETADDR *addr, const char *path)
{
	mem_zero(addr, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	str_copy(addr->sun_path, path);
}

void net_unix_close(UNIXSOCKET sock)
{
	close(sock);
}
#endif
