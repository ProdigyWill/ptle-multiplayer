// --------------------------------------------------------------------------------
// Multiplayer (milestone 1 : puppet test).
//
// Settings : config\Multiplayer\Multiplayer.ini in the game folder (written with
// defaults the first time; mods\Multiplayer.ini, where it used to be, is still read
// if that's the only one), section
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
//   F1 : the menu (players, display settings, keys).
//   F7 : cycle the playback delay (0.13s .. 3s).
// --------------------------------------------------------------------------------


/*
David note :

There are several "TODO" comments in this file and other files indicating changes to be
made in the future and features to be made core.
Do NOT remove them, I use these to Ctrl+F locations that require my attention.
*/


#include <time.h>
#include <intrin.h>
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
#include "gizmod/item/InventoryItem.h"

#include "injector/injector.hpp"

#include "ptle/EIHarry.h"
#include "ptle/EIEvilHarryP.h"
#include "ptle/EBeastStateMachine.h"
#include "ptle/ERAnim.h"
#include "ptle/ERLevel.h"
#include "ptle/ERCharacter.h"
#include "ptle/EIStaticModel.h"
#include "ptle/EItemRaft.h"

#include "ptle/math/Matrix4f.h"
#include "ptle/math/Vector3f.h"
#include "ptle/types/types.h"
#include "ptle/levels/level_info.h"

#include "utils/func.h"

#include "Multiplayer.h"

#include "version.h"
#include "net/protocol.h"
#include "net/remote.h"
#include "net/snapshot.h"
#include "net/transport.h"
#include "overlay.h"
#include "menu.h"

#include "tests.h"

#include "util/MatrixUtils.h"
#include "util/MemoryWriter.h"
#include "util/MemoryReader.h"

#include "gamemodes/HideAndSeek/HideAndSeek.h"



#define countof(arr) (sizeof(arr) / sizeof(arr[0]))

#define FIELD( obj, type, offset ) (*(type*)((uint8_t*)(obj) + (offset)))

GET_METHOD( 0x626670, void, AddToWorld, ERLevel*, EInstance* );
GET_METHOD( 0x6268D0, void, ERLevel_SetupEntity, ERLevel*, EInstance*, EInstance* );
GET_METHOD( 0x412850, void, EIBeast_LoadBeastAsset, EIBeast* );
GET_FUNC(   0x6F8EC0, uint32_t, HashPath, const char* );


static const uint32_t VTABLE_SLOT_TICK   = 0x2C;     // EInstance::Tick().
static const uint32_t VTABLE_SLOT_RENDER = 0x34;     // EInstance::Render( context, arg ).


typedef void (__thiscall *TickFunc)( EInstance* );
typedef void (__thiscall *RenderFunc)( EInstance* self, void* context, void* arg );

GET_METHOD( 0x4B2CD0, void, EIHarry_Tick, EIHarry* );
GET_METHOD( 0x414F80, void, EIBeast_Tick, EIBeast* );
GET_METHOD( 0x41C6A0, void, EIBeast_Render, EIBeast*, void*, void* );

// Shortcut.
static inline Logger* logger() { return Gizmod::getInstance()->getLogger(); }



// --------------------------------------------------------------------------------
// Logging : Gizmod console + %LOCALAPPDATA%\PTLE Multiplayer\logs\multiplayer.log
// (the game folder isn't writable without admin rights, and nothing of the mod's
// belongs in the player's Documents).
// --------------------------------------------------------------------------------
static FILE* g_logFile = 0;
static CRITICAL_SECTION g_logLock;
char g_logDir[MAX_PATH] = { 0, };

// %LOCALAPPDATA%\PTLE Multiplayer (created) : what the mod writes goes there.
bool GetDataDir( char* path )
{
	if ( FAILED(SHGetFolderPathA(0, CSIDL_LOCAL_APPDATA, 0, 0, path)) ) return false;
	strcat_s( path, MAX_PATH, "\\PTLE Multiplayer" );
	CreateDirectoryA( path, 0 );
	return true;
}

static void OpenLogFile()
{
	InitializeCriticalSection( &g_logLock );

	char path[MAX_PATH];
	if ( !GetDataDir(path) ) return;
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

	// The last games' logs are kept : multiplayer-1.log (the one before), -2, -3. A crash
	// is in its game's log, which the restart used to overwrite. Not for the autotest
	// (it runs often : the player's own games would soon be gone).
	char env[4];
	const bool autotest = GetEnvironmentVariableA( "PTLE_MP_AUTOTEST", env, sizeof(env) ) && env[0] == '1';
	if ( !autotest ) {
		const int KEEP = 3;
		char from[MAX_PATH], to[MAX_PATH];
		for ( int i = KEEP; i >= 1; i-- ) {
			if ( i > 1 ) sprintf_s( from, "%s\\multiplayer-%d.log", g_logDir, i - 1 );
			else strcpy_s( from, path );
			sprintf_s( to, "%s\\multiplayer-%d.log", g_logDir, i );
			MoveFileExA( from, to, MOVEFILE_REPLACE_EXISTING );
		}
	}

	g_logFile = _fsopen( path, "w", _SH_DENYWR );
}

bool GameHasFocus()
{
	HWND fg = GetForegroundWindow();
	if ( !fg ) return false;
	DWORD pid = 0;
	GetWindowThreadProcessId( fg, &pid );
	return pid == GetCurrentProcessId();   // Game window or its console.
}

void mp_log( const char* fmt, ... )
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

// "name.dll+0x1234" for an address inside a loaded module (a crash in a mod or a
// system DLL : their addresses change from one PC to the next), else false.
static bool DescribeAddress( uint32_t address, char* out, int size )
{
	HMODULE module = 0;
	if ( !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR) address, &module ) || !module ) return false;
	char path[MAX_PATH] = { 0, };
	GetModuleFileNameA( module, path, MAX_PATH );
	const char* name = strrchr( path, '\\' );
	sprintf_s( out, size, "%s+0x%X", name ? name + 1 : path, address - (uint32_t) module );
	return true;
}

// Records crashes (address + registers + what's on the stack) before Gizmod's crash
// handler runs : every exception that is an error (access violation, stack
// overflow, division by zero, heap corruption...), first-chance, capped so handled
// ones don't flood the log. Looking through the stack (IsBadReadPtr...) raises
// exceptions of its own on this thread : those are ours, not logged (they used to
// use up the cap, so a real crash after them went unlogged).
static volatile DWORD g_crashLoggerThread = 0;

static const char* ExceptionName( DWORD code )
{
	switch ( code ) {
	case EXCEPTION_ACCESS_VIOLATION:      return "access violation";
	case EXCEPTION_STACK_OVERFLOW:        return "stack overflow";
	case EXCEPTION_ILLEGAL_INSTRUCTION:   return "illegal instruction";
	case EXCEPTION_PRIV_INSTRUCTION:      return "privileged instruction";
	case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "integer division by zero";
	case EXCEPTION_INT_OVERFLOW:          return "integer overflow";
	case EXCEPTION_IN_PAGE_ERROR:         return "in-page error";
	case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
	case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
	case 0xC0000374:                      return "heap corruption";
	case 0xC0000409:                      return "stack buffer overrun";
	case 0xE06D7363:                      return "C++ exception";
	}
	if ( (code & 0xF0000000) == 0xC0000000 ) return "error";
	return 0;
}

static LONG CALLBACK CrashLogger( EXCEPTION_POINTERS* e )
{
	static volatile LONG count = 0;
	DWORD code = e->ExceptionRecord->ExceptionCode;
	const char* name = ExceptionName( code );
	if ( !name ) return EXCEPTION_CONTINUE_SEARCH;              // Not an error (debugger messages, thread names...).
	if ( g_crashLoggerThread == GetCurrentThreadId() ) return EXCEPTION_CONTINUE_SEARCH;   // Our own stack probing.
	static volatile LONG cppCount = 0;   // C++ exceptions are often caught (drivers...) : a few only, apart from the rest.
	if ( code == 0xE06D7363 ? InterlockedIncrement( &cppCount ) > 3 : InterlockedIncrement( &count ) > 12 ) return EXCEPTION_CONTINUE_SEARCH;

	CONTEXT* c = e->ContextRecord;
	if ( code == EXCEPTION_STACK_OVERFLOW ) {   // Little stack left : one short line, no stack walk.
		mp_log( "[MP] EXCEPTION 0xC00000FD (stack overflow) at EIP 0x%.8X ESP=%.8X\n", c->Eip, c->Esp );
		return EXCEPTION_CONTINUE_SEARCH;
	}
	g_crashLoggerThread = GetCurrentThreadId();
	if ( code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR )
		mp_log( "[MP] EXCEPTION 0x%.8X (%s) at EIP 0x%.8X (%s 0x%.8X) EAX=%.8X EBX=%.8X ECX=%.8X EDX=%.8X ESI=%.8X EDI=%.8X ESP=%.8X\n",
			code, name, c->Eip,
			e->ExceptionRecord->ExceptionInformation[0] == 8 ? "executing" : e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
			(uint32_t) e->ExceptionRecord->ExceptionInformation[1],
			c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp );
	else
		mp_log( "[MP] EXCEPTION 0x%.8X (%s) at EIP 0x%.8X EAX=%.8X EBX=%.8X ECX=%.8X EDX=%.8X ESI=%.8X EDI=%.8X ESP=%.8X\n",
			code, name, c->Eip, c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp );

	char where[MAX_PATH + 16];
	if ( DescribeAddress( c->Eip, where, sizeof(where) ) ) mp_log( "[MP]   in %s\n", where );

	// Return addresses from the stack help locate the caller (in the game, or in a
	// mod's or a system DLL's code).
	uint32_t* sp = (uint32_t*) c->Esp;
	for ( int i = 0, shown = 0; i < 96 && shown < 16; i++ ) {
		uint32_t v = 0;
		if ( !IsBadReadPtr( sp + i, 4 ) ) v = sp[i];
		if ( v >= 0x401000 && v < 0x869000 ) { mp_log( "[MP]   stack+%02X : 0x%.8X\n", i * 4, v ); shown++; }
		else if ( v >= 0x10000 && DescribeAddress( v, where, sizeof(where) ) ) {
			mp_log( "[MP]   stack+%02X : 0x%.8X %s\n", i * 4, v, where );
			shown++;
		}
	}
	g_crashLoggerThread = 0;
	return EXCEPTION_CONTINUE_SEARCH;
}



// Game modes (F1 -> Gamemodes; Multiplayer.ini, [Gamemodes] Mode). Off : just
// see each other.
static const char* const MODE_NAMES[NUM_MODES] = { "Off", "Hide and seek" };
int g_gameMode = MODE_OFF;


// Swapping roles without the menu : hold the controller button or the key, then
// either direction of the d-pad or either arrow key (Multiplayer.ini, [Keys]
// RoleSwapButton, RoleSwapKey). While it's held, the d-pad and the arrows don't
// reach the game : no item comes out.
static int g_swapButton = 4;     // DirectInput button : the fifth, L1 / LB.
static int g_swapKey = 0x2A;     // DirectInput key code : Left Shift.

static const int MAX_VERSION = 15;
static char g_myVersion[MAX_VERSION + 1] = MP_VERSION;   // Sent to the others (tests : PTLE_MP_FAKEVERSION, another one).
static char g_myName[MAX_NAME + 1] = "Harry";
static int g_myNameLen = 5;
static char g_roomName[64] = "";
static uint32_t g_lastInfoSent = 0;
static uint32_t g_lastNetUpdate = 0;

static_assert( sizeof(MP_VERSION) - 1 <= MAX_VERSION, "version too long" );



bool g_allowPuppetFreeze = false;
volatile LONG g_puppetFreezesStopped = 0;     // Puppets kept out of the ice cube (ThrownFromWater_Hook).

// Leaving a level (see NoteLevelLeaving) : the old one still runs and is shown.
static bool g_levelLeaving = false;
static volatile DWORD g_harryTickAt = 0;   // GetTickCount at Harry's last tick.
static const DWORD PAUSED_AFTER_MS = 300;  // Harry not ticking this long (not for a load) : the game is paused.
static uint32_t g_tickLevel = 0;   // The level's CRC at Harry's last tick (the one shown).

// The level Harry is in, as far as the others are concerned : the game's CRC is
// the new level's as soon as a load is announced, but the old one still runs (and
// Harry in it) a couple of seconds. Saying we were in the new one then showed us
// to its players at our place in the old one (2026-11-16, Twin Outposts -> Turtle
// Monument : Harry running in the middle of the level, nowhere near the loadzone);
// saying we were still in the old one showed the game pulling Harry about in the
// loadzone (rubber-banding). So none : we're hidden until we're in the new one.
static inline uint32_t ShownLevel()
{
	return g_levelLeaving ? 0 : Gizmod::getCurrentLevelCRC();
}

volatile LONG g_cropPending = 0;          // A cropped grab, taken in Present.
char g_cropName[64];
RECT g_crop;

static PlayerSnapshot g_local;                 // Static storage keeps Matrix4f 16-byte aligned.
static bool g_haveLocal = false;

// One per player id the relay hands out (its rooms hold 8). Each other player in
// the room gets their own puppet; with echo (or the loopback), our own id gets one
// too and replays us.

Peer g_peers[MAX_PEERS];
PlayerInfo g_players[MAX_PEERS];

// The peer being ticked or drawn. Between those : the main peer, the one F5, F6, F8
// and the autotest work on (the first player heard from).
static int g_mainId = -1;

Peer* MainPeer()
{
	return &g_peers[g_mainId >= 0 ? g_mainId : 0];
}



// Who is in the room (from their PACKET_INFO), kept across levels. Presence goes by
// those packets (sent even while a player's game is loading or paused), not by
// snapshots.
static const uint32_t INFO_INTERVAL_MS = 1000;
static const uint32_t INFO_TIMEOUT_MS = 15000;
static const uint8_t INFO_PAUSED = 0x01;   // PACKET_INFO's flags.
static const uint32_t PAUSE_GRACE_MS = 3000;   // Unpaused, but no snapshot yet : still kept this long.



// --------------------------------------------------------------------------------
// Outfits.
// --------------------------------------------------------------------------------
const Outfit OUTFITS[] = {
	{ "Harry",       0xC9252A08, 0 },
	{ "Nicole",      0x7BBEDDE3, 0x90DA18 },
	{ "Native",      0x182A7689, 0x90DA19 },
	{ "Retro Harry", 0xBF8D2C68, 0x90DA1F },
	{ "Snowy Harry", 0x94DB2F98, 0 },
};
const int NUM_OUTFITS = countof(OUTFITS);
const int RETRO_OUTFIT = 3;
const int SNOWY_OUTFIT = 4;

static uint32_t g_forceHarryModel = 0;   // The model Harry gets instead of the level's, when a level loads (0 : the level's).
const int OUTFIT_DEFAULT = -1;   // Chosen : whatever the level gives Harry.
int g_myOutfit = OUTFIT_DEFAULT;  // The one chosen (Multiplayer.ini, [Appearance] Outfit, by name) : worn from the next level load.
int g_myWornOutfit = 0;   // The one Harry wears now : what the others are told.

// Retro Harry's colours. His model takes all its colour from a 32x32 texture of four
// flat squares, so each player can have their own (see the Retro palette section).
static const char* const RETRO_PART_NAMES[RETRO_PARTS] = { "Hair", "Head", "Shirt", "Pants" };   // In the menu, and the ini keys (RetroHair...).
static const uint8_t RETRO_DEFAULT[RETRO_PARTS][3] = { { 142, 118, 62 }, { 255, 161, 123 }, { 78, 200, 74 }, { 0, 87, 0 } };
uint8_t g_myColors[RETRO_PARTS][3];   // Multiplayer.ini, [Appearance] RetroHair... (RRGGBB).



// --------------------------------------------------------------------------------
// Toasts.
// Messages shown at the bottom of the screen for a few seconds (e.g. "Bob joined").
// --------------------------------------------------------------------------------

static const int MAX_TOASTS = 4;
static const uint32_t TOAST_MS = 5000;
static struct { char text[96]; uint32_t time; } g_toasts[MAX_TOASTS];

void Toast( const char* fmt, ... )
{
	for ( int i = MAX_TOASTS - 1; i > 0; i-- ) g_toasts[i] = g_toasts[i - 1];
	va_list args;
	va_start( args, fmt );
	vsnprintf_s( g_toasts[0].text, _TRUNCATE, fmt, args );
	va_end( args );
	g_toasts[0].time = NetNowMs();
}



static int CleanName( const char* in, int length, char* out )
{
	int n = 0;
	for ( int i = 0; i < length && n < MAX_NAME; i++ ) {
		char c = in[i];
		if ( c >= 32 && c <= 126 ) out[n++] = c;
	}
	while ( n > 0 && out[n - 1] == ' ' ) n--;
	out[n] = 0;
	return n;
}

static const char* LevelName( uint32_t crc )
{
	if ( !crc ) return "loading";
	uint32_t id = level_get_by_crc( crc );
	return id == (uint32_t) -1 ? "somewhere" : level_get_name( id );
}



// --------------------------------------------------------------------------------
// Animation viewer (F10) : a puppet pinned beside Harry plays one clip at a time,
// for going through Harry's animations and noting what they are.
// --------------------------------------------------------------------------------

bool g_animViewer = false;
Peer* g_viewerPeer = 0;          // The puppet it uses.
static Vector3f g_viewerPin;            // Where that puppet stands.

uint32_t g_animPreviewCRC = 0;   // The clip the puppet plays (0 : normal).

// The Emotes page's preview uses the same : g_viewerPeer, while it's choosing. Between
// previews its puppet is parked out of sight (not drawn, far below), kept for the next.
bool g_emotePreview = false;
Peer* g_parkedPeer = 0;
static Vector3f g_parkedPin;

static bool IsParkedPeer( const Peer* p ) { return p && p == g_parkedPeer && !((g_animViewer || g_emotePreview) && p == g_viewerPeer); }
static bool IsViewerPeer( const Peer* p ) { return ((g_animViewer || g_emotePreview) && p == g_viewerPeer) || IsParkedPeer( p ); }

// The clip the current peer's puppet is made to play (survey, viewer), else 0.
static uint32_t ForcedClip( const Peer* p )
{
	return (IsViewerPeer(p) || TestAnimSurveyOn()) ? g_animPreviewCRC : 0;
}

static Peer* FindPeerFromBeast( const EIEvilHarryP* beast )
{
	for ( int i = 0; i < MAX_PEERS; i++ )
		if ( g_peers[i].beast == beast )
			return &g_peers[i];
	return 0;
}

// Hands a puppet (and its locomotion state, everything from "beast" on) to another peer.
static void MovePuppet( Peer* from, Peer* to )
{
	size_t start = (uint8_t*) &from->beast - (uint8_t*) from;
	memcpy( (uint8_t*) to + start, (uint8_t*) from + start, sizeof(Peer) - start );
	from->beast = 0;
}

static void InitPeers()
{
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		g_peers[i].limboTicks = -1;
	}
}

static void ResetPeer( Peer* p, int id )   // Forgets them (their puppet is left alone).
{
	p->id = id;
	p->heard = false;
	p->lastHeard = 0;
	p->lastLevel = 0;
	p->keptPaused = false;
	p->pausedSeen = 0;
	p->remote.Reset();
	p->target = 0;
	p->lastSpawnTry = 0;
	p->forcedState = -1;
	p->lastLoggedState = p->lastRequested = -2;
	p->tagFrame = 0;
}



static const int DELAY_CHOICES[] = { 133, 250, 500, 1000, 1500, 3000 };   // Playback delay (ms).
int g_delayIndex = 1;   // 250 ms (Multiplayer.ini : Delay).

// The network side.
Transport* g_transport = 0;
static bool g_networked = false;               // Through a server (not the loopback).
static uint32_t g_sendSequence = 0;
// Snapshots go out at most this often (30 per second : the game runs at 60, the
// receiver blends between them). A little under, so frame timing jitter at 60 fps
// doesn't skip one.
static const uint32_t SEND_INTERVAL_MS = 33 - 8;
static uint32_t g_lastSentTime = 0;
static const uint32_t REMOTE_SILENT_MS = 5000; // Then : forget them.
NetStats g_netStats;

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

	TestCheckPacking( g_local );
}

void LogNetStats()
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
	while ( (length = g_transport->Receive(packet, sizeof(packet))) > 0 ) {
		MemoryReader r( packet, length );   // Only what was received : reads past it fail.
		PacketHeader h;
		if ( !DecodeHeader(packet, length, &h) ) { g_netStats.badPackets++; continue; }
		r.seek( PACKET_HEADER_SIZE );
		if ( h.player >= MAX_PEERS ) { g_netStats.badPackets++; continue; }
		Peer* peer = &g_peers[h.player];
		switch ( h.type )
		{
		case PACKET_LEAVE:
			if ( peer->heard || g_players[h.player].present ) {
				mp_log( "[NET] Player %d left.\n", h.player );
				if ( g_players[h.player].present ) Toast( "%s left", PlayerName(h.player) );
				ResetPeer( peer, h.player );   // Their puppet stays hidden (no target).
			}
			g_players[h.player].present = false;
			g_players[h.player].name[0] = 0;
			break;

		case PACKET_CAUGHT:
			if ( h.player == me && !echo ) break;
			if ( length < PACKET_HEADER_SIZE + 1 || packet[PACKET_HEADER_SIZE] >= MAX_PEERS ) { g_netStats.badPackets++; break; }

			HsReceiveCaught( h.player, packet[PACKET_HEADER_SIZE], h.sequence, me );
			break;

		case PACKET_INFO: {
			if ( h.player == me && !echo ) break;
			if ( length < PACKET_HEADER_SIZE + 5 ) { g_netStats.badPackets++; break; }

			PlayerInfo& info = g_players[h.player];

			// Level.
			r.read<uint32_t>( info.level );

			// Player name.
			int nameLength; r.read<uint8_t>(nameLength);
			if ( PACKET_HEADER_SIZE + 5 + nameLength > length ) { g_netStats.badPackets++; break; }
			char name[MAX_NAME + 1];
			CleanName( (const char*) r.getDataPtr(), nameLength, name );
			r.seek( nameLength );

			// Outfit & retro colors.
			r.read<uint8_t>( info.outfit );
			if ( info.outfit >= NUM_OUTFITS ) info.outfit = 0;
			r.read( info.colors, sizeof(info.colors) );

			// Version string.
			char version[MAX_VERSION + 1] = "";
			int versionLength = 0; r.read<uint8_t>( versionLength );
			if ( versionLength > MAX_VERSION || (int) r.getPtr() + versionLength > length ) { g_netStats.badPackets++; break; }
			CleanName( (const char*) r.getDataPtr(), versionLength, version );
			r.seek( versionLength );

			bool versionNews = !info.present || strcmp( info.version, version ) != 0;
			strcpy_s( info.version, version );

			// Gamemode role.
			r.read<uint8_t>( info.role );
			if ( info.role >= ROLE_NONE ) info.role = ROLE_NONE;

			// Hide & seek hide duration.
			info.hideMs = 0;
			if ( info.role == ROLE_HIDER || info.role == ROLE_SEEKER ) {
				r.read<uint32_t>( info.hideMs );
			}

			// Flags (paused, ...).
			uint8_t flags = 0; r.read<uint8_t>( flags );
			info.paused = (flags & INFO_PAUSED);

			info.lastInfo = now;

			// Update presence.
			if ( !info.present || strcmp(info.name, name) != 0 ) {
				mp_log( "[NET] Player %d is \"%s\", in %s.\n", h.player, name, LevelName(info.level) );
				if ( !info.present && h.player != me ) Toast( "%s joined", name );
				strcpy_s( info.name, name );
				info.present = true;
			}
			if ( versionNews ) {
				mp_log( "[NET] Player %d has version %s.\n", h.player, version[0] ? version : "(older, none sent)" );
				if ( h.player != me && strcmp(version, MP_VERSION) != 0 )
					Toast( "%s has %s%s (you : %s)", name, version[0] ? "version " : "an older version", version, MP_VERSION );
			}
			}
			break;

		case PACKET_SNAPSHOT:
			if ( h.player == me && !echo ) break;   // Ourselves.
			if ( !DecodeSnapshot(packet, length, &incoming) ) { g_netStats.badPackets++; break; }
			if ( !peer->heard ) {
				peer->heard = true;
				mp_log( "[NET] Showing player %d.\n", h.player );
				if ( g_mainId < 0 ) {
					// A puppet spawned before anyone was heard from (F5, the autotest) is
					// peer 0's : it now shows this player.
					Peer* first = &g_peers[0];
					if ( h.player != 0 && first->beast && !first->heard && !peer->beast && !PeerInLimbo(peer) ) MovePuppet( first, peer );
					g_mainId = h.player;
				}
			}
			peer->remote.Add( incoming, h.sequence );
			peer->lastHeard = now;
			peer->lastLevel = incoming.level;
			if ( peer->keptPaused ) {
				peer->keptPaused = false;
				mp_log( "[NET] Player %d goes on.\n", h.player );
			}
			break;
		}
	}
	uint32_t level = ShownLevel();
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		PlayerInfo& info = g_players[i];
		if ( info.present && now - info.lastInfo > INFO_TIMEOUT_MS ) {
			mp_log( "[NET] No news from player %d (\"%s\") for %u s : they're gone.\n", i, info.name, INFO_TIMEOUT_MS / 1000 );
			Toast( "%s left (connection lost)", info.name );
			info.present = false;
			info.name[0] = 0;
		}
	}
	if ( me >= 0 && g_caughtOut.left > 0 && now - g_caughtOut.lastSent >= CAUGHT_RESEND_MS ) {   // We were caught.
		g_caughtOut.left--;
		g_caughtOut.lastSent = now;
		uint8_t caught[PACKET_HEADER_SIZE + 1];
		EncodeHeader( PACKET_CAUGHT, (uint8_t) me, g_caughtOut.number, caught );
		caught[PACKET_HEADER_SIZE] = (uint8_t) g_caughtOut.seeker;
		g_transport->Send( caught, sizeof(caught) );
	}
	if ( me >= 0 && now - g_lastInfoSent >= INFO_INTERVAL_MS ) {   // Who we are, where.
		g_lastInfoSent = now;
		uint8_t info[PACKET_HEADER_SIZE + 5 + MAX_NAME + 1 + sizeof(g_myColors) + 1 + MAX_VERSION + 6];
		MemoryWriter w(info, sizeof(info));

		const uint32_t where = g_haveLocal ? level : 0;

		EncodeHeader( PACKET_INFO, (uint8_t) me, 0, info ); w.seek(PACKET_HEADER_SIZE);
		w.write<uint32_t>( where );
		w.write<uint8_t>( g_myNameLen );
		w.write( g_myName, g_myNameLen );
		w.write<uint8_t>( g_myWornOutfit );
		w.write( g_myColors, sizeof(g_myColors) );

		const int v = (int) strlen( g_myVersion );
		w.write<uint8_t>( v );
		w.write( g_myVersion, v );

		LoadHideTime();

		w.write<uint8_t>( PlayingHs() ? g_hsMyRole : ROLE_NONE );   // ROLE_NONE : no hide time follows.
		if ( PlayingHs() ) {
			w.write<uint32_t>( g_hsMyHideMs );
		}

		const bool paused = g_haveLocal && where && !g_levelLeaving && GetTickCount() - g_harryTickAt > PAUSED_AFTER_MS;
		w.write<uint8_t>( paused ? INFO_PAUSED : 0 );

		g_transport->Send( info, w.getPtr() );
	}
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer* peer = &g_peers[i];

		// Silent but paused, here : kept, still (their last snapshot), not forgotten.
		const PlayerInfo& pi = g_players[i];
		const bool silent = peer->heard && now - peer->lastHeard > REMOTE_SILENT_MS;
		bool paused = pi.present && pi.paused && pi.level && pi.level == peer->lastLevel;
		if ( paused ) peer->pausedSeen = now;
		else if ( peer->keptPaused && now - peer->pausedSeen < PAUSE_GRACE_MS ) paused = true;

		if ( silent && paused && !peer->keptPaused ) {
			peer->keptPaused = true;
			mp_log( "[NET] Player %d is paused : kept where they are.\n", i );
		}
		if ( silent && !paused ) {
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
GET_METHOD( 0x6F3A50, int,  AnimMap_Find,     void*, int, AnimLayer** );
GET_METHOD( 0x61CAD0, void, EAnimController_SetClip, EAnimController*, int, uint32_t );

static const int      MAX_LAYER_SCAN  = 256;

static const uint32_t* RENDER_FRAME   = (const uint32_t*) 0x91779C;   // Global frame counter.

Matrix4f* GetPose( EAnimController* animCtrl, int* bones )
{
	ERCharacter* skeleton = animCtrl->m_characterResource;
	*bones = skeleton ? skeleton->m_numBones : 0;
	return animCtrl->m_pose;
}

AnimLayer* GetAnimLayer( EICharacter* c, int key )
{
	AnimLayer* layer = 0;
	TreeMap* map = &c->m_animController.m_animMap;
	if ( !AnimMap_Find(map, key, &layer) ) return 0;
	return layer;
}

uint32_t GetLayerClipCRC( AnimLayer* layer ) { return layer->m_anim ? layer->m_anim->m_crc : 0; }

// Layer 217 holds a frozen pose (its clock never moves) that Harry blends from
// when he switches item animations; mirroring it froze the puppet mid pull-out.
static const int HOLD_POSE_LAYER = 217;

// The clip a character is mostly showing : its highest-weight layer.
uint32_t GetPlayingClip( EICharacter* c, int* outLayer, float* outWeight )
{
	uint32_t best = 0;
	float bestWeight = 0.01F;
	int bestLayer = -1;
	for ( int i = 0; i < MAX_LAYER_SCAN; i++ ) {
		if ( i == HOLD_POSE_LAYER ) continue;
		AnimLayer* layer = GetAnimLayer( c, i );
		if ( !layer ) continue;
		float w = layer->m_weight;
		uint32_t crc = GetLayerClipCRC( layer );
		if ( crc && w > bestWeight ) { best = crc; bestWeight = w; bestLayer = i; }
	}
	if ( outLayer ) *outLayer = bestLayer;
	if ( outWeight ) *outWeight = bestWeight;
	return best;
}


ItemStruct* ITEM_TABLE = (ItemStruct*) 0x8EEB90;
static const int NUM_ITEMS = 9;

#include "ptle/EItemBase.h"
EItemBase* GetItemObject( int index ) { return ITEM_TABLE[index].m_object; }

int GetHeldItem( EIHarry* harry )
{
	EInventory* inv = harry->m_inventory;
	if ( !inv ) return -1;

	EItemBase* held = inv->m_itemInHandNext;
	if ( !held ) return -1;

	for ( int i = 0; i < NUM_ITEMS; i++ )
		if ( GetItemObject(i) == held ) return i;
	return -1;
}

void ApplyMyOutfit();

static void KeepUndrawnItem( EIHarry* harry );

static void PushLocalSnapshot( EIHarry* harry )
{
	KeepUndrawnItem( harry );
	SendLocalSnapshot();   // The previous one, complete now.

	int worn = 0;
	for ( int i = 1; i < NUM_OUTFITS; i++ ) {
		if ( harry->m_modelCRC == OUTFITS[i].model ) {
			worn = i;
			break;
		}
	}
	if ( worn != g_myWornOutfit || !g_haveLocal ) mp_log( "[MP] Harry wears the %s outfit (model 0x%.8X; chosen : %s).\n", OUTFITS[worn].name, harry->m_modelCRC, g_myOutfit < 0 ? "Default" : OUTFITS[g_myOutfit].name );
	g_myWornOutfit = worn;
	ApplyMyOutfit();

	PlayerSnapshot& s = g_local;
	s.time          = NetNowMs();
	s.level         = ShownLevel();
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
	if ( MatInverseAffine(harry->m_transformMatrix, &inverse) ) {
		int nbones = 0;
		Matrix4f* pose = GetPose( &harry->m_animController, &nbones );
		if ( pose && nbones > 0 && nbones <= MAX_CHAR_BONES ) {
			MatsMul( pose, nbones, inverse, s.pose );
			s.bones = nbones;
		}
		MatMul( harry->m_matRHandHold, inverse, &s.handR );
		MatMul( harry->m_matLHand, inverse, &s.handL );
	}
	static int lastItem = -1;
	if ( s.heldItem != lastItem ) {
		mp_log( "[MP] Harry holds item %d.\n", s.heldItem );
		lastItem = s.heldItem;
	}

	g_haveLocal = true;
}

const PlayerSnapshot* GetPuppetTarget( const Peer* peer )
{
	if ( !peer->target && IsViewerPeer(peer) && g_haveLocal ) return &g_local;
	return peer->target;
}



// --------------------------------------------------------------------------------
// Puppet.
// --------------------------------------------------------------------------------

// Index of a state by its short name ("idle left" matches "evil harry.idle left").
int FindStateIndex( EIBeast* beast, const char* shortName )
{
	EBeastStateMachine* sm = beast->GetStateMachine();
	if ( !sm || !sm->m_states ) return -1;

	for ( int i = 0; i < (int) sm->m_numStates; i++ ) {
		const char* name = sm->m_states[i].m_name;
		const char* dot = name ? strchr(name, '.') : 0;

		if ( dot && strcmp(dot + 1, shortName) == 0 ) return i;
	}
	return -1;
}
static char g_puppetName[] = "evil harry";
static char* const GAME_EMPTY_STRING = (char*) 0x9410B0;   // The game strings' "" (0x6F047A).


int g_puppetsSpawned = 0;   // For the autotest.

static void RememberSpawned( EIEvilHarryP* beast, ERLevel* world, int owner );
static void InitLocomotion( EIEvilHarryP* p, EIHarry* harry, Peer* peer );

void SpawnPuppet( Peer* peer )
{
	if ( peer->beast ) {
		mp_log( "[MP] Player %d's puppet already spawned in this level.\n", peer->id );
		return;
	}

	EIEvilHarryP* p = instantiate_object<EIEvilHarryP>();

	EIHarry* harry = Gizmod::getHarry();
	ERLevel* world = harry->m_world;

	p->EIBeast::m_position.x = harry->m_position.x;
	p->EIBeast::m_position.y = harry->m_position.y + 15.0F;
	p->EIBeast::m_position.z = harry->m_position.z;
	p->m_rotation.x = 0.0F;
	p->m_rotation.y = 0.0F;
	p->m_rotation.z = 0.0F;
	p->m_rotation.w = 1.0F;

	p->m_beastTypeCRC = HashPath( "BEvilHarryP" );
	p->m_beastTypeName = g_puppetName;
	EIBeast_LoadBeastAsset( p );
	p->DeserializePart2();

	if ( !p->m_beastType ) {
		mp_log( "[MP] WARNING : Evil Harry asset didn't load in this level, puppet may be invisible.\n" );
	}

	AddToWorld( world, p );
	ERLevel_SetupEntity( world, p, 0 );
	p->Init();
	p->m_beastTypeName = GAME_EMPTY_STRING;

	peer->beast = p;
	peer->shownOutfit = -1;
	g_puppetsSpawned++;
	peer->lastLoggedState = peer->lastRequested = -2;
	RememberSpawned( p, world, peer->id );
	int count = 0;
	for ( int i = 0; i < MAX_PEERS; i++ ) if ( g_peers[i].beast ) count++;
	mp_log( "[MP] Player %d's puppet spawned (%d puppet(s) in this level).\n", peer->id, count );

	// Its default state (21, "get ready for boss fight") draws it as a black
	// silhouette. Start calm and lit instead.
	peer->forcedState = FindStateIndex( p, "idle left" );
	mp_log( "[MP] Puppet starts in state %d (idle left).\n", peer->forcedState );
	InitLocomotion( p, harry, peer );

	if ( count > 1 ) return;
	// Raw dump of the transform-related area, before we touch anything.
	const uint32_t ranges[][2] = { { 0x120, 0x1B0 }, { 0x2F0, 0x350 } };
	for ( int r = 0; r < 2; r++ ) {
		for ( uint32_t o = ranges[r][0]; o < ranges[r][1]; o += 16 ) {
			float* f = (float*) ((uint8_t*) p + o);
			mp_log( "[MP]   +0x%03X : %10.3f %10.3f %10.3f %10.3f\n", o, f[0], f[1], f[2], f[3] );
		}
	}
}

static ERLevel* g_currentWorld = 0;
static uint32_t g_currentCRC = 0;

// Called when Harry's world or the level changes.
static void ForgetPuppets()
{
	int count = 0;
	const DWORD now = GetTickCount();

	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( !p.beast ) continue;

		p.limboSince = now;
		p.limboTicks = 0;
		count++;
	}

	if ( count ) mp_log( "[MP] New level, %d puppet(s) set aside (kept if they're still here).\n", count );
	ResetHistory();
}

// Every puppet spawned in Harry's current world, to catch one nobody drives any more
// (an orphan) : it would stand still, a frozen copy of its player.
struct SpawnedPuppet { EIEvilHarryP* beast; ERLevel* world; int owner; bool reported; };
static const int MAX_SPAWNED = 32;
static SpawnedPuppet g_spawned[MAX_SPAWNED];
static int g_spawnedCount = 0;

static void RememberSpawned( EIEvilHarryP* beast, ERLevel* world, int owner )
{
	int keep = 0;   // Other worlds' are kept too : the old level runs on a moment after a loadzone.
	for ( int i = 0; i < g_spawnedCount; i++ ) if ( g_spawned[i].beast != beast ) g_spawned[keep++] = g_spawned[i];
	g_spawnedCount = keep;
	if ( g_spawnedCount == MAX_SPAWNED ) { memmove( g_spawned, g_spawned + 1, (MAX_SPAWNED - 1) * sizeof(SpawnedPuppet) ); g_spawnedCount--; }
	SpawnedPuppet s = { beast, world, owner, false };
	g_spawned[g_spawnedCount++] = s;
}

// Between a load's announcement and the new level's arrival the old level still
// runs and is shown a couple of seconds, while the level's CRC is already the new
// one : a puppet of a player there was drawn at their place in the new level, in
// the old one's scenery (a Harry running into the loadzone just left; 2026-11-16,
// Turtle Monument <-> Twin Outposts). Puppets aren't shown in that time.
static void* g_levelLeavingWorld = 0;
static DWORD g_levelLeavingSince = 0;
static const DWORD LEVEL_LEAVING_MAX_MS = 8000;   // Should Harry's world not change (it does).

static void NoteLevelLeaving( uint32_t newLevel )
{
	EIHarry* harry = Gizmod::getHarry();
	if ( !harry || !g_tickLevel || newLevel == g_tickLevel ) return;   // Coming back to life here, or already there.
	g_levelLeaving = true;
	g_levelLeavingWorld = harry->m_world;
	g_levelLeavingSince = GetTickCount();
	mp_log( "[MP] Leaving the level : puppets hidden until the next one is up.\n" );
}

static void* g_prevWorld = 0;   // Harry's world before this one.

static void CheckLevelLeaving( ERLevel* myWorld )   // Each Harry tick.
{
	static ERLevel* world = 0;
	if ( myWorld != world ) { g_prevWorld = world; world = myWorld; }
	if ( !g_levelLeaving || myWorld != g_levelLeavingWorld ) g_tickLevel = Gizmod::getCurrentLevelCRC();
	if ( !g_levelLeaving ) return;
	bool arrived = myWorld != g_levelLeavingWorld;
	if ( !arrived && GetTickCount() - g_levelLeavingSince < LEVEL_LEAVING_MAX_MS ) return;
	g_levelLeaving = false;
	mp_log( "[MP] %s : puppets shown again.\n", arrived ? "In the new level" : "Still no new level after 8 s" );
}

static Peer* ReclaimPuppet( EIEvilHarryP* beast )
{
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( p.beast != beast || !PeerInLimbo(&p) ) continue;
		p.limboTicks = -1;
		p.lastLoggedState = p.lastRequested = -2;

		mp_log( "[MP] Player %d's puppet is still there : kept.\n", i );
		return &p;
	}
	return 0;
}

// A puppet the game stopped ticking while its player is here : a pitfall caught it
// as they were eaten on their screen (it stops a character it catches; ours stayed
// in the pit, not drawn, and its player was never shown again until a reload). Far
// away or not, a puppet ticks every frame, so two seconds without one means it's
// gone : it's set aside (kept if it ticks again) and its player gets a new one.
static const DWORD PUPPET_STALL_MS = 2000;

static void ReplaceStalledPuppets()
{
	const DWORD now = GetTickCount();
	static DWORD lastCall = 0;
	bool paused = now - lastCall > 500;   // Harry didn't tick either (a pause, a load) : nothing stalled.
	lastCall = now;
	if ( g_levelLeaving ) return;

	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( p.beast != p.watched || paused ) {
			p.watched = p.beast;
			p.lastTick = now;
		}
		if ( !p.beast || now - p.lastTick <= PUPPET_STALL_MS ) continue;

		mp_log( "[MP] Player %d's puppet 0x%.8X hasn't ticked for %u ms (a pitfall caught it?) : replaced.\n", i, p.beast, now - p.lastTick );
		p.watched = 0;
		p.limboSince = now;
		p.limboTicks = 0;
	}
}

// Scan puppets that have been in limbo for too long.
// In this case, we can assume they've been destroyed by a level switch.
static void DropLostPuppets()
{
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( !PeerInLimbo(&p) ) continue; 

		if ( ++p.limboTicks <= 30 || GetTickCount() - p.limboSince <= 1000 ) continue;
		p.beast = 0;
		p.limboTicks = -1;
		mp_log( "[MP] Player %d's puppet went with the old level.\n", i );
	}
}


GET_METHOD( 0x60FEA0, void, EICharacter_SetPos, EICharacter*, const Vector3f*, bool );
GET_METHOD( 0x60FFA0, void, EICharacter_SetRot, EICharacter*, const Vector3f* );

static bool GetPuppetTargetPosition( Vector3f* out, Peer* peer )
{
	const PlayerSnapshot* t = GetPuppetTarget( peer );
	if ( !t ) return false;
	if ( IsParkedPeer(peer) ) { *out = g_parkedPin; return true; }
	if ( IsViewerPeer(peer) ) { *out = g_viewerPin; return true; }
	out->x = t->position.x;
	out->y = t->position.y;
	out->z = t->position.z;
	TestMovePuppetTarget( out );
	return true;
}


// --------------------------------------------------------------------------------
// Locomotion : the puppet's animation and facing come from how its target moves
// (later : how the remote player moves). x/y is the ground plane, z is up.
// --------------------------------------------------------------------------------
const float MP_PI = 3.14159265F;

bool  g_autoLocomotion = true;   // F6 switches to hand-forced states.
// The rest of the locomotion state is per puppet (Peer).
static const int AIRBORNE_MIN_TICKS = 6;   // Slopes and bumps blip for up to ~5 ticks.
static const float JUMP_RISE_SPEED  = 0.10F;   // Units per tick upward while off the ground.

static const float RUN_START_SPEED = 0.05F;  // Units per tick (hysteresis).
static const float RUN_STOP_SPEED  = 0.02F;
// Harry's normal run measured about 0.36/tick (peaks near 0.44 on slopes).
const float SPRINT_START_SPEED = 0.50F;
static const float SPRINT_STOP_SPEED  = 0.45F;
static const float TELEPORT_DIST   = 5.0F;   // Bigger jumps aren't movement.

// Angle of the model's forward axis in its own frame (0 = +X, pi/2 = +Y).
// +Y made it run backwards, so the model faces local -Y.
float g_forwardLocalAngle = -MP_PI / 2.0F;

static void ResetLocomotion( Peer* peer )
{
	peer->velX = peer->velY = peer->moveSpeed = 0.0F;
	peer->moving = false;
	peer->sprinting = false;
	peer->peakSpeed = 0.0F;
	peer->airborne = false;
	peer->airTicks = 0;
	peer->haveLastTarget = false;
}

static void UpdateLocomotion( EIEvilHarryP* p, Peer* peer )
{
	Vector3f pos;
	if ( !GetPuppetTargetPosition(&pos, peer) ) return;

	if ( peer->haveLastTarget ) {
		float dx = pos.x - peer->lastTarget.x;
		float dy = pos.y - peer->lastTarget.y;
		if ( dx * dx + dy * dy > TELEPORT_DIST * TELEPORT_DIST ) dx = dy = 0.0F;

		peer->velX = peer->velX * 0.75F + dx * 0.25F;
		peer->velY = peer->velY * 0.75F + dy * 0.25F;
		peer->moveSpeed = sqrtf( peer->velX * peer->velX + peer->velY * peer->velY );
		if ( peer->moveSpeed > 0.01F ) peer->heading = atan2f( peer->velY, peer->velX );
	}
	// Face where Harry faces, not where he travels : he turns on the spot, walks
	// backwards a step, and in a raft paddles one side and turns freely. His
	// transform's row 1 points opposite to where he faces (see InitLocomotion).
	if ( const PlayerSnapshot* f = GetPuppetTarget(peer) ) {
		float fx = -f->transform.data[1][0], fy = -f->transform.data[1][1];
		if ( fx * fx + fy * fy > 0.01F ) peer->heading = atan2f( fy, fx );
	}
	peer->lastTarget = pos;
	peer->haveLastTarget = true;

	bool wasMoving = peer->moving;
	if ( peer->moving && peer->moveSpeed < RUN_STOP_SPEED ) peer->moving = false;
	else if ( !peer->moving && peer->moveSpeed > RUN_START_SPEED ) peer->moving = true;
	if ( peer->moving != wasMoving ) {
		if ( peer->moving ) {
			mp_log( "[MP] Puppet %d starts moving (speed %.3f/tick, heading %.0f deg).\n", peer->id, peer->moveSpeed,peer->heading * 180.0F / MP_PI );
			peer->peakSpeed = 0.0F;
		}
		else {
			mp_log( "[MP] Puppet %d stops (top speed %.3f/tick).\n", peer->id, peer->peakSpeed );
		}
	}
	if ( peer->moving && peer->moveSpeed > peer->peakSpeed ) peer->peakSpeed = peer->moveSpeed;

	// Airborne : the replayed player has been off the ground for a few ticks
	// (ignores one-tick blips on bumps and slopes). Landing is immediate.
	const PlayerSnapshot* t = GetPuppetTarget( peer );
	bool wasAirborne = peer->airborne;
	peer->airTicks = (t && !t->onGround) ? peer->airTicks + 1 : 0;
	// A real jump rises fast from its first tick (~0.25/tick) : switch at once then,
	// instead of waiting AIRBORNE_MIN_TICKS (which lands near the top of the jump).
	float dz = pos.z - peer->prevZ;
	peer->prevZ = pos.z;
	if ( peer->airTicks == 0 ) peer->airborne = false;
	else if ( peer->airTicks >= AIRBORNE_MIN_TICKS || dz > JUMP_RISE_SPEED ) peer->airborne = true;
	if ( peer->airborne != wasAirborne ) {
		if ( peer->airborne ) {
			peer->airStartZ = pos.z;
			mp_log( "[MP] Puppet leaves the ground (z %.2f).\n", pos.z );
		}
		else {
			mp_log( "[MP] Puppet lands (z %.2f, %.2f from take-off).\n", pos.z, pos.z - peer->airStartZ );
		}
	}

	bool wasSprinting = peer->sprinting;
	if ( !peer->moving || (peer->sprinting && peer->moveSpeed < SPRINT_STOP_SPEED) ) peer->sprinting = false;
	else if ( !peer->sprinting && peer->moveSpeed > SPRINT_START_SPEED ) peer->sprinting = true;
	if ( peer->sprinting != wasSprinting && peer->moving ) {
		mp_log( "[MP] Puppet %s (speed %.3f/tick).\n", peer->sprinting ? "sprints" : "back to running", peer->moveSpeed );
	}

	// The state machine always stays in a calm idle state : forcing boss-fight
	// states like "runner" crashes (they expect setup we don't do). Running is
	// done by switching animation layers instead (ApplyLocomotionAnim).
	if ( g_autoLocomotion ) {
		peer->forcedState = peer->idleState;
	}
}

// Beast animation layers (from the beast's own PlayAnim, 0x41A820). The animation
// controller holds one layer per state-machine state (layer = EBeastState +0x38,
// set to the state's index), then the "locomotion" layers from the beast's
// quickdata at [+0x52C] : +0 move (CRC at +0x494), +1 idle (CRC at +0x498),
// +2 turn left, +3 turn right. [+0x530] is the layer currently playing.
GET_METHOD( 0x61D0D0, void, EAnimController_FadeTo,  EAnimController*, int, float );
GET_METHOD( 0x61D8A0, void, EAnimController_Restart, EAnimController*, int );

static int GetStateAnimLayer( EIBeast* p, int state )
{
	EBeastStateMachine* sm = p->GetStateMachine();
	if ( !sm || !sm->m_states || state < 0 || state >= (int) sm->m_numStates ) return -1;
	return sm->m_states[state].m_animID;
}

// Crossfades to an animation layer, like the beast's PlayAnim does.
static void PlayAnimLayer( EIEvilHarryP* p, int layer )
{
	EAnimController* ctrl = &p->m_animController;
	int current = p->m_animIndex;
	if ( layer < 0 || layer == current ) return;

	if ( current >= 0 ) EAnimController_FadeTo( ctrl, current, 0.0F );
	EAnimController_Restart( ctrl, layer );
	EAnimController_FadeTo( ctrl, layer, 1.0F );
	p->m_animIndex = layer;
}

// Mirroring : the puppet plays the replayed player's own clip (Harry's jump, climb,
// etc. : Evil Harry's state clips have no ordinary jump). Two spare layers take
// turns so the old clip can fade out while the new one fades in.
static bool g_mirrorClips = true;
const int MIRROR_LAYERS[2] = { 200, 201 };

static bool ApplyMirroredClip( EIEvilHarryP* p, Peer* peer )
{
	const PlayerSnapshot* t = GetPuppetTarget( peer );
	uint32_t forced = ForcedClip( peer );
	uint32_t clip = forced ? forced : (t ? t->clipCRC : 0);
	if ( !g_mirrorClips || !clip ) return false;

	int current = p->m_animIndex;
	if ( clip == peer->mirrorClip && current == MIRROR_LAYERS[peer->mirrorSlot] ) return true;

	EAnimController* ctrl = &p->m_animController;
	int next = (current == MIRROR_LAYERS[peer->mirrorSlot]) ? 1 - peer->mirrorSlot : peer->mirrorSlot;
	EAnimController_SetClip( ctrl, MIRROR_LAYERS[next], clip );
	PlayAnimLayer( p, MIRROR_LAYERS[next] );
	peer->mirrorSlot = next;
	peer->mirrorClip = clip;
	return true;
}

static void ApplyLocomotionAnim( EIEvilHarryP* p, Peer* peer )
{
	static int overridden = 0;
	if ( ApplyMirroredClip(p, peer) ) return;

	int wanted = peer->idleLayer;
	if ( peer->airborne && peer->airLayer >= 0 ) wanted = peer->airLayer;
	else if ( peer->moving ) wanted = (peer->sprinting && peer->sprintLayer >= 0) ? peer->sprintLayer : peer->runLayer;
	int current = p->m_animIndex;
	if ( wanted < 0 || wanted == current ) return;

	// The idle state may re-play its own animation; count it to spot fights.
	if ( (current == peer->idleLayer || current == peer->runLayer || current == peer->sprintLayer || current == peer->airLayer) == false ) {
		if ( ++overridden <= 5 ) mp_log( "[MP] Puppet anim layer was %d, playing %d.\n", current, wanted );
	}
	PlayAnimLayer( p, wanted );
}

static void SetRunAnimation( EIEvilHarryP* p, const char* stateName, Peer* peer )
{
	if ( !stateName ) {
		int base = p->m_locoBase;
		peer->runLayer = p->m_animMoveCRC ? base : -1;
		mp_log( "[MP] Run animation : quickdata move anim, layer %d.\n", peer->runLayer );
	}
	else {
		peer->runLayer = GetStateAnimLayer( p, FindStateIndex(p, stateName) );
		mp_log( "[MP] Run animation : \"%s\" state's anim, layer %d.\n", stateName, peer->runLayer );
	}
}

// Standing animation. 0 = the beast's quickdata idle (locomotion layer +1); the
// "idle left/right" states' animation is the wall-climbing idle (pickaxes out).
void SetIdleAnimation( EIEvilHarryP* p, const char* stateName, Peer* peer )
{
	if ( !stateName ) {
		int base = p->m_locoBase;
		peer->idleLayer = p->m_animIdleCRC ? base + 1 : -1;
		mp_log( "[MP] Idle animation : quickdata idle anim, layer %d.\n", peer->idleLayer );
	}
	else {
		peer->idleLayer = GetStateAnimLayer( p, FindStateIndex(p, stateName) );
		mp_log( "[MP] Idle animation : \"%s\" state's anim, layer %d.\n", stateName, peer->idleLayer );
	}
}

void SetAirAnimation( EIEvilHarryP* p, const char* stateName, Peer* peer )
{
	peer->airLayer = GetStateAnimLayer( p, FindStateIndex(p, stateName) );
	mp_log( "[MP] Air animation : \"%s\" state's anim, layer %d.\n", stateName, peer->airLayer );
}

static void InitLocomotion( EIEvilHarryP* p, EIHarry* harry, Peer* peer )
{
	ResetLocomotion( peer );
	g_autoLocomotion = true;
	peer->mirrorClip = 0;
	peer->mirrorSlot = 0;
	peer->idleState = FindStateIndex( p, "idle left" );
	peer->idleLayer = GetStateAnimLayer( p, peer->idleState );

	mp_log( "[MP] Beast anims : move CRC 0x%.8X, idle CRC 0x%.8X, locomotion base layer %d, current layer %d, idle layer %d, runner layer %d.\n",
		p->m_animMoveCRC, p->m_animIdleCRC, p->m_locoBase, p->m_animIndex,
		peer->idleLayer, GetStateAnimLayer(p, FindStateIndex(p, "runner")) );

	SetIdleAnimation( p, 0, peer );
	if ( peer->idleLayer < 0 ) peer->idleLayer = GetStateAnimLayer( p, peer->idleState );

	// The quickdata move animation if the beast has one (Evil Harry doesn't), else
	// the "chase player" state's : the most natural upright run of the candidates
	// ("runner" is hunched, "superrun" is a dive).
	SetRunAnimation( p, 0, peer );
	if ( peer->runLayer < 0 ) SetRunAnimation( p, "chase player", peer );

	// Above sprint speed : the "superrun to spot to ice pick" state's clip.
	peer->sprintLayer = GetStateAnimLayer( p, FindStateIndex( p, "superrun to spot to ice pick" ) );
	mp_log( "[MP] Sprint animation : layer %d (above %.2f/tick).\n", peer->sprintLayer, SPRINT_START_SPEED );

	// In the air (jumping, falling) : the "falling" state's clip.
	SetAirAnimation( p, "falling", peer );

	// Face where Harry faces. Walking showed his rigid body's row 1 (local +Y)
	// points where he moves; m_transformMatrix is turned 180 degrees from that.
	const Matrix4f& m = harry->m_rigidBodyMatrix;
	peer->heading = atan2f( m.data[1][1], m.data[1][0] );
	mp_log( "[MP] Locomotion : idle state %d, heading %.0f deg.\n", peer->idleState, peer->heading * 180.0F / MP_PI );
}



// Writes the beast's world transform : facing g_cur->heading, at the target position.
// Rolling as a ball : Harry's display matrix (m_transformMatrix) is raised 1.2
// above his position and spins about his side axis, while his position and
// rigid-body matrix stay upright. Detected by that raise.
static const float BALL_RAISE_MIN = 0.5F;

static inline bool IsBallMode( const PlayerSnapshot* t )
{
	return t && t->transform.data[3][2] - t->position.z > BALL_RAISE_MIN;
}

static void SetBeastWorldMatrix( Peer* peer, EIEvilHarryP* p, const Vector3f& pos, const PlayerSnapshot* t )
{
	Matrix4f& world = p->m_worldMatrix;
	if ( IsBallMode(t) ) {
		// Take Harry's spinning, raised matrix as is (same skeleton, same forward axis).
		world = t->transform;
		world.data[3][0] = pos.x + (t->transform.data[3][0] - t->position.x);
		world.data[3][1] = pos.y + (t->transform.data[3][1] - t->position.y);
		world.data[3][2] = pos.z + (t->transform.data[3][2] - t->position.z);
		return;
	}
	float a = peer->heading - g_forwardLocalAngle;
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
static void PrePositionPuppet( EIEvilHarryP* p, Peer* peer )
{
	Vector3f pos;
	if ( GetPuppetTargetPosition(&pos, peer) ) {
		p->m_position = pos;
		SetBeastWorldMatrix( peer, p, pos, GetPuppetTarget(peer) );
		p->m_previousPosition = pos;
	}
}

// Body collision. The beast's "collision off" routine (0x418840, used when it dies
// or goes limp) clears the byte at +0x4C4 and its "on" routine (0x418910) sets it
// back; with it at 0 Harry walks straight through. (The collision layer masks at
// +0x230/+0x564/+0x574/+0x584 made no difference, cleared or not.)
// The puppet is only a visual, so it is never solid.
bool g_puppetSolid = false;                 // Autotest flips it to compare.

static inline void ApplyPuppetCollision( EIEvilHarryP* p )
{
	p->m_enableCollision = g_puppetSolid;
}

// EInstance flags (offset 0x2C).
const uint32_t INSTANCE_VISIBLE = 0x1;

static inline void ShowModel( EInstance* model, bool show )
{
	uint32_t flags = model->m_flags;
	model->m_flags = show ? (flags | INSTANCE_VISIBLE) : (flags & ~INSTANCE_VISIBLE);
}

static void ApplyPuppetItem( EIEvilHarryP* p )
{
	if ( p->m_pickaxeModel0 ) ShowModel( p->m_pickaxeModel0, false );
	if ( p->m_pickaxeModel1 ) ShowModel( p->m_pickaxeModel1, false );
}

// The puppet shows Harry's pose from the snapshot : his bones relative to his
// transform, placed on the puppet's. Both share one skeleton (76 bones), so this
// copies everything his clips blend together (taking items out, moving them with
// the item keys, etc.), which mirroring his main clip alone can't. Done right
// before the puppet renders, and marked posed for this frame so the game doesn't
// pose it over them. (The mirrored clip still runs underneath.)
int g_puppetPoseCopies = 0;   // For the autotest.

static void ApplyPuppetPose( EIEvilHarryP* p, const PlayerSnapshot* t )
{
	EAnimController* animCtrl = &p->m_animController;
	int bones = 0;
	Matrix4f* pose = GetPose( animCtrl, &bones );
	if ( !pose || !t->bones || bones != t->bones ) return;
	MatsMul( t->pose, bones, p->m_transformMatrix, pose );
	animCtrl->m_frameStamp = *RENDER_FRAME;
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
static const uint32_t ITEM_SLOT_DRAW = 4 * 4;
static Matrix4f g_puppetItemPose[MAX_ITEM_BONES];     // The copy's bones (static : 16-byte aligned).

// "anim" : offset of the item's animated model (an EAnimController), 0 if none.
// "shown" : a byte its draw needs set (cleared as soon as Harry's is put away, so
// the copy forces it), 0 if none. (The raft's is "inflated" : without it, its draw
// only poses it.)
// The stink bomb isn't in the wheel menu, but costs nothing to cover.
static const struct { int item; uint32_t vtable; uint32_t anim; uint32_t shown; } DRAWN_ITEMS[] = {
	{ InventoryItem::TORCH,      0x89A200, 0,    0     },
	{ InventoryItem::TNT,        0x89A160, 0,    0     },
	{ InventoryItem::RAFT,       0x8949B0, 0xA4, 0x108 },
	{ InventoryItem::SLING,      0x8971D0, 0xA0, 0x104 },
	{ InventoryItem::SHIELD,     0x896F90, 0xA4, 0x108 },
	{ InventoryItem::GAS_MASK,   0x87CAD8, 0,    0     },
	{ InventoryItem::CANTEEN,    0x86DCB0, 0,    0     },
	{ InventoryItem::STINK_BOMB, 0x8994B8, 0,    0     },
	{ InventoryItem::PICKAXES,   0x88ED70, 0,    0     },
};
static const int NUM_DRAWN_ITEMS = sizeof(DRAWN_ITEMS) / sizeof(DRAWN_ITEMS[0]);
static ItemDrawFunc g_origItemDraw[NUM_DRAWN_ITEMS];

int g_puppetItemDraws = 0;     // For the autotest.
int g_aimItemDraws = 0;
int g_copyDrawsAlone = 0;      // For the autotest : copies drawn while Harry draws no item.
static DWORD g_harryItemDrawTime = 0;

static void NoteHarryItemDraw( int slot, EItemBase* item )
{
	EIHarry* owner = item->m_harry;
	PlayerSnapshot* s = LatestSnapshot();
	if ( !owner || owner != Gizmod::getHarry() || !s ) return;
	// Until it has inflated, the raft's draw only poses it (something else shows it).
	if ( DRAWN_ITEMS[slot].item == InventoryItem::RAFT && !((EItemRaft*) item)->m_inflated ) return;
	Matrix4f inverse;
	if ( !MatInverseAffine( owner->m_transformMatrix, &inverse ) ) return;

	s->itemBones = 0;
	if ( DRAWN_ITEMS[slot].anim ) {
		int bones = 0;
		Matrix4f* pose = GetPose( (EAnimController*) ((uint8_t*) item + DRAWN_ITEMS[slot].anim), &bones );
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
	CopyMatrix( world, item->m_matrix );
	MatMul( world, inverse, &s->itemLocal );
	s->itemState = item->m_state;
	s->drawnSlot = slot;
	g_harryItemDrawTime = GetTickCount();
}

// The game doesn't draw Harry's item when it doesn't need to (on the raft beside a
// wall, the camera goes into his head : the raft isn't drawn), but it's still out :
// the others kept seeing his raft vanish. While the item last drawn is still out,
// a frame without its draw sends it as it was last drawn (relative to Harry).
static struct { int slot; int itemState; int itemBones; Matrix4f itemLocal; Matrix4f itemPose[MAX_ITEM_BONES]; } g_lastItemDraw = { -1 };

static void KeepUndrawnItem( EIHarry* harry )   // On the snapshot about to be sent.
{
	if ( !g_haveLocal ) return;
	PlayerSnapshot& s = g_local;
	if ( s.drawnSlot >= 0 ) {   // Drawn : noted.
		g_lastItemDraw.slot = s.drawnSlot;
		g_lastItemDraw.itemState = s.itemState;
		g_lastItemDraw.itemBones = s.itemBones;
		g_lastItemDraw.itemLocal = s.itemLocal;
		if ( s.itemBones > 0 ) memcpy( g_lastItemDraw.itemPose, s.itemPose, s.itemBones * sizeof(Matrix4f) );
		return;
	}
	int slot = g_lastItemDraw.slot;
	if ( slot < 0 ) return;
	int index = DRAWN_ITEMS[slot].item;
	EItemBase* item = GetItemObject( index );
	if ( !item || GetHeldItem(harry) != index || item->m_state != 3 ) { g_lastItemDraw.slot = -1; return; }   // Put away.
	s.drawnSlot = slot;
	s.itemState = g_lastItemDraw.itemState;
	s.itemBones = g_lastItemDraw.itemBones;
	s.itemLocal = g_lastItemDraw.itemLocal;
	if ( s.itemBones > 0 ) memcpy( s.itemPose, g_lastItemDraw.itemPose, s.itemBones * sizeof(Matrix4f) );
	static DWORD lastLog = 0;
	if ( GetTickCount() - lastLog > 5000 ) {
		lastLog = GetTickCount();
		mp_log( "[MP] Harry's item %d is out but wasn't drawn (camera in his head?) : sent as last drawn.\n", index );
	}
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
	EItemBase* item = GetItemObject( index );
	EIHarry* harry = Gizmod::getHarry();
	if ( !item || !harry ) return;

	// Animated items : the snapshot's bones, marked posed so the draw keeps them.
	EAnimController* anim = (EAnimController*) (DRAWN_ITEMS[slot].anim ? (uint8_t*) item + DRAWN_ITEMS[slot].anim : 0);
	Matrix4f* harryPose = 0;
	uint32_t stamp = 0;
	if ( anim ) {
		int bones = 0;
		harryPose = GetPose( anim, &bones );   // (None yet if Harry never took his out : the copy brings its own.)
		if ( !t->itemBones || bones != t->itemBones ) return;
		MatsMul( t->itemPose, bones, base, g_puppetItemPose );
		stamp = anim->m_frameStamp;
		anim->m_pose = g_puppetItemPose;
		anim->m_frameStamp = *RENDER_FRAME;
	}

	Matrix4f savedMatrix, savedRight, savedLeft;
	SwapMatrix( &item->m_matrix, t->itemLocal, base, &savedMatrix );
	bool hands = index == InventoryItem::PICKAXES;   // They draw on Harry's hands.
	if ( hands ) {
		SwapMatrix( &harry->m_matRHandHold, t->handR, base, &savedRight );
		SwapMatrix( &harry->m_matLHand, t->handL, base, &savedLeft );
	}
	int savedState = item->m_state;
	bool savedActive = item->m_active;
	uint32_t shown = DRAWN_ITEMS[slot].shown;
	bool savedShown = shown ? FIELD(item, bool, shown) : false;
	item->m_state = t->itemState;
	item->m_active = true;
	if ( shown ) FIELD(item, bool, shown) = true;

	g_origItemDraw[slot]( item, context, arg );
	g_puppetItemDraws++;
	if ( GetTickCount() - g_harryItemDrawTime > 150 ) g_copyDrawsAlone++;

	if ( shown ) FIELD(item, bool, shown) = savedShown;
	item->m_active = savedActive;
	item->m_state = savedState;
	if ( hands ) {
		CopyMatrix( harry->m_matLHand, savedLeft );
		CopyMatrix( harry->m_matRHandHold, savedRight );
	}
	CopyMatrix( item->m_matrix, savedMatrix );
	if ( anim ) {
		anim->m_pose = harryPose;
		anim->m_frameStamp = stamp;
	}
}

// Draws the item the puppet's snapshot says Harry drew, if it is "slot".
static void DrawPuppetItem( Peer* peer, int slot, void* context, void* arg )
{
	if ( !peer->beast ) return;
	const PlayerSnapshot* t = GetPuppetTarget( peer );
	if ( !t || t->drawnSlot != slot ) return;
	DrawItemAt( slot, t, peer->beast->m_transformMatrix, context, arg );
}

// The copies are drawn by the puppets' render (the raft's too : it used to be drawn
// right after Harry's own raft, so the other players' rafts vanished whenever Harry
// put his away).
#define ITEM_DRAW_HOOK( n ) \
	static void __fastcall ItemDraw_Hook##n( EItemBase* item, void*, void* context, void* arg ) \
	{ \
		if ( TestHidesHarryItem(item) ) return; \
		g_origItemDraw[n]( item, context, arg ); \
		NoteHarryItemDraw( n, item ); \
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
const uint32_t RENDERER_DEVICE = 0x926790 + 0x74;
static const float TAG_ABOVE_FEET = 5.6F;   // The top of Harry's head (measured on screenshots).
uint32_t g_presentCount = 1;

// The camera : the game builds its matrix each frame with a look-at (0x6EE560,
// called from 0x449B9A : eye, target, up), the camera's own world matrix (it looks
// down its -Z). Its projection is captured below (right-handed : w is minus the
// view depth; 50 degrees, 4:3, whatever the window's shape). The matrix at 0x926A40
// looks like one but isn't the main view's.
//
// Each camera mode builds it with its own call (about thirty : the usual one behind
// Harry, 0x449B9A, but also looking around and aiming in first person, and others),
// all into a camera's matrix at +0xE0. Only hooking the usual one left the camera
// as it was when another mode took over : aiming the TNT, another player's tag
// jumped onto Harry's head. All are hooked. Most build a camera of their own (the
// mode's), which the usual call then follows : the main camera is the one it
// writes, used whenever it was built this frame; otherwise the last camera built
// this frame. A frame where none was (a cutscene, its own camera) shows no tags.
typedef Matrix4f* (__thiscall *LookAtFunc)( Matrix4f*, Vector3f*, Vector3f*, Vector3f* );
static const uint32_t CAMERA_LOOKAT_CALL = 0x449B9A;
const uint32_t CAMERA_LOOKAT_CALLS[] = {
	0x449B9A, 0x449BC4, 0x456169, 0x457065, 0x457783, 0x457A58, 0x4582BE, 0x4584E7, 0x459A08, 0x45AC65,
	0x45B30C, 0x45B6E7, 0x45B774, 0x45C9F2, 0x45D6B1, 0x45D956, 0x45E46D, 0x45E7E0, 0x460815, 0x4609BA,
	0x460BDA, 0x461AF0, 0x461EE7, 0x462720, 0x4645D4, 0x465106, 0x4657AF, 0x466054, 0x46736F, 0x467C7D,
	0x677851, 0x677B58,
};
const int NUM_CAMERA_LOOKAT_CALLS = sizeof(CAMERA_LOOKAT_CALLS) / sizeof(CAMERA_LOOKAT_CALLS[0]);
static const LookAtFunc CameraLookAt = (LookAtFunc) 0x6EE560;
Matrix4f g_cameraWorld;
bool g_haveCamera = false;
uint32_t g_cameraSite = 0;           // The call that built it.
static Matrix4f* g_mainCamera = 0;          // What the usual call (0x449B9A) writes.
static uint32_t g_mainCameraFrame = 0;
int g_cameraSiteCount[NUM_CAMERA_LOOKAT_CALLS];

// Diagnostics (name tags near water) : the look-ats of the frame, and where the
// puppet was drawn. Reset by the survey; logged per level.
TagDiag g_tagDiag;
bool g_tagDiagOn = false;

static Matrix4f* __fastcall CameraLookAt_Hook( Matrix4f* self, void*, Vector3f* eye, Vector3f* target, Vector3f* up )
{
	Matrix4f* m = CameraLookAt( self, eye, target, up );
	uint32_t site = (uint32_t) _ReturnAddress() - 5;
	for ( int i = 0; i < NUM_CAMERA_LOOKAT_CALLS; i++ ) {
		if ( CAMERA_LOOKAT_CALLS[i] != site ) continue;
		if ( g_cameraSiteCount[i]++ == 0 ) mp_log( "[MP] Camera built by 0x%.8X (camera matrix 0x%.8X), eye (%.1f, %.1f, %.1f) : first time.\n", site, self, eye->x, eye->y, eye->z );
		break;
	}
	if ( site == CAMERA_LOOKAT_CALL ) g_mainCamera = self;
	bool main = self == g_mainCamera;
	if ( main ) g_mainCameraFrame = g_presentCount;
	else if ( g_mainCameraFrame == g_presentCount ) return m;   // The main camera is this frame's.
	g_cameraWorld = *m;
	g_haveCamera = true;
	g_cameraSite = site;
	if ( g_tagDiagOn ) {
		if ( g_tagDiag.lookAtsThisFrame < 4 ) g_tagDiag.eyes[g_tagDiag.lookAtsThisFrame] = *eye;
		g_tagDiag.lookAtsThisFrame++;
		g_tagDiag.lookAts++;
	}
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

// The renderer's own view (what it draws with) : set through 0x64A480 (a view
// matrix), 0x64A4C0 (a camera matrix) or 0x64A500 (a look-at), each keeping the
// view at +0xB0 and the camera's matrix at +0xF0.
Matrix4f g_rendCam;
uint32_t g_rendFrame = 0, g_rendSets = 0, g_rendSetsThisFrame = 0, g_rendSetsMax = 0;
void* g_rendSelf = 0;
static Vector3f g_rendEyes[4];
int g_rendTwiceLogged = 0;
static void NoteRendererView( void* self )
{
	if ( g_rendFrame != g_presentCount ) { g_rendSetsThisFrame = 0; g_rendFrame = g_presentCount; }
	if ( g_rendSetsThisFrame < 4 ) g_rendEyes[g_rendSetsThisFrame] = *(Vector3f*) ((uint8_t*) self + 0xF0 + 0x30);
	g_rendSetsThisFrame++;
	if ( g_rendSetsThisFrame > g_rendSetsMax ) g_rendSetsMax = g_rendSetsThisFrame;
	g_rendSets++;
	g_rendSelf = self;
	g_rendCam = *(Matrix4f*) ((uint8_t*) self + 0xF0);
}
typedef void (__thiscall *RendSetMatrixFunc)( void*, Matrix4f* );
typedef void (__thiscall *RendLookAtFunc)( void*, Vector3f*, Vector3f*, Vector3f* );
static void __fastcall RendSetView_Hook( void* self, void*, Matrix4f* m ) { ((RendSetMatrixFunc) 0x64A480)( self, m ); NoteRendererView( self ); }
static void __fastcall RendSetCamera_Hook( void* self, void*, Matrix4f* m ) { ((RendSetMatrixFunc) 0x64A4C0)( self, m ); NoteRendererView( self ); }
static void __fastcall RendLookAt_Hook( void* self, void*, Vector3f* e, Vector3f* t, Vector3f* u ) { ((RendLookAtFunc) 0x64A500)( self, e, t, u ); NoteRendererView( self ); }

static void HookCamera()
{
	{
		struct { uint32_t site, target; void* hook; } calls[] = {
			{ 0x634C2E, 0x64A480, &RendSetView_Hook }, { 0x634BFE, 0x64A4C0, &RendSetCamera_Hook }, { 0x634BD6, 0x64A500, &RendLookAt_Hook } };
		for ( auto& c : calls ) {
			if ( *(uint8_t*) c.site == 0xE8 && c.site + 5 + *(int32_t*) (c.site + 1) == c.target ) injector::MakeCALL( c.site, c.hook );
			else mp_log( "[MP] Renderer view call 0x%.8X isn't what we expect.\n", c.site );
		}
	}
	static const uint8_t PERSPECTIVE_ORIGINAL[5] = { 0xE8, 0x6F, 0x9E, 0x0B, 0x00 };   // call 0x6EE990.
	if ( memcmp( (void*) PERSPECTIVE_CALL, PERSPECTIVE_ORIGINAL, 5 ) == 0 ) injector::MakeCALL( PERSPECTIVE_CALL, &Perspective_Hook );
	else mp_log( "[MP] The perspective call isn't what we expect.\n" );

	int hooked = 0;
	for ( int i = 0; i < NUM_CAMERA_LOOKAT_CALLS; i++ ) {
		uint32_t site = CAMERA_LOOKAT_CALLS[i];
		if ( *(uint8_t*) site != 0xE8 || site + 5 + *(int32_t*) (site + 1) != (uint32_t) CameraLookAt ) {
			mp_log( "[MP] Camera call 0x%.8X isn't what we expect%s.\n", site, site == CAMERA_LOOKAT_CALL ? " (free camera on?)" : "" );
			continue;
		}
		injector::MakeCALL( site, &CameraLookAt_Hook );
		hooked++;
	}
	if ( !hooked ) mp_log( "[MP] No camera call hooked : no name tags.\n" );
}

int g_tagPasses[2] = { 0, 0 };   // Puppet draws elsewhere than to the screen, and to the screen (diagnostics).

static void NoteNameTag( Peer* peer, EIEvilHarryP* p )
{
	Matrix4f view;
	if ( !g_haveCamera || !MatInverseAffine( g_cameraWorld, &view ) ) return;
	if ( !g_haveProj ) return;
	const float* proj = &g_cameraProj.data[0][0];
	IDirect3DDevice9* dev = *(IDirect3DDevice9**) RENDERER_DEVICE;
	D3DVIEWPORT9 vp;
	if ( !dev || FAILED( dev->GetViewport( &vp ) ) ) return;

	// Levels with water also draw him into a 512x512 texture, their reflection. Some
	// draw him once for the screen and once for it (the Great Tree, Butterfly Glade);
	// others once only, while the reflection's texture is the target, and the screen
	// shows that same draw (Jungle Canyon, Bittenbinder's Camp, Cavern Lake...). Only
	// accepting a draw to the screen left those without a tag, and the Great Tree's
	// flickering in the frames it skipped the screen's. The camera captured is the
	// main view's whatever the target (one look-at a frame, water levels too), so
	// the tag is placed with it on the screen's own image, whichever draw this is.
	IDirect3DSurface9* target = 0;
	IDirect3DSurface9* screen = 0;
	bool onScreen = SUCCEEDED( dev->GetRenderTarget( 0, &target ) ) && SUCCEEDED( dev->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &screen ) ) && target == screen;
	D3DSURFACE_DESC screenDesc = { };
	bool haveScreen = screen && SUCCEEDED( screen->GetDesc( &screenDesc ) );
	if ( target ) target->Release();
	if ( screen ) screen->Release();
	if ( !onScreen ) {
		if ( !haveScreen ) return;
		vp.X = vp.Y = 0;
		vp.Width = screenDesc.Width;
		vp.Height = screenDesc.Height;
	}
	g_tagPasses[onScreen ? 1 : 0]++;
	if ( g_tagDiagOn ) {
		D3DSURFACE_DESC d = { };
		IDirect3DSurface9* rt = 0;
		if ( SUCCEEDED( dev->GetRenderTarget( 0, &rt ) ) && rt ) { rt->GetDesc( &d ); rt->Release(); }
		if ( onScreen ) { g_tagDiag.drawsOnScreen++; g_tagDiag.screenW = d.Width; g_tagDiag.screenH = d.Height; }
		else { g_tagDiag.drawsElsewhere++; g_tagDiag.elseW = d.Width; g_tagDiag.elseH = d.Height; }
		if ( g_tagDiag.frames >= 30 && g_tagDiag.sampleLogged < 2 ) {
			g_tagDiag.sampleLogged++;
			const float* e = &g_cameraWorld.data[3][0];
			mp_log( "[AT] TAGS sample : draw to %s %ux%u, viewport %ux%u at (%u, %u); camera eye now (%.1f, %.1f, %.1f); %d look-at(s) this frame so far :\n",
				onScreen ? "screen" : "other target", d.Width, d.Height, vp.Width, vp.Height, vp.X, vp.Y, e[0], e[1], e[2], g_tagDiag.lookAtsThisFrame );
			for ( int i = 0; i < g_tagDiag.lookAtsThisFrame && i < 4; i++ )
				mp_log( "[AT]   look-at %d : eye (%.1f, %.1f, %.1f)\n", i, g_tagDiag.eyes[i].x, g_tagDiag.eyes[i].y, g_tagDiag.eyes[i].z );
		}
	}

	// Above his head : his transform is at his feet. (The highest bone isn't reliable :
	// some bones, not drawn, sit far above him in some poses.) Placed again each
	// frame it's shown (PlaceNameTag); this one is for the logs and tests.
	const Matrix4f& m = p->m_transformMatrix;
	float x = m.data[3][0], y = m.data[3][1], z = m.data[3][2] + TAG_ABOVE_FEET;

	float v[4], c[4];   // Row vector times view, times projection.
	for ( int j = 0; j < 4; j++ ) v[j] = x * view.data[0][j] + y * view.data[1][j] + z * view.data[2][j] + view.data[3][j];
	for ( int j = 0; j < 4; j++ ) c[j] = v[0] * proj[j] + v[1] * proj[4 + j] + v[2] * proj[8 + j] + v[3] * proj[12 + j];
	if ( c[3] < 0.1F ) { if ( g_tagDiagOn ) g_tagDiag.behind++; return; }   // Behind the camera.
	if ( g_tagDiagOn ) g_tagDiag.noted++;
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

static void __stdcall _EIEvilHarryP_Render_Hook( EIEvilHarryP* self, void* context, void* arg )
{
	Peer* peer = FindPeerFromBeast( self );
	if ( !peer ) {
		// One of our puppets nobody drives now (set aside, or left over) : the game
		// drew it as its own Evil Harry, a frozen copy of its player. Not drawn.
		// (Only this world's and the last one's : the game's own Evil Harry may reuse
		// an old puppet's address in a level long gone.)
		int spawned = -1;
		EIHarry* harry = Gizmod::getHarry();
		for ( int i = 0; i < g_spawnedCount; i++ )
			if ( g_spawned[i].beast == self && (g_spawned[i].world == harry->m_world || g_spawned[i].world == g_prevWorld) )
				spawned = i;

		if ( spawned >= 0 ) {
			return;
		}

		EIBeast_Render( self, context, arg );
		return;
	}

	if ( IsParkedPeer(peer) ) return;   // The emote preview's, between previews.

	const PlayerSnapshot* t = GetPuppetTarget( peer );
	// Online with nobody to show here (they left, or are in another level) : not drawn.
	if ( g_networked && !t ) return;
	if ( g_levelLeaving ) return;   // The old level, still shown (NoteLevelLeaving).
	bool viewer = IsViewerPeer( peer );   // No pose copy, name tag or items : just the clip.
	if ( t && !ForcedClip(peer) ) ApplyPuppetPose( self, t );

	EIBeast_Render( self, context, arg );

	if ( t && !viewer ) NoteNameTag( peer, self );
	if ( t && !viewer && t->drawnSlot >= 0 ) DrawPuppetItem( peer, t->drawnSlot, context, arg );
}
MAKE_THISCALL_WRAPPER( EIEvilHarryP_Render_Hook, _EIEvilHarryP_Render_Hook );


static void InstallItemDrawHooks()
{
	for ( int i = 0; i < NUM_DRAWN_ITEMS; i++ ) {
		const uint32_t slot = DRAWN_ITEMS[i].vtable + ITEM_SLOT_DRAW;
		g_origItemDraw[i] = injector::ReadMemory<ItemDrawFunc>( slot );
		injector::WriteMemory( slot, ITEM_DRAW_HOOKS[i] );
	}
	const uint32_t render = EIEvilHarryP::VTABLE + VTABLE_SLOT_RENDER;
	injector::WriteMemory( render, &EIEvilHarryP_Render_Hook );
}



// --------------------------------------------------------------------------------
// Pitfalls (EIPitMonster) eat any beast that touches their mouth : the beast is put to sleep and the pit
// stays shut on it. A puppet caught there kept the pit shut on the other players' screens.
// The pit's two contact handlers now let puppets through.
// --------------------------------------------------------------------------------

#include "ptle/EIPitMonster.h"
static const uint32_t PIT_CONTACT_SLOTS[2] = { 0x890444, 0x890448 };

GET_METHOD( 0x542DD0, bool, EIPitMonster_Contact4, EIPitMonster*, OverlapStruct* c, int a, int b, int d );
GET_METHOD( 0x542FD0, bool, EIPitMonster_Contact5, EIPitMonster*, OverlapStruct* c, int a, int b, int d, int e );

static bool IsAnyPuppet( EInstance* beast )
{
	if ( beast->GetType() != get_etypeinfo<EIEvilHarryP>() ) return false;
	if ( FindPeerFromBeast((EIEvilHarryP*) beast) ) return true;
	for ( int i = 0; i < g_spawnedCount; i++ ) if ( g_spawned[i].beast == beast ) return true;
	return false;
}

static bool PitSkips( OverlapStruct* c )
{
	if ( !c || !IsAnyPuppet(c->m_other) ) return false;
	return true;
}

static bool __stdcall _PitContact4_Hook( EIPitMonster* self, OverlapStruct* c, int a, int b, int d )
{
	if ( PitSkips(c) ) return false;
	return EIPitMonster_Contact4( self, c, a, b, d );
}
MAKE_THISCALL_WRAPPER( PitContact4_Hook, _PitContact4_Hook );

static bool __stdcall _PitContact5_Hook( EIPitMonster* self, OverlapStruct* c, int a, int b, int d, int e )
{
	if ( PitSkips(c) ) return false;
	return EIPitMonster_Contact5( self, c, a, b, d, e );
}
MAKE_THISCALL_WRAPPER( PitContact5_Hook, _PitContact5_Hook );

static void InstallPitHooks()
{
	injector::WriteMemory( PIT_CONTACT_SLOTS[0], PitContact4_Hook );
	injector::WriteMemory( PIT_CONTACT_SLOTS[1], PitContact5_Hook );
}


// Creatures fade out with their distance to the camera (vtable +0x108, 0x41ED70,
// shared by every kind : sets the fade at +0x24 from 1 to 0, about 150 away, and
// the alpha at +0x1C, and returns true once gone : the render then skips it).
// The other players, and the enemies too, now stay in full view however far (up
// to the game's own cut-off, a few hundred away); the name tags still fade
// (TAG_FADE_STARTS).
static const uint32_t VTABLE_SLOT_DISTANCE_FADE = 0x108;
static const uint32_t DISTANCE_FADE_VTABLES[] = {   // Every vtable with it in that slot.
	0x869440, 0x869AD0, 0x86B3C8, 0x86C3D0, 0x86D1D8, 0x86D7A0, 0x86FD18, 0x873B68,
	0x874688, 0x875240, 0x875B30, 0x876400, 0x876800, 0x876D28, 0x877E78, 0x878310,
	0x878810, 0x878C00, 0x8792D0, 0x879770, 0x879B90, 0x881BE8, 0x8822D0, 0x889700,
	0x88A2C0, 0x88A668, 0x88AFE8, 0x88B4F0, 0x88B868, 0x88BE78, 0x88E8C8, 0x88F078,
	0x890400, 0x890A40, 0x8925A8, 0x893EA8, 0x894398, 0x896438, 0x896C10, 0x8974E0,
	0x897ED0, 0x898660, 0x898C20, 0x899130, 0x8999C8, 0x899DE0, 0x89A6D8, 0x8A18A0,
	0x8A1CF0, 0x8A2C70,
};

volatile LONG g_fadesStopped = 0;
float g_lastPuppetFade = 1.0F;   // What the game wanted for a puppet (tests).
volatile LONG g_puppetFadeCalls = 0;
bool g_allowPuppetFade = false;

GET_METHOD( 0x41ED70, bool, EIBeast_DistanceFade, EIBeast*, float );
static bool __stdcall _DistanceFade_Hook( EIBeast* self, float distance )
{
	const bool gone = EIBeast_DistanceFade( self, distance );
	float fade = self->m_opacity3;
	bool puppet = IsAnyPuppet( self );
	if ( puppet ) {
		InterlockedIncrement( &g_puppetFadeCalls );
		g_lastPuppetFade = fade;
	}
	if ( g_allowPuppetFade ) return gone;

	if ( puppet && fade < 1.0F && InterlockedIncrement(&g_fadesStopped) == 1 ) mp_log( "[MP] A puppet was fading out with distance (%.2f) : kept in full view.\n", fade );
	self->m_opacity3 = 1.0F;
	self->m_opacityFinal = self->m_opacity2 * self->m_alphaUser;
	return false;
}
MAKE_THISCALL_WRAPPER( DistanceFade_Hook, _DistanceFade_Hook );

static void InstallFadeHooks()
{
	int hooked = 0, n = sizeof(DISTANCE_FADE_VTABLES) / sizeof(DISTANCE_FADE_VTABLES[0]);
	for ( int i = 0; i < n; i++ ) {
		const uint32_t slot = DISTANCE_FADE_VTABLES[i] + VTABLE_SLOT_DISTANCE_FADE;
		if ( injector::ReadMemory<uint32_t>(slot) != (uint32_t) EIBeast_DistanceFade ) continue;

		injector::WriteMemory( slot, &DistanceFade_Hook );
		hooked++;
	}
	if ( hooked != n ) mp_log( "[MP] Distance fade : %d of %d kinds of creature hooked; the others may still fade out.\n", hooked, n );
}


static void DrivePuppet( EIEvilHarryP* p, Peer* peer )
{
	ApplyPuppetCollision( p );

	Vector3f pos;
	if ( !GetPuppetTargetPosition(&pos, peer) ) {
		ApplyPuppetItem( p );
		return;
	}

	const PlayerSnapshot* t = GetPuppetTarget( peer );
	ApplyPuppetItem( p );
	const Vector3f* rot = &t->rotationEuler;

	p->m_position = pos;
	EICharacter_SetPos( p, &pos, true );
	EICharacter_SetRot( p, rot );

	SetBeastWorldMatrix( peer, p, pos, t );

	// Same orientation and position in the header transform.
	const Matrix4f& world = p->m_worldMatrix;
	for ( int r = 0; r < 3; r++ )
		for ( int c = 0; c < 3; c++ )
			p->m_transformMatrix.data[r][c] = world.data[r][c];
	p->m_transformMatrix.data[3][0] = world.data[3][0];
	p->m_transformMatrix.data[3][1] = world.data[3][1];
	p->m_transformMatrix.data[3][2] = world.data[3][2];
	p->m_previousPosition = pos;
}



// --------------------------------------------------------------------------------
// Hotkeys & menu typing.
// --------------------------------------------------------------------------------
// Keys are polled on our own thread (independent of the game's tick rate and of
// which window the game thinks it owns), and turned into requests the game
// thread consumes in HandleHotkeys().
static volatile LONG g_reqDelay = 0;
volatile LONG g_reqMenu = 0;

bool g_showTags = true;    // Names above the other players.
// Name tags fade out with the camera distance, between these two (the menu's
// sliders; Multiplayer.ini : Display, TagFadeFrom and TagFadeTo).
const int TAG_FADE_MAX = 1000, TAG_FADE_STEP = 10;
int g_tagFadeFrom = 100, g_tagFadeTo = 200;
bool g_showList = false;   // The player list stays on screen while playing.
static char g_iniPath[MAX_PATH] = { 0, };
volatile LONG g_menuKeys[MK_COUNT];   // Presses, from the hotkey thread.
static volatile LONG g_menuOpen = 0;         // For the hotkey thread and the keyboard poll.

// Typing into the menu (a colour) : characters from the hotkey thread.
static SRWLOCK g_typedLock = SRWLOCK_INIT;
static char g_typedChars[64];
static volatile LONG g_menuTyping = 0, g_menuPaste = 0, g_menuCopy = 0;

void AddTyped( char c )
{
	AcquireSRWLockExclusive( &g_typedLock );
	size_t n = strlen( g_typedChars );
	if ( n + 1 < sizeof(g_typedChars) ) { g_typedChars[n] = c; g_typedChars[n + 1] = 0; }
	ReleaseSRWLockExclusive( &g_typedLock );
}

static void TakeTyped( char* out, size_t size )
{
	AcquireSRWLockExclusive( &g_typedLock );
	strcpy_s( out, size, g_typedChars );
	g_typedChars[0] = 0;
	ReleaseSRWLockExclusive( &g_typedLock );
}

HWND* const HWND_GAME_WINDOW   = (HWND*) 0x93ED60;

static WNDPROC g_gameWndProc = 0;

static bool PollTyping( int vk );
static bool RecordMenuMouseWheel( short delta );

static void Keydown( int vk, bool repeat );
static bool MouseWheel( short delta );

static LRESULT CALLBACK GameWndProc_Hook( HWND wnd, UINT msg, WPARAM wp, LPARAM lp )
{
	switch ( msg )
	{
	case WM_KEYDOWN:
		Keydown( wp, (lp & 0x40000000) != 0 );   // Bit 30 : the key was already down (auto-repeat).
		if ( g_menuOpen )
			return 0;   // The menu has the keyboard.
		break;

	case WM_MOUSEWHEEL:
		if ( MouseWheel(GET_WHEEL_DELTA_WPARAM(wp)) )
			return 0;
		break;

	default:
		break;
	}
	return CallWindowProcA( g_gameWndProc, wnd, msg, wp, lp );
}

// TODO: late init
// Currently checked every tick.
static void HookGameWindowProc()
{
	static bool installed = false;
	if ( installed ) return;

	HWND hwnd = *HWND_GAME_WINDOW;
	if ( !hwnd ) return;

	installed = true;

	g_gameWndProc = (WNDPROC) GetWindowLongPtrA( hwnd, GWLP_WNDPROC );
	SetWindowLongPtrA( hwnd, GWLP_WNDPROC, (LONG_PTR) GameWndProc_Hook );

	mp_log( "[MP] Game window 0x%.8X : mouse wheel hooked.\n", hwnd );
}


//
// Win32 clipboard utils.
//

static bool ReadClipboard( char* out, size_t size )
{
	out[0] = 0;
	if ( !OpenClipboard( 0 ) ) return false;

	HANDLE h = GetClipboardData( CF_TEXT );
	const char* text = h ? (const char*) GlobalLock( h ) : 0;
	if ( text ) {
		strncpy_s( out, size, text, _TRUNCATE );
		GlobalUnlock( h );
	}
	CloseClipboard();
	return out[0] != 0;
}

static void WriteClipboard( const char* text )
{
	size_t n = strlen( text ) + 1;
	HGLOBAL h = GlobalAlloc( GMEM_MOVEABLE, n );
	if ( !h ) return;

	memcpy( GlobalLock(h), text, n );
	GlobalUnlock( h );
	if ( OpenClipboard(0) ) {
		EmptyClipboard();
		if ( !SetClipboardData(CF_TEXT, h) )
			GlobalFree( h );
		CloseClipboard();
	}
	else
		GlobalFree( h );
}



// The keys that type : digits, letters (Shift : capitals), space, , . - ' # ( ) !, backspace.
static inline bool TypingAcceptedKey( int vk )
{
	return
		(vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
		|| vk == VK_SPACE || vk == VK_OEM_COMMA || vk == VK_OEM_PERIOD || vk == VK_OEM_MINUS || vk == VK_OEM_7 || vk == VK_BACK;
}

static bool IsMenuOpen();
static bool PollTyping( int vk )
{
	if ( !IsMenuOpen() || !GameHasFocus() ) return false;

	const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

	if ( !TypingAcceptedKey(vk) ) return false;

	if ( ctrl ) {
		if ( vk == 'V' ) InterlockedIncrement( &g_menuPaste );
		if ( vk == 'C' ) InterlockedIncrement( &g_menuCopy );
	}
	else if ( g_menuTyping ) {
		char c = 0;
		if      ( vk == VK_OEM_COMMA )  c = ',';
		else if ( vk == VK_OEM_PERIOD ) c = '.';
		else if ( vk == VK_OEM_MINUS )  c = '-';
		else if ( vk == VK_OEM_7 )      c = '\'';
		else if ( vk == '1' && shift )  c = '!';
		else if ( vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 ) c = (char) ('0' + vk - VK_NUMPAD0);
		else if ( vk == '3' && shift )  c = '#';
		else if ( vk == '9' && shift )  c = '(';
		else if ( vk == '0' && shift )  c = ')';
		else if ( vk >= 'A' && vk <= 'Z' ) c = (char) (vk - 'A' + (shift ? 'A' : 'a'));
		else c = (char) vk;
		AddTyped( c );
	}

	return true;
}

// In-game hotkeys.
volatile LONG g_reqViewer = 0, g_reqStar = 0, g_reqPrev = 0, g_reqNext = 0, g_reqPrev10 = 0, g_reqNext10 = 0,
g_reqReplay = 0, g_reqStarredOnly = 0;
volatile LONG g_harryTicks = 0;

static void ProcessHotkey( int vk )
{
	// The animation viewer's keys ([ ] \ ') only matter while it's open.
	static const int NUM_KEYS = 8;
	const int keys[NUM_KEYS] = { VK_F7, VK_F1, VK_F10, VK_F11, VK_OEM_4, VK_OEM_6, VK_OEM_5, VK_OEM_7 };
	const char* names[NUM_KEYS] = { "F7", "F1", "F10", "F11", "[", "]", "\\", "'" };
	volatile LONG* reqs[NUM_KEYS] = { &g_reqDelay, &g_reqMenu,
		&g_reqViewer, &g_reqStar, &g_reqPrev, &g_reqNext, &g_reqReplay, &g_reqStarredOnly };

	for ( int i = 0; i < NUM_KEYS; i++ ) {
		if ( keys[i] != vk ) continue;

		if ( GameHasFocus() ) {
			volatile LONG* req = reqs[i];
			const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;   // Shift + [ ] : by ten.
			if ( shift && req == &g_reqPrev ) req = &g_reqPrev10;
			if ( shift && req == &g_reqNext ) req = &g_reqNext10;
			InterlockedIncrement( req );
			if ( i <= 6 ) mp_log( "[MP] %s pressed (Harry ticks so far : %ld).\n", names[i], g_harryTicks );
		}
		else if ( i <= 6 ) {
			mp_log( "[MP] %s ignored : game window not focused.\n", names[i] );
		}
	}
}

static void ProcessMenuKey( int vk, bool repeat )
{
	static const int NUM_MENU_VK = 10;
	static const int MENU_VK[NUM_MENU_VK] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE, VK_TAB, VK_DELETE, VK_PRIOR, VK_NEXT };
	static const int MENU_SLOT[NUM_MENU_VK] = { MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT, MK_ACCEPT, MK_BACK, MK_NEXT, MK_DELETE, MK_PGUP, MK_PGDN };

	if ( !g_menuOpen || !GameHasFocus() ) return;

	const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	for ( int i = 0; i < NUM_MENU_VK; i++ ) {
		if ( MENU_VK[i] != vk ) continue;

		const bool repeats = i < 4 || i >= 8;   // Held, the arrows and Page Up / Down repeat.
		if ( repeat && !repeats )
			continue;

		int slot = MENU_SLOT[i];
		if ( shift && slot == MK_NEXT ) slot = MK_PREV;
		if ( shift && slot == MK_LEFT ) slot = MK_SLEFT;
		if ( shift && slot == MK_RIGHT ) slot = MK_SRIGHT;
		InterlockedIncrement( &g_menuKeys[slot] );
	}
}

static volatile LONG g_menuWheel = 0;   // WHEEL_DELTA a notch (a touchpad's steps can be smaller).
static bool RecordMenuMouseWheel( short delta )
{
	if ( g_menuOpen ) {
		InterlockedExchangeAdd( &g_menuWheel, delta );
		return true;
	}
	return false;
}

static void Keydown( int vk, bool repeat )
{
	if ( !repeat )   // Held, F1 would open and close the menu over and over.
		ProcessHotkey( vk );
	ProcessMenuKey( vk, repeat );
	PollTyping( vk );
}
static bool MouseWheel( short delta )
{
	return RecordMenuMouseWheel( delta );
}



// --------------------------------------------------------------------------------
// Animation notes. Its clip list and notes are in
// %LOCALAPPDATA%\PTLE Multiplayer\anim-notes.txt, one clip per line :
//     <number> <clip id> <where found> [*] [notes]
// The numbers are the animation survey's. Edited by hand (reloaded when it changes)
// and by F11 (star). Clips found on Harry and not in it yet are added.
// --------------------------------------------------------------------------------
static const int MAX_ANIM_NOTES = 1024;
AnimNote g_animNotes[MAX_ANIM_NOTES];
int g_animNoteCount = 0;
int g_animIndex = 0;
static bool g_animStarredOnly = false;
static char g_animNotesPath[MAX_PATH];
static FILETIME g_animNotesTime;
static uint32_t g_animNotesChecked = 0;

static bool AnimNotesFileTime( FILETIME* t )
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	if ( !GetFileAttributesExA(g_animNotesPath, GetFileExInfoStandard, &a) ) return false;
	*t = a.ftLastWriteTime;
	return true;
}

static void LoadAnimNotes()
{
	if ( !g_animNotesPath[0] ) {
		if ( !GetDataDir(g_animNotesPath) ) return;
		strcat_s( g_animNotesPath, "\\anim-notes.txt" );
	}
	uint32_t keep = (g_animIndex < g_animNoteCount) ? g_animNotes[g_animIndex].crc : 0;
	FILE* f = 0;
	if ( fopen_s( &f, g_animNotesPath, "r" ) != 0 || !f ) return;
	g_animNoteCount = 0;
	char line[512];
	while ( fgets( line, sizeof(line), f ) && g_animNoteCount < MAX_ANIM_NOTES ) {
		AnimNote& n = g_animNotes[g_animNoteCount];
		int used = 0;
		if ( line[0] == '#' ) continue;
		if ( sscanf_s(line, "%d %x %23s %n", &n.num, &n.crc, n.source, (unsigned) sizeof(n.source), &used) < 3 ) continue;
		char* rest = line + used;
		n.star = (rest[0] == '*');
		if ( n.star ) rest++;
		while ( *rest == ' ' || *rest == '\t' ) rest++;
		size_t len = strlen( rest );
		while ( len && (rest[len - 1] == '\n' || rest[len - 1] == '\r' || rest[len - 1] == ' ') ) rest[--len] = 0;
		strncpy_s( n.note, rest, _TRUNCATE );
		g_animNoteCount++;
	}
	fclose( f );
	AnimNotesFileTime( &g_animNotesTime );
	g_animIndex = 0;
	for ( int i = 0; i < g_animNoteCount; i++ )
		if ( g_animNotes[i].crc == keep )
			g_animIndex = i;
	mp_log( "[MP] Animation notes : %d clips read.\n", g_animNoteCount );
}

static void SaveAnimNotes()
{
	FILE* f = 0;
	if ( fopen_s( &f, g_animNotesPath, "w" ) != 0 || !f ) { Toast( "Couldn't write anim-notes.txt" ); return; }
	fprintf( f, "# Harry's animations : number, clip id, where it was found (layer), * if starred, then notes.\n" );
	fprintf( f, "# Edit freely, even while the game runs (it reloads). In the game : F10 opens the viewer,\n" );
	fprintf( f, "# [ ] previous / next (Shift : by 10), \\ replays, F11 stars, ' shows only starred ones.\n" );
	fprintf( f, "# Other lines starting with # are not kept.\n" );
	for ( int i = 0; i < g_animNoteCount; i++ ) {
		const AnimNote& n = g_animNotes[i];
		fprintf( f, "%03d 0x%.8X %-14s %s%s%s\n", n.num, n.crc, n.source, n.star ? "*" : "", (n.star && n.note[0]) ? " " : "", n.note );
	}
	fclose( f );
	AnimNotesFileTime( &g_animNotesTime );
}

// Clips loaded on these characters and not in the notes yet.
static void AddFoundClips( EICharacter* harry, EICharacter* beast )
{
	int added = 0, next = 0;
	for ( int i = 0; i < g_animNoteCount; i++ ) if ( g_animNotes[i].num >= next ) next = g_animNotes[i].num + 1;
	EICharacter* owners[2] = { harry, beast };
	const char* ownerNames[2] = { "harry", "evilharry" };
	for ( int o = 0; o < 2; o++ ) {
		if ( !owners[o] ) continue;
		for ( int key = 0; key < 4096 && g_animNoteCount < MAX_ANIM_NOTES; key++ ) {
			if ( o == 1 && (key == MIRROR_LAYERS[0] || key == MIRROR_LAYERS[1]) ) continue;
			AnimLayer* layer = GetAnimLayer( owners[o], key );
			uint32_t crc = layer ? GetLayerClipCRC( layer ) : 0;
			if ( !crc ) continue;
			bool known = false;
			for ( int i = 0; i < g_animNoteCount && !known; i++ ) known = g_animNotes[i].crc == crc;
			if ( known ) continue;
			AnimNote& n = g_animNotes[g_animNoteCount++];
			n.num = next++;
			n.crc = crc;
			sprintf_s( n.source, "%s/%d", ownerNames[o], key );
			n.star = false;
			n.note[0] = 0;
			added++;
		}
	}
	if ( added ) {
		mp_log( "[MP] Animation notes : %d new clips added.\n", added );
		SaveAnimNotes();
	}
}

static void ShowAnimClip( bool restart )
{
	if ( g_animIndex >= g_animNoteCount ) return;
	g_animPreviewCRC = g_animNotes[g_animIndex].crc;
	if ( restart && g_viewerPeer ) g_viewerPeer->mirrorClip = 0;   // Loads it again, from the start.
}

// Moves through the list (only starred clips if asked, when there are some).
static void StepAnimClip( int by )
{
	if ( !g_animNoteCount ) return;
	bool anyStar = false;
	for ( int i = 0; i < g_animNoteCount && !anyStar; i++ ) anyStar = g_animNotes[i].star;
	bool starred = g_animStarredOnly && anyStar;
	int dir = by > 0 ? 1 : -1, steps = by > 0 ? by : -by;
	int i = g_animIndex;
	while ( steps-- > 0 ) {
		int tries = g_animNoteCount;
		do { i = (i + dir + g_animNoteCount) % g_animNoteCount; } while ( starred && !g_animNotes[i].star && --tries > 0 );
	}
	g_animIndex = i;
	ShowAnimClip( true );
}

static void CloseAnimViewer( const char* why );

static void OpenAnimViewer( EIHarry* harry )
{
	if ( g_emotePreview ) {
		Toast( "Close the menu first" );
		return;
	}

	// Its puppet : offline the main one (it replays Harry); online one of a free
	// player slot, so other players' puppets are left alone.
	Peer* vp = MainPeer();
	if ( g_networked ) {
		int me = g_transport ? g_transport->PlayerId() : -1;
		vp = 0;
		for ( int i = MAX_PEERS - 1; i >= 0 && !vp; i-- )
			if ( i != me && !g_players[i].present && !g_peers[i].heard && !g_peers[i].target )
				vp = &g_peers[i];
		if ( !vp ) {
			Toast( "No room for the animation viewer (8 players)" );
			return;
		}
	}

	g_viewerPeer = vp;
	g_animViewer = true;

	if ( !vp->beast ) SpawnPuppet( vp );

	if ( !vp->beast ) {
		CloseAnimViewer( "no puppet" );
		Toast( "Evil Harry can't be placed here" );
		return;
	}

	// Beside Harry as the camera sees him (along its right axis), facing the camera.
	float rx = 1.0F, ry = 0.0F;
	if ( g_haveCamera ) {
		float x = g_cameraWorld.data[0][0], y = g_cameraWorld.data[0][1], l = sqrtf( x * x + y * y );
		if ( l > 0.01F ) { rx = x / l; ry = y / l; }
	}
	g_viewerPin = harry->m_position;
	g_viewerPin.x += rx * 7.0F;
	g_viewerPin.y += ry * 7.0F;
	g_autoLocomotion = true;

	LoadAnimNotes();
	AddFoundClips( harry, vp->beast );
	if ( !g_animNoteCount ) {
		CloseAnimViewer( "no clips" );
		Toast( "No animations found" );
		return;
	}
	if ( g_animIndex >= g_animNoteCount ) g_animIndex = 0;
	ShowAnimClip( true );
	mp_log( "[MP] Animation viewer open : puppet of slot %d, %d clips.\n", vp->id, g_animNoteCount );
}

static void CloseAnimViewer( const char* why )
{
	if ( !g_animViewer ) return;

	g_animViewer = false;
	g_animPreviewCRC = 0;
	if ( g_viewerPeer ) g_viewerPeer->mirrorClip = 0;
	g_viewerPeer = 0;
	mp_log( "[MP] Animation viewer closed (%s).\n", why );
}

static void HandleAnimViewer( EIHarry* harry )
{
	bool open    = InterlockedExchange( &g_reqViewer, 0 ) != 0;
	bool star    = InterlockedExchange( &g_reqStar, 0 ) != 0;
	int  by      = InterlockedExchange( &g_reqNext, 0 ) - InterlockedExchange( &g_reqPrev, 0 )
				 + 10 * (InterlockedExchange( &g_reqNext10, 0 ) - InterlockedExchange( &g_reqPrev10, 0 ));
	bool replay  = InterlockedExchange( &g_reqReplay, 0 ) != 0;
	bool starred = InterlockedExchange( &g_reqStarredOnly, 0 ) != 0;

	if ( open ) {
		if ( g_animViewer ) CloseAnimViewer( "F10" );
		else OpenAnimViewer( harry );
	}
	if ( !g_animViewer ) return;
	if ( !g_viewerPeer || !g_viewerPeer->beast ) {
		CloseAnimViewer( "puppet gone" );
		return;
	}

	uint32_t now = NetNowMs();
	if ( now - g_animNotesChecked >= 1000 ) {   // Edited by hand ?
		g_animNotesChecked = now;
		FILETIME t;
		if ( AnimNotesFileTime(&t) && CompareFileTime(&t, &g_animNotesTime) != 0 ) {
			LoadAnimNotes();
			ShowAnimClip( false );
		}
	}
	if ( !g_animNoteCount ) return;
	if ( starred ) {
		g_animStarredOnly = !g_animStarredOnly;
		if ( g_animStarredOnly && !g_animNotes[g_animIndex].star ) StepAnimClip( 1 );
	}
	if ( by ) StepAnimClip( by );
	if ( replay ) ShowAnimClip( true );
	if ( star ) {
		FILETIME t;   // Take hand edits first.
		if ( AnimNotesFileTime(&t) && CompareFileTime(&t, &g_animNotesTime) != 0 ) LoadAnimNotes();
		AnimNote& n = g_animNotes[g_animIndex];
		n.star = !n.star;
		SaveAnimNotes();
		Toast( "#%03d %s", n.num, n.star ? "starred" : "unstarred" );
	}
}



// --------------------------------------------------------------------------------
// Emotes : Harry plays one of his own clips, the game's own animation held at no
// weight meanwhile. The others see it as they see everything Harry does (his pose
// and clip go in the snapshot). The clips are the catalogue's (emotes_data.h, made
// by make-emotes.py from the animation notes); up to 4 wheels of 8 hold the ones
// a player picked.
// --------------------------------------------------------------------------------

#include "emotes_data.h"

EmoteState g_emoteState;

EmoteWheel g_emoteWheels[MAX_WHEELS];
int g_wheelCount = 0;
static int g_emoteButton = 4;        // DirectInput button held for the wheel : L1 / LB.
static bool g_emoteNeedsR1 = false;  // ... with R1 / RB too.
static int g_emoteKey = 0x14;        // DirectInput key code : T.
int g_wheelIndex = 0;         // The wheel up in the game (kept between openings).

int FindEmoteByName( const char* name )
{
	for ( int i = 0; name && i < NUM_EMOTES; i++ ) if ( _stricmp( EMOTES[i].name, name ) == 0 ) return i;
	return -1;
}

// In the ini : a chain's id, a clip's CRC (hex), "-" for nothing.
static int FindEmoteByCode( const char* code )
{
	if ( !code[0] || strcmp( code, "-" ) == 0 ) return -1;
	for ( int i = 0; i < NUM_EMOTES; i++ ) if ( EMOTES[i].id && _stricmp( EMOTES[i].id, code ) == 0 ) return i;
	char* end = 0;
	unsigned long crc = strtoul( code, &end, 16 );
	if ( !end || *end ) return -1;
	for ( int i = 0; i < NUM_EMOTES; i++ ) if ( !EMOTES[i].id && EMOTES[i].clip == crc ) return i;
	return -1;
}

static void EmoteCode( int e, char* out, size_t size )
{
	if ( e < 0 || e >= NUM_EMOTES ) strcpy_s( out, size, "-" );
	else if ( EMOTES[e].id ) strcpy_s( out, size, EMOTES[e].id );
	else sprintf_s( out, size, "%.8X", EMOTES[e].clip );
}

// The wheels everyone starts with (Reset puts them back; the others start empty).
void MakeDefaultWheel( int w )
{
	static const char* const PARTY[WHEEL_SLOTS] = { "Victory Dance", "Jig", "Running Man", "Breakdance", "Pickaxe Spin", "Teleport Zen", "Scratch Butt", "T-Pose" };
	static const char* const SILLY[WHEEL_SLOTS] = { "Bug Buzz", "Dizzy Walk", "Poison Gas Walk", "Death", "Death 2", "Breakdance Finish", 0, 0 };
	EmoteWheel& wheel = g_emoteWheels[w];
	if ( w == 0 )
		strcpy_s( wheel.name, "Party" );
	else if ( w == 1 )
		strcpy_s( wheel.name, "Silly" );
	else
		sprintf_s( wheel.name, "Wheel %d", w + 1 );
	for ( int s = 0; s < WHEEL_SLOTS; s++ )
		wheel.slot[s] = w == 0 ? FindEmoteByName( PARTY[s] ) : w == 1 ? FindEmoteByName( SILLY[s] ) : -1;
}

// [Emotes] Wheels=N, WheelK=Name|slot,slot,... (each : EmoteCode).
void LoadEmoteWheels( const char* ini )
{
	for ( int w = 0; w < MAX_WHEELS; w++ )
		MakeDefaultWheel( w );
	g_wheelCount = GetPrivateProfileIntA( "Emotes", "Wheels", 0, ini );
	if ( g_wheelCount <= 0 || g_wheelCount > MAX_WHEELS ) { g_wheelCount = 2; return; }
	for ( int w = 0; w < g_wheelCount; w++ ) {
		char key[16], line[512];
		sprintf_s( key, "Wheel%d", w + 1 );
		if ( !GetPrivateProfileStringA( "Emotes", key, "", line, sizeof(line), ini ) ) continue;
		char* bar = strchr( line, '|' );
		if ( !bar ) continue;
		*bar = 0;
		strncpy_s( g_emoteWheels[w].name, line, _TRUNCATE );
		char* next = bar + 1;
		for ( int s = 0; s < WHEEL_SLOTS; s++ ) {
			char* comma = next ? strchr( next, ',' ) : 0;
			if ( comma ) *comma = 0;
			g_emoteWheels[w].slot[s] = next ? FindEmoteByCode( next ) : -1;
			next = comma ? comma + 1 : 0;
		}
	}
}

void SaveEmoteWheels( const char* ini )
{
	char n[16], key[16], line[512];
	sprintf_s( n, "%d", g_wheelCount );
	WritePrivateProfileStringA( "Emotes", "Wheels", n, ini );
	for ( int w = 0; w < MAX_WHEELS; w++ ) {
		sprintf_s( key, "Wheel%d", w + 1 );
		if ( w >= g_wheelCount ) {
			WritePrivateProfileStringA( "Emotes", key, 0, ini );
			continue;
		}
		strcpy_s( line, g_emoteWheels[w].name );
		for ( char* c = line; *c; c++ ) if ( *c == '|' ) *c = '/';
		strcat_s( line, "|" );
		for ( int s = 0; s < WHEEL_SLOTS; s++ ) {
			char code[32];
			EmoteCode( g_emoteWheels[w].slot[s], code, sizeof(code) );
			if ( s ) strcat_s( line, "," );
			strcat_s( line, code );
		}
		WritePrivateProfileStringA( "Emotes", key, line, ini );
	}
}

const int EMOTE_LAYERS[2] = { 254, 255 };   // Spare layers of Harry's (his own go up to 253), taking turns.

GET_METHOD( 0x61E4F0, void, EAnimController_SetTicked, EAnimController*, void*, bool );
static const float EMOTE_MOVE = 0.5F;               // Harry got moved this far from where he started : over.
static const uint32_t EMOTE_FADE_MS = 200;          // Over : blended back into the game's own animation this long.
static const uint32_t EMOTE_SKIP_FADE_MS = 120;     // A chain's ending skipped.
static const uint32_t EMOTE_ENDING_BLEND_MS = 250;  // Into an ending that starts partway (lying there, then getting up).

static struct { bool on; int layer; uint32_t since, ms; } g_emoteFade;   // Over, blending out.
static int g_emoteRequest = -1;           // EMOTES index : the wheel's choice, played on Harry's next tick.
static const char* g_emoteCancel = 0;     // Set by the input hooks (a button, a key) : over, on the next tick.
static Vector3f g_stillAt;                // Harry standing still : where ...
static uint32_t g_stillSince = 0;         // ... and since when.

void RequestPlayEmote( int id )
{
	g_emoteRequest = id;
}

// A chain's ending playing : the input hooks keep Harry where he is.
static inline bool EmoteHoldsHarry()
{
	return g_emoteState.active && (g_emoteState.ending >= 0 || g_emoteState.finishing);
}

static float ClipSecondsLeft( AnimLayer* layer )
{
	ERAnim* anim = layer->m_anim;
	float rate = layer->m_fps * layer->m_speed;
	if ( !anim ) return 0.0F;
	if ( rate <= 0.0F ) return 1e6F;   // Stopped : not getting to its end.
	return ((float) (anim->m_frames - 1) - layer->m_frame) / rate;
}

// Ours, whatever the game does : starting one of Harry's idles, it fades every other
// layer out (ours too) and drops them.
static void KeepLayer( EAnimController* ctrl, AnimLayer* layer )
{
	layer->m_fading = 0;
	layer->m_targetWeight = 1.0F;
	if ( !layer->m_ticked ) EAnimController_SetTicked( ctrl, layer, true );
}

static void DropLayer( EIHarry* harry, int key )
{
	AnimLayer* layer = GetAnimLayer( harry, key );
	if ( !layer ) return;
	layer->m_weight = 0.0F;
	layer->m_targetWeight = 0.0F;
	layer->m_fading = 0;
	EAnimController_SetTicked( &harry->m_animController, layer, false );
}

// Ours (layer key) at w (and key2 at w2); the game's own at the weights it wants (it only
// sets them when it changes them), times what's left.
static void SetEmoteWeights( EIHarry* harry, int key, float w, int key2 = -1, float w2 = 0.0F )
{
	for ( int i = 0; i < MAX_LAYER_SCAN; i++ ) {
		AnimLayer* layer = GetAnimLayer( harry, i );
		if ( !layer ) continue;
		if ( i == key ) layer->m_weight = w;
		else if ( i == key2 ) layer->m_weight = w2;
		else if ( i == EMOTE_LAYERS[0] || i == EMOTE_LAYERS[1] ) layer->m_weight = 0.0F;
		else layer->m_weight = layer->m_targetWeight * (1.0F - w - w2);
	}
}

static void EndFade( EIHarry* harry )
{
	if ( !g_emoteFade.on ) return;
	g_emoteFade.on = false;
	SetEmoteWeights( harry, -1, 0.0F );
	DropLayer( harry, EMOTE_LAYERS[0] );
	DropLayer( harry, EMOTE_LAYERS[1] );
}

// The next clip, from frame `from`; blendMs : the one before blends out meanwhile (else it's gone at once).
static void BeginEmoteAnim( uint32_t clip, bool loop, float from = 0.0F, uint32_t blendMs = 0 )
{
	EmoteState& s = g_emoteState;

	s.clip = clip;
	s.loop = loop;
	s.blendFrom = (blendMs && s.started) ? EMOTE_LAYERS[s.slot] : -1;
	s.blendMs = blendMs;
	s.slot = 1 - s.slot;
	s.started = false;
	s.startFrame = from;
	s.lastFrame = -1.0F;
}

static bool HasHold( const EmoteDef& d )
{
	return d.hold[0] != 0.0F || d.hold[1] != 0.0F;
}

static void StartEmote( EIHarry* harry, int e )
{
	EmoteState& s = g_emoteState;

	const EmoteDef& d = EMOTES[e];
	EndFade( harry );
	s.active = true;
	s.emote = e;
	s.plays = 0;
	s.ending = -1;
	s.holding = s.finishing = false;
	s.start = NetNowMs();
	s.where = harry->m_position;
	s.health = harry->m_healthValue;
	g_emoteCancel = 0;
	BeginEmoteAnim( d.intro ? d.intro : d.clip, d.intro ? false : d.loops );
	mp_log( "[EM] Emote : %s (clip 0x%.8X%s%s).\n", d.name, d.clip, d.intro ? ", after its intro" : "", d.loops ? ", loops" : "" );
}

// Over : blended out (fadeMs 0 : at once).
static void StopEmote( EIHarry* harry, const char* why, uint32_t fadeMs = EMOTE_FADE_MS )
{
	EmoteState& s = g_emoteState;

	if ( !s.active ) return;
	s.active = false;

	if ( harry ) {
		EndFade( harry );
		// The layer showing : a clip just begun hasn't loaded yet, the one before it shows.
		g_emoteFade.layer = EMOTE_LAYERS[s.started ? s.slot : 1 - s.slot];
		g_emoteFade.since = NetNowMs();
		g_emoteFade.ms = fadeMs;
		g_emoteFade.on = true;
		AnimLayer* layer = GetAnimLayer( harry, g_emoteFade.layer );
		if ( !fadeMs || !layer || layer->m_weight <= 0.0F ) EndFade( harry );
	}
	mp_log( "[EM] Emote ended (%s) after %u ms, %d play%s.\n", why, NetNowMs() - s.start, s.plays, s.plays == 1 ? "" : "s" );
}

// Why Harry can't emote now, 0 if he can.
static const char* WhyNoEmote( EIHarry* harry )
{
	if ( harry->m_healthValue <= 0 ) return "You can't emote now";
	if ( GetHeldItem(harry) >= 0 ) return "Put your item away to emote";
	if ( !harry->m_onGround ) return "Land first to emote";
	if ( NetNowMs() - g_stillSince < 150 ) return "Stand still to emote";
	return 0;
}

static void EmoteTick( EIHarry* harry )
{
	const uint32_t now = NetNowMs();
	EAnimController* ctrl = &harry->m_animController;
	Vector3f at = harry->m_position;
	float dx = at.x - g_stillAt.x, dy = at.y - g_stillAt.y, dz = at.z - g_stillAt.z;
	if ( !g_stillSince || dx * dx + dy * dy + dz * dz > 0.05F * 0.05F ) { g_stillAt = at; g_stillSince = now; }

	EmoteState& s = g_emoteState;

	// The last one blending out.
	if ( g_emoteFade.on ) {
		AnimLayer* layer = GetAnimLayer( harry, g_emoteFade.layer );
		uint32_t gone = now - g_emoteFade.since;
		if ( !layer || gone >= g_emoteFade.ms ) EndFade( harry );
		else {
			KeepLayer( ctrl, layer );
			SetEmoteWeights( harry, g_emoteFade.layer, 1.0F - (float) gone / (float) g_emoteFade.ms );
		}
	}

	// Catch requested emote, reset it and play its clip.
	if ( g_emoteRequest >= 0 ) {
		int e = g_emoteRequest;
		g_emoteRequest = -1;
		const char* why = WhyNoEmote( harry );
		if ( why ) {
			mp_log( "[EM] No %s : %s.\n", EMOTES[e].name, why );
			Toast( "%s", why );
		}
		else {
			if ( s.active ) StopEmote( harry, "another one", 0 );
			StartEmote( harry, e );
		}
	}
	if ( !s.active ) return;
	const EmoteDef& d = EMOTES[s.emote];

	// Over : a button or a key, Harry moved (pushed, fell), an item came out, he got hurt.
	const char* over = g_emoteCancel;
	bool byYou = over != 0;
	g_emoteCancel = 0;
	dx = at.x - s.where.x; dy = at.y - s.where.y; dz = at.z - s.where.z;
	if ( !over && dx * dx + dy * dy + dz * dz > EMOTE_MOVE * EMOTE_MOVE ) over = "moved";
	if ( !over && !harry->m_onGround ) over = "off the ground";
	if ( !over && GetHeldItem( harry ) >= 0 ) over = "an item";
	if ( !over && harry->m_healthValue < s.health ) over = "hurt";
	if ( over ) {
		bool held = EmoteHoldsHarry();
		// A chain you end, or a hold you leave : its ending first (another press skips it) ...
		if ( byYou && !held && d.ending[0] && (d.intro || s.holding) ) {
			mp_log( "[EM] %s : its ending (%s).\n", d.name, over );
			s.ending = 0;
			BeginEmoteAnim( d.ending[0], false, d.endingFrom, d.endingFrom > 0.0F ? EMOTE_ENDING_BLEND_MS : 0 );
		}
		// ... or the rest of its clip (none after a hold on its last frame : it fades out).
		else if ( byYou && !held && s.holding && !(d.hold[0] < 0.0F && d.hold[1] < 0.0F) ) {
			mp_log( "[EM] %s : finishing (%s).\n", d.name, over );
			s.finishing = true;
			AnimLayer* mine = GetAnimLayer( harry, EMOTE_LAYERS[s.slot] );
			if ( mine ) mine->m_speed = 1.0F;
		}
		else {
			StopEmote( harry, over, held && byYou ? EMOTE_SKIP_FADE_MS : EMOTE_FADE_MS );
			return;
		}
	}

	if ( s.started ) {
		AnimLayer* mine = GetAnimLayer( harry, EMOTE_LAYERS[s.slot] );
		if ( mine ) {
			float frame = mine->m_frame;
			if ( HasHold( d ) && s.clip == d.clip && s.ending < 0 && !s.finishing ) {
				// Its hold : from its first frame on, it stays (between the two frames, again and again).
				ERAnim* clip = mine->m_anim;
				float last = clip ? (float) (clip->m_frames - 1) : 0.0F;
				float a = d.hold[0] < 0.0F ? last : d.hold[0], b = d.hold[1] < 0.0F ? last : d.hold[1];
				if ( frame >= a ) {
					if ( !s.holding ) { s.holding = true; s.holdSince = now; mp_log( "[EM] %s : held (frame %.0f).\n", d.name, frame ); }
					if ( b > a ) {
						// There and back (a clip has no loop of its own : jumping back shows), easing in and out
						// at each end (at its fastest, as fast as the clip plays).
						float fps = mine->m_fps;
						float half = fps > 0.0F ? (b - a) / fps * 1.5707963F : 1.0F;   // Seconds, a to b.
						float phase = (float) (now - s.holdSince) / 1000.0F / half;
						mine->m_speed = 0.0F;
						mine->m_frame = frame = a + (b - a) * 0.5F * (1.0F - cosf( 3.1415927F * phase ));
						s.plays = 1 + (int) (phase / 2.0F);
					}
					else if ( a < last ) { mine->m_speed = 0.0F; mine->m_frame = frame = a; }
				}
			}
			else if ( s.loop ) {   // The game loops it : count the plays.
				if ( frame < s.lastFrame ) s.plays++;
			}
			else {
				bool intro = d.intro && s.clip == d.intro;
				bool more = intro || (s.ending == 0 && d.ending[1]);
				float left = ClipSecondsLeft( mine );
				if ( !more && left <= EMOTE_FADE_MS / 1000.0F ) {   // Its last clip : blended out as it finishes.
					StopEmote( harry, "played", left > 0.05F ? (uint32_t) (left * 1000.0F) : 50 );
					return;
				}
				if ( mine->m_ended ) {   // The next clip, right away.
					if ( intro ) BeginEmoteAnim( d.clip, true );
					else if ( s.ending == 0 ) { s.ending = 1; BeginEmoteAnim( d.ending[1], false ); }
				}
			}
			s.lastFrame = frame;
		}
	}
	int key = EMOTE_LAYERS[s.slot];
	if ( !s.started ) {
		EAnimController_SetClip( ctrl, key, s.clip );
		EAnimController_Restart( ctrl, key );
		AnimLayer* layer = GetAnimLayer( harry, key );
		if ( layer ) {
			layer->m_atEnd = !s.loop;
			layer->m_speed = 1.0F;
			if ( s.startFrame > 0.0F ) layer->m_frame = s.startFrame;
		}
		if ( s.blendFrom >= 0 ) s.blendSince = now;
		else DropLayer( harry, EMOTE_LAYERS[1 - s.slot] );
		s.started = true;
		s.plays++;
	}
	AnimLayer* mine = GetAnimLayer( harry, key );
	if ( !mine || GetLayerClipCRC( mine ) != s.clip ) { StopEmote( harry, "the clip didn't load", 0 ); return; }
	KeepLayer( ctrl, mine );
	// Blending from the clip before (its last frame held) into this one.
	float w = 1.0F;
	AnimLayer* from = s.blendFrom >= 0 ? GetAnimLayer( harry, s.blendFrom ) : 0;
	if ( from && now - s.blendSince < s.blendMs ) {
		KeepLayer( ctrl, from );
		w = (float) (now - s.blendSince) / (float) s.blendMs;
	}
	else if ( s.blendFrom >= 0 ) {
		DropLayer( harry, s.blendFrom );
		s.blendFrom = -1;
	}
	SetEmoteWeights( harry, key, w, s.blendFrom, 1.0F - w );
}

// --------------------------------------------------------------------------------
// The Emotes page (F1) : a wheel's 8 slots round a ring, and choosing an emote for
// one, with a preview : the animation viewer's puppet beside Harry, playing the
// highlighted emote again and again.
// --------------------------------------------------------------------------------
static int g_editWheel = 0;       // The wheel the page shows.
int g_ringSlot = 0;        // Its slot the ring points at.
bool g_chooser = false;    // Choosing an emote for that slot.
char g_emoteSearch[32] = "";
int g_emoteCategory = 0;   // 0 : favourites, 1 : all, then EMOTE_CATEGORIES.
int g_listCursor = 0, g_listScroll = 0;
static int g_listWant = -2;       // EMOTES index to put the list's cursor on (-1 : "Empty slot"; -2 : none).
static int g_armed = 0;           // Pressed once, to be pressed again : 1 reset, 2 delete.
static DWORD g_armedAt = 0;
int g_previewWant = -2;    // The chooser, each frame : EMOTES index, -1 : none (idle), -2 : no preview.
static bool g_previewFailed = false;
static int g_previewShown = -3;
static int g_previewStage = 0;    // A chain : 0 its intro, 1 - 2 its loop, 3 - 4 its ending.
static bool g_previewArmed = false;   // The clip seen under way (not at its end from last time).
static const uint32_t IDLE_CLIP = 0xB23CB6D0;
void SetMenuOpen( bool open );
static void SaveSettings();

static void SetPreviewClip( uint32_t animCRC )
{
	g_animPreviewCRC = animCRC;
	if ( g_viewerPeer ) g_viewerPeer->mirrorClip = 0;   // Loads it again, from the start.
	g_previewArmed = false;
}

static bool OpenPreview( EIHarry* harry )
{
	// Its puppet : the one parked from last time, else one of a free player slot (not
	// the main one, so a puppet replaying Harry is left alone).
	Peer* vp = (g_parkedPeer && g_parkedPeer->beast && !g_parkedPeer->target) ? g_parkedPeer : 0;
	if ( !vp ) {
		int me = g_transport ? g_transport->PlayerId() : -1;
		for ( int i = MAX_PEERS - 1; i >= 0 && !vp; i-- )
			if ( i != me && &g_peers[i] != MainPeer() && !g_players[i].present && !g_peers[i].heard && !g_peers[i].target ) vp = &g_peers[i];
		if ( !vp ) { Toast( "No room for the preview (8 players)" ); return false; }
	}
	g_viewerPeer = vp;
	g_emotePreview = true;

	if ( !vp->beast ) SpawnPuppet( vp );

	if ( !vp->beast ) {
		g_emotePreview = false;
		g_viewerPeer = 0;
		mp_log( "[EM] No preview : Evil Harry can't be placed here.\n" );
		return false;
	}
	g_parkedPeer = vp;   // Parked when the preview closes.
	// Beside Harry as the camera sees him (along its right axis), facing the camera.
	float rx = 1.0F, ry = 0.0F;
	if ( g_haveCamera ) {
		float x = g_cameraWorld.data[0][0], y = g_cameraWorld.data[0][1], l = sqrtf( x * x + y * y );
		if ( l > 0.01F ) { rx = x / l; ry = y / l; }
	}
	g_viewerPin = harry->m_position;
	g_viewerPin.x += rx * 7.0F;
	g_viewerPin.y += ry * 7.0F;
	g_autoLocomotion = true;
	g_previewShown = -3;
	mp_log( "[EM] Preview : the puppet of slot %d.\n", vp->id );
	return true;
}

static void ClosePreview( const char* why )
{
	if ( !g_emotePreview ) return;
	g_emotePreview = false;
	g_parkedPin = g_viewerPin;
	g_parkedPin.z -= 5000.0F;
	if ( g_viewerPeer ) g_viewerPeer->mirrorClip = 0;
	g_viewerPeer = 0;
	g_animPreviewCRC = 0;
	mp_log( "[EM] Preview closed (%s); its puppet is parked.\n", why );
}

static bool IsMenuOpen();

static void EmotePreviewTick( EIHarry* harry )
{
	if ( !IsMenuOpen() ) { g_chooser = false; g_previewWant = -2; }
	// A parked puppet whose slot a player took (or that went with the level) : theirs, or gone.
	if ( g_parkedPeer && !g_emotePreview && (g_parkedPeer->target || !g_parkedPeer->beast || g_players[g_parkedPeer->id].present) ) g_parkedPeer = 0;
	bool want = g_previewWant >= -1 && !g_animViewer;
	if ( !want ) {
		ClosePreview( "done" );
		g_previewFailed = false;
		return;
	}
	if ( !g_emotePreview ) {
		if ( g_previewFailed ) return;
		if ( !OpenPreview(harry) ) {
			g_previewFailed = true;
			return;
		}
	}
	if ( !g_viewerPeer || !g_viewerPeer->beast ) {
		ClosePreview( "puppet gone" );
		g_previewFailed = true;
		return;
	}
	int e = g_previewWant;
	bool chain = e >= 0 && EMOTES[e].intro;
	if ( e != g_previewShown ) {
		g_previewShown = e;
		g_previewStage = 0;
		SetPreviewClip( e < 0 ? IDLE_CLIP : chain ? EMOTES[e].intro : EMOTES[e].clip );
		return;
	}
	// At the clip's end : again, or a chain's next (intro, its loop twice, its ending).
	AnimLayer* layer = GetAnimLayer( g_viewerPeer->beast, MIRROR_LAYERS[g_viewerPeer->mirrorSlot] );
	if ( !layer || GetLayerClipCRC(layer) != g_animPreviewCRC ) return;
	if ( !layer->m_ended ) {
		g_previewArmed = true;
		return;
	}
	if ( !g_previewArmed ) return;
	if ( !chain ) {
		SetPreviewClip( g_animPreviewCRC );
		return;
	}

	const EmoteDef& d = EMOTES[e];
	g_previewStage = (g_previewStage + 1) % 5;
	if ( g_previewStage == 4 && !d.ending[1] ) g_previewStage = 0;
	if ( g_previewStage == 3 && !d.ending[0] ) g_previewStage = 0;
	SetPreviewClip( g_previewStage == 0 ? d.intro : g_previewStage <= 2 ? d.clip : d.ending[g_previewStage - 3] );
}

static bool EmoteMatches( int e )
{
	const EmoteDef& d = EMOTES[e];
	if ( g_emoteCategory == 0 && !d.favourite ) return false;
	if ( g_emoteCategory >= 2 && d.category != g_emoteCategory - 2 ) return false;
	if ( g_emoteSearch[0] ) {
		char name[96], search[32];
		int n = 0;
		for ( const char* c = d.name; *c && n < 95; c++ ) name[n++] = (char) tolower( (unsigned char) *c );
		name[n] = 0;
		n = 0;
		for ( const char* c = g_emoteSearch; *c && n < 31; c++ ) search[n++] = (char) tolower( (unsigned char) *c );
		search[n] = 0;
		if ( !strstr( name, search ) ) return false;
	}
	return true;
}

static void OpenChooser()
{
	int e = g_emoteWheels[g_editWheel].slot[g_ringSlot];
	g_chooser = true;
	g_emoteSearch[0] = 0;
	g_listCursor = g_listScroll = 0;
	g_listWant = e;
	if ( e >= 0 && !EmoteMatches(e) ) g_emoteCategory = 1;
	mp_log( "[EM] Choosing an emote : %s, slot %d.\n", g_emoteWheels[g_editWheel].name, g_ringSlot + 1 );
}

static void HandleHotkeys( EIHarry* harry )
{
	bool f7 = InterlockedExchange( &g_reqDelay, 0 ) != 0;
	HandleAnimViewer( harry );
	EmotePreviewTick( harry );

	if ( f7 ) {
		g_delayIndex = (g_delayIndex + 1) % (sizeof(DELAY_CHOICES) / sizeof(DELAY_CHOICES[0]));
		mp_log( "[MP] Playback delay : %d ms.\n", DELAY_CHOICES[g_delayIndex] );
	}
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
void GrabScreen( const char* name )
{
	HWND wnd = *HWND_GAME_WINDOW;
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
volatile LONG g_grabRequested = 0;
char g_grabName[64];
HANDLE g_grabDone = 0;

static void GrabBackBuffer( IDirect3DDevice9* dev, const char* name, const RECT* crop )
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
		int x0 = 0, y0 = 0, w = d.Width, h = d.Height;
		if ( crop ) {   // Clamped to the frame.
			x0 = max( 0, (int) crop->left ); y0 = max( 0, (int) crop->top );
			w = min( (int) d.Width, (int) crop->right ) - x0; h = min( (int) d.Height, (int) crop->bottom ) - y0;
			if ( w <= 0 || h <= 0 ) { x0 = y0 = 0; w = d.Width; h = d.Height; }
		}
		int bpp = (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) ? 4 : 2;
		int stride = ((w * 3) + 3) & ~3;
		uint8_t* bits = (uint8_t*) calloc( stride, h );
		for ( int y = 0; bits && y < h; y++ ) {
			const uint8_t* src = (const uint8_t*) lr.pBits + (y0 + y) * lr.Pitch + x0 * bpp;
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
		if ( bits ) WriteBmp( name, w, h, bits );
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
	if ( g_tagDiagOn ) {
		g_tagDiag.frames++;
		if ( g_tagDiag.lookAtsThisFrame > g_tagDiag.maxLookAts ) g_tagDiag.maxLookAts = g_tagDiag.lookAtsThisFrame;
		g_tagDiag.lookAtsThisFrame = 0;
		if ( g_rendFrame + 1 == g_presentCount && g_rendSetsThisFrame >= 2 && g_rendTwiceLogged < 6 ) {
			g_rendTwiceLogged++;
			mp_log( "[AT] TAGS renderer view set %u times in a frame :", g_rendSetsThisFrame );
			for ( uint32_t i = 0; i < g_rendSetsThisFrame && i < 4; i++ ) mp_log( " (%.1f, %.1f, %.1f)", g_rendEyes[i].x, g_rendEyes[i].y, g_rendEyes[i].z );
			mp_log( "; look-at camera (%.1f, %.1f, %.1f).\n", g_cameraWorld.data[3][0], g_cameraWorld.data[3][1], g_cameraWorld.data[3][2] );
		}
	}
	if ( g_cropPending ) {
		GrabBackBuffer( dev, g_cropName, &g_crop );
		InterlockedExchange( &g_cropPending, 0 );
	}
	if ( g_grabRequested ) {
		GrabBackBuffer( dev, g_grabName, 0 );
		InterlockedExchange( &g_grabRequested, 0 );
		SetEvent( g_grabDone );
	}
	return g_origPresent( dev, src, dst, wnd, dirty );
}

// Hooks the device's Present once it exists.
// --------------------------------------------------------------------------------
// The Retro palette. Retro Harry's model is 15 pieces, all textured with one 32x32
// DXT1 texture of four flat squares : shirt (top left), trousers (top right), skin
// (bottom left), hair (bottom right). The game draws every Retro character's pieces
// one after the other with that texture set (DrawPrimitive, a vertex shader whose
// constants c44..c46 are the piece's first bone, in the world). So when a piece is
// about to be drawn with it, the character it belongs to is the one standing there,
// and that player's own palette is put in its place for the draw.
// --------------------------------------------------------------------------------
typedef HRESULT (__stdcall *SetTextureFunc)( IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9* );
typedef HRESULT (__stdcall *DrawPrimFunc)( IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT );
static const int VTABLE_SLOT_SETTEXTURE = 65, VTABLE_SLOT_DRAWPRIMITIVE = 81;
static SetTextureFunc g_origSetTexture = 0;
static DrawPrimFunc g_origDrawPrim = 0;
static IDirect3DBaseTexture9* g_stage0 = 0;   // The texture the game has set.
static bool g_stage0Retro = false;            // It's the Retro palette.
int g_retroSwaps = 0;                  // Pieces drawn with a player's palette (for the autotest).

// Is this texture the game's Retro palette ? Asked for each texture set, so the
// answer is remembered : the palette itself, and the few other textures of its size
// and kind that had to be looked into. Forgotten when a level loads.
static IDirect3DBaseTexture9* g_retroPalette = 0;
static IDirect3DBaseTexture9* g_retroNot[8];
static int g_retroNotNext = 0;

static void ForgetRetroTextures()
{
	g_retroPalette = 0;
	ZeroMemory( g_retroNot, sizeof(g_retroNot) );
	g_stage0 = 0;
	g_stage0Retro = false;
}

static bool IsRetroPalette( IDirect3DBaseTexture9* tex )
{
	if ( tex == g_retroPalette ) return true;
	for ( int i = 0; i < 8; i++ ) if ( g_retroNot[i] == tex ) return false;
	bool retro = false;
	D3DSURFACE_DESC desc;
	IDirect3DTexture9* t = (IDirect3DTexture9*) tex;
	if ( tex->GetType() != D3DRTYPE_TEXTURE || FAILED( t->GetLevelDesc( 0, &desc ) )
	  || desc.Width != 32 || desc.Height != 32 || desc.Format != D3DFMT_DXT1 ) return false;
	{
		// One DXT1 block (two colours, 16 two-bit choices) per square is enough to tell.
		static const uint32_t BLOCKS[4][2] = { { 0x46496E29, 0xFFFFFFFF }, { 0x02C002A0, 0xAAAAAAAA }, { 0xFD10FD0F, 0x00000000 }, { 0x8BA893C7, 0xFFFFFFFF } };
		D3DLOCKED_RECT lr;
		if ( SUCCEEDED( t->LockRect( 0, &lr, 0, D3DLOCK_READONLY ) ) ) {
			retro = true;
			for ( int q = 0; q < 4; q++ ) {
				const uint32_t* block = (const uint32_t*) ((const uint8_t*) lr.pBits + (q / 2) * 4 * lr.Pitch + (q % 2) * 4 * 8);
				if ( block[0] != BLOCKS[q][0] || block[1] != BLOCKS[q][1] ) retro = false;
			}
			t->UnlockRect( 0 );
		}
	}
	if ( retro ) {
		g_retroPalette = tex;
		mp_log( "[MP] The Retro palette is texture 0x%.8X.\n", (uint32_t) tex );
	}
	else {
		g_retroNot[g_retroNotNext] = tex;
		g_retroNotNext = (g_retroNotNext + 1) % 8;
	}
	return retro;
}

// A player's palette : the same four squares in their colours. One per player
// (the last is ours), made and repainted as needed.
static struct { IDirect3DTexture9* tex; uint8_t colors[RETRO_PARTS][3]; } g_retroTex[MAX_PEERS + 1];

static IDirect3DTexture9* RetroTexture( IDirect3DDevice9* dev, int slot, const uint8_t colors[RETRO_PARTS][3] )
{
	bool paint = false;
	if ( !g_retroTex[slot].tex ) {
		if ( FAILED( dev->CreateTexture( 32, 32, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &g_retroTex[slot].tex, 0 ) ) ) return 0;
		paint = true;
	}
	if ( paint || memcmp( g_retroTex[slot].colors, colors, sizeof(g_retroTex[slot].colors) ) != 0 ) {
		static const int SQUARE[2][2] = { { RETRO_SHIRT, RETRO_TROUSERS }, { RETRO_SKIN, RETRO_HAIR } };   // [row][column].
		D3DLOCKED_RECT lr;
		if ( FAILED( g_retroTex[slot].tex->LockRect( 0, &lr, 0, 0 ) ) ) return 0;
		for ( int y = 0; y < 32; y++ ) {
			uint32_t* row = (uint32_t*) ((uint8_t*) lr.pBits + y * lr.Pitch);
			for ( int x = 0; x < 32; x++ ) {
				const uint8_t* c = colors[SQUARE[y / 16][x / 16]];
				row[x] = 0xFF000000 | (c[0] << 16) | (c[1] << 8) | c[2];
			}
		}
		g_retroTex[slot].tex->UnlockRect( 0 );
		memcpy( g_retroTex[slot].colors, colors, sizeof(g_retroTex[slot].colors) );
	}
	return g_retroTex[slot].tex;
}

// Whose piece is about to be drawn : the palette of the Retro character it belongs
// to (0 : the game's own, they kept its colours). The piece's first bone (constants
// c44..c46) is one of its character's bone matrices as they are right now, so the
// character is the one whose pose has that very matrix : no guessing from where
// players stand, which mixed their colours up when they stood close together. A
// piece whose bone isn't found (not seen so far) goes to the previous piece's owner :
// a character's 15 pieces are drawn in a row.
static int g_retroOwner = -1;   // The slot of g_retroTex (-1 : nobody yet since the palette was set).
static const uint8_t (*g_retroOwnerColors)[3] = 0;
int g_retroMatched = 0, g_retroUnmatched = 0;   // For the autotest.

static bool HasBoneAt( EICharacter* c, const float bone[3][4] )
{
	const float E = 0.01F;

	int bones = 0;
	const Matrix4f* pose = GetPose( &c->m_animController, &bones );
	if ( !pose || bones <= 0 || bones > MAX_CHAR_BONES ) return false;
	for ( int i = 0; i < bones; i++ ) {
		const float* t = pose[i].data[3];
		if ( fabsf(t[0] - bone[0][3]) < E && fabsf(t[1] - bone[1][3]) < E && fabsf(t[2] - bone[2][3]) < E ) return true;
	}
	return false;
}

static IDirect3DTexture9* RetroTextureHere( IDirect3DDevice9* dev )
{
	float bone[3][4];   // Rows : the translation is each row's w.
	if ( FAILED( dev->GetVertexShaderConstantF( 44, &bone[0][0], 3 ) ) ) return 0;
	int me = g_transport ? g_transport->PlayerId() : -1;
	bool found = false;
	for ( int i = -1; i < MAX_PEERS && !found; i++ ) {   // -1 : our own Harry.
		EICharacter* who;
		const uint8_t (*c)[3];
		if ( i < 0 ) {
			EIHarry* harry = Gizmod::getHarry();
			if ( !harry || g_myWornOutfit != RETRO_OUTFIT ) continue;
			who = harry;
			c = g_myColors;
		}
		else {
			const Peer& p = g_peers[i];
			if ( !p.beast || PeerInLimbo(&p) || p.shownOutfit != RETRO_OUTFIT ) continue;
			who = p.beast;
			c = (i == me || !g_networked || !g_players[i].present) ? g_myColors : g_players[i].colors;
		}
		if ( !HasBoneAt( who, bone ) ) continue;
		g_retroOwner = i < 0 ? MAX_PEERS : i;
		g_retroOwnerColors = c;
		found = true;
	}
	if ( found ) g_retroMatched++; else g_retroUnmatched++;
	if ( g_retroOwner < 0 || memcmp( g_retroOwnerColors, RETRO_DEFAULT, sizeof(RETRO_DEFAULT) ) == 0 ) return 0;
	return RetroTexture( dev, g_retroOwner, g_retroOwnerColors );
}

static HRESULT __stdcall SetTexture_Hook( IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex )
{
	if ( stage == 0 ) {
		g_stage0 = tex;
		g_stage0Retro = tex && IsRetroPalette( tex );
		g_retroOwner = -1;
	}
	return g_origSetTexture( dev, stage, tex );
}

static HRESULT __stdcall DrawPrim_Hook( IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT prims )
{
	IDirect3DTexture9* own = g_stage0Retro ? RetroTextureHere( dev ) : 0;
	if ( !own ) return g_origDrawPrim( dev, type, start, prims );
	g_origSetTexture( dev, 0, own );
	HRESULT hr = g_origDrawPrim( dev, type, start, prims );
	g_origSetTexture( dev, 0, g_stage0 );
	g_retroSwaps++;
	return hr;
}

bool HookPresent()
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
	g_origSetTexture = (SetTextureFunc) vtable[VTABLE_SLOT_SETTEXTURE];
	injector::WriteMemory( &vtable[VTABLE_SLOT_SETTEXTURE], &SetTexture_Hook, true );
	g_origDrawPrim = (DrawPrimFunc) vtable[VTABLE_SLOT_DRAWPRIMITIVE];
	injector::WriteMemory( &vtable[VTABLE_SLOT_DRAWPRIMITIVE], &DrawPrim_Hook, true );
	mp_log( "[AT] Screenshots come from the Direct3D back buffer.\n" );
	return true;
}



//
// Input hooks (keyboard + gamepad).
//

#include <dinput.h>

// The game polls the keyboard (0x6E8A80, called from 0x663BBE) through DirectInput
// in foreground mode : the keys down now at 0x940EC0, a frame earlier at 0x940C90
// (all up while the window isn't in front). In background autotests the keys come
// from the test instead, whatever window is in front.
static const int DIRECTINPUT_NUM_KEYS = 256;
static uint8_t* const g_directInputKeysCurrent = (uint8_t*) 0x940EC0;
static uint8_t* const g_directInputKeysLast = (uint8_t*) 0x940C90;

static const uint32_t POLL_KEYBOARD_CALL = 0x663BBE, POLL_GAMEPAD_CALL = 0x663BAD;

// The state is a DIJOYSTATE2 (0x110 bytes, 0x940B78 + index * 0x110; the game
// keeps the one before at 0x940DA8 + ...) : the d-pad is the hat at +0x20 (hundredths
// of a degree, 0xFFFF... : centred), the buttons at +0x30.
static DIJOYSTATE2 g_gamepadSeen;   // The last state polled (the Keys page reads its buttons).
static bool g_padSeenAny = false;

static bool g_inputBlocked = false;   // Should all input to the game be blocked (e.g. when F1 menu brought up).

// Custom in-game handlers (when not in menu).
static void CustomGameInput_Keyboard( uint8_t* keys );
static void CustomGameInput_Gamepad( DIJOYSTATE2* pad );


// DirectInput key code of a virtual key (the arrows are "extended" keys).
uint8_t KeyToDIK( WORD vk )
{
	switch ( vk ) {
	case VK_UP:    return 0xC8;
	case VK_DOWN:  return 0xD0;
	case VK_LEFT:  return 0xCB;
	case VK_RIGHT: return 0xCD;
	}
	return (uint8_t) MapVirtualKeyA( vk, MAPVK_VK_TO_VSC );
}


static const auto POLL_KEYBOARD_ORIGINAL = (void (*)()) 0x6E8A80;
static void PollKeyboard_Hook()
{
	if ( !TestPollKeyboard(POLL_KEYBOARD_ORIGINAL, g_directInputKeysCurrent, g_directInputKeysLast) )
		POLL_KEYBOARD_ORIGINAL();

	if ( g_inputBlocked ) {
		memset( g_directInputKeysCurrent, 0, DIRECTINPUT_NUM_KEYS );
	}
	else {
		CustomGameInput_Keyboard( g_directInputKeysCurrent );
	}

	TestSawKeys( g_directInputKeysCurrent );
}

static const auto POLL_GAMEPAD_ORIGINAL = (DIJOYSTATE2* (__cdecl*)(int)) 0x6E8660;
static DIJOYSTATE2* __cdecl PollGamepad_Hook( int index )
{
	DIJOYSTATE2* state = TestPollGamepad( index, POLL_GAMEPAD_ORIGINAL(index) );
	if ( !state ) return 0;

	memcpy( &g_gamepadSeen, state, sizeof(DIJOYSTATE2) );
	g_padSeenAny = true;

	if ( g_inputBlocked ) {
		return 0;
	}
	else {
		CustomGameInput_Gamepad( state );   // The game's (g_gamepadSeen stays as the controller is).
		TestSawGamepad( state );
		return state;
	}
}

// TODO: should be core.
static void InstallInputHooks()
{
	static const uint8_t POLL_ORIGINAL[1] = { 0xE8 };
	if ( memcmp((void*) POLL_KEYBOARD_CALL, POLL_ORIGINAL, 1) == 0 )
		injector::MakeCALL( POLL_KEYBOARD_CALL, &PollKeyboard_Hook );
	else
		mp_log( "[MP] The keyboard poll call isn't what we expect : keys reach the game while the menu is open.\n" );

	static const uint8_t PAD_ORIGINAL[5] = { 0xE8, 0xAE, 0x4A, 0x08, 0x00 };   // call 0x6E8660.
	if ( memcmp( (void*) POLL_GAMEPAD_CALL, PAD_ORIGINAL, 5 ) == 0 )
		injector::MakeCALL( POLL_GAMEPAD_CALL, &PollGamepad_Hook );
	else
		mp_log( "[MP] The gamepad poll call isn't what we expect : the gamepad reaches the game while the menu is open.\n" );
}



// --------------------------------------------------------------------------------
// The emote wheel : held open (L1, or T), pointed at with the right stick (the
// mouse or the arrows), played when let go (1-8 and a click play at once). R1 (Tab) :
// the next wheel. While it's open the game doesn't get the right stick and R1 (the
// arrows, Tab, 1-8). While Harry emotes, any other button or key, or the left stick,
// ends it (a chain plays its ending first).
// --------------------------------------------------------------------------------
const int PAD_R1 = 5;
static const int DIK_ARROWS[4] = { 0xC8, 0xD0, 0xCB, 0xCD };   // DirectInput key codes : up, down, left, right.

// The game's controls (its options, kept in the registry) : 20 per controller (0x941020,
// 0x2C apart; a button is its number + 11) and for the keyboard (0x940FC0, DIK codes).
// 4 - 7 : the right stick (reaching), 14 - 15 : turning the camera.
const uint32_t PAD_BINDINGS = 0x941020, KEY_BINDINGS = 0x940FC0;
static bool IsCameraButton( int b )
{
	const int* bind = (const int*) PAD_BINDINGS;
	return bind[14] == b + 11 || bind[15] == b + 11;
}

static const uint32_t WHEEL_SHOW_MS = 150;   // Held this long (or pointed) : the wheel shows (a tap of L1 doesn't).
static const uint32_t WHEEL_KEEP_MS = 300;   // The stick back in the middle : the choice stays this long.

WheelUi g_wheelUi;

bool EmoteWheelShown()
{
	return g_wheelUi.open && !g_wheelUi.done && (g_wheelUi.shown || NetNowMs() - g_wheelUi.since >= WHEEL_SHOW_MS);
}

void EmoteWheelOpen( const DIJOYSTATE2* pad )
{
	memset( &g_wheelUi, 0, sizeof(g_wheelUi) );
	g_wheelUi.open = true;
	g_wheelUi.byPad = pad != 0;
	g_wheelUi.since = NetNowMs();
	g_wheelUi.hover = -1;
	if ( g_wheelIndex >= g_wheelCount ) g_wheelIndex = 0;
	if ( pad ) {
		for ( int i = 0; i < 8; i++ ) g_wheelUi.centre[i] = ((const LONG*) pad)[i];
		g_wheelUi.r1Was = (pad->rgbButtons[PAD_R1] & 0x80) != 0;
		mp_log( "[EM] Wheel opened (controller; axes %ld %ld %ld %ld %ld %ld).\n", g_wheelUi.centre[0], g_wheelUi.centre[1], g_wheelUi.centre[2],
			g_wheelUi.centre[3], g_wheelUi.centre[4], g_wheelUi.centre[5] );
	}
	else {
		GetCursorPos( &g_wheelUi.mouse );
		mp_log( "[EM] Wheel opened (keyboard).\n" );
	}
}

static void WheelPoint( int slot )
{
	g_wheelUi.hover = slot;
	g_wheelUi.hoverAt = NetNowMs();
	g_wheelUi.shown = true;
}

static void WheelNext()
{
	if ( g_wheelCount > 1 ) g_wheelIndex = (g_wheelIndex + 1) % g_wheelCount;
	g_wheelUi.hover = -1;
	g_wheelUi.shown = true;
	mp_log( "[EM] Wheel : %s.\n", g_emoteWheels[g_wheelIndex].name );
}

static void WheelPlay( int slot, const char* how )
{
	g_wheelUi.done = true;
	if ( slot < 0 ) return;
	int e = g_emoteWheels[g_wheelIndex].slot[slot];
	mp_log( "[EM] Wheel %s, slot %d (%s) : %s.\n", g_emoteWheels[g_wheelIndex].name, slot + 1, how, e >= 0 ? EMOTES[e].name : "empty" );
	if ( e >= 0 ) g_emoteRequest = e;
}

// The slot a direction points at (dy : down), the first at the top, clockwise.
static int WedgeAt( float dx, float dy )
{
	float a = atan2f( dx, -dy );
	if ( a < 0.0F ) a += 6.2831853F;
	return ((int) floorf( (a + 0.3926991F) / 0.7853982F )) % WHEEL_SLOTS;
}

static void Emote_Gamepad( DIJOYSTATE2* state )
{
	LONG* axes = (LONG*) state;
	uint8_t* buttons = state->rgbButtons;
	static bool big[8] = { false, };   // The axis goes 0 .. 65535 (or so), not -1000 .. 1000.
	for ( int i = 0; i < 8; i++ ) if ( axes[i] > 1100 || axes[i] < -1100 ) big[i] = true;
	DWORD hat = state->rgdwPOV[0];
	bool hatCentred = LOWORD( hat ) == 0xFFFF;
	bool held = g_emoteButton >= 0 && g_emoteButton < 32 && (buttons[g_emoteButton] & 0x80);
	bool r1 = (buttons[PAD_R1] & 0x80) != 0;
	bool combo = held && (!g_emoteNeedsR1 || r1);
	static bool comboWas = false;
	static uint8_t before[32];
	static DWORD hatBefore = 0xFFFFFFFF;
	uint8_t raw[32];
	memcpy( raw, buttons, sizeof(raw) );

	if ( combo && !comboWas && !g_wheelUi.open ) EmoteWheelOpen( state );
	comboWas = combo;
	if ( g_wheelUi.open && g_wheelUi.byPad ) {
		if ( !held ) {
			if ( !g_wheelUi.done ) WheelPlay( g_wheelUi.hover, "let go" );
			g_wheelUi.open = false;
		}
		else {
			if ( r1 && !g_wheelUi.r1Was && !g_wheelUi.done ) WheelNext();
			g_wheelUi.r1Was = r1;
			// The right stick : a DualShock's (Z, Rz) or an Xbox controller's (Rx, Ry), whichever leans more.
			static const int PAIRS[2][2] = { { 2, 5 }, { 3, 4 } };
			float best = 0.0F, bx = 0.0F, by = 0.0F;
			for ( int p = 0; p < 2; p++ ) {
				int ix = PAIRS[p][0], iy = PAIRS[p][1];
				float x = (axes[ix] - g_wheelUi.centre[ix]) / (big[ix] ? 32767.0F : 1000.0F);
				float y = (axes[iy] - g_wheelUi.centre[iy]) / (big[iy] ? 32767.0F : 1000.0F);
				if ( x * x + y * y > best ) { best = x * x + y * y; bx = x; by = y; }
			}
			if ( !g_wheelUi.done ) {
				if ( best > 0.5F * 0.5F ) WheelPoint( WedgeAt( bx, by ) );
				else if ( g_wheelUi.hover >= 0 && NetNowMs() - g_wheelUi.hoverAt > WHEEL_KEEP_MS ) g_wheelUi.hover = -1;
			}
			// Hide and seek's role swap is the same button and the d-pad : no wheel then.
			if ( PlayingHs() && g_emoteButton == g_swapButton && !hatCentred ) g_wheelUi.done = true;
			for ( int i = 2; i < 8; i++ ) axes[i] = g_wheelUi.centre[i];   // The right stick : Z .. Rz, or a slider (the game's options).
			buttons[PAD_R1] = 0;
		}
	}
	// Emoting : the left stick, another button or the d-pad end it. A chain's ending
	// keeps Harry where he is (a new press skips it, and gets to the game next time).
	else if ( g_emoteState.active ) {
		static uint32_t restFor = 0;
		static LONG rest[2];
		if ( restFor != g_emoteState.start ) { restFor = g_emoteState.start; rest[0] = axes[0]; rest[1] = axes[1]; }
		float x = (axes[0] - rest[0]) / (big[0] ? 32767.0F : 1000.0F), y = (axes[1] - rest[1]) / (big[1] ? 32767.0F : 1000.0F);
		bool hold = EmoteHoldsHarry();
		const char* why = 0;
		if ( !hold && x * x + y * y > 0.4F * 0.4F ) why = "the left stick";
		for ( int b = 0; b < 32 && !why; b++ )
			if ( (raw[b] & 0x80) && !(before[b] & 0x80) && b != g_emoteButton && b != PAD_R1 && !IsCameraButton( b ) ) why = "a button";
		if ( !why && !hatCentred && LOWORD( hatBefore ) == 0xFFFF ) why = "the d-pad";
		if ( why && !g_emoteCancel ) g_emoteCancel = why;
		if ( hold ) {   // (The camera still turns.)
			axes[0] = rest[0]; axes[1] = rest[1];
			for ( int b = 0; b < 32; b++ ) if ( !IsCameraButton( b ) ) buttons[b] = 0;
			state->rgdwPOV[0] = 0xFFFFFFFF;
		}
	}
	// The right stick reaches with Harry's arm (and aims some items) : not while it points
	// the wheel, nor after, until it's back in the middle.
	static bool stickHeld = false;
	static LONG stickCentre[8];
	if ( g_wheelUi.open && g_wheelUi.byPad ) { stickHeld = true; memcpy( stickCentre, g_wheelUi.centre, sizeof(stickCentre) ); }
	else if ( stickHeld ) {
		bool centred = true;
		for ( int i = 2; i < 8; i++ ) if ( fabsf( (axes[i] - stickCentre[i]) / (big[i] ? 32767.0F : 1000.0F) ) > 0.25F ) centred = false;
		if ( centred ) stickHeld = false;
		else for ( int i = 2; i < 8; i++ ) axes[i] = stickCentre[i];
	}
	memcpy( before, raw, sizeof(before) );
	hatBefore = hat;
}

// Keys that don't end an emote : the wheel's, the camera's, the right stick's (it reaches
// behind the emote), the function keys, Shift, Ctrl, Alt.
static bool KeyKeepsEmote( int k )
{
	const int* bind = (const int*) KEY_BINDINGS;
	for ( int i = 4; i <= 7; i++ ) if ( bind[i] == k ) return true;
	if ( bind[14] == k || bind[15] == k ) return true;
	if ( k == g_emoteKey || k == 0x0F || (k >= 0x02 && k <= 0x09) || (k >= 0x3B && k <= 0x44) || k == 0x57 || k == 0x58 ) return true;
	return k == 0x2A || k == 0x36 || k == 0x1D || k == 0x9D || k == 0x38 || k == 0xB8;
}

static void Emote_Keyboard( uint8_t* keys )
{
	static uint8_t before[256];
	uint8_t raw[256];
	memcpy( raw, keys, sizeof(raw) );
	bool held = g_emoteKey > 0 && g_emoteKey < 256 && (raw[g_emoteKey] & 0x80);
	static bool heldWas = false;
	if ( held && !heldWas && !g_wheelUi.open ) EmoteWheelOpen( 0 );
	heldWas = held;
	if ( g_wheelUi.open && !g_wheelUi.byPad ) {
		if ( !held ) {
			if ( !g_wheelUi.done ) WheelPlay( g_wheelUi.hover, "let go" );
			g_wheelUi.open = false;
		}
		else if ( !g_wheelUi.done ) {
			if ( (raw[0x0F] & 0x80) && !(before[0x0F] & 0x80) ) WheelNext();   // Tab.
			// The arrows (the keyboard's right stick) : 8 ways. Letting go of a diagonal keeps it.
			int ax = ((raw[0xCD] & 0x80) ? 1 : 0) - ((raw[0xCB] & 0x80) ? 1 : 0), ay = ((raw[0xD0] & 0x80) ? 1 : 0) - ((raw[0xC8] & 0x80) ? 1 : 0);
			int arrows = 0, arrowsBefore = 0;
			for ( int i = 0; i < 4; i++ ) { arrows += (raw[DIK_ARROWS[i]] & 0x80) ? 1 : 0; arrowsBefore += (before[DIK_ARROWS[i]] & 0x80) ? 1 : 0; }
			if ( (ax || ay) && arrows >= arrowsBefore ) WheelPoint( WedgeAt( (float) ax, (float) ay ) );
			for ( int i = 0; i < WHEEL_SLOTS && !g_wheelUi.done; i++ )   // 1 - 8.
				if ( (raw[0x02 + i] & 0x80) && !(before[0x02 + i] & 0x80) ) { WheelPoint( i ); WheelPlay( i, "number key" ); }
			// The mouse : which way it went (the start follows it, so turning back is quick).
			POINT p;
			if ( !g_wheelUi.done && !TestInBackground() && GameHasFocus() && GetCursorPos(&p) ) {
				float dx = (float) (p.x - g_wheelUi.mouse.x), dy = (float) (p.y - g_wheelUi.mouse.y);
				float d = sqrtf( dx * dx + dy * dy );
				if ( d > 30.0F ) WheelPoint( WedgeAt( dx, dy ) );
				if ( d > 80.0F ) { g_wheelUi.mouse.x = p.x - (LONG) (dx * 80.0F / d); g_wheelUi.mouse.y = p.y - (LONG) (dy * 80.0F / d); }
				bool click = (GetAsyncKeyState( VK_LBUTTON ) & 0x8000) != 0;
				if ( click && !g_wheelUi.clickWas && g_wheelUi.hover >= 0 ) WheelPlay( g_wheelUi.hover, "click" );
				g_wheelUi.clickWas = click;
			}
		}
		keys[0x0F] = 0;
		for ( int i = 0; i < WHEEL_SLOTS; i++ ) keys[0x02 + i] = 0;
	}
	else if ( g_emoteState.active ) {
		for ( int k = 1; k < 256; k++ )
			if ( (raw[k] & 0x80) && !(before[k] & 0x80) && !KeyKeepsEmote( k ) ) { if ( !g_emoteCancel ) g_emoteCancel = "a key"; break; }
		if ( EmoteHoldsHarry() ) for ( int k = 1; k < 256; k++ ) if ( !KeyKeepsEmote( k ) ) keys[k] = 0;
	}
	// The arrows (and the game's right stick keys) : not for the game while the wheel is out, nor
	// after, until they're let go. Whichever opened it : a controller's right stick can come as
	// the arrows too (JoyToKey and the like).
	int stickKeys[8];
	for ( int i = 0; i < 4; i++ ) { stickKeys[i] = DIK_ARROWS[i]; stickKeys[4 + i] = ((const int*) KEY_BINDINGS)[4 + i]; }
	static bool arrowsHeld = false;
	bool anyArrow = false;
	for ( int i = 0; i < 8; i++ ) if ( stickKeys[i] > 0 && stickKeys[i] < 256 ) anyArrow |= (raw[stickKeys[i]] & 0x80) != 0;
	if ( g_wheelUi.open ) arrowsHeld = true;
	else if ( !anyArrow ) arrowsHeld = false;
	if ( arrowsHeld ) for ( int i = 0; i < 8; i++ ) if ( stickKeys[i] > 0 && stickKeys[i] < 256 ) keys[stickKeys[i]] = 0;
	if ( held ) keys[g_emoteKey] = 0;
	memcpy( before, raw, sizeof(before) );
}

// Role swap controls.
static void RoleSwap_Keyboard( uint8_t* keys )
{
	bool held = g_swapKey > 0 && g_swapKey < 256 && (keys[g_swapKey] & 0x80);
	bool left = (keys[0xCB] & 0x80) != 0, right = (keys[0xCD] & 0x80) != 0;
	bool any = false;
	for ( int i = 0; i < 4; i++ ) any |= (keys[DIK_ARROWS[i]] & 0x80) != 0;
	static bool wasLeft = false, wasRight = false, masking = false;
	if ( !PlayingHs() ) {
		wasLeft = left;
		wasRight = right;
		masking = false;
		return;
	}
	if ( held && ((left && !wasLeft) || (right && !wasRight)) )
		HsToggleRole( "keyboard" );
	wasLeft = left; wasRight = right;
	if ( held ) masking = true;
	if ( masking ) {
		if ( !held && !any )
			masking = false;
		else
			for ( int i = 0; i < 4; i++ )
				keys[DIK_ARROWS[i]] = 0;
	}
}

static void RoleSwap_Gamepad( DIJOYSTATE2* state )
{
	DWORD& hat = state->rgdwPOV[0];
	bool centred = LOWORD( hat ) == 0xFFFF;
	int dir = 0;   // -1 : left, 1 : right.
	if ( !centred && hat > 4500 && hat < 13500 ) dir = 1;
	if ( !centred && hat > 22500 && hat < 31500 ) dir = -1;
	bool held = g_swapButton >= 0 && g_swapButton < 128 && (state->rgbButtons[g_swapButton] & 0x80);
	static int lastDir = 0;
	static bool masking = false;
	if ( !PlayingHs() ) {
		lastDir = dir;
		masking = false;
		return;
	}
	if ( held && dir && dir != lastDir ) HsToggleRole( "controller" );
	lastDir = dir;
	if ( held ) masking = true;
	if ( masking ) {   // Until the d-pad is let go too : no item when the button goes first.
		if ( !held && centred ) masking = false;
		else hat = 0xFFFFFFFF;
	}
}

static void CustomGameInput_Keyboard( uint8_t* keys )
{
	Emote_Keyboard( keys );
	RoleSwap_Keyboard( keys );
}

static void CustomGameInput_Gamepad( DIJOYSTATE2* pad )
{
	Emote_Gamepad( pad );
	RoleSwap_Gamepad( pad );
}



void ApplyMyOutfit();


static void __stdcall _EIHarry_Tick_Hook( EIHarry* self )
{
	TestBeforeHarryTick();

	EIHarry_Tick( self );

	HookGameWindowProc();

	g_harryTickAt = GetTickCount();
	if ( InterlockedIncrement(&g_harryTicks) == 1 ) {
		mp_log( "[MP] Harry tick hook is running (this 0x%.8X, global Harry 0x%.8X).\n", self, Gizmod::getHarry() );
		if ( !g_autotest )
			Toast( "F1 : multiplayer menu" );
	}

	// TODO: this is useless at the moment. Though we might spawn actual Harrys in the future.
	if ( self != Gizmod::getHarry() ) return;

	ERLevel* world = self->m_world;

	// Another level since the puppet was spawned. The load events don't cover every
	// way of changing levels (the "Go to" menu, loading a save), this does.
	// TODO: Gizmod 0.4 gotta get on top of this.
	if ( g_currentWorld != world || g_currentCRC != Gizmod::getCurrentLevelCRC() ) {
		g_currentWorld = world;   // So this change isn't seen again.
		g_currentCRC = Gizmod::getCurrentLevelCRC();

		ForgetPuppets();
	}
	DropLostPuppets();
	ReplaceStalledPuppets();
	CheckLevelLeaving( world );

	if ( !g_origPresent ) {   // The overlay draws as frames are presented.
		HookPresent();
		InterlockedExchange( &g_reqMenu, 0 );   // An F1 pressed before there was anything to draw (the main menu) doesn't count.
	}
	EmoteTick( self );
	PushLocalSnapshot( self );
	NetUpdate();
	HsUpdate( self );
	// Another player in this level : show them (Evil Harry appears on his own).
	// (The autotest spawns the main peer's itself, when its steps need it.)
	if ( g_networked ) {
		uint32_t now = NetNowMs();
		for ( int i = 0; i < MAX_PEERS; i++ ) {
			Peer* p = &g_peers[i];
			if ( p->beast || !p->target || now - p->lastSpawnTry < 1000 ) continue;
			if ( TestSpawnsPuppet(p) ) continue;

			p->lastSpawnTry = now;
			mp_log( "[NET] Player %d is in this level : spawning their puppet.\n", i );

			SpawnPuppet( p );
		}
	}

	TestAfterHarryTick( self );
	HandleHotkeys( self );
}
MAKE_THISCALL_WRAPPER( EIHarry_Tick_Hook, _EIHarry_Tick_Hook );

// The puppet wears its player's outfit (ours, when it replays us).
GET_METHOD( 0x60F770, void, EICharacter_SetModel, EICharacter*, uint32_t );
static void ApplyPuppetOutfit( EIEvilHarryP* p, Peer* peer )
{
	int me = g_transport ? g_transport->PlayerId() : -1;
	bool ours = peer->id == me || !g_networked;   // It replays us (echo, the loopback).
	int testOutfit = TestPuppetOutfit();
	if ( !ours && !g_players[peer->id].present && testOutfit < 0 ) return;   // They left : as it is.

	int outfit = ours ? g_myWornOutfit : g_players[peer->id].outfit;
	if ( testOutfit >= 0 )
		outfit = testOutfit;
	if ( outfit == peer->shownOutfit ) return;

	uint32_t model = OUTFITS[outfit].model;
	p->m_modelCRC = model;
	p->m_characterCRC = model;
	EICharacter_SetModel( p, model );

	mp_log( "[MP] Player %d's puppet wears the %s outfit.\n", peer->id, OUTFITS[outfit].name );
	peer->shownOutfit = outfit;
}

// Evil Harry's state callbacks are a table of (function, state name) built when the
// game starts (0x84A4B0). In his boss fight, falling in the water puts him in
// "thrown from water", whose enter callback (0x47D490) shuts him in his ice cube
// (0x54B480 : added to the level as an instance of its own, where he is) and counts
// down something of the level's ([0x917034]+0xB38). A puppet runs Evil
// Harry's state machine where its player is : when they swam, the game put the
// puppet in that state, and the others saw them as a block of ice. Puppets skip
// that callback; the game's own Evil Harry still freezes.
typedef void (__thiscall *StateCallback)( EIEvilHarryP* self );
static StateCallback g_origThrownFromWater = 0;

static void __stdcall _ThrownFromWater_Hook( EIEvilHarryP* self )
{
	if ( FindPeerFromBeast(self) && !g_allowPuppetFreeze ) {
		if ( InterlockedIncrement(&g_puppetFreezesStopped) <= 3 ) mp_log( "[MP] A puppet was about to be frozen (Evil Harry's \"thrown from water\") : skipped.\n" );
		return;
	}
	g_origThrownFromWater( self );
}
MAKE_THISCALL_WRAPPER( ThrownFromWater_Hook, _ThrownFromWater_Hook );

// Each state of the state machine keeps its own copy of its callbacks (enter,
// update, leave), and every Evil Harry shares the one state machine : the hooks go
// there, checked on each Evil Harry tick (a level load may build it again).
static void HookThrownFromWater( EIEvilHarryP* b )
{
	static bool warned = false;
	EBeastStateMachine* sm = b->GetStateMachine();
	if ( !sm || !sm->m_states ) return;
	int hooked = 0;
	for ( int i = 0; i < (int) sm->m_numStates; i++ ) {
		EBeastState& st = sm->m_states[i];
		MemberFuncCallback* cbs[3] = { &st.m_onEnter, &st.m_onUpdate, &st.m_onLeave };
		for ( int k = 0; k < 3; k++ ) {
			uint32_t f = (uint32_t) cbs[k]->m_func;
			if ( f == (uint32_t) &ThrownFromWater_Hook ) { hooked++; continue; }
			if ( f != 0x47D490 ) continue;
			g_origThrownFromWater = (StateCallback) f;
			cbs[k]->m_func = (void (*)()) &ThrownFromWater_Hook;
			hooked++;
		}
	}
	if ( !hooked && !warned ) warned = true, mp_log( "[MP] Evil Harry's \"thrown from water\" isn't where we expect : puppets may freeze in water.\n" );
}

// A puppet we spawned in this world that no player has any more. Given back to its
// player if they have none (false : it ticks on as theirs); otherwise sent far below
// the level and not ticked, out of sight (true).
static bool HandleOrphan( EIEvilHarryP* self )
{
	EIHarry* harry = Gizmod::getHarry();
	if ( !harry ) return false;

	for ( int i = 0; i < g_spawnedCount; i++ ) {
		SpawnedPuppet& s = g_spawned[i];
		if ( s.beast != self || s.world != harry->m_world ) continue;

		Peer& owner = g_peers[s.owner];
		if ( !owner.beast ) {
			owner.beast = self;
			owner.lastLoggedState = owner.lastRequested = -2;
			owner.shownOutfit = -1;
			mp_log( "[MP] Player %d's old puppet 0x%.8X ticked with nobody driving it : given back to them.\n", s.owner, self );
			return false;
		}
		if ( !s.reported ) {
			s.reported = true;
			mp_log( "[MP] Player %d's old puppet 0x%.8X ticked with nobody driving it, and they have another : hidden.\n", s.owner, self );
		}
		Vector3f away = { self->EIBeast::m_position.x, self->EIBeast::m_position.y, -5000.0F };
		EICharacter_SetPos( self, &away, true );
		return true;
	}
	return false;
}

static void __stdcall _EIEvilHarryP_Tick_Hook( EIEvilHarryP* self )
{
	if ( TestStallsPuppet(self) ) return;

	HookThrownFromWater( self );
	Peer* peer = FindPeerFromBeast( self );
	if ( !peer ) peer = ReclaimPuppet( self );
	if ( !peer && HandleOrphan( self ) ) return;
	if ( !peer ) peer = FindPeerFromBeast( self );   // Given back to its player.
	if ( !peer ) {   // The game's own Evil Harry.
		EIBeast_Tick( self );
		return;
	}
	peer->lastTick = GetTickCount();
	peer->ticks++;

	if ( TestSkipsPuppetAI() ) {
		DrivePuppet( self, peer );
		return;
	}

	// When Harry dies and comes back the game removes the level's loose models, the
	// puppet's pickaxes among them, and Evil Harry's tick crashes without them
	// (0x47B04F). Init() makes new ones (it only makes those that are missing).
	if ( !self->m_pickaxeModel0 || !self->m_pickaxeModel1 ) {
		mp_log( "[MP] Player %d's puppet lost its pickaxe models (Harry died ?) : making new ones.\n", peer->id );
		self->Init();
		peer->lastRequested = -2;
		peer->shownOutfit = -1;   // Init gave it our Harry's outfit.
		if ( !self->m_pickaxeModel0 || !self->m_pickaxeModel1 ) return;   // Still none : its tick would crash.
	}
	ApplyPuppetOutfit( self, peer );

	UpdateLocomotion( self, peer );
	if ( ForcedClip(peer) && g_haveCamera ) {   // Animation survey, viewer : face the camera.
		const Matrix4f& cam = g_cameraWorld;
		peer->heading = atan2f( cam.data[3][1] - self->EIBeast::m_position.y, cam.data[3][0] - self->EIBeast::m_position.x );
	}

	if ( peer->forcedState >= 0 && self->m_stateIndexCurrent != peer->forcedState ) {
		if ( peer->lastRequested != peer->forcedState ) {
			mp_log( "[MP] Requesting state %d (current %d)...\n", peer->forcedState, self->m_stateIndexCurrent );
			peer->lastRequested = peer->forcedState;
		}
		self->m_forceNextState = true;
		self->m_nextState = peer->forcedState;
	}

	PrePositionPuppet( self, peer );

	EIBeast_Tick( self );

	if ( g_autoLocomotion ) ApplyLocomotionAnim( self, peer );

	if ( self->m_stateIndexCurrent != peer->lastLoggedState ) {
		mp_log( "[MP] Player %d's puppet is now in state %d.\n", peer->id, self->m_stateIndexCurrent );
		peer->lastLoggedState = self->m_stateIndexCurrent;
	}

	DrivePuppet( self, peer );           // Then override where it is.
}
MAKE_THISCALL_WRAPPER( EIEvilHarryP_Tick_Hook, _EIEvilHarryP_Tick_Hook );



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
	// config\Multiplayer\Multiplayer.ini in the game folder (this plugin is in its
	// mods folder), like the other mods' settings. It used to be mods\Multiplayer.ini :
	// that one is still read if it's the only one there.
	char ini[MAX_PATH] = { 0, }, old[MAX_PATH] = { 0, }, dir[MAX_PATH] = { 0, };
	HMODULE self = 0;
	GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR) &ReadNetConfig, &self );
	GetModuleFileNameA( self, dir, MAX_PATH );
	char* slash = strrchr( dir, '\\' );
	if ( slash ) *slash = 0;                                    // ...\Game\mods
	sprintf_s( old, "%s\\Multiplayer.ini", dir );
	slash = strrchr( dir, '\\' );
	if ( slash ) *slash = 0;                                    // ...\Game
	strcat_s( dir, "\\config" );
	CreateDirectoryA( dir, 0 );
	strcat_s( dir, "\\Multiplayer" );
	CreateDirectoryA( dir, 0 );
	sprintf_s( ini, "%s\\Multiplayer.ini", dir );

	// Settings in the old place (packages before config\Multiplayer) move here.
	bool haveIni = GetFileAttributesA( ini ) != INVALID_FILE_ATTRIBUTES;
	bool haveOld = GetFileAttributesA( old ) != INVALID_FILE_ATTRIBUTES;
	if ( !haveIni && haveOld ) {
		if ( CopyFileA( old, ini, TRUE ) ) {
			DeleteFileA( old );
			mp_log( "[NET] Settings moved from %s to config\\Multiplayer.\n", old );
		}
		else {
			strcpy_s( ini, old );
			mp_log( "[NET] Settings : %s (the old place; they now belong in config\\Multiplayer).\n", ini );
		}
	}
	else if ( !haveIni ) {
		FILE* f = 0;
		if ( fopen_s( &f, ini, "wb" ) == 0 && f ) {
			fwrite( DEFAULT_INI, 1, sizeof(DEFAULT_INI) - 1, f );
			fclose( f );
			mp_log( "[NET] Wrote default settings to %s.\n", ini );
		}
	}
	// Another copy of the game on this computer (PTLE_MP_INSTANCE, outside the
	// autotests) : its own settings, Multiplayer-<instance>.ini, so its name, outfit
	// and colours aren't the first copy's. Made from Multiplayer.ini the first time,
	// with the instance after the name.
	char instance[32] = { 0, }, autotestEnv[8] = { 0, };
	DWORD instanceLength = GetEnvironmentVariableA( "PTLE_MP_INSTANCE", instance, sizeof(instance) );
	bool autotestRun = GetEnvironmentVariableA( "PTLE_MP_AUTOTEST", autotestEnv, sizeof(autotestEnv) ) && autotestEnv[0] == '1';
	if ( instanceLength > 0 && instanceLength < sizeof(instance) && !autotestRun ) {
		char own[MAX_PATH];
		sprintf_s( own, "%s\\Multiplayer-%s.ini", dir, instance );
		if ( GetFileAttributesA( own ) == INVALID_FILE_ATTRIBUTES && CopyFileA( ini, own, TRUE ) ) {
			char name[64] = { 0, }, renamed[96];
			GetPrivateProfileStringA( "Network", "Name", "Harry", name, sizeof(name), own );
			sprintf_s( renamed, "%s %s", name, instance );
			WritePrivateProfileStringA( "Network", "Name", renamed, own );
			mp_log( "[NET] Instance \"%s\" : its own settings, made from %s.\n", instance, ini );
		}
		if ( GetFileAttributesA( own ) != INVALID_FILE_ATTRIBUTES ) strcpy_s( ini, own );
		mp_log( "[NET] Settings : %s.\n", ini );
	}
	strcpy_s( g_iniPath, ini );
	{
		char name[32] = { 0, };
		GetPrivateProfileStringA( "Gamemodes", "Mode", "Off", name, sizeof(name), ini );
		for ( int i = 0; i < NUM_MODES; i++ ) if ( _stricmp( name, MODE_NAMES[i] ) == 0 ) g_gameMode = i;
		g_swapButton = GetPrivateProfileIntA( "Keys", "RoleSwapButton", 4, ini );
		g_swapKey = GetPrivateProfileIntA( "Keys", "RoleSwapKey", 0x2A, ini );
		g_emoteButton = GetPrivateProfileIntA( "Keys", "EmoteButton", 4, ini );
		g_emoteNeedsR1 = GetPrivateProfileIntA( "Keys", "EmoteNeedsR1", 0, ini ) != 0;
		g_emoteKey = GetPrivateProfileIntA( "Keys", "EmoteKey", 0x14, ini );
		LoadEmoteWheels( ini );
		GetPrivateProfileStringA( "Appearance", "Outfit", "Default", name, sizeof(name), ini );
		g_myOutfit = OUTFIT_DEFAULT;
		for ( int i = 0; i < NUM_OUTFITS; i++ ) if ( _stricmp( name, OUTFITS[i].name ) == 0 ) g_myOutfit = i;
	}
	char colors[64] = { 0, };
	GetEnvOverride( "PTLE_MP_FAKEVERSION", g_myVersion, sizeof(g_myVersion) );
	GetEnvOverride( "PTLE_MP_COLORS", colors, sizeof(colors) );   // Tests : "RRGGBB,RRGGBB,RRGGBB,RRGGBB" (hair, head, shirt, pants).
	for ( int i = 0; i < RETRO_PARTS; i++ ) {
		char key[32], value[16];
		sprintf_s( key, "Retro%s", RETRO_PART_NAMES[i] );
		GetPrivateProfileStringA( "Appearance", key, "", value, sizeof(value), ini );
		if ( strlen( colors ) >= (size_t) (i * 7 + 6) ) strncpy_s( value, colors + i * 7, 6 );
		unsigned rgb = 0;
		if ( strlen( value ) == 6 && sscanf_s( value, "%x", &rgb ) == 1 ) {
			g_myColors[i][0] = (uint8_t) (rgb >> 16); g_myColors[i][1] = (uint8_t) (rgb >> 8); g_myColors[i][2] = (uint8_t) rgb;
		}
		else memcpy( g_myColors[i], RETRO_DEFAULT[i], 3 );
	}
	char outfit[8] = { 0, };
	GetEnvOverride( "PTLE_MP_OUTFIT", outfit, sizeof(outfit) );   // Tests.
	if ( outfit[0] ) g_myOutfit = atoi( outfit );   // (-1 : Default.)
	if ( g_myOutfit < 0 || g_myOutfit >= NUM_OUTFITS ) g_myOutfit = OUTFIT_DEFAULT;
	g_showTags = GetPrivateProfileIntA( "Display", "NameTags", 1, ini ) != 0;
	g_tagFadeFrom = GetPrivateProfileIntA( "Display", "TagFadeFrom", 100, ini );
	g_tagFadeTo = GetPrivateProfileIntA( "Display", "TagFadeTo", 200, ini );
	g_tagFadeFrom = max( 0, min( g_tagFadeFrom, TAG_FADE_MAX - TAG_FADE_STEP ) );
	g_tagFadeTo = max( g_tagFadeFrom + TAG_FADE_STEP, min( g_tagFadeTo, TAG_FADE_MAX ) );
	g_showList = GetPrivateProfileIntA( "Display", "PlayerList", 0, ini ) != 0;
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
// TODO: Gizmod 0.4 should perform corrupt / compat checks like this.
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

// Our outfit : the game's cheat flag for it (and none of the others). Harry changes
// when the next level loads.
void ApplyMyOutfit()
{
	for ( int i = 1; i < NUM_OUTFITS; i++ )
		if ( OUTFITS[i].cheatFlag )
			*(uint8_t*) OUTFITS[i].cheatFlag = (i == g_myOutfit) ? 1 : 0;

	g_forceHarryModel = g_myOutfit < 0 || OUTFITS[g_myOutfit].cheatFlag ? 0 : OUTFITS[g_myOutfit].model;   // Harry : the jungle one, even in the snow.
}



// Without a cheat, Harry's model is the level's (0x4ADC1E : mov [ecx+0x130], eax, the
// CRC from the level's data). Ours instead, when one is chosen that has no cheat.
static const uint32_t HARRY_MODEL_STORE = 0x4ADC1E;

static __declspec(naked) void HarryModel_Hook()
{
	__asm {
		cmp dword ptr [g_forceHarryModel], 0
		je keep
		mov eax, dword ptr [g_forceHarryModel]
	keep:
		mov dword ptr [ecx + 0x130], eax
		ret
	}
}

// The menu's settings, kept in the ini (not the autotests' : they leave it alone).
static void SaveSettings()
{
	if ( g_autotest || !g_iniPath[0] ) return;   // (The tests leave the player's settings alone.)

	WritePrivateProfileStringA( "Appearance", "Outfit", g_myOutfit < 0 ? "Default" : OUTFITS[g_myOutfit].name, g_iniPath );
	WritePrivateProfileStringA( "Gamemodes", "Mode", MODE_NAMES[g_gameMode], g_iniPath );
	{
		char n[16];
		sprintf_s( n, "%d", g_swapButton ); WritePrivateProfileStringA( "Keys", "RoleSwapButton", n, g_iniPath );
		sprintf_s( n, "%d", g_swapKey );    WritePrivateProfileStringA( "Keys", "RoleSwapKey", n, g_iniPath );
		sprintf_s( n, "%d", g_emoteButton ); WritePrivateProfileStringA( "Keys", "EmoteButton", n, g_iniPath );
		WritePrivateProfileStringA( "Keys", "EmoteNeedsR1", g_emoteNeedsR1 ? "1" : "0", g_iniPath );
		sprintf_s( n, "%d", g_emoteKey );   WritePrivateProfileStringA( "Keys", "EmoteKey", n, g_iniPath );
	}
	SaveEmoteWheels( g_iniPath );
	for ( int i = 0; i < RETRO_PARTS; i++ ) {
		char key[32], value[16];
		sprintf_s( key, "Retro%s", RETRO_PART_NAMES[i] );
		sprintf_s( value, "%02X%02X%02X", g_myColors[i][0], g_myColors[i][1], g_myColors[i][2] );
		WritePrivateProfileStringA( "Appearance", key, value, g_iniPath );
	}
	// Gone from older versions : no longer read.
	WritePrivateProfileStringA( "Appearance", "RetroSkin", 0, g_iniPath );
	WritePrivateProfileStringA( "Appearance", "RetroTrousers", 0, g_iniPath );
	WritePrivateProfileStringA( "Display", "TagFade", 0, g_iniPath );
	char delay[16];
	sprintf_s( delay, "%d", DELAY_CHOICES[g_delayIndex] );
	WritePrivateProfileStringA( "Display", "NameTags", g_showTags ? "1" : "0", g_iniPath );
	char fade[16];
	sprintf_s( fade, "%d", g_tagFadeFrom );
	WritePrivateProfileStringA( "Display", "TagFadeFrom", fade, g_iniPath );
	sprintf_s( fade, "%d", g_tagFadeTo );
	WritePrivateProfileStringA( "Display", "TagFadeTo", fade, g_iniPath );
	WritePrivateProfileStringA( "Display", "PlayerList", g_showList ? "1" : "0", g_iniPath );
	WritePrivateProfileStringA( "Network", "Delay", delay, g_iniPath );
}

// The player list's rows : the room, us, the others, the ping.
static const int MAX_LIST_ROWS = MAX_PEERS + 3;
// Hide and seek : a hider's name tag and whereabouts are hidden from everyone else
// (other hiders too : they'd know where to look once they're seekers).
bool HiddenFromMe( int id )
{
	int me = g_transport ? g_transport->PlayerId() : -1;
	return id != me && g_players[id].role == ROLE_HIDER;
}

static int BuildPlayerRows( char left[][96], char right[][64], uint32_t* colors, const char* lastRight )
{
	int rows = 0;
	int me = g_transport ? g_transport->PlayerId() : -1;
	uint32_t here = ShownLevel();
	int count = me >= 0 ? 1 : 0;
	for ( int i = 0; i < MAX_PEERS; i++ ) if ( g_players[i].present && i != me ) count++;

	if ( !g_networked ) {
		sprintf_s( left[rows], 96, "Not online" );
		sprintf_s( right[rows], 64, "(no Server in Multiplayer.ini)" );
	}
	else if ( me < 0 ) {
		sprintf_s( left[rows], 96, "Room \"%s\"", g_roomName );
		sprintf_s( right[rows], 64, "connecting..." );
	}
	else {
		sprintf_s( left[rows], 96, "Room \"%s\"", g_roomName );
		sprintf_s( right[rows], 64, "%d player%s", count, count == 1 ? "" : "s" );
	}
	colors[rows++] = 0xFFFFE080;
	if ( me >= 0 ) {
		sprintf_s( left[rows], 96, "%s (you)", g_myName );
		sprintf_s( right[rows], 64, "%s", LevelName( g_haveLocal ? here : 0 ) );
		colors[rows++] = 0xFFA0FFA0;
	}
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		const PlayerInfo& info = g_players[i];
		if ( !info.present || i == me ) continue;
		bool sameVersion = strcmp( info.version, MP_VERSION ) == 0;
		if ( sameVersion ) sprintf_s( left[rows], 96, "%s", info.name );
		else sprintf_s( left[rows], 96, "%s (version %s)", info.name, info.version[0] ? info.version : "older" );
		if ( HiddenFromMe( i ) ) sprintf_s( right[rows], 64, "hiding" );
		else if ( info.level && info.level == here ) sprintf_s( right[rows], 64, "here" );
		else sprintf_s( right[rows], 64, "%s", LevelName( info.level ) );
		colors[rows++] = sameVersion ? 0xFFFFFFFF : 0xFFFFA040;
	}
	int ping = g_transport ? g_transport->RoundTripMs() : -1;
	if ( ping >= 0 ) sprintf_s( left[rows], 96, "Ping %d ms", ping );
	else left[rows][0] = 0;
	sprintf_s( right[rows], 64, "%s", lastRight );
	colors[rows++] = 0xFFB0B0B0;
	return rows;
}

// The role swap's bindings : the Keys page waits for the next key or button.
enum { BIND_NONE, BIND_BUTTON, BIND_KEY };
static int g_binding = BIND_NONE;
static bool g_bindArmed = false;   // Everything was let go since (the Enter that started it doesn't count).

static int* g_bindTarget = 0;      // The setting it goes in (g_swapButton, g_emoteKey, ...).

static void StartBinding( int what, int* target ) { g_binding = what; g_bindTarget = target; g_bindArmed = false; }

static void SetGameMode( int mode )
{
	if ( mode == g_gameMode ) return;
	g_gameMode = mode;
	if ( mode != MODE_HIDE_AND_SEEK ) HsExit();
	mp_log( "[HS] Game mode : %s.\n", MODE_NAMES[mode] );
	SaveSettings();
}

// A Windows key's DirectInput code (its scan code, + 0x80 for the extended ones).
int VkToDik( int vk )
{
	switch ( vk ) {
	case VK_UP: return 0xC8; case VK_DOWN: return 0xD0; case VK_LEFT: return 0xCB; case VK_RIGHT: return 0xCD;
	case VK_RCONTROL: return 0x9D; case VK_RMENU: return 0xB8; case VK_INSERT: return 0xD2; case VK_DELETE: return 0xD3;
	case VK_HOME: return 0xC7; case VK_END: return 0xCF; case VK_PRIOR: return 0xC9; case VK_NEXT: return 0xD1;
	case VK_DIVIDE: return 0xB5; case VK_RSHIFT: return 0x36;
	}
	return (int) MapVirtualKeyA( vk, MAPVK_VK_TO_VSC ) & 0x7F;
}

const char* KeyName( int dik )
{
	static char name[4][40];
	static int next = 0;
	char* out = name[next++ & 3];
	LONG lp = ((dik & 0x7F) << 16) | ((dik & 0x80) ? (1 << 24) : 0);
	if ( dik == 0x2A ) strcpy_s( out, 40, "Left Shift" );
	else if ( dik == 0x36 ) strcpy_s( out, 40, "Right Shift" );
	else if ( dik == 0x1D ) strcpy_s( out, 40, "Left Ctrl" );
	else if ( dik == 0x9D ) strcpy_s( out, 40, "Right Ctrl" );
	else if ( dik == 0x38 ) strcpy_s( out, 40, "Left Alt" );
	else if ( dik == 0xB8 ) strcpy_s( out, 40, "Right Alt" );
	else if ( !GetKeyNameTextA( lp, out, 40 ) ) sprintf_s( out, 40, "key %d", dik );
	return out;
}

static const char* PadButtonName( int button )
{
	static char name[4][40];
	static int next = 0;
	char* out = name[next++ & 3];
	if ( button == 4 ) strcpy_s( out, 40, "L1 / LB (button 5)" );
	else if ( button == 5 ) strcpy_s( out, 40, "R1 / RB (button 6)" );
	else sprintf_s( out, 40, "button %d", button + 1 );
	return out;
}

static void PollBinding()
{
	if ( g_binding == BIND_NONE ) return;
	bool escape = (GetAsyncKeyState( VK_ESCAPE ) & 0x8000) != 0;
	if ( g_binding == BIND_KEY ) {
		int down = -1;
		for ( int vk = 0x08; vk < 0xFF; vk++ ) {
			if ( vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || (vk >= VK_LBUTTON && vk <= VK_XBUTTON2 && vk != VK_CANCEL) ) continue;   // (Their left / right ones count.)
			if ( GetAsyncKeyState( vk ) & 0x8000 ) { down = vk; break; }
		}
		if ( !g_bindArmed ) { g_bindArmed = down < 0; return; }
		if ( down < 0 ) return;
		if ( down == VK_ESCAPE ) { g_binding = BIND_NONE; return; }
		if ( down == VK_LEFT || down == VK_RIGHT || down == VK_UP || down == VK_DOWN ) return;   // They're the directions.
		*g_bindTarget = VkToDik( down );
		mp_log( "[MP] %s key : %s (%d).\n", g_bindTarget == &g_swapKey ? "Role swap" : "Emote wheel", KeyName( *g_bindTarget ), *g_bindTarget );
	}
	else {
		int down = -1;
		for ( int b = 0; g_padSeenAny && b < 32; b++ ) {
			if ( g_gamepadSeen.rgbButtons[b] & 0x80 ) {
				down = b;
				break;
			}
		}
		if ( !g_bindArmed ) {
			g_bindArmed = down < 0 && !escape;
			return;
		}
		if ( escape ) {
			g_binding = BIND_NONE;
			return;
		}
		if ( down < 0 ) return;
		*g_bindTarget = down;
		mp_log( "[MP] %s button : %d.\n", g_bindTarget == &g_swapButton ? "Role swap" : "Emote wheel", down + 1 );
	}
	g_binding = BIND_NONE;
	SaveSettings();
}



// --------------------------------------------------------------------------------
// The menu (F1).
// --------------------------------------------------------------------------------

Menu g_menu;

static const char MENU_OUTFIT_DESC[] =
	"What you look like, to you and to the others. Changes the next time you go to another area. "
	"Default : as in the game, jungle or snowy clothes as the area has it. Harry : always the jungle clothes.";

static const char MENU_PLAYBACKDELAY_DESC[] =
	"How far behind the others are shown. Shorter : closer to live, but jerkier on a bad connection.";

static const char MENU_NAMETAGSFADE_DESC[] =
	"Distance from the camera where the others' names start to fade. The players themselves stay in view.";

static const char MENU_NAMETAGSGONE_DESC[] =
	"Distance from the camera where the others' names have faded out completely.";

static bool IsMenuOpen()
{
	return g_menu.IsOpen();
}

static void EmotesPage()
{
	if ( g_editWheel >= g_wheelCount ) g_editWheel = g_wheelCount - 1;
	if ( g_editWheel < 0 ) g_editWheel = 0;
	EmoteWheel& wheel = g_emoteWheels[g_editWheel];
	if ( g_armed && GetTickCount() - g_armedAt > 4000 ) g_armed = 0;
	if ( !g_chooser ) {
		g_previewWant = -2;
		const char* names[MAX_WHEELS];
		for ( int w = 0; w < g_wheelCount; w++ ) names[w] = g_emoteWheels[w].name[0] ? g_emoteWheels[w].name : "(no name)";
		if ( g_menu.Choice( "Wheel", &g_editWheel, names, g_wheelCount,
			"The wheel shown below. In the game, R1 (Tab on the keyboard) goes from one wheel to the next." ) ) g_armed = 0;
		if ( g_menu.TextField( "Name", wheel.name, WHEEL_NAME_SIZE, "(no name)", "Its name, in the middle of the wheel. Enter, type it, Enter." ) ) SaveSettings();
		const char* slots[WHEEL_SLOTS];
		for ( int i = 0; i < WHEEL_SLOTS; i++ ) slots[i] = wheel.slot[i] >= 0 ? EMOTES[wheel.slot[i]].name : 0;
		int from = 0;
		switch ( g_menu.Ring( slots, &g_ringSlot, wheel.name, &from,
			"Enter or a click : choose this slot's emote.  Left / Right : go round.  Delete : empty it.  Shift + Left / Right : move it round." ) ) {
		case Menu::RING_CHOOSE:
			OpenChooser();
			break;
		case Menu::RING_CLEAR:
			wheel.slot[g_ringSlot] = -1;
			SaveSettings();
			break;
		case Menu::RING_SWAP: {
			int moved = wheel.slot[from];
			wheel.slot[from] = wheel.slot[g_ringSlot];
			wheel.slot[g_ringSlot] = moved;
			SaveSettings();
			break;
		}
		}
		if ( g_wheelCount < MAX_WHEELS && g_menu.Action( "+ New wheel", "Another wheel, empty, to fill (up to 4)." ) ) {
			EmoteWheel& added = g_emoteWheels[g_wheelCount];
			sprintf_s( added.name, "Wheel %d", g_wheelCount + 1 );
			for ( int i = 0; i < WHEEL_SLOTS; i++ ) added.slot[i] = -1;
			g_editWheel = g_wheelCount++;
			g_armed = 0;
			SaveSettings();
		}
		if ( g_menu.Action( g_armed == 1 ? "Reset this wheel : press again" : "Reset this wheel",
			"Puts it back as it was at first (Party and Silly; the others empty). Press twice." ) ) {
			if ( g_armed == 1 ) {
				char name[WHEEL_NAME_SIZE];
				strcpy_s( name, wheel.name );
				MakeDefaultWheel( g_editWheel );
				if ( g_editWheel >= 2 ) strcpy_s( wheel.name, name );
				g_armed = 0;
				SaveSettings();
				Toast( "Wheel reset" );
			}
			else { g_armed = 1; g_armedAt = GetTickCount(); }
		}
		if ( g_wheelCount > 1 && g_menu.Action( g_armed == 2 ? "Delete this wheel : press again" : "Delete this wheel", "Gone for good. Press twice." ) ) {
			if ( g_armed == 2 ) {
				for ( int w = g_editWheel; w < g_wheelCount - 1; w++ ) g_emoteWheels[w] = g_emoteWheels[w + 1];
				g_wheelCount--;
				if ( g_editWheel >= g_wheelCount ) g_editWheel = g_wheelCount - 1;
				if ( g_wheelIndex >= g_wheelCount ) g_wheelIndex = 0;
				g_armed = 0;
				SaveSettings();
				Toast( "Wheel deleted" );
			}
			else { g_armed = 2; g_armedAt = GetTickCount(); }
		}
		return;
	}

	// Choosing : the menu at the left, the preview at the right.
	g_menu.SetKeys( "Enter : choose    Esc : back    Mouse : click, wheel" );
	char title[64];
	sprintf_s( title, "Choose an emote for %s, slot %d", wheel.name, g_ringSlot + 1 );
	g_menu.Info( title, 0, Menu::GOLD );
	if ( g_menu.TextField( "Search", g_emoteSearch, sizeof(g_emoteSearch), "(Enter to type)",
		"Only the emotes with this in their name. Enter, type, Enter." ) ) g_listCursor = g_listScroll = 0;
	static const char* categories[2 + NUM_EMOTE_CATEGORIES];
	categories[0] = "Favourites";
	categories[1] = "All";
	for ( int i = 0; i < NUM_EMOTE_CATEGORIES; i++ ) categories[2 + i] = EMOTE_CATEGORIES[i];
	if ( g_menu.Choice( "Category", &g_emoteCategory, categories, 2 + NUM_EMOTE_CATEGORIES,
		"Favourites : the ones picked out for dancing and fooling about. All : every one of Harry's animations." ) ) g_listCursor = g_listScroll = 0;
	static int items[NUM_EMOTES + 1];
	static const char* names[NUM_EMOTES + 1];
	static const char* tags[NUM_EMOTES + 1];
	int n = 0;
	items[n] = -1; names[n] = "Empty slot"; tags[n] = ""; n++;
	for ( int e = 0; e < NUM_EMOTES; e++ ) {
		if ( !EmoteMatches( e ) ) continue;
		items[n] = e;
		names[n] = EMOTES[e].name;
		tags[n] = EMOTES[e].intro ? "chain" : EMOTES[e].loops ? "loop" : "once";
		n++;
	}
	if ( g_listWant != -2 ) {
		for ( int i = 0; i < n; i++ ) if ( items[i] == g_listWant ) g_listCursor = i;
		g_listWant = -2;
	}
	if ( g_menu.List( names, tags, n, &g_listCursor, &g_listScroll, 8,
		"once : plays, then it's over. loop : plays until you move. chain : a start, then a loop until you move." ) ) {
		int e = items[g_listCursor];
		wheel.slot[g_ringSlot] = e;
		mp_log( "[EM] %s, slot %d : %s.\n", wheel.name, g_ringSlot + 1, e >= 0 ? EMOTES[e].name : "empty" );
		SaveSettings();
		g_chooser = false;
	}
	if ( g_menu.Back() ) g_chooser = false;
	g_previewWant = g_chooser && g_listCursor >= 0 && g_listCursor < n ? items[g_listCursor] : -2;
}

// The menu's pages. Each setting is one row here.
static void DrawMenu( float sw, float sh, float h )
{
	MenuInput in;
	ZeroMemory( &in, sizeof(in) );
	in.up = InterlockedExchange( &g_menuKeys[MK_UP], 0 );
	in.down = InterlockedExchange( &g_menuKeys[MK_DOWN], 0 );
	in.left = InterlockedExchange( &g_menuKeys[MK_LEFT], 0 );
	in.right = InterlockedExchange( &g_menuKeys[MK_RIGHT], 0 );
	in.accept = InterlockedExchange( &g_menuKeys[MK_ACCEPT], 0 );
	in.back = InterlockedExchange( &g_menuKeys[MK_BACK], 0 );
	in.nextPage = InterlockedExchange( &g_menuKeys[MK_NEXT], 0 );
	in.prevPage = InterlockedExchange( &g_menuKeys[MK_PREV], 0 );
	in.del = InterlockedExchange( &g_menuKeys[MK_DELETE], 0 );
	in.pageUp = InterlockedExchange( &g_menuKeys[MK_PGUP], 0 );
	in.pageDown = InterlockedExchange( &g_menuKeys[MK_PGDN], 0 );
	in.shiftLeft = InterlockedExchange( &g_menuKeys[MK_SLEFT], 0 );
	in.shiftRight = InterlockedExchange( &g_menuKeys[MK_SRIGHT], 0 );
	static char typed[64], pasted[128];
	TakeTyped( typed, sizeof(typed) );
	in.typed = typed[0] ? typed : 0;
	in.copy = InterlockedExchange( &g_menuCopy, 0 ) != 0;
	if ( InterlockedExchange( &g_menuPaste, 0 ) && ReadClipboard( pasted, sizeof(pasted) ) ) in.pasted = pasted;

	// The mouse, in the overlay's pixels (the window may be scaled).
	bool cursorShown = true;
	if ( !TestMenuInput(&in, pasted, sizeof(pasted)) ) {
		HWND wnd = *HWND_GAME_WINDOW;
		POINT p;
		RECT rc;
		if ( wnd && GameHasFocus() && GetCursorPos(&p) && ScreenToClient(wnd, &p) && GetClientRect(wnd, &rc) && rc.right > 0 && rc.bottom > 0 ) {
			in.mouseX = p.x * sw / rc.right;
			in.mouseY = p.y * sh / rc.bottom;
			in.mouseInside = p.x >= 0 && p.y >= 0 && p.x < rc.right && p.y < rc.bottom;
			in.mouseDown = in.mouseInside && (GetAsyncKeyState( VK_LBUTTON ) & 0x8000) != 0;
		}
		CURSORINFO ci;
		ci.cbSize = sizeof(ci);
		cursorShown = GetCursorInfo( &ci ) && (ci.flags & CURSOR_SHOWING);
	}
	static float lastX = -1.0F, lastY = -1.0F;
	static bool lastDown = false;
	in.mouseMoved = in.mouseInside && (in.mouseX != lastX || in.mouseY != lastY);
	in.mouseClicked = in.mouseDown && !lastDown;
	lastX = in.mouseX; lastY = in.mouseY; lastDown = in.mouseDown;

	// The wheel : whole notches (smaller steps add up).
	static LONG wheelRest = 0;
	wheelRest += InterlockedExchange( &g_menuWheel, 0 );
	in.wheel = wheelRest / WHEEL_DELTA;
	wheelRest -= in.wheel * WHEEL_DELTA;

	static char left[MAX_LIST_ROWS][96], right[MAX_LIST_ROWS][64];
	static uint32_t colors[MAX_LIST_ROWS];
	int rows = BuildPlayerRows( left, right, colors, "" );

	if ( !g_menu.IsOpen() ) g_binding = BIND_NONE;
	if ( g_binding != BIND_NONE ) {   // Waiting for a key or a button : the menu doesn't get them.
		PollBinding();
		in.up = in.down = in.left = in.right = in.accept = in.back = in.nextPage = in.prevPage = 0;
		in.del = in.pageUp = in.pageDown = in.shiftLeft = in.shiftRight = in.wheel = 0;
		in.mouseClicked = false;
	}
	g_menu.SetNarrow( g_chooser );
	g_menu.Begin( in, sw, sh, h, "Multiplayer " MP_VERSION, right[0][0] ? right[0] : 0 );
	if ( g_menu.Page( "Players" ) ) {
		for ( int r = 0; r < rows; r++ ) g_menu.Info( left[r], r == 0 ? "" : right[r], colors[r] );
	}
	// Game modes : one page each, under a heading. One on at a time (g_gameMode); each
	// page's Enabled turns its own on, or all off.
	g_menu.Group( "Gamemodes" );
	if ( g_menu.Page("Hide and seek") ) {
		g_menu.Section( "Settings" );
		bool on = g_gameMode == MODE_HIDE_AND_SEEK;
		if ( g_menu.Toggle( "Enabled", &on,
			"On : you're playing. You get a role, your hide time adds up while you're a hider, and seekers can catch you. "
			"Everyone who plays turns it on. Off : just see each other." ) ) SetGameMode( on ? MODE_HIDE_AND_SEEK : MODE_OFF );
		char hidden[16];
		if ( on ) {
			int role = g_hsMyRole;
			static const char* const ROLES[] = { "Seeker", "Hider" };
			char swapHint[200];
			sprintf_s( swapHint, "Also without the menu : hold %s (controller) or %s (keyboard) and press left or right. "
				"Caught : you're a seeker. Only seekers have name tags.", PadButtonName( g_swapButton ), KeyName( g_swapKey ) );
			if ( g_menu.Choice("Your role", &role, ROLES, 2, swapHint) ) HsSetRole( role );
			FormatHideTime( g_hsMyHideMs, hidden, sizeof(hidden) );
			g_menu.Info( "Your hide time", hidden );
			static uint32_t resetAsked = 0;
			bool asking = resetAsked && GetTickCount() - resetAsked < 4000;
			if ( g_menu.Action(asking ? "Reset my timer : press again to be sure" : "Reset my timer", "Your hide time back to 0:00. Only you can reset it : leaving, restarting and being caught keep it.") ) {
				if ( asking ) {
					mp_log( "[HS] Hide time reset (was %u ms).\n", g_hsMyHideMs );
					HsResetHideTime();
					resetAsked = 0;
				}
				else resetAsked = GetTickCount();
			}
		}

		// Who's playing : a table of the players who have it on.
		static const float COLUMNS[] = { 0.0F, 0.47F, 0.64F, 1.0F };
		static const char* const TITLES[] = { "Name", "Role", "Time", "Where" };
		g_menu.Columns( COLUMNS, 4 );
		g_menu.Section( "Who's playing", TITLES, 4 );
		int me = g_transport ? g_transport->PlayerId() : -1;
		uint32_t here = ShownLevel();
		int listed = 0;
		for ( int i = 0; i < MAX_PEERS; i++ ) {
			bool mine = i == me;
			const PlayerInfo& info = g_players[i];
			if ( mine ? !on : (!info.present || info.role == ROLE_NONE) ) continue;

			int r = mine ? g_hsMyRole : info.role;
			char name[48], where[64];
			FormatHideTime( mine ? g_hsMyHideMs : info.hideMs, hidden, sizeof(hidden) );
			sprintf_s( name, "%s%s", mine ? g_myName : info.name, mine ? " (you)" : "" );
			uint32_t level = mine ? (g_haveLocal ? here : 0) : info.level;
			if ( HiddenFromMe(i) ) strcpy_s( where, "hiding" );
			else if ( !mine && level && level == here ) strcpy_s( where, "here" );
			else strcpy_s( where, LevelName(level) );
			const char* cells[4] = { name, r == ROLE_HIDER ? "Hider" : "Seeker", hidden, where };
			g_menu.TableRow( cells, 4, mine ? Menu::GREEN : (r == ROLE_HIDER ? Menu::GOLD : Menu::WHITE) );
			listed++;
		}
		if ( !listed ) {
			const char* cells[1] = { me < 0 ? "Not in a room" : "Nobody here is playing" };
			g_menu.TableRow( cells, 1, Menu::DIM );
		}
	}
	g_menu.EndGroup();
	if ( g_menu.Page("Appearance") ) {
		static const int ORDER[NUM_OUTFITS + 1] = { OUTFIT_DEFAULT, 0, SNOWY_OUTFIT, 1, 2, RETRO_OUTFIT };   // Default, Harry, Snowy Harry, Nicole, Native, Retro Harry.
		static const char* names[NUM_OUTFITS + 1];
		int choice = 0;
		for ( int i = 0; i <= NUM_OUTFITS; i++ ) {
			names[i] = ORDER[i] < 0 ? "Default" : OUTFITS[ORDER[i]].name;
			if ( ORDER[i] == g_myOutfit ) choice = i;
		}
		if ( g_menu.Choice("Outfit", &choice, names, NUM_OUTFITS + 1, MENU_OUTFIT_DESC) ) {
			g_myOutfit = ORDER[choice];
			ApplyMyOutfit();
			SaveSettings();
		}
		if ( g_myOutfit == OUTFIT_DEFAULT && g_myWornOutfit == SNOWY_OUTFIT ) g_menu.Info( "", "his snowy clothes, in this area", Menu::DIM );
		if ( g_myOutfit == RETRO_OUTFIT ) {
			// A row of ready colours per part (the first is the game's own), and sliders for any other.
			static const uint8_t READY[][3] = { { 220, 50, 40 }, { 240, 140, 30 }, { 240, 215, 60 }, { 60, 190, 220 }, { 50, 100, 220 },
				{ 150, 80, 200 }, { 240, 120, 180 }, { 245, 245, 245 }, { 130, 130, 130 }, { 30, 30, 30 } };
			static const int NUM_READY = sizeof(READY) / sizeof(READY[0]);
			static int editing = RETRO_SHIRT;
			bool changed = false;
			g_menu.Section( "Retro Harry's colors" );
			for ( int part = 0; part < RETRO_PARTS; part++ ) {
				uint32_t swatches[NUM_READY + 1];
				int index = -1;
				for ( int i = 0; i <= NUM_READY; i++ ) {
					const uint8_t* c = i == 0 ? RETRO_DEFAULT[part] : READY[i - 1];
					swatches[i] = (c[0] << 16) | (c[1] << 8) | c[2];
					if ( memcmp( c, g_myColors[part], 3 ) == 0 ) index = i;
				}
				if ( g_menu.Swatches( RETRO_PART_NAMES[part], &index, swatches, NUM_READY + 1, "Left / Right or click : pick a color. Shows straight away." ) && index >= 0 ) {
					memcpy( g_myColors[part], index == 0 ? RETRO_DEFAULT[part] : READY[index - 1], 3 );
					editing = part;
					changed = true;
				}
			}
			g_menu.Choice( "Mix a color for", &editing, RETRO_PART_NAMES, RETRO_PARTS, "Which part the color and the three sliders below change." );
			changed |= g_menu.Colour("Color", g_myColors[editing], "Enter : type a hex code (#DC3228) or red, green, blue (220, 50, 40). Ctrl + V pastes one, Ctrl + C copies this one." );

			static const char* const CHANNELS[3] = { "Red", "Green", "Blue" };
			for ( int ch = 0; ch < 3; ch++ ) {
				int v = g_myColors[editing][ch];
				if ( g_menu.Slider(CHANNELS[ch], &v, 0, 255, 5, "Left / Right, or click and drag.") ) {
					g_myColors[editing][ch] = (uint8_t) v;
					changed = true;
				}
			}
			if ( g_menu.Action("Reset colors to default", "Retro Harry's own colors, for all four parts.") ) {
				memcpy( g_myColors, RETRO_DEFAULT, sizeof(g_myColors) );
				changed = true;
			}
			if ( changed ) {
				g_lastInfoSent = 0;   // The others hear about it now.
				SaveSettings();
			}
		}
	}
	if ( g_menu.Page("Display") ) {
		static const char* const DELAYS[] = { "133 ms", "250 ms", "500 ms", "1 s", "1.5 s", "3 s" };
		bool changed = false;
		changed |= g_menu.Toggle( "Name tags", &g_showTags, "Names above the other players." );
		changed |= g_menu.Toggle( "Player list while playing", &g_showList, "Keeps the player list in the corner of the screen." );
		changed |= g_menu.Choice( "Playback delay", &g_delayIndex, DELAYS, countof(DELAYS), MENU_PLAYBACKDELAY_DESC );
		if ( g_menu.Slider("Name tags start fading at", &g_tagFadeFrom, 0, TAG_FADE_MAX - TAG_FADE_STEP, TAG_FADE_STEP, MENU_NAMETAGSFADE_DESC) ) {
			if ( g_tagFadeTo < g_tagFadeFrom + TAG_FADE_STEP )
				g_tagFadeTo = g_tagFadeFrom + TAG_FADE_STEP;
			changed = true;
		}
		if ( g_menu.Slider("Name tags gone at", &g_tagFadeTo, TAG_FADE_STEP, TAG_FADE_MAX, TAG_FADE_STEP, MENU_NAMETAGSGONE_DESC) ) {
			if ( g_tagFadeFrom > g_tagFadeTo - TAG_FADE_STEP )
				g_tagFadeFrom = g_tagFadeTo - TAG_FADE_STEP;
			changed = true;
		}
		if ( changed ) SaveSettings();
	}
	bool emotes = g_menu.Page( "Emotes" );
	if ( emotes )
		EmotesPage();
	else { g_chooser = false; g_previewWant = -2; }
	if ( g_menu.Page("Keys") ) {
		g_menu.Info( "F1", "This menu" );
		{
			// What each binding row shows : the button or key, or that it's waiting for one.
			struct Bound {
				static const char* Pad( int* target ) { return g_binding == BIND_BUTTON && g_bindTarget == target ? "press a button... (Esc : leave it)" : PadButtonName( *target ); }
				static const char* Key( int* target ) { return g_binding == BIND_KEY && g_bindTarget == target ? "press a key... (Esc : leave it)" : KeyName( *target ); }
			};
			if ( g_menu.Button( "Swap roles, controller : hold", Bound::Pad( &g_swapButton ),
				"Hide and seek : hold this and press left or right on the d-pad to switch between hider and seeker. "
				"Enter, then press the button to use." ) ) StartBinding( BIND_BUTTON, &g_swapButton );
			if ( g_menu.Button( "Swap roles, keyboard : hold", Bound::Key( &g_swapKey ),
				"Hide and seek : hold this and press the left or right arrow to switch between hider and seeker. "
				"Enter, then press the key to use." ) ) StartBinding( BIND_KEY, &g_swapKey );
			if ( g_menu.Button( "Emote wheel, controller : hold", Bound::Pad( &g_emoteButton ),
				"Hold it for the emote wheel, point the right stick at an emote and let go to play it. R1 : the next wheel. "
				"Enter, then press the button to use." ) ) StartBinding( BIND_BUTTON, &g_emoteButton );
			if ( g_menu.Toggle( "Emote wheel : needs R1 too", &g_emoteNeedsR1,
				"On : the wheel opens only with R1 held as well (L1 + R1), so the button alone does nothing new. "
				"R1 pressed again while it's open : the next wheel." ) ) SaveSettings();
			if ( g_menu.Button( "Emote wheel, keyboard : hold", Bound::Key( &g_emoteKey ),
				"Hold it for the emote wheel, point the mouse or the arrows at an emote (or press 1 - 8) and let go to play it. Tab : the next wheel. "
				"Enter, then press the key to use." ) ) StartBinding( BIND_KEY, &g_emoteKey );
		}
		g_menu.Info( "F10", "Animation viewer ( [ ] \\ ' and F11 inside it)" );
		g_menu.Section( "For testing" );
		g_menu.Info( "F7", "Next playback delay", Menu::DIM );
	}
	g_menu.End();
	InterlockedExchange( &g_menuTyping, g_menu.IsTyping() ? 1 : 0 );
	if ( const char* copy = g_menu.CopyText() )
		WriteClipboard( copy );

	// Our own pointer where the game hides Windows'.
	if ( in.mouseInside && !cursorShown ) {
		OverlayRect( in.mouseX - 1.0F, in.mouseY - 1.0F, 10.0F, 10.0F, 0xFF000000 );
		OverlayRect( in.mouseX, in.mouseY, 8.0F, 8.0F, 0xFFFFFFFF );
	}
}

// Tells the key handlers and the input polls whether the menu is open (it closes
// itself on Esc : this notices).
static void SyncMenuOpen()
{
	bool open = g_menu.IsOpen();
	if ( (g_menuOpen != 0) == open ) return;

	if ( open )
		for ( int i = 0; i < MK_COUNT; i++ )
			InterlockedExchange( &g_menuKeys[i], 0 );

	InterlockedExchange( &g_menuOpen, open ? 1 : 0 );
	g_inputBlocked = open;
	mp_log( "[MP] Menu %s.\n", open ? "opened" : "closed" );
}

void SetMenuOpen( bool open )
{
	if ( open == g_menu.IsOpen() ) return;
	if ( open ) {
		g_menu.Open();
		g_wheelUi.open = false;
	}
	else {
		g_menu.Close();
	}
	SyncMenuOpen();
}

// An emote's name in its wedge : two lines when it doesn't fit on one.
static void DrawWheelLabel( float x, float y, const char* name, float maxW, float lh, uint32_t color )
{
	float w = OverlayTextWidth( name );
	int split = -1;
	float bestW = w;
	char first[64];
	for ( const char* c = name; w > maxW && *c; c++ ) {
		int n = (int) (c - name);
		if ( *c != ' ' || n >= (int) sizeof(first) ) continue;
		memcpy( first, name, n ); first[n] = 0;
		float m = max( OverlayTextWidth( first ), OverlayTextWidth( c + 1 ) );
		if ( m < bestW ) { bestW = m; split = n; }
	}
	if ( split < 0 ) { OverlayText( floorf( x - w * 0.5F ), floorf( y - lh * 0.5F ), name, color ); return; }
	memcpy( first, name, split ); first[split] = 0;
	const char* second = name + split + 1;
	OverlayText( floorf( x - OverlayTextWidth( first ) * 0.5F ), floorf( y - lh ), first, color );
	OverlayText( floorf( x - OverlayTextWidth( second ) * 0.5F ), floorf( y ), second, color );
}

// A ring's piece, angles from the top, clockwise.
static void WheelSector( float cx, float cy, float r0, float r1, float a0, float a1, uint32_t color )
{
	const int N = 10;
	for ( int k = 0; k < N; k++ ) {
		float b0 = a0 + (a1 - a0) * k / N, b1 = a0 + (a1 - a0) * (k + 1) / N;
		float s0 = sinf( b0 ), c0 = cosf( b0 ), s1 = sinf( b1 ), c1 = cosf( b1 );
		OverlayTriangle( cx + s0 * r0, cy - c0 * r0, cx + s0 * r1, cy - c0 * r1, cx + s1 * r1, cy - c1 * r1, color );
		OverlayTriangle( cx + s0 * r0, cy - c0 * r0, cx + s1 * r1, cy - c1 * r1, cx + s1 * r0, cy - c1 * r0, color );
	}
}

// The emote wheel, middle of the screen : 8 wedges round the wheel's name.
static void DrawWheel( float sw, float sh, float h )
{
	const float PI = 3.14159265F, STEP = PI / 4.0F;
	const EmoteWheel& wheel = g_emoteWheels[g_wheelIndex];
	float cx = floorf( sw * 0.5F ), cy = floorf( sh * 0.47F );
	float outer = floorf( sh * 0.3F ), inner = floorf( outer * 0.36F ), mid = (inner + outer) * 0.5F;
	float lh = h * 1.15F;
	float gap = (3.0F / mid) * 0.5F;   // Half the angle between two wedges (3 pixels, midway).
	for ( int q = 0; q < 4; q++ ) WheelSector( cx, cy, 0.0F, inner - 3.0F, q * PI * 0.5F, (q + 1) * PI * 0.5F, 0xC0000000 );
	for ( int s = 0; s < WHEEL_SLOTS; s++ ) {
		bool on = s == g_wheelUi.hover;
		int e = wheel.slot[s];
		float a0 = (s - 0.5F) * STEP + gap, a1 = (s + 0.5F) * STEP - gap, ac = s * STEP;
		WheelSector( cx, cy, inner, outer, a0, a1, on ? 0xE0503C0C : 0xC0000000 );
		if ( on ) WheelSector( cx, cy, outer + 3.0F, outer + 7.0F, a0, a1, 0xFFFFE080 );
		float maxW = (s == 2 || s == 6) ? (outer - inner) * 0.9F : 2.0F * mid * sinf( STEP * 0.5F ) * 0.9F;
		DrawWheelLabel( cx + sinf( ac ) * mid, cy - cosf( ac ) * mid, e >= 0 ? EMOTES[e].name : "Empty", maxW, lh,
			e < 0 ? 0xFF808080 : on ? 0xFFFFE080 : 0xFFFFFFFF );
		if ( !g_wheelUi.byPad ) {   // The number keys.
			char n[4];
			sprintf_s( n, "%d", s + 1 );
			float r = outer + h * 1.1F;
			OverlayText( floorf( cx + sinf( ac ) * r - OverlayTextWidth( n ) * 0.5F ), floorf( cy - cosf( ac ) * r - h * 0.5F ), n, 0xFFB0B0B0 );
		}
	}
	// The middle : the wheel's name (and which of how many).
	char count[16];
	sprintf_s( count, "%d / %d", g_wheelIndex + 1, g_wheelCount );
	float ny = g_wheelCount > 1 ? cy - lh : cy - h * 0.5F;
	OverlayText( floorf( cx - OverlayTextWidth( wheel.name ) * 0.5F ), floorf( ny ), wheel.name, 0xFFFFE080 );
	if ( g_wheelCount > 1 ) OverlayText( floorf( cx - OverlayTextWidth( count ) * 0.5F ), floorf( ny + lh ), count, 0xFFB0B0B0 );
	// What to do, under it.
	char hint[160];
	if ( g_wheelUi.byPad ) {
		char button[24];
		if ( g_emoteButton == 4 ) strcpy_s( button, "L1" );
		else sprintf_s( button, "button %d", g_emoteButton + 1 );
		sprintf_s( hint, "Right stick : choose   Let go of %s : play%s", button, g_wheelCount > 1 ? (g_emoteNeedsR1 ? "   R1 again : next wheel" : "   R1 : next wheel") : "" );
	}
	else sprintf_s( hint, "Mouse, arrows or 1 - 8 : choose   Let go of %s : play%s", KeyName( g_emoteKey ), g_wheelCount > 1 ? "   Tab : next wheel" : "" );
	float hw = OverlayTextWidth( hint ), hy = floorf( cy + outer + h * 2.0F );
	float pad = h * 0.3F;
	OverlayRect( floorf( cx - hw * 0.5F ) - pad, hy - pad * 0.5F, hw + 2.0F * pad, h + pad, 0xA0000000 );
	OverlayText( floorf( cx - hw * 0.5F ), hy, hint, 0xFFD8D8D8 );
}

// Where a puppet's tag goes on the screen now : the camera of this frame, the
// puppet where it is. The game doesn't draw a puppet in every frame where it's seen
// through a wall (St. Claire's camp at night : one frame in two or three) : a tag
// only shown on frames it was drawn flickered there. It stays up for a few frames
// after the last draw, placed again each frame. False : not shown.
static const uint32_t TAG_GRACE_FRAMES = 12;

bool PlaceNameTag( const Peer& peer, IDirect3DDevice9* dev, float* x, float* y, float* depth )
{
	EIEvilHarryP* p = peer.beast;
	if ( !p || !peer.tagFrame || g_presentCount - peer.tagFrame > TAG_GRACE_FRAMES || g_levelLeaving ) return false;

	// The view the renderer drew this frame with (the last it was given : any
	// reflection comes first), whichever camera mode built it. Choosing among the
	// cameras' own look-ats instead left the tag behind when aiming the TNT, and
	// jumping about while the camera swung into it.
	if ( !g_haveProj || g_presentCount - g_rendFrame > 1 ) return false;

	Matrix4f view;
	if ( !MatInverseAffine(g_rendCam, &view) ) return false;

	IDirect3DSurface9* screen = 0;
	D3DSURFACE_DESC d = { };
	bool ok = SUCCEEDED( dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &screen) ) && screen && SUCCEEDED( screen->GetDesc(&d) );
	if ( screen ) screen->Release();
	if ( !ok ) return false;

	const float* proj = &g_cameraProj.data[0][0];
	const Matrix4f& m = p->m_transformMatrix;
	float px = m.data[3][0], py = m.data[3][1], pz = m.data[3][2] + TAG_ABOVE_FEET;
	float v[4], c[4];
	for ( int j = 0; j < 4; j++ ) v[j] = px * view.data[0][j] + py * view.data[1][j] + pz * view.data[2][j] + view.data[3][j];
	for ( int j = 0; j < 4; j++ ) c[j] = v[0] * proj[j] + v[1] * proj[4 + j] + v[2] * proj[8 + j] + v[3] * proj[12 + j];
	if ( c[3] < 0.1F ) return false;   // Behind the camera.

	*x = (c[0] / c[3] + 1.0F) * 0.5F * d.Width;
	*y = (1.0F - c[1] / c[3]) * 0.5F * d.Height;
	*depth = c[3];
	return true;
}

// --------------------------------------------------------------------------------
// The overlay : name tags over the other players, the player list (F9), and
// "joined" / "left" messages.
// --------------------------------------------------------------------------------

static uint32_t WithAlpha( uint32_t color, float alpha )
{
	if ( alpha < 0.0F ) alpha = 0.0F;
	if ( alpha > 1.0F ) alpha = 1.0F;
	return (color & 0x00FFFFFF) | ((uint32_t) (((color >> 24) & 0xFF) * alpha + 0.5F) << 24);
}

static void DrawOverlay( IDirect3DDevice9* dev )
{
	if ( TestAnimSurveyOn() ) return;   // Clean frames.

	uint32_t now = NetNowMs();
	if ( InterlockedExchange(&g_reqMenu, 0) ) SetMenuOpen( !g_menu.IsOpen() );
	bool any = g_showList || g_animViewer || g_menu.IsOpen() || PlayingHs() || EmoteWheelShown();
	for ( int i = 0; i < MAX_PEERS && !any; i++ ) any = g_peers[i].beast && g_peers[i].tagFrame && g_presentCount - g_peers[i].tagFrame <= TAG_GRACE_FRAMES;
	for ( int i = 0; i < MAX_TOASTS && !any; i++ ) any = g_toasts[i].text[0] && now - g_toasts[i].time < TOAST_MS;
	if ( !any ) return;

	D3DVIEWPORT9 vp;
	if ( FAILED(dev->GetViewport(&vp)) ) return;
	float h = vp.Height / 38.0F;
	if ( h < 14.0F ) h = 14.0F;
	if ( !OverlayBegin(dev, h) ) return;
	float sw, sh;
	OverlayScreenSize( &sw, &sh );
	float pad = h * 0.3F;

	// Name tags.
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		const Peer& p = g_peers[i];

		float tagX, tagY, tagDepth;
		if ( !g_showTags || g_menu.IsOpen() || EmoteWheelShown() || HiddenFromMe(p.id) || !PlaceNameTag(p, dev, &tagX, &tagY, &tagDepth) ) continue;

		const float from = (float) g_tagFadeFrom, to = (float) g_tagFadeTo;
		const float alpha = to > from ? 1.0F - (tagDepth - from) / (to - from) : (tagDepth < from ? 1.0F : 0.0F);
		if ( alpha <= 0.0F ) continue;

		const char* name = PlayerName( p.id );
		float w = OverlayTextWidth( name );
		float x = floorf( tagX - w * 0.5F ), y = floorf( tagY - h * 1.2F );
		OverlayRect( x - pad, y - pad * 0.5F, w + 2.0F * pad, h + pad, WithAlpha(0x80000000, alpha) );
		OverlayText( x, y, name, WithAlpha(0xFFFFFFFF, alpha) );
	}

	float listLeft = sw;   // Messages stay clear of the player list.
	// The player list, top right.
	if ( g_showList && !g_menu.IsOpen() ) {
		char left[MAX_LIST_ROWS][96], right[MAX_LIST_ROWS][64];
		uint32_t colors[MAX_LIST_ROWS];
		int rows = BuildPlayerRows( left, right, colors, "F1 : menu" );

		float wl = 0.0F, wr = 0.0F;
		for ( int r = 0; r < rows; r++ ) {
			float a = OverlayTextWidth( left[r] ), b = OverlayTextWidth( right[r] );
			if ( a > wl ) wl = a;
			if ( b > wr ) wr = b;
		}
		float gap = h * 1.5F;
		float bw = wl + gap + wr + 2.0F * pad, bh = rows * h * 1.3F + 2.0F * pad;
		float bx = floorf( sw - bw - h ), by = h;
		listLeft = bx - pad;
		OverlayRect( bx, by, bw, bh, 0xA0000000 );
		for ( int r = 0; r < rows; r++ ) {
			float y = by + pad + r * h * 1.3F;
			OverlayText( bx + pad, y, left[r], colors[r] );
			OverlayText( bx + bw - pad - OverlayTextWidth( right[r] ), y, right[r], colors[r] );
		}
	}

	// Messages : bottom middle, the newest lowest (clear of the player list, whatever its width).
	float ty = floorf( sh - h * 3.5F );
	for ( int i = 0; i < MAX_TOASTS; i++ ) {
		if ( !g_toasts[i].text[0] || now - g_toasts[i].time >= TOAST_MS ) continue;
		float alpha = (TOAST_MS - (now - g_toasts[i].time)) / 1000.0F;   // Fades out over the last second.
		float w = OverlayTextWidth( g_toasts[i].text );
		float x = floorf( (sw - w) * 0.5F );
		OverlayRect( x - pad, ty - pad * 0.5F, w + 2.0F * pad, h + pad, WithAlpha( 0xA0000000, alpha ) );
		OverlayText( x, ty, g_toasts[i].text, WithAlpha( 0xFFFFE080, alpha ) );
		ty -= h * 1.4F;
	}

	// Hide and seek : our role and hide time, top left, under the game's health.
	if ( PlayingHs() && !g_menu.IsOpen() ) {
		char hidden[16], text[48];
		FormatHideTime( g_hsMyHideMs, hidden, sizeof(hidden) );
		const char* role = g_hsMyRole == ROLE_HIDER ? "HIDER" : "SEEKER";
		sprintf_s( text, "%s   %s", role, hidden );

		const float x = floorf( sw * (28.0F / 640.0F) + pad ), y = floorf( sh * 0.14F );   // The box's left : the first health box's (the game's HUD is laid out 640 wide).
		const float w = OverlayTextWidth( text );
		OverlayRect( x - pad, y - pad * 0.5F, w + 2.0F * pad, h + pad, 0xA0000000 );
		OverlayText( x, y, role, g_hsMyRole == ROLE_HIDER ? 0xFFFFE080 : 0xFFFFFFFF );
		OverlayText( x + w - OverlayTextWidth( hidden ), y, hidden, 0xFFFFFFFF );
	}

	if ( EmoteWheelShown() && !g_menu.IsOpen() )
		DrawWheel( sw, sh, h );

	// The animation viewer, bottom left.
	if ( g_animViewer && g_animIndex < g_animNoteCount ) {
		const AnimNote& n = g_animNotes[g_animIndex];
		int stars = 0, place = 0;
		for ( int i = 0; i < g_animNoteCount; i++ ) {
			if ( g_animNotes[i].star ) { stars++; if ( i <= g_animIndex ) place++; }
		}
		char line[3][256];
		uint32_t colors[3] = { n.star ? 0xFFFFE080 : 0xFFFFFFFF, 0xFFFFFFFF, 0xFFB0B0B0 };
		if ( g_animStarredOnly && stars )
			sprintf_s( line[0], "%sAnimation #%03d   (starred %d of %d)   0x%.8X   %s", n.star ? "* " : "", n.num, place, stars, n.crc, n.source );
		else
			sprintf_s( line[0], "%sAnimation #%03d   (%d of %d)   0x%.8X   %s", n.star ? "* " : "", n.num, g_animIndex + 1, g_animNoteCount, n.crc, n.source );
		if ( n.note[0] ) sprintf_s( line[1], "%s", n.note );
		else sprintf_s( line[1], "(no notes yet : %%LOCALAPPDATA%%\\PTLE Multiplayer\\anim-notes.txt)" );
		colors[1] = n.note[0] ? 0xFFFFFFFF : 0xFFB0B0B0;
		sprintf_s( line[2], "[ ] : previous / next (Shift : 10)   \\ : replay   F11 : star   ' : %s   F10 : close",
			g_animStarredOnly ? "all clips" : "starred only" );
		float w = 0.0F;
		for ( int r = 0; r < 3; r++ ) { float a = OverlayTextWidth( line[r] ); if ( a > w ) w = a; }
		float bw = w + 2.0F * pad, bh = 3 * h * 1.3F + 2.0F * pad;
		float bx = h, by = floorf( sh - bh - h );
		OverlayRect( bx, by, bw, bh, 0xA0000000 );
		for ( int r = 0; r < 3; r++ ) OverlayText( bx + pad, by + pad + r * h * 1.3F, line[r], colors[r] );
	}

	if ( g_menu.IsOpen() ) {
		DrawMenu( sw, sh, h );
		SyncMenuOpen();   // Esc closed it.
	}

	OverlayEnd();
}

// Leaving the room when the game closes, so the others drop our puppet at once
// instead of after a timeout. The game ends through kernel32's ExitProcess (from
// its own import table, patched below), and so does the autotest (QuitGame).
ExitProcessFunc g_origExitProcess = ExitProcess;

void LeaveRoom()
{
	if ( g_transport ) g_transport->Leave();
}

static VOID WINAPI ExitProcess_Hook( UINT code )
{
	mp_log( "[MP] The game is closing (exit code %u).\n", code );   // A log that ends without this : the game was killed or crashed.
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

#include "gizmod/event/HarryDeathEvent.h"

class MultiplayerPlugin : public GizmodPlugin
	, public ILoadLevelListener
	, public ILevelLoadedListener
	, public IHarryDeathListener
{
public:

	virtual const char* getDisplayName() const { return "Multiplayer (puppet test)"; }
	virtual const char* getSystemName() const { return "Multiplayer"; }

	virtual void onEnable()
	{
		OpenLogFile();
		mp_log( "[MP] Multiplayer mod version %s.\n", MP_VERSION );
		if ( !IsKnownGameVersion() ) {
			mp_log( "[MP] This isn't the game version the mod was made for (PC 1.0.12.18-1) : the mod is off.\n" );
			MessageBoxA( 0, "The Multiplayer mod was made for Pitfall: The Lost Expedition PC version 1.0.12.18-1, "
				"and this copy of the game is a different version.\n\nThe mod is turned off. The game will run without it.",
				"Multiplayer mod", MB_OK | MB_ICONWARNING );
			return;
		}
		AddVectoredExceptionHandler( 1, CrashLogger );

		register_types();

		InitPeers();

		// Replace Harry and Evil Harry tick functions with our own.
		const uint32_t harrySlot = EIHarry::VTABLE + VTABLE_SLOT_TICK;
		const uint32_t evilSlot  = EIEvilHarryP::VTABLE + VTABLE_SLOT_TICK;
		injector::WriteMemory( harrySlot, &EIHarry_Tick_Hook );
		injector::WriteMemory( evilSlot, &EIEvilHarryP_Tick_Hook );

		InstallItemDrawHooks();
		InstallPitHooks();
		InstallFadeHooks();
		HookCamera();

		Gizmod::getInstance()->getEventListener()->registerEvent<LoadLevelEvent>( this );
		Gizmod::getInstance()->getEventListener()->registerEvent<LevelLoadedEvent>( this );
		Gizmod::getInstance()->getEventListener()->registerEvent<HarryDeathEvent>( this );

		g_netLog = mp_log;
		NetConfig config;
		ReadNetConfig( &config );
		ApplyMyOutfit();   // Before the first level loads.
		ResetHistory();
		g_myNameLen = CleanName( config.name, (int) strlen(config.name), g_myName );
		if ( !g_myName[0] ) {
			strcpy_s( g_myName, "Harry" );
			g_myNameLen = 5;
		}
		strncpy_s( g_roomName, config.room, _TRUNCATE );
		if ( !HookGameExitProcess() ) mp_log( "[NET] Couldn't hook the game's exit : the others drop us after a timeout.\n" );
		if ( config.delayMs > 0 ) {   // The nearest choice.
			const int n = countof(DELAY_CHOICES);
			for ( int i = 0; i < n; i++ ) {
				if ( abs(DELAY_CHOICES[i] - config.delayMs) < abs(DELAY_CHOICES[g_delayIndex] - config.delayMs) )
					g_delayIndex = i;
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

		static const uint8_t MODEL_ORIGINAL[6] = { 0x89, 0x81, 0x30, 0x01, 0x00, 0x00 };   // mov [ecx+0x130], eax.
		if ( memcmp( (void*) HARRY_MODEL_STORE, MODEL_ORIGINAL, 6 ) == 0 ) {
			injector::MakeCALL( HARRY_MODEL_STORE, &HarryModel_Hook );
			injector::MakeNOP( HARRY_MODEL_STORE + 5, 1 );
		}
		else mp_log( "[MP] Harry's model isn't stored where we expect : no Snowy Harry outfit for us.\n" );

		InstallInputHooks();

		EnableTests();

		mp_log( "[MP] Hooked ticks. F1 menu, F7 delay.\n" );
	}

	virtual void onHarryDeath( HarryDeathEvent& e )
	{

	}

	virtual void onLoadLevel( LoadLevelEvent& e )
	{
		mp_log( "[MP] Loading level 0x%.8X.\n", e.getLevelCRC() );
		NoteLevelLeaving( e.getLevelCRC() );
		ApplyMyOutfit();   // The game clears its cheats while it starts : set again before each level loads.
		ForgetRetroTextures();
		ForgetPuppets();
	}

	virtual void onLevelLoaded( LevelLoadedEvent& e )
	{
		ForgetPuppets();
	}
};

DECLARE_PLUGIN( MultiplayerPlugin );
