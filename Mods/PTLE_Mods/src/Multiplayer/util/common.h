#pragma once

#include <stdint.h>


#define countof(arr) (sizeof(arr) / sizeof(arr[0]))

#define FIELD( obj, type, offset ) (*(type*)((uint8_t*)(obj) + (offset)))
