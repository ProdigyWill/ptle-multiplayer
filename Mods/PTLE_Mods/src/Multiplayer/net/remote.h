#pragma once

// A remote player : the snapshots received from them, and which one to show now.
//
// Snapshots are shown a fixed delay behind the newest one received (the playback
// delay), so late, lost or reordered packets don't make the puppet stutter. The
// playback clock runs on this computer's clock and is only nudged towards
// "newest - delay", so the two computers' clocks never need to agree.

#include "net/snapshot.h"


class RemotePlayer
{
public:
	RemotePlayer() { Reset(); }

	void Reset();

	// A snapshot that arrived, with its packet's sequence number.
	void Add( const PlayerSnapshot& s, uint32_t sequence );

	// Moves the playback clock to now (ms, this computer) and returns the snapshot
	// to show, or 0 if there's none yet. Valid until the next Add() or Update().
	const PlayerSnapshot* Update( uint32_t nowMs, int delayMs );

	// Stats.
	uint32_t received;     // Packets kept.
	uint32_t late;         // Arrived after their time was shown : dropped.
	uint32_t reordered;    // Arrived after a newer one.
	uint32_t Lost() const; // Sequence numbers never received (so far).

private:
	static const int SIZE = 256;   // 4 s at 60 snapshots per second.

	struct Slot
	{
		bool valid;
		uint32_t sequence;
		PlayerSnapshot snapshot;
	};

	Slot m_slots[SIZE];
	bool m_any;
	uint32_t m_firstSequence;
	uint32_t m_newestSequence;
	uint32_t m_newestTime;
	uint32_t m_shownSequence;
	bool m_clockStarted;
	double m_playTime;       // Sender's time being shown.
	uint32_t m_lastNow;
};
