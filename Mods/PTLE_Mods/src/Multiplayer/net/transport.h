#pragma once

// How packets leave and arrive. Used from the game thread only.

#include <stdint.h>


// Milliseconds on a steady clock (the snapshots' timestamps).
uint32_t NetNowMs();

// Where the network code logs (set by the plugin).
extern void (*g_netLog)( const char* fmt, ... );


class Transport
{
public:
	virtual ~Transport() {}

	// Sends one packet to the other players (unreliable, like UDP).
	virtual void Send( const uint8_t* data, int length ) = 0;

	// Next packet that arrived : its length, or 0 when there's none.
	virtual int Receive( uint8_t* buffer, int capacity ) = 0;

	// Called every tick (joining, keep-alive...).
	virtual void Update() {}

	// Our player id, or -1 while not in a room yet.
	virtual int PlayerId() const = 0;

	// Round trip to the server (ms), or -1 if unknown.
	virtual int RoundTripMs() const { return -1; }

	// True if every packet we send also comes back to us (the puppet shows us).
	virtual bool Echoes() const = 0;
};

// Network conditions to simulate on top of the transport.
struct NetSimulation
{
	int latencyMs;   // One way.
	int jitterMs;    // Each packet gets a random extra 0 .. jitterMs (so they can arrive out of order).
	float loss;      // Fraction of packets dropped (0 .. 1).
};

// Every packet sent comes back to us (as if the server echoed it), after the
// simulated delay : the puppet then replays the local player through the network
// code. Testing without a server.
Transport* CreateLoopbackTransport( const NetSimulation& sim );

// Through the relay server (server/relay.py) : joins the room, then exchanges
// packets with its other players. echo : the server sends ours back too (testing
// alone over the internet). server : "host:port".
Transport* CreateUdpTransport( const char* server, const char* room, const char* name, bool echo );
