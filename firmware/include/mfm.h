#ifndef MFM_H
#define MFM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * IBM System 34 MFM for 1.44 MB HD floppies (pure C, host-testable).
 *
 * Track geometry: 18 sectors x 512 B, 500 kbit/s, 300 RPM -> 200 ms/rev
 * -> 12500 data bytes/track. Each data byte = 16 MFM cells.
 *
 * READ side: mfm_track_byte() describes byte p of a track (value + whether it
 * is a missing-clock sync mark) from the track's raw sector data. The firmware
 * turns that into flux intervals on the fly (no 25 KB encoded-track cache);
 * mfm_encode_track() is the same thing looped into a buffer (host tests).
 *
 * WRITE side: mfm_dec_* is a streaming decoder. Feed it flux intervals in
 * cells (2/3/4); it finds A1 A1 A1 sync, frames the following bytes and hands
 * complete ID (FE) and data (FB) fields to a callback.
 *
 * CRC = CRC-CCITT (poly 0x1021, init 0xFFFF) over the 3 sync bytes + address
 * mark + field, stored big-endian. Layout details: docs/07-floppy-format.md.
 */

#define MFM_SECTORS_PER_TRACK  18u
#define MFM_SECTOR_SIZE        512u
#define MFM_TRACK_SIZE         (MFM_SECTORS_PER_TRACK * MFM_SECTOR_SIZE)  /* 9216 */
#define MFM_TRACK_DATA_BYTES   12500u
#define MFM_TRACK_MAX_BYTES    25600u  /* encoded (2 bytes per data byte) + headroom */

/* Track geometry. HD (1.44 MB) and DD (720 KB) share 300 RPM, 512-byte
 * sectors, the lead-in (gap4a, IAM, gap1) and the sector field layout; DD runs
 * at half the data rate (250 kbit/s: 6250 bytes per revolution) with 9 sectors
 * and the standard 720 KB gap3 of 80. The MFM_* constants above are the HD
 * (largest) case, for sizing buffers. */
typedef struct {
  uint8_t  sectors;         /* per track */
  uint16_t track_bytes;     /* data bytes per revolution */
  uint16_t stride;          /* sector stride: ID 22 + gap2 22 + data 530 + gap3 */
} mfm_geom_t;
extern const mfm_geom_t mfm_geom_hd, mfm_geom_dd;

/* CRC-CCITT over len bytes, given a running crc (start 0xFFFF). */
uint16_t mfm_crc_ccitt(uint16_t crc, const uint8_t *data, size_t len);

/* CRC of a data field (A1 A1 A1 FB + 512 bytes). */
uint16_t mfm_data_crc(const uint8_t sector[MFM_SECTOR_SIZE]);

/* ---- read side ---- */
enum { MFM_PLAIN = 0, MFM_SYNC_A1 = 1, MFM_SYNC_C2 = 2 };

typedef struct {
  uint8_t cyl, head;
  const uint8_t  *data;     /* MFM_TRACK_SIZE bytes: sectors 1..18 in order */
  const uint16_t *dcrc;     /* 18 data-field CRCs (mfm_data_crc) */
  bool hide;                /* true: no address marks at all (track not ready) */
  const mfm_geom_t *g;      /* geometry; NULL = HD */
} mfm_track_t;

/* Byte p (0 .. geometry track_bytes - 1) of the track: *val, and *sync =
 * MFM_PLAIN / MFM_SYNC_A1 / MFM_SYNC_C2 (missing-clock mark). */
void mfm_track_byte(const mfm_track_t *t, unsigned p, uint8_t *val, uint8_t *sync);

/* 16 MFM cells for one byte, MSB first. prev = previous data bit (updated). */
uint16_t mfm_encode_byte(uint8_t val, uint8_t sync, int *prev);

/* Encode a whole track into out (2 bytes per data byte). Returns bytes written,
 * or 0 if out_cap is too small. */
size_t mfm_encode_track(const mfm_track_t *t, uint8_t *out, size_t out_cap);

/* Data-byte offset of the start of sector s (0..17): its ID sync (12 x 00).
 * Used to find which sector a write lands in from the rotational position. */
#define MFM_TRACK_PRE      146u     /* gap4a 80 + sync 12 + IAM 4 + gap1 50 */
#define MFM_SECTOR_STRIDE  658u     /* ID 22 + gap2 22 + data 530 + gap3 84 */

/* ---- write side (streaming decoder) ---- */
typedef void (*mfm_field_fn)(uint8_t mark, const uint8_t *field, unsigned len, void *ctx);

typedef struct {
  uint32_t sh;          /* last cells, newest in bit 0 */
  uint8_t  state;       /* 0 search, 1 sync, 2 field */
  uint8_t  a1;          /* consecutive A1 marks seen */
  uint8_t  nbit;        /* cells collected for the current 16-cell word */
  uint8_t  mark;        /* FE / FB */
  uint16_t len, need;   /* field bytes collected / required (incl. CRC) */
  uint8_t  buf[MFM_SECTOR_SIZE + 2];
  mfm_field_fn cb;
  void *ctx;
} mfm_dec_t;

void mfm_dec_init(mfm_dec_t *d, mfm_field_fn cb, void *ctx);
void mfm_dec_reset(mfm_dec_t *d);               /* lose sync (e.g. new write) */
/* Push one flux interval of n cells (valid MFM: 2..4). Anything else resets. */
void mfm_dec_push(mfm_dec_t *d, unsigned n);

/* Host-test helper: decode an encoded track, check all CRCs, copy the data
 * back into sectors_out. Returns good sectors (18 on a clean track). */
int mfm_decode_verify(const uint8_t *mfm, size_t mfm_len, uint8_t *sectors_out);

/* ---- data rate of a write (720 KB DD / 1.44 MB HD) ----
 * Classify a write's raw flux intervals: 1.5-3.5 us only occur at HD
 * (500 kbit/s), 5.5-8.5 us only at DD (250 kbit/s); 4 us is both.
 * tpu = timer ticks per microsecond. */
typedef struct { uint32_t hd, dd; } mfm_rate_t;
static inline void mfm_rate_add(mfm_rate_t *r, uint32_t dt, uint32_t tpu)
{
  if(dt >= tpu * 3u / 2u && dt < tpu * 7u / 2u) r->hd++;
  else if(dt >= tpu * 11u / 2u && dt < tpu * 17u / 2u) r->dd++;
}
/* After a write: switch the disk's density? 1 = to DD, 0 = to HD, -1 = no.
 * Only a FORMAT counts: the write must span at least MFM_FORMAT_MIN_MS (3/4 of
 * a revolution; a format writes a whole track at once, a sector write lasts
 * 10-20 ms), and the other density's intervals must outnumber the current
 * one's 8 to 1, at least 100 of them. */
#define MFM_FORMAT_MIN_MS 150u
int mfm_rate_decide(const mfm_rate_t *r, bool is_dd, uint32_t gate_ms);

#endif /* MFM_H */
