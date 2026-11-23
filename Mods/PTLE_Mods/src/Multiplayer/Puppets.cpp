#include "Puppets.h"

#include "Multiplayer.h"
#include "Network.h"
#include "Render.h"
#include "Outfits.h"
#include "AnimViewer.h"

#include "net/snapshot.h"
#include "util/MatrixUtils.h"
#include "tests.h"

#include "gizmod/Gizmod.h"
#include "gizmod/item/InventoryItem.h"
#include "injector/injector.hpp"

#include "ptle/EIHarry.h"
#include "ptle/EIEvilHarryP.h"
#include "ptle/EBeastStateMachine.h"
#include "ptle/ERAnim.h"
#include "ptle/ERLevel.h"
#include "ptle/ERCharacter.h"
#include "ptle/EIStaticModel.h"
#include "ptle/EItemRaft.h"
#include "ptle/math/Vector3f.h"
#include "ptle/EItemBase.h"
#include "ptle/EIPitMonster.h"
#include "utils/func.h"

#include <Windows.h>

#include <string.h>
#include <math.h>


GET_METHOD( 0x626670, void, AddToWorld, ERLevel*, EInstance* );
GET_METHOD( 0x6268D0, void, ERLevel_SetupEntity, ERLevel*, EInstance*, EInstance* );
GET_METHOD( 0x412850, void, EIBeast_LoadBeastAsset, EIBeast* );
GET_FUNC(   0x6F8EC0, uint32_t, HashPath, const char* );
GET_METHOD( 0x414F80, void, EIBeast_Tick, EIBeast* );
GET_METHOD( 0x41C6A0, void, EIBeast_Render, EIBeast*, void*, void* );


bool g_allowPuppetFreeze = false;
volatile LONG g_puppetFreezesStopped = 0;            // Puppets kept out of the ice cube (ThrownFromWater_Hook).

// --------------------------------------------------------------------------------
// Animation clips. EAnimController (+0xB4 in every character) keeps its layers in a
// map at +0x2C keyed by layer index (lookup 0x6F3A50). A layer (0xA0 bytes) holds
// its clip instance at +0x78 (clip CRC at +0x0C) and its weight at +0x4C.
// 0x61CAD0( layer, clipCRC ) loads a clip into a layer (creating the layer).
// --------------------------------------------------------------------------------
GET_METHOD( 0x6F3A50, int,  AnimMap_Find,     void*, int, AnimLayer** );
GET_METHOD( 0x61CAD0, void, EAnimController_SetClip, EAnimController*, int, uint32_t );


static const uint32_t* RENDER_FRAME   = (const uint32_t*) 0x91779C;   // Global frame counter.

Matrix4f* GetPose( EAnimController* animCtrl, int* bones )
{
	ERCharacter* skeleton = animCtrl->m_characterResource;
	*bones = skeleton ? skeleton->m_numBones : 0;
	return animCtrl->m_pose;
}

AnimLayer* GetAnimLayer( EICharacter* c, int key )
{
	AnimLayer* layer = 0;
	TreeMap* map = &c->m_animController.m_animMap;
	if ( !AnimMap_Find(map, key, &layer) ) return 0;
	return layer;
}

// Layer 217 holds a frozen pose (its clock never moves) that Harry blends from
// when he switches item animations; mirroring it froze the puppet mid pull-out.
static const int HOLD_POSE_LAYER = 217;

// The clip a character is mostly showing : its highest-weight layer.
uint32_t GetPlayingClip( EICharacter* c, int* outLayer, float* outWeight )
{
	uint32_t best = 0;
	float bestWeight = 0.01F;
	int bestLayer = -1;
	for ( int i = 0; i < MAX_LAYER_SCAN; i++ ) {
		if ( i == HOLD_POSE_LAYER ) continue;
		AnimLayer* layer = GetAnimLayer( c, i );
		if ( !layer ) continue;
		float w = layer->m_weight;
		uint32_t crc = GetLayerClipCRC( layer );
		if ( crc && w > bestWeight ) { best = crc; bestWeight = w; bestLayer = i; }
	}
	if ( outLayer ) *outLayer = bestLayer;
	if ( outWeight ) *outWeight = bestWeight;
	return best;
}


ItemStruct* ITEM_TABLE = (ItemStruct*) 0x8EEB90;
static const int NUM_ITEMS = 9;

EItemBase* GetItemObject( int index ) { return ITEM_TABLE[index].m_object; }

int GetHeldItem( EIHarry* harry )
{
	EInventory* inv = harry->m_inventory;
	if ( !inv ) return -1;

	EItemBase* held = inv->m_itemInHandNext;
	if ( !held ) return -1;

	for ( int i = 0; i < NUM_ITEMS; i++ )
		if ( GetItemObject(i) == held ) return i;
	return -1;
}

// --------------------------------------------------------------------------------
// Puppet.
// --------------------------------------------------------------------------------

// Index of a state by its short name ("idle left" matches "evil harry.idle left").
int FindStateIndex( EIBeast* beast, const char* shortName )
{
	EBeastStateMachine* sm = beast->GetStateMachine();
	if ( !sm || !sm->m_states ) return -1;

	for ( int i = 0; i < (int) sm->m_numStates; i++ ) {
		const char* name = sm->m_states[i].m_name;
		const char* dot = name ? strchr(name, '.') : 0;

		if ( dot && strcmp(dot + 1, shortName) == 0 ) return i;
	}
	return -1;
}
static char g_puppetName[] = "evil harry";
static char* const GAME_EMPTY_STRING = (char*) 0x9410B0;   // The game strings' "" (0x6F047A).


int g_puppetsSpawned = 0;          // For the autotest.

static void RememberSpawned( EIEvilHarryP* beast, ERLevel* world, int owner );
static void InitLocomotion( EIEvilHarryP* p, EIHarry* harry, Peer* peer );

void SpawnPuppet( Peer* peer )
{
	if ( peer->beast ) {
		mp_log( "[MP] Player %d's puppet already spawned in this level.\n", peer->id );
		return;
	}

	EIEvilHarryP* p = instantiate_object<EIEvilHarryP>();

	EIHarry* harry = Gizmod::getHarry();
	ERLevel* world = harry->m_world;

	p->EIBeast::m_position.x = harry->m_position.x;
	p->EIBeast::m_position.y = harry->m_position.y + 15.0F;
	p->EIBeast::m_position.z = harry->m_position.z;
	p->m_rotation.x = 0.0F;
	p->m_rotation.y = 0.0F;
	p->m_rotation.z = 0.0F;
	p->m_rotation.w = 1.0F;

	p->m_beastTypeCRC = HashPath( "BEvilHarryP" );
	p->m_beastTypeName = g_puppetName;
	EIBeast_LoadBeastAsset( p );
	p->DeserializePart2();

	if ( !p->m_beastType ) {
		mp_log( "[MP] WARNING : Evil Harry asset didn't load in this level, puppet may be invisible.\n" );
	}

	AddToWorld( world, p );
	ERLevel_SetupEntity( world, p, 0 );
	p->Init();
	p->m_beastTypeName = GAME_EMPTY_STRING;

	peer->beast = p;
	peer->shownOutfit = -1;
	g_puppetsSpawned++;
	peer->lastLoggedState = peer->lastRequested = -2;
	RememberSpawned( p, world, peer->id );
	int count = 0;
	for ( int i = 0; i < MAX_PEERS; i++ ) if ( g_peers[i].beast ) count++;
	mp_log( "[MP] Player %d's puppet spawned (%d puppet(s) in this level).\n", peer->id, count );

	// Its default state (21, "get ready for boss fight") draws it as a black
	// silhouette. Start calm and lit instead.
	peer->forcedState = FindStateIndex( p, "idle left" );
	mp_log( "[MP] Puppet starts in state %d (idle left).\n", peer->forcedState );
	InitLocomotion( p, harry, peer );

	if ( count > 1 ) return;
	// Raw dump of the transform-related area, before we touch anything.
	const uint32_t ranges[][2] = { { 0x120, 0x1B0 }, { 0x2F0, 0x350 } };
	for ( int r = 0; r < 2; r++ ) {
		for ( uint32_t o = ranges[r][0]; o < ranges[r][1]; o += 16 ) {
			float* f = (float*) ((uint8_t*) p + o);
			mp_log( "[MP]   +0x%03X : %10.3f %10.3f %10.3f %10.3f\n", o, f[0], f[1], f[2], f[3] );
		}
	}
}

// Called when Harry's world or the level changes.
void ForgetPuppets()
{
	int count = 0;
	const DWORD now = GetTickCount();

	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( !p.beast ) continue;

		p.limboSince = now;
		p.limboTicks = 0;
		count++;
	}

	if ( count ) mp_log( "[MP] New level, %d puppet(s) set aside (kept if they're still here).\n", count );
	ResetHistory();
}

// Every puppet spawned in Harry's current world, to catch one nobody drives any more
// (an orphan) : it would stand still, a frozen copy of its player.
struct SpawnedPuppet { EIEvilHarryP* beast; ERLevel* world; int owner; bool reported; };
static const int MAX_SPAWNED = 32;
static SpawnedPuppet g_spawned[MAX_SPAWNED];
static int g_spawnedCount = 0;

static void RememberSpawned( EIEvilHarryP* beast, ERLevel* world, int owner )
{
	int keep = 0;   // Other worlds' are kept too : the old level runs on a moment after a loadzone.
	for ( int i = 0; i < g_spawnedCount; i++ ) if ( g_spawned[i].beast != beast ) g_spawned[keep++] = g_spawned[i];
	g_spawnedCount = keep;
	if ( g_spawnedCount == MAX_SPAWNED ) { memmove( g_spawned, g_spawned + 1, (MAX_SPAWNED - 1) * sizeof(SpawnedPuppet) ); g_spawnedCount--; }
	SpawnedPuppet s = { beast, world, owner, false };
	g_spawned[g_spawnedCount++] = s;
}

// Between a load's announcement and the new level's arrival the old level still
// runs and is shown a couple of seconds, while the level's CRC is already the new
// one : a puppet of a player there was drawn at their place in the new level, in
// the old one's scenery (a Harry running into the loadzone just left; 2026-11-16,
// Turtle Monument <-> Twin Outposts). Puppets aren't shown in that time.
static void* g_levelLeavingWorld = 0;
static DWORD g_levelLeavingSince = 0;
static const DWORD LEVEL_LEAVING_MAX_MS = 8000;   // Should Harry's world not change (it does).

void NoteLevelLeaving( uint32_t newLevel )
{
	EIHarry* harry = Gizmod::getHarry();
	if ( !harry || !g_tickLevel || newLevel == g_tickLevel ) return;   // Coming back to life here, or already there.
	g_levelLeaving = true;
	g_levelLeavingWorld = harry->m_world;
	g_levelLeavingSince = GetTickCount();
	mp_log( "[MP] Leaving the level : puppets hidden until the next one is up.\n" );
}

static void* g_prevWorld = 0;   // Harry's world before this one.

void CheckLevelLeaving( ERLevel* myWorld )          // Each Harry tick.
{
	static ERLevel* world = 0;
	if ( myWorld != world ) { g_prevWorld = world; world = myWorld; }
	if ( !g_levelLeaving || myWorld != g_levelLeavingWorld ) g_tickLevel = Gizmod::getCurrentLevelCRC();
	if ( !g_levelLeaving ) return;
	bool arrived = myWorld != g_levelLeavingWorld;
	if ( !arrived && GetTickCount() - g_levelLeavingSince < LEVEL_LEAVING_MAX_MS ) return;
	g_levelLeaving = false;
	mp_log( "[MP] %s : puppets shown again.\n", arrived ? "In the new level" : "Still no new level after 8 s" );
}

static Peer* ReclaimPuppet( EIEvilHarryP* beast )
{
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( p.beast != beast || !PeerInLimbo(&p) ) continue;
		p.limboTicks = -1;
		p.lastLoggedState = p.lastRequested = -2;

		mp_log( "[MP] Player %d's puppet is still there : kept.\n", i );
		return &p;
	}
	return 0;
}

// A puppet the game stopped ticking while its player is here : a pitfall caught it
// as they were eaten on their screen (it stops a character it catches; ours stayed
// in the pit, not drawn, and its player was never shown again until a reload). Far
// away or not, a puppet ticks every frame, so two seconds without one means it's
// gone : it's set aside (kept if it ticks again) and its player gets a new one.
static const DWORD PUPPET_STALL_MS = 2000;

void ReplaceStalledPuppets()
{
	const DWORD now = GetTickCount();
	static DWORD lastCall = 0;
	bool paused = now - lastCall > 500;   // Harry didn't tick either (a pause, a load) : nothing stalled.
	lastCall = now;
	if ( g_levelLeaving ) return;

	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( p.beast != p.watched || paused ) {
			p.watched = p.beast;
			p.lastTick = now;
		}
		if ( !p.beast || now - p.lastTick <= PUPPET_STALL_MS ) continue;

		mp_log( "[MP] Player %d's puppet 0x%.8X hasn't ticked for %u ms (a pitfall caught it?) : replaced.\n", i, p.beast, now - p.lastTick );
		p.watched = 0;
		p.limboSince = now;
		p.limboTicks = 0;
	}
}

// Scan puppets that have been in limbo for too long.
// In this case, we can assume they've been destroyed by a level switch.
void DropLostPuppets()
{
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		Peer& p = g_peers[i];
		if ( !PeerInLimbo(&p) ) continue; 

		if ( ++p.limboTicks <= 30 || GetTickCount() - p.limboSince <= 1000 ) continue;
		p.beast = 0;
		p.limboTicks = -1;
		mp_log( "[MP] Player %d's puppet went with the old level.\n", i );
	}
}


GET_METHOD( 0x60FEA0, void, EICharacter_SetPos, EICharacter*, const Vector3f*, bool );
GET_METHOD( 0x60FFA0, void, EICharacter_SetRot, EICharacter*, const Vector3f* );

static bool GetPuppetTargetPosition( Vector3f* out, Peer* peer )
{
	const PlayerSnapshot* t = GetPuppetTarget( peer );
	if ( !t ) return false;
	if ( IsParkedPeer(peer) ) { *out = g_parkedPin; return true; }
	if ( IsViewerPeer(peer) ) { *out = g_viewerPin; return true; }
	out->x = t->position.x;
	out->y = t->position.y;
	out->z = t->position.z;
	TestMovePuppetTarget( out );
	return true;
}


// --------------------------------------------------------------------------------
// Locomotion : the puppet's animation and facing come from how its target moves
// (later : how the remote player moves). x/y is the ground plane, z is up.
// --------------------------------------------------------------------------------

bool  g_autoLocomotion = true;          // F6 switches to hand-forced states.
// The rest of the locomotion state is per puppet (Peer).
static const int AIRBORNE_MIN_TICKS = 6;   // Slopes and bumps blip for up to ~5 ticks.
static const float JUMP_RISE_SPEED  = 0.10F;   // Units per tick upward while off the ground.

static const float RUN_START_SPEED = 0.05F;  // Units per tick (hysteresis).
static const float RUN_STOP_SPEED  = 0.02F;
static const float SPRINT_STOP_SPEED  = 0.45F;
static const float TELEPORT_DIST   = 5.0F;   // Bigger jumps aren't movement.

// Angle of the model's forward axis in its own frame (0 = +X, pi/2 = +Y).
// +Y made it run backwards, so the model faces local -Y.
float g_forwardLocalAngle = -MP_PI / 2.0F;

static void ResetLocomotion( Peer* peer )
{
	peer->velX = peer->velY = peer->moveSpeed = 0.0F;
	peer->moving = false;
	peer->sprinting = false;
	peer->peakSpeed = 0.0F;
	peer->airborne = false;
	peer->airTicks = 0;
	peer->haveLastTarget = false;
}

static void UpdateLocomotion( EIEvilHarryP* p, Peer* peer )
{
	Vector3f pos;
	if ( !GetPuppetTargetPosition(&pos, peer) ) return;

	if ( peer->haveLastTarget ) {
		float dx = pos.x - peer->lastTarget.x;
		float dy = pos.y - peer->lastTarget.y;
		if ( dx * dx + dy * dy > TELEPORT_DIST * TELEPORT_DIST ) dx = dy = 0.0F;

		peer->velX = peer->velX * 0.75F + dx * 0.25F;
		peer->velY = peer->velY * 0.75F + dy * 0.25F;
		peer->moveSpeed = sqrtf( peer->velX * peer->velX + peer->velY * peer->velY );
		if ( peer->moveSpeed > 0.01F ) peer->heading = atan2f( peer->velY, peer->velX );
	}
	// Face where Harry faces, not where he travels : he turns on the spot, walks
	// backwards a step, and in a raft paddles one side and turns freely. His
	// transform's row 1 points opposite to where he faces (see InitLocomotion).
	if ( const PlayerSnapshot* f = GetPuppetTarget(peer) ) {
		float fx = -f->transform.data[1][0], fy = -f->transform.data[1][1];
		if ( fx * fx + fy * fy > 0.01F ) peer->heading = atan2f( fy, fx );
	}
	peer->lastTarget = pos;
	peer->haveLastTarget = true;

	bool wasMoving = peer->moving;
	if ( peer->moving && peer->moveSpeed < RUN_STOP_SPEED ) peer->moving = false;
	else if ( !peer->moving && peer->moveSpeed > RUN_START_SPEED ) peer->moving = true;
	if ( peer->moving != wasMoving ) {
		if ( peer->moving ) {
			mp_log( "[MP] Puppet %d starts moving (speed %.3f/tick, heading %.0f deg).\n", peer->id, peer->moveSpeed,peer->heading * 180.0F / MP_PI );
			peer->peakSpeed = 0.0F;
		}
		else {
			mp_log( "[MP] Puppet %d stops (top speed %.3f/tick).\n", peer->id, peer->peakSpeed );
		}
	}
	if ( peer->moving && peer->moveSpeed > peer->peakSpeed ) peer->peakSpeed = peer->moveSpeed;

	// Airborne : the replayed player has been off the ground for a few ticks
	// (ignores one-tick blips on bumps and slopes). Landing is immediate.
	const PlayerSnapshot* t = GetPuppetTarget( peer );
	bool wasAirborne = peer->airborne;
	peer->airTicks = (t && !t->onGround) ? peer->airTicks + 1 : 0;
	// A real jump rises fast from its first tick (~0.25/tick) : switch at once then,
	// instead of waiting AIRBORNE_MIN_TICKS (which lands near the top of the jump).
	float dz = pos.z - peer->prevZ;
	peer->prevZ = pos.z;
	if ( peer->airTicks == 0 ) peer->airborne = false;
	else if ( peer->airTicks >= AIRBORNE_MIN_TICKS || dz > JUMP_RISE_SPEED ) peer->airborne = true;
	if ( peer->airborne != wasAirborne ) {
		if ( peer->airborne ) {
			peer->airStartZ = pos.z;
			mp_log( "[MP] Puppet leaves the ground (z %.2f).\n", pos.z );
		}
		else {
			mp_log( "[MP] Puppet lands (z %.2f, %.2f from take-off).\n", pos.z, pos.z - peer->airStartZ );
		}
	}

	bool wasSprinting = peer->sprinting;
	if ( !peer->moving || (peer->sprinting && peer->moveSpeed < SPRINT_STOP_SPEED) ) peer->sprinting = false;
	else if ( !peer->sprinting && peer->moveSpeed > SPRINT_START_SPEED ) peer->sprinting = true;
	if ( peer->sprinting != wasSprinting && peer->moving ) {
		mp_log( "[MP] Puppet %s (speed %.3f/tick).\n", peer->sprinting ? "sprints" : "back to running", peer->moveSpeed );
	}

	// The state machine always stays in a calm idle state : forcing boss-fight
	// states like "runner" crashes (they expect setup we don't do). Running is
	// done by switching animation layers instead (ApplyLocomotionAnim).
	if ( g_autoLocomotion ) {
		peer->forcedState = peer->idleState;
	}
}

// Beast animation layers (from the beast's own PlayAnim, 0x41A820). The animation
// controller holds one layer per state-machine state (layer = EBeastState +0x38,
// set to the state's index), then the "locomotion" layers from the beast's
// quickdata at [+0x52C] : +0 move (CRC at +0x494), +1 idle (CRC at +0x498),
// +2 turn left, +3 turn right. [+0x530] is the layer currently playing.
GET_METHOD( 0x61D0D0, void, EAnimController_FadeTo,  EAnimController*, int, float );
GET_METHOD( 0x61D8A0, void, EAnimController_Restart, EAnimController*, int );

static int GetStateAnimLayer( EIBeast* p, int state )
{
	EBeastStateMachine* sm = p->GetStateMachine();
	if ( !sm || !sm->m_states || state < 0 || state >= (int) sm->m_numStates ) return -1;
	return sm->m_states[state].m_animID;
}

// Crossfades to an animation layer, like the beast's PlayAnim does.
static void PlayAnimLayer( EIEvilHarryP* p, int layer )
{
	EAnimController* ctrl = &p->m_animController;
	int current = p->m_animIndex;
	if ( layer < 0 || layer == current ) return;

	if ( current >= 0 ) EAnimController_FadeTo( ctrl, current, 0.0F );
	EAnimController_Restart( ctrl, layer );
	EAnimController_FadeTo( ctrl, layer, 1.0F );
	p->m_animIndex = layer;
}

// Mirroring : the puppet plays the replayed player's own clip (Harry's jump, climb,
// etc. : Evil Harry's state clips have no ordinary jump). Two spare layers take
// turns so the old clip can fade out while the new one fades in.
static bool g_mirrorClips = true;

static bool ApplyMirroredClip( EIEvilHarryP* p, Peer* peer )
{
	const PlayerSnapshot* t = GetPuppetTarget( peer );
	uint32_t forced = ForcedClip( peer );
	uint32_t clip = forced ? forced : (t ? t->clipCRC : 0);
	if ( !g_mirrorClips || !clip ) return false;

	int current = p->m_animIndex;
	if ( clip == peer->mirrorClip && current == MIRROR_LAYERS[peer->mirrorSlot] ) return true;

	EAnimController* ctrl = &p->m_animController;
	int next = (current == MIRROR_LAYERS[peer->mirrorSlot]) ? 1 - peer->mirrorSlot : peer->mirrorSlot;
	EAnimController_SetClip( ctrl, MIRROR_LAYERS[next], clip );
	PlayAnimLayer( p, MIRROR_LAYERS[next] );
	peer->mirrorSlot = next;
	peer->mirrorClip = clip;
	return true;
}

static void ApplyLocomotionAnim( EIEvilHarryP* p, Peer* peer )
{
	static int overridden = 0;
	if ( ApplyMirroredClip(p, peer) ) return;

	int wanted = peer->idleLayer;
	if ( peer->airborne && peer->airLayer >= 0 ) wanted = peer->airLayer;
	else if ( peer->moving ) wanted = (peer->sprinting && peer->sprintLayer >= 0) ? peer->sprintLayer : peer->runLayer;
	int current = p->m_animIndex;
	if ( wanted < 0 || wanted == current ) return;

	// The idle state may re-play its own animation; count it to spot fights.
	if ( (current == peer->idleLayer || current == peer->runLayer || current == peer->sprintLayer || current == peer->airLayer) == false ) {
		if ( ++overridden <= 5 ) mp_log( "[MP] Puppet anim layer was %d, playing %d.\n", current, wanted );
	}
	PlayAnimLayer( p, wanted );
}

static void SetRunAnimation( EIEvilHarryP* p, const char* stateName, Peer* peer )
{
	if ( !stateName ) {
		int base = p->m_locoBase;
		peer->runLayer = p->m_animMoveCRC ? base : -1;
		mp_log( "[MP] Run animation : quickdata move anim, layer %d.\n", peer->runLayer );
	}
	else {
		peer->runLayer = GetStateAnimLayer( p, FindStateIndex(p, stateName) );
		mp_log( "[MP] Run animation : \"%s\" state's anim, layer %d.\n", stateName, peer->runLayer );
	}
}

// Standing animation. 0 = the beast's quickdata idle (locomotion layer +1); the
// "idle left/right" states' animation is the wall-climbing idle (pickaxes out).
void SetIdleAnimation( EIEvilHarryP* p, const char* stateName, Peer* peer )
{
	if ( !stateName ) {
		int base = p->m_locoBase;
		peer->idleLayer = p->m_animIdleCRC ? base + 1 : -1;
		mp_log( "[MP] Idle animation : quickdata idle anim, layer %d.\n", peer->idleLayer );
	}
	else {
		peer->idleLayer = GetStateAnimLayer( p, FindStateIndex(p, stateName) );
		mp_log( "[MP] Idle animation : \"%s\" state's anim, layer %d.\n", stateName, peer->idleLayer );
	}
}

void SetAirAnimation( EIEvilHarryP* p, const char* stateName, Peer* peer )
{
	peer->airLayer = GetStateAnimLayer( p, FindStateIndex(p, stateName) );
	mp_log( "[MP] Air animation : \"%s\" state's anim, layer %d.\n", stateName, peer->airLayer );
}

static void InitLocomotion( EIEvilHarryP* p, EIHarry* harry, Peer* peer )
{
	ResetLocomotion( peer );
	g_autoLocomotion = true;
	peer->mirrorClip = 0;
	peer->mirrorSlot = 0;
	peer->idleState = FindStateIndex( p, "idle left" );
	peer->idleLayer = GetStateAnimLayer( p, peer->idleState );

	mp_log( "[MP] Beast anims : move CRC 0x%.8X, idle CRC 0x%.8X, locomotion base layer %d, current layer %d, idle layer %d, runner layer %d.\n",
		p->m_animMoveCRC, p->m_animIdleCRC, p->m_locoBase, p->m_animIndex,
		peer->idleLayer, GetStateAnimLayer(p, FindStateIndex(p, "runner")) );

	SetIdleAnimation( p, 0, peer );
	if ( peer->idleLayer < 0 ) peer->idleLayer = GetStateAnimLayer( p, peer->idleState );

	// The quickdata move animation if the beast has one (Evil Harry doesn't), else
	// the "chase player" state's : the most natural upright run of the candidates
	// ("runner" is hunched, "superrun" is a dive).
	SetRunAnimation( p, 0, peer );
	if ( peer->runLayer < 0 ) SetRunAnimation( p, "chase player", peer );

	// Above sprint speed : the "superrun to spot to ice pick" state's clip.
	peer->sprintLayer = GetStateAnimLayer( p, FindStateIndex( p, "superrun to spot to ice pick" ) );
	mp_log( "[MP] Sprint animation : layer %d (above %.2f/tick).\n", peer->sprintLayer, SPRINT_START_SPEED );

	// In the air (jumping, falling) : the "falling" state's clip.
	SetAirAnimation( p, "falling", peer );

	// Face where Harry faces. Walking showed his rigid body's row 1 (local +Y)
	// points where he moves; m_transformMatrix is turned 180 degrees from that.
	const Matrix4f& m = harry->m_rigidBodyMatrix;
	peer->heading = atan2f( m.data[1][1], m.data[1][0] );
	mp_log( "[MP] Locomotion : idle state %d, heading %.0f deg.\n", peer->idleState, peer->heading * 180.0F / MP_PI );
}



// Writes the beast's world transform : facing g_cur->heading, at the target position.
// Rolling as a ball : Harry's display matrix (m_transformMatrix) is raised 1.2
// above his position and spins about his side axis, while his position and
// rigid-body matrix stay upright. Detected by that raise.
static const float BALL_RAISE_MIN = 0.5F;

static inline bool IsBallMode( const PlayerSnapshot* t )
{
	return t && t->transform.data[3][2] - t->position.z > BALL_RAISE_MIN;
}

static void SetBeastWorldMatrix( Peer* peer, EIEvilHarryP* p, const Vector3f& pos, const PlayerSnapshot* t )
{
	Matrix4f& world = p->m_worldMatrix;
	if ( IsBallMode(t) ) {
		// Take Harry's spinning, raised matrix as is (same skeleton, same forward axis).
		world = t->transform;
		world.data[3][0] = pos.x + (t->transform.data[3][0] - t->position.x);
		world.data[3][1] = pos.y + (t->transform.data[3][1] - t->position.y);
		world.data[3][2] = pos.z + (t->transform.data[3][2] - t->position.z);
		return;
	}
	float a = peer->heading - g_forwardLocalAngle;
	float c = cosf( a ), s = sinf( a );
	world.data[0][0] =  c;   world.data[0][1] = s;     world.data[0][2] = 0.0F;  world.data[0][3] = 0.0F;
	world.data[1][0] = -s;   world.data[1][1] = c;     world.data[1][2] = 0.0F;  world.data[1][3] = 0.0F;
	world.data[2][0] = 0.0F; world.data[2][1] = 0.0F;  world.data[2][2] = 1.0F;  world.data[2][3] = 0.0F;
	world.data[3][0] = pos.x;
	world.data[3][1] = pos.y;
	world.data[3][2] = pos.z;
	world.data[3][3] = 1.0F;
}

// Before the beast's own tick : make it think it already is at the target.
static void PrePositionPuppet( EIEvilHarryP* p, Peer* peer )
{
	Vector3f pos;
	if ( GetPuppetTargetPosition(&pos, peer) ) {
		p->m_position = pos;
		SetBeastWorldMatrix( peer, p, pos, GetPuppetTarget(peer) );
		p->m_previousPosition = pos;
	}
}

// Body collision. The beast's "collision off" routine (0x418840, used when it dies
// or goes limp) clears the byte at +0x4C4 and its "on" routine (0x418910) sets it
// back; with it at 0 Harry walks straight through. (The collision layer masks at
// +0x230/+0x564/+0x574/+0x584 made no difference, cleared or not.)
// The puppet is only a visual, so it is never solid.
bool g_puppetSolid = false;                        // Autotest flips it to compare.

static inline void ApplyPuppetCollision( EIEvilHarryP* p )
{
	p->m_enableCollision = g_puppetSolid;
}


static inline void ShowModel( EInstance* model, bool show )
{
	uint32_t flags = model->m_flags;
	model->m_flags = show ? (flags | INSTANCE_VISIBLE) : (flags & ~INSTANCE_VISIBLE);
}

static void ApplyPuppetItem( EIEvilHarryP* p )
{
	if ( p->m_pickaxeModel0 ) ShowModel( p->m_pickaxeModel0, false );
	if ( p->m_pickaxeModel1 ) ShowModel( p->m_pickaxeModel1, false );
}

// The puppet shows Harry's pose from the snapshot : his bones relative to his
// transform, placed on the puppet's. Both share one skeleton (76 bones), so this
// copies everything his clips blend together (taking items out, moving them with
// the item keys, etc.), which mirroring his main clip alone can't. Done right
// before the puppet renders, and marked posed for this frame so the game doesn't
// pose it over them. (The mirrored clip still runs underneath.)
int g_puppetPoseCopies = 0;          // For the autotest.

static void ApplyPuppetPose( EIEvilHarryP* p, const PlayerSnapshot* t )
{
	EAnimController* animCtrl = &p->m_animController;
	int bones = 0;
	Matrix4f* pose = GetPose( animCtrl, &bones );
	if ( !pose || !t->bones || bones != t->bones ) return;
	MatsMul( t->pose, bones, p->m_transformMatrix, pose );
	animCtrl->m_frameStamp = *RENDER_FRAME;
	g_puppetPoseCopies++;
}

// Items draw themselves (vtable slot 4 : (context, arg)) while out (item state
// +0x70 is 2-4, byte +0x91 set; the raft also needs byte +0x108), including while
// Harry takes them out and puts them away : most with their own matrix at +0x30;
// the animated ones (sling, shield, raft) with their bones; the pickaxes on Harry's
// hand matrices.
//
// Each time one draws for Harry, the latest snapshot notes where, relative to his
// transform. Since the puppet shows the same pose, the puppet's render then draws
// the item there relative to the puppet's transform, with those fields forced for
// the duration of the call.
typedef void (__thiscall *ItemDrawFunc)( void* item, void* context, void* arg );
static const uint32_t ITEM_SLOT_DRAW = 4 * 4;
static Matrix4f g_puppetItemPose[MAX_ITEM_BONES];     // The copy's bones (static : 16-byte aligned).

// "anim" : offset of the item's animated model (an EAnimController), 0 if none.
// "shown" : a byte its draw needs set (cleared as soon as Harry's is put away, so
// the copy forces it), 0 if none. (The raft's is "inflated" : without it, its draw
// only poses it.)
// The stink bomb isn't in the wheel menu, but costs nothing to cover.
static const struct { int item; uint32_t vtable; uint32_t anim; uint32_t shown; } DRAWN_ITEMS[] = {
	{ InventoryItem::TORCH,      0x89A200, 0,    0     },
	{ InventoryItem::TNT,        0x89A160, 0,    0     },
	{ InventoryItem::RAFT,       0x8949B0, 0xA4, 0x108 },
	{ InventoryItem::SLING,      0x8971D0, 0xA0, 0x104 },
	{ InventoryItem::SHIELD,     0x896F90, 0xA4, 0x108 },
	{ InventoryItem::GAS_MASK,   0x87CAD8, 0,    0     },
	{ InventoryItem::CANTEEN,    0x86DCB0, 0,    0     },
	{ InventoryItem::STINK_BOMB, 0x8994B8, 0,    0     },
	{ InventoryItem::PICKAXES,   0x88ED70, 0,    0     },
};
static const int NUM_DRAWN_ITEMS = sizeof(DRAWN_ITEMS) / sizeof(DRAWN_ITEMS[0]);
static ItemDrawFunc g_origItemDraw[NUM_DRAWN_ITEMS];

int g_puppetItemDraws = 0;            // For the autotest.
int g_aimItemDraws = 0;
int g_copyDrawsAlone = 0;             // For the autotest : copies drawn while Harry draws no item.
static DWORD g_harryItemDrawTime = 0;

static void NoteHarryItemDraw( int slot, EItemBase* item )
{
	EIHarry* owner = item->m_harry;
	PlayerSnapshot* s = LatestSnapshot();
	if ( !owner || owner != Gizmod::getHarry() || !s ) return;
	// Until it has inflated, the raft's draw only poses it (something else shows it).
	if ( DRAWN_ITEMS[slot].item == InventoryItem::RAFT && !((EItemRaft*) item)->m_inflated ) return;
	Matrix4f inverse;
	if ( !MatInverseAffine( owner->m_transformMatrix, &inverse ) ) return;

	s->itemBones = 0;
	if ( DRAWN_ITEMS[slot].anim ) {
		int bones = 0;
		Matrix4f* pose = GetPose( (EAnimController*) ((uint8_t*) item + DRAWN_ITEMS[slot].anim), &bones );
		static bool logged[NUM_DRAWN_ITEMS] = { false, };
		if ( !logged[slot] ) {
			mp_log( "[MP] Item %d's animated model has %d bones.\n", DRAWN_ITEMS[slot].item, bones );
			logged[slot] = true;
		}
		if ( !pose || bones <= 0 || bones > MAX_ITEM_BONES ) return;
		MatsMul( pose, bones, inverse, s->itemPose );   // Posed by the draw that just ran.
		s->itemBones = bones;
	}
	Matrix4f world;
	CopyMatrix( world, item->m_matrix );
	MatMul( world, inverse, &s->itemLocal );
	s->itemState = item->m_state;
	s->drawnSlot = slot;
	g_harryItemDrawTime = GetTickCount();
}

// The game doesn't draw Harry's item when it doesn't need to (on the raft beside a
// wall, the camera goes into his head : the raft isn't drawn), but it's still out :
// the others kept seeing his raft vanish. While the item last drawn is still out,
// a frame without its draw sends it as it was last drawn (relative to Harry).
static struct { int slot; int itemState; int itemBones; Matrix4f itemLocal; Matrix4f itemPose[MAX_ITEM_BONES]; } g_lastItemDraw = { -1 };

void KeepUndrawnItem( EIHarry* harry )          // On the snapshot about to be sent.
{
	if ( !g_haveLocal ) return;
	PlayerSnapshot& s = g_local;
	if ( s.drawnSlot >= 0 ) {   // Drawn : noted.
		g_lastItemDraw.slot = s.drawnSlot;
		g_lastItemDraw.itemState = s.itemState;
		g_lastItemDraw.itemBones = s.itemBones;
		g_lastItemDraw.itemLocal = s.itemLocal;
		if ( s.itemBones > 0 ) memcpy( g_lastItemDraw.itemPose, s.itemPose, s.itemBones * sizeof(Matrix4f) );
		return;
	}
	int slot = g_lastItemDraw.slot;
	if ( slot < 0 ) return;
	int index = DRAWN_ITEMS[slot].item;
	EItemBase* item = GetItemObject( index );
	if ( !item || GetHeldItem(harry) != index || item->m_state != 3 ) { g_lastItemDraw.slot = -1; return; }   // Put away.
	s.drawnSlot = slot;
	s.itemState = g_lastItemDraw.itemState;
	s.itemBones = g_lastItemDraw.itemBones;
	s.itemLocal = g_lastItemDraw.itemLocal;
	if ( s.itemBones > 0 ) memcpy( s.itemPose, g_lastItemDraw.itemPose, s.itemBones * sizeof(Matrix4f) );
	static DWORD lastLog = 0;
	if ( GetTickCount() - lastLog > 5000 ) {
		lastLog = GetTickCount();
		mp_log( "[MP] Harry's item %d is out but wasn't drawn (camera in his head?) : sent as last drawn.\n", index );
	}
}

// Swaps a Matrix4f field (maybe not 16-byte aligned) for "m * base", keeping the old one.
static void SwapMatrix( void* field, const Matrix4f& m, const Matrix4f& base, Matrix4f* saved )
{
	Matrix4f world;
	MatMul( m, base, &world );
	memcpy( saved, field, sizeof(Matrix4f) );
	memcpy( field, &world, sizeof(Matrix4f) );
}

// Draws item slot "slot" as snapshot "t" saw it, relative to "base" instead of
// Harry's transform, whatever state Harry's item is in now.
static void DrawItemAt( int slot, const PlayerSnapshot* t, const Matrix4f& base, void* context, void* arg )
{
	int index = DRAWN_ITEMS[slot].item;
	EItemBase* item = GetItemObject( index );
	EIHarry* harry = Gizmod::getHarry();
	if ( !item || !harry ) return;

	// Animated items : the snapshot's bones, marked posed so the draw keeps them.
	EAnimController* anim = (EAnimController*) (DRAWN_ITEMS[slot].anim ? (uint8_t*) item + DRAWN_ITEMS[slot].anim : 0);
	Matrix4f* harryPose = 0;
	uint32_t stamp = 0;
	if ( anim ) {
		int bones = 0;
		harryPose = GetPose( anim, &bones );   // (None yet if Harry never took his out : the copy brings its own.)
		if ( !t->itemBones || bones != t->itemBones ) return;
		MatsMul( t->itemPose, bones, base, g_puppetItemPose );
		stamp = anim->m_frameStamp;
		anim->m_pose = g_puppetItemPose;
		anim->m_frameStamp = *RENDER_FRAME;
	}

	Matrix4f savedMatrix, savedRight, savedLeft;
	SwapMatrix( &item->m_matrix, t->itemLocal, base, &savedMatrix );
	bool hands = index == InventoryItem::PICKAXES;   // They draw on Harry's hands.
	if ( hands ) {
		SwapMatrix( &harry->m_matRHandHold, t->handR, base, &savedRight );
		SwapMatrix( &harry->m_matLHand, t->handL, base, &savedLeft );
	}
	int savedState = item->m_state;
	bool savedActive = item->m_active;
	uint32_t shown = DRAWN_ITEMS[slot].shown;
	bool savedShown = shown ? FIELD(item, bool, shown) : false;
	item->m_state = t->itemState;
	item->m_active = true;
	if ( shown ) FIELD(item, bool, shown) = true;

	g_origItemDraw[slot]( item, context, arg );
	g_puppetItemDraws++;
	if ( GetTickCount() - g_harryItemDrawTime > 150 ) g_copyDrawsAlone++;

	if ( shown ) FIELD(item, bool, shown) = savedShown;
	item->m_active = savedActive;
	item->m_state = savedState;
	if ( hands ) {
		CopyMatrix( harry->m_matLHand, savedLeft );
		CopyMatrix( harry->m_matRHandHold, savedRight );
	}
	CopyMatrix( item->m_matrix, savedMatrix );
	if ( anim ) {
		anim->m_pose = harryPose;
		anim->m_frameStamp = stamp;
	}
}

// Draws the item the puppet's snapshot says Harry drew, if it is "slot".
static void DrawPuppetItem( Peer* peer, int slot, void* context, void* arg )
{
	if ( !peer->beast ) return;
	const PlayerSnapshot* t = GetPuppetTarget( peer );
	if ( !t || t->drawnSlot != slot ) return;
	DrawItemAt( slot, t, peer->beast->m_transformMatrix, context, arg );
}

// The copies are drawn by the puppets' render (the raft's too : it used to be drawn
// right after Harry's own raft, so the other players' rafts vanished whenever Harry
// put his away).
#define ITEM_DRAW_HOOK( n ) \
	static void __fastcall ItemDraw_Hook##n( EItemBase* item, void*, void* context, void* arg ) \
	{ \
		if ( TestHidesHarryItem(item) ) return; \
		g_origItemDraw[n]( item, context, arg ); \
		NoteHarryItemDraw( n, item ); \
	}
ITEM_DRAW_HOOK( 0 )
ITEM_DRAW_HOOK( 1 )
ITEM_DRAW_HOOK( 2 )
ITEM_DRAW_HOOK( 3 )
ITEM_DRAW_HOOK( 4 )
ITEM_DRAW_HOOK( 5 )
ITEM_DRAW_HOOK( 6 )
ITEM_DRAW_HOOK( 7 )
ITEM_DRAW_HOOK( 8 )
static void* const ITEM_DRAW_HOOKS[] = {
	&ItemDraw_Hook0, &ItemDraw_Hook1, &ItemDraw_Hook2, &ItemDraw_Hook3, &ItemDraw_Hook4,
	&ItemDraw_Hook5, &ItemDraw_Hook6, &ItemDraw_Hook7, &ItemDraw_Hook8,
};
static_assert( sizeof(ITEM_DRAW_HOOKS) / sizeof(ITEM_DRAW_HOOKS[0]) == NUM_DRAWN_ITEMS, "one hook per drawn item" );

static void __stdcall _EIEvilHarryP_Render_Hook( EIEvilHarryP* self, void* context, void* arg )
{
	Peer* peer = FindPeerFromBeast( self );
	if ( !peer ) {
		// One of our puppets nobody drives now (set aside, or left over) : the game
		// drew it as its own Evil Harry, a frozen copy of its player. Not drawn.
		// (Only this world's and the last one's : the game's own Evil Harry may reuse
		// an old puppet's address in a level long gone.)
		int spawned = -1;
		EIHarry* harry = Gizmod::getHarry();
		for ( int i = 0; i < g_spawnedCount; i++ )
			if ( g_spawned[i].beast == self && (g_spawned[i].world == harry->m_world || g_spawned[i].world == g_prevWorld) )
				spawned = i;

		if ( spawned >= 0 ) {
			return;
		}

		EIBeast_Render( self, context, arg );
		return;
	}

	if ( IsParkedPeer(peer) ) return;   // The emote preview's, between previews.

	const PlayerSnapshot* t = GetPuppetTarget( peer );
	// Online with nobody to show here (they left, or are in another level) : not drawn.
	if ( g_networked && !t ) return;
	if ( g_levelLeaving ) return;   // The old level, still shown (NoteLevelLeaving).
	bool viewer = IsViewerPeer( peer );   // No pose copy, name tag or items : just the clip.
	if ( t && !ForcedClip(peer) ) ApplyPuppetPose( self, t );

	EIBeast_Render( self, context, arg );

	if ( t && !viewer ) NoteNameTag( peer, self );
	if ( t && !viewer && t->drawnSlot >= 0 ) DrawPuppetItem( peer, t->drawnSlot, context, arg );
}
MAKE_THISCALL_WRAPPER( EIEvilHarryP_Render_Hook, _EIEvilHarryP_Render_Hook );


void InstallItemDrawHooks()
{
	for ( int i = 0; i < NUM_DRAWN_ITEMS; i++ ) {
		const uint32_t slot = DRAWN_ITEMS[i].vtable + ITEM_SLOT_DRAW;
		g_origItemDraw[i] = injector::ReadMemory<ItemDrawFunc>( slot );
		injector::WriteMemory( slot, ITEM_DRAW_HOOKS[i] );
	}
	const uint32_t render = EIEvilHarryP::VTABLE + VTABLE_SLOT_RENDER;
	injector::WriteMemory( render, &EIEvilHarryP_Render_Hook );
}



// --------------------------------------------------------------------------------
// Pitfalls (EIPitMonster) eat any beast that touches their mouth : the beast is put to sleep and the pit
// stays shut on it. A puppet caught there kept the pit shut on the other players' screens.
// The pit's two contact handlers now let puppets through.
// --------------------------------------------------------------------------------

static const uint32_t PIT_CONTACT_SLOTS[2] = { 0x890444, 0x890448 };

GET_METHOD( 0x542DD0, bool, EIPitMonster_Contact4, EIPitMonster*, OverlapStruct* c, int a, int b, int d );
GET_METHOD( 0x542FD0, bool, EIPitMonster_Contact5, EIPitMonster*, OverlapStruct* c, int a, int b, int d, int e );

static bool IsAnyPuppet( EInstance* beast )
{
	if ( beast->GetType() != get_etypeinfo<EIEvilHarryP>() ) return false;
	if ( FindPeerFromBeast((EIEvilHarryP*) beast) ) return true;
	for ( int i = 0; i < g_spawnedCount; i++ ) if ( g_spawned[i].beast == beast ) return true;
	return false;
}

static bool PitSkips( OverlapStruct* c )
{
	if ( !c || !IsAnyPuppet(c->m_other) ) return false;
	return true;
}

static bool __stdcall _PitContact4_Hook( EIPitMonster* self, OverlapStruct* c, int a, int b, int d )
{
	if ( PitSkips(c) ) return false;
	return EIPitMonster_Contact4( self, c, a, b, d );
}
MAKE_THISCALL_WRAPPER( PitContact4_Hook, _PitContact4_Hook );

static bool __stdcall _PitContact5_Hook( EIPitMonster* self, OverlapStruct* c, int a, int b, int d, int e )
{
	if ( PitSkips(c) ) return false;
	return EIPitMonster_Contact5( self, c, a, b, d, e );
}
MAKE_THISCALL_WRAPPER( PitContact5_Hook, _PitContact5_Hook );

void InstallPitHooks()
{
	injector::WriteMemory( PIT_CONTACT_SLOTS[0], PitContact4_Hook );
	injector::WriteMemory( PIT_CONTACT_SLOTS[1], PitContact5_Hook );
}


// Creatures fade out with their distance to the camera (vtable +0x108, 0x41ED70,
// shared by every kind : sets the fade at +0x24 from 1 to 0, about 150 away, and
// the alpha at +0x1C, and returns true once gone : the render then skips it).
// The other players, and the enemies too, now stay in full view however far (up
// to the game's own cut-off, a few hundred away); the name tags still fade
// (TAG_FADE_STARTS).
static const uint32_t VTABLE_SLOT_DISTANCE_FADE = 0x108;
static const uint32_t DISTANCE_FADE_VTABLES[] = {   // Every vtable with it in that slot.
	0x869440, 0x869AD0, 0x86B3C8, 0x86C3D0, 0x86D1D8, 0x86D7A0, 0x86FD18, 0x873B68,
	0x874688, 0x875240, 0x875B30, 0x876400, 0x876800, 0x876D28, 0x877E78, 0x878310,
	0x878810, 0x878C00, 0x8792D0, 0x879770, 0x879B90, 0x881BE8, 0x8822D0, 0x889700,
	0x88A2C0, 0x88A668, 0x88AFE8, 0x88B4F0, 0x88B868, 0x88BE78, 0x88E8C8, 0x88F078,
	0x890400, 0x890A40, 0x8925A8, 0x893EA8, 0x894398, 0x896438, 0x896C10, 0x8974E0,
	0x897ED0, 0x898660, 0x898C20, 0x899130, 0x8999C8, 0x899DE0, 0x89A6D8, 0x8A18A0,
	0x8A1CF0, 0x8A2C70,
};

volatile LONG g_fadesStopped = 0;
float g_lastPuppetFade = 1.0F;          // What the game wanted for a puppet (tests).
volatile LONG g_puppetFadeCalls = 0;
bool g_allowPuppetFade = false;

GET_METHOD( 0x41ED70, bool, EIBeast_DistanceFade, EIBeast*, float );
static bool __stdcall _DistanceFade_Hook( EIBeast* self, float distance )
{
	const bool gone = EIBeast_DistanceFade( self, distance );
	float fade = self->m_opacity3;
	bool puppet = IsAnyPuppet( self );
	if ( puppet ) {
		InterlockedIncrement( &g_puppetFadeCalls );
		g_lastPuppetFade = fade;
	}
	if ( g_allowPuppetFade ) return gone;

	if ( puppet && fade < 1.0F && InterlockedIncrement(&g_fadesStopped) == 1 ) mp_log( "[MP] A puppet was fading out with distance (%.2f) : kept in full view.\n", fade );
	self->m_opacity3 = 1.0F;
	self->m_opacityFinal = self->m_opacity2 * self->m_alphaUser;
	return false;
}
MAKE_THISCALL_WRAPPER( DistanceFade_Hook, _DistanceFade_Hook );

void InstallFadeHooks()
{
	int hooked = 0, n = sizeof(DISTANCE_FADE_VTABLES) / sizeof(DISTANCE_FADE_VTABLES[0]);
	for ( int i = 0; i < n; i++ ) {
		const uint32_t slot = DISTANCE_FADE_VTABLES[i] + VTABLE_SLOT_DISTANCE_FADE;
		if ( injector::ReadMemory<uint32_t>(slot) != (uint32_t) EIBeast_DistanceFade ) continue;

		injector::WriteMemory( slot, &DistanceFade_Hook );
		hooked++;
	}
	if ( hooked != n ) mp_log( "[MP] Distance fade : %d of %d kinds of creature hooked; the others may still fade out.\n", hooked, n );
}


static void DrivePuppet( EIEvilHarryP* p, Peer* peer )
{
	ApplyPuppetCollision( p );

	Vector3f pos;
	if ( !GetPuppetTargetPosition(&pos, peer) ) {
		ApplyPuppetItem( p );
		return;
	}

	const PlayerSnapshot* t = GetPuppetTarget( peer );
	ApplyPuppetItem( p );
	const Vector3f* rot = &t->rotationEuler;

	p->m_position = pos;
	EICharacter_SetPos( p, &pos, true );
	EICharacter_SetRot( p, rot );

	SetBeastWorldMatrix( peer, p, pos, t );

	// Same orientation and position in the header transform.
	const Matrix4f& world = p->m_worldMatrix;
	for ( int r = 0; r < 3; r++ )
		for ( int c = 0; c < 3; c++ )
			p->m_transformMatrix.data[r][c] = world.data[r][c];
	p->m_transformMatrix.data[3][0] = world.data[3][0];
	p->m_transformMatrix.data[3][1] = world.data[3][1];
	p->m_transformMatrix.data[3][2] = world.data[3][2];
	p->m_previousPosition = pos;
}



// Evil Harry's state callbacks are a table of (function, state name) built when the
// game starts (0x84A4B0). In his boss fight, falling in the water puts him in
// "thrown from water", whose enter callback (0x47D490) shuts him in his ice cube
// (0x54B480 : added to the level as an instance of its own, where he is) and counts
// down something of the level's ([0x917034]+0xB38). A puppet runs Evil
// Harry's state machine where its player is : when they swam, the game put the
// puppet in that state, and the others saw them as a block of ice. Puppets skip
// that callback; the game's own Evil Harry still freezes.
typedef void (__thiscall *StateCallback)( EIEvilHarryP* self );
static StateCallback g_origThrownFromWater = 0;

static void __stdcall _ThrownFromWater_Hook( EIEvilHarryP* self )
{
	if ( FindPeerFromBeast(self) && !g_allowPuppetFreeze ) {
		if ( InterlockedIncrement(&g_puppetFreezesStopped) <= 3 ) mp_log( "[MP] A puppet was about to be frozen (Evil Harry's \"thrown from water\") : skipped.\n" );
		return;
	}
	g_origThrownFromWater( self );
}
MAKE_THISCALL_WRAPPER( ThrownFromWater_Hook, _ThrownFromWater_Hook );

// Each state of the state machine keeps its own copy of its callbacks (enter,
// update, leave), and every Evil Harry shares the one state machine : the hooks go
// there, checked on each Evil Harry tick (a level load may build it again).
static void HookThrownFromWater( EIEvilHarryP* b )
{
	static bool warned = false;
	EBeastStateMachine* sm = b->GetStateMachine();
	if ( !sm || !sm->m_states ) return;
	int hooked = 0;
	for ( int i = 0; i < (int) sm->m_numStates; i++ ) {
		EBeastState& st = sm->m_states[i];
		MemberFuncCallback* cbs[3] = { &st.m_onEnter, &st.m_onUpdate, &st.m_onLeave };
		for ( int k = 0; k < 3; k++ ) {
			uint32_t f = (uint32_t) cbs[k]->m_func;
			if ( f == (uint32_t) &ThrownFromWater_Hook ) { hooked++; continue; }
			if ( f != 0x47D490 ) continue;
			g_origThrownFromWater = (StateCallback) f;
			cbs[k]->m_func = (void (*)()) &ThrownFromWater_Hook;
			hooked++;
		}
	}
	if ( !hooked && !warned ) warned = true, mp_log( "[MP] Evil Harry's \"thrown from water\" isn't where we expect : puppets may freeze in water.\n" );
}

// A puppet we spawned in this world that no player has any more. Given back to its
// player if they have none (false : it ticks on as theirs); otherwise sent far below
// the level and not ticked, out of sight (true).
static bool HandleOrphan( EIEvilHarryP* self )
{
	EIHarry* harry = Gizmod::getHarry();
	if ( !harry ) return false;

	for ( int i = 0; i < g_spawnedCount; i++ ) {
		SpawnedPuppet& s = g_spawned[i];
		if ( s.beast != self || s.world != harry->m_world ) continue;

		Peer& owner = g_peers[s.owner];
		if ( !owner.beast ) {
			owner.beast = self;
			owner.lastLoggedState = owner.lastRequested = -2;
			owner.shownOutfit = -1;
			mp_log( "[MP] Player %d's old puppet 0x%.8X ticked with nobody driving it : given back to them.\n", s.owner, self );
			return false;
		}
		if ( !s.reported ) {
			s.reported = true;
			mp_log( "[MP] Player %d's old puppet 0x%.8X ticked with nobody driving it, and they have another : hidden.\n", s.owner, self );
		}
		Vector3f away = { self->EIBeast::m_position.x, self->EIBeast::m_position.y, -5000.0F };
		EICharacter_SetPos( self, &away, true );
		return true;
	}
	return false;
}

static void __stdcall _EIEvilHarryP_Tick_Hook( EIEvilHarryP* self )
{
	if ( TestStallsPuppet(self) ) return;

	HookThrownFromWater( self );
	Peer* peer = FindPeerFromBeast( self );
	if ( !peer ) peer = ReclaimPuppet( self );
	if ( !peer && HandleOrphan( self ) ) return;
	if ( !peer ) peer = FindPeerFromBeast( self );   // Given back to its player.
	if ( !peer ) {   // The game's own Evil Harry.
		EIBeast_Tick( self );
		return;
	}
	peer->lastTick = GetTickCount();
	peer->ticks++;

	if ( TestSkipsPuppetAI() ) {
		DrivePuppet( self, peer );
		return;
	}

	// When Harry dies and comes back the game removes the level's loose models, the
	// puppet's pickaxes among them, and Evil Harry's tick crashes without them
	// (0x47B04F). Init() makes new ones (it only makes those that are missing).
	if ( !self->m_pickaxeModel0 || !self->m_pickaxeModel1 ) {
		mp_log( "[MP] Player %d's puppet lost its pickaxe models (Harry died ?) : making new ones.\n", peer->id );
		self->Init();
		peer->lastRequested = -2;
		peer->shownOutfit = -1;   // Init gave it our Harry's outfit.
		if ( !self->m_pickaxeModel0 || !self->m_pickaxeModel1 ) return;   // Still none : its tick would crash.
	}
	ApplyPuppetOutfit( self, peer );

	UpdateLocomotion( self, peer );
	if ( ForcedClip(peer) && g_haveCamera ) {   // Animation survey, viewer : face the camera.
		const Matrix4f& cam = g_cameraWorld;
		peer->heading = atan2f( cam.data[3][1] - self->EIBeast::m_position.y, cam.data[3][0] - self->EIBeast::m_position.x );
	}

	if ( peer->forcedState >= 0 && self->m_stateIndexCurrent != peer->forcedState ) {
		if ( peer->lastRequested != peer->forcedState ) {
			mp_log( "[MP] Requesting state %d (current %d)...\n", peer->forcedState, self->m_stateIndexCurrent );
			peer->lastRequested = peer->forcedState;
		}
		self->m_forceNextState = true;
		self->m_nextState = peer->forcedState;
	}

	PrePositionPuppet( self, peer );

	EIBeast_Tick( self );

	if ( g_autoLocomotion ) ApplyLocomotionAnim( self, peer );

	if ( self->m_stateIndexCurrent != peer->lastLoggedState ) {
		mp_log( "[MP] Player %d's puppet is now in state %d.\n", peer->id, self->m_stateIndexCurrent );
		peer->lastLoggedState = self->m_stateIndexCurrent;
	}

	DrivePuppet( self, peer );           // Then override where it is.
}
MAKE_THISCALL_WRAPPER( EIEvilHarryP_Tick_Hook, _EIEvilHarryP_Tick_Hook );

void InstallPuppetTickHook()
{
	const uint32_t evilSlot = EIEvilHarryP::VTABLE + VTABLE_SLOT_TICK;
	injector::WriteMemory( evilSlot, &EIEvilHarryP_Tick_Hook );
}
