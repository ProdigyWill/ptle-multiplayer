#include "Render.h"

#include "Multiplayer.h"
#include "Network.h"
#include "Outfits.h"
#include "Input.h"
#include "MenuPages.h"
#include "AnimViewer.h"

#include "overlay.h"
#include "menu.h"
#include "net/transport.h"
#include "util/MatrixUtils.h"
#include "gamemodes/HideAndSeek/HideAndSeek.h"
#include "tests.h"

#include "injector/injector.hpp"

#include "ptle/EIEvilHarryP.h"
#include "ptle/math/Vector3f.h"

#include <Windows.h>
#include <d3d9.h>
#include <intrin.h>

#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>


volatile LONG g_cropPending = 0;                 // A cropped grab, taken in Present.
char g_cropName[64];
RECT g_crop;

// --------------------------------------------------------------------------------
// Toasts.
// Messages shown at the bottom of the screen for a few seconds (e.g. "Bob joined").
// --------------------------------------------------------------------------------

static const int MAX_TOASTS = 4;
static const uint32_t TOAST_MS = 5000;
static struct { char text[96]; uint32_t time; } g_toasts[MAX_TOASTS];

void Toast( const char* fmt, ... )
{
	for ( int i = MAX_TOASTS - 1; i > 0; i-- ) g_toasts[i] = g_toasts[i - 1];
	va_list args;
	va_start( args, fmt );
	vsnprintf_s( g_toasts[0].text, _TRUNCATE, fmt, args );
	va_end( args );
	g_toasts[0].time = NetNowMs();
}



// --------------------------------------------------------------------------------
// Name tags : where the top of a puppet's head lands on screen, from the camera
// matrices the game rendered it with. Drawn when the frame is presented.
// --------------------------------------------------------------------------------
static const float TAG_ABOVE_FEET = 5.6F;   // The top of Harry's head (measured on screenshots).
uint32_t g_presentCount = 1;

// The camera : the game builds its matrix each frame with a look-at (0x6EE560,
// called from 0x449B9A : eye, target, up), the camera's own world matrix (it looks
// down its -Z). Its projection is captured below (right-handed : w is minus the
// view depth; 50 degrees, 4:3, whatever the window's shape). The matrix at 0x926A40
// looks like one but isn't the main view's.
//
// Each camera mode builds it with its own call (about thirty : the usual one behind
// Harry, 0x449B9A, but also looking around and aiming in first person, and others),
// all into a camera's matrix at +0xE0. Only hooking the usual one left the camera
// as it was when another mode took over : aiming the TNT, another player's tag
// jumped onto Harry's head. All are hooked. Most build a camera of their own (the
// mode's), which the usual call then follows : the main camera is the one it
// writes, used whenever it was built this frame; otherwise the last camera built
// this frame. A frame where none was (a cutscene, its own camera) shows no tags.
typedef Matrix4f* (__thiscall *LookAtFunc)( Matrix4f*, Vector3f*, Vector3f*, Vector3f* );
static const uint32_t CAMERA_LOOKAT_CALL = 0x449B9A;
static const LookAtFunc CameraLookAt = (LookAtFunc) 0x6EE560;
Matrix4f g_cameraWorld;
bool g_haveCamera = false;
uint32_t g_cameraSite = 0;                  // The call that built it.
static Matrix4f* g_mainCamera = 0;          // What the usual call (0x449B9A) writes.
static uint32_t g_mainCameraFrame = 0;
int g_cameraSiteCount[NUM_CAMERA_LOOKAT_CALLS];

TagDiag g_tagDiag;
bool g_tagDiagOn = false;

static Matrix4f* __fastcall CameraLookAt_Hook( Matrix4f* self, void*, Vector3f* eye, Vector3f* target, Vector3f* up )
{
	Matrix4f* m = CameraLookAt( self, eye, target, up );
	uint32_t site = (uint32_t) _ReturnAddress() - 5;
	for ( int i = 0; i < NUM_CAMERA_LOOKAT_CALLS; i++ ) {
		if ( CAMERA_LOOKAT_CALLS[i] != site ) continue;
		if ( g_cameraSiteCount[i]++ == 0 ) mp_log( "[MP] Camera built by 0x%.8X (camera matrix 0x%.8X), eye (%.1f, %.1f, %.1f) : first time.\n", site, self, eye->x, eye->y, eye->z );
		break;
	}
	if ( site == CAMERA_LOOKAT_CALL ) g_mainCamera = self;
	bool main = self == g_mainCamera;
	if ( main ) g_mainCameraFrame = g_presentCount;
	else if ( g_mainCameraFrame == g_presentCount ) return m;   // The main camera is this frame's.
	g_cameraWorld = *m;
	g_haveCamera = true;
	g_cameraSite = site;
	if ( g_tagDiagOn ) {
		if ( g_tagDiag.lookAtsThisFrame < 4 ) g_tagDiag.eyes[g_tagDiag.lookAtsThisFrame] = *eye;
		g_tagDiag.lookAtsThisFrame++;
		g_tagDiag.lookAts++;
	}
	return m;
}

// The perspective matrix, built at 0x6EE990 (called from 0x634B1C) : captured.
typedef Matrix4f* (__thiscall *PerspectiveFunc)( Matrix4f*, float, float, float, float );
static const uint32_t PERSPECTIVE_CALL = 0x634B1C;
static const PerspectiveFunc MakePerspective = (PerspectiveFunc) 0x6EE990;
static Matrix4f g_cameraProj;
static bool g_haveProj = false;

static Matrix4f* __fastcall Perspective_Hook( Matrix4f* self, void*, float a, float b, float c, float d )
{
	Matrix4f* m = MakePerspective( self, a, b, c, d );
	g_cameraProj = *m;
	static int logged = 0;
	if ( logged < 2 ) {
		logged++;
		const float (*p)[4] = m->data;
		mp_log( "[MP] Perspective (%.3f, %.3f, %.3f, %.3f) : %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f\n", a, b, c, d,
			p[0][0], p[1][1], p[2][0], p[2][1], p[2][2], p[2][3], p[3][2], p[3][3] );
	}
	g_haveProj = true;
	return m;
}

// The renderer's own view (what it draws with) : set through 0x64A480 (a view
// matrix), 0x64A4C0 (a camera matrix) or 0x64A500 (a look-at), each keeping the
// view at +0xB0 and the camera's matrix at +0xF0.
Matrix4f g_rendCam;
uint32_t g_rendFrame = 0, g_rendSets = 0, g_rendSetsMax = 0;
static uint32_t g_rendSetsThisFrame = 0;
void* g_rendSelf = 0;
static Vector3f g_rendEyes[4];
int g_rendTwiceLogged = 0;
static void NoteRendererView( void* self )
{
	if ( g_rendFrame != g_presentCount ) { g_rendSetsThisFrame = 0; g_rendFrame = g_presentCount; }
	if ( g_rendSetsThisFrame < 4 ) g_rendEyes[g_rendSetsThisFrame] = *(Vector3f*) ((uint8_t*) self + 0xF0 + 0x30);
	g_rendSetsThisFrame++;
	if ( g_rendSetsThisFrame > g_rendSetsMax ) g_rendSetsMax = g_rendSetsThisFrame;
	g_rendSets++;
	g_rendSelf = self;
	g_rendCam = *(Matrix4f*) ((uint8_t*) self + 0xF0);
}
typedef void (__thiscall *RendSetMatrixFunc)( void*, Matrix4f* );
typedef void (__thiscall *RendLookAtFunc)( void*, Vector3f*, Vector3f*, Vector3f* );
static void __fastcall RendSetView_Hook( void* self, void*, Matrix4f* m ) { ((RendSetMatrixFunc) 0x64A480)( self, m ); NoteRendererView( self ); }
static void __fastcall RendSetCamera_Hook( void* self, void*, Matrix4f* m ) { ((RendSetMatrixFunc) 0x64A4C0)( self, m ); NoteRendererView( self ); }
static void __fastcall RendLookAt_Hook( void* self, void*, Vector3f* e, Vector3f* t, Vector3f* u ) { ((RendLookAtFunc) 0x64A500)( self, e, t, u ); NoteRendererView( self ); }

void HookCamera()
{
	{
		struct { uint32_t site, target; void* hook; } calls[] = {
			{ 0x634C2E, 0x64A480, &RendSetView_Hook }, { 0x634BFE, 0x64A4C0, &RendSetCamera_Hook }, { 0x634BD6, 0x64A500, &RendLookAt_Hook } };
		for ( auto& c : calls ) {
			if ( *(uint8_t*) c.site == 0xE8 && c.site + 5 + *(int32_t*) (c.site + 1) == c.target ) injector::MakeCALL( c.site, c.hook );
			else mp_log( "[MP] Renderer view call 0x%.8X isn't what we expect.\n", c.site );
		}
	}
	static const uint8_t PERSPECTIVE_ORIGINAL[5] = { 0xE8, 0x6F, 0x9E, 0x0B, 0x00 };   // call 0x6EE990.
	if ( memcmp( (void*) PERSPECTIVE_CALL, PERSPECTIVE_ORIGINAL, 5 ) == 0 ) injector::MakeCALL( PERSPECTIVE_CALL, &Perspective_Hook );
	else mp_log( "[MP] The perspective call isn't what we expect.\n" );

	int hooked = 0;
	for ( int i = 0; i < NUM_CAMERA_LOOKAT_CALLS; i++ ) {
		uint32_t site = CAMERA_LOOKAT_CALLS[i];
		if ( *(uint8_t*) site != 0xE8 || site + 5 + *(int32_t*) (site + 1) != (uint32_t) CameraLookAt ) {
			mp_log( "[MP] Camera call 0x%.8X isn't what we expect%s.\n", site, site == CAMERA_LOOKAT_CALL ? " (free camera on?)" : "" );
			continue;
		}
		injector::MakeCALL( site, &CameraLookAt_Hook );
		hooked++;
	}
	if ( !hooked ) mp_log( "[MP] No camera call hooked : no name tags.\n" );
}

int g_tagPasses[2] = { 0, 0 };          // Puppet draws elsewhere than to the screen, and to the screen (diagnostics).

void NoteNameTag( Peer* peer, EIEvilHarryP* p )
{
	Matrix4f view;
	if ( !g_haveCamera || !MatInverseAffine( g_cameraWorld, &view ) ) return;
	if ( !g_haveProj ) return;
	const float* proj = &g_cameraProj.data[0][0];
	IDirect3DDevice9* dev = *(IDirect3DDevice9**) RENDERER_DEVICE;
	D3DVIEWPORT9 vp;
	if ( !dev || FAILED( dev->GetViewport( &vp ) ) ) return;

	// Levels with water also draw him into a 512x512 texture, their reflection. Some
	// draw him once for the screen and once for it (the Great Tree, Butterfly Glade);
	// others once only, while the reflection's texture is the target, and the screen
	// shows that same draw (Jungle Canyon, Bittenbinder's Camp, Cavern Lake...). Only
	// accepting a draw to the screen left those without a tag, and the Great Tree's
	// flickering in the frames it skipped the screen's. The camera captured is the
	// main view's whatever the target (one look-at a frame, water levels too), so
	// the tag is placed with it on the screen's own image, whichever draw this is.
	IDirect3DSurface9* target = 0;
	IDirect3DSurface9* screen = 0;
	bool onScreen = SUCCEEDED( dev->GetRenderTarget( 0, &target ) ) && SUCCEEDED( dev->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &screen ) ) && target == screen;
	D3DSURFACE_DESC screenDesc = { };
	bool haveScreen = screen && SUCCEEDED( screen->GetDesc( &screenDesc ) );
	if ( target ) target->Release();
	if ( screen ) screen->Release();
	if ( !onScreen ) {
		if ( !haveScreen ) return;
		vp.X = vp.Y = 0;
		vp.Width = screenDesc.Width;
		vp.Height = screenDesc.Height;
	}
	g_tagPasses[onScreen ? 1 : 0]++;
	if ( g_tagDiagOn ) {
		D3DSURFACE_DESC d = { };
		IDirect3DSurface9* rt = 0;
		if ( SUCCEEDED( dev->GetRenderTarget( 0, &rt ) ) && rt ) { rt->GetDesc( &d ); rt->Release(); }
		if ( onScreen ) { g_tagDiag.drawsOnScreen++; g_tagDiag.screenW = d.Width; g_tagDiag.screenH = d.Height; }
		else { g_tagDiag.drawsElsewhere++; g_tagDiag.elseW = d.Width; g_tagDiag.elseH = d.Height; }
		if ( g_tagDiag.frames >= 30 && g_tagDiag.sampleLogged < 2 ) {
			g_tagDiag.sampleLogged++;
			const float* e = &g_cameraWorld.data[3][0];
			mp_log( "[AT] TAGS sample : draw to %s %ux%u, viewport %ux%u at (%u, %u); camera eye now (%.1f, %.1f, %.1f); %d look-at(s) this frame so far :\n",
				onScreen ? "screen" : "other target", d.Width, d.Height, vp.Width, vp.Height, vp.X, vp.Y, e[0], e[1], e[2], g_tagDiag.lookAtsThisFrame );
			for ( int i = 0; i < g_tagDiag.lookAtsThisFrame && i < 4; i++ )
				mp_log( "[AT]   look-at %d : eye (%.1f, %.1f, %.1f)\n", i, g_tagDiag.eyes[i].x, g_tagDiag.eyes[i].y, g_tagDiag.eyes[i].z );
		}
	}

	// Above his head : his transform is at his feet. (The highest bone isn't reliable :
	// some bones, not drawn, sit far above him in some poses.) Placed again each
	// frame it's shown (PlaceNameTag); this one is for the logs and tests.
	const Matrix4f& m = p->m_transformMatrix;
	float x = m.data[3][0], y = m.data[3][1], z = m.data[3][2] + TAG_ABOVE_FEET;

	float v[4], c[4];   // Row vector times view, times projection.
	for ( int j = 0; j < 4; j++ ) v[j] = x * view.data[0][j] + y * view.data[1][j] + z * view.data[2][j] + view.data[3][j];
	for ( int j = 0; j < 4; j++ ) c[j] = v[0] * proj[j] + v[1] * proj[4 + j] + v[2] * proj[8 + j] + v[3] * proj[12 + j];
	if ( c[3] < 0.1F ) { if ( g_tagDiagOn ) g_tagDiag.behind++; return; }   // Behind the camera.
	if ( g_tagDiagOn ) g_tagDiag.noted++;
	peer->tagX = vp.X + (c[0] / c[3] + 1.0F) * 0.5F * vp.Width;
	peer->tagY = vp.Y + (1.0F - c[1] / c[3]) * 0.5F * vp.Height;
	peer->tagDepth = c[3];
	peer->tagFrame = g_presentCount;

	static int logged = 0;
	if ( logged < 3 ) {
		logged++;
		mp_log( "[MP] Name tag : head (%.1f, %.1f, %.1f) at screen (%.0f, %.0f), depth %.1f, viewport %ux%u.\n",
			x, y, z, peer->tagX, peer->tagY, peer->tagDepth, vp.Width, vp.Height );
	}
}

bool g_showTags = true;           // Names above the other players.
int g_tagFadeFrom = 100, g_tagFadeTo = 200;
bool g_showList = false;          // The player list stays on screen while playing.

// Writes a 24-bit bottom-up image to logs\shots\<name>.bmp.
static void WriteBmp( const char* name, int w, int h, const void* bits )
{
	char path[MAX_PATH];
	sprintf_s( path, "%s\\shots", g_logDir );
	CreateDirectoryA( path, 0 );
	sprintf_s( path, "%s\\shots\\%s.bmp", g_logDir, name );

	BITMAPINFOHEADER ih; ZeroMemory( &ih, sizeof(ih) );
	ih.biSize = sizeof(BITMAPINFOHEADER);
	ih.biWidth = w;
	ih.biHeight = h;   // Bottom-up, as BMP files expect.
	ih.biPlanes = 1;
	ih.biBitCount = 24;
	ih.biCompression = BI_RGB;
	DWORD imageSize = (((w * 3) + 3) & ~3) * h;
	BITMAPFILEHEADER fh; ZeroMemory( &fh, sizeof(fh) );
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
	fh.bfSize = fh.bfOffBits + imageSize;

	FILE* f = 0;
	if ( fopen_s( &f, path, "wb" ) == 0 && f ) {
		fwrite( &fh, sizeof(fh), 1, f );
		fwrite( &ih, sizeof(ih), 1, f );
		fwrite( bits, imageSize, 1, f );
		fclose( f );
		mp_log( "[AT] Screenshot : shots\\%s.bmp\n", name );
	}
}

// Foreground : grabs the game window's client area from the screen.
void GrabScreen( const char* name )
{
	HWND wnd = *HWND_GAME_WINDOW;
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
	bi.bmiHeader.biHeight = h;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 24;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = 0;
	HBITMAP dib = CreateDIBSection( screen, &bi, DIB_RGB_COLORS, &bits, 0, 0 );
	HGDIOBJ old = SelectObject( mem, dib );
	BitBlt( mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT );
	GdiFlush();
	WriteBmp( name, w, h, bits );
	SelectObject( mem, old );
	DeleteObject( dib );
	DeleteDC( mem );
	ReleaseDC( 0, screen );
}

// Background : the window may be covered (and its Direct3D 9 output isn't in what
// Windows can capture of it), so the frame is copied from the back buffer right
// before the game presents it. The renderer is a static object at 0x926790 with
// its IDirect3DDevice9 at +0x74 (filled by CreateDevice at 0x697FB2).
static const int VTABLE_SLOT_PRESENT = 17;   // IDirect3DDevice9::Present.
PresentFunc g_origPresent = 0;
volatile LONG g_grabRequested = 0;
char g_grabName[64];
HANDLE g_grabDone = 0;

static void GrabBackBuffer( IDirect3DDevice9* dev, const char* name, const RECT* crop )
{
	IDirect3DSurface9* back = 0;
	IDirect3DSurface9* resolved = 0;
	IDirect3DSurface9* copy = 0;
	if ( FAILED( dev->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &back ) ) ) { mp_log( "[AT] Screenshot : no back buffer.\n" ); return; }
	D3DSURFACE_DESC d;
	back->GetDesc( &d );
	IDirect3DSurface9* source = back;
	if ( d.MultiSampleType != D3DMULTISAMPLE_NONE
	  && SUCCEEDED( dev->CreateRenderTarget( d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, 0 ) )
	  && SUCCEEDED( dev->StretchRect( back, 0, resolved, 0, D3DTEXF_NONE ) ) ) {
		source = resolved;
	}
	D3DLOCKED_RECT lr;
	if ( SUCCEEDED( dev->CreateOffscreenPlainSurface( d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &copy, 0 ) )
	  && SUCCEEDED( dev->GetRenderTargetData( source, copy ) )
	  && SUCCEEDED( copy->LockRect( &lr, 0, D3DLOCK_READONLY ) ) ) {
		int x0 = 0, y0 = 0, w = d.Width, h = d.Height;
		if ( crop ) {   // Clamped to the frame.
			x0 = max( 0, (int) crop->left ); y0 = max( 0, (int) crop->top );
			w = min( (int) d.Width, (int) crop->right ) - x0; h = min( (int) d.Height, (int) crop->bottom ) - y0;
			if ( w <= 0 || h <= 0 ) { x0 = y0 = 0; w = d.Width; h = d.Height; }
		}
		int bpp = (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) ? 4 : 2;
		int stride = ((w * 3) + 3) & ~3;
		uint8_t* bits = (uint8_t*) calloc( stride, h );
		for ( int y = 0; bits && y < h; y++ ) {
			const uint8_t* src = (const uint8_t*) lr.pBits + (y0 + y) * lr.Pitch + x0 * bpp;
			uint8_t* dst = bits + (h - 1 - y) * stride;
			for ( int x = 0; x < w; x++, dst += 3 ) {
				if ( d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8 ) {
					dst[0] = src[x * 4]; dst[1] = src[x * 4 + 1]; dst[2] = src[x * 4 + 2];
				}
				else {   // 16-bit : R5G6B5, or X1R5G5B5.
					uint16_t v = ((const uint16_t*) src)[x];
					bool g6 = d.Format == D3DFMT_R5G6B5;
					dst[0] = (uint8_t) ((v & 0x1F) << 3);
					dst[1] = (uint8_t) (g6 ? ((v >> 5) & 0x3F) << 2 : ((v >> 5) & 0x1F) << 3);
					dst[2] = (uint8_t) (g6 ? ((v >> 11) & 0x1F) << 3 : ((v >> 10) & 0x1F) << 3);
				}
			}
		}
		if ( bits ) WriteBmp( name, w, h, bits );
		free( bits );
		copy->UnlockRect();
	}
	else {
		mp_log( "[AT] Screenshot : couldn't read the back buffer (format %d).\n", d.Format );
	}
	if ( copy ) copy->Release();
	if ( resolved ) resolved->Release();
	back->Release();
}

static void DrawOverlay( IDirect3DDevice9* dev );

static HRESULT __stdcall Present_Hook( IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty )
{
	// Loading, menus, pause : Harry doesn't tick, but the room still hears from us
	// (and we from it).
	if ( NetNowMs() - g_lastNetUpdate > 250 ) NetUpdate();
	DrawOverlay( dev );
	g_presentCount++;
	if ( g_tagDiagOn ) {
		g_tagDiag.frames++;
		if ( g_tagDiag.lookAtsThisFrame > g_tagDiag.maxLookAts ) g_tagDiag.maxLookAts = g_tagDiag.lookAtsThisFrame;
		g_tagDiag.lookAtsThisFrame = 0;
		if ( g_rendFrame + 1 == g_presentCount && g_rendSetsThisFrame >= 2 && g_rendTwiceLogged < 6 ) {
			g_rendTwiceLogged++;
			mp_log( "[AT] TAGS renderer view set %u times in a frame :", g_rendSetsThisFrame );
			for ( uint32_t i = 0; i < g_rendSetsThisFrame && i < 4; i++ ) mp_log( " (%.1f, %.1f, %.1f)", g_rendEyes[i].x, g_rendEyes[i].y, g_rendEyes[i].z );
			mp_log( "; look-at camera (%.1f, %.1f, %.1f).\n", g_cameraWorld.data[3][0], g_cameraWorld.data[3][1], g_cameraWorld.data[3][2] );
		}
	}
	if ( g_cropPending ) {
		GrabBackBuffer( dev, g_cropName, &g_crop );
		InterlockedExchange( &g_cropPending, 0 );
	}
	if ( g_grabRequested ) {
		GrabBackBuffer( dev, g_grabName, 0 );
		InterlockedExchange( &g_grabRequested, 0 );
		SetEvent( g_grabDone );
	}
	return g_origPresent( dev, src, dst, wnd, dirty );
}

// Hooks the device's Present once it exists.
bool HookPresent()
{
	if ( g_origPresent ) return true;
	IDirect3DDevice9* dev = *(IDirect3DDevice9**) RENDERER_DEVICE;
	HMODULE d3d9 = GetModuleHandleA( "d3d9.dll" );
	if ( !dev || !d3d9 ) return false;
	// Windows' d3d9.dll gives each device its own copy of the vtable (heap memory),
	// so the check is on Present itself : it must be code in a loaded module.
	void** vtable = *(void***) dev;
	MEMORY_BASIC_INFORMATION mbi;
	if ( !VirtualQuery( vtable, &mbi, sizeof(mbi) ) || mbi.State != MEM_COMMIT
	  || !VirtualQuery( vtable[VTABLE_SLOT_PRESENT], &mbi, sizeof(mbi) ) || mbi.Type != MEM_IMAGE ) {
		mp_log( "[AT] The renderer's device (0x%.8X) doesn't look like a Direct3D 9 device.\n", (uint32_t) dev );
		return false;
	}
	char module[MAX_PATH] = "?";
	GetModuleFileNameA( (HMODULE) mbi.AllocationBase, module, MAX_PATH );
	mp_log( "[AT] Direct3D device 0x%.8X, Present in %s.\n", (uint32_t) dev, module );
	g_grabDone = CreateEventA( 0, FALSE, FALSE, 0 );
	g_origPresent = (PresentFunc) vtable[VTABLE_SLOT_PRESENT];
	injector::WriteMemory( &vtable[VTABLE_SLOT_PRESENT], &Present_Hook, true );
	HookRetroPalette( vtable );
	mp_log( "[AT] Screenshots come from the Direct3D back buffer.\n" );
	return true;
}

// Where a puppet's tag goes on the screen now : the camera of this frame, the
// puppet where it is. The game doesn't draw a puppet in every frame where it's seen
// through a wall (St. Claire's camp at night : one frame in two or three) : a tag
// only shown on frames it was drawn flickered there. It stays up for a few frames
// after the last draw, placed again each frame. False : not shown.
static const uint32_t TAG_GRACE_FRAMES = 12;

bool PlaceNameTag( const Peer& peer, IDirect3DDevice9* dev, float* x, float* y, float* depth )
{
	EIEvilHarryP* p = peer.beast;
	if ( !p || !peer.tagFrame || g_presentCount - peer.tagFrame > TAG_GRACE_FRAMES || g_levelLeaving ) return false;

	// The view the renderer drew this frame with (the last it was given : any
	// reflection comes first), whichever camera mode built it. Choosing among the
	// cameras' own look-ats instead left the tag behind when aiming the TNT, and
	// jumping about while the camera swung into it.
	if ( !g_haveProj || g_presentCount - g_rendFrame > 1 ) return false;

	Matrix4f view;
	if ( !MatInverseAffine(g_rendCam, &view) ) return false;

	IDirect3DSurface9* screen = 0;
	D3DSURFACE_DESC d = { };
	bool ok = SUCCEEDED( dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &screen) ) && screen && SUCCEEDED( screen->GetDesc(&d) );
	if ( screen ) screen->Release();
	if ( !ok ) return false;

	const float* proj = &g_cameraProj.data[0][0];
	const Matrix4f& m = p->m_transformMatrix;
	float px = m.data[3][0], py = m.data[3][1], pz = m.data[3][2] + TAG_ABOVE_FEET;
	float v[4], c[4];
	for ( int j = 0; j < 4; j++ ) v[j] = px * view.data[0][j] + py * view.data[1][j] + pz * view.data[2][j] + view.data[3][j];
	for ( int j = 0; j < 4; j++ ) c[j] = v[0] * proj[j] + v[1] * proj[4 + j] + v[2] * proj[8 + j] + v[3] * proj[12 + j];
	if ( c[3] < 0.1F ) return false;   // Behind the camera.

	*x = (c[0] / c[3] + 1.0F) * 0.5F * d.Width;
	*y = (1.0F - c[1] / c[3]) * 0.5F * d.Height;
	*depth = c[3];
	return true;
}

// --------------------------------------------------------------------------------
// The overlay : name tags over the other players, the player list (F9), and
// "joined" / "left" messages.
// --------------------------------------------------------------------------------

static uint32_t WithAlpha( uint32_t color, float alpha )
{
	if ( alpha < 0.0F ) alpha = 0.0F;
	if ( alpha > 1.0F ) alpha = 1.0F;
	return (color & 0x00FFFFFF) | ((uint32_t) (((color >> 24) & 0xFF) * alpha + 0.5F) << 24);
}

static void DrawOverlay( IDirect3DDevice9* dev )
{
	if ( TestAnimSurveyOn() ) return;   // Clean frames.

	uint32_t now = NetNowMs();
	if ( InterlockedExchange(&g_reqMenu, 0) ) SetMenuOpen( !g_menu.IsOpen() );
	bool any = g_showList || g_animViewer || g_menu.IsOpen() || PlayingHs() || EmoteWheelShown();
	for ( int i = 0; i < MAX_PEERS && !any; i++ ) any = g_peers[i].beast && g_peers[i].tagFrame && g_presentCount - g_peers[i].tagFrame <= TAG_GRACE_FRAMES;
	for ( int i = 0; i < MAX_TOASTS && !any; i++ ) any = g_toasts[i].text[0] && now - g_toasts[i].time < TOAST_MS;
	if ( !any ) return;

	D3DVIEWPORT9 vp;
	if ( FAILED(dev->GetViewport(&vp)) ) return;
	float h = vp.Height / 38.0F;
	if ( h < 14.0F ) h = 14.0F;
	if ( !OverlayBegin(dev, h) ) return;
	float sw, sh;
	OverlayScreenSize( &sw, &sh );
	float pad = h * 0.3F;

	// Name tags.
	for ( int i = 0; i < MAX_PEERS; i++ ) {
		const Peer& p = g_peers[i];

		float tagX, tagY, tagDepth;
		if ( !g_showTags || g_menu.IsOpen() || EmoteWheelShown() || HiddenFromMe(p.id) || !PlaceNameTag(p, dev, &tagX, &tagY, &tagDepth) ) continue;

		const float from = (float) g_tagFadeFrom, to = (float) g_tagFadeTo;
		const float alpha = to > from ? 1.0F - (tagDepth - from) / (to - from) : (tagDepth < from ? 1.0F : 0.0F);
		if ( alpha <= 0.0F ) continue;

		const char* name = PlayerName( p.id );
		float w = OverlayTextWidth( name );
		float x = floorf( tagX - w * 0.5F ), y = floorf( tagY - h * 1.2F );
		OverlayRect( x - pad, y - pad * 0.5F, w + 2.0F * pad, h + pad, WithAlpha(0x80000000, alpha) );
		OverlayText( x, y, name, WithAlpha(0xFFFFFFFF, alpha) );
	}

	float listLeft = sw;   // Messages stay clear of the player list.
	// The player list, top right.
	if ( g_showList && !g_menu.IsOpen() ) {
		char left[MAX_LIST_ROWS][96], right[MAX_LIST_ROWS][64];
		uint32_t colors[MAX_LIST_ROWS];
		int rows = BuildPlayerRows( left, right, colors, "F1 : menu" );

		float wl = 0.0F, wr = 0.0F;
		for ( int r = 0; r < rows; r++ ) {
			float a = OverlayTextWidth( left[r] ), b = OverlayTextWidth( right[r] );
			if ( a > wl ) wl = a;
			if ( b > wr ) wr = b;
		}
		float gap = h * 1.5F;
		float bw = wl + gap + wr + 2.0F * pad, bh = rows * h * 1.3F + 2.0F * pad;
		float bx = floorf( sw - bw - h ), by = h;
		listLeft = bx - pad;
		OverlayRect( bx, by, bw, bh, 0xA0000000 );
		for ( int r = 0; r < rows; r++ ) {
			float y = by + pad + r * h * 1.3F;
			OverlayText( bx + pad, y, left[r], colors[r] );
			OverlayText( bx + bw - pad - OverlayTextWidth( right[r] ), y, right[r], colors[r] );
		}
	}

	// Messages : bottom middle, the newest lowest (clear of the player list, whatever its width).
	float ty = floorf( sh - h * 3.5F );
	for ( int i = 0; i < MAX_TOASTS; i++ ) {
		if ( !g_toasts[i].text[0] || now - g_toasts[i].time >= TOAST_MS ) continue;
		float alpha = (TOAST_MS - (now - g_toasts[i].time)) / 1000.0F;   // Fades out over the last second.
		float w = OverlayTextWidth( g_toasts[i].text );
		float x = floorf( (sw - w) * 0.5F );
		OverlayRect( x - pad, ty - pad * 0.5F, w + 2.0F * pad, h + pad, WithAlpha( 0xA0000000, alpha ) );
		OverlayText( x, ty, g_toasts[i].text, WithAlpha( 0xFFFFE080, alpha ) );
		ty -= h * 1.4F;
	}

	// Hide and seek : our role and hide time, top left, under the game's health.
	if ( PlayingHs() && !g_menu.IsOpen() ) {
		char hidden[16], text[48];
		FormatHideTime( g_hsMyHideMs, hidden, sizeof(hidden) );
		const char* role = g_hsMyRole == ROLE_HIDER ? "HIDER" : "SEEKER";
		sprintf_s( text, "%s   %s", role, hidden );

		const float x = floorf( sw * (28.0F / 640.0F) + pad ), y = floorf( sh * 0.14F );   // The box's left : the first health box's (the game's HUD is laid out 640 wide).
		const float w = OverlayTextWidth( text );
		OverlayRect( x - pad, y - pad * 0.5F, w + 2.0F * pad, h + pad, 0xA0000000 );
		OverlayText( x, y, role, g_hsMyRole == ROLE_HIDER ? 0xFFFFE080 : 0xFFFFFFFF );
		OverlayText( x + w - OverlayTextWidth( hidden ), y, hidden, 0xFFFFFFFF );
	}

	if ( EmoteWheelShown() && !g_menu.IsOpen() )
		DrawWheel( sw, sh, h );

	// The animation viewer, bottom left.
	if ( g_animViewer && g_animIndex < g_animNoteCount ) {
		const AnimNote& n = g_animNotes[g_animIndex];
		int stars = 0, place = 0;
		for ( int i = 0; i < g_animNoteCount; i++ ) {
			if ( g_animNotes[i].star ) { stars++; if ( i <= g_animIndex ) place++; }
		}
		char line[3][256];
		uint32_t colors[3] = { n.star ? 0xFFFFE080 : 0xFFFFFFFF, 0xFFFFFFFF, 0xFFB0B0B0 };
		if ( g_animStarredOnly && stars )
			sprintf_s( line[0], "%sAnimation #%03d   (starred %d of %d)   0x%.8X   %s", n.star ? "* " : "", n.num, place, stars, n.crc, n.source );
		else
			sprintf_s( line[0], "%sAnimation #%03d   (%d of %d)   0x%.8X   %s", n.star ? "* " : "", n.num, g_animIndex + 1, g_animNoteCount, n.crc, n.source );
		if ( n.note[0] ) sprintf_s( line[1], "%s", n.note );
		else sprintf_s( line[1], "(no notes yet : %%LOCALAPPDATA%%\\PTLE Multiplayer\\anim-notes.txt)" );
		colors[1] = n.note[0] ? 0xFFFFFFFF : 0xFFB0B0B0;
		sprintf_s( line[2], "[ ] : previous / next (Shift : 10)   \\ : replay   F11 : star   ' : %s   F10 : close",
			g_animStarredOnly ? "all clips" : "starred only" );
		float w = 0.0F;
		for ( int r = 0; r < 3; r++ ) { float a = OverlayTextWidth( line[r] ); if ( a > w ) w = a; }
		float bw = w + 2.0F * pad, bh = 3 * h * 1.3F + 2.0F * pad;
		float bx = h, by = floorf( sh - bh - h );
		OverlayRect( bx, by, bw, bh, 0xA0000000 );
		for ( int r = 0; r < 3; r++ ) OverlayText( bx + pad, by + pad + r * h * 1.3F, line[r], colors[r] );
	}

	if ( g_menu.IsOpen() ) {
		DrawMenu( sw, sh, h );
		SyncMenuOpen();   // Esc closed it.
	}

	OverlayEnd();
}
