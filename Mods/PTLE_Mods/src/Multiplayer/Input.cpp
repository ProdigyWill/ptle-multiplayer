#include "Input.h"

#include "Multiplayer.h"
#include "MenuPages.h"

#include "gamemodes/HideAndSeek/HideAndSeek.h"
#include "tests.h"

#include "injector/injector.hpp"

#include <Windows.h>
#include <dinput.h>

#include <string.h>


// Swapping roles without the menu : hold the controller button or the key, then
// either direction of the d-pad or either arrow key (Multiplayer.ini, [Keys]
// RoleSwapButton, RoleSwapKey). While it's held, the d-pad and the arrows don't
// reach the game : no item comes out.
int g_swapButton = 4;            // DirectInput button : the fifth, L1 / LB.
int g_swapKey = 0x2A;            // DirectInput key code : Left Shift.

// --------------------------------------------------------------------------------
// Hotkeys & menu typing.
// --------------------------------------------------------------------------------
// Keys are polled on our own thread (independent of the game's tick rate and of
// which window the game thinks it owns), and turned into requests the game
// thread consumes in HandleHotkeys().
volatile LONG g_reqDelay = 0;
volatile LONG g_reqMenu = 0;

volatile LONG g_menuKeys[MK_COUNT];          // Presses, from the hotkey thread.
volatile LONG g_menuOpen = 0;                // For the hotkey thread and the keyboard poll.

// Typing into the menu (a colour) : characters from the hotkey thread.
static SRWLOCK g_typedLock = SRWLOCK_INIT;
static char g_typedChars[64];
volatile LONG g_menuTyping = 0, g_menuPaste = 0, g_menuCopy = 0;

void AddTyped( char c )
{
	AcquireSRWLockExclusive( &g_typedLock );
	size_t n = strlen( g_typedChars );
	if ( n + 1 < sizeof(g_typedChars) ) { g_typedChars[n] = c; g_typedChars[n + 1] = 0; }
	ReleaseSRWLockExclusive( &g_typedLock );
}

void TakeTyped( char* out, size_t size )
{
	AcquireSRWLockExclusive( &g_typedLock );
	strcpy_s( out, size, g_typedChars );
	g_typedChars[0] = 0;
	ReleaseSRWLockExclusive( &g_typedLock );
}


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
void HookGameWindowProc()
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

bool ReadClipboard( char* out, size_t size )
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

void WriteClipboard( const char* text )
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

volatile LONG g_menuWheel = 0;          // WHEEL_DELTA a notch (a touchpad's steps can be smaller).
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



//
// Input hooks (keyboard + gamepad).
//


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
DIJOYSTATE2 g_gamepadSeen;          // The last state polled (the Keys page reads its buttons).
bool g_padSeenAny = false;

bool g_inputBlocked = false;          // Should all input to the game be blocked (e.g. when F1 menu brought up).

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
void InstallInputHooks()
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
