#pragma once

#include "Multiplayer.h"


// The animation viewer (F10) and its notes.


extern bool g_animViewer;
extern Peer* g_viewerPeer;   // The puppet it uses.
extern Vector3f g_viewerPin;   // Where that puppet stands.
extern uint32_t g_animPreviewCRC;   // The clip the puppet plays (0 : normal).
extern bool g_emotePreview;
extern Peer* g_parkedPeer;
extern Vector3f g_parkedPin;

bool IsParkedPeer( const Peer* p );
bool IsViewerPeer( const Peer* p );
uint32_t ForcedClip( const Peer* p );

struct AnimNote
{
	int num;
	uint32_t crc;
	char source[24];
	bool star;
	char note[200];
};

static const int MAX_ANIM_NOTES = 1024;
extern AnimNote g_animNotes[MAX_ANIM_NOTES];
extern int g_animNoteCount;
extern int g_animIndex;
extern bool g_animStarredOnly;

void HandleAnimViewer( EIHarry* harry );
