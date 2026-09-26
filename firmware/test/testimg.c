/**
 * testimg.c - blank FAT12 volume + one known file (TEST.BIN). See testimg.h.
 * Layout constants follow fat12.c: FAT1 LBA 1-9, FAT2 10-18, root 19-32,
 * data from LBA 33 = cluster 2, one sector per cluster.
 */
#include "testimg.h"
#include "fat12.h"
#include <string.h>

#define FAT1_LBA      1u
#define FAT_SECTORS   9u
#define ROOT_LBA      19u
#define DATA_LBA      33u
#define FILE_CLUSTERS (TESTIMG_FILE_SIZE / 512u)          /* 128 */
#define FIRST_CL      2u
#define LAST_CL       (FIRST_CL + FILE_CLUSTERS - 1u)     /* 129 */

/* 12-bit FAT entry n */
static uint16_t fat_entry(uint32_t n)
{
  if(n == 0) return 0xFF0;                       /* media F0 */
  if(n == 1) return 0xFFF;
  if(n >= FIRST_CL && n < LAST_CL) return (uint16_t)(n + 1);  /* chain */
  if(n == LAST_CL) return 0xFFF;                 /* end of file */
  return 0x000;                                  /* free */
}

/* byte k of a FAT: entries n=2j and n+1 share bytes 3j..3j+2 */
static uint8_t fat_byte(uint32_t k)
{
  uint32_t n = (k / 3u) * 2u;
  uint16_t a = fat_entry(n), b = fat_entry(n + 1);
  switch(k % 3u)
  {
    case 0:  return (uint8_t)a;
    case 1:  return (uint8_t)((a >> 8) | ((b & 0x0F) << 4));
    default: return (uint8_t)(b >> 4);
  }
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void testimg_sector(uint32_t lba, uint8_t dst[512])
{
  fat12_blank_sector(lba, dst, (lba == 0 || lba == ROOT_LBA) ? "TDSFLOPPY  " : NULL);

  if(lba >= FAT1_LBA && lba < FAT1_LBA + 2u * FAT_SECTORS)          /* both FATs */
  {
    uint32_t base = ((lba - FAT1_LBA) % FAT_SECTORS) * 512u;
    for(uint32_t i = 0; i < 512u; i++) dst[i] = fat_byte(base + i);
  }
  else if(lba == ROOT_LBA)                                          /* entry 1 */
  {
    uint8_t *e = dst + 32;                   /* entry 0 = volume label */
    memcpy(e, "TEST    BIN", 11);
    e[11] = 0x20;                            /* archive */
    put16(e + 22, 12u << 11);                /* 12:00:00 */
    put16(e + 24, (uint16_t)(((2026 - 1980) << 9) | (9 << 5) | 23));  /* 2026-09-23 */
    put16(e + 26, FIRST_CL);
    e[28] = (uint8_t)TESTIMG_FILE_SIZE;       e[29] = (uint8_t)(TESTIMG_FILE_SIZE >> 8);
    e[30] = (uint8_t)(TESTIMG_FILE_SIZE >> 16); e[31] = (uint8_t)(TESTIMG_FILE_SIZE >> 24);
  }
  else if(lba >= DATA_LBA && lba < DATA_LBA + FILE_CLUSTERS)        /* file data */
  {
    uint32_t f0 = (lba - DATA_LBA) * 512u;
    for(uint32_t i = 0; i < 512u; i++) dst[i] = testimg_pattern(f0 + i);
  }
}
