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
#include <Windows.h>
#include <ShlObj.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "gizmod/Gizmod.h"
#include "gizmod/GizmodPlugin.h"
#include "gizmod/event/LoadLevelEvent.h"
#include "gizmod/event/LevelLoadedEvent.h"

#include "injector/injector.hpp"

#include "ptle/EIHarry.h"
#include "ptle/EIEvilHarryP.h"
#include "ptle/ERAnim.h"
#include "ptle/ERLevel.h"
#include "ptle/ERCharacter.h"
#include "ptle/EIStaticModel.h"

#include "ptle/types/types.h"

#include "utils/func.h"

#include "Multiplayer.h"
#include "Network.h"
#include "Puppets.h"
#include "Render.h"
#include "Outfits.h"
#include "Input.h"
#include "MenuPages.h"
#include "AnimViewer.h"

#include "version.h"
#include "net/transport.h"

#include "tests.h"

#include "gamemodes/HideAndSeek/HideAndSeek.h"



typedef void (__thiscall *TickFunc)( EInstance* );
typedef void (__thiscall *RenderFunc)( EInstance* self, void* context, void* arg );

GET_METHOD( 0x4B2CD0, void, EIHarry_Tick, EIHarry* );

int g_gameMode = MODE_OFF;   // F1 -> Gamemodes; Multiplayer.ini, [Gamemodes] Mode.



// --------------------------------------------------------------------------------
// Logging : Gizmod console + %LOCALAPPDATA%\PTLE Multiplayer\logs\multiplayer.log
// (the game folder isn't writable without admin rights, and nothing of the mod's
// belongs in the player's Documents).
// --------------------------------------------------------------------------------
static FILE* g_logFile = 0;
static CRITICAL_SECTION g_logLock;
char g_logDir[MAX_PATH] = { 0, };

// Shortcut.
static inline Logger* logger() { return Gizmod::getInstance()->getLogger(); }

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



// --------------------------------------------------------------------------------
// Custom Harry tick.
// This is where we handle updating gamemode state, emotes, and perform
//  network communication.
// --------------------------------------------------------------------------------

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
	static ERLevel* currentWorld = 0;
	static uint32_t currentCRC = 0;
	if ( currentWorld != world || currentCRC != Gizmod::getCurrentLevelCRC() ) {
		currentWorld = world;   // So this change isn't seen again.
		currentCRC = Gizmod::getCurrentLevelCRC();

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



// --------------------------------------------------------------------------------
// Settings (see the top of this file).
// --------------------------------------------------------------------------------
static char g_iniPath[MAX_PATH] = { 0, };

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

// The menu's settings, kept in the ini (not the autotests' : they leave it alone).
void SaveSettings()
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
		// TODO: 0.4 subclassing.
		const uint32_t harrySlot = EIHarry::VTABLE + VTABLE_SLOT_TICK;
		injector::WriteMemory( harrySlot, &EIHarry_Tick_Hook );
		InstallPuppetTickHook();

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

		InstallHarryModelHook();

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
