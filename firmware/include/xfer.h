#ifndef XFER_H
#define XFER_H
#include <stdbool.h>
#include <stdint.h>



/*
 * File transfers between the USB stick (root directory) and the internal
 * 1.44 MB disk (the SPI-flash buffer the scope sees as fd0:). docs/11-usb.md.
 */

typedef enum {
  XFER_OK = 0,
  XFER_NO_STICK,      /* no stick, or it isn't ready */
  XFER_USB_ERROR,     /* mount / read / write on the stick failed */
  XFER_BAD_IMAGE,     /* internal disk isn't a readable FAT12 volume */
  XFER_NOTHING,       /* data in: the stick has no copyable files (disk untouched) */
  XFER_VERIFY_FAILED, /* data out: a file read back from the stick differs (disk kept) */
  XFER_BUSY,          /* the scope kept using the drive; nothing was done (try again) */
  XFER_BAD_FORMAT,    /* stick not readable: NTFS, unformatted, or not 512-byte sectors */
  XFER_CANCELLED,     /* stopped by pressing the transfer's own button again (see xfer.c) */
  XFER_STATUS,        /* update: no UPDATE.UPD, so the status report was written instead */
} xfer_result_t;

/* DATA IN (top button as installed): replace the internal disk with the next "page" of
 * the stick's root files - alphabetical, as many as fit in 1.44 MB. Repeated
 * calls page through the stick and wrap to the start. */
xfer_result_t xfer_in(void);

/* DATA OUT (bottom button as installed) = "take the disk out": copy every file on the internal
 * disk to the stick's root (overwriting same-named files, keeping the original
 * dates), read them all back and compare, and only if everything matches, erase
 * the internal disk to a blank volume ready for new data. On any failure the
 * internal disk is left untouched. */
xfer_result_t xfer_out(void);

/* Stick removed/replaced: data in starts again from the first page. */
void xfer_reset_paging(void);

/* Files copied by the last transfer (debug / tests). */
extern volatile uint32_t xfer_dbg_files, xfer_dbg_bytes;
/* SWD test hook: cancel the next transfer once xfer_dbg_bytes reaches this (0 = off) */
extern volatile uint32_t xfer_dbg_cancel_at;

/* FIRMWARE UPDATE (both buttons held 3 s): check 0:/UPDATE.UPD and stage it in
 * the SPI flash for the bootloader. XFER_OK means staged: the caller restarts.
 * XFER_NOTHING: no such file. Anything else: nothing changed. docs/13. */
xfer_result_t xfer_update(void);

/* Record a DATA IN / DATA OUT and its result for the status report. */
void xfer_note(const char *what, xfer_result_t r);

/* Startup: once the firmware installed from a staged update is running, withdraw
 * its commit record (see xfer.c). */
void xfer_update_ack(void);

/* SWD test hook: read/write len bytes at off of stick file name (<= 64 chars).
 * Returns bytes transferred, < 0 on error. */
int xfer_dbg_stick(bool write, const char *name, uint32_t off, void *buf, uint32_t len);

#endif /* XFER_H */
