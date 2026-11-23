#pragma once

#include <stdint.h>

class EIHarry;


// Hide and seek (the "PTLE Multiplayer: Hide-and-Seek Plan" doc). No rounds and no
// host : each player chooses their own role. A hider's hide time adds up while
// they're one and Harry is in a level (not loading, not paused); only they can
// reset it, and it's kept in a file so restarts and crashes don't lose it.
enum { ROLE_SEEKER, ROLE_HIDER, ROLE_NONE };   // ROLE_NONE (sent) : not playing, the game mode is off.

// Caught : a seeker as close as this to a hider, as the hider's game shows him
// (Harry is about 7 units tall). The hider's game decides, so a catch is never a
// surprise to the one caught.
static const float CATCH_DISTANCE = 3.5F;
static const int CAUGHT_SENDS = 5;             // The CAUGHT packet, this many times...
static const uint32_t CAUGHT_RESEND_MS = 200;  // ...this far apart.


extern int g_hsMyRole;
extern uint32_t g_hsMyHideMs;

// Used to track packet sends when caught.
extern struct HsCaughtOut { int seeker; uint32_t number; int left; uint32_t lastSent; } g_caughtOut;


// Definitions.

bool PlayingHs();   // The game mode is hide and seek.
bool HiddenFromMe( int id );   // A hider's name tag and whereabouts are hidden from everyone else.


void HsSetRole( int role );
void HsToggleRole( const char* how );
void HsResetHideTime();

void HsReceiveCaught( int player, int seeker, int seq, int me );

void HsUpdate( EIHarry* context );

void HsEnter();
void HsExit();

void FormatHideTime( uint32_t ms, char* out, size_t size );

void HideTimePath( char* path );   // MAX_PATH.
void LoadHideTime();
void SaveHideTime();
