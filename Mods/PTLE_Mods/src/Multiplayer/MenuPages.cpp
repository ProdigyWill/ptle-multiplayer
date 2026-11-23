#include "MenuPages.h"

#include "Multiplayer.h"
#include "Network.h"
#include "Puppets.h"
#include "Render.h"
#include "Outfits.h"
#include "Input.h"
#include "AnimViewer.h"

#include "version.h"
#include "emotes_data.h"
#include "overlay.h"
#include "menu.h"
#include "net/transport.h"
#include "gamemodes/HideAndSeek/HideAndSeek.h"
#include "tests.h"

#include "ptle/EIHarry.h"
#include "ptle/EIEvilHarryP.h"

#include <Windows.h>

#include <stdio.h>
#include <string.h>
#include <math.h>


// --------------------------------------------------------------------------------
// The Emotes page (F1) : a wheel's 8 slots round a ring, and choosing an emote for
// one, with a preview : the animation viewer's puppet beside Harry, playing the
// highlighted emote again and again.
// --------------------------------------------------------------------------------
static int g_editWheel = 0;       // The wheel the page shows.
int g_ringSlot = 0;               // Its slot the ring points at.
bool g_chooser = false;           // Choosing an emote for that slot.
char g_emoteSearch[32] = "";
int g_emoteCategory = 0;          // 0 : favourites, 1 : all, then EMOTE_CATEGORIES.
int g_listCursor = 0, g_listScroll = 0;
static int g_listWant = -2;       // EMOTES index to put the list's cursor on (-1 : "Empty slot"; -2 : none).
static int g_armed = 0;           // Pressed once, to be pressed again : 1 reset, 2 delete.
static DWORD g_armedAt = 0;
int g_previewWant = -2;           // The chooser, each frame : EMOTES index, -1 : none (idle), -2 : no preview.
static bool g_previewFailed = false;
static int g_previewShown = -3;
static int g_previewStage = 0;    // A chain : 0 its intro, 1 - 2 its loop, 3 - 4 its ending.
static bool g_previewArmed = false;   // The clip seen under way (not at its end from last time).
static const uint32_t IDLE_CLIP = 0xB23CB6D0;

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


void EmotePreviewTick( EIHarry* harry )
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


int BuildPlayerRows( char left[][96], char right[][64], uint32_t* colors, const char* lastRight )
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

bool IsMenuOpen()
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
void DrawMenu( float sw, float sh, float h )
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
void SyncMenuOpen()
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
