#ifndef BUFFER_H
#define BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include "fatimg.h"

/*
 * The 1.44 MB disk buffer in the SST25VF016B SPI flash (docs/10).
 *
 * Layout (2 MB, 4 KB erase sectors):
 *   0x000000  162 track slots of 12 KB, one per track (cyl*2 + head): cylinders
 *             0-80, as the TDS formats and verifies cylinder 80, one past the
 *             disk. A track is 18 x 512 = 9216 bytes, so storing one is "erase
 *             its 3 sectors, program 9216 bytes".
 *   0x1E6000  3 journal slots of 12 KB (power-safe write-back, below)
 *   0x1EF000  meta sector: the "complete image" marker
 *   0x1F0000  8 KB spare
 *   0x1F2000  firmware-update staging (update.h); 0x1FF000 self-test sector
 *
 * Power-safe write-back. Erasing a slot destroys the old track before the new
 * one is in, so a power cut at that moment would lose the whole track (and
 * track 0 holds the boot sector and both FATs). So each track is first written
 * to the next journal slot (rotating over 3, to spread the wear), read back,
 * and committed by a header (magic, track, sequence number, CRC-32) written
 * last; only then is the track's own slot erased and programmed. At boot,
 * buffer_init() finds the newest committed journal entry and, if its track
 * slot does not match it (power was lost while the slot was being rewritten),
 * restores the slot from the journal. A power cut can lose only the write in
 * progress, never data already on the disk.
 *
 * Failures: every step is read back. A step that fails is retried (a journal
 * failure moves on to the next journal slot); after 3 failures the write-back
 * gives up instead of looping, and buffer_fault() reports it (the flash is
 * failing: the LED shows it).
 *
 * The marker is written last after a rebuild, so an interrupted rebuild is
 * re-formatted on the next boot instead of being served half-built. An image
 * from earlier firmware (1.0.0 and before: marker in slot 0, no journal) is
 * taken over as it is, not re-formatted.
 */

#define BUF_TRACKS        162u        /* 81 cylinders x 2 (see FLPY_CYL_LIMIT) */
#define BUF_SECTORS_PER_TRACK 18u
#define BUF_TRACK_SIZE    (BUF_SECTORS_PER_TRACK * 512u)   /* 9216 */
#define BUF_SLOT_SIZE     12288u      /* 3 erase sectors */
#define BUF_JOURNAL_ADDR  0x1E6000u
#define BUF_JOURNALS      3u
#define BUF_META_ADDR     0x1EF000u

/* Boot: restore a track whose write-back was cut short (journal); format to an
 * empty FAT12 volume if no complete image is present. */
void buffer_init(void);
/* Erase everything and write an empty FAT12 volume (~1 s). */
void buffer_format(void);

/* Whole-track access for the floppy layer. t = cyl*2 + head. */
void buffer_load_track(unsigned t, uint8_t dst[BUF_TRACK_SIZE]);
/* Background write-back of one track, so the floppy never waits for flash:
 * start it, then call buffer_wb_poll() from the main loop (a few ms per call)
 * until it returns true; buffer_wb_ok() then tells whether it was stored (false
 * only after the retries ran out). src must stay untouched until it finishes.
 * buffer_wb_finish() blocks. */
void buffer_wb_start(unsigned t, const uint8_t src[BUF_TRACK_SIZE]);
bool buffer_wb_poll(void);
bool buffer_wb_active(void);
bool buffer_wb_ok(void);
void buffer_wb_finish(void);
/* Idle work, called when the drive is not in use: erase the journal slot the
 * next write-back will use, so writing it back costs no erase time. */
void buffer_bg_poll(void);
/* Sticky: a track could not be stored, or the flash stopped responding. */
bool buffer_fault(void);

/* Sector access (LBA 0..2879) for building / reading the image over USB. */
void buffer_read_lba(uint32_t lba, uint8_t dst[512]);
void buffer_program(uint32_t lba, unsigned off, const void *src, unsigned len);  /* onto erased flash */

/* Rebuilding the whole image: begin erases it (and the marker), then build
 * with fatimg through buffer_fatimg_io, then end marks it complete. */
void buffer_begin_rebuild(void);
void buffer_end_rebuild(void);
extern const fatimg_io_t buffer_fatimg_io;

#endif /* BUFFER_H */
