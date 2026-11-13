#include "net/transport.h"

#include <Windows.h>
#include <string.h>

#include <vector>


static void NoLog( const char*, ... ) {}
void (*g_netLog)( const char* fmt, ... ) = NoLog;

uint32_t NetNowMs()
{
	static LARGE_INTEGER freq = { 0 };
	if ( !freq.QuadPart ) QueryPerformanceFrequency( &freq );
	LARGE_INTEGER now;
	QueryPerformanceCounter( &now );
	return (uint32_t) (now.QuadPart * 1000 / freq.QuadPart);
}


class LoopbackTransport : public Transport
{
public:
	explicit LoopbackTransport( const NetSimulation& sim ) : m_sim( sim ), m_random( 12345 ) {}

	void Send( const uint8_t* data, int length ) override
	{
		if ( length <= 0 ) return;
		if ( m_sim.loss > 0.0F && Random01() < m_sim.loss ) return;
		Packet p;
		p.deliverAt = NetNowMs() + m_sim.latencyMs + (m_sim.jitterMs > 0 ? (int) (Random01() * m_sim.jitterMs) : 0);
		p.data.assign( data, data + length );
		m_inFlight.push_back( p );
	}

	int Receive( uint8_t* buffer, int capacity ) override
	{
		// The earliest packet due by now (jitter can reorder them).
		uint32_t now = NetNowMs();
		int best = -1;
		for ( int i = 0; i < (int) m_inFlight.size(); i++ ) {
			if ( (int32_t) (m_inFlight[i].deliverAt - now) > 0 ) continue;
			if ( best < 0 || (int32_t) (m_inFlight[i].deliverAt - m_inFlight[best].deliverAt) < 0 ) best = i;
		}
		if ( best < 0 ) return 0;
		int length = (int) m_inFlight[best].data.size();
		if ( length > capacity ) length = 0;   // Wouldn't fit : dropped.
		else memcpy( buffer, m_inFlight[best].data.data(), length );
		m_inFlight.erase( m_inFlight.begin() + best );
		return length;
	}

	int PlayerId() const override { return 0; }
	bool Echoes() const override { return true; }

private:
	struct Packet
	{
		uint32_t deliverAt;
		std::vector<uint8_t> data;
	};

	float Random01()
	{
		m_random = m_random * 1664525u + 1013904223u;
		return (float) (m_random >> 8) / (float) (1 << 24);
	}

	NetSimulation m_sim;
	uint32_t m_random;
	std::vector<Packet> m_inFlight;
};


Transport* CreateLoopbackTransport( const NetSimulation& sim )
{
	return new LoopbackTransport( sim );
}
