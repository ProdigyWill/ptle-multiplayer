#include "AnimViewer.h"

#include "Multiplayer.h"
#include "Network.h"
#include "Puppets.h"
#include "Render.h"
#include "Input.h"

#include "net/transport.h"
#include "tests.h"

#include "ptle/EIHarry.h"
#include "ptle/EIEvilHarryP.h"
#include "ptle/math/Vector3f.h"

#include <Windows.h>

#include <stdio.h>
#include <string.h>
#include <math.h>


// --------------------------------------------------------------------------------
// Animation viewer (F10) : a puppet pinned beside Harry plays one clip at a time,
// for going through Harry's animations and noting what they are.
// --------------------------------------------------------------------------------

bool g_animViewer = false;
Peer* g_viewerPeer = 0;                 // The puppet it uses.
Vector3f g_viewerPin;                   // Where that puppet stands.

uint32_t g_animPreviewCRC = 0;          // The clip the puppet plays (0 : normal).

// The Emotes page's preview uses the same : g_viewerPeer, while it's choosing. Between
// previews its puppet is parked out of sight (not drawn, far below), kept for the next.
bool g_emotePreview = false;
Peer* g_parkedPeer = 0;
Vector3f g_parkedPin;

bool IsParkedPeer( const Peer* p ) { return p && p == g_parkedPeer && !((g_animViewer || g_emotePreview) && p == g_viewerPeer); }
bool IsViewerPeer( const Peer* p ) { return ((g_animViewer || g_emotePreview) && p == g_viewerPeer) || IsParkedPeer( p ); }

// The clip the current peer's puppet is made to play (survey, viewer), else 0.
uint32_t ForcedClip( const Peer* p )
{
	return (IsViewerPeer(p) || TestAnimSurveyOn()) ? g_animPreviewCRC : 0;
}

// --------------------------------------------------------------------------------
// Animation notes. Its clip list and notes are in
// %LOCALAPPDATA%\PTLE Multiplayer\anim-notes.txt, one clip per line :
//     <number> <clip id> <where found> [*] [notes]
// The numbers are the animation survey's. Edited by hand (reloaded when it changes)
// and by F11 (star). Clips found on Harry and not in it yet are added.
// --------------------------------------------------------------------------------
AnimNote g_animNotes[MAX_ANIM_NOTES];
int g_animNoteCount = 0;
int g_animIndex = 0;
bool g_animStarredOnly = false;
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

void HandleAnimViewer( EIHarry* harry )
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
