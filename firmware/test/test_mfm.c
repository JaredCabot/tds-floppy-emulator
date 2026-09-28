/**
 * test_mfm.c - host self-test for the MFM encoder + FAT12 image (ponytail: one
 * runnable check for the non-trivial logic). Not part of the firmware build.
 * Compile with -DMFM_HOST_TEST.
 */
#include "mfm.h"
#include "status.h"
#include "fat12.h"
#include "testimg.h"
#include "fatimg.h"
#include "update.h"
#include "buttons.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, msg) do{ if(!(cond)){ printf("FAIL: %s\n", msg); fails++; } }while(0)

/* Known CRC-CCITT test vector: "123456789" -> 0x29B1 */
static void test_crc(void)
{
  uint16_t c = mfm_crc_ccitt(0xFFFF, (const uint8_t*)"123456789", 9);
  printf("crc(\"123456789\") = 0x%04X (expect 0x29B1)\n", c);
  CHECK(c == 0x29B1, "CRC-CCITT test vector");
}

static uint8_t  g_src[MFM_TRACK_SIZE];
static uint16_t g_crc[MFM_SECTORS_PER_TRACK];
static uint8_t  g_mfm[MFM_TRACK_MAX_BYTES];

static mfm_track_t make_track(uint8_t cyl, uint8_t head, bool hide)
{
  for(unsigned s = 0; s < MFM_SECTORS_PER_TRACK; s++)
  {
    for(unsigned i = 0; i < MFM_SECTOR_SIZE; i++)
      g_src[s*MFM_SECTOR_SIZE + i] = (uint8_t)(s*7 + i*3 + 0x11 + cyl);
    g_crc[s] = mfm_data_crc(&g_src[s * MFM_SECTOR_SIZE]);
  }
  mfm_track_t t = { cyl, head, g_src, g_crc, hide, NULL };
  return t;
}

static void test_track_roundtrip(void)
{
  static uint8_t back[MFM_TRACK_SIZE];
  mfm_track_t t = make_track(5, 1, false);
  size_t n = mfm_encode_track(&t, g_mfm, sizeof g_mfm);
  printf("encoded track: %zu MFM cell-bytes (expect 25000)\n", n);
  CHECK(n == 25000, "track length");

  memset(back, 0, sizeof back);
  int good = mfm_decode_verify(g_mfm, n, back);
  printf("decoded good sectors: %d / %d\n", good, MFM_SECTORS_PER_TRACK);
  CHECK(good == MFM_SECTORS_PER_TRACK, "all 18 sectors decode with valid CRCs");
  CHECK(memcmp(g_src, back, sizeof g_src) == 0, "decoded data matches source");

  /* every CLOCK cell must obey MFM (decoder only reads data cells); the only
   * exceptions are the deliberate missing-clock sync words */
  int bad = 0, syncs = 0, prev_d = 0;
  for(size_t i = 0; i + 1 < n; i += 2)
  {
    uint16_t wd = (uint16_t)((g_mfm[i] << 8) | g_mfm[i + 1]);
    if(wd == 0x4489 || wd == 0x5224) { syncs++; prev_d = wd & 1; continue; }
    for(int b = 7; b >= 0; b--)
    {
      int c = (wd >> (2 * b + 1)) & 1, d = (wd >> (2 * b)) & 1;
      if(c != (!prev_d && !d)) bad++;
      prev_d = d;
    }
  }
  printf("clock-rule violations: %d, sync words: %d (expect 0, 3+18*6=111)\n", bad, syncs);
  CHECK(bad == 0, "every clock cell obeys the MFM rule");
  CHECK(syncs == 111, "exactly the expected sync marks");

  /* hidden track: no findable sectors at all, and still valid MFM */
  mfm_track_t h = make_track(5, 1, true);
  mfm_encode_track(&h, g_mfm, sizeof g_mfm);
  int hidden_good = mfm_decode_verify(g_mfm, n, back);
  printf("hidden track: %d readable sectors (expect 0)\n", hidden_good);
  CHECK(hidden_good == 0, "hide=true exposes no sectors");
}

/* ---- write-path decoder: feed flux intervals, collect fields ---- */
typedef struct { int ids, ids_ok, data, data_ok; uint8_t last_r; uint8_t out[MFM_TRACK_SIZE]; } fields_t;

static void on_field(uint8_t mark, const uint8_t *f, unsigned len, void *ctx)
{
  fields_t *r = ctx;
  if(mark == 0xFE)
  {
    uint8_t b[8] = { 0xA1, 0xA1, 0xA1, 0xFE, f[0], f[1], f[2], f[3] };
    r->ids++;
    if(mfm_crc_ccitt(0xFFFF, b, 8) == ((f[4] << 8) | f[5])) { r->ids_ok++; r->last_r = f[2]; }
  }
  else
  {
    r->data++;
    if(len == MFM_SECTOR_SIZE + 2 && mfm_data_crc(f) == ((f[512] << 8) | f[513]))
    {
      r->data_ok++;
      if(r->last_r >= 1 && r->last_r <= 18) memcpy(&r->out[(r->last_r - 1) * 512], f, 512);
    }
  }
}

/* Turn an encoded track into flux intervals (in timer ticks, 144/cell) and push
 * them through the decoder the way the firmware will: n = round(ticks / 144).
 * jitter_ticks > 0 adds a +/- write-precompensation-like shift to every edge. */
static void run_decoder_from(size_t n, size_t start_cell, int jitter_ticks, fields_t *r)
{
  mfm_dec_t d;
  memset(r, 0, sizeof *r);
  mfm_dec_init(&d, on_field, r);
  long last = 0, edge = 0;
  unsigned seed = 1;
  for(size_t i = start_cell, cell = start_cell; i < n * 8; i++, cell++)
  {
    if(!((g_mfm[i >> 3] >> (7 - (i & 7))) & 1)) continue;
    seed = seed * 1103515245u + 12345u;
    long j = jitter_ticks ? (long)((seed >> 16) % (2u * jitter_ticks + 1)) - jitter_ticks : 0;
    long t = cell * 144 + j;
    if(edge++) mfm_dec_push(&d, (unsigned)((t - last + 72) / 144));
    last = t;
  }
}

static void run_decoder(size_t n, int jitter_ticks, fields_t *r) { run_decoder_from(n, 0, jitter_ticks, r); }

static void test_write_decoder(void)
{
  fields_t r;
  mfm_track_t t = make_track(9, 0, false);
  size_t n = mfm_encode_track(&t, g_mfm, sizeof g_mfm);

  run_decoder(n, 0, &r);
  printf("write decoder, clean:  IDs %d/%d ok, data %d/%d ok\n", r.ids_ok, r.ids, r.data_ok, r.data);
  CHECK(r.ids_ok == 18 && r.data_ok == 18, "decoder recovers all 18 IDs + data fields");
  CHECK(memcmp(r.out, g_src, sizeof g_src) == 0, "decoded data == source");

  run_decoder(n, 18, &r);   /* +/-125 ns precompensation-sized shifts */
  printf("write decoder, jitter: IDs %d/%d ok, data %d/%d ok\n", r.ids_ok, r.ids, r.data_ok, r.data);
  CHECK(r.ids_ok == 18 && r.data_ok == 18, "decoder tolerates +/-125 ns edge jitter");

  /* capture starting late, inside the 2nd A1 of sector 0's data field (the
   * WGATE interrupt can be delayed past the sync bytes and the first A1) */
  size_t late = (MFM_TRACK_PRE + 57u) * 16u + 5u;
  run_decoder_from(n, late, 18, &r);
  r.last_r = 1;                                    /* that field's ID was before the start */
  printf("write decoder, late start: data %d/%d ok (expect 18/18)\n", r.data_ok, r.data);
  CHECK(r.data_ok == 18, "decoder syncs on the remaining A1s after a late start");
}

static void test_fat12(void)
{
  uint8_t s[512];
  fat12_blank_sector(0, s, "TDSFLOPPY  ");
  CHECK(s[510]==0x55 && s[511]==0xAA, "boot signature 55 AA");
  CHECK(s[21]==0xF0, "media descriptor F0");
  CHECK(s[11]==0x00 && s[12]==0x02, "512 bytes/sector");
  CHECK((s[19]|(s[20]<<8))==2880, "2880 total sectors");
  CHECK(memcmp(&s[54],"FAT12   ",8)==0, "FAT12 fs type string");

  fat12_blank_sector(1, s, NULL);
  CHECK(s[0]==0xF0 && s[1]==0xFF && s[2]==0xFF, "FAT1 reserved entries");
  fat12_blank_sector(10, s, NULL);
  CHECK(s[0]==0xF0 && s[1]==0xFF && s[2]==0xFF, "FAT2 reserved entries");

  FILE *f = fopen("build/blank144.img", "wb");
  if(f){
    for(uint32_t lba=0; lba<FAT12_TOTAL_SECTORS; lba++){
      fat12_blank_sector(lba, s, (lba? NULL : "TDSFLOPPY  "));
      if(lba==19) fat12_blank_sector(19, s, "TDSFLOPPY  ");
      fwrite(s,1,512,f);
    }
    fclose(f);
    printf("wrote blank144.img (1474560 bytes)\n");
  }
}

/* Walk TEST.BIN through the FAT exactly as a FAT12 driver would. */
static void test_testimg(void)
{
  static uint8_t fat1[9 * 512], fat2[9 * 512];
  uint8_t s[512];
  for(unsigned i = 0; i < 9; i++) { testimg_sector(1 + i, &fat1[i * 512]); testimg_sector(10 + i, &fat2[i * 512]); }
  CHECK(memcmp(fat1, fat2, sizeof fat1) == 0, "FAT1 == FAT2");
  CHECK(fat1[0] == 0xF0 && fat1[1] == 0xFF && fat1[2] == 0xFF, "FAT media/reserved bytes");

  testimg_sector(19, s);
  CHECK(memcmp(s, "TDSFLOPPY  ", 11) == 0 && s[11] == 0x08, "volume label entry");
  const uint8_t *e = s + 32;
  CHECK(memcmp(e, "TEST    BIN", 11) == 0, "TEST.BIN dir entry name");
  uint32_t size = e[28] | e[29] << 8 | e[30] << 16 | (uint32_t)e[31] << 24;
  uint32_t cl = e[26] | e[27] << 8;
  CHECK(size == TESTIMG_FILE_SIZE, "TEST.BIN size");

  uint32_t off = 0, n_cl = 0, bad = 0;
  while(cl >= 2 && cl < 0xFF8 && n_cl < 3000)
  {
    testimg_sector(33 + (cl - 2), s);                 /* cluster -> LBA */
    for(unsigned i = 0; i < 512 && off < size; i++, off++)
      if(s[i] != testimg_pattern(off)) bad++;
    uint32_t k = cl * 3 / 2;                          /* 12-bit FAT decode */
    uint16_t v = (uint16_t)(fat1[k] | fat1[k + 1] << 8);
    cl = (cl & 1) ? (v >> 4) : (v & 0xFFF);
    n_cl++;
  }
  printf("TEST.BIN: %u clusters walked, %u bytes, %u mismatches, end=0x%03X\n",
         (unsigned)n_cl, (unsigned)off, (unsigned)bad, (unsigned)cl);
  CHECK(n_cl == TESTIMG_FILE_SIZE / 512 && off == size && bad == 0, "TEST.BIN chain + data");
  CHECK(cl >= 0xFF8, "chain ends with EOC");

  FILE *f = fopen("build/testimg144.img", "wb");   /* the exact image the fw serves */
  if(f) { for(uint32_t lba = 0; lba < 2880; lba++) { testimg_sector(lba, s); fwrite(s, 1, 512, f); } fclose(f); }
}

/* ---- fatimg: build an image the way data-in will, read it back the way
 * data-out will. The RAM "flash" only allows 1->0 bit changes, like the SST25:
 * programming anything twice or onto non-erased bytes shows up as corruption. */
static uint8_t g_img[2880 * 512];
static int g_overwrites;
static void img_read(uint32_t lba, uint8_t buf[512], void *ctx) { (void)ctx; memcpy(buf, &g_img[lba * 512], 512); }
static void img_prog(uint32_t lba, unsigned off, const void *src, unsigned len, void *ctx)
{
  (void)ctx;
  const uint8_t *p = src;
  for(unsigned i = 0; i < len; i++)
  {
    uint8_t *d = &g_img[lba * 512 + off + i];
    if((*d & p[i]) != p[i]) g_overwrites++;      /* would need a 0->1 change */
    *d &= p[i];
  }
}
static uint8_t file_byte(unsigned f, uint32_t i) { return (uint8_t)(i * 13 + f * 71 + (i >> 8)); }

static void test_fatimg(void)
{
  static const uint32_t sizes[] = { 0, 1, 511, 512, 513, 5000, 100000, 65536 };
  const unsigned nf = sizeof sizes / sizeof sizes[0];
  fatimg_io_t io = { img_read, img_prog, NULL };
  static fatimg_build_t b;
  memset(g_img, 0xFF, sizeof g_img);               /* erased flash */
  g_overwrites = 0;

  fatimg_build_begin(&b, &io, false);
  for(unsigned f = 0; f < nf; f++)
  {
    uint32_t lba;
    CHECK(fatimg_alloc(&b, sizes[f], &lba), "alloc fits");
    uint8_t s[512];
    for(uint32_t off = 0; off < sizes[f]; off += 512, lba++)
    {
      memset(s, 0xFF, sizeof s);                   /* tail of last sector stays erased */
      for(unsigned i = 0; i < 512 && off + i < sizes[f]; i++) s[i] = file_byte(f, off + i);
      img_prog(lba, 0, s, 512, NULL);
    }
    if(f == 3) { uint32_t dummy; CHECK(fatimg_alloc(&b, 3000, &dummy), "alloc (abandoned copy)"); }
    char name[12];
    snprintf(name, sizeof name, "FILE%d   BIN", f);
    if(f == 3) continue;                           /* simulate a failed copy: never committed */
    fatimg_commit(&b, name, sizes[f], 0x5A21, 0x6000);
  }
  fatimg_finish(&b);
  printf("fatimg build: %d bad programs (expect 0)\n", g_overwrites);
  CHECK(g_overwrites == 0, "builder only programs erased bytes");

  FILE *fp = fopen("build/fatimg_test.img", "wb");  /* for an independent check (tools/fatinfo.py) */
  if(fp) { fwrite(g_img, 1, sizeof g_img, fp); fclose(fp); }

  static fatimg_read_t r;
  fatimg_file_t fi;
  CHECK(fatimg_open(&r, &io), "open built image");
  unsigned found = 0, bad = 0;
  while(fatimg_next_file(&r, &fi))
  {
    unsigned f = (unsigned)(fi.name83[4] - '0');
    uint32_t got = 0;
    for(uint16_t cl = fi.first_cl; cl && got < fi.size; cl = fatimg_next_cluster(&r, cl))
    {
      uint8_t s[512];
      img_read(fatimg_cluster_lba(&r, cl), s, NULL);
      for(unsigned i = 0; i < 512 && got < fi.size; i++, got++) if(s[i] != file_byte(f, got)) bad++;
    }
    if(got != sizes[f] || fi.size != sizes[f]) bad++;
    found++;
  }
  printf("fatimg read-back: %u files (expect %u), %u bad bytes\n", found, nf - 1, bad);
  CHECK(found == nf - 1 && bad == 0, "every committed file reads back exactly");
  CHECK(!fatimg_is_empty(&io), "is_empty false with files");

  /* capacity: 2847 clusters; a full disk must refuse the next file */
  fatimg_build_begin(&b, &io, false);
  uint32_t lba; unsigned n = 0;
  while(fatimg_alloc(&b, 100000, &lba)) n++;       /* 196 clusters each */
  printf("fatimg capacity: %u x 100000-byte files fit (expect 14)\n", n);
  CHECK(n == 14, "capacity limit");

  /* empty image */
  memset(g_img, 0xFF, sizeof g_img);
  fatimg_build_begin(&b, &io, false);
  fatimg_finish(&b);
  CHECK(fatimg_is_empty(&io), "freshly built image is empty");

  /* corrupt chain (loop) must not hang a reader that bounds by size */
  CHECK(fatimg_next_cluster(&r, 0) == 0 && fatimg_next_cluster(&r, 4000) == 0, "out-of-range clusters end the chain");
}

/* ---- fatimg directory walk: subdirectories as the scope makes them (MKDir) ----
 * A built image (one root file) plus a tree written by hand: a two-cluster
 * subdirectory holding a nested one, a directory whose chain loops on itself,
 * and a file whose chain is shorter than its size. */
static void fat_set(uint16_t cl, uint16_t v)          /* both FATs, FAT12 packing */
{
  for(unsigned fat = 0; fat < 2; fat++)
  {
    uint8_t *p = &g_img[(1u + fat * 9u) * 512u + cl + cl / 2u];
    if(cl & 1) { p[0] = (uint8_t)((p[0] & 0x0F) | (v << 4)); p[1] = (uint8_t)(v >> 4); }
    else       { p[0] = (uint8_t)v; p[1] = (uint8_t)((p[1] & 0xF0) | ((v >> 8) & 0x0F)); }
  }
}
static void dirent(uint8_t *e, const char n83[11], uint8_t attr, uint16_t cl, uint32_t size)
{
  memset(e, 0, 32);
  memcpy(e, n83, 11);
  e[11] = attr;
  e[26] = (uint8_t)cl; e[27] = (uint8_t)(cl >> 8);
  e[28] = (uint8_t)size; e[29] = (uint8_t)(size >> 8); e[30] = (uint8_t)(size >> 16); e[31] = (uint8_t)(size >> 24);
}
static uint8_t *clus(uint16_t cl) { return &g_img[(33u + cl - 2u) * 512u]; }

static void test_fatimg_dirs(void)
{
  fatimg_io_t io = { img_read, img_prog, NULL };
  static fatimg_build_t b;
  static fatimg_read_t r;
  memset(g_img, 0xFF, sizeof g_img);
  fatimg_build_begin(&b, &io, false);
  uint32_t lba;
  fatimg_alloc(&b, 10, &lba);
  fatimg_commit(&b, "ROOTFILETXT", 10, 0, 0);
  fatimg_finish(&b);

  uint8_t *root = &g_img[19u * 512u];                 /* entry 0 = ROOTFILE.TXT */
  dirent(root + 32,  "SUB        ", 0x10, 2000, 0);
  dirent(root + 64,  "LOOP       ", 0x10, 2010, 0);
  dirent(root + 96,  "CORRUPT BIN", 0x20, 2020, 5000); /* needs 10 clusters, has 1 */
  fat_set(2020, 0xFFF);

  memset(clus(2000), 0, 512);                           /* SUB, first cluster: full */
  dirent(clus(2000) + 0,  ".          ", 0x10, 2000, 0);
  dirent(clus(2000) + 32, "..         ", 0x10, 0, 0);
  dirent(clus(2000) + 64, "INNER   TXT", 0x20, 2001, 100);
  dirent(clus(2000) + 96, "DEEP       ", 0x10, 2002, 0);
  for(unsigned i = 4; i < 16; i++) { dirent(clus(2000) + i * 32, "GONE    TXT", 0x20, 0, 0); clus(2000)[i * 32] = 0xE5; }
  clus(2000)[4 * 32 + 11] = 0x0F; clus(2000)[4 * 32] = 0x41;   /* a long-name part, skipped */
  memset(clus(2003), 0, 512);                           /* SUB, second cluster */
  dirent(clus(2003) + 0, "LATE    TXT", 0x20, 2004, 10);
  fat_set(2000, 2003); fat_set(2003, 0xFFF);
  fat_set(2001, 0xFFF); fat_set(2004, 0xFFF);
  memset(clus(2002), 0, 512);                           /* SUB/DEEP */
  dirent(clus(2002) + 0,  ".          ", 0x10, 2002, 0);
  dirent(clus(2002) + 32, "..         ", 0x10, 2000, 0);
  dirent(clus(2002) + 64, "X       BIN", 0x20, 2005, 1);
  fat_set(2002, 0xFFF); fat_set(2005, 0xFFF);
  for(unsigned i = 0; i < 16; i++) { dirent(clus(2010) + i * 32, "LOOPED  TXT", 0x20, 0, 0); clus(2010)[i * 32] = 0xE5; }
  fat_set(2010, 2010);                                  /* LOOP: chain points at itself */

  CHECK(fatimg_open(&r, &io), "open image with a tree");
  /* walk the whole tree, depth first, as DATA OUT does */
  char seen[256] = "";
  fatimg_dir_t st[8];
  int depth = 0, bad_dirs = 0;
  fatimg_file_t f;
  fatimg_dir_root(&st[0]);
  while(depth >= 0)
  {
    if(!fatimg_dir_next(&r, &st[depth], &f)) { bad_dirs += st[depth].bad; depth--; continue; }
    char nm[13]; unsigned k = 0;
    for(unsigned i = 0; i < 8 && f.name83[i] != ' '; i++) nm[k++] = f.name83[i];
    if(f.name83[8] != ' ') { nm[k++] = '.'; for(unsigned i = 8; i < 11 && f.name83[i] != ' '; i++) nm[k++] = f.name83[i]; }
    nm[k] = 0;
    strcat(seen, nm); strcat(seen, (f.attr & FATIMG_ATTR_DIR) ? "/ " : " ");
    if((f.attr & FATIMG_ATTR_DIR) && depth < 7) fatimg_dir_sub(&r, &st[++depth], f.first_cl);
  }
  printf("fatimg tree: %s(bad dirs %d)\n", seen, bad_dirs);
  CHECK(strcmp(seen, "ROOTFILE.TXT SUB/ INNER.TXT DEEP/ X.BIN LATE.TXT LOOP/ CORRUPT.BIN ") == 0,
        "tree walk finds every file and folder, in order, across a two-cluster directory");
  CHECK(bad_dirs == 1, "a looped directory chain ends the walk, flagged bad (no hang)");
  CHECK(fatimg_chain_len(&r, 2020, 10) == 1, "short chain detected (corrupt file)");
  CHECK(fatimg_chain_len(&r, 2000, 10) == 2, "two-cluster chain length");
  CHECK(fatimg_chain_len(&r, 2010, 50) == 50, "looped chain bounded by the limit");
  CHECK(!fatimg_is_empty(&io), "a disk with folders is not empty");

  memset(g_img, 0xFF, sizeof g_img);                    /* a folder alone is not empty either */
  fatimg_build_begin(&b, &io, false); fatimg_finish(&b);
  dirent(root, "ONLYDIR    ", 0x10, 2000, 0);
  CHECK(!fatimg_is_empty(&io), "a disk holding only a folder is not empty");
}

/* 8.3 names for exFAT / long-named stick files (Windows-style numeric tails) */
static void name_is(const char *in, unsigned n, int want_ok, const char *want11)
{
  char out[12] = {0};
  bool ok = fatimg_short_name(in, n, out);
  char msg[96];
  snprintf(msg, sizeof msg, "short name \"%s\" ~%u -> \"%.11s\" (want %s\"%s\")", in, n, out, want_ok ? "" : "fail ", want11 ? want11 : "");
  CHECK(ok == (bool)want_ok && (!want_ok || memcmp(out, want11, 11) == 0), msg);
}

static void test_short_names(void)
{
  name_is("TEST.BIN", 0, 1, "TEST    BIN");
  name_is("readme.txt", 0, 1, "README  TXT");         /* case only: still plain */
  name_is("NOEXT", 0, 1, "NOEXT      ");
  name_is("A_B-C.D", 0, 1, "A_B-C   D  ");
  name_is("TEK00000_1.BMP", 0, 0, 0);                 /* 10-char base: not 8.3 */
  name_is("TEK00000_1.BMP", 1, 1, "TEK000~1BMP");
  name_is("Scope capture 1.bmp", 0, 0, 0);            /* space */
  name_is("Scope capture 1.bmp", 2, 1, "SCOPEC~2BMP");
  name_is("photo.jpeg", 0, 0, 0);                     /* 4-char extension */
  name_is("photo.jpeg", 1, 1, "PHOTO~1 JPE");
  name_is("a.b.c.txt", 0, 0, 0);                      /* extension is after the LAST dot */
  name_is("a.b.c.txt", 1, 1, "ABC~1   TXT");
  name_is("x+y=z.set", 0, 0, 0);                      /* + and = become _ */
  name_is("x+y=z.set", 1, 1, "X_Y_Z~1 SET");
  name_is("LONGBASENAME.WFM", 12, 1, "LONGB~12WFM");  /* 2-digit tail: 5-char base */
  name_is("...", 1, 1, "_~1        ");                /* nothing usable */

  /* uniqueness bookkeeping in the builder */
  static fatimg_build_t b;
  fatimg_io_t io = { img_read, img_prog, NULL };
  uint32_t lba;
  memset(g_img, 0xFF, sizeof g_img);
  fatimg_build_begin(&b, &io, false);
  fatimg_alloc(&b, 10, &lba);
  fatimg_commit(&b, "TEK000~1BMP", 10, 0, 0);
  CHECK(fatimg_name_used(&b, "TEK000~1BMP"), "committed name is reported as used");
  CHECK(!fatimg_name_used(&b, "TEK000~2BMP"), "other name is free");
}

/* firmware-update format (update.c) */
static void test_update_format(void)
{
  CHECK(upd_crc32(0, "123456789", 9) == 0xCBF43926u, "CRC-32 check value (zlib)");
  CHECK(upd_crc32(upd_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u, "CRC-32 in pieces");

  upd_header_t h;
  memset(&h, 0, sizeof h);
  memcpy(h.magic, UPD_MAGIC, 8); memcpy(h.board, UPD_BOARD, 8);
  h.format = UPD_FORMAT; h.version = 0x010000; h.length = 45000; h.image_crc = 0x12345678;
  h.header_crc = upd_crc32(0, &h, offsetof(upd_header_t, header_crc));
  CHECK(upd_header_ok(&h), "valid update header accepted");
  upd_header_t b = h; b.board[0] = 'X';                 CHECK(!upd_header_ok(&b), "wrong board rejected");
  b = h; b.magic[7] = '?';                              CHECK(!upd_header_ok(&b), "wrong magic rejected");
  b = h; b.version++;                                   CHECK(!upd_header_ok(&b), "header CRC catches a changed field");
  b = h; b.length = UPD_IMAGE_MAX + 4;
  b.header_crc = upd_crc32(0, &b, offsetof(upd_header_t, header_crc)); CHECK(!upd_header_ok(&b), "oversized image rejected");
  b = h; b.length = 45001;
  b.header_crc = upd_crc32(0, &b, offsetof(upd_header_t, header_crc)); CHECK(!upd_header_ok(&b), "length not a multiple of 4 rejected");
  b = h; b.format = 2;
  b.header_crc = upd_crc32(0, &b, offsetof(upd_header_t, header_crc)); CHECK(!upd_header_ok(&b), "unknown format rejected");

  const uint8_t good[8] = { 0x00,0x80,0x00,0x20,  0x01,0x31,0x00,0x08 };   /* SP 0x20008000, PC 0x08003101 */
  const uint8_t old0[8] = { 0x00,0x80,0x00,0x20,  0x01,0x11,0x00,0x08 };   /* linked at 0x08000000 */
  const uint8_t arm [8] = { 0x00,0x80,0x00,0x20,  0x00,0x31,0x00,0x08 };   /* not Thumb */
  const uint8_t badsp[8]= { 0xFF,0xFF,0xFF,0xFF,  0x01,0x31,0x00,0x08 };   /* erased flash */
  CHECK(upd_vectors_ok(good, 45000), "app vector table accepted");
  CHECK(!upd_vectors_ok(old0, 45000), "image linked at 0x08000000 rejected");
  CHECK(!upd_vectors_ok(arm, 45000), "non-Thumb reset vector rejected");
  CHECK(!upd_vectors_ok(badsp, 45000), "erased vector table rejected");
  CHECK(!upd_vectors_ok(good, 0x1000), "reset vector outside the image rejected");
}

/* button gestures (buttons.c): feed timed segments {raw state, ms}, collect gestures */
typedef struct { uint16_t raw; uint32_t ms; } btn_seg_t;
static unsigned btn_run(const btn_seg_t *seg, unsigned n, uint16_t *out, uint32_t *at, bool *armed_seen)
{
  buttons_t b = {0};
  unsigned k = 0; uint32_t t = 0;
  *armed_seen = false;
  for(unsigned i = 0; i < n; i++)
    for(uint32_t ms = 0; ms < seg[i].ms; ms++, t++)
    {
      uint16_t g = buttons_step(&b, seg[i].raw);
      if(buttons_armed(&b)) *armed_seen = true;
      if(g && k < 8) { out[k] = g; at[k] = t; k++; }
    }
  return k;
}

static void test_buttons(void)
{
  enum { L = BUTTON_LEFT, R = BUTTON_RIGHT, LR = BUTTON_LEFT | BUTTON_RIGHT };
  uint16_t g[8]; uint32_t at[8]; bool armed; unsigned n;

  const btn_seg_t right[] = { {0,100}, {R,200}, {0,200} };
  n = btn_run(right, 3, g, at, &armed);
  CHECK(n == 1 && g[0] == BUTTON_RIGHT, "right press -> RIGHT");
  CHECK(n == 1 && at[0] >= 300, "RIGHT fires on release, not on press");

  const btn_seg_t bounce[] = { {0,50}, {L,3}, {0,4}, {L,2}, {0,5}, {L,6}, {0,300} };
  n = btn_run(bounce, 7, g, at, &armed);
  CHECK(n == 0, "contact bounce shorter than the debounce time -> nothing");

  const btn_seg_t bouncy[] = { {0,50}, {L,3}, {0,2}, {L,200}, {0,4}, {L,3}, {0,300} };
  n = btn_run(bouncy, 7, g, at, &armed);
  CHECK(n == 1 && g[0] == BUTTON_LEFT, "bouncy left press -> exactly one LEFT");

  const btn_seg_t upd[] = { {0,50}, {LR,3100}, {0,100} };
  n = btn_run(upd, 3, g, at, &armed);
  CHECK(n == 1 && g[0] == BUTTON_UPDATE && armed, "both held 3.1 s -> armed, then UPDATE only");

  const btn_seg_t shortboth[] = { {0,50}, {LR,1000}, {0,100} };
  n = btn_run(shortboth, 3, g, at, &armed);
  CHECK(n == 0 && !armed, "both held 1 s -> nothing (not LEFT, not RIGHT)");

  const btn_seg_t staggered[] = { {0,50}, {L,500}, {LR,3100}, {R,400}, {0,100} };
  n = btn_run(staggered, 5, g, at, &armed);
  CHECK(n == 1 && g[0] == BUTTON_UPDATE, "left, then both 3.1 s, released one at a time -> one UPDATE");

  const btn_seg_t broken[] = { {0,50}, {LR,2000}, {L,100}, {LR,2000}, {0,100} };
  n = btn_run(broken, 5, g, at, &armed);
  CHECK(n == 0 && !armed, "hold interrupted (2 s + 2 s) -> nothing: must be continuous");

  const btn_seg_t two[] = { {0,50}, {L,200}, {0,300}, {R,200}, {0,300} };
  n = btn_run(two, 5, g, at, &armed);
  CHECK(n == 2 && g[0] == BUTTON_LEFT && g[1] == BUTTON_RIGHT, "left then right, separately -> LEFT, RIGHT");

  const btn_seg_t idle[] = { {0,10000} };
  n = btn_run(idle, 1, g, at, &armed);
  CHECK(n == 0, "no presses -> nothing");
}

/* buttons_resync: a transfer blocks the 1 ms calls for seconds; afterwards the
 * logic restarts from the pins, and a button held across the pause is ignored */
static uint16_t btn_hold(buttons_t *b, uint16_t raw, unsigned ms)
{
  uint16_t got = 0;
  while(ms--) got |= buttons_step(b, raw);
  return got;
}
static void test_buttons_resync(void)
{
  enum { L = BUTTON_LEFT, R = BUTTON_RIGHT, LR = BUTTON_LEFT | BUTTON_RIGHT };
  buttons_t b = {0};
  uint16_t got = btn_hold(&b, R, 100);             /* pressed before the pause ... */
  buttons_resync(&b, R);                           /* ... and still held after it */
  got |= btn_hold(&b, R, 200) | btn_hold(&b, 0, 100);
  CHECK(got == 0, "button held across a pause: no gesture on release");
  got = btn_hold(&b, L, 100) | btn_hold(&b, 0, 100);
  CHECK(got == BUTTON_LEFT, "after the pause, a new press works");

  buttons_t c = {0};
  btn_hold(&c, LR, 500);                           /* both, 0.5 s before the pause */
  buttons_resync(&c, LR);
  got = btn_hold(&c, LR, 1000) | btn_hold(&c, 0, 100);
  CHECK(got == 0, "both held across a pause, then 1 s: nothing (the pause is not hold time)");
  buttons_resync(&c, LR);
  got = btn_hold(&c, LR, 3100) | btn_hold(&c, 0, 100);
  CHECK(got == BUTTON_UPDATE, "a deliberate 3 s two-button hold after a pause still updates");
}
/* 720 KB (DD) geometry: 9 sectors in 6250 bytes, same field layout */
static void test_mfm_dd(void)
{
  mfm_track_t t = make_track(3, 1, false);
  t.g = &mfm_geom_dd;
  size_t n = mfm_encode_track(&t, g_mfm, sizeof g_mfm);
  CHECK(n == 2u * 6250u, "DD track: 6250 data bytes per revolution");
  static uint8_t back[MFM_TRACK_SIZE];
  memset(back, 0, sizeof back);
  int good = mfm_decode_verify(g_mfm, n, back);
  CHECK(good == 9, "DD track: 9 sectors decode with good CRCs");
  CHECK(memcmp(back, g_src, 9u * MFM_SECTOR_SIZE) == 0, "DD track: sector data round-trips");
  uint8_t v, sy;
  mfm_track_byte(&t, MFM_TRACK_PRE + 9u * 654u, &v, &sy);
  CHECK(v == 0x4E && sy == MFM_PLAIN, "DD track: gap4b after the 9th sector");
  mfm_track_byte(&t, MFM_TRACK_PRE + 8u * 654u + 18u, &v, &sy);
  CHECK(v == 9, "DD track: the last ID field says sector 9");
  mfm_track_t h = make_track(3, 1, false);        /* NULL geometry is still HD */
  CHECK(mfm_encode_track(&h, g_mfm, sizeof g_mfm) == 2u * 12500u, "NULL geometry = HD");
}

/* 720 KB DD: build a volume (2 sectors per cluster), check its BPB and limits,
 * read every file back through the sector iterator, and walk a folder entry in
 * the SECOND sector of a folder cluster. */
static const uint32_t dd_sizes[7] = { 0, 1, 512, 1024, 1025, 5000, 70000 };
static uint8_t dd_data[70016], dd_got[70016];
static void dd_fat_set(uint16_t cl, uint16_t v)        /* both 3-sector FATs */
{
  for(unsigned fat = 0; fat < 2; fat++)
  {
    uint8_t *p = &g_img[(1u + fat * 3u) * 512u + cl + cl / 2u];
    if(cl & 1) { p[0] = (uint8_t)((p[0] & 0x0F) | (v << 4)); p[1] = (uint8_t)(v >> 4); }
    else       { p[0] = (uint8_t)v; p[1] = (uint8_t)((p[1] & 0xF0) | ((v >> 8) & 0x0F)); }
  }
}
static void test_fatimg_dd(void)
{
  fatimg_io_t io = { img_read, img_prog, NULL };
  static fatimg_build_t b;
  static fatimg_read_t r;
  for(unsigned i = 0; i < sizeof dd_data; i++) dd_data[i] = (uint8_t)(i * 13u + (i >> 8));
  memset(g_img, 0xFF, sizeof g_img);
  fatimg_build_begin(&b, &io, true);
  for(unsigned i = 0; i < 7; i++)
  {
    uint32_t lba;
    CHECK(fatimg_alloc(&b, dd_sizes[i], &lba), "DD: file allocated");
    for(uint32_t off = 0; off < dd_sizes[i]; off += 512)
      img_prog(lba + off / 512, 0, dd_data + off + i, (dd_sizes[i] - off < 512) ? dd_sizes[i] - off : 512, NULL);
    char n83[12];
    snprintf(n83, sizeof n83, "DDFILE%u BIN", i);
    fatimg_commit(&b, n83, dd_sizes[i], 0, 0);
  }
  fatimg_finish(&b);
  const uint8_t *bs = g_img;
  CHECK(bs[13] == 2 && bs[21] == 0xF9 && (bs[19] | bs[20] << 8) == 1440 && (bs[17] | bs[18] << 8) == 112
        && bs[22] == 3 && bs[24] == 9 && bs[510] == 0x55, "DD: the BPB describes a 720 KB volume");
  CHECK(fatimg_open(&r, &io) && r.spc == 2 && r.media == 0xF9 && r.data_lba == 14, "DD: the reader opens it (2 sectors per cluster)");
  CHECK(fatimg_valid(&r, &io), "DD: fatimg_valid (media F9 at the start of both FATs)");
  CHECK(fatimg_fat_entry(&r, 0) == 0xFF7 && fatimg_fat_entry(&r, 1) == 0xFF7
        && fatimg_fat_entry(&r, (uint16_t)(r.max_cl + 1u)) == 0xFF7 && fatimg_fat_entry(&r, r.max_cl) == 0,
        "fatimg_fat_entry: outside 2..max_cl reads as bad, never free");
  fatimg_open(&r, &io);
  fatimg_file_t f;
  unsigned nf = 0, bad = 0;
  while(fatimg_next_file(&r, &f))
  {
    fatimg_pos_t p;
    uint32_t lba, n, at = 0;
    uint8_t sec[512];
    fatimg_pos_start(&p, &f);
    while((n = fatimg_pos_next(&r, &p, &lba)) != 0) { img_read(lba, sec, NULL); memcpy(dd_got + at, sec, n); at += n; }
    unsigned i = (unsigned)(f.name83[6] - '0');
    if(i > 6 || at != dd_sizes[i] || p.left || memcmp(dd_got, dd_data + i, at) != 0) bad++;
    if(i <= 6 && fatimg_clusters_for(&r, dd_sizes[i]) != (dd_sizes[i] + 1023u) / 1024u) bad++;
    nf++;
  }
  printf("fatimg DD: %u files read back, %u bad\n", nf, bad);
  CHECK(nf == 7 && bad == 0, "DD: every file reads back, byte for byte, through the sector iterator");

  fatimg_build_begin(&b, &io, true);             /* limits */
  CHECK(fatimg_fits(&b, FATIMG_DD_BYTES) && !fatimg_fits(&b, FATIMG_DD_BYTES + 1u), "DD: capacity is 713 x 1 KB clusters");
  unsigned k = 0;
  uint32_t lba;
  while(fatimg_fits(&b, 0) && k < 300) { char n83[12]; fatimg_alloc(&b, 0, &lba); snprintf(n83, sizeof n83, "E%07u   ", k++); fatimg_commit(&b, n83, 0, 0, 0); }
  CHECK(k == 112, "DD: 112 root entries");

  memset(g_img, 0xFF, sizeof g_img);             /* a folder whose entry is in its cluster's 2nd sector */
  fatimg_build_begin(&b, &io, true);
  fatimg_alloc(&b, 10, &lba); fatimg_commit(&b, "ROOTFILETXT", 10, 0, 0);
  fatimg_finish(&b);
  uint8_t *root = &g_img[7u * 512u];
  dirent(root + 32, "SUBDD      ", 0x10, 300, 0);
  uint8_t *c0 = &g_img[(14u + (300u - 2u) * 2u) * 512u];
  for(unsigned i = 0; i < 16; i++) { dirent(c0 + i * 32, "GONE    TXT", 0x20, 0, 0); c0[i * 32] = 0xE5; }
  memset(c0 + 512, 0, 512);
  dirent(c0 + 512, "INNER2  TXT", 0x20, 0, 0);           /* entry 16: the 2nd sector' first */
  dd_fat_set(300, 0xFFF);
  fatimg_open(&r, &io);
  fatimg_dir_t d[2];
  int found = 0;
  fatimg_dir_root(&d[0]);
  while(fatimg_dir_next(&r, &d[0], &f))
    if(f.attr & FATIMG_ATTR_DIR)
    {
      fatimg_dir_sub(&r, &d[1], f.first_cl);
      fatimg_file_t g;
      while(fatimg_dir_next(&r, &d[1], &g)) found += memcmp(g.name83, "INNER2  TXT", 11) == 0;
    }
  CHECK(found == 1, "DD: a folder entry in the second sector of a 2-sector cluster is found");
}

/* density switch decision: only a whole-track write (a format) at the other
 * rate switches; sector writes, noise and ambiguous timing never do */
static void rate_fill(mfm_rate_t *r, unsigned n, const unsigned us[3], uint32_t tpu)
{
  for(unsigned i = 0; i < n; i++)
  {
    uint32_t dt = us[i % 3] * tpu;
    dt = dt + dt * ((int)(i % 11) - 5) / 100;     /* +-5 % jitter */
    mfm_rate_add(r, dt, tpu);
  }
}
static void test_rate(void)
{
  static const unsigned DD[3] = { 4, 6, 8 }, HD[3] = { 2, 3, 4 }, AMB[3] = { 4, 4, 4 };
  mfm_rate_t d = {0}, h = {0}, m = {0}, a = {0}, few = {0}, d72 = {0};
  rate_fill(&d, 30000, DD, 144);
  CHECK(mfm_rate_decide(&d, false, 200) == 1, "rate: a DD format on an HD disk switches to DD");
  CHECK(mfm_rate_decide(&d, false, 20) == -1, "rate: a DD SECTOR write (20 ms) on an HD disk does not switch");
  CHECK(mfm_rate_decide(&d, false, 149) == -1 && mfm_rate_decide(&d, false, 150) == 1, "rate: 150 ms is the format threshold");
  CHECK(mfm_rate_decide(&d, true, 200) == -1, "rate: DD writes on a DD disk never switch");
  rate_fill(&h, 60000, HD, 144);
  CHECK(mfm_rate_decide(&h, true, 200) == 0 && mfm_rate_decide(&h, false, 200) == -1, "rate: an HD format switches a DD disk only");
  rate_fill(&m, 3000, DD, 144); rate_fill(&m, 3000, HD, 144);
  CHECK(mfm_rate_decide(&m, false, 200) == -1 && mfm_rate_decide(&m, true, 200) == -1, "rate: mixed evidence never switches");
  rate_fill(&a, 30000, AMB, 144);
  CHECK(a.hd == 0 && a.dd == 0 && mfm_rate_decide(&a, false, 200) == -1, "rate: 4 us intervals (both densities) count for neither");
  rate_fill(&few, 90, DD, 144);
  CHECK(mfm_rate_decide(&few, false, 200) == -1, "rate: too little evidence never switches");
  rate_fill(&d72, 30000, DD, 72);
  CHECK(mfm_rate_decide(&d72, false, 200) == 1, "rate: thresholds follow the timer rate (72 ticks/us)");
}

/* the status report EMUSTAT.TXT (status.c) */
static void test_status(void)
{
  static char out[4096];
  status_t st;
  memset(&st, 0, sizeof st);
  st.version = "1.2.0"; st.build = 0xd63f10ebu; st.board = "SFRC2D.B";
  st.uid[0] = 0x11223344u; st.uid[1] = 0x55667788u; st.uid[2] = 0x99AABBCCu;
  st.flash_id = 0xBF2541u;
  st.disk_ok = true; st.files = 12; st.dirs = 1; st.used_bytes = 44032; st.free_bytes = 1413632;
  st.writebacks = 303; st.reset_cause = 0x0C000000u; st.uptime_ms = 5025000u;
  st.stick_fs = "exFAT"; st.stick_bytes = 7751090176ull;
  st.last_what = "DATA OUT"; st.last_result = "OK"; st.last_files = 5; st.last_bytes = 19682;
  size_t n = status_text(out, sizeof out, &st);
  CHECK(n == strlen(out) && n > 400, "status: text written, NUL-terminated");
  CHECK(strstr(out, "Firmware version  : 1.2.0\r\n") && strstr(out, "Build ID          : D63F10EB\r\n"),
        "status: version and build ID");
  CHECK(strstr(out, "MCU unique ID     : 99AABBCC-55667788-11223344") && strstr(out, "SPI flash ID      : BF2541"),
        "status: unit identity");
  CHECK(strstr(out, "Clock             : 8 MHz crystal\r\n") != NULL, "status: clock source (crystal)");
  CHECK(strstr(out, "Density           : 1.44 MB (HD)") && strstr(out, "Files             : 12\r\n") && strstr(out, "Folders           : 1\r\n")
        && strstr(out, "Free              : 1,413,632 bytes"), "status: disk, with thousands separators");
  CHECK(strstr(out, "Last reset        : power-on") && strstr(out, "Uptime            : 1 h 23 min 45 s"),
        "status: reset cause and uptime");
  CHECK(strstr(out, "Capacity          : 7,751,090,176 bytes") && strstr(out, "Transfer          : DATA OUT, OK, 5 files, 19,682 bytes"),
        "status: stick and last transfer");
  CHECK(strstr(out, "\r\n") && !strstr(out, "\n\n"), "status: CRLF lines only");

  st.dd = true; st.dirs = 3; st.fault = true; st.hick = true; st.reset_cause = 0x34000000u;   /* watchdog + sw + pin */
  st.last_what = NULL; st.disk_ok = false;
  status_text(out, sizeof out, &st);
  CHECK(strstr(out, "720 KB (DD)") && strstr(out, "YES: save the files") && strstr(out, "Last reset        : watchdog")
        && strstr(out, "None since power-on") && strstr(out, "not a readable FAT12 disk"),
        "status: DD, fault, watchdog first, no transfer, unreadable disk");
  CHECK(strstr(out, "Clock             : internal oscillator") != NULL, "status: clock source (crystal failed)");
  CHECK(strcmp(status_reset_text(0x10000000u), "software (update or restart)") == 0
        && strcmp(status_reset_text(0x04000000u), "reset pin") == 0 && strcmp(status_reset_text(0), "unknown") == 0,
        "status: reset cause words");

  memset(out, 'x', sizeof out);
  n = status_text(out, 20, &st);
  CHECK(n == 19 && out[19] == 0 && out[20] == 'x', "status: a small buffer is cut off safely");
  CHECK(status_text(out, 0, &st) == 0 && out[20] == 'x', "status: a zero-size buffer writes nothing");
}

int main(void)
{
  test_crc();
  test_track_roundtrip();
  test_write_decoder();
  test_fat12();
  test_testimg();
  test_mfm_dd();
  test_rate();
  test_status();
  test_fatimg();
  test_fatimg_dirs();
  test_fatimg_dd();
  test_short_names();
  test_update_format();
  test_buttons();
  test_buttons_resync();
  printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
  return fails ? 1 : 0;
}
