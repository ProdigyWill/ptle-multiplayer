#pragma once

#include "Multiplayer.h"

#include <d3d9.h>


// Rendering : the camera, the name tags, the toasts, screen grabs and the overlay drawn on each frame.


static const uint32_t RENDERER_DEVICE = 0x926790 + 0x74;

static const uint32_t CAMERA_LOOKAT_CALLS[] = {
	0x449B9A, 0x449BC4, 0x456169, 0x457065, 0x457783, 0x457A58, 0x4582BE, 0x4584E7, 0x459A08, 0x45AC65,
	0x45B30C, 0x45B6E7, 0x45B774, 0x45C9F2, 0x45D6B1, 0x45D956, 0x45E46D, 0x45E7E0, 0x460815, 0x4609BA,
	0x460BDA, 0x461AF0, 0x461EE7, 0x462720, 0x4645D4, 0x465106, 0x4657AF, 0x466054, 0x46736F, 0x467C7D,
	0x677851, 0x677B58,
};

static const int NUM_CAMERA_LOOKAT_CALLS = sizeof(CAMERA_LOOKAT_CALLS) / sizeof(CAMERA_LOOKAT_CALLS[0]);
extern Matrix4f g_cameraWorld;
extern bool g_haveCamera;

// Diagnostics (name tags near water) : the look-ats of the frame, and where the
// puppet was drawn. Reset by the survey; logged per level.
struct TagDiag
{
	int frames, lookAts, maxLookAts;
	int drawsOnScreen, drawsElsewhere, behind, noted;
	UINT elseW, elseH, screenW, screenH;
	int lookAtsThisFrame;
	Vector3f eyes[4];                 // This frame's look-ats (eye positions).
	int sampleLogged;
};

void HookCamera();
void NoteNameTag( Peer* peer, EIEvilHarryP* p );

extern bool g_showTags;   // Names above the other players.

// Name tags fade out with the camera distance, between these two (the menu's
// sliders; Multiplayer.ini : Display, TagFadeFrom and TagFadeTo).
static const int TAG_FADE_MAX = 1000, TAG_FADE_STEP = 10;

extern int g_tagFadeFrom, g_tagFadeTo;
extern bool g_showList;   // The player list stays on screen while playing.

typedef HRESULT (__stdcall *PresentFunc)( IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA* );

extern PresentFunc g_origPresent;

bool HookPresent();


#ifdef PTLE_MP_EMBEDTESTS   // For the tests (tests.cpp).

extern volatile LONG g_cropPending;   // A cropped grab, taken in Present.
extern char g_cropName[64];
extern RECT g_crop;
extern uint32_t g_presentCount;
extern uint32_t g_cameraSite;   // The call that built it.
extern int g_cameraSiteCount[NUM_CAMERA_LOOKAT_CALLS];
extern TagDiag g_tagDiag;
extern bool g_tagDiagOn;
extern Matrix4f g_rendCam;
extern uint32_t g_rendFrame, g_rendSets, g_rendSetsMax;
extern void* g_rendSelf;
extern int g_rendTwiceLogged;
extern int g_tagPasses[2];   // Puppet draws elsewhere than to the screen, and to the screen (diagnostics).

void GrabScreen( const char* name );

extern volatile LONG g_grabRequested;
extern char g_grabName[64];
extern HANDLE g_grabDone;

bool PlaceNameTag( const Peer& peer, IDirect3DDevice9* dev, float* x, float* y, float* depth );

#endif
