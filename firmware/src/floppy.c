/**
 * floppy.c - floppy-bus slave for the TDS (26-pin, 1.44 MB). Pins: docs/02,
 * behaviour: docs/09 (TEAC FD-05HF spec + measured real drive).
 *
 * TRACK: the current track is held raw (18 x 512 B) in RAM, loaded from the
 * SPI-flash buffer (buffer.c) when the head moves. Unsaved writes are written
 * back in the BACKGROUND from a second buffer (on a head move, or 100 ms after
 * the last write), so the head never waits for flash (docs/10).
 *
 * READ: TMR3 ch2 (PA7) PWM, active-low 400 ns pulse per flux transition; DMA1
 * ch3 reloads the period from a ring that the DMA interrupt refills by
 * generating MFM on the fly from the raw track (mfm_track_byte). IDs always
 * carry the head's current cyl/head; while the loaded track doesn't match
 * (just stepped, or loading) the track is generated with no address marks, so
 * the controller simply retries. INDEX comes from the same stream (wrap).
 *
 * WRITE: WGATE (PB9) on EXINT9. While it is asserted, TMR1 ch1 (PA8) captures
 * each WDATA falling edge into a DMA ring; the intervals feed the streaming MFM
 * decoder (mfm_dec_*), which hands complete fields to wr_field(). Data fields
 * with a good CRC replace the sector in the RAM track (dirty -> written back as
 * described above). The target sector comes from an
 * ID field in the same write (format), else from the rotational position at
 * which WGATE was asserted.
 *
 * DRIVE: outputs are only driven while DRIVE SELECT (released otherwise, like a
 * real drive's 3-state outputs). Selecting starts a 480 ms "spin-up"; READY,
 * INDEX and READ DATA only appear after it (the TDS waits for READY). STEP is
 * taken on its trailing edge and only while selected; it clears DISK CHANGE and
 * masks INDEX/READ DATA for 17 ms (seek-complete).
 */
#include "floppy.h"
#include "board.h"
#include "mfm.h"
#include "buffer.h"
_Static_assert(BUF_TRACKS >= FLPY_CYL_LIMIT * FLPY_HEADS, "every track the head can reach needs a slot");
#include "at32f415_conf.h"
#include <string.h>

/* interrupt handlers (vector table in the startup file) */
void DMA1_Channel3_IRQHandler(void);
void DMA1_Channel2_IRQHandler(void);
void EXINT9_5_IRQHandler(void);
void EXINT1_IRQHandler(void);
void EXINT0_IRQHandler(void);
void SysTick_Handler(void);

/* ---- pins (docs/02-pinmap.csv) ---- */
#define PIN_SEL     GPIO_PINS_0   /* PA0 in  (EXINT0) */
#define PIN_STEP    GPIO_PINS_1   /* PA1 in  (EXINT1) */
#define PIN_WDATA   GPIO_PINS_8   /* PA8 in  (TMR1_CH1 capture) */
#define PIN_RDATA   GPIO_PINS_7   /* PA7 out (TMR3_CH2) */
#define PIN_DIR     GPIO_PINS_0   /* PB0 in  */
#define PIN_SIDE1   GPIO_PINS_4   /* PB4 in  */
#define PIN_WGATE   GPIO_PINS_9   /* PB9 in  (EXINT9) */
#define PIN_INDEX   GPIO_PINS_8   /* PB8 out */
#define PIN_TRK0    GPIO_PINS_6   /* PB6 out */
#define PIN_WPROT   GPIO_PINS_5   /* PB5 out */
#define PIN_READY   GPIO_PINS_3   /* PB3 out (JTDO until JTAG-DP is turned off) */
#define PIN_DSKCHG  GPIO_PINS_7   /* PB7 out */

/* ---- timing ----
 * TMR1 (APB2) and TMR3 (APB1) both run at 144 MHz; 1 MFM cell = 1 us = 144. */
#define TICKS_PER_CELL     144u
#define RD_PULSE_TICKS     58u      /* ~400 ns READ DATA pulse */
#define RD_RING            512u     /* transitions, ~1.3 ms of stream */
#define WR_RING            512u     /* captured WDATA edges */
#define INDEX_PULSE_MS     3u       /* SysTick countdown -> 2-3 ms; spec 1.5-5 */
#define SEEK_COMPLETE_MS   17u      /* spec 15.8-17.9 ms after the last STEP */
#define SPINUP_MS          480u     /* READY this long after select (motor on) */
/* Flush a dirty track to flash this long after the last write ends. A real
 * drive commits each sector as it is written; the emulator is powered by the host,
 * so anything still only in RAM is lost at power-off (seen: a torn FAT after a
 * scope power-cycle). Short, but long enough to batch a multi-sector save. */
#define WRITEBACK_IDLE_MS  100u

static inline void outB_assert(uint16_t pin)   { gpio_bits_reset(GPIOB, pin); }
static inline void outB_deassert(uint16_t pin) { gpio_bits_set(GPIOB, pin); }
static inline int  in_low(gpio_type *port, uint16_t pin) { return (port->idt & pin) == 0; }

/* ---- head / drive state ---- */
static volatile uint8_t s_cyl, s_head;     /* s_cyl: STEP ISR, s_head: poll */
static volatile bool s_selected;           /* SEL ISR */
static volatile bool s_writing;            /* WGATE ISR */
static volatile bool s_media = true;       /* disk "inserted" (false during USB transfers) */
static volatile bool st_trk0 = true, st_dskchg = true, st_wprot = false, st_ready = false;
static volatile uint32_t s_seek_ms, s_spinup_ms, s_index_ms, s_wr_end_ms;
static volatile uint32_t s_last_active_ms;   /* last select/deselect or write end */

/* ---- current track (raw) ---- */
/* Two track buffers: the LIVE one (s_trk/s_crc: read stream + writes) and a
 * spare that holds a track being written back to flash in the background, so
 * the head never waits for flash after a step (docs/10). */
static uint8_t  s_trkbuf[2][MFM_TRACK_SIZE];
static uint16_t s_crcbuf[2][MFM_SECTORS_PER_TRACK];
static uint8_t  *s_trk = s_trkbuf[0];
static uint16_t *s_crc = s_crcbuf[0];
static uint8_t  s_live;                    /* index of the live buffer */
static bool     s_pend_busy;               /* spare buffer: write-back running */
static unsigned s_pend_t;                  /* ... of this track (cyl*2 + head) */
static bool     s_started;                 /* flpy_init() done */
static volatile uint8_t s_trk_cyl = 0xFF, s_trk_head = 0xFF;
static volatile bool s_trk_valid, s_trk_dirty;

/* The disk's geometry (HD / DD) and MFM cell time in timer ticks (1 / 2 us). */
static const mfm_geom_t *s_geom = &mfm_geom_hd;
static uint32_t s_cell = TICKS_PER_CELL;
/* Data rate of the current write, from its raw flux intervals (mfm_rate_*),
 * and when it started: only a whole-track write (a format) can switch. */
static mfm_rate_t s_rate;
static uint32_t s_wr_t0;
static volatile int8_t s_switch_to = -1;         /* density switch requested: 1 DD, 0 HD */
volatile uint32_t flpy_dbg_dd, flpy_dbg_density_switches, flpy_dbg_ev_hd, flpy_dbg_ev_dd;

static bool track_visible(void)
{
  return s_media && s_trk_valid && s_trk_cyl == s_cyl && s_trk_head == s_head;
}

/* ---- debug, readable over SWD (tools/fdstat.ps1) ---- */
volatile uint32_t flpy_dbg_ms, flpy_dbg_index_count, flpy_dbg_step_count, flpy_dbg_sel_count;
volatile uint32_t flpy_dbg_load_us, flpy_dbg_store_us;
volatile uint32_t flpy_dbg_wr_gates, flpy_dbg_wr_sectors, flpy_dbg_wr_badcrc, flpy_dbg_wr_lost;
/* self-checks: should all stay 0 */
volatile uint32_t flpy_dbg_store_verify_fail, flpy_dbg_load_while_dirty, flpy_dbg_wr_track_changed;
/* per write gate: A = ms<<16 | cyl<<8 | head;  B = ids<<24 | data<<16 | visible<<8 | first sector */
volatile uint32_t flpy_dbg_gate_a[64], flpy_dbg_gate_b[64];
static uint8_t s_wr_nid, s_wr_ndata, s_wr_first;
volatile uint32_t flpy_dbg_trace[64], flpy_dbg_trace_n;   /* ms<<16 | store<<15 | head<<8 | cyl */
volatile uint32_t flpy_dbg_steplog[64];                   /* ms<<16 | dir_in<<8 | cyl */
static void trace(bool store, uint8_t cyl, uint8_t head)
{
  flpy_dbg_trace[flpy_dbg_trace_n++ & 63] = (flpy_dbg_ms << 16) | ((uint32_t)store << 15) | ((uint32_t)head << 8) | cyl;
}

/* ---- outputs ---- */
static void outputs_apply(void)
{
  uint32_t pm = __get_PRIMASK();
  __disable_irq();
  bool on = s_selected;
  uint16_t a = 0, d = 0;
  if(on && st_trk0)   a |= PIN_TRK0;   else d |= PIN_TRK0;
  if(on && st_wprot)  a |= PIN_WPROT;  else d |= PIN_WPROT;
  if(on && st_dskchg) a |= PIN_DSKCHG; else d |= PIN_DSKCHG;   /* pin 6 (TDS confirmed) */
  if(on && st_ready)  a |= PIN_READY;  else d |= PIN_READY;    /* pin 8 */
  if(!on) { d |= PIN_INDEX; s_index_ms = 0; }
  if(a) outB_assert(a);
  if(d) outB_deassert(d);
  TMR3->c2dt = (on && st_ready && s_seek_ms == 0 && !s_writing) ? RD_PULSE_TICKS : 0;
  __set_PRIMASK(pm);
}

/* ================= READ: flux stream generated on the fly ================= */
static uint16_t s_rd_ring[RD_RING];
static uint32_t s_rd_p;             /* next track byte to generate */
static uint32_t s_rd_cur;           /* cells of the current byte, left-aligned */
static uint8_t  s_rd_nb;            /* cells left in s_rd_cur */
static int      s_rd_prev;          /* last data bit (MFM clock rule) */
static volatile int8_t s_index_half = -1;

static void rd_fill(uint16_t *dst, unsigned n, int8_t half)
{
  mfm_track_t t = { s_cyl, s_head, s_trk, s_crc, !track_visible(), s_geom };
  for(unsigned i = 0; i < n; i++)
  {
    unsigned cells = 0;
    for(;;)
    {
      if(s_rd_nb == 0)
      {
        uint8_t v, sy;
        mfm_track_byte(&t, s_rd_p, &v, &sy);
        s_rd_cur = (uint32_t)mfm_encode_byte(v, sy, &s_rd_prev) << 16;
        s_rd_nb = 16;
        if(++s_rd_p >= t.g->track_bytes) { s_rd_p = 0; s_index_half = half; }
      }
      unsigned bit = s_rd_cur >> 31;
      s_rd_cur <<= 1; s_rd_nb--; cells++;
      if(bit || cells >= 8) break;             /* MFM never exceeds 4 */
    }
    dst[i] = (uint16_t)(cells * s_cell - 1u);
  }
}

static void rd_half_started(int8_t h)     /* ring half h has just started playing */
{
  if(s_index_half != h) return;
  s_index_half = -1;
  flpy_dbg_index_count++;
  if(s_selected && st_ready && s_seek_ms == 0) { outB_assert(PIN_INDEX); s_index_ms = INDEX_PULSE_MS; }
}

void DMA1_Channel3_IRQHandler(void)
{
  if(dma_flag_get(DMA1_HDT3_FLAG) == SET)
  { dma_flag_clear(DMA1_HDT3_FLAG); rd_half_started(1); rd_fill(&s_rd_ring[0], RD_RING / 2, 0); }
  if(dma_flag_get(DMA1_FDT3_FLAG) == SET)
  { dma_flag_clear(DMA1_FDT3_FLAG); rd_half_started(0); rd_fill(&s_rd_ring[RD_RING / 2], RD_RING / 2, 1); }
}

static void rdata_init(void)
{
  gpio_init_type gi;
  crm_periph_clock_enable(CRM_TMR3_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
  gpio_default_para_init(&gi);                   /* PA7 = TMR3_CH2 */
  gi.gpio_pins = PIN_RDATA;
  gi.gpio_mode = GPIO_MODE_MUX;
  gi.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(GPIOA, &gi);

  rd_fill(&s_rd_ring[0], RD_RING / 2, 0);
  rd_fill(&s_rd_ring[RD_RING / 2], RD_RING / 2, 1);

  tmr_base_init(TMR3, s_rd_ring[0], 0);
  tmr_cnt_dir_set(TMR3, TMR_COUNT_UP);
  tmr_period_buffer_enable(TMR3, TRUE);
  tmr_output_config_type oc;
  tmr_output_default_para_init(&oc);
  oc.oc_mode = TMR_OUTPUT_CONTROL_PWM_MODE_A;    /* active while cnt < c2dt */
  oc.oc_polarity = TMR_OUTPUT_ACTIVE_LOW;
  oc.oc_output_state = TRUE;
  tmr_output_channel_config(TMR3, TMR_SELECT_CHANNEL_2, &oc);
  tmr_channel_value_set(TMR3, TMR_SELECT_CHANNEL_2, 0);   /* silent until selected+ready */
  tmr_output_channel_buffer_enable(TMR3, TMR_SELECT_CHANNEL_2, TRUE);

  dma_init_type di;
  dma_reset(DMA1_CHANNEL3);
  dma_default_para_init(&di);
  di.peripheral_base_addr = (uint32_t)&TMR3->pr;
  di.memory_base_addr = (uint32_t)s_rd_ring;
  di.direction = DMA_DIR_MEMORY_TO_PERIPHERAL;
  di.buffer_size = RD_RING;
  di.memory_inc_enable = TRUE;
  di.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_HALFWORD;
  di.memory_data_width = DMA_MEMORY_DATA_WIDTH_HALFWORD;
  di.loop_mode_enable = TRUE;
  di.priority = DMA_PRIORITY_VERY_HIGH;
  dma_init(DMA1_CHANNEL3, &di);
  dma_flexible_config(DMA1, FLEX_CHANNEL3, DMA_FLEXIBLE_TMR3_OVERFLOW);
  dma_interrupt_enable(DMA1_CHANNEL3, DMA_HDT_INT | DMA_FDT_INT, TRUE);
  nvic_irq_enable(DMA1_Channel3_IRQn, 0, 0);
  dma_channel_enable(DMA1_CHANNEL3, TRUE);
  tmr_dma_request_enable(TMR3, TMR_OVERFLOW_DMA_REQUEST, TRUE);
  tmr_counter_enable(TMR3, TRUE);
}

/* ================= WRITE: capture WDATA, decode, update track ================= */
static uint16_t s_wr_ring[WR_RING];
static unsigned s_wr_idx;           /* next ring entry to decode */
static uint16_t s_wr_last;          /* previous capture value */
static bool     s_wr_have_last;
static bool     s_wr_visible;       /* track matched the head when WGATE rose */
static uint8_t  s_wr_trk_cyl, s_wr_trk_head;   /* RAM track at that moment */
static int      s_wr_next;          /* sector for the next data field (-1 unknown) */
static mfm_dec_t s_dec;

static void wr_field(uint8_t mark, const uint8_t *f, unsigned len, void *ctx)
{
  (void)ctx;
  if(mark == 0xFE)                                 /* ID written by a FORMAT */
  {
    if(len < 6) return;                            /* C H R N + CRC */
    uint8_t b[8] = { 0xA1, 0xA1, 0xA1, 0xFE, f[0], f[1], f[2], f[3] };
    if(mfm_crc_ccitt(0xFFFF, b, 8) == ((f[4] << 8) | f[5]) && f[2] >= 1 && f[2] <= s_geom->sectors)
    { s_wr_next = f[2] - 1; s_wr_nid++; }
    return;
  }
  if(len != MFM_SECTOR_SIZE + 2) { flpy_dbg_wr_badcrc++; return; }
  uint16_t crc = mfm_data_crc(f);
  if(crc != ((f[512] << 8) | f[513])) { flpy_dbg_wr_badcrc++; return; }
  if(!s_wr_visible || s_wr_next < 0 || s_wr_next >= (int)s_geom->sectors) { flpy_dbg_wr_lost++; return; }
  if(s_trk_cyl != s_wr_trk_cyl || s_trk_head != s_wr_trk_head || !s_trk_valid)
  { flpy_dbg_wr_track_changed++; return; }         /* RAM track replaced mid-write: never misfile */
  memcpy(&s_trk[s_wr_next * MFM_SECTOR_SIZE], f, MFM_SECTOR_SIZE);
  s_crc[s_wr_next] = crc;
  s_trk_dirty = true;
  if(s_wr_ndata++ == 0) s_wr_first = (uint8_t)s_wr_next;
  flpy_dbg_wr_sectors++;
  s_wr_next++;                                     /* multi-sector write continues */
}

static void wr_decode_to(unsigned end)             /* decode ring entries up to end */
{
  while(s_wr_idx != end)
  {
    uint16_t v = s_wr_ring[s_wr_idx];
    s_wr_idx = (s_wr_idx + 1) % WR_RING;
    if(s_wr_have_last)
    {
      uint32_t dt = (uint16_t)(v - s_wr_last);
      mfm_rate_add(&s_rate, dt, TICKS_PER_CELL);  /* (an HD cell is 1 us) */
      mfm_dec_push(&s_dec, (dt + s_cell / 2u) / s_cell);
    }
    s_wr_last = v; s_wr_have_last = true;
  }
}

/* WDATA edges are captured continuously (no start-up latency at WGATE, which
 * once lost the first A1 of a data field). Outside a write, skip them. */
static void wr_consume_to(unsigned end)
{
  if(s_writing) wr_decode_to(end);
  else if(!in_low(GPIOB, PIN_WGATE)) s_wr_idx = end;   /* gate down: keep for wr_start */
}

void DMA1_Channel2_IRQHandler(void)
{
  if(dma_flag_get(DMA1_HDT2_FLAG) == SET) { dma_flag_clear(DMA1_HDT2_FLAG); wr_consume_to(WR_RING / 2); }
  if(dma_flag_get(DMA1_FDT2_FLAG) == SET) { dma_flag_clear(DMA1_FDT2_FLAG); wr_consume_to(0); }
}

/* Track byte currently passing "under the head": cells generated so far minus
 * the READ-DMA ring entries generated but not yet sent to TMR3. Exact to within
 * one flux interval (<= 4 us). The ring half the DMA is playing is followed by
 * the other half, already regenerated - unless that half's refill interrupt is
 * still pending (flag set), in which case only the playing half is queued. */
static uint32_t playback_byte(void)
{
  uint32_t pm = __get_PRIMASK();
  __disable_irq();
  unsigned playing = RD_RING - dma_data_number_get(DMA1_CHANNEL3);
  unsigned end;
  if(playing < RD_RING / 2) end = dma_flag_get(DMA1_FDT3_FLAG) ? RD_RING / 2 : RD_RING;
  else                      end = dma_flag_get(DMA1_HDT3_FLAG) ? RD_RING : RD_RING + RD_RING / 2;
  uint32_t queued = 0;
  for(unsigned i = playing; i < end; i++) queued += (s_rd_ring[i % RD_RING] + 1u) / s_cell;
  uint32_t total = s_geom->track_bytes * 16u;
  uint32_t gen = (s_rd_p ? s_rd_p : s_geom->track_bytes) * 16u - s_rd_nb;   /* cells emitted
                                                     (s_rd_p == 0: byte in progress is the last one) */
  __set_PRIMASK(pm);
  return ((gen + total - queued % total) % total) / 16u;
}

static void wr_start(void)
{
  /* Everything the capture ISR (higher priority) uses is set up BEFORE
   * s_writing, which is what makes it decode. */
  s_wr_nid = s_wr_ndata = 0; s_wr_first = 0xFF;
  s_wr_visible = track_visible();
  s_wr_trk_cyl = s_trk_cyl; s_wr_trk_head = s_trk_head;
  /* Which sector? A normal write starts in gap 2 of the sector whose ID the
   * controller has just read, i.e. ~44 bytes into that sector's 658-byte slot;
   * +300 centres that in the slot (margins ~300 bytes each side). */
  uint32_t p = playback_byte();
  uint32_t s = (p + 300u - MFM_TRACK_PRE) / s_geom->stride;
  s_wr_next = (p + 300u >= MFM_TRACK_PRE && s < s_geom->sectors) ? (int)s : -1;
  mfm_dec_reset(&s_dec);
  s_rate.hd = s_rate.dd = 0;
  s_wr_t0 = flpy_dbg_ms;
  s_wr_have_last = false;
  s_writing = true;                                /* decoding continues from where the last
                                                      write ended: every edge since is this one */
  outputs_apply();                                 /* READ DATA off while writing */
  flpy_dbg_wr_gates++;
}

static void wr_stop(void)
{
  uint32_t pm = __get_PRIMASK();
  __disable_irq();                                  /* drain what's left */
  wr_decode_to((WR_RING - dma_data_number_get(DMA1_CHANNEL2)) % WR_RING);
  __set_PRIMASK(pm);
  s_writing = false;
  /* a whole track formatted at the other data rate? The host's density line
   * says the other density (flpy_poll switches; mfm_rate_decide). */
  flpy_dbg_ev_hd = s_rate.hd; flpy_dbg_ev_dd = s_rate.dd;
  int sw = mfm_rate_decide(&s_rate, s_geom == &mfm_geom_dd, flpy_dbg_ms - s_wr_t0);
  if(sw >= 0) s_switch_to = (int8_t)sw;
  s_wr_end_ms = flpy_dbg_ms;
  s_last_active_ms = flpy_dbg_ms;
  unsigned g = flpy_dbg_wr_gates - 1u;
  if(g < 64)                                       /* keep the FIRST 64 */
  {
    flpy_dbg_gate_a[g] = (flpy_dbg_ms << 16) | ((uint32_t)s_wr_trk_cyl << 8) | s_wr_trk_head;
    flpy_dbg_gate_b[g] = ((uint32_t)s_wr_nid << 24) | ((uint32_t)s_wr_ndata << 16) | ((uint32_t)s_wr_visible << 8) | s_wr_first;
  }
  outputs_apply();
}

void EXINT9_5_IRQHandler(void)
{
  exint_flag_clear(EXINT_LINE_9);
  bool gate = in_low(GPIOB, PIN_WGATE) && s_selected && s_media && !st_wprot;
  if(gate && !s_writing) wr_start();
  else if(!gate && s_writing) wr_stop();
}

static void wdata_init(void)
{
  crm_periph_clock_enable(CRM_TMR1_PERIPH_CLOCK, TRUE);
  mfm_dec_init(&s_dec, wr_field, NULL);
  tmr_base_init(TMR1, 0xFFFF, 0);                  /* free-running, 144 MHz */
  tmr_cnt_dir_set(TMR1, TMR_COUNT_UP);
  tmr_input_config_type ic;
  tmr_input_default_para_init(&ic);
  ic.input_channel_select = TMR_SELECT_CHANNEL_1;
  ic.input_mapped_select = TMR_CC_CHANNEL_MAPPED_DIRECT;
  ic.input_polarity_select = TMR_INPUT_FALLING_EDGE;   /* leading edge of the pulse */
  ic.input_filter_value = 0;
  tmr_input_channel_init(TMR1, &ic, TMR_CHANNEL_INPUT_DIV_1);

  dma_init_type di;
  dma_reset(DMA1_CHANNEL2);
  dma_default_para_init(&di);
  di.peripheral_base_addr = (uint32_t)&TMR1->c1dt;
  di.memory_base_addr = (uint32_t)s_wr_ring;
  di.direction = DMA_DIR_PERIPHERAL_TO_MEMORY;
  di.buffer_size = WR_RING;
  di.memory_inc_enable = TRUE;
  di.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_HALFWORD;
  di.memory_data_width = DMA_MEMORY_DATA_WIDTH_HALFWORD;
  di.loop_mode_enable = TRUE;
  di.priority = DMA_PRIORITY_VERY_HIGH;
  dma_init(DMA1_CHANNEL2, &di);
  dma_flexible_config(DMA1, FLEX_CHANNEL2, DMA_FLEXIBLE_TMR1_CH1);
  dma_interrupt_enable(DMA1_CHANNEL2, DMA_HDT_INT | DMA_FDT_INT, TRUE);
  nvic_irq_enable(DMA1_Channel2_IRQn, 0, 0);
  dma_channel_enable(DMA1_CHANNEL2, TRUE);
  tmr_dma_request_enable(TMR1, TMR_C1_DMA_REQUEST, TRUE);   /* capture always on */
  tmr_counter_enable(TMR1, TRUE);
}

/* ================= head movement, select, timers ================= */
void EXINT1_IRQHandler(void)                       /* STEP (trailing edge) */
{
  exint_flag_clear(EXINT_LINE_1);
  if(!s_selected || s_writing) return;             /* spec: only while selected */
  int dir_in = in_low(GPIOB, PIN_DIR);             /* DIR low = step in */
  if(dir_in) { if(s_cyl < FLPY_CYL_LIMIT - 1) s_cyl++; }
  else       { if(s_cyl > 0) s_cyl--; }
  st_trk0 = (s_cyl == 0);
  st_dskchg = false;                               /* a selected STEP clears DISK CHANGE */
  s_seek_ms = SEEK_COMPLETE_MS;
  outputs_apply();
  if(flpy_dbg_step_count < 64)                    /* keep the FIRST 64 (debugging a format) */
    flpy_dbg_steplog[flpy_dbg_step_count] = (flpy_dbg_ms << 16) | ((uint32_t)dir_in << 8) | s_cyl;
  flpy_dbg_step_count++;
}

void EXINT0_IRQHandler(void)                       /* DRIVE SELECT (+ MOTOR ON on the TDS) */
{
  exint_flag_clear(EXINT_LINE_0);
  bool sel = in_low(GPIOA, PIN_SEL);
  if(sel != s_selected)
  {
    if(sel) flpy_dbg_sel_count++;
    s_last_active_ms = flpy_dbg_ms;
    st_ready = false;                              /* motor starting, or stopped */
    s_spinup_ms = sel ? SPINUP_MS : 0;
  }
  s_selected = sel;
  if(!sel && s_writing) wr_stop();
  outputs_apply();
}

void SysTick_Handler(void)
{
  flpy_dbg_ms++;
  board_tick_1ms();                                /* LED activity flashing */
  if(s_index_ms && --s_index_ms == 0) outB_deassert(PIN_INDEX);
  if(s_seek_ms && --s_seek_ms == 0) outputs_apply();
  if(s_spinup_ms && --s_spinup_ms == 0 && s_selected && s_media) { st_ready = true; outputs_apply(); }
}

static void irq_init(void)
{
  exint_init_type ei;
  crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
  gpio_exint_line_config(GPIO_PORT_SOURCE_GPIOA, GPIO_PINS_SOURCE0);   /* SEL   */
  gpio_exint_line_config(GPIO_PORT_SOURCE_GPIOA, GPIO_PINS_SOURCE1);   /* STEP  */
  gpio_exint_line_config(GPIO_PORT_SOURCE_GPIOB, GPIO_PINS_SOURCE9);   /* WGATE */
  exint_default_para_init(&ei);
  ei.line_enable = TRUE;
  ei.line_mode = EXINT_LINE_INTERRUPT;
  ei.line_select = EXINT_LINE_0 | EXINT_LINE_9;
  ei.line_polarity = EXINT_TRIGGER_BOTH_EDGE;
  exint_init(&ei);
  ei.line_select = EXINT_LINE_1;
  ei.line_polarity = EXINT_TRIGGER_RISING_EDGE;    /* STEP trailing edge (spec 8.3.4) */
  exint_init(&ei);
  nvic_irq_enable(EXINT0_IRQn, 1, 0);
  nvic_irq_enable(EXINT1_IRQn, 1, 0);
  nvic_irq_enable(EXINT9_5_IRQn, 1, 0);
}

static void gpio_conf(void)
{
  gpio_init_type gi;
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  /* PB3 (READY) is JTDO and PB4 (SIDE1) NJTRST while JTAG-DP is on (reset
   * default): GPIO writes to PB3 are ignored. Keep SW-DP only. */
  crm_periph_clock_enable(CRM_IOMUX_PERIPH_CLOCK, TRUE);
  gpio_pin_remap_config(SWJTAG_GMUX_010, TRUE);

  gpio_default_para_init(&gi);                     /* inputs idle high */
  gi.gpio_mode = GPIO_MODE_INPUT;
  gi.gpio_pull = GPIO_PULL_UP;
  gi.gpio_pins = PIN_SEL | PIN_STEP | PIN_WDATA;
  gpio_init(GPIOA, &gi);
  gi.gpio_pins = PIN_DIR | PIN_SIDE1 | PIN_WGATE;
  gpio_init(GPIOB, &gi);

  /* status outputs, released: set the release level BEFORE making them outputs
   * (the output register resets to 0 = asserted: a boot-time glitch otherwise) */
  outB_deassert(PIN_INDEX | PIN_READY | PIN_TRK0 | PIN_WPROT | PIN_DSKCHG);
  gi.gpio_mode = GPIO_MODE_OUTPUT;
  gi.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gi.gpio_pull = GPIO_PULL_NONE;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gi.gpio_pins = PIN_INDEX | PIN_TRK0 | PIN_WPROT | PIN_READY | PIN_DSKCHG;
  gpio_init(GPIOB, &gi);
}

/* ================= track load / write-back (main loop) ================= */
static unsigned tidx(uint8_t cyl, uint8_t head) { return (unsigned)cyl * FLPY_HEADS + head; }

/* Advance the background write-back of the spare buffer. The buffer retries a
 * failed read-back itself and gives up after 3 tries (buffer_fault() then
 * reports it): never an endless loop that would freeze the drive. */
static void wb_poll(void)
{
  if(!s_pend_busy || !buffer_wb_poll()) return;
  if(!buffer_wb_ok()) flpy_dbg_store_verify_fail++;
  s_pend_busy = false;
  trace(true, (uint8_t)(s_pend_t / FLPY_HEADS), (uint8_t)(s_pend_t % FLPY_HEADS));
}

static void wb_finish(void) { while(s_pend_busy) wb_poll(); }

/* Hand the live track's unsaved writes to the spare buffer and start writing
 * them back in the background.
 *  keep_live = true  (idle flush): copy; the live track stays in use.
 *  keep_live = false (head moved): swap; the live buffer is free for the next
 *                    track straight away (it is reloaded by track_load). */
static void track_hand_off(bool keep_live)
{
  uint32_t t0 = DWT->CYCCNT;
  wb_finish();                                     /* spare must be free (rarely waits) */
  uint8_t spare = s_live ^ 1;
  uint32_t pm = __get_PRIMASK();
  __disable_irq();
  if(keep_live)
  {
    memcpy(s_trkbuf[spare], s_trk, MFM_TRACK_SIZE);
    memcpy(s_crcbuf[spare], s_crc, sizeof s_crcbuf[spare]);
  }
  else
  {
    s_trk_valid = false;                           /* live buffer about to change */
    s_live = spare;
    s_trk = s_trkbuf[s_live];
    s_crc = s_crcbuf[s_live];
    spare ^= 1;                                    /* the old data is now the spare */
  }
  s_trk_dirty = false;
  __set_PRIMASK(pm);
  s_pend_t = tidx(s_trk_cyl, s_trk_head);
  s_pend_busy = true;
  buffer_wb_start(s_pend_t, s_trkbuf[spare]);
  flpy_dbg_store_us = (DWT->CYCCNT - t0) / (system_core_clock / 1000000u);   /* time the head waited */
}

static void track_load(uint8_t cyl, uint8_t head)
{
  uint32_t t0 = DWT->CYCCNT;
  if(s_trk_dirty) flpy_dbg_load_while_dirty++;     /* unsaved writes about to be lost */
  s_trk_valid = false;                             /* generator hides the track */
  s_trk_cyl = cyl; s_trk_head = head;
  unsigned t = tidx(cyl, head);
  if(s_pend_busy && s_pend_t == t)                 /* back on a track still being written: */
  {                                                /* its newest data is in the spare buffer */
    memcpy(s_trk, s_trkbuf[s_live ^ 1], MFM_TRACK_SIZE);
    memcpy(s_crc, s_crcbuf[s_live ^ 1], sizeof s_crcbuf[0]);
  }
  else
  {
    buffer_load_track(t, s_trk);
    for(unsigned s = 0; s < MFM_SECTORS_PER_TRACK; s++) s_crc[s] = mfm_data_crc(&s_trk[s * MFM_SECTOR_SIZE]);
  }
  s_trk_valid = true;
  flpy_dbg_load_us = (DWT->CYCCNT - t0) / (system_core_clock / 1000000u);
  trace(false, cyl, head);
}

static void geom_apply(bool dd)
{
  uint32_t pm = __get_PRIMASK();
  __disable_irq();
  s_geom = dd ? &mfm_geom_dd : &mfm_geom_hd;
  s_cell = dd ? 2u * TICKS_PER_CELL : TICKS_PER_CELL;
  s_rd_p = 0; s_rd_nb = 0;                         /* restart the revolution */
  flpy_dbg_dd = dd;
  __set_PRIMASK(pm);
}

bool flpy_is_dd(void) { return s_geom == &mfm_geom_dd; }

void flpy_set_density(bool dd)
{
  if(flpy_is_dd() == dd) return;
  if(s_trk_dirty) track_hand_off(true);            /* commit anything pending first */
  wb_finish();
  buffer_set_density(dd);
  geom_apply(dd);
  /* the track just formatted holds the format filler, not the old density's
   * bytes (its format write, at the other rate, was not decodable) */
  memset(s_trk, 0xF6, MFM_SECTORS_PER_TRACK * MFM_SECTOR_SIZE);
  for(unsigned s = 0; s < MFM_SECTORS_PER_TRACK; s++) s_crc[s] = mfm_data_crc(&s_trk[s * MFM_SECTOR_SIZE]);
  s_trk_dirty = true;
  flpy_dbg_density_switches++;
}

void flpy_request_density(bool dd) { s_switch_to = dd ? 1 : 0; }

void flpy_init(void)
{
  geom_apply(buffer_is_dd());
  gpio_conf();
  track_load(0, 0);
  SysTick_Config(system_core_clock / 1000u);
  rdata_init();
  wdata_init();
  s_selected = in_low(GPIOA, PIN_SEL);
  s_spinup_ms = s_selected ? SPINUP_MS : 0;
  outputs_apply();
  irq_init();
  s_started = true;
}

void flpy_poll(void)
{
  if(!s_writing) s_head = in_low(GPIOB, PIN_SIDE1) ? 1 : 0;   /* SIDE1 low = head 1 */
  wb_poll();                                                   /* background write-back */
  if(!s_selected && !s_writing && !s_pend_busy) buffer_bg_poll();   /* idle: pre-erase */
  if(s_writing || !s_media) return;                            /* never touch the track mid-write */
  if(s_switch_to >= 0) { bool dd = s_switch_to == 1; s_switch_to = -1; flpy_set_density(dd); }

  uint8_t cyl = s_cyl, head = s_head;
  if(cyl != s_trk_cyl || head != s_trk_head)
  {
    if(s_trk_dirty) track_hand_off(false);         /* old track -> background write-back */
    track_load(cyl, head);                         /* new track now: ~18 ms, like a real drive */
  }
  else if(s_trk_dirty && !s_pend_busy && flpy_dbg_ms - s_wr_end_ms > WRITEBACK_IDLE_MS)
    track_hand_off(true);                          /* commit soon after a write ends */
}

/* Take the disk out (USB transfer about to rewrite / read the buffer), but only
 * once the host has left the drive alone for EJECT_IDLE_MS: a save deselects
 * briefly between steps, and pulling the disk mid-save lets the host write its
 * cached FAT/directory over the new image (seen 2026-09-24). Gives up after
 * EJECT_WAIT_MS and returns false, touching nothing. On success: everything is
 * committed and the host sees "no disk". */
#define EJECT_IDLE_MS  1500u
#define EJECT_WAIT_MS 15000u   /* the TDS holds select ~12 s over a save + follow-up; the LED flashes meanwhile */
bool flpy_eject(bool (*abort)(void))
{
  uint32_t t0 = flpy_dbg_ms;
  for(;;)
  {
    uint32_t pm = __get_PRIMASK();
    __disable_irq();
    bool idle = !s_selected && !s_writing && flpy_dbg_ms - s_last_active_ms >= EJECT_IDLE_MS;
    if(idle)
    {                                              /* disk out first: nothing new can start */
      s_media = false;
      st_ready = false; st_dskchg = true; s_spinup_ms = 0;
      outputs_apply();
    }
    __set_PRIMASK(pm);
    if(idle) break;
    if(flpy_dbg_ms - t0 > EJECT_WAIT_MS) return false;
    if(abort && abort()) return false;             /* e.g. the user cancelled the transfer */
    watchdog_feed();                               /* waiting for the host is progress */
    flpy_poll();                                   /* keep serving the host while it works:
                                                      track loads + write-back (else it
                                                      skips sectors, see docs/10) */
  }
  if(s_trk_dirty) track_hand_off(true);            /* then commit what the host wrote */
  wb_finish();
  return true;
}

/* Put the (possibly new) disk back: reload the track, flag DISK CHANGE. */
void flpy_insert(void)
{
  s_trk_valid = false;
  s_trk_cyl = 0xFF;                                /* force a reload from the buffer */
  st_dskchg = true;
  s_spinup_ms = s_selected ? SPINUP_MS : 0;
  s_media = true;
  outputs_apply();
}

void flpy_no_disk(void)
{
  s_media = false;
  st_ready = false;
  s_spinup_ms = 0;
  outputs_apply();
}

bool flpy_idle(void) { return !s_selected && !s_writing && !s_trk_dirty && !s_pend_busy; }

void *flpy_scratch(void)
{
  if(s_started && (s_media || s_pend_busy)) return NULL;   /* in use by the floppy */
  return s_trkbuf[s_live ^ 1];
}

uint8_t flpy_current_cyl(void)  { return s_cyl; }
uint8_t flpy_current_head(void) { return s_head; }
bool    flpy_selected(void)     { return s_selected; }
