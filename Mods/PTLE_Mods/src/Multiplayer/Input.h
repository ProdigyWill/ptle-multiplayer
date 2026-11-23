#pragma once

#include "Multiplayer.h"

#include <dinput.h>


// Input : the hotkeys, typing into the menu, and the hooks on the keyboard and controller polls.


extern int g_swapButton;   // DirectInput button : the fifth, L1 / LB.
extern int g_swapKey;   // DirectInput key code : Left Shift.
extern volatile LONG g_reqDelay;
extern volatile LONG g_reqMenu;

enum { MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT, MK_ACCEPT, MK_BACK, MK_NEXT, MK_PREV, MK_DELETE, MK_PGUP, MK_PGDN, MK_SLEFT, MK_SRIGHT, MK_COUNT };

extern volatile LONG g_menuKeys[MK_COUNT];   // Presses, from the hotkey thread.
extern volatile LONG g_menuOpen;   // For the hotkey thread and the keyboard poll.
extern volatile LONG g_menuTyping, g_menuPaste, g_menuCopy;

void TakeTyped( char* out, size_t size );

static HWND* const HWND_GAME_WINDOW   = (HWND*) 0x93ED60;

void HookGameWindowProc();
bool ReadClipboard( char* out, size_t size );
void WriteClipboard( const char* text );

extern volatile LONG g_reqViewer, g_reqStar, g_reqPrev, g_reqNext, g_reqPrev10, g_reqNext10, g_reqReplay, g_reqStarredOnly;
extern volatile LONG g_harryTicks;
extern volatile LONG g_menuWheel;   // WHEEL_DELTA a notch (a touchpad's steps can be smaller).
extern DIJOYSTATE2 g_gamepadSeen;   // The last state polled (the Keys page reads its buttons).
extern bool g_padSeenAny;
extern bool g_inputBlocked;   // Should all input to the game be blocked (e.g. when F1 menu brought up).

void InstallInputHooks();


#ifdef PTLE_MP_EMBEDTESTS   // For the tests (tests.cpp).

void AddTyped( char c );
uint8_t KeyToDIK( WORD vk );

#endif
