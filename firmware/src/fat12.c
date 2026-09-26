/**
 * fat12.c - blank 1.44 MB FAT12 volume, one sector at a time (pure C).
 * Field values follow the standard DOS 1.44 MB BPB. See fat12.h for layout.
 */
#include "fat12.h"
#include <string.h>

#define FAT_START      1u
#define FAT_SECTORS    9u
#define NUM_FATS       2u
#define ROOT_START     (FAT_START + NUM_FATS * FAT_SECTORS)   /* 19 */
#define ROOT_SECTORS   14u
#define DATA_START     (ROOT_START + ROOT_SECTORS)            /* 33 */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

static void boot_sector(uint8_t *s, bool dd, const char *label)
{
  s[0]=0xEB; s[1]=0x3C; s[2]=0x90;               /* jump */
  memcpy(&s[3], "MSDOS5.0", 8);                  /* OEM name */
  put16(&s[11], 512);       /* bytes/sector    */
  s[13]=dd ? 2 : 1;         /* sectors/cluster */
  put16(&s[14], 1);         /* reserved sectors*/
  s[16]=NUM_FATS;           /* num FATs        */
  put16(&s[17], dd ? 112 : 224);   /* root entries */
  put16(&s[19], dd ? 1440 : 2880); /* total sectors 16 */
  s[21]=dd ? 0xF9 : 0xF0;   /* media descriptor*/
  put16(&s[22], dd ? 3 : 9);  /* sectors/FAT     */
  put16(&s[24], dd ? 9 : 18); /* sectors/track   */
  put16(&s[26], 2);         /* heads           */
  put32(&s[28], 0);         /* hidden sectors  */
  put32(&s[32], 0);         /* total sectors 32*/
  s[36]=0x00;               /* drive number    */
  s[38]=0x29;               /* ext boot sig    */
  put32(&s[39], 0x12345678);/* volume serial   */
  memcpy(&s[43], label ? label : "NO NAME    ", 11);
  memcpy(&s[54], "FAT12   ", 8);
  s[510]=0x55; s[511]=0xAA; /* boot signature  */
}

void fat12_blank_sector(uint32_t lba, uint8_t out[FAT12_SECTOR_SIZE], const char *label)
{
  memset(out, 0, FAT12_SECTOR_SIZE);

  if(lba == 0) { boot_sector(out, false, label); return; }

  /* first sector of each FAT holds the reserved cluster entries F0 FF FF */
  if(lba == FAT_START || lba == FAT_START + FAT_SECTORS)
  {
    out[0]=0xF0; out[1]=0xFF; out[2]=0xFF;      /* media + EOC for clusters 0,1 */
    return;
  }

  /* root directory: sector 19 gets the volume label entry if a label is set */
  if(lba == ROOT_START && label)
  {
    memcpy(&out[0], label, 11);   /* 8.3 name field = label */
    out[11] = 0x08;               /* attribute = volume label */
    return;
  }

  /* everything else (rest of FATs, root, all data) is zero-filled */
}

void fat12_boot_sector(uint8_t out[FAT12_SECTOR_SIZE], bool dd, const char *label)
{
  memset(out, 0, FAT12_SECTOR_SIZE);
  boot_sector(out, dd, label);
}
