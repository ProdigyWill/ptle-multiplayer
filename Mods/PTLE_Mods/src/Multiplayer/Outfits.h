// Outfits
// Harry's and the puppets' models, and Retro Harry's palette.

#pragma once

#include "util/common.h"

class EIEvilHarryP;

struct Peer;


enum { RETRO_HAIR, RETRO_SKIN, RETRO_SHIRT, RETRO_TROUSERS, RETRO_PARTS };

struct Outfit { const char* name; uint32_t model; uint32_t cheatFlag; };

static const Outfit OUTFITS[] = {
	{ "Harry",       0xC9252A08, 0 },
	{ "Nicole",      0x7BBEDDE3, 0x90DA18 },
	{ "Native",      0x182A7689, 0x90DA19 },
	{ "Retro Harry", 0xBF8D2C68, 0x90DA1F },
	{ "Snowy Harry", 0x94DB2F98, 0 },
};

static const int NUM_OUTFITS = countof(OUTFITS);
static const int RETRO_OUTFIT = 3;
static const int SNOWY_OUTFIT = 4;
static const int OUTFIT_DEFAULT = -1;   // Chosen : whatever the level gives Harry.
extern int g_myOutfit;   // The one chosen (Multiplayer.ini, [Appearance] Outfit, by name) : worn from the next level load.
extern int g_myWornOutfit;   // The one Harry wears now : what the others are told.

// Retro Harry's colours. His model takes all its colour from a 32x32 texture of four
// flat squares, so each player can have their own (see the Retro palette section).
static const char* const RETRO_PART_NAMES[RETRO_PARTS] = { "Hair", "Head", "Shirt", "Pants" };   // In the menu, and the ini keys (RetroHair...).

static const uint8_t RETRO_DEFAULT[RETRO_PARTS][3] = { { 142, 118, 62 }, { 255, 161, 123 }, { 78, 200, 74 }, { 0, 87, 0 } };
extern uint8_t g_myColors[RETRO_PARTS][3];   // Multiplayer.ini, [Appearance] RetroHair... (RRGGBB).

void ForgetRetroTextures();
void HookRetroPalette( void** vtable );
void ApplyPuppetOutfit( EIEvilHarryP* p, Peer* peer );
void ApplyMyOutfit();
void InstallHarryModelHook();


#ifdef PTLE_MP_EMBEDTESTS   // For the tests (tests.cpp).

extern int g_retroSwaps;   // Pieces drawn with a player's palette (for the autotest).
extern int g_retroMatched, g_retroUnmatched;   // For the autotest.

#endif
