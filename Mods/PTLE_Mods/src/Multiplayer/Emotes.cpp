#include "Multiplayer.h"
#include "Puppets.h"
#include "Input.h"
#include "MenuPages.h"

#include "emotes_data.h"
#include "overlay.h"
#include "net/transport.h"
#include "gamemodes/HideAndSeek/HideAndSeek.h"
#include "tests.h"

#include "ptle/EIHarry.h"
#include "ptle/ERAnim.h"
#include "ptle/math/Vector3f.h"
#include "utils/func.h"

#include <Windows.h>
#include <dinput.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>


GET_METHOD( 0x61CAD0, void, EAnimController_SetClip, EAnimController*, int, uint32_t );
GET_METHOD( 0x61D8A0, void, EAnimController_Restart, EAnimController*, int );


// --------------------------------------------------------------------------------
// Emotes : Harry plays one of his own clips, the game's own animation held at no
// weight meanwhile. The others see it as they see everything Harry does (his pose
// and clip go in the snapshot). The clips are the catalogue's (emotes_data.h, made
// by make-emotes.py from the animation notes); up to 4 wheels of 8 hold the ones
// a player picked.
// --------------------------------------------------------------------------------


EmoteState g_emoteState;

EmoteWheel g_emoteWheels[MAX_WHEELS];
int g_wheelCount = 0;
int g_emoteButton = 4;               // DirectInput button held for the wheel : L1 / LB.
bool g_emoteNeedsR1 = false;         // ... with R1 / RB too.
int g_emoteKey = 0x14;               // DirectInput key code : T.
int g_wheelIndex = 0;                // The wheel up in the game (kept between openings).

int FindEmoteByName( const char* name )
{
	for ( int i = 0; name && i < NUM_EMOTES; i++ ) if ( _stricmp( EMOTES[i].name, name ) == 0 ) return i;
	return -1;
}

// In the ini : a chain's id, a clip's CRC (hex), "-" for nothing.
static int FindEmoteByCode( const char* code )
{
	if ( !code[0] || strcmp( code, "-" ) == 0 ) return -1;
	for ( int i = 0; i < NUM_EMOTES; i++ ) if ( EMOTES[i].id && _stricmp( EMOTES[i].id, code ) == 0 ) return i;
	char* end = 0;
	unsigned long crc = strtoul( code, &end, 16 );
	if ( !end || *end ) return -1;
	for ( int i = 0; i < NUM_EMOTES; i++ ) if ( !EMOTES[i].id && EMOTES[i].clip == crc ) return i;
	return -1;
}

static void EmoteCode( int e, char* out, size_t size )
{
	if ( e < 0 || e >= NUM_EMOTES ) strcpy_s( out, size, "-" );
	else if ( EMOTES[e].id ) strcpy_s( out, size, EMOTES[e].id );
	else sprintf_s( out, size, "%.8X", EMOTES[e].clip );
}

// The wheels everyone starts with (Reset puts them back; the others start empty).
void MakeDefaultWheel( int w )
{
	static const char* const PARTY[WHEEL_SLOTS] = { "Victory Dance", "Jig", "Running Man", "Breakdance", "Pickaxe Spin", "Teleport Zen", "Scratch Butt", "T-Pose" };
	static const char* const SILLY[WHEEL_SLOTS] = { "Bug Buzz", "Dizzy Walk", "Poison Gas Walk", "Death", "Death 2", "Breakdance Finish", 0, 0 };
	EmoteWheel& wheel = g_emoteWheels[w];
	if ( w == 0 )
		strcpy_s( wheel.name, "Party" );
	else if ( w == 1 )
		strcpy_s( wheel.name, "Silly" );
	else
		sprintf_s( wheel.name, "Wheel %d", w + 1 );
	for ( int s = 0; s < WHEEL_SLOTS; s++ )
		wheel.slot[s] = w == 0 ? FindEmoteByName( PARTY[s] ) : w == 1 ? FindEmoteByName( SILLY[s] ) : -1;
}

// [Emotes] Wheels=N, WheelK=Name|slot,slot,... (each : EmoteCode).
void LoadEmoteWheels( const char* ini )
{
	for ( int w = 0; w < MAX_WHEELS; w++ )
		MakeDefaultWheel( w );
	g_wheelCount = GetPrivateProfileIntA( "Emotes", "Wheels", 0, ini );
	if ( g_wheelCount <= 0 || g_wheelCount > MAX_WHEELS ) { g_wheelCount = 2; return; }
	for ( int w = 0; w < g_wheelCount; w++ ) {
		char key[16], line[512];
		sprintf_s( key, "Wheel%d", w + 1 );
		if ( !GetPrivateProfileStringA( "Emotes", key, "", line, sizeof(line), ini ) ) continue;
		char* bar = strchr( line, '|' );
		if ( !bar ) continue;
		*bar = 0;
		strncpy_s( g_emoteWheels[w].name, line, _TRUNCATE );
		char* next = bar + 1;
		for ( int s = 0; s < WHEEL_SLOTS; s++ ) {
			char* comma = next ? strchr( next, ',' ) : 0;
			if ( comma ) *comma = 0;
			g_emoteWheels[w].slot[s] = next ? FindEmoteByCode( next ) : -1;
			next = comma ? comma + 1 : 0;
		}
	}
}

void SaveEmoteWheels( const char* ini )
{
	char n[16], key[16], line[512];
	sprintf_s( n, "%d", g_wheelCount );
	WritePrivateProfileStringA( "Emotes", "Wheels", n, ini );
	for ( int w = 0; w < MAX_WHEELS; w++ ) {
		sprintf_s( key, "Wheel%d", w + 1 );
		if ( w >= g_wheelCount ) {
			WritePrivateProfileStringA( "Emotes", key, 0, ini );
			continue;
		}
		strcpy_s( line, g_emoteWheels[w].name );
		for ( char* c = line; *c; c++ ) if ( *c == '|' ) *c = '/';
		strcat_s( line, "|" );
		for ( int s = 0; s < WHEEL_SLOTS; s++ ) {
			char code[32];
			EmoteCode( g_emoteWheels[w].slot[s], code, sizeof(code) );
			if ( s ) strcat_s( line, "," );
			strcat_s( line, code );
		}
		WritePrivateProfileStringA( "Emotes", key, line, ini );
	}
}


GET_METHOD( 0x61E4F0, void, EAnimController_SetTicked, EAnimController*, void*, bool );
static const float EMOTE_MOVE = 0.5F;               // Harry got moved this far from where he started : over.
static const uint32_t EMOTE_FADE_MS = 200;          // Over : blended back into the game's own animation this long.
static const uint32_t EMOTE_SKIP_FADE_MS = 120;     // A chain's ending skipped.
static const uint32_t EMOTE_ENDING_BLEND_MS = 250;  // Into an ending that starts partway (lying there, then getting up).

static struct { bool on; int layer; uint32_t since, ms; } g_emoteFade;   // Over, blending out.
static int g_emoteRequest = -1;           // EMOTES index : the wheel's choice, played on Harry's next tick.
static const char* g_emoteCancel = 0;     // Set by the input hooks (a button, a key) : over, on the next tick.
static Vector3f g_stillAt;                // Harry standing still : where ...
static uint32_t g_stillSince = 0;         // ... and since when.

void RequestPlayEmote( int id )
{
	g_emoteRequest = id;
}

// A chain's ending playing : the input hooks keep Harry where he is.
static inline bool EmoteHoldsHarry()
{
	return g_emoteState.active && (g_emoteState.ending >= 0 || g_emoteState.finishing);
}

static float ClipSecondsLeft( AnimLayer* layer )
{
	ERAnim* anim = layer->m_anim;
	float rate = layer->m_fps * layer->m_speed;
	if ( !anim ) return 0.0F;
	if ( rate <= 0.0F ) return 1e6F;   // Stopped : not getting to its end.
	return ((float) (anim->m_frames - 1) - layer->m_frame) / rate;
}

// Ours, whatever the game does : starting one of Harry's idles, it fades every other
// layer out (ours too) and drops them.
static void KeepLayer( EAnimController* ctrl, AnimLayer* layer )
{
	layer->m_fading = 0;
	layer->m_targetWeight = 1.0F;
	if ( !layer->m_ticked ) EAnimController_SetTicked( ctrl, layer, true );
}

static void DropLayer( EIHarry* harry, int key )
{
	AnimLayer* layer = GetAnimLayer( harry, key );
	if ( !layer ) return;
	layer->m_weight = 0.0F;
	layer->m_targetWeight = 0.0F;
	layer->m_fading = 0;
	EAnimController_SetTicked( &harry->m_animController, layer, false );
}

// Ours (layer key) at w (and key2 at w2); the game's own at the weights it wants (it only
// sets them when it changes them), times what's left.
static void SetEmoteWeights( EIHarry* harry, int key, float w, int key2 = -1, float w2 = 0.0F )
{
	for ( int i = 0; i < MAX_LAYER_SCAN; i++ ) {
		AnimLayer* layer = GetAnimLayer( harry, i );
		if ( !layer ) continue;
		if ( i == key ) layer->m_weight = w;
		else if ( i == key2 ) layer->m_weight = w2;
		else if ( i == EMOTE_LAYERS[0] || i == EMOTE_LAYERS[1] ) layer->m_weight = 0.0F;
		else layer->m_weight = layer->m_targetWeight * (1.0F - w - w2);
	}
}

static void EndFade( EIHarry* harry )
{
	if ( !g_emoteFade.on ) return;
	g_emoteFade.on = false;
	SetEmoteWeights( harry, -1, 0.0F );
	DropLayer( harry, EMOTE_LAYERS[0] );
	DropLayer( harry, EMOTE_LAYERS[1] );
}

// The next clip, from frame `from`; blendMs : the one before blends out meanwhile (else it's gone at once).
static void BeginEmoteAnim( uint32_t clip, bool loop, float from = 0.0F, uint32_t blendMs = 0 )
{
	EmoteState& s = g_emoteState;

	s.clip = clip;
	s.loop = loop;
	s.blendFrom = (blendMs && s.started) ? EMOTE_LAYERS[s.slot] : -1;
	s.blendMs = blendMs;
	s.slot = 1 - s.slot;
	s.started = false;
	s.startFrame = from;
	s.lastFrame = -1.0F;
}

static bool HasHold( const EmoteDef& d )
{
	return d.hold[0] != 0.0F || d.hold[1] != 0.0F;
}

static void StartEmote( EIHarry* harry, int e )
{
	EmoteState& s = g_emoteState;

	const EmoteDef& d = EMOTES[e];
	EndFade( harry );
	s.active = true;
	s.emote = e;
	s.plays = 0;
	s.ending = -1;
	s.holding = s.finishing = false;
	s.start = NetNowMs();
	s.where = harry->m_position;
	s.health = harry->m_healthValue;
	g_emoteCancel = 0;
	BeginEmoteAnim( d.intro ? d.intro : d.clip, d.intro ? false : d.loops );
	mp_log( "[EM] Emote : %s (clip 0x%.8X%s%s).\n", d.name, d.clip, d.intro ? ", after its intro" : "", d.loops ? ", loops" : "" );
}

// Over : blended out (fadeMs 0 : at once).
static void StopEmote( EIHarry* harry, const char* why, uint32_t fadeMs = EMOTE_FADE_MS )
{
	EmoteState& s = g_emoteState;

	if ( !s.active ) return;
	s.active = false;

	if ( harry ) {
		EndFade( harry );
		// The layer showing : a clip just begun hasn't loaded yet, the one before it shows.
		g_emoteFade.layer = EMOTE_LAYERS[s.started ? s.slot : 1 - s.slot];
		g_emoteFade.since = NetNowMs();
		g_emoteFade.ms = fadeMs;
		g_emoteFade.on = true;
		AnimLayer* layer = GetAnimLayer( harry, g_emoteFade.layer );
		if ( !fadeMs || !layer || layer->m_weight <= 0.0F ) EndFade( harry );
	}
	mp_log( "[EM] Emote ended (%s) after %u ms, %d play%s.\n", why, NetNowMs() - s.start, s.plays, s.plays == 1 ? "" : "s" );
}

// Why Harry can't emote now, 0 if he can.
static const char* WhyNoEmote( EIHarry* harry )
{
	if ( harry->m_healthValue <= 0 ) return "You can't emote now";
	if ( GetHeldItem(harry) >= 0 ) return "Put your item away to emote";
	if ( !harry->m_onGround ) return "Land first to emote";
	if ( NetNowMs() - g_stillSince < 150 ) return "Stand still to emote";
	return 0;
}

void EmoteTick( EIHarry* harry )
{
	const uint32_t now = NetNowMs();
	EAnimController* ctrl = &harry->m_animController;
	Vector3f at = harry->m_position;
	float dx = at.x - g_stillAt.x, dy = at.y - g_stillAt.y, dz = at.z - g_stillAt.z;
	if ( !g_stillSince || dx * dx + dy * dy + dz * dz > 0.05F * 0.05F ) { g_stillAt = at; g_stillSince = now; }

	EmoteState& s = g_emoteState;

	// The last one blending out.
	if ( g_emoteFade.on ) {
		AnimLayer* layer = GetAnimLayer( harry, g_emoteFade.layer );
		uint32_t gone = now - g_emoteFade.since;
		if ( !layer || gone >= g_emoteFade.ms ) EndFade( harry );
		else {
			KeepLayer( ctrl, layer );
			SetEmoteWeights( harry, g_emoteFade.layer, 1.0F - (float) gone / (float) g_emoteFade.ms );
		}
	}

	// Catch requested emote, reset it and play its clip.
	if ( g_emoteRequest >= 0 ) {
		int e = g_emoteRequest;
		g_emoteRequest = -1;
		const char* why = WhyNoEmote( harry );
		if ( why ) {
			mp_log( "[EM] No %s : %s.\n", EMOTES[e].name, why );
			Toast( "%s", why );
		}
		else {
			if ( s.active ) StopEmote( harry, "another one", 0 );
			StartEmote( harry, e );
		}
	}
	if ( !s.active ) return;
	const EmoteDef& d = EMOTES[s.emote];

	// Over : a button or a key, Harry moved (pushed, fell), an item came out, he got hurt.
	const char* over = g_emoteCancel;
	bool byYou = over != 0;
	g_emoteCancel = 0;
	dx = at.x - s.where.x; dy = at.y - s.where.y; dz = at.z - s.where.z;
	if ( !over && dx * dx + dy * dy + dz * dz > EMOTE_MOVE * EMOTE_MOVE ) over = "moved";
	if ( !over && !harry->m_onGround ) over = "off the ground";
	if ( !over && GetHeldItem( harry ) >= 0 ) over = "an item";
	if ( !over && harry->m_healthValue < s.health ) over = "hurt";
	if ( over ) {
		bool held = EmoteHoldsHarry();
		// A chain you end, or a hold you leave : its ending first (another press skips it) ...
		if ( byYou && !held && d.ending[0] && (d.intro || s.holding) ) {
			mp_log( "[EM] %s : its ending (%s).\n", d.name, over );
			s.ending = 0;
			BeginEmoteAnim( d.ending[0], false, d.endingFrom, d.endingFrom > 0.0F ? EMOTE_ENDING_BLEND_MS : 0 );
		}
		// ... or the rest of its clip (none after a hold on its last frame : it fades out).
		else if ( byYou && !held && s.holding && !(d.hold[0] < 0.0F && d.hold[1] < 0.0F) ) {
			mp_log( "[EM] %s : finishing (%s).\n", d.name, over );
			s.finishing = true;
			AnimLayer* mine = GetAnimLayer( harry, EMOTE_LAYERS[s.slot] );
			if ( mine ) mine->m_speed = 1.0F;
		}
		else {
			StopEmote( harry, over, held && byYou ? EMOTE_SKIP_FADE_MS : EMOTE_FADE_MS );
			return;
		}
	}

	if ( s.started ) {
		AnimLayer* mine = GetAnimLayer( harry, EMOTE_LAYERS[s.slot] );
		if ( mine ) {
			float frame = mine->m_frame;
			if ( HasHold( d ) && s.clip == d.clip && s.ending < 0 && !s.finishing ) {
				// Its hold : from its first frame on, it stays (between the two frames, again and again).
				ERAnim* clip = mine->m_anim;
				float last = clip ? (float) (clip->m_frames - 1) : 0.0F;
				float a = d.hold[0] < 0.0F ? last : d.hold[0], b = d.hold[1] < 0.0F ? last : d.hold[1];
				if ( frame >= a ) {
					if ( !s.holding ) { s.holding = true; s.holdSince = now; mp_log( "[EM] %s : held (frame %.0f).\n", d.name, frame ); }
					if ( b > a ) {
						// There and back (a clip has no loop of its own : jumping back shows), easing in and out
						// at each end (at its fastest, as fast as the clip plays).
						float fps = mine->m_fps;
						float half = fps > 0.0F ? (b - a) / fps * 1.5707963F : 1.0F;   // Seconds, a to b.
						float phase = (float) (now - s.holdSince) / 1000.0F / half;
						mine->m_speed = 0.0F;
						mine->m_frame = frame = a + (b - a) * 0.5F * (1.0F - cosf( 3.1415927F * phase ));
						s.plays = 1 + (int) (phase / 2.0F);
					}
					else if ( a < last ) { mine->m_speed = 0.0F; mine->m_frame = frame = a; }
				}
			}
			else if ( s.loop ) {   // The game loops it : count the plays.
				if ( frame < s.lastFrame ) s.plays++;
			}
			else {
				bool intro = d.intro && s.clip == d.intro;
				bool more = intro || (s.ending == 0 && d.ending[1]);
				float left = ClipSecondsLeft( mine );
				if ( !more && left <= EMOTE_FADE_MS / 1000.0F ) {   // Its last clip : blended out as it finishes.
					StopEmote( harry, "played", left > 0.05F ? (uint32_t) (left * 1000.0F) : 50 );
					return;
				}
				if ( mine->m_ended ) {   // The next clip, right away.
					if ( intro ) BeginEmoteAnim( d.clip, true );
					else if ( s.ending == 0 ) { s.ending = 1; BeginEmoteAnim( d.ending[1], false ); }
				}
			}
			s.lastFrame = frame;
		}
	}
	int key = EMOTE_LAYERS[s.slot];
	if ( !s.started ) {
		EAnimController_SetClip( ctrl, key, s.clip );
		EAnimController_Restart( ctrl, key );
		AnimLayer* layer = GetAnimLayer( harry, key );
		if ( layer ) {
			layer->m_atEnd = !s.loop;
			layer->m_speed = 1.0F;
			if ( s.startFrame > 0.0F ) layer->m_frame = s.startFrame;
		}
		if ( s.blendFrom >= 0 ) s.blendSince = now;
		else DropLayer( harry, EMOTE_LAYERS[1 - s.slot] );
		s.started = true;
		s.plays++;
	}
	AnimLayer* mine = GetAnimLayer( harry, key );
	if ( !mine || GetLayerClipCRC( mine ) != s.clip ) { StopEmote( harry, "the clip didn't load", 0 ); return; }
	KeepLayer( ctrl, mine );
	// Blending from the clip before (its last frame held) into this one.
	float w = 1.0F;
	AnimLayer* from = s.blendFrom >= 0 ? GetAnimLayer( harry, s.blendFrom ) : 0;
	if ( from && now - s.blendSince < s.blendMs ) {
		KeepLayer( ctrl, from );
		w = (float) (now - s.blendSince) / (float) s.blendMs;
	}
	else if ( s.blendFrom >= 0 ) {
		DropLayer( harry, s.blendFrom );
		s.blendFrom = -1;
	}
	SetEmoteWeights( harry, key, w, s.blendFrom, 1.0F - w );
}

// --------------------------------------------------------------------------------
// The emote wheel : held open (L1, or T), pointed at with the right stick (the
// mouse or the arrows), played when let go (1-8 and a click play at once). R1 (Tab) :
// the next wheel. While it's open the game doesn't get the right stick and R1 (the
// arrows, Tab, 1-8). While Harry emotes, any other button or key, or the left stick,
// ends it (a chain plays its ending first).
// --------------------------------------------------------------------------------

static bool IsCameraButton( int b )
{
	const int* bind = (const int*) PAD_BINDINGS;
	return bind[14] == b + 11 || bind[15] == b + 11;
}

static const uint32_t WHEEL_SHOW_MS = 150;   // Held this long (or pointed) : the wheel shows (a tap of L1 doesn't).
static const uint32_t WHEEL_KEEP_MS = 300;   // The stick back in the middle : the choice stays this long.

WheelUi g_wheelUi;

bool EmoteWheelShown()
{
	return g_wheelUi.open && !g_wheelUi.done && (g_wheelUi.shown || NetNowMs() - g_wheelUi.since >= WHEEL_SHOW_MS);
}

void EmoteWheelOpen( const DIJOYSTATE2* pad )
{
	memset( &g_wheelUi, 0, sizeof(g_wheelUi) );
	g_wheelUi.open = true;
	g_wheelUi.byPad = pad != 0;
	g_wheelUi.since = NetNowMs();
	g_wheelUi.hover = -1;
	if ( g_wheelIndex >= g_wheelCount ) g_wheelIndex = 0;
	if ( pad ) {
		for ( int i = 0; i < 8; i++ ) g_wheelUi.centre[i] = ((const LONG*) pad)[i];
		g_wheelUi.r1Was = (pad->rgbButtons[PAD_R1] & 0x80) != 0;
		mp_log( "[EM] Wheel opened (controller; axes %ld %ld %ld %ld %ld %ld).\n", g_wheelUi.centre[0], g_wheelUi.centre[1], g_wheelUi.centre[2],
			g_wheelUi.centre[3], g_wheelUi.centre[4], g_wheelUi.centre[5] );
	}
	else {
		GetCursorPos( &g_wheelUi.mouse );
		mp_log( "[EM] Wheel opened (keyboard).\n" );
	}
}

static void WheelPoint( int slot )
{
	g_wheelUi.hover = slot;
	g_wheelUi.hoverAt = NetNowMs();
	g_wheelUi.shown = true;
}

static void WheelNext()
{
	if ( g_wheelCount > 1 ) g_wheelIndex = (g_wheelIndex + 1) % g_wheelCount;
	g_wheelUi.hover = -1;
	g_wheelUi.shown = true;
	mp_log( "[EM] Wheel : %s.\n", g_emoteWheels[g_wheelIndex].name );
}

static void WheelPlay( int slot, const char* how )
{
	g_wheelUi.done = true;
	if ( slot < 0 ) return;
	int e = g_emoteWheels[g_wheelIndex].slot[slot];
	mp_log( "[EM] Wheel %s, slot %d (%s) : %s.\n", g_emoteWheels[g_wheelIndex].name, slot + 1, how, e >= 0 ? EMOTES[e].name : "empty" );
	if ( e >= 0 ) g_emoteRequest = e;
}

// The slot a direction points at (dy : down), the first at the top, clockwise.
static int WedgeAt( float dx, float dy )
{
	float a = atan2f( dx, -dy );
	if ( a < 0.0F ) a += 6.2831853F;
	return ((int) floorf( (a + 0.3926991F) / 0.7853982F )) % WHEEL_SLOTS;
}

void Emote_Gamepad( DIJOYSTATE2* state )
{
	LONG* axes = (LONG*) state;
	uint8_t* buttons = state->rgbButtons;
	static bool big[8] = { false, };   // The axis goes 0 .. 65535 (or so), not -1000 .. 1000.
	for ( int i = 0; i < 8; i++ ) if ( axes[i] > 1100 || axes[i] < -1100 ) big[i] = true;
	DWORD hat = state->rgdwPOV[0];
	bool hatCentred = LOWORD( hat ) == 0xFFFF;
	bool held = g_emoteButton >= 0 && g_emoteButton < 32 && (buttons[g_emoteButton] & 0x80);
	bool r1 = (buttons[PAD_R1] & 0x80) != 0;
	bool combo = held && (!g_emoteNeedsR1 || r1);
	static bool comboWas = false;
	static uint8_t before[32];
	static DWORD hatBefore = 0xFFFFFFFF;
	uint8_t raw[32];
	memcpy( raw, buttons, sizeof(raw) );

	if ( combo && !comboWas && !g_wheelUi.open ) EmoteWheelOpen( state );
	comboWas = combo;
	if ( g_wheelUi.open && g_wheelUi.byPad ) {
		if ( !held ) {
			if ( !g_wheelUi.done ) WheelPlay( g_wheelUi.hover, "let go" );
			g_wheelUi.open = false;
		}
		else {
			if ( r1 && !g_wheelUi.r1Was && !g_wheelUi.done ) WheelNext();
			g_wheelUi.r1Was = r1;
			// The right stick : a DualShock's (Z, Rz) or an Xbox controller's (Rx, Ry), whichever leans more.
			static const int PAIRS[2][2] = { { 2, 5 }, { 3, 4 } };
			float best = 0.0F, bx = 0.0F, by = 0.0F;
			for ( int p = 0; p < 2; p++ ) {
				int ix = PAIRS[p][0], iy = PAIRS[p][1];
				float x = (axes[ix] - g_wheelUi.centre[ix]) / (big[ix] ? 32767.0F : 1000.0F);
				float y = (axes[iy] - g_wheelUi.centre[iy]) / (big[iy] ? 32767.0F : 1000.0F);
				if ( x * x + y * y > best ) { best = x * x + y * y; bx = x; by = y; }
			}
			if ( !g_wheelUi.done ) {
				if ( best > 0.5F * 0.5F ) WheelPoint( WedgeAt( bx, by ) );
				else if ( g_wheelUi.hover >= 0 && NetNowMs() - g_wheelUi.hoverAt > WHEEL_KEEP_MS ) g_wheelUi.hover = -1;
			}
			// Hide and seek's role swap is the same button and the d-pad : no wheel then.
			if ( PlayingHs() && g_emoteButton == g_swapButton && !hatCentred ) g_wheelUi.done = true;
			for ( int i = 2; i < 8; i++ ) axes[i] = g_wheelUi.centre[i];   // The right stick : Z .. Rz, or a slider (the game's options).
			buttons[PAD_R1] = 0;
		}
	}
	// Emoting : the left stick, another button or the d-pad end it. A chain's ending
	// keeps Harry where he is (a new press skips it, and gets to the game next time).
	else if ( g_emoteState.active ) {
		static uint32_t restFor = 0;
		static LONG rest[2];
		if ( restFor != g_emoteState.start ) { restFor = g_emoteState.start; rest[0] = axes[0]; rest[1] = axes[1]; }
		float x = (axes[0] - rest[0]) / (big[0] ? 32767.0F : 1000.0F), y = (axes[1] - rest[1]) / (big[1] ? 32767.0F : 1000.0F);
		bool hold = EmoteHoldsHarry();
		const char* why = 0;
		if ( !hold && x * x + y * y > 0.4F * 0.4F ) why = "the left stick";
		for ( int b = 0; b < 32 && !why; b++ )
			if ( (raw[b] & 0x80) && !(before[b] & 0x80) && b != g_emoteButton && b != PAD_R1 && !IsCameraButton( b ) ) why = "a button";
		if ( !why && !hatCentred && LOWORD( hatBefore ) == 0xFFFF ) why = "the d-pad";
		if ( why && !g_emoteCancel ) g_emoteCancel = why;
		if ( hold ) {   // (The camera still turns.)
			axes[0] = rest[0]; axes[1] = rest[1];
			for ( int b = 0; b < 32; b++ ) if ( !IsCameraButton( b ) ) buttons[b] = 0;
			state->rgdwPOV[0] = 0xFFFFFFFF;
		}
	}
	// The right stick reaches with Harry's arm (and aims some items) : not while it points
	// the wheel, nor after, until it's back in the middle.
	static bool stickHeld = false;
	static LONG stickCentre[8];
	if ( g_wheelUi.open && g_wheelUi.byPad ) { stickHeld = true; memcpy( stickCentre, g_wheelUi.centre, sizeof(stickCentre) ); }
	else if ( stickHeld ) {
		bool centred = true;
		for ( int i = 2; i < 8; i++ ) if ( fabsf( (axes[i] - stickCentre[i]) / (big[i] ? 32767.0F : 1000.0F) ) > 0.25F ) centred = false;
		if ( centred ) stickHeld = false;
		else for ( int i = 2; i < 8; i++ ) axes[i] = stickCentre[i];
	}
	memcpy( before, raw, sizeof(before) );
	hatBefore = hat;
}

// Keys that don't end an emote : the wheel's, the camera's, the right stick's (it reaches
// behind the emote), the function keys, Shift, Ctrl, Alt.
static bool KeyKeepsEmote( int k )
{
	const int* bind = (const int*) KEY_BINDINGS;
	for ( int i = 4; i <= 7; i++ ) if ( bind[i] == k ) return true;
	if ( bind[14] == k || bind[15] == k ) return true;
	if ( k == g_emoteKey || k == 0x0F || (k >= 0x02 && k <= 0x09) || (k >= 0x3B && k <= 0x44) || k == 0x57 || k == 0x58 ) return true;
	return k == 0x2A || k == 0x36 || k == 0x1D || k == 0x9D || k == 0x38 || k == 0xB8;
}

void Emote_Keyboard( uint8_t* keys )
{
	static uint8_t before[256];
	uint8_t raw[256];
	memcpy( raw, keys, sizeof(raw) );
	bool held = g_emoteKey > 0 && g_emoteKey < 256 && (raw[g_emoteKey] & 0x80);
	static bool heldWas = false;
	if ( held && !heldWas && !g_wheelUi.open ) EmoteWheelOpen( 0 );
	heldWas = held;
	if ( g_wheelUi.open && !g_wheelUi.byPad ) {
		if ( !held ) {
			if ( !g_wheelUi.done ) WheelPlay( g_wheelUi.hover, "let go" );
			g_wheelUi.open = false;
		}
		else if ( !g_wheelUi.done ) {
			if ( (raw[0x0F] & 0x80) && !(before[0x0F] & 0x80) ) WheelNext();   // Tab.
			// The arrows (the keyboard's right stick) : 8 ways. Letting go of a diagonal keeps it.
			int ax = ((raw[0xCD] & 0x80) ? 1 : 0) - ((raw[0xCB] & 0x80) ? 1 : 0), ay = ((raw[0xD0] & 0x80) ? 1 : 0) - ((raw[0xC8] & 0x80) ? 1 : 0);
			int arrows = 0, arrowsBefore = 0;
			for ( int i = 0; i < 4; i++ ) { arrows += (raw[DIK_ARROWS[i]] & 0x80) ? 1 : 0; arrowsBefore += (before[DIK_ARROWS[i]] & 0x80) ? 1 : 0; }
			if ( (ax || ay) && arrows >= arrowsBefore ) WheelPoint( WedgeAt( (float) ax, (float) ay ) );
			for ( int i = 0; i < WHEEL_SLOTS && !g_wheelUi.done; i++ )   // 1 - 8.
				if ( (raw[0x02 + i] & 0x80) && !(before[0x02 + i] & 0x80) ) { WheelPoint( i ); WheelPlay( i, "number key" ); }
			// The mouse : which way it went (the start follows it, so turning back is quick).
			POINT p;
			if ( !g_wheelUi.done && !TestInBackground() && GameHasFocus() && GetCursorPos(&p) ) {
				float dx = (float) (p.x - g_wheelUi.mouse.x), dy = (float) (p.y - g_wheelUi.mouse.y);
				float d = sqrtf( dx * dx + dy * dy );
				if ( d > 30.0F ) WheelPoint( WedgeAt( dx, dy ) );
				if ( d > 80.0F ) { g_wheelUi.mouse.x = p.x - (LONG) (dx * 80.0F / d); g_wheelUi.mouse.y = p.y - (LONG) (dy * 80.0F / d); }
				bool click = (GetAsyncKeyState( VK_LBUTTON ) & 0x8000) != 0;
				if ( click && !g_wheelUi.clickWas && g_wheelUi.hover >= 0 ) WheelPlay( g_wheelUi.hover, "click" );
				g_wheelUi.clickWas = click;
			}
		}
		keys[0x0F] = 0;
		for ( int i = 0; i < WHEEL_SLOTS; i++ ) keys[0x02 + i] = 0;
	}
	else if ( g_emoteState.active ) {
		for ( int k = 1; k < 256; k++ )
			if ( (raw[k] & 0x80) && !(before[k] & 0x80) && !KeyKeepsEmote( k ) ) { if ( !g_emoteCancel ) g_emoteCancel = "a key"; break; }
		if ( EmoteHoldsHarry() ) for ( int k = 1; k < 256; k++ ) if ( !KeyKeepsEmote( k ) ) keys[k] = 0;
	}
	// The arrows (and the game's right stick keys) : not for the game while the wheel is out, nor
	// after, until they're let go. Whichever opened it : a controller's right stick can come as
	// the arrows too (JoyToKey and the like).
	int stickKeys[8];
	for ( int i = 0; i < 4; i++ ) { stickKeys[i] = DIK_ARROWS[i]; stickKeys[4 + i] = ((const int*) KEY_BINDINGS)[4 + i]; }
	static bool arrowsHeld = false;
	bool anyArrow = false;
	for ( int i = 0; i < 8; i++ ) if ( stickKeys[i] > 0 && stickKeys[i] < 256 ) anyArrow |= (raw[stickKeys[i]] & 0x80) != 0;
	if ( g_wheelUi.open ) arrowsHeld = true;
	else if ( !anyArrow ) arrowsHeld = false;
	if ( arrowsHeld ) for ( int i = 0; i < 8; i++ ) if ( stickKeys[i] > 0 && stickKeys[i] < 256 ) keys[stickKeys[i]] = 0;
	if ( held ) keys[g_emoteKey] = 0;
	memcpy( before, raw, sizeof(before) );
}

// An emote's name in its wedge : two lines when it doesn't fit on one.
static void DrawWheelLabel( float x, float y, const char* name, float maxW, float lh, uint32_t color )
{
	float w = OverlayTextWidth( name );
	int split = -1;
	float bestW = w;
	char first[64];
	for ( const char* c = name; w > maxW && *c; c++ ) {
		int n = (int) (c - name);
		if ( *c != ' ' || n >= (int) sizeof(first) ) continue;
		memcpy( first, name, n ); first[n] = 0;
		float m = max( OverlayTextWidth( first ), OverlayTextWidth( c + 1 ) );
		if ( m < bestW ) { bestW = m; split = n; }
	}
	if ( split < 0 ) { OverlayText( floorf( x - w * 0.5F ), floorf( y - lh * 0.5F ), name, color ); return; }
	memcpy( first, name, split ); first[split] = 0;
	const char* second = name + split + 1;
	OverlayText( floorf( x - OverlayTextWidth( first ) * 0.5F ), floorf( y - lh ), first, color );
	OverlayText( floorf( x - OverlayTextWidth( second ) * 0.5F ), floorf( y ), second, color );
}

// A ring's piece, angles from the top, clockwise.
static void WheelSector( float cx, float cy, float r0, float r1, float a0, float a1, uint32_t color )
{
	const int N = 10;
	for ( int k = 0; k < N; k++ ) {
		float b0 = a0 + (a1 - a0) * k / N, b1 = a0 + (a1 - a0) * (k + 1) / N;
		float s0 = sinf( b0 ), c0 = cosf( b0 ), s1 = sinf( b1 ), c1 = cosf( b1 );
		OverlayTriangle( cx + s0 * r0, cy - c0 * r0, cx + s0 * r1, cy - c0 * r1, cx + s1 * r1, cy - c1 * r1, color );
		OverlayTriangle( cx + s0 * r0, cy - c0 * r0, cx + s1 * r1, cy - c1 * r1, cx + s1 * r0, cy - c1 * r0, color );
	}
}

// The emote wheel, middle of the screen : 8 wedges round the wheel's name.
void DrawWheel( float sw, float sh, float h )
{
	const float PI = 3.14159265F, STEP = PI / 4.0F;
	const EmoteWheel& wheel = g_emoteWheels[g_wheelIndex];
	float cx = floorf( sw * 0.5F ), cy = floorf( sh * 0.47F );
	float outer = floorf( sh * 0.3F ), inner = floorf( outer * 0.36F ), mid = (inner + outer) * 0.5F;
	float lh = h * 1.15F;
	float gap = (3.0F / mid) * 0.5F;   // Half the angle between two wedges (3 pixels, midway).
	for ( int q = 0; q < 4; q++ ) WheelSector( cx, cy, 0.0F, inner - 3.0F, q * PI * 0.5F, (q + 1) * PI * 0.5F, 0xC0000000 );
	for ( int s = 0; s < WHEEL_SLOTS; s++ ) {
		bool on = s == g_wheelUi.hover;
		int e = wheel.slot[s];
		float a0 = (s - 0.5F) * STEP + gap, a1 = (s + 0.5F) * STEP - gap, ac = s * STEP;
		WheelSector( cx, cy, inner, outer, a0, a1, on ? 0xE0503C0C : 0xC0000000 );
		if ( on ) WheelSector( cx, cy, outer + 3.0F, outer + 7.0F, a0, a1, 0xFFFFE080 );
		float maxW = (s == 2 || s == 6) ? (outer - inner) * 0.9F : 2.0F * mid * sinf( STEP * 0.5F ) * 0.9F;
		DrawWheelLabel( cx + sinf( ac ) * mid, cy - cosf( ac ) * mid, e >= 0 ? EMOTES[e].name : "Empty", maxW, lh,
			e < 0 ? 0xFF808080 : on ? 0xFFFFE080 : 0xFFFFFFFF );
		if ( !g_wheelUi.byPad ) {   // The number keys.
			char n[4];
			sprintf_s( n, "%d", s + 1 );
			float r = outer + h * 1.1F;
			OverlayText( floorf( cx + sinf( ac ) * r - OverlayTextWidth( n ) * 0.5F ), floorf( cy - cosf( ac ) * r - h * 0.5F ), n, 0xFFB0B0B0 );
		}
	}
	// The middle : the wheel's name (and which of how many).
	char count[16];
	sprintf_s( count, "%d / %d", g_wheelIndex + 1, g_wheelCount );
	float ny = g_wheelCount > 1 ? cy - lh : cy - h * 0.5F;
	OverlayText( floorf( cx - OverlayTextWidth( wheel.name ) * 0.5F ), floorf( ny ), wheel.name, 0xFFFFE080 );
	if ( g_wheelCount > 1 ) OverlayText( floorf( cx - OverlayTextWidth( count ) * 0.5F ), floorf( ny + lh ), count, 0xFFB0B0B0 );
	// What to do, under it.
	char hint[160];
	if ( g_wheelUi.byPad ) {
		char button[24];
		if ( g_emoteButton == 4 ) strcpy_s( button, "L1" );
		else sprintf_s( button, "button %d", g_emoteButton + 1 );
		sprintf_s( hint, "Right stick : choose   Let go of %s : play%s", button, g_wheelCount > 1 ? (g_emoteNeedsR1 ? "   R1 again : next wheel" : "   R1 : next wheel") : "" );
	}
	else sprintf_s( hint, "Mouse, arrows or 1 - 8 : choose   Let go of %s : play%s", KeyName( g_emoteKey ), g_wheelCount > 1 ? "   Tab : next wheel" : "" );
	float hw = OverlayTextWidth( hint ), hy = floorf( cy + outer + h * 2.0F );
	float pad = h * 0.3F;
	OverlayRect( floorf( cx - hw * 0.5F ) - pad, hy - pad * 0.5F, hw + 2.0F * pad, h + pad, 0xA0000000 );
	OverlayText( floorf( cx - hw * 0.5F ), hy, hint, 0xFFD8D8D8 );
}
