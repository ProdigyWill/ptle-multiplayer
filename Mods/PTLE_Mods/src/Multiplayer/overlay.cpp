#include "overlay.h"

#include <Windows.h>
#include <string.h>

#include <vector>


// The font texture : printable ASCII (32 .. 126) in a grid, white with the glyphs in
// alpha, drawn at FONT_PX and scaled to the wanted height.
static const int FONT_PX = 32;
static const int FIRST_CHAR = 32, LAST_CHAR = 126, NUM_CHARS = LAST_CHAR - FIRST_CHAR + 1;
static const int COLUMNS = 16;
static const int CELL_W = 40, CELL_H = 40;
static const int ATLAS_W = 1024, ATLAS_H = 256;   // 16 x 6 cells.
static const int PAD = 2;

static IDirect3DDevice9* g_fontDevice = 0;
static IDirect3DTexture9* g_font = 0;
static bool g_fontFailed = false;
static float g_advance[NUM_CHARS];   // Pixels at FONT_PX.

struct Vertex
{
	float x, y, z, rhw;
	DWORD color;
	float u, v;
};
static const DWORD VERTEX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

static IDirect3DDevice9* g_dev = 0;
static float g_scale = 1.0F;
static float g_screenW = 0.0F, g_screenH = 0.0F;
static std::vector<Vertex> g_rects, g_text;

static bool CreateFontTexture( IDirect3DDevice9* dev )
{
	BITMAPINFO bi;
	ZeroMemory( &bi, sizeof(bi) );
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = ATLAS_W;
	bi.bmiHeader.biHeight = -ATLAS_H;   // Top down.
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = 0;
	HDC dc = CreateCompatibleDC( 0 );
	HBITMAP bmp = CreateDIBSection( dc, &bi, DIB_RGB_COLORS, &bits, 0, 0 );
	if ( !dc || !bmp ) { if ( dc ) DeleteDC( dc ); return false; }
	HFONT font = CreateFontA( -FONT_PX, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
		CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Arial" );
	HGDIOBJ oldBmp = SelectObject( dc, bmp );
	HGDIOBJ oldFont = SelectObject( dc, font );
	SetBkMode( dc, TRANSPARENT );
	SetTextColor( dc, RGB( 255, 255, 255 ) );
	memset( bits, 0, ATLAS_W * ATLAS_H * 4 );
	for ( int i = 0; i < NUM_CHARS; i++ ) {
		char c = (char) (FIRST_CHAR + i);
		SIZE size;
		GetTextExtentPoint32A( dc, &c, 1, &size );
		g_advance[i] = (float) size.cx;
		TextOutA( dc, (i % COLUMNS) * CELL_W + PAD, (i / COLUMNS) * CELL_H + PAD, &c, 1 );
	}
	GdiFlush();

	bool ok = false;
	IDirect3DTexture9* tex = 0;
	if ( SUCCEEDED( dev->CreateTexture( ATLAS_W, ATLAS_H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, 0 ) ) ) {
		D3DLOCKED_RECT lr;
		if ( SUCCEEDED( tex->LockRect( 0, &lr, 0, 0 ) ) ) {
			for ( int y = 0; y < ATLAS_H; y++ ) {
				const uint32_t* src = (const uint32_t*) bits + y * ATLAS_W;
				uint32_t* dst = (uint32_t*) ((uint8_t*) lr.pBits + y * lr.Pitch);
				for ( int x = 0; x < ATLAS_W; x++ ) {
					uint32_t a = (src[x] >> 8) & 0xFF;   // Green : antialiased, not ClearType.
					dst[x] = (a << 24) | 0x00FFFFFF;
				}
			}
			tex->UnlockRect( 0 );
			g_font = tex;
			ok = true;
		}
		else tex->Release();
	}

	SelectObject( dc, oldFont );
	SelectObject( dc, oldBmp );
	DeleteObject( font );
	DeleteObject( bmp );
	DeleteDC( dc );
	return ok;
}

bool OverlayBegin( IDirect3DDevice9* dev, float textHeight )
{
	if ( !dev ) return false;
	if ( dev != g_fontDevice ) {
		if ( g_font ) { g_font->Release(); g_font = 0; }
		g_fontDevice = dev;
		g_fontFailed = !CreateFontTexture( dev );
	}
	if ( g_fontFailed ) return false;
	D3DVIEWPORT9 vp;
	if ( FAILED( dev->GetViewport( &vp ) ) ) return false;
	g_screenW = (float) vp.Width;
	g_screenH = (float) vp.Height;
	g_dev = dev;
	g_scale = textHeight / (float) FONT_PX;
	g_rects.clear();
	g_text.clear();
	return true;
}

void OverlayScreenSize( float* w, float* h )
{
	*w = g_screenW;
	*h = g_screenH;
}

float OverlayTextWidth( const char* text )
{
	float w = 0.0F;
	for ( const char* p = text; *p; p++ ) {
		int c = (unsigned char) *p;
		if ( c < FIRST_CHAR || c > LAST_CHAR ) c = '?';
		w += g_advance[c - FIRST_CHAR];
	}
	return w * g_scale;
}

static void Quad( std::vector<Vertex>& out, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, DWORD color )
{
	x0 -= 0.5F; y0 -= 0.5F; x1 -= 0.5F; y1 -= 0.5F;   // Texels onto pixels.
	Vertex a = { x0, y0, 0.0F, 1.0F, color, u0, v0 };
	Vertex b = { x1, y0, 0.0F, 1.0F, color, u1, v0 };
	Vertex c = { x1, y1, 0.0F, 1.0F, color, u1, v1 };
	Vertex d = { x0, y1, 0.0F, 1.0F, color, u0, v1 };
	out.push_back( a ); out.push_back( b ); out.push_back( c );
	out.push_back( a ); out.push_back( c ); out.push_back( d );
}

static void TextPass( float x, float y, const char* text, DWORD color )
{
	for ( const char* p = text; *p; p++ ) {
		int c = (unsigned char) *p;
		if ( c < FIRST_CHAR || c > LAST_CHAR ) c = '?';
		int i = c - FIRST_CHAR;
		if ( c != ' ' ) {
			float u0 = (float) ((i % COLUMNS) * CELL_W) / ATLAS_W;
			float v0 = (float) ((i / COLUMNS) * CELL_H) / ATLAS_H;
			float u1 = u0 + (float) CELL_W / ATLAS_W;
			float v1 = v0 + (float) CELL_H / ATLAS_H;
			float px = x - PAD * g_scale, py = y - PAD * g_scale;
			Quad( g_text, px, py, px + CELL_W * g_scale, py + CELL_H * g_scale, u0, v0, u1, v1, color );
		}
		x += g_advance[i] * g_scale;
	}
}

void OverlayText( float x, float y, const char* text, uint32_t color )
{
	float s = g_scale * 2.0F < 1.0F ? 1.0F : g_scale * 2.0F;   // Shadow offset.
	TextPass( x + s, y + s, text, (color & 0xFF000000) | 0x000000 );
	TextPass( x, y, text, color );
}

void OverlayRect( float x, float y, float w, float h, uint32_t color )
{
	Quad( g_rects, x, y, x + w, y + h, 0.0F, 0.0F, 0.0F, 0.0F, color );
}

void OverlayTriangle( float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color )
{
	Vertex a = { x0 - 0.5F, y0 - 0.5F, 0.0F, 1.0F, color, 0.0F, 0.0F };
	Vertex b = { x1 - 0.5F, y1 - 0.5F, 0.0F, 1.0F, color, 0.0F, 0.0F };
	Vertex c = { x2 - 0.5F, y2 - 0.5F, 0.0F, 1.0F, color, 0.0F, 0.0F };
	g_rects.push_back( a ); g_rects.push_back( b ); g_rects.push_back( c );
}

// The game's state, put back after drawing.
static const D3DRENDERSTATETYPE SAVED_RS[] = {
	D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
	D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE, D3DRS_LIGHTING, D3DRS_FOGENABLE, D3DRS_STENCILENABLE,
	D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE, D3DRS_SEPARATEALPHABLENDENABLE,
	D3DRS_SRGBWRITEENABLE, D3DRS_SHADEMODE, D3DRS_CLIPPING,
};
static const D3DTEXTURESTAGESTATETYPE SAVED_TSS[] = {
	D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
	D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS,
};
static const D3DSAMPLERSTATETYPE SAVED_SS[] = {
	D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV,
};
static const int N_RS = sizeof(SAVED_RS) / sizeof(SAVED_RS[0]);
static const int N_TSS = sizeof(SAVED_TSS) / sizeof(SAVED_TSS[0]);
static const int N_SS = sizeof(SAVED_SS) / sizeof(SAVED_SS[0]);

void OverlayEnd()
{
	IDirect3DDevice9* dev = g_dev;
	g_dev = 0;
	if ( !dev || (g_rects.empty() && g_text.empty()) ) return;

	DWORD rs[N_RS], tss0[N_TSS], tss1Color, tss1Alpha, ss[N_SS];
	for ( int i = 0; i < N_RS; i++ ) dev->GetRenderState( SAVED_RS[i], &rs[i] );
	for ( int i = 0; i < N_TSS; i++ ) dev->GetTextureStageState( 0, SAVED_TSS[i], &tss0[i] );
	dev->GetTextureStageState( 1, D3DTSS_COLOROP, &tss1Color );
	dev->GetTextureStageState( 1, D3DTSS_ALPHAOP, &tss1Alpha );
	for ( int i = 0; i < N_SS; i++ ) dev->GetSamplerState( 0, SAVED_SS[i], &ss[i] );
	DWORD fvf = 0;
	dev->GetFVF( &fvf );
	IDirect3DVertexDeclaration9* decl = 0;
	dev->GetVertexDeclaration( &decl );
	IDirect3DVertexShader9* vs = 0;
	dev->GetVertexShader( &vs );
	IDirect3DPixelShader9* ps = 0;
	dev->GetPixelShader( &ps );
	IDirect3DBaseTexture9* tex = 0;
	dev->GetTexture( 0, &tex );
	IDirect3DVertexBuffer9* stream = 0;
	UINT streamOffset = 0, streamStride = 0;
	dev->GetStreamSource( 0, &stream, &streamOffset, &streamStride );

	if ( SUCCEEDED( dev->BeginScene() ) ) {
		dev->SetVertexShader( 0 );
		dev->SetPixelShader( 0 );
		dev->SetFVF( VERTEX_FVF );
		dev->SetRenderState( D3DRS_ZENABLE, D3DZB_FALSE );
		dev->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
		dev->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
		dev->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
		dev->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
		dev->SetRenderState( D3DRS_BLENDOP, D3DBLENDOP_ADD );
		dev->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
		dev->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
		dev->SetRenderState( D3DRS_LIGHTING, FALSE );
		dev->SetRenderState( D3DRS_FOGENABLE, FALSE );
		dev->SetRenderState( D3DRS_STENCILENABLE, FALSE );
		dev->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
		dev->SetRenderState( D3DRS_COLORWRITEENABLE, 0xF );
		dev->SetRenderState( D3DRS_FILLMODE, D3DFILL_SOLID );
		dev->SetRenderState( D3DRS_SEPARATEALPHABLENDENABLE, FALSE );
		dev->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
		dev->SetRenderState( D3DRS_SHADEMODE, D3DSHADE_GOURAUD );
		dev->SetRenderState( D3DRS_CLIPPING, TRUE );
		dev->SetTextureStageState( 0, D3DTSS_TEXCOORDINDEX, 0 );
		dev->SetTextureStageState( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE );
		dev->SetTextureStageState( 1, D3DTSS_COLOROP, D3DTOP_DISABLE );
		dev->SetTextureStageState( 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE );
		dev->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
		dev->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
		dev->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
		dev->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		dev->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );

		if ( !g_rects.empty() ) {   // Plain color.
			dev->SetTexture( 0, 0 );
			dev->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 );
			dev->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE );
			dev->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 );
			dev->SetTextureStageState( 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE );
			dev->DrawPrimitiveUP( D3DPT_TRIANGLELIST, (UINT) g_rects.size() / 3, g_rects.data(), sizeof(Vertex) );
		}
		if ( !g_text.empty() ) {    // Vertex color, the font's alpha.
			dev->SetTexture( 0, g_font );
			dev->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_MODULATE );
			dev->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			dev->SetTextureStageState( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
			dev->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE );
			dev->SetTextureStageState( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
			dev->SetTextureStageState( 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE );
			dev->DrawPrimitiveUP( D3DPT_TRIANGLELIST, (UINT) g_text.size() / 3, g_text.data(), sizeof(Vertex) );
		}
		dev->EndScene();
	}

	for ( int i = 0; i < N_RS; i++ ) dev->SetRenderState( SAVED_RS[i], rs[i] );
	for ( int i = 0; i < N_TSS; i++ ) dev->SetTextureStageState( 0, SAVED_TSS[i], tss0[i] );
	dev->SetTextureStageState( 1, D3DTSS_COLOROP, tss1Color );
	dev->SetTextureStageState( 1, D3DTSS_ALPHAOP, tss1Alpha );
	for ( int i = 0; i < N_SS; i++ ) dev->SetSamplerState( 0, SAVED_SS[i], ss[i] );
	dev->SetTexture( 0, tex );
	if ( tex ) tex->Release();
	dev->SetPixelShader( ps );
	if ( ps ) ps->Release();
	dev->SetVertexShader( vs );
	if ( vs ) vs->Release();
	if ( decl ) { dev->SetVertexDeclaration( decl ); decl->Release(); }
	else dev->SetFVF( fvf );
	dev->SetStreamSource( 0, stream, streamOffset, streamStride );
	if ( stream ) stream->Release();
}
