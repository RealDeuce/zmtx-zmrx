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

#ifndef CRCTAB_H_INCLUDED
#define CRCTAB_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

extern const uint16_t crc16tab[0x100];
extern const uint32_t crc32tab[0x100];

uint32_t crc32_update(uint32_t, const uint8_t *, size_t);
uint16_t crc16_update(uint16_t, uint8_t);
uint16_t crc16_buffer_update(uint16_t, const uint8_t *, size_t);
uint32_t crc32_byte_update(uint32_t, uint8_t);

#endif
