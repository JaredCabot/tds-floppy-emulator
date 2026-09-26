/**
 * buffer.c - the disk image in SPI flash, one 12 KB slot per track, written
 * back through a journal so that a power cut never loses stored data. See
 * buffer.h for the layout and the protocol.
 */
#include "buffer.h"
#include "spiflash.h"
#include "fatimg.h"
#include "floppy.h"
#include "update.h"                           /* upd_crc32, UPD_STAGE_ADDR */
#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(fatimg_build_t) <= FLPY_SCRATCH_SIZE, "builder must fit in the floppy scratch");
_Static_assert(sizeof(fatimg_read_t) <= FLPY_SCRATCH_SIZE, "reader must fit in the floppy scratch");
_Static_assert(BUF_SLOT_SIZE == 3u * SPIFLASH_SECTOR_SIZE, "a slot is 3 erase sectors");
_Static_assert(BUF_TRACKS * BUF_SLOT_SIZE <= BUF_JOURNAL_ADDR, "track slots overlap the journal");
_Static_assert(BUF_JOURNAL_ADDR + BUF_JOURNALS * BUF_SLOT_SIZE <= BUF_META_ADDR, "journal overlaps the meta sector");
_Static_assert(BUF_META_ADDR + SPIFLASH_SECTOR_SIZE <= UPD_STAGE_ADDR, "meta sector overlaps the update staging");

#define SLOT_SECTORS  3u
#define CHUNK         256u                    /* per poll: a fraction of a ms of SPI work */

/* "complete image" marker, in the meta sector. Earlier firmware kept it in
 * slot 0's spare area (LEGACY_*): such an image is taken over, not formatted. */
static const char MARKER[8]         __attribute__((nonstring)) = "TDSFBUF2";
static const char LEGACY_MARKER1[8] __attribute__((nonstring)) = "TDSFBUF1";
static const char LEGACY_MARKER0[8] __attribute__((nonstring)) = "GTKBUF01";
#define LEGACY_MARKER_ADDR  BUF_TRACK_SIZE

/* A write-back that gave up leaves this record in the meta sector: the fault
 * outlives a power cycle (the LED keeps showing it) until a full rebuild (DATA
 * IN, or the reformat after a successful DATA OUT) erases it. */
static const char FAULT_MARK[8] __attribute__((nonstring)) = "TDSFAULT";
#define FAULT_ADDR  (BUF_META_ADDR + 64u)

/* SWD test hook: while it holds BUFFER_INJECT_FAIL, every track read-back
 * "fails" (the data is in fact written correctly), to exercise the give-up path
 * and the persistent fault without damaging the flash. */
#define BUFFER_INJECT_FAIL  0xFA11FA11u
volatile uint32_t buffer_dbg_inject;

/* A journal slot holds a track's data at offset 0 and this header after it,
 * written last: the commit record. */
typedef struct {
  char     magic[4];
  uint16_t track;
  uint16_t zero;
  uint32_t seq;
  uint32_t crc;                               /* CRC-32 of the data, then of the header up to here */
} jhdr_t;
_Static_assert(sizeof(jhdr_t) == 16, "journal header layout");
static const char JMAGIC[4] = { 'T', 'J', 'N', '1' };

volatile uint32_t buffer_dbg_recovered;       /* tracks restored from the journal at boot */
volatile uint32_t buffer_dbg_marker_repaired; /* boots that found a sound disk without its marker */
volatile uint32_t buffer_dbg_writebacks;      /* journal writes since power-on (wear: docs/10) */

static uint32_t slot_addr(unsigned t)    { return (uint32_t)t * BUF_SLOT_SIZE; }
static uint32_t journal_addr(unsigned j) { return BUF_JOURNAL_ADDR + (uint32_t)j * BUF_SLOT_SIZE; }
static uint32_t lba_addr(uint32_t lba)
{
  return slot_addr(lba / BUF_SECTORS_PER_TRACK) + (lba % BUF_SECTORS_PER_TRACK) * 512u;
}

void buffer_read_lba(uint32_t lba, uint8_t dst[512])  { spiflash_read(lba_addr(lba), dst, 512); }
void buffer_program(uint32_t lba, unsigned off, const void *src, unsigned len)
{
  spiflash_program(lba_addr(lba) + off, src, len);
}
void buffer_load_track(unsigned t, uint8_t dst[BUF_TRACK_SIZE]) { spiflash_read(slot_addr(t), dst, BUF_TRACK_SIZE); }

/* ================================ write-back ================================ */
static uint32_t s_seq;                        /* sequence number of the next journal entry */
static bool     s_fault;                      /* a track could not be stored */
static unsigned s_pre_j = 0xFFu, s_pre_n;     /* background pre-erase: slot, sector erases started */
static uint8_t  s_chk[CHUNK], s_chk2[CHUNK];

static void fault_set(void)
{
  if(s_fault) return;
  s_fault = true;
  spiflash_program(FAULT_ADDR, FAULT_MARK, sizeof FAULT_MARK);   /* survives power-off */
}

enum { W_SAME, W_JERASE, W_JPROG, W_JVERIFY, W_JCOMMIT, W_TERASE, W_TPROG, W_TVERIFY };
static struct {
  const uint8_t *src;
  unsigned t, j, step, n, off, tries;
  uint32_t seq, crc;
  bool active, ok;
} s_wb;

/* Take the next journal slot (erased already, in part or whole, by the
 * background pre-erase if it got there first). */
static void journal_begin(void)
{
  buffer_dbg_writebacks++;
  s_wb.seq = s_seq++;
  s_wb.j = s_wb.seq % BUF_JOURNALS;
  s_wb.n = (s_pre_j == s_wb.j) ? s_pre_n : 0;
  s_pre_j = 0xFFu; s_pre_n = 0;
  s_wb.crc = 0; s_wb.off = 0;
  s_wb.step = W_JERASE;
}

/* A read-back did not match. Retry: a journal failure takes the next journal
 * slot, a track failure rewrites the track (its journal entry is committed).
 * After 3 failures give up rather than loop: returns true (finished). */
static bool wb_failed(bool in_journal)
{
  if(++s_wb.tries >= 3) { s_wb.active = false; s_wb.ok = false; fault_set(); return true; }
  if(in_journal) journal_begin();
  else { s_wb.step = W_TERASE; s_wb.n = 0; s_wb.off = 0; }
  return false;
}

/* One erase per call, without waiting; then on to the next step. */
static void erase_step(uint32_t base, unsigned next)
{
  if(spiflash_busy()) return;
  if(s_wb.n < SLOT_SECTORS) { spiflash_erase_sector_start(base + s_wb.n * SPIFLASH_SECTOR_SIZE); s_wb.n++; return; }
  s_wb.step = next; s_wb.off = 0;
}

void buffer_wb_start(unsigned t, const uint8_t src[BUF_TRACK_SIZE])
{
  s_wb.src = src; s_wb.t = t; s_wb.tries = 0; s_wb.off = 0;
  s_wb.ok = true; s_wb.active = true;
  s_wb.step = W_SAME;
}

bool buffer_wb_active(void) { return s_wb.active; }
bool buffer_wb_ok(void)     { return s_wb.ok; }
bool buffer_fault(void)     { return s_fault || spiflash_fault(); }

bool buffer_wb_poll(void)
{
  if(!s_wb.active) return false;
  uint32_t ja = journal_addr(s_wb.j), ta = slot_addr(s_wb.t);
  switch(s_wb.step)
  {
  case W_SAME:                                /* unchanged track: nothing to write (saves wear) */
    spiflash_read(ta + s_wb.off, s_chk, CHUNK);
    if(memcmp(s_chk, s_wb.src + s_wb.off, CHUNK) != 0) { journal_begin(); return false; }
    if((s_wb.off += CHUNK) < BUF_TRACK_SIZE) return false;
    s_wb.active = false;
    return true;
  case W_JERASE:
    erase_step(ja, W_JPROG);
    return false;
  case W_JPROG:
    if(spiflash_busy()) return false;
    spiflash_program(ja + s_wb.off, s_wb.src + s_wb.off, CHUNK);
    s_wb.crc = upd_crc32(s_wb.crc, s_wb.src + s_wb.off, CHUNK);
    if((s_wb.off += CHUNK) >= BUF_TRACK_SIZE) { s_wb.step = W_JVERIFY; s_wb.off = 0; }
    return false;
  case W_JVERIFY:
    spiflash_read(ja + s_wb.off, s_chk, CHUNK);
    if(memcmp(s_chk, s_wb.src + s_wb.off, CHUNK) != 0) return wb_failed(true);
    if((s_wb.off += CHUNK) >= BUF_TRACK_SIZE) s_wb.step = W_JCOMMIT;
    return false;
  case W_JCOMMIT:                             /* the header last: the entry now counts */
  {
    jhdr_t h;
    memcpy(h.magic, JMAGIC, sizeof h.magic);
    h.track = (uint16_t)s_wb.t; h.zero = 0; h.seq = s_wb.seq;
    h.crc = upd_crc32(s_wb.crc, &h, offsetof(jhdr_t, crc));
    spiflash_program(ja + BUF_TRACK_SIZE, &h, sizeof h);
    spiflash_read(ja + BUF_TRACK_SIZE, s_chk, sizeof h);
    if(memcmp(s_chk, &h, sizeof h) != 0) return wb_failed(true);
    s_wb.step = W_TERASE; s_wb.n = 0;
    return false;
  }
  case W_TERASE:                              /* only now is the old track given up */
    erase_step(ta, W_TPROG);
    return false;
  case W_TPROG:
    if(spiflash_busy()) return false;
    spiflash_program(ta + s_wb.off, s_wb.src + s_wb.off, CHUNK);
    if((s_wb.off += CHUNK) >= BUF_TRACK_SIZE) { s_wb.step = W_TVERIFY; s_wb.off = 0; }
    return false;
  default:                                    /* W_TVERIFY */
    spiflash_read(ta + s_wb.off, s_chk, CHUNK);
    if(memcmp(s_chk, s_wb.src + s_wb.off, CHUNK) != 0 || buffer_dbg_inject == BUFFER_INJECT_FAIL)
      return wb_failed(false);
    if((s_wb.off += CHUNK) < BUF_TRACK_SIZE) return false;
    s_wb.active = false;
    return true;
  }
}

void buffer_wb_finish(void) { while(s_wb.active) buffer_wb_poll(); }

void buffer_bg_poll(void)
{
  if(s_wb.active || spiflash_busy()) return;
  unsigned j = s_seq % BUF_JOURNALS;          /* the slot the next write-back will take: */
  if(s_pre_j != j) { s_pre_j = j; s_pre_n = 0; }   /* the oldest entry, long superseded */
  if(s_pre_n < SLOT_SECTORS)
  {
    spiflash_erase_sector_start(journal_addr(j) + s_pre_n * SPIFLASH_SECTOR_SIZE);
    s_pre_n++;
  }
}

/* ================================ rebuild ================================ */
void buffer_begin_rebuild(void)
{
  spiflash_erase_chip();                      /* also the marker, the journal and any
                                                 firmware-update record (by now always
                                                 acknowledged: see xfer_update_ack) */
  s_seq = 0;
  s_pre_j = 0; s_pre_n = SLOT_SECTORS;        /* the first journal slot is erased too */
  s_fault = false;                            /* its record is erased: the RAM flag follows */
}

void buffer_end_rebuild(void)
{
  spiflash_program(BUF_META_ADDR, MARKER, sizeof MARKER);   /* last: image complete */
}

/* fatimg I/O on the buffer */
static void io_read(uint32_t lba, uint8_t buf[512], void *ctx) { (void)ctx; buffer_read_lba(lba, buf); }
static void io_prog(uint32_t lba, unsigned off, const void *src, unsigned len, void *ctx)
{ (void)ctx; buffer_program(lba, off, src, len); }
const fatimg_io_t buffer_fatimg_io = { io_read, io_prog, NULL };

void buffer_format(void)
{
  fatimg_build_t *b = flpy_scratch();          /* caller ejects the disk first */
  if(!b) return;
  buffer_begin_rebuild();
  fatimg_build_begin(b, &buffer_fatimg_io);
  fatimg_finish(b);                            /* empty FAT12 volume */
  buffer_end_rebuild();
}

/* ================================ boot ================================ */
static bool marker_at(uint32_t addr, const char m[8])
{
  char b[8];
  spiflash_read(addr, b, sizeof b);
  return memcmp(b, m, sizeof b) == 0;
}

/* The newest committed journal entry (header sane, data matching its CRC). */
static bool journal_newest(unsigned *jout, jhdr_t *hout)
{
  bool found = false;
  for(unsigned j = 0; j < BUF_JOURNALS; j++)
  {
    jhdr_t h;
    spiflash_read(journal_addr(j) + BUF_TRACK_SIZE, &h, sizeof h);
    if(memcmp(h.magic, JMAGIC, sizeof h.magic) != 0 || h.track >= BUF_TRACKS || h.zero != 0) continue;
    if(found && h.seq <= hout->seq) continue;
    uint32_t crc = 0;
    for(uint32_t off = 0; off < BUF_TRACK_SIZE; off += CHUNK)
    {
      spiflash_read(journal_addr(j) + off, s_chk, CHUNK);
      crc = upd_crc32(crc, s_chk, CHUNK);
    }
    if(upd_crc32(crc, &h, offsetof(jhdr_t, crc)) != h.crc) continue;
    *jout = j; *hout = h; found = true;
  }
  return found;
}

static bool slot_equals_journal(uint32_t ta, uint32_t ja)
{
  for(uint32_t off = 0; off < BUF_TRACK_SIZE; off += CHUNK)
  {
    spiflash_read(ja + off, s_chk, CHUNK);
    spiflash_read(ta + off, s_chk2, CHUNK);
    if(memcmp(s_chk, s_chk2, CHUNK) != 0) return false;
  }
  return true;
}

/* If power was lost while a track slot was being rewritten, the newest journal
 * entry holds its data: put it back. */
static void journal_recover(void)
{
  unsigned j;
  jhdr_t h;
  if(!journal_newest(&j, &h)) { s_seq = 0; return; }
  s_seq = h.seq + 1u;
  uint32_t ja = journal_addr(j), ta = slot_addr(h.track);
  if(slot_equals_journal(ta, ja)) return;      /* the usual case: that write-back finished */
  for(unsigned tries = 0; tries < 3; tries++)
  {
    for(unsigned s = 0; s < SLOT_SECTORS; s++) spiflash_erase_sector(ta + s * SPIFLASH_SECTOR_SIZE);
    for(uint32_t off = 0; off < BUF_TRACK_SIZE; off += CHUNK)
    {
      spiflash_read(ja + off, s_chk, CHUNK);
      spiflash_program(ta + off, s_chk, CHUNK);
    }
    if(slot_equals_journal(ta, ja)) { buffer_dbg_recovered++; return; }
  }
  fault_set();
}

/* The marker decides whether the whole disk is erased, so one bad read must
 * not be enough: read it again, and before formatting look whether a sound
 * FAT12 volume is there after all. */
static bool marker_ok(void)
{
  for(unsigned i = 0; i < 3; i++)
    if(marker_at(BUF_META_ADDR, MARKER)) return true;
  return false;
}

/* A sound disk without a readable marker: rewrite the meta sector, keeping a
 * recorded fault, rather than wipe the disk. */
static void marker_repair(void)
{
  bool fault = marker_at(FAULT_ADDR, FAULT_MARK);
  spiflash_erase_sector(BUF_META_ADDR);
  spiflash_program(BUF_META_ADDR, MARKER, sizeof MARKER);
  if(fault) spiflash_program(FAULT_ADDR, FAULT_MARK, sizeof FAULT_MARK);
  buffer_dbg_marker_repaired++;
}

void buffer_init(void)
{
  if(!marker_ok())
  {
    if(marker_at(LEGACY_MARKER_ADDR, LEGACY_MARKER1) || marker_at(LEGACY_MARKER_ADDR, LEGACY_MARKER0))
    {                                          /* an image from earlier firmware: keep it, */
      for(uint32_t a = BUF_JOURNAL_ADDR; a <= BUF_META_ADDR; a += SPIFLASH_SECTOR_SIZE)
        spiflash_erase_sector(a);              /* clear the areas it did not use */
      spiflash_program(BUF_META_ADDR, MARKER, sizeof MARKER);
    }
    else if(fatimg_valid((fatimg_read_t *)flpy_scratch(), &buffer_fatimg_io))
      marker_repair();                         /* never wipe a sound disk */
    else { buffer_format(); return; }          /* no complete image: blank, or a rebuild cut short */
  }
  if(marker_at(FAULT_ADDR, FAULT_MARK)) s_fault = true;   /* a fault from before the power-off */
  journal_recover();
}

#ifdef BUFFER_HOST_TEST
/* Host tests only (test/test_buffer.c): forget all RAM state, as a power cut
 * does, before a simulated reboot. */
void buffer_test_reset(void);
void buffer_test_reset(void)
{
  memset(&s_wb, 0, sizeof s_wb);
  s_seq = 0; s_fault = false; s_pre_j = 0xFFu; s_pre_n = 0;
}
#endif
