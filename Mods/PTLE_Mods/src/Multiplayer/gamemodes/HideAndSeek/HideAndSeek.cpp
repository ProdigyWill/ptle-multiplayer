#include "HideAndSeek.h"

#include "Multiplayer.h"

#include "net/transport.h"

#include "ptle/EIHarry.h"
#include "ptle/EIEvilHarryP.h"

#include <Windows.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>


int      g_hsMyRole = ROLE_SEEKER;      // Everyone starts as a seeker.
uint32_t g_hsMyHideMs = 0;              // Total hide time.

HsCaughtOut g_caughtOut = { -1, 0, 0, 0 };
static uint32_t g_catchNumber = 0;             // Ours (a random start : another session's don't look the same).
static uint32_t g_lastCatchHeard[MAX_PEERS];   // Each hider's last catch number heard.


//
// Hide & seek time as a hider.
//

// Names : printable ASCII only (the font has nothing else), never empty.
// The hide time's file : %LOCALAPPDATA%\PTLE Multiplayer\hide-time.txt, the total in
// milliseconds (the autotests keep theirs in hide-time-autotest.txt).
static void HideTimePath( char* path )   // MAX_PATH.
{
	path[0] = 0;
	if ( GetDataDir(path) )
		strcat_s( path, MAX_PATH, "\\hide-time.txt" );
}

static bool g_hideTimeLoaded = false;
static uint32_t g_hideSavedMs = 0;

static void SetMyRole( int role )
{
	if ( role == g_hsMyRole ) return;
	g_hsMyRole = role;
	mp_log( "[HS] Now a %s (hide time %u ms).\n", role == ROLE_HIDER ? "hider" : "seeker", g_hsMyHideMs );
	SaveHideTime();
}


void HsSetRole( int role )
{
	SetMyRole( role );
}

void HsToggleRole( const char* how )
{
	SetMyRole( g_hsMyRole == ROLE_HIDER ? ROLE_SEEKER : ROLE_HIDER );
	mp_log( "[HS] Role swapped (%s).\n", how );
	Toast( g_hsMyRole == ROLE_HIDER ? "You are now a hider" : "You are now a seeker" );
}

void HsResetHideTime()
{
	g_hsMyHideMs = 0;
	SaveHideTime();
}

void HsReceiveCaught( int player, int seeker, int seq, int me )
{
	if ( g_lastCatchHeard[player] == seq ) return;   // A resend.

	g_lastCatchHeard[player] = seq;
	g_players[player].role = ROLE_SEEKER;   // (Their next info says so too.)

	const char* playerName = PlayerName( player );
	const char* seekerName = PlayerName( seeker );

	mp_log( "[HS] Player %d (\"%s\") was caught by player %d (\"%s\").\n", player, playerName, seeker, seekerName );
	if ( player == me ) { }
	else if ( seeker == me )
		Toast( "You caught %s!", playerName );
	else {
		char hider[MAX_NAME + 1];
		strcpy_s( hider, playerName );
		Toast( "%s caught %s", seekerName, hider );
	}
}

// A hider touched by a seeker (their puppet, as we show it) : Harry dies the game's
// way (he comes back at the last loadzone), and we're a seeker from now on.
static void CheckCaught( EIHarry* harry )
{
	int me = g_transport ? g_transport->PlayerId() : -1;

	if ( !PlayingHs() || g_hsMyRole != ROLE_HIDER || me < 0 || harry->m_healthValue <= 0 ) return;

	for ( int i = 0; i < MAX_PEERS; i++ ) {
		const PlayerInfo& info = g_players[i];
		const Peer& p = g_peers[i];
		if ( i == me || !info.present || info.role != ROLE_SEEKER || !p.beast || PeerInLimbo(&p) || !p.target ) continue;

		const Vector3f& s = p.beast->EIBeast::m_position;
		const float dx = s.x - harry->m_position.x, dy = s.y - harry->m_position.y, dz = s.z - harry->m_position.z;
		const float d = sqrtf( dx * dx + dy * dy + dz * dz );
		if ( d >= CATCH_DISTANCE ) continue;

		mp_log( "[HS] Caught by player %d (\"%s\"), %.2f away : Harry dies.\n", i, info.name, d );
		Toast( "%s caught you!", info.name );
		if ( !g_catchNumber )
			g_catchNumber = GetTickCount() | 1;
		g_caughtOut.seeker = i;
		g_caughtOut.number = ++g_catchNumber;
		g_caughtOut.left = CAUGHT_SENDS;
		g_caughtOut.lastSent = 0;
		SetMyRole( ROLE_SEEKER );
		harry->m_healthValue = 0;
		return;
	}
}

// Each of Harry's ticks : a hider's time goes on. A gap (loading, the pause menu :
// Harry doesn't tick then) doesn't count.
static void HideTick()
{
	LoadHideTime();
	static uint32_t last = 0;
	const uint32_t now = GetTickCount();
	const uint32_t dt = now - last;
	last = now;

	if ( g_hsMyRole != ROLE_HIDER ) return;

	if ( PlayingHs() && dt < 250 )
		g_hsMyHideMs += dt;
	if ( g_hsMyHideMs - g_hideSavedMs >= 5000 )
		SaveHideTime();   // Every 5 s while hiding.
}

void HsUpdate( EIHarry* harry )
{
	HideTick();
	CheckCaught( harry );
}

void HsEnter()
{

}

void HsExit()
{
	SetMyRole( ROLE_SEEKER );   // Back as a seeker, next time.
}

// 754000 ms : "12:34"; an hour or more : "1:02:03".
void FormatHideTime( uint32_t ms, char* out, size_t size )
{
	uint32_t s = ms / 1000;
	if ( s >= 3600 ) sprintf_s( out, size, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60 );
	else sprintf_s( out, size, "%u:%02u", s / 60, s % 60 );
}

void LoadHideTime()
{
	if ( g_hideTimeLoaded ) return;

	g_hideTimeLoaded = true;
	char path[MAX_PATH];
	HideTimePath( path );
	FILE* f = 0;
	if ( path[0] && fopen_s(&f, path, "r") == 0 && f ) {
		unsigned long ms = 0;
		if ( fscanf_s(f, "%lu", &ms) == 1 ) g_hsMyHideMs = (uint32_t) ms;
		fclose( f );
	}
	g_hideSavedMs = g_hsMyHideMs;
	mp_log( "[HS] Hide time : %u ms (%s).\n", g_hsMyHideMs, path );
}

void SaveHideTime()
{
	if ( !g_hideTimeLoaded ) return;

	char path[MAX_PATH], temp[MAX_PATH];
	HideTimePath( path );
	if ( !path[0] ) return;

	sprintf_s( temp, "%s.new", path );
	FILE* f = 0;
	if ( fopen_s(&f, temp, "w") != 0 || !f ) return;

	fprintf( f, "%u\n", g_hsMyHideMs );
	fclose( f );
	if ( MoveFileExA(temp, path, MOVEFILE_REPLACE_EXISTING) )
		g_hideSavedMs = g_hsMyHideMs;
}
