#ifndef FAT12_H
#define FAT12_H

#include <stdint.h>
#include <stdbool.h>

/* Boot sector (BPB) of a blank floppy volume, as a FAT12 driver expects it:
 *   HD, 1.44 MB: 2880 sectors, 1 per cluster, 2 FATs x 9 sectors, 224 root
 *                entries (14 sectors), media 0xF0, 18 sectors/track, 2 heads
 *   DD, 720 KB:  1440 sectors, 2 per cluster, 2 FATs x 3 sectors, 112 root
 *                entries, media 0xF9, 9 sectors/track
 * fatimg.c lays out the rest of the volume (FATs, root, data). */
#define FAT12_SECTOR_SIZE    512u

/* label: an 11-char (padded) volume label, or NULL for "NO NAME    ". */
void fat12_boot_sector(uint8_t out[FAT12_SECTOR_SIZE], bool dd, const char *label);

#endif /* FAT12_H */
