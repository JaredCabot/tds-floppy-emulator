/**
 * xfer.c - stick <-> internal disk file transfers (see xfer.h, docs/11-usb.md).
 * FatFs drive "0:" is the stick; the internal disk is built/read with fatimg
 * through the SPI-flash buffer. The floppy is "ejected" for the duration, so
 * the scope sees no disk, then a disk change.
 */
#include "xfer.h"
#include "status.h"
#include "version.h"
#include "usbhost.h"
#include "buffer.h"
#include "floppy.h"
#include "fatimg.h"
#include "update.h"
#include "spiflash.h"
#include "board.h"
#include "buttons.h"
#include "clock.h"
#include "ff.h"
#include <string.h>

/* the largest file the internal disk holds: 1.44 MB HD, or 720 KB DD */
static uint32_t disk_bytes(void) { return fatimg_capacity(buffer_is_dd()); }

volatile uint32_t xfer_dbg_files, xfer_dbg_bytes;
/* The last transfer's time, in ms, by where it went (SWD; tools/button.ps1):
 * waiting for the host to leave the drive, the flash drive (f_read/f_write),
 * and the internal flash (programming, verifying, reading, erasing). */
volatile uint32_t xfer_dbg_ms_wait, xfer_dbg_ms_usb, xfer_dbg_ms_flash;
#define TIC(t)      uint32_t t = flpy_dbg_ms
#define TOC(t, acc) ((acc) += flpy_dbg_ms - (t))
static void timing_reset(void) { xfer_dbg_ms_wait = xfer_dbg_ms_usb = xfer_dbg_ms_flash = 0; }

static FATFS   s_fs;
static FIL     s_fil;
static DIR     s_dir;
/* Working memory for a transfer, borrowed from the floppy while it is ejected
 * (flpy_scratch): the FAT12 builder or reader, one sector buffer, and for DATA
 * OUT the stick path and the stack of folders being walked. */
#define XFER_DEPTH     8u                  /* folder levels copied (root = 0) */
/* USB data moves in chunks of up to XBUF bytes: one mass-storage command for 16
 * sectors instead of 16. The chunk lives in the floppy's second track buffer,
 * idle while the disk is ejected (flpy_scratch2). */
#define XBUF           8192u
_Static_assert(XBUF <= FLPY_SCRATCH_SIZE, "chunk fits a track buffer");
static uint8_t *s_xbuf;
#define XFER_NAME_MAX  17u                 /* "NAME_999.EXT/" + slack */
#define XFER_PATH_MAX  (3u + XFER_DEPTH * XFER_NAME_MAX + XFER_NAME_MAX + 1u)
typedef struct {
  union { fatimg_build_t b; fatimg_read_t r; };
  uint8_t sec[512];
  union {                                            /* read-back from the stick (verify), */
    uint8_t chk[512];                                /* DATA IN: the path, DATA OUT: FILINFO */
    FILINFO fi;
  };
  char path[XFER_PATH_MAX];                          /* DATA OUT: "0:/DIR/SUB/NAME.EXT" */
  fatimg_dir_t dirs[XFER_DEPTH];                     /* DATA OUT: folders being walked */
  uint8_t plen[XFER_DEPTH];                          /* ... and where each one's path ends */
  char page_start[FF_LFN_BUF + 1];                   /* DATA IN: s_last when the page began */
} xfer_ws_t;
_Static_assert(sizeof(xfer_ws_t) <= FLPY_SCRATCH_SIZE, "transfer workspace must fit the floppy scratch");
static xfer_ws_t *s_ws;
#define s_sec (s_ws->sec)
/* last file copied in ("" = from the start); a full name: exFAT has only long ones */
static char    s_last[FF_LFN_BUF + 1];

void xfer_reset_paging(void) { s_last[0] = 0; }

/* A stick file's name for DATA IN: its short (8.3) name where the stick has one
 * (FAT: FatFs puts it in altname when the file also has a long name or
 * lower-case flags, else the short name is in fname), otherwise its long name
 * (exFAT keeps no short names; altname is empty). Also the paging order. */
static const char *sfn(const FILINFO *fi) { return fi->altname[0] ? fi->altname : fi->fname; }

/* 8.3 name for the internal disk: the name itself if it is 8.3, else the
 * Windows-style NAME~n, unique on the disk being built. */
static int disk_name(const fatimg_build_t *b, const char *name, char n83[11])
{
  if(fatimg_short_name(name, 0, n83) && !fatimg_name_used(b, n83)) return 1;
  for(unsigned n = 1; n < 100; n++)
    if(fatimg_short_name(name, n, n83) && !fatimg_name_used(b, n83)) return 1;
  return 0;
}

/* Mount the stick. Anything FatFs cannot read (NTFS, no file system, logical
 * blocks other than 512 bytes) is XFER_BAD_FORMAT: the user can reformat it
 * as FAT32 or exFAT. */
static xfer_result_t mount_stick(void)
{
  if(!usbh_app_ready()) return XFER_NO_STICK;
  if(usbh_app_block_size() != 512) return XFER_BAD_FORMAT;
  FRESULT fr = f_mount(&s_fs, "0:", 1);
  if(fr == FR_OK) return XFER_OK;
  return fr == FR_NO_FILESYSTEM ? XFER_BAD_FORMAT : XFER_USB_ERROR;
}

/* ---- cancel ----
 * Pressing the button that started a transfer again (held CANCEL_PRESS_MS)
 * while the LED flashes stops it at the next safe point: between sectors, or
 * while waiting for the scope. The copy loops ask cancelled() once per sector.
 *   DATA OUT: any partial copy is deleted, files already copied stay on the
 *     stick, and the internal disk is NOT erased. A later DATA OUT recognises
 *     the files already there (no duplicates).
 *   DATA IN: the disk keeps the files loaded so far (a partial page) and the
 *     next press loads that page again. The disk's previous contents cannot
 *     come back: DATA IN erases the disk before it starts.
 *   The firmware update is not cancellable (about 5 s, strictly ordered). */
#define CANCEL_PRESS_MS 50u
volatile uint32_t xfer_dbg_cancel_at;
static uint16_t s_cancel_btn;
static uint32_t s_press_ms;
static bool s_cancelled;

static void cancel_arm(uint16_t btn) { s_cancel_btn = btn; s_press_ms = 0; s_cancelled = false; }

static bool cancelled(void)
{
  if(s_cancelled || !s_cancel_btn) return s_cancelled;
  if(xfer_dbg_cancel_at && xfer_dbg_bytes >= xfer_dbg_cancel_at) s_cancelled = true;
  else if(buttons_raw() & s_cancel_btn)
  {
    if(!s_press_ms) s_press_ms = flpy_dbg_ms | 1u;
    else if(flpy_dbg_ms - s_press_ms >= CANCEL_PRESS_MS) s_cancelled = true;
  }
  else s_press_ms = 0;
  return s_cancelled;
}

/* A failure that a cancel caused reports as the cancel. */
static xfer_result_t why(xfer_result_t r) { return s_cancelled ? XFER_CANCELLED : r; }

/* Case-insensitive names (exFAT sticks may say "update.upd"), and the order
 * DATA IN pages through them: alphabetical regardless of case, as Windows
 * lists them (FAT short names are upper case already; exFAT long names are not,
 * and a plain strcmp would put "Zebra" before "apple"). ASCII letters only. */
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int name_cmp(const char *a, const char *b)
{
  for(; *a && lower(*a) == lower(*b); a++, b++) { }
  return (unsigned char)lower(*a) - (unsigned char)lower(*b);
}
static int same_name(const char *a, const char *b) { return name_cmp(a, b) == 0; }

/* Alphabetically next regular file after 'after' in the stick's root. */
static int next_file(const char *after, FILINFO *best)
{
  static FILINFO fi;                  /* static: ~600 B kept off the 4 KB stack. Not reentrant: */
                                      /* transfers run only from the main loop, one at a time */
  int found = 0;
  if(f_opendir(&s_dir, "0:/") != FR_OK) return 0;
  while(f_readdir(&s_dir, &fi) == FR_OK && fi.fname[0])
  {
    if(fi.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
    if(fi.fname[0] == '.') continue;                /* dot files: hidden by convention (macOS ._*) */
    if(same_name(fi.fname, UPD_FILE_NAME)) continue; /* the firmware update is not a scope file */
    if(same_name(fi.fname, STATUS_FILE_NAME)) continue;   /* nor is the status report */
    /* short (8.3) names throughout: all the scope can use, and FatFs keeps them in
     * altname even when the file has a long name (e.g. TEK00000_1.BMP -> TEK000~1.BMP) */
    if(fi.fsize > disk_bytes() || name_cmp(sfn(&fi), after) <= 0) continue;
    if(!found || name_cmp(sfn(&fi), sfn(best)) < 0) { *best = fi; found = 1; }
  }
  f_closedir(&s_dir);
  return found;
}

/* Copy one stick file onto the disk being built. Every sector is read back
 * from the flash and compared as it is written. */
static xfer_result_t copy_in(fatimg_build_t *b, const FILINFO *fi, const char n83[11])
{
  char *path = (char *)s_ws->chk;                   /* "0:/" + name (long on exFAT); disk ejected */
  uint32_t lba;
  UINT got;
  strcpy(path, "0:/");
  strcat(path, sfn(fi));
  if(!fatimg_alloc(b, fi->fsize, &lba)) return XFER_USB_ERROR;
  if(f_open(&s_fil, path, FA_READ) != FR_OK) return XFER_USB_ERROR;   /* (path no longer needed) */
  for(uint32_t left = fi->fsize; left; )
  {
    if(cancelled()) { f_close(&s_fil); return XFER_CANCELLED; }   /* (not committed: not on the disk) */
    uint32_t want = left < XBUF ? left : XBUF;
    TIC(tu);
    if(f_read(&s_fil, s_xbuf, want, &got) != FR_OK || got != want) { f_close(&s_fil); return XFER_USB_ERROR; }
    TOC(tu, xfer_dbg_ms_usb);
    TIC(tf);
    for(uint32_t off = 0; off < got; off += FATIMG_SECTOR)   /* each sector verified */
    {
      uint32_t n = got - off < FATIMG_SECTOR ? got - off : FATIMG_SECTOR;
      buffer_program(lba, 0, s_xbuf + off, n);
      buffer_read_lba(lba++, s_ws->chk);
      if(memcmp(s_ws->chk, s_xbuf + off, n) != 0) { f_close(&s_fil); return XFER_VERIFY_FAILED; }
    }
    TOC(tf, xfer_dbg_ms_flash);
    left -= got;
    xfer_dbg_bytes += got;
  }
  f_close(&s_fil);
  fatimg_commit(b, n83, fi->fsize, fi->fdate, fi->ftime);   /* only now visible */
  return XFER_OK;
}

/* The finished disk reads back as built: a valid volume, the same number of
 * files, each with an intact cluster chain. */
static bool disk_ok(uint16_t nfiles)
{
  fatimg_read_t *r = &s_ws->r;                       /* (shares memory with the builder: done) */
  fatimg_file_t f;
  unsigned n = 0;
  if(!fatimg_open(r, &buffer_fatimg_io)) return false;
  while(fatimg_next_file(r, &f))
  {
    uint32_t need = fatimg_clusters_for(r, f.size);
    if(fatimg_chain_len(r, f.first_cl, need) < need) return false;
    n++;
  }
  return n == nfiles;
}

xfer_result_t xfer_in(void)
{
  static FILINFO fi;                  /* static: kept off the stack (see next_file) */
  char n83[11];
  xfer_result_t res = XFER_OK;
  xfer_dbg_files = xfer_dbg_bytes = 0;

  { xfer_result_t m = mount_stick(); if(m != XFER_OK) return m; }
  if(!next_file(s_last, &fi))                        /* end of the list: wrap around */
  {
    s_last[0] = 0;
    if(!next_file(s_last, &fi)) { f_mount(0, "0:", 0); return XFER_NOTHING; }
  }

  cancel_arm(BUTTON_RIGHT);
  timing_reset();
  TIC(tw);
  if(!flpy_eject(cancelled)) { f_mount(0, "0:", 0); cancel_arm(0); return why(XFER_BUSY); }
  TOC(tw, xfer_dbg_ms_wait);
  s_ws = flpy_scratch();
  s_xbuf = flpy_scratch2();
  strcpy(s_ws->page_start, s_last);                  /* a failed page is reloaded next time */
  fatimg_build_t *b = &s_ws->b;
  TIC(te);
  buffer_begin_rebuild();                            /* (chip erase) */
  TOC(te, xfer_dbg_ms_flash);
  fatimg_build_begin(b, &buffer_fatimg_io, buffer_is_dd());
  do {
    if(!disk_name(b, sfn(&fi), n83)) { strcpy(s_last, sfn(&fi)); continue; }   /* no free name: skip */
    if(!fatimg_fits(b, fi.fsize)) break;             /* page full: next press continues here */
    if((res = copy_in(b, &fi, n83)) != XFER_OK) break;
    strcpy(s_last, sfn(&fi));
    xfer_dbg_files++;
  } while(next_file(s_last, &fi));
  fatimg_finish(b);
  uint16_t nfiles = b->nfiles;
  if(res == XFER_OK && !disk_ok(nfiles)) res = XFER_VERIFY_FAILED;
  buffer_end_rebuild();
  if(res != XFER_OK) strcpy(s_last, s_ws->page_start);   /* try the same page again */
  f_mount(0, "0:", 0);
  flpy_insert();
  cancel_arm(0);
  return res;
}

/* ---- DATA OUT ---- */

/* Stick name of a disk entry, written at p: "NAME.EXT", or "NAME_n.EXT" for n
 * 1..999. Returns its length. */
static unsigned put_name(char *p, const char n83[11], unsigned n)
{
  unsigned k = 0;
  for(unsigned i = 0; i < 8 && n83[i] != ' '; i++) p[k++] = n83[i];
  if(n)
  {
    p[k++] = '_';
    if(n >= 100) p[k++] = (char)('0' + n / 100);
    if(n >= 10)  p[k++] = (char)('0' + n / 10 % 10);
    p[k++] = (char)('0' + n % 10);
  }
  if(n83[8] != ' ') { p[k++] = '.'; for(unsigned i = 8; i < 11 && n83[i] != ' '; i++) p[k++] = n83[i]; }
  p[k] = 0;
  return k;
}

/* Is the stick file at s_ws->path byte-for-byte the disk file f?
 * 1 = identical, 0 = different, -1 = could not read it. */
static int same_as_disk(const fatimg_file_t *f)
{
  fatimg_read_t *r = &s_ws->r;
  UINT n2;
  if(f_open(&s_fil, s_ws->path, FA_READ) != FR_OK) return -1;
  int res = (f_size(&s_fil) == f->size) ? 1 : 0;
  fatimg_pos_t pos;
  uint32_t lba, n;
  fatimg_pos_start(&pos, f);
  while(res == 1 && pos.left)                       /* the stick's copy, a chunk at a time */
  {
    uint32_t want = pos.left < XBUF ? pos.left : XBUF;
    if(cancelled()) { res = -1; break; }
    TIC(tu);
    if(f_read(&s_fil, s_xbuf, want, &n2) != FR_OK) { res = -1; break; }
    TOC(tu, xfer_dbg_ms_usb);
    if(n2 != want) { res = 0; break; }
    TIC(tf);
    for(uint32_t off = 0; off < want; off += n)
    {
      if((n = fatimg_pos_next(r, &pos, &lba)) == 0) { res = 0; break; }   /* chain short */
      buffer_read_lba(lba, s_sec);
      if(memcmp(s_sec, s_xbuf + off, n) != 0) { res = 0; break; }
    }
    TOC(tf, xfer_dbg_ms_flash);
  }
  if(res == 1 && pos.left) res = 0;
  f_close(&s_fil);
  return res;
}

/* Where a disk file goes, in the stick folder s_ws->path[0..base): its own name,
 * else NAME_1.EXT, NAME_2.EXT, ... An existing file is never replaced, but one
 * that is already an identical copy is recognised, so saving the same disk
 * twice, or retrying after a failure, makes no duplicates. */
enum { PLACE_FREE, PLACE_SAME, PLACE_ERR };
static int place_file(unsigned base, const fatimg_file_t *f)
{
  for(unsigned n = 0; n < 1000; n++)
  {
    put_name(s_ws->path + base, f->name83, n);
    FRESULT r = f_stat(s_ws->path, &s_ws->fi);
    if(r == FR_NO_FILE) return PLACE_FREE;
    if(r != FR_OK) return PLACE_ERR;
    if(!(s_ws->fi.fattrib & AM_DIR) && s_ws->fi.fsize == f->size)
    {
      int s = same_as_disk(f);                     /* (overwrites fi: read above) */
      if(s < 0) return PLACE_ERR;
      if(s == 1) return PLACE_SAME;
    }
  }
  return PLACE_ERR;
}

/* The stick folder for a disk folder: an existing folder of that name is used
 * (the contents merge); if a FILE has the name, NAME_1 ... Leaves the folder's
 * path in s_ws->path. */
static bool place_dir(unsigned base, const char n83[11])
{
  for(unsigned n = 0; n < 1000; n++)
  {
    put_name(s_ws->path + base, n83, n);
    FRESULT r = f_stat(s_ws->path, &s_ws->fi);
    if(r == FR_OK && (s_ws->fi.fattrib & AM_DIR)) return true;
    if(r == FR_NO_FILE) return f_mkdir(s_ws->path) == FR_OK;
    if(r != FR_OK) return false;
  }
  return false;
}

/* Copy the disk file f to the free stick path s_ws->path, then read it back and
 * compare. A failed or unverified copy is deleted: never a partial file. */
static xfer_result_t copy_file(const fatimg_file_t *f)
{
  fatimg_read_t *r = &s_ws->r;
  UINT n2;
  if(f_open(&s_fil, s_ws->path, FA_CREATE_NEW | FA_WRITE) != FR_OK) return XFER_USB_ERROR;
  xfer_result_t res = XFER_OK;
  fatimg_pos_t pos;
  uint32_t lba, n;
  fatimg_pos_start(&pos, f);
  for(;;)                                           /* gather up to XBUF, write it at once */
  {
    uint32_t fill = 0;
    TIC(tf);
    while(fill + FATIMG_SECTOR <= XBUF && (n = fatimg_pos_next(r, &pos, &lba)) != 0)
    {
      buffer_read_lba(lba, s_xbuf + fill);
      fill += n;
      if(n < FATIMG_SECTOR) break;                   /* the file's last sector */
    }
    TOC(tf, xfer_dbg_ms_flash);
    if(fill == 0) break;
    if(cancelled()) { res = XFER_CANCELLED; break; }
    TIC(tu);
    if(f_write(&s_fil, s_xbuf, fill, &n2) != FR_OK || n2 != fill) { res = XFER_USB_ERROR; break; }
    TOC(tu, xfer_dbg_ms_usb);
    xfer_dbg_bytes += fill;
  }
  if(f_close(&s_fil) != FR_OK) res = XFER_USB_ERROR;
  if(res == XFER_OK && pos.left) res = XFER_BAD_IMAGE;          /* chain shorter than the file */
  if(res != XFER_OK) { f_unlink(s_ws->path); return res; }
  FILINFO t = { .fdate = f->date, .ftime = f->time };
  f_utime(s_ws->path, &t);                         /* keep the scope's timestamp */
  if(same_as_disk(f) != 1) { f_unlink(s_ws->path); return why(XFER_VERIFY_FAILED); }
  return XFER_OK;
}

/* Copy the whole internal disk, folders included, to the stick (disk ejected,
 * stick mounted). A stick problem stops at once. A problem with the disk itself
 * (a corrupt file or folder, or folders nested deeper than XFER_DEPTH) skips
 * just that part, saves everything else, and returns XFER_BAD_IMAGE so the
 * caller keeps the disk: nothing is ever erased that was not saved. */
static xfer_result_t copy_out(void)
{
  fatimg_read_t *r = &s_ws->r;
  fatimg_file_t f;
  xfer_result_t kept = XFER_OK;                    /* a disk problem worked around */
  if(!fatimg_open(r, &buffer_fatimg_io)) return XFER_BAD_IMAGE;
  strcpy(s_ws->path, "0:/");
  s_ws->plen[0] = 3;
  fatimg_dir_root(&s_ws->dirs[0]);
  for(int depth = 0; depth >= 0; )
  {
    if(cancelled()) return XFER_CANCELLED;         /* between files: nothing half-done */
    fatimg_dir_t *d = &s_ws->dirs[depth];
    unsigned base = s_ws->plen[depth];
    if(!fatimg_dir_next(r, d, &f))                 /* folder done: back to its parent */
    {
      if(d->bad) kept = XFER_BAD_IMAGE;            /* corrupt folder: some of it unread */
      depth--;
      continue;
    }
    if(f.attr & FATIMG_ATTR_DIR)
    {
      if(depth + 1 >= (int)XFER_DEPTH) { kept = XFER_BAD_IMAGE; continue; }
      if(!place_dir(base, f.name83)) return why(XFER_USB_ERROR);
      unsigned len = (unsigned)strlen(s_ws->path);
      s_ws->path[len++] = '/';
      s_ws->path[len] = 0;
      s_ws->plen[++depth] = (uint8_t)len;
      fatimg_dir_sub(r, &s_ws->dirs[depth], f.first_cl);
      continue;
    }
    uint32_t need = fatimg_clusters_for(r, f.size); /* a chain shorter than the file: */
    if(fatimg_chain_len(r, f.first_cl, need) < need) { kept = XFER_BAD_IMAGE; continue; }
    int p = place_file(base, &f);
    if(p == PLACE_ERR) return why(XFER_USB_ERROR);
    if(p == PLACE_FREE)
    {
      xfer_result_t res = copy_file(&f);
      if(res != XFER_OK) return res;
    }
    xfer_dbg_files++;                              /* copied, or already there */
  }
  return kept;
}

xfer_result_t xfer_out(void)
{
  xfer_dbg_files = xfer_dbg_bytes = 0;
  { xfer_result_t m = mount_stick(); if(m != XFER_OK) return m; }
  cancel_arm(BUTTON_LEFT);
  timing_reset();
  TIC(tw);
  if(!flpy_eject(cancelled)) { f_mount(0, "0:", 0); cancel_arm(0); return why(XFER_BUSY); }
  TOC(tw, xfer_dbg_ms_wait);
  s_ws = flpy_scratch();
  s_xbuf = flpy_scratch2();
  xfer_result_t res = copy_out();                    /* every file written AND verified */
  cancel_arm(0);
  f_mount(0, "0:", 0);
  /* Only when every file is safely on the stick: blank the internal disk, like
   * taking the floppy out and putting in a fresh one. Otherwise keep it. */
  TIC(te);
  if(res == XFER_OK) buffer_format();
  TOC(te, xfer_dbg_ms_flash);
  flpy_insert();                                     /* disk change either way */
  return res;
}

/* ---- status report (both buttons, no UPDATE.UPD; docs/13, status.h) ---- */

extern volatile uint32_t dbg_fw_build, dbg_reset_cause;
extern volatile uint32_t buffer_dbg_writebacks, buffer_dbg_recovered, buffer_dbg_marker_repaired;
static const char *s_last_what, *s_last_res;
static uint32_t s_last_files, s_last_bytes;

static const char *result_text(xfer_result_t r)
{
  switch(r)
  {
    case XFER_OK:            return "OK";
    case XFER_NO_STICK:      return "no flash drive";
    case XFER_USB_ERROR:     return "USB error";
    case XFER_BAD_IMAGE:     return "internal disk unreadable";
    case XFER_NOTHING:       return "nothing to load";
    case XFER_VERIFY_FAILED: return "verification failed";
    case XFER_BUSY:          return "drive busy";
    case XFER_BAD_FORMAT:    return "flash drive format not supported";
    case XFER_CANCELLED:     return "cancelled";
    default:                 return "-";
  }
}

void xfer_note(const char *what, xfer_result_t r)
{
  s_last_what = what; s_last_res = result_text(r);
  s_last_files = xfer_dbg_files; s_last_bytes = xfer_dbg_bytes;
}

static xfer_result_t write_status(void)
{
  static status_t st;
  if(!flpy_eject(NULL)) return XFER_BUSY;           /* a still disk: consistent counts */
  s_ws = flpy_scratch();
  s_xbuf = flpy_scratch2();
  memset(&st, 0, sizeof st);
  st.version = FW_VERSION_STR;
  st.build = dbg_fw_build;
  st.board = UPD_BOARD;
  for(unsigned i = 0; i < 3; i++) st.uid[i] = ((const volatile uint32_t *)0x1FFFF7E8u)[i];
  st.flash_id = spiflash_read_jedec_id();
  st.hick = clock_on_hick;
  st.dd = buffer_is_dd();
  fatimg_read_t *r = &s_ws->r;
  if(fatimg_open(r, &buffer_fatimg_io))
  {
    fatimg_file_t f;
    int depth = 0;
    st.disk_ok = true;
    fatimg_dir_root(&s_ws->dirs[0]);
    while(depth >= 0)                               /* every file, folders included */
    {
      if(!fatimg_dir_next(r, &s_ws->dirs[depth], &f)) { depth--; continue; }
      if(f.attr & FATIMG_ATTR_DIR)
      {
        st.dirs++;
        if(depth + 1 < (int)XFER_DEPTH) fatimg_dir_sub(r, &s_ws->dirs[++depth], f.first_cl);
      }
      else { st.files++; st.used_bytes += f.size; }
    }
    for(uint16_t cl = 2; cl <= r->max_cl; cl++)
      if(fatimg_fat_entry(r, cl) == 0) st.free_bytes += FATIMG_SECTOR * r->spc;
  }
  st.fault = buffer_fault();
  st.writebacks = buffer_dbg_writebacks;
  st.recovered = buffer_dbg_recovered;
  st.repaired = buffer_dbg_marker_repaired;
  st.reset_cause = dbg_reset_cause;
  st.uptime_ms = flpy_dbg_ms;
  st.stick_fs = s_fs.fs_type == FS_EXFAT ? "exFAT" : s_fs.fs_type == FS_FAT32 ? "FAT32"
              : s_fs.fs_type == FS_FAT16 ? "FAT16" : "FAT12";
  st.stick_bytes = (uint64_t)(s_fs.n_fatent - 2u) * s_fs.csize * FATIMG_SECTOR;
  st.last_what = s_last_what; st.last_result = s_last_res;
  st.last_files = s_last_files; st.last_bytes = s_last_bytes;

  size_t n = status_text((char *)s_xbuf, XBUF, &st);
  UINT w = 0;
  FRESULT fr = f_open(&s_fil, "0:/" STATUS_FILE_NAME, FA_CREATE_ALWAYS | FA_WRITE);
  if(fr == FR_OK)
  {
    fr = f_write(&s_fil, s_xbuf, (UINT)n, &w);
    FRESULT fc = f_close(&s_fil);
    if(fr == FR_OK) fr = fc;
  }
  flpy_insert();
  xfer_dbg_files = 1; xfer_dbg_bytes = w;
  return (fr == FR_OK && w == n) ? XFER_STATUS : XFER_USB_ERROR;
}

/* ---- firmware update (docs/13, include/update.h) ---- */

/* Check the open update file, then copy it to the SPI-flash staging area:
 * image first (read back + verified), header last as the commit record.
 * Disk ejected; s_fil positioned just after the header. */
static xfer_result_t stage_update(const upd_header_t *h)
{
  UINT got;
  uint32_t crc = 0;
  /* 1. the file is intact and an app for this board: nothing touched yet */
  for(uint32_t off = 0; off < h->length; off += got)
  {
    uint32_t n = h->length - off < XBUF ? h->length - off : XBUF;
    if(f_read(&s_fil, s_xbuf, n, &got) != FR_OK || got != n) return XFER_USB_ERROR;
    if(off == 0 && !upd_vectors_ok(s_xbuf, h->length)) return XFER_BAD_IMAGE;
    crc = upd_crc32(crc, s_xbuf, n);
  }
  if(crc != h->image_crc) return XFER_BAD_IMAGE;

  /* 2. stage the image; erasing also withdraws any older commit record */
  for(uint32_t a = UPD_STAGE_ADDR; a < UPD_STAGE_END; a += SPIFLASH_SECTOR_SIZE) spiflash_erase_sector(a);
  if(f_lseek(&s_fil, sizeof *h) != FR_OK) return XFER_USB_ERROR;
  for(uint32_t off = 0; off < h->length; off += got)
  {
    uint32_t n = h->length - off < XBUF ? h->length - off : XBUF;
    if(f_read(&s_fil, s_xbuf, n, &got) != FR_OK || got != n) return XFER_USB_ERROR;
    spiflash_program(UPD_STAGE_DATA + off, s_xbuf, n);
    xfer_dbg_bytes += n;
  }
  crc = 0;
  for(uint32_t off = 0; off < h->length; off += sizeof s_ws->chk)
  {
    uint32_t n = h->length - off < sizeof s_ws->chk ? h->length - off : sizeof s_ws->chk;
    spiflash_read(UPD_STAGE_DATA + off, s_ws->chk, n);
    crc = upd_crc32(crc, s_ws->chk, n);
  }
  if(crc != h->image_crc) return XFER_VERIFY_FAILED;

  /* 3. commit: the bootloader installs it at the next reset */
  spiflash_program(UPD_STAGE_ADDR, h, sizeof *h);
  spiflash_read(UPD_STAGE_ADDR, s_ws->chk, sizeof *h);
  if(memcmp(s_ws->chk, h, sizeof *h) != 0) return XFER_VERIFY_FAILED;
  xfer_dbg_files = 1;
  return XFER_OK;
}

xfer_result_t xfer_update(void)
{
  upd_header_t h;
  UINT got;
  xfer_dbg_files = xfer_dbg_bytes = 0;
  { xfer_result_t m = mount_stick(); if(m != XFER_OK) return m; }
  timing_reset();
  FRESULT fr = f_open(&s_fil, "0:/" UPD_FILE_NAME, FA_READ);
  if(fr == FR_NO_FILE)                             /* no update: report the status instead */
  {
    xfer_result_t r = write_status();
    f_mount(0, "0:", 0);
    return r;
  }
  if(fr != FR_OK) { f_mount(0, "0:", 0); return XFER_USB_ERROR; }
  xfer_result_t res = XFER_BAD_IMAGE;
  if(f_read(&s_fil, &h, sizeof h, &got) == FR_OK && got == sizeof h && upd_header_ok(&h)
     && f_size(&s_fil) == sizeof h + h.length)
  {
    if(!flpy_eject(NULL)) res = XFER_BUSY;           /* (the update is not cancellable) */
    else
    {
      s_ws = flpy_scratch();
      s_xbuf = flpy_scratch2();
      res = stage_update(&h);
      if(res != XFER_OK) flpy_insert();            /* carry on with this firmware */
    }
  }
  f_close(&s_fil);
  f_mount(0, "0:", 0);
  return res;                                      /* XFER_OK: caller restarts */
}

/* ---- SWD test hook (tools/stick.ps1): read or write part of a stick file ----
 * Lets the bench put files on / take files off the stick in the emulator. Returns
 * bytes transferred, or < 0 on error. Uses FatFs only (floppy keeps running). */
int xfer_dbg_stick(bool write, const char *name, uint32_t off, void *buf, uint32_t len)
{
  char path[3 + 64 + 1] = "0:/";
  UINT n = 0;
  if(strlen(name) > 64) return -4;
  strcat(path, name);
  if(!usbh_app_ready()) return -1;
  if(f_mount(&s_fs, "0:", 1) != FR_OK) return -2;
  FRESULT fr = f_open(&s_fil, path, write ? (FA_WRITE | (off ? FA_OPEN_EXISTING : FA_CREATE_ALWAYS)) : FA_READ);
  if(fr == FR_OK)
  {
    fr = f_lseek(&s_fil, off);
    if(fr == FR_OK) fr = write ? f_write(&s_fil, buf, len, &n) : f_read(&s_fil, buf, len, &n);
    if(f_close(&s_fil) != FR_OK && fr == FR_OK) fr = FR_DISK_ERR;
  }
  f_mount(0, "0:", 0);
  return fr == FR_OK ? (int)n : -3;
}

/* Called at startup. If a committed update is staged and THIS firmware is that
 * image (i.e. the bootloader installed it and it started), withdraw the commit
 * record: otherwise the bootloader would re-install the staged image over any
 * later different firmware (e.g. a new build flashed over SWD). A record whose
 * image is not what is running is left for the bootloader to install. */
void xfer_update_ack(void)
{
  upd_header_t h;
  spiflash_read(UPD_STAGE_ADDR, &h, sizeof h);
  if(upd_header_ok(&h) && upd_crc32(0, (const void *)APP_BASE, h.length) == h.image_crc)
    spiflash_erase_sector(UPD_STAGE_ADDR);        /* header sector: record gone */
}
