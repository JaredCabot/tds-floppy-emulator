#ifndef SPIFLASH_H
#define SPIFLASH_H

#include <stdint.h>
#include <stdbool.h>

/*
 * SST25VF016B SPI NOR driver (U8 on SPI2). This is the 2 MB "buffer" store that
 * holds the 1.44 MB floppy image. See docs/01-hardware.md and the datasheet in
 * Datasheets/. CE# is driven as a software GPIO (PB12); SPI2 = PB13/14/15.
 *
 * Geometry: 2 MB, 4 KB sectors. Program only turns 1->0, so a byte range must be
 * erased (to 0xFF) before programming.
 */

#define SPIFLASH_SIZE          (2u * 1024u * 1024u)  /* 16 Mbit */
#define SPIFLASH_SECTOR_SIZE   4096u
#define SPIFLASH_JEDEC_ID      0x00BF2541u           /* BF=SST/Microchip,25,41 */

/* Bring up SPI2 + CE# GPIO, then clear the power-on block protection so
 * erase/program take effect. Returns true if the JEDEC ID matches. */
bool spiflash_init(void);

/* Read the 3-byte JEDEC ID as 0x00_BF_25_41. */
uint32_t spiflash_read_jedec_id(void);

/* Read len bytes starting at addr into buf. */
void spiflash_read(uint32_t addr, void *buf, uint32_t len);

/* Erase the 4 KB sector containing addr (blocks until done). */
void spiflash_erase_sector(uint32_t addr);

/* Non-blocking erase for background write-back: start it, then poll busy.
 * Every other call waits for a running erase to finish first. */
void spiflash_erase_sector_start(uint32_t addr);
bool spiflash_busy(void);

/* Erase the whole chip to 0xFF (blocks, ~50 ms). */
void spiflash_erase_chip(void);

/* Program len bytes from buf at addr. Caller must have erased first. The range
 * must not cross past end of device. Blocks until done. */
void spiflash_program(uint32_t addr, const void *buf, uint32_t len);

/* Self-test: JEDEC ID, then erase+program+read-back one scratch sector at the
 * TOP of the device (last sector, away from any real image). Returns true on
 * pass (shown by the red LED at boot). Safe to run repeatedly. */
bool spiflash_selftest(void);

/* Sticky: true once any operation timed out waiting for the chip (it stopped
 * responding). Errors are otherwise caught by read-back verification. */
bool spiflash_fault(void);

#endif /* SPIFLASH_H */
