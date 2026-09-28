#ifndef FATIMG_H
#define FATIMG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Read and build a 1.44 MB FAT12 disk image through a tiny I/O interface, so
 * the same code runs on the MCU (SPI-flash buffer) and in host tests (RAM).
 * Pure C, no FatFs. The builder writes the root directory only (DATA IN loads
 * a flat list of files); the reader also walks subdirectories, which the scope
 * can create (DATA OUT copies the whole tree).
 *
 * BUILD (stick -> image): the image must be erased (all 0xFF) first. For each
 * file: fatimg_alloc() reserves contiguous clusters and returns the first data
 * LBA; the caller programs the data; fatimg_commit() then writes the directory
 * entry. A file whose copy fails is simply never committed (its clusters stay
 * free). fatimg_finish() writes the boot sector, both FATs (generated from the
 * committed extents) and clears the rest of the root directory.
 *
 * READ (image -> stick): fatimg_open() checks the BPB; fatimg_next_file()
 * walks the root directory; fatimg_next_cluster() follows a FAT12 chain. All
 * reads are bounds-checked so a corrupt image can't loop forever.
 */

#define FATIMG_SECTOR       512u
#define FATIMG_MAX_FILES    224u        /* root entries on a 1.44 MB disk */
#define FATIMG_DATA_CLUSTERS 2847u      /* 1.44 MB: 2880 sectors - 33 (boot, 2 FATs, root) */
#define FATIMG_DATA_BYTES   (FATIMG_DATA_CLUSTERS * FATIMG_SECTOR)   /* largest file */
#define FATIMG_DD_CLUSTERS  713u        /* 720 KB: (1440 - 14) / 2 */
#define FATIMG_DD_BYTES     (FATIMG_DD_CLUSTERS * 2u * FATIMG_SECTOR)
/* The largest file a disk of that density holds. */
static inline uint32_t fatimg_capacity(bool dd) { return dd ? FATIMG_DD_BYTES : FATIMG_DATA_BYTES; }

typedef struct {
  void (*read)(uint32_t lba, uint8_t buf[FATIMG_SECTOR], void *ctx);
  /* program bytes onto erased (0xFF) storage; only 1->0 bit changes needed */
  void (*program)(uint32_t lba, unsigned off, const void *src, unsigned len, void *ctx);
  void *ctx;
} fatimg_io_t;

/* ---- build ---- */
typedef struct {
  fatimg_io_t io;
  uint16_t next_cl;                    /* next free cluster */
  uint16_t nfiles;                     /* committed files */
  uint16_t pend_cl, pend_n;            /* allocated, not yet committed */
  uint16_t ext_cl[FATIMG_MAX_FILES];   /* committed extents: first cluster */
  uint16_t ext_n[FATIMG_MAX_FILES];    /* ... and cluster count */
  char     names[FATIMG_MAX_FILES][11];/* committed names (uniqueness checks) */
  bool     dd;                         /* building a 720 KB DD volume */
} fatimg_build_t;

/* dd: build a 720 KB DD volume (2 sectors per cluster) instead of 1.44 MB HD. */
void fatimg_build_begin(fatimg_build_t *b, const fatimg_io_t *io, bool dd);
/* Would a file of size bytes still fit (clusters and a directory slot)? */
bool fatimg_fits(const fatimg_build_t *b, uint32_t size);
/* Reserve space; *first_lba = where to program the data (sequential LBAs). */
bool fatimg_alloc(fatimg_build_t *b, uint32_t size, uint32_t *first_lba);
/* name83 = 11 chars, space padded ("TEST    BIN"). date/time in FAT format. */
void fatimg_commit(fatimg_build_t *b, const char name83[11], uint32_t size,
                   uint16_t date, uint16_t time);
void fatimg_finish(fatimg_build_t *b);
/* Is this 8.3 name already on the image being built? */
bool fatimg_name_used(const fatimg_build_t *b, const char name83[11]);

/* ---- names ----
 * An 8.3 name for any file name, following the Windows rules (exFAT sticks have
 * no short names; long names on FAT sticks do). n = 0: the name itself,
 * upper-cased, and false if it isn't a valid 8.3 name. n >= 1: the "numeric
 * tail" form: up to 6 valid characters of the base + "~n" (fewer for n >= 10)
 * and up to 3 of the extension (after the last dot). Characters DOS forbids in
 * short names become '_'; spaces and dots in the base are dropped. */
bool fatimg_short_name(const char *name, unsigned n, char name83[11]);

/* ---- read ---- */
typedef struct {
  fatimg_io_t io;
  uint16_t fat_lba, root_lba, root_entries, data_lba, max_cl;
  uint8_t  spc, media;                 /* sectors per cluster (1 HD, 2 DD), media byte */
  uint16_t dir_index;                  /* iterator position */
  uint32_t cache_lba;                  /* sector cache for FAT/dir reads */
  uint8_t  cache[FATIMG_SECTOR];
} fatimg_read_t;

typedef struct {
  char     name83[11];
  uint8_t  attr;
  uint16_t first_cl, date, time;
  uint32_t size;
} fatimg_file_t;

/* false if the image doesn't look like a sane 512 B/sector FAT12 volume. */
bool fatimg_open(fatimg_read_t *r, const fatimg_io_t *io);
/* Next regular file in the root directory (skips deleted, volume, dirs, LFN). */
bool fatimg_next_file(fatimg_read_t *r, fatimg_file_t *f);

/* ---- directory walk (root and subdirectories) ----
 * The scope can make directories on the floppy (FILESystem:MKDir), so anything
 * that copies the whole disk must walk them. A subdirectory is a cluster chain
 * of 16 entries per cluster. Bounded: a looped or corrupt chain ends the walk
 * with d->bad set, it never hangs. */
#define FATIMG_ATTR_DIR     0x10u
#define FATIMG_DIR_MAX_CL   128u        /* 2048 entries: far beyond a floppy's needs */
typedef struct {
  uint16_t first_cl;                    /* 0 = the root directory */
  uint16_t cl;                          /* current cluster (subdirectory), 0 = ended */
  uint16_t index;                       /* next entry: in the root, or in cl */
  uint16_t clusters;                    /* clusters walked (loop guard) */
  bool     bad;                         /* corrupt or looped chain: walk cut short */
} fatimg_dir_t;
void fatimg_dir_root(fatimg_dir_t *d);
void fatimg_dir_sub(const fatimg_read_t *r, fatimg_dir_t *d, uint16_t first_cl);
/* Next entry: a regular file, or a subdirectory (f->attr & FATIMG_ATTR_DIR).
 * Skips deleted entries, "." and "..", long-name parts and the volume label. */
bool fatimg_dir_next(fatimg_read_t *r, fatimg_dir_t *d, fatimg_file_t *f);
/* Clusters in the chain from first_cl, counting at most limit (loop guard). A
 * file of size bytes is intact only if this reaches its cluster count. */
uint32_t fatimg_chain_len(fatimg_read_t *r, uint16_t first_cl, uint32_t limit);

/* A file's data, sector by sector (clusters may hold 1 or 2 sectors): start,
 * then each call gives the next sector's LBA and how many of its bytes belong
 * to the file; 0 at the end. p->left != 0 afterwards: the chain was short. */
typedef struct { uint16_t cl; uint8_t k; uint32_t left; } fatimg_pos_t;
void fatimg_pos_start(fatimg_pos_t *p, const fatimg_file_t *f);
uint32_t fatimg_pos_next(fatimg_read_t *r, fatimg_pos_t *p, uint32_t *lba);
/* Clusters a file of size bytes needs on this volume. */
uint32_t fatimg_clusters_for(const fatimg_read_t *r, uint32_t size);

/* The raw FAT entry of cluster cl (0 = free); 0xFF7 (bad) for a cluster
 * outside 2..max_cl, so no caller can mistake it for a free one. */
uint16_t fatimg_fat_entry(fatimg_read_t *r, uint16_t cl);

/* Next cluster in the chain, or 0 at end / on a corrupt entry. */
uint16_t fatimg_next_cluster(fatimg_read_t *r, uint16_t cl);
/* LBA of a data cluster (1 sector per cluster on 1.44 MB; spc handled). */
uint32_t fatimg_cluster_lba(const fatimg_read_t *r, uint16_t cl);
/* true if the root directory holds no files and no subdirectories */
bool fatimg_is_empty(const fatimg_io_t *io);
/* true if the image holds a sound 1.44 MB FAT12 volume: a valid boot sector and
 * both FATs starting with the media descriptor. (Before wiping a disk because a
 * marker is missing, look whether there is anything to wipe.) */
bool fatimg_valid(fatimg_read_t *r, const fatimg_io_t *io);   /* r: scratch, 540 B */

#endif /* FATIMG_H */
