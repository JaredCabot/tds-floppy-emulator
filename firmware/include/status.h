#ifndef STATUS_H
#define STATUS_H
/*
 * status.h - the text of EMUSTAT.TXT: the report the emulator writes to a
 * USB flash drive when both buttons are held and there is no UPDATE.UPD
 * (docs/13). Pure C, host-tested: xfer.c gathers the values, this formats them.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STATUS_FILE_NAME "EMUSTAT.TXT"

typedef struct {
  const char *version;          /* "1.2.0" */
  uint32_t    build;            /* build ID: CRC-32 of the image */
  const char *board;            /* "SFRC2D.B" */
  uint32_t    uid[3];           /* MCU unique ID (96 bits) */
  uint32_t    flash_id;         /* SPI flash JEDEC ID */
  bool        hick;             /* running on the internal oscillator (crystal failed) */
  bool        dd;               /* internal disk: 720 KB DD, else 1.44 MB HD */
  bool        disk_ok;          /* the internal disk could be read */
  uint32_t    files, dirs;      /* on the internal disk, folders included */
  uint32_t    used_bytes;       /* total size of the files */
  uint32_t    free_bytes;       /* free clusters x cluster size */
  bool        fault;            /* flash fault recorded */
  uint32_t    writebacks;       /* since power-on */
  uint32_t    recovered;        /* tracks restored from the journal at boot */
  uint32_t    repaired;         /* marker repairs at boot */
  uint32_t    reset_cause;      /* CRM ctrlsts at boot */
  uint32_t    uptime_ms;
  const char *stick_fs;         /* "FAT32", "exFAT", ... */
  uint64_t    stick_bytes;      /* capacity */
  const char *last_what;        /* "DATA IN" / "DATA OUT", or NULL: none since power-on */
  const char *last_result;      /* "OK", "cancelled", ... */
  uint32_t    last_files, last_bytes;
} status_t;

/* Write the report into out (NUL-terminated, CRLF lines). Never writes more
 * than cap bytes; returns the length (without the NUL). */
size_t status_text(char *out, size_t cap, const status_t *s);

/* The reset cause as words: "watchdog", "software (update or restart)",
 * "power-on", "reset pin" or "unknown". */
const char *status_reset_text(uint32_t reset_cause);

#endif /* STATUS_H */
