#ifndef UPDATE_H
#define UPDATE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Firmware update from a USB stick (docs/13-firmware-update.md).
 *
 * Internal flash (128 KB, 128 sectors of 1 KB; RM_AT32F415 5.1. NOT 2 KB: only
 * the 256 KB part has 2 KB sectors, and OpenOCD's "flash info" wrongly shows 2 KB):
 *   0x08000000  bootloader, 8 KB  (boot/boot.c; installed once over SWD)
 *   0x08002000  application, up to 120 KB (this firmware, linked here)
 *
 * The file UPDATE.UPD (made by tools/mkupdate.py) = upd_header_t + the
 * application binary (padded to a multiple of 4). The application checks it,
 * copies it to the STAGING area of the SPI flash (image first, verified, then
 * the header as the commit record) and restarts. At every boot the bootloader
 * compares a staged image with the installed one and installs it if they
 * differ; an interrupted install simply repeats at the next power-up.
 *
 * Pure C (no hardware): shared by the application, the bootloader and the
 * host tests.
 */

#define APP_BASE          0x08002000u
#define APP_END           0x08020000u
#define APP_MAX           (APP_END - APP_BASE)            /* 120 KB */
#define APP_SECTOR_SIZE   1024u                           /* internal flash sector */

/* SPI flash staging area: between the 166 track slots (end 0x1F2000) and the
 * self-test sector at the top (0x1FF000). 13 x 4 KB = 52 KB. */
#define UPD_STAGE_ADDR    0x1F2000u
#define UPD_STAGE_END     0x1FF000u
#define UPD_STAGE_DATA    (UPD_STAGE_ADDR + 256u)          /* image after the header */
#define UPD_IMAGE_MAX     (UPD_STAGE_END - UPD_STAGE_DATA) /* 52992 bytes */

#define UPD_FILE_NAME     "UPDATE.UPD"
#define UPD_MAGIC         "TDSFLUPD"                      /* 8 chars, no NUL */
#define UPD_BOARD         "SFRC2D.B"                      /* 8 chars, no NUL */
#define UPD_FORMAT        1u

typedef struct {
  char     magic[8];      /* UPD_MAGIC */
  char     board[8];      /* UPD_BOARD: refuses files for other hardware */
  uint32_t format;        /* UPD_FORMAT */
  uint32_t version;       /* FW_VERSION of the image (information only) */
  uint32_t length;        /* image bytes, multiple of 4, 8 .. UPD_IMAGE_MAX */
  uint32_t image_crc;     /* crc32 of the image */
  uint32_t reserved[3];   /* 0 */
  uint32_t header_crc;    /* crc32 of the 44 bytes above */
} upd_header_t;           /* 48 bytes, little-endian (as on the MCU) */

_Static_assert(sizeof(upd_header_t) == 48, "update header layout");
_Static_assert(UPD_IMAGE_MAX < APP_MAX, "a staged image always fits the app area");

/* CRC-32 (IEEE 802.3, same as zlib.crc32 / PKZIP). Start with crc = 0, feed
 * data in any number of pieces. */
uint32_t upd_crc32(uint32_t crc, const void *data, size_t len);

/* Header sane: magic, board, format, header CRC, length in range. */
bool upd_header_ok(const upd_header_t *h);

/* The image's first 8 bytes (initial SP, reset vector) look like an application
 * linked at APP_BASE: SP inside RAM, reset handler a Thumb address inside the
 * image. Guards against files built for the wrong address. */
bool upd_vectors_ok(const uint8_t first8[8], uint32_t length);

#endif /* UPDATE_H */
