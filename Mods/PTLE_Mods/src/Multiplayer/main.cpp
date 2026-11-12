// --------------------------------------------------------------------------------
// Multiplayer (milestone 1 : puppet test).
//
// Spawns a second Harry-looking character (the game's Evil Harry beast) and drives
// its transform from code every frame. There is no networking yet : the puppet
// replays the local player's own movement a short delay behind, which proves we
// can own and move a second character. The "remote player" source is isolated in
// PushLocalSnapshot() / GetPuppetTarget() so it can later be fed from a server.
//
// Hotkeys (only while the game window has focus) :
//   F5 : spawn the puppet in the current level.
//   F6 : cycle the puppet's animation : automatic (idle when still, run when
//        moving, facing its direction of travel), then idle/rest states by hand.
//   F7 : cycle the replay delay (0.25s .. 3s).
// --------------------------------------------------------------------------------

#include <Windows.h>
#include <math.h>
#include <ShlObj.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "gizmod/Gizmod.h"
#include "gizmod/GizmodPlugin.h"
#include "gizmod/event/LoadLevelEvent.h"

#include "injector/injector.hpp"

#include "ptle/EIHarry.h"
#include "ptle/EIEvilHarryP.h"
#include "ptle/EBeastStateMachine.h"
#include "ptle/ERLevel.h"
#include "ptle/math/Matrix4f.h"
#include "ptle/math/Vector3f.h"
#include "ptle/types/types.h"

#include "utils/func.h"


GET_METHOD( 0x626670, void, AddToWorld, ERLevel*, EInstance* );
GET_METHOD( 0x6268D0, void, ERLevel_SetupEntity, ERLevel*, EInstance*, EInstance* );
GET_METHOD( 0x412850, void, EIBeast_LoadBeastAsset, EIBeast* );
GET_FUNC(   0x6F8EC0, uint32_t, HashPath, char* );


// Virtual tables (see "Memory Research/PC/Virtual tables.txt").
static const uint32_t VTABLE_HARRY       = 0x87D300;
static const uint32_t VTABLE_EVILHARRY_P = 0x875240;
static const uint32_t VTABLE_SLOT_TICK   = 0x2C;     // EInstance::Tick().

static const uint32_t HWND_GAME_WINDOW   = 0x93ED60;

// Offset the EIBeast destructor frees (0x412A20 : "add ecx, 30Ch"). We expect it to be
// m_beastTypeName, which must therefore not point to static memory when the level unloads.
static const size_t EXPECTED_BEASTNAME_OFFSET = 0x30C;


typedef void (__thiscall *TickFunc)( EInstance* );
static TickFunc g_origHarryTick = 0;
static TickFunc g_origEvilHarryTick = 0;

static Logger* logger() { return Gizmod::getInstance()->getLogger(); }



// --------------------------------------------------------------------------------
// Logging : Gizmod console + Documents\PTLE Multiplayer\logs\multiplayer.log
// (the game folder isn't writable without admin rights).
// --------------------------------------------------------------------------------
static FILE* g_logFile = 0;
static CRITICAL_SECTION g_logLock;
static char g_logDir[MAX_PATH] = { 0, };

static void OpenLogFile()
{
	InitializeCriticalSection( &g_logLock );

	char path[MAX_PATH];
	if ( FAILED( SHGetFolderPathA( 0, CSIDL_PERSONAL, 0, 0, path ) ) ) return;

	strcat_s( path, "\\PTLE Multiplayer" );
	CreateDirectoryA( path, 0 );
	strcat_s( path, "\\logs" );
	CreateDirectoryA( path, 0 );
	strcpy_s( g_logDir, path );
	strcat_s( path, "\\multiplayer.log" );

	g_logFile = _fsopen( path, "w", _SH_DENYWR );
}

static void mp_log( const char* fmt, ... )
{
	char buf[1024];
	va_list args;
	va_start( args, fmt );
	vsnprintf_s( buf, _TRUNCATE, fmt, args );
	va_end( args );

	EnterCriticalSection( &g_logLock );
	logger()->log( buf );
	if ( g_logFile ) {
		SYSTEMTIME t;
		GetLocalTime( &t );
		fprintf( g_logFile, "%02d:%02d:%02d.%03d %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf );
		fflush( g_logFile );
	}
	LeaveCriticalSection( &g_logLock );
}

// Records access violations (address + registers) before Gizmod's crash handler
// runs. First-chance only, and capped, so handled exceptions don't flood the log.
static LONG CALLBACK CrashLogger( EXCEPTION_POINTERS* e )
{
	static volatile LONG count = 0;
	DWORD code = e->ExceptionRecord->ExceptionCode;
	if ( code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION ) return EXCEPTION_CONTINUE_SEARCH;
	if ( InterlockedIncrement( &count ) > 5 ) return EXCEPTION_CONTINUE_SEARCH;

	CONTEXT* c = e->ContextRecord;
	mp_log( "[MP] EXCEPTION 0x%.8X at EIP 0x%.8X (%s 0x%.8X) EAX=%.8X EBX=%.8X ECX=%.8X EDX=%.8X ESI=%.8X EDI=%.8X ESP=%.8X\n",
		code, c->Eip,
		e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
		(uint32_t) e->ExceptionRecord->ExceptionInformation[1],
		c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp );

	// A few return addresses from the stack help locate the caller.
	uint32_t* sp = (uint32_t*) c->Esp;
	for ( int i = 0; i < 24; i++ ) {
		uint32_t v = 0;
		if ( !IsBadReadPtr( sp + i, 4 ) ) v = sp[i];
		if ( v >= 0x401000 && v < 0x869000 ) mp_log( "[MP]   stack+%02X : 0x%.8X\n", i * 4, v );
	}
	return EXCEPTION_CONTINUE_SEARCH;
}



// --------------------------------------------------------------------------------
// Remote player source (for now : a delayed echo of the local player).
// --------------------------------------------------------------------------------
struct PlayerSnapshot
{
	Matrix4f transform;
	Vector3f position;
	Vector3f rotationEuler;
	bool onGround;                               // EICharacter::m_onGround (CheatLab writes it too).
	uint32_t clipCRC;                            // Animation clip the player is playing (0 = unknown).
	float clipWeight;
};

static const int HISTORY_SIZE = 256;
static PlayerSnapshot g_history[HISTORY_SIZE];   // Static storage keeps Matrix4f 16-byte aligned.
static int g_historyHead = 0;                    // Next slot to write.
static int g_historyCount = 0;

static const int DELAY_CHOICES[] = { 8, 15, 30, 60, 90, 180 };   // In game ticks.
static int g_delayIndex = 3;

static void ResetHistory()
{
	g_historyHead = 0;
	g_historyCount = 0;
}

// --------------------------------------------------------------------------------
// Animation clips. EAnimController (+0xB4 in every character) keeps its layers in a
// map at +0x2C keyed by layer index (lookup 0x6F3A50). A layer (0xA0 bytes) holds
// its clip instance at +0x78 (clip CRC at +0x0C) and its weight at +0x4C.
// 0x61CAD0( layer, clipCRC ) loads a clip into a layer (creating the layer).
// --------------------------------------------------------------------------------
GET_METHOD( 0x6F3A50, int,  AnimMap_Find,     void*, int, void** );
GET_METHOD( 0x61CAD0, void, AnimCtrl_SetClip, EAnimController*, int, uint32_t );

static const uint32_t CHAR_ANIMCTRL   = 0xB4;
static const uint32_t ANIMCTRL_LAYERS = 0x2C;
static const uint32_t LAYER_CLIP      = 0x78;
static const uint32_t LAYER_WEIGHT    = 0x4C;
static const uint32_t CLIP_CRC        = 0x0C;
static const int      MAX_LAYER_SCAN  = 256;

static uint8_t* GetAnimLayer( EICharacter* c, int key )
{
	void* layer = 0;
	void* map = (uint8_t*) c + CHAR_ANIMCTRL + ANIMCTRL_LAYERS;
	if ( !AnimMap_Find( map, key, &layer ) ) return 0;
	return (uint8_t*) layer;
}

static uint32_t GetLayerClipCRC( uint8_t* layer )
{
	uint8_t* clip = *(uint8_t**) (layer + LAYER_CLIP);
	return clip ? *(uint32_t*) (clip + CLIP_CRC) : 0;
}

// The clip a character is mostly showing : its highest-weight layer.
static uint32_t GetPlayingClip( EICharacter* c, int* outLayer, float* outWeight )
{
	uint32_t best = 0;
	float bestWeight = 0.01F;
	int bestLayer = -1;
	for ( int i = 0; i < MAX_LAYER_SCAN; i++ ) {
		uint8_t* layer = GetAnimLayer( c, i );
		if ( !layer ) continue;
		float w = *(float*) (layer + LAYER_WEIGHT);
		uint32_t crc = GetLayerClipCRC( layer );
		if ( crc && w > bestWeight ) { best = crc; bestWeight = w; bestLayer = i; }
	}
	if ( outLayer ) *outLayer = bestLayer;
	if ( outWeight ) *outWeight = bestWeight;
	return best;
}

static void PushLocalSnapshot( EIHarry* harry )
{
	PlayerSnapshot& s = g_history[g_historyHead];
	s.transform     = harry->m_transformMatrix;
	s.position      = harry->m_position;
	s.rotationEuler = harry->m_rotationEuler;
	s.onGround      = harry->m_onGround;

	int layer = -1;
	s.clipCRC = GetPlayingClip( harry, &layer, &s.clipWeight );
	static uint32_t lastClip = 0;
	if ( s.clipCRC != lastClip ) {
		mp_log( "[MP] Harry plays clip 0x%.8X (layer %d, weight %.2f, %s).\n",
			s.clipCRC, layer, s.clipWeight, harry->m_onGround ? "ground" : "air" );
		lastClip = s.clipCRC;
	}

	g_historyHead = (g_historyHead + 1) % HISTORY_SIZE;
	if ( g_historyCount < HISTORY_SIZE ) g_historyCount++;
}

static const PlayerSnapshot* GetPuppetTarget()
{
	int delay = DELAY_CHOICES[g_delayIndex];
	if ( g_historyCount <= delay ) return 0;

	int idx = (g_historyHead - 1 - delay + HISTORY_SIZE) % HISTORY_SIZE;
	return &g_history[idx];
}



// --------------------------------------------------------------------------------
// Puppet.
// --------------------------------------------------------------------------------
static EIEvilHarryP* g_puppet = 0;
static int g_forcedState = -1;

// Index of a state by its short name ("idle left" matches "evil harry.idle left").
static int FindStateIndex( EIBeast* beast, const char* shortName )
{
	EBeastStateMachine* sm = beast->GetStateMachine();
	if ( !sm || !sm->m_states ) return -1;
	for ( int i = 0; i < (int) sm->m_numStates; i++ ) {
		const char* name = sm->m_states[i].m_name;
		const char* dot = name ? strchr( name, '.' ) : 0;
		if ( dot && strcmp( dot + 1, shortName ) == 0 ) return i;
	}
	return -1;
}
static char g_puppetName[] = "evil harry";

static void LogStateMachine( EIBeast* beast )
{
	EBeastStateMachine* sm = beast->GetStateMachine();
	if ( !sm || !sm->m_states ) {
		mp_log( "[MP] Puppet has no state machine.\n" );
		return;
	}

	mp_log( "[MP] Puppet state machine \"%s\", %d states :\n", sm->m_beastTypeName, sm->m_numStates );
	for ( int i = 0; i < sm->m_numStates; i++ ) {
		mp_log( "[MP]   %d : %s\n", i, sm->m_states[i].m_name );
	}
}

static void InitLocomotion( EIEvilHarryP* p, EIHarry* harry );

static void SpawnPuppet( EIHarry* harry )
{
	if ( g_puppet ) {
		mp_log( "[MP] Puppet already spawned in this level.\n" );
		return;
	}
	if ( !harry->m_world ) {
		mp_log( "[MP] No world yet, can't spawn.\n" );
		return;
	}

	const type_info_t* type = get_type_by_vtable( VTABLE_EVILHARRY_P );
	if ( !type ) {
		mp_log( "[MP] EIEvilHarryP type not registered.\n" );
		return;
	}

	EIEvilHarryP* p = instantiate_object<EIEvilHarryP>( type );

	p->EIBeast::m_position.x = harry->m_position.x;
	p->EIBeast::m_position.y = harry->m_position.y + 15.0F;
	p->EIBeast::m_position.z = harry->m_position.z;
	p->m_rotation.x = 0.0F;
	p->m_rotation.y = 0.0F;
	p->m_rotation.z = 0.0F;
	p->m_rotation.w = 1.0F;

	// NOTE : the beast destructor frees this string. ForgetPuppet() clears it
	// before the level unloads so the game never frees our static memory.
	p->m_beastTypeCRC = HashPath( (char*) "BEvilHarryP" );
	p->m_beastTypeName = g_puppetName;
	EIBeast_LoadBeastAsset( p );
	p->DeserializePart2();

	if ( !p->m_beastType ) {
		mp_log( "[MP] WARNING : Evil Harry asset didn't load in this level, puppet may be invisible.\n" );
	}

	AddToWorld( harry->m_world, p );
	ERLevel_SetupEntity( harry->m_world, p, 0 );
	p->Init();

	g_puppet = p;

	// Its default state (21, "get ready for boss fight") draws it as a black
	// silhouette. Start calm and lit instead.
	g_forcedState = FindStateIndex( p, "idle left" );
	mp_log( "[MP] Puppet starts in state %d (idle left).\n", g_forcedState );
	InitLocomotion( p, harry );

	mp_log( "[MP] Puppet spawned at 0x%.8X (m_beastTypeName offset 0x%X, expected 0x%X).\n",
		p, (unsigned) offsetof(EIBeast, m_beastTypeName), (unsigned) EXPECTED_BEASTNAME_OFFSET );
	mp_log( "[MP] Harry at (%.2f, %.2f, %.2f). header offsets : EICharacter::m_position 0x%X, m_transformMatrix 0x%X, EIBeast::m_position 0x%X, m_rotation 0x%X\n",
		harry->m_position.x, harry->m_position.y, harry->m_position.z,
		(unsigned) offsetof(EICharacter, m_position), (unsigned) offsetof(EICharacter, m_transformMatrix),
		(unsigned) offsetof(EIBeast, m_position), (unsigned) offsetof(EIBeast, m_rotation) );

	// Raw dump of the transform-related area, before we touch anything.
	const uint32_t ranges[][2] = { { 0x120, 0x1B0 }, { 0x2F0, 0x350 } };
	for ( int r = 0; r < 2; r++ ) {
		for ( uint32_t o = ranges[r][0]; o < ranges[r][1]; o += 16 ) {
			float* f = (float*) ((uint8_t*) p + o);
			mp_log( "[MP]   +0x%03X : %10.3f %10.3f %10.3f %10.3f\n", o, f[0], f[1], f[2], f[3] );
		}
	}
	LogStateMachine( p );
}

// Called before the level is torn down : the puppet gets destroyed with it.
static void ForgetPuppet()
{
	if ( g_puppet ) {
		// The name points to our static memory. Detach it so the game's
		// destructor doesn't try to free it (known crash at 0x412A32).
		if ( g_puppet->m_beastTypeName == g_puppetName ) {
			g_puppet->m_beastTypeName = 0;
		}
		mp_log( "[MP] Level unloading, puppet released.\n" );
	}
	g_puppet = 0;
	ResetHistory();
}

// The game's own setters (writing the fields directly isn't enough : the model kept
// rendering at the spawn point). SetPos also moves the instance in the level's
// spatial structure (0x62B470), rebuilds the transform (vtable +0x124) and updates
// the collision object at +0x270. SetRot writes +0x140 and calls vtable +0x120.
GET_METHOD( 0x60FEA0, void, EICharacter_SetPos, EICharacter*, Vector3f*, bool );
GET_METHOD( 0x60FFA0, void, EICharacter_SetRot, EICharacter*, Vector3f* );

// Extra offset added to the puppet's position (the autotest uses it so the
// puppet stands beside Harry instead of inside him).
static Vector3f g_puppetOffset = { 0.0F, 0.0F, 0.0F };

static bool GetPuppetTargetPosition( Vector3f* out )
{
	const PlayerSnapshot* t = GetPuppetTarget();
	if ( !t ) return false;
	out->x = t->position.x + g_puppetOffset.x;
	out->y = t->position.y + g_puppetOffset.y;
	out->z = t->position.z + g_puppetOffset.z;
	return true;
}

// Raw field access. Spawn-time dump (Mama-Oullo Tower) showed : +0x134 position,
// +0x140 rotation, +0x14C scale (1,1,1), +0x160 transform matrix with its
// translation row at +0x190, +0x2FC EIBeast position, +0x310 quaternion.
// (0x414B10, vtable +0x120, rebuilds the matrix rotation and applies the scale;
// it does NOT set the translation.)
//
// The beast tick (0x414F80) reads its world position from +0x360, the translation
// row of a matrix at +0x330, and stores it at +0x370 ("previous position") when
// done. +0x330 is the beast's real world transform.
#define FIELD( obj, type, offset ) (*(type*)((uint8_t*)(obj) + (offset)))
static const uint32_t BEAST_SCALE       = 0x14C;
static const uint32_t BEAST_WORLD       = 0x330;   // Matrix4f.
static const uint32_t BEAST_PREV_POS    = 0x370;   // Vector3f.

// --------------------------------------------------------------------------------
// Locomotion : the puppet's animation and facing come from how its target moves
// (later : how the remote player moves). x/y is the ground plane, z is up.
// --------------------------------------------------------------------------------
static const float MP_PI = 3.14159265F;

static bool  g_autoLocomotion = true;   // F6 switches to hand-forced states.
static int   g_idleState = -1;
static int   g_runLayer = -1;                // Animation layers (see below).
static int   g_idleLayer = -1;
static int   g_sprintLayer = -1;
static float g_velX = 0.0F, g_velY = 0.0F;   // Smoothed, units per tick.
static float g_moveSpeed = 0.0F;
static float g_heading = 0.0F;               // Direction of travel, atan2( y, x ).
static bool  g_moving = false;
static bool  g_sprinting = false;
static float g_peakSpeed = 0.0F;             // Fastest speed of the current move (logged).
static bool  g_haveLastTarget = false;
static Vector3f g_lastTarget;
static int   g_airLayer = -1;
static bool  g_airborne = false;
static int   g_airTicks = 0;
static float g_airStartZ = 0.0F;
static const int AIRBORNE_MIN_TICKS = 6;   // Slopes and bumps blip for up to ~5 ticks.
static const float JUMP_RISE_SPEED  = 0.10F;   // Units per tick upward while off the ground.

static const float RUN_START_SPEED = 0.05F;  // Units per tick (hysteresis).
static const float RUN_STOP_SPEED  = 0.02F;
// Harry's normal run measured about 0.36/tick (peaks near 0.44 on slopes).
static const float SPRINT_START_SPEED = 0.50F;
static const float SPRINT_STOP_SPEED  = 0.45F;
static const float TELEPORT_DIST   = 5.0F;   // Bigger jumps aren't movement.

// Angle of the model's forward axis in its own frame (0 = +X, pi/2 = +Y).
// +Y made it run backwards, so the model faces local -Y.
static float g_forwardLocalAngle = -MP_PI / 2.0F;

static void ResetLocomotion()
{
	g_velX = g_velY = g_moveSpeed = 0.0F;
	g_moving = false;
	g_sprinting = false;
	g_peakSpeed = 0.0F;
	g_airborne = false;
	g_airTicks = 0;
	g_haveLastTarget = false;
}

static void UpdateLocomotion( EIEvilHarryP* p )
{
	Vector3f pos;
	if ( !GetPuppetTargetPosition( &pos ) ) return;

	if ( g_haveLastTarget ) {
		float dx = pos.x - g_lastTarget.x;
		float dy = pos.y - g_lastTarget.y;
		if ( dx * dx + dy * dy > TELEPORT_DIST * TELEPORT_DIST ) dx = dy = 0.0F;

		g_velX = g_velX * 0.75F + dx * 0.25F;
		g_velY = g_velY * 0.75F + dy * 0.25F;
		g_moveSpeed = sqrtf( g_velX * g_velX + g_velY * g_velY );
		if ( g_moveSpeed > 0.01F ) g_heading = atan2f( g_velY, g_velX );
	}
	g_lastTarget = pos;
	g_haveLastTarget = true;

	bool wasMoving = g_moving;
	if ( g_moving && g_moveSpeed < RUN_STOP_SPEED ) g_moving = false;
	else if ( !g_moving && g_moveSpeed > RUN_START_SPEED ) g_moving = true;
	if ( g_moving != wasMoving ) {
		if ( g_moving ) {
			mp_log( "[MP] Puppet starts moving (speed %.3f/tick, heading %.0f deg).\n", g_moveSpeed, g_heading * 180.0F / MP_PI );
			g_peakSpeed = 0.0F;
		}
		else {
			mp_log( "[MP] Puppet stops (top speed %.3f/tick).\n", g_peakSpeed );
		}
	}
	if ( g_moving && g_moveSpeed > g_peakSpeed ) g_peakSpeed = g_moveSpeed;

	// Airborne : the replayed player has been off the ground for a few ticks
	// (ignores one-tick blips on bumps and slopes). Landing is immediate.
	const PlayerSnapshot* t = GetPuppetTarget();
	bool wasAirborne = g_airborne;
	g_airTicks = (t && !t->onGround) ? g_airTicks + 1 : 0;
	// A real jump rises fast from its first tick (~0.25/tick) : switch at once then,
	// instead of waiting AIRBORNE_MIN_TICKS (which lands near the top of the jump).
	static float prevZ = 0.0F;
	float dz = pos.z - prevZ;
	prevZ = pos.z;
	if ( g_airTicks == 0 ) g_airborne = false;
	else if ( g_airTicks >= AIRBORNE_MIN_TICKS || dz > JUMP_RISE_SPEED ) g_airborne = true;
	if ( g_airborne != wasAirborne ) {
		if ( g_airborne ) {
			g_airStartZ = pos.z;
			mp_log( "[MP] Puppet leaves the ground (z %.2f).\n", pos.z );
		}
		else {
			mp_log( "[MP] Puppet lands (z %.2f, %.2f from take-off).\n", pos.z, pos.z - g_airStartZ );
		}
	}

	bool wasSprinting = g_sprinting;
	if ( !g_moving || (g_sprinting && g_moveSpeed < SPRINT_STOP_SPEED) ) g_sprinting = false;
	else if ( !g_sprinting && g_moveSpeed > SPRINT_START_SPEED ) g_sprinting = true;
	if ( g_sprinting != wasSprinting && g_moving ) {
		mp_log( "[MP] Puppet %s (speed %.3f/tick).\n", g_sprinting ? "sprints" : "back to running", g_moveSpeed );
	}

	// The state machine always stays in a calm idle state : forcing boss-fight
	// states like "runner" crashes (they expect setup we don't do). Running is
	// done by switching animation layers instead (ApplyLocomotionAnim).
	if ( g_autoLocomotion ) {
		g_forcedState = g_idleState;
	}
}

// Beast animation layers (from the beast's own PlayAnim, 0x41A820). The animation
// controller holds one layer per state-machine state (layer = EBeastState +0x38,
// set to the state's index), then the "locomotion" layers from the beast's
// quickdata at [+0x52C] : +0 move (CRC at +0x494), +1 idle (CRC at +0x498),
// +2 turn left, +3 turn right. [+0x530] is the layer currently playing.
GET_METHOD( 0x61D0D0, void, AnimCtrl_FadeTo,  EAnimController*, int, float );
GET_METHOD( 0x61D8A0, void, AnimCtrl_Restart, EAnimController*, int );

static const uint32_t BEAST_ANIMCTRL     = 0xB4;
static const uint32_t BEAST_ANIM_MOVE    = 0x494;   // CRC.
static const uint32_t BEAST_ANIM_IDLE    = 0x498;   // CRC.
static const uint32_t BEAST_LOCO_BASE    = 0x52C;   // int.
static const uint32_t BEAST_CUR_LAYER    = 0x530;   // int.
static const uint32_t BEAST_STATE_LAYER  = 0x38;    // In EBeastState (0x58 bytes).

static int GetStateAnimLayer( EIBeast* p, int state )
{
	EBeastStateMachine* sm = p->GetStateMachine();
	if ( !sm || !sm->m_states || state < 0 || state >= (int) sm->m_numStates ) return -1;
	return FIELD( &sm->m_states[state], int, BEAST_STATE_LAYER );
}

// Crossfades to an animation layer, like the beast's PlayAnim does.
static void PlayAnimLayer( EIEvilHarryP* p, int layer )
{
	EAnimController* ctrl = &FIELD( p, EAnimController, BEAST_ANIMCTRL );
	int current = FIELD( p, int, BEAST_CUR_LAYER );
	if ( layer < 0 || layer == current ) return;

	if ( current >= 0 ) AnimCtrl_FadeTo( ctrl, current, 0.0F );
	AnimCtrl_Restart( ctrl, layer );
	AnimCtrl_FadeTo( ctrl, layer, 1.0F );
	FIELD( p, int, BEAST_CUR_LAYER ) = layer;
}

// Mirroring : the puppet plays the replayed player's own clip (Harry's jump, climb,
// etc. : Evil Harry's state clips have no ordinary jump). Two spare layers take
// turns so the old clip can fade out while the new one fades in.
static bool g_mirrorClips = true;
static const int MIRROR_LAYERS[2] = { 200, 201 };
static int g_mirrorSlot = 0;
static uint32_t g_mirrorClip = 0;

static bool ApplyMirroredClip( EIEvilHarryP* p )
{
	const PlayerSnapshot* t = GetPuppetTarget();
	if ( !g_mirrorClips || !t || !t->clipCRC ) return false;

	int current = FIELD( p, int, BEAST_CUR_LAYER );
	if ( t->clipCRC == g_mirrorClip && current == MIRROR_LAYERS[g_mirrorSlot] ) return true;

	EAnimController* ctrl = &FIELD( p, EAnimController, BEAST_ANIMCTRL );
	int next = (current == MIRROR_LAYERS[g_mirrorSlot]) ? 1 - g_mirrorSlot : g_mirrorSlot;
	AnimCtrl_SetClip( ctrl, MIRROR_LAYERS[next], t->clipCRC );
	PlayAnimLayer( p, MIRROR_LAYERS[next] );
	g_mirrorSlot = next;
	g_mirrorClip = t->clipCRC;
	return true;
}

static void ApplyLocomotionAnim( EIEvilHarryP* p )
{
	static int overridden = 0;
	if ( ApplyMirroredClip( p ) ) return;

	int wanted = g_idleLayer;
	if ( g_airborne && g_airLayer >= 0 ) wanted = g_airLayer;
	else if ( g_moving ) wanted = (g_sprinting && g_sprintLayer >= 0) ? g_sprintLayer : g_runLayer;
	int current = FIELD( p, int, BEAST_CUR_LAYER );
	if ( wanted < 0 || wanted == current ) return;

	// The idle state may re-play its own animation; count it to spot fights.
	if ( (current == g_idleLayer || current == g_runLayer || current == g_sprintLayer || current == g_airLayer) == false ) {
		if ( ++overridden <= 5 ) mp_log( "[MP] Puppet anim layer was %d, playing %d.\n", current, wanted );
	}
	PlayAnimLayer( p, wanted );
}

static void SetRunAnimation( EIEvilHarryP* p, const char* stateName )
{
	if ( !stateName ) {
		int base = FIELD( p, int, BEAST_LOCO_BASE );
		g_runLayer = FIELD( p, uint32_t, BEAST_ANIM_MOVE ) ? base : -1;
		mp_log( "[MP] Run animation : quickdata move anim, layer %d.\n", g_runLayer );
	}
	else {
		g_runLayer = GetStateAnimLayer( p, FindStateIndex( p, stateName ) );
		mp_log( "[MP] Run animation : \"%s\" state's anim, layer %d.\n", stateName, g_runLayer );
	}
}

// Standing animation. 0 = the beast's quickdata idle (locomotion layer +1); the
// "idle left/right" states' animation is the wall-climbing idle (pickaxes out).
static void SetIdleAnimation( EIEvilHarryP* p, const char* stateName )
{
	if ( !stateName ) {
		int base = FIELD( p, int, BEAST_LOCO_BASE );
		g_idleLayer = FIELD( p, uint32_t, BEAST_ANIM_IDLE ) ? base + 1 : -1;
		mp_log( "[MP] Idle animation : quickdata idle anim, layer %d.\n", g_idleLayer );
	}
	else {
		g_idleLayer = GetStateAnimLayer( p, FindStateIndex( p, stateName ) );
		mp_log( "[MP] Idle animation : \"%s\" state's anim, layer %d.\n", stateName, g_idleLayer );
	}
}

static void SetAirAnimation( EIEvilHarryP* p, const char* stateName )
{
	g_airLayer = GetStateAnimLayer( p, FindStateIndex( p, stateName ) );
	mp_log( "[MP] Air animation : \"%s\" state's anim, layer %d.\n", stateName, g_airLayer );
}

static void InitLocomotion( EIEvilHarryP* p, EIHarry* harry )
{
	ResetLocomotion();
	g_autoLocomotion = true;
	g_mirrorClip = 0;
	g_mirrorSlot = 0;
	g_idleState = FindStateIndex( p, "idle left" );
	g_idleLayer = GetStateAnimLayer( p, g_idleState );

	mp_log( "[MP] Beast anims : move CRC 0x%.8X, idle CRC 0x%.8X, locomotion base layer %d, current layer %d, idle layer %d, runner layer %d.\n",
		FIELD( p, uint32_t, BEAST_ANIM_MOVE ), FIELD( p, uint32_t, BEAST_ANIM_IDLE ),
		FIELD( p, int, BEAST_LOCO_BASE ), FIELD( p, int, BEAST_CUR_LAYER ),
		g_idleLayer, GetStateAnimLayer( p, FindStateIndex( p, "runner" ) ) );

	SetIdleAnimation( p, 0 );
	if ( g_idleLayer < 0 ) g_idleLayer = GetStateAnimLayer( p, g_idleState );

	// The quickdata move animation if the beast has one (Evil Harry doesn't), else
	// the "chase player" state's : the most natural upright run of the candidates
	// ("runner" is hunched, "superrun" is a dive).
	SetRunAnimation( p, 0 );
	if ( g_runLayer < 0 ) SetRunAnimation( p, "chase player" );

	// Above sprint speed : the "superrun to spot to ice pick" state's clip.
	g_sprintLayer = GetStateAnimLayer( p, FindStateIndex( p, "superrun to spot to ice pick" ) );
	mp_log( "[MP] Sprint animation : layer %d (above %.2f/tick).\n", g_sprintLayer, SPRINT_START_SPEED );

	// In the air (jumping, falling) : the "falling" state's clip.
	SetAirAnimation( p, "falling" );

	// Face where Harry faces. Walking showed his rigid body's row 1 (local +Y)
	// points where he moves; m_transformMatrix is turned 180 degrees from that.
	const Matrix4f& m = harry->m_rigidBodyMatrix;
	g_heading = atan2f( m.data[1][1], m.data[1][0] );
	mp_log( "[MP] Locomotion : idle state %d, heading %.0f deg.\n", g_idleState, g_heading * 180.0F / MP_PI );
}

// Writes the beast's world transform : facing g_heading, at the target position.
static void SetBeastWorld( EIEvilHarryP* p, const Vector3f& pos, const PlayerSnapshot* )
{
	Matrix4f& world = FIELD( p, Matrix4f, BEAST_WORLD );
	float a = g_heading - g_forwardLocalAngle;
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
static void PrePositionPuppet( EIEvilHarryP* p )
{
	Vector3f pos;
	if ( GetPuppetTargetPosition( &pos ) ) {
		p->EIBeast::m_position = pos;
		SetBeastWorld( p, pos, GetPuppetTarget() );
		FIELD( p, Vector3f, BEAST_PREV_POS ) = pos;
	}
}

static void DrivePuppet( EIEvilHarryP* p )
{
	Vector3f pos;
	if ( !GetPuppetTargetPosition( &pos ) ) return;

	const PlayerSnapshot* t = GetPuppetTarget();
	Vector3f rot = t->rotationEuler;

	p->EIBeast::m_position = pos;
	EICharacter_SetPos( p, &pos, true );   // Spatial structure, bounds, collision.
	EICharacter_SetRot( p, &rot );         // Rebuilds rotation + scale of the matrix.

	// The beast's own world transform (what its tick and AI work from).
	SetBeastWorld( p, pos, t );

	// Same orientation and position in the header transform.
	const Matrix4f& world = FIELD( p, Matrix4f, BEAST_WORLD );
	for ( int r = 0; r < 3; r++ )
		for ( int c = 0; c < 3; c++ )
			p->m_transformMatrix.data[r][c] = world.data[r][c];
	p->m_transformMatrix.data[3][0] = pos.x;
	p->m_transformMatrix.data[3][1] = pos.y;
	p->m_transformMatrix.data[3][2] = pos.z;
	FIELD( p, Vector3f, BEAST_PREV_POS ) = pos;
}



// --------------------------------------------------------------------------------
// Hotkeys.
// --------------------------------------------------------------------------------
// Keys are polled on our own thread (independent of the game's tick rate and of
// which window the game thinks it owns), and turned into requests the game
// thread consumes in HandleHotkeys().
static volatile LONG g_reqSpawn = 0;
static volatile LONG g_reqState = 0;
static volatile LONG g_reqDelay = 0;
static volatile LONG g_harryTicks = 0;

static bool GameHasFocus()
{
	HWND fg = GetForegroundWindow();
	if ( !fg ) return false;
	DWORD pid = 0;
	GetWindowThreadProcessId( fg, &pid );
	return pid == GetCurrentProcessId();   // Game window or its console.
}

static DWORD WINAPI HotkeyThread( void* )
{
	bool wasDown[3] = { false, false, false };
	const int keys[3] = { VK_F5, VK_F6, VK_F7 };
	volatile LONG* reqs[3] = { &g_reqSpawn, &g_reqState, &g_reqDelay };

	for ( ;; ) {
		for ( int i = 0; i < 3; i++ ) {
			bool down = (GetAsyncKeyState( keys[i] ) & 0x8000) != 0;
			if ( down && !wasDown[i] ) {
				if ( GameHasFocus() ) {
					InterlockedIncrement( reqs[i] );
					mp_log( "[MP] F%d pressed (Harry ticks so far : %ld).\n", 5 + i, g_harryTicks );
				}
				else {
					mp_log( "[MP] F%d ignored : game window not focused.\n", 5 + i );
				}
			}
			wasDown[i] = down;
		}
		Sleep( 15 );
	}
	return 0;
}

static void HandleHotkeys( EIHarry* harry )
{
	bool f5 = InterlockedExchange( &g_reqSpawn, 0 ) != 0;
	bool f6 = InterlockedExchange( &g_reqState, 0 ) != 0;
	bool f7 = InterlockedExchange( &g_reqDelay, 0 ) != 0;

	if ( f5 ) {
		SpawnPuppet( harry );
	}

	if ( f6 && g_puppet ) {
		// Only calm-looking states : forcing arbitrary ones (e.g. state 0) crashes,
		// they expect context set up by the boss fight.
		static const char* const CANDIDATES[] = { "idle left", "idle right", "rest left", "rest right" };
		static const int NUM_CANDIDATES = sizeof(CANDIDATES) / sizeof(CANDIDATES[0]);
		static int choice = -1;   // -1 = automatic (idle / run from movement).

		EBeastStateMachine* sm = g_puppet->GetStateMachine();
		choice++;
		if ( choice >= NUM_CANDIDATES ) choice = -1;

		g_autoLocomotion = (choice < 0);
		g_forcedState = (choice >= 0) ? FindStateIndex( g_puppet, CANDIDATES[choice] ) : g_idleState;

		if ( choice < 0 ) {
			mp_log( "[MP] Puppet animation : automatic (idle / run).\n" );
		}
		else if ( g_forcedState < 0 ) {
			mp_log( "[MP] Puppet state \"%s\" not found.\n", CANDIDATES[choice] );
		}
		else {
			mp_log( "[MP] Puppet forced state : %d (%s)\n", g_forcedState, sm->m_states[g_forcedState].m_name );
		}
	}

	if ( f7 ) {
		g_delayIndex = (g_delayIndex + 1) % (sizeof(DELAY_CHOICES) / sizeof(DELAY_CHOICES[0]));
		mp_log( "[MP] Replay delay : %d ticks.\n", DELAY_CHOICES[g_delayIndex] );
	}
}



// --------------------------------------------------------------------------------
// Autotest mode (environment variable PTLE_MP_AUTOTEST=1, set by
// tools\run-autotest.ps1). Presses Enter through the menus, auto-skips
// cutscenes, then runs a scripted test on the game thread, takes screenshots
// into logs\shots\ and exits the game. Normal launches are unaffected.
// --------------------------------------------------------------------------------
GET_METHOD( 0x4B55F0, void, EIHarry_Teleport, EIHarry*, Matrix4f*, bool, float );

GET_METHOD( 0x5EBA90, void, ScheduleWorldLoad, void*, uint32_t, bool );
// Level to test in, or 0 to stay in the randomizer's start area (set in
// Game\config\Randomizer\config.ini, currently Native Village).
// Turtle Monument (0x239A2165) has hostile monkeys that get in the way.
static const uint32_t AUTOTEST_LEVEL = 0;
static volatile LONG g_settled = 0;           // 1 = cutscene/book dismissed, game runs.

static bool g_autotest = false;
static bool g_skipPuppetAI = false;           // true = don't run Evil Harry's own tick (no animation).
static volatile LONG g_autotestDone = 0;      // 1 = pass, 2 = fail.
static volatile LONG g_snapPending = 0;
static volatile LONG g_holdKey = 0;           // Virtual key the autotest thread should hold...
static volatile LONG g_holdMs = 0;            // ...for this long.
static volatile LONG g_tapKey = 0;            // Virtual key the autotest thread should tap.
static char g_snapName[64];

static const DWORD AUTOTEST_MENU_TIMEOUT_MS  = 180000;
static const DWORD AUTOTEST_TOTAL_TIMEOUT_MS = 360000;

static void RequestSnapshot( const char* name )
{
	strcpy_s( g_snapName, name );
	InterlockedExchange( &g_snapPending, 1 );
}

// Grabs the game window's client area from the screen into logs\shots\<name>.bmp.
static void TakeSnapshot( const char* name )
{
	HWND wnd = *((HWND*) HWND_GAME_WINDOW);
	if ( !wnd || !g_logDir[0] ) return;

	RECT rc;
	GetClientRect( wnd, &rc );
	POINT origin = { 0, 0 };
	ClientToScreen( wnd, &origin );
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	if ( w <= 0 || h <= 0 ) return;

	HDC screen = GetDC( 0 );
	HDC mem = CreateCompatibleDC( screen );
	BITMAPINFO bi; ZeroMemory( &bi, sizeof(bi) );
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = h;   // Bottom-up, as BMP files expect.
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 24;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = 0;
	HBITMAP dib = CreateDIBSection( screen, &bi, DIB_RGB_COLORS, &bits, 0, 0 );
	HGDIOBJ old = SelectObject( mem, dib );
	BitBlt( mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT );
	GdiFlush();

	char path[MAX_PATH];
	sprintf_s( path, "%s\\shots", g_logDir );
	CreateDirectoryA( path, 0 );
	sprintf_s( path, "%s\\shots\\%s.bmp", g_logDir, name );

	DWORD stride = ((w * 3) + 3) & ~3;
	DWORD imageSize = stride * h;
	BITMAPFILEHEADER fh; ZeroMemory( &fh, sizeof(fh) );
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
	fh.bfSize = fh.bfOffBits + imageSize;

	FILE* f = 0;
	if ( fopen_s( &f, path, "wb" ) == 0 && f ) {
		fwrite( &fh, sizeof(fh), 1, f );
		fwrite( &bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f );
		fwrite( bits, imageSize, 1, f );
		fclose( f );
		mp_log( "[AT] Screenshot : shots\\%s.bmp\n", name );
	}

	SelectObject( mem, old );
	DeleteObject( dib );
	DeleteDC( mem );
	ReleaseDC( 0, screen );
}

static void SendKey( WORD vk, bool down )
{
	INPUT in; ZeroMemory( &in, sizeof(in) );
	in.type = INPUT_KEYBOARD;
	in.ki.wScan = (WORD) MapVirtualKeyA( vk, MAPVK_VK_TO_VSC );
	in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
	if ( vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
	SendInput( 1, &in, sizeof(INPUT) );
}

static void TapKey( WORD vk )
{
	INPUT in[2]; ZeroMemory( in, sizeof(in) );
	WORD scan = (WORD) MapVirtualKeyA( vk, MAPVK_VK_TO_VSC );
	in[0].type = INPUT_KEYBOARD;
	in[0].ki.wScan = scan;
	in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
	in[1] = in[0];
	in[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;

	SendInput( 1, &in[0], sizeof(INPUT) );
	Sleep( 100 );
	SendInput( 1, &in[1], sizeof(INPUT) );
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

static DWORD WINAPI AutotestThread( void* )
{
	DWORD start = GetTickCount();
	mp_log( "[AT] Autotest mode on.\n" );

	HWND wnd = 0;
	while ( !(wnd = *((HWND*) HWND_GAME_WINDOW)) ) {
		if ( GetTickCount() - start > 60000 ) {
			mp_log( "[AT] FAIL : game window never appeared.\n" );
			ExitProcess( 2 );
		}
		Sleep( 200 );
	}
	Sleep( 3000 );

	// Menus : keep pressing Enter until Harry starts ticking.
	int presses = 0;
	while ( g_harryTicks == 0 ) {
		if ( GetTickCount() - start > AUTOTEST_MENU_TIMEOUT_MS ) {
			TakeSnapshot( "fail_menu_timeout" );
			mp_log( "[AT] FAIL : never reached gameplay after %d Enter presses.\n", presses );
			ExitProcess( 2 );
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
			ExitProcess( 2 );
		}
		char name[32];
		sprintf_s( name, "settle_stalled_%d", attempt );
		TakeSnapshot( name );
		if ( GameHasFocus() ) { TapKey( VK_ESCAPE ); mp_log( "[AT] Game looks paused, pressed Esc.\n" ); }
		Sleep( 1500 );
	}
	TakeSnapshot( "settle_2_free" );
	InterlockedExchange( &g_settled, 1 );
	mp_log( "[AT] Game runs freely, starting the test script.\n" );

	// The scripted part runs on the game thread (AutotestStep). Service screenshots
	// and key holds.
	WORD heldKey = 0;
	DWORD releaseAt = 0;
	while ( g_autotestDone == 0 ) {
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
		if ( heldKey && (GetTickCount() >= releaseAt || !GameHasFocus()) ) {
			SendKey( heldKey, false );
			heldKey = 0;
		}

		if ( GetTickCount() - start > AUTOTEST_TOTAL_TIMEOUT_MS ) {
			TakeSnapshot( "fail_total_timeout" );
			mp_log( "[AT] FAIL : test script timed out.\n" );
			ExitProcess( 2 );
		}
		if ( InterlockedExchange( &g_snapPending, 0 ) ) {
			TakeSnapshot( g_snapName );
		}
		Sleep( 30 );
	}

	if ( heldKey ) SendKey( heldKey, false );
	if ( InterlockedExchange( &g_snapPending, 0 ) ) TakeSnapshot( g_snapName );
	mp_log( "[AT] %s\n", g_autotestDone == 1 ? "RESULT : PASS" : "RESULT : FAIL" );
	Sleep( 300 );
	ExitProcess( g_autotestDone == 1 ? 0 : 2 );
	return 0;
}

static void LogTelemetry( EIHarry* harry, const char* tag )
{
	if ( g_puppet ) {
		const Matrix4f& w = FIELD( g_puppet, Matrix4f, BEAST_WORLD );
		mp_log( "[AT] %s beast world (%.1f, %.1f, %.1f), anim layer %d, moving %d, speed %.3f, heading %.0f\n", tag,
			w.data[3][0], w.data[3][1], w.data[3][2], FIELD( g_puppet, int, BEAST_CUR_LAYER ),
			(int) g_moving, g_moveSpeed, g_heading * 180.0F / MP_PI );
		const Vector3f& r = FIELD( g_puppet, Vector3f, BEAST_SCALE );
		mp_log( "[AT] %s harry (%.1f, %.1f, %.1f) puppet (%.1f, %.1f, %.1f) scale (%.2f, %.2f, %.2f) matrix (%.1f, %.1f, %.1f) state %d\n", tag,
			harry->m_position.x, harry->m_position.y, harry->m_position.z,
			g_puppet->EICharacter::m_position.x, g_puppet->EICharacter::m_position.y, g_puppet->EICharacter::m_position.z,
			r.x, r.y, r.z,
			g_puppet->m_transformMatrix.data[3][0], g_puppet->m_transformMatrix.data[3][1], g_puppet->m_transformMatrix.data[3][2],
			g_puppet->m_stateIndexCurrent );
	}
	else {
		mp_log( "[AT] %s harry (%.1f, %.1f, %.1f), no puppet\n", tag, harry->m_position.x, harry->m_position.y, harry->m_position.z );
	}
}

// Runs every Harry tick on the game thread while in autotest mode.
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

	if ( g_puppet && now - lastTelemetry >= 500 ) {
		LogTelemetry( harry, "tele" );
		lastTelemetry = now;
	}

	switch ( phase )
	{
	case -1:  // Go to an open level (same call as the Gizmod "Go to" menu).
		if ( !g_settled ) { phaseStart = now; break; }
		if ( AUTOTEST_LEVEL == 0 ) {
			mp_log( "[AT] Testing in the start level 0x%.8X.\n", Gizmod::getCurrentLevelCRC() );
			phase = 0; phaseStart = now - 5000;   // Already settled : shorter wait.
			break;
		}
		if ( now - phaseStart < 3000 ) break;
		mp_log( "[AT] Going to Turtle Monument (from level 0x%.8X).\n", Gizmod::getCurrentLevelCRC() );
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
		SpawnPuppet( harry );
		if ( !g_puppet ) {
			mp_log( "[AT] FAIL : puppet did not spawn.\n" );
			InterlockedExchange( &g_autotestDone, 2 );
			return;
		}
		phase = 2; phaseStart = now; step = 0; snapped = false; circleAngle = 0.0F;   // Circle first : the walk moves the camera.
		break;

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
				SetAirAnimation( g_puppet, AIR_CANDIDATES[0] );
				phase = 3; phaseStart = now;
				break;
			}
			SetAirAnimation( g_puppet, AIR_CANDIDATES[JUMP_CLIP[step]] );
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
				SetIdleAnimation( g_puppet, 0 );
				phase = 1; phaseStart = now; step = 0; snapped = false;
				break;
			}
			mp_log( "[AT] Idle %d : %s.\n", step, IDLE_CANDIDATES[step] ? IDLE_CANDIDATES[step] : "quickdata idle anim" );
			SetIdleAnimation( g_puppet, IDLE_CANDIDATES[step] );
			step++; phaseStart = now; snapped = false;
		}
		else if ( !snapped && now - phaseStart >= 2500 ) {
			char name[40];
			sprintf_s( name, "05_idle_%d", step - 1 );
			RequestSnapshot( name );
			snapped = true;
		}
		break;

	case 3:   // Back to idle once it stops.
		if ( now - phaseStart < 3000 ) break;
		LogTelemetry( harry, "final" );
		RequestSnapshot( "05_stopped" );
		InterlockedExchange( &g_autotestDone, 1 );
		break;
	}
}



// --------------------------------------------------------------------------------
// Tick hooks. __fastcall receives "this" in ECX like __thiscall (EDX is unused),
// so these can replace the vtable entries directly.
// --------------------------------------------------------------------------------
// The game's tick code doesn't preserve every register it should : Harry's tick
// returns with EDI holding a return address (0x4B2DE6, after "call 0x547180").
// Game code never relies on EDI, but our compiled hooks may keep "self" in it, so
// "self" is kept in memory across the original call and read back afterwards.
static EIHarry* volatile g_harryTickSelf = 0;
static EIEvilHarryP* volatile g_evilTickSelf = 0;

static void __fastcall HarryTick_Hook( EIHarry* self, void* )
{
	g_harryTickSelf = self;
	g_origHarryTick( self );
	self = g_harryTickSelf;

	if ( InterlockedIncrement( &g_harryTicks ) == 1 ) {
		mp_log( "[MP] Harry tick hook is running (this 0x%.8X, global Harry 0x%.8X).\n", self, Gizmod::getHarry() );
	}

	if ( self != Gizmod::getHarry() ) return;

	PushLocalSnapshot( self );
	if ( g_autotest ) AutotestStep( self );
	HandleHotkeys( self );
}

static void __fastcall EvilHarryTick_Hook( EIEvilHarryP* self, void* )
{
	if ( self != g_puppet ) {
		g_origEvilHarryTick( self );
		return;
	}

	if ( g_skipPuppetAI ) {
		// No AI, no animation : just place it.
		DrivePuppet( self );
		return;
	}

	static int lastLoggedState = -2;
	static int lastRequested = -2;

	UpdateLocomotion( self );

	if ( g_forcedState >= 0 && self->m_stateIndexCurrent != g_forcedState ) {
		if ( lastRequested != g_forcedState ) {
			mp_log( "[MP] Requesting state %d (current %d)...\n", g_forcedState, self->m_stateIndexCurrent );
			lastRequested = g_forcedState;
		}
		self->m_forceNextState = true;
		self->m_nextState = g_forcedState;
	}

	PrePositionPuppet( self );
	g_evilTickSelf = self;
	g_origEvilHarryTick( self );   // Animations, state machine.
	self = g_evilTickSelf;         // See g_harryTickSelf.
	if ( g_autoLocomotion ) ApplyLocomotionAnim( self );

	if ( self->m_stateIndexCurrent != lastLoggedState ) {
		mp_log( "[MP] Puppet is now in state %d.\n", self->m_stateIndexCurrent );
		lastLoggedState = self->m_stateIndexCurrent;
	}

	DrivePuppet( self );           // Then override where it is.
}



// --------------------------------------------------------------------------------
// Plugin.
// --------------------------------------------------------------------------------
class MultiplayerPlugin : public GizmodPlugin, public ILoadLevelListener
{
public:

	virtual const char* getDisplayName() const { return "Multiplayer (puppet test)"; }
	virtual const char* getSystemName() const { return "Multiplayer"; }

	virtual void onEnable()
	{
		OpenLogFile();
		AddVectoredExceptionHandler( 1, CrashLogger );

		register_types();

		uint32_t harrySlot = VTABLE_HARRY + VTABLE_SLOT_TICK;
		uint32_t evilSlot  = VTABLE_EVILHARRY_P + VTABLE_SLOT_TICK;

		g_origHarryTick     = injector::ReadMemory<TickFunc>( harrySlot, true );
		g_origEvilHarryTick = injector::ReadMemory<TickFunc>( evilSlot, true );

		injector::WriteMemory( harrySlot, &HarryTick_Hook, true );
		injector::WriteMemory( evilSlot,  &EvilHarryTick_Hook, true );

		Gizmod::getInstance()->getEventListener()->registerEvent<LoadLevelEvent>( this );

		CreateThread( 0, 0, HotkeyThread, 0, 0, 0 );

		char env[8] = { 0, };
		if ( GetEnvironmentVariableA( "PTLE_MP_AUTOTEST", env, sizeof(env) ) && env[0] == '1' ) {
			g_autotest = true;
			g_puppetOffset.x = 6.0F;   // Stand beside Harry, so screenshots show whether it follows.
			EnableAutoSkipCutscenes();
			CreateThread( 0, 0, AutotestThread, 0, 0, 0 );
		}

		mp_log( "[MP] Hooked ticks (Harry 0x%.8X, Evil Harry 0x%.8X). F5 spawn, F6 state, F7 delay.\n",
			(uint32_t) g_origHarryTick, (uint32_t) g_origEvilHarryTick );
	}

	virtual void onLoadLevel( LoadLevelEvent& e )
	{
		ForgetPuppet();
	}
};

DECLARE_PLUGIN( MultiplayerPlugin );
