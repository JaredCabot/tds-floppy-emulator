/*
 * update.c - firmware-update file format helpers (see update.h). Pure C.
 */
#include "update.h"
#include <string.h>

/* Nibble-table CRC-32 (reflected, poly 0xEDB88320): 64 bytes of table instead
 * of 1 KB, fast enough for the bootloader (~50 KB in well under 0.1 s). */
static const uint32_t crc_nib[16] = {
  0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
  0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
  0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
  0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t upd_crc32(uint32_t crc, const void *data, size_t len)
{
  const uint8_t *p = (const uint8_t *)data;
  crc = ~crc;
  while(len--)
  {
    crc ^= *p++;
    crc = (crc >> 4) ^ crc_nib[crc & 15u];
    crc = (crc >> 4) ^ crc_nib[crc & 15u];
  }
  return ~crc;
}

bool upd_header_ok(const upd_header_t *h)
{
  return memcmp(h->magic, UPD_MAGIC, 8) == 0
      && memcmp(h->board, UPD_BOARD, 8) == 0
      && h->format == UPD_FORMAT
      && upd_crc32(0, h, offsetof(upd_header_t, header_crc)) == h->header_crc
      && h->length >= 8u && h->length <= UPD_IMAGE_MAX && (h->length & 3u) == 0;
}

bool upd_vectors_ok(const uint8_t first8[8], uint32_t length)
{
  uint32_t sp = (uint32_t)first8[0] | (uint32_t)first8[1] << 8 | (uint32_t)first8[2] << 16 | (uint32_t)first8[3] << 24;
  uint32_t pc = (uint32_t)first8[4] | (uint32_t)first8[5] << 8 | (uint32_t)first8[6] << 16 | (uint32_t)first8[7] << 24;
  return sp > 0x20000000u && sp <= 0x20008000u && (sp & 3u) == 0
      && (pc & 1u) && pc > APP_BASE && pc < APP_BASE + length;
}
