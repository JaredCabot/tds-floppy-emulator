#ifndef TESTIMG_H
#define TESTIMG_H

#include <stdint.h>

/*
 * Test disk image (bring-up only, replaced by the SPI-flash buffer in Phase 1e):
 * a blank 1.44 MB FAT12 volume labelled TDSFLOPPY containing one file, TEST.BIN,
 * TESTIMG_FILE_SIZE bytes of a known pattern in consecutive clusters from
 * cluster 2 (LBA 33 on). 64 KB spans cylinders 0-4 on both heads, so reading
 * it back exercises stepping, side select, the FAT chain and data integrity.
 * The host PC verifies the bytes with tools/verify_testbin.py.
 */

#define TESTIMG_FILE_SIZE  65536u

/* byte at file offset f - every 512-byte sector differs, catching swaps */
static inline uint8_t testimg_pattern(uint32_t f)
{
  return (uint8_t)(f ^ (f >> 8) ^ (f >> 16) ^ 0x5A);
}

/* Fill dst with logical sector lba of the test image. */
void testimg_sector(uint32_t lba, uint8_t dst[512]);

#endif /* TESTIMG_H */
