/**
 * fatimg.c - read/build a 1.44 MB FAT12 image through fatimg_io_t. See fatimg.h.
 */
#include "fatimg.h"
#include "fat12.h"
#include <string.h>

/* Geometry of the images we BUILD: standard DOS 1.44 MB HD and 720 KB DD, as
 * fat12.c. FATs start at LBA 1, the root directory follows both FATs. */
#define B_FAT_LBA     1u
typedef struct { uint16_t fat_secs, root_secs, data_lba, clusters, max_files; uint8_t spc, media; } bgeom_t;
static const bgeom_t BG[2] = {
  { 9u, 14u, 33u, 2847u, 224u, 1u, 0xF0u },     /* HD: 2880 sectors */
  { 3u,  7u, 14u,  713u, 112u, 2u, 0xF9u },     /* DD: 1440 sectors */
};
_Static_assert(2880u - 33u == FATIMG_DATA_CLUSTERS, "fatimg.h HD geometry");
_Static_assert((1440u - 14u) / 2u == FATIMG_DD_CLUSTERS, "fatimg.h DD geometry");
#define G(b)          (&BG[(b)->dd ? 1 : 0])
#define B_ROOT_LBA(b) (B_FAT_LBA + 2u * G(b)->fat_secs)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ================================ build ================================ */
void fatimg_build_begin(fatimg_build_t *b, const fatimg_io_t *io, bool dd)
{
  memset(b, 0, sizeof *b);
  b->io = *io;
  b->dd = dd;
  b->next_cl = 2;
}

static uint16_t clusters_for(const fatimg_build_t *b, uint32_t size)
{
  uint32_t cb = FATIMG_SECTOR * G(b)->spc;
  return (uint16_t)((size + cb - 1u) / cb);
}

bool fatimg_fits(const fatimg_build_t *b, uint32_t size)
{
  if(b->nfiles >= G(b)->max_files) return false;
  return (uint32_t)b->next_cl - 2u + clusters_for(b, size) <= G(b)->clusters;
}

bool fatimg_alloc(fatimg_build_t *b, uint32_t size, uint32_t *first_lba)
{
  if(!fatimg_fits(b, size)) return false;
  b->pend_n = clusters_for(b, size);
  b->pend_cl = b->pend_n ? b->next_cl : 0;          /* empty file: no clusters */
  b->next_cl = (uint16_t)(b->next_cl + b->pend_n);
  *first_lba = G(b)->data_lba + (b->pend_cl ? (uint32_t)(b->pend_cl - 2u) * G(b)->spc : 0u);   /* contiguous */
  return true;
}

void fatimg_commit(fatimg_build_t *b, const char name83[11], uint32_t size, uint16_t date, uint16_t time)
{
  uint8_t e[32];
  memset(e, 0, sizeof e);
  memcpy(e, name83, 11);
  e[11] = 0x20;                                     /* archive */
  e[14] = (uint8_t)time; e[15] = (uint8_t)(time >> 8);   /* created */
  e[16] = (uint8_t)date; e[17] = (uint8_t)(date >> 8);
  e[18] = (uint8_t)date; e[19] = (uint8_t)(date >> 8);   /* accessed */
  e[22] = (uint8_t)time; e[23] = (uint8_t)(time >> 8);   /* written */
  e[24] = (uint8_t)date; e[25] = (uint8_t)(date >> 8);
  e[26] = (uint8_t)b->pend_cl; e[27] = (uint8_t)(b->pend_cl >> 8);
  e[28] = (uint8_t)size; e[29] = (uint8_t)(size >> 8); e[30] = (uint8_t)(size >> 16); e[31] = (uint8_t)(size >> 24);
  uint32_t off = (uint32_t)b->nfiles * 32u;
  b->io.program(B_ROOT_LBA(b) + off / FATIMG_SECTOR, off % FATIMG_SECTOR, e, 32, b->io.ctx);
  b->ext_cl[b->nfiles] = b->pend_cl;
  b->ext_n[b->nfiles] = b->pend_n;
  memcpy(b->names[b->nfiles], name83, 11);
  b->nfiles++;
  b->pend_n = 0;
}

/* FAT12 value for cluster c, from the committed extents */
static uint16_t fat_value(const fatimg_build_t *b, uint32_t c)
{
  if(c == 0) return (uint16_t)(0xF00u | G(b)->media);   /* media byte (F0 HD, F9 DD) */
  if(c == 1) return 0xFFF;
  for(unsigned i = 0; i < b->nfiles; i++)
    if(b->ext_n[i] && c >= b->ext_cl[i] && c < (uint32_t)b->ext_cl[i] + b->ext_n[i])
      return (c == (uint32_t)b->ext_cl[i] + b->ext_n[i] - 1u) ? 0xFFF : (uint16_t)(c + 1);
  return 0;                                         /* free */
}

void fatimg_finish(fatimg_build_t *b)
{
  uint8_t s[FATIMG_SECTOR];
  fat12_boot_sector(s, b->dd, NULL);                /* boot sector / BPB */
  b->io.program(0, 0, s, FATIMG_SECTOR, b->io.ctx);

  for(unsigned sec = 0; sec < G(b)->fat_secs; sec++)   /* FAT1 and FAT2 */
  {
    for(unsigned i = 0; i < FATIMG_SECTOR; i++)
    {
      uint32_t k = sec * FATIMG_SECTOR + i;         /* byte k of the FAT */
      uint32_t c = k * 2u / 3u;                     /* cluster whose entry holds it */
      uint16_t v = fat_value(b, c);
      uint8_t byte;
      if((c & 1) == 0) byte = (k % 3u == 0) ? (uint8_t)v : (uint8_t)((v >> 8) | (fat_value(b, c + 1) << 4));
      else             byte = (uint8_t)(v >> 4);    /* odd entry: second byte */
      s[i] = byte;
    }
    b->io.program(B_FAT_LBA + sec, 0, s, FATIMG_SECTOR, b->io.ctx);
    b->io.program(B_FAT_LBA + G(b)->fat_secs + sec, 0, s, FATIMG_SECTOR, b->io.ctx);
  }

  /* unused root entries must read 00 (end of directory), not erased FF */
  memset(s, 0, sizeof s);
  for(uint32_t off = (uint32_t)b->nfiles * 32u; off < G(b)->root_secs * FATIMG_SECTOR; )
  {
    unsigned in = off % FATIMG_SECTOR, n = FATIMG_SECTOR - in;
    b->io.program(B_ROOT_LBA(b) + off / FATIMG_SECTOR, in, s, n, b->io.ctx);
    off += n;
  }
}

/* ================================ read ================================ */
static const uint8_t *cached(fatimg_read_t *r, uint32_t lba)
{
  if(r->cache_lba != lba) { r->io.read(lba, r->cache, r->io.ctx); r->cache_lba = lba; }
  return r->cache;
}

bool fatimg_open(fatimg_read_t *r, const fatimg_io_t *io)
{
  memset(r, 0, sizeof *r);
  r->io = *io;
  r->cache_lba = 0xFFFFFFFFu;
  const uint8_t *s = cached(r, 0);
  uint16_t bps = rd16(&s[11]), res = rd16(&s[14]), root = rd16(&s[17]), spf = rd16(&s[22]);
  uint32_t total = rd16(&s[19]) ? rd16(&s[19]) : rd32(&s[32]);
  uint8_t spc = s[13], nfats = s[16];
  if(s[510] != 0x55 || s[511] != 0xAA || bps != FATIMG_SECTOR || (spc != 1 && spc != 2) || !res || !nfats
     || !spf || !root || root > 1024 || total > 2880 || total < 100)
    return false;                                   /* not a 1.44 MB-style volume */
  r->fat_lba = res;
  r->root_lba = (uint16_t)(res + nfats * spf);
  r->root_entries = root;
  r->data_lba = (uint16_t)(r->root_lba + (root * 32u + FATIMG_SECTOR - 1) / FATIMG_SECTOR);
  r->spc = spc;
  r->media = s[21];
  if(r->data_lba >= total) return false;
  r->max_cl = (uint16_t)((total - r->data_lba) / spc + 1u);   /* highest valid cluster */
  uint32_t fat_entries = (uint32_t)spf * FATIMG_SECTOR * 2u / 3u;
  if(r->max_cl >= fat_entries) r->max_cl = (uint16_t)(fat_entries - 1);
  return r->data_lba < total;
}

bool fatimg_next_file(fatimg_read_t *r, fatimg_file_t *f)
{
  while(r->dir_index < r->root_entries)
  {
    uint32_t off = (uint32_t)r->dir_index++ * 32u;
    const uint8_t *e = cached(r, r->root_lba + off / FATIMG_SECTOR) + off % FATIMG_SECTOR;
    if(e[0] == 0x00) { r->dir_index = r->root_entries; return false; }  /* end of directory */
    if(e[0] == 0xE5 || e[0] == 0x2E) continue;      /* deleted, dot entries */
    if((e[11] & 0x0F) == 0x0F || (e[11] & 0x18)) continue;   /* LFN, volume label, directory */
    memcpy(f->name83, e, 11);
    if(f->name83[0] == 0x05) f->name83[0] = (char)0xE5;
    f->attr = e[11];
    f->time = rd16(&e[22]);
    f->date = rd16(&e[24]);
    f->first_cl = rd16(&e[26]);
    f->size = rd32(&e[28]);
    return true;
  }
  return false;
}

uint16_t fatimg_fat_entry(fatimg_read_t *r, uint16_t cl)
{
  if(cl < 2 || cl > r->max_cl) return 0xFF7;       /* outside the volume */
  uint32_t k = cl + cl / 2u;                        /* byte offset of the entry */
  uint8_t lo = cached(r, r->fat_lba + k / FATIMG_SECTOR)[k % FATIMG_SECTOR];
  uint8_t hi = cached(r, r->fat_lba + (k + 1) / FATIMG_SECTOR)[(k + 1) % FATIMG_SECTOR];
  uint16_t v = (uint16_t)(lo | hi << 8);
  return (cl & 1) ? (uint16_t)(v >> 4) : (uint16_t)(v & 0xFFF);
}

uint16_t fatimg_next_cluster(fatimg_read_t *r, uint16_t cl)
{
  if(cl < 2 || cl > r->max_cl) return 0;
  uint16_t v = fatimg_fat_entry(r, cl);
  return (v >= 2 && v <= r->max_cl) ? v : 0;        /* EOC, free, bad or corrupt -> end */
}

uint32_t fatimg_cluster_lba(const fatimg_read_t *r, uint16_t cl)
{
  return r->data_lba + (uint32_t)(cl - 2u) * r->spc;
}

uint32_t fatimg_clusters_for(const fatimg_read_t *r, uint32_t size)
{
  uint32_t cb = FATIMG_SECTOR * r->spc;
  return (size + cb - 1u) / cb;
}

void fatimg_pos_start(fatimg_pos_t *p, const fatimg_file_t *f)
{
  p->cl = f->first_cl; p->k = 0; p->left = f->size;
}

uint32_t fatimg_pos_next(fatimg_read_t *r, fatimg_pos_t *p, uint32_t *lba)
{
  if(!p->left || p->cl < 2 || p->cl > r->max_cl) return 0;
  *lba = fatimg_cluster_lba(r, p->cl) + p->k;
  uint32_t n = p->left < FATIMG_SECTOR ? p->left : FATIMG_SECTOR;
  p->left -= n;
  if(++p->k >= r->spc) { p->k = 0; p->cl = fatimg_next_cluster(r, p->cl); }
  return n;
}

/* ---- directory walk ---- */
void fatimg_dir_root(fatimg_dir_t *d)
{
  memset(d, 0, sizeof *d);
}

void fatimg_dir_sub(const fatimg_read_t *r, fatimg_dir_t *d, uint16_t first_cl)
{
  memset(d, 0, sizeof *d);
  d->first_cl = first_cl;
  if(first_cl >= 2 && first_cl <= r->max_cl) { d->cl = first_cl; d->clusters = 1; }
  else d->bad = true;                               /* no valid start: nothing to walk */
}

bool fatimg_dir_next(fatimg_read_t *r, fatimg_dir_t *d, fatimg_file_t *f)
{
  for(;;)
  {
    const uint8_t *e;
    if(d->first_cl == 0 && !d->bad)                 /* root: a fixed run of entries */
    {
      if(d->index >= r->root_entries) return false;
      uint32_t off = (uint32_t)d->index++ * 32u;
      e = cached(r, r->root_lba + off / FATIMG_SECTOR) + off % FATIMG_SECTOR;
    }
    else                                            /* subdirectory: a cluster chain */
    {
      if(d->cl == 0) return false;
      if(d->index >= (FATIMG_SECTOR / 32u) * r->spc)     /* 16 or 32 entries per cluster */
      {
        d->cl = fatimg_next_cluster(r, d->cl);      /* 0 at the end (or a corrupt entry) */
        d->index = 0;
        if(d->cl == 0) return false;
        if(++d->clusters > FATIMG_DIR_MAX_CL) { d->bad = true; d->cl = 0; return false; }
      }
      e = cached(r, fatimg_cluster_lba(r, d->cl) + d->index / 16u) + (d->index % 16u) * 32u;
      d->index++;
    }
    if(e[0] == 0x00)                                /* end of the directory */
    {
      if(d->first_cl == 0) d->index = r->root_entries; else d->cl = 0;
      return false;
    }
    if(e[0] == 0xE5 || e[0] == 0x2E) continue;      /* deleted, "." and ".." */
    if((e[11] & 0x0F) == 0x0F || (e[11] & 0x08)) continue;   /* long-name part, volume label */
    memcpy(f->name83, e, 11);
    if(f->name83[0] == 0x05) f->name83[0] = (char)0xE5;
    f->attr = e[11];
    f->time = rd16(&e[22]);
    f->date = rd16(&e[24]);
    f->first_cl = rd16(&e[26]);
    f->size = (e[11] & FATIMG_ATTR_DIR) ? 0 : rd32(&e[28]);
    return true;
  }
}

uint32_t fatimg_chain_len(fatimg_read_t *r, uint16_t first_cl, uint32_t limit)
{
  uint32_t n = 0;
  for(uint16_t cl = first_cl; cl >= 2 && cl <= r->max_cl && n < limit; cl = fatimg_next_cluster(r, cl)) n++;
  return n;
}

bool fatimg_is_empty(const fatimg_io_t *io)
{
  static fatimg_read_t r;                           /* 540 B: keep off the stack */
  fatimg_file_t f;
  fatimg_dir_t d;
  if(!fatimg_open(&r, io)) return true;
  fatimg_dir_root(&d);
  return !fatimg_dir_next(&r, &d, &f);              /* a file or a directory: not empty */
}

bool fatimg_valid(fatimg_read_t *r, const fatimg_io_t *io)
{
  if(!fatimg_open(r, io)) return false;             /* boot sector + BPB sanity */
  uint32_t fat2 = r->fat_lba + (uint32_t)(r->root_lba - r->fat_lba) / 2u;
  uint32_t fats[2] = { r->fat_lba, fat2 };
  for(unsigned k = 0; k < 2; k++)                   /* both FATs start with the media descriptor */
  {
    io->read(fats[k], r->cache, io->ctx);
    r->cache_lba = fats[k];
    if(r->cache[0] != r->media || r->cache[1] != 0xFF || r->cache[2] != 0xFF) return false;
  }
  return true;
}

bool fatimg_name_used(const fatimg_build_t *b, const char name83[11])
{
  for(unsigned i = 0; i < b->nfiles; i++)
    if(memcmp(b->names[i], name83, 11) == 0) return true;
  return false;
}

/* ---- 8.3 names (see fatimg.h) ---- */

/* The character as it may appear in a short name: upper-cased if valid, '_' for
 * characters DOS forbids there (+ , ; = [ ] and non-ASCII), 0 for ones that are
 * dropped (space, '.') or never valid (control characters, \ / : * ? " < > |). */
static char sfn_char(char c)
{
  unsigned char u = (unsigned char)c;
  if(u >= 'a' && u <= 'z') return (char)(u - 32);
  if((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')) return c;
  if(u == ' ' || u == '.') return 0;
  if(u >= 0x80) return '_';
  switch(u)
  {
  case '!': case '#': case '$': case '%': case '&': case '\'': case '(': case ')':
  case '-': case '@': case '^': case '_': case '`': case '{': case '}': case '~':
    return c;
  case '+': case ',': case ';': case '=': case '[': case ']':
    return '_';
  default:
    return 0;
  }
}

bool fatimg_short_name(const char *name, unsigned n, char name83[11])
{
  memset(name83, ' ', 11);
  const char *dot = 0;                              /* extension = after the LAST dot */
  for(const char *p = name; *p; p++) if(*p == '.' && p != name) dot = p;
  const char *end = dot ? dot : name + strlen(name);

  if(n == 0)                                        /* the name itself, if it is 8.3 */
  {
    unsigned k = 0;
    if(name[0] == '.' || end == name || end - name > 8) return false;
    for(const char *p = name; p < end; p++)
    {
      char c = sfn_char(*p);
      if(!c || (c == '_' && *p != '_')) return false; /* would change: not a plain name */
      name83[k++] = c;
    }
    if(dot)
    {
      if(strlen(dot + 1) > 3) return false;
      k = 8;
      for(const char *p = dot + 1; *p; p++)
      {
        char c = sfn_char(*p);
        if(!c || (c == '_' && *p != '_')) return false;
        name83[k++] = c;
      }
    }
    return true;
  }

  char tail[6];                                     /* "~n" */
  unsigned t = 0;
  { char d[5]; unsigned nd = 0; unsigned v = n; do { d[nd++] = (char)('0' + v % 10); v /= 10; } while(v && nd < 4);
    tail[t++] = '~'; while(nd) tail[t++] = d[--nd]; }
  unsigned max_base = 8u - t;
  if(max_base > 6) max_base = 6;
  unsigned k = 0;
  for(const char *p = name; p < end && k < max_base; p++) { char c = sfn_char(*p); if(c) name83[k++] = c; }
  if(k == 0) name83[k++] = '_';                     /* nothing usable in the base */
  for(unsigned i = 0; i < t; i++) name83[k++] = tail[i];
  if(dot)
  {
    k = 8;
    for(const char *p = dot + 1; *p && k < 11; p++) { char c = sfn_char(*p); if(c) name83[k++] = c; }
  }
  return true;
}
