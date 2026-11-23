#include "Network.h"

#include "Multiplayer.h"
#include "Puppets.h"
#include "Outfits.h"
#include "AnimViewer.h"

#include "version.h"
#include "net/protocol.h"
#include "net/remote.h"
#include "net/snapshot.h"
#include "net/transport.h"
#include "util/MatrixUtils.h"
#include "util/MemoryWriter.h"
#include "util/MemoryReader.h"
#include "gamemodes/HideAndSeek/HideAndSeek.h"
#include "tests.h"

#include "gizmod/Gizmod.h"

#include "ptle/EIHarry.h"
#include "ptle/EIEvilHarryP.h"
#include "ptle/levels/level_info.h"

#include <Windows.h>

#include <time.h>
#include <string.h>


char g_myVersion[MAX_VERSION + 1] = MP_VERSION;          // Sent to the others (tests : PTLE_MP_FAKEVERSION, another one).
char g_myName[MAX_NAME + 1] = "Harry";
int g_myNameLen = 5;
char g_roomName[64] = "";
uint32_t g_lastInfoSent = 0;
uint32_t g_lastNetUpdate = 0;

static_assert( sizeof(MP_VERSION) - 1 <= MAX_VERSION, "version too long" );



// Leaving a level (see NoteLevelLeaving) : the old one still runs and is shown.
bool g_levelLeaving = false;
volatile DWORD g_harryTickAt = 0;          // GetTickCount at Harry's last tick.
static const DWORD PAUSED_AFTER_MS = 300;  // Harry not ticking this long (not for a load) : the game is paused.
uint32_t g_tickLevel = 0;          // The level's CRC at Harry's last tick (the one shown).

// The level Harry is in, as far as the others are concerned : the game's CRC is
// the new level's as soon as a load is announced, but the old one still runs (and
// Harry in it) a couple of seconds. Saying we were in the new one then showed us
// to its players at our place in the old one (2026-11-16, Twin Outposts -> Turtle
// Monument : Harry running in the middle of the level, nowhere near the loadzone);
// saying we were still in the old one showed the game pulling Harry about in the
// loadzone (rubber-banding). So none : we're hidden until we're in the new one.
uint32_t ShownLevel()
{
	return g_levelLeaving ? 0 : Gizmod::getCurrentLevelCRC();
}

PlayerSnapshot g_local;                        // Static storage keeps Matrix4f 16-byte aligned.
bool g_haveLocal = false;

// One per player id the relay hands out (its rooms hold 8). Each other player in
// the room gets their own puppet; with echo (or the loopback), our own id gets one
// too and replays us.

Peer g_peers[MAX_PEERS];
PlayerInfo g_players[MAX_PEERS];

// The peer being ticked or drawn. Between those : the main peer, the one F5, F6, F8
// and the autotest work on (the first player heard from).
static int g_mainId = -1;

Peer* MainPeer()
{
	return &g_peers[g_mainId >= 0 ? g_mainId : 0];
}



// Who is in the room (from their PACKET_INFO), kept across levels. Presence goes by
// those packets (sent even while a player's game is loading or paused), not by
// snapshots.
static const uint32_t INFO_INTERVAL_MS = 1000;
static const uint32_t INFO_TIMEOUT_MS = 15000;
static const uint8_t INFO_PAUSED = 0x01;   // PACKET_INFO's flags.
static const uint32_t PAUSE_GRACE_MS = 3000;   // Unpaused, but no snapshot yet : still kept this long.



int CleanName( const char* in, int length, char* out )
{
	int n = 0;
	for ( int i = 0; i < length && n < MAX_NAME; i++ ) {
		char c = in[i];
		if ( c >= 32 && c <= 126 ) out[n++] = c;
	}
	while ( n > 0 && out[n - 1] == ' ' ) n--;
	out[n] = 0;
	return n;
}

const char* LevelName( uint32_t crc )
{
	if ( !crc ) return "loading";
	uint32_t id = level_get_by_crc( crc );
	return id == (uint32_t) -1 ? "somewhere" : level_get_name( id );
}



Peer* FindPeerFromBeast( const EIEvilHarryP* beast )
{
	for ( int i = 0; i < MAX_PEERS; i++ )
		if ( g_peers[i].beast == beast )
			return &g_peers[i];
	return 0;
}

// Hands a puppet (and its locomotion state, everything from "beast" on) to another peer.
static void MovePuppet( Peer* from, Peer* to )
{
	size_t start = (uint8_t*) &from->beast - (uint8_t*) from;
	memcpy( (uint8_t*) to + start, (uint8_t*) from + start, sizeof(Peer) - start );
	from->beast = 0;
}

void InitPeers()
{
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		g_peers[i].limboTicks = -1;
	}
}

static void ResetPeer( Peer* p, int id )   // Forgets them (their puppet is left alone).
{
	p->id = id;
	p->heard = false;
	p->lastHeard = 0;
	p->lastLevel = 0;
	p->keptPaused = false;
	p->pausedSeen = 0;
	p->remote.Reset();
	p->target = 0;
	p->lastSpawnTry = 0;
	p->forcedState = -1;
	p->lastLoggedState = p->lastRequested = -2;
	p->tagFrame = 0;
}



int g_delayIndex = 1;          // 250 ms (Multiplayer.ini : Delay).

// The network side.
Transport* g_transport = 0;
bool g_networked = false;                      // Through a server (not the loopback).
static uint32_t g_sendSequence = 0;
// Snapshots go out at most this often (30 per second : the game runs at 60, the
// receiver blends between them). A little under, so frame timing jitter at 60 fps
// doesn't skip one.
static const uint32_t SEND_INTERVAL_MS = 33 - 8;
static uint32_t g_lastSentTime = 0;
static const uint32_t REMOTE_SILENT_MS = 5000; // Then : forget them.
NetStats g_netStats;

// The local snapshot written last : Harry's item draws (after his tick) add to it.
PlayerSnapshot* LatestSnapshot()
{
	return g_haveLocal ? &g_local : 0;
}

void ResetHistory()
{
	g_haveLocal = false;
	for ( int i = 0; i < MAX_PEERS; i++ ) ResetPeer( &g_peers[i], i );
	g_mainId = -1;
}

// Sends the local snapshot once it's complete (the tick after it was recorded :
// item draws come after the tick).
static void SendLocalSnapshot()
{
	if ( !g_haveLocal || !g_transport ) return;
	int me = g_transport->PlayerId();
	if ( me < 0 ) return;   // Not in a room yet.
	if ( g_netStats.sent && g_local.time - g_lastSentTime < SEND_INTERVAL_MS ) return;
	g_lastSentTime = g_local.time;

	uint8_t packet[MAX_PACKET];
	int length = EncodeSnapshot( g_local, (uint8_t) me, ++g_sendSequence, packet, sizeof(packet) );
	if ( !length ) {
		if ( !g_netStats.tooBig++ ) mp_log( "[NET] Snapshot doesn't fit in a packet (%d bones, %d item bones).\n", g_local.bones, g_local.itemBones );
		return;
	}
	g_transport->Send( packet, length );
	g_netStats.sent++;
	g_netStats.sentBytes += length;
	if ( (uint32_t) length > g_netStats.maxBytes ) g_netStats.maxBytes = length;

	TestCheckPacking( g_local );
}

void LogNetStats()
{
	const NetStats& n = g_netStats;
	int shown = 0;
	for ( int i = 0; i < MAX_PEERS; i++ ) if ( g_peers[i].heard ) shown++;
	mp_log( "[NET] Sent %u snapshots (avg %u bytes, max %u, %u too big). Bad %u. Ping %d ms. Max packing error %.4f. Delay %d ms. %d player(s) :\n",
		n.sent, n.sent ? n.sentBytes / n.sent : 0, n.maxBytes, n.tooBig, n.badPackets,
		g_transport ? g_transport->RoundTripMs() : -1, n.maxPackingError, DELAY_CHOICES[g_delayIndex], shown );
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		const Peer& p = g_peers[i];
		if ( !p.heard ) continue;
		const RemotePlayer& r = p.remote;
		mp_log( "[NET]   Player %d : received %u, lost %u, late %u, reordered %u. Playback : blended %u, held %u. Puppet %s.\n",
			p.id, r.received, r.Lost(), r.late, r.reordered, r.blended, r.held,
			p.beast ? "shown" : p.target ? "not spawned" : "not here" );
	}
}

// Takes in what arrived and picks what the puppet shows this tick.
void NetUpdate()
{
	static PlayerSnapshot incoming;   // Static : big, and 16-byte aligned.
	uint8_t packet[MAX_PACKET];
	int length;
	uint32_t now = NetNowMs();
	if ( !g_transport ) return;
	g_lastNetUpdate = now;
	g_transport->Update();
	int me = g_transport->PlayerId();
	bool echo = g_transport->Echoes();
	while ( (length = g_transport->Receive(packet, sizeof(packet))) > 0 ) {
		MemoryReader r( packet, length );   // Only what was received : reads past it fail.
		PacketHeader h;
		if ( !DecodeHeader(packet, length, &h) ) { g_netStats.badPackets++; continue; }
		r.seek( PACKET_HEADER_SIZE );
		if ( h.player >= MAX_PEERS ) { g_netStats.badPackets++; continue; }
		Peer* peer = &g_peers[h.player];
		switch ( h.type )
		{
		case PACKET_LEAVE:
			if ( peer->heard || g_players[h.player].present ) {
				mp_log( "[NET] Player %d left.\n", h.player );
				if ( g_players[h.player].present ) Toast( "%s left", PlayerName(h.player) );
				ResetPeer( peer, h.player );   // Their puppet stays hidden (no target).
			}
			g_players[h.player].present = false;
			g_players[h.player].name[0] = 0;
			break;

		case PACKET_CAUGHT:
			if ( h.player == me && !echo ) break;
			if ( length < PACKET_HEADER_SIZE + 1 || packet[PACKET_HEADER_SIZE] >= MAX_PEERS ) { g_netStats.badPackets++; break; }

			HsReceiveCaught( h.player, packet[PACKET_HEADER_SIZE], h.sequence, me );
			break;

		case PACKET_INFO: {
			if ( h.player == me && !echo ) break;
			if ( length < PACKET_HEADER_SIZE + 5 ) { g_netStats.badPackets++; break; }

			PlayerInfo& info = g_players[h.player];

			// Level.
			r.read<uint32_t>( info.level );

			// Player name.
			int nameLength; r.read<uint8_t>(nameLength);
			if ( PACKET_HEADER_SIZE + 5 + nameLength > length ) { g_netStats.badPackets++; break; }
			char name[MAX_NAME + 1];
			CleanName( (const char*) r.getDataPtr(), nameLength, name );
			r.seek( nameLength );

			// Outfit & retro colors.
			r.read<uint8_t>( info.outfit );
			if ( info.outfit >= NUM_OUTFITS ) info.outfit = 0;
			r.read( info.colors, sizeof(info.colors) );

			// Version string.
			char version[MAX_VERSION + 1] = "";
			int versionLength = 0; r.read<uint8_t>( versionLength );
			if ( versionLength > MAX_VERSION || (int) r.getPtr() + versionLength > length ) { g_netStats.badPackets++; break; }
			CleanName( (const char*) r.getDataPtr(), versionLength, version );
			r.seek( versionLength );

			bool versionNews = !info.present || strcmp( info.version, version ) != 0;
			strcpy_s( info.version, version );

			// Gamemode role.
			r.read<uint8_t>( info.role );
			if ( info.role >= ROLE_NONE ) info.role = ROLE_NONE;

			// Hide & seek hide duration.
			info.hideMs = 0;
			if ( info.role == ROLE_HIDER || info.role == ROLE_SEEKER ) {
				r.read<uint32_t>( info.hideMs );
			}

			// Flags (paused, ...).
			uint8_t flags = 0; r.read<uint8_t>( flags );
			info.paused = (flags & INFO_PAUSED);

			info.lastInfo = now;

			// Update presence.
			if ( !info.present || strcmp(info.name, name) != 0 ) {
				mp_log( "[NET] Player %d is \"%s\", in %s.\n", h.player, name, LevelName(info.level) );
				if ( !info.present && h.player != me ) Toast( "%s joined", name );
				strcpy_s( info.name, name );
				info.present = true;
			}
			if ( versionNews ) {
				mp_log( "[NET] Player %d has version %s.\n", h.player, version[0] ? version : "(older, none sent)" );
				if ( h.player != me && strcmp(version, MP_VERSION) != 0 )
					Toast( "%s has %s%s (you : %s)", name, version[0] ? "version " : "an older version", version, MP_VERSION );
			}
			}
			break;

		case PACKET_SNAPSHOT:
			if ( h.player == me && !echo ) break;   // Ourselves.
			if ( !DecodeSnapshot(packet, length, &incoming) ) { g_netStats.badPackets++; break; }
			if ( !peer->heard ) {
				peer->heard = true;
				mp_log( "[NET] Showing player %d.\n", h.player );
				if ( g_mainId < 0 ) {
					// A puppet spawned before anyone was heard from (F5, the autotest) is
					// peer 0's : it now shows this player.
					Peer* first = &g_peers[0];
					if ( h.player != 0 && first->beast && !first->heard && !peer->beast && !PeerInLimbo(peer) ) MovePuppet( first, peer );
					g_mainId = h.player;
				}
			}
			peer->remote.Add( incoming, h.sequence );
			peer->lastHeard = now;
			peer->lastLevel = incoming.level;
			if ( peer->keptPaused ) {
				peer->keptPaused = false;
				mp_log( "[NET] Player %d goes on.\n", h.player );
			}
			break;
		}
	}
	uint32_t level = ShownLevel();
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		PlayerInfo& info = g_players[i];
		if ( info.present && now - info.lastInfo > INFO_TIMEOUT_MS ) {
			mp_log( "[NET] No news from player %d (\"%s\") for %u s : they're gone.\n", i, info.name, INFO_TIMEOUT_MS / 1000 );
			Toast( "%s left (connection lost)", info.name );
			info.present = false;
			info.name[0] = 0;
		}
	}
	if ( me >= 0 && g_caughtOut.left > 0 && now - g_caughtOut.lastSent >= CAUGHT_RESEND_MS ) {   // We were caught.
		g_caughtOut.left--;
		g_caughtOut.lastSent = now;
		uint8_t caught[PACKET_HEADER_SIZE + 1];
		EncodeHeader( PACKET_CAUGHT, (uint8_t) me, g_caughtOut.number, caught );
		caught[PACKET_HEADER_SIZE] = (uint8_t) g_caughtOut.seeker;
		g_transport->Send( caught, sizeof(caught) );
	}
	if ( me >= 0 && now - g_lastInfoSent >= INFO_INTERVAL_MS ) {   // Who we are, where.
		g_lastInfoSent = now;
		uint8_t info[PACKET_HEADER_SIZE + 5 + MAX_NAME + 1 + sizeof(g_myColors) + 1 + MAX_VERSION + 6];
		MemoryWriter w(info, sizeof(info));

		const uint32_t where = g_haveLocal ? level : 0;

		EncodeHeader( PACKET_INFO, (uint8_t) me, 0, info ); w.seek(PACKET_HEADER_SIZE);
		w.write<uint32_t>( where );
		w.write<uint8_t>( g_myNameLen );
		w.write( g_myName, g_myNameLen );
		w.write<uint8_t>( g_myWornOutfit );
		w.write( g_myColors, sizeof(g_myColors) );

		const int v = (int) strlen( g_myVersion );
		w.write<uint8_t>( v );
		w.write( g_myVersion, v );

		LoadHideTime();

		w.write<uint8_t>( PlayingHs() ? g_hsMyRole : ROLE_NONE );   // ROLE_NONE : no hide time follows.
		if ( PlayingHs() ) {
			w.write<uint32_t>( g_hsMyHideMs );
		}

		const bool paused = g_haveLocal && where && !g_levelLeaving && GetTickCount() - g_harryTickAt > PAUSED_AFTER_MS;
		w.write<uint8_t>( paused ? INFO_PAUSED : 0 );

		g_transport->Send( info, w.getPtr() );
	}
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer* peer = &g_peers[i];

		// Silent but paused, here : kept, still (their last snapshot), not forgotten.
		const PlayerInfo& pi = g_players[i];
		const bool silent = peer->heard && now - peer->lastHeard > REMOTE_SILENT_MS;
		bool paused = pi.present && pi.paused && pi.level && pi.level == peer->lastLevel;
		if ( paused ) peer->pausedSeen = now;
		else if ( peer->keptPaused && now - peer->pausedSeen < PAUSE_GRACE_MS ) paused = true;

		if ( silent && paused && !peer->keptPaused ) {
			peer->keptPaused = true;
			mp_log( "[NET] Player %d is paused : kept where they are.\n", i );
		}
		if ( silent && !paused ) {
			mp_log( "[NET] Nothing from player %d for %u s : forgetting them.\n", i, REMOTE_SILENT_MS / 1000 );
			ResetPeer( peer, i );
		}
		peer->target = peer->remote.Update( now, DELAY_CHOICES[g_delayIndex] );
		if ( peer->target && peer->target->level != level ) peer->target = 0;   // Another level : not shown.
	}

	static uint32_t lastStats = now;
	if ( now - lastStats >= 10000 ) {
		lastStats = now;
		LogNetStats();
	}
}



void PushLocalSnapshot( EIHarry* harry )
{
	KeepUndrawnItem( harry );
	SendLocalSnapshot();   // The previous one, complete now.

	int worn = 0;
	for ( int i = 1; i < NUM_OUTFITS; i++ ) {
		if ( harry->m_modelCRC == OUTFITS[i].model ) {
			worn = i;
			break;
		}
	}
	if ( worn != g_myWornOutfit || !g_haveLocal ) mp_log( "[MP] Harry wears the %s outfit (model 0x%.8X; chosen : %s).\n", OUTFITS[worn].name, harry->m_modelCRC, g_myOutfit < 0 ? "Default" : OUTFITS[g_myOutfit].name );
	g_myWornOutfit = worn;
	ApplyMyOutfit();

	PlayerSnapshot& s = g_local;
	s.time          = NetNowMs();
	s.level         = ShownLevel();
	s.transform     = harry->m_transformMatrix;
	s.position      = harry->m_position;
	s.rotationEuler = harry->m_rotationEuler;
	s.onGround      = harry->m_onGround;
	static float groundZ = 0.0F;
	if ( s.onGround ) groundZ = s.position.z;
	s.lift = s.onGround ? 0.0F : max( 0.0F, s.position.z - groundZ );

	int layer = -1;
	s.clipCRC = GetPlayingClip( harry, &layer, &s.clipWeight );
	static uint32_t lastPlayed = 0;   // Only the hold-pose layer is up : keep the clip before it.
	if ( s.clipCRC ) lastPlayed = s.clipCRC; else s.clipCRC = lastPlayed;
	static uint32_t lastClip = 0;
	if ( s.clipCRC != lastClip ) {
		mp_log( "[MP] Harry plays clip 0x%.8X (layer %d, weight %.2f, %s).\n",
			s.clipCRC, layer, s.clipWeight, harry->m_onGround ? "ground" : "air" );
		lastClip = s.clipCRC;
	}

	s.heldItem = GetHeldItem( harry );
	s.drawnSlot = -1;   // Set if Harry draws an item this frame (NoteHarryItemDraw).
	s.itemBones = 0;

	// Harry is posed by now (his pose stamp is this frame's).
	s.bones = 0;
	Matrix4f inverse;
	if ( MatInverseAffine(harry->m_transformMatrix, &inverse) ) {
		int nbones = 0;
		Matrix4f* pose = GetPose( &harry->m_animController, &nbones );
		if ( pose && nbones > 0 && nbones <= MAX_CHAR_BONES ) {
			MatsMul( pose, nbones, inverse, s.pose );
			s.bones = nbones;
		}
		MatMul( harry->m_matRHandHold, inverse, &s.handR );
		MatMul( harry->m_matLHand, inverse, &s.handL );
	}
	static int lastItem = -1;
	if ( s.heldItem != lastItem ) {
		mp_log( "[MP] Harry holds item %d.\n", s.heldItem );
		lastItem = s.heldItem;
	}

	g_haveLocal = true;
}

const PlayerSnapshot* GetPuppetTarget( const Peer* peer )
{
	if ( !peer->target && IsViewerPeer(peer) && g_haveLocal ) return &g_local;
	return peer->target;
}
