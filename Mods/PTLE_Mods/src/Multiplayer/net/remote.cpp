#include "net/remote.h"

#include <math.h>


static const double SNAP_DRIFT_MS = 250.0;   // Further off than this : jump to the right time.
static const double DRIFT_PULL = 1.0 / 32.0; // Else : close this part of the gap each update.

void RemotePlayer::Reset()
{
	for ( int i = 0; i < SIZE; i++ ) m_slots[i].valid = false;
	m_any = false;
	m_firstSequence = m_newestSequence = m_newestTime = m_shownSequence = 0;
	m_clockStarted = false;
	m_playTime = 0.0;
	m_lastNow = 0;
	received = late = reordered = 0;
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

	// The newest snapshot recorded at or before the playback time.
	uint32_t play = (uint32_t) (int64_t) m_playTime;
	const Slot* best = 0;
	for ( int i = 0; i < SIZE; i++ ) {
		const Slot& s = m_slots[i];
		if ( !s.valid || (int32_t) (m_newestSequence - s.sequence) >= SIZE ) continue;
		if ( (int32_t) (s.snapshot.time - play) > 0 ) continue;
		if ( !best || (int32_t) (s.sequence - best->sequence) > 0 ) best = &s;
	}
	if ( !best ) return 0;
	m_shownSequence = best->sequence;
	return &best->snapshot;
}

uint32_t RemotePlayer::Lost() const
{
	if ( !m_any ) return 0;
	uint32_t expected = m_newestSequence - m_firstSequence + 1;
	return expected > received ? expected - received : 0;
}
