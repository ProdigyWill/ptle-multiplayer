#pragma once

#include <stdint.h>
#include <string.h>


// A memory buffer output stream.
// Write operations return the number of bytes successfully written, and will never write
// if the source memory block cannot be entirely written.
class MemoryWriter
{
public:

	MemoryWriter(uint8_t* buf, uint32_t size)
		: buf(buf), size(size), ptr(0)
	{
	}

	size_t write(const void* src, size_t n)
	{
		if (size - ptr < n) return 0;

		memcpy(buf + ptr, src, n);
		ptr += n;
		return n;
	}

	template<typename T>
	size_t write(const T& t)
	{
		if (size - ptr < sizeof(T)) return 0;

		*((T*) (buf + ptr)) = t;
		ptr += sizeof(T);
		return sizeof(T);
	}

	void seek(int32_t off) { ptr += off; }

	uint32_t getPtr() const { return ptr; }


private:

	uint8_t* buf;
	uint32_t size;
	uint32_t ptr;
};
