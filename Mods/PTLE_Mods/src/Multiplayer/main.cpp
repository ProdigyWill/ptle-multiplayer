// --------------------------------------------------------------------------------
// Multiplayer (milestone 1 : puppet test).
//
// Spawns a second Harry-looking character (the game's Evil Harry beast) and drives
// its transform from code every frame. Every tick the local player's state goes out
// as a snapshot packet (net/protocol.h) through a transport (net/transport.h), and
// the puppet replays the snapshots that come in (net/remote.h).
//
// Settings : mods\Multiplayer.ini (written with defaults the first time), section
// [Network] : Server (relay server "host:port", see server/relay.py), Room, Name,
// Echo (1 : the server sends our own moves back, to test alone). Environment
// variables PTLE_MP_SERVER, PTLE_MP_ROOM, PTLE_MP_NAME, PTLE_MP_ECHO override them.
// With no server, a loopback transport echoes our own packets back, so the puppet
// replays the local player through the network code; PTLE_MP_NETSIM="latency,
// jitter,loss" (ms, ms, fraction) simulates a bad connection on it.
//
// For now one other player is shown (the first one heard from).
//
// Hotkeys (only while the game window has focus) :
//   F5 : spawn the puppet in the current level.
//   F6 : cycle the puppet's animation : automatic (idle when still, run when
//        moving, facing its direction of travel), then idle/rest states by hand.
//   F7 : cycle the playback delay (0.13s .. 3s).
//   F8 : Evil Harry stays where he is (spawned where Harry stands if needed) and
//        copies Harry in place : animations, items, facing, jumps and climbs (up and
//        down only). F8 again : he follows.
// --------------------------------------------------------------------------------

#include <Windows.h>
#include <d3d9.h>
#include <math.h>
#include <ShlObj.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "gizmod/Gizmod.h"
#include "gizmod/GizmodPlugin.h"
#include "gizmod/event/LoadLevelEvent.h"
#include "gizmod/event/LevelLoadedEvent.h"

#include "injector/injector.hpp"

#include "ptle/EIHarry.h"
#include "ptle/EIEvilHarryP.h"
#include "ptle/EBeastStateMachine.h"
#include "ptle/ERLevel.h"
#include "ptle/EIStaticModel.h"
#include "ptle/math/Matrix4f.h"
#include "ptle/math/Vector3f.h"
#include "ptle/types/types.h"

#include "utils/func.h"

#include "net/protocol.h"
#include "net/remote.h"
#include "net/snapshot.h"
#include "net/transport.h"
#include "overlay.h"
#include "ptle/levels/level_info.h"


#define FIELD( obj, type, offset ) (*(type*)((uint8_t*)(obj) + (offset)))

GET_METHOD( 0x626670, void, AddToWorld, ERLevel*, EInstance* );
GET_METHOD( 0x6268D0, void, ERLevel_SetupEntity, ERLevel*, EInstance*, EInstance* );
GET_METHOD( 0x412850, void, EIBeast_LoadBeastAsset, EIBeast* );
GET_FUNC(   0x6F8EC0, uint32_t, HashPath, char* );


// Virtual tables (see "Memory Research/PC/Virtual tables.txt").
static const uint32_t VTABLE_HARRY       = 0x87D300;
static const uint32_t VTABLE_EVILHARRY_P = 0x875240;
static const uint32_t VTABLE_SLOT_TICK   = 0x2C;     // EInstance::Tick().

static const uint32_t HWND_GAME_WINDOW   = 0x93ED60;

// Offset the EIBeast destructor frees (0x412A20 : "add ecx, 30Ch"). We expect it to be
// m_beastTypeName, which must therefore not point to static memory when the level unloads.
static const size_t EXPECTED_BEASTNAME_OFFSET = 0x30C;


typedef void (__thiscall *TickFunc)( EInstance* );
static TickFunc g_origHarryTick = 0;
static TickFunc g_origEvilHarryTick = 0;

static Logger* logger() { return Gizmod::getInstance()->getLogger(); }



// --------------------------------------------------------------------------------
// Logging : Gizmod console + Documents\PTLE Multiplayer\logs\multiplayer.log
// (the game folder isn't writable without admin rights).
// --------------------------------------------------------------------------------
static FILE* g_logFile = 0;
static CRITICAL_SECTION g_logLock;
static char g_logDir[MAX_PATH] = { 0, };

static void OpenLogFile()
{
	InitializeCriticalSection( &g_logLock );

	char path[MAX_PATH];
	if ( FAILED( SHGetFolderPathA( 0, CSIDL_PERSONAL, 0, 0, path ) ) ) return;

	strcat_s( path, "\\PTLE Multiplayer" );
	CreateDirectoryA( path, 0 );
	strcat_s( path, "\\logs" );
	CreateDirectoryA( path, 0 );
	// PTLE_MP_INSTANCE=<name> : logs\<name>\ (several games running at once).
	char instance[32];
	DWORD n = GetEnvironmentVariableA( "PTLE_MP_INSTANCE", instance, sizeof(instance) );
	if ( n > 0 && n < sizeof(instance) ) {
		strcat_s( path, "\\" );
		strcat_s( path, instance );
		CreateDirectoryA( path, 0 );
	}
	strcpy_s( g_logDir, path );
	strcat_s( path, "\\multiplayer.log" );

	g_logFile = _fsopen( path, "w", _SH_DENYWR );
}

static void mp_log( const char* fmt, ... )
{
	char buf[1024];
	va_list args;
	va_start( args, fmt );
	vsnprintf_s( buf, _TRUNCATE, fmt, args );
	va_end( args );

	EnterCriticalSection( &g_logLock );
	logger()->log( buf );
	if ( g_logFile ) {
		SYSTEMTIME t;
		GetLocalTime( &t );
		fprintf( g_logFile, "%02d:%02d:%02d.%03d %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf );
		fflush( g_logFile );
	}
	LeaveCriticalSection( &g_logLock );
}

// Records access violations (address + registers) before Gizmod's crash handler
// runs. First-chance only, and capped, so handled exceptions don't flood the log.
static LONG CALLBACK CrashLogger( EXCEPTION_POINTERS* e )
{
	static volatile LONG count = 0;
	DWORD code = e->ExceptionRecord->ExceptionCode;
	if ( code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION ) return EXCEPTION_CONTINUE_SEARCH;
	if ( InterlockedIncrement( &count ) > 5 ) return EXCEPTION_CONTINUE_SEARCH;

	CONTEXT* c = e->ContextRecord;
	mp_log( "[MP] EXCEPTION 0x%.8X at EIP 0x%.8X (%s 0x%.8X) EAX=%.8X EBX=%.8X ECX=%.8X EDX=%.8X ESI=%.8X EDI=%.8X ESP=%.8X\n",
		code, c->Eip,
		e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
		(uint32_t) e->ExceptionRecord->ExceptionInformation[1],
		c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp );

	// A few return addresses from the stack help locate the caller.
	uint32_t* sp = (uint32_t*) c->Esp;
	for ( int i = 0; i < 24; i++ ) {
		uint32_t v = 0;
		if ( !IsBadReadPtr( sp + i, 4 ) ) v = sp[i];
		if ( v >= 0x401000 && v < 0x869000 ) mp_log( "[MP]   stack+%02X : 0x%.8X\n", i * 4, v );
	}
	return EXCEPTION_CONTINUE_SEARCH;
}



// --------------------------------------------------------------------------------
// Snapshots of the local player, and the remote player's.
// --------------------------------------------------------------------------------
// Row-vector matrices (translation in row 3) : world = local * parent.
static void MatMul( const Matrix4f& a, const Matrix4f& b, Matrix4f* out )
{
	Matrix4f r;
	for ( int i = 0; i < 4; i++ )
		for ( int j = 0; j < 4; j++ )
			r.data[i][j] = a.data[i][0] * b.data[0][j] + a.data[i][1] * b.data[1][j]
			             + a.data[i][2] * b.data[2][j] + a.data[i][3] * b.data[3][j];
	*out = r;
}

// Inverse of an affine matrix (3x3 part may be scaled; last column 0,0,0,1).
static bool MatInverseAffine( const Matrix4f& m, Matrix4f* out )
{
	const float (*a)[4] = m.data;
	float c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
	float c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
	float c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
	float det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
	if ( fabsf( det ) < 1e-8F ) return false;
	float k = 1.0F / det;
	Matrix4f r;
	r.data[0][0] = c00 * k;
	r.data[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) * k;
	r.data[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) * k;
	r.data[1][0] = c01 * k;
	r.data[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) * k;
	r.data[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) * k;
	r.data[2][0] = c02 * k;
	r.data[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) * k;
	r.data[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) * k;
	for ( int j = 0; j < 3; j++ ) {
		r.data[3][j] = -(a[3][0] * r.data[0][j] + a[3][1] * r.data[1][j] + a[3][2] * r.data[2][j]);
		r.data[j][3] = 0.0F;
	}
	r.data[3][3] = 1.0F;
	*out = r;
	return true;
}

// out[i] = m[i] * b (b : a parent to place on, or an inverse to make relative).
static void MatsMul( const Matrix4f* m, int count, const Matrix4f& b, Matrix4f* out )
{
	for ( int i = 0; i < count; i++ ) MatMul( m[i], b, &out[i] );
}

static bool g_autotest = false;                // PTLE_MP_AUTOTEST=1 (see the autotest section).

static PlayerSnapshot g_local;                 // Static storage keeps Matrix4f 16-byte aligned.
static bool g_haveLocal = false;

// One per player id the relay hands out (its rooms hold 8). Each other player in
// the room gets their own puppet; with echo (or the loopback), our own id gets one
// too and replays us.
class EIEvilHarryP;
static const int MAX_PEERS = 8;
struct Peer
{
	int id;
	bool heard;                        // Snapshots are coming in from them.
	uint32_t lastHeard;
	RemotePlayer remote;
	const PlayerSnapshot* target;      // What their puppet shows this tick (see NetUpdate).

	// Their puppet, and where it was spawned : a puppet whose level is gone was
	// freed with it.
	EIEvilHarryP* beast;
	void* world;
	uint32_t level;
	uint32_t lastSpawnTry;
	int forcedState;

	// Locomotion (see UpdateLocomotion).
	int idleState, runLayer, idleLayer, sprintLayer, airLayer;
	float velX, velY;                  // Smoothed, units per tick.
	float moveSpeed;
	float heading;                     // Direction of travel, atan2( y, x ).
	bool moving, sprinting;
	float peakSpeed;                   // Fastest speed of the current move (logged).
	bool haveLastTarget;
	Vector3f lastTarget;
	bool airborne;
	int airTicks;
	float airStartZ, prevZ;
	int mirrorSlot;
	uint32_t mirrorClip;
	int lastLoggedState, lastRequested;

	// Where their name tag goes (screen pixels), noted when the puppet renders.
	float tagX, tagY, tagDepth;
	uint32_t tagFrame;                 // g_presentCount then.
};
static Peer g_peers[MAX_PEERS];        // Static : big, and 16-byte aligned.

// Who is in the room (from their PACKET_INFO), kept across levels. Presence goes by
// those packets (sent even while a player's game is loading or paused), not by
// snapshots.
static const uint32_t INFO_INTERVAL_MS = 1000;
static const uint32_t INFO_TIMEOUT_MS = 15000;
struct PlayerInfo
{
	bool present;
	char name[MAX_NAME + 1];
	uint32_t level;
	uint32_t lastInfo;
};
static PlayerInfo g_players[MAX_PEERS];
static char g_myName[MAX_NAME + 1] = "Harry";
static char g_roomName[64] = "";
static uint32_t g_lastInfoSent = 0;
static uint32_t g_lastNetUpdate = 0;

// Messages shown for a few seconds ("Bob joined").
static const int MAX_TOASTS = 4;
static const uint32_t TOAST_MS = 5000;
static struct { char text[96]; uint32_t time; } g_toasts[MAX_TOASTS];

static void Toast( const char* fmt, ... )
{
	for ( int i = MAX_TOASTS - 1; i > 0; i-- ) g_toasts[i] = g_toasts[i - 1];
	va_list args;
	va_start( args, fmt );
	vsnprintf_s( g_toasts[0].text, _TRUNCATE, fmt, args );
	va_end( args );
	g_toasts[0].time = NetNowMs();
}

// Names : printable ASCII only (the font has nothing else), never empty.
static void CleanName( const char* in, int length, char* out )
{
	int n = 0;
	for ( int i = 0; i < length && n < MAX_NAME; i++ ) {
		char c = in[i];
		if ( c >= 32 && c <= 126 ) out[n++] = c;
	}
	while ( n > 0 && out[n - 1] == ' ' ) n--;
	out[n] = 0;
}

static const char* PlayerName( int id )
{
	static char fallback[16];
	if ( id >= 0 && id < MAX_PEERS && g_players[id].name[0] ) return g_players[id].name;
	sprintf_s( fallback, "Player %d", id + 1 );
	return fallback;
}

static const char* LevelName( uint32_t crc )
{
	if ( !crc ) return "loading";
	uint32_t id = level_get_by_crc( crc );
	return id == (uint32_t) -1 ? "somewhere" : level_get_name( id );
}

// The peer being ticked or drawn. Between those : the main peer, the one F5, F6, F8
// and the autotest work on (the first player heard from).
static Peer* g_cur = &g_peers[0];
static int g_mainId = -1;

static Peer* MainPeer()
{
	return &g_peers[g_mainId >= 0 ? g_mainId : 0];
}

struct CurrentPeer
{
	Peer* saved;
	explicit CurrentPeer( Peer* p ) : saved( g_cur ) { g_cur = p; }
	~CurrentPeer() { g_cur = saved; }
};

static Peer* FindPeer( const void* beast )
{
	if ( !beast ) return 0;
	for ( int i = 0; i < MAX_PEERS; i++ ) if ( g_peers[i].beast == beast ) return &g_peers[i];
	return 0;
}

// Hands a puppet (and its locomotion state, everything from "beast" on) to another peer.
static void MovePuppet( Peer* from, Peer* to )
{
	size_t start = (uint8_t*) &from->beast - (uint8_t*) from;
	memcpy( (uint8_t*) to + start, (uint8_t*) from + start, sizeof(Peer) - start );
	from->beast = 0;
}

static void ResetPeer( Peer* p, int id )   // Forgets them (their puppet is left alone).
{
	p->id = id;
	p->heard = false;
	p->lastHeard = 0;
	p->remote.Reset();
	p->target = 0;
	p->lastSpawnTry = 0;
	p->forcedState = -1;
	p->lastLoggedState = p->lastRequested = -2;
	p->tagFrame = 0;
}

static const int DELAY_CHOICES[] = { 133, 250, 500, 1000, 1500, 3000 };   // Playback delay (ms).
static int g_delayIndex = 1;   // 250 ms (Multiplayer.ini : Delay).

// The network side.
static Transport* g_transport = 0;
static void QuitGame( UINT code );   // Leaves the room, then ends the game.
static bool g_networked = false;               // Through a server (not the loopback).
static uint32_t g_sendSequence = 0;
// Snapshots go out at most this often (30 per second : the game runs at 60, the
// receiver blends between them). A little under, so frame timing jitter at 60 fps
// doesn't skip one.
static const uint32_t SEND_INTERVAL_MS = 33 - 8;
static uint32_t g_lastSentTime = 0;
static const uint32_t REMOTE_SILENT_MS = 5000; // Then : forget them.
static struct NetStats {
	uint32_t sent, sentBytes, maxBytes, tooBig, badPackets, otherPlayers;
	float maxPackingError;
} g_netStats;

// The local snapshot written last : Harry's item draws (after his tick) add to it.
static PlayerSnapshot* LatestSnapshot()
{
	return g_haveLocal ? &g_local : 0;
}

static void ResetHistory()
{
	g_haveLocal = false;
	for ( int i = 0; i < MAX_PEERS; i++ ) ResetPeer( &g_peers[i], i );
	g_mainId = -1;
	g_cur = MainPeer();
}

// Autotest : how much packing a matrix loses (logs the worst ones).
static void CheckPacking( const char* what, int index, const Matrix4f& m, bool perAxis = false )
{
	if ( !IsSaneMatrix( m ) ) return;   // Sent as nothing.
	float err = PackingError( m, perAxis );
	if ( err <= g_netStats.maxPackingError ) return;
	g_netStats.maxPackingError = err;
	if ( err < 0.01F ) return;
	const float (*a)[4] = m.data;
	mp_log( "[NET] Packing loses %.4f on %s %d (held item %d, drawn %d) : (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) t (%.3f %.3f %.3f)\n",
		err, what, index, g_local.heldItem, g_local.drawnSlot, a[0][0], a[0][1], a[0][2], a[1][0], a[1][1], a[1][2], a[2][0], a[2][1], a[2][2], a[3][0], a[3][1], a[3][2] );
}

// Sends the local snapshot once it's complete (the tick after it was recorded :
// item draws come after the tick).
static void SendLocalSnapshot()
{
	if ( !g_haveLocal || !g_transport ) return;
	int me = g_transport->PlayerId();
	if ( me < 0 ) return;   // Not in a room yet.
	if ( g_netStats.sent && g_local.time - g_lastSentTime < SEND_INTERVAL_MS ) return;
	g_lastSentTime = g_local.time;
	uint8_t packet[MAX_PACKET];
	int length = EncodeSnapshot( g_local, (uint8_t) me, ++g_sendSequence, packet, sizeof(packet) );
	if ( !length ) {
		if ( !g_netStats.tooBig++ ) mp_log( "[NET] Snapshot doesn't fit in a packet (%d bones, %d item bones).\n", g_local.bones, g_local.itemBones );
		return;
	}
	g_transport->Send( packet, length );
	g_netStats.sent++;
	g_netStats.sentBytes += length;
	if ( (uint32_t) length > g_netStats.maxBytes ) g_netStats.maxBytes = length;

	if ( g_autotest ) {   // How much the packing loses.
		for ( int i = 0; i < g_local.bones; i++ ) CheckPacking( "bone", i, g_local.pose[i] );
		CheckPacking( "right hand", 0, g_local.handR );
		CheckPacking( "left hand", 0, g_local.handL );
		for ( int i = 0; i < g_local.itemBones; i++ ) CheckPacking( "item bone", i, g_local.itemPose[i], true );
	}
}

static void LogNetStats()
{
	const NetStats& n = g_netStats;
	int shown = 0;
	for ( int i = 0; i < MAX_PEERS; i++ ) if ( g_peers[i].heard ) shown++;
	mp_log( "[NET] Sent %u snapshots (avg %u bytes, max %u, %u too big). Bad %u. Ping %d ms. Max packing error %.4f. Delay %d ms. %d player(s) :\n",
		n.sent, n.sent ? n.sentBytes / n.sent : 0, n.maxBytes, n.tooBig, n.badPackets,
		g_transport ? g_transport->RoundTripMs() : -1, n.maxPackingError, DELAY_CHOICES[g_delayIndex], shown );
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		const Peer& p = g_peers[i];
		if ( !p.heard ) continue;
		const RemotePlayer& r = p.remote;
		mp_log( "[NET]   Player %d : received %u, lost %u, late %u, reordered %u. Playback : blended %u, held %u. Puppet %s.\n",
			p.id, r.received, r.Lost(), r.late, r.reordered, r.blended, r.held,
			p.beast ? "shown" : p.target ? "not spawned" : "not here" );
	}
}

// Takes in what arrived and picks what the puppet shows this tick.
static void NetUpdate()
{
	static PlayerSnapshot incoming;   // Static : big, and 16-byte aligned.
	uint8_t packet[MAX_PACKET];
	int length;
	uint32_t now = NetNowMs();
	if ( !g_transport ) return;
	g_lastNetUpdate = now;
	g_transport->Update();
	int me = g_transport->PlayerId();
	bool echo = g_transport->Echoes();
	while ( (length = g_transport->Receive( packet, sizeof(packet) )) > 0 ) {
		PacketHeader h;
		if ( !DecodeHeader( packet, length, &h ) ) { g_netStats.badPackets++; continue; }
		if ( h.player >= MAX_PEERS ) { g_netStats.badPackets++; continue; }
		Peer* peer = &g_peers[h.player];
		if ( h.type == PACKET_LEAVE ) {
			if ( peer->heard || g_players[h.player].present ) {
				mp_log( "[NET] Player %d left.\n", h.player );
				if ( g_players[h.player].present ) Toast( "%s left", PlayerName( h.player ) );
				ResetPeer( peer, h.player );   // Their puppet stays hidden (no target).
			}
			g_players[h.player].present = false;
			g_players[h.player].name[0] = 0;
			continue;
		}
		if ( h.type == PACKET_INFO ) {
			if ( h.player == me && !echo ) continue;
			if ( length < PACKET_HEADER_SIZE + 5 ) { g_netStats.badPackets++; continue; }
			const uint8_t* q = packet + PACKET_HEADER_SIZE;
			int nameLength = q[4];
			if ( PACKET_HEADER_SIZE + 5 + nameLength > length ) { g_netStats.badPackets++; continue; }
			PlayerInfo& info = g_players[h.player];
			char name[MAX_NAME + 1];
			CleanName( (const char*) q + 5, nameLength, name );
			if ( !name[0] ) sprintf_s( name, "Player %d", h.player + 1 );
			memcpy( &info.level, q, 4 );
			info.lastInfo = now;
			if ( !info.present || strcmp( info.name, name ) != 0 ) {
				mp_log( "[NET] Player %d is \"%s\", in %s.\n", h.player, name, LevelName( info.level ) );
				if ( !info.present && h.player != me ) Toast( "%s joined", name );
				strcpy_s( info.name, name );
				info.present = true;
			}
			continue;
		}
		if ( h.type != PACKET_SNAPSHOT ) continue;
		if ( h.player == me && !echo ) continue;   // Ourselves.
		if ( !DecodeSnapshot( packet, length, &incoming ) ) { g_netStats.badPackets++; continue; }
		if ( !peer->heard ) {
			peer->heard = true;
			mp_log( "[NET] Showing player %d.\n", h.player );
			if ( g_mainId < 0 ) {
				// A puppet spawned before anyone was heard from (F5, the autotest) is
				// peer 0's : it now shows this player.
				Peer* first = &g_peers[0];
				if ( h.player != 0 && first->beast && !first->heard && !peer->beast ) MovePuppet( first, peer );
				g_mainId = h.player;
				g_cur = MainPeer();
			}
		}
		peer->remote.Add( incoming, h.sequence );
		peer->lastHeard = now;
	}
	uint32_t level = Gizmod::getCurrentLevelCRC();
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		PlayerInfo& info = g_players[i];
		if ( info.present && now - info.lastInfo > INFO_TIMEOUT_MS ) {
			mp_log( "[NET] No news from player %d (\"%s\") for %u s : they're gone.\n", i, info.name, INFO_TIMEOUT_MS / 1000 );
			Toast( "%s left (connection lost)", info.name );
			info.present = false;
			info.name[0] = 0;
		}
	}
	if ( me >= 0 && now - g_lastInfoSent >= INFO_INTERVAL_MS ) {   // Who we are, where.
		g_lastInfoSent = now;
		uint8_t info[PACKET_HEADER_SIZE + 5 + MAX_NAME];
		EncodeHeader( PACKET_INFO, (uint8_t) me, 0, info );
		uint32_t where = g_haveLocal ? level : 0;
		memcpy( info + PACKET_HEADER_SIZE, &where, 4 );
		int n = (int) strlen( g_myName );
		info[PACKET_HEADER_SIZE + 4] = (uint8_t) n;
		memcpy( info + PACKET_HEADER_SIZE + 5, g_myName, n );
		g_transport->Send( info, PACKET_HEADER_SIZE + 5 + n );
	}
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer* peer = &g_peers[i];
		if ( peer->heard && now - peer->lastHeard > REMOTE_SILENT_MS ) {
			mp_log( "[NET] Nothing from player %d for %u s : forgetting them.\n", i, REMOTE_SILENT_MS / 1000 );
			ResetPeer( peer, i );
		}
		peer->target = peer->remote.Update( now, DELAY_CHOICES[g_delayIndex] );
		if ( peer->target && peer->target->level != level ) peer->target = 0;   // Another level : not shown.
	}

	static uint32_t lastStats = now;
	if ( now - lastStats >= 10000 ) {
		lastStats = now;
		LogNetStats();
	}
}

// --------------------------------------------------------------------------------
// Animation clips. EAnimController (+0xB4 in every character) keeps its layers in a
// map at +0x2C keyed by layer index (lookup 0x6F3A50). A layer (0xA0 bytes) holds
// its clip instance at +0x78 (clip CRC at +0x0C) and its weight at +0x4C.
// 0x61CAD0( layer, clipCRC ) loads a clip into a layer (creating the layer).
// --------------------------------------------------------------------------------
GET_METHOD( 0x6F3A50, int,  AnimMap_Find,     void*, int, void** );
GET_METHOD( 0x61CAD0, void, AnimCtrl_SetClip, EAnimController*, int, uint32_t );

static const uint32_t CHAR_ANIMCTRL   = 0xB4;
static const uint32_t ANIMCTRL_LAYERS = 0x2C;
static const uint32_t LAYER_CLIP      = 0x78;
static const uint32_t LAYER_WEIGHT    = 0x4C;
static const uint32_t CLIP_CRC        = 0x0C;
static const int      MAX_LAYER_SCAN  = 256;

// Every animated model (characters at +0xB4, and the animated items) is posed by an
// EAnimController : world-space bone matrices at +0x4, its skeleton (ERCharacter) at
// +0x18 with the bone count at +0x18, and at +0x24 the frame it last posed at
// (0x61A970 poses again only when that differs from the frame counter).
static const uint32_t ANIMCTRL_POSE     = 0x04;
static const uint32_t ANIMCTRL_SKELETON = 0x18;
static const uint32_t SKELETON_BONES    = 0x18;
static const uint32_t ANIMCTRL_STAMP    = 0x24;
static const uint32_t RENDER_FRAME      = 0x91779C;   // uint32_t : the frame counter it compares with.

static Matrix4f* GetPose( uint8_t* animCtrl, int* bones )
{
	void* skeleton = FIELD( animCtrl, void*, ANIMCTRL_SKELETON );
	*bones = skeleton ? FIELD( skeleton, int, SKELETON_BONES ) : 0;
	return FIELD( animCtrl, Matrix4f*, ANIMCTRL_POSE );
}

static uint8_t* GetAnimLayer( EICharacter* c, int key )
{
	void* layer = 0;
	void* map = (uint8_t*) c + CHAR_ANIMCTRL + ANIMCTRL_LAYERS;
	if ( !AnimMap_Find( map, key, &layer ) ) return 0;
	return (uint8_t*) layer;
}

static uint32_t GetLayerClipCRC( uint8_t* layer )
{
	uint8_t* clip = *(uint8_t**) (layer + LAYER_CLIP);
	return clip ? *(uint32_t*) (clip + CLIP_CRC) : 0;
}

// Layer 217 holds a frozen pose (its clock never moves) that Harry blends from
// when he switches item animations; mirroring it froze the puppet mid pull-out.
static const int HOLD_POSE_LAYER = 217;

// The clip a character is mostly showing : its highest-weight layer.
static uint32_t GetPlayingClip( EICharacter* c, int* outLayer, float* outWeight )
{
	uint32_t best = 0;
	float bestWeight = 0.01F;
	int bestLayer = -1;
	for ( int i = 0; i < MAX_LAYER_SCAN; i++ ) {
		if ( i == HOLD_POSE_LAYER ) continue;
		uint8_t* layer = GetAnimLayer( c, i );
		if ( !layer ) continue;
		float w = *(float*) (layer + LAYER_WEIGHT);
		uint32_t crc = GetLayerClipCRC( layer );
		if ( crc && w > bestWeight ) { best = crc; bestWeight = w; bestLayer = i; }
	}
	if ( outLayer ) *outLayer = bestLayer;
	if ( outWeight ) *outWeight = bestWeight;
	return best;
}

// --------------------------------------------------------------------------------
// Items. Harry+0x93C (EIPlayer::m_itemHotbar) is really his item manager : its
// first four pointers are the wheel's items, +0x10 is the item being taken out or
// put away and +0x14 the item in his hand (0 when empty-handed; cleared as soon as
// he starts putting it away). The game's item table (0x8EEB90, 9 entries of 0x1C
// bytes) has each item's object at +4.
// --------------------------------------------------------------------------------
static const uint32_t ITEM_TABLE = 0x8EEB90;
static const uint32_t ITEM_ENTRY_SIZE = 0x1C;
static const int NUM_ITEMS = 9;
static const int ITEM_SLING = 0, ITEM_TORCH = 1, ITEM_PICKAXES = 2, ITEM_TNT = 3, ITEM_SHIELD = 4, ITEM_RAFT = 5,
                 ITEM_GAS_MASK = 6, ITEM_CANTEEN = 7, ITEM_STINK_BOMB = 8;
static const uint32_t ITEM_MANAGER_HELD = 0x14;
static const uint32_t ITEM_WORLD_MATRIX = 0x30;   // Matrix4f the item draws its model with while out.
static const uint32_t HARRY_RIGHT_HAND = 0x470;   // Matrix4f (the torch draws at 1.5 x it).
static const uint32_t HARRY_LEFT_HAND = 0x4B0;    // Matrix4f (the pickaxes draw on both hands).

static void* GetItemObject( int index ) { return *(void**) (ITEM_TABLE + index * ITEM_ENTRY_SIZE + 4); }

static int GetHeldItem( EIHarry* harry )
{
	void* manager = harry->m_itemHotbar;
	if ( !manager ) return -1;
	void* held = FIELD( manager, void*, ITEM_MANAGER_HELD );
	if ( !held ) return -1;
	for ( int i = 0; i < NUM_ITEMS; i++ )
		if ( GetItemObject( i ) == held ) return i;
	return -1;
}

static void PushLocalSnapshot( EIHarry* harry )
{
	SendLocalSnapshot();   // The previous one, complete now.

	PlayerSnapshot& s = g_local;
	s.time          = NetNowMs();
	s.level         = Gizmod::getCurrentLevelCRC();
	s.transform     = harry->m_transformMatrix;
	s.position      = harry->m_position;
	s.rotationEuler = harry->m_rotationEuler;
	s.onGround      = harry->m_onGround;
	static float groundZ = 0.0F;
	if ( s.onGround ) groundZ = s.position.z;
	s.lift = s.onGround ? 0.0F : max( 0.0F, s.position.z - groundZ );

	int layer = -1;
	s.clipCRC = GetPlayingClip( harry, &layer, &s.clipWeight );
	static uint32_t lastPlayed = 0;   // Only the hold-pose layer is up : keep the clip before it.
	if ( s.clipCRC ) lastPlayed = s.clipCRC; else s.clipCRC = lastPlayed;
	static uint32_t lastClip = 0;
	if ( s.clipCRC != lastClip ) {
		mp_log( "[MP] Harry plays clip 0x%.8X (layer %d, weight %.2f, %s).\n",
			s.clipCRC, layer, s.clipWeight, harry->m_onGround ? "ground" : "air" );
		lastClip = s.clipCRC;
	}

	s.heldItem = GetHeldItem( harry );
	s.drawnSlot = -1;   // Set if Harry draws an item this frame (NoteHarryItemDraw).
	s.itemBones = 0;

	// Harry is posed by now (his pose stamp is this frame's).
	s.bones = 0;
	Matrix4f inverse;
	if ( MatInverseAffine( harry->m_transformMatrix, &inverse ) ) {
		int bones = 0;
		Matrix4f* pose = GetPose( (uint8_t*) harry + CHAR_ANIMCTRL, &bones );
		if ( pose && bones > 0 && bones <= MAX_CHAR_BONES ) {
			MatsMul( pose, bones, inverse, s.pose );
			s.bones = bones;
		}
		MatMul( FIELD( harry, Matrix4f, HARRY_RIGHT_HAND ), inverse, &s.handR );
		MatMul( FIELD( harry, Matrix4f, HARRY_LEFT_HAND ), inverse, &s.handL );
	}
	static int lastItem = -1;
	if ( s.heldItem != lastItem ) {
		mp_log( "[MP] Harry holds item %d.\n", s.heldItem );
		lastItem = s.heldItem;
	}

	g_haveLocal = true;
}

static const PlayerSnapshot* GetPuppetTarget()
{
	return g_cur->target;
}



// --------------------------------------------------------------------------------
// Puppet.
// --------------------------------------------------------------------------------

// Index of a state by its short name ("idle left" matches "evil harry.idle left").
static int FindStateIndex( EIBeast* beast, const char* shortName )
{
	EBeastStateMachine* sm = beast->GetStateMachine();
	if ( !sm || !sm->m_states ) return -1;
	for ( int i = 0; i < (int) sm->m_numStates; i++ ) {
		const char* name = sm->m_states[i].m_name;
		const char* dot = name ? strchr( name, '.' ) : 0;
		if ( dot && strcmp( dot + 1, shortName ) == 0 ) return i;
	}
	return -1;
}
static char g_puppetName[] = "evil harry";
static char* const GAME_EMPTY_STRING = (char*) 0x9410B0;   // The game strings' "" (0x6F047A).

static void LogStateMachine( EIBeast* beast )
{
	EBeastStateMachine* sm = beast->GetStateMachine();
	if ( !sm || !sm->m_states ) {
		mp_log( "[MP] Puppet has no state machine.\n" );
		return;
	}

	mp_log( "[MP] Puppet state machine \"%s\", %d states :\n", sm->m_beastTypeName, sm->m_numStates );
	for ( int i = 0; i < sm->m_numStates; i++ ) {
		mp_log( "[MP]   %d : %s\n", i, sm->m_states[i].m_name );
	}
}

static void InitLocomotion( EIEvilHarryP* p, EIHarry* harry );

static void SpawnPuppet( EIHarry* harry )
{
	if ( g_cur->beast ) {
		mp_log( "[MP] Player %d's puppet already spawned in this level.\n", g_cur->id );
		return;
	}
	if ( !harry->m_world ) {
		mp_log( "[MP] No world yet, can't spawn.\n" );
		return;
	}

	const type_info_t* type = get_type_by_vtable( VTABLE_EVILHARRY_P );
	if ( !type ) {
		mp_log( "[MP] EIEvilHarryP type not registered.\n" );
		return;
	}

	EIEvilHarryP* p = instantiate_object<EIEvilHarryP>( type );

	p->EIBeast::m_position.x = harry->m_position.x;
	p->EIBeast::m_position.y = harry->m_position.y + 15.0F;
	p->EIBeast::m_position.z = harry->m_position.z;
	p->m_rotation.x = 0.0F;
	p->m_rotation.y = 0.0F;
	p->m_rotation.z = 0.0F;
	p->m_rotation.w = 1.0F;

	// The asset loads by this name. Once loaded the name becomes the game's empty
	// string (see below) : the beast's destructor frees its name with the game's heap
	// (0x6F0490 -> 0x617D50) unless it's that one, and freeing our static memory
	// corrupted the heap when the level unloaded.
	p->m_beastTypeCRC = HashPath( (char*) "BEvilHarryP" );
	p->m_beastTypeName = g_puppetName;
	EIBeast_LoadBeastAsset( p );
	p->DeserializePart2();

	if ( !p->m_beastType ) {
		mp_log( "[MP] WARNING : Evil Harry asset didn't load in this level, puppet may be invisible.\n" );
	}

	AddToWorld( harry->m_world, p );
	ERLevel_SetupEntity( harry->m_world, p, 0 );
	p->Init();
	p->m_beastTypeName = GAME_EMPTY_STRING;

	g_cur->beast = p;
	g_cur->world = harry->m_world;
	g_cur->level = Gizmod::getCurrentLevelCRC();
	g_cur->lastLoggedState = g_cur->lastRequested = -2;
	int count = 0;
	for ( int i = 0; i < MAX_PEERS; i++ ) if ( g_peers[i].beast ) count++;
	mp_log( "[MP] Player %d's puppet spawned (%d puppet(s) in this level).\n", g_cur->id, count );

	// Its default state (21, "get ready for boss fight") draws it as a black
	// silhouette. Start calm and lit instead.
	g_cur->forcedState = FindStateIndex( p, "idle left" );
	mp_log( "[MP] Puppet starts in state %d (idle left).\n", g_cur->forcedState );
	InitLocomotion( p, harry );

	mp_log( "[MP] Puppet spawned at 0x%.8X (m_beastTypeName offset 0x%X, expected 0x%X).\n",
		p, (unsigned) offsetof(EIBeast, m_beastTypeName), (unsigned) EXPECTED_BEASTNAME_OFFSET );
	mp_log( "[MP] Harry at (%.2f, %.2f, %.2f). header offsets : EICharacter::m_position 0x%X, m_transformMatrix 0x%X, EIBeast::m_position 0x%X, m_rotation 0x%X\n",
		harry->m_position.x, harry->m_position.y, harry->m_position.z,
		(unsigned) offsetof(EICharacter, m_position), (unsigned) offsetof(EICharacter, m_transformMatrix),
		(unsigned) offsetof(EIBeast, m_position), (unsigned) offsetof(EIBeast, m_rotation) );

	if ( count > 1 ) return;
	// Raw dump of the transform-related area, before we touch anything.
	const uint32_t ranges[][2] = { { 0x120, 0x1B0 }, { 0x2F0, 0x350 } };
	for ( int r = 0; r < 2; r++ ) {
		for ( uint32_t o = ranges[r][0]; o < ranges[r][1]; o += 16 ) {
			float* f = (float*) ((uint8_t*) p + o);
			mp_log( "[MP]   +0x%03X : %10.3f %10.3f %10.3f %10.3f\n", o, f[0], f[1], f[2], f[3] );
		}
	}
	LogStateMachine( p );
}

// Called when a level loads : the old level's puppet was destroyed with it (already
// gone by now : don't touch it).
static void ForgetPuppet()
{
	int count = 0;
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		if ( g_peers[i].beast ) count++;
		g_peers[i].beast = 0;
	}
	if ( count ) mp_log( "[MP] New level, %d puppet(s) went with the old one.\n", count );
	ResetHistory();
}

// The game's own setters (writing the fields directly isn't enough : the model kept
// rendering at the spawn point). SetPos also moves the instance in the level's
// spatial structure (0x62B470), rebuilds the transform (vtable +0x124) and updates
// the collision object at +0x270. SetRot writes +0x140 and calls vtable +0x120.
GET_METHOD( 0x60FEA0, void, EICharacter_SetPos, EICharacter*, Vector3f*, bool );
GET_METHOD( 0x60FFA0, void, EICharacter_SetRot, EICharacter*, Vector3f* );

// Extra offset added to the puppet's position (the autotest uses it so the
// puppet stands beside Harry instead of inside him).
static Vector3f g_puppetOffset = { 0.0F, 0.0F, 0.0F };

// Hold the puppet at a fixed spot : the autotest's collision test, and F8 ("stay
// here") for testing by hand, where it keeps copying Harry's animations, items and
// facing without following him around.
static bool g_puppetPinned = false;
static bool g_puppetStaysHere = false;   // F8 : pinned, faces where Harry faced.
static Vector3f g_puppetPin = { 0.0F, 0.0F, 0.0F };

static bool GetPuppetTargetPosition( Vector3f* out )
{
	const PlayerSnapshot* t = GetPuppetTarget();
	if ( !t ) return false;
	if ( g_puppetPinned ) {
		*out = g_puppetPin;
		if ( g_puppetStaysHere ) out->z += t->lift;   // F8 : still jumps and climbs, in place.
		return true;
	}
	out->x = t->position.x + g_puppetOffset.x;
	out->y = t->position.y + g_puppetOffset.y;
	out->z = t->position.z + g_puppetOffset.z;
	return true;
}

// Raw field access. Spawn-time dump (Mama-Oullo Tower) showed : +0x134 position,
// +0x140 rotation, +0x14C scale (1,1,1), +0x160 transform matrix with its
// translation row at +0x190, +0x2FC EIBeast position, +0x310 quaternion.
// (0x414B10, vtable +0x120, rebuilds the matrix rotation and applies the scale;
// it does NOT set the translation.)
//
// The beast tick (0x414F80) reads its world position from +0x360, the translation
// row of a matrix at +0x330, and stores it at +0x370 ("previous position") when
// done. +0x330 is the beast's real world transform.
static const uint32_t BEAST_SCALE       = 0x14C;
static const uint32_t BEAST_WORLD       = 0x330;   // Matrix4f.
static const uint32_t BEAST_PREV_POS    = 0x370;   // Vector3f.

// --------------------------------------------------------------------------------
// Locomotion : the puppet's animation and facing come from how its target moves
// (later : how the remote player moves). x/y is the ground plane, z is up.
// --------------------------------------------------------------------------------
static const float MP_PI = 3.14159265F;

static bool  g_autoLocomotion = true;   // F6 switches to hand-forced states.
// The rest of the locomotion state is per puppet (Peer).
static const int AIRBORNE_MIN_TICKS = 6;   // Slopes and bumps blip for up to ~5 ticks.
static const float JUMP_RISE_SPEED  = 0.10F;   // Units per tick upward while off the ground.

static const float RUN_START_SPEED = 0.05F;  // Units per tick (hysteresis).
static const float RUN_STOP_SPEED  = 0.02F;
// Harry's normal run measured about 0.36/tick (peaks near 0.44 on slopes).
static const float SPRINT_START_SPEED = 0.50F;
static const float SPRINT_STOP_SPEED  = 0.45F;
static const float TELEPORT_DIST   = 5.0F;   // Bigger jumps aren't movement.

// Angle of the model's forward axis in its own frame (0 = +X, pi/2 = +Y).
// +Y made it run backwards, so the model faces local -Y.
static float g_forwardLocalAngle = -MP_PI / 2.0F;

static void ResetLocomotion()
{
	g_cur->velX = g_cur->velY = g_cur->moveSpeed = 0.0F;
	g_cur->moving = false;
	g_cur->sprinting = false;
	g_cur->peakSpeed = 0.0F;
	g_cur->airborne = false;
	g_cur->airTicks = 0;
	g_cur->haveLastTarget = false;
}

static void UpdateLocomotion( EIEvilHarryP* p )
{
	Vector3f pos;
	if ( !GetPuppetTargetPosition( &pos ) ) return;

	if ( g_cur->haveLastTarget ) {
		float dx = pos.x - g_cur->lastTarget.x;
		float dy = pos.y - g_cur->lastTarget.y;
		if ( dx * dx + dy * dy > TELEPORT_DIST * TELEPORT_DIST ) dx = dy = 0.0F;

		g_cur->velX = g_cur->velX * 0.75F + dx * 0.25F;
		g_cur->velY = g_cur->velY * 0.75F + dy * 0.25F;
		g_cur->moveSpeed = sqrtf( g_cur->velX * g_cur->velX + g_cur->velY * g_cur->velY );
		if ( g_cur->moveSpeed > 0.01F ) g_cur->heading = atan2f( g_cur->velY, g_cur->velX );
	}
	if ( g_puppetStaysHere ) {
		// Not moving : face where Harry faced. His transform's row 1 points opposite
		// to where he walks (see InitLocomotion).
		const PlayerSnapshot* f = GetPuppetTarget();
		if ( f ) g_cur->heading = atan2f( -f->transform.data[1][1], -f->transform.data[1][0] );
	}
	g_cur->lastTarget = pos;
	g_cur->haveLastTarget = true;

	bool wasMoving = g_cur->moving;
	if ( g_cur->moving && g_cur->moveSpeed < RUN_STOP_SPEED ) g_cur->moving = false;
	else if ( !g_cur->moving && g_cur->moveSpeed > RUN_START_SPEED ) g_cur->moving = true;
	if ( g_cur->moving != wasMoving ) {
		if ( g_cur->moving ) {
			mp_log( "[MP] Puppet %d starts moving (speed %.3f/tick, heading %.0f deg).\n", g_cur->id, g_cur->moveSpeed,g_cur->heading * 180.0F / MP_PI );
			g_cur->peakSpeed = 0.0F;
		}
		else {
			mp_log( "[MP] Puppet %d stops (top speed %.3f/tick).\n", g_cur->id, g_cur->peakSpeed );
		}
	}
	if ( g_cur->moving && g_cur->moveSpeed > g_cur->peakSpeed ) g_cur->peakSpeed = g_cur->moveSpeed;

	// Airborne : the replayed player has been off the ground for a few ticks
	// (ignores one-tick blips on bumps and slopes). Landing is immediate.
	const PlayerSnapshot* t = GetPuppetTarget();
	bool wasAirborne = g_cur->airborne;
	g_cur->airTicks = (t && !t->onGround) ? g_cur->airTicks + 1 : 0;
	// A real jump rises fast from its first tick (~0.25/tick) : switch at once then,
	// instead of waiting AIRBORNE_MIN_TICKS (which lands near the top of the jump).
	float dz = pos.z - g_cur->prevZ;
	g_cur->prevZ = pos.z;
	if ( g_cur->airTicks == 0 ) g_cur->airborne = false;
	else if ( g_cur->airTicks >= AIRBORNE_MIN_TICKS || dz > JUMP_RISE_SPEED ) g_cur->airborne = true;
	if ( g_cur->airborne != wasAirborne ) {
		if ( g_cur->airborne ) {
			g_cur->airStartZ = pos.z;
			mp_log( "[MP] Puppet leaves the ground (z %.2f).\n", pos.z );
		}
		else {
			mp_log( "[MP] Puppet lands (z %.2f, %.2f from take-off).\n", pos.z, pos.z - g_cur->airStartZ );
		}
	}

	bool wasSprinting = g_cur->sprinting;
	if ( !g_cur->moving || (g_cur->sprinting && g_cur->moveSpeed < SPRINT_STOP_SPEED) ) g_cur->sprinting = false;
	else if ( !g_cur->sprinting && g_cur->moveSpeed > SPRINT_START_SPEED ) g_cur->sprinting = true;
	if ( g_cur->sprinting != wasSprinting && g_cur->moving ) {
		mp_log( "[MP] Puppet %s (speed %.3f/tick).\n", g_cur->sprinting ? "sprints" : "back to running", g_cur->moveSpeed );
	}

	// The state machine always stays in a calm idle state : forcing boss-fight
	// states like "runner" crashes (they expect setup we don't do). Running is
	// done by switching animation layers instead (ApplyLocomotionAnim).
	if ( g_autoLocomotion ) {
		g_cur->forcedState = g_cur->idleState;
	}
}

// Beast animation layers (from the beast's own PlayAnim, 0x41A820). The animation
// controller holds one layer per state-machine state (layer = EBeastState +0x38,
// set to the state's index), then the "locomotion" layers from the beast's
// quickdata at [+0x52C] : +0 move (CRC at +0x494), +1 idle (CRC at +0x498),
// +2 turn left, +3 turn right. [+0x530] is the layer currently playing.
GET_METHOD( 0x61D0D0, void, AnimCtrl_FadeTo,  EAnimController*, int, float );
GET_METHOD( 0x61D8A0, void, AnimCtrl_Restart, EAnimController*, int );

static const uint32_t BEAST_ANIMCTRL     = 0xB4;
static const uint32_t BEAST_ANIM_MOVE    = 0x494;   // CRC.
static const uint32_t BEAST_ANIM_IDLE    = 0x498;   // CRC.
static const uint32_t BEAST_LOCO_BASE    = 0x52C;   // int.
static const uint32_t BEAST_CUR_LAYER    = 0x530;   // int.
static const uint32_t BEAST_STATE_LAYER  = 0x38;    // In EBeastState (0x58 bytes).

static int GetStateAnimLayer( EIBeast* p, int state )
{
	EBeastStateMachine* sm = p->GetStateMachine();
	if ( !sm || !sm->m_states || state < 0 || state >= (int) sm->m_numStates ) return -1;
	return FIELD( &sm->m_states[state], int, BEAST_STATE_LAYER );
}

// Crossfades to an animation layer, like the beast's PlayAnim does.
static void PlayAnimLayer( EIEvilHarryP* p, int layer )
{
	EAnimController* ctrl = &FIELD( p, EAnimController, BEAST_ANIMCTRL );
	int current = FIELD( p, int, BEAST_CUR_LAYER );
	if ( layer < 0 || layer == current ) return;

	if ( current >= 0 ) AnimCtrl_FadeTo( ctrl, current, 0.0F );
	AnimCtrl_Restart( ctrl, layer );
	AnimCtrl_FadeTo( ctrl, layer, 1.0F );
	FIELD( p, int, BEAST_CUR_LAYER ) = layer;
}

// Mirroring : the puppet plays the replayed player's own clip (Harry's jump, climb,
// etc. : Evil Harry's state clips have no ordinary jump). Two spare layers take
// turns so the old clip can fade out while the new one fades in.
static bool g_mirrorClips = true;
static const int MIRROR_LAYERS[2] = { 200, 201 };

static bool ApplyMirroredClip( EIEvilHarryP* p )
{
	const PlayerSnapshot* t = GetPuppetTarget();
	if ( !g_mirrorClips || !t || !t->clipCRC ) return false;

	int current = FIELD( p, int, BEAST_CUR_LAYER );
	if ( t->clipCRC == g_cur->mirrorClip && current == MIRROR_LAYERS[g_cur->mirrorSlot] ) return true;

	EAnimController* ctrl = &FIELD( p, EAnimController, BEAST_ANIMCTRL );
	int next = (current == MIRROR_LAYERS[g_cur->mirrorSlot]) ? 1 - g_cur->mirrorSlot : g_cur->mirrorSlot;
	AnimCtrl_SetClip( ctrl, MIRROR_LAYERS[next], t->clipCRC );
	PlayAnimLayer( p, MIRROR_LAYERS[next] );
	g_cur->mirrorSlot = next;
	g_cur->mirrorClip = t->clipCRC;
	return true;
}

static void ApplyLocomotionAnim( EIEvilHarryP* p )
{
	static int overridden = 0;
	if ( ApplyMirroredClip( p ) ) return;

	int wanted = g_cur->idleLayer;
	if ( g_cur->airborne && g_cur->airLayer >= 0 ) wanted = g_cur->airLayer;
	else if ( g_cur->moving ) wanted = (g_cur->sprinting && g_cur->sprintLayer >= 0) ? g_cur->sprintLayer : g_cur->runLayer;
	int current = FIELD( p, int, BEAST_CUR_LAYER );
	if ( wanted < 0 || wanted == current ) return;

	// The idle state may re-play its own animation; count it to spot fights.
	if ( (current == g_cur->idleLayer || current == g_cur->runLayer || current == g_cur->sprintLayer || current == g_cur->airLayer) == false ) {
		if ( ++overridden <= 5 ) mp_log( "[MP] Puppet anim layer was %d, playing %d.\n", current, wanted );
	}
	PlayAnimLayer( p, wanted );
}

static void SetRunAnimation( EIEvilHarryP* p, const char* stateName )
{
	if ( !stateName ) {
		int base = FIELD( p, int, BEAST_LOCO_BASE );
		g_cur->runLayer = FIELD( p, uint32_t, BEAST_ANIM_MOVE ) ? base : -1;
		mp_log( "[MP] Run animation : quickdata move anim, layer %d.\n", g_cur->runLayer );
	}
	else {
		g_cur->runLayer = GetStateAnimLayer( p, FindStateIndex( p, stateName ) );
		mp_log( "[MP] Run animation : \"%s\" state's anim, layer %d.\n", stateName, g_cur->runLayer );
	}
}

// Standing animation. 0 = the beast's quickdata idle (locomotion layer +1); the
// "idle left/right" states' animation is the wall-climbing idle (pickaxes out).
static void SetIdleAnimation( EIEvilHarryP* p, const char* stateName )
{
	if ( !stateName ) {
		int base = FIELD( p, int, BEAST_LOCO_BASE );
		g_cur->idleLayer = FIELD( p, uint32_t, BEAST_ANIM_IDLE ) ? base + 1 : -1;
		mp_log( "[MP] Idle animation : quickdata idle anim, layer %d.\n", g_cur->idleLayer );
	}
	else {
		g_cur->idleLayer = GetStateAnimLayer( p, FindStateIndex( p, stateName ) );
		mp_log( "[MP] Idle animation : \"%s\" state's anim, layer %d.\n", stateName, g_cur->idleLayer );
	}
}

static void SetAirAnimation( EIEvilHarryP* p, const char* stateName )
{
	g_cur->airLayer = GetStateAnimLayer( p, FindStateIndex( p, stateName ) );
	mp_log( "[MP] Air animation : \"%s\" state's anim, layer %d.\n", stateName, g_cur->airLayer );
}

static void InitLocomotion( EIEvilHarryP* p, EIHarry* harry )
{
	ResetLocomotion();
	g_autoLocomotion = true;
	g_cur->mirrorClip = 0;
	g_cur->mirrorSlot = 0;
	g_cur->idleState = FindStateIndex( p, "idle left" );
	g_cur->idleLayer = GetStateAnimLayer( p, g_cur->idleState );

	mp_log( "[MP] Beast anims : move CRC 0x%.8X, idle CRC 0x%.8X, locomotion base layer %d, current layer %d, idle layer %d, runner layer %d.\n",
		FIELD( p, uint32_t, BEAST_ANIM_MOVE ), FIELD( p, uint32_t, BEAST_ANIM_IDLE ),
		FIELD( p, int, BEAST_LOCO_BASE ), FIELD( p, int, BEAST_CUR_LAYER ),
		g_cur->idleLayer, GetStateAnimLayer( p, FindStateIndex( p, "runner" ) ) );

	SetIdleAnimation( p, 0 );
	if ( g_cur->idleLayer < 0 ) g_cur->idleLayer = GetStateAnimLayer( p, g_cur->idleState );

	// The quickdata move animation if the beast has one (Evil Harry doesn't), else
	// the "chase player" state's : the most natural upright run of the candidates
	// ("runner" is hunched, "superrun" is a dive).
	SetRunAnimation( p, 0 );
	if ( g_cur->runLayer < 0 ) SetRunAnimation( p, "chase player" );

	// Above sprint speed : the "superrun to spot to ice pick" state's clip.
	g_cur->sprintLayer = GetStateAnimLayer( p, FindStateIndex( p, "superrun to spot to ice pick" ) );
	mp_log( "[MP] Sprint animation : layer %d (above %.2f/tick).\n", g_cur->sprintLayer, SPRINT_START_SPEED );

	// In the air (jumping, falling) : the "falling" state's clip.
	SetAirAnimation( p, "falling" );

	// Face where Harry faces. Walking showed his rigid body's row 1 (local +Y)
	// points where he moves; m_transformMatrix is turned 180 degrees from that.
	const Matrix4f& m = harry->m_rigidBodyMatrix;
	g_cur->heading = atan2f( m.data[1][1], m.data[1][0] );
	mp_log( "[MP] Locomotion : idle state %d, heading %.0f deg.\n", g_cur->idleState, g_cur->heading * 180.0F / MP_PI );
}

// Writes the beast's world transform : facing g_cur->heading, at the target position.
// Rolling as a ball : Harry's display matrix (m_transformMatrix) is raised 1.2
// above his position and spins about his side axis, while his position and
// rigid-body matrix stay upright. Detected by that raise.
static const float BALL_RAISE_MIN = 0.5F;

static bool IsBallMode( const PlayerSnapshot* t )
{
	return t && t->transform.data[3][2] - t->position.z > BALL_RAISE_MIN;
}

static void SetBeastWorld( EIEvilHarryP* p, const Vector3f& pos, const PlayerSnapshot* t )
{
	Matrix4f& world = FIELD( p, Matrix4f, BEAST_WORLD );
	if ( IsBallMode( t ) ) {
		// Take Harry's spinning, raised matrix as is (same skeleton, same forward axis).
		world = t->transform;
		world.data[3][0] = pos.x + (t->transform.data[3][0] - t->position.x);
		world.data[3][1] = pos.y + (t->transform.data[3][1] - t->position.y);
		world.data[3][2] = pos.z + (t->transform.data[3][2] - t->position.z);
		return;
	}
	float a = g_cur->heading - g_forwardLocalAngle;
	float c = cosf( a ), s = sinf( a );
	world.data[0][0] =  c;   world.data[0][1] = s;     world.data[0][2] = 0.0F;  world.data[0][3] = 0.0F;
	world.data[1][0] = -s;   world.data[1][1] = c;     world.data[1][2] = 0.0F;  world.data[1][3] = 0.0F;
	world.data[2][0] = 0.0F; world.data[2][1] = 0.0F;  world.data[2][2] = 1.0F;  world.data[2][3] = 0.0F;
	world.data[3][0] = pos.x;
	world.data[3][1] = pos.y;
	world.data[3][2] = pos.z;
	world.data[3][3] = 1.0F;
}

// Before the beast's own tick : make it think it already is at the target.
static void PrePositionPuppet( EIEvilHarryP* p )
{
	Vector3f pos;
	if ( GetPuppetTargetPosition( &pos ) ) {
		p->EIBeast::m_position = pos;
		SetBeastWorld( p, pos, GetPuppetTarget() );
		FIELD( p, Vector3f, BEAST_PREV_POS ) = pos;
	}
}

// Body collision. The beast's "collision off" routine (0x418840, used when it dies
// or goes limp) clears the byte at +0x4C4 and its "on" routine (0x418910) sets it
// back; with it at 0 Harry walks straight through. (The collision layer masks at
// +0x230/+0x564/+0x574/+0x584 made no difference, cleared or not.)
// The puppet is only a visual, so it is never solid.
static const uint32_t BEAST_BODY_SOLID = 0x4C4;   // uint8_t.
static bool g_puppetSolid = false;                 // Autotest flips it to compare.

static void ApplyPuppetCollision( EIEvilHarryP* p )
{
	FIELD( p, uint8_t, BEAST_BODY_SOLID ) = g_puppetSolid ? 1 : 0;
}

// Evil Harry's hands hold two static models (m_pickaxeModel0/1, attached to his
// hand bones). They stay hidden : the puppet gets a copy of Harry's own pickaxes,
// like his other items. Bit 0 of an instance's flags (+0x2C) is "visible".
static const uint32_t INSTANCE_FLAGS = 0x2C;
static const uint32_t INSTANCE_VISIBLE = 0x1;

static void ShowModel( void* model, bool show )
{
	if ( !model ) return;
	uint32_t& flags = FIELD( model, uint32_t, INSTANCE_FLAGS );
	flags = show ? (flags | INSTANCE_VISIBLE) : (flags & ~INSTANCE_VISIBLE);
}

static void ApplyPuppetItem( EIEvilHarryP* p )
{
	ShowModel( p->m_pickaxeModel0, false );
	ShowModel( p->m_pickaxeModel1, false );
}

// The puppet shows Harry's pose from the snapshot : his bones relative to his
// transform, placed on the puppet's. Both share one skeleton (76 bones), so this
// copies everything his clips blend together (taking items out, moving them with
// the item keys, etc.), which mirroring his main clip alone can't. Done right
// before the puppet renders, and marked posed for this frame so the game doesn't
// pose it over them. (The mirrored clip still runs underneath.)
static int g_puppetPoseCopies = 0;   // For the autotest.

static void ApplyPuppetPose( EIEvilHarryP* p, const PlayerSnapshot* t )
{
	uint8_t* animCtrl = (uint8_t*) p + CHAR_ANIMCTRL;
	int bones = 0;
	Matrix4f* pose = GetPose( animCtrl, &bones );
	if ( !pose || !t->bones || bones != t->bones ) return;
	MatsMul( t->pose, bones, p->m_transformMatrix, pose );
	FIELD( animCtrl, uint32_t, ANIMCTRL_STAMP ) = *(uint32_t*) RENDER_FRAME;
	g_puppetPoseCopies++;
}

// Items draw themselves (vtable slot 4 : (context, arg)) while out (item state
// +0x70 is 2-4, byte +0x91 set; the raft also needs byte +0x108), including while
// Harry takes them out and puts them away : most with their own matrix at +0x30;
// the animated ones (sling, shield, raft) with their bones; the pickaxes on Harry's
// hand matrices.
//
// Each time one draws for Harry, the latest snapshot notes where, relative to his
// transform. Since the puppet shows the same pose, the puppet's render then draws
// the item there relative to the puppet's transform, with those fields forced for
// the duration of the call.
typedef void (__thiscall *ItemDrawFunc)( void* item, void* context, void* arg );
typedef void (__thiscall *RenderFunc)( EInstance* self, void* context, void* arg );
static const uint32_t ITEM_SLOT_DRAW = 4 * 4;
static const uint32_t VTABLE_SLOT_RENDER = 0x34;      // EInstance::Render( context, arg ).
static const uint32_t ITEM_OWNER = 0x20;              // The player holding it.
static const uint32_t ITEM_STATE = 0x70;              // 3 : out.
static const uint32_t ITEM_ACTIVE = 0x91;             // uint8_t.
static const uint32_t RAFT_INFLATED = 0x108;          // uint8_t.
static Matrix4f g_puppetItemPose[MAX_ITEM_BONES];     // The copy's bones (static : 16-byte aligned).

// "anim" : offset of the item's animated model (an EAnimController), 0 if none.
// "shown" : a byte its draw needs set (cleared as soon as Harry's is put away, so
// the copy forces it), 0 if none. (The raft's, +0x108, can't be forced.)
// The stink bomb isn't in the wheel menu, but costs nothing to cover.
static const struct { int item; uint32_t vtable; uint32_t anim; uint32_t shown; } DRAWN_ITEMS[] = {
	{ ITEM_TORCH,      0x89A200, 0,    0     },
	{ ITEM_TNT,        0x89A160, 0,    0     },
	{ ITEM_RAFT,       0x8949B0, 0xA4, 0     },
	{ ITEM_SLING,      0x8971D0, 0xA0, 0x104 },
	{ ITEM_SHIELD,     0x896F90, 0xA4, 0x108 },
	{ ITEM_GAS_MASK,   0x87CAD8, 0,    0     },
	{ ITEM_CANTEEN,    0x86DCB0, 0,    0     },
	{ ITEM_STINK_BOMB, 0x8994B8, 0,    0     },
	{ ITEM_PICKAXES,   0x88ED70, 0,    0     },
};
static const int NUM_DRAWN_ITEMS = sizeof(DRAWN_ITEMS) / sizeof(DRAWN_ITEMS[0]);
static ItemDrawFunc g_origItemDraw[NUM_DRAWN_ITEMS];
static RenderFunc g_origEvilHarryRender = 0;

static int g_puppetItemDraws = 0;     // For the autotest.
static int g_copyDrawsAlone = 0;      // For the autotest : copies drawn while Harry draws no item.
static DWORD g_harryItemDrawTime = 0;

static void NoteHarryItemDraw( int slot, void* item )
{
	EIHarry* owner = FIELD( item, EIHarry*, ITEM_OWNER );
	PlayerSnapshot* s = LatestSnapshot();
	if ( !owner || owner != Gizmod::getHarry() || !s ) return;
	// Until it has inflated, the raft's draw only poses it (something else shows it).
	if ( DRAWN_ITEMS[slot].item == ITEM_RAFT && !FIELD( item, uint8_t, RAFT_INFLATED ) ) return;
	Matrix4f inverse;
	if ( !MatInverseAffine( owner->m_transformMatrix, &inverse ) ) return;

	s->itemBones = 0;
	if ( DRAWN_ITEMS[slot].anim ) {
		int bones = 0;
		Matrix4f* pose = GetPose( (uint8_t*) item + DRAWN_ITEMS[slot].anim, &bones );
		static bool logged[NUM_DRAWN_ITEMS] = { false, };
		if ( !logged[slot] ) {
			mp_log( "[MP] Item %d's animated model has %d bones.\n", DRAWN_ITEMS[slot].item, bones );
			logged[slot] = true;
		}
		if ( !pose || bones <= 0 || bones > MAX_ITEM_BONES ) return;
		MatsMul( pose, bones, inverse, s->itemPose );   // Posed by the draw that just ran.
		s->itemBones = bones;
	}
	Matrix4f world;
	memcpy( &world, (uint8_t*) item + ITEM_WORLD_MATRIX, sizeof(world) );
	MatMul( world, inverse, &s->itemLocal );
	s->itemState = FIELD( item, int, ITEM_STATE );
	s->drawnSlot = slot;
	g_harryItemDrawTime = GetTickCount();
}

// Swaps a Matrix4f field (maybe not 16-byte aligned) for "m * base", keeping the old one.
static void SwapMatrix( void* field, const Matrix4f& m, const Matrix4f& base, Matrix4f* saved )
{
	Matrix4f world;
	MatMul( m, base, &world );
	memcpy( saved, field, sizeof(Matrix4f) );
	memcpy( field, &world, sizeof(Matrix4f) );
}

// Draws item slot "slot" as snapshot "t" saw it, relative to "base" instead of
// Harry's transform, whatever state Harry's item is in now.
static void DrawItemAt( int slot, const PlayerSnapshot* t, const Matrix4f& base, void* context, void* arg )
{
	int index = DRAWN_ITEMS[slot].item;
	uint8_t* item = (uint8_t*) GetItemObject( index );
	EIHarry* harry = Gizmod::getHarry();
	if ( !item || !harry ) return;

	// The raft's animated model only exists in a drawable state while Harry's own
	// raft is inflated (forcing it earlier crashes the renderer) : no copy otherwise.
	if ( index == ITEM_RAFT && !FIELD( item, uint8_t, RAFT_INFLATED ) ) return;

	// Animated items : the snapshot's bones, marked posed so the draw keeps them.
	uint8_t* anim = DRAWN_ITEMS[slot].anim ? item + DRAWN_ITEMS[slot].anim : 0;
	Matrix4f* harryPose = 0;
	uint32_t stamp = 0;
	if ( anim ) {
		int bones = 0;
		harryPose = GetPose( anim, &bones );
		if ( !harryPose || !t->itemBones || bones != t->itemBones ) return;
		MatsMul( t->itemPose, bones, base, g_puppetItemPose );
		stamp = FIELD( anim, uint32_t, ANIMCTRL_STAMP );
		FIELD( anim, Matrix4f*, ANIMCTRL_POSE ) = g_puppetItemPose;
		FIELD( anim, uint32_t, ANIMCTRL_STAMP ) = *(uint32_t*) RENDER_FRAME;
	}

	Matrix4f savedMatrix, savedRight, savedLeft;
	SwapMatrix( item + ITEM_WORLD_MATRIX, t->itemLocal, base, &savedMatrix );
	bool hands = index == ITEM_PICKAXES;   // They draw on Harry's hands.
	if ( hands ) {
		SwapMatrix( (uint8_t*) harry + HARRY_RIGHT_HAND, t->handR, base, &savedRight );
		SwapMatrix( (uint8_t*) harry + HARRY_LEFT_HAND, t->handL, base, &savedLeft );
	}
	int savedState = FIELD( item, int, ITEM_STATE );
	uint8_t savedActive = FIELD( item, uint8_t, ITEM_ACTIVE );
	uint32_t shown = DRAWN_ITEMS[slot].shown;
	uint8_t savedShown = shown ? FIELD( item, uint8_t, shown ) : 0;
	FIELD( item, int, ITEM_STATE ) = t->itemState;
	FIELD( item, uint8_t, ITEM_ACTIVE ) = 1;
	if ( shown ) FIELD( item, uint8_t, shown ) = 1;

	g_origItemDraw[slot]( item, context, arg );
	g_puppetItemDraws++;
	if ( GetTickCount() - g_harryItemDrawTime > 150 ) g_copyDrawsAlone++;

	if ( shown ) FIELD( item, uint8_t, shown ) = savedShown;
	FIELD( item, uint8_t, ITEM_ACTIVE ) = savedActive;
	FIELD( item, int, ITEM_STATE ) = savedState;
	if ( hands ) {
		memcpy( (uint8_t*) harry + HARRY_LEFT_HAND, &savedLeft, sizeof(Matrix4f) );
		memcpy( (uint8_t*) harry + HARRY_RIGHT_HAND, &savedRight, sizeof(Matrix4f) );
	}
	memcpy( item + ITEM_WORLD_MATRIX, &savedMatrix, sizeof(Matrix4f) );
	if ( anim ) {
		FIELD( anim, Matrix4f*, ANIMCTRL_POSE ) = harryPose;
		FIELD( anim, uint32_t, ANIMCTRL_STAMP ) = stamp;
	}
}

// Draws the item the puppet's snapshot says Harry drew, if it is "slot".
static void DrawPuppetItem( int slot, void* context, void* arg )
{
	if ( !g_cur->beast ) return;
	const PlayerSnapshot* t = GetPuppetTarget();
	if ( !t || t->drawnSlot != slot ) return;
	DrawItemAt( slot, t, g_cur->beast->m_transformMatrix, context, arg );
}

static void DrawPuppetRafts( int slot, void* context, void* arg )
{
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		if ( !g_peers[i].beast ) continue;
		CurrentPeer current( &g_peers[i] );
		DrawPuppetItem( slot, context, arg );
	}
}

// The raft's copy is drawn right after Harry's raft (drawing it from the puppet's
// render crashed the renderer while flushing the frame), so it only shows while
// Harry's raft is out too. The other items are drawn by the puppet's render.
#define ITEM_DRAW_HOOK( n ) \
	static void __fastcall ItemDraw_Hook##n( void* item, void*, void* context, void* arg ) \
	{ \
		g_origItemDraw[n]( item, context, arg ); \
		NoteHarryItemDraw( n, item ); \
		if ( DRAWN_ITEMS[n].item == ITEM_RAFT ) DrawPuppetRafts( n, context, arg ); \
	}
ITEM_DRAW_HOOK( 0 )
ITEM_DRAW_HOOK( 1 )
ITEM_DRAW_HOOK( 2 )
ITEM_DRAW_HOOK( 3 )
ITEM_DRAW_HOOK( 4 )
ITEM_DRAW_HOOK( 5 )
ITEM_DRAW_HOOK( 6 )
ITEM_DRAW_HOOK( 7 )
ITEM_DRAW_HOOK( 8 )
static void* const ITEM_DRAW_HOOKS[] = {
	&ItemDraw_Hook0, &ItemDraw_Hook1, &ItemDraw_Hook2, &ItemDraw_Hook3, &ItemDraw_Hook4,
	&ItemDraw_Hook5, &ItemDraw_Hook6, &ItemDraw_Hook7, &ItemDraw_Hook8,
};
static_assert( sizeof(ITEM_DRAW_HOOKS) / sizeof(ITEM_DRAW_HOOKS[0]) == NUM_DRAWN_ITEMS, "one hook per drawn item" );

// --------------------------------------------------------------------------------
// Name tags : where the top of a puppet's head lands on screen, from the camera
// matrices the game rendered it with. Drawn when the frame is presented.
// --------------------------------------------------------------------------------
static const uint32_t RENDERER_DEVICE = 0x926790 + 0x74;
static const float TAG_ABOVE_FEET = 5.6F;   // The top of Harry's head (measured on screenshots).
static uint32_t g_presentCount = 1;

// The camera : the game builds its matrix each frame with a look-at (0x6EE560,
// called from 0x449B9A : eye, target, up), the camera's own world matrix (it looks
// down its -Z). Its projection is captured below (right-handed : w is minus the
// view depth; 50 degrees, 4:3, whatever the window's shape). The matrix at 0x926A40
// looks like one but isn't the main view's.
typedef Matrix4f* (__thiscall *LookAtFunc)( Matrix4f*, Vector3f*, Vector3f*, Vector3f* );
static const uint32_t CAMERA_LOOKAT_CALL = 0x449B9A;
static const LookAtFunc CameraLookAt = (LookAtFunc) 0x6EE560;
static Matrix4f g_cameraWorld;
static bool g_haveCamera = false;

static Matrix4f* __fastcall CameraLookAt_Hook( Matrix4f* self, void*, Vector3f* eye, Vector3f* target, Vector3f* up )
{
	Matrix4f* m = CameraLookAt( self, eye, target, up );
	g_cameraWorld = *m;
	g_haveCamera = true;
	return m;
}

// The perspective matrix, built at 0x6EE990 (called from 0x634B1C) : captured.
typedef Matrix4f* (__thiscall *PerspectiveFunc)( Matrix4f*, float, float, float, float );
static const uint32_t PERSPECTIVE_CALL = 0x634B1C;
static const PerspectiveFunc MakePerspective = (PerspectiveFunc) 0x6EE990;
static Matrix4f g_cameraProj;
static bool g_haveProj = false;

static Matrix4f* __fastcall Perspective_Hook( Matrix4f* self, void*, float a, float b, float c, float d )
{
	Matrix4f* m = MakePerspective( self, a, b, c, d );
	g_cameraProj = *m;
	static int logged = 0;
	if ( logged < 2 ) {
		logged++;
		const float (*p)[4] = m->data;
		mp_log( "[MP] Perspective (%.3f, %.3f, %.3f, %.3f) : %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f\n", a, b, c, d,
			p[0][0], p[1][1], p[2][0], p[2][1], p[2][2], p[2][3], p[3][2], p[3][3] );
	}
	g_haveProj = true;
	return m;
}

static void HookCamera()
{
	static const uint8_t PERSPECTIVE_ORIGINAL[5] = { 0xE8, 0x6F, 0x9E, 0x0B, 0x00 };   // call 0x6EE990.
	if ( memcmp( (void*) PERSPECTIVE_CALL, PERSPECTIVE_ORIGINAL, 5 ) == 0 ) injector::MakeCALL( PERSPECTIVE_CALL, &Perspective_Hook );
	else mp_log( "[MP] The perspective call isn't what we expect.\n" );

	static const uint8_t ORIGINAL[5] = { 0xE8, 0xC1, 0x49, 0x2A, 0x00 };   // call 0x6EE560.
	if ( memcmp( (void*) CAMERA_LOOKAT_CALL, ORIGINAL, 5 ) != 0 ) {
		mp_log( "[MP] The camera call isn't what we expect (free camera on?) : no name tags.\n" );
		return;
	}
	injector::MakeCALL( CAMERA_LOOKAT_CALL, &CameraLookAt_Hook );
}

static void NoteNameTag( Peer* peer, EIEvilHarryP* p )
{
	Matrix4f view;
	if ( !g_haveCamera || !MatInverseAffine( g_cameraWorld, &view ) ) return;
	if ( !g_haveProj ) return;
	const float* proj = &g_cameraProj.data[0][0];
	IDirect3DDevice9* dev = *(IDirect3DDevice9**) RENDERER_DEVICE;
	D3DVIEWPORT9 vp;
	if ( !dev || FAILED( dev->GetViewport( &vp ) ) ) return;

	// Above his head : his transform is at his feet. (The highest bone isn't reliable :
	// some bones, not drawn, sit far above him in some poses.)
	const Matrix4f& m = p->m_transformMatrix;
	float x = m.data[3][0], y = m.data[3][1], z = m.data[3][2] + TAG_ABOVE_FEET;

	float v[4], c[4];   // Row vector times view, times projection.
	for ( int j = 0; j < 4; j++ ) v[j] = x * view.data[0][j] + y * view.data[1][j] + z * view.data[2][j] + view.data[3][j];
	for ( int j = 0; j < 4; j++ ) c[j] = v[0] * proj[j] + v[1] * proj[4 + j] + v[2] * proj[8 + j] + v[3] * proj[12 + j];
	if ( c[3] < 0.1F ) return;   // Behind the camera.
	peer->tagX = vp.X + (c[0] / c[3] + 1.0F) * 0.5F * vp.Width;
	peer->tagY = vp.Y + (1.0F - c[1] / c[3]) * 0.5F * vp.Height;
	peer->tagDepth = c[3];
	peer->tagFrame = g_presentCount;

	static int logged = 0;
	if ( logged < 3 ) {
		logged++;
		mp_log( "[MP] Name tag : head (%.1f, %.1f, %.1f) at screen (%.0f, %.0f), depth %.1f, viewport %ux%u.\n",
			x, y, z, peer->tagX, peer->tagY, peer->tagDepth, vp.Width, vp.Height );
	}
}

static void __fastcall EvilHarryRender_Hook( EIEvilHarryP* self, void*, void* context, void* arg )
{
	Peer* peer = FindPeer( self );
	if ( !peer ) { g_origEvilHarryRender( self, context, arg ); return; }   // The game's own Evil Harry.
	CurrentPeer current( peer );
	const PlayerSnapshot* t = GetPuppetTarget();
	// Online with nobody to show here (they left, or are in another level) : not drawn.
	if ( g_networked && !t ) return;
	if ( t ) ApplyPuppetPose( self, t );
	g_origEvilHarryRender( self, context, arg );
	if ( t ) NoteNameTag( peer, self );
	if ( t && t->drawnSlot >= 0 && DRAWN_ITEMS[t->drawnSlot].item != ITEM_RAFT ) DrawPuppetItem( t->drawnSlot, context, arg );
}

static void InstallItemDrawHooks()
{
	for ( int i = 0; i < NUM_DRAWN_ITEMS; i++ ) {
		uint32_t slot = DRAWN_ITEMS[i].vtable + ITEM_SLOT_DRAW;
		g_origItemDraw[i] = injector::ReadMemory<ItemDrawFunc>( slot, true );
		injector::WriteMemory( slot, ITEM_DRAW_HOOKS[i], true );
	}
	uint32_t render = VTABLE_EVILHARRY_P + VTABLE_SLOT_RENDER;
	g_origEvilHarryRender = injector::ReadMemory<RenderFunc>( render, true );
	injector::WriteMemory( render, &EvilHarryRender_Hook, true );
}

static void DrivePuppet( EIEvilHarryP* p )
{
	ApplyPuppetCollision( p );

	Vector3f pos;
	if ( !GetPuppetTargetPosition( &pos ) ) { ApplyPuppetItem( p ); return; }

	const PlayerSnapshot* t = GetPuppetTarget();
	ApplyPuppetItem( p );
	Vector3f rot = t->rotationEuler;

	p->EIBeast::m_position = pos;
	EICharacter_SetPos( p, &pos, true );   // Spatial structure, bounds, collision.
	EICharacter_SetRot( p, &rot );         // Rebuilds rotation + scale of the matrix.

	// The beast's own world transform (what its tick and AI work from).
	SetBeastWorld( p, pos, t );

	// Same orientation and position in the header transform.
	const Matrix4f& world = FIELD( p, Matrix4f, BEAST_WORLD );
	for ( int r = 0; r < 3; r++ )
		for ( int c = 0; c < 3; c++ )
			p->m_transformMatrix.data[r][c] = world.data[r][c];
	p->m_transformMatrix.data[3][0] = world.data[3][0];   // Raised when rolling.
	p->m_transformMatrix.data[3][1] = world.data[3][1];
	p->m_transformMatrix.data[3][2] = world.data[3][2];
	FIELD( p, Vector3f, BEAST_PREV_POS ) = pos;

}



// --------------------------------------------------------------------------------
// Hotkeys.
// --------------------------------------------------------------------------------
// Keys are polled on our own thread (independent of the game's tick rate and of
// which window the game thinks it owns), and turned into requests the game
// thread consumes in HandleHotkeys().
static volatile LONG g_reqSpawn = 0;
static volatile LONG g_reqState = 0;
static volatile LONG g_reqDelay = 0;
static volatile LONG g_reqStay = 0;
static volatile LONG g_reqList = 0;
static bool g_showList = false;   // F9.
static volatile LONG g_harryTicks = 0;

static bool GameHasFocus()
{
	HWND fg = GetForegroundWindow();
	if ( !fg ) return false;
	DWORD pid = 0;
	GetWindowThreadProcessId( fg, &pid );
	return pid == GetCurrentProcessId();   // Game window or its console.
}

static DWORD WINAPI HotkeyThread( void* )
{
	static const int NUM_KEYS = 5;
	bool wasDown[NUM_KEYS] = { false, false, false, false, false };
	const int keys[NUM_KEYS] = { VK_F5, VK_F6, VK_F7, VK_F8, VK_F9 };
	volatile LONG* reqs[NUM_KEYS] = { &g_reqSpawn, &g_reqState, &g_reqDelay, &g_reqStay, &g_reqList };

	for ( ;; ) {
		for ( int i = 0; i < NUM_KEYS; i++ ) {
			bool down = (GetAsyncKeyState( keys[i] ) & 0x8000) != 0;
			if ( down && !wasDown[i] ) {
				if ( GameHasFocus() ) {
					InterlockedIncrement( reqs[i] );
					mp_log( "[MP] F%d pressed (Harry ticks so far : %ld).\n", 5 + i, g_harryTicks );
				}
				else {
					mp_log( "[MP] F%d ignored : game window not focused.\n", 5 + i );
				}
			}
			wasDown[i] = down;
		}
		Sleep( 15 );
	}
	return 0;
}

static void HandleHotkeys( EIHarry* harry )
{
	bool f5 = InterlockedExchange( &g_reqSpawn, 0 ) != 0;
	bool f6 = InterlockedExchange( &g_reqState, 0 ) != 0;
	bool f7 = InterlockedExchange( &g_reqDelay, 0 ) != 0;
	bool f8 = InterlockedExchange( &g_reqStay, 0 ) != 0;
	if ( InterlockedExchange( &g_reqList, 0 ) ) g_showList = !g_showList;

	// The puppet went with the last level : F8's "stay here" goes too.
	if ( !g_cur->beast && g_puppetStaysHere ) g_puppetPinned = g_puppetStaysHere = false;

	if ( f5 ) {
		SpawnPuppet( harry );
	}

	if ( f6 && g_cur->beast ) {
		// Only calm-looking states : forcing arbitrary ones (e.g. state 0) crashes,
		// they expect context set up by the boss fight.
		static const char* const CANDIDATES[] = { "idle left", "idle right", "rest left", "rest right" };
		static const int NUM_CANDIDATES = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);
		static int choice = -1;   // -1 = automatic (idle / run from movement).

		EBeastStateMachine* sm = g_cur->beast->GetStateMachine();
		choice++;
		if ( choice >= NUM_CANDIDATES ) choice = -1;

		g_autoLocomotion = (choice < 0);
		g_cur->forcedState = (choice >= 0) ? FindStateIndex( g_cur->beast, CANDIDATES[choice] ) : g_cur->idleState;

		if ( choice < 0 ) {
			mp_log( "[MP] Puppet animation : automatic (idle / run).\n" );
		}
		else if ( g_cur->forcedState < 0 ) {
			mp_log( "[MP] Puppet state \"%s\" not found.\n", CANDIDATES[choice] );
		}
		else {
			mp_log( "[MP] Puppet forced state : %d (%s)\n", g_cur->forcedState, sm->m_states[g_cur->forcedState].m_name );
		}
	}

	if ( f7 ) {
		g_delayIndex = (g_delayIndex + 1) % (sizeof(DELAY_CHOICES) / sizeof(DELAY_CHOICES[0]));
		mp_log( "[MP] Playback delay : %d ms.\n", DELAY_CHOICES[g_delayIndex] );
	}

	// F8 : Evil Harry stays here (spawning him where Harry stands if needed) and
	// keeps copying Harry in place; again : he follows Harry again.
	if ( f8 ) {
		if ( !g_puppetStaysHere ) {
			Vector3f here = harry->m_position;
			if ( !g_cur->beast ) SpawnPuppet( harry );
			else here = g_cur->beast->EIBeast::m_position;
			if ( g_cur->beast ) {
				g_puppetPin = here;
				g_puppetPinned = g_puppetStaysHere = true;
				mp_log( "[MP] Evil Harry stays at (%.1f, %.1f, %.1f) and copies Harry there.\n", here.x, here.y, here.z );
			}
		}
		else {
			g_puppetPinned = g_puppetStaysHere = false;
			mp_log( "[MP] Evil Harry follows Harry again.\n" );
		}
	}
}



// --------------------------------------------------------------------------------
// Autotest mode (environment variable PTLE_MP_AUTOTEST=1, set by
// run-autotest.ps1). Presses Enter through the menus, auto-skips cutscenes,
// then runs a scripted test on the game thread, takes screenshots into
// logs\shots\ and exits the game. Normal launches are unaffected.
//
// It runs in the background : the game never takes focus, its keys come from the
// test (fed into the game's keyboard poll) and screenshots come from the window
// itself, so the computer stays usable meanwhile. PTLE_MP_FOREGROUND=1 instead
// brings the game to the front and sends real key presses (the old way).
// --------------------------------------------------------------------------------
GET_METHOD( 0x4B55F0, void, EIHarry_Teleport, EIHarry*, Matrix4f*, bool, float );

GET_METHOD( 0x5EBA90, void, ScheduleWorldLoad, void*, uint32_t, bool );
// Level to test in, or 0 to stay wherever the game starts. 0x7567D75A is the
// developers' "0-sound" (Sound Range Test Level) : flat and empty.
// Turtle Monument (0x239A2165) has hostile monkeys that get in the way.
static const uint32_t AUTOTEST_LEVEL = 0x7567D75A;
static volatile LONG g_settled = 0;           // 1 = cutscene/book dismissed, game runs.

static bool g_background = false;            // Autotest without focus (see above).
static volatile uint8_t g_fakeKeys[256];      // Background : the keys the test holds (DirectInput codes).
static bool g_skipPuppetAI = false;           // true = don't run Evil Harry's own tick (no animation).
static volatile LONG g_autotestDone = 0;      // 1 = pass, 2 = fail.
static volatile LONG g_snapPending = 0;
static volatile LONG g_windowFailures = 0;    // Game window checks (always on top).
static volatile LONG g_holdKey = 0;           // Virtual key the autotest thread should hold...
static volatile LONG g_holdMs = 0;            // ...for this long.
static volatile LONG g_holdKey2 = 0;          // Second key held at the same time (e.g. crouch + walk).
static volatile LONG g_holdMs2 = 0;
static volatile LONG g_tapKey = 0;            // Virtual key the autotest thread should tap.
static char g_snapName[64];

static const DWORD AUTOTEST_MENU_TIMEOUT_MS  = 180000;
static DWORD AUTOTEST_TOTAL_TIMEOUT_MS = 360000;   // Longer for the level survey.

// Level survey (PTLE_MP_SURVEY=1) : instead of the scripted test, visit these levels
// and spawn Evil Harry beside Harry in each, to see where his model loads and draws.
static bool g_survey = false;
static const struct { uint32_t crc; const char* name; } SURVEY_LEVELS[] = {
	{ 0x7A9B3870, "a55_jungle_trail" },
	{ 0xEE8F6900, "a03_crash_site" },
	{ 0x07ECCC35, "a05_mama_oullo_tower" },
	{ 0x05AA726C, "b05_native_village" },
	{ 0x72AD42FA, "b04_st_claire_night" },
	{ 0x0C1C3E47, "b09_renegade_fort" },
	{ 0x8147EA91, "s06_copacanti_lake" },
	{ 0x1553BBE1, "s50_penguin_den" },
	{ 0x907C492B, "c53_mountain_overlook" },
	{ 0xEA667977, "c07_abandoned_cavern" },
	{ 0x93F89D45, "a53_gates_of_el_dorado" },
	{ 0x0081082C, "a01_chameleon_temple" },
};
static const int NUM_SURVEY_LEVELS = sizeof(SURVEY_LEVELS) / sizeof(SURVEY_LEVELS[0]);
static int g_surveyLoaded = 0, g_surveyDrawn = 0;

static void RequestSnapshot( const char* name )
{
	strcpy_s( g_snapName, name );
	InterlockedExchange( &g_snapPending, 1 );
}

// Writes a 24-bit bottom-up image to logs\shots\<name>.bmp.
static void WriteBmp( const char* name, int w, int h, const void* bits )
{
	char path[MAX_PATH];
	sprintf_s( path, "%s\\shots", g_logDir );
	CreateDirectoryA( path, 0 );
	sprintf_s( path, "%s\\shots\\%s.bmp", g_logDir, name );

	BITMAPINFOHEADER ih; ZeroMemory( &ih, sizeof(ih) );
	ih.biSize = sizeof(BITMAPINFOHEADER);
	ih.biWidth = w;
	ih.biHeight = h;   // Bottom-up, as BMP files expect.
	ih.biPlanes = 1;
	ih.biBitCount = 24;
	ih.biCompression = BI_RGB;
	DWORD imageSize = (((w * 3) + 3) & ~3) * h;
	BITMAPFILEHEADER fh; ZeroMemory( &fh, sizeof(fh) );
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
	fh.bfSize = fh.bfOffBits + imageSize;

	FILE* f = 0;
	if ( fopen_s( &f, path, "wb" ) == 0 && f ) {
		fwrite( &fh, sizeof(fh), 1, f );
		fwrite( &ih, sizeof(ih), 1, f );
		fwrite( bits, imageSize, 1, f );
		fclose( f );
		mp_log( "[AT] Screenshot : shots\\%s.bmp\n", name );
	}
}

// Foreground : grabs the game window's client area from the screen.
static void GrabScreen( const char* name )
{
	HWND wnd = *((HWND*) HWND_GAME_WINDOW);
	RECT rc;
	GetClientRect( wnd, &rc );
	POINT origin = { 0, 0 };
	ClientToScreen( wnd, &origin );
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	if ( w <= 0 || h <= 0 ) return;

	HDC screen = GetDC( 0 );
	HDC mem = CreateCompatibleDC( screen );
	BITMAPINFO bi; ZeroMemory( &bi, sizeof(bi) );
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 24;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = 0;
	HBITMAP dib = CreateDIBSection( screen, &bi, DIB_RGB_COLORS, &bits, 0, 0 );
	HGDIOBJ old = SelectObject( mem, dib );
	BitBlt( mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT );
	GdiFlush();
	WriteBmp( name, w, h, bits );
	SelectObject( mem, old );
	DeleteObject( dib );
	DeleteDC( mem );
	ReleaseDC( 0, screen );
}

// Background : the window may be covered (and its Direct3D 9 output isn't in what
// Windows can capture of it), so the frame is copied from the back buffer right
// before the game presents it. The renderer is a static object at 0x926790 with
// its IDirect3DDevice9 at +0x74 (filled by CreateDevice at 0x697FB2).
static const int VTABLE_SLOT_PRESENT = 17;   // IDirect3DDevice9::Present.
typedef HRESULT (__stdcall *PresentFunc)( IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA* );
static PresentFunc g_origPresent = 0;
static volatile LONG g_grabRequested = 0;
static char g_grabName[64];
static HANDLE g_grabDone = 0;

static void GrabBackBuffer( IDirect3DDevice9* dev )
{
	IDirect3DSurface9* back = 0;
	IDirect3DSurface9* resolved = 0;
	IDirect3DSurface9* copy = 0;
	if ( FAILED( dev->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &back ) ) ) { mp_log( "[AT] Screenshot : no back buffer.\n" ); return; }
	D3DSURFACE_DESC d;
	back->GetDesc( &d );
	IDirect3DSurface9* source = back;
	if ( d.MultiSampleType != D3DMULTISAMPLE_NONE
	  && SUCCEEDED( dev->CreateRenderTarget( d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, 0 ) )
	  && SUCCEEDED( dev->StretchRect( back, 0, resolved, 0, D3DTEXF_NONE ) ) ) {
		source = resolved;
	}
	D3DLOCKED_RECT lr;
	if ( SUCCEEDED( dev->CreateOffscreenPlainSurface( d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &copy, 0 ) )
	  && SUCCEEDED( dev->GetRenderTargetData( source, copy ) )
	  && SUCCEEDED( copy->LockRect( &lr, 0, D3DLOCK_READONLY ) ) ) {
		int w = d.Width, h = d.Height;
		int stride = ((w * 3) + 3) & ~3;
		uint8_t* bits = (uint8_t*) calloc( stride, h );
		for ( int y = 0; bits && y < h; y++ ) {
			const uint8_t* src = (const uint8_t*) lr.pBits + y * lr.Pitch;
			uint8_t* dst = bits + (h - 1 - y) * stride;
			for ( int x = 0; x < w; x++, dst += 3 ) {
				if ( d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8 ) {
					dst[0] = src[x * 4]; dst[1] = src[x * 4 + 1]; dst[2] = src[x * 4 + 2];
				}
				else {   // 16-bit : R5G6B5, or X1R5G5B5.
					uint16_t v = ((const uint16_t*) src)[x];
					bool g6 = d.Format == D3DFMT_R5G6B5;
					dst[0] = (uint8_t) ((v & 0x1F) << 3);
					dst[1] = (uint8_t) (g6 ? ((v >> 5) & 0x3F) << 2 : ((v >> 5) & 0x1F) << 3);
					dst[2] = (uint8_t) (g6 ? ((v >> 11) & 0x1F) << 3 : ((v >> 10) & 0x1F) << 3);
				}
			}
		}
		if ( bits ) WriteBmp( g_grabName, w, h, bits );
		free( bits );
		copy->UnlockRect();
	}
	else {
		mp_log( "[AT] Screenshot : couldn't read the back buffer (format %d).\n", d.Format );
	}
	if ( copy ) copy->Release();
	if ( resolved ) resolved->Release();
	back->Release();
}

static void DrawOverlay( IDirect3DDevice9* dev );

static HRESULT __stdcall Present_Hook( IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty )
{
	// Loading, menus, pause : Harry doesn't tick, but the room still hears from us
	// (and we from it).
	if ( NetNowMs() - g_lastNetUpdate > 250 ) NetUpdate();
	DrawOverlay( dev );
	g_presentCount++;
	if ( g_grabRequested ) {
		GrabBackBuffer( dev );
		InterlockedExchange( &g_grabRequested, 0 );
		SetEvent( g_grabDone );
	}
	return g_origPresent( dev, src, dst, wnd, dirty );
}

// Hooks the device's Present once it exists.
static bool HookPresent()
{
	if ( g_origPresent ) return true;
	IDirect3DDevice9* dev = *(IDirect3DDevice9**) RENDERER_DEVICE;
	HMODULE d3d9 = GetModuleHandleA( "d3d9.dll" );
	if ( !dev || !d3d9 ) return false;
	// Windows' d3d9.dll gives each device its own copy of the vtable (heap memory),
	// so the check is on Present itself : it must be code in a loaded module.
	void** vtable = *(void***) dev;
	MEMORY_BASIC_INFORMATION mbi;
	if ( !VirtualQuery( vtable, &mbi, sizeof(mbi) ) || mbi.State != MEM_COMMIT
	  || !VirtualQuery( vtable[VTABLE_SLOT_PRESENT], &mbi, sizeof(mbi) ) || mbi.Type != MEM_IMAGE ) {
		mp_log( "[AT] The renderer's device (0x%.8X) doesn't look like a Direct3D 9 device.\n", (uint32_t) dev );
		return false;
	}
	char module[MAX_PATH] = "?";
	GetModuleFileNameA( (HMODULE) mbi.AllocationBase, module, MAX_PATH );
	mp_log( "[AT] Direct3D device 0x%.8X, Present in %s.\n", (uint32_t) dev, module );
	g_grabDone = CreateEventA( 0, FALSE, FALSE, 0 );
	g_origPresent = (PresentFunc) vtable[VTABLE_SLOT_PRESENT];
	injector::WriteMemory( &vtable[VTABLE_SLOT_PRESENT], &Present_Hook, true );
	mp_log( "[AT] Screenshots come from the Direct3D back buffer.\n" );
	return true;
}

// Saves the game's picture into logs\shots\<name>.bmp.
static void TakeSnapshot( const char* name )
{
	if ( !*((HWND*) HWND_GAME_WINDOW) || !g_logDir[0] ) return;
	if ( !g_background ) { GrabScreen( name ); return; }
	if ( !HookPresent() ) { mp_log( "[AT] Screenshot %s skipped : no Direct3D device yet.\n", name ); return; }
	strcpy_s( g_grabName, name );
	ResetEvent( g_grabDone );
	InterlockedExchange( &g_grabRequested, 1 );
	if ( WaitForSingleObject( g_grabDone, 3000 ) != WAIT_OBJECT_0 ) {
		InterlockedExchange( &g_grabRequested, 0 );
		mp_log( "[AT] Screenshot %s skipped : no frame presented.\n", name );
	}
}

// DirectInput key code of a virtual key (the arrows are "extended" keys).
static uint8_t KeyToDIK( WORD vk )
{
	switch ( vk ) {
	case VK_UP:    return 0xC8;
	case VK_DOWN:  return 0xD0;
	case VK_LEFT:  return 0xCB;
	case VK_RIGHT: return 0xCD;
	}
	return (uint8_t) MapVirtualKeyA( vk, MAPVK_VK_TO_VSC );
}

// The game polls the keyboard (0x6E8A80, called from 0x663BBE) through DirectInput
// in foreground mode : the keys down now at 0x940EC0, a frame earlier at 0x940C90
// (all up while the window isn't in front). In background autotests the keys come
// from the test instead, whatever window is in front.
static const uint32_t KEYS_NOW = 0x940EC0, KEYS_BEFORE = 0x940C90;
static const uint32_t POLL_KEYBOARD = 0x6E8A80, POLL_KEYBOARD_CALL = 0x663BBE;

static void PollKeyboard_Hook()
{
	uint8_t before[256];
	memcpy( before, (void*) KEYS_NOW, sizeof(before) );
	((void (*)()) POLL_KEYBOARD)();
	memcpy( (void*) KEYS_BEFORE, before, sizeof(before) );   // Not cleared : held keys stay held.
	for ( int i = 0; i < 256; i++ ) ((uint8_t*) KEYS_NOW)[i] = g_fakeKeys[i];
}

// The game window shows up without being activated or brought forward : its
// startup SetFocus and SetForegroundWindow calls just drop their argument,
// SetWindowPos gets SWP_NOACTIVATE and ShowWindow SW_SHOWNOACTIVATE (same for the
// SetFocus + ShowWindow pair at 0x698383). Its SetWindowPos( HWND_TOPMOST ) call
// (0x60D68F) puts it at the bottom of the window stack instead, so it never even
// shows in front of other windows.
static BOOL __stdcall SetWindowPos_Bottom( HWND wnd, HWND, int x, int y, int cx, int cy, UINT flags )
{
	return SetWindowPos( wnd, HWND_BOTTOM, x, y, cx, cy, flags | SWP_NOACTIVATE );
}

static void PatchWindowNoActivate()
{
	injector::MakeCALL( 0x60D68F, &SetWindowPos_Bottom, true );
	injector::MakeNOP( 0x60D694, 1, true );
	static const uint8_t POP_ARG[6] = { 0x83, 0xC4, 0x04, 0x90, 0x90, 0x90 };   // add esp, 4
	injector::WriteMemoryRaw( 0x60D660, (void*) POP_ARG, sizeof(POP_ARG), true );   // SetFocus.
	injector::WriteMemoryRaw( 0x60D66D, (void*) POP_ARG, sizeof(POP_ARG), true );   // SetForegroundWindow.
	injector::WriteMemory<uint8_t>( 0x60D674, SWP_NOACTIVATE, true );
	injector::WriteMemory<uint8_t>( 0x60D6AA, SW_SHOWNOACTIVATE, true );
	injector::WriteMemory<uint8_t>( 0x60D180, SW_SHOWNOACTIVATE, true );
	injector::WriteMemoryRaw( 0x698383, (void*) POP_ARG, sizeof(POP_ARG), true );   // SetFocus.
	injector::WriteMemory<uint8_t>( 0x69838A, SW_SHOWNOACTIVATE, true );
}

static void SendKey( WORD vk, bool down )
{
	if ( g_background ) { g_fakeKeys[KeyToDIK( vk )] = down ? 0x80 : 0; return; }
	INPUT in; ZeroMemory( &in, sizeof(in) );
	in.type = INPUT_KEYBOARD;
	in.ki.wScan = (WORD) MapVirtualKeyA( vk, MAPVK_VK_TO_VSC );
	in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
	if ( vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
	SendInput( 1, &in, sizeof(INPUT) );
}

static void TapKey( WORD vk )
{
	if ( g_background ) {
		SendKey( vk, true );
		Sleep( 100 );
		SendKey( vk, false );
		return;
	}
	INPUT in[2]; ZeroMemory( in, sizeof(in) );
	WORD scan = (WORD) MapVirtualKeyA( vk, MAPVK_VK_TO_VSC );
	in[0].type = INPUT_KEYBOARD;
	in[0].ki.wScan = scan;
	DWORD ext = (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) ? KEYEVENTF_EXTENDEDKEY : 0;
	in[0].ki.dwFlags = KEYEVENTF_SCANCODE | ext;
	in[1] = in[0];
	in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | ext;

	SendInput( 1, &in[0], sizeof(INPUT) );
	Sleep( 100 );
	SendInput( 1, &in[1], sizeof(INPUT) );
}

// Same patch as CheatLab's "Auto Skip Cutscene".
static void EnableAutoSkipCutscenes()
{
	static float skipTime = 0.1F;
	injector::MakeJMP( 0x42FE93, 0x42FF45 );
	injector::WriteMemory( 0x42FE84, &skipTime, true );
}

// Brings the game back in front (a click elsewhere steals focus mid-test) and
// waits up to a second for it. Keys are only ever sent to the game itself.
static bool EnsureGameFocus( HWND wnd )
{
	if ( g_background ) return true;   // Keys reach the game whatever is in front.
	for ( int i = 0; i < 10 && !GameHasFocus(); i++ ) {
		// Windows ignores SetForegroundWindow from a background thread unless it
		// shares input with the current foreground window, so attach briefly.
		DWORD fgThread = GetWindowThreadProcessId( GetForegroundWindow(), 0 );
		DWORD me = GetCurrentThreadId();
		bool attached = fgThread && fgThread != me && AttachThreadInput( me, fgThread, TRUE );
		BringWindowToTop( wnd );
		SetForegroundWindow( wnd );
		if ( attached ) AttachThreadInput( me, fgThread, FALSE );
		Sleep( 100 );
	}
	return GameHasFocus();
}

static DWORD WINAPI AutotestThread( void* )
{
	DWORD start = GetTickCount();
	mp_log( "[AT] Autotest mode on.\n" );

	HWND wnd = 0;
	while ( !(wnd = *((HWND*) HWND_GAME_WINDOW)) || (g_background && !IsWindowVisible( wnd )) ) {
		if ( GetTickCount() - start > 60000 ) {
			mp_log( "[AT] FAIL : game window never appeared.\n" );
			QuitGame( 2 );
		}
		Sleep( 200 );
	}
	if ( g_background ) {
		// Behind every other window, without activating it : it keeps rendering.
		SetWindowPos( wnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE );
		mp_log( "[AT] Running in the background (game window behind the others, keys fed to it directly).\n" );
	}
	Sleep( 3000 );

	// Menus : keep pressing Enter until Harry starts ticking.
	int presses = 0;
	while ( g_harryTicks == 0 ) {
		if ( GetTickCount() - start > AUTOTEST_MENU_TIMEOUT_MS ) {
			TakeSnapshot( "fail_menu_timeout" );
			mp_log( "[AT] FAIL : never reached gameplay after %d Enter presses.\n", presses );
			QuitGame( 2 );
		}
		if ( presses % 2 == 0 ) {
			char name[32];
			sprintf_s( name, "menu_%02d", presses );
			TakeSnapshot( name );
		}
		if ( EnsureGameFocus( wnd ) ) {
			// Title screen wants Esc ("Press [Esc] to Begin"), menus want Enter.
			// Esc again every 10 presses in case we fell back to the title.
			WORD key = (presses % 10 == 0) ? VK_ESCAPE : VK_RETURN;
			TapKey( key );
			mp_log( "[AT] Pressed %s (#%d).\n", key == VK_ESCAPE ? "Esc" : "Enter", presses );
			presses++;
		}
		else if ( presses % 5 == 0 ) {
			// Never inject keys into another application.
			mp_log( "[AT] Game isn't the active window, not pressing Enter (click the game window).\n" );
		}
		Sleep( 1500 );
	}
	mp_log( "[AT] Gameplay reached after %d Enter presses (%.1fs).\n", presses, (GetTickCount() - start) / 1000.0F );

	// Make sure the game runs freely before the script starts : while a menu or the
	// journal is open Harry doesn't tick, so a stalled tick count means press Esc.
	// (The randomizer's start area plays no cutscene; others are auto-skipped.)
	Sleep( 3000 );
	for ( int attempt = 0; ; attempt++ ) {
		LONG t0 = g_harryTicks;
		Sleep( 1000 );
		LONG rate = g_harryTicks - t0;
		mp_log( "[AT] Harry ticks in the last second : %ld.\n", rate );
		if ( rate >= 30 ) break;
		if ( attempt >= 5 ) {
			TakeSnapshot( "fail_not_settled" );
			mp_log( "[AT] FAIL : game still not running freely.\n" );
			QuitGame( 2 );
		}
		char name[32];
		sprintf_s( name, "settle_stalled_%d", attempt );
		TakeSnapshot( name );
		if ( g_background || GameHasFocus() ) { TapKey( VK_ESCAPE ); mp_log( "[AT] Game looks paused, pressed Esc.\n" ); }
		Sleep( 1500 );
	}
	TakeSnapshot( "settle_2_free" );
	InterlockedExchange( &g_settled, 1 );
	mp_log( "[AT] Game runs freely, starting the test script.\n" );
	// The WindowedMode mod keeps the game from staying above every other window.
	bool topmost = (GetWindowLongA( wnd, GWL_EXSTYLE ) & WS_EX_TOPMOST) != 0;
	mp_log( "[AT] Game window always on top : %s.\n", topmost ? "yes (WRONG)" : "no (ok)" );
	if ( topmost ) InterlockedIncrement( &g_windowFailures );

	// The scripted part runs on the game thread (AutotestStep). Service screenshots
	// and key holds.
	WORD heldKey = 0, heldKey2 = 0;
	DWORD releaseAt = 0, releaseAt2 = 0;
	while ( g_autotestDone == 0 ) {
		LONG tap = InterlockedExchange( &g_tapKey, 0 );
		if ( tap ) {
			if ( EnsureGameFocus( wnd ) ) TapKey( (WORD) tap );
			else mp_log( "[AT] Game isn't the active window, not tapping key 0x%X.\n", tap );
		}

		LONG k = InterlockedExchange( &g_holdKey, 0 );
		if ( k && !heldKey ) {
			if ( EnsureGameFocus( wnd ) ) {
				SendKey( (WORD) k, true );
				heldKey = (WORD) k;
				releaseAt = GetTickCount() + g_holdMs;
			}
			else {
				mp_log( "[AT] Game isn't the active window, not holding key 0x%X.\n", k );
			}
		}
		if ( heldKey && (GetTickCount() >= releaseAt || (!g_background && !GameHasFocus())) ) {
			SendKey( heldKey, false );
			heldKey = 0;
		}

		LONG k2 = InterlockedExchange( &g_holdKey2, 0 );
		if ( k2 && !heldKey2 && EnsureGameFocus( wnd ) ) {
			SendKey( (WORD) k2, true );
			heldKey2 = (WORD) k2;
			releaseAt2 = GetTickCount() + g_holdMs2;
		}
		if ( heldKey2 && (GetTickCount() >= releaseAt2 || (!g_background && !GameHasFocus())) ) {
			SendKey( heldKey2, false );
			heldKey2 = 0;
		}

		if ( GetTickCount() - start > AUTOTEST_TOTAL_TIMEOUT_MS ) {
			TakeSnapshot( "fail_total_timeout" );
			mp_log( "[AT] FAIL : test script timed out.\n" );
			QuitGame( 2 );
		}
		if ( InterlockedExchange( &g_snapPending, 0 ) ) {
			TakeSnapshot( g_snapName );
		}
		Sleep( 30 );
	}

	if ( heldKey ) SendKey( heldKey, false );
	if ( InterlockedExchange( &g_snapPending, 0 ) ) TakeSnapshot( g_snapName );
	LogNetStats();
	mp_log( "[AT] %s\n", g_autotestDone == 1 ? "RESULT : PASS" : "RESULT : FAIL" );
	Sleep( 300 );
	QuitGame( g_autotestDone == 1 ? 0 : 2 );
	return 0;
}

static void LogTelemetry( EIHarry* harry, const char* tag )
{
	if ( g_cur->beast ) {
		const Matrix4f& w = FIELD( g_cur->beast, Matrix4f, BEAST_WORLD );
		mp_log( "[AT] %s beast world (%.1f, %.1f, %.1f), anim layer %d, moving %d, speed %.3f, heading %.0f\n", tag,
			w.data[3][0], w.data[3][1], w.data[3][2], FIELD( g_cur->beast, int, BEAST_CUR_LAYER ),
			(int) g_cur->moving, g_cur->moveSpeed, g_cur->heading * 180.0F / MP_PI );
		const Vector3f& r = FIELD( g_cur->beast, Vector3f, BEAST_SCALE );
		mp_log( "[AT] %s harry (%.1f, %.1f, %.1f) puppet (%.1f, %.1f, %.1f) scale (%.2f, %.2f, %.2f) matrix (%.1f, %.1f, %.1f) state %d\n", tag,
			harry->m_position.x, harry->m_position.y, harry->m_position.z,
			g_cur->beast->EICharacter::m_position.x, g_cur->beast->EICharacter::m_position.y, g_cur->beast->EICharacter::m_position.z,
			r.x, r.y, r.z,
			g_cur->beast->m_transformMatrix.data[3][0], g_cur->beast->m_transformMatrix.data[3][1], g_cur->beast->m_transformMatrix.data[3][2],
			g_cur->beast->m_stateIndexCurrent );
	}
	else {
		mp_log( "[AT] %s harry (%.1f, %.1f, %.1f), no puppet\n", tag, harry->m_position.x, harry->m_position.y, harry->m_position.z );
	}
}

// Runs every Harry tick on the game thread while in autotest mode.
// Gives Harry the raft, pickaxes, torch and TNT (like CheatLab's "give stuff").
static void GiveTestItems( EIHarry* harry )
{
	void** wheel = harry->m_itemHotbar;
	if ( !wheel ) return;
	wheel[0] = GetItemObject( ITEM_RAFT );       // Up.
	wheel[1] = GetItemObject( ITEM_PICKAXES );   // Right.
	wheel[2] = GetItemObject( ITEM_TORCH );      // Down.
	wheel[3] = GetItemObject( ITEM_TNT );        // Left.
	for ( int i = 0; i < 8; i++ ) FIELD( ITEM_TABLE + i * ITEM_ENTRY_SIZE, int, 0x14 ) = 1;   // Unlocked.
	mp_log( "[AT] Gave raft, pickaxes, torch, TNT.\n" );
}

static int g_itemFailures = 0;

static void AutotestStep( EIHarry* harry )
{
	static const LONG WALK_KEYS[] = { 'W', 'A', 'S', 'D' };   // Arrow keys don't move Harry.
	static const char* const WALK_KEY_NAMES[] = { "w", "a", "s", "d" };
	static const int NUM_WALK_KEYS = 4;
	static const LONG WALK_HOLD_MS = 1500;
	static const char* const AIR_CANDIDATES[] = { "falling", "off screen drop", "knocked back" };
	static const int  JUMP_CLIP[]    = { 0, 1, 2, 0 };
	static const bool JUMP_RUNNING[] = { false, false, false, true };
	static const int  NUM_JUMPS = 4;
	static bool jumped = false;
	static const float CIRCLE_SPEEDS[] = { 0.30F, 0.65F };   // Run, then sprint (units per tick).
	static const int NUM_CIRCLE_SPEEDS = 2;
	static const char* const IDLE_CANDIDATES[] = { 0, "rest left", "rest right", "i win", "gloat hit", "admire hit" };
	static const int NUM_IDLE_CANDIDATES = 6;   // Phase 4 (idle tour) is no longer scheduled.
	static const float CIRCLE_RADIUS = 8.0F;
	static float circleAngle = 0.0F;
	static DWORD lastWalkLog = 0;
	static Vector3f lastHarryPos;
	static LONG lastHarryTicks = 0;

	static int phase = -1;
	static uint32_t lastLevel = 0;
	static DWORD phaseStart = 0, lastTelemetry = 0, firstTick = 0;
	static int step = 0;
	static bool snapped = false;
	static int ticksThisSecond = 0;
	static DWORD secondStart = 0;
	static Matrix4f base;

	if ( g_autotestDone ) return;

	DWORD now = GetTickCount();
	if ( !firstTick ) { firstTick = now; secondStart = now; phaseStart = now; }

	// Tick rate, logged once.
	if ( secondStart ) {
		ticksThisSecond++;
		if ( now - secondStart >= 1000 ) {
			mp_log( "[AT] Harry ticks per second : %d\n", ticksThisSecond );
			secondStart = 0;
		}
	}

	if ( g_cur->beast && now - lastTelemetry >= 500 ) {
		LogTelemetry( harry, "tele" );
		lastTelemetry = now;
	}

	if ( g_survey && phase == -1 && g_settled ) { phase = 100; phaseStart = now; step = 0; }

	switch ( phase )
	{
	case 100: {   // Survey : go to the next level.
		if ( now - phaseStart < 3000 ) break;
		if ( step >= NUM_SURVEY_LEVELS ) {
			mp_log( "[AT] SURVEY : Evil Harry's model loaded in %d of %d levels, drawn in %d.\n", g_surveyLoaded, NUM_SURVEY_LEVELS, g_surveyDrawn );
			InterlockedExchange( &g_autotestDone, 1 );
			break;
		}
		mp_log( "[AT] SURVEY : going to %s (0x%.8X).\n", SURVEY_LEVELS[step].name, SURVEY_LEVELS[step].crc );
		ScheduleWorldLoad( (void*) 0x917028, SURVEY_LEVELS[step].crc, false );
		phase = 101; phaseStart = now;
		break;
	}
	case 101:   // Survey : wait for the level, let it settle, spawn.
		if ( Gizmod::getCurrentLevelCRC() != SURVEY_LEVELS[step].crc ) {
			if ( now - phaseStart > 60000 ) {
				mp_log( "[AT] SURVEY %s : FAILED TO LOAD (still in 0x%.8X).\n", SURVEY_LEVELS[step].name, Gizmod::getCurrentLevelCRC() );
				step++; phase = 100; phaseStart = now;
			}
			break;
		}
		if ( !snapped ) { snapped = true; phaseStart = now; break; }   // Arrived : settle from now.
		if ( now - phaseStart < 10000 ) break;
		SpawnPuppet( harry );
		phase = 102; phaseStart = now;
		lastHarryTicks = g_puppetPoseCopies;
		break;
	case 102:   // Survey : look.
		if ( now - phaseStart < 3000 ) break;
		{
			bool loaded = g_cur->beast && g_cur->beast->m_beastType;
			int draws = g_puppetPoseCopies - (int) lastHarryTicks;
			if ( loaded ) g_surveyLoaded++;
			if ( draws > 0 ) g_surveyDrawn++;
			mp_log( "[AT] SURVEY %s : spawned %s, model %s, drawn %d times in 3 s.\n", SURVEY_LEVELS[step].name,
				g_cur->beast ? "yes" : "NO", loaded ? "loaded" : "MISSING", draws );
			char shot[64];
			sprintf_s( shot, "survey_%02d_%s", step, SURVEY_LEVELS[step].name );
			RequestSnapshot( shot );
		}
		step++; phase = 100; phaseStart = now; snapped = false;
		break;

	case -1:  // Go to an open level (same call as the Gizmod "Go to" menu).
		if ( !g_settled ) { phaseStart = now; break; }
		if ( AUTOTEST_LEVEL == 0 ) {
			mp_log( "[AT] Testing in the start level 0x%.8X.\n", Gizmod::getCurrentLevelCRC() );
			phase = 0; phaseStart = now - 5000;   // Already settled : shorter wait.
			break;
		}
		if ( now - phaseStart < 3000 ) break;
		mp_log( "[AT] Going to level 0x%.8X (from level 0x%.8X).\n", AUTOTEST_LEVEL, Gizmod::getCurrentLevelCRC() );
		ScheduleWorldLoad( (void*) 0x917028, AUTOTEST_LEVEL, false );
		phase = -2; phaseStart = now; lastLevel = Gizmod::getCurrentLevelCRC();
		break;

	case -2:  // Wait for it to load.
		if ( Gizmod::getCurrentLevelCRC() == AUTOTEST_LEVEL ) {
			mp_log( "[AT] Arrived in level 0x%.8X.\n", AUTOTEST_LEVEL );
			phase = 0; phaseStart = now;
		}
		else if ( now - phaseStart > 60000 ) {
			mp_log( "[AT] FAIL : still in level 0x%.8X after 60s.\n", Gizmod::getCurrentLevelCRC() );
			InterlockedExchange( &g_autotestDone, 2 );
		}
		break;

	case 0:   // Let the level settle (cutscenes are auto-skipped).
		if ( now - phaseStart < 8000 ) break;
		LogTelemetry( harry, "before spawn" );
		RequestSnapshot( "01_before_spawn" );
		base = harry->m_rigidBodyMatrix;
		g_skipPuppetAI = false;
		mp_log( "[AT] Hop test runs with the puppet's AI tick ENABLED (animation test).\n" );
		SpawnPuppet( harry );
		if ( !g_cur->beast ) {
			mp_log( "[AT] FAIL : puppet did not spawn.\n" );
			InterlockedExchange( &g_autotestDone, 2 );
			return;
		}
		phase = 7; phaseStart = now; step = 0; snapped = false; circleAngle = 0.0F;   // Roll, collision (open space), circle, walk, jumps, items.
		break;

	case 1:   // Walk Harry with the keyboard; the puppet replays it ~1s later.
		if ( now - phaseStart >= 500 && !snapped && step == 0 ) {
			RequestSnapshot( "02_after_spawn" );
			snapped = true;
		}
		if ( now - phaseStart >= 1000 && step == 0 ) {
			step = 1; phaseStart = now; snapped = false;
			InterlockedExchange( &g_holdMs, WALK_HOLD_MS );
			InterlockedExchange( &g_holdKey, WALK_KEYS[0] );
			mp_log( "[AT] Walk 0 : holding %s.\n", WALK_KEY_NAMES[0] );
		}
		else if ( step > 0 ) {
			int k = step - 1;
			if ( now - lastWalkLog >= 100 ) {
				// Harry's speed and direction vs his matrices : calibrates the forward axis.
				float dx = harry->m_position.x - lastHarryPos.x, dy = harry->m_position.y - lastHarryPos.y;
				float ticks = (float) (g_harryTicks - lastHarryTicks);
				if ( ticks > 0 && dx * dx + dy * dy > 0.0001F ) {
					const Matrix4f& m = harry->m_transformMatrix;
					const Matrix4f& rb = harry->m_rigidBodyMatrix;
					mp_log( "[AT] walk %s : speed %.3f/tick, move %.0f deg, transform row0 %.0f row1 %.0f, rigidbody row0 %.0f row1 %.0f, euler (%.2f, %.2f, %.2f)\n",
						WALK_KEY_NAMES[k], sqrtf( dx * dx + dy * dy ) / ticks, atan2f( dy, dx ) * 180.0F / MP_PI,
						atan2f( m.data[0][1], m.data[0][0] ) * 180.0F / MP_PI, atan2f( m.data[1][1], m.data[1][0] ) * 180.0F / MP_PI,
						atan2f( rb.data[0][1], rb.data[0][0] ) * 180.0F / MP_PI, atan2f( rb.data[1][1], rb.data[1][0] ) * 180.0F / MP_PI,
						harry->m_rotationEuler.x, harry->m_rotationEuler.y, harry->m_rotationEuler.z );
				}
				lastHarryPos = harry->m_position; lastHarryTicks = g_harryTicks; lastWalkLog = now;
			}
			if ( !snapped && now - phaseStart >= 1300 ) {
				char name[32];
				sprintf_s( name, "03_walk_%d_%s", k, WALK_KEY_NAMES[k] );
				RequestSnapshot( name );   // Harry stopping, puppet still running behind.
				snapped = true;
			}
			if ( now - phaseStart >= 3500 ) {
				if ( step < NUM_WALK_KEYS ) {
					InterlockedExchange( &g_holdMs, WALK_HOLD_MS );
					InterlockedExchange( &g_holdKey, WALK_KEYS[step] );
					mp_log( "[AT] Walk %d : holding %s.\n", step, WALK_KEY_NAMES[step] );
					step++; phaseStart = now; snapped = false;
				}
				else {
					phase = 5; phaseStart = now; step = 0; snapped = false;
				}
			}
		}
		break;

	case 5:   // Jumps : standing jumps with each air-clip candidate, then a running jump.
	{
		// Harry's own ground flag, to check m_onGround is real.
		static bool harryWasOnGround = true;
		if ( harry->m_onGround != harryWasOnGround ) {
			mp_log( "[AT] Harry %s (z %.2f).\n", harry->m_onGround ? "is on the ground" : "is in the air", harry->m_position.z );
			harryWasOnGround = harry->m_onGround;
		}

		if ( step == 0 || now - phaseStart >= 4000 ) {
			if ( step >= NUM_JUMPS ) {
				SetAirAnimation( g_cur->beast, AIR_CANDIDATES[0] );
				phase = 8; phaseStart = now; step = 0; snapped = false;
				break;
			}
			SetAirAnimation( g_cur->beast, AIR_CANDIDATES[JUMP_CLIP[step]] );
			if ( JUMP_RUNNING[step] ) {
				InterlockedExchange( &g_holdMs, 2000 );
				InterlockedExchange( &g_holdKey, 'W' );
			}
			mp_log( "[AT] Jump %d : %s jump, air clip \"%s\".\n", step, JUMP_RUNNING[step] ? "running" : "standing", AIR_CANDIDATES[JUMP_CLIP[step]] );
			step++; phaseStart = now; snapped = false; jumped = false;
		}
		if ( !jumped && now - phaseStart >= (DWORD) (JUMP_RUNNING[step - 1] ? 500 : 0) ) {
			InterlockedExchange( &g_tapKey, VK_NUMPAD2 );   // Jump (registry binding k0_8 = scan code 80, numpad 2).
			jumped = true;
		}
		// The puppet replays ~1s behind : catch it mid-air.
		int ms = (int) (now - phaseStart) - (JUMP_RUNNING[step - 1] ? 500 : 0);
		if ( (!snapped && ms >= 1300) || (snapped && ms >= 1600 && ms < 1660) ) {
			char name[40];
			sprintf_s( name, "06_jump_%d_%c", step - 1, snapped ? 'b' : 'a' );
			RequestSnapshot( name );
			snapped = true;
		}
		break;
	}

	case 2:   // Circle the puppet around Harry : one lap at run speed, one at sprint speed.
		if ( step == 0 || now - phaseStart >= 4500 ) {
			if ( step >= NUM_CIRCLE_SPEEDS ) {
				g_puppetOffset.x = 6.0F; g_puppetOffset.y = 0.0F;
				mp_log( "[AT] Circle done, puppet stands still again.\n" );
				phase = 1; phaseStart = now; step = 0; snapped = false;
				break;
			}
			mp_log( "[AT] Circle %d at %.2f/tick (%s expected).\n", step, CIRCLE_SPEEDS[step],
				CIRCLE_SPEEDS[step] > SPRINT_START_SPEED ? "sprint" : "run" );
			step++; phaseStart = now; snapped = false;
		}
		circleAngle += CIRCLE_SPEEDS[step - 1] / CIRCLE_RADIUS;
		g_puppetOffset.x = CIRCLE_RADIUS * cosf( circleAngle );
		g_puppetOffset.y = CIRCLE_RADIUS * sinf( circleAngle );
		if ( (!snapped && now - phaseStart >= 2000) || (snapped && now - phaseStart >= 4000 && now - phaseStart < 4000 + 60) ) {
			char name[40];
			sprintf_s( name, "04_circle_%d_%c", step - 1, snapped ? 'b' : 'a' );
			RequestSnapshot( name );
			snapped = true;
		}
		break;

	case 4:   // Standing still beside Harry : show each idle candidate.
		if ( step == 0 || now - phaseStart >= 3000 ) {
			if ( step >= NUM_IDLE_CANDIDATES ) {
				SetIdleAnimation( g_cur->beast, 0 );
				phase = 1; phaseStart = now; step = 0; snapped = false;
				break;
			}
			mp_log( "[AT] Idle %d : %s.\n", step, IDLE_CANDIDATES[step] ? IDLE_CANDIDATES[step] : "quickdata idle anim" );
			SetIdleAnimation( g_cur->beast, IDLE_CANDIDATES[step] );
			step++; phaseStart = now; snapped = false;
		}
		else if ( !snapped && now - phaseStart >= 2500 ) {
			char name[40];
			sprintf_s( name, "05_idle_%d", step - 1 );
			RequestSnapshot( name );
			snapped = true;
		}
		break;

	case 10:  // F8 "stay here" : Evil Harry stays put but keeps copying Harry.
	{
		static Vector3f pinned;
		static float headingAtStart = 0.0F;
		static bool sawMirror = false;
		static float maxRise = 0.0F;
		DWORD t = now - phaseStart;
		if ( step == 0 ) {
			InterlockedIncrement( &g_reqStay );   // As if F8 was pressed.
			step = 1;
		}
		else if ( step == 1 && t >= 500 ) {
			if ( !g_puppetStaysHere ) { mp_log( "[AT] FAIL : F8 didn't pin Evil Harry.\n" ); g_itemFailures++; phase = 3; phaseStart = now; break; }
			pinned = g_cur->beast->EIBeast::m_position;
			headingAtStart = g_cur->heading;
			sawMirror = false;
			maxRise = 0.0F;
			InterlockedExchange( &g_holdMs, 2000 );
			InterlockedExchange( &g_holdKey, 'D' );   // Walk away, turning.
			step = 2;
		}
		else if ( step == 2 ) {
			if ( g_cur->mirrorClip && g_cur->mirrorClip != 0xB23CB6D0 ) sawMirror = true;   // Copying something besides standing.
			maxRise = max( maxRise, g_cur->beast->EIBeast::m_position.z - pinned.z );
			if ( t >= 2700 && t < 2750 ) InterlockedExchange( &g_tapKey, VK_NUMPAD2 );   // Jump : he should too, in place.
			if ( t >= 4500 ) {
				RequestSnapshot( "12_stay_here" );
				Vector3f p = g_cur->beast->EIBeast::m_position;
				float dx = p.x - pinned.x, dy = p.y - pinned.y, dh = harry->m_position.x - pinned.x, dhy = harry->m_position.y - pinned.y;
				float moved = sqrtf( dx * dx + dy * dy ), harryAway = sqrtf( dh * dh + dhy * dhy );
				float turned = fabsf( g_cur->heading - headingAtStart ) * 180.0F / MP_PI;
				bool ok = moved < 0.5F && harryAway > 3.0F && sawMirror && turned > 20.0F && maxRise > 0.5F;
				if ( !ok ) g_itemFailures++;
				mp_log( "[AT] Stay here : Evil Harry moved %.2f, Harry is %.1f away, puppet copied a clip %s, turned %.0f deg, jumped %.2f high : %s\n",
					moved, harryAway, sawMirror ? "yes" : "no", turned, maxRise, ok ? "ok" : "WRONG" );
				InterlockedIncrement( &g_reqStay );   // F8 again : follow.
				step = 3;
			}
		}
		else if ( step == 3 && t >= 5000 ) {
			bool ok = !g_puppetStaysHere && !g_puppetPinned;
			if ( !ok ) g_itemFailures++;
			mp_log( "[AT] F8 again : Evil Harry follows : %s\n", ok ? "ok" : "WRONG" );
			phase = 3; phaseStart = now;
		}
		break;
	}

	case 8:   // Items : hold numpad 5 and tap an arrow to take an item out or put it away.
	{
		// Evil Harry should hold, and move, what Harry did a second earlier. The wheel
		// starts as raft (up), pickaxes (right), torch (down), TNT (left); the other
		// items go on the up slot (like picking them in the wheel menu), then an empty
		// slot (the menu's hand). "move" steps hold an arrow alone : it moves the item
		// in hand (torch aside, shield over the head, mask on, drink).
		static const int KEEP = -2, EMPTY = -1;
		static const struct { LONG key; bool move; int upItem; int expect; const char* name; } STEPS[] = {
			{ 0,        false, KEEP,          -1,            "empty handed" },
			{ VK_RIGHT, false, KEEP,          ITEM_PICKAXES, "pickaxes out" },
			{ VK_RIGHT, false, KEEP,          -1,            "pickaxes away" },
			{ VK_DOWN,  false, KEEP,          ITEM_TORCH,    "torch out" },
			{ VK_LEFT,  true,  KEEP,          ITEM_TORCH,    "torch moved" },
			{ VK_DOWN,  false, KEEP,          -1,            "torch away" },
			{ VK_UP,    false, KEEP,          ITEM_RAFT,     "raft out" },
			{ VK_UP,    false, KEEP,          -1,            "raft away" },
			{ VK_LEFT,  false, KEEP,          ITEM_TNT,      "TNT out" },
			{ VK_LEFT,  false, KEEP,          -1,            "TNT away" },
			{ VK_UP,    false, ITEM_SLING,    ITEM_SLING,    "sling out" },
			{ VK_UP,    false, KEEP,          -1,            "sling away" },
			{ VK_UP,    false, ITEM_SHIELD,   ITEM_SHIELD,   "shield out" },
			{ VK_UP,    true,  KEEP,          ITEM_SHIELD,   "shield raised" },
			{ VK_UP,    false, KEEP,          -1,            "shield away" },
			{ VK_UP,    false, ITEM_GAS_MASK, ITEM_GAS_MASK, "gas mask out" },
			{ VK_UP,    true,  KEEP,          ITEM_GAS_MASK, "gas mask on" },
			{ VK_UP,    false, KEEP,          -1,            "gas mask away" },
			{ VK_UP,    false, ITEM_CANTEEN,  ITEM_CANTEEN,  "canteen out" },
			{ VK_UP,    true,  KEEP,          ITEM_CANTEEN,  "canteen drink" },
			{ VK_UP,    false, KEEP,          -1,            "canteen away" },
			{ VK_UP,    false, EMPTY,         -1,            "empty slot" },
		};
		static const int NUM = sizeof(STEPS) / sizeof(STEPS[0]);
		static int drawsAtStart = 0, posesAtStart = 0, aloneAtStart = 0;
		DWORD t = now - phaseStart;
		if ( step == 0 || t >= 4500 ) {
			if ( step >= NUM ) { g_puppetOffset.x = 6.0F; phase = 10; phaseStart = now; step = 0; break; }
			if ( step == 0 ) {
				GiveTestItems( harry );
				g_puppetOffset.x = -6.0F;   // On Harry's right-hand side, which holds the items.
			}
			if ( STEPS[step].upItem != KEEP ) harry->m_itemHotbar[0] = STEPS[step].upItem == EMPTY ? 0 : GetItemObject( STEPS[step].upItem );
			if ( STEPS[step].key && STEPS[step].move ) {
				InterlockedExchange( &g_holdMs, 3500 );   // Held past the check, so the puppet still shows it.
				InterlockedExchange( &g_holdKey, STEPS[step].key );
			}
			else if ( STEPS[step].key ) {
				InterlockedExchange( &g_holdMs2, 700 );
				InterlockedExchange( &g_holdKey2, VK_NUMPAD5 );   // Toggle item, held...
			}
			step++; phaseStart = now; snapped = false;
			aloneAtStart = g_copyDrawsAlone;
		}
		else if ( t >= 250 && t < 300 && STEPS[step - 1].key && !STEPS[step - 1].move ) {
			InterlockedExchange( &g_tapKey, STEPS[step - 1].key );   // ...while the arrow is tapped.
		}
		else if ( t >= 1300 && t < 1400 && !snapped && step >= 2 && STEPS[step - 1].expect < 0 && STEPS[step - 2].expect >= 0
		          && STEPS[step - 2].expect != ITEM_RAFT && STEPS[step - 1].upItem == KEEP ) {
			// Harry has put it away; Evil Harry, a second behind, still holds it.
			char name[48];
			sprintf_s( name, "09_item_%02d_%s_lag", step - 1, STEPS[step - 1].name );
			for ( char* c = name; *c; c++ ) if ( *c == ' ' ) *c = '_';
			RequestSnapshot( name );
			snapped = true;
		}
		else if ( t >= 3000 && t < 3100 ) {   // After the delayed put-away animation (the raft takes longest).
			snapped = false;
			drawsAtStart = g_puppetItemDraws;
			posesAtStart = g_puppetPoseCopies;
		}
		else if ( !snapped && t >= 4000 ) {   // Past the replay delay.
			int i = step - 1;
			int held = GetHeldItem( harry );
			void* m0 = g_cur->beast ? g_cur->beast->m_pickaxeModel0 : 0;
			void* m1 = g_cur->beast ? g_cur->beast->m_pickaxeModel1 : 0;
			bool shown = (m0 && (FIELD( m0, uint32_t, INSTANCE_FLAGS ) & INSTANCE_VISIBLE))
			          || (m1 && (FIELD( m1, uint32_t, INSTANCE_FLAGS ) & INSTANCE_VISIBLE));
			int draws = g_puppetItemDraws - drawsAtStart;   // Puppet copies drawn over the last 0.9 s.
			int poses = g_puppetPoseCopies - posesAtStart;
			int want = STEPS[i].expect;
			// Put away (but the raft, whose copy needs Harry's) : the copy outlives Harry's item.
			int prev = i > 0 ? STEPS[i - 1].expect : -1;
			bool putAway = want < 0 && prev >= 0 && prev != ITEM_RAFT && STEPS[i].upItem == KEEP;
			int alone = g_copyDrawsAlone - aloneAtStart;
			bool ok = held == want && !shown && (want >= 0 ? draws > 30 : draws == 0) && poses > 30 && (!putAway || alone > 5);
			if ( !ok ) g_itemFailures++;
			mp_log( "[AT] Item step %d (%s) : Harry holds item %d (expected %d), Evil Harry's own pickaxes %s, copy drawn %d times (%d after Harry's was away), pose copied %d times : %s\n",
				i, STEPS[i].name, held, want, shown ? "shown" : "hidden", draws, alone, poses, ok ? "ok" : "WRONG" );
			char name[40];
			sprintf_s( name, "09_item_%02d_%s", i, STEPS[i].name );
			for ( char* c = name; *c; c++ ) if ( *c == ' ' ) *c = '_';
			RequestSnapshot( name );
			snapped = true;
		}
		break;
	}

	case 7:   // Roll : hold crouch (numpad 8), then walk forward; log Harry's matrices.
	{
		static DWORD lastLog = 0;
		DWORD t = now - phaseStart;
		if ( step == 0 ) {
			InterlockedExchange( &g_holdMs2, 3500 );
			InterlockedExchange( &g_holdKey2, VK_NUMPAD8 );   // Crouch.
			mp_log( "[AT] Roll : holding crouch, then W.\n" );
			step = 1; snapped = false;
		}
		else if ( step == 1 && t >= 400 ) {
			InterlockedExchange( &g_holdMs, 2500 );
			InterlockedExchange( &g_holdKey, 'W' );
			step = 2;
		}
		if ( now - lastLog >= 100 && t < 4500 ) {
			const Matrix4f& m = harry->m_transformMatrix;
			const Matrix4f& rb = harry->m_rigidBodyMatrix;
			const PlayerSnapshot* tg = GetPuppetTarget();
			mp_log( "[AT] roll t=%4u clip 0x%.8X ground %d pos (%.2f %.2f %.2f) | xf r0 (%.2f %.2f %.2f) r1 (%.2f %.2f %.2f) r2 (%.2f %.2f %.2f) t (%.2f %.2f %.2f) | rb r0 (%.2f %.2f %.2f) r2 (%.2f %.2f %.2f) t (%.2f %.2f %.2f) | puppet z %.2f\n",
				t, tg ? tg->clipCRC : 0, harry->m_onGround, harry->m_position.x, harry->m_position.y, harry->m_position.z,
				m.data[0][0], m.data[0][1], m.data[0][2], m.data[1][0], m.data[1][1], m.data[1][2], m.data[2][0], m.data[2][1], m.data[2][2],
				m.data[3][0], m.data[3][1], m.data[3][2],
				rb.data[0][0], rb.data[0][1], rb.data[0][2], rb.data[2][0], rb.data[2][1], rb.data[2][2], rb.data[3][0], rb.data[3][1], rb.data[3][2],
				g_cur->beast ? FIELD( g_cur->beast, Matrix4f, BEAST_WORLD ).data[3][2] : 0.0F );
			lastLog = now;
		}
		if ( (!snapped && t >= 1500) || (snapped && t >= 2300 && t < 2360) ) {
			RequestSnapshot( snapped ? "08_roll_b" : "08_roll_a" );
			snapped = true;
		}
		if ( t >= 6000 ) { phase = 6; phaseStart = now; step = -1; snapped = false; }
		break;
	}

	case 6:   // Collision : walk Harry into the puppet held just ahead of him, solid vs not.
	{
		static int trial = 0;
		static Vector3f start, s_collideDir;
		static bool s_collideFar = false;
		DWORD t = now - phaseStart;
		if ( step == -1 ) {   // Back away from the gate into the open space behind Harry.
			if ( t == 0 || !snapped ) {
				InterlockedExchange( &g_holdMs, 600 );
				InterlockedExchange( &g_holdKey, 'S' );
				snapped = true;
			}
			if ( t >= 2500 ) { step = 0; phaseStart = now; }
			break;
		}
		if ( step == 0 ) {
			// TRIAL_FAR = control run with the puppet out of the way.
			static const uint32_t TRIAL_FAR = 0xFFFFFFFF;
			static const uint32_t TRIALS[] = { TRIAL_FAR, 1, 0, 1, 0 };   // 1 = solid (the game's default), 0 = our puppet.
			static const int NUM_TRIALS = sizeof(TRIALS) / sizeof(TRIALS[0]);
			if ( trial >= NUM_TRIALS ) {
				g_puppetPinned = false; g_puppetSolid = false;
				phase = 2; phaseStart = now; step = 0; snapped = false;   // Circle next : the walk moves the camera.
				break;
			}
			s_collideFar = (TRIALS[trial] == TRIAL_FAR);
			g_puppetSolid = !s_collideFar && TRIALS[trial] == 1;
			start = harry->m_position;
			InterlockedExchange( &g_holdMs, 250 );   // Short step to get the walking direction.
			InterlockedExchange( &g_holdKey, (trial % 2) ? 'S' : 'W' );   // Out into the field, then back.
			step = 1; phaseStart = now; snapped = false;
		}
		else if ( step == 1 && t >= 1200 ) {
			static Vector3f dir;
			float dx = harry->m_position.x - start.x, dy = harry->m_position.y - start.y;
			float len = sqrtf( dx * dx + dy * dy );
			if ( len < 0.1F ) { mp_log( "[AT] Harry didn't move, skipping trial %d.\n", trial ); trial++; step = 0; break; }
			dir.x = dx / len; dir.y = dy / len; dir.z = 0.0F;
			start = harry->m_position;
			g_puppetPin.x = start.x + dir.x * 2.0F; g_puppetPin.y = start.y + dir.y * 2.0F; g_puppetPin.z = start.z;
			g_puppetPinned = true;
			mp_log( "[AT] Collision trial %d : puppet %s, walking into it.\n", trial,
				s_collideFar ? "out of the way" : g_puppetSolid ? "solid" : "not solid" );
			s_collideDir = dir;
			step = 2; phaseStart = now;
		}
		else if ( step == 2 && t >= 500 ) {
			InterlockedExchange( &g_holdMs, 500 );
			InterlockedExchange( &g_holdKey, (trial % 2) ? 'S' : 'W' );
			step = 3; phaseStart = now; snapped = false;
		}
		else if ( step == 3 ) {
			// Keep the puppet 1 unit in front of Harry, along his movement.
			static Vector3f lastPos;
			if ( t > 0 ) {
				float vx = harry->m_position.x - lastPos.x, vy = harry->m_position.y - lastPos.y;
				float vl = sqrtf( vx * vx + vy * vy );
				if ( vl > 0.01F ) { s_collideDir.x = vx / vl; s_collideDir.y = vy / vl; }
			}
			lastPos = harry->m_position;
			float ahead = s_collideFar ? -15.0F : 1.0F;   // Control : far behind him.
			g_puppetPin.x = harry->m_position.x + s_collideDir.x * ahead;
			g_puppetPin.y = harry->m_position.y + s_collideDir.y * ahead;
			g_puppetPin.z = harry->m_position.z;
			if ( !snapped && t >= 250 ) {
				char name[40];
				sprintf_s( name, "07_collide_%d_%s", trial, s_collideFar ? "none" : g_puppetSolid ? "solid" : "ghost" );
				RequestSnapshot( name );
				snapped = true;
			}
			if ( t >= 1500 ) {
				float dx = harry->m_position.x - start.x, dy = harry->m_position.y - start.y;
				mp_log( "[AT] Collision trial %d (%s) : Harry moved %.2f (about 8.7 with no puppet, about 3.9 when blocked).\n",
					trial, s_collideFar ? "NO PUPPET" : g_puppetSolid ? "puppet ahead, solid" : "puppet ahead, NOT solid", sqrtf( dx * dx + dy * dy ) );
				g_puppetPinned = false;
				trial++; step = 0; phaseStart = now;
			}
		}
		break;
	}

	case 3:   // Back to idle once it stops.
		if ( now - phaseStart < 3000 ) break;
		LogTelemetry( harry, "final" );
		RequestSnapshot( "05_stopped" );
		if ( g_itemFailures ) mp_log( "[AT] FAIL : %d item steps wrong.\n", g_itemFailures );
		if ( g_windowFailures ) mp_log( "[AT] FAIL : the game window stays on top of others.\n" );
		InterlockedExchange( &g_autotestDone, (g_itemFailures || g_windowFailures) ? 2 : 1 );
		break;
	}
}



// --------------------------------------------------------------------------------
// Tick hooks. __fastcall receives "this" in ECX like __thiscall (EDX is unused),
// so these can replace the vtable entries directly.
// --------------------------------------------------------------------------------
// The game's tick code doesn't preserve every register it should : Harry's tick
// returns with EDI holding a return address (0x4B2DE6, after "call 0x547180").
// Game code never relies on EDI, but our compiled hooks may keep "self" in it, so
// "self" is kept in memory across the original call and read back afterwards.
static EIHarry* volatile g_harryTickSelf = 0;
static EIEvilHarryP* volatile g_evilTickSelf = 0;

static void __fastcall HarryTick_Hook( EIHarry* self, void* )
{
	g_harryTickSelf = self;
	g_origHarryTick( self );
	self = g_harryTickSelf;

	if ( InterlockedIncrement( &g_harryTicks ) == 1 ) {
		mp_log( "[MP] Harry tick hook is running (this 0x%.8X, global Harry 0x%.8X).\n", self, Gizmod::getHarry() );
	}

	if ( self != Gizmod::getHarry() ) return;

	// Another level since the puppet was spawned. The load events don't cover every
	// way of changing levels (the "Go to" menu, loading a save), this does.
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		const Peer& p = g_peers[i];
		if ( p.beast && (self->m_world != p.world || Gizmod::getCurrentLevelCRC() != p.level) ) {
			ForgetPuppet();
			break;
		}
	}

	if ( !g_origPresent ) HookPresent();   // The overlay draws as frames are presented.
	PushLocalSnapshot( self );
	NetUpdate();
	// Another player in this level : show them (Evil Harry appears on his own).
	// (The autotest spawns the main peer's itself, when its steps need it.)
	if ( g_networked ) {
		uint32_t now = NetNowMs();
		for ( int i = 0; i < MAX_PEERS; i++ ) {
			Peer* p = &g_peers[i];
			if ( g_autotest && p == MainPeer() ) continue;
			if ( p->beast || !p->target || now - p->lastSpawnTry < 1000 ) continue;
			p->lastSpawnTry = now;
			mp_log( "[NET] Player %d is in this level : spawning their puppet.\n", i );
			CurrentPeer current( p );
			SpawnPuppet( self );
		}
	}
	g_cur = MainPeer();
	if ( g_autotest ) AutotestStep( self );
	HandleHotkeys( self );
}

static void __fastcall EvilHarryTick_Hook( EIEvilHarryP* self, void* )
{
	Peer* peer = FindPeer( self );
	if ( !peer ) {   // The game's own Evil Harry.
		g_origEvilHarryTick( self );
		return;
	}
	CurrentPeer current( peer );

	if ( g_skipPuppetAI ) {
		// No AI, no animation : just place it.
		DrivePuppet( self );
		return;
	}

	UpdateLocomotion( self );

	if ( g_cur->forcedState >= 0 && self->m_stateIndexCurrent != g_cur->forcedState ) {
		if ( g_cur->lastRequested != g_cur->forcedState ) {
			mp_log( "[MP] Requesting state %d (current %d)...\n", g_cur->forcedState, self->m_stateIndexCurrent );
			g_cur->lastRequested = g_cur->forcedState;
		}
		self->m_forceNextState = true;
		self->m_nextState = g_cur->forcedState;
	}

	PrePositionPuppet( self );
	g_evilTickSelf = self;
	g_origEvilHarryTick( self );   // Animations, state machine.
	self = g_evilTickSelf;         // See g_harryTickSelf.
	if ( g_autoLocomotion ) ApplyLocomotionAnim( self );

	if ( self->m_stateIndexCurrent != g_cur->lastLoggedState ) {
		mp_log( "[MP] Player %d's puppet is now in state %d.\n", g_cur->id, self->m_stateIndexCurrent );
		g_cur->lastLoggedState = self->m_stateIndexCurrent;
	}

	DrivePuppet( self );           // Then override where it is.
}



// --------------------------------------------------------------------------------
// Settings (see the top of this file).
// --------------------------------------------------------------------------------
struct NetConfig
{
	char server[128];
	char room[64];
	char name[64];
	bool echo;
	int delayMs;   // Playback delay, 0 : the default.
};

static const char DEFAULT_INI[] =
	"; Multiplayer mod settings.\r\n"
	"[Network]\r\n"
	"; Relay server (host:port). Empty : no network, Evil Harry (F5) replays you.\r\n"
	"Server=\r\n"
	"; Players in the same room see each other.\r\n"
	"Room=default\r\n"
	"Name=Harry\r\n"
	"; 1 : the server sends your own moves back to you (Evil Harry replays you\r\n"
	"; through the internet, to test alone).\r\n"
	"Echo=0\r\n"
	"; How far behind the other players are shown (ms) : 133, 250, 500, 1000, 1500\r\n"
	"; or 3000 (F7 cycles through them). Shorter : closer to where they really are,\r\n"
	"; but they stutter when the connection does.\r\n"
	"Delay=250\r\n";

static void GetEnvOverride( const char* var, char* value, int size )
{
	char buf[128];
	DWORD n = GetEnvironmentVariableA( var, buf, sizeof(buf) );
	if ( n > 0 && n < sizeof(buf) ) strncpy_s( value, size, buf, _TRUNCATE );
}

static void ReadNetConfig( NetConfig* c )
{
	// mods\Multiplayer.ini, next to this plugin.
	char ini[MAX_PATH] = { 0, };
	HMODULE self = 0;
	GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR) &ReadNetConfig, &self );
	GetModuleFileNameA( self, ini, MAX_PATH );
	char* slash = strrchr( ini, '\\' );
	if ( slash ) strcpy_s( slash + 1, MAX_PATH - (slash + 1 - ini), "Multiplayer.ini" );

	if ( GetFileAttributesA( ini ) == INVALID_FILE_ATTRIBUTES ) {
		FILE* f = 0;
		if ( fopen_s( &f, ini, "wb" ) == 0 && f ) {
			fwrite( DEFAULT_INI, 1, sizeof(DEFAULT_INI) - 1, f );
			fclose( f );
			mp_log( "[NET] Wrote default settings to %s.\n", ini );
		}
	}
	GetPrivateProfileStringA( "Network", "Server", "", c->server, sizeof(c->server), ini );
	GetPrivateProfileStringA( "Network", "Room", "default", c->room, sizeof(c->room), ini );
	GetPrivateProfileStringA( "Network", "Name", "Harry", c->name, sizeof(c->name), ini );
	char echo[8] = "0";
	GetPrivateProfileStringA( "Network", "Echo", "0", echo, sizeof(echo), ini );

	// The autotests choose their server themselves (none : loopback), whatever the ini says.
	char autotest[8] = { 0, };
	if ( GetEnvironmentVariableA( "PTLE_MP_AUTOTEST", autotest, sizeof(autotest) ) && autotest[0] == '1' ) {
		c->server[0] = 0;
		strcpy_s( echo, "0" );
	}
	GetEnvOverride( "PTLE_MP_SERVER", c->server, sizeof(c->server) );
	GetEnvOverride( "PTLE_MP_ROOM", c->room, sizeof(c->room) );
	GetEnvOverride( "PTLE_MP_NAME", c->name, sizeof(c->name) );
	GetEnvOverride( "PTLE_MP_ECHO", echo, sizeof(echo) );
	char delay[16] = { 0, };
	GetPrivateProfileStringA( "Network", "Delay", "", delay, sizeof(delay), ini );
	GetEnvOverride( "PTLE_MP_DELAY", delay, sizeof(delay) );
	c->delayMs = atoi( delay );
	c->echo = echo[0] == '1';
}



// --------------------------------------------------------------------------------
// Plugin.
// --------------------------------------------------------------------------------
// The PC game has one retail build, 1.0.12.18-1 : every address in this mod is
// that build's. Its version string, in .rdata.
static const uint32_t GAME_VERSION_ADDRESS = 0x887B5C;
static const char GAME_VERSION[] = "Pitfall Harry    1.0.12.18-1";

static bool IsKnownGameVersion()
{
	MEMORY_BASIC_INFORMATION mbi;
	if ( !VirtualQuery( (void*) GAME_VERSION_ADDRESS, &mbi, sizeof(mbi) ) || mbi.State != MEM_COMMIT
		|| (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ) return false;
	if ( (uint8_t*) mbi.BaseAddress + mbi.RegionSize < (uint8_t*) GAME_VERSION_ADDRESS + sizeof(GAME_VERSION) ) return false;
	return memcmp( (void*) GAME_VERSION_ADDRESS, GAME_VERSION, sizeof(GAME_VERSION) ) == 0;
}

// --------------------------------------------------------------------------------
// The overlay : name tags over the other players, the player list (F9), and
// "joined" / "left" messages.
// --------------------------------------------------------------------------------
static const float TAG_FADE_START = 150.0F;   // Camera distance : tags fade out from here...
static const float TAG_FADE_END = 300.0F;    // ...to here.

static uint32_t WithAlpha( uint32_t color, float alpha )
{
	if ( alpha < 0.0F ) alpha = 0.0F;
	if ( alpha > 1.0F ) alpha = 1.0F;
	return (color & 0x00FFFFFF) | ((uint32_t) (((color >> 24) & 0xFF) * alpha + 0.5F) << 24);
}

static void DrawOverlay( IDirect3DDevice9* dev )
{
	uint32_t now = NetNowMs();
	bool any = g_showList;
	for ( int i = 0; i < MAX_PEERS && !any; i++ ) any = g_peers[i].beast && g_peers[i].tagFrame == g_presentCount;
	for ( int i = 0; i < MAX_TOASTS && !any; i++ ) any = g_toasts[i].text[0] && now - g_toasts[i].time < TOAST_MS;
	if ( !any ) return;

	D3DVIEWPORT9 vp;
	if ( FAILED( dev->GetViewport( &vp ) ) ) return;
	float h = vp.Height / 38.0F;
	if ( h < 14.0F ) h = 14.0F;
	if ( !OverlayBegin( dev, h ) ) return;
	float sw, sh;
	OverlayScreenSize( &sw, &sh );
	float pad = h * 0.3F;

	// Name tags.
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		const Peer& p = g_peers[i];
		if ( !p.beast || p.tagFrame != g_presentCount ) continue;
		float alpha = 1.0F - (p.tagDepth - TAG_FADE_START) / (TAG_FADE_END - TAG_FADE_START);
		if ( alpha <= 0.0F ) continue;
		const char* name = PlayerName( p.id );
		float w = OverlayTextWidth( name );
		float x = floorf( p.tagX - w * 0.5F ), y = floorf( p.tagY - h * 1.2F );
		OverlayRect( x - pad, y - pad * 0.5F, w + 2.0F * pad, h + pad, WithAlpha( 0x80000000, alpha ) );
		OverlayText( x, y, name, WithAlpha( 0xFFFFFFFF, alpha ) );
	}

	// Messages, top middle.
	float ty = h;
	for ( int i = 0; i < MAX_TOASTS; i++ ) {
		if ( !g_toasts[i].text[0] || now - g_toasts[i].time >= TOAST_MS ) continue;
		float alpha = (TOAST_MS - (now - g_toasts[i].time)) / 1000.0F;   // Fades out over the last second.
		float w = OverlayTextWidth( g_toasts[i].text );
		float x = floorf( (sw - w) * 0.5F );
		OverlayRect( x - pad, ty - pad * 0.5F, w + 2.0F * pad, h + pad, WithAlpha( 0xA0000000, alpha ) );
		OverlayText( x, ty, g_toasts[i].text, WithAlpha( 0xFFFFE080, alpha ) );
		ty += h * 1.4F;
	}

	// The player list, top right.
	if ( g_showList ) {
		static const int MAX_ROWS = MAX_PEERS + 3;
		char left[MAX_ROWS][96], right[MAX_ROWS][64];
		uint32_t colors[MAX_ROWS];
		int rows = 0;
		int me = g_transport ? g_transport->PlayerId() : -1;
		uint32_t here = Gizmod::getCurrentLevelCRC();
		int count = me >= 0 ? 1 : 0;
		for ( int i = 0; i < MAX_PEERS; i++ ) if ( g_players[i].present && i != me ) count++;

		if ( !g_networked ) {
			sprintf_s( left[rows], "Not online" );
			sprintf_s( right[rows], "(no Server in Multiplayer.ini)" );
		}
		else if ( me < 0 ) {
			sprintf_s( left[rows], "Room \"%s\"", g_roomName );
			sprintf_s( right[rows], "connecting..." );
		}
		else {
			sprintf_s( left[rows], "Room \"%s\"", g_roomName );
			sprintf_s( right[rows], "%d player%s", count, count == 1 ? "" : "s" );
		}
		colors[rows++] = 0xFFFFE080;
		if ( me >= 0 ) {
			sprintf_s( left[rows], "%s (you)", g_myName );
			sprintf_s( right[rows], "%s", LevelName( g_haveLocal ? here : 0 ) );
			colors[rows++] = 0xFFA0FFA0;
		}
		for ( int i = 0; i < MAX_PEERS; i++ ) {
			const PlayerInfo& info = g_players[i];
			if ( !info.present || i == me ) continue;
			sprintf_s( left[rows], "%s", info.name );
			if ( info.level && info.level == here ) sprintf_s( right[rows], "here" );
			else sprintf_s( right[rows], "%s", LevelName( info.level ) );
			colors[rows++] = 0xFFFFFFFF;
		}
		int ping = g_transport ? g_transport->RoundTripMs() : -1;
		if ( ping >= 0 ) sprintf_s( left[rows], "Ping %d ms", ping );
		else left[rows][0] = 0;
		sprintf_s( right[rows], "F9 : hide" );
		colors[rows++] = 0xFFB0B0B0;

		float wl = 0.0F, wr = 0.0F;
		for ( int r = 0; r < rows; r++ ) {
			float a = OverlayTextWidth( left[r] ), b = OverlayTextWidth( right[r] );
			if ( a > wl ) wl = a;
			if ( b > wr ) wr = b;
		}
		float gap = h * 1.5F;
		float bw = wl + gap + wr + 2.0F * pad, bh = rows * h * 1.3F + 2.0F * pad;
		float bx = floorf( sw - bw - h ), by = h;
		OverlayRect( bx, by, bw, bh, 0xA0000000 );
		for ( int r = 0; r < rows; r++ ) {
			float y = by + pad + r * h * 1.3F;
			OverlayText( bx + pad, y, left[r], colors[r] );
			OverlayText( bx + bw - pad - OverlayTextWidth( right[r] ), y, right[r], colors[r] );
		}
	}

	OverlayEnd();
}

// Leaving the room when the game closes, so the others drop our puppet at once
// instead of after a timeout. The game ends through kernel32's ExitProcess (from
// its own import table, patched below), and so does the autotest (QuitGame).
typedef VOID (WINAPI *ExitProcessFunc)( UINT );
static ExitProcessFunc g_origExitProcess = ExitProcess;

static void LeaveRoom()
{
	if ( g_transport ) g_transport->Leave();
}

static void QuitGame( UINT code )
{
	LeaveRoom();
	g_origExitProcess( code );
}

static VOID WINAPI ExitProcess_Hook( UINT code )
{
	LeaveRoom();
	g_origExitProcess( code );
}

// Points the game's import of kernel32's ExitProcess at the hook.
static bool HookGameExitProcess()
{
	uint8_t* image = (uint8_t*) GetModuleHandleA( 0 );
	IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*) (image + ((IMAGE_DOS_HEADER*) image)->e_lfanew);
	const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if ( !dir.VirtualAddress ) return false;
	FARPROC target = GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "ExitProcess" );
	for ( IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*) (image + dir.VirtualAddress); d->Name; d++ ) {
		if ( _stricmp( (const char*) (image + d->Name), "kernel32.dll" ) != 0 ) continue;
		IMAGE_THUNK_DATA* thunk = (IMAGE_THUNK_DATA*) (image + d->FirstThunk);
		for ( ; thunk->u1.Function; thunk++ ) {
			if ( (FARPROC) thunk->u1.Function != target ) continue;
			g_origExitProcess = (ExitProcessFunc) thunk->u1.Function;
			injector::WriteMemory( &thunk->u1.Function, (uint32_t) &ExitProcess_Hook, true );
			return true;
		}
	}
	return false;
}

class MultiplayerPlugin : public GizmodPlugin, public ILoadLevelListener, public ILevelLoadedListener
{
public:

	virtual const char* getDisplayName() const { return "Multiplayer (puppet test)"; }
	virtual const char* getSystemName() const { return "Multiplayer"; }

	virtual void onEnable()
	{
		OpenLogFile();
		if ( !IsKnownGameVersion() ) {
			mp_log( "[MP] This isn't the game version the mod was made for (PC 1.0.12.18-1) : the mod is off.\n" );
			MessageBoxA( 0, "The Multiplayer mod was made for Pitfall: The Lost Expedition PC version 1.0.12.18-1, "
				"and this copy of the game is a different version.\n\nThe mod is turned off. The game will run without it.",
				"Multiplayer mod", MB_OK | MB_ICONWARNING );
			return;
		}
		AddVectoredExceptionHandler( 1, CrashLogger );

		register_types();

		uint32_t harrySlot = VTABLE_HARRY + VTABLE_SLOT_TICK;
		uint32_t evilSlot  = VTABLE_EVILHARRY_P + VTABLE_SLOT_TICK;

		g_origHarryTick     = injector::ReadMemory<TickFunc>( harrySlot, true );
		g_origEvilHarryTick = injector::ReadMemory<TickFunc>( evilSlot, true );

		injector::WriteMemory( harrySlot, &HarryTick_Hook, true );
		injector::WriteMemory( evilSlot,  &EvilHarryTick_Hook, true );
		InstallItemDrawHooks();
		HookCamera();

		Gizmod::getInstance()->getEventListener()->registerEvent<LoadLevelEvent>( this );
		Gizmod::getInstance()->getEventListener()->registerEvent<LevelLoadedEvent>( this );

		CreateThread( 0, 0, HotkeyThread, 0, 0, 0 );

		// Several games at once (PTLE_MP_INSTANCE, multiplayer tests on one computer) :
		// the game allows one copy only (CreateMutex at 0x60D456; if it already exists
		// the new copy brings the old one's window up and quits). Always "doesn't exist".
		char instance[32];
		if ( GetEnvironmentVariableA( "PTLE_MP_INSTANCE", instance, sizeof(instance) ) ) {
			injector::WriteMemory<uint8_t>( 0x60D46A, 0xEB, true );   // jne -> jmp.
			mp_log( "[MP] Instance \"%s\" : more than one copy of the game allowed.\n", instance );
		}

		g_netLog = mp_log;
		NetConfig config;
		ReadNetConfig( &config );
		ResetHistory();
		CleanName( config.name, (int) strlen( config.name ), g_myName );
		if ( !g_myName[0] ) strcpy_s( g_myName, "Harry" );
		strncpy_s( g_roomName, config.room, _TRUNCATE );
		if ( !HookGameExitProcess() ) mp_log( "[NET] Couldn't hook the game's exit : the others drop us after a timeout.\n" );
		if ( config.delayMs > 0 ) {   // The nearest choice.
			int n = sizeof(DELAY_CHOICES) / sizeof(DELAY_CHOICES[0]);
			for ( int i = 0; i < n; i++ ) {
				if ( abs( DELAY_CHOICES[i] - config.delayMs ) < abs( DELAY_CHOICES[g_delayIndex] - config.delayMs ) ) g_delayIndex = i;
			}
		}
		mp_log( "[NET] Playback delay : %d ms.\n", DELAY_CHOICES[g_delayIndex] );
		if ( config.server[0] ) {
			g_networked = true;
			g_transport = CreateUdpTransport( config.server, config.room, config.name, config.echo );
			mp_log( "[NET] Server %s, room \"%s\", name \"%s\"%s.\n", config.server, config.room, config.name, config.echo ? ", echo" : "" );
		}
		else {
			// Loopback (the puppet replays us), with simulated network conditions
			// from PTLE_MP_NETSIM="latency,jitter,loss" (ms, ms, fraction).
			NetSimulation sim = { 0, 0, 0.0F };
			char simEnv[64] = { 0, };
			if ( GetEnvironmentVariableA( "PTLE_MP_NETSIM", simEnv, sizeof(simEnv) ) ) {
				sscanf_s( simEnv, "%d,%d,%f", &sim.latencyMs, &sim.jitterMs, &sim.loss );
			}
			g_transport = CreateLoopbackTransport( sim );
			mp_log( "[NET] No server : loopback (latency %d ms, jitter %d ms, loss %.0f%%).\n", sim.latencyMs, sim.jitterMs, sim.loss * 100.0F );
		}

		char env[8] = { 0, };
		if ( GetEnvironmentVariableA( "PTLE_MP_AUTOTEST", env, sizeof(env) ) && env[0] == '1' ) {
			g_autotest = true;
			g_background = !(GetEnvironmentVariableA( "PTLE_MP_FOREGROUND", env, sizeof(env) ) && env[0] == '1');
			if ( g_background ) {
				injector::MakeCALL( POLL_KEYBOARD_CALL, &PollKeyboard_Hook );
				PatchWindowNoActivate();
			}
			g_puppetOffset.x = 6.0F;   // Stand beside Harry, so screenshots show whether it follows.
			g_showList = true;         // Screenshots show the player list.
			EnableAutoSkipCutscenes();
			if ( GetEnvironmentVariableA( "PTLE_MP_SURVEY", env, sizeof(env) ) && env[0] == '1' ) {
				g_survey = true;
				AUTOTEST_TOTAL_TIMEOUT_MS = 1200000;
				mp_log( "[AT] Level survey mode.\n" );
			}
			CreateThread( 0, 0, AutotestThread, 0, 0, 0 );
		}

		mp_log( "[MP] Hooked ticks (Harry 0x%.8X, Evil Harry 0x%.8X). F5 spawn, F6 state, F7 delay, F8 stay here, F9 player list.\n",
			(uint32_t) g_origHarryTick, (uint32_t) g_origEvilHarryTick );
	}

	virtual void onLoadLevel( LoadLevelEvent& e )
	{
		ForgetPuppet();
	}

	virtual void onLevelLoaded( LevelLoadedEvent& e )
	{
		ForgetPuppet();
	}
};

DECLARE_PLUGIN( MultiplayerPlugin );
