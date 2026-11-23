#pragma once

#include "Multiplayer.h"


// Networking : the other players (peers), their info, and the snapshots sent and received.


static const int MAX_VERSION = 15;
extern char g_myVersion[MAX_VERSION + 1];   // Sent to the others (tests : PTLE_MP_FAKEVERSION, another one).
extern char g_myName[MAX_NAME + 1];
extern int g_myNameLen;
extern char g_roomName[64];
extern uint32_t g_lastInfoSent;
extern uint32_t g_lastNetUpdate;
extern bool g_levelLeaving;
extern volatile DWORD g_harryTickAt;   // GetTickCount at Harry's last tick.
extern uint32_t g_tickLevel;   // The level's CRC at Harry's last tick (the one shown).

uint32_t ShownLevel();

extern PlayerSnapshot g_local;   // Static storage keeps Matrix4f 16-byte aligned.
extern bool g_haveLocal;

Peer* MainPeer();
int CleanName( const char* in, int length, char* out );
const char* LevelName( uint32_t crc );
Peer* FindPeerFromBeast( const EIEvilHarryP* beast );
void InitPeers();

static const int DELAY_CHOICES[] = { 133, 250, 500, 1000, 1500, 3000 };   // Playback delay (ms).
extern int g_delayIndex;   // 250 ms (Multiplayer.ini : Delay).
extern bool g_networked;   // Through a server (not the loopback).

struct NetStats
{
	uint32_t sent, sentBytes, maxBytes, tooBig, badPackets, otherPlayers;
	float maxPackingError;
};

PlayerSnapshot* LatestSnapshot();
void ResetHistory();
void NetUpdate();
void PushLocalSnapshot( EIHarry* harry );
const PlayerSnapshot* GetPuppetTarget( const Peer* peer );


#ifdef PTLE_MP_EMBEDTESTS   // For the tests (tests.cpp).

extern NetStats g_netStats;

void LogNetStats();

#endif
