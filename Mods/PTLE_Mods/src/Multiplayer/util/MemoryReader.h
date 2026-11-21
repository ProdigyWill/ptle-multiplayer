#pragma once

#include <stdint.h>
#include <string.h>


// A memory buffer input stream.
// The templated read operation allows differing the amount of read data and
// the size of the destination by automatically performing a conversion.
class MemoryReader
{
public:

	MemoryReader(const uint8_t* buf, uint32_t size)
		: buf(buf), size(size), ptr(0)
	{
	}

	size_t read(void* dst, size_t n)
	{
		if (size - ptr < n) return 0;

		memcpy(dst, buf + ptr, n);
		ptr += n;
		return n;
	}

	template<typename T, typename D>
	size_t read(D& out)
	{
		if (size - ptr < sizeof(T)) return 0;

		out = (D) *((T*) (buf + ptr));
		ptr += sizeof(T);
		return sizeof(T);
	}

	void seek(int32_t off) { ptr += off; }

	const uint8_t* getDataPtr() const { return buf + ptr; }
	uint32_t getPtr() const { return ptr; }


private:

	const uint8_t* buf;
	uint32_t size;
	uint32_t ptr;
};
