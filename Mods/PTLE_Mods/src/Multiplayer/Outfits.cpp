#include "Outfits.h"

#include "Multiplayer.h"
#include "Network.h"
#include "Puppets.h"

#include "net/snapshot.h"
#include "net/transport.h"
#include "tests.h"

#include "gizmod/Gizmod.h"
#include "injector/injector.hpp"

#include "ptle/EIHarry.h"
#include "ptle/EIEvilHarryP.h"
#include "utils/func.h"

#include <Windows.h>
#include <d3d9.h>

#include <string.h>
#include <math.h>


// --------------------------------------------------------------------------------
// Outfits.
// --------------------------------------------------------------------------------

static uint32_t g_forceHarryModel = 0;   // The model Harry gets instead of the level's, when a level loads (0 : the level's).
int g_myOutfit = OUTFIT_DEFAULT;         // The one chosen (Multiplayer.ini, [Appearance] Outfit, by name) : worn from the next level load.
int g_myWornOutfit = 0;          // The one Harry wears now : what the others are told.

uint8_t g_myColors[RETRO_PARTS][3];          // Multiplayer.ini, [Appearance] RetroHair... (RRGGBB).



// --------------------------------------------------------------------------------
// The Retro palette. Retro Harry's model is 15 pieces, all textured with one 32x32
// DXT1 texture of four flat squares : shirt (top left), trousers (top right), skin
// (bottom left), hair (bottom right). The game draws every Retro character's pieces
// one after the other with that texture set (DrawPrimitive, a vertex shader whose
// constants c44..c46 are the piece's first bone, in the world). So when a piece is
// about to be drawn with it, the character it belongs to is the one standing there,
// and that player's own palette is put in its place for the draw.
// --------------------------------------------------------------------------------
typedef HRESULT (__stdcall *SetTextureFunc)( IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9* );
typedef HRESULT (__stdcall *DrawPrimFunc)( IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT );
static const int VTABLE_SLOT_SETTEXTURE = 65, VTABLE_SLOT_DRAWPRIMITIVE = 81;
static SetTextureFunc g_origSetTexture = 0;
static DrawPrimFunc g_origDrawPrim = 0;
static IDirect3DBaseTexture9* g_stage0 = 0;   // The texture the game has set.
static bool g_stage0Retro = false;            // It's the Retro palette.
int g_retroSwaps = 0;                         // Pieces drawn with a player's palette (for the autotest).

// Is this texture the game's Retro palette ? Asked for each texture set, so the
// answer is remembered : the palette itself, and the few other textures of its size
// and kind that had to be looked into. Forgotten when a level loads.
static IDirect3DBaseTexture9* g_retroPalette = 0;
static IDirect3DBaseTexture9* g_retroNot[8];
static int g_retroNotNext = 0;

void ForgetRetroTextures()
{
	g_retroPalette = 0;
	ZeroMemory( g_retroNot, sizeof(g_retroNot) );
	g_stage0 = 0;
	g_stage0Retro = false;
}

static bool IsRetroPalette( IDirect3DBaseTexture9* tex )
{
	if ( tex == g_retroPalette ) return true;
	for ( int i = 0; i < 8; i++ ) if ( g_retroNot[i] == tex ) return false;
	bool retro = false;
	D3DSURFACE_DESC desc;
	IDirect3DTexture9* t = (IDirect3DTexture9*) tex;
	if ( tex->GetType() != D3DRTYPE_TEXTURE || FAILED( t->GetLevelDesc( 0, &desc ) )
	  || desc.Width != 32 || desc.Height != 32 || desc.Format != D3DFMT_DXT1 ) return false;
	{
		// One DXT1 block (two colours, 16 two-bit choices) per square is enough to tell.
		static const uint32_t BLOCKS[4][2] = { { 0x46496E29, 0xFFFFFFFF }, { 0x02C002A0, 0xAAAAAAAA }, { 0xFD10FD0F, 0x00000000 }, { 0x8BA893C7, 0xFFFFFFFF } };
		D3DLOCKED_RECT lr;
		if ( SUCCEEDED( t->LockRect( 0, &lr, 0, D3DLOCK_READONLY ) ) ) {
			retro = true;
			for ( int q = 0; q < 4; q++ ) {
				const uint32_t* block = (const uint32_t*) ((const uint8_t*) lr.pBits + (q / 2) * 4 * lr.Pitch + (q % 2) * 4 * 8);
				if ( block[0] != BLOCKS[q][0] || block[1] != BLOCKS[q][1] ) retro = false;
			}
			t->UnlockRect( 0 );
		}
	}
	if ( retro ) {
		g_retroPalette = tex;
		mp_log( "[MP] The Retro palette is texture 0x%.8X.\n", (uint32_t) tex );
	}
	else {
		g_retroNot[g_retroNotNext] = tex;
		g_retroNotNext = (g_retroNotNext + 1) % 8;
	}
	return retro;
}

// A player's palette : the same four squares in their colours. One per player
// (the last is ours), made and repainted as needed.
static struct { IDirect3DTexture9* tex; uint8_t colors[RETRO_PARTS][3]; } g_retroTex[MAX_PEERS + 1];

static IDirect3DTexture9* RetroTexture( IDirect3DDevice9* dev, int slot, const uint8_t colors[RETRO_PARTS][3] )
{
	bool paint = false;
	if ( !g_retroTex[slot].tex ) {
		if ( FAILED( dev->CreateTexture( 32, 32, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &g_retroTex[slot].tex, 0 ) ) ) return 0;
		paint = true;
	}
	if ( paint || memcmp( g_retroTex[slot].colors, colors, sizeof(g_retroTex[slot].colors) ) != 0 ) {
		static const int SQUARE[2][2] = { { RETRO_SHIRT, RETRO_TROUSERS }, { RETRO_SKIN, RETRO_HAIR } };   // [row][column].
		D3DLOCKED_RECT lr;
		if ( FAILED( g_retroTex[slot].tex->LockRect( 0, &lr, 0, 0 ) ) ) return 0;
		for ( int y = 0; y < 32; y++ ) {
			uint32_t* row = (uint32_t*) ((uint8_t*) lr.pBits + y * lr.Pitch);
			for ( int x = 0; x < 32; x++ ) {
				const uint8_t* c = colors[SQUARE[y / 16][x / 16]];
				row[x] = 0xFF000000 | (c[0] << 16) | (c[1] << 8) | c[2];
			}
		}
		g_retroTex[slot].tex->UnlockRect( 0 );
		memcpy( g_retroTex[slot].colors, colors, sizeof(g_retroTex[slot].colors) );
	}
	return g_retroTex[slot].tex;
}

// Whose piece is about to be drawn : the palette of the Retro character it belongs
// to (0 : the game's own, they kept its colours). The piece's first bone (constants
// c44..c46) is one of its character's bone matrices as they are right now, so the
// character is the one whose pose has that very matrix : no guessing from where
// players stand, which mixed their colours up when they stood close together. A
// piece whose bone isn't found (not seen so far) goes to the previous piece's owner :
// a character's 15 pieces are drawn in a row.
static int g_retroOwner = -1;   // The slot of g_retroTex (-1 : nobody yet since the palette was set).
static const uint8_t (*g_retroOwnerColors)[3] = 0;
int g_retroMatched = 0, g_retroUnmatched = 0;          // For the autotest.

static bool HasBoneAt( EICharacter* c, const float bone[3][4] )
{
	const float E = 0.01F;

	int bones = 0;
	const Matrix4f* pose = GetPose( &c->m_animController, &bones );
	if ( !pose || bones <= 0 || bones > MAX_CHAR_BONES ) return false;
	for ( int i = 0; i < bones; i++ ) {
		const float* t = pose[i].data[3];
		if ( fabsf(t[0] - bone[0][3]) < E && fabsf(t[1] - bone[1][3]) < E && fabsf(t[2] - bone[2][3]) < E ) return true;
	}
	return false;
}

static IDirect3DTexture9* RetroTextureHere( IDirect3DDevice9* dev )
{
	float bone[3][4];   // Rows : the translation is each row's w.
	if ( FAILED( dev->GetVertexShaderConstantF( 44, &bone[0][0], 3 ) ) ) return 0;
	int me = g_transport ? g_transport->PlayerId() : -1;
	bool found = false;
	for ( int i = -1; i < MAX_PEERS && !found; i++ ) {   // -1 : our own Harry.
		EICharacter* who;
		const uint8_t (*c)[3];
		if ( i < 0 ) {
			EIHarry* harry = Gizmod::getHarry();
			if ( !harry || g_myWornOutfit != RETRO_OUTFIT ) continue;
			who = harry;
			c = g_myColors;
		}
		else {
			const Peer& p = g_peers[i];
			if ( !p.beast || PeerInLimbo(&p) || p.shownOutfit != RETRO_OUTFIT ) continue;
			who = p.beast;
			c = (i == me || !g_networked || !g_players[i].present) ? g_myColors : g_players[i].colors;
		}
		if ( !HasBoneAt( who, bone ) ) continue;
		g_retroOwner = i < 0 ? MAX_PEERS : i;
		g_retroOwnerColors = c;
		found = true;
	}
	if ( found ) g_retroMatched++; else g_retroUnmatched++;
	if ( g_retroOwner < 0 || memcmp( g_retroOwnerColors, RETRO_DEFAULT, sizeof(RETRO_DEFAULT) ) == 0 ) return 0;
	return RetroTexture( dev, g_retroOwner, g_retroOwnerColors );
}

static HRESULT __stdcall SetTexture_Hook( IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* tex )
{
	if ( stage == 0 ) {
		g_stage0 = tex;
		g_stage0Retro = tex && IsRetroPalette( tex );
		g_retroOwner = -1;
	}
	return g_origSetTexture( dev, stage, tex );
}

static HRESULT __stdcall DrawPrim_Hook( IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT prims )
{
	IDirect3DTexture9* own = g_stage0Retro ? RetroTextureHere( dev ) : 0;
	if ( !own ) return g_origDrawPrim( dev, type, start, prims );
	g_origSetTexture( dev, 0, own );
	HRESULT hr = g_origDrawPrim( dev, type, start, prims );
	g_origSetTexture( dev, 0, g_stage0 );
	g_retroSwaps++;
	return hr;
}

// The device's SetTexture and DrawPrimitive (see HookPresent).
void HookRetroPalette( void** vtable )
{
	g_origSetTexture = (SetTextureFunc) vtable[VTABLE_SLOT_SETTEXTURE];
	injector::WriteMemory( &vtable[VTABLE_SLOT_SETTEXTURE], &SetTexture_Hook, true );
	g_origDrawPrim = (DrawPrimFunc) vtable[VTABLE_SLOT_DRAWPRIMITIVE];
	injector::WriteMemory( &vtable[VTABLE_SLOT_DRAWPRIMITIVE], &DrawPrim_Hook, true );
}



// The puppet wears its player's outfit (ours, when it replays us).
GET_METHOD( 0x60F770, void, EICharacter_SetModel, EICharacter*, uint32_t );
void ApplyPuppetOutfit( EIEvilHarryP* p, Peer* peer )
{
	int me = g_transport ? g_transport->PlayerId() : -1;
	bool ours = peer->id == me || !g_networked;   // It replays us (echo, the loopback).
	int testOutfit = TestPuppetOutfit();
	if ( !ours && !g_players[peer->id].present && testOutfit < 0 ) return;   // They left : as it is.

	int outfit = ours ? g_myWornOutfit : g_players[peer->id].outfit;
	if ( testOutfit >= 0 )
		outfit = testOutfit;
	if ( outfit == peer->shownOutfit ) return;

	uint32_t model = OUTFITS[outfit].model;
	p->m_modelCRC = model;
	p->m_characterCRC = model;
	EICharacter_SetModel( p, model );

	mp_log( "[MP] Player %d's puppet wears the %s outfit.\n", peer->id, OUTFITS[outfit].name );
	peer->shownOutfit = outfit;
}

// Our outfit : the game's cheat flag for it (and none of the others). Harry changes
// when the next level loads.
void ApplyMyOutfit()
{
	for ( int i = 1; i < NUM_OUTFITS; i++ )
		if ( OUTFITS[i].cheatFlag )
			*(uint8_t*) OUTFITS[i].cheatFlag = (i == g_myOutfit) ? 1 : 0;

	g_forceHarryModel = g_myOutfit < 0 || OUTFITS[g_myOutfit].cheatFlag ? 0 : OUTFITS[g_myOutfit].model;   // Harry : the jungle one, even in the snow.
}



// Without a cheat, Harry's model is the level's (0x4ADC1E : mov [ecx+0x130], eax, the
// CRC from the level's data). Ours instead, when one is chosen that has no cheat.
static const uint32_t HARRY_MODEL_STORE = 0x4ADC1E;

static __declspec(naked) void HarryModel_Hook()
{
	__asm {
		cmp dword ptr [g_forceHarryModel], 0
		je keep
		mov eax, dword ptr [g_forceHarryModel]
	keep:
		mov dword ptr [ecx + 0x130], eax
		ret
	}
}

void InstallHarryModelHook()
{
	static const uint8_t MODEL_ORIGINAL[6] = { 0x89, 0x81, 0x30, 0x01, 0x00, 0x00 };   // mov [ecx+0x130], eax.
	if ( memcmp( (void*) HARRY_MODEL_STORE, MODEL_ORIGINAL, 6 ) == 0 ) {
		injector::MakeCALL( HARRY_MODEL_STORE, &HarryModel_Hook );
		injector::MakeNOP( HARRY_MODEL_STORE + 5, 1 );
	}
	else mp_log( "[MP] Harry's model isn't stored where we expect : no Snowy Harry outfit for us.\n" );
}
