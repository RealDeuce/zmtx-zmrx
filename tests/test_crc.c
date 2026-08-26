#include "plat.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "crctab.h"

static uint32_t
reference_crc32(uint32_t crc,const uint8_t * data,size_t length)

{
	while (length-- > 0) {
		crc = crc32_byte_update(crc,*data);
		data += 1U;
	}
	return crc;
}

int
main(void)

{
	static const uint16_t initial_states16[] = {
		UINT16_C(0), UINT16_C(1), UINT16_C(0x1234), UINT16_MAX
	};
	static const uint32_t initial_states[] = {
		UINT32_C(0), UINT32_C(1), UINT32_C(0x12345678), UINT32_MAX
	};
	uint8_t data[1032];
	uint32_t state = UINT32_C(0x6d2b79f5);
	size_t i;
	size_t initial;
	size_t length;
	size_t offset;

	for (i=0;i<sizeof(data);i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		data[i] = (uint8_t)state;
	}

	for (initial=0;initial<sizeof(initial_states) / sizeof(initial_states[0]);
	    initial++) {
		for (offset=0;offset<8;offset++) {
			for (length=0;length<=1024;length++) {
				uint32_t expected = reference_crc32(initial_states[initial],
				    data + offset,length);
				uint32_t actual = crc32_update(initial_states[initial],
				    data + offset,length);

				if (actual != expected) {
					fprintf(stderr,"CRC mismatch: initial=%" PRIx32
					    " offset=%zu length=%zu expected=%" PRIx32
					    " actual=%" PRIx32 "\n",initial_states[initial],
					    offset,length,expected,actual);
					return 1;
				}
			}
		}
	}
	for (initial=0;initial<sizeof(initial_states16) /
	    sizeof(initial_states16[0]);initial++) {
		for (offset=0;offset<8;offset++) {
			for (length=0;length<=1024;length++) {
				uint16_t expected = initial_states16[initial];
				uint16_t actual;

				for (i=0;i<length;i++) {
					expected = crc16_update(expected,data[offset + i]);
				}
				actual = crc16_buffer_update(initial_states16[initial],
				    data + offset,length);
				if (actual != expected) {
					fprintf(stderr,"CRC16 mismatch: initial=%" PRIx16
					    " offset=%zu length=%zu expected=%" PRIx16
					    " actual=%" PRIx16 "\n",
					    initial_states16[initial],offset,length,
					    expected,actual);
					return 1;
				}
			}
		}
	}
	return 0;
}
