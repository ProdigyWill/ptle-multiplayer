// Transport through the relay server (server/relay.py), over UDP.

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <mstcpip.h>
#include <Windows.h>

#include <stdio.h>
#include <string.h>

#include "net/protocol.h"
#include "net/transport.h"

#pragma comment( lib, "ws2_32.lib" )

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW( IOC_VENDOR, 12 )
#endif


static const uint32_t JOIN_RETRY_MS = 500;
static const uint32_t PING_EVERY_MS = 1000;
static const uint32_t SERVER_SILENT_MS = 5000;   // Then : join again.
static const int RECEIVE_BUFFER = 1 << 20;       // 7 players x 60 snapshots/s x ~1 KB, with room.

class UdpTransport : public Transport
{
public:
	UdpTransport( const char* server, const char* room, const char* name, bool echo )
		: m_socket( INVALID_SOCKET ), m_resolved( 0 ), m_state( WAITING_FOR_ADDRESS ), m_id( -1 ), m_echo( echo ),
		  m_nonce( 0 ), m_lastJoin( 0 ), m_lastPing( 0 ), m_lastHeard( 0 ), m_rtt( -1 )
	{
		strncpy_s( m_serverText, server, _TRUNCATE );
		strncpy_s( m_room, room, _TRUNCATE );
		strncpy_s( m_name, name, _TRUNCATE );
		ZeroMemory( &m_server, sizeof(m_server) );

		WSADATA wsa;
		if ( WSAStartup( MAKEWORD( 2, 2 ), &wsa ) != 0 ) { g_netLog( "[NET] WSAStartup failed.\n" ); return; }
		m_socket = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
		if ( m_socket == INVALID_SOCKET ) { g_netLog( "[NET] Can't create a UDP socket (%d).\n", WSAGetLastError() ); return; }
		u_long nonBlocking = 1;
		ioctlsocket( m_socket, FIONBIO, &nonBlocking );
		int size = RECEIVE_BUFFER;
		setsockopt( m_socket, SOL_SOCKET, SO_RCVBUF, (const char*) &size, sizeof(size) );
		// Without this, an ICMP "port unreachable" (server down) makes the next
		// recvfrom fail with WSAECONNRESET.
		BOOL off = FALSE;
		DWORD bytes = 0;
		WSAIoctl( m_socket, SIO_UDP_CONNRESET, &off, sizeof(off), 0, 0, &bytes, 0, 0 );

		m_nonce = GetTickCount() ^ (GetCurrentProcessId() << 16);
		// Looking up the server's address can take a while : not on the game thread.
		CreateThread( 0, 0, ResolveThread, this, 0, 0 );
	}

	~UdpTransport()
	{
		if ( m_socket != INVALID_SOCKET ) {
			if ( m_state == JOINED ) SendControl( PACKET_LEAVE, 0, 0, 0 );
			closesocket( m_socket );
		}
	}

	void Update() override
	{
		if ( m_socket == INVALID_SOCKET || m_state == WAITING_FOR_ADDRESS || m_state == REJECTED ) {
			if ( m_state == WAITING_FOR_ADDRESS && m_resolved ) m_state = JOINING;
			else return;
		}
		uint32_t now = NetNowMs();
		if ( m_state == JOINED && now - m_lastHeard > SERVER_SILENT_MS ) {
			g_netLog( "[NET] No news from the server for %u s : joining again.\n", SERVER_SILENT_MS / 1000 );
			m_state = JOINING;
		}
		if ( m_state == JOINING && now - m_lastJoin >= JOIN_RETRY_MS ) {
			m_lastJoin = now;
			uint8_t payload[2 + 64 + 1 + 64];
			int n = (int) strlen( m_name ), r = (int) strlen( m_room );
			int len = 0;
			payload[len++] = m_echo ? JOIN_ECHO : 0;
			payload[len++] = (uint8_t) n;
			memcpy( payload + len, m_name, n ); len += n;
			payload[len++] = (uint8_t) r;
			memcpy( payload + len, m_room, r ); len += r;
			SendControl( PACKET_JOIN, m_nonce, payload, len );
		}
		if ( m_state == JOINED && now - m_lastPing >= PING_EVERY_MS ) {
			m_lastPing = now;
			SendControl( PACKET_PING, 0, (const uint8_t*) &now, sizeof(now) );
		}
	}

	void Send( const uint8_t* data, int length ) override
	{
		if ( m_state != JOINED || length < PACKET_HEADER_SIZE || length > MAX_PACKET ) return;
		uint8_t packet[MAX_PACKET];
		memcpy( packet, data, length );
		packet[4] = (uint8_t) m_id;   // (The server stamps it too.)
		sendto( m_socket, (const char*) packet, length, 0, (const sockaddr*) &m_server, sizeof(m_server) );
	}

	int Receive( uint8_t* buffer, int capacity ) override
	{
		if ( m_socket == INVALID_SOCKET || !m_resolved ) return 0;
		for ( ;; ) {
			sockaddr_in from;
			int fromLen = sizeof(from);
			int length = recvfrom( m_socket, (char*) buffer, capacity, 0, (sockaddr*) &from, &fromLen );
			if ( length <= 0 ) return 0;   // Nothing more (WSAEWOULDBLOCK), or an error.
			if ( from.sin_addr.s_addr != m_server.sin_addr.s_addr || from.sin_port != m_server.sin_port ) continue;
			PacketHeader h;
			if ( !DecodeHeader( buffer, length, &h ) ) continue;
			uint32_t now = NetNowMs();
			m_lastHeard = now;

			switch ( h.type ) {
			case PACKET_WELCOME:
				if ( h.sequence != m_nonce ) continue;
				if ( m_state != JOINED || m_id != h.player ) {
					g_netLog( "[NET] Joined room \"%s\" on %s as player %d%s.\n", m_room, m_serverText, h.player, m_echo ? " (echo)" : "" );
				}
				m_state = JOINED;
				m_id = h.player;
				continue;
			case PACKET_REJECT:
				g_netLog( "[NET] The server turned us down (%s).\n",
					length > PACKET_HEADER_SIZE && buffer[PACKET_HEADER_SIZE] == REJECT_ROOM_FULL ? "room full" : "unknown reason" );
				m_state = REJECTED;
				continue;
			case PACKET_PONG:
				if ( length >= PACKET_HEADER_SIZE + 4 ) {
					uint32_t sent;
					memcpy( &sent, buffer + PACKET_HEADER_SIZE, 4 );
					m_rtt = (int) (now - sent);
				}
				continue;
			}
			return length;   // Snapshots, LEAVE notices...
		}
	}

	int PlayerId() const override { return m_state == JOINED ? m_id : -1; }
	int RoundTripMs() const override { return m_rtt; }
	bool Echoes() const override { return m_echo; }

private:
	enum State { WAITING_FOR_ADDRESS, JOINING, JOINED, REJECTED };

	void SendControl( uint8_t type, uint32_t sequence, const uint8_t* payload, int length )
	{
		uint8_t packet[PACKET_HEADER_SIZE + 256];
		EncodeHeader( type, 0, sequence, packet );
		if ( length > 256 ) length = 256;
		if ( length > 0 ) memcpy( packet + PACKET_HEADER_SIZE, payload, length );
		sendto( m_socket, (const char*) packet, PACKET_HEADER_SIZE + length, 0, (const sockaddr*) &m_server, sizeof(m_server) );
	}

	static DWORD WINAPI ResolveThread( LPVOID param )
	{
		UdpTransport* t = (UdpTransport*) param;
		char host[128];
		strncpy_s( host, t->m_serverText, _TRUNCATE );
		const char* port = "27777";
		char* colon = strrchr( host, ':' );
		if ( colon ) { *colon = 0; port = colon + 1; }

		addrinfo hints;
		ZeroMemory( &hints, sizeof(hints) );
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		for ( int attempt = 0; ; attempt++ ) {
			addrinfo* result = 0;
			if ( getaddrinfo( host, port, &hints, &result ) == 0 && result ) {
				memcpy( &t->m_server, result->ai_addr, sizeof(t->m_server) );
				freeaddrinfo( result );
				// A server on this computer (tests) : stay on the loopback interface,
				// which also keeps Windows' firewall from asking about the game.
				if ( (ntohl( t->m_server.sin_addr.s_addr ) >> 24) == 127 ) {
					sockaddr_in local;
					ZeroMemory( &local, sizeof(local) );
					local.sin_family = AF_INET;
					local.sin_addr.s_addr = htonl( INADDR_LOOPBACK );
					bind( t->m_socket, (const sockaddr*) &local, sizeof(local) );
				}
				char ip[32];
				inet_ntop( AF_INET, &t->m_server.sin_addr, ip, sizeof(ip) );
				g_netLog( "[NET] Server %s is %s:%d.\n", t->m_serverText, ip, ntohs( t->m_server.sin_port ) );
				InterlockedExchange( &t->m_resolved, 1 );
				return 0;
			}
			if ( attempt == 0 ) g_netLog( "[NET] Can't find server %s (%d), retrying.\n", t->m_serverText, WSAGetLastError() );
			Sleep( 5000 );
		}
	}

	SOCKET m_socket;
	sockaddr_in m_server;
	volatile LONG m_resolved;
	State m_state;
	int m_id;
	bool m_echo;
	uint32_t m_nonce;
	uint32_t m_lastJoin, m_lastPing, m_lastHeard;
	int m_rtt;
	char m_serverText[128];
	char m_room[64];
	char m_name[64];
};


Transport* CreateUdpTransport( const char* server, const char* room, const char* name, bool echo )
{
	return new UdpTransport( server, room, name, echo );
}
