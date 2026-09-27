#ifndef FLOPPY_H
#define FLOPPY_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Floppy-bus slave: presents the board to the host (TDS scope) as a 1.44 MB
 * drive on the 26-pin FFC, reading and writing the disk image held in the SPI
 * flash buffer (buffer.h). Hard real-time parts run from interrupts; see the
 * header comment in floppy.c and docs/09 for the behaviour.
 *
 * Output sense: MCU pin LOW -> 74AHC04 -> NPN pulls the host line LOW
 * (asserted). Geometry: 80 cylinders, 2 heads, 18 x 512 B, 300 RPM.
 */

#define FLPY_CYLINDERS   80u      /* the disk (what the file system uses) */
/* How far the head can step. Real 3.5" mechanisms reach a few cylinders past
 * 79, and the TDS relies on it: its format covers cylinders 0..80 (two passes,
 * 40->80 then 40->0) and verifies cylinder 80. Clamping at 79 made it fail.
 * It never goes further, so the head stops at 80, like a mechanism's end stop:
 * the space of cylinders 81-82 holds the write-back journal (buffer.h). */
#define FLPY_CYL_LIMIT   81u
#define FLPY_HEADS       2u
#define FLPY_SECTORS     18u
#define FLPY_SECTOR_SIZE 512u

/* Set up the bus. Call after buffer_init(). */
void flpy_init(void);

/* Call continuously from the main loop: side select, loading the track under
 * the head, and writing dirty tracks back to flash. */
void flpy_poll(void);

/* Present "no disk" / a changed disk around USB transfers of the buffer. */
/* flpy_eject() returns false (and changes nothing) if the host keeps using the
 * drive: it waits for 1.5 s of host inactivity, at most 15 s. */
/* Milliseconds since boot (SysTick); runs during blocking transfers too. */
extern volatile uint32_t flpy_dbg_ms;
/* Disk density, 720 KB DD or 1.44 MB HD: a property of the disk (buffer.h).
 * The drive follows it; a write at the other data rate (the host's density
 * line, jumper JE, says the other) switches it, as formatting a real disk in
 * the other density would. docs/10. */
void flpy_set_density(bool dd);
/* Ask for a switch; flpy_poll() makes it between writes (test hooks use this). */
void flpy_request_density(bool dd);
bool flpy_is_dd(void);
bool flpy_eject(bool (*abort)(void));   /* abort (may be NULL): stop waiting early */
void flpy_insert(void);
/* No disk at all, at once (boot: the buffer flash failed its self-test). */
void flpy_no_disk(void);
/* Host not using the drive and nothing waiting to be written back. */
bool flpy_idle(void);

/* 9 KB of RAM (the spare track buffer) that other code may borrow while the
 * disk is ejected, or before flpy_init(); NULL otherwise. */
#define FLPY_SCRATCH_SIZE 9216u
void *flpy_scratch(void);

uint8_t flpy_current_cyl(void);
uint8_t flpy_current_head(void);
bool    flpy_selected(void);

#endif /* FLOPPY_H */
