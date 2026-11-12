// --------------------------------------------------------------------------------
// Multiplayer (milestone 1 : puppet test).
//
// Spawns a second Harry-looking character (the game's Evil Harry beast) and drives
// its transform from code every frame. There is no networking yet : the puppet
// replays the local player's own movement a short delay behind, which proves we
// can own and move a second character. The "remote player" source is isolated in
// PushLocalSnapshot() / GetPuppetTarget() so it can later be fed from a server.
//
// Hotkeys (only while the game window has focus) :
//   F5 : spawn the puppet in the current level.
//   F6 : cycle the state the puppet is forced into (-1 = let its AI run).
//        Each press logs the state name, to find a harmless idle state.
//   F7 : cycle the replay delay (0.25s .. 3s).
// --------------------------------------------------------------------------------

#include <Windows.h>
#include <ShlObj.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "gizmod/Gizmod.h"
#include "gizmod/GizmodPlugin.h"
#include "gizmod/event/LoadLevelEvent.h"

#include "injector/injector.hpp"

#include "ptle/EIHarry.h"
#include "ptle/EIEvilHarryP.h"
#include "ptle/EBeastStateMachine.h"
#include "ptle/ERLevel.h"
#include "ptle/math/Matrix4f.h"
#include "ptle/math/Vector3f.h"
#include "ptle/types/types.h"

#include "utils/func.h"


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
// Remote player source (for now : a delayed echo of the local player).
// --------------------------------------------------------------------------------
struct PlayerSnapshot
{
	Matrix4f transform;
	Vector3f position;
	Vector3f rotationEuler;
};

static const int HISTORY_SIZE = 256;
static PlayerSnapshot g_history[HISTORY_SIZE];   // Static storage keeps Matrix4f 16-byte aligned.
static int g_historyHead = 0;                    // Next slot to write.
static int g_historyCount = 0;

static const int DELAY_CHOICES[] = { 8, 15, 30, 60, 90, 180 };   // In game ticks.
static int g_delayIndex = 3;

static void ResetHistory()
{
	g_historyHead = 0;
	g_historyCount = 0;
}

static void PushLocalSnapshot( EIHarry* harry )
{
	PlayerSnapshot& s = g_history[g_historyHead];
	s.transform     = harry->m_transformMatrix;
	s.position      = harry->m_position;
	s.rotationEuler = harry->m_rotationEuler;

	g_historyHead = (g_historyHead + 1) % HISTORY_SIZE;
	if ( g_historyCount < HISTORY_SIZE ) g_historyCount++;
}

static const PlayerSnapshot* GetPuppetTarget()
{
	int delay = DELAY_CHOICES[g_delayIndex];
	if ( g_historyCount <= delay ) return 0;

	int idx = (g_historyHead - 1 - delay + HISTORY_SIZE) % HISTORY_SIZE;
	return &g_history[idx];
}



// --------------------------------------------------------------------------------
// Puppet.
// --------------------------------------------------------------------------------
static EIEvilHarryP* g_puppet = 0;
static int g_forcedState = -1;
static char g_puppetName[] = "evil harry";

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

static void SpawnPuppet( EIHarry* harry )
{
	if ( g_puppet ) {
		mp_log( "[MP] Puppet already spawned in this level.\n" );
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

	// NOTE : the beast destructor frees this string. ForgetPuppet() clears it
	// before the level unloads so the game never frees our static memory.
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

	g_puppet = p;

	mp_log( "[MP] Puppet spawned at 0x%.8X (m_beastTypeName offset 0x%X, expected 0x%X).\n",
		p, (unsigned) offsetof(EIBeast, m_beastTypeName), (unsigned) EXPECTED_BEASTNAME_OFFSET );
	LogStateMachine( p );
}

// Called before the level is torn down : the puppet gets destroyed with it.
static void ForgetPuppet()
{
	if ( g_puppet ) {
		// The name points to our static memory. Detach it so the game's
		// destructor doesn't try to free it (known crash at 0x412A32).
		if ( g_puppet->m_beastTypeName == g_puppetName ) {
			g_puppet->m_beastTypeName = 0;
		}
		mp_log( "[MP] Level unloading, puppet released.\n" );
	}
	g_puppet = 0;
	ResetHistory();
}

static void DrivePuppet( EIEvilHarryP* p )
{
	const PlayerSnapshot* t = GetPuppetTarget();
	if ( !t ) return;

	p->m_transformMatrix = t->transform;
	p->EICharacter::m_position = t->position;
	p->EIBeast::m_position = t->position;
	p->m_rotationEuler = t->rotationEuler;
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
	bool wasDown[3] = { false, false, false };
	const int keys[3] = { VK_F5, VK_F6, VK_F7 };
	volatile LONG* reqs[3] = { &g_reqSpawn, &g_reqState, &g_reqDelay };

	for ( ;; ) {
		for ( int i = 0; i < 3; i++ ) {
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

	if ( f5 ) {
		SpawnPuppet( harry );
	}

	if ( f6 && g_puppet ) {
		// Only calm-looking states : forcing arbitrary ones (e.g. state 0) crashes,
		// they expect context set up by the boss fight.
		static const char* const CANDIDATES[] = { "idle left", "idle right", "rest left", "rest right" };
		static const int NUM_CANDIDATES = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);
		static int choice = -1;   // -1 = AI runs freely.

		EBeastStateMachine* sm = g_puppet->GetStateMachine();
		choice++;
		if ( choice >= NUM_CANDIDATES ) choice = -1;

		g_forcedState = -1;
		if ( choice >= 0 && sm && sm->m_states ) {
			for ( int i = 0; i < (int) sm->m_numStates; i++ ) {
				const char* name = sm->m_states[i].m_name;
				const char* dot = name ? strchr( name, '.' ) : 0;
				if ( dot && strcmp( dot + 1, CANDIDATES[choice] ) == 0 ) {
					g_forcedState = i;
					break;
				}
			}
		}

		if ( g_forcedState < 0 ) {
			mp_log( "[MP] Puppet AI state : free (not forced)%s.\n", choice >= 0 ? " - candidate not found" : "" );
		}
		else {
			mp_log( "[MP] Puppet forced state : %d (%s)\n", g_forcedState, sm->m_states[g_forcedState].m_name );
		}
	}

	if ( f7 ) {
		g_delayIndex = (g_delayIndex + 1) % (sizeof(DELAY_CHOICES) / sizeof(DELAY_CHOICES[0]));
		mp_log( "[MP] Replay delay : %d ticks.\n", DELAY_CHOICES[g_delayIndex] );
	}
}



// --------------------------------------------------------------------------------
// Autotest mode (environment variable PTLE_MP_AUTOTEST=1, set by
// tools\run-autotest.ps1). Presses Enter through the menus, auto-skips
// cutscenes, then runs a scripted test on the game thread, takes screenshots
// into logs\shots\ and exits the game. Normal launches are unaffected.
// --------------------------------------------------------------------------------
GET_METHOD( 0x4B55F0, void, EIHarry_Teleport, EIHarry*, Matrix4f*, bool, float );

static bool g_autotest = false;
static volatile LONG g_autotestDone = 0;      // 1 = pass, 2 = fail.
static volatile LONG g_snapPending = 0;
static char g_snapName[64];

static const DWORD AUTOTEST_MENU_TIMEOUT_MS  = 180000;
static const DWORD AUTOTEST_TOTAL_TIMEOUT_MS = 360000;

static void RequestSnapshot( const char* name )
{
	strcpy_s( g_snapName, name );
	InterlockedExchange( &g_snapPending, 1 );
}

// Grabs the game window's client area from the screen into logs\shots\<name>.bmp.
static void TakeSnapshot( const char* name )
{
	HWND wnd = *((HWND*) HWND_GAME_WINDOW);
	if ( !wnd || !g_logDir[0] ) return;

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
	bi.bmiHeader.biHeight = h;   // Bottom-up, as BMP files expect.
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 24;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = 0;
	HBITMAP dib = CreateDIBSection( screen, &bi, DIB_RGB_COLORS, &bits, 0, 0 );
	HGDIOBJ old = SelectObject( mem, dib );
	BitBlt( mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT );
	GdiFlush();

	char path[MAX_PATH];
	sprintf_s( path, "%s\\shots", g_logDir );
	CreateDirectoryA( path, 0 );
	sprintf_s( path, "%s\\shots\\%s.bmp", g_logDir, name );

	DWORD stride = ((w * 3) + 3) & ~3;
	DWORD imageSize = stride * h;
	BITMAPFILEHEADER fh; ZeroMemory( &fh, sizeof(fh) );
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
	fh.bfSize = fh.bfOffBits + imageSize;

	FILE* f = 0;
	if ( fopen_s( &f, path, "wb" ) == 0 && f ) {
		fwrite( &fh, sizeof(fh), 1, f );
		fwrite( &bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f );
		fwrite( bits, imageSize, 1, f );
		fclose( f );
		mp_log( "[AT] Screenshot : shots\\%s.bmp\n", name );
	}

	SelectObject( mem, old );
	DeleteObject( dib );
	DeleteDC( mem );
	ReleaseDC( 0, screen );
}

static void TapKey( WORD vk )
{
	INPUT in[2]; ZeroMemory( in, sizeof(in) );
	WORD scan = (WORD) MapVirtualKeyA( vk, MAPVK_VK_TO_VSC );
	in[0].type = INPUT_KEYBOARD;
	in[0].ki.wScan = scan;
	in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
	in[1] = in[0];
	in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;

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

static DWORD WINAPI AutotestThread( void* )
{
	DWORD start = GetTickCount();
	mp_log( "[AT] Autotest mode on.\n" );

	HWND wnd = 0;
	while ( !(wnd = *((HWND*) HWND_GAME_WINDOW)) ) {
		if ( GetTickCount() - start > 60000 ) {
			mp_log( "[AT] FAIL : game window never appeared.\n" );
			ExitProcess( 2 );
		}
		Sleep( 200 );
	}
	Sleep( 3000 );

	// Menus : keep pressing Enter until Harry starts ticking.
	int presses = 0;
	while ( g_harryTicks == 0 ) {
		if ( GetTickCount() - start > AUTOTEST_MENU_TIMEOUT_MS ) {
			TakeSnapshot( "fail_menu_timeout" );
			mp_log( "[AT] FAIL : never reached gameplay after %d Enter presses.\n", presses );
			ExitProcess( 2 );
		}
		if ( presses % 2 == 0 ) {
			char name[32];
			sprintf_s( name, "menu_%02d", presses );
			TakeSnapshot( name );
		}
		SetForegroundWindow( wnd );
		Sleep( 100 );
		if ( GameHasFocus() ) {
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

	// The scripted part runs on the game thread (AutotestStep). Service screenshots.
	while ( g_autotestDone == 0 ) {
		if ( GetTickCount() - start > AUTOTEST_TOTAL_TIMEOUT_MS ) {
			TakeSnapshot( "fail_total_timeout" );
			mp_log( "[AT] FAIL : test script timed out.\n" );
			ExitProcess( 2 );
		}
		if ( InterlockedExchange( &g_snapPending, 0 ) ) {
			TakeSnapshot( g_snapName );
		}
		Sleep( 30 );
	}

	if ( InterlockedExchange( &g_snapPending, 0 ) ) TakeSnapshot( g_snapName );
	mp_log( "[AT] %s\n", g_autotestDone == 1 ? "RESULT : PASS" : "RESULT : FAIL" );
	Sleep( 300 );
	ExitProcess( g_autotestDone == 1 ? 0 : 2 );
	return 0;
}

static void LogTelemetry( EIHarry* harry, const char* tag )
{
	if ( g_puppet ) {
		mp_log( "[AT] %s harry (%.1f, %.1f, %.1f) puppet (%.1f, %.1f, %.1f) matrix (%.1f, %.1f, %.1f) state %d\n", tag,
			harry->m_position.x, harry->m_position.y, harry->m_position.z,
			g_puppet->EICharacter::m_position.x, g_puppet->EICharacter::m_position.y, g_puppet->EICharacter::m_position.z,
			g_puppet->m_transformMatrix.data[3][0], g_puppet->m_transformMatrix.data[3][1], g_puppet->m_transformMatrix.data[3][2],
			g_puppet->m_stateIndexCurrent );
	}
	else {
		mp_log( "[AT] %s harry (%.1f, %.1f, %.1f), no puppet\n", tag, harry->m_position.x, harry->m_position.y, harry->m_position.z );
	}
}

// Runs every Harry tick on the game thread while in autotest mode.
static void AutotestStep( EIHarry* harry )
{
	static const float HOPS[][2] = { { 15, 0 }, { 15, 15 }, { 0, 15 }, { 0, 0 } };
	static const int NUM_HOPS = 4;
	static const int NUM_STATE_PRESSES = 5;   // 4 calm states, then back to free AI.

	static int phase = 0;
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

	if ( g_puppet && now - lastTelemetry >= 500 ) {
		LogTelemetry( harry, "tele" );
		lastTelemetry = now;
	}

	switch ( phase )
	{
	case 0:   // Let the level settle (cutscenes are auto-skipped).
		if ( now - phaseStart < 8000 ) break;
		LogTelemetry( harry, "before spawn" );
		RequestSnapshot( "01_before_spawn" );
		base = harry->m_rigidBodyMatrix;
		SpawnPuppet( harry );
		if ( !g_puppet ) {
			mp_log( "[AT] FAIL : puppet did not spawn.\n" );
			InterlockedExchange( &g_autotestDone, 2 );
			return;
		}
		phase = 1; phaseStart = now; step = 0; snapped = false;
		break;

	case 1:   // Hop Harry around a square; the puppet should trail behind.
		if ( now - phaseStart >= 500 && !snapped && step == 0 ) {
			RequestSnapshot( "02_after_spawn" );
			snapped = true;
		}
		if ( now - phaseStart >= 2500 ) {
			if ( step < NUM_HOPS ) {
				Matrix4f m = base;
				m.data[3][0] += HOPS[step][0];
				m.data[3][1] += HOPS[step][1];
				m.data[3][2] += 2.0F;
				EIHarry_Teleport( harry, &m, false, 1.0F );
				mp_log( "[AT] Hop %d : teleported Harry by (%.0f, %.0f).\n", step, HOPS[step][0], HOPS[step][1] );
				step++;
				phaseStart = now; snapped = false;
			}
			else {
				phase = 2; phaseStart = now; step = 0; snapped = false;
			}
		}
		else if ( step > 0 && !snapped && now - phaseStart >= 2300 ) {
			char name[32];
			sprintf_s( name, "03_hop_%d", step - 1 );
			RequestSnapshot( name );
			snapped = true;
		}
		break;

	case 2:   // Cycle the calm F6 states.
		if ( step == 0 || now - phaseStart >= 3000 ) {
			if ( step >= NUM_STATE_PRESSES ) {
				phase = 3; phaseStart = now;
				break;
			}
			InterlockedIncrement( &g_reqState );   // Same as pressing F6.
			step++;
			phaseStart = now; snapped = false;
		}
		else if ( !snapped && now - phaseStart >= 2500 ) {
			char name[32];
			sprintf_s( name, "04_state_press_%d", step );
			RequestSnapshot( name );
			snapped = true;
		}
		break;

	case 3:
		if ( now - phaseStart < 1000 ) break;
		LogTelemetry( harry, "final" );
		RequestSnapshot( "05_final" );
		InterlockedExchange( &g_autotestDone, 1 );
		break;
	}
}



// --------------------------------------------------------------------------------
// Tick hooks. __fastcall receives "this" in ECX like __thiscall (EDX is unused),
// so these can replace the vtable entries directly.
// --------------------------------------------------------------------------------
static void __fastcall HarryTick_Hook( EIHarry* self, void* )
{
	g_origHarryTick( self );

	if ( InterlockedIncrement( &g_harryTicks ) == 1 ) {
		mp_log( "[MP] Harry tick hook is running (this 0x%.8X, global Harry 0x%.8X).\n", self, Gizmod::getHarry() );
	}

	if ( self != Gizmod::getHarry() ) return;

	PushLocalSnapshot( self );
	if ( g_autotest ) AutotestStep( self );
	HandleHotkeys( self );
}

static void __fastcall EvilHarryTick_Hook( EIEvilHarryP* self, void* )
{
	if ( self != g_puppet ) {
		g_origEvilHarryTick( self );
		return;
	}

	static int lastLoggedState = -2;
	static int lastRequested = -2;

	if ( g_forcedState >= 0 && self->m_stateIndexCurrent != g_forcedState ) {
		if ( lastRequested != g_forcedState ) {
			mp_log( "[MP] Requesting state %d (current %d)...\n", g_forcedState, self->m_stateIndexCurrent );
			lastRequested = g_forcedState;
		}
		self->m_forceNextState = true;
		self->m_nextState = g_forcedState;
	}

	g_origEvilHarryTick( self );   // Animations, state machine.

	if ( self->m_stateIndexCurrent != lastLoggedState ) {
		mp_log( "[MP] Puppet is now in state %d.\n", self->m_stateIndexCurrent );
		lastLoggedState = self->m_stateIndexCurrent;
	}

	DrivePuppet( self );           // Then override where it is.
}



// --------------------------------------------------------------------------------
// Plugin.
// --------------------------------------------------------------------------------
class MultiplayerPlugin : public GizmodPlugin, public ILoadLevelListener
{
public:

	virtual const char* getDisplayName() const { return "Multiplayer (puppet test)"; }
	virtual const char* getSystemName() const { return "Multiplayer"; }

	virtual void onEnable()
	{
		OpenLogFile();
		AddVectoredExceptionHandler( 1, CrashLogger );

		register_types();

		uint32_t harrySlot = VTABLE_HARRY + VTABLE_SLOT_TICK;
		uint32_t evilSlot  = VTABLE_EVILHARRY_P + VTABLE_SLOT_TICK;

		g_origHarryTick     = injector::ReadMemory<TickFunc>( harrySlot, true );
		g_origEvilHarryTick = injector::ReadMemory<TickFunc>( evilSlot, true );

		injector::WriteMemory( harrySlot, &HarryTick_Hook, true );
		injector::WriteMemory( evilSlot,  &EvilHarryTick_Hook, true );

		Gizmod::getInstance()->getEventListener()->registerEvent<LoadLevelEvent>( this );

		CreateThread( 0, 0, HotkeyThread, 0, 0, 0 );

		char env[8] = { 0, };
		if ( GetEnvironmentVariableA( "PTLE_MP_AUTOTEST", env, sizeof(env) ) && env[0] == '1' ) {
			g_autotest = true;
			EnableAutoSkipCutscenes();
			CreateThread( 0, 0, AutotestThread, 0, 0, 0 );
		}

		mp_log( "[MP] Hooked ticks (Harry 0x%.8X, Evil Harry 0x%.8X). F5 spawn, F6 state, F7 delay.\n",
			(uint32_t) g_origHarryTick, (uint32_t) g_origEvilHarryTick );
	}

	virtual void onLoadLevel( LoadLevelEvent& e )
	{
		ForgetPuppet();
	}
};

DECLARE_PLUGIN( MultiplayerPlugin );
