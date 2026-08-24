/*
 * updcrc macro derived from article Copyright (C) 1986 Stephen Satchell. 
 *  NOTE: First srgument must be in range 0 to 255.
 *        Second argument is referenced twice.
 * 
 * Programmers may incorporate any or all code into their programs, 
 * giving proper credit within the source. Publication of the 
 * source routines is permitted so long as proper credit is given 
 * to Stephen Satchell, Satchell Evaluations
 *
 * wow ! a whole macro ! lets copyright it.....
 */

#include <stddef.h>
#include <stdint.h>

extern const uint16_t crc16tab[0x100];
extern const uint32_t crc32tab[0x100];

uint32_t crc32_update(uint32_t, const uint8_t *, size_t);

#define UPDCRC16(cp, crc) \
	((uint16_t)(crc16tab[((uint16_t)(crc) >> 8) & UINT16_C(0xff)] ^ \
	((uint32_t)(uint16_t)(crc) << 8) ^ (uint8_t)(cp)))

#define UPDCRC32(b, c) \
	(crc32tab[((uint32_t)(c) ^ (uint8_t)(b)) & UINT32_C(0xff)] ^ \
	((uint32_t)(c) >> 8))
