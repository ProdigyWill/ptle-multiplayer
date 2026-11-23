#pragma once

#include "net/remote.h"
#include "net/protocol.h"

#include <Windows.h>


#ifdef _DEBUG
#	define PTLE_MP_EMBEDTESTS   // The autotests (tests.h).
#endif


class EAnimController;
class EIBeast;
class EICharacter;
class EIEvilHarryP;
class EIHarry;
class EItemBase;
class ERLevel;
class Matrix4f;
class Menu;

struct AnimLayer;
struct IDirect3DDevice9;
struct ItemStruct;

struct PlayerSnapshot;
class Transport;


extern Transport* g_transport;


// Peers.

static const int MAX_PEERS = 8;
struct Peer
{
	int id;
	bool heard;                        // Snapshots are coming in from them.
	uint32_t lastHeard;
	uint32_t lastLevel;                // Their last snapshot's level.
	bool keptPaused;                   // Silent, but paused : kept where they are.
	uint32_t pausedSeen;               // When they last said they were paused (kept a moment after : their
	// presence saying they go on can come before their first snapshot).
	RemotePlayer remote;
	const PlayerSnapshot* target;      // What their puppet shows this tick (see NetUpdate).

	// Their puppet.
	EIEvilHarryP* beast;
	int           shownOutfit;         // The outfit its model is (-1 : whatever the game gave it).
	uint32_t      limboSince;
	int           limboTicks;          // Negative if not in limbo.
	uint32_t      lastSpawnTry;
	int           forcedState;

	// Locomotion (see UpdateLocomotion).
	int idleState, runLayer, idleLayer, sprintLayer, airLayer;
	float velX, velY;                  // Smoothed, units per tick.
	float moveSpeed;
	float heading;                     // Direction of travel, atan2( y, x ).
	bool moving, sprinting;
	float peakSpeed;                   // Fastest speed of the current move (logged).
	bool haveLastTarget;
	Vector3f lastTarget;
	bool airborne;
	int airTicks;
	float airStartZ, prevZ;
	int mirrorSlot;
	uint32_t mirrorClip;
	int lastLoggedState, lastRequested;
	DWORD lastTick;                    // When the puppet last ticked (GetTickCount).
	int ticks;
	EIEvilHarryP* watched;             // The puppet lastTick is about (see ReplaceStalledPuppets).

	// Where their name tag goes (screen pixels), noted when the puppet renders.
	float tagX, tagY, tagDepth;
	uint32_t tagFrame;                 // g_presentCount then.
};
extern Peer g_peers[MAX_PEERS];

static inline bool PeerInLimbo( const Peer* p ) { return p->limboTicks >= 0; }


// Player info.

#include "Outfits.h"

struct PlayerInfo
{
	bool present;
	char name[MAX_NAME + 1];
	uint32_t level;
	uint32_t lastInfo;
	uint8_t outfit;
	uint8_t colors[RETRO_PARTS][3];
	char version[16];    // Their mod's (MP_VERSION); empty : a build from before versions.
	uint8_t role;        // Hide and seek : ROLE_SEEKER, ROLE_HIDER.
	uint32_t hideMs;     // Their total hide time.
	bool paused;         // Their game is paused (the Esc menu, F1) : their puppet stays, still.
};
extern PlayerInfo g_players[MAX_PEERS];

static inline const char* PlayerName( int id ) { return (id >= 0 && id < MAX_PEERS) ? g_players[id].name : "<INVALID_PLAYER_ID>"; }


// Emotes.

#include "Emotes.h"

extern EmoteState g_emoteState;
extern EmoteWheel g_emoteWheels[MAX_WHEELS];


// API.

void mp_log( const char* fmt, ... );

bool GetDataDir( char* path );

void Toast( const char* fmt, ... );

void SpawnPuppet( Peer* peer );

bool GameHasFocus();


// main.cpp's.

static const uint32_t VTABLE_SLOT_TICK   = 0x2C;     // EInstance::Tick().
static const uint32_t VTABLE_SLOT_RENDER = 0x34;     // EInstance::Render( context, arg ).
extern char g_logDir[MAX_PATH];

// Game modes (F1 -> Gamemodes; Multiplayer.ini, [Gamemodes] Mode). Off : just
// see each other.
enum { MODE_OFF, MODE_HIDE_AND_SEEK, NUM_MODES };

static const char* const MODE_NAMES[NUM_MODES] = { "Off", "Hide and seek" };
extern int g_gameMode;

void SaveSettings();

typedef VOID (WINAPI *ExitProcessFunc)( UINT );


#ifdef PTLE_MP_EMBEDTESTS   // For the tests (tests.cpp).

extern ExitProcessFunc g_origExitProcess;

void LeaveRoom();

#endif
