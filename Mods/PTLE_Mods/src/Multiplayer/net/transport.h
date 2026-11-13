#pragma once

// How packets leave and arrive. Used from the game thread only.

#include <stdint.h>


// Milliseconds on a steady clock (the snapshots' timestamps).
uint32_t NetNowMs();


class Transport
{
public:
	virtual ~Transport() {}

	// Sends one packet to the other players (unreliable, like UDP).
	virtual void Send( const uint8_t* data, int length ) = 0;

	// Next packet that arrived : its length, or 0 when there's none.
	virtual int Receive( uint8_t* buffer, int capacity ) = 0;
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
