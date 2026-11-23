
#include "gizmod/Gizmod.h"
#include "ptle/EIHarry.h"
#include "ptle/EIEvilHarryP.h"
#include "ptle/EItemBase.h"

#include "utils/func.h"
#include "injector/injector.hpp"

#include <Windows.h>
#include <dinput.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "Multiplayer.h"
#include "Network.h"
#include "Puppets.h"
#include "Render.h"
#include "Outfits.h"
#include "Input.h"
#include "MenuPages.h"
#include "AnimViewer.h"
#include "tests.h"

#include "menu.h"
#include "net/protocol.h"
#include "net/snapshot.h"
#include "net/transport.h"
#include "gamemodes/HideAndSeek/HideAndSeek.h"
#include "emotes_data.h"


#ifdef PTLE_MP_EMBEDTESTS

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

static volatile LONG g_settled = 0;           // 1 = cutscene/book dismissed, game runs.

static bool g_background = false;            // Autotest without focus (see above).
static volatile uint8_t g_fakeKeys[256];      // Background : the keys the test holds (DirectInput codes).
static bool g_skipPuppetAI = false;           // true = don't run Evil Harry's own tick (no animation).
static volatile LONG g_snapPending = 0;
static volatile LONG g_windowFailures = 0;    // Game window checks (always on top).
static volatile LONG g_holdKey = 0;           // Virtual key the autotest thread should hold...
static volatile LONG g_holdMs = 0;            // ...for this long.
static volatile LONG g_holdKey2 = 0;          // Second key held at the same time (e.g. crouch + walk).
static volatile LONG g_holdMs2 = 0;
static volatile LONG g_tapKey = 0;            // Virtual key the autotest thread should tap.
static volatile LONG g_tapLaterKey = 0;       // ... and this one at g_tapLaterAt (GetTickCount) : the game's
static volatile LONG g_tapLaterAt = 0;        // tick (the test script) doesn't run while it's paused.
static char g_snapName[64];

static uint32_t AUTOTEST_LEVEL = 0x7567D75A;   // The ice test's walk : Spinja Lair (cold water just ahead).
static volatile LONG g_autotestDone = 0;      // 1 = pass, 2 = fail.
static int g_itemFailures = 0;
static int g_animClipCount = 0;

static int g_testOutfit = -1;                  // Every puppet wears this.
static bool g_testStallPuppet = false;         // The puppet's ticks are skipped...
static EIEvilHarryP* g_testStalled = 0;        // ...this one's.
static bool g_testSkipHarryItemDraw = false;   // Harry's item isn't drawn (as with the camera in his head).

static struct { bool active; float x, y; bool down; } g_testMouse;   // Stands in for the mouse.
static int g_testWheelFrom = 0;                // The list's top before a wheel notch.
static char g_testPaste[128];                  // Stands in for the clipboard.
static volatile LONG g_testArrowFrames = 0, g_testHatFrames = 0;   // Polls in which the game got the right arrow / the d-pad.

static bool g_testHideReport = false;          // Hide and seek : log the hide time each second.
static int g_testCaughtBy = -1, g_testCaughtHider = -1;   // The last catch heard.
static float g_testNearestSeeker = -1.0F;      // This tick's nearest seeker (-1 : none shown).

static void RequestSnapshot( const char* name )
{
	strcpy_s( g_snapName, name );
	InterlockedExchange( &g_snapPending, 1 );
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

// Runs every Harry tick on the game thread while in autotest mode.
// Gives Harry the raft, pickaxes, torch and TNT (like CheatLab's "give stuff").
static void GiveTestItems( EIHarry* harry )
{
	EInventory* inv = harry->m_inventory;
	if ( !inv ) return;
	inv->m_hotbar[0] = GetItemObject( InventoryItem::RAFT );       // Up.
	inv->m_hotbar[1] = GetItemObject( InventoryItem::PICKAXES );   // Right.
	inv->m_hotbar[2] = GetItemObject( InventoryItem::TORCH );      // Down.
	inv->m_hotbar[3] = GetItemObject( InventoryItem::TNT );        // Left.
	for ( int i = 0; i < 8; i++ ) ITEM_TABLE[i].m_unlocked = 1;
	mp_log( "[AT] Gave raft, pickaxes, torch, TNT.\n" );
}


bool g_autotest = false;                       // PTLE_MP_AUTOTEST=1 (see the autotest section).

// Animation survey (PTLE_MP_ANIMSURVEY=1) : every clip loaded on Harry and on Evil
// Harry, played in turn on the puppet (which then stops copying Harry's pose), each
// filmed in a few cropped frames (logs\shots\anim_<n>_f<k>.bmp).
static bool g_animSurvey = false;
static int g_crashTest = 0;                       // PTLE_MP_CRASHTEST=av|div|stack : the autotest crashes the game (the crash logger's test).
static int g_pitTest = 0;                        // PTLE_MP_PITTEST=1 : Harry walks into Crash Site's pitfall, the puppet right behind him; 2 : the puppet (ahead of him) goes in alone, as another player's does.
static int g_iceTest = 0;                         // PTLE_MP_ICETEST=1 : the autotest puts the puppet in "thrown from water"; 2 : walks Harry (and the puppet) into Spinja Lair's cold water. PTLE_MP_ICEALLOW=1 : let it freeze, as before the fix.

static int g_pauseTest = 0;   // PTLE_MP_PAUSETEST=1 : (two games) Harry runs, the game is paused mid-run for 8 s, then goes on; 2 : watches the other.
static int g_raftTest = 0;    // PTLE_MP_RAFTTEST=1 : Spinja Lair's wading water, the raft out, Harry turns paddling one side : the puppet must face as he does.
static int g_aimTest = 0;   // 2 : with the raft out instead (Down looks from Harry's head, the raft isn't drawn).
// PTLE_MP_AIMTEST=1 : the autotest holds Down (first-person look) with the puppet ahead, to check its tag.
static int g_distTest = 0;                        // PTLE_MP_DISTTEST=1 : the autotest moves the puppet away, to see how far it's drawn; two games : 2 runs away, 3 watches. PTLE_MP_FADEALLOW=1 : puppets fade as before.
static int g_emoteTest = 0;                       // PTLE_MP_EMOTETEST=1 : emotes, one game.
static int g_hsTest = 0;                          // PTLE_MP_HSTEST=1 : hide and seek, one game (role, hide time).
static bool g_skinTest = false;                  // PTLE_MP_SKINTEST=1 : the autotest puts each outfit's model on the puppet.
static bool g_menuTest = false;                  // PTLE_MP_MENUTEST=1 : the autotest drives the menu (F1).
static bool g_viewerTest = false;                // PTLE_MP_VIEWERTEST=1 : the autotest drives the animation viewer.
static bool g_deathTest = false;                 // PTLE_MP_DEATHTEST=1 : the autotest kills Harry beside his puppet.

static const int PAD_HAT = 0x20, PAD_BUTTONS = 0x30, PAD_STATE_SIZE = 0x110;
static uint8_t g_testPad[PAD_STATE_SIZE];
static bool g_testPadOn = false;

static bool g_puppetPinned = false;
static Vector3f g_puppetPin = { 0, 0, 0 };
static Vector3f g_puppetOffset = { 0, 0, 0 };

static const int MAX_ANIM_CLIPS = 1024;
static struct { uint32_t crc; int owner; int layer; } g_animClips[MAX_ANIM_CLIPS];


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

static void LogTelemetry( EIHarry* harry, const char* tag );

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

	if ( MainPeer()->beast && now - lastTelemetry >= 500 ) {
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
		SpawnPuppet( MainPeer() );
		phase = 102; phaseStart = now;
		lastHarryTicks = g_puppetPoseCopies;
		memset( &g_tagDiag, 0, sizeof(g_tagDiag) );
		g_tagDiagOn = true;
		break;
	case 102:   // Survey : look.
		if ( now - phaseStart < 3000 ) break;
		{
			bool loaded = MainPeer()->beast && MainPeer()->beast->m_beastType;
			int draws = g_puppetPoseCopies - (int) lastHarryTicks;
			if ( loaded ) g_surveyLoaded++;
			if ( draws > 0 ) g_surveyDrawn++;
			mp_log( "[AT] SURVEY %s : spawned %s, model %s, drawn %d times in 3 s.\n", SURVEY_LEVELS[step].name,
				MainPeer()->beast ? "yes" : "NO", loaded ? "loaded" : "MISSING", draws );
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
		SpawnPuppet( MainPeer() );
		if ( !MainPeer()->beast ) {
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
		if ( g_pauseTest ) { phase = 1220; phaseStart = now; step = 0; break; }
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
		EICharacter* owners[2] = { harry, MainPeer()->beast };
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
			EIEvilHarryP* b = MainPeer()->beast;
			mp_log( "[AT] Death %5u ms : Harry 0x%.8X health %d world 0x%.8X level 0x%.8X | puppet 0x%.8X pickaxes 0x%.8X 0x%.8X\n",
				now - phaseStart, harry, harry->m_healthValue, harry->m_world, Gizmod::getCurrentLevelCRC(),
				b, b ? (uint32_t) b->m_pickaxeModel0 : 0, b ? (uint32_t) b->m_pickaxeModel1 : 0 );
		}
		if ( now - phaseStart >= 30000 ) {
			RequestSnapshot( "death_after" );
			EIEvilHarryP* b = MainPeer()->beast;
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
				EIEvilHarryP* b = MainPeer()->beast;
				const PlayerSnapshot* tg = GetPuppetTarget( MainPeer() );
				mp_log( "[AT] Pit %5u ms : Harry (%.1f, %.1f, %.1f) health %d | other's puppet 0x%.8X at (%.1f, %.1f, %.1f), their place %s (%.1f, %.1f, %.1f), ticked %d (last %u ms ago), drawn %d\n",
					t, h.x, h.y, h.z, harry->m_healthValue, b, b ? b->EIBeast::m_position.x : 0.0F, b ? b->EIBeast::m_position.y : 0.0F, b ? b->EIBeast::m_position.z : 0.0F,
					tg ? "known" : "unknown", tg ? tg->position.x : 0.0F, tg ? tg->position.y : 0.0F, tg ? tg->position.z : 0.0F,
					MainPeer()->ticks, GetTickCount() - MainPeer()->lastTick, g_puppetPoseCopies );
			}
			if ( g_pitTest == 5 && ( t == 0 || (t >= 20000 && t < 20100) || (t >= 24000 && t < 24100) || (t >= 32000 && t < 32100) ) && !snapped ) { }
			if ( g_pitTest == 5 && step == 0 && t >= 21000 ) { RequestSnapshot( "pit_w1_eating" ); step = 1; }
			if ( g_pitTest == 5 && step == 1 && t >= 26000 ) { RequestSnapshot( "pit_w2_eaten" ); step = 2; }
			if ( step == 2 && t >= 40000 ) { RequestSnapshot( "pit_w3_back" ); drawsAt = g_puppetPoseCopies; ticksAt = MainPeer()->ticks; step = 3; }
			if ( g_pitTest == 4 && step == 1 && t >= 40000 ) step = 3;
			if ( step == 3 && t >= (g_pitTest == 4 ? 60000u : 45000u) ) {   // (The eater stays a while : the watcher checks while it's still here.)
				EIEvilHarryP* b = MainPeer()->beast;
				const PlayerSnapshot* tg = GetPuppetTarget( MainPeer() );
				float d = 999.0F;
				if ( b && tg ) { float dx = b->EIBeast::m_position.x - tg->position.x, dy = b->EIBeast::m_position.y - tg->position.y, dz = b->EIBeast::m_position.z - tg->position.z; d = sqrtf( dx * dx + dy * dy + dz * dz ); }
				int draws = g_puppetPoseCopies - drawsAt, ticks = MainPeer()->ticks - ticksAt;
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
			if ( g_testStallPuppet && !g_testStalled ) g_testStalled = MainPeer()->beast;
			if ( !MainPeer()->beast && !PeerInLimbo(MainPeer()) && t >= 8000 ) {
				mp_log( "[AT] Pit : the caught puppet was dropped : spawning another.\n" );
				SpawnPuppet( MainPeer() );
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
			EIEvilHarryP* b = MainPeer()->beast;
			const Vector3f& h = harry->m_position;
			mp_log( "[AT] Pit %5u ms : Harry (%.1f, %.1f, %.1f) health %d | puppet 0x%.8X limbo 0x%.8X at (%.1f, %.1f, %.1f) state %d, target %s, drawn %d\n",
				t, h.x, h.y, h.z, harry->m_healthValue, b, PeerInLimbo(MainPeer()),
				b ? b->EIBeast::m_position.x : 0.0F, b ? b->EIBeast::m_position.y : 0.0F, b ? b->EIBeast::m_position.z : 0.0F,
				b ? b->m_stateIndexCurrent : -1, GetPuppetTarget( MainPeer() ) ? "yes" : "no", g_puppetPoseCopies );
		}
		if ( step == 3 && t >= 25000 ) {
			RequestSnapshot( "pit_3_after" );
			EIEvilHarryP* b = MainPeer()->beast;
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
			EIEvilHarryP* b = MainPeer()->beast;
			if ( g_distTest == 3 && b && now - lastLog >= 250 ) {
				lastLog = now;
				const Matrix4f& cam = g_cameraWorld;
				float dx = b->EIBeast::m_position.x - cam.data[3][0], dy = b->EIBeast::m_position.y - cam.data[3][1], dz = b->EIBeast::m_position.z - cam.data[3][2];
				float d = sqrtf( dx * dx + dy * dy + dz * dz );
				int renders = g_puppetPoseCopies - posesAt; posesAt = g_puppetPoseCopies;
				float a = b->m_opacityFinal;
				if ( renders > 0 && t > 12000 ) { if ( d > maxDepthDrawn ) maxDepthDrawn = d; if ( a < minAlphaDrawn ) minAlphaDrawn = a; }
				mp_log( "[AT] Distance %5u ms : the other's puppet %.1f from the camera, rendered %d, game fade %.2f, +1C %.2f +20 %.2f +24 %.2f +28 %.2f, tag depth %.1f\n",
					t, d, renders, g_lastPuppetFade, a, b->m_opacity2, b->m_opacity3, b->m_alphaUser, MainPeer()->tagDepth );
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
				g_puppetPoseCopies - posesAtStart, g_tagPasses[1] - passesAtStart, MainPeer()->tagDepth, MainPeer()->tagFrame == g_presentCount ? "noted" : "not noted",
				MainPeer()->ticks, GetTickCount() - MainPeer()->lastTick, g_lastPuppetFade, g_puppetFadeCalls, g_fadesStopped );
			snapped = true;
		}
		else if ( t >= 3500 ) { step++; phaseStart = now; }
		break;
	}
	case 1100: {   // Raft : Harry paddles one side and turns; the puppet faces where he faces, not where he drifts.
		DWORD t = now - phaseStart;
		EIEvilHarryP* b = MainPeer()->beast;
		const PlayerSnapshot* tg = GetPuppetTarget( MainPeer() );
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
			if ( MainPeer()->moveSpeed > 0.03F ) maxDrift = max( maxDrift, fabsf( wrap( atan2f( MainPeer()->velY, MainPeer()->velX ) - tYaw ) ) );
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
		bool shown = dev && PlaceNameTag( *MainPeer(), dev, &tx, &ty, &td );
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
		EIEvilHarryP* b = MainPeer()->beast;
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
					MainPeer()->target ? MainPeer()->target->clipCRC : 0 );
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
			MainPeer()->forcedState = FindStateIndex( b, "thrown from water" );
			mp_log( "[AT] Ice : forcing state %d (\"thrown from water\").\n", MainPeer()->forcedState );
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
		if ( g_emoteState.active && g_emoteState.ending >= 0 ) sawEnding[g_emoteState.ending] = true;
		if ( g_emoteState.active && now - lastLog >= 500 ) {
			lastLog = now;
			AnimLayer* l = GetAnimLayer( harry, EMOTE_LAYERS[g_emoteState.slot] );
			int layer = -1; float w = 0;
			uint32_t clip = GetPlayingClip( harry, &layer, &w );
			mp_log( "[AT] Emote %s : clip 0x%.8X, frame %.3f, clock %.3f, play %d; Harry shows 0x%.8X (layer %d, %.2f), the puppet 0x%.8X.\n",
				EMOTES[g_emoteState.emote].name, g_emoteState.clip, l ? l->m_frame : 0.0F, l ? l->m_time : 0.0F, g_emoteState.plays,
				clip, layer, w, MainPeer()->mirrorClip );
		}
		if ( step == 0 && t >= 1500 ) {
			g_wheelCount = 2;
			for ( int w = 0; w < MAX_WHEELS; w++ ) MakeDefaultWheel( w );
			victory = FindEmoteByName( "Victory Dance" ); jig = FindEmoteByName( "Jig" ); breakdance = FindEmoteByName( "Breakdance" );
			running = FindEmoteByName( "Running Man" ); dizzy = FindEmoteByName( "Dizzy Walk" ); pickaxe = FindEmoteByName( "Pickaxe Spin" ); zen = FindEmoteByName( "Teleport Zen" ); death = FindEmoteByName( "Death" ); bdFinish = FindEmoteByName( "Breakdance Finish" );
			mp_log( "[AT] Emote : %d emotes in the catalogue.\n", NUM_EMOTES );
			bool all = true;
			for ( int s = 0; s < WHEEL_SLOTS; s++ ) all &= g_emoteWheels[0].slot[s] >= 0 && (s >= 6 || g_emoteWheels[1].slot[s] >= 0);
			Local::Check( "the default wheels' emotes are all in the catalogue", all && NUM_EMOTES > 200 );
			Local::Check( "Breakdance is a chain, with an ending", breakdance >= 0 && EMOTES[breakdance].intro != 0 && EMOTES[breakdance].ending[0] && EMOTES[breakdance].ending[1] && g_emoteWheels[0].slot[3] == breakdance );
			Local::Check( "Jig plays once, Running Man loops", jig >= 0 && !EMOTES[jig].loops && running >= 0 && EMOTES[running].loops );
			// The ini : saved, changed, loaded back.
			char ini[MAX_PATH];
			GetTempPathA( MAX_PATH, ini );
			strcat_s( ini, "ptle-emote-test.ini" );
			DeleteFileA( ini );
			EmoteWheel saved[MAX_WHEELS];
			g_wheelCount = 3;
			strcpy_s( g_emoteWheels[2].name, "Mine, all | mine" );
			g_emoteWheels[2].slot[5] = dizzy;
			SaveEmoteWheels( ini );
			memcpy( saved, g_emoteWheels, sizeof(saved) );
			strcpy_s( g_emoteWheels[2].name, "Mine, all / mine" );   // As it comes back.
			memcpy( saved[2].name, g_emoteWheels[2].name, sizeof(saved[2].name) );
			for ( int w = 0; w < MAX_WHEELS; w++ )
				MakeDefaultWheel( w );
			g_wheelCount = 0;
			LoadEmoteWheels( ini );
			Local::Check( "the wheels go through the ini", g_wheelCount == 3 && [&]() {
				for ( int w = 0; w < 3; w++ ) if ( strcmp( saved[w].name, g_emoteWheels[w].name ) || memcmp( saved[w].slot, g_emoteWheels[w].slot, sizeof(saved[w].slot) ) ) return false;
				return true; }() );
			char line[256] = "";
			GetPrivateProfileStringA( "Emotes", "Wheel1", "", line, sizeof(line), ini );
			mp_log( "[AT] Emote : Wheel1=%s\n", line );
			DeleteFileA( ini );
			g_wheelCount = 2;
			MakeDefaultWheel( 2 );
			RequestPlayEmote( victory );
			watch = true; longestStill = 0;
			step = 1; phaseStart = now;
		}
		else if ( step == 1 ) {
			if ( t >= 1200 && t < 1300 ) RequestSnapshot( "emote_1_victory" );
			if ( t > 300 && !g_emoteState.active ) {
				mp_log( "[AT] Emote : Victory Dance lasted %u ms.\n", t );
				Local::Check( "Victory Dance plays to its end", t >= 2000 );
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
			RequestPlayEmote( jig );
			step = 101; phaseStart = now;
		}
		else if ( step == 101 && (!g_emoteState.active && t > 300 || t > 20000) ) {
			Local::Check( "Jig plays once", !g_emoteState.active );
			RequestPlayEmote( running );
			longestStill = 0;
			mark = 0;
			step = 3; phaseStart = now;
		}
		else if ( step == 3 && t >= 12000 && t < 12500 && !mark ) { mark = 1; SendKey( VK_NUMPAD7, true ); }
		else if ( step == 3 && t >= 12500 && t < 13000 && mark == 1 ) {
			mark = 2;
			SendKey( VK_NUMPAD7, false );
			Local::Check( "turning the camera (numpad 7) doesn't end it", g_emoteState.active && g_emoteState.emote == running );
		}
		else if ( step == 3 && t >= 16000 ) {   // Long enough for one of Harry's idles (it fades every layer out).
			AnimLayer* mine = GetAnimLayer( harry, EMOTE_LAYERS[g_emoteState.slot] );
			mp_log( "[AT] Emote : Running Man, %d plays; longest still %u ms.\n", g_emoteState.plays, longestStill );
			Local::Check( "Running Man loops on, through Harry's idles", g_emoteState.active && g_emoteState.emote == running && g_emoteState.plays >= 8 && mine && mine->m_ticked && longestStill < 200 );
			RequestSnapshot( "emote_2b_running" );
			watch = false;
			SendKey( 'W', true );
			step = 4; phaseStart = now;
		}
		else if ( step == 4 && (!g_emoteState.active || t > 3000) ) {
			Local::Check( "a key ends it", !g_emoteState.active );
			SendKey( 'W', false );
			step = 5; phaseStart = now;
		}
		else if ( step == 5 && t >= 2500 ) {
			RequestPlayEmote( breakdance );
			mark = 0;
			watch = true; longestStill = 0;
			step = 6; phaseStart = now;
		}
		else if ( step == 6 ) {
			if ( !mark && g_emoteState.active && g_emoteState.clip == EMOTES[breakdance].clip ) { mark = t; mp_log( "[AT] Emote : Breakdance's loop after %u ms.\n", t ); }
			if ( t >= 5000 && t < 5100 ) RequestSnapshot( "emote_3_breakdance" );
			if ( t >= 7000 ) {
				Local::Check( "Breakdance : its intro, then its loop", mark && g_emoteState.active && g_emoteState.clip == EMOTES[breakdance].clip );
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
			Local::Check( "a button : Breakdance's ending, the game not getting the button", g_emoteState.active && g_emoteState.ending >= 0 && !(((uint8_t*) 0x940B78)[PAD_BUTTONS + 2] & 0x80) );
		}
		else if ( step == 8 && (!g_emoteState.active || t > 8000) ) {
			mp_log( "[AT] Emote : Breakdance's ending over after %u ms.\n", t );
			Local::Check( "both its ending clips play, then it's over", !g_emoteState.active && sawEnding[0] && sawEnding[1] );
			g_testPad[PAD_BUTTONS + 2] = 0;
			RequestPlayEmote( pickaxe );
			step = 110; phaseStart = now;
		}
		// Pickaxe Spin : the left stick (its ending, Harry held), then a button skips it.
		else if ( step == 110 && t >= 2000 && t < 2300 ) g_testPad[PAD_BUTTONS + ((const int*) PAD_BINDINGS)[14] - 11] = 0x80;
		else if ( step == 110 && t >= 2300 && t < 4000 ) g_testPad[PAD_BUTTONS + ((const int*) PAD_BINDINGS)[14] - 11] = 0;
		else if ( step == 110 && t >= 4000 ) {
			Local::Check( "turning the camera (its button) doesn't end it", g_emoteState.active && g_emoteState.emote == pickaxe );
			*(LONG*) (g_testPad + 0) = 1000;
			step = 111; phaseStart = now;
		}
		else if ( step == 111 && t >= 300 ) {
			Local::Check( "the left stick : Pickaxe Spin's ending, Harry held", g_emoteState.active && g_emoteState.ending >= 0 && *(LONG*) 0x940B78 != 1000 );
			g_testPad[PAD_BUTTONS + 3] = 0x80;
			step = 112; phaseStart = now;
		}
		else if ( step == 112 && t >= 300 ) {
			Local::Check( "a button skips the ending", !g_emoteState.active );
			g_testPad[PAD_BUTTONS + 3] = 0;
			*(LONG*) (g_testPad + 0) = 0;
			step = 130; phaseStart = now;
		}
		// Holds : Teleport Zen floats till you move (then finishes); Death lies there (then gets up).
		else if ( step == 130 && t >= 2500 ) {
			RequestPlayEmote( zen );
			step = 131;
			phaseStart = now;
		}
		else if ( step == 131 && t >= 10000 ) {
			AnimLayer* l = g_emoteState.active ? GetAnimLayer( harry, EMOTE_LAYERS[g_emoteState.slot] ) : 0;
			float f = l ? l->m_frame : -1.0F;
			mp_log( "[AT] Emote : Teleport Zen at frame %.1f, %d plays.\n", f, g_emoteState.plays );
			Local::Check( "Teleport Zen floats on, smiling (its frames 246 - 288, there and back)", g_emoteState.active && g_emoteState.holding && f >= 245.9F && f <= 288.1F && g_emoteState.plays >= 2 );
			RequestSnapshot( "emote_11_zen" );
			SendKey( 'W', true );
			step = 132; phaseStart = now;
		}
		else if ( step == 132 && t >= 300 ) {
			Local::Check( "W : it finishes, Harry staying put", g_emoteState.active && g_emoteState.finishing && g_emoteState.ending < 0 );
			step = 133; phaseStart = now;
		}
		else if ( step == 133 && (!g_emoteState.active || t > 6000) ) {
			mp_log( "[AT] Emote : Teleport Zen finished after %u ms more.\n", t + 300 );
			Local::Check( "then it's over", !g_emoteState.active );
			SendKey( 'W', false );
			step = 134; phaseStart = now;
		}
		else if ( step == 134 && t >= 2500 ) { RequestPlayEmote( death ); step = 135; phaseStart = now; }
		else if ( step == 135 && t >= 7000 ) {
			Local::Check( "Death : Harry lies there", g_emoteState.active && g_emoteState.emote == death && g_emoteState.holding );
			RequestSnapshot( "emote_12_death" );
			SendKey( 'W', true );
			step = 136; phaseStart = now;
		}
		else if ( step == 136 && t >= 700 ) {
			Local::Check( "W : he gets up (Falling Recovery), staying put", g_emoteState.active && g_emoteState.ending == 0 && g_emoteState.clip == EMOTES[death].ending[0] );
			RequestSnapshot( "emote_13_getup" );
			step = 137; phaseStart = now;
		}
		else if ( step == 137 && (!g_emoteState.active || t > 6000) ) {
			Local::Check( "then it's over", !g_emoteState.active );
			SendKey( 'W', false );
			step = 138; phaseStart = now;
		}
		// Breakdance Finish (on its own) : its pose stays, till you move.
		else if ( step == 138 && t >= 2500 ) { RequestPlayEmote( bdFinish ); step = 139; phaseStart = now; }
		else if ( step == 139 && (g_emoteState.holding || t > 10000) ) { step = 140; phaseStart = now; }
		else if ( step == 140 && t >= 2000 ) {
			Local::Check( "Breakdance Finish stays at its end", bdFinish >= 0 && g_emoteState.active && g_emoteState.emote == bdFinish && g_emoteState.holding );
			RequestSnapshot( "emote_14_bdfinish" );
			SendKey( 'W', true );
			step = 141; phaseStart = now;
		}
		else if ( step == 141 && (!g_emoteState.active || t > 3000) ) {
			mp_log( "[AT] Emote : Breakdance Finish over after %u ms.\n", t );
			Local::Check( "W : it fades out", !g_emoteState.active && t < 500 );
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
			Local::Check( "L1 + the right stick : the wheel, Running Man picked", EmoteWheelShown() && g_wheelUi.byPad && g_wheelUi.hover == 2 );
			Local::Check( "the game doesn't get the right stick (nor the slider it can be on)", *(LONG*) (0x940B78 + 0x0C) == 0 && *(LONG*) (0x940B78 + 0x18) == 0 );
			Local::Check( "nor the arrows (JoyToKey's right stick)", (uint32_t) g_testArrowFrames == arrowFrames );
			RequestSnapshot( "emote_4_wheel" );
			step = 12; phaseStart = now;
		}
		else if ( step == 12 && t >= 300 ) { g_testPad[PAD_BUTTONS + PAD_R1] = 0x80; step = 13; phaseStart = now; }
		else if ( step == 13 && t >= 300 ) {
			Local::Check( "R1 : the next wheel", g_wheelIndex == 1 && EmoteWheelShown() );
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
			Local::Check( "letting go of L1 plays it", !g_wheelUi.open && g_emoteState.active && g_emoteState.emote == running );
			step = 18; phaseStart = now;
		}
		else if ( step == 18 && t >= 2000 ) { *(LONG*) (g_testPad + 0) = 1000; step = 19; phaseStart = now; }
		else if ( step == 19 && (!g_emoteState.active || t > 3000) ) {
			Local::Check( "the left stick ends it", !g_emoteState.active );
			*(LONG*) (g_testPad + 0) = 0;
			g_testPadOn = false;
			step = 20; phaseStart = now;
		}
		// By keyboard : T, Tab (the second wheel), 2 (Dizzy Walk).
		else if ( step == 20 && t >= 2500 ) { SendKey( 'T', true ); step = 21; phaseStart = now; }
		else if ( step == 21 && t >= 400 ) {
			Local::Check( "T : the wheel", EmoteWheelShown() && !g_wheelUi.byPad );
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
			Local::Check( "2 plays the second slot at once", !EmoteWheelShown() && g_emoteState.active && g_emoteState.emote == dizzy );
			SendKey( 'T', false );
			step = 24; phaseStart = now;
		}
		else if ( step == 24 && t >= 1500 ) {
			Local::Check( "letting go of T after it plays nothing more", g_emoteState.active && g_emoteState.emote == dizzy );
			RequestSnapshot( "emote_7_dizzy" );
			SendKey( 'W', true );   // Walking : it ends, and no other starts.
			step = 25; phaseStart = now;
		}
		else if ( step == 25 && t >= 600 ) {
			Local::Check( "walking ended it", !g_emoteState.active );
			RequestPlayEmote( jig );
			step = 26; phaseStart = now;
		}
		else if ( step == 26 && t >= 300 ) {
			Local::Check( "no emote while walking", !g_emoteState.active );
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
			Local::Check( "Down, Down, Right, Right : the ring, on slot 3", g_ringSlot == 2 && g_emoteWheels[0].slot[2] == running );
			RequestSnapshot( "emote_8_page" );
			InterlockedIncrement( &g_menuKeys[MK_SRIGHT] );
			step = 32; phaseStart = now;
		}
		else if ( step == 32 && t >= 400 ) {
			Local::Check( "Shift + Right moves it round", g_ringSlot == 3 && g_emoteWheels[0].slot[3] == running && g_emoteWheels[0].slot[2] == breakdance );
			InterlockedIncrement( &g_menuKeys[MK_SLEFT] );
			step = 33; phaseStart = now;
		}
		else if ( step == 33 && t >= 400 ) {
			Local::Check( "Shift + Left puts it back", g_ringSlot == 2 && g_emoteWheels[0].slot[2] == running && g_emoteWheels[0].slot[3] == breakdance );
			InterlockedIncrement( &g_menuKeys[MK_DELETE] );
			step = 34; phaseStart = now;
		}
		else if ( step == 34 && t >= 400 ) {
			Local::Check( "Delete empties it", g_emoteWheels[0].slot[2] < 0 );
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
			g_emoteWheels[0].slot[2] = FindEmoteByName( "Idle" );   // Not a favourite : the list goes to All, on it.
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
			// The mouse wheel (a real message to the game window), the pointer over the list : a notch down.
			g_testWheelFrom = g_listScroll;
			g_menu.ListCenter( &g_testMouse.x, &g_testMouse.y );
			g_testMouse.active = true; g_testMouse.down = false;
			PostMessageA( *HWND_GAME_WINDOW, WM_MOUSEWHEEL, MAKEWPARAM( 0, (WORD) (SHORT) -WHEEL_DELTA ), 0 );
			step = 380; phaseStart = now;
		}
		else if ( step == 380 && t >= 600 ) {
			mp_log( "[AT] Emote : wheel, the list's top %d -> %d, current %d.\n", g_testWheelFrom, g_listScroll, g_listCursor );
			Local::Check( "the mouse wheel scrolls the list, the row under the pointer is the current one",
				g_listScroll == g_testWheelFrom + 3 && g_listCursor >= g_listScroll + 3 && g_listCursor <= g_listScroll + 4 );
			g_testMouse.active = false;
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
			Local::Check( "Enter : it's in the slot, back to the wheel", g_emoteWheels[0].slot[2] == dizzy && !g_chooser );
			RequestSnapshot( "emote_12_chosen" );
			MakeDefaultWheel( 0 );
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
			Local::Check( "everyone starts as a seeker", g_hsMyRole == ROLE_SEEKER );
			startMs = g_hsMyHideMs;
			HsSetRole( ROLE_HIDER );
			step = 1;
		}
		else if ( step == 1 && t >= 7000 ) {
			uint32_t gained = g_hsMyHideMs - startMs;
			mp_log( "[AT] HS : 5 s as a hider : %u ms more.\n", gained );
			Local::Check( "a hider's time goes on", gained >= 4000 && gained <= 6000 );
			bool heard = me >= 0 && g_players[me].present && g_players[me].role == ROLE_HIDER && g_hsMyHideMs - g_players[me].hideMs < 2500;
			mp_log( "[AT] HS : the room hears : role %d, %u ms (me %d).\n", me >= 0 ? g_players[me].role : -1, me >= 0 ? g_players[me].hideMs : 0, me );
			Local::Check( "the room hears our role and time", heard );
			RequestSnapshot( "hs_1_hider" );
			HsSetRole( ROLE_SEEKER );
			stopMs = g_hsMyHideMs;
			step = 2;
		}
		else if ( step == 2 && t >= 10000 ) {
			Local::Check( "a seeker's time stops (and stays)", g_hsMyHideMs == stopMs && stopMs > startMs );
			char path[MAX_PATH];
			HideTimePath( path );
			FILE* f = 0;
			unsigned long saved = 0;
			if ( fopen_s( &f, path, "r" ) == 0 && f ) { fscanf_s( f, "%lu", &saved ); fclose( f ); }
			Local::Check( "the file keeps it", saved == g_hsMyHideMs );
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
			Local::Check( "Left Shift + Right : a hider", g_hsMyRole == ROLE_HIDER );
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
			Local::Check( "L1 + d-pad left : a seeker again", g_hsMyRole == ROLE_SEEKER );
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
	case 1220: {   // Pausing, two games : 1 runs and pauses mid-run (8 s), 2 watches the other's puppet.
		DWORD t = now - phaseStart;
		g_puppetOffset.x = 0.0F;   // Puppets where the players are, as in play.
		if ( g_pauseTest == 1 ) {
			if ( step == 0 && t >= 15000 ) { mp_log( "[AT] Pause : running.\n" ); InterlockedExchange( &g_holdMs, 1500 ); InterlockedExchange( &g_holdKey, 'W' ); step = 1; }
			else if ( step == 1 && t >= 16300 ) {
				mp_log( "[AT] Pause : pausing mid-run (Harry at %.2f, %.2f).\n", harry->m_position.x, harry->m_position.y );
				InterlockedExchange( &g_tapLaterAt, (LONG) (GetTickCount() + 8000) );
				InterlockedExchange( &g_tapLaterKey, VK_ESCAPE );
				InterlockedExchange( &g_tapKey, VK_ESCAPE );
				step = 2;
			}
			else if ( step == 2 && t >= 17500 ) { mp_log( "[AT] Pause : going on (Harry at %.2f, %.2f).\n", harry->m_position.x, harry->m_position.y ); step = 3; }
			else if ( step == 3 && t >= 40000 ) InterlockedExchange( &g_autotestDone, 1 );
		}
		else {
			// Every tick : what we have of the other (their snapshot's time and place) and where their puppet is.
			static int failures = 0, backs = 0;
			static uint32_t lastT = 0, pausedT = 0;
			static DWORD lastChange = 0, pausedAt = 0, resumedAt = 0;
			struct Local { static void Check( const char* what, bool ok ) { mp_log( "[AT] Pause : %s : %s\n", what, ok ? "ok" : "WRONG" ); if ( !ok ) failures++; } };
			const PlayerSnapshot* tg = GetPuppetTarget( MainPeer() );
			EIEvilHarryP* b = MainPeer()->beast;
			if ( tg ) {
				if ( lastT && (int32_t) (tg->time - lastT) < 0 ) backs++;   // Played back in time : the replay loop.
				if ( tg->time != lastT ) lastChange = now;
				lastT = tg->time;
			}
			if ( step == 0 && t >= 10000 && tg && now - lastChange >= 1000 ) {
				pausedAt = now; pausedT = lastT;
				mp_log( "[AT] Pause : the other stopped (paused).\n" );
				RequestSnapshot( "pause_1_stopped" );
				step = 1;
			}
			else if ( step == 1 && now - pausedAt >= 5500 ) {
				Local::Check( "5.5 s into their pause, their puppet is still shown, still", tg && b && lastT == pausedT && MainPeer()->keptPaused );
				RequestSnapshot( "pause_2_kept" );
				step = 2;
			}
			else if ( step == 2 && tg && (int32_t) (lastT - pausedT) > 0 ) { resumedAt = now; mp_log( "[AT] Pause : the other goes on.\n" ); step = 3; }
			else if ( step == 3 && now - resumedAt >= 1500 ) {
				Local::Check( "they go on from there", tg && b && now - lastChange < 500 );
				RequestSnapshot( "pause_3_after" );
				step = 4;
			}
			else if ( step == 4 || t >= 50000 ) {
				Local::Check( "the test got through (pause seen, then the end of it)", step == 4 );
				Local::Check( "no replay : their playback never went back in time", backs == 0 );
				mp_log( "[AT] Pause : %d time(s) back.\n", backs );
				InterlockedExchange( &g_autotestDone, failures ? 2 : 1 );
				step = 5;
			}
			if ( t >= 10000 && step < 4 && (step != 1 || now - pausedAt < 300 || now - pausedAt > 5300) ) {
				mp_log( "[AT] Watch %5u ms : target %s t %u (%.2f, %.2f, %.2f) clip 0x%.8X | puppet %s (%.2f, %.2f) | held %u blended %u, poses %d\n",
					t, tg ? "yes" : "no", tg ? tg->time : 0, tg ? tg->position.x : 0.0F, tg ? tg->position.y : 0.0F, tg ? tg->position.z : 0.0F, tg ? tg->clipCRC : 0,
					b ? "yes" : "no", b ? b->EIBeast::m_position.x : 0.0F, b ? b->EIBeast::m_position.y : 0.0F,
					MainPeer()->remote.held, MainPeer()->remote.blended, g_puppetPoseCopies );
			}
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
			mp_log( "[AT] HS %5u ms : %s, %u ms hidden, health %d, nearest seeker %.1f\n", t, g_hsMyRole == ROLE_HIDER ? "hider" : "seeker", g_hsMyHideMs, harry->m_healthValue, g_testNearestSeeker );
		}
		if ( g_hsTest == 2 || g_hsTest == 5 ) {
			if ( step == 0 && t >= 12000 ) { mp_log( "[AT] HS : running off.\n" ); InterlockedExchange( &g_holdMs, 2500 ); InterlockedExchange( &g_holdKey, 'W' ); step = 1; }
			else if ( step == 1 && t >= 17000 ) {
				startHealth = harry->m_healthValue;
				HsSetRole( ROLE_HIDER );
				hiddenAt = g_hsMyHideMs;
				step = 2;
			}
			else if ( step == 2 && t >= 22000 ) {
				if ( expectCatch ) Local::Check( "a seeker far off doesn't catch", g_hsMyRole == ROLE_HIDER && g_testNearestSeeker > CATCH_DISTANCE );
				else Local::Check( "someone not playing isn't a seeker", g_hsMyRole == ROLE_HIDER && g_testNearestSeeker < 0.0F );
				int other = -1;
				for ( int i = 0; i < MAX_PEERS; i++ ) if ( i != me && g_players[i].present ) other = i;
				if ( expectCatch ) Local::Check( "we see the seeker's name tag", other >= 0 && !HiddenFromMe( other ) );
				Local::Check( "the hide time goes on", g_hsMyHideMs - hiddenAt >= 4000 );
				RequestSnapshot( "hs_3_hiding" );
				step = 3;   // Waiting for the seeker.
			}
			else if ( step == 3 && !expectCatch && t >= 45000 ) { Local::Check( "not caught by someone not playing", g_hsMyRole == ROLE_HIDER ); step = 5; }
			else if ( step == 3 && g_hsMyRole == ROLE_SEEKER && !caughtAt ) {
				if ( !expectCatch ) { Local::Check( "not caught by someone not playing", false ); step = 5; break; }
				caughtAt = now; caughtMs = g_hsMyHideMs;
				Local::Check( "touching the seeker catches us, Harry dies", harry->m_healthValue <= 0 );
				RequestSnapshot( "hs_4_caught" );
				step = 4;
			}
			else if ( step == 3 && t >= 45000 ) { Local::Check( "caught once the seeker came", false ); step = 5; }
			else if ( step == 4 && now - caughtAt >= 15000 ) {
				Local::Check( "back after dying, health full again", harry->m_healthValue > 0 && harry->m_healthValue >= startHealth );
				Local::Check( "a caught hider's time stops", g_hsMyHideMs == caughtMs );
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
	// Outfits : the puppet wears each in turn; then ours, which a level load puts on Harry.
	case 700: {
		static int failures = 0;
		DWORD t = now - phaseStart;
		EIEvilHarryP* b = MainPeer()->beast;
		if ( step < NUM_OUTFITS ) {
			if ( !b ) { InterlockedExchange( &g_autotestDone, 2 ); break; }
			g_testOutfit = step;
			if ( t < 2500 ) break;

			bool ok = MainPeer()->shownOutfit == step && b->m_modelCRC == OUTFITS[step].model;
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
			SpawnPuppet( MainPeer() );   // It replays us : Retro too.
			static const uint8_t TEST[RETRO_PARTS][3] = { { 245, 245, 245 }, { 255, 161, 123 }, { 220, 50, 40 }, { 50, 100, 220 } };
			memcpy( g_myColors, TEST, sizeof(g_myColors) );   // White hair, red shirt, blue trousers.
			g_retroSwaps = g_retroMatched = g_retroUnmatched = 0;
			step++; phaseStart = now;
		}
		else {
			if ( t < 3000 ) break;
			bool ok = b && MainPeer()->shownOutfit == 3 && g_retroSwaps > 100 && g_retroUnmatched * 50 < g_retroMatched;
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
	// The menu : keys, the mouse, and Harry standing still meanwhile.
	case 600: {
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
			Local::Check( "Right on the role : a hider", g_hsMyRole == ROLE_HIDER );
			InterlockedIncrement( &g_menuKeys[MK_RIGHT] ); step = 26; phaseStart = now; break;
		case 26:
			if ( t < 400 ) break;
			Local::Check( "Right again : a seeker", g_hsMyRole == ROLE_SEEKER );
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
				g_viewerPeer ? g_viewerPeer->mirrorClip : 0, MainPeer()->beast ? MainPeer()->beast->m_animIndex : -1 );
		}
		step++;
		break;
	}
	case 201:   // Animation survey : next clip.
		if ( step >= g_animClipCount ) {
			g_animPreviewCRC = 0;
			mp_log( "[AT] ANIM : done, %d clips filmed.\n", g_animClipCount );
			InterlockedExchange( &g_autotestDone, 1 );
			break;
		}
		g_animPreviewCRC = g_animClips[step].crc;
		phase = 202; phaseStart = now; snapped = false;
		lastHarryTicks = 0;   // Frames taken of this clip.
		break;
	case 202: {   // Animation survey : film it.
		static const int FRAMES = 6;
		static const DWORD FIRST_FRAME_MS = 200, FRAME_GAP_MS = 500;
		if ( lastHarryTicks >= FRAMES ) { step++; phase = 201; break; }
		if ( now - phaseStart < FIRST_FRAME_MS + (DWORD) lastHarryTicks * FRAME_GAP_MS || g_cropPending ) break;
		// Around the puppet : its name tag point is just above its head.
		const Peer* pe = MainPeer();
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
				SetAirAnimation( MainPeer()->beast, AIR_CANDIDATES[0], MainPeer() );
				phase = 8; phaseStart = now; step = 0; snapped = false;
				break;
			}
			SetAirAnimation( MainPeer()->beast, AIR_CANDIDATES[JUMP_CLIP[step]], MainPeer() );
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
				SetIdleAnimation( MainPeer()->beast, 0, MainPeer() );
				phase = 1; phaseStart = now; step = 0; snapped = false;
				break;
			}
			mp_log( "[AT] Idle %d : %s.\n", step, IDLE_CANDIDATES[step] ? IDLE_CANDIDATES[step] : "quickdata idle anim" );
			SetIdleAnimation( MainPeer()->beast, IDLE_CANDIDATES[step], MainPeer() );
			step++; phaseStart = now; snapped = false;
		}
		else if ( !snapped && now - phaseStart >= 2500 ) {
			char name[40];
			sprintf_s( name, "05_idle_%d", step - 1 );
			RequestSnapshot( name );
			snapped = true;
		}
		break;

	case 8:   // Items : hold numpad 5 and tap an arrow to take an item out or put it away.
	{
		// Evil Harry should hold, and move, what Harry did a second earlier. The wheel
		// starts as raft (up), pickaxes (right), torch (down), TNT (left); the other
		// items go on the up slot (like picking them in the wheel menu), then an empty
		// slot (the menu's hand). "move" steps hold an arrow alone : it moves the item
		// in hand (torch aside, shield over the head, mask on, drink).
		static const int KEEP = -2, EMPTY = -1;
		static const struct { LONG key; bool move; int upItem; int expect; const char* name; } STEPS[] = {
			{ 0,        false, KEEP,                    -1,                      "empty handed" },
			{ VK_RIGHT, false, KEEP,                    InventoryItem::PICKAXES, "pickaxes out" },
			{ VK_RIGHT, false, KEEP,                    -1,                      "pickaxes away" },
			{ VK_DOWN,  false, KEEP,                    InventoryItem::TORCH,    "torch out" },
			{ VK_LEFT,  true,  KEEP,                    InventoryItem::TORCH,    "torch moved" },
			{ VK_DOWN,  false, KEEP,                    -1,                      "torch away" },
			{ VK_UP,    false, KEEP,                    InventoryItem::RAFT,     "raft out" },
			{ VK_UP,    false, KEEP,                    -1,                      "raft away" },
			{ VK_LEFT,  false, KEEP,                    InventoryItem::TNT,      "TNT out" },
			{ VK_LEFT,  false, KEEP,                    -1,                      "TNT away" },
			{ VK_UP,    false, InventoryItem::SLING,    InventoryItem::SLING,    "sling out" },
			{ VK_UP,    false, KEEP,                    -1,                      "sling away" },
			{ VK_UP,    false, InventoryItem::SHIELD,   InventoryItem::SHIELD,   "shield out" },
			{ VK_UP,    true,  KEEP,                    InventoryItem::SHIELD,   "shield raised" },
			{ VK_UP,    false, KEEP,                    -1,                      "shield away" },
			{ VK_UP,    false, InventoryItem::GAS_MASK, InventoryItem::GAS_MASK, "gas mask out" },
			{ VK_UP,    true,  KEEP,                    InventoryItem::GAS_MASK, "gas mask on" },
			{ VK_UP,    false, KEEP,                    -1,                      "gas mask away" },
			{ VK_UP,    false, InventoryItem::CANTEEN,  InventoryItem::CANTEEN,  "canteen out" },
			{ VK_UP,    true,  KEEP,                    InventoryItem::CANTEEN,  "canteen drink" },
			{ VK_UP,    false, KEEP,                    -1,                      "canteen away" },
			{ VK_UP,    false, EMPTY,                   -1,                      "empty slot" },
		};
		static const int NUM = sizeof(STEPS) / sizeof(STEPS[0]);
		static int drawsAtStart = 0, posesAtStart = 0, aloneAtStart = 0;
		DWORD t = now - phaseStart;
		if ( step == 0 || t >= 4500 ) {
			if ( step >= NUM ) { g_puppetOffset.x = 6.0F; phase = 3; phaseStart = now; step = 0; break; }
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
			EInstance* m0 = MainPeer()->beast ? MainPeer()->beast->m_pickaxeModel0 : 0;
			EInstance* m1 = MainPeer()->beast ? MainPeer()->beast->m_pickaxeModel1 : 0;
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
			const PlayerSnapshot* tg = GetPuppetTarget( MainPeer() );
			mp_log( "[AT] roll t=%4u clip 0x%.8X ground %d pos (%.2f %.2f %.2f) | xf r0 (%.2f %.2f %.2f) r1 (%.2f %.2f %.2f) r2 (%.2f %.2f %.2f) t (%.2f %.2f %.2f) | rb r0 (%.2f %.2f %.2f) r2 (%.2f %.2f %.2f) t (%.2f %.2f %.2f) | puppet z %.2f\n",
				t, tg ? tg->clipCRC : 0, harry->m_onGround, harry->m_position.x, harry->m_position.y, harry->m_position.z,
				m.data[0][0], m.data[0][1], m.data[0][2], m.data[1][0], m.data[1][1], m.data[1][2], m.data[2][0], m.data[2][1], m.data[2][2],
				m.data[3][0], m.data[3][1], m.data[3][2],
				rb.data[0][0], rb.data[0][1], rb.data[0][2], rb.data[2][0], rb.data[2][1], rb.data[2][2], rb.data[3][0], rb.data[3][1], rb.data[3][2],
				MainPeer()->beast ? MainPeer()->beast->m_worldMatrix.data[3][2] : 0.0F );
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


static void QuitGame( UINT code )
{
	mp_log( "[MP] The game is closing (exit code %u).\n", code );
	LeaveRoom();
	g_origExitProcess( code );
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
		if ( g_tapLaterKey && (LONG) (GetTickCount() - (DWORD) g_tapLaterAt) >= 0 ) InterlockedExchange( &g_tapKey, InterlockedExchange(&g_tapLaterKey, 0) );
		if ( g_pauseTest == 1 ) {   // While paused, the script doesn't run : what Harry and the sending do, from here.
			static DWORD lastLog = 0;
			static LONG ticksWas = 0, sentWas = 0;
			if ( GetTickCount() - lastLog >= 100 ) {
				lastLog = GetTickCount();
				LONG ticks = g_harryTicks, sent = (LONG) g_netStats.sent;
				if ( ticks != ticksWas || sent != sentWas || g_tapLaterKey ) mp_log( "[AT] Pause : Harry ticks %ld (+%ld), sent %ld (+%ld).\n", ticks, ticks - ticksWas, sent, sent - sentWas );
				ticksWas = ticks; sentWas = sent;
			}
		}
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

// Where the main peer's puppet and Harry are.
static void LogTelemetry( EIHarry* harry, const char* tag )
{
	Peer* peer = MainPeer();
	if ( !peer->beast ) {
		mp_log( "[AT] %s harry (%.1f, %.1f, %.1f), no puppet\n", tag, harry->m_position.x, harry->m_position.y, harry->m_position.z );
		return;
	}

	EIEvilHarryP* b = peer->beast;
	const Matrix4f& w = b->m_worldMatrix;
	mp_log( "[AT] %s beast world (%.1f, %.1f, %.1f), anim layer %d, moving %d, speed %.3f, heading %.0f\n", tag,
		w.data[3][0], w.data[3][1], w.data[3][2], b->m_animIndex,
		(int) peer->moving, peer->moveSpeed, peer->heading * 180.0F / MP_PI );
	const Vector3f& r = b->m_scale;
	mp_log( "[AT] %s harry (%.1f, %.1f, %.1f) puppet (%.1f, %.1f, %.1f) scale (%.2f, %.2f, %.2f) matrix (%.1f, %.1f, %.1f) state %d\n", tag,
		harry->m_position.x, harry->m_position.y, harry->m_position.z,
		b->EICharacter::m_position.x, b->EICharacter::m_position.y, b->EICharacter::m_position.z,
		r.x, r.y, r.z,
		b->m_transformMatrix.data[3][0], b->m_transformMatrix.data[3][1], b->m_transformMatrix.data[3][2],
		b->m_stateIndexCurrent );
}

// How much packing a matrix loses (logs the worst ones).
static void CheckPacking( const PlayerSnapshot& sent, const char* what, int index, const Matrix4f& m, bool perAxis = false )
{
	if ( !IsSaneMatrix(m) ) return;   // Sent as nothing.

	float err = PackingError( m, perAxis );
	if ( err <= g_netStats.maxPackingError ) return;

	g_netStats.maxPackingError = err;
	if ( err < 0.01F ) return;

	const float (*a)[4] = m.data;
	mp_log( "[NET] Packing loses %.4f on %s %d (held item %d, drawn %d) : (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f) t (%.3f %.3f %.3f)\n",
		err, what, index, sent.heldItem, sent.drawnSlot, a[0][0], a[0][1], a[0][2], a[1][0], a[1][1], a[1][2], a[2][0], a[2][1], a[2][2], a[3][0], a[3][1], a[3][2] );
}

// PTLE_MP_CRASHTEST : the crash logger's test.
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


//
// Hooks (see tests.h).
//

void TestBeforeHarryTick()
{
	if ( g_crashTest && g_harryTicks == 300 )
		RunCrashTest();
}

void TestAfterHarryTick( EIHarry* harry )
{
	if ( g_autotest )
		AutotestStep( harry );
}

bool TestSpawnsPuppet( const Peer* peer )
{
	// The two-game tests leave it to the game, as in play.
	return g_autotest && peer == MainPeer() && g_pitTest < 4 && g_distTest < 2 && g_hsTest < 2;
}

void TestMovePuppetTarget( Vector3f* position )
{
	if ( g_puppetPinned ) {
		*position = g_puppetPin;
		return;
	}
	position->x += g_puppetOffset.x;
	position->y += g_puppetOffset.y;
	position->z += g_puppetOffset.z;
}

int TestPuppetOutfit()
{
	return g_testOutfit;
}

bool TestStallsPuppet( EIEvilHarryP* puppet )
{
	return g_testStallPuppet && puppet == g_testStalled;
}

bool TestSkipsPuppetAI()
{
	return g_skipPuppetAI;
}

bool TestHidesHarryItem( EItemBase* item )
{
	return g_testSkipHarryItemDraw && item->m_harry == Gizmod::getHarry();
}

bool TestAnimSurveyOn()
{
	return g_animSurvey;
}

void TestCheckPacking( const PlayerSnapshot& sent )
{
	if ( !g_autotest ) return;

	for ( int i = 0; i < sent.bones; i++ )
		CheckPacking( sent, "bone", i, sent.pose[i] );
	CheckPacking( sent, "right hand", 0, sent.handR );
	CheckPacking( sent, "left hand", 0, sent.handL );
	for ( int i = 0; i < sent.itemBones; i++ )
		CheckPacking( sent, "item bone", i, sent.itemPose[i], true );
}

bool TestInBackground()
{
	return g_background;
}

bool TestPollKeyboard( void (*poll)(), uint8_t* keysNow, uint8_t* keysBefore )
{
	if ( !g_background ) return false;

	uint8_t before[256];
	memcpy( before, keysNow, sizeof(before) );
	poll();
	memcpy( keysBefore, before, sizeof(before) );   // Not cleared : held keys stay held.
	for ( int i = 0; i < 256; i++ )
		keysNow[i] = g_fakeKeys[i];
	return true;
}

void TestSawKeys( const uint8_t* keys )
{
	if ( keys[0xCD] & 0x80 )   // The right arrow.
		InterlockedIncrement( &g_testArrowFrames );
}

DIJOYSTATE2* TestPollGamepad( int index, DIJOYSTATE2* state )
{
	if ( !g_testPadOn || index != 0 ) return state;

	// As the game does : the last state kept, then the new one.
	uint8_t* now = (uint8_t*) (0x940B78 + index * PAD_STATE_SIZE);
	memcpy( (void*) (0x940DA8 + index * PAD_STATE_SIZE), now, PAD_STATE_SIZE );
	memcpy( now, g_testPad, PAD_STATE_SIZE );
	return (DIJOYSTATE2*) now;
}

void TestSawGamepad( const DIJOYSTATE2* state )
{
	if ( LOWORD(state->rgdwPOV[0]) != 0xFFFF )   // The d-pad.
		InterlockedIncrement( &g_testHatFrames );
}

bool TestMenuInput( MenuInput* in, char* pasted, size_t size )
{
	if ( g_testPaste[0] ) {
		strcpy_s( pasted, size, g_testPaste );
		g_testPaste[0] = 0;
		in->pasted = pasted;
	}
	if ( !g_testMouse.active ) return false;

	in->mouseX = g_testMouse.x;
	in->mouseY = g_testMouse.y;
	in->mouseInside = true;
	in->mouseDown = g_testMouse.down;
	return true;
}

// The nearest seeker (while we hide) and, each second, the hide time and where the
// others' puppets are.
void TestHsTick( EIHarry* harry )
{
	if ( !g_autotest ) return;

	const int me = g_transport ? g_transport->PlayerId() : -1;
	g_testNearestSeeker = -1.0F;
	if ( PlayingHs() && g_hsMyRole == ROLE_HIDER && me >= 0 && harry->m_healthValue > 0 ) {
		for ( int i = 0; i < MAX_PEERS; i++ ) {
			const PlayerInfo& info = g_players[i];
			const Peer& p = g_peers[i];
			if ( i == me || !info.present || info.role != ROLE_SEEKER || !p.beast || PeerInLimbo(&p) || !p.target ) continue;

			const Vector3f& s = p.beast->EIBeast::m_position;
			const float dx = s.x - harry->m_position.x, dy = s.y - harry->m_position.y, dz = s.z - harry->m_position.z;
			const float d = sqrtf( dx * dx + dy * dy + dz * dz );
			if ( g_testNearestSeeker < 0.0F || d < g_testNearestSeeker )
				g_testNearestSeeker = d;
		}
	}

	static uint32_t lastReport = 0;
	const uint32_t now = GetTickCount();
	if ( !g_testHideReport || now - lastReport < 1000 ) return;

	lastReport = now;
	mp_log( "[AT] HS : %s, hide time %u ms.\n", g_hsMyRole == ROLE_HIDER ? "hider" : "seeker", g_hsMyHideMs );
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		const Peer& p = g_peers[i];
		if ( i == me || !p.beast || PeerInLimbo(&p) ) continue;

		const Vector3f& s = p.beast->EIBeast::m_position;
		mp_log( "[AT] HS : player %d's puppet is (%.2f, %.2f, %.2f) from Harry.\n", i, s.x - harry->m_position.x, s.y - harry->m_position.y, s.z - harry->m_position.z );
	}
}

void TestHsHeardCatch( int hider, int seeker )
{
	g_testCaughtBy = seeker;
	g_testCaughtHider = hider;
}


void EnableTests()
{
	char env[8] = { 0, };
	if ( !GetEnvironmentVariableA("PTLE_MP_AUTOTEST", env, sizeof(env)) || env[0] != '1' ) return;

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
	if ( GetEnvironmentVariableA( "PTLE_MP_PAUSETEST", env, sizeof(env) ) && (env[0] == '1' || env[0] == '2') ) {
		g_pauseTest = env[0] - '0';
		mp_log( "[AT] Pause test (%s).\n", g_pauseTest == 1 ? "pausing" : "watching" );
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
	if ( GetEnvironmentVariableA("PTLE_MP_HSTEST", env, sizeof(env)) && env[0] >= '1' && env[0] <= '9' ) {
		g_hsTest = env[0] - '0';
		g_testHideReport = true;
		mp_log( "[AT] Hide and seek test %d.\n", g_hsTest );
	}
	if ( GetEnvironmentVariableA("PTLE_MP_SKINTEST", env, sizeof(env)) && env[0] == '1' ) {
		g_skinTest = true;
		g_showList = false;
		mp_log( "[AT] Outfit test.\n" );
	}
	if ( GetEnvironmentVariableA("PTLE_MP_MENUTEST", env, sizeof(env)) && env[0] == '1' ) {
		g_menuTest = true;
		g_showList = false;
		mp_log( "[AT] Menu test.\n" );
	}
	if ( GetEnvironmentVariableA("PTLE_MP_VIEWERTEST", env, sizeof(env)) && env[0] == '1' ) {
		g_viewerTest = true;
		mp_log( "[AT] Animation viewer test.\n" );
	}
	if ( GetEnvironmentVariableA("PTLE_MP_ANIMSURVEY", env, sizeof(env)) && env[0] == '1' ) {
		g_animSurvey = true;
		g_puppetOffset.x = 6.0F;   // Placed beside Harry, from the camera, when the survey starts.
		AUTOTEST_TOTAL_TIMEOUT_MS = 3600000;
		mp_log( "[AT] Animation survey mode.\n" );
	}
	if ( GetEnvironmentVariableA("PTLE_MP_SURVEY", env, sizeof(env)) && (env[0] == '1' || env[0] == '2' || env[0] == '3') ) {
		g_survey = true;
		if ( env[0] == '2' ) { SURVEY_LEVELS = SURVEY_BIG; NUM_SURVEY_LEVELS = sizeof(SURVEY_BIG) / sizeof(SURVEY_BIG[0]); }
		if ( env[0] == '3' ) { SURVEY_LEVELS = SURVEY_WATER; NUM_SURVEY_LEVELS = sizeof(SURVEY_WATER) / sizeof(SURVEY_WATER[0]); }
		AUTOTEST_TOTAL_TIMEOUT_MS = 1200000;
		mp_log( "[AT] Level survey mode.\n" );
	}

	CreateThread( 0, 0, AutotestThread, 0, 0, 0 );
}


#endif // PTLE_MP_EMBEDTESTS
