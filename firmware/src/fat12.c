/**
 * fat12.c - the boot sector of a blank floppy volume (pure C). Field values
 * follow the standard DOS BPB; see fat12.h for the two geometries.
 */
#include "fat12.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

void fat12_boot_sector(uint8_t s[FAT12_SECTOR_SIZE], bool dd, const char *label)
{
  memset(s, 0, FAT12_SECTOR_SIZE);
  s[0]=0xEB; s[1]=0x3C; s[2]=0x90;               /* jump */
  memcpy(&s[3], "MSDOS5.0", 8);                  /* OEM name */
  put16(&s[11], 512);       /* bytes/sector    */
  s[13]=dd ? 2 : 1;         /* sectors/cluster */
  put16(&s[14], 1);         /* reserved sectors*/
  s[16]=2;                  /* num FATs        */
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
