/*
 * test_buffer.c - host test of the power-safe write-back (buffer.c) against a
 * simulated SPI flash that can lose power at any operation (review item 2/17).
 *
 * The fake flash counts every erase and program. Given a cut point, the
 * operation in flight is left half done, as on a real chip: a half-erased
 * sector reads as noise, a half-programmed run as only its first bytes. The
 * test then "reboots" (RAM state forgotten, buffer_init()) and checks that
 * every track is either entirely old or entirely new, never damaged, and that
 * the disk was not re-formatted. It does this for EVERY operation of a
 * realistic sequence. Also: a sector that never programs (bounded retries, no
 * hang), unchanged tracks skipped, a 1.0.0 image taken over, a blank flash
 * formatted.
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "buffer.h"
#include "spiflash.h"
#include "floppy.h"

void buffer_test_reset(void);

static int fails;
#define CHECK(cond, msg) do{ if(!(cond)){ printf("FAIL: %s\n", msg); fails++; } }while(0)

/* ---------------------------- simulated flash ---------------------------- */
static uint8_t  g_flash[SPIFLASH_SIZE], g_base[SPIFLASH_SIZE];
static long     g_ops, g_cut = -1, g_erases;
static jmp_buf  g_power;
static uint32_t g_rng = 12345, g_bad_lo = 1, g_bad_hi = 0;   /* programs in [lo,hi) never take */

static uint32_t rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }
static int cut_now(void) { g_ops++; return g_cut >= 0 && g_ops == g_cut; }

static void erase4k(uint32_t a)
{
  a &= ~(SPIFLASH_SECTOR_SIZE - 1u);
  g_erases++;
  if(cut_now())                                /* power lost mid-erase: garbage */
  {
    for(unsigned i = 0; i < SPIFLASH_SECTOR_SIZE; i++) g_flash[a + i] = (uint8_t)rnd();
    longjmp(g_power, 1);
  }
  memset(&g_flash[a], 0xFF, SPIFLASH_SECTOR_SIZE);
}

void spiflash_read(uint32_t addr, void *buf, uint32_t len) { memcpy(buf, &g_flash[addr], len); }
void spiflash_erase_sector(uint32_t addr)       { erase4k(addr); }
void spiflash_erase_sector_start(uint32_t addr) { erase4k(addr); }
bool spiflash_busy(void)  { return false; }
bool spiflash_fault(void) { return false; }
void spiflash_erase_chip(void)
{
  if(cut_now()) { for(uint32_t i = 0; i < SPIFLASH_SIZE; i += 4096) g_flash[i] = (uint8_t)rnd(); longjmp(g_power, 1); }
  memset(g_flash, 0xFF, sizeof g_flash);
}
void spiflash_program(uint32_t addr, const void *buf, uint32_t len)
{
  const uint8_t *p = buf;
  uint32_t n = len;
  int cut = cut_now();
  if(cut) n = len ? rnd() % len : 0;           /* power lost mid-program: a prefix */
  for(uint32_t i = 0; i < n; i++)
    if(!(addr + i >= g_bad_lo && addr + i < g_bad_hi)) g_flash[addr + i] &= p[i];
  if(cut) longjmp(g_power, 1);
}
void *flpy_scratch(void) { static uint8_t s[FLPY_SCRATCH_SIZE]; return s; }

/* ------------------------------- helpers ------------------------------- */
static void pattern(uint8_t *d, unsigned t, unsigned gen)
{
  for(unsigned i = 0; i < BUF_TRACK_SIZE; i++) d[i] = (uint8_t)(i * 7u + t * 31u + gen * 101u + (i >> 9));
}
static uint8_t g_src[BUF_TRACK_SIZE];          /* stays put while a write-back runs */
static void store(unsigned t, unsigned gen)
{
  pattern(g_src, t, gen);
  buffer_wb_start(t, g_src);
  buffer_wb_finish();
}
/* which generation a track holds: 0..4, or -1 if it is none of them (damaged) */
static int track_gen(unsigned t)
{
  static uint8_t got[BUF_TRACK_SIZE], want[BUF_TRACK_SIZE];
  buffer_load_track(t, got);
  for(unsigned g = 0; g <= 4; g++) { pattern(want, t, g); if(memcmp(got, want, sizeof got) == 0) return (int)g; }
  return -1;
}
static void reboot(void) { g_cut = -1; buffer_test_reset(); buffer_init(); }

/* ------------------------------- scenario ------------------------------- */
/* idle pre-erase, then two write-backs of different tracks, as a save does */
static void scenario(void)
{
  for(int i = 0; i < 6; i++) buffer_bg_poll();
  store(0, 2);
  for(int i = 0; i < 6; i++) buffer_bg_poll();
  store(7, 2);
}

static void test_power_cuts(void)
{
  memset(g_flash, 0xFF, sizeof g_flash);
  reboot();                                    /* blank flash: formatted */
  store(0, 1); store(1, 1); store(7, 1); store(20, 1);
  memcpy(g_base, g_flash, sizeof g_flash);

  reboot(); g_ops = 0; scenario();             /* how many operations the scenario takes */
  long total = g_ops;
  CHECK(track_gen(0) == 2 && track_gen(7) == 2, "scenario stores both tracks");

  int damaged = 0, formatted = 0, order = 0, after = 0, restored = 0;
  for(long c = 1; c <= total; c++)
  {
    memcpy(g_flash, g_base, sizeof g_flash);
    reboot();
    g_ops = 0; g_cut = c;
    if(setjmp(g_power) == 0) scenario();       /* runs until the power goes */
    extern volatile uint32_t buffer_dbg_recovered;
    uint32_t before = buffer_dbg_recovered;
    reboot();
    restored += buffer_dbg_recovered != before;
    int g0 = track_gen(0), g7 = track_gen(7);
    if(g0 < 1 || g0 > 2 || g7 < 1 || g7 > 2) damaged++;
    if(track_gen(1) != 1 || track_gen(20) != 1) formatted++;
    if(g7 == 2 && g0 != 2) order++;            /* written in order: 7 new implies 0 new */
    store(0, 3); store(7, 3);                  /* and it carries on normally afterwards */
    if(track_gen(0) != 3 || track_gen(7) != 3 || track_gen(1) != 1 || buffer_fault()) after++;
  }
  printf("buffer power cuts: %ld cut points, %d damaged, %d formatted, %d out of order, "
         "%d restored from the journal, %d bad afterwards\n", total, damaged, formatted, order, restored, after);
  CHECK(total > 50, "the scenario has many operations to cut");
  CHECK(damaged == 0, "a power cut at ANY operation leaves each track entirely old or entirely new");
  CHECK(formatted == 0, "a power cut never loses the rest of the disk (no re-format)");
  CHECK(order == 0, "tracks change in the order they were written");
  CHECK(restored > 0, "some cut points needed the journal (the protection is exercised)");
  CHECK(after == 0, "after any cut, later write-backs work and no fault is left");
}

static void test_bad_sector(void)
{
  memset(g_flash, 0xFF, sizeof g_flash);
  reboot();
  store(20, 1);
  g_bad_lo = 20u * BUF_SLOT_SIZE + 5000u; g_bad_hi = g_bad_lo + 64u;   /* part of track 20 never programs */
  g_erases = 0;
  store(20, 2);                                /* must come back, not hang */
  printf("buffer bad sector: gave up after %ld erases, ok=%d fault=%d\n", g_erases, buffer_wb_ok(), buffer_fault());
  CHECK(!buffer_wb_ok() && buffer_fault(), "a sector that never programs: write-back gives up and reports a fault");
  CHECK(g_erases <= 12, "bounded retries (no endless loop)");
  CHECK(!buffer_wb_active(), "and it is finished, not still running");
  g_bad_lo = 1; g_bad_hi = 0;

  /* the fault survives a power cycle; the journal still holds the track's data */
  extern volatile uint32_t buffer_dbg_recovered;
  uint32_t before = buffer_dbg_recovered;
  reboot();
  CHECK(buffer_fault(), "a write-back fault is remembered across a power cycle");
  CHECK(buffer_dbg_recovered == before + 1 && track_gen(20) == 2,
        "at boot the journal restores the track (the sector works again)");
  buffer_format();                             /* a full rebuild (DATA IN / after DATA OUT) */
  CHECK(!buffer_fault(), "a full rebuild clears the fault at once, not only after a reboot");
  reboot();
  CHECK(!buffer_fault(), "a full rebuild clears the remembered fault");
}

static void test_unchanged_skipped(void)
{
  memset(g_flash, 0xFF, sizeof g_flash);
  reboot();
  store(5, 1);
  g_erases = 0;
  store(5, 1);                                 /* identical data again */
  CHECK(g_erases == 0 && track_gen(5) == 1, "an unchanged track is not rewritten (no erase: less wear)");
}

static void test_legacy_and_blank(void)
{
  memset(g_flash, 0xFF, sizeof g_flash);       /* a 1.0.0 image: marker in slot 0's spare area */
  pattern(&g_flash[0], 0, 1);
  pattern(&g_flash[3u * BUF_SLOT_SIZE], 3, 1);
  memcpy(&g_flash[BUF_TRACK_SIZE], "TDSFBUF1", 8);
  reboot();
  CHECK(track_gen(0) == 1 && track_gen(3) == 1, "an image from 1.0.0 is taken over, not re-formatted");
  CHECK(memcmp(&g_flash[BUF_META_ADDR], "TDSFBUF2", 8) == 0, "... and gets the new marker");
  store(3, 2);
  reboot();
  CHECK(track_gen(3) == 2, "... and write-back works on it");

  memset(g_flash, 0xFF, sizeof g_flash);       /* blank flash */
  reboot();
  CHECK(g_flash[510] == 0x55 && g_flash[511] == 0xAA, "a blank flash is formatted (boot sector)");
  CHECK(memcmp(&g_flash[BUF_META_ADDR], "TDSFBUF2", 8) == 0, "... and marked complete");
}

/* the marker decides whether the disk is wiped: a damaged marker on a sound
 * disk must be repaired, never formatted; a rebuild cut short still formats */
static void test_marker_damage(void)
{
  extern volatile uint32_t buffer_dbg_marker_repaired;
  memset(g_flash, 0xFF, sizeof g_flash);
  reboot();                                    /* blank: formatted */
  store(5, 1); store(9, 1);                    /* (not track 0: it holds the boot sector and FATs) */
  memcpy(&g_flash[BUF_META_ADDR + 64u], "TDSFAULT", 8);   /* a recorded fault, to be kept */
  g_flash[BUF_META_ADDR] &= 0x00;              /* damage the marker (as a bad read would see it) */
  uint32_t before = buffer_dbg_marker_repaired;
  reboot();
  CHECK(buffer_dbg_marker_repaired == before + 1, "a sound disk without its marker is repaired, not wiped");
  CHECK(track_gen(5) == 1 && track_gen(9) == 1, "... and every track is kept");
  CHECK(memcmp(&g_flash[BUF_META_ADDR], "TDSFBUF2", 8) == 0, "... the marker is rewritten");
  CHECK(buffer_fault(), "... and a recorded fault is kept");

  buffer_begin_rebuild();                      /* a DATA IN cut short: erased, no volume yet */
  reboot();
  CHECK(g_flash[510] == 0x55 && g_flash[511] == 0xAA && track_gen(9) != 1,
        "a rebuild cut short (no sound volume) is still formatted");
}

/* the disk's density (720 KB DD / 1.44 MB HD) is recorded, survives power
 * cycles, rebuilds and marker repairs, maps 9 sectors per track, and can be
 * switched back, keeping a recorded fault */
static void test_density(void)
{
  extern volatile uint32_t buffer_dbg_marker_repaired;
  static uint8_t s[512];
  memset(g_flash, 0xFF, sizeof g_flash);
  reboot();
  CHECK(!buffer_is_dd(), "a new disk is HD");
  buffer_set_density(true);
  CHECK(memcmp(&g_flash[BUF_META_ADDR + 32u], "TDSDD720", 8) == 0, "DD is recorded in the meta sector");
  reboot();
  CHECK(buffer_is_dd(), "DD survives a power cycle");
  buffer_format();                             /* a rebuild (DATA IN / after DATA OUT) */
  reboot();
  buffer_read_lba(0, s);
  CHECK(buffer_is_dd() && s[13] == 2 && s[21] == 0xF9 && (s[19] | s[20] << 8) == 1440,
        "a rebuild keeps DD and writes a 720 KB volume");
  buffer_read_lba(9, s);
  CHECK(memcmp(s, &g_flash[BUF_SLOT_SIZE], 512) == 0, "DD: LBA 9 is the first sector of track 1 (9 per track)");
  g_flash[BUF_META_ADDR] &= 0x00;              /* damaged marker on a sound DD disk */
  uint32_t before = buffer_dbg_marker_repaired;
  reboot();
  CHECK(buffer_dbg_marker_repaired == before + 1 && buffer_is_dd(), "a marker repair keeps DD");
  memset(&g_flash[BUF_META_ADDR], 0xFF, SPIFLASH_SECTOR_SIZE);   /* meta sector lost: record gone too */
  reboot();
  CHECK(buffer_is_dd(), "a repair takes DD from the volume's boot sector when the record is gone");
  memcpy(&g_flash[BUF_META_ADDR + 64u], "TDSFAULT", 8);   /* a recorded fault */
  reboot();
  buffer_set_density(false);                   /* the disk is being formatted HD again */
  reboot();
  CHECK(!buffer_is_dd() && memcmp(&g_flash[BUF_META_ADDR], "TDSFBUF2", 8) == 0,
        "switching back to HD rewrites the meta sector: HD, marker intact");
  CHECK(buffer_fault(), "... and keeps the recorded fault");
}

int main(void)
{
  test_density();
  test_marker_damage();
  test_power_cuts();
  test_bad_sector();
  test_unchanged_skipped();
  test_legacy_and_blank();
  printf(fails ? "BUFFER TESTS: %d FAILED\n" : "BUFFER TESTS PASSED\n", fails);
  return fails != 0;
}
