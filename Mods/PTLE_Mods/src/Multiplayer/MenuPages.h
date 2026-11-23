#pragma once

#include "Multiplayer.h"

#include "menu.h"


// The menu's pages (F1) : players, display, keys, game modes and emotes.


void EmotePreviewTick( EIHarry* harry );

// The player list's rows : the room, us, the others, the ping.
static const int MAX_LIST_ROWS = MAX_PEERS + 3;

int BuildPlayerRows( char left[][96], char right[][64], uint32_t* colors, const char* lastRight );
const char* KeyName( int dik );

extern Menu g_menu;

static inline bool IsMenuOpen() { return g_menu.IsOpen(); }

void DrawMenu( float sw, float sh, float h );
void SyncMenuOpen();
void SetMenuOpen( bool open );


#ifdef PTLE_MP_EMBEDTESTS   // For the tests (tests.cpp).

extern int g_ringSlot;   // Its slot the ring points at.
extern bool g_chooser;   // Choosing an emote for that slot.
extern char g_emoteSearch[32];
extern int g_emoteCategory;   // 0 : favourites, 1 : all, then EMOTE_CATEGORIES.
extern int g_listCursor, g_listScroll;
extern int g_previewWant;   // The chooser, each frame : EMOTES index, -1 : none (idle), -2 : no preview.

int VkToDik( int vk );

#endif
