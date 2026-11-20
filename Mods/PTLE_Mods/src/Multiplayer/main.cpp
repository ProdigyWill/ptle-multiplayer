// --------------------------------------------------------------------------------
// Multiplayer (milestone 1 : puppet test).
//
// Spawns a second Harry-looking character (the game's Evil Harry beast) and drives
// its transform from code every frame. Every tick the local player's state goes out
// as a snapshot packet (net/protocol.h) through a transport (net/transport.h), and
// the puppet replays the snapshots that come in (net/remote.h).
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
//   F5 : spawn the puppet in the current level.
//   F6 : cycle the puppet's animation : automatic (idle when still, run when
//        moving, facing its direction of travel), then idle/rest states by hand.
//   F7 : cycle the playback delay (0.13s .. 3s).
//   F8 : Evil Harry stays where he is (spawned where Harry stands if needed) and
//        copies Harry in place : animations, items, facing, jumps and climbs (up and
//        down only). F8 again : he follows.
// --------------------------------------------------------------------------------

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

#include "utils/func.h"

#include "version.h"
#include "net/protocol.h"
#include "net/remote.h"
#include "net/snapshot.h"
#include "net/transport.h"
#include "overlay.h"
#include "menu.h"
#include "ptle/levels/level_info.h"


#define FIELD( obj, type, offset ) (*(type*)((uint8_t*)(obj) + (offset)))

GET_METHOD( 0x626670, void, AddToWorld, ERLevel*, EInstance* );
GET_METHOD( 0x6268D0, void, ERLevel_SetupEntity, ERLevel*, EInstance*, EInstance* );
GET_METHOD( 0x412850, void, EIBeast_LoadBeastAsset, EIBeast* );
GET_FUNC(   0x6F8EC0, uint32_t, HashPath, const char* );


static const uint32_t VTABLE_SLOT_TICK   = 0x2C;     // EInstance::Tick().
static const uint32_t VTABLE_SLOT_RENDER = 0x34;     // EInstance::Render( context, arg ).

static const uint32_t HWND_GAME_WINDOW   = 0x93ED60;


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
static char g_logDir[MAX_PATH] = { 0, };

// %LOCALAPPDATA%\PTLE Multiplayer (created) : what the mod writes goes there.
static bool GetDataDir( char* path )   // MAX_PATH.
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

// Which mod loader (binkw32.dll) runs us : a crash inside it means something only
// with its own build at hand. Its size and build time tell builds apart.
static void LogModLoader()
{
	HMODULE loader = GetModuleHandleA( "binkw32.dll" );
	char path[MAX_PATH] = { 0, };
	if ( !loader || !GetModuleFileNameA( loader, path, MAX_PATH ) ) { mp_log( "[MP] Mod loader : binkw32.dll isn't loaded?\n" ); return; }
	IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*) ((uint8_t*) loader + ((IMAGE_DOS_HEADER*) loader)->e_lfanew);
	time_t built = (time_t) nt->FileHeader.TimeDateStamp;
	tm t = { 0, };
	gmtime_s( &t, &built );
	WIN32_FILE_ATTRIBUTE_DATA a = { 0, };
	GetFileAttributesExA( path, GetFileExInfoStandard, &a );
	mp_log( "[MP] Mod loader : %s, %u bytes, built %04d-%02d-%02d %02d:%02d UTC.\n", path, a.nFileSizeLow,
		t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min );
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

static inline void CopyMatrix( Matrix4f& to, const Matrix4f& from )
{
	memcpy( &to, &from, sizeof(Matrix4f) );
}

static bool g_autotest = false;                // PTLE_MP_AUTOTEST=1 (see the autotest section).

// Animation survey (PTLE_MP_ANIMSURVEY=1) : every clip loaded on Harry and on Evil
// Harry, played in turn on the puppet (which then stops copying Harry's pose), each
// filmed in a few cropped frames (logs\shots\anim_<n>_f<k>.bmp).
static bool g_animSurvey = false;
static int g_crashTest = 0;                       // PTLE_MP_CRASHTEST=av|div|stack : the autotest crashes the game (the crash logger's test).
static int g_pitTest = 0;                        // PTLE_MP_PITTEST=1 : Harry walks into Crash Site's pitfall, the puppet right behind him; 2 : the puppet (ahead of him) goes in alone, as another player's does.
static int g_iceTest = 0;                         // PTLE_MP_ICETEST=1 : the autotest puts the puppet in "thrown from water"; 2 : walks Harry (and the puppet) into Spinja Lair's cold water. PTLE_MP_ICEALLOW=1 : let it freeze, as before the fix.
static bool g_allowPuppetFreeze = false;
static volatile LONG g_puppetFreezesStopped = 0;     // Puppets kept out of the ice cube (ThrownFromWater_Hook).
static bool g_deathTest = false;                 // PTLE_MP_DEATHTEST=1 : the autotest kills Harry beside his puppet.
// Leaving a level (see NoteLevelLeaving) : the old one still runs and is shown.
static bool g_levelLeaving = false;
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

static int g_raftTest = 0;   // PTLE_MP_RAFTTEST=1 : Spinja Lair's wading water, the raft out, Harry turns paddling one side : the puppet must face as he does.
static int g_aimTest = 0;   // 2 : with the raft out instead (Down looks from Harry's head, the raft isn't drawn).
                   // PTLE_MP_AIMTEST=1 : the autotest holds Down (first-person look) with the puppet ahead, to check its tag.
static int g_distTest = 0;                        // PTLE_MP_DISTTEST=1 : the autotest moves the puppet away, to see how far it's drawn; two games : 2 runs away, 3 watches. PTLE_MP_FADEALLOW=1 : puppets fade as before.
static bool g_allowPuppetFade = false;
static int g_emoteTest = 0;                      // PTLE_MP_EMOTETEST=1 : emotes, one game.
static int g_hsTest = 0;                         // PTLE_MP_HSTEST=1 : hide and seek, one game (role, hide time).
static bool g_skinTest = false;                  // PTLE_MP_SKINTEST=1 : the autotest puts each outfit's model on the puppet.
static bool g_menuTest = false;                  // PTLE_MP_MENUTEST=1 : the autotest drives the menu (F1).
static bool g_viewerTest = false;                // PTLE_MP_VIEWERTEST=1 : the autotest drives the animation viewer.
static uint32_t g_animSurveyClip = 0;            // The clip the puppet plays (0 : normal).
static volatile LONG g_cropPending = 0;          // A cropped grab, taken in Present.
static char g_cropName[64];
static RECT g_crop;

static PlayerSnapshot g_local;                 // Static storage keeps Matrix4f 16-byte aligned.
static bool g_haveLocal = false;

// One per player id the relay hands out (its rooms hold 8). Each other player in
// the room gets their own puppet; with echo (or the loopback), our own id gets one
// too and replays us.
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
	int shownOutfit;                   // The outfit its model is (-1 : whatever the game gave it).
	EIEvilHarryP* limbo;               // Set aside when the game announced a level load (see SuspectPuppets).
	uint32_t limboSince;
	int limboTicks;
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
	DWORD lastTick;                    // When the puppet last ticked (GetTickCount).
	int ticks;
	EIEvilHarryP* watched;             // The puppet lastTick is about (see ReplaceStalledPuppets).

	// Where their name tag goes (screen pixels), noted when the puppet renders.
	float tagX, tagY, tagDepth;
	uint32_t tagFrame;                 // g_presentCount then.
};
static Peer g_peers[MAX_PEERS];

// Who is in the room (from their PACKET_INFO), kept across levels. Presence goes by
// those packets (sent even while a player's game is loading or paused), not by
// snapshots.
static const uint32_t INFO_INTERVAL_MS = 1000;
static const uint32_t INFO_TIMEOUT_MS = 15000;
// Outfits. Harry's own comes from the level : his jungle clothes, or his snowy
// ones in the snowy levels (White Valley...), which is also the model Evil Harry
// has. The game has three more, each a cheat (a flag, read when a level loads :
// Harry gets that model, 0x4ADB38). "Default" (OUTFIT_DEFAULT, not one of these :
// the others are told what's worn) leaves it to the level; "Harry" makes it the
// jungle one everywhere, "Snowy Harry" the snowy one (HarryModel_Hook). A character's model is its CRC at
// +0x130 for Harry, +0x2F4 for a beast; 0x60F770( character, crc ) changes a
// beast's on the spot (Evil Harry's Init does it to match Harry, 0x47A90D), which is
// how each puppet wears its own player's outfit. All four share Harry's skeleton.
struct Outfit { const char* name; uint32_t model; uint32_t cheatFlag; };
static const Outfit OUTFITS[] = {
	{ "Harry",       0xC9252A08, 0 },
	{ "Nicole",      0x7BBEDDE3, 0x90DA18 },
	{ "Native",      0x182A7689, 0x90DA19 },
	{ "Retro Harry", 0xBF8D2C68, 0x90DA1F },
	{ "Snowy Harry", 0x94DB2F98, 0 },
};
static const int SNOWY_OUTFIT = 4;
static uint32_t g_forceHarryModel = 0;   // The model Harry gets instead of the level's, when a level loads (0 : the level's).
static const int NUM_OUTFITS = sizeof(OUTFITS) / sizeof(OUTFITS[0]);
static const int OUTFIT_DEFAULT = -1;   // Chosen : whatever the level gives Harry.
static int g_myOutfit = OUTFIT_DEFAULT;  // The one chosen (Multiplayer.ini, [Appearance] Outfit, by name) : worn from the next level load.
static int g_myWornOutfit = 0;   // The one Harry wears now : what the others are told.
static int g_testOutfit = -1;    // Autotest : every puppet wears this.

// Retro Harry's colours. His model takes all its colour from a 32x32 texture of four
// flat squares, so each player can have their own (see the Retro palette section).
static const int RETRO_OUTFIT = 3;
enum { RETRO_HAIR, RETRO_SKIN, RETRO_SHIRT, RETRO_TROUSERS, RETRO_PARTS };
static const char* const RETRO_PART_NAMES[RETRO_PARTS] = { "Hair", "Head", "Shirt", "Pants" };   // In the menu, and the ini keys (RetroHair...).
static const uint8_t RETRO_DEFAULT[RETRO_PARTS][3] = { { 142, 118, 62 }, { 255, 161, 123 }, { 78, 200, 74 }, { 0, 87, 0 } };
static uint8_t g_myColors[RETRO_PARTS][3];   // Multiplayer.ini, [Appearance] RetroHair... (RRGGBB).

struct PlayerInfo
{
	bool present;
	char name[MAX_NAME + 1];
	uint32_t level;
	uint32_t lastInfo;
	uint8_t outfit;
	uint8_t colors[RETRO_PARTS][3];
	char version[16];    // Their mod's (MP_VERSION); empty : a build from before versions.
	uint8_t role;        // Hide and seek : ROLE_SEEKER, ROLE_HIDER.
	uint32_t hideMs;     // Their total hide time.
	bool playsHs;        // Their build has hide and seek (sends a role).
};

// Hide and seek (the "PTLE Multiplayer: Hide-and-Seek Plan" doc). No rounds and no
// host : each player chooses their own role. A hider's hide time adds up while
// they're one and Harry is in a level (not loading, not paused); only they can
// reset it, and it's kept in a file so restarts and crashes don't lose it.
enum { ROLE_SEEKER, ROLE_HIDER, ROLE_NONE };   // ROLE_NONE (sent) : not playing, the game mode is off.
static int g_myRole = ROLE_SEEKER;      // Everyone starts as a seeker.

// Game modes (F1 -> Gamemodes; Multiplayer.ini, [Gamemodes] Mode). Off : just
// see each other.
enum { MODE_OFF, MODE_HIDE_AND_SEEK, NUM_MODES };
static const char* const MODE_NAMES[NUM_MODES] = { "Off", "Hide and seek" };
static int g_gameMode = MODE_OFF;
static bool PlayingHs() { return g_gameMode == MODE_HIDE_AND_SEEK; }

// Swapping roles without the menu : hold the controller button or the key, then
// either direction of the d-pad or either arrow key (Multiplayer.ini, [Keys]
// RoleSwapButton, RoleSwapKey). While it's held, the d-pad and the arrows don't
// reach the game : no item comes out.
static int g_swapButton = 4;     // DirectInput button : the fifth, L1 / LB.
static int g_swapKey = 0x2A;     // DirectInput key code : Left Shift.
static uint32_t g_myHideMs = 0;         // Total hide time (hide-time.txt).
static bool g_testHideReport = false;   // Autotest : log the hide time each second.

// Caught : a seeker as close as this to a hider, as the hider's game shows him
// (Harry is about 7 units tall). The hider's game decides, so a catch is never a
// surprise to the one caught.
static const float CATCH_DISTANCE = 3.5F;
static const int CAUGHT_SENDS = 5;             // The CAUGHT packet, this many times...
static const uint32_t CAUGHT_RESEND_MS = 200;  // ...this far apart.
static struct { int seeker; uint32_t number; int left; uint32_t lastSent; } g_caughtOut = { -1, 0, 0, 0 };
static uint32_t g_catchNumber = 0;             // Ours (a random start : another session's don't look the same).
static uint32_t g_lastCatchHeard[MAX_PEERS];   // Each hider's last catch number heard.
static int g_testCaughtBy = -1, g_testCaughtHider = -1;   // Autotest : the last catch heard.
static float g_testNearestSeeker = -1.0F;   // Autotest : this tick's nearest seeker (-1 : none shown).
static bool HiddenFromMe( int id );   // (Below : a hider, to a seeker.)
static int VkToDik( int vk );            // (Below : the role swap's key binding.)
static const char* KeyName( int dik );
static const int MAX_VERSION = 15;
static char g_myVersion[MAX_VERSION + 1] = MP_VERSION;   // Sent to the others (tests : PTLE_MP_FAKEVERSION, another one).
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
// The hide time's file : %LOCALAPPDATA%\PTLE Multiplayer\hide-time.txt, the total in
// milliseconds (the autotests keep theirs in hide-time-autotest.txt).
static void HideTimePath( char* path )   // MAX_PATH.
{
	path[0] = 0;
	if ( GetDataDir( path ) ) strcat_s( path, MAX_PATH, g_autotest ? "\\hide-time-autotest.txt" : "\\hide-time.txt" );
}

static bool g_hideTimeLoaded = false;
static uint32_t g_hideSavedMs = 0;

static void LoadHideTime()
{
	if ( g_hideTimeLoaded ) return;
	g_hideTimeLoaded = true;
	char path[MAX_PATH];
	HideTimePath( path );
	FILE* f = 0;
	if ( path[0] && fopen_s( &f, path, "r" ) == 0 && f ) {
		unsigned long ms = 0;
		if ( fscanf_s( f, "%lu", &ms ) == 1 ) g_myHideMs = (uint32_t) ms;
		fclose( f );
	}
	g_hideSavedMs = g_myHideMs;
	mp_log( "[HS] Hide time : %u ms (%s).\n", g_myHideMs, path );
}

// Written to a new file, then moved over the old one : a crash mid-write keeps the old.
static void SaveHideTime()
{
	if ( !g_hideTimeLoaded ) return;
	char path[MAX_PATH], temp[MAX_PATH];
	HideTimePath( path );
	if ( !path[0] ) return;
	sprintf_s( temp, "%s.new", path );
	FILE* f = 0;
	if ( fopen_s( &f, temp, "w" ) != 0 || !f ) return;
	fprintf( f, "%u\n", g_myHideMs );
	fclose( f );
	if ( MoveFileExA( temp, path, MOVEFILE_REPLACE_EXISTING ) ) g_hideSavedMs = g_myHideMs;
}

static void SetMyRole( int role )
{
	if ( role == g_myRole ) return;
	g_myRole = role;
	mp_log( "[HS] Now a %s (hide time %u ms).\n", role == ROLE_HIDER ? "hider" : "seeker", g_myHideMs );
	SaveHideTime();
}

// Each of Harry's ticks : a hider's time goes on. A gap (loading, the pause menu :
// Harry doesn't tick then) doesn't count.
static void HideTick()
{
	LoadHideTime();
	static uint32_t last = 0;
	uint32_t now = GetTickCount();
	uint32_t dt = now - last;
	last = now;
	if ( PlayingHs() && g_myRole == ROLE_HIDER && dt < 250 ) g_myHideMs += dt;
	if ( g_myHideMs - g_hideSavedMs >= 5000 ) SaveHideTime();   // Every 5 s while hiding.
	static uint32_t lastReport = 0;
	if ( g_testHideReport && now - lastReport >= 1000 ) { lastReport = now; mp_log( "[AT] HS : %s, hide time %u ms.\n", g_myRole == ROLE_HIDER ? "hider" : "seeker", g_myHideMs ); }
}

// 754000 ms : "12:34"; an hour or more : "1:02:03".
static void FormatHideTime( uint32_t ms, char* out, size_t size )
{
	uint32_t s = ms / 1000;
	if ( s >= 3600 ) sprintf_s( out, size, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60 );
	else sprintf_s( out, size, "%u:%02u", s / 60, s % 60 );
}

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

// Animation viewer (F10) : a puppet pinned beside Harry plays one clip at a time,
// for going through Harry's animations and noting what they are.
static bool g_animViewer = false;
static Peer* g_viewerPeer = 0;          // The puppet it uses.
static Vector3f g_viewerPin;            // Where that puppet stands.

// The Emotes page's preview uses the same : g_viewerPeer, while it's choosing. Between
// previews its puppet is parked out of sight (not drawn, far below), kept for the next.
static bool g_emotePreview = false;
static Peer* g_parkedPeer = 0;
static Vector3f g_parkedPin;

static bool IsParkedPeer( const Peer* p ) { return p && p == g_parkedPeer && !((g_animViewer || g_emotePreview) && p == g_viewerPeer); }
static bool IsViewerPeer( const Peer* p ) { return ((g_animViewer || g_emotePreview) && p == g_viewerPeer) || IsParkedPeer( p ); }

// The clip the current peer's puppet is made to play (survey, viewer), else 0.
static uint32_t ForcedClip()
{
	if ( g_animSurvey ) return g_animSurveyClip;
	return IsViewerPeer( g_cur ) ? g_animSurveyClip : 0;
}

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
		if ( h.type == PACKET_CAUGHT ) {
			if ( h.player == me && !echo ) continue;
			if ( length < PACKET_HEADER_SIZE + 1 || packet[PACKET_HEADER_SIZE] >= MAX_PEERS ) { g_netStats.badPackets++; continue; }
			if ( g_lastCatchHeard[h.player] == h.sequence ) continue;   // A resend.
			g_lastCatchHeard[h.player] = h.sequence;
			int seeker = packet[PACKET_HEADER_SIZE];
			g_players[h.player].role = ROLE_SEEKER;   // (Their next info says so too.)
			g_testCaughtBy = seeker; g_testCaughtHider = h.player;
			mp_log( "[HS] Player %d (\"%s\") was caught by player %d (\"%s\").\n", h.player, PlayerName( h.player ), seeker, PlayerName( seeker ) );
			if ( h.player == me ) {}   // (Our own, echoed : we said so when it happened.)
			else if ( seeker == me ) Toast( "You caught %s!", PlayerName( h.player ) );
			else {
				char hider[MAX_NAME + 1];
				strcpy_s( hider, PlayerName( h.player ) );
				Toast( "%s caught %s", PlayerName( seeker ), hider );
			}
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
			int extra = PACKET_HEADER_SIZE + 5 + nameLength;   // After the name : the outfit (not sent by older builds).
			info.outfit = (extra < length && packet[extra] < NUM_OUTFITS) ? packet[extra] : 0;
			if ( extra + 1 + (int) sizeof(info.colors) <= length ) memcpy( info.colors, packet + extra + 1, sizeof(info.colors) );
			else memcpy( info.colors, RETRO_DEFAULT, sizeof(info.colors) );
			int at = extra + 1 + (int) sizeof(info.colors);   // Then the version : u8 length, text (not sent before versions).
			char version[MAX_VERSION + 1] = "";
			if ( at < length && packet[at] <= MAX_VERSION && at + 1 + packet[at] <= length ) CleanName( (const char*) packet + at + 1, packet[at], version );
			bool versionNews = !info.present || strcmp( info.version, version ) != 0;
			strcpy_s( info.version, version );
			at += at < length ? 1 + packet[at] : 0;   // Then hide and seek : u8 role, u32 hide time (ms).
			info.playsHs = at + 5 <= length;
			info.role = info.playsHs && packet[at] <= ROLE_NONE ? packet[at] : ROLE_NONE;
			info.hideMs = 0;
			if ( info.playsHs ) memcpy( &info.hideMs, packet + at + 1, 4 );
			if ( !info.present || strcmp( info.name, name ) != 0 ) {
				mp_log( "[NET] Player %d is \"%s\", in %s.\n", h.player, name, LevelName( info.level ) );
				if ( !info.present && h.player != me ) Toast( "%s joined", name );
				strcpy_s( info.name, name );
				info.present = true;
			}
			if ( versionNews ) {
				mp_log( "[NET] Player %d has version %s.\n", h.player, version[0] ? version : "(older, none sent)" );
				if ( h.player != me && strcmp( version, MP_VERSION ) != 0 )
					Toast( "%s has %s%s (you : %s)", name, version[0] ? "version " : "an older version", version, MP_VERSION );
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
				if ( h.player != 0 && first->beast && !first->heard && !peer->beast && !peer->limbo ) MovePuppet( first, peer );
				g_mainId = h.player;
				g_cur = MainPeer();
			}
		}
		peer->remote.Add( incoming, h.sequence );
		peer->lastHeard = now;
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
		uint8_t info[PACKET_HEADER_SIZE + 5 + MAX_NAME + 1 + sizeof(g_myColors) + 1 + MAX_VERSION + 5];
		EncodeHeader( PACKET_INFO, (uint8_t) me, 0, info );
		uint32_t where = g_haveLocal ? level : 0;
		memcpy( info + PACKET_HEADER_SIZE, &where, 4 );
		int n = (int) strlen( g_myName );
		info[PACKET_HEADER_SIZE + 4] = (uint8_t) n;
		memcpy( info + PACKET_HEADER_SIZE + 5, g_myName, n );
		info[PACKET_HEADER_SIZE + 5 + n] = (uint8_t) g_myWornOutfit;
		memcpy( info + PACKET_HEADER_SIZE + 5 + n + 1, g_myColors, sizeof(g_myColors) );
		int at = PACKET_HEADER_SIZE + 5 + n + 1 + sizeof(g_myColors);
		static_assert( sizeof(MP_VERSION) - 1 <= MAX_VERSION, "version too long" );
		int v = (int) strlen( g_myVersion );
		info[at] = (uint8_t) v;
		memcpy( info + at + 1, g_myVersion, v );
		at += 1 + v;
		LoadHideTime();
		info[at] = (uint8_t) (PlayingHs() ? g_myRole : ROLE_NONE);
		memcpy( info + at + 1, &g_myHideMs, 4 );
		g_transport->Send( info, at + 5 );
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
GET_METHOD( 0x6F3A50, int,  AnimMap_Find,     void*, int, AnimLayer** );
GET_METHOD( 0x61CAD0, void, EAnimController_SetClip, EAnimController*, int, uint32_t );

static const int      MAX_LAYER_SCAN  = 256;

static const uint32_t* RENDER_FRAME   = (const uint32_t*) 0x91779C;   // Global frame counter.

static Matrix4f* GetPose( EAnimController* animCtrl, int* bones )
{
	ERCharacter* skeleton = animCtrl->m_characterResource;
	*bones = skeleton ? skeleton->m_numBones : 0;
	return animCtrl->m_pose;
}

static AnimLayer* GetAnimLayer( EICharacter* c, int key )
{
	AnimLayer* layer = 0;
	TreeMap* map = &c->m_animController.m_animMap;
	if ( !AnimMap_Find(map, key, &layer) ) return 0;
	return layer;
}

static inline uint32_t GetLayerClipCRC( AnimLayer* layer )
{
	ERAnim* anim = layer->m_anim;
	return anim ? anim->m_crc : 0;
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


static ItemStruct* ITEM_TABLE = (ItemStruct*) 0x8EEB90;
static const int NUM_ITEMS = 9;
static const int ITEM_SLING = 0, ITEM_TORCH = 1, ITEM_PICKAXES = 2, ITEM_TNT = 3, ITEM_SHIELD = 4, ITEM_RAFT = 5,
                 ITEM_GAS_MASK = 6, ITEM_CANTEEN = 7, ITEM_STINK_BOMB = 8;

#include "ptle/EItemBase.h"
static inline EItemBase* GetItemObject( int index ) { return ITEM_TABLE[index].m_object; }

static int GetHeldItem( EIHarry* harry )
{
	EInventory* inv = harry->m_inventory;
	if ( !inv ) return -1;

	EItemBase* held = inv->m_itemInHandNext;
	if ( !held ) return -1;

	for ( int i = 0; i < NUM_ITEMS; i++ )
		if ( GetItemObject(i) == held ) return i;
	return -1;
}

static void ApplyMyOutfit();

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

static const PlayerSnapshot* GetPuppetTarget()
{
	if ( !g_cur->target && IsViewerPeer(g_cur) && g_haveLocal ) return &g_local;
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
		const char* dot = name ? strchr(name, '.') : 0;

		if ( dot && strcmp(dot + 1, shortName) == 0 ) return i;
	}
	return -1;
}
static char g_puppetName[] = "evil harry";
static char* const GAME_EMPTY_STRING = (char*) 0x9410B0;   // The game strings' "" (0x6F047A).


static void InitLocomotion( EIEvilHarryP* p, EIHarry* harry );
static int g_puppetsSpawned = 0;   // For the autotest.

static void RememberSpawned( EIEvilHarryP* beast, void* world, int owner );

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

	EIEvilHarryP* p = instantiate_object<EIEvilHarryP>();

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

	AddToWorld( harry->m_world, p );
	ERLevel_SetupEntity( harry->m_world, p, 0 );
	p->Init();
	p->m_beastTypeName = GAME_EMPTY_STRING;

	g_cur->beast = p;
	g_cur->shownOutfit = -1;
	g_puppetsSpawned++;
	g_cur->world = harry->m_world;
	g_cur->level = Gizmod::getCurrentLevelCRC();
	g_cur->lastLoggedState = g_cur->lastRequested = -2;
	RememberSpawned( p, harry->m_world, g_cur->id );
	int count = 0;
	for ( int i = 0; i < MAX_PEERS; i++ ) if ( g_peers[i].beast ) count++;
	mp_log( "[MP] Player %d's puppet spawned (%d puppet(s) in this level).\n", g_cur->id, count );

	// Its default state (21, "get ready for boss fight") draws it as a black
	// silhouette. Start calm and lit instead.
	g_cur->forcedState = FindStateIndex( p, "idle left" );
	mp_log( "[MP] Puppet starts in state %d (idle left).\n", g_cur->forcedState );
	InitLocomotion( p, harry );

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

// Called when Harry's world or the level changes. The old level's puppets usually
// went with it, but the level's CRC changes as soon as a load is announced, while the
// old level still runs a moment : forgetting its puppets then left one standing still
// in the old area, ticked by the game as its own Evil Harry (a second, frozen copy of
// its player next to a fresh puppet; 2026-11-16, going back and forth between Turtle
// Monument and Twin Outposts). So they are only set aside, as when a load is
// announced : one that ticks again is kept, the others are gone after a second
// (DropLostPuppets), and only then is a new one spawned. The pointers are only
// compared, never followed.
static void ForgetPuppet( EIHarry* harry )
{
	int count = 0;
	DWORD now = GetTickCount();
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( p.beast ) p.limbo = p.beast;
		p.beast = 0;
		if ( p.limbo ) { p.limboSince = now; p.limboTicks = 0; count++; }
		p.world = harry->m_world;   // So this change isn't seen again.
		p.level = Gizmod::getCurrentLevelCRC();
	}
	if ( count ) mp_log( "[MP] New level, %d puppet(s) set aside (kept if they're still here).\n", count );
	ResetHistory();
}

// Every puppet spawned in Harry's current world, to catch one nobody drives any more
// (an orphan) : it would stand still, a frozen copy of its player.
struct SpawnedPuppet { EIEvilHarryP* beast; void* world; int owner; bool reported; };
static const int MAX_SPAWNED = 32;
static SpawnedPuppet g_spawned[MAX_SPAWNED];
static int g_spawnedCount = 0;
static volatile LONG g_orphansFound = 0;

static void RememberSpawned( EIEvilHarryP* beast, void* world, int owner )
{
	int keep = 0;   // Other worlds' are kept too : the old level runs on a moment after a loadzone.
	for ( int i = 0; i < g_spawnedCount; i++ ) if ( g_spawned[i].beast != beast ) g_spawned[keep++] = g_spawned[i];
	g_spawnedCount = keep;
	if ( g_spawnedCount == MAX_SPAWNED ) { memmove( g_spawned, g_spawned + 1, (MAX_SPAWNED - 1) * sizeof(SpawnedPuppet) ); g_spawnedCount--; }
	SpawnedPuppet s = { beast, world, owner, false };
	g_spawned[g_spawnedCount++] = s;
}

// The game announces a level load when one loads, but also when Harry dies and
// comes back in the same level (the puppets live on), and sometimes a moment after
// the new level's first ticks (puppets already spawned in it). Forgetting a live
// puppet left it standing there as the game's own Evil Harry, and a second one was
// spawned for its player. So the puppets are only set aside : one that ticks again
// was alive after all (ReclaimPuppet), the others are gone (DropLostPuppets).
static void SuspectPuppets()
{
	int count = 0;
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( !p.beast ) continue;
		p.limbo = p.beast;
		p.beast = 0;
		p.limboSince = GetTickCount();
		p.limboTicks = 0;
		count++;
	}
	if ( count ) mp_log( "[MP] Level load announced : %d puppet(s) set aside.\n", count );
	ResetHistory();
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

static void CheckLevelLeaving( EIHarry* harry )   // Each Harry tick.
{
	static void* world = 0;
	if ( harry->m_world != world ) { g_prevWorld = world; world = harry->m_world; }
	if ( !g_levelLeaving || harry->m_world != g_levelLeavingWorld ) g_tickLevel = Gizmod::getCurrentLevelCRC();
	if ( !g_levelLeaving ) return;
	bool arrived = harry->m_world != g_levelLeavingWorld;
	if ( !arrived && GetTickCount() - g_levelLeavingSince < LEVEL_LEAVING_MAX_MS ) return;
	g_levelLeaving = false;
	mp_log( "[MP] %s : puppets shown again.\n", arrived ? "In the new level" : "Still no new level after 8 s" );
}

static Peer* ReclaimPuppet( EIEvilHarryP* beast )
{
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( p.limbo != beast ) continue;
		p.beast = beast;
		p.limbo = 0;
		p.lastLoggedState = p.lastRequested = -2;
		if ( EIHarry* harry = Gizmod::getHarry() ) { p.world = harry->m_world; p.level = Gizmod::getCurrentLevelCRC(); }
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
static bool g_testStallPuppet = false;   // Autotest : the puppet's ticks are skipped...
static EIEvilHarryP* g_testStalled = 0;  // ...this one's.
static const DWORD PUPPET_STALL_MS = 2000;

static void ReplaceStalledPuppets()   // Each Harry tick.
{
	DWORD now = GetTickCount();
	static DWORD lastCall = 0;
	bool paused = now - lastCall > 500;   // Harry didn't tick either (a pause, a load) : nothing stalled.
	lastCall = now;
	if ( g_levelLeaving ) return;
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( p.beast != p.watched || paused ) { p.watched = p.beast; p.lastTick = now; }
		if ( !p.beast || now - p.lastTick <= PUPPET_STALL_MS ) continue;
		mp_log( "[MP] Player %d's puppet 0x%.8X hasn't ticked for %u ms (a pitfall caught it?) : replaced.\n", i, p.beast, now - p.lastTick );
		p.limbo = p.beast;
		p.beast = 0;
		p.watched = 0;
		p.limboSince = now;
		p.limboTicks = 0;
	}
}

static void DropLostPuppets()   // Each Harry tick.
{
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( !p.limbo || ++p.limboTicks <= 30 || GetTickCount() - p.limboSince <= 1000 ) continue;
		p.limbo = 0;
		mp_log( "[MP] Player %d's puppet went with the old level.\n", i );
	}
}


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
	if ( IsParkedPeer(g_cur) ) { *out = g_parkedPin; return true; }
	if ( IsViewerPeer(g_cur) ) { *out = g_viewerPin; return true; }
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
	if ( !GetPuppetTargetPosition(&pos) ) return;

	if ( g_cur->haveLastTarget ) {
		float dx = pos.x - g_cur->lastTarget.x;
		float dy = pos.y - g_cur->lastTarget.y;
		if ( dx * dx + dy * dy > TELEPORT_DIST * TELEPORT_DIST ) dx = dy = 0.0F;

		g_cur->velX = g_cur->velX * 0.75F + dx * 0.25F;
		g_cur->velY = g_cur->velY * 0.75F + dy * 0.25F;
		g_cur->moveSpeed = sqrtf( g_cur->velX * g_cur->velX + g_cur->velY * g_cur->velY );
		if ( g_cur->moveSpeed > 0.01F ) g_cur->heading = atan2f( g_cur->velY, g_cur->velX );
	}
	// Face where Harry faces, not where he travels : he turns on the spot, walks
	// backwards a step, and in a raft paddles one side and turns freely. His
	// transform's row 1 points opposite to where he faces (see InitLocomotion).
	if ( const PlayerSnapshot* f = GetPuppetTarget() ) {
		float fx = -f->transform.data[1][0], fy = -f->transform.data[1][1];
		if ( fx * fx + fy * fy > 0.01F ) g_cur->heading = atan2f( fy, fx );
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
static const int MIRROR_LAYERS[2] = { 200, 201 };

static bool ApplyMirroredClip( EIEvilHarryP* p )
{
	const PlayerSnapshot* t = GetPuppetTarget();
	uint32_t forced = ForcedClip();
	uint32_t clip = forced ? forced : (t ? t->clipCRC : 0);
	if ( !g_mirrorClips || !clip ) return false;

	int current = p->m_animIndex;
	if ( clip == g_cur->mirrorClip && current == MIRROR_LAYERS[g_cur->mirrorSlot] ) return true;

	EAnimController* ctrl = &p->m_animController;
	int next = (current == MIRROR_LAYERS[g_cur->mirrorSlot]) ? 1 - g_cur->mirrorSlot : g_cur->mirrorSlot;
	EAnimController_SetClip( ctrl, MIRROR_LAYERS[next], clip );
	PlayAnimLayer( p, MIRROR_LAYERS[next] );
	g_cur->mirrorSlot = next;
	g_cur->mirrorClip = clip;
	return true;
}

static void ApplyLocomotionAnim( EIEvilHarryP* p )
{
	static int overridden = 0;
	if ( ApplyMirroredClip(p) ) return;

	int wanted = g_cur->idleLayer;
	if ( g_cur->airborne && g_cur->airLayer >= 0 ) wanted = g_cur->airLayer;
	else if ( g_cur->moving ) wanted = (g_cur->sprinting && g_cur->sprintLayer >= 0) ? g_cur->sprintLayer : g_cur->runLayer;
	int current = p->m_animIndex;
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
		int base = p->m_locoBase;
		g_cur->runLayer = p->m_animMoveCRC ? base : -1;
		mp_log( "[MP] Run animation : quickdata move anim, layer %d.\n", g_cur->runLayer );
	}
	else {
		g_cur->runLayer = GetStateAnimLayer( p, FindStateIndex(p, stateName) );
		mp_log( "[MP] Run animation : \"%s\" state's anim, layer %d.\n", stateName, g_cur->runLayer );
	}
}

// Standing animation. 0 = the beast's quickdata idle (locomotion layer +1); the
// "idle left/right" states' animation is the wall-climbing idle (pickaxes out).
static void SetIdleAnimation( EIEvilHarryP* p, const char* stateName )
{
	if ( !stateName ) {
		int base = p->m_locoBase;
		g_cur->idleLayer = p->m_animIdleCRC ? base + 1 : -1;
		mp_log( "[MP] Idle animation : quickdata idle anim, layer %d.\n", g_cur->idleLayer );
	}
	else {
		g_cur->idleLayer = GetStateAnimLayer( p, FindStateIndex(p, stateName) );
		mp_log( "[MP] Idle animation : \"%s\" state's anim, layer %d.\n", stateName, g_cur->idleLayer );
	}
}

static void SetAirAnimation( EIEvilHarryP* p, const char* stateName )
{
	g_cur->airLayer = GetStateAnimLayer( p, FindStateIndex(p, stateName) );
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
		p->m_animMoveCRC, p->m_animIdleCRC, p->m_locoBase, p->m_animIndex,
		g_cur->idleLayer, GetStateAnimLayer(p, FindStateIndex(p, "runner")) );

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

static inline bool IsBallMode( const PlayerSnapshot* t )
{
	return t && t->transform.data[3][2] - t->position.z > BALL_RAISE_MIN;
}

static void SetBeastWorldMatrix( EIEvilHarryP* p, const Vector3f& pos, const PlayerSnapshot* t )
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
	if ( GetPuppetTargetPosition(&pos) ) {
		p->m_position = pos;
		SetBeastWorldMatrix( p, pos, GetPuppetTarget() );
		p->m_previousPosition = pos;
	}
}

// Body collision. The beast's "collision off" routine (0x418840, used when it dies
// or goes limp) clears the byte at +0x4C4 and its "on" routine (0x418910) sets it
// back; with it at 0 Harry walks straight through. (The collision layer masks at
// +0x230/+0x564/+0x574/+0x584 made no difference, cleared or not.)
// The puppet is only a visual, so it is never solid.
static bool g_puppetSolid = false;                 // Autotest flips it to compare.

static inline void ApplyPuppetCollision( EIEvilHarryP* p )
{
	p->m_enableCollision = g_puppetSolid;
}

// EInstance flags (offset 0x2C).
static const uint32_t INSTANCE_VISIBLE = 0x1;

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
static int g_puppetPoseCopies = 0;   // For the autotest.

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
	{ ITEM_TORCH,      0x89A200, 0,    0     },
	{ ITEM_TNT,        0x89A160, 0,    0     },
	{ ITEM_RAFT,       0x8949B0, 0xA4, 0x108 },
	{ ITEM_SLING,      0x8971D0, 0xA0, 0x104 },
	{ ITEM_SHIELD,     0x896F90, 0xA4, 0x108 },
	{ ITEM_GAS_MASK,   0x87CAD8, 0,    0     },
	{ ITEM_CANTEEN,    0x86DCB0, 0,    0     },
	{ ITEM_STINK_BOMB, 0x8994B8, 0,    0     },
	{ ITEM_PICKAXES,   0x88ED70, 0,    0     },
};
static const int NUM_DRAWN_ITEMS = sizeof(DRAWN_ITEMS) / sizeof(DRAWN_ITEMS[0]);
static ItemDrawFunc g_origItemDraw[NUM_DRAWN_ITEMS];

static int g_puppetItemDraws = 0;     // For the autotest.
static int g_aimItemDraws = 0;
static bool g_testSkipHarryItemDraw = false;   // Autotest : Harry's item isn't drawn (as with the camera in his head).
static int g_copyDrawsAlone = 0;      // For the autotest : copies drawn while Harry draws no item.
static DWORD g_harryItemDrawTime = 0;

static void NoteHarryItemDraw( int slot, EItemBase* item )
{
	EIHarry* owner = item->m_harry;
	PlayerSnapshot* s = LatestSnapshot();
	if ( !owner || owner != Gizmod::getHarry() || !s ) return;
	// Until it has inflated, the raft's draw only poses it (something else shows it).
	if ( DRAWN_ITEMS[slot].item == ITEM_RAFT && !((EItemRaft*) item)->m_inflated ) return;
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
	bool hands = index == ITEM_PICKAXES;   // They draw on Harry's hands.
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
static void DrawPuppetItem( int slot, void* context, void* arg )
{
	if ( !g_cur->beast ) return;
	const PlayerSnapshot* t = GetPuppetTarget();
	if ( !t || t->drawnSlot != slot ) return;
	DrawItemAt( slot, t, g_cur->beast->m_transformMatrix, context, arg );
}

// The copies are drawn by the puppets' render (the raft's too : it used to be drawn
// right after Harry's own raft, so the other players' rafts vanished whenever Harry
// put his away).
#define ITEM_DRAW_HOOK( n ) \
	static void __fastcall ItemDraw_Hook##n( EItemBase* item, void*, void* context, void* arg ) \
	{ \
		if ( g_testSkipHarryItemDraw && item->m_harry == Gizmod::getHarry() ) return; \
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
static const uint32_t RENDERER_DEVICE = 0x926790 + 0x74;
static const float TAG_ABOVE_FEET = 5.6F;   // The top of Harry's head (measured on screenshots).
static uint32_t g_presentCount = 1;

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
static const uint32_t CAMERA_LOOKAT_CALLS[] = {
	0x449B9A, 0x449BC4, 0x456169, 0x457065, 0x457783, 0x457A58, 0x4582BE, 0x4584E7, 0x459A08, 0x45AC65,
	0x45B30C, 0x45B6E7, 0x45B774, 0x45C9F2, 0x45D6B1, 0x45D956, 0x45E46D, 0x45E7E0, 0x460815, 0x4609BA,
	0x460BDA, 0x461AF0, 0x461EE7, 0x462720, 0x4645D4, 0x465106, 0x4657AF, 0x466054, 0x46736F, 0x467C7D,
	0x677851, 0x677B58,
};
static const int NUM_CAMERA_LOOKAT_CALLS = sizeof(CAMERA_LOOKAT_CALLS) / sizeof(CAMERA_LOOKAT_CALLS[0]);
static const LookAtFunc CameraLookAt = (LookAtFunc) 0x6EE560;
static Matrix4f g_cameraWorld;
static bool g_haveCamera = false;
static uint32_t g_cameraFrame = 0;          // g_presentCount when g_cameraWorld was last set.
static uint32_t g_cameraSite = 0;           // The call that built it.
static Matrix4f* g_mainCamera = 0;          // What the usual call (0x449B9A) writes.
static uint32_t g_mainCameraFrame = 0;
static int g_cameraSiteCount[NUM_CAMERA_LOOKAT_CALLS];

// Diagnostics (name tags near water) : the look-ats of the frame, and where the
// puppet was drawn. Reset by the survey; logged per level.
struct TagDiag
{
	int frames, lookAts, maxLookAts;
	int drawsOnScreen, drawsElsewhere, behind, noted;
	UINT elseW, elseH, screenW, screenH;
	int lookAtsThisFrame;
	Vector3f eyes[4];                 // This frame's look-ats (eye positions).
	int sampleLogged;
};
static TagDiag g_tagDiag;
static bool g_tagDiagOn = false;

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
	g_cameraFrame = g_presentCount;
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
static Matrix4f g_rendCam;
static uint32_t g_rendFrame = 0, g_rendSets = 0, g_rendSetsThisFrame = 0, g_rendSetsMax = 0;
static void* g_rendSelf = 0;
static Vector3f g_rendEyes[4];
static int g_rendTwiceLogged = 0;
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

static int g_tagPasses[2] = { 0, 0 };   // Puppet draws elsewhere than to the screen, and to the screen (diagnostics).

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

// Where a puppet's tag goes on the screen now : the camera of this frame, the
// puppet where it is. The game doesn't draw a puppet in every frame where it's seen
// through a wall (St. Claire's camp at night : one frame in two or three) : a tag
// only shown on frames it was drawn flickered there. It stays up for a few frames
// after the last draw, placed again each frame. False : not shown.
static const uint32_t TAG_GRACE_FRAMES = 12;

static bool PlaceNameTag( const Peer& peer, IDirect3DDevice9* dev, float* x, float* y, float* depth )
{
	EIEvilHarryP* p = peer.beast;
	if ( !p || !peer.tagFrame || g_presentCount - peer.tagFrame > TAG_GRACE_FRAMES || g_levelLeaving ) return false;
	// The view the renderer drew this frame with (the last it was given : any
	// reflection comes first), whichever camera mode built it. Choosing among the
	// cameras' own look-ats instead left the tag behind when aiming the TNT, and
	// jumping about while the camera swung into it.
	if ( !g_haveProj || g_presentCount - g_rendFrame > 1 ) return false;
	Matrix4f view;
	if ( !MatInverseAffine( g_rendCam, &view ) ) return false;
	IDirect3DSurface9* screen = 0;
	D3DSURFACE_DESC d = { };
	bool ok = SUCCEEDED( dev->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &screen ) ) && screen && SUCCEEDED( screen->GetDesc( &d ) );
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

static bool IsPuppet( EIEvilHarryP* beast )
{
	for ( int i = 0; i < MAX_PEERS; i++ ) if ( g_peers[i].beast == beast || g_peers[i].limbo == beast ) return true;
	return false;
}

static void __stdcall _EIEvilHarryP_Render_Hook( EIEvilHarryP* self, void* context, void* arg )
{
	Peer* peer = FindPeer( self );
	if ( !peer ) {
		// One of our puppets nobody drives now (set aside, or left over) : the game
		// drew it as its own Evil Harry, a frozen copy of its player. Not drawn.
		// (Only this world's and the last one's : the game's own Evil Harry may reuse
		// an old puppet's address in a level long gone.)
		int spawned = -1;
		EIHarry* harry = Gizmod::getHarry();
		for ( int i = 0; i < g_spawnedCount; i++ )
			if ( g_spawned[i].beast == self && (g_spawned[i].world == harry->m_world || g_spawned[i].world == g_prevWorld) ) spawned = i;

		bool setAside = IsPuppet( self );
		if ( spawned >= 0 || setAside ) {
			static void* logged[8];
			static int numLogged = 0;
			bool seen = false;
			for ( int i = 0; i < numLogged; i++ ) if ( logged[i] == self ) seen = true;
			if ( !seen ) {
				logged[numLogged++ % 8] = self;
				if ( numLogged > 8 ) numLogged = 8;
				mp_log( "[MP] Puppet 0x%.8X (%s, player %d) drawn with nobody driving it, at (%.1f, %.1f, %.1f) : hidden.\n", self,
					setAside ? "set aside" : (g_spawned[spawned].world == harry->m_world ? "left over, this world" : "left over, another world"),
					spawned >= 0 ? g_spawned[spawned].owner : -1, self->EIBeast::m_position.x, self->EIBeast::m_position.y, self->EIBeast::m_position.z );
			}
			return;
		}

		EIBeast_Render( self, context, arg );
		return;
	}

	CurrentPeer current( peer );
	if ( IsParkedPeer(peer) ) return;   // The emote preview's, between previews.

	const PlayerSnapshot* t = GetPuppetTarget();
	// Online with nobody to show here (they left, or are in another level) : not drawn.
	if ( g_networked && !t ) return;
	if ( g_levelLeaving ) return;   // The old level, still shown (NoteLevelLeaving).
	bool viewer = IsViewerPeer( peer );   // No pose copy, name tag or items : just the clip.
	if ( t && !ForcedClip() ) ApplyPuppetPose( self, t );

	EIBeast_Render( self, context, arg );

	if ( t && !viewer ) NoteNameTag( peer, self );
	if ( t && !viewer && t->drawnSlot >= 0 ) DrawPuppetItem( t->drawnSlot, context, arg );
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

// Pitfalls (EIPitMonster) eat any beast that touches their mouth : the beast is put to sleep and the pit
// stays shut on it. A puppet caught there kept the pit shut on the other players' screens.
// The pit's two contact handlers now let puppets through.
#include "ptle/EIPitMonster.h"
static const uint32_t PIT_CONTACT_SLOTS[2] = { 0x890444, 0x890448 };

static volatile LONG g_pitSkips = 0;

GET_METHOD( 0x542DD0, bool, EIPitMonster_Contact4, EIPitMonster*, OverlapStruct* c, int a, int b, int d );
GET_METHOD( 0x542FD0, bool, EIPitMonster_Contact5, EIPitMonster*, OverlapStruct* c, int a, int b, int d, int e );

static bool IsAnyPuppet( EInstance* beast )
{
	if ( beast->GetType() != get_etypeinfo<EIEvilHarryP>() ) return false;
	if ( IsPuppet((EIEvilHarryP*) beast) ) return true;
	for ( int i = 0; i < g_spawnedCount; i++ ) if ( g_spawned[i].beast == beast ) return true;
	return false;
}

static bool PitSkips( OverlapStruct* c )
{
	if ( !c || !IsAnyPuppet(c->m_other) ) return false;
	if ( InterlockedIncrement(&g_pitSkips) <= 3 ) mp_log( "[MP] A pitfall touched a puppet (0x%.8X) : not eaten.\n", c->m_other );
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

static volatile LONG g_fadesStopped = 0;
static float g_lastPuppetFade = 1.0F;   // What the game wanted for a puppet (tests).
static volatile LONG g_puppetFadeCalls = 0;

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

static void InstallPitHooks()
{
	injector::WriteMemory( PIT_CONTACT_SLOTS[0], PitContact4_Hook );
	injector::WriteMemory( PIT_CONTACT_SLOTS[1], PitContact5_Hook );
}

static void DrivePuppet( EIEvilHarryP* p )
{
	ApplyPuppetCollision( p );

	Vector3f pos;
	if ( !GetPuppetTargetPosition(&pos) ) {
		ApplyPuppetItem( p );
		return;
	}

	const PlayerSnapshot* t = GetPuppetTarget();
	ApplyPuppetItem( p );
	Vector3f rot = t->rotationEuler;

	p->m_position = pos;
	EICharacter_SetPos( p, &pos, true );
	EICharacter_SetRot( p, &rot );

	SetBeastWorldMatrix( p, pos, t );

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
// Hotkeys.
// --------------------------------------------------------------------------------
// Keys are polled on our own thread (independent of the game's tick rate and of
// which window the game thinks it owns), and turned into requests the game
// thread consumes in HandleHotkeys().
static volatile LONG g_reqSpawn = 0;
static volatile LONG g_reqState = 0;
static volatile LONG g_reqDelay = 0;
static volatile LONG g_reqStay = 0;
static volatile LONG g_reqMenu = 0;

// The menu (F1) and what it sets. Display settings are kept in the ini ([Display]).
static Menu g_menu;
static bool g_showTags = true;    // Names above the other players.
// Name tags fade out with the camera distance, between these two (the menu's
// sliders; Multiplayer.ini : Display, TagFadeFrom and TagFadeTo).
static const int TAG_FADE_MAX = 1000, TAG_FADE_STEP = 10;
static int g_tagFadeFrom = 100, g_tagFadeTo = 200;
static bool g_showList = false;   // The player list stays on screen while playing.
static char g_iniPath[MAX_PATH] = { 0, };
enum { MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT, MK_ACCEPT, MK_BACK, MK_NEXT, MK_PREV, MK_DELETE, MK_PGUP, MK_PGDN, MK_SLEFT, MK_SRIGHT, MK_COUNT };
static volatile LONG g_menuKeys[MK_COUNT];   // Presses, from the hotkey thread.
static volatile LONG g_menuOpen = 0;         // For the hotkey thread and the keyboard poll.
static volatile DWORD g_menuBlockUntil = 0;  // Keys stay away from the game a moment after it closes (the Esc that closed it).
static struct { bool active; float x, y; bool down; } g_testMouse;   // Autotest : stands in for the mouse.

// Typing into the menu (a colour) : characters from the hotkey thread.
static SRWLOCK g_typedLock = SRWLOCK_INIT;
static char g_typedChars[64];
static volatile LONG g_menuTyping = 0, g_menuPaste = 0, g_menuCopy = 0;
static char g_testPaste[128];      // Autotest : stands in for the clipboard.
static DWORD g_lastCopied = 0;

static void AddTyped( char c )
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

static bool ReadClipboard( char* out, size_t size )
{
	out[0] = 0;
	if ( !OpenClipboard( 0 ) ) return false;
	HANDLE h = GetClipboardData( CF_TEXT );
	const char* text = h ? (const char*) GlobalLock( h ) : 0;
	if ( text ) { strncpy_s( out, size, text, _TRUNCATE ); GlobalUnlock( h ); }
	CloseClipboard();
	return out[0] != 0;
}

static void WriteClipboard( const char* text )
{
	size_t n = strlen( text ) + 1;
	HGLOBAL h = GlobalAlloc( GMEM_MOVEABLE, n );
	if ( !h ) return;
	memcpy( GlobalLock( h ), text, n );
	GlobalUnlock( h );
	if ( OpenClipboard( 0 ) ) {
		EmptyClipboard();
		if ( !SetClipboardData( CF_TEXT, h ) ) GlobalFree( h );
		CloseClipboard();
	}
	else GlobalFree( h );
}

// The keys that type : digits, letters (Shift : capitals), space, , . - ' # ( ) !, backspace.
static void PollTyping( bool active )
{
	static bool was[256] = { false, };
	bool ctrl = (GetAsyncKeyState( VK_CONTROL ) & 0x8000) != 0, shift = (GetAsyncKeyState( VK_SHIFT ) & 0x8000) != 0;
	for ( int vk = 0x08; vk < 0xE0; vk++ ) {
		bool typing = (vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z') || (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
			|| vk == VK_SPACE || vk == VK_OEM_COMMA || vk == VK_OEM_PERIOD || vk == VK_OEM_MINUS || vk == VK_OEM_7 || vk == VK_BACK;
		if ( !typing ) continue;
		bool down = active && (GetAsyncKeyState( vk ) & 0x8000) != 0;
		if ( down && !was[vk] ) {
			if ( ctrl ) {
				if ( vk == 'V' ) InterlockedIncrement( &g_menuPaste );
				if ( vk == 'C' ) InterlockedIncrement( &g_menuCopy );
			}
			else if ( g_menuTyping ) {
				char c = 0;
				if ( vk == VK_BACK ) c = '\b';
				else if ( vk == VK_SPACE ) c = ' ';
				else if ( vk == VK_OEM_COMMA ) c = ',';
				else if ( vk == VK_OEM_PERIOD ) c = '.';
				else if ( vk == VK_OEM_MINUS ) c = '-';
				else if ( vk == VK_OEM_7 ) c = '\'';
				else if ( vk == '1' && shift ) c = '!';
				else if ( vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9 ) c = (char) ('0' + vk - VK_NUMPAD0);
				else if ( vk == '3' && shift ) c = '#';
				else if ( vk == '9' && shift ) c = '(';
				else if ( vk == '0' && shift ) c = ')';
				else if ( vk >= 'A' && vk <= 'Z' ) c = (char) (vk - 'A' + (shift ? 'A' : 'a'));
				else c = (char) vk;
				AddTyped( c );
			}
		}
		was[vk] = down;
	}
}

static bool MenuBlocksInput()
{
	return g_menuOpen || (int) (g_menuBlockUntil - GetTickCount()) > 0;
}
static volatile LONG g_reqViewer = 0, g_reqStar = 0, g_reqPrev = 0, g_reqNext = 0, g_reqPrev10 = 0, g_reqNext10 = 0,
                     g_reqReplay = 0, g_reqStarredOnly = 0;
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
	// The animation viewer's keys ([ ] \ ') only matter while it's open.
	static const int NUM_KEYS = 11;
	bool wasDown[NUM_KEYS] = { false, };
	const int keys[NUM_KEYS] = { VK_F5, VK_F6, VK_F7, VK_F8, VK_F1, VK_F10, VK_F11, VK_OEM_4, VK_OEM_6, VK_OEM_5, VK_OEM_7 };
	const char* names[NUM_KEYS] = { "F5", "F6", "F7", "F8", "F1", "F10", "F11", "[", "]", "\\", "'" };
	volatile LONG* reqs[NUM_KEYS] = { &g_reqSpawn, &g_reqState, &g_reqDelay, &g_reqStay, &g_reqMenu,
		&g_reqViewer, &g_reqStar, &g_reqPrev, &g_reqNext, &g_reqReplay, &g_reqStarredOnly };

	for ( ;; ) {
		for ( int i = 0; i < NUM_KEYS; i++ ) {
			bool down = (GetAsyncKeyState( keys[i] ) & 0x8000) != 0;
			if ( down && !wasDown[i] ) {
				if ( GameHasFocus() ) {
					volatile LONG* req = reqs[i];
					bool shift = (GetAsyncKeyState( VK_SHIFT ) & 0x8000) != 0;   // Shift + [ ] : by ten.
					if ( shift && req == &g_reqPrev ) req = &g_reqPrev10;
					if ( shift && req == &g_reqNext ) req = &g_reqNext10;
					InterlockedIncrement( req );
					if ( i <= 6 ) mp_log( "[MP] %s pressed (Harry ticks so far : %ld).\n", names[i], g_harryTicks );
				}
				else if ( i <= 6 ) {
					mp_log( "[MP] %s ignored : game window not focused.\n", names[i] );
				}
			}
			wasDown[i] = down;
		}

		// The menu's keys, while it's open. Held arrows (and Page Up / Down) repeat; with
		// Shift, Tab goes back and Left / Right are the Shift ones.
		static const int NUM_MENU_VK = 10;
		static const int MENU_VK[NUM_MENU_VK] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, VK_RETURN, VK_ESCAPE, VK_TAB, VK_DELETE, VK_PRIOR, VK_NEXT };
		static const int MENU_SLOT[NUM_MENU_VK] = { MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT, MK_ACCEPT, MK_BACK, MK_NEXT, MK_DELETE, MK_PGUP, MK_PGDN };
		static bool menuDown[NUM_MENU_VK] = { false, };
		static DWORD menuNext[NUM_MENU_VK];
		bool menu = g_menuOpen && GameHasFocus();
		bool shift = (GetAsyncKeyState( VK_SHIFT ) & 0x8000) != 0;
		for ( int i = 0; i < NUM_MENU_VK; i++ ) {
			bool down = menu && (GetAsyncKeyState( MENU_VK[i] ) & 0x8000) != 0;
			DWORD now = GetTickCount();
			bool repeats = i < 4 || i >= 8;
			if ( down && (!menuDown[i] || (repeats && (int) (now - menuNext[i]) >= 0)) ) {
				int slot = MENU_SLOT[i];
				if ( shift && slot == MK_NEXT ) slot = MK_PREV;
				if ( shift && slot == MK_LEFT ) slot = MK_SLEFT;
				if ( shift && slot == MK_RIGHT ) slot = MK_SRIGHT;
				InterlockedIncrement( &g_menuKeys[slot] );
				menuNext[i] = now + (menuDown[i] ? 70 : 350);
			}
			menuDown[i] = down;
		}
		PollTyping( menu );
		Sleep( 15 );
	}
	return 0;
}

// --------------------------------------------------------------------------------
// Animation viewer (F10). Its clip list and notes are in
// %LOCALAPPDATA%\PTLE Multiplayer\anim-notes.txt, one clip per line :
//     <number> <clip id> <where found> [*] [notes]
// The numbers are the animation survey's. Edited by hand (reloaded when it changes)
// and by F11 (star). Clips found on Harry and not in it yet are added.
// --------------------------------------------------------------------------------
struct AnimNote
{
	int num;
	uint32_t crc;
	char source[24];
	bool star;
	char note[200];
};
static const int MAX_ANIM_NOTES = 1024;
static AnimNote g_animNotes[MAX_ANIM_NOTES];
static int g_animNoteCount = 0;
static int g_animIndex = 0;
static bool g_animStarredOnly = false;
static char g_animNotesPath[MAX_PATH];
static FILETIME g_animNotesTime;
static uint32_t g_animNotesChecked = 0;

static bool AnimNotesFileTime( FILETIME* t )
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	if ( !GetFileAttributesExA( g_animNotesPath, GetFileExInfoStandard, &a ) ) return false;
	*t = a.ftLastWriteTime;
	return true;
}

static void LoadAnimNotes()
{
	if ( !g_animNotesPath[0] ) {
		if ( !GetDataDir( g_animNotesPath ) ) return;
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
		if ( sscanf_s( line, "%d %x %23s %n", &n.num, &n.crc, n.source, (unsigned) sizeof(n.source), &used ) < 3 ) continue;
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
	for ( int i = 0; i < g_animNoteCount; i++ ) if ( g_animNotes[i].crc == keep ) g_animIndex = i;
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
	g_animSurveyClip = g_animNotes[g_animIndex].crc;
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

static void CloseAnimViewer( const char* why )
{
	if ( !g_animViewer ) return;
	g_animViewer = false;
	g_animSurveyClip = 0;
	if ( g_viewerPeer ) g_viewerPeer->mirrorClip = 0;
	g_viewerPeer = 0;
	mp_log( "[MP] Animation viewer closed (%s).\n", why );
}

static void OpenAnimViewer( EIHarry* harry )
{
	if ( g_emotePreview ) { Toast( "Close the menu first" ); return; }
	// Its puppet : offline the main one (it replays Harry); online one of a free
	// player slot, so other players' puppets are left alone.
	Peer* vp = MainPeer();
	if ( g_networked ) {
		int me = g_transport ? g_transport->PlayerId() : -1;
		vp = 0;
		for ( int i = MAX_PEERS - 1; i >= 0 && !vp; i-- )
			if ( i != me && !g_players[i].present && !g_peers[i].heard && !g_peers[i].target ) vp = &g_peers[i];
		if ( !vp ) { Toast( "No room for the animation viewer (8 players)" ); return; }
	}
	g_viewerPeer = vp;
	g_animViewer = true;
	{
		CurrentPeer current( vp );
		if ( !vp->beast ) SpawnPuppet( harry );
	}
	if ( !vp->beast ) { CloseAnimViewer( "no puppet" ); Toast( "Evil Harry can't be placed here" ); return; }

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
	if ( !g_animNoteCount ) { CloseAnimViewer( "no clips" ); Toast( "No animations found" ); return; }
	if ( g_animIndex >= g_animNoteCount ) g_animIndex = 0;
	ShowAnimClip( true );
	mp_log( "[MP] Animation viewer open : puppet of slot %d, %d clips.\n", vp->id, g_animNoteCount );
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
	if ( !g_viewerPeer || !g_viewerPeer->beast ) { CloseAnimViewer( "puppet gone" ); return; }

	uint32_t now = NetNowMs();
	if ( now - g_animNotesChecked >= 1000 ) {   // Edited by hand ?
		g_animNotesChecked = now;
		FILETIME t;
		if ( AnimNotesFileTime( &t ) && CompareFileTime( &t, &g_animNotesTime ) != 0 ) {
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
		if ( AnimNotesFileTime( &t ) && CompareFileTime( &t, &g_animNotesTime ) != 0 ) LoadAnimNotes();
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
struct EmoteDef
{
	const char* id;        // Chains : their name in the ini (the others go by their clip).
	const char* name;
	int category;          // EMOTE_CATEGORIES.
	bool loops;            // Plays again until Harry moves.
	bool favourite;
	uint32_t intro;        // Chains : played once first, then the clip loops.
	uint32_t clip;
	uint32_t ending[2];    // Played (one, then the other) when you move : a chain's, or getting up from a hold.
	float hold[2];         // Harry stays between these frames of the clip (there and back, slowing at each end; the
	                       // same : on that frame; -1 : the last) until you move. 0, 0 : no hold.
	float endingFrom;      // The ending's first frame.
};
#include "emotes_data.h"

static const int MAX_WHEELS = 4, WHEEL_SLOTS = 8, WHEEL_NAME_SIZE = 24;
struct EmoteWheel
{
	char name[WHEEL_NAME_SIZE];
	int slot[WHEEL_SLOTS];   // EMOTES index, -1 : empty. Clockwise from the top.
};
static EmoteWheel g_wheels[MAX_WHEELS];
static int g_wheelCount = 0;
static int g_emoteButton = 4;        // DirectInput button held for the wheel : L1 / LB.
static bool g_emoteNeedsR1 = false;  // ... with R1 / RB too.
static int g_emoteKey = 0x14;        // DirectInput key code : T.
static int g_wheelIndex = 0;         // The wheel up in the game (kept between openings).

static int FindEmoteByName( const char* name )
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
static void DefaultWheel( int w )
{
	static const char* const PARTY[WHEEL_SLOTS] = { "Victory Dance", "Jig", "Running Man", "Breakdance", "Pickaxe Spin", "Teleport Zen", "Scratch Butt", "T-Pose" };
	static const char* const SILLY[WHEEL_SLOTS] = { "Bug Buzz", "Dizzy Walk", "Poison Gas Walk", "Death", "Death 2", "Breakdance Finish", 0, 0 };
	EmoteWheel& wheel = g_wheels[w];
	if ( w == 0 ) strcpy_s( wheel.name, "Party" );
	else if ( w == 1 ) strcpy_s( wheel.name, "Silly" );
	else sprintf_s( wheel.name, "Wheel %d", w + 1 );
	for ( int s = 0; s < WHEEL_SLOTS; s++ ) wheel.slot[s] = w == 0 ? FindEmoteByName( PARTY[s] ) : w == 1 ? FindEmoteByName( SILLY[s] ) : -1;
}

// [Emotes] Wheels=N, WheelK=Name|slot,slot,... (each : EmoteCode).
static void LoadWheels( const char* ini )
{
	for ( int w = 0; w < MAX_WHEELS; w++ ) DefaultWheel( w );
	g_wheelCount = GetPrivateProfileIntA( "Emotes", "Wheels", 0, ini );
	if ( g_wheelCount <= 0 || g_wheelCount > MAX_WHEELS ) { g_wheelCount = 2; return; }
	for ( int w = 0; w < g_wheelCount; w++ ) {
		char key[16], line[512];
		sprintf_s( key, "Wheel%d", w + 1 );
		if ( !GetPrivateProfileStringA( "Emotes", key, "", line, sizeof(line), ini ) ) continue;
		char* bar = strchr( line, '|' );
		if ( !bar ) continue;
		*bar = 0;
		strncpy_s( g_wheels[w].name, line, _TRUNCATE );
		char* next = bar + 1;
		for ( int s = 0; s < WHEEL_SLOTS; s++ ) {
			char* comma = next ? strchr( next, ',' ) : 0;
			if ( comma ) *comma = 0;
			g_wheels[w].slot[s] = next ? FindEmoteByCode( next ) : -1;
			next = comma ? comma + 1 : 0;
		}
	}
}

static void SaveWheels( const char* ini )
{
	char n[16], key[16], line[512];
	sprintf_s( n, "%d", g_wheelCount );
	WritePrivateProfileStringA( "Emotes", "Wheels", n, ini );
	for ( int w = 0; w < MAX_WHEELS; w++ ) {
		sprintf_s( key, "Wheel%d", w + 1 );
		if ( w >= g_wheelCount ) { WritePrivateProfileStringA( "Emotes", key, 0, ini ); continue; }
		strcpy_s( line, g_wheels[w].name );
		for ( char* c = line; *c; c++ ) if ( *c == '|' ) *c = '/';
		strcat_s( line, "|" );
		for ( int s = 0; s < WHEEL_SLOTS; s++ ) {
			char code[32];
			EmoteCode( g_wheels[w].slot[s], code, sizeof(code) );
			if ( s ) strcat_s( line, "," );
			strcat_s( line, code );
		}
		WritePrivateProfileStringA( "Emotes", key, line, ini );
	}
}

static const int EMOTE_LAYERS[2] = { 254, 255 };   // Spare layers of Harry's (his own go up to 253), taking turns.
// More of a layer (EAnimController, 0x619300 moves them on) :
static const uint32_t LAYER_TARGET = 0x60;          // float : the weight the game set it to (or is fading it to).

static const uint32_t CLIP_FRAMES = 0x14;           // int, in the clip (layer +0x78) : its frames (the last is this - 1).

// Not named in AnimLayer / ERAnim yet. The frames are the ones the game's layer update
// counts (0x619396 reads the clip's +0x14), not ERAnim::m_framesCount (+0x20).
static inline float& LayerTarget( AnimLayer* layer ) { return FIELD(layer, float, LAYER_TARGET); }
static inline int ClipFrames( const ERAnim* clip ) { return FIELD(clip, int, CLIP_FRAMES); }
GET_METHOD( 0x61E4F0, void, AnimCtrl_SetTicked, EAnimController*, void*, bool );
static const float EMOTE_MOVE = 0.5F;               // Harry got moved this far from where he started : over.
static const uint32_t EMOTE_FADE_MS = 200;          // Over : blended back into the game's own animation this long.
static const uint32_t EMOTE_SKIP_FADE_MS = 120;     // A chain's ending skipped.
static const uint32_t EMOTE_ENDING_BLEND_MS = 250;  // Into an ending that starts partway (lying there, then getting up).

struct EmoteState
{
	bool active;
	int emote;            // EMOTES index.
	uint32_t clip;        // Playing now (a chain's intro, then its clip, then its ending).
	bool loop;
	int slot;             // Which of EMOTE_LAYERS.
	bool started;
	int ending;           // Its ending : which of its clips plays (-1 : not yet). Harry stays put meanwhile.
	bool holding;         // At its hold (Teleport Zen floating, Death lying there) ...
	uint32_t holdSince;   // ... since then.
	bool finishing;       // Let go of its hold, the rest of the clip playing (no ending). Harry stays put meanwhile.
	float startFrame;     // The clip begins here.
	int blendFrom;        // Layer blended out as the clip comes in (-1 : none) ...
	uint32_t blendSince, blendMs;   // ... from then, this long.
	float lastFrame;      // The layer's frame last time (a loop's goes back to the start : another play).
	int plays;
	uint32_t start;       // NetNowMs.
	Vector3f where;       // Harry, at the start.
	int health;
};
static EmoteState g_emote;
static struct { bool on; int layer; uint32_t since, ms; } g_emoteFade;   // Over, blending out.
static int g_emoteRequest = -1;           // EMOTES index : the wheel's choice, played on Harry's next tick.
static const char* g_emoteCancel = 0;     // Set by the input hooks (a button, a key) : over, on the next tick.
static const char* g_emoteLastEnd = "";   // Why the last one ended (tests).
static const char* g_emoteRefused = "";   // Why the last one didn't start (tests).
static Vector3f g_stillAt;                // Harry standing still : where ...
static uint32_t g_stillSince = 0;         // ... and since when.

// A chain's ending playing : the input hooks keep Harry where he is.
static bool EmoteHoldsHarry()
{
	return g_emote.active && (g_emote.ending >= 0 || g_emote.finishing);
}

static float ClipSecondsLeft( AnimLayer* layer )
{
	ERAnim* clip = layer->m_anim;
	float rate = layer->m_fps * layer->m_speed;
	if ( !clip ) return 0.0F;
	if ( rate <= 0.0F ) return 1e6F;   // Stopped : not getting to its end.
	return ((float) (ClipFrames(clip) - 1) - layer->m_frame) / rate;
}

// Ours, whatever the game does : starting one of Harry's idles, it fades every other
// layer out (ours too) and drops them.
static void KeepLayer( EAnimController* ctrl, AnimLayer* layer )
{
	layer->m_fading = 0;
	LayerTarget( layer ) = 1.0F;
	if ( !layer->m_ticked ) AnimCtrl_SetTicked( ctrl, layer, true );
}

static void DropLayer( EIHarry* harry, int key )
{
	AnimLayer* layer = GetAnimLayer( harry, key );
	if ( !layer ) return;
	layer->m_weight = 0.0F;
	LayerTarget( layer ) = 0.0F;
	layer->m_fading = 0;
	AnimCtrl_SetTicked( &harry->m_animController, layer, false );
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
		else layer->m_weight = LayerTarget(layer) * (1.0F - w - w2);
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
static void BeginClip( uint32_t clip, bool loop, float from = 0.0F, uint32_t blendMs = 0 )
{
	g_emote.clip = clip;
	g_emote.loop = loop;
	g_emote.blendFrom = (blendMs && g_emote.started) ? EMOTE_LAYERS[g_emote.slot] : -1;
	g_emote.blendMs = blendMs;
	g_emote.slot = 1 - g_emote.slot;
	g_emote.started = false;
	g_emote.startFrame = from;
	g_emote.lastFrame = -1.0F;
}

static bool HasHold( const EmoteDef& d )
{
	return d.hold[0] != 0.0F || d.hold[1] != 0.0F;
}

static void StartEmote( EIHarry* harry, int e )
{
	const EmoteDef& d = EMOTES[e];
	EndFade( harry );
	g_emote.active = true;
	g_emote.emote = e;
	g_emote.plays = 0;
	g_emote.ending = -1;
	g_emote.holding = g_emote.finishing = false;
	g_emote.started = false;
	g_emote.start = NetNowMs();
	g_emote.where = harry->m_position;
	g_emote.health = harry->m_healthValue;
	g_emoteCancel = 0;
	BeginClip( d.intro ? d.intro : d.clip, d.intro ? false : d.loops );
	mp_log( "[EM] Emote : %s (clip 0x%.8X%s%s).\n", d.name, d.clip, d.intro ? ", after its intro" : "", d.loops ? ", loops" : "" );
}

// Over : blended out (fadeMs 0 : at once).
static void StopEmote( EIHarry* harry, const char* why, uint32_t fadeMs = EMOTE_FADE_MS )
{
	if ( !g_emote.active ) return;
	g_emote.active = false;
	g_emoteLastEnd = why;
	if ( harry ) {
		EndFade( harry );
		// The layer showing : a clip just begun hasn't loaded yet, the one before it shows.
		g_emoteFade.layer = EMOTE_LAYERS[g_emote.started ? g_emote.slot : 1 - g_emote.slot];
		g_emoteFade.since = NetNowMs();
		g_emoteFade.ms = fadeMs;
		g_emoteFade.on = true;
		AnimLayer* layer = GetAnimLayer( harry, g_emoteFade.layer );
		if ( !fadeMs || !layer || layer->m_weight <= 0.0F ) EndFade( harry );
	}
	mp_log( "[EM] Emote ended (%s) after %u ms, %d play%s.\n", why, NetNowMs() - g_emote.start, g_emote.plays, g_emote.plays == 1 ? "" : "s" );
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
	uint32_t now = NetNowMs();
	EAnimController* ctrl = &harry->m_animController;
	Vector3f at = harry->m_position;
	float dx = at.x - g_stillAt.x, dy = at.y - g_stillAt.y, dz = at.z - g_stillAt.z;
	if ( !g_stillSince || dx * dx + dy * dy + dz * dz > 0.05F * 0.05F ) { g_stillAt = at; g_stillSince = now; }

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

	if ( g_emoteRequest >= 0 ) {
		int e = g_emoteRequest;
		g_emoteRequest = -1;
		const char* why = WhyNoEmote( harry );
		if ( why ) {
			g_emoteRefused = why;
			mp_log( "[EM] No %s : %s.\n", EMOTES[e].name, why );
			Toast( "%s", why );
		}
		else {
			if ( g_emote.active ) StopEmote( harry, "another one", 0 );
			StartEmote( harry, e );
		}
	}
	if ( !g_emote.active ) return;
	const EmoteDef& d = EMOTES[g_emote.emote];

	// Over : a button or a key, Harry moved (pushed, fell), an item came out, he got hurt.
	const char* over = g_emoteCancel;
	bool byYou = over != 0;
	g_emoteCancel = 0;
	dx = at.x - g_emote.where.x; dy = at.y - g_emote.where.y; dz = at.z - g_emote.where.z;
	if ( !over && dx * dx + dy * dy + dz * dz > EMOTE_MOVE * EMOTE_MOVE ) over = "moved";
	if ( !over && !harry->m_onGround ) over = "off the ground";
	if ( !over && GetHeldItem( harry ) >= 0 ) over = "an item";
	if ( !over && harry->m_healthValue < g_emote.health ) over = "hurt";
	if ( over ) {
		bool held = EmoteHoldsHarry();
		// A chain you end, or a hold you leave : its ending first (another press skips it) ...
		if ( byYou && !held && d.ending[0] && (d.intro || g_emote.holding) ) {
			mp_log( "[EM] %s : its ending (%s).\n", d.name, over );
			g_emote.ending = 0;
			BeginClip( d.ending[0], false, d.endingFrom, d.endingFrom > 0.0F ? EMOTE_ENDING_BLEND_MS : 0 );
		}
		// ... or the rest of its clip (none after a hold on its last frame : it fades out).
		else if ( byYou && !held && g_emote.holding && !(d.hold[0] < 0.0F && d.hold[1] < 0.0F) ) {
			mp_log( "[EM] %s : finishing (%s).\n", d.name, over );
			g_emote.finishing = true;
			AnimLayer* mine = GetAnimLayer( harry, EMOTE_LAYERS[g_emote.slot] );
			if ( mine ) mine->m_speed = 1.0F;
		}
		else {
			StopEmote( harry, over, held && byYou ? EMOTE_SKIP_FADE_MS : EMOTE_FADE_MS );
			return;
		}
	}

	if ( g_emote.started ) {
		AnimLayer* mine = GetAnimLayer( harry, EMOTE_LAYERS[g_emote.slot] );
		if ( mine ) {
			float frame = mine->m_frame;
			if ( HasHold( d ) && g_emote.clip == d.clip && g_emote.ending < 0 && !g_emote.finishing ) {
				// Its hold : from its first frame on, it stays (between the two frames, again and again).
				ERAnim* clip = mine->m_anim;
				float last = clip ? (float) (ClipFrames(clip) - 1) : 0.0F;
				float a = d.hold[0] < 0.0F ? last : d.hold[0], b = d.hold[1] < 0.0F ? last : d.hold[1];
				if ( frame >= a ) {
					if ( !g_emote.holding ) { g_emote.holding = true; g_emote.holdSince = now; mp_log( "[EM] %s : held (frame %.0f).\n", d.name, frame ); }
					if ( b > a ) {
						// There and back (a clip has no loop of its own : jumping back shows), easing in and out
						// at each end (at its fastest, as fast as the clip plays).
						float fps = mine->m_fps;
						float half = fps > 0.0F ? (b - a) / fps * 1.5707963F : 1.0F;   // Seconds, a to b.
						float phase = (float) (now - g_emote.holdSince) / 1000.0F / half;
						mine->m_speed = 0.0F;
						mine->m_frame = frame = a + (b - a) * 0.5F * (1.0F - cosf( 3.1415927F * phase ));
						g_emote.plays = 1 + (int) (phase / 2.0F);
					}
					else if ( a < last ) { mine->m_speed = 0.0F; mine->m_frame = frame = a; }
				}
			}
			else if ( g_emote.loop ) {   // The game loops it : count the plays.
				if ( frame < g_emote.lastFrame ) g_emote.plays++;
			}
			else {
				bool intro = d.intro && g_emote.clip == d.intro;
				bool more = intro || (g_emote.ending == 0 && d.ending[1]);
				float left = ClipSecondsLeft( mine );
				if ( !more && left <= EMOTE_FADE_MS / 1000.0F ) {   // Its last clip : blended out as it finishes.
					StopEmote( harry, "played", left > 0.05F ? (uint32_t) (left * 1000.0F) : 50 );
					return;
				}
				if ( mine->m_ended ) {   // The next clip, right away.
					if ( intro ) BeginClip( d.clip, true );
					else if ( g_emote.ending == 0 ) { g_emote.ending = 1; BeginClip( d.ending[1], false ); }
				}
			}
			g_emote.lastFrame = frame;
		}
	}
	int key = EMOTE_LAYERS[g_emote.slot];
	if ( !g_emote.started ) {
		EAnimController_SetClip( ctrl, key, g_emote.clip );
		EAnimController_Restart( ctrl, key );
		AnimLayer* layer = GetAnimLayer( harry, key );
		if ( layer ) {
			layer->m_atEnd = !g_emote.loop;
			layer->m_speed = 1.0F;
			if ( g_emote.startFrame > 0.0F ) layer->m_frame = g_emote.startFrame;
		}
		if ( g_emote.blendFrom >= 0 ) g_emote.blendSince = now;
		else DropLayer( harry, EMOTE_LAYERS[1 - g_emote.slot] );
		g_emote.started = true;
		g_emote.plays++;
	}
	AnimLayer* mine = GetAnimLayer( harry, key );
	if ( !mine || GetLayerClipCRC( mine ) != g_emote.clip ) { StopEmote( harry, "the clip didn't load", 0 ); return; }
	KeepLayer( ctrl, mine );
	// Blending from the clip before (its last frame held) into this one.
	float w = 1.0F;
	AnimLayer* from = g_emote.blendFrom >= 0 ? GetAnimLayer( harry, g_emote.blendFrom ) : 0;
	if ( from && now - g_emote.blendSince < g_emote.blendMs ) {
		KeepLayer( ctrl, from );
		w = (float) (now - g_emote.blendSince) / (float) g_emote.blendMs;
	}
	else if ( g_emote.blendFrom >= 0 ) { DropLayer( harry, g_emote.blendFrom ); g_emote.blendFrom = -1; }
	SetEmoteWeights( harry, key, w, g_emote.blendFrom, 1.0F - w );
}

// --------------------------------------------------------------------------------
// The Emotes page (F1) : a wheel's 8 slots round a ring, and choosing an emote for
// one, with a preview : the animation viewer's puppet beside Harry, playing the
// highlighted emote again and again.
// --------------------------------------------------------------------------------
static int g_editWheel = 0;       // The wheel the page shows.
static int g_ringSlot = 0;        // Its slot the ring points at.
static bool g_chooser = false;    // Choosing an emote for that slot.
static char g_emoteSearch[32] = "";
static int g_emoteCategory = 0;   // 0 : favourites, 1 : all, then EMOTE_CATEGORIES.
static int g_listCursor = 0, g_listScroll = 0;
static int g_listWant = -2;       // EMOTES index to put the list's cursor on (-1 : "Empty slot"; -2 : none).
static int g_armed = 0;           // Pressed once, to be pressed again : 1 reset, 2 delete.
static DWORD g_armedAt = 0;
static int g_previewWant = -2;    // The chooser, each frame : EMOTES index, -1 : none (idle), -2 : no preview.
static bool g_previewFailed = false;
static int g_previewShown = -3;
static int g_previewStage = 0;    // A chain : 0 its intro, 1 - 2 its loop, 3 - 4 its ending.
static bool g_previewArmed = false;   // The clip seen under way (not at its end from last time).
static const uint32_t IDLE_CLIP = 0xB23CB6D0;
static void SetMenuOpen( bool open );
static void SaveSettings();

static void SetPreviewClip( uint32_t clip )
{
	g_animSurveyClip = clip;
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
	{
		CurrentPeer current( vp );
		if ( !vp->beast ) SpawnPuppet( harry );
	}
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
	g_animSurveyClip = 0;
	mp_log( "[EM] Preview closed (%s); its puppet is parked.\n", why );
}

static void EmotePreviewTick( EIHarry* harry )
{
	if ( !g_menu.IsOpen() ) { g_chooser = false; g_previewWant = -2; }
	// A parked puppet whose slot a player took (or that went with the level) : theirs, or gone.
	if ( g_parkedPeer && !g_emotePreview && (g_parkedPeer->target || !g_parkedPeer->beast || g_players[g_parkedPeer->id].present) ) g_parkedPeer = 0;
	bool want = g_previewWant >= -1 && !g_animViewer;
	if ( !want ) { ClosePreview( "done" ); g_previewFailed = false; return; }
	if ( !g_emotePreview ) {
		if ( g_previewFailed ) return;
		if ( !OpenPreview( harry ) ) { g_previewFailed = true; return; }
	}
	if ( !g_viewerPeer || !g_viewerPeer->beast ) { ClosePreview( "puppet gone" ); g_previewFailed = true; return; }
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
	if ( !layer || GetLayerClipCRC( layer ) != g_animSurveyClip ) return;
	if ( !layer->m_ended ) { g_previewArmed = true; return; }
	if ( !g_previewArmed ) return;
	if ( !chain ) { SetPreviewClip( g_animSurveyClip ); return; }
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
	int e = g_wheels[g_editWheel].slot[g_ringSlot];
	g_chooser = true;
	g_emoteSearch[0] = 0;
	g_listCursor = g_listScroll = 0;
	g_listWant = e;
	if ( e >= 0 && !EmoteMatches( e ) ) g_emoteCategory = 1;   // All : it's in there.
	mp_log( "[EM] Choosing an emote : %s, slot %d.\n", g_wheels[g_editWheel].name, g_ringSlot + 1 );
}

static void EmotesPage()
{
	if ( g_editWheel >= g_wheelCount ) g_editWheel = g_wheelCount - 1;
	if ( g_editWheel < 0 ) g_editWheel = 0;
	EmoteWheel& wheel = g_wheels[g_editWheel];
	if ( g_armed && GetTickCount() - g_armedAt > 4000 ) g_armed = 0;
	if ( !g_chooser ) {
		g_previewWant = -2;
		const char* names[MAX_WHEELS];
		for ( int w = 0; w < g_wheelCount; w++ ) names[w] = g_wheels[w].name[0] ? g_wheels[w].name : "(no name)";
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
			EmoteWheel& added = g_wheels[g_wheelCount];
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
				DefaultWheel( g_editWheel );
				if ( g_editWheel >= 2 ) strcpy_s( wheel.name, name );
				g_armed = 0;
				SaveSettings();
				Toast( "Wheel reset" );
			}
			else { g_armed = 1; g_armedAt = GetTickCount(); }
		}
		if ( g_wheelCount > 1 && g_menu.Action( g_armed == 2 ? "Delete this wheel : press again" : "Delete this wheel", "Gone for good. Press twice." ) ) {
			if ( g_armed == 2 ) {
				for ( int w = g_editWheel; w < g_wheelCount - 1; w++ ) g_wheels[w] = g_wheels[w + 1];
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
	g_menu.SetKeys( "Enter : choose    Esc : back    Mouse : click" );
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

static void HandleHotkeys( EIHarry* harry )
{
	bool f5 = InterlockedExchange( &g_reqSpawn, 0 ) != 0;
	bool f6 = InterlockedExchange( &g_reqState, 0 ) != 0;
	bool f7 = InterlockedExchange( &g_reqDelay, 0 ) != 0;
	bool f8 = InterlockedExchange( &g_reqStay, 0 ) != 0;
	HandleAnimViewer( harry );
	EmotePreviewTick( harry );

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
static uint32_t AUTOTEST_LEVEL = 0x7567D75A;   // The ice test's walk : Spinja Lair (cold water just ahead).
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
struct SurveyLevel { uint32_t crc; const char* name; };
static const SurveyLevel SURVEY_ALL[] = {
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
// PTLE_MP_SURVEY=2 : only these (a small level, then the big ones where name tags were off).
static const SurveyLevel SURVEY_BIG[] = {
	{ 0x7A9B3870, "a55_jungle_trail" },
	{ 0x07ECCC35, "a05_mama_oullo_tower" },
	{ 0x3292B6C9, "great_tree" },
	{ 0x69F0CDE2, "battered_bridge" },
	{ 0xCD944049, "butterfly_glade" },
	{ 0x62548B77, "white_valley" },
};
// PTLE_MP_SURVEY=3 : every level with water, as William listed them after the
// 2026-11-14 playtest (name tags flickered on the Great Tree and were gone in Jungle
// Canyon). Water may draw the scene a second time for its reflection; see
// water-levels.md.
static const SurveyLevel SURVEY_WATER[] = {
	{ 0x0EF63551, "m05_bittenbinders_camp" },
	{ 0xCD944049, "b62_butterfly_glade" },
	{ 0x8B372E42, "s57_cavern_lake" },
	{ 0x8147EA91, "s06_copacanti_lake" },
	{ 0x3E7EE822, "s60_ekkeko_ice_cavern" },
	{ 0xECC9D759, "b03_flooded_courtyard" },
	{ 0x3292B6C9, "a202_great_tree" },
	{ 0xDEDA69BC, "a100_jungle_canyon" },
	{ 0x619E1126, "save_a3_ladder_of_miles" },
	{ 0x07ECCC35, "a05_mama_oullo_tower" },
	{ 0x1B8833D3, "s58_mountain_sled_run" },   // Water at the very end only.
	{ 0x1553BBE1, "s50_penguin_den" },
	{ 0x7652BAFC, "s14_spinja_lair" },
	{ 0xBA9370DF, "b63_st_claire_day" },
	{ 0x72AD42FA, "b04_st_claire_night" },
	{ 0xE6B9138A, "b52_twin_outposts" },
	{ 0x9D6149E1, "c06_underground_dam" },
	{ 0x6F498BBD, "s04_viracocha_monoliths" },
	{ 0x62548B77, "s51_white_valley" },
};
static const SurveyLevel* SURVEY_LEVELS = SURVEY_ALL;
static int NUM_SURVEY_LEVELS = sizeof(SURVEY_ALL) / sizeof(SURVEY_ALL[0]);
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
static int g_retroSwaps = 0;                  // Pieces drawn with a player's palette (for the autotest).

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
static int g_retroMatched = 0, g_retroUnmatched = 0;   // For the autotest.

static bool HasBoneAt( EICharacter* c, const float bone[3][4] )
{
	int bones = 0;
	const Matrix4f* pose = GetPose( &c->m_animController, &bones );
	if ( !pose || bones <= 0 || bones > MAX_CHAR_BONES ) return false;
	for ( int i = 0; i < bones; i++ ) {
		const float* t = pose[i].data[3];
		if ( fabsf( t[0] - bone[0][3] ) < 0.01F && fabsf( t[1] - bone[1][3] ) < 0.01F && fabsf( t[2] - bone[2][3] ) < 0.01F ) return true;
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
			if ( !p.beast || p.shownOutfit != RETRO_OUTFIT ) continue;
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
	g_origSetTexture = (SetTextureFunc) vtable[VTABLE_SLOT_SETTEXTURE];
	injector::WriteMemory( &vtable[VTABLE_SLOT_SETTEXTURE], &SetTexture_Hook, true );
	g_origDrawPrim = (DrawPrimFunc) vtable[VTABLE_SLOT_DRAWPRIMITIVE];
	injector::WriteMemory( &vtable[VTABLE_SLOT_DRAWPRIMITIVE], &DrawPrim_Hook, true );
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

// The gamepad is polled just before (0x6E8660( index ), called from 0x663BAD : the
// DirectInput state, 0 if there's none). While the menu is open the game is told
// there's none, so Harry doesn't walk off under it.
static const uint32_t POLL_GAMEPAD = 0x6E8660, POLL_GAMEPAD_CALL = 0x663BAD;

// The state is a DIJOYSTATE2 (0x110 bytes, 0x940B78 + index * 0x110; the game
// keeps the one before at 0x940DA8 + ...) : the d-pad is the hat at +0x20 (hundredths
// of a degree, 0xFFFF... : centred), the buttons at +0x30.
static const int PAD_HAT = 0x20, PAD_BUTTONS = 0x30, PAD_STATE_SIZE = 0x110;
static uint8_t g_padSeen[PAD_STATE_SIZE];   // The last state polled (the Keys page reads its buttons).
static bool g_padSeenAny = false;
static uint8_t g_testPad[PAD_STATE_SIZE];   // Autotest : a controller of its own (g_testPadOn).
static bool g_testPadOn = false;
static volatile LONG g_testArrowFrames = 0, g_testHatFrames = 0;   // Autotest : polls in which the game got the right arrow / the d-pad.

static void ToggleRole( const char* how )
{
	SetMyRole( g_myRole == ROLE_HIDER ? ROLE_SEEKER : ROLE_HIDER );
	mp_log( "[HS] Role swapped (%s).\n", how );
	Toast( g_myRole == ROLE_HIDER ? "You're a hider" : "You're a seeker" );
}

// --------------------------------------------------------------------------------
// The emote wheel : held open (L1, or T), pointed at with the right stick (the
// mouse or the arrows), played when let go (1-8 and a click play at once). R1 (Tab) :
// the next wheel. While it's open the game doesn't get the right stick and R1 (the
// arrows, Tab, 1-8). While Harry emotes, any other button or key, or the left stick,
// ends it (a chain plays its ending first).
// --------------------------------------------------------------------------------
static const int PAD_R1 = 5;
static const int DIK_ARROWS[4] = { 0xC8, 0xD0, 0xCB, 0xCD };   // DirectInput key codes : up, down, left, right.

// The game's controls (its options, kept in the registry) : 20 per controller (0x941020,
// 0x2C apart; a button is its number + 11) and for the keyboard (0x940FC0, DIK codes).
// 4 - 7 : the right stick (reaching), 14 - 15 : turning the camera.
static const uint32_t PAD_BINDINGS = 0x941020, KEY_BINDINGS = 0x940FC0;
static bool IsCameraButton( int b )
{
	const int* bind = (const int*) PAD_BINDINGS;
	return bind[14] == b + 11 || bind[15] == b + 11;
}

static const uint32_t WHEEL_SHOW_MS = 150;   // Held this long (or pointed) : the wheel shows (a tap of L1 doesn't).
static const uint32_t WHEEL_KEEP_MS = 300;   // The stick back in the middle : the choice stays this long.

struct WheelUi
{
	bool open;
	bool byPad;          // Else the keyboard's.
	bool shown;          // Pointed at, or flipped : showing whatever the time.
	bool done;           // Played (1-8, a click) or given up : nothing more until it's let go.
	uint32_t since;
	int hover;           // Slot pointed at, -1 : none.
	uint32_t hoverAt;
	LONG centre[8];      // The controller's axes when it opened (lX lY lZ lRx lRy lRz, the sliders).
	POINT mouse;         // Where the cursor's way is measured from.
	bool r1Was, clickWas;
};
static WheelUi g_wheelUi;

static bool WheelShown()
{
	return g_wheelUi.open && !g_wheelUi.done && (g_wheelUi.shown || NetNowMs() - g_wheelUi.since >= WHEEL_SHOW_MS);
}

static void WheelOpen( const uint8_t* pad )
{
	memset( &g_wheelUi, 0, sizeof(g_wheelUi) );
	g_wheelUi.open = true;
	g_wheelUi.byPad = pad != 0;
	g_wheelUi.since = NetNowMs();
	g_wheelUi.hover = -1;
	if ( g_wheelIndex >= g_wheelCount ) g_wheelIndex = 0;
	if ( pad ) {
		for ( int i = 0; i < 8; i++ ) g_wheelUi.centre[i] = ((const LONG*) pad)[i];
		g_wheelUi.r1Was = (pad[PAD_BUTTONS + PAD_R1] & 0x80) != 0;
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
	mp_log( "[EM] Wheel : %s.\n", g_wheels[g_wheelIndex].name );
}

static void WheelPlay( int slot, const char* how )
{
	g_wheelUi.done = true;
	if ( slot < 0 ) return;
	int e = g_wheels[g_wheelIndex].slot[slot];
	mp_log( "[EM] Wheel %s, slot %d (%s) : %s.\n", g_wheels[g_wheelIndex].name, slot + 1, how, e >= 0 ? EMOTES[e].name : "empty" );
	if ( e >= 0 ) g_emoteRequest = e;
}

// The slot a direction points at (dy : down), the first at the top, clockwise.
static int WedgeAt( float dx, float dy )
{
	float a = atan2f( dx, -dy );
	if ( a < 0.0F ) a += 6.2831853F;
	return ((int) floorf( (a + 0.3926991F) / 0.7853982F )) % WHEEL_SLOTS;
}

static void EmotePad( uint8_t* state )
{
	LONG* axes = (LONG*) state;
	uint8_t* buttons = state + PAD_BUTTONS;
	static bool big[8] = { false, };   // The axis goes 0 .. 65535 (or so), not -1000 .. 1000.
	for ( int i = 0; i < 8; i++ ) if ( axes[i] > 1100 || axes[i] < -1100 ) big[i] = true;
	DWORD hat = *(DWORD*) (state + PAD_HAT);
	bool hatCentred = LOWORD( hat ) == 0xFFFF;
	bool held = g_emoteButton >= 0 && g_emoteButton < 32 && (buttons[g_emoteButton] & 0x80);
	bool r1 = (buttons[PAD_R1] & 0x80) != 0;
	bool combo = held && (!g_emoteNeedsR1 || r1);
	static bool comboWas = false;
	static uint8_t before[32];
	static DWORD hatBefore = 0xFFFFFFFF;
	uint8_t raw[32];
	memcpy( raw, buttons, sizeof(raw) );

	if ( combo && !comboWas && !g_wheelUi.open ) WheelOpen( state );
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
	else if ( g_emote.active ) {
		static uint32_t restFor = 0;
		static LONG rest[2];
		if ( restFor != g_emote.start ) { restFor = g_emote.start; rest[0] = axes[0]; rest[1] = axes[1]; }
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
			*(DWORD*) (state + PAD_HAT) = 0xFFFFFFFF;
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

static void EmoteKeys( uint8_t* keys )
{
	static uint8_t before[256];
	uint8_t raw[256];
	memcpy( raw, keys, sizeof(raw) );
	bool held = g_emoteKey > 0 && g_emoteKey < 256 && (raw[g_emoteKey] & 0x80);
	static bool heldWas = false;
	if ( held && !heldWas && !g_wheelUi.open ) WheelOpen( 0 );
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
			if ( !g_wheelUi.done && !g_background && GameHasFocus() && GetCursorPos( &p ) ) {
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
	else if ( g_emote.active ) {
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

static void RoleSwapPad( uint8_t* state )
{
	DWORD& hat = *(DWORD*) (state + PAD_HAT);
	bool centred = LOWORD( hat ) == 0xFFFF;
	int dir = 0;   // -1 : left, 1 : right.
	if ( !centred && hat > 4500 && hat < 13500 ) dir = 1;
	if ( !centred && hat > 22500 && hat < 31500 ) dir = -1;
	bool held = g_swapButton >= 0 && g_swapButton < 128 && (state[PAD_BUTTONS + g_swapButton] & 0x80);
	static int lastDir = 0;
	static bool masking = false;
	if ( !PlayingHs() ) { lastDir = dir; masking = false; return; }
	if ( held && dir && dir != lastDir ) ToggleRole( "controller" );
	lastDir = dir;
	if ( held ) masking = true;
	if ( masking ) {   // Until the d-pad is let go too : no item when the button goes first.
		if ( !held && centred ) masking = false;
		else hat = 0xFFFFFFFF;
	}
}

static void* __cdecl PollGamepad_Hook( int index )
{
	void* state = ((void* (__cdecl *)( int )) POLL_GAMEPAD)( index );
	if ( g_testPadOn && index == 0 ) {   // As the game does : the last state kept, then the new one.
		memcpy( (void*) (0x940DA8 + index * PAD_STATE_SIZE), (void*) (0x940B78 + index * PAD_STATE_SIZE), PAD_STATE_SIZE );
		memcpy( (void*) (0x940B78 + index * PAD_STATE_SIZE), g_testPad, PAD_STATE_SIZE );
		state = (void*) (0x940B78 + index * PAD_STATE_SIZE);
	}
	if ( state ) {
		memcpy( g_padSeen, state, PAD_STATE_SIZE );
		g_padSeenAny = true;
		if ( !MenuBlocksInput() ) { EmotePad( (uint8_t*) state ); RoleSwapPad( (uint8_t*) state ); }
		else if ( g_wheelUi.byPad ) g_wheelUi.open = false;
		if ( LOWORD( *(DWORD*) ((uint8_t*) state + PAD_HAT) ) != 0xFFFF ) InterlockedIncrement( &g_testHatFrames );
	}
	return MenuBlocksInput() ? 0 : state;
}

static void RoleSwapKeys( uint8_t* keys )
{
	bool held = g_swapKey > 0 && g_swapKey < 256 && (keys[g_swapKey] & 0x80);
	bool left = (keys[0xCB] & 0x80) != 0, right = (keys[0xCD] & 0x80) != 0;
	bool any = false;
	for ( int i = 0; i < 4; i++ ) any |= (keys[DIK_ARROWS[i]] & 0x80) != 0;
	static bool wasLeft = false, wasRight = false, masking = false;
	if ( !PlayingHs() ) { wasLeft = left; wasRight = right; masking = false; return; }
	if ( held && ((left && !wasLeft) || (right && !wasRight)) ) ToggleRole( "keyboard" );
	wasLeft = left; wasRight = right;
	if ( held ) masking = true;
	if ( masking ) {
		if ( !held && !any ) masking = false;
		else for ( int i = 0; i < 4; i++ ) keys[DIK_ARROWS[i]] = 0;
	}
}

static void PollKeyboard_Hook()
{
	if ( g_background ) {
		uint8_t before[256];
		memcpy( before, (void*) KEYS_NOW, sizeof(before) );
		((void (*)()) POLL_KEYBOARD)();
		memcpy( (void*) KEYS_BEFORE, before, sizeof(before) );   // Not cleared : held keys stay held.
		for ( int i = 0; i < 256; i++ ) ((uint8_t*) KEYS_NOW)[i] = g_fakeKeys[i];
	}
	else ((void (*)()) POLL_KEYBOARD)();
	if ( MenuBlocksInput() ) {   // The menu has the keyboard.
		memset( (void*) KEYS_NOW, 0, 256 );
		if ( !g_wheelUi.byPad ) g_wheelUi.open = false;
	}
	else {
		EmoteKeys( (uint8_t*) KEYS_NOW );
		RoleSwapKeys( (uint8_t*) KEYS_NOW );
	}
	if ( ((uint8_t*) KEYS_NOW)[0xCD] & 0x80 ) InterlockedIncrement( &g_testArrowFrames );
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
		const Matrix4f& w = g_cur->beast->m_worldMatrix;
		mp_log( "[AT] %s beast world (%.1f, %.1f, %.1f), anim layer %d, moving %d, speed %.3f, heading %.0f\n", tag,
			w.data[3][0], w.data[3][1], w.data[3][2], g_cur->beast->m_animIndex,
			(int) g_cur->moving, g_cur->moveSpeed, g_cur->heading * 180.0F / MP_PI );
		const Vector3f& r = g_cur->beast->m_scale;
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
	EInventory* wheel = harry->m_inventory;
	if ( !wheel ) return;
	wheel->m_hotbar[0] = GetItemObject( ITEM_RAFT );       // Up.
	wheel->m_hotbar[1] = GetItemObject( ITEM_PICKAXES );   // Right.
	wheel->m_hotbar[2] = GetItemObject( ITEM_TORCH );      // Down.
	wheel->m_hotbar[3] = GetItemObject( ITEM_TNT );        // Left.
	for ( int i = 0; i < 8; i++ ) ITEM_TABLE[i].m_unlocked = 1;
	mp_log( "[AT] Gave raft, pickaxes, torch, TNT.\n" );
}

static int g_itemFailures = 0;

static const int MAX_ANIM_CLIPS = 1024;
static struct { uint32_t crc; int owner; int layer; } g_animClips[MAX_ANIM_CLIPS];
static int g_animClipCount = 0;

static void ApplyMyOutfit();

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
		memset( &g_tagDiag, 0, sizeof(g_tagDiag) );
		g_tagDiagOn = true;
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
			g_tagDiagOn = false;
			mp_log( "[AT] TAGS %s : renderer view set up to %u times a frame; its eye (%.1f, %.1f, %.1f), the look-at camera's (%.1f, %.1f, %.1f).\n", SURVEY_LEVELS[step].name, g_rendSetsMax,
				g_rendCam.data[3][0], g_rendCam.data[3][1], g_rendCam.data[3][2], g_cameraWorld.data[3][0], g_cameraWorld.data[3][1], g_cameraWorld.data[3][2] );
			g_rendSetsMax = 0;
			g_rendTwiceLogged = 0;
			const TagDiag& d = g_tagDiag;
			mp_log( "[AT] TAGS %s : %d frames, %.2f look-ats a frame (max %d); puppet drawn to the screen %d times (%ux%u), elsewhere %d (%ux%u); tag noted %d, behind the camera %d.\n",
				SURVEY_LEVELS[step].name, d.frames, d.frames ? (float) d.lookAts / d.frames : 0.0F, d.maxLookAts,
				d.drawsOnScreen, d.screenW, d.screenH, d.drawsElsewhere, d.elseW, d.elseH, d.noted, d.behind );
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
		if ( g_animSurvey ) { phase = 200; phaseStart = now; step = 0; break; }
		if ( g_viewerTest ) { phase = 300; phaseStart = now; step = 0; break; }
		if ( g_menuTest ) { phase = 600; phaseStart = now; step = 0; break; }
		if ( g_skinTest ) { phase = 700; phaseStart = now; step = 0; break; }
		if ( g_hsTest ) { phase = g_hsTest >= 2 ? 1210 : 1200; phaseStart = now; step = 0; break; }
		if ( g_emoteTest ) { phase = 1300; phaseStart = now; step = 0; break; }
		if ( g_distTest ) { phase = 500; phaseStart = now; step = 0; break; }
		if ( g_aimTest ) { phase = 900; phaseStart = now; step = 0; break; }
		if ( g_deathTest ) { phase = 400; phaseStart = now; step = 0; break; }
		if ( g_pitTest ) { phase = 1000; phaseStart = now; step = 0; break; }
		if ( g_iceTest ) { phase = 800; phaseStart = now; step = 0; break; }
		if ( g_raftTest ) { phase = 1100; phaseStart = now; step = 0; break; }
		phase = 7; phaseStart = now; step = 0; snapped = false; circleAngle = 0.0F;   // Roll, collision (open space), circle, walk, jumps, items.
		break;

	case 200: {   // Animation survey : list the clips.
		if ( now - phaseStart < 2000 ) break;
		if ( g_haveCamera ) {   // Beside Harry as the camera sees him : along its right axis.
			float rx = g_cameraWorld.data[0][0], ry = g_cameraWorld.data[0][1];
			float l = sqrtf( rx * rx + ry * ry );
			if ( l > 0.01F ) { g_puppetOffset.x = rx / l * 7.0F; g_puppetOffset.y = ry / l * 7.0F; }
		}
		g_animClipCount = 0;
		EICharacter* owners[2] = { harry, g_cur->beast };
		const char* ownerNames[2] = { "harry", "evil harry" };
		for ( int o = 0; o < 2; o++ ) {
			if ( !owners[o] ) continue;
			int layers = 0;
			for ( int key = 0; key < 4096; key++ ) {
				if ( o == 1 && (key == MIRROR_LAYERS[0] || key == MIRROR_LAYERS[1]) ) continue;
				AnimLayer* layer = GetAnimLayer( owners[o], key );
				if ( !layer ) continue;
				layers++;
				uint32_t crc = GetLayerClipCRC( layer );
				if ( !crc ) continue;
				bool seen = false;
				for ( int i = 0; i < g_animClipCount && !seen; i++ ) seen = g_animClips[i].crc == crc;
				if ( seen || g_animClipCount >= MAX_ANIM_CLIPS ) continue;
				g_animClips[g_animClipCount].crc = crc;
				g_animClips[g_animClipCount].owner = o;
				g_animClips[g_animClipCount].layer = key;
				g_animClipCount++;
			}
			mp_log( "[AT] ANIM : %s has %d animation layers.\n", ownerNames[o], layers );
		}
		mp_log( "[AT] ANIM : %d different clips to film.\n", g_animClipCount );
		for ( int i = 0; i < g_animClipCount; i++ ) {
			mp_log( "[AT] ANIM %03d : clip 0x%.8X (%s, layer %d)\n", i, g_animClips[i].crc, ownerNames[g_animClips[i].owner], g_animClips[i].layer );
		}
		phase = 201; phaseStart = now; step = 0;
		break;
	}
	case 400: {   // Death : Harry dies beside his puppet, comes back; the puppet must survive it (or be replaced).
		static DWORD lastLog = 0;
		if ( (step == 0 && now - phaseStart >= 3000) || (step == 1 && now - phaseStart >= 16000) ) {
			mp_log( "[AT] Death : killing Harry.\n" );
			harry->m_healthValue = 0;
			step++;
		}
		if ( now - lastLog >= 250 ) {
			lastLog = now;
			EIEvilHarryP* b = g_cur->beast;
			mp_log( "[AT] Death %5u ms : Harry 0x%.8X health %d world 0x%.8X level 0x%.8X | puppet 0x%.8X pickaxes 0x%.8X 0x%.8X\n",
				now - phaseStart, harry, harry->m_healthValue, harry->m_world, Gizmod::getCurrentLevelCRC(),
				b, b ? (uint32_t) b->m_pickaxeModel0 : 0, b ? (uint32_t) b->m_pickaxeModel1 : 0 );
		}
		if ( now - phaseStart >= 30000 ) {
			RequestSnapshot( "death_after" );
			EIEvilHarryP* b = g_cur->beast;
			bool ok = b && b->m_pickaxeModel0 && b->m_pickaxeModel1 && g_puppetsSpawned == 1;
			mp_log( "[AT] Death : %s (puppet 0x%.8X, %d spawned in all).\n", ok ? "survived, puppet kept" : "FAIL", b, g_puppetsSpawned );
			InterlockedExchange( &g_autotestDone, ok ? 1 : 2 );
		}
		break;
	}
	case 1000: {   // Pitfall : Harry walks into the pit and is eaten; he comes back, the puppet must follow him back.
		if ( g_pitTest >= 4 ) {   // Two games : 4 walks into the pit, 5 stays at the start and watches the other's puppet.
			static DWORD lastLog = 0;
			static int drawsAt = 0, ticksAt = 0;
			DWORD t = now - phaseStart;
			if ( g_pitTest == 4 && step == 0 && t >= 15000 ) {
				mp_log( "[AT] Pit : walking into the pit.\n" );
				InterlockedExchange( &g_holdMs, 6000 ); InterlockedExchange( &g_holdKey, 'W' );
				step = 1;
			}
			if ( now - lastLog >= 500 ) {
				lastLog = now;
				const Vector3f& h = harry->m_position;
				EIEvilHarryP* b = g_cur->beast;
				const PlayerSnapshot* tg = GetPuppetTarget();
				mp_log( "[AT] Pit %5u ms : Harry (%.1f, %.1f, %.1f) health %d | other's puppet 0x%.8X at (%.1f, %.1f, %.1f), their place %s (%.1f, %.1f, %.1f), ticked %d (last %u ms ago), drawn %d\n",
					t, h.x, h.y, h.z, harry->m_healthValue, b, b ? b->EIBeast::m_position.x : 0.0F, b ? b->EIBeast::m_position.y : 0.0F, b ? b->EIBeast::m_position.z : 0.0F,
					tg ? "known" : "unknown", tg ? tg->position.x : 0.0F, tg ? tg->position.y : 0.0F, tg ? tg->position.z : 0.0F,
					g_cur->ticks, GetTickCount() - g_cur->lastTick, g_puppetPoseCopies );
			}
			if ( g_pitTest == 5 && ( t == 0 || (t >= 20000 && t < 20100) || (t >= 24000 && t < 24100) || (t >= 32000 && t < 32100) ) && !snapped ) { }
			if ( g_pitTest == 5 && step == 0 && t >= 21000 ) { RequestSnapshot( "pit_w1_eating" ); step = 1; }
			if ( g_pitTest == 5 && step == 1 && t >= 26000 ) { RequestSnapshot( "pit_w2_eaten" ); step = 2; }
			if ( step == 2 && t >= 40000 ) { RequestSnapshot( "pit_w3_back" ); drawsAt = g_puppetPoseCopies; ticksAt = g_cur->ticks; step = 3; }
			if ( g_pitTest == 4 && step == 1 && t >= 40000 ) step = 3;
			if ( step == 3 && t >= (g_pitTest == 4 ? 60000u : 45000u) ) {   // (The eater stays a while : the watcher checks while it's still here.)
				EIEvilHarryP* b = g_cur->beast;
				const PlayerSnapshot* tg = GetPuppetTarget();
				float d = 999.0F;
				if ( b && tg ) { float dx = b->EIBeast::m_position.x - tg->position.x, dy = b->EIBeast::m_position.y - tg->position.y, dz = b->EIBeast::m_position.z - tg->position.z; d = sqrtf( dx * dx + dy * dy + dz * dz ); }
				int draws = g_puppetPoseCopies - drawsAt, ticks = g_cur->ticks - ticksAt;
				bool ok = g_pitTest == 4 || (b && d < 6.0F && draws > 30 && ticks > 30);
				mp_log( "[AT] Pit : %s (the other's puppet 0x%.8X %.1f from their place, drawn %d and ticked %d times in 5 s, %d spawned in all).\n",
					ok ? "the other is shown" : "FAIL", b, d, draws, ticks, g_puppetsSpawned );
				if ( g_pitTest == 5 ) RequestSnapshot( "pit_w4_after" );
				InterlockedExchange( &g_autotestDone, ok ? 1 : 2 );
			}
			break;
		}
		static DWORD lastLog = 0;
		static int drawsAt = 0;
		DWORD t = now - phaseStart;
		if ( step == 0 && t >= 2000 ) {
			RequestSnapshot( "pit_0_before" );
			mp_log( "[AT] Pit : walking forward.\n" );
			InterlockedExchange( &g_holdMs, g_pitTest == 3 ? 1500 : g_pitTest == 2 ? 3800 : 6000 ); InterlockedExchange( &g_holdKey, 'W' );
			step = 1;
		}
		if ( g_pitTest == 3 && step >= 1 && t >= 6000 && t < 12000 ) {
			// Another player eaten by the pit, as we receive it : down into it (it opened
			// on their screen, not here), from the ground to 60 below in 2 s, and held.
			float depth = t < 8000 ? (t - 6000) / 2000.0F * 60.0F : 60.0F;
			const Vector3f& h = harry->m_position;
			g_puppetOffset.x = -0.1F - h.x; g_puppetOffset.y = 633.6F - h.y; g_puppetOffset.z = -8.9F - depth - h.z;
		}
		if ( g_pitTest == 3 ) {
			g_testStallPuppet = t >= 8000 && t < 16000;   // Caught by the pit (here, as if).
			if ( g_testStallPuppet && !g_testStalled ) g_testStalled = g_cur->beast;
			if ( !g_cur->beast && !g_cur->limbo && t >= 8000 ) {
				mp_log( "[AT] Pit : the caught puppet was dropped : spawning another.\n" );
				SpawnPuppet( harry );
			}
		}
		if ( g_pitTest == 3 && t >= 12000 && (g_puppetOffset.y != 0.0F || g_puppetOffset.z != 0.0F) ) {
			mp_log( "[AT] Pit : the eaten player is back (puppet jumps to Harry).\n" );
			g_puppetOffset.x = g_puppetOffset.y = g_puppetOffset.z = 0.0F;
		}
		if ( g_pitTest == 2 && step >= 1 && t >= 12000 && g_puppetOffset.y != 0.0F ) {
			mp_log( "[AT] Pit : the puppet's player is back (puppet jumps to Harry).\n" );
			g_puppetOffset.y = 0.0F;
		}
		if ( step == 1 && t >= (g_pitTest == 3 ? 9000u : 5000u) ) { RequestSnapshot( "pit_1_eaten" ); step = 2; }
		if ( step == 2 && t >= 20000 ) { RequestSnapshot( "pit_2_back" ); drawsAt = g_puppetPoseCopies; step = 3; }
		if ( now - lastLog >= 250 && t < 30000 ) {
			lastLog = now;
			EIEvilHarryP* b = g_cur->beast;
			const Vector3f& h = harry->m_position;
			mp_log( "[AT] Pit %5u ms : Harry (%.1f, %.1f, %.1f) health %d | puppet 0x%.8X limbo 0x%.8X at (%.1f, %.1f, %.1f) state %d, target %s, drawn %d\n",
				t, h.x, h.y, h.z, harry->m_healthValue, b, g_cur->limbo,
				b ? b->EIBeast::m_position.x : 0.0F, b ? b->EIBeast::m_position.y : 0.0F, b ? b->EIBeast::m_position.z : 0.0F,
				b ? b->m_stateIndexCurrent : -1, GetPuppetTarget() ? "yes" : "no", g_puppetPoseCopies );
		}
		if ( step == 3 && t >= 25000 ) {
			RequestSnapshot( "pit_3_after" );
			EIEvilHarryP* b = g_cur->beast;
			float dx = 0, dy = 0, dz = 0;
			if ( b ) { dx = b->EIBeast::m_position.x - harry->m_position.x; dy = b->EIBeast::m_position.y - harry->m_position.y; dz = b->EIBeast::m_position.z - harry->m_position.z; }
			float d = sqrtf( dx * dx + dy * dy + dz * dz );
			int draws = g_puppetPoseCopies - drawsAt;
			bool ok = b && d < 6.0F && draws > 30 && (g_pitTest != 3 || g_puppetsSpawned == 2);
			mp_log( "[AT] Pit : %s (puppet 0x%.8X %.1f from Harry, drawn %d times in 5 s, %d spawned in all).\n", ok ? "puppet back with Harry" : "FAIL", b, d, draws, g_puppetsSpawned );
			InterlockedExchange( &g_autotestDone, ok ? 1 : 2 );
		}
		break;
	}
	case 500: {   // Distance : the puppet stands further and further ahead of the camera.
		if ( g_distTest >= 2 ) {   // Two games : 2 runs away from the start, 3 stays and watches the other's puppet.
			static DWORD lastLog = 0; static int posesAt = 0; static float maxDepthDrawn = 0.0F, minAlphaDrawn = 9.0F;
			DWORD t = now - phaseStart;
			if ( g_distTest == 2 && step == 0 && t >= 12000 ) { mp_log( "[AT] Distance : running away.\n" ); InterlockedExchange( &g_holdMs, 14000 ); InterlockedExchange( &g_holdKey, 'W' ); step = 1; }
			EIEvilHarryP* b = g_cur->beast;
			if ( g_distTest == 3 && b && now - lastLog >= 250 ) {
				lastLog = now;
				const Matrix4f& cam = g_cameraWorld;
				float dx = b->EIBeast::m_position.x - cam.data[3][0], dy = b->EIBeast::m_position.y - cam.data[3][1], dz = b->EIBeast::m_position.z - cam.data[3][2];
				float d = sqrtf( dx * dx + dy * dy + dz * dz );
				int renders = g_puppetPoseCopies - posesAt; posesAt = g_puppetPoseCopies;
				float a = b->m_opacityFinal;
				if ( renders > 0 && t > 12000 ) { if ( d > maxDepthDrawn ) maxDepthDrawn = d; if ( a < minAlphaDrawn ) minAlphaDrawn = a; }
				mp_log( "[AT] Distance %5u ms : the other's puppet %.1f from the camera, rendered %d, game fade %.2f, +1C %.2f +20 %.2f +24 %.2f +28 %.2f, tag depth %.1f\n",
					t, d, renders, g_lastPuppetFade, a, b->m_opacity2, b->m_opacity3, b->m_alphaUser, g_cur->tagDepth );
			}
			if ( g_distTest == 3 && ((step == 0 && t >= 15000) || (step == 1 && t >= 20000) || (step == 2 && t >= 25000)) ) {
				char name[32]; sprintf_s( name, "distw_%d", step ); RequestSnapshot( name ); step++;
			}
			if ( t >= (g_distTest == 2 ? 45000u : 35000u) ) {
				bool ok = g_distTest == 2 || (maxDepthDrawn > 150.0F && minAlphaDrawn > 0.9F);
				if ( g_distTest == 3 ) mp_log( "[AT] Distance : the other was rendered up to %.1f from the camera, alpha at least %.2f : %s\n", maxDepthDrawn, minAlphaDrawn, ok ? "ok" : "WRONG" );
				InterlockedExchange( &g_autotestDone, ok ? 1 : 2 );
			}
			break;
		}
		static const float DISTANCES[] = { 20.0F, 60.0F, 100.0F, 140.0F, 180.0F, 220.0F, 260.0F, 300.0F, 400.0F, 800.0F, 1500.0F, 3000.0F };
		static const int NUM = sizeof(DISTANCES) / sizeof(DISTANCES[0]);
		static int posesAtStart = 0, passesAtStart = 0;
		DWORD t = now - phaseStart;
		if ( step >= NUM ) { InterlockedExchange( &g_autotestDone, 1 ); break; }
		if ( t < 100 ) {
			const Matrix4f& cam = g_cameraWorld;   // It looks down its -Z.
			float fx = -cam.data[2][0], fy = -cam.data[2][1];
			float l = sqrtf( fx * fx + fy * fy );
			if ( l > 0.01F ) { g_puppetOffset.x = fx / l * DISTANCES[step]; g_puppetOffset.y = fy / l * DISTANCES[step]; }
			snapped = false;
		}
		else if ( t >= 2000 && t < 2100 ) { posesAtStart = g_puppetPoseCopies; passesAtStart = g_tagPasses[1]; }
		else if ( t >= 3000 && !snapped ) {
			char name[32];
			sprintf_s( name, "dist_%03d", (int) DISTANCES[step] );
			RequestSnapshot( name );
			mp_log( "[AT] Distance %.0f ahead of Harry : render called %d times in 1 s (%d to the screen), tag depth %.1f, tag %s; ticked %d times, last %u ms ago; the game's fade %.2f (%ld calls, kept 1 : %ld time(s)).\n", DISTANCES[step],
				g_puppetPoseCopies - posesAtStart, g_tagPasses[1] - passesAtStart, g_cur->tagDepth, g_cur->tagFrame == g_presentCount ? "noted" : "not noted",
				g_cur->ticks, GetTickCount() - g_cur->lastTick, g_lastPuppetFade, g_puppetFadeCalls, g_fadesStopped );
			snapped = true;
		}
		else if ( t >= 3500 ) { step++; phaseStart = now; }
		break;
	}
	case 1100: {   // Raft : Harry paddles one side and turns; the puppet faces where he faces, not where he drifts.
		DWORD t = now - phaseStart;
		EIEvilHarryP* b = g_cur->beast;
		const PlayerSnapshot* tg = GetPuppetTarget();
		if ( !b || !tg ) break;
		static float startYaw, maxDiff, maxDrift, lastYaw, turned; static int samples;
		float hYaw = atan2f( -harry->m_transformMatrix.data[1][1], -harry->m_transformMatrix.data[1][0] );
		float tYaw = atan2f( -tg->transform.data[1][1], -tg->transform.data[1][0] );
		const Matrix4f& w = b->m_worldMatrix;
		float pYaw = atan2f( w.data[1][1], w.data[1][0] ) + g_forwardLocalAngle - MP_PI / 2.0F;   // (SetBeastWorld : row 1 = heading - forward + 90 deg.)
		auto wrap = []( float a ) { while ( a > MP_PI ) a -= 2.0F * MP_PI; while ( a < -MP_PI ) a += 2.0F * MP_PI; return a; };
		if ( step == 0 ) {
			if ( t < 6000 ) break;   // (He wades in by himself.)
			const Matrix4f& cam = g_cameraWorld;
			float fx = -cam.data[2][0], fy = -cam.data[2][1];
			float l = sqrtf( fx * fx + fy * fy );
			if ( l > 0.01F ) { g_puppetOffset.x = fx / l * 10.0F; g_puppetOffset.y = fy / l * 10.0F; }
			bool raft = InventoryItem::setItemInHand( InventoryItem::RAFT );
			mp_log( "[AT] Raft : in hand : %s; Harry at (%.1f, %.1f, %.1f) facing %.0f deg.\n", raft ? "yes" : "NO", harry->m_position.x, harry->m_position.y, harry->m_position.z, hYaw * 180.0F / MP_PI );
			step = 1; phaseStart = now;
		}
		else if ( step == 1 ) {
			if ( t < 4000 ) break;
			RequestSnapshot( "raft_0_before" );
			startYaw = lastYaw = hYaw; turned = 0; maxDiff = maxDrift = 0; samples = 0;
			InterlockedExchange( &g_holdMs, 4500 ); InterlockedExchange( &g_holdKey, VK_UP );   // The right arm paddles (right stick up).
			step = 2; phaseStart = now;
		}
		else if ( step == 2 || step == 3 ) {
			turned += fabsf( wrap( hYaw - lastYaw ) ); lastYaw = hYaw;
			if ( t > 500 ) {   // (The puppet's target lags a quarter second.)
				maxDiff = max( maxDiff, fabsf( wrap( pYaw - tYaw ) ) );
				samples++;
			}
			if ( g_cur->moveSpeed > 0.03F ) maxDrift = max( maxDrift, fabsf( wrap( atan2f( g_cur->velY, g_cur->velX ) - tYaw ) ) );
			if ( step == 2 && t >= 5200 ) { RequestSnapshot( "raft_1_turned" ); InterlockedExchange( &g_holdMs, 4500 ); InterlockedExchange( &g_holdKey, 'W' ); step = 3; phaseStart = now; }
			else if ( step == 3 && t >= 5200 ) {
				RequestSnapshot( "raft_2_turned_back" );
				bool ok = turned * 180.0F / MP_PI > 45.0F && samples > 100 && maxDiff * 180.0F / MP_PI < 10.0F;
				mp_log( "[AT] Raft : Harry turned %.0f deg in all (now %.0f from the start); the puppet faced up to %.1f deg off him (%d samples); his drift was up to %.0f deg off his facing : %s\n",
					turned * 180.0F / MP_PI, wrap( hYaw - startYaw ) * 180.0F / MP_PI, maxDiff * 180.0F / MP_PI, samples, maxDrift * 180.0F / MP_PI, ok ? "ok" : "WRONG" );
				InterlockedExchange( &g_autotestDone, ok ? 1 : 2 );
			}
		}
		break;
	}
	case 900: {   // Aim : the puppet ahead; Harry takes the TNT and aims it (Down held), then back.
		DWORD t = now - phaseStart;
		IDirect3DDevice9* dev = *(IDirect3DDevice9**) RENDERER_DEVICE;
		float tx = -1.0F, ty = -1.0F, td = 0.0F;
		bool shown = dev && PlaceNameTag( *g_cur, dev, &tx, &ty, &td );
		const float* eye = &g_cameraWorld.data[3][0];
		if ( step == 0 ) {
			if ( t < 100 ) {
				const Matrix4f& cam = g_cameraWorld;
				float fx = -cam.data[2][0], fy = -cam.data[2][1];
				float l = sqrtf( fx * fx + fy * fy );
				if ( l > 0.01F ) { g_puppetOffset.x = fx / l * 25.0F; g_puppetOffset.y = fy / l * 25.0F; }
				bool tnt = InventoryItem::setItemInHand( g_aimTest == 2 ? InventoryItem::RAFT : InventoryItem::TNT );
				mp_log( "[AT] Aim : %s in hand : %s.\n", g_aimTest == 2 ? "raft" : "TNT", tnt ? "yes" : "NO" );
			}
			if ( t < 3000 ) break;
			mp_log( "[AT] Aim : before, camera 0x%.8X eye (%.1f, %.1f, %.1f), tag %s at (%.0f, %.0f).\n", g_cameraSite, eye[0], eye[1], eye[2], shown ? "shown" : "NOT shown", tx, ty );
			RequestSnapshot( "aim_0_before" );
			g_aimItemDraws = g_puppetItemDraws;
			if ( g_aimTest == 2 ) g_testSkipHarryItemDraw = true;
			InterlockedExchange( &g_holdMs, 6000 ); InterlockedExchange( &g_holdKey, VK_DOWN );
			step = 1; phaseStart = now;
		}
		else if ( step == 1 ) {
			// The swing into aiming : the tag should glide, not jump up and down.
			static float lastY, lastDy; static int reversals, samples; static float maxStep;
			if ( t < 100 ) { lastY = ty; lastDy = 0; reversals = samples = 0; maxStep = 0; }
			else if ( shown && t < 2000 ) {
				float dy = ty - lastY;
				if ( fabsf( dy ) > 2.0F ) {
					if ( lastDy != 0 && ( dy > 0 ) != ( lastDy > 0 ) ) reversals++;
					lastDy = dy;
				}
				if ( fabsf( dy ) > maxStep ) maxStep = fabsf( dy );
				lastY = ty; samples++;
			}
			if ( t < 3000 ) break;
			mp_log( "[AT] Aim : swing into aiming : %d samples, tag turned up/down %d time(s), biggest step %.0f px.\n", samples, reversals, maxStep );
			mp_log( "[AT] Aim : looking (Down held), camera 0x%.8X eye (%.1f, %.1f, %.1f), tag %s at (%.0f, %.0f).\n", g_cameraSite, eye[0], eye[1], eye[2], shown ? "shown" : "NOT shown", tx, ty );
			RequestSnapshot( "aim_1_looking" );
			mp_log( "[AT] Aim : the puppet's item drawn %d times while looking (%d before)%s.\n", g_puppetItemDraws - g_aimItemDraws, g_aimItemDraws,
				g_testSkipHarryItemDraw ? ", Harry's not drawn" : "" );
			if ( g_aimTest == 2 && g_puppetItemDraws - g_aimItemDraws < 60 ) { mp_log( "[AT] FAIL : the raft vanished for the others.\n" ); InterlockedExchange( &g_autotestDone, 2 ); }
			g_testSkipHarryItemDraw = false;
			{
				const float* re = &g_rendCam.data[3][0];
				mp_log( "[AT] Aim : renderer 0x%.8X view set %u time(s) (up to %u a frame), last frame %u (now %u), eye (%.1f, %.1f, %.1f)\n",
					g_rendSelf, g_rendSets, g_rendSetsMax, g_rendFrame, g_presentCount, re[0], re[1], re[2] );
			}
			step = 2; phaseStart = now;
		}
		else if ( step == 2 ) {
			if ( t < 5000 ) break;
			mp_log( "[AT] Aim : after, camera 0x%.8X eye (%.1f, %.1f, %.1f), tag %s at (%.0f, %.0f).\n", g_cameraSite, eye[0], eye[1], eye[2], shown ? "shown" : "NOT shown", tx, ty );
			RequestSnapshot( "aim_2_after" );
			for ( int i = 0; i < NUM_CAMERA_LOOKAT_CALLS; i++ )
				if ( g_cameraSiteCount[i] ) mp_log( "[AT] Aim : camera call 0x%.8X built %d camera(s).\n", CAMERA_LOOKAT_CALLS[i], g_cameraSiteCount[i] );
			InterlockedExchange( &g_autotestDone, 1 );
		}
		break;
	}
	case 800: {   // Ice : the puppet is put in "thrown from water" (what swimming did), then back to idle.
		DWORD t = now - phaseStart;
		EIEvilHarryP* b = g_cur->beast;
		if ( !b ) { InterlockedExchange( &g_autotestDone, 2 ); break; }
		if ( g_iceTest >= 2 ) {   // 2 : Harry walks into the cold water beside the puppet; 3 : the puppet stands out in it.
			if ( step == 0 ) {
				if ( t < 2000 ) break;
				RequestSnapshot( "ice_0_before" );
				if ( g_iceTest == 3 ) {   // The puppet out in the pool ahead (the camera looks down its -Z); Harry stays on the shore.
					const Matrix4f& cam = g_cameraWorld;
					float fx = -cam.data[2][0], fy = -cam.data[2][1];
					float l = sqrtf( fx * fx + fy * fy );
					if ( l > 0.01F ) { g_puppetOffset.x = fx / l * 14.0F; g_puppetOffset.y = fy / l * 14.0F; }
				}
				else { InterlockedExchange( &g_holdMs, 5000 ); InterlockedExchange( &g_holdKey, 'W' ); }   // Harry walks into the water and freezes.
				step = 1; phaseStart = now;
			}
			else if ( step >= 1 && step <= 8 ) {
				if ( t < 1200 ) break;
				char name[32];
				sprintf_s( name, "ice_%d_walk", step );
				RequestSnapshot( name );
				uint8_t* pc = (uint8_t*) b->m_playerIceCube;
				uint8_t* hc = (uint8_t*) Gizmod::getHarry()->m_playerIceCube;
				mp_log( "[AT] Ice %d : puppet at (%.1f, %.1f, %.1f) in state %d, freezes stopped %ld; puppet's cube 0x%.8X (world 0x%.8X, frozen %d, parent 0x%.8X), Harry's 0x%.8X (world 0x%.8X, frozen %d, parent 0x%.8X), clip 0x%.8X.\n", step,
					b->EIBeast::m_position.x, b->EIBeast::m_position.y, b->EIBeast::m_position.z, b->m_stateIndexCurrent, g_puppetFreezesStopped,
					(uint32_t) pc, pc ? *(uint32_t*) (pc + 0xC) : 0, pc ? pc[0x2F5] : -1, pc ? *(uint32_t*) (pc + 0x1B4) : 0,
					(uint32_t) hc, hc ? *(uint32_t*) (hc + 0xC) : 0, hc ? hc[0x2F5] : -1, hc ? *(uint32_t*) (hc + 0x1B4) : 0,
					g_cur->target ? g_cur->target->clipCRC : 0 );
				step++; phaseStart = now;
			}
			else {   // Harry's freeze also put the puppet in "thrown from water" (the pool alone doesn't) : it must have been stopped, its cube kept out of the level.
				uint8_t* pc = (uint8_t*) b->m_playerIceCube;
				bool ok = g_allowPuppetFreeze || g_iceTest == 3 || (g_puppetFreezesStopped > 0 && (!pc || (!*(uint32_t*) (pc + 0xC) && !pc[0x2F5])));
				mp_log( "[AT] Ice : the puppet kept out of the ice : %s\n", ok ? "ok" : "WRONG" );
				InterlockedExchange( &g_autotestDone, ok ? 1 : 2 );
			}
			break;
		}
		if ( step == 0 ) {
			if ( t < 2000 ) break;
			RequestSnapshot( "ice_0_before" );
			g_autoLocomotion = false;
			g_cur->forcedState = FindStateIndex( b, "thrown from water" );
			mp_log( "[AT] Ice : forcing state %d (\"thrown from water\").\n", g_cur->forcedState );
			step = 1; phaseStart = now;
		}
		else if ( step == 1 ) {
			if ( t < 1500 ) break;
			bool ok = g_allowPuppetFreeze || g_puppetFreezesStopped > 0;
			mp_log( "[AT] Ice : puppet in state %d, freezes stopped %ld : %s\n", b->m_stateIndexCurrent, g_puppetFreezesStopped, ok ? "ok" : "WRONG" );
			RequestSnapshot( "ice_1_forced" );
			g_autoLocomotion = true;
			step = 2; phaseStart = now;
			if ( !ok ) InterlockedExchange( &g_autotestDone, 2 );
		}
		else if ( step == 2 ) {
			if ( t < 2500 ) break;
			mp_log( "[AT] Ice : back in state %d.\n", b->m_stateIndexCurrent );
			RequestSnapshot( "ice_2_after" );
			step = 3; phaseStart = now;
		}
		else if ( t > 1000 ) InterlockedExchange( &g_autotestDone, 1 );
		break;
	}
	case 1300: {   // Emotes, one game : how they play and end, the wheel by controller and by keyboard.
		static int failures = 0;
		static DWORD lastLog = 0, mark = 0;
		DWORD t = now - phaseStart;
		struct Local { static void Check( const char* what, bool ok ) { mp_log( "[AT] Emote : %s : %s\n", what, ok ? "ok" : "WRONG" ); if ( !ok ) failures++; } };
		static int victory, jig, breakdance, running, dizzy, pickaxe, zen, death, bdFinish;
		static uint32_t arrowFrames = 0;
		static bool sawEnding[2];
		// Harry's pose not moving (the same bones as last time) : the longest stretch, while watched.
		static bool watch = false;
		static float poseSum = 0.0F;
		static DWORD stillFrom = 0, longestStill = 0;
		{
			int bones = 0;
			Matrix4f* pose = GetPose( &harry->m_animController, &bones );
			float sum = 0.0F;
			for ( int b = 0; pose && b < bones && b < MAX_CHAR_BONES; b++ )
				for ( int r = 0; r < 4; r++ ) for ( int c = 0; c < 4; c++ ) sum += pose[b].data[r][c] * (float) (1 + b * 16 + r * 4 + c);
			if ( sum != poseSum || !stillFrom ) { poseSum = sum; stillFrom = now; }
			if ( watch && now - stillFrom > longestStill ) longestStill = now - stillFrom;
		}
		if ( g_emote.active && g_emote.ending >= 0 ) sawEnding[g_emote.ending] = true;
		if ( g_emote.active && now - lastLog >= 500 ) {
			lastLog = now;
			AnimLayer* l = GetAnimLayer( harry, EMOTE_LAYERS[g_emote.slot] );
			int layer = -1; float w = 0;
			uint32_t clip = GetPlayingClip( harry, &layer, &w );
			mp_log( "[AT] Emote %s : clip 0x%.8X, frame %.3f, clock %.3f, play %d; Harry shows 0x%.8X (layer %d, %.2f), the puppet 0x%.8X.\n",
				EMOTES[g_emote.emote].name, g_emote.clip, l ? l->m_frame : 0.0F, l ? l->m_time : 0.0F, g_emote.plays,
				clip, layer, w, g_cur->mirrorClip );
		}
		if ( step == 0 && t >= 1500 ) {
			g_wheelCount = 2;
			for ( int w = 0; w < MAX_WHEELS; w++ ) DefaultWheel( w );
			victory = FindEmoteByName( "Victory Dance" ); jig = FindEmoteByName( "Jig" ); breakdance = FindEmoteByName( "Breakdance" );
			running = FindEmoteByName( "Running Man" ); dizzy = FindEmoteByName( "Dizzy Walk" ); pickaxe = FindEmoteByName( "Pickaxe Spin" ); zen = FindEmoteByName( "Teleport Zen" ); death = FindEmoteByName( "Death" ); bdFinish = FindEmoteByName( "Breakdance Finish" );
			mp_log( "[AT] Emote : %d emotes in the catalogue.\n", NUM_EMOTES );
			bool all = true;
			for ( int s = 0; s < WHEEL_SLOTS; s++ ) all &= g_wheels[0].slot[s] >= 0 && (s >= 6 || g_wheels[1].slot[s] >= 0);
			Local::Check( "the default wheels' emotes are all in the catalogue", all && NUM_EMOTES > 200 );
			Local::Check( "Breakdance is a chain, with an ending", breakdance >= 0 && EMOTES[breakdance].intro != 0 && EMOTES[breakdance].ending[0] && EMOTES[breakdance].ending[1] && g_wheels[0].slot[3] == breakdance );
			Local::Check( "Jig plays once, Running Man loops", jig >= 0 && !EMOTES[jig].loops && running >= 0 && EMOTES[running].loops );
			// The ini : saved, changed, loaded back.
			char ini[MAX_PATH];
			GetTempPathA( MAX_PATH, ini );
			strcat_s( ini, "ptle-emote-test.ini" );
			DeleteFileA( ini );
			EmoteWheel saved[MAX_WHEELS];
			g_wheelCount = 3;
			strcpy_s( g_wheels[2].name, "Mine, all | mine" );
			g_wheels[2].slot[5] = dizzy;
			SaveWheels( ini );
			memcpy( saved, g_wheels, sizeof(saved) );
			strcpy_s( g_wheels[2].name, "Mine, all / mine" );   // As it comes back.
			memcpy( saved[2].name, g_wheels[2].name, sizeof(saved[2].name) );
			for ( int w = 0; w < MAX_WHEELS; w++ ) DefaultWheel( w );
			g_wheelCount = 0;
			LoadWheels( ini );
			Local::Check( "the wheels go through the ini", g_wheelCount == 3 && [&]() {
				for ( int w = 0; w < 3; w++ ) if ( strcmp( saved[w].name, g_wheels[w].name ) || memcmp( saved[w].slot, g_wheels[w].slot, sizeof(saved[w].slot) ) ) return false;
				return true; }() );
			char line[256] = "";
			GetPrivateProfileStringA( "Emotes", "Wheel1", "", line, sizeof(line), ini );
			mp_log( "[AT] Emote : Wheel1=%s\n", line );
			DeleteFileA( ini );
			g_wheelCount = 2;
			DefaultWheel( 2 );
			g_emoteRequest = victory;
			watch = true; longestStill = 0;
			step = 1; phaseStart = now;
		}
		else if ( step == 1 ) {
			if ( t >= 1200 && t < 1300 ) RequestSnapshot( "emote_1_victory" );
			if ( t > 300 && !g_emote.active ) {
				mp_log( "[AT] Emote : Victory Dance lasted %u ms.\n", t );
				Local::Check( "Victory Dance plays to its end", strcmp( g_emoteLastEnd, "played" ) == 0 && t >= 2000 );
				step = 2; phaseStart = now;
			}
			else if ( t > 20000 ) { Local::Check( "Victory Dance ends", false ); step = 2; phaseStart = now; }
		}
		else if ( step == 2 && t >= 1000 ) {
			int layer = -1; float w = 0;
			uint32_t clip = GetPlayingClip( harry, &layer, &w );
			mp_log( "[AT] Emote : after it, Harry shows 0x%.8X (layer %d, %.2f).\n", clip, layer, w );
			Local::Check( "the game's own animation is back", clip && layer < EMOTE_LAYERS[0] );
			mp_log( "[AT] Emote : Victory Dance and after, Harry's longest still : %u ms.\n", longestStill );
			Local::Check( "no freeze in Victory Dance, nor at its end", longestStill < 200 );
			RequestSnapshot( "emote_2_after" );
			g_emoteRequest = jig;
			step = 101; phaseStart = now;
		}
		else if ( step == 101 && (!g_emote.active && t > 300 || t > 20000) ) {
			Local::Check( "Jig plays once", !g_emote.active && strcmp( g_emoteLastEnd, "played" ) == 0 );
			g_emoteRequest = running;
			longestStill = 0;
			mark = 0;
			step = 3; phaseStart = now;
		}
		else if ( step == 3 && t >= 12000 && t < 12500 && !mark ) { mark = 1; SendKey( VK_NUMPAD7, true ); }
		else if ( step == 3 && t >= 12500 && t < 13000 && mark == 1 ) {
			mark = 2;
			SendKey( VK_NUMPAD7, false );
			Local::Check( "turning the camera (numpad 7) doesn't end it", g_emote.active && g_emote.emote == running );
		}
		else if ( step == 3 && t >= 16000 ) {   // Long enough for one of Harry's idles (it fades every layer out).
			AnimLayer* mine = GetAnimLayer( harry, EMOTE_LAYERS[g_emote.slot] );
			mp_log( "[AT] Emote : Running Man, %d plays; longest still %u ms.\n", g_emote.plays, longestStill );
			Local::Check( "Running Man loops on, through Harry's idles", g_emote.active && g_emote.emote == running && g_emote.plays >= 8 && mine && mine->m_ticked && longestStill < 200 );
			RequestSnapshot( "emote_2b_running" );
			watch = false;
			SendKey( 'W', true );
			step = 4; phaseStart = now;
		}
		else if ( step == 4 && (!g_emote.active || t > 3000) ) {
			Local::Check( "a key ends it", !g_emote.active && strcmp( g_emoteLastEnd, "a key" ) == 0 );
			SendKey( 'W', false );
			step = 5; phaseStart = now;
		}
		else if ( step == 5 && t >= 2500 ) {
			g_emoteRequest = breakdance;
			mark = 0;
			watch = true; longestStill = 0;
			step = 6; phaseStart = now;
		}
		else if ( step == 6 ) {
			if ( !mark && g_emote.active && g_emote.clip == EMOTES[breakdance].clip ) { mark = t; mp_log( "[AT] Emote : Breakdance's loop after %u ms.\n", t ); }
			if ( t >= 5000 && t < 5100 ) RequestSnapshot( "emote_3_breakdance" );
			if ( t >= 7000 ) {
				Local::Check( "Breakdance : its intro, then its loop", mark && g_emote.active && g_emote.clip == EMOTES[breakdance].clip );
				mp_log( "[AT] Emote : Breakdance, longest still %u ms.\n", longestStill );
				Local::Check( "no freeze from its intro to its loop", longestStill < 200 );
				watch = false;
				sawEnding[0] = sawEnding[1] = false;
				memset( g_testPad, 0, sizeof(g_testPad) );
				for ( int i = 0; i < 4; i++ ) *(DWORD*) (g_testPad + PAD_HAT + 4 * i) = 0xFFFFFFFF;
				g_testPadOn = true;
				step = 7; phaseStart = now;
			}
		}
		else if ( step == 7 && t >= 300 ) { g_testPad[PAD_BUTTONS + 2] = 0x80; mark = 0; step = 8; phaseStart = now; }
		else if ( step == 8 && t >= 300 && !mark ) {
			mark = 1;
			RequestSnapshot( "emote_3b_ending" );
			Local::Check( "a button : Breakdance's ending, the game not getting the button", g_emote.active && g_emote.ending >= 0 && !(((uint8_t*) 0x940B78)[PAD_BUTTONS + 2] & 0x80) );
		}
		else if ( step == 8 && (!g_emote.active || t > 8000) ) {
			mp_log( "[AT] Emote : Breakdance's ending over after %u ms.\n", t );
			Local::Check( "both its ending clips play, then it's over", !g_emote.active && sawEnding[0] && sawEnding[1] && strcmp( g_emoteLastEnd, "played" ) == 0 );
			g_testPad[PAD_BUTTONS + 2] = 0;
			g_emoteRequest = pickaxe;
			step = 110; phaseStart = now;
		}
		// Pickaxe Spin : the left stick (its ending, Harry held), then a button skips it.
		else if ( step == 110 && t >= 2000 && t < 2300 ) g_testPad[PAD_BUTTONS + ((const int*) PAD_BINDINGS)[14] - 11] = 0x80;
		else if ( step == 110 && t >= 2300 && t < 4000 ) g_testPad[PAD_BUTTONS + ((const int*) PAD_BINDINGS)[14] - 11] = 0;
		else if ( step == 110 && t >= 4000 ) {
			Local::Check( "turning the camera (its button) doesn't end it", g_emote.active && g_emote.emote == pickaxe );
			*(LONG*) (g_testPad + 0) = 1000;
			step = 111; phaseStart = now;
		}
		else if ( step == 111 && t >= 300 ) {
			Local::Check( "the left stick : Pickaxe Spin's ending, Harry held", g_emote.active && g_emote.ending >= 0 && *(LONG*) 0x940B78 != 1000 );
			g_testPad[PAD_BUTTONS + 3] = 0x80;
			step = 112; phaseStart = now;
		}
		else if ( step == 112 && t >= 300 ) {
			Local::Check( "a button skips the ending", !g_emote.active && strcmp( g_emoteLastEnd, "a button" ) == 0 );
			g_testPad[PAD_BUTTONS + 3] = 0;
			*(LONG*) (g_testPad + 0) = 0;
			step = 130; phaseStart = now;
		}
		// Holds : Teleport Zen floats till you move (then finishes); Death lies there (then gets up).
		else if ( step == 130 && t >= 2500 ) { g_emoteRequest = zen; step = 131; phaseStart = now; }
		else if ( step == 131 && t >= 10000 ) {
			AnimLayer* l = g_emote.active ? GetAnimLayer( harry, EMOTE_LAYERS[g_emote.slot] ) : 0;
			float f = l ? l->m_frame : -1.0F;
			mp_log( "[AT] Emote : Teleport Zen at frame %.1f, %d plays.\n", f, g_emote.plays );
			Local::Check( "Teleport Zen floats on, smiling (its frames 246 - 288, there and back)", g_emote.active && g_emote.holding && f >= 245.9F && f <= 288.1F && g_emote.plays >= 2 );
			RequestSnapshot( "emote_11_zen" );
			SendKey( 'W', true );
			step = 132; phaseStart = now;
		}
		else if ( step == 132 && t >= 300 ) {
			Local::Check( "W : it finishes, Harry staying put", g_emote.active && g_emote.finishing && g_emote.ending < 0 );
			step = 133; phaseStart = now;
		}
		else if ( step == 133 && (!g_emote.active || t > 6000) ) {
			mp_log( "[AT] Emote : Teleport Zen finished after %u ms more.\n", t + 300 );
			Local::Check( "then it's over", !g_emote.active && strcmp( g_emoteLastEnd, "played" ) == 0 );
			SendKey( 'W', false );
			step = 134; phaseStart = now;
		}
		else if ( step == 134 && t >= 2500 ) { g_emoteRequest = death; step = 135; phaseStart = now; }
		else if ( step == 135 && t >= 7000 ) {
			Local::Check( "Death : Harry lies there", g_emote.active && g_emote.emote == death && g_emote.holding );
			RequestSnapshot( "emote_12_death" );
			SendKey( 'W', true );
			step = 136; phaseStart = now;
		}
		else if ( step == 136 && t >= 700 ) {
			Local::Check( "W : he gets up (Falling Recovery), staying put", g_emote.active && g_emote.ending == 0 && g_emote.clip == EMOTES[death].ending[0] );
			RequestSnapshot( "emote_13_getup" );
			step = 137; phaseStart = now;
		}
		else if ( step == 137 && (!g_emote.active || t > 6000) ) {
			Local::Check( "then it's over", !g_emote.active && strcmp( g_emoteLastEnd, "played" ) == 0 );
			SendKey( 'W', false );
			step = 138; phaseStart = now;
		}
		// Breakdance Finish (on its own) : its pose stays, till you move.
		else if ( step == 138 && t >= 2500 ) { g_emoteRequest = bdFinish; step = 139; phaseStart = now; }
		else if ( step == 139 && (g_emote.holding || t > 10000) ) { step = 140; phaseStart = now; }
		else if ( step == 140 && t >= 2000 ) {
			Local::Check( "Breakdance Finish stays at its end", bdFinish >= 0 && g_emote.active && g_emote.emote == bdFinish && g_emote.holding );
			RequestSnapshot( "emote_14_bdfinish" );
			SendKey( 'W', true );
			step = 141; phaseStart = now;
		}
		else if ( step == 141 && (!g_emote.active || t > 3000) ) {
			mp_log( "[AT] Emote : Breakdance Finish over after %u ms (%s).\n", t, g_emoteLastEnd );
			Local::Check( "W : it fades out", !g_emote.active && t < 500 );
			SendKey( 'W', false );
			step = 9; phaseStart = now;
		}
		// The wheel, by controller : L1, the right stick to the right (Running Man), R1 twice, let go.
		else if ( step == 9 && t >= 2500 ) { g_testPad[PAD_BUTTONS + 4] = 0x80; step = 10; phaseStart = now; }
		else if ( step == 10 && t >= 300 ) {
			*(LONG*) (g_testPad + 0x0C) = 1000; *(LONG*) (g_testPad + 0x18) = 1000;
			arrowFrames = (uint32_t) g_testArrowFrames;
			SendKey( VK_RIGHT, true );   // As JoyToKey sends it, from the same stick.
			step = 11; phaseStart = now;
		}
		else if ( step == 11 && t >= 400 ) {
			Local::Check( "L1 + the right stick : the wheel, Running Man picked", WheelShown() && g_wheelUi.byPad && g_wheelUi.hover == 2 );
			Local::Check( "the game doesn't get the right stick (nor the slider it can be on)", *(LONG*) (0x940B78 + 0x0C) == 0 && *(LONG*) (0x940B78 + 0x18) == 0 );
			Local::Check( "nor the arrows (JoyToKey's right stick)", (uint32_t) g_testArrowFrames == arrowFrames );
			RequestSnapshot( "emote_4_wheel" );
			step = 12; phaseStart = now;
		}
		else if ( step == 12 && t >= 300 ) { g_testPad[PAD_BUTTONS + PAD_R1] = 0x80; step = 13; phaseStart = now; }
		else if ( step == 13 && t >= 300 ) {
			Local::Check( "R1 : the next wheel", g_wheelIndex == 1 && WheelShown() );
			RequestSnapshot( "emote_5_wheel2" );
			g_testPad[PAD_BUTTONS + PAD_R1] = 0;
			step = 14; phaseStart = now;
		}
		else if ( step == 14 && t >= 300 ) { g_testPad[PAD_BUTTONS + PAD_R1] = 0x80; step = 15; phaseStart = now; }
		else if ( step == 15 && t >= 300 ) {
			Local::Check( "R1 again : back to the first", g_wheelIndex == 0 );
			g_testPad[PAD_BUTTONS + PAD_R1] = 0;
			step = 16; phaseStart = now;
		}
		else if ( step == 16 && t >= 300 ) { g_testPad[PAD_BUTTONS + 4] = 0; step = 17; phaseStart = now; }
		else if ( step == 17 && t >= 300 ) {
			Local::Check( "nor after, while it's still pushed", *(LONG*) (0x940B78 + 0x0C) == 0 && *(LONG*) (0x940B78 + 0x18) == 0 && (uint32_t) g_testArrowFrames == arrowFrames );
			SendKey( VK_RIGHT, false );
			*(LONG*) (g_testPad + 0x0C) = 0;
			*(LONG*) (g_testPad + 0x18) = 0;
			Local::Check( "letting go of L1 plays it", !g_wheelUi.open && g_emote.active && g_emote.emote == running );
			step = 18; phaseStart = now;
		}
		else if ( step == 18 && t >= 2000 ) { *(LONG*) (g_testPad + 0) = 1000; step = 19; phaseStart = now; }
		else if ( step == 19 && (!g_emote.active || t > 3000) ) {
			Local::Check( "the left stick ends it", !g_emote.active && strcmp( g_emoteLastEnd, "the left stick" ) == 0 );
			*(LONG*) (g_testPad + 0) = 0;
			g_testPadOn = false;
			step = 20; phaseStart = now;
		}
		// By keyboard : T, Tab (the second wheel), 2 (Dizzy Walk).
		else if ( step == 20 && t >= 2500 ) { SendKey( 'T', true ); step = 21; phaseStart = now; }
		else if ( step == 21 && t >= 400 ) {
			Local::Check( "T : the wheel", WheelShown() && !g_wheelUi.byPad );
			RequestSnapshot( "emote_6_keyboard" );
			arrowFrames = (uint32_t) g_testArrowFrames;
			SendKey( VK_RIGHT, true );
			step = 120; phaseStart = now;
		}
		else if ( step == 120 && t >= 300 ) {
			Local::Check( "Right arrow : the slot on the right, the game not getting it", g_wheelUi.hover == 2 && (uint32_t) g_testArrowFrames == arrowFrames );
			SendKey( VK_TAB, true );
			step = 22; phaseStart = now;
		}
		else if ( step == 22 && t >= 200 ) {
			SendKey( VK_TAB, false );
			Local::Check( "Tab : the next wheel", g_wheelIndex == 1 );
			SendKey( '2', true );
			step = 23; phaseStart = now;
		}
		else if ( step == 23 && t >= 200 ) {
			SendKey( '2', false );
			SendKey( VK_RIGHT, false );
			Local::Check( "2 plays the second slot at once", !WheelShown() && g_emote.active && g_emote.emote == dizzy );
			SendKey( 'T', false );
			step = 24; phaseStart = now;
		}
		else if ( step == 24 && t >= 1500 ) {
			Local::Check( "letting go of T after it plays nothing more", g_emote.active && g_emote.emote == dizzy );
			RequestSnapshot( "emote_7_dizzy" );
			SendKey( 'W', true );   // Walking : it ends, and no other starts.
			step = 25; phaseStart = now;
		}
		else if ( step == 25 && t >= 600 ) {
			Local::Check( "walking ended it", !g_emote.active && strcmp( g_emoteLastEnd, "a key" ) == 0 );
			g_emoteRequest = jig;
			step = 26; phaseStart = now;
		}
		else if ( step == 26 && t >= 300 ) {
			Local::Check( "no emote while walking", !g_emote.active && strcmp( g_emoteRefused, "Stand still to emote" ) == 0 );
			SendKey( 'W', false );
			step = 27; phaseStart = now;
		}
		// The Emotes page : the ring (moving an emote round, emptying a slot), choosing an
		// emote (Esc backs out; search, the list), the preview puppet.
		else if ( step == 27 && t >= 1500 ) { SetMenuOpen( true ); step = 28; phaseStart = now; }
		else if ( step == 28 && t >= 500 ) {
			g_menu.TabCenter( 4, &g_testMouse.x, &g_testMouse.y );
			g_testMouse.active = true; g_testMouse.down = true;
			step = 29; phaseStart = now;
		}
		else if ( step == 29 && t >= 300 ) { g_testMouse.down = false; step = 30; phaseStart = now; }
		else if ( step == 30 && t >= 300 ) {
			g_testMouse.active = false;
			Local::Check( "the Emotes page", g_menu.CurrentPage() == 4 );
			InterlockedExchangeAdd( &g_menuKeys[MK_DOWN], 2 );
			InterlockedExchangeAdd( &g_menuKeys[MK_RIGHT], 2 );
			step = 31; phaseStart = now;
		}
		else if ( step == 31 && t >= 400 ) {
			Local::Check( "Down, Down, Right, Right : the ring, on slot 3", g_ringSlot == 2 && g_wheels[0].slot[2] == running );
			RequestSnapshot( "emote_8_page" );
			InterlockedIncrement( &g_menuKeys[MK_SRIGHT] );
			step = 32; phaseStart = now;
		}
		else if ( step == 32 && t >= 400 ) {
			Local::Check( "Shift + Right moves it round", g_ringSlot == 3 && g_wheels[0].slot[3] == running && g_wheels[0].slot[2] == breakdance );
			InterlockedIncrement( &g_menuKeys[MK_SLEFT] );
			step = 33; phaseStart = now;
		}
		else if ( step == 33 && t >= 400 ) {
			Local::Check( "Shift + Left puts it back", g_ringSlot == 2 && g_wheels[0].slot[2] == running && g_wheels[0].slot[3] == breakdance );
			InterlockedIncrement( &g_menuKeys[MK_DELETE] );
			step = 34; phaseStart = now;
		}
		else if ( step == 34 && t >= 400 ) {
			Local::Check( "Delete empties it", g_wheels[0].slot[2] < 0 );
			InterlockedIncrement( &g_menuKeys[MK_ACCEPT] );
			step = 35; phaseStart = now;
		}
		else if ( step == 35 && t >= 400 ) {
			Local::Check( "Enter : choosing an emote", g_chooser );
			InterlockedIncrement( &g_menuKeys[MK_BACK] );
			step = 36; phaseStart = now;
		}
		else if ( step == 36 && t >= 400 ) {
			Local::Check( "Esc : back to the wheel, the menu still open", !g_chooser && g_menu.IsOpen() );
			g_wheels[0].slot[2] = FindEmoteByName( "Idle" );   // Not a favourite : the list goes to All, on it.
			InterlockedIncrement( &g_menuKeys[MK_ACCEPT] );
			step = 37; phaseStart = now;
		}
		else if ( step == 37 && t >= 2500 ) {
			Local::Check( "the list starts on the slot's emote (in All), the preview plays it", g_emoteCategory == 1 && g_previewWant == FindEmoteByName( "Idle" ) && g_emotePreview && g_viewerPeer && g_viewerPeer->beast );
			RequestSnapshot( "emote_9_chooser" );
			InterlockedIncrement( &g_menuKeys[MK_PGDN] );
			step = 38; phaseStart = now;
		}
		else if ( step == 38 && t >= 1500 ) {
			Local::Check( "Page Down : further down the list, the preview follows", g_listCursor > 8 && g_previewWant != FindEmoteByName( "Idle" ) );
			RequestSnapshot( "emote_10_pagedown" );
			g_listCursor = 0;   // Back to the top ("Empty slot"), then Up : out of the list, to Category, then Search.
			InterlockedIncrement( &g_menuKeys[MK_UP] );
			step = 39; phaseStart = now;
		}
		else if ( step == 39 && t >= 300 ) { InterlockedIncrement( &g_menuKeys[MK_UP] ); step = 40; phaseStart = now; }
		else if ( step == 40 && t >= 300 ) { InterlockedIncrement( &g_menuKeys[MK_ACCEPT] ); step = 41; phaseStart = now; }
		else if ( step == 41 && t >= 300 ) {
			Local::Check( "Enter on Search : typing", g_menu.IsTyping() );
			for ( const char* c = "Dizzy W"; *c; c++ ) AddTyped( *c );
			step = 42; phaseStart = now;
		}
		else if ( step == 42 && t >= 300 ) {
			Local::Check( "typed : it searches as it goes", strcmp( g_emoteSearch, "Dizzy W" ) == 0 );
			InterlockedIncrement( &g_menuKeys[MK_ACCEPT] );
			step = 43; phaseStart = now;
		}
		else if ( step == 43 && t >= 300 ) { InterlockedIncrement( &g_menuKeys[MK_DOWN] ); step = 44; phaseStart = now; }
		else if ( step == 44 && t >= 300 ) { InterlockedIncrement( &g_menuKeys[MK_DOWN] ); step = 45; phaseStart = now; }
		else if ( step == 45 && t >= 300 ) { InterlockedIncrement( &g_menuKeys[MK_DOWN] ); step = 46; phaseStart = now; }
		else if ( step == 46 && t >= 1500 ) {
			Local::Check( "Down, Down, Down : the list, its first match", g_previewWant == dizzy );
			RequestSnapshot( "emote_11_search" );
			InterlockedIncrement( &g_menuKeys[MK_ACCEPT] );
			step = 47; phaseStart = now;
		}
		else if ( step == 47 && t >= 400 ) {
			Local::Check( "Enter : it's in the slot, back to the wheel", g_wheels[0].slot[2] == dizzy && !g_chooser );
			RequestSnapshot( "emote_12_chosen" );
			DefaultWheel( 0 );
			InterlockedIncrement( &g_menuKeys[MK_BACK] );
			step = 48; phaseStart = now;
		}
		else if ( step == 48 && t >= 1000 ) {
			Local::Check( "Esc closes the menu, the preview's puppet is parked", !g_menu.IsOpen() && !g_emotePreview && g_parkedPeer );
			RequestSnapshot( "emote_13_closed" );
			InterlockedExchange( &g_autotestDone, failures ? 2 : 1 );
		}
		break;
	}
	case 1200: {   // Hide and seek, one game : a hider's time goes on, a seeker's stops; the room hears it; the file keeps it.
		static int failures = 0;
		static uint32_t startMs = 0, stopMs = 0;
		DWORD t = now - phaseStart;
		struct Local { static void Check( const char* what, bool ok ) { mp_log( "[AT] HS : %s : %s\n", what, ok ? "ok" : "WRONG" ); if ( !ok ) failures++; } };
		int me = g_transport ? g_transport->PlayerId() : -1;
		if ( step == 0 && t >= 2000 ) {
			g_testHideReport = true;
			g_gameMode = MODE_HIDE_AND_SEEK;
			Local::Check( "everyone starts as a seeker", g_myRole == ROLE_SEEKER );
			startMs = g_myHideMs;
			SetMyRole( ROLE_HIDER );
			step = 1;
		}
		else if ( step == 1 && t >= 7000 ) {
			uint32_t gained = g_myHideMs - startMs;
			mp_log( "[AT] HS : 5 s as a hider : %u ms more.\n", gained );
			Local::Check( "a hider's time goes on", gained >= 4000 && gained <= 6000 );
			bool heard = me >= 0 && g_players[me].present && g_players[me].playsHs && g_players[me].role == ROLE_HIDER && g_myHideMs - g_players[me].hideMs < 2500;
			mp_log( "[AT] HS : the room hears : role %d, %u ms (me %d).\n", me >= 0 ? g_players[me].role : -1, me >= 0 ? g_players[me].hideMs : 0, me );
			Local::Check( "the room hears our role and time", heard );
			RequestSnapshot( "hs_1_hider" );
			SetMyRole( ROLE_SEEKER );
			stopMs = g_myHideMs;
			step = 2;
		}
		else if ( step == 2 && t >= 10000 ) {
			Local::Check( "a seeker's time stops (and stays)", g_myHideMs == stopMs && stopMs > startMs );
			char path[MAX_PATH];
			HideTimePath( path );
			FILE* f = 0;
			unsigned long saved = 0;
			if ( fopen_s( &f, path, "r" ) == 0 && f ) { fscanf_s( f, "%lu", &saved ); fclose( f ); }
			Local::Check( "the file keeps it", saved == g_myHideMs );
			RequestSnapshot( "hs_2_seeker" );
			step = 3;
		}
		// The shortcuts (we're a seeker now). Items on the hotbar first, so the arrows have
		// something to bring out : Right alone does, Shift + Right swaps and doesn't.
		static int itemBefore = -1;
		if ( step == 3 && t >= 11000 ) {
			for ( int slot = 0; slot < 4; slot++ ) InventoryItem::setItemInHotbar( slot, slot == 3 ? InventoryItem::TORCH : InventoryItem::PICKAXES );
			itemBefore = GetHeldItem( harry );
			InterlockedExchange( &g_testArrowFrames, 0 );
			SendKey( VK_RIGHT, true ); step = 4;
		}
		else if ( step == 4 && t >= 11300 ) { SendKey( VK_RIGHT, false ); step = 5; }
		else if ( step == 5 && t >= 13000 ) {
			int item = GetHeldItem( harry );
			mp_log( "[AT] HS : Right arrow alone : held item %d -> %d, the game got it in %d polls.\n", itemBefore, item, g_testArrowFrames );
			Local::Check( "the Right arrow alone reaches the game", g_testArrowFrames > 0 );
			itemBefore = item;
			InterlockedExchange( &g_testArrowFrames, 0 );
			SendKey( VK_LSHIFT, true ); step = 6;
		}
		else if ( step == 6 && t >= 13300 ) { SendKey( VK_RIGHT, true ); step = 7; }
		else if ( step == 7 && t >= 13600 ) { SendKey( VK_RIGHT, false ); step = 8; }
		else if ( step == 8 && t >= 13900 ) { SendKey( VK_LSHIFT, false ); step = 9; }
		else if ( step == 9 && t >= 15500 ) {
			int item = GetHeldItem( harry );
			Local::Check( "Left Shift + Right : a hider", g_myRole == ROLE_HIDER );
			mp_log( "[AT] HS : with Shift, the game got the Right arrow in %d polls.\n", g_testArrowFrames );
			Local::Check( "and the arrow didn't reach the game (no item)", g_testArrowFrames == 0 && item == itemBefore );
			RequestSnapshot( "hs_7_shortcut_hud" );
			// The controller : L1 held, the d-pad's left.
			memset( g_testPad, 0, sizeof(g_testPad) );
			for ( int i = 0; i < 4; i++ ) *(DWORD*) (g_testPad + PAD_HAT + 4 * i) = 0xFFFFFFFF;
			g_testPadOn = true;
			InterlockedExchange( &g_testHatFrames, 0 );
			step = 10;
		}
		else if ( step == 10 && t >= 16000 ) { g_testPad[PAD_BUTTONS + 4] = 0x80; step = 11; }
		else if ( step == 11 && t >= 16300 ) { *(DWORD*) (g_testPad + PAD_HAT) = 27000; step = 12; }
		else if ( step == 12 && t >= 16600 ) { *(DWORD*) (g_testPad + PAD_HAT) = 0xFFFFFFFF; step = 13; }
		else if ( step == 13 && t >= 16900 ) { g_testPad[PAD_BUTTONS + 4] = 0; step = 14; }
		else if ( step == 14 && t >= 18500 ) {
			int item = GetHeldItem( harry );
			Local::Check( "L1 + d-pad left : a seeker again", g_myRole == ROLE_SEEKER );
			mp_log( "[AT] HS : with L1, the game got the d-pad in %d polls.\n", g_testHatFrames );
			Local::Check( "and the d-pad didn't reach the game (no item)", g_testHatFrames == 0 && item == itemBefore );
			InterlockedExchange( &g_testHatFrames, 0 );
			*(DWORD*) (g_testPad + PAD_HAT) = 9000;   // The d-pad alone : it does reach the game.
			step = 15;
		}
		else if ( step == 15 && t >= 18800 ) { *(DWORD*) (g_testPad + PAD_HAT) = 0xFFFFFFFF; step = 16; }
		else if ( step == 16 && t >= 20500 ) {
			mp_log( "[AT] HS : d-pad right alone : the game got it in %d polls (held item %d -> %d).\n", g_testHatFrames, itemBefore, GetHeldItem( harry ) );
			Local::Check( "the d-pad alone reaches the game", g_testHatFrames > 0 );
			g_testPadOn = false;
			g_gameMode = MODE_OFF;
			step = 17;
		}
		else if ( step == 17 && t >= 21500 ) {
			Local::Check( "game mode off : the timer and the HUD stop", !PlayingHs() );
			InterlockedExchange( &g_autotestDone, failures ? 2 : 1 );
		}
		break;
	}
	case 1210: {   // Hide and seek, two games : 2 runs off and hides; 3 (a seeker) makes the same run later, into 2 : caught.
		static int failures = 0;
		static uint32_t hiddenAt = 0, caughtMs = 0;
		static DWORD lastLog = 0, caughtAt = 0;
		static int startHealth = 0;
		DWORD t = now - phaseStart;
		g_puppetOffset.x = 0.0F;   // Puppets where the players are, as in play (not beside them).
		g_gameMode = g_hsTest == 4 ? MODE_OFF : MODE_HIDE_AND_SEEK;   // 4 : the "seeker" isn't playing.
		bool expectCatch = g_hsTest == 2;
		struct Local { static void Check( const char* what, bool ok ) { mp_log( "[AT] HS : %s : %s\n", what, ok ? "ok" : "WRONG" ); if ( !ok ) failures++; } };
		int me = g_transport ? g_transport->PlayerId() : -1;
		if ( now - lastLog >= 500 ) {
			lastLog = now;
			mp_log( "[AT] HS %5u ms : %s, %u ms hidden, health %d, nearest seeker %.1f\n", t, g_myRole == ROLE_HIDER ? "hider" : "seeker", g_myHideMs, harry->m_healthValue, g_testNearestSeeker );
		}
		if ( g_hsTest == 2 || g_hsTest == 5 ) {
			if ( step == 0 && t >= 12000 ) { mp_log( "[AT] HS : running off.\n" ); InterlockedExchange( &g_holdMs, 2500 ); InterlockedExchange( &g_holdKey, 'W' ); step = 1; }
			else if ( step == 1 && t >= 17000 ) {
				startHealth = harry->m_healthValue;
				SetMyRole( ROLE_HIDER );
				hiddenAt = g_myHideMs;
				step = 2;
			}
			else if ( step == 2 && t >= 22000 ) {
				if ( expectCatch ) Local::Check( "a seeker far off doesn't catch", g_myRole == ROLE_HIDER && g_testNearestSeeker > CATCH_DISTANCE );
				else Local::Check( "someone not playing isn't a seeker", g_myRole == ROLE_HIDER && g_testNearestSeeker < 0.0F );
				int other = -1;
				for ( int i = 0; i < MAX_PEERS; i++ ) if ( i != me && g_players[i].present ) other = i;
				if ( expectCatch ) Local::Check( "we see the seeker's name tag", other >= 0 && !HiddenFromMe( other ) );
				Local::Check( "the hide time goes on", g_myHideMs - hiddenAt >= 4000 );
				RequestSnapshot( "hs_3_hiding" );
				step = 3;   // Waiting for the seeker.
			}
			else if ( step == 3 && !expectCatch && t >= 45000 ) { Local::Check( "not caught by someone not playing", g_myRole == ROLE_HIDER ); step = 5; }
			else if ( step == 3 && g_myRole == ROLE_SEEKER && !caughtAt ) {
				if ( !expectCatch ) { Local::Check( "not caught by someone not playing", false ); step = 5; break; }
				caughtAt = now; caughtMs = g_myHideMs;
				Local::Check( "touching the seeker catches us, Harry dies", harry->m_healthValue <= 0 );
				RequestSnapshot( "hs_4_caught" );
				step = 4;
			}
			else if ( step == 3 && t >= 45000 ) { Local::Check( "caught once the seeker came", false ); step = 5; }
			else if ( step == 4 && now - caughtAt >= 15000 ) {
				Local::Check( "back after dying, health full again", harry->m_healthValue > 0 && harry->m_healthValue >= startHealth );
				Local::Check( "a caught hider's time stops", g_myHideMs == caughtMs );
				RequestSnapshot( "hs_5_back" );
				step = 5;
			}
			else if ( step == 5 && t >= 65000 ) InterlockedExchange( &g_autotestDone, failures ? 2 : 1 );
		}
		else {   // 3 : the seeker. Both games start on the same spot, facing the same way : the same run ends where 2 is.
			if ( step == 0 && t >= 30000 ) {
				int other = -1;
				for ( int i = 0; i < MAX_PEERS; i++ ) if ( i != me && g_players[i].present ) other = i;
				Local::Check( "the hider's name and whereabouts are hidden from us", other >= 0 && HiddenFromMe( other ) );
				RequestSnapshot( "hs_6_seeker_view" );
				mp_log( "[AT] HS : seeking (the hider's run).\n" ); InterlockedExchange( &g_holdMs, 2500 ); InterlockedExchange( &g_holdKey, 'W' ); step = 1;
			}
			else if ( step == 1 && t >= 60000 ) {
				if ( g_hsTest == 4 ) Local::Check( "not playing : no catch", g_testCaughtBy < 0 );
				else {
					Local::Check( "we hear that we caught them", g_testCaughtBy == me && g_testCaughtHider >= 0 && g_testCaughtHider != me );
					Local::Check( "the caught one is a seeker now", g_testCaughtHider >= 0 && g_players[g_testCaughtHider].role == ROLE_SEEKER );
					Local::Check( "their name shows again", g_testCaughtHider >= 0 && !HiddenFromMe( g_testCaughtHider ) );
				}
				step = 2;
			}
			else if ( step == 2 && t >= 65000 ) InterlockedExchange( &g_autotestDone, failures ? 2 : 1 );
		}
		break;
	}
	case 700: {   // Outfits : the puppet wears each in turn; then ours, which a level load puts on Harry.
		static int failures = 0;
		DWORD t = now - phaseStart;
		EIEvilHarryP* b = g_cur->beast;
		if ( step < NUM_OUTFITS ) {
			if ( !b ) { InterlockedExchange( &g_autotestDone, 2 ); break; }
			g_testOutfit = step;
			if ( t < 2500 ) break;

			bool ok = g_cur->shownOutfit == step && b->m_modelCRC == OUTFITS[step].model;
			mp_log( "[AT] Outfit : the puppet as %s : %s\n", OUTFITS[step].name, ok ? "ok" : "WRONG" );
			if ( !ok ) failures++;
			char name[40];
			sprintf_s( name, "outfit_%d_%s", step, OUTFITS[step].name );
			for ( char* c = name; *c; c++ ) if ( *c == ' ' ) *c = '_';
			RequestSnapshot( name );
			step++; phaseStart = now;
		}
		else if ( step == NUM_OUTFITS ) {   // Ours : Retro, then the level again.
			g_testOutfit = -1;
			g_myOutfit = 3;
			ApplyMyOutfit();
			ScheduleWorldLoad( (void*) 0x917028, Gizmod::getCurrentLevelCRC(), false );
			step++; phaseStart = now;
		}
		else if ( step == 100 ) {
			if ( t < 9000 ) break;
			bool ok = g_myWornOutfit == SNOWY_OUTFIT;
			mp_log( "[AT] Outfit : Snowy Harry chosen, after the level loaded again (model 0x%.8X) : %s\n", harry->m_modelCRC, ok ? "ok" : "WRONG" );
			if ( !ok ) failures++;
			RequestSnapshot( "outfit_6_snowy" );
			g_myOutfit = 0;   // Then Harry, in a snowy level (Spinja Lair) : still the jungle clothes.
			ApplyMyOutfit();
			ScheduleWorldLoad( (void*) 0x917028, 0x7652BAFC, false );
			step = 101; phaseStart = now;
		}
		else if ( step == 101 ) {
			if ( t < 12000 ) break;
			bool ok = Gizmod::getCurrentLevelCRC() == 0x7652BAFC && g_myWornOutfit == 0 && harry->m_modelCRC == OUTFITS[0].model;
			mp_log( "[AT] Outfit : Harry chosen, in a snowy level (model 0x%.8X) : %s\n", harry->m_modelCRC, ok ? "ok" : "WRONG" );
			if ( !ok ) failures++;
			RequestSnapshot( "outfit_7_harry_in_snow" );
			g_myOutfit = OUTFIT_DEFAULT;   // Then Default there : the snowy clothes, as the game has it.
			ApplyMyOutfit();
			ScheduleWorldLoad( (void*) 0x917028, 0x7652BAFC, false );
			step = 102; phaseStart = now;
		}
		else if ( step == 102 ) {
			if ( t < 12000 ) break;
			bool ok = g_myWornOutfit == SNOWY_OUTFIT;
			mp_log( "[AT] Outfit : Default chosen, in a snowy level (model 0x%.8X) : %s\n", harry->m_modelCRC, ok ? "ok" : "WRONG" );
			if ( !ok ) failures++;
			RequestSnapshot( "outfit_8_default_in_snow" );
			step = 103; phaseStart = now;
		}
		else if ( step == 103 ) {
			if ( t < 1000 ) break;
			InterlockedExchange( &g_autotestDone, failures ? 2 : 1 );
		}
		else if ( step == NUM_OUTFITS + 1 ) {
			if ( t < 9000 ) break;
			bool ok = g_myWornOutfit == 3 && harry->m_modelCRC == OUTFITS[3].model;
			mp_log( "[AT] Outfit : Harry after the level loaded again (model 0x%.8X) : %s\n", harry->m_modelCRC, ok ? "ok" : "WRONG" );
			if ( !ok ) failures++;
			SpawnPuppet( harry );   // It replays us : Retro too.
			static const uint8_t TEST[RETRO_PARTS][3] = { { 245, 245, 245 }, { 255, 161, 123 }, { 220, 50, 40 }, { 50, 100, 220 } };
			memcpy( g_myColors, TEST, sizeof(g_myColors) );   // White hair, red shirt, blue trousers.
			g_retroSwaps = g_retroMatched = g_retroUnmatched = 0;
			step++; phaseStart = now;
		}
		else {
			if ( t < 3000 ) break;
			bool ok = b && g_cur->shownOutfit == 3 && g_retroSwaps > 100 && g_retroUnmatched * 50 < g_retroMatched;
			mp_log( "[AT] Outfit : the puppet that replays us, both in our colours (%d pieces drawn with them; owner found for %d, not for %d) : %s\n",
				g_retroSwaps, g_retroMatched, g_retroUnmatched, ok ? "ok" : "WRONG" );
			if ( !ok ) failures++;
			RequestSnapshot( "outfit_5_ours" );
			g_myOutfit = SNOWY_OUTFIT;   // Then Snowy Harry, in a level that isn't snowy.
			ApplyMyOutfit();
			ScheduleWorldLoad( (void*) 0x917028, Gizmod::getCurrentLevelCRC(), false );
			step = 100; phaseStart = now;
		}
		break;
	}
	case 600: {   // The menu : keys, the mouse, and Harry standing still meanwhile.
		static Vector3f from;
		static int failures = 0;
		static const int START_DELAY = g_delayIndex;
		static const int START_FADE = g_tagFadeFrom;
		static uint8_t shirtBefore[3];
		DWORD t = now - phaseStart;
		struct Local {
			static float Moved( EIHarry* h, const Vector3f& from ) {
				float dx = h->m_position.x - from.x, dy = h->m_position.y - from.y;
				return sqrtf( dx * dx + dy * dy );
			}
			static void Check( const char* what, bool ok ) {
				mp_log( "[AT] Menu : %s : %s\n", what, ok ? "ok" : "WRONG" );
				if ( !ok ) failures++;
			}
			static void HoldW() { InterlockedExchange( &g_holdMs, 1200 ); InterlockedExchange( &g_holdKey, 'W' ); }
		};
		switch ( step ) {
		case 0:
			step++; break;
		case 1:
			InterlockedIncrement( &g_reqMenu ); step++; phaseStart = now; break;
		case 2:   // Open, on the first page.
			if ( t < 1000 ) break;
			Local::Check( "F1 opens it", g_menu.IsOpen() && g_menu.CurrentPage() == 0 );
			RequestSnapshot( "menu_1_players" );
			InterlockedIncrement( &g_menuKeys[MK_NEXT] ); step = 20; phaseStart = now; break;
		case 20:   // Tab : Gamemodes opens, on Hide and seek. Enter : enabled.
			if ( t < 700 ) break;
			g_gameMode = MODE_OFF;
			Local::Check( "Tab goes to the next page (Hide and seek, under Gamemodes)", g_menu.CurrentPage() == 1 && g_menu.GroupOpen( 0 ) );
			InterlockedIncrement( &g_menuKeys[MK_ACCEPT] ); step = 24; phaseStart = now; break;
		case 24:   // Down to the role, right : a hider.
			if ( t < 400 ) break;
			Local::Check( "Enter on Enabled : Hide and seek on", g_gameMode == MODE_HIDE_AND_SEEK && g_menu.IsOpen() );
			InterlockedIncrement( &g_menuKeys[MK_DOWN] ); InterlockedIncrement( &g_menuKeys[MK_RIGHT] ); step = 25; phaseStart = now; break;
		case 25:
			if ( t < 400 ) break;
			Local::Check( "Right on the role : a hider", g_myRole == ROLE_HIDER );
			InterlockedIncrement( &g_menuKeys[MK_RIGHT] ); step = 26; phaseStart = now; break;
		case 26:
			if ( t < 400 ) break;
			Local::Check( "Right again : a seeker", g_myRole == ROLE_SEEKER );
			Local::Check( "Left Shift's key code and name", VkToDik( VK_LSHIFT ) == 0x2A && strcmp( KeyName( 0x2A ), "Left Shift" ) == 0 && VkToDik( VK_RCONTROL ) == 0x9D );
			g_gameMode = MODE_OFF;
			InterlockedIncrement( &g_menuKeys[MK_NEXT] ); step = 3; phaseStart = now; break;
		case 3:   // Tab : Appearance, with Retro Harry's colours showing. Down three times, right : another shirt.
			if ( t < 100 ) { g_myOutfit = RETRO_OUTFIT; break; }
			if ( t < 700 ) break;
			{
				float x, y;
				Local::Check( "Tab goes to Appearance; Gamemodes closes", g_menu.CurrentPage() == 2 && !g_menu.GroupOpen( 0 ) && !g_menu.TabCenter( 1, &x, &y ) );
			}
			for ( int i = 0; i < 3; i++ ) InterlockedIncrement( &g_menuKeys[MK_DOWN] );
			step = 30; phaseStart = now; break;
		case 30:
			if ( t < 300 ) break;
			memcpy( shirtBefore, g_myColors[RETRO_SHIRT], 3 );
			InterlockedIncrement( &g_menuKeys[MK_RIGHT] ); step = 31; phaseStart = now; break;
		case 31:   // Tab again : Display. Enter : name tags off.
			if ( t < 500 ) break;
			Local::Check( "Right picks the next shirt colour", memcmp( g_myColors[RETRO_SHIRT], shirtBefore, 3 ) != 0 );
			RequestSnapshot( "menu_1b_appearance" );
			step = 34; phaseStart = now; break;
		case 34:   // Down three times : the shirt's colour row. A pasted hex code.
			if ( t < 300 ) break;
			for ( int i = 0; i < 3; i++ ) InterlockedIncrement( &g_menuKeys[MK_DOWN] );
			step = 35; phaseStart = now; break;
		case 35:
			if ( t < 300 ) break;
			strcpy_s( g_testPaste, "#123456" );
			step = 36; phaseStart = now; break;
		case 36:   // Enter, then typed : "10, 20, 30".
			if ( t < 300 ) break;
			Local::Check( "Pasting #123456 sets the shirt", g_myColors[RETRO_SHIRT][0] == 0x12 && g_myColors[RETRO_SHIRT][1] == 0x34 && g_myColors[RETRO_SHIRT][2] == 0x56 );
			InterlockedIncrement( &g_menuKeys[MK_ACCEPT] );
			step = 37; phaseStart = now; break;
		case 37:
			if ( t < 300 ) break;
			Local::Check( "Enter starts typing", g_menu.IsTyping() );
			for ( const char* c = "10, 20, 30"; *c; c++ ) AddTyped( *c );
			step = 38; phaseStart = now; break;
		case 38:
			if ( t < 300 ) break;
			RequestSnapshot( "menu_1c_typing" );
			InterlockedIncrement( &g_menuKeys[MK_ACCEPT] );
			step = 39; phaseStart = now; break;
		case 39:
			if ( t < 400 ) break;
			Local::Check( "Typing 10, 20, 30 sets it", !g_menu.IsTyping() && g_myColors[RETRO_SHIRT][0] == 10 && g_myColors[RETRO_SHIRT][1] == 20 && g_myColors[RETRO_SHIRT][2] == 30 );
			RequestSnapshot( "menu_1d_colour" );
			step = 33; phaseStart = now; break;
		case 33:
			if ( t < 300 ) break;
			InterlockedIncrement( &g_menuKeys[MK_NEXT] ); step = 32; phaseStart = now; break;
		case 32:
			if ( t < 700 ) break;
			Local::Check( "Tab again goes to Display", g_menu.CurrentPage() == 3 );
			InterlockedIncrement( &g_menuKeys[MK_ACCEPT] ); step = 4; phaseStart = now; break;
		case 4:   // Down twice, right : the next playback delay.
			if ( t < 600 ) break;
			Local::Check( "Enter turns name tags off", !g_showTags );
			InterlockedIncrement( &g_menuKeys[MK_DOWN] ); InterlockedIncrement( &g_menuKeys[MK_DOWN] ); step++; phaseStart = now; break;
		case 5:
			if ( t < 300 ) break;
			InterlockedIncrement( &g_menuKeys[MK_RIGHT] ); step++; phaseStart = now; break;
		case 6:   // Down, right : name tags start fading further away.
			if ( t < 500 ) break;
			Local::Check( "Right changes the playback delay", g_delayIndex == (START_DELAY + 1) % 6 );
			InterlockedIncrement( &g_menuKeys[MK_DOWN] ); step = 61; phaseStart = now; break;
		case 61:
			if ( t < 300 ) break;
			InterlockedIncrement( &g_menuKeys[MK_RIGHT] ); step = 62; phaseStart = now; break;
		case 62:   // W held with the menu open : Harry stays.
			if ( t < 500 ) break;
			Local::Check( "Right moves where name tags start fading", g_tagFadeFrom == START_FADE + TAG_FADE_STEP && g_tagFadeTo > g_tagFadeFrom );
			RequestSnapshot( "menu_2_display" );
			from = harry->m_position; Local::HoldW(); step = 7; phaseStart = now; break;
		case 7:   // The mouse : click the last page's tab (Keys).
			if ( t < 1800 ) break;
			Local::Check( "Harry stays put while it's open", Local::Moved( harry, from ) < 0.5F );
			g_menu.TabCenter( 5, &g_testMouse.x, &g_testMouse.y );
			g_testMouse.active = true; g_testMouse.down = true; step++; phaseStart = now; break;
		case 8:   // Then the first row of the second page (name tags back on).
			if ( t < 500 ) break;
			g_testMouse.down = false;
			Local::Check( "Clicking a tab opens its page", g_menu.CurrentPage() == 5 );
			RequestSnapshot( "menu_3_keys" );
			step = 80; phaseStart = now; break;
		case 80:   // Click Gamemodes : it opens, on Hide and seek.
			if ( t < 500 ) break;
			g_menu.GroupCenter( 0, &g_testMouse.x, &g_testMouse.y ); g_testMouse.down = true; step = 81; phaseStart = now; break;
		case 81:
			if ( t < 300 ) break;
			g_testMouse.down = false;
			if ( t < 600 ) break;
			Local::Check( "Clicking Gamemodes opens it, on Hide and seek", g_menu.GroupOpen( 0 ) && g_menu.CurrentPage() == 1 );
			g_gameMode = MODE_HIDE_AND_SEEK;   // For the picture : the whole page.
			step = 82; phaseStart = now; break;
		case 82:
			if ( t < 400 ) break;
			RequestSnapshot( "menu_1i_gamemodes" );
			step = 83; phaseStart = now; break;
		case 83:   // Click it again : closed, no page showing.
			if ( t < 400 ) break;
			g_gameMode = MODE_OFF;
			g_menu.GroupCenter( 0, &g_testMouse.x, &g_testMouse.y ); g_testMouse.down = true; step = 84; phaseStart = now; break;
		case 84:
			if ( t < 300 ) break;
			g_testMouse.down = false;
			if ( t < 600 ) break;
			Local::Check( "Clicking Gamemodes again closes it, no page showing", !g_menu.GroupOpen( 0 ) && g_menu.CurrentPage() < 0 );
			RequestSnapshot( "menu_1j_no_page" );
			step = 9; phaseStart = now; break;
		case 9:
			if ( t < 500 ) break;
			g_menu.TabCenter( 3, &g_testMouse.x, &g_testMouse.y ); g_testMouse.down = true; step++; phaseStart = now; break;   // Display.
		case 10:
			if ( t < 300 ) break;
			g_testMouse.down = false;
			if ( t < 500 ) break;
			g_menu.TabCenter( 0, &g_testMouse.x, &g_testMouse.y );
			g_testMouse.x += 400.0F;   // Same height as the first tab : the first row.
			g_testMouse.down = true; step++; phaseStart = now; break;
		case 11:   // Esc closes it; Harry walks again.
			if ( t < 300 ) break;
			g_testMouse.down = false;
			if ( t < 600 ) break;
			Local::Check( "Clicking a row changes it (name tags back on)", g_showTags );
			RequestSnapshot( "menu_4_mouse" );
			g_testMouse.active = false;
			InterlockedIncrement( &g_menuKeys[MK_BACK] ); step++; phaseStart = now; break;
		case 12:
			if ( t < 1000 ) break;
			Local::Check( "Esc closes it", !g_menu.IsOpen() );
			from = harry->m_position; Local::HoldW(); step++; phaseStart = now; break;
		case 13:
			if ( t < 1800 ) break;
			Local::Check( "Harry walks again once it's closed", Local::Moved( harry, from ) > 1.0F );
			RequestSnapshot( "menu_5_closed" );
			g_delayIndex = START_DELAY;
			g_tagFadeFrom = START_FADE;
			InterlockedExchange( &g_autotestDone, failures ? 2 : 1 );
			break;
		}
		break;
	}
	case 300: {   // Animation viewer : keys pressed in turn, a picture after each.
		static const struct { DWORD at; volatile LONG* key; const char* shot; } SCRIPT[] = {
			{ 1500, &g_reqViewer, 0 },      { 3000, 0, "viewer_1_open" },
			{ 3500, &g_reqNext10, 0 },      { 5000, 0, "viewer_2_plus10" },
			{ 5500, &g_reqStarredOnly, 0 }, { 7000, 0, "viewer_3_starred" },
			{ 7500, &g_reqNext, 0 },        { 9000, 0, "viewer_4_next_starred" },
			{ 9500, &g_reqStar, 0 },        { 10500, 0, "viewer_5_unstarred" },
			{ 11000, &g_reqStar, 0 },       { 11500, &g_reqReplay, 0 },
			{ 12000, &g_reqViewer, 0 },     { 13500, 0, "viewer_6_closed" },
		};
		static const int NUM = sizeof(SCRIPT) / sizeof(SCRIPT[0]);
		if ( step >= NUM ) {
			bool ok = !g_animViewer && g_animNoteCount > 0;
			mp_log( "[AT] Viewer : %s (%d clips).\n", ok ? "closed again" : "FAIL : still open or no clips", g_animNoteCount );
			InterlockedExchange( &g_autotestDone, ok ? 1 : 2 );
			break;
		}
		if ( now - phaseStart < SCRIPT[step].at ) break;
		if ( SCRIPT[step].key ) InterlockedIncrement( SCRIPT[step].key );
		if ( SCRIPT[step].shot ) {
			RequestSnapshot( SCRIPT[step].shot );
			const AnimNote* n = g_animIndex < g_animNoteCount ? &g_animNotes[g_animIndex] : 0;
			mp_log( "[AT] Viewer %s : open %d, clip #%d 0x%.8X star %d, playing 0x%.8X on layer %d.\n", SCRIPT[step].shot,
				(int) g_animViewer, n ? n->num : -1, n ? n->crc : 0, n ? (int) n->star : -1,
				g_viewerPeer ? g_viewerPeer->mirrorClip : 0, g_cur->beast ? g_cur->beast->m_animIndex : -1 );
		}
		step++;
		break;
	}
	case 201:   // Animation survey : next clip.
		if ( step >= g_animClipCount ) {
			g_animSurveyClip = 0;
			mp_log( "[AT] ANIM : done, %d clips filmed.\n", g_animClipCount );
			InterlockedExchange( &g_autotestDone, 1 );
			break;
		}
		g_animSurveyClip = g_animClips[step].crc;
		phase = 202; phaseStart = now; snapped = false;
		lastHarryTicks = 0;   // Frames taken of this clip.
		break;
	case 202: {   // Animation survey : film it.
		static const int FRAMES = 6;
		static const DWORD FIRST_FRAME_MS = 200, FRAME_GAP_MS = 500;
		if ( lastHarryTicks >= FRAMES ) { step++; phase = 201; break; }
		if ( now - phaseStart < FIRST_FRAME_MS + (DWORD) lastHarryTicks * FRAME_GAP_MS || g_cropPending ) break;
		// Around the puppet : its name tag point is just above its head.
		const Peer* pe = g_cur;
		float cx = 880.0F, top = 300.0F;
		if ( pe->beast && pe->tagFrame ) { cx = pe->tagX; top = pe->tagY - 200.0F; }
		g_crop.left = (LONG) (cx - 260.0F); g_crop.right = g_crop.left + 520;
		g_crop.top = (LONG) top; g_crop.bottom = g_crop.top + 620;
		sprintf_s( g_cropName, "anim_%03d_f%d", step, (int) lastHarryTicks );
		InterlockedExchange( &g_cropPending, 1 );
		lastHarryTicks++;
		break;
	}

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
			if ( STEPS[step].upItem != KEEP ) harry->m_inventory->m_hotbar[0] = STEPS[step].upItem == EMPTY ? 0 : GetItemObject( STEPS[step].upItem );
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
		          && STEPS[step - 1].upItem == KEEP ) {
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
			EInstance* m0 = g_cur->beast ? g_cur->beast->m_pickaxeModel0 : 0;
			EInstance* m1 = g_cur->beast ? g_cur->beast->m_pickaxeModel1 : 0;
			const bool shown = (m0 && (m0->m_flags & INSTANCE_VISIBLE)) || (m1 && (m1->m_flags & INSTANCE_VISIBLE));
			int draws = g_puppetItemDraws - drawsAtStart;   // Puppet copies drawn over the last 0.9 s.
			int poses = g_puppetPoseCopies - posesAtStart;
			int want = STEPS[i].expect;
			// Put away : the copy outlives Harry's item.
			int prev = i > 0 ? STEPS[i - 1].expect : -1;
			bool putAway = want < 0 && prev >= 0 && STEPS[i].upItem == KEEP;
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
				g_cur->beast ? g_cur->beast->m_worldMatrix.data[3][2] : 0.0F );
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


#pragma optimize( "", off )
static int CrashTestRecurse( int depth )
{
	volatile char room[4096];
	room[0] = (char) depth;
	return CrashTestRecurse( depth + 1 ) + room[0];
}
static void RunCrashTest()
{
	mp_log( "[AT] Crash test %d : crashing now.\n", g_crashTest );
	if ( g_crashTest == 1 ) *(volatile int*) 0x3B7FB1B1 = 1;
	if ( g_crashTest == 2 ) { volatile int zero = 0; volatile int x = 1 / zero; (void) x; }
	if ( g_crashTest == 3 ) CrashTestRecurse( 0 );
}
#pragma optimize( "", on )

// A hider touched by a seeker (their puppet, as we show it) : Harry dies the game's
// way (he comes back at the last loadzone), and we're a seeker from now on.

static void CheckCaught( EIHarry* harry )
{
	g_testNearestSeeker = -1.0F;
	int me = g_transport ? g_transport->PlayerId() : -1;
	if ( g_testHideReport ) {   // Autotest : how far the others' puppets are, each second.
		static uint32_t lastReport = 0;
		uint32_t now = GetTickCount();
		for ( int i = 0; i < MAX_PEERS && now - lastReport >= 1000; i++ ) {
			const Peer& p = g_peers[i];
			if ( i == me || !p.beast ) continue;

			const Vector3f& s = p.beast->EIBeast::m_position;
			mp_log( "[AT] HS : player %d's puppet is (%.2f, %.2f, %.2f) from Harry.\n", i, s.x - harry->m_position.x, s.y - harry->m_position.y, s.z - harry->m_position.z );
			lastReport = now;
		}
	}
	if ( !PlayingHs() || g_myRole != ROLE_HIDER || me < 0 || harry->m_healthValue <= 0 ) return;
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		const PlayerInfo& info = g_players[i];
		const Peer& p = g_peers[i];
		if ( i == me || !info.present || !info.playsHs || info.role != ROLE_SEEKER || !p.beast || !p.target ) continue;

		const Vector3f& s = p.beast->EIBeast::m_position;
		const float dx = s.x - harry->m_position.x, dy = s.y - harry->m_position.y, dz = s.z - harry->m_position.z;
		const float d = sqrtf( dx * dx + dy * dy + dz * dz );
		if ( g_testNearestSeeker < 0.0F || d < g_testNearestSeeker ) g_testNearestSeeker = d;
		if ( d >= CATCH_DISTANCE ) continue;

		mp_log( "[HS] Caught by player %d (\"%s\"), %.2f away : Harry dies.\n", i, info.name, d );
		Toast( "%s caught you!", info.name );
		if ( !g_catchNumber ) g_catchNumber = GetTickCount() | 1;
		g_caughtOut.seeker = i;
		g_caughtOut.number = ++g_catchNumber;
		g_caughtOut.left = CAUGHT_SENDS;
		g_caughtOut.lastSent = 0;
		SetMyRole( ROLE_SEEKER );
		harry->m_healthValue = 0;
		return;
	}
}

static void __stdcall _EIHarry_Tick_Hook( EIHarry* self )
{
	if ( g_crashTest && g_harryTicks == 300 ) RunCrashTest();

	EIHarry_Tick( self );

	if ( InterlockedIncrement(&g_harryTicks) == 1 ) {
		mp_log( "[MP] Harry tick hook is running (this 0x%.8X, global Harry 0x%.8X).\n", self, Gizmod::getHarry() );
		if ( !g_autotest ) Toast( "F1 : multiplayer menu" );
	}

	if ( self != Gizmod::getHarry() ) return;

	// Another level since the puppet was spawned. The load events don't cover every
	// way of changing levels (the "Go to" menu, loading a save), this does.
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		const Peer& p = g_peers[i];
		if ( (p.beast || p.limbo) && (self->m_world != p.world || Gizmod::getCurrentLevelCRC() != p.level) ) {
			ForgetPuppet( self );
			break;
		}
	}
	DropLostPuppets();
	ReplaceStalledPuppets();
	CheckLevelLeaving( self );

	if ( !g_origPresent ) {   // The overlay draws as frames are presented.
		HookPresent();
		InterlockedExchange( &g_reqMenu, 0 );   // An F1 pressed before there was anything to draw (the main menu) doesn't count.
	}
	HideTick();
	EmoteTick( self );
	PushLocalSnapshot( self );
	NetUpdate();
	CheckCaught( self );
	// Another player in this level : show them (Evil Harry appears on his own).
	// (The autotest spawns the main peer's itself, when its steps need it.)
	if ( g_networked ) {
		uint32_t now = NetNowMs();
		for ( int i = 0; i < MAX_PEERS; i++ ) {
			Peer* p = &g_peers[i];
			if ( g_autotest && p == MainPeer() && g_pitTest < 4 && g_distTest < 2 && g_hsTest < 2 ) continue;   // (The two-game pit test : as in play.)
			if ( p->beast || p->limbo || !p->target || now - p->lastSpawnTry < 1000 ) continue;
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
MAKE_THISCALL_WRAPPER( EIHarry_Tick_Hook, _EIHarry_Tick_Hook );

// The puppet wears its player's outfit (ours, when it replays us).
GET_METHOD( 0x60F770, void, EICharacter_SetModel, EICharacter*, uint32_t );
static void ApplyPuppetOutfit( EIEvilHarryP* p )
{
	int me = g_transport ? g_transport->PlayerId() : -1;
	bool ours = g_cur->id == me || !g_networked;   // It replays us (echo, the loopback).
	if ( !ours && !g_players[g_cur->id].present && g_testOutfit < 0 ) return;   // They left : as it is.

	int outfit = ours ? g_myWornOutfit : g_players[g_cur->id].outfit;
	if ( g_testOutfit >= 0 ) outfit = g_testOutfit;
	if ( outfit == g_cur->shownOutfit ) return;

	uint32_t model = OUTFITS[outfit].model;
	p->m_modelCRC = model;
	p->m_characterCRC = model;
	EICharacter_SetModel( p, model );

	mp_log( "[MP] Player %d's puppet wears the %s outfit.\n", g_cur->id, OUTFITS[outfit].name );
	g_cur->shownOutfit = outfit;
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
	if ( IsPuppet(self) && !g_allowPuppetFreeze ) {
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
		if ( !owner.beast && !owner.limbo ) {
			owner.beast = self;
			owner.world = harry->m_world;
			owner.level = Gizmod::getCurrentLevelCRC();
			owner.lastLoggedState = owner.lastRequested = -2;
			owner.shownOutfit = -1;
			InterlockedIncrement( &g_orphansFound );
			mp_log( "[MP] Player %d's old puppet 0x%.8X ticked with nobody driving it : given back to them.\n", s.owner, self );
			return false;
		}
		if ( !s.reported ) {
			s.reported = true;
			InterlockedIncrement( &g_orphansFound );
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
	if ( g_testStallPuppet && self == g_testStalled ) return;   // Autotest : as if a pitfall caught it.
	HookThrownFromWater( self );
	Peer* peer = FindPeer( self );
	if ( !peer ) peer = ReclaimPuppet( self );
	if ( !peer && HandleOrphan( self ) ) return;
	if ( !peer ) peer = FindPeer( self );   // Given back to its player.
	if ( !peer ) {   // The game's own Evil Harry.
		EIBeast_Tick( self );
		return;
	}
	peer->lastTick = GetTickCount();
	peer->ticks++;
	CurrentPeer current( peer );

	if ( g_skipPuppetAI ) {
		// No AI, no animation : just place it.
		DrivePuppet( self );
		return;
	}

	// When Harry dies and comes back the game removes the level's loose models, the
	// puppet's pickaxes among them, and Evil Harry's tick crashes without them
	// (0x47B04F). Init() makes new ones (it only makes those that are missing).
	if ( !self->m_pickaxeModel0 || !self->m_pickaxeModel1 ) {
		mp_log( "[MP] Player %d's puppet lost its pickaxe models (Harry died ?) : making new ones.\n", g_cur->id );
		self->Init();
		g_cur->lastRequested = -2;
		g_cur->shownOutfit = -1;   // Init gave it our Harry's outfit.
		if ( !self->m_pickaxeModel0 || !self->m_pickaxeModel1 ) return;   // Still none : its tick would crash.
	}
	ApplyPuppetOutfit( self );

	UpdateLocomotion( self );
	if ( ForcedClip() && g_haveCamera ) {   // Animation survey, viewer : face the camera.
		const Matrix4f& cam = g_cameraWorld;
		g_cur->heading = atan2f( cam.data[3][1] - self->EIBeast::m_position.y, cam.data[3][0] - self->EIBeast::m_position.x );
	}

	if ( g_cur->forcedState >= 0 && self->m_stateIndexCurrent != g_cur->forcedState ) {
		if ( g_cur->lastRequested != g_cur->forcedState ) {
			mp_log( "[MP] Requesting state %d (current %d)...\n", g_cur->forcedState, self->m_stateIndexCurrent );
			g_cur->lastRequested = g_cur->forcedState;
		}
		self->m_forceNextState = true;
		self->m_nextState = g_cur->forcedState;
	}

	PrePositionPuppet( self );

	EIBeast_Tick( self );

	if ( g_autoLocomotion ) ApplyLocomotionAnim( self );

	if ( self->m_stateIndexCurrent != g_cur->lastLoggedState ) {
		mp_log( "[MP] Player %d's puppet is now in state %d.\n", g_cur->id, self->m_stateIndexCurrent );
		g_cur->lastLoggedState = self->m_stateIndexCurrent;
	}

	DrivePuppet( self );           // Then override where it is.
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
		LoadWheels( ini );
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

static uint32_t WithAlpha( uint32_t color, float alpha )
{
	if ( alpha < 0.0F ) alpha = 0.0F;
	if ( alpha > 1.0F ) alpha = 1.0F;
	return (color & 0x00FFFFFF) | ((uint32_t) (((color >> 24) & 0xFF) * alpha + 0.5F) << 24);
}

// Our outfit : the game's cheat flag for it (and none of the others). Harry changes
// when the next level loads.
static void ApplyMyOutfit()
{
	for ( int i = 1; i < NUM_OUTFITS; i++ ) if ( OUTFITS[i].cheatFlag ) *(uint8_t*) OUTFITS[i].cheatFlag = (i == g_myOutfit) ? 1 : 0;
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
	if ( g_autotest || !g_iniPath[0] ) return;
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
	SaveWheels( g_iniPath );
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
static bool HiddenFromMe( int id )
{
	int me = g_transport ? g_transport->PlayerId() : -1;
	return id != me && g_players[id].playsHs && g_players[id].role == ROLE_HIDER;
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
	if ( mode != MODE_HIDE_AND_SEEK ) SetMyRole( ROLE_SEEKER );   // Back as a seeker, next time.
	mp_log( "[HS] Game mode : %s.\n", MODE_NAMES[mode] );
	SaveSettings();
}

// A Windows key's DirectInput code (its scan code, + 0x80 for the extended ones).
static int VkToDik( int vk )
{
	switch ( vk ) {
	case VK_UP: return 0xC8; case VK_DOWN: return 0xD0; case VK_LEFT: return 0xCB; case VK_RIGHT: return 0xCD;
	case VK_RCONTROL: return 0x9D; case VK_RMENU: return 0xB8; case VK_INSERT: return 0xD2; case VK_DELETE: return 0xD3;
	case VK_HOME: return 0xC7; case VK_END: return 0xCF; case VK_PRIOR: return 0xC9; case VK_NEXT: return 0xD1;
	case VK_DIVIDE: return 0xB5; case VK_RSHIFT: return 0x36;
	}
	return (int) MapVirtualKeyA( vk, MAPVK_VK_TO_VSC ) & 0x7F;
}

static const char* KeyName( int dik )
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
		for ( int b = 0; g_padSeenAny && b < 32; b++ ) if ( g_padSeen[PAD_BUTTONS + b] & 0x80 ) { down = b; break; }
		if ( !g_bindArmed ) { g_bindArmed = down < 0 && !escape; return; }
		if ( escape ) { g_binding = BIND_NONE; return; }
		if ( down < 0 ) return;
		*g_bindTarget = down;
		mp_log( "[MP] %s button : %d.\n", g_bindTarget == &g_swapButton ? "Role swap" : "Emote wheel", down + 1 );
	}
	g_binding = BIND_NONE;
	SaveSettings();
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
	if ( g_testPaste[0] ) { strcpy_s( pasted, g_testPaste ); g_testPaste[0] = 0; in.pasted = pasted; }

	// The mouse, in the overlay's pixels (the window may be scaled).
	bool cursorShown = true;
	if ( g_testMouse.active ) {
		in.mouseX = g_testMouse.x; in.mouseY = g_testMouse.y; in.mouseInside = true; in.mouseDown = g_testMouse.down;
	}
	else {
		HWND wnd = *((HWND*) HWND_GAME_WINDOW);
		POINT p;
		RECT rc;
		if ( wnd && GameHasFocus() && GetCursorPos( &p ) && ScreenToClient( wnd, &p ) && GetClientRect( wnd, &rc ) && rc.right > 0 && rc.bottom > 0 ) {
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

	static char left[MAX_LIST_ROWS][96], right[MAX_LIST_ROWS][64];
	static uint32_t colors[MAX_LIST_ROWS];
	int rows = BuildPlayerRows( left, right, colors, "" );

	if ( !g_menu.IsOpen() ) g_binding = BIND_NONE;
	if ( g_binding != BIND_NONE ) {   // Waiting for a key or a button : the menu doesn't get them.
		PollBinding();
		in.up = in.down = in.left = in.right = in.accept = in.back = in.nextPage = in.prevPage = 0;
		in.del = in.pageUp = in.pageDown = in.shiftLeft = in.shiftRight = 0;
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
	if ( g_menu.Page( "Hide and seek" ) ) {
		g_menu.Section( "Settings" );
		bool on = g_gameMode == MODE_HIDE_AND_SEEK;
		if ( g_menu.Toggle( "Enabled", &on,
			"On : you're playing. You get a role, your hide time adds up while you're a hider, and seekers can catch you. "
			"Everyone who plays turns it on. Off : just see each other." ) ) SetGameMode( on ? MODE_HIDE_AND_SEEK : MODE_OFF );
		char hidden[16];
		if ( on ) {
			int role = g_myRole;
			static const char* const ROLES[] = { "Seeker", "Hider" };
			char swapHint[200];
			sprintf_s( swapHint, "Also without the menu : hold %s (controller) or %s (keyboard) and press left or right. "
				"Caught : you're a seeker. Only seekers have name tags.", PadButtonName( g_swapButton ), KeyName( g_swapKey ) );
			if ( g_menu.Choice( "Your role", &role, ROLES, 2, swapHint ) ) SetMyRole( role );
			FormatHideTime( g_myHideMs, hidden, sizeof(hidden) );
			g_menu.Info( "Your hide time", hidden );
			static uint32_t resetAsked = 0;
			bool asking = resetAsked && GetTickCount() - resetAsked < 4000;
			if ( g_menu.Action( asking ? "Reset my timer : press again to be sure" : "Reset my timer",
				"Your hide time back to 0:00. Only you can reset it : leaving, restarting and being caught keep it." ) ) {
				if ( asking ) {
					mp_log( "[HS] Hide time reset (was %u ms).\n", g_myHideMs );
					g_myHideMs = 0;
					SaveHideTime();
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
			if ( mine ? !on : (!info.present || !info.playsHs || info.role == ROLE_NONE) ) continue;
			int r = mine ? g_myRole : info.role;
			char name[48], where[64];
			FormatHideTime( mine ? g_myHideMs : info.hideMs, hidden, sizeof(hidden) );
			sprintf_s( name, "%s%s", mine ? g_myName : info.name, mine ? " (you)" : "" );
			uint32_t level = mine ? (g_haveLocal ? here : 0) : info.level;
			if ( HiddenFromMe( i ) ) strcpy_s( where, "hiding" );
			else if ( !mine && level && level == here ) strcpy_s( where, "here" );
			else strcpy_s( where, LevelName( level ) );
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
	if ( g_menu.Page( "Appearance" ) ) {
		static const int ORDER[NUM_OUTFITS + 1] = { OUTFIT_DEFAULT, 0, SNOWY_OUTFIT, 1, 2, RETRO_OUTFIT };   // Default, Harry, Snowy Harry, Nicole, Native, Retro Harry.
		static const char* names[NUM_OUTFITS + 1];
		int choice = 0;
		for ( int i = 0; i <= NUM_OUTFITS; i++ ) {
			names[i] = ORDER[i] < 0 ? "Default" : OUTFITS[ORDER[i]].name;
			if ( ORDER[i] == g_myOutfit ) choice = i;
		}
		if ( g_menu.Choice( "Outfit", &choice, names, NUM_OUTFITS + 1, "What you look like, to you and to the others. Changes the next time you go to another area. "
			"Default : as in the game, jungle or snowy clothes as the area has it. Harry : always the jungle clothes." ) ) {
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
			if ( g_menu.Colour( "Color", g_myColors[editing],
				"Enter : type a hex code (#DC3228) or red, green, blue (220, 50, 40). Ctrl + V pastes one, Ctrl + C copies this one." ) ) changed = true;
			static const char* const CHANNELS[3] = { "Red", "Green", "Blue" };
			for ( int ch = 0; ch < 3; ch++ ) {
				int v = g_myColors[editing][ch];
				if ( g_menu.Slider( CHANNELS[ch], &v, 0, 255, 5, "Left / Right, or click and drag." ) ) {
					g_myColors[editing][ch] = (uint8_t) v;
					changed = true;
				}
			}
			if ( g_menu.Action( "Reset colors to default", "Retro Harry's own colors, for all four parts." ) ) {
				memcpy( g_myColors, RETRO_DEFAULT, sizeof(g_myColors) );
				changed = true;
			}
			if ( changed ) {
				g_lastInfoSent = 0;   // The others hear about it now.
				SaveSettings();
			}
		}
	}
	if ( g_menu.Page( "Display" ) ) {
		static const char* const DELAYS[] = { "133 ms", "250 ms", "500 ms", "1 s", "1.5 s", "3 s" };
		bool changed = false;
		changed |= g_menu.Toggle( "Name tags", &g_showTags, "Names above the other players." );
		changed |= g_menu.Toggle( "Player list while playing", &g_showList, "Keeps the player list in the corner of the screen." );
		changed |= g_menu.Choice( "Playback delay", &g_delayIndex, DELAYS, sizeof(DELAYS) / sizeof(DELAYS[0]),
			"How far behind the others are shown. Shorter : closer to live, but jerkier on a bad connection." );
		if ( g_menu.Slider( "Name tags start fading at", &g_tagFadeFrom, 0, TAG_FADE_MAX - TAG_FADE_STEP, TAG_FADE_STEP,
			"Distance from the camera where the others' names start to fade. The players themselves stay in view." ) ) {
			if ( g_tagFadeTo < g_tagFadeFrom + TAG_FADE_STEP ) g_tagFadeTo = g_tagFadeFrom + TAG_FADE_STEP;
			changed = true;
		}
		if ( g_menu.Slider( "Name tags gone at", &g_tagFadeTo, TAG_FADE_STEP, TAG_FADE_MAX, TAG_FADE_STEP,
			"Distance from the camera where the others' names have faded out completely." ) ) {
			if ( g_tagFadeFrom > g_tagFadeTo - TAG_FADE_STEP ) g_tagFadeFrom = g_tagFadeTo - TAG_FADE_STEP;
			changed = true;
		}
		if ( changed ) SaveSettings();
	}
	bool emotes = g_menu.Page( "Emotes" );
	if ( emotes ) EmotesPage();
	else { g_chooser = false; g_previewWant = -2; }
	if ( g_menu.Page( "Keys" ) ) {
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
		g_menu.Info( "F5", "Spawn a puppet that copies you", Menu::DIM );
		g_menu.Info( "F6", "Puppet's animation state", Menu::DIM );
		g_menu.Info( "F7", "Next playback delay", Menu::DIM );
		g_menu.Info( "F8", "Puppet stays where it is", Menu::DIM );
	}
	g_menu.End();
	InterlockedExchange( &g_menuTyping, g_menu.IsTyping() ? 1 : 0 );
	if ( const char* copy = g_menu.CopyText() ) { WriteClipboard( copy ); g_lastCopied = GetTickCount(); }

	// Our own pointer where the game hides Windows'.
	if ( in.mouseInside && !cursorShown ) {
		OverlayRect( in.mouseX - 1.0F, in.mouseY - 1.0F, 10.0F, 10.0F, 0xFF000000 );
		OverlayRect( in.mouseX, in.mouseY, 8.0F, 8.0F, 0xFFFFFFFF );
	}
}

// Tells the hotkey thread and the keyboard poll whether the menu is open.
static void SyncMenuOpen()
{
	bool open = g_menu.IsOpen();
	if ( (g_menuOpen != 0) == open ) return;
	if ( open ) for ( int i = 0; i < MK_COUNT; i++ ) InterlockedExchange( &g_menuKeys[i], 0 );
	else g_menuBlockUntil = GetTickCount() + 300;
	InterlockedExchange( &g_menuOpen, open ? 1 : 0 );
	mp_log( "[MP] Menu %s.\n", open ? "opened" : "closed" );
}

static void SetMenuOpen( bool open )
{
	if ( open == g_menu.IsOpen() ) return;
	if ( open ) g_menu.Open(); else g_menu.Close();
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
	const EmoteWheel& wheel = g_wheels[g_wheelIndex];
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

static void DrawOverlay( IDirect3DDevice9* dev )
{
	if ( g_animSurvey ) return;   // Clean frames.
	uint32_t now = NetNowMs();
	if ( InterlockedExchange( &g_reqMenu, 0 ) ) SetMenuOpen( !g_menu.IsOpen() );
	bool any = g_showList || g_animViewer || g_menu.IsOpen() || PlayingHs() || WheelShown();
	for ( int i = 0; i < MAX_PEERS && !any; i++ ) any = g_peers[i].beast && g_peers[i].tagFrame && g_presentCount - g_peers[i].tagFrame <= TAG_GRACE_FRAMES;
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
		float tagX, tagY, tagDepth;
		if ( !g_showTags || g_menu.IsOpen() || WheelShown() || HiddenFromMe( p.id ) || !PlaceNameTag( p, dev, &tagX, &tagY, &tagDepth ) ) continue;
		float from = (float) g_tagFadeFrom, to = (float) g_tagFadeTo;
		float alpha = to > from ? 1.0F - (tagDepth - from) / (to - from) : (tagDepth < from ? 1.0F : 0.0F);
		if ( alpha <= 0.0F ) continue;
		const char* name = PlayerName( p.id );
		float w = OverlayTextWidth( name );
		float x = floorf( tagX - w * 0.5F ), y = floorf( tagY - h * 1.2F );
		OverlayRect( x - pad, y - pad * 0.5F, w + 2.0F * pad, h + pad, WithAlpha( 0x80000000, alpha ) );
		OverlayText( x, y, name, WithAlpha( 0xFFFFFFFF, alpha ) );
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
		FormatHideTime( g_myHideMs, hidden, sizeof(hidden) );
		const char* role = g_myRole == ROLE_HIDER ? "HIDER" : "SEEKER";
		sprintf_s( text, "%s   %s", role, hidden );
		float x = floorf( sw * (28.0F / 640.0F) + pad ), y = floorf( sh * 0.14F );   // The box's left : the first health box's (the game's HUD is laid out 640 wide).
		float w = OverlayTextWidth( text );
		OverlayRect( x - pad, y - pad * 0.5F, w + 2.0F * pad, h + pad, 0xA0000000 );
		OverlayText( x, y, role, g_myRole == ROLE_HIDER ? 0xFFFFE080 : 0xFFFFFFFF );
		OverlayText( x + w - OverlayTextWidth( hidden ), y, hidden, 0xFFFFFFFF );
	}

	if ( WheelShown() && !g_menu.IsOpen() ) DrawWheel( sw, sh, h );

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
typedef VOID (WINAPI *ExitProcessFunc)( UINT );
static ExitProcessFunc g_origExitProcess = ExitProcess;

static void LeaveRoom()
{
	if ( g_transport ) g_transport->Leave();
}

static void QuitGame( UINT code )
{
	mp_log( "[MP] The game is closing (exit code %u).\n", code );
	LeaveRoom();
	g_origExitProcess( code );
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
		LogModLoader();

		register_types();

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

		CreateThread( 0, 0, HotkeyThread, 0, 0, 0 );

		g_netLog = mp_log;
		NetConfig config;
		ReadNetConfig( &config );
		ApplyMyOutfit();   // Before the first level loads.
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

		static const uint8_t POLL_ORIGINAL[1] = { 0xE8 };
		if ( memcmp( (void*) POLL_KEYBOARD_CALL, POLL_ORIGINAL, 1 ) == 0 ) injector::MakeCALL( POLL_KEYBOARD_CALL, &PollKeyboard_Hook );
		else mp_log( "[MP] The keyboard poll call isn't what we expect : keys reach the game while the menu is open.\n" );
		static const uint8_t MODEL_ORIGINAL[6] = { 0x89, 0x81, 0x30, 0x01, 0x00, 0x00 };   // mov [ecx+0x130], eax.
		if ( memcmp( (void*) HARRY_MODEL_STORE, MODEL_ORIGINAL, 6 ) == 0 ) {
			injector::MakeCALL( HARRY_MODEL_STORE, &HarryModel_Hook );
			injector::MakeNOP( HARRY_MODEL_STORE + 5, 1 );
		}
		else mp_log( "[MP] Harry's model isn't stored where we expect : no Snowy Harry outfit for us.\n" );
		static const uint8_t PAD_ORIGINAL[5] = { 0xE8, 0xAE, 0x4A, 0x08, 0x00 };   // call 0x6E8660.
		if ( memcmp( (void*) POLL_GAMEPAD_CALL, PAD_ORIGINAL, 5 ) == 0 ) injector::MakeCALL( POLL_GAMEPAD_CALL, &PollGamepad_Hook );
		else mp_log( "[MP] The gamepad poll call isn't what we expect : the gamepad reaches the game while the menu is open.\n" );

		char env[8] = { 0, };
		if ( GetEnvironmentVariableA( "PTLE_MP_AUTOTEST", env, sizeof(env) ) && env[0] == '1' ) {
			g_autotest = true;
			g_background = !(GetEnvironmentVariableA( "PTLE_MP_FOREGROUND", env, sizeof(env) ) && env[0] == '1');
			g_puppetOffset.x = 6.0F;   // Stand beside Harry, so screenshots show whether it follows.
			g_showList = true;         // Screenshots show the player list.
			EnableAutoSkipCutscenes();
			if ( GetEnvironmentVariableA( "PTLE_MP_CRASHTEST", env, sizeof(env) ) ) {
				g_crashTest = strcmp( env, "div" ) == 0 ? 2 : strcmp( env, "stack" ) == 0 ? 3 : 1;
				mp_log( "[AT] Crash test (%s).\n", env );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_ICETEST", env, sizeof(env) ) && env[0] >= '1' && env[0] <= '3' ) {
				g_iceTest = env[0] - '0';
				if ( g_iceTest >= 2 ) AUTOTEST_LEVEL = 0x7652BAFC;   // Spinja Lair.
				g_allowPuppetFreeze = GetEnvironmentVariableA( "PTLE_MP_ICEALLOW", env, sizeof(env) ) && env[0] == '1';
				mp_log( "[AT] Ice test%s.\n", g_allowPuppetFreeze ? " (puppets may freeze)" : "" );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_PITTEST", env, sizeof(env) ) && env[0] >= '1' && env[0] <= '5' ) {
				g_pitTest = env[0] - '0';
				AUTOTEST_LEVEL = 0xEE8F6900;   // Crash Site : a pitfall just ahead of the start (Harry walks down -Y into it).
				g_puppetOffset.x = 0.0F;       // Where Harry was (a quarter second ago), as another player's.
				if ( g_pitTest == 2 ) g_puppetOffset.y = -45.0F;   // Ahead of him : it reaches the pit, he doesn't.
				mp_log( "[AT] Pitfall test.\n" );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_RAFTTEST", env, sizeof(env) ) && env[0] == '1' ) {
				g_raftTest = 1;
				AUTOTEST_LEVEL = 0x7652BAFC;   // Spinja Lair : Harry wades in on arrival.
				mp_log( "[AT] Raft test.\n" );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_DEATHTEST", env, sizeof(env) ) && env[0] == '1' ) {
				g_deathTest = true;
				mp_log( "[AT] Death test.\n" );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_AIMTEST", env, sizeof(env) ) && (env[0] == '1' || env[0] == '2') ) {
				g_aimTest = env[0] - '0';
				mp_log( "[AT] Aim test.\n" );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_DISTTEST", env, sizeof(env) ) && env[0] >= '1' && env[0] <= '3' ) {
				g_distTest = env[0] - '0';
				g_allowPuppetFade = GetEnvironmentVariableA( "PTLE_MP_FADEALLOW", env, sizeof(env) ) && env[0] == '1';
				{ char lv[16] = { 0, }; if ( GetEnvironmentVariableA( "PTLE_MP_LEVEL", lv, sizeof(lv) ) ) AUTOTEST_LEVEL = strtoul( lv, 0, 16 ); }   // Another level (its CRC, hex).
				mp_log( "[AT] Distance test.\n" );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_EMOTETEST", env, sizeof(env) ) && env[0] >= '1' && env[0] <= '9' ) {
				g_emoteTest = env[0] - '0';
				mp_log( "[AT] Emote test %d.\n", g_emoteTest );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_HSTEST", env, sizeof(env) ) && env[0] >= '1' && env[0] <= '9' ) {
				g_hsTest = env[0] - '0';
				g_testHideReport = true;
				mp_log( "[AT] Hide and seek test %d.\n", g_hsTest );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_SKINTEST", env, sizeof(env) ) && env[0] == '1' ) {
				g_skinTest = true;
				g_showList = false;
				mp_log( "[AT] Outfit test.\n" );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_MENUTEST", env, sizeof(env) ) && env[0] == '1' ) {
				g_menuTest = true;
				g_showList = false;
				mp_log( "[AT] Menu test.\n" );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_VIEWERTEST", env, sizeof(env) ) && env[0] == '1' ) {
				g_viewerTest = true;
				mp_log( "[AT] Animation viewer test.\n" );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_ANIMSURVEY", env, sizeof(env) ) && env[0] == '1' ) {
				g_animSurvey = true;
				g_puppetOffset.x = 6.0F;   // Placed beside Harry, from the camera, when the survey starts.
				AUTOTEST_TOTAL_TIMEOUT_MS = 3600000;
				mp_log( "[AT] Animation survey mode.\n" );
			}
			if ( GetEnvironmentVariableA( "PTLE_MP_SURVEY", env, sizeof(env) ) && (env[0] == '1' || env[0] == '2' || env[0] == '3') ) {
				g_survey = true;
				if ( env[0] == '2' ) { SURVEY_LEVELS = SURVEY_BIG; NUM_SURVEY_LEVELS = sizeof(SURVEY_BIG) / sizeof(SURVEY_BIG[0]); }
				if ( env[0] == '3' ) { SURVEY_LEVELS = SURVEY_WATER; NUM_SURVEY_LEVELS = sizeof(SURVEY_WATER) / sizeof(SURVEY_WATER[0]); }
				AUTOTEST_TOTAL_TIMEOUT_MS = 1200000;
				mp_log( "[AT] Level survey mode.\n" );
			}
			CreateThread( 0, 0, AutotestThread, 0, 0, 0 );
		}

		mp_log( "[MP] Hooked ticks. F1 menu, F5 spawn, F6 state, F7 delay, F8 stay here.\n" );
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
		SuspectPuppets();
	}

	virtual void onLevelLoaded( LevelLoadedEvent& e )
	{
		SuspectPuppets();
	}
};

DECLARE_PLUGIN( MultiplayerPlugin );
