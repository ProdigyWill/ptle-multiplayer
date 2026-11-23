#pragma once


struct DIJOYSTATE2;


static const int MAX_WHEELS = 4, WHEEL_SLOTS = 8, WHEEL_NAME_SIZE = 24;

struct EmoteDef
{
	const char* id;        // Chains : their name in the ini (the others go by their clip).
	const char* name;
	int category;          // EMOTE_CATEGORIES.
	bool loops;            // Plays again until Harry moves.
	bool favourite;
	uint32_t intro;        // Chains : played once first, then the clip loops.
	uint32_t clip;
	uint32_t ending[2];    // Played (one, then the other) when you move : a chain's, or getting up from a hold.
	float hold[2];         // Harry stays between these frames of the clip (there and back, slowing at each end; the
	// same : on that frame; -1 : the last) until you move. 0, 0 : no hold.
	float endingFrom;      // The ending's first frame.
};

struct EmoteWheel
{
	char name[WHEEL_NAME_SIZE];
	int slot[WHEEL_SLOTS];   // EMOTES index, -1 : empty. Clockwise from the top.
};

struct EmoteState
{
	bool active;
	int emote;            // EMOTES index.
	uint32_t clip;        // Playing now (a chain's intro, then its clip, then its ending).
	int slot;             // Which of EMOTE_LAYERS.
	bool started;
	bool holding;         // At its hold (Teleport Zen floating, Death lying there) ...
	bool finishing;       // Let go of its hold, the rest of the clip playing (no ending). Harry stays put meanwhile.
	bool loop;
	int ending;           // Its ending : which of its clips plays (-1 : not yet). Harry stays put meanwhile.
	uint32_t holdSince;   // ... since then.
	float startFrame;     // The clip begins here.
	int blendFrom;        // Layer blended out as the clip comes in (-1 : none) ...
	uint32_t blendSince, blendMs;   // ... from then, this long.
	float lastFrame;      // The layer's frame last time (a loop's goes back to the start : another play).
	int plays;
	uint32_t start;       // NetNowMs.
	Vector3f where;       // Harry, at the start.
	int health;
};


void LoadEmoteWheels( const char* ini );
void SaveEmoteWheels( const char* ini );

bool EmoteWheelShown();
void EmoteWheelOpen( const DIJOYSTATE2* pad );

void RequestPlayEmote( int id );


// Emotes.cpp's.

extern int g_wheelCount;
extern int g_emoteButton;   // DirectInput button held for the wheel : L1 / LB.
extern bool g_emoteNeedsR1;   // ... with R1 / RB too.
extern int g_emoteKey;   // DirectInput key code : T.
extern int g_wheelIndex;   // The wheel up in the game (kept between openings).

void MakeDefaultWheel( int w );

static const int EMOTE_LAYERS[2] = { 254, 255 };   // Spare layers of Harry's (his own go up to 253), taking turns.

void EmoteTick( EIHarry* harry );

static const int PAD_R1 = 5;
static const int DIK_ARROWS[4] = { 0xC8, 0xD0, 0xCB, 0xCD };   // DirectInput key codes : up, down, left, right.

// The game's controls (its options, kept in the registry) : 20 per controller (0x941020,
// 0x2C apart; a button is its number + 11) and for the keyboard (0x940FC0, DIK codes).
// 4 - 7 : the right stick (reaching), 14 - 15 : turning the camera.
static const uint32_t PAD_BINDINGS = 0x941020, KEY_BINDINGS = 0x940FC0;

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

extern WheelUi g_wheelUi;

void Emote_Gamepad( DIJOYSTATE2* state );
void Emote_Keyboard( uint8_t* keys );
void DrawWheel( float sw, float sh, float h );


#ifdef PTLE_MP_EMBEDTESTS   // For the tests (tests.cpp).

int FindEmoteByName( const char* name );

#endif
