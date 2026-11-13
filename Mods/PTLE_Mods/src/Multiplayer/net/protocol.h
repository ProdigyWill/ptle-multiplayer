#pragma once

// Wire format of the multiplayer packets (UDP-sized : always under MAX_PACKET bytes).
//
// Every packet starts with a header :
//   u16 magic 'PM', u8 version, u8 type, u8 player id, u32 sequence number.
// Snapshot packets (type PACKET_SNAPSHOT) then hold one PlayerSnapshot, with every
// bone matrix packed into 12 bytes : rotation as a "smallest three" quaternion
// (32 bits), translation as 3 x int16 (1/512 units, +-64) and a uniform scale
// (int16, its top bit for mirrored matrices; items' bones have one per axis, they
// squash and stretch). Matrices with garbage in them (items never posed yet) go as
// scale 0. Harry's transform and the drawn item's matrix go
// at full precision. A full snapshot of Harry with no item out is about 1 KB.

#include "net/snapshot.h"


static const int MAX_PACKET = 1400;   // Under the usual internet MTU (1500, minus IP and UDP headers).
static const int PACKET_HEADER_SIZE = 9;

// The relay server's control packets are described in server/relay.py. Every other
// type is forwarded to the room's other players, the sender's id stamped in.
enum PacketType : uint8_t
{
	PACKET_SNAPSHOT = 1,
	PACKET_JOIN     = 2,   // u8 flags (JOIN_ECHO), u8 name length, name, u8 room length, room.
	PACKET_WELCOME  = 3,   // Our player id in the header, u8 max players.
	PACKET_REJECT   = 4,   // u8 reason (REJECT_ROOM_FULL).
	PACKET_LEAVE    = 5,   // To the server : we leave. From it : that player left.
	PACKET_PING     = 6,   // Payload sent back as is in a PONG.
	PACKET_PONG     = 7,
};

static const uint8_t JOIN_ECHO = 1;          // The server sends our packets back to us too.
static const uint8_t REJECT_ROOM_FULL = 1;

struct PacketHeader
{
	uint8_t type;
	uint8_t player;
	uint32_t sequence;
};

// Returns the packet size, or 0 if the snapshot doesn't fit (too many bones).
int EncodeSnapshot( const PlayerSnapshot& s, uint8_t player, uint32_t sequence, uint8_t* out, int capacity );

// Writes a packet header (PACKET_HEADER_SIZE bytes).
void EncodeHeader( uint8_t type, uint8_t player, uint32_t sequence, uint8_t* out );

// Reads a packet's header. False if it isn't one of ours (or another version).
bool DecodeHeader( const uint8_t* in, int length, PacketHeader* header );

// Reads a snapshot packet. False if it's malformed.
bool DecodeSnapshot( const uint8_t* in, int length, PlayerSnapshot* s );

// Largest difference between a matrix element and its packed-unpacked copy
// (for tests : how much the packing loses).
float PackingError( const Matrix4f& m, bool perAxis );

// False if the matrix holds garbage (huge values, NaN) : it's sent as nothing.
bool IsSaneMatrix( const Matrix4f& m );
