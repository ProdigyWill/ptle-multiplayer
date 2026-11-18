#pragma once

// Text and boxes drawn on top of the game's frame, right before it's presented
// (name tags, the player list). The font is drawn once with GDI into a texture;
// everything else is screen-space quads. The game's Direct3D state is saved and put
// back around the drawing.

#include <d3d9.h>
#include <stdint.h>


// Starts drawing on this frame (false : nothing can be drawn, skip the rest).
// textHeight : pixels, the font is scaled to it.
bool OverlayBegin( IDirect3DDevice9* dev, float textHeight );

// Width of a text in pixels at the current height.
float OverlayTextWidth( const char* text );

// ARGB colors. The text gets a dark shadow.
void OverlayText( float x, float y, const char* text, uint32_t color );
void OverlayRect( float x, float y, float w, float h, uint32_t color );
void OverlayTriangle( float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color );

// Draws what was queued and restores the game's state.
void OverlayEnd();

// The viewport's size (valid between Begin and End).
void OverlayScreenSize( float* w, float* h );
