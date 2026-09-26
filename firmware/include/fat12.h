#ifndef FAT12_H
#define FAT12_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Blank 1.44 MB FAT12 volume, generated on demand one 512-byte LBA at a time
 * (pure/host-testable - no storage of the whole 1.44 MB). Used to present an
 * empty, mountable fd0 to the scope in Phase 1c before the USB/buffer path
 * exists. Later phases overlay real buffer contents; this stays the fallback
 * "freshly formatted disk" generator.
 *
 * Geometry (standard 1.44 MB): 512 B/sector, 2880 sectors, 2 FATs x 9 sectors,
 * 224 root entries (14 sectors), media 0xF0, 18 sec/track, 2 heads.
 *   LBA 0        boot sector (BPB)
 *   LBA 1..9     FAT1
 *   LBA 10..18   FAT2
 *   LBA 19..32   root directory (14 sectors)
 *   LBA 33..2879 data (all zero on a blank volume)
 */

#define FAT12_TOTAL_SECTORS  2880u
#define FAT12_SECTOR_SIZE    512u

/* Fill out[512] with the contents of logical sector 'lba' of a blank volume.
 * label is an 11-char (padded) volume label; NULL -> "NO NAME    ". */
void fat12_blank_sector(uint32_t lba, uint8_t out[FAT12_SECTOR_SIZE], const char *label);

/* The boot sector (BPB) alone: 1.44 MB HD as above, or 720 KB DD (1440
 * sectors, 2 per cluster, 2 FATs x 3 sectors, 112 root entries, media 0xF9,
 * 9 sectors/track). */
void fat12_boot_sector(uint8_t out[FAT12_SECTOR_SIZE], bool dd, const char *label);

#endif /* FAT12_H */
