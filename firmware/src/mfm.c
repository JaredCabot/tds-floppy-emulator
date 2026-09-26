/**
 * mfm.c - IBM System 34 MFM track encoder (pure C, host-testable).
 *
 * MFM rule: between consecutive data bits a clock bit is inserted; the clock is
 * 1 only when both the previous and current data bits are 0. Each data bit thus
 * becomes 2 cells: [clock][data]. We emit cells MSB-first into a byte buffer.
 *
 * Sync marks break the rule on purpose (a "missing clock") so the controller
 * can find them:
 *   A1 sync  -> cells 0x4489  (normal A1 would be 0x44A9)
 *   C2 sync  -> cells 0x5224  (normal C2 would be 0x5254)
 * We emit those 16-bit cell patterns literally for the 3 sync bytes.
 */
#include "mfm.h"

/* ---- CRC-CCITT (0x1021, init supplied by caller), table-driven ----
 * Table = CRC of each byte value shifted into the top of the register.
 * (The bitwise version cost ~half of a track rebuild on the AT32F415.) */
static const uint16_t crc_tab[256] = {
  0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
  0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
  0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
  0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
  0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
  0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
  0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
  0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
  0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
  0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
  0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
  0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
  0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
  0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
  0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
  0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
  0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
  0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
  0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
  0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
  0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
  0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
  0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
  0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
  0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
  0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
  0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
  0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
  0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
  0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
  0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
  0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0,
};

uint16_t mfm_crc_ccitt(uint16_t crc, const uint8_t *data, size_t len)
{
  for(size_t i = 0; i < len; i++)
    crc = (uint16_t)((crc << 8) ^ crc_tab[(crc >> 8) ^ data[i]]);
  return crc;
}

uint16_t mfm_data_crc(const uint8_t sector[MFM_SECTOR_SIZE])
{
  static const uint8_t hdr[4] = { 0xA1, 0xA1, 0xA1, 0xFB };
  return mfm_crc_ccitt(mfm_crc_ccitt(0xFFFF, hdr, 4), sector, MFM_SECTOR_SIZE);
}

/* ================================================================
 * Track layout (data-byte offsets), 12500 bytes:
 *   0..79    gap4a 4E        80..91 sync 00     92..94 C2* (IAM)   95 FC
 *   96..145  gap1 4E
 *   per sector s at base = 146 + 658*s:
 *     +0..11 sync 00   +12..14 A1*   +15 FE  +16 C +17 H +18 R +19 N
 *     +20,21 ID CRC    +22..43 gap2 4E
 *     +44..55 sync 00  +56..58 A1*   +59 FB  +60..571 data  +572,573 CRC
 *     +574..657 gap3 4E
 *   11990..12499 gap4b 4E
 * (* = missing-clock sync mark)
 * ================================================================ */
void mfm_track_byte(const mfm_track_t *t, unsigned p, uint8_t *val, uint8_t *sync)
{
  *sync = MFM_PLAIN;
  if(p < MFM_TRACK_PRE)
  {
    if(p < 80 || p >= 96) { *val = 0x4E; return; }
    if(p < 92)            { *val = 0x00; return; }
    if(p < 95)            { *val = 0xC2; *sync = t->hide ? MFM_PLAIN : MFM_SYNC_C2; return; }
    *val = 0xFC; return;
  }
  unsigned q = p - MFM_TRACK_PRE;
  unsigned s = q / MFM_SECTOR_STRIDE, o = q % MFM_SECTOR_STRIDE;
  if(s >= MFM_SECTORS_PER_TRACK) { *val = 0x4E; return; }       /* gap4b */

  if(o < 12 || (o >= 44 && o < 56)) { *val = 0x00; return; }
  if(o < 15 || (o >= 56 && o < 59))
  {
    *val = 0xA1;                                   /* hidden -> plain A1: no sync */
    *sync = t->hide ? MFM_PLAIN : MFM_SYNC_A1;
    return;
  }
  if(o < 22)                                       /* ID field */
  {
    uint8_t id[8] = { 0xA1, 0xA1, 0xA1, 0xFE, t->cyl, t->head, (uint8_t)(s + 1), 0x02 };
    if(o < 20) { *val = id[o - 12]; return; }
    uint16_t crc = mfm_crc_ccitt(0xFFFF, id, 8);
    *val = (o == 20) ? (uint8_t)(crc >> 8) : (uint8_t)crc;
    return;
  }
  if(o < 44 || o >= 574) { *val = 0x4E; return; }   /* gap2, gap3 */
  if(o == 59) { *val = 0xFB; return; }
  if(o < 572) { *val = t->data[s * MFM_SECTOR_SIZE + (o - 60)]; return; }
  *val = (o == 572) ? (uint8_t)(t->dcrc[s] >> 8) : (uint8_t)t->dcrc[s];
}

/* spread bit i of x to bit 2i (8 -> 16 bits) */
static uint16_t spread8(uint16_t x)
{
  x = (x | (x << 4)) & 0x0F0F;
  x = (x | (x << 2)) & 0x3333;
  x = (x | (x << 1)) & 0x5555;
  return x;
}

/* One byte -> 16 cells [c7 d7 ... c0 d0]. Clock ci = 1 only when data bit i and
 * the data bit before it are both 0. Sync marks are fixed patterns with one
 * clock deliberately missing: A1 -> 0x4489, C2 -> 0x5224. */
uint16_t mfm_encode_byte(uint8_t val, uint8_t sync, int *prev)
{
  uint16_t w;
  if(sync == MFM_SYNC_A1)      w = 0x4489;
  else if(sync == MFM_SYNC_C2) w = 0x5224;
  else
  {
    uint16_t before = (uint16_t)((val >> 1) | (*prev << 7));
    uint16_t clk = (uint16_t)(~(val | before) & 0xFF);
    w = (uint16_t)((spread8(clk) << 1) | spread8(val));
  }
  *prev = val & 1;
  return w;
}

size_t mfm_encode_track(const mfm_track_t *t, uint8_t *out, size_t out_cap)
{
  if(out_cap < 2u * MFM_TRACK_DATA_BYTES) return 0;
  int prev = 0;
  for(unsigned p = 0; p < MFM_TRACK_DATA_BYTES; p++)
  {
    uint8_t v, sy;
    mfm_track_byte(t, p, &v, &sy);
    uint16_t w = mfm_encode_byte(v, sy, &prev);
    out[2 * p] = (uint8_t)(w >> 8);
    out[2 * p + 1] = (uint8_t)w;
  }
  return 2u * MFM_TRACK_DATA_BYTES;
}

/* ================================================================
 * Streaming decoder for the write path.
 *  search: shift cells in; a 0x4489 word = one A1 sync (fixes byte framing)
 *  sync:   collect 16-cell words; more 0x4489 -> count; after >= 3 A1, the next
 *          word is the address mark: FE -> 6 more bytes, FB -> 514, else search
 *  field:  collect bytes (data bits = odd cells), then call cb and search again
 * ================================================================ */
static uint8_t word_data(uint16_t w)       /* keep the 8 data cells */
{
  uint8_t v = 0;
  for(int i = 7; i >= 0; i--) v = (uint8_t)((v << 1) | ((w >> (2 * i)) & 1));
  return v;
}

void mfm_dec_reset(mfm_dec_t *d)
{
  d->sh = 0; d->state = 0; d->a1 = 0; d->nbit = 0; d->len = 0; d->need = 0;
}

void mfm_dec_init(mfm_dec_t *d, mfm_field_fn cb, void *ctx)
{
  d->cb = cb; d->ctx = ctx;
  mfm_dec_reset(d);
}

static void dec_cell(mfm_dec_t *d, unsigned c)
{
  d->sh = (d->sh << 1) | c;
  if(d->state == 0)
  {
    if((d->sh & 0xFFFF) == 0x4489) { d->state = 1; d->a1 = 1; d->nbit = 0; }
    return;
  }
  if(++d->nbit < 16) return;
  d->nbit = 0;
  uint16_t w = (uint16_t)d->sh;
  if(d->state == 1)
  {
    if(w == 0x4489) { d->a1++; return; }
    uint8_t m = word_data(w);
    /* >= 1 A1: capture may begin part-way into the sync marks. Integrity comes
     * from the CRC, which always covers A1 A1 A1 + mark as the host wrote it. */
    if(d->a1 >= 1 && (m == 0xFE || m == 0xFB))
    {
      d->mark = m; d->len = 0;
      d->need = (m == 0xFE) ? 6 : MFM_SECTOR_SIZE + 2;
      d->state = 2;
    }
    else d->state = 0;
    return;
  }
  d->buf[d->len++] = word_data(w);                 /* state 2: field byte */
  if(d->len == d->need)
  {
    d->cb(d->mark, d->buf, d->len, d->ctx);
    d->state = 0;
  }
}

void mfm_dec_push(mfm_dec_t *d, unsigned n)
{
  if(n < 2 || n > 4) { mfm_dec_reset(d); return; }
  while(--n) dec_cell(d, 0);
  dec_cell(d, 1);
}

/* ==================================================================
 * Decoder for host tests only (mfm_decode_verify). Scans the cell
 * stream, finds A1 sync marks, decodes the following bytes, and checks
 * ID and data CRCs. Not compiled into the MCU firmware path by callers.
 * ================================================================== */
#ifdef MFM_HOST_TEST

/* pull one cell bit from the buffer at absolute cell index */
static int cell_at(const uint8_t *mfm, size_t mfm_len, size_t idx)
{
  size_t byte = idx >> 3;
  if(byte >= mfm_len) return -1;
  return (mfm[byte] >> (7 - (idx & 7))) & 1;
}

/* decode 8 data bits starting at a given cell index (data bits are odd cells);
 * returns byte value, advances *cell by 16. */
static int decode_byte(const uint8_t *mfm, size_t mfm_len, size_t *cell)
{
  uint8_t v = 0;
  for(int i = 0; i < 8; i++)
  {
    (void)cell_at(mfm, mfm_len, *cell);        /* clock cell, ignored */
    int d = cell_at(mfm, mfm_len, *cell + 1);
    if(d < 0) return -1;
    v = (uint8_t)((v << 1) | d);
    *cell += 2;
  }
  return v;
}

/* find next A1 (0x4489) sync mark at or after cell index *cell (byte-cell
 * aligned search). Returns cell index just past one A1, or (size_t)-1. */
static size_t find_a1(const uint8_t *mfm, size_t mfm_len, size_t cell)
{
  size_t total = mfm_len * 8;
  for(; cell + 16 <= total; cell++)
  {
    uint16_t w = 0;
    for(int i = 0; i < 16; i++) w = (uint16_t)((w << 1) | cell_at(mfm, mfm_len, cell + i));
    if(w == 0x4489) return cell + 16;
  }
  return (size_t)-1;
}

int mfm_decode_verify(const uint8_t *mfm, size_t mfm_len, uint8_t *sectors_out)
{
  int good = 0;
  size_t cell = 0;
  size_t total = mfm_len * 8;

  while(cell < total)
  {
    size_t a1 = find_a1(mfm, mfm_len, cell);
    if(a1 == (size_t)-1) break;
    /* expect two more A1s */
    size_t c = a1;
    int is_triple = 1;
    for(int k = 0; k < 2; k++)
    {
      uint16_t w = 0;
      for(int i = 0; i < 16; i++) w = (uint16_t)((w << 1) | cell_at(mfm, mfm_len, c + i));
      if(w != 0x4489) { is_triple = 0; break; }
      c += 16;
    }
    if(!is_triple) { cell = a1; continue; }

    int am = decode_byte(mfm, mfm_len, &c);
    if(am == 0xFE)  /* ID field */
    {
      uint8_t id[5]; id[0] = 0xFE;
      for(int i = 1; i < 5; i++) { int b = decode_byte(mfm, mfm_len, &c); if(b < 0) return good; id[i] = (uint8_t)b; }
      int ch = decode_byte(mfm, mfm_len, &c), cl = decode_byte(mfm, mfm_len, &c);
      if(ch < 0 || cl < 0) return good;
      uint8_t crcbuf[8] = {0xA1,0xA1,0xA1, id[0],id[1],id[2],id[3],id[4]};
      uint16_t want = mfm_crc_ccitt(0xFFFF, crcbuf, 8);
      if(want != (((uint16_t)ch << 8) | (uint16_t)cl)) { cell = a1; continue; }
      /* find the data-field A1 that follows */
      size_t da1 = find_a1(mfm, mfm_len, c);
      if(da1 == (size_t)-1) return good;
      size_t d = da1;
      for(int k = 0; k < 2; k++) { d += 16; }  /* skip 2 more A1 */
      int dam = decode_byte(mfm, mfm_len, &d);
      if(dam != 0xFB) { cell = a1; continue; }
      uint8_t sec = id[3];
      if(sec < 1 || sec > MFM_SECTORS_PER_TRACK) { cell = a1; continue; }
      uint8_t *dst = sectors_out + (size_t)(sec - 1) * MFM_SECTOR_SIZE;
      for(unsigned i = 0; i < MFM_SECTOR_SIZE; i++)
      { int b = decode_byte(mfm, mfm_len, &d); if(b < 0) return good; dst[i] = (uint8_t)b; }
      int dh = decode_byte(mfm, mfm_len, &d), dl = decode_byte(mfm, mfm_len, &d);
      if(dh < 0 || dl < 0) return good;
      uint8_t hdr[4] = {0xA1,0xA1,0xA1,0xFB};
      uint16_t dwant = mfm_crc_ccitt(0xFFFF, hdr, 4);
      dwant = mfm_crc_ccitt(dwant, dst, MFM_SECTOR_SIZE);
      if(dwant == (((uint16_t)dh << 8) | (uint16_t)dl)) good++;
      cell = d;
    }
    else cell = a1;  /* not an ID mark (e.g. C2/IAM) - keep scanning */
  }
  return good;
}

#endif /* MFM_HOST_TEST */
