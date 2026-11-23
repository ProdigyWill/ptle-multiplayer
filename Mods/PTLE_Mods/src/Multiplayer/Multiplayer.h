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
class Matrix4f;
class Menu;

struct AnimLayer;
struct IDirect3DDevice9;
struct ItemStruct;

struct PlayerSnapshot;
class Transport;


enum { RETRO_HAIR, RETRO_SKIN, RETRO_SHIRT, RETRO_TROUSERS, RETRO_PARTS };


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


// main.cpp's, shared with the tests (tests.cpp).

enum { MODE_OFF, MODE_HIDE_AND_SEEK, NUM_MODES };   // The game modes.
extern int g_gameMode;
enum { MK_UP, MK_DOWN, MK_LEFT, MK_RIGHT, MK_ACCEPT, MK_BACK, MK_NEXT, MK_PREV, MK_DELETE, MK_PGUP, MK_PGDN, MK_SLEFT, MK_SRIGHT, MK_COUNT };   // The menu's keys.

struct Outfit { const char* name; uint32_t model; uint32_t cheatFlag; };

struct TagDiag
{
	int frames, lookAts, maxLookAts;
	int drawsOnScreen, drawsElsewhere, behind, noted;
	UINT elseW, elseH, screenW, screenH;
	int lookAtsThisFrame;
	Vector3f eyes[4];                 // This frame's look-ats (eye positions).
	int sampleLogged;
};

struct AnimNote
{
	int num;
	uint32_t crc;
	char source[24];
	bool star;
	char note[200];
};

struct WheelUi
{
	bool open;
	bool byPad;          // Else the keyboard's.
	bool shown;          // Pointed at, or flipped : showing whatever the time.
	bool done;           // Played (1-8, a click) or given up : nothing more until it's let go.
	uint32_t since;
	int hover;           // Slot pointed at, -1 : none.
	uint32_t hoverAt;
	LONG centre[8];      // The controller's axes when it opened (lX lY lZ lRx lRy lRz, the sliders).
	POINT mouse;         // Where the cursor's way is measured from.
	bool r1Was, clickWas;
};

typedef VOID (WINAPI *ExitProcessFunc)( UINT );

struct NetStats
{
	uint32_t sent, sentBytes, maxBytes, tooBig, badPackets, otherPlayers;
	float maxPackingError;
};

#ifdef PTLE_MP_EMBEDTESTS

Peer* MainPeer();
void LeaveRoom();
extern NetStats g_netStats;
void LogNetStats();
bool HiddenFromMe( int id );

// Settings.
extern char g_logDir[MAX_PATH];
extern int g_delayIndex;
extern bool g_showTags;
extern bool g_showList;
extern const int TAG_FADE_STEP;
extern int g_tagFadeFrom, g_tagFadeTo;

// Outfits.
extern const Outfit OUTFITS[];
extern const int NUM_OUTFITS;
extern const int RETRO_OUTFIT;
extern const int SNOWY_OUTFIT;
extern const int OUTFIT_DEFAULT;
extern int g_myOutfit;
extern int g_myWornOutfit;
extern uint8_t g_myColors[RETRO_PARTS][3];
extern int g_retroSwaps;
extern int g_retroMatched, g_retroUnmatched;
void ApplyMyOutfit();

// Harry, his animations and items.
extern volatile LONG g_harryTicks;
extern const float MP_PI;
extern float g_forwardLocalAngle;
extern ItemStruct* ITEM_TABLE;
Matrix4f* GetPose( EAnimController* animCtrl, int* bones );
AnimLayer* GetAnimLayer( EICharacter* c, int key );
uint32_t GetLayerClipCRC( AnimLayer* layer );
uint32_t GetPlayingClip( EICharacter* c, int* outLayer, float* outWeight );
EItemBase* GetItemObject( int index );
int GetHeldItem( EIHarry* harry );
int FindStateIndex( EIBeast* beast, const char* shortName );

// The puppets.
extern int g_puppetsSpawned;
extern bool g_puppetSolid;
extern bool g_allowPuppetFreeze;
extern volatile LONG g_puppetFreezesStopped;
extern bool g_allowPuppetFade;
extern volatile LONG g_fadesStopped;
extern volatile LONG g_puppetFadeCalls;
extern float g_lastPuppetFade;
extern bool g_autoLocomotion;
extern const float SPRINT_START_SPEED;
extern const int MIRROR_LAYERS[];
extern const uint32_t INSTANCE_VISIBLE;
extern int g_puppetPoseCopies;
extern int g_puppetItemDraws, g_aimItemDraws, g_copyDrawsAlone;
const PlayerSnapshot* GetPuppetTarget( const Peer* peer );
void SetIdleAnimation( EIEvilHarryP* p, const char* stateName, Peer* peer );
void SetAirAnimation( EIEvilHarryP* p, const char* stateName, Peer* peer );

// Rendering, the camera and the name tags.
extern const uint32_t RENDERER_DEVICE;
extern uint32_t g_presentCount;
extern const uint32_t CAMERA_LOOKAT_CALLS[];
extern const int NUM_CAMERA_LOOKAT_CALLS;
extern int g_cameraSiteCount[];
extern uint32_t g_cameraSite;
extern Matrix4f g_cameraWorld;
extern bool g_haveCamera;
extern Matrix4f g_rendCam;
extern uint32_t g_rendFrame, g_rendSets, g_rendSetsMax;
extern void* g_rendSelf;
extern int g_rendTwiceLogged;
extern TagDiag g_tagDiag;
extern bool g_tagDiagOn;
extern int g_tagPasses[];
bool PlaceNameTag( const Peer& peer, IDirect3DDevice9* dev, float* x, float* y, float* depth );
bool HookPresent();

// Screen grabs.
extern volatile LONG g_grabRequested;
extern char g_grabName[64];
extern HANDLE g_grabDone;
extern volatile LONG g_cropPending;
extern char g_cropName[64];
extern RECT g_crop;
void GrabScreen( const char* name );

// The animation viewer and survey.
extern bool g_animViewer;
extern Peer* g_viewerPeer;
extern uint32_t g_animPreviewCRC;
extern AnimNote g_animNotes[];
extern int g_animNoteCount;
extern int g_animIndex;
extern volatile LONG g_reqViewer, g_reqStar, g_reqNext, g_reqNext10, g_reqReplay, g_reqStarredOnly;

// Emotes.
extern const int EMOTE_LAYERS[];
extern bool g_emotePreview;
extern Peer* g_parkedPeer;
extern int g_wheelCount;
extern int g_wheelIndex;
extern int g_ringSlot;
extern bool g_chooser;
extern char g_emoteSearch[32];
extern int g_emoteCategory;
extern int g_listCursor, g_listScroll;
extern int g_previewWant;
extern WheelUi g_wheelUi;
int FindEmoteByName( const char* name );
void MakeDefaultWheel( int w );

// The menu and input.
extern Menu g_menu;
extern volatile LONG g_reqMenu;
extern volatile LONG g_menuKeys[];
extern HWND* const HWND_GAME_WINDOW;
extern const int PAD_R1;
extern const uint32_t PAD_BINDINGS;
void SetMenuOpen( bool open );
void AddTyped( char c );
uint8_t KeyToDIK( WORD vk );
int VkToDik( int vk );
const char* KeyName( int dik );

extern ExitProcessFunc g_origExitProcess;

#endif
