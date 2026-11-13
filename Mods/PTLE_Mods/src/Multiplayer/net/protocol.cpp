#include "net/protocol.h"

#include <math.h>
#include <string.h>


static const uint16_t MAGIC = 0x4D50;   // "PM".
static const uint8_t VERSION = 3;
static const int HEADER_SIZE = 2 + 1 + 1 + 1 + 4;

static const float TRANSLATION_STEP = 1.0F / 512.0F;    // int16 : +-64 units.
static const float MAX_SANE_VALUE = 10000.0F;           // Beyond : garbage (an item never posed yet).
static const float MAX_SANE_SCALE = 100.0F;
static const float SCALE_STEP = 1.0F / 4096.0F;         // 15 bits : 0 .. 8.
static const uint16_t SCALE_MIRRORED = 0x8000;
static const float QUAT_RANGE = 0.70710678F;            // Smallest three : each within +-1/sqrt(2).
static const int QUAT_BITS = 10;
static const int QUAT_MAX = (1 << QUAT_BITS) - 1;
static const int PACKED_MATRIX_SIZE = 4 + 3 * 2 + 3 * 2;   // The largest (per-axis scales).


// --------------------------------------------------------------------------------
// Byte streams (little-endian, like x86 : plain copies).
// --------------------------------------------------------------------------------
struct Writer
{
	uint8_t* p;
	uint8_t* end;
	bool ok;

	template<typename T> void put( T v ) {
		if ( p + sizeof(T) > end ) { ok = false; return; }
		memcpy( p, &v, sizeof(T) );
		p += sizeof(T);
	}
};

struct Reader
{
	const uint8_t* p;
	const uint8_t* end;
	bool ok;

	template<typename T> T get() {
		T v = T();
		if ( p + sizeof(T) > end ) { ok = false; return v; }
		memcpy( &v, p, sizeof(T) );
		p += sizeof(T);
		return v;
	}
};


// --------------------------------------------------------------------------------
// Matrices. Affine, row-vector (rows 0..2 the axes, row 3 the translation).
// --------------------------------------------------------------------------------
static int16_t ToInt16( float v, float step )
{
	float q = v / step;
	if ( q > 32767.0F ) q = 32767.0F;
	if ( q < -32767.0F ) q = -32767.0F;
	return (int16_t) (q < 0.0F ? q - 0.5F : q + 0.5F);
}

bool IsSaneMatrix( const Matrix4f& m )
{
	for ( int i = 0; i < 4; i++ )
		for ( int j = 0; j < 3; j++ )
			if ( !(fabsf( m.data[i][j] ) < MAX_SANE_VALUE) ) return false;   // Also false for NaN.
	for ( int i = 0; i < 3; i++ ) {
		const float* r = m.data[i];
		if ( r[0] * r[0] + r[1] * r[1] + r[2] * r[2] > MAX_SANE_SCALE * MAX_SANE_SCALE ) return false;
	}
	return true;
}

static uint16_t PackScale( float scale )
{
	float sq = scale / SCALE_STEP + 0.5F;
	if ( sq > 0x7FFF ) sq = (float) 0x7FFF;
	return (uint16_t) sq;
}

// perAxis : a scale per axis (items squash and stretch), else one for all three.
static void PutMatrix( Writer& w, const Matrix4f& m, bool perAxis )
{
	if ( !IsSaneMatrix( m ) ) {   // Sent as nothing : scale 0 at the origin.
		w.put<uint32_t>( 0 );
		for ( int j = 0; j < 3; j++ ) w.put<int16_t>( 0 );
		for ( int j = 0; j < (perAxis ? 3 : 1); j++ ) w.put<uint16_t>( 0 );
		return;
	}
	const float (*a)[4] = m.data;
	float len[3];
	for ( int i = 0; i < 3; i++ ) len[i] = sqrtf( a[i][0] * a[i][0] + a[i][1] * a[i][1] + a[i][2] * a[i][2] );
	float scale = (len[0] + len[1] + len[2]) / 3.0F;
	if ( !perAxis ) len[0] = len[1] = len[2] = scale;
	float det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
	          - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
	          + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
	bool mirrored = det < 0.0F;

	// The rotation (a mirrored matrix is minus a rotation).
	float r[3][3];
	bool flat = false;
	for ( int i = 0; i < 3; i++ ) {
		if ( len[i] <= 1e-6F ) flat = true;
		float k = len[i] > 1e-6F ? (mirrored ? -1.0F : 1.0F) / len[i] : 0.0F;
		for ( int j = 0; j < 3; j++ ) r[i][j] = a[i][j] * k;
	}

	float q[4];   // x, y, z, w.
	if ( flat ) {
		q[0] = q[1] = q[2] = 0.0F; q[3] = 1.0F;
	}
	else {
		float trace = r[0][0] + r[1][1] + r[2][2];
		if ( trace > 0.0F ) {
			float s = 0.5F / sqrtf( trace + 1.0F );
			q[3] = 0.25F / s;
			q[0] = (r[2][1] - r[1][2]) * s;
			q[1] = (r[0][2] - r[2][0]) * s;
			q[2] = (r[1][0] - r[0][1]) * s;
		}
		else if ( r[0][0] > r[1][1] && r[0][0] > r[2][2] ) {
			float s = 2.0F * sqrtf( 1.0F + r[0][0] - r[1][1] - r[2][2] );
			q[3] = (r[2][1] - r[1][2]) / s;
			q[0] = 0.25F * s;
			q[1] = (r[0][1] + r[1][0]) / s;
			q[2] = (r[0][2] + r[2][0]) / s;
		}
		else if ( r[1][1] > r[2][2] ) {
			float s = 2.0F * sqrtf( 1.0F + r[1][1] - r[0][0] - r[2][2] );
			q[3] = (r[0][2] - r[2][0]) / s;
			q[0] = (r[0][1] + r[1][0]) / s;
			q[1] = 0.25F * s;
			q[2] = (r[1][2] + r[2][1]) / s;
		}
		else {
			float s = 2.0F * sqrtf( 1.0F + r[2][2] - r[0][0] - r[1][1] );
			q[3] = (r[1][0] - r[0][1]) / s;
			q[0] = (r[0][2] + r[2][0]) / s;
			q[1] = (r[1][2] + r[2][1]) / s;
			q[2] = 0.25F * s;
		}
		float n = sqrtf( q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] );
		for ( int i = 0; i < 4; i++ ) q[i] /= n;
	}

	// Smallest three : drop the largest component (made positive), keep the others.
	int largest = 0;
	for ( int i = 1; i < 4; i++ ) if ( fabsf( q[i] ) > fabsf( q[largest] ) ) largest = i;
	float sign = q[largest] < 0.0F ? -1.0F : 1.0F;
	uint32_t packed = (uint32_t) largest << 30;
	int shift = 20;
	for ( int i = 0; i < 4; i++ ) {
		if ( i == largest ) continue;
		float v = (q[i] * sign / QUAT_RANGE + 1.0F) * 0.5F * QUAT_MAX;
		int iv = (int) (v + 0.5F);
		if ( iv < 0 ) iv = 0;
		if ( iv > QUAT_MAX ) iv = QUAT_MAX;
		packed |= (uint32_t) iv << shift;
		shift -= QUAT_BITS;
	}
	w.put<uint32_t>( packed );

	for ( int j = 0; j < 3; j++ ) w.put<int16_t>( ToInt16( a[3][j], TRANSLATION_STEP ) );

	w.put<uint16_t>( (uint16_t) (PackScale( len[0] ) | (mirrored ? SCALE_MIRRORED : 0)) );
	if ( perAxis ) {
		w.put<uint16_t>( PackScale( len[1] ) );
		w.put<uint16_t>( PackScale( len[2] ) );
	}
}

static void GetMatrix( Reader& r, Matrix4f* out, bool perAxis )
{
	uint32_t packed = r.get<uint32_t>();
	int16_t t[3];
	for ( int j = 0; j < 3; j++ ) t[j] = r.get<int16_t>();
	uint16_t sv = r.get<uint16_t>();
	float scale[3];
	scale[0] = (float) (sv & 0x7FFF) * SCALE_STEP;
	scale[1] = perAxis ? (float) (r.get<uint16_t>() & 0x7FFF) * SCALE_STEP : scale[0];
	scale[2] = perAxis ? (float) (r.get<uint16_t>() & 0x7FFF) * SCALE_STEP : scale[0];
	if ( sv & SCALE_MIRRORED ) for ( int i = 0; i < 3; i++ ) scale[i] = -scale[i];

	float q[4];
	int largest = (int) (packed >> 30);
	int shift = 20;
	float sum = 0.0F;
	for ( int i = 0; i < 4; i++ ) {
		if ( i == largest ) continue;
		int iv = (int) ((packed >> shift) & QUAT_MAX);
		q[i] = ((float) iv / QUAT_MAX * 2.0F - 1.0F) * QUAT_RANGE;
		sum += q[i] * q[i];
		shift -= QUAT_BITS;
	}
	q[largest] = sum < 1.0F ? sqrtf( 1.0F - sum ) : 0.0F;

	float x = q[0], y = q[1], z = q[2], wq = q[3];
	float (*a)[4] = out->data;
	a[0][0] = (1.0F - 2.0F * (y * y + z * z)) * scale[0];
	a[0][1] = 2.0F * (x * y - z * wq) * scale[0];
	a[0][2] = 2.0F * (x * z + y * wq) * scale[0];
	a[1][0] = 2.0F * (x * y + z * wq) * scale[1];
	a[1][1] = (1.0F - 2.0F * (x * x + z * z)) * scale[1];
	a[1][2] = 2.0F * (y * z - x * wq) * scale[1];
	a[2][0] = 2.0F * (x * z - y * wq) * scale[2];
	a[2][1] = 2.0F * (y * z + x * wq) * scale[2];
	a[2][2] = (1.0F - 2.0F * (x * x + y * y)) * scale[2];
	for ( int j = 0; j < 3; j++ ) {
		a[3][j] = t[j] * TRANSLATION_STEP;
		a[j][3] = 0.0F;
	}
	a[3][3] = 1.0F;
}

static void PutFullMatrix( Writer& w, const Matrix4f& m )
{
	for ( int i = 0; i < 4; i++ )
		for ( int j = 0; j < 3; j++ )
			w.put<float>( m.data[i][j] );
}

static void GetFullMatrix( Reader& r, Matrix4f* m )
{
	for ( int i = 0; i < 4; i++ ) {
		for ( int j = 0; j < 3; j++ ) m->data[i][j] = r.get<float>();
		m->data[i][3] = i == 3 ? 1.0F : 0.0F;
	}
}

float PackingError( const Matrix4f& m, bool perAxis )
{
	uint8_t buf[PACKED_MATRIX_SIZE];
	Writer w = { buf, buf + sizeof(buf), true };
	PutMatrix( w, m, perAxis );
	Reader r = { buf, buf + sizeof(buf), true };
	Matrix4f back;
	GetMatrix( r, &back, perAxis );
	float err = 0.0F;
	for ( int i = 0; i < 4; i++ )
		for ( int j = 0; j < 4; j++ ) {
			float d = fabsf( back.data[i][j] - m.data[i][j] );
			if ( d > err ) err = d;
		}
	return err;
}


// --------------------------------------------------------------------------------
// Packets.
// --------------------------------------------------------------------------------
static void PutHeader( Writer& w, uint8_t type, uint8_t player, uint32_t sequence )
{
	w.put<uint16_t>( MAGIC );
	w.put<uint8_t>( VERSION );
	w.put<uint8_t>( type );
	w.put<uint8_t>( player );
	w.put<uint32_t>( sequence );
}

bool DecodeHeader( const uint8_t* in, int length, PacketHeader* header )
{
	Reader r = { in, in + length, true };
	if ( r.get<uint16_t>() != MAGIC || r.get<uint8_t>() != VERSION ) return false;
	header->type = r.get<uint8_t>();
	header->player = r.get<uint8_t>();
	header->sequence = r.get<uint32_t>();
	return r.ok;
}

static const uint8_t FLAG_ON_GROUND = 1;

int EncodeSnapshot( const PlayerSnapshot& s, uint8_t player, uint32_t sequence, uint8_t* out, int capacity )
{
	if ( capacity > MAX_PACKET ) capacity = MAX_PACKET;
	if ( s.bones < 0 || s.bones > MAX_CHAR_BONES || s.itemBones < 0 || s.itemBones > MAX_ITEM_BONES ) return 0;

	Writer w = { out, out + capacity, true };
	PutHeader( w, PACKET_SNAPSHOT, player, sequence );
	w.put<uint32_t>( s.time );
	w.put<uint32_t>( s.level );
	PutFullMatrix( w, s.transform );   // Full precision : where the player is.
	for ( int j = 0; j < 3; j++ ) w.put<float>( s.position.data[j] );
	for ( int j = 0; j < 3; j++ ) w.put<float>( s.rotationEuler.data[j] );
	w.put<uint8_t>( s.onGround ? FLAG_ON_GROUND : 0 );
	w.put<float>( s.lift );
	w.put<uint32_t>( s.clipCRC );
	w.put<float>( s.clipWeight );
	w.put<int8_t>( (int8_t) s.heldItem );
	w.put<int8_t>( (int8_t) s.drawnSlot );
	w.put<uint8_t>( (uint8_t) s.itemState );
	w.put<uint8_t>( (uint8_t) s.bones );
	w.put<uint8_t>( (uint8_t) s.itemBones );
	for ( int i = 0; i < s.bones; i++ ) PutMatrix( w, s.pose[i], false );
	PutMatrix( w, s.handR, false );
	PutMatrix( w, s.handL, false );
	if ( s.drawnSlot >= 0 ) PutFullMatrix( w, s.itemLocal );   // Can be far from Harry (pickaxes).
	for ( int i = 0; i < s.itemBones; i++ ) PutMatrix( w, s.itemPose[i], true );
	return w.ok ? (int) (w.p - out) : 0;
}

bool DecodeSnapshot( const uint8_t* in, int length, PlayerSnapshot* s )
{
	Reader r = { in + HEADER_SIZE, in + length, length >= HEADER_SIZE };
	s->time = r.get<uint32_t>();
	s->level = r.get<uint32_t>();
	GetFullMatrix( r, &s->transform );
	for ( int j = 0; j < 3; j++ ) s->position.data[j] = r.get<float>();
	for ( int j = 0; j < 3; j++ ) s->rotationEuler.data[j] = r.get<float>();
	s->onGround = (r.get<uint8_t>() & FLAG_ON_GROUND) != 0;
	s->lift = r.get<float>();
	s->clipCRC = r.get<uint32_t>();
	s->clipWeight = r.get<float>();
	s->heldItem = r.get<int8_t>();
	s->drawnSlot = r.get<int8_t>();
	s->itemState = r.get<uint8_t>();
	s->bones = r.get<uint8_t>();
	s->itemBones = r.get<uint8_t>();
	if ( !r.ok || s->bones > MAX_CHAR_BONES || s->itemBones > MAX_ITEM_BONES ) return false;
	for ( int i = 0; i < s->bones; i++ ) GetMatrix( r, &s->pose[i], false );
	GetMatrix( r, &s->handR, false );
	GetMatrix( r, &s->handL, false );
	if ( s->drawnSlot >= 0 ) GetFullMatrix( r, &s->itemLocal );
	for ( int i = 0; i < s->itemBones; i++ ) GetMatrix( r, &s->itemPose[i], true );
	return r.ok;
}
