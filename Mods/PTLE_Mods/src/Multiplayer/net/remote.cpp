#include "net/remote.h"
#include "net/affine.h"
#include "net/protocol.h"

#include <math.h>


static const double SNAP_DRIFT_MS = 250.0;   // Further off than this : jump to the right time.
static const double DRIFT_PULL = 1.0 / 32.0; // Else : close this part of the gap each update.
static const uint32_t MAX_BLEND_GAP_MS = 250; // Snapshots further apart (lost packets) : no blend.
static const float TELEPORT_DISTANCE = 20.0F; // Moved further between two snapshots : no blend.

static void BlendMatrix( const Matrix4f& a, const Matrix4f& b, float f, Matrix4f* out )
{
	if ( IsSaneMatrix( a ) && IsSaneMatrix( b ) ) BlendAffine( a, b, f, out );
	else *out = f < 0.5F ? a : b;
}

// A bone's matrix is a skinning matrix : it takes a vertex from where the model was
// built to where the bone carries it, so its translation depends on its rotation
// (the bone turns about its joint, not about the origin). Blending the two
// separately (BlendAffine) moved bones off their joints whenever a limb turned
// fast between two snapshots, the more so the further the bone is from the model's
// origin : hands, feet and the backpack stretched away and snapped back, worst when
// a snapshot was lost. Blending the matrix as a whole keeps every joint in one
// piece : a point shared by two bones lands at the same place through either.
static void BlendBone( const Matrix4f& a, const Matrix4f& b, float f, Matrix4f* out )
{
	if ( !IsSaneMatrix( a ) || !IsSaneMatrix( b ) ) { *out = f < 0.5F ? a : b; return; }
	for ( int i = 0; i < 4; i++ )
		for ( int j = 0; j < 4; j++ ) out->data[i][j] = a.data[i][j] + (b.data[i][j] - a.data[i][j]) * f;
}

static float BlendAngle( float a, float b, float f )   // Radians, the shorter way round.
{
	const float PI = 3.14159265F;
	float d = fmodf( b - a, 2.0F * PI );
	if ( d > PI ) d -= 2.0F * PI;
	if ( d < -PI ) d += 2.0F * PI;
	return a + d * f;
}

// a, with its continuous values moved fraction f of the way to b. What can't be
// blended (the animation clip, the item in hand) is a's.
static void BlendSnapshots( const PlayerSnapshot& a, const PlayerSnapshot& b, float f, PlayerSnapshot* out )
{
	*out = a;
	out->time = a.time + (uint32_t) ((float) (b.time - a.time) * f + 0.5F);
	BlendMatrix( a.transform, b.transform, f, &out->transform );
	for ( int j = 0; j < 3; j++ ) {
		out->position.data[j] = a.position.data[j] + (b.position.data[j] - a.position.data[j]) * f;
		out->rotationEuler.data[j] = BlendAngle( a.rotationEuler.data[j], b.rotationEuler.data[j], f );
	}
	out->lift = a.lift + (b.lift - a.lift) * f;
	if ( a.clipCRC == b.clipCRC ) out->clipWeight = a.clipWeight + (b.clipWeight - a.clipWeight) * f;
	if ( a.bones == b.bones ) {
		for ( int i = 0; i < a.bones; i++ ) BlendBone( a.pose[i], b.pose[i], f, &out->pose[i] );
	}
	BlendMatrix( a.handR, b.handR, f, &out->handR );
	BlendMatrix( a.handL, b.handL, f, &out->handL );
	bool sameItem = a.drawnSlot == b.drawnSlot && a.itemState == b.itemState && a.heldItem == b.heldItem;
	if ( sameItem && a.drawnSlot >= 0 ) BlendMatrix( a.itemLocal, b.itemLocal, f, &out->itemLocal );
	if ( sameItem && a.itemBones == b.itemBones ) {
		for ( int i = 0; i < a.itemBones; i++ ) BlendBone( a.itemPose[i], b.itemPose[i], f, &out->itemPose[i] );
	}
}

void RemotePlayer::Reset()
{
	for ( int i = 0; i < SIZE; i++ ) m_slots[i].valid = false;
	m_any = false;
	m_firstSequence = m_newestSequence = m_newestTime = m_shownSequence = 0;
	m_clockStarted = false;
	m_playTime = 0.0;
	m_lastNow = 0;
	received = late = reordered = blended = held = 0;
}

void RemotePlayer::Add( const PlayerSnapshot& s, uint32_t sequence )
{
	if ( m_any ) {
		if ( m_clockStarted && (int32_t) (sequence - m_shownSequence) <= 0 ) { late++; return; }
		if ( (int32_t) (sequence - m_newestSequence) < 0 ) reordered++;
	}
	Slot& slot = m_slots[sequence % SIZE];
	if ( slot.valid && slot.sequence == sequence ) return;   // Duplicate.
	slot.valid = true;
	slot.sequence = sequence;
	slot.snapshot = s;
	received++;

	if ( !m_any ) {
		m_any = true;
		m_firstSequence = m_newestSequence = sequence;
		m_newestTime = s.time;
	}
	else if ( (int32_t) (sequence - m_newestSequence) > 0 ) {
		m_newestSequence = sequence;
		m_newestTime = s.time;
	}
}

const PlayerSnapshot* RemotePlayer::Update( uint32_t nowMs, int delayMs )
{
	if ( !m_any ) return 0;

	double target = (double) (uint32_t) (m_newestTime - (uint32_t) delayMs);
	if ( !m_clockStarted ) {
		m_clockStarted = true;
		m_playTime = target;
	}
	else {
		m_playTime += (double) (int32_t) (nowMs - m_lastNow);
		double drift = target - m_playTime;
		if ( fabs( drift ) > SNAP_DRIFT_MS ) m_playTime = target;
		else m_playTime += drift * DRIFT_PULL;
	}
	m_lastNow = nowMs;

	// The newest snapshot recorded at or before the playback time, and the first
	// one after it.
	uint32_t play = (uint32_t) (int64_t) m_playTime;
	const Slot* best = 0;
	const Slot* next = 0;
	for ( int i = 0; i < SIZE; i++ ) {
		const Slot& s = m_slots[i];
		if ( !s.valid || (int32_t) (m_newestSequence - s.sequence) >= SIZE ) continue;
		if ( (int32_t) (s.snapshot.time - play) > 0 ) {
			if ( !next || (int32_t) (s.snapshot.time - next->snapshot.time) < 0 ) next = &s;
			continue;
		}
		if ( !best || (int32_t) (s.sequence - best->sequence) > 0 ) best = &s;
	}
	if ( !best ) return 0;
	m_shownSequence = best->sequence;

	const PlayerSnapshot& a = best->snapshot;
	if ( !next ) { held++; return &a; }
	const PlayerSnapshot& b = next->snapshot;
	uint32_t gap = b.time - a.time;
	float dx = b.position.x - a.position.x, dy = b.position.y - a.position.y, dz = b.position.z - a.position.z;
	if ( gap == 0 || gap > MAX_BLEND_GAP_MS || a.level != b.level
		|| dx * dx + dy * dy + dz * dz > TELEPORT_DISTANCE * TELEPORT_DISTANCE ) {
		return &a;
	}
	float f = (float) ((m_playTime - (double) a.time) / (double) gap);
	if ( f < 0.0F ) f = 0.0F;
	if ( f > 1.0F ) f = 1.0F;
	BlendSnapshots( a, b, f, &m_blend );
	blended++;
	return &m_blend;
}

uint32_t RemotePlayer::Lost() const
{
	if ( !m_any ) return 0;
	uint32_t expected = m_newestSequence - m_firstSequence + 1;
	return expected > received ? expected - received : 0;
}
