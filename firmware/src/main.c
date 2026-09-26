/**
 * main.c - TDS Floppy Emulator Firmware (clean-room, SFR1M44-DU26 hardware) (docs/04-architecture.md).
 *
 * The scope sees a 1.44 MB floppy (floppy.c) whose contents live in the SPI
 * flash (buffer.c). A USB stick moves files in and out (xfer.c):
 *   RIGHT button (PC8): DATA IN - load the next page of the stick's files
 *   LEFT  button (PC7): DATA OUT - copy every file on the internal disk to it,
 *                       verify, then blank the internal disk ("take the disk out")
 *   Inserting a stick does nothing by itself: transfers only on a button press
 *   (Jared's requirement; the stock unit auto-loaded on insertion).
 * LED (red, PB10): flashes 4x/s while copying; 6 fast blinks = error, 2 = nothing to load,
 *   3 slow = stick format not supported; continuous 5 Hz = the buffer flash failed
 *   (self-test at boot, or a track could not be stored). Off when idle.
 * Green LED is hard-wired to the scope's drive-select line.
 */
#include "board.h"
#include "clock.h"
#include "spiflash.h"
#include "buffer.h"
#include "floppy.h"
#include "usbhost.h"
#include "xfer.h"
#include "update.h"
#include "buttons.h"
#include "version.h"


/* ---- SWD debug hooks (used by the scripts in tools/) ----
 * dbg_flash_req = 1: copy 512 B of SPI flash at dbg_flash_addr to dbg_flash_buf.
 * dbg_flash_req = 2: re-format the internal disk (empty volume) and restart.
 * dbg_flash_req = 3 / 4: write / read dbg_flash_len bytes of stick file dbg_name
 *                 at offset dbg_flash_addr via dbg_flash_buf (tools/stick.ps1);
 *                 dbg_flash_len = bytes done, or negative on error.
 * dbg_button    = 1 / 2 / 3: RIGHT (data in) / LEFT (data out) / firmware update,
 *                 as the real buttons do; dbg_xfer_result/_count report the outcome.
 * dbg_fw_version = FW_VERSION of the running firmware.
 * These need physical SWD access, which can reflash the MCU anyway, so they stay
 * in every build (the bench tools use them). The ones that change data (2, 3 and
 * the buttons) also need dbg_unlock == DBG_UNLOCK, so a stray write to one of
 * these variables can never format the disk or start a transfer. */
/* dbg_flash_req = 5 (unlocked): program dbg_flash_len bytes of dbg_flash_buf at
 * dbg_flash_addr in the SPI flash (fault-injection tests, docs/10). */
#define DBG_UNLOCK 0x5AFE1234u
volatile uint32_t dbg_unlock;
volatile uint32_t dbg_reset_cause;         /* CRM ctrlsts at boot: bit 29 = watchdog reset */
volatile uint32_t dbg_fw_build;            /* build ID: CRC-32 of this firmware image, the "crc" */
                                           /* make prints and UPDATE.UPD carries (docs/06) */
volatile uint32_t dbg_fault;               /* buffer_fault(): the flash has failed (docs/10) */
extern uint32_t _sidata, _sdata, _edata;   /* linker: the image ends after .data's load image */
volatile uint32_t dbg_flash_addr, dbg_flash_req;
volatile int32_t  dbg_flash_len;
volatile uint8_t  dbg_flash_buf[512] __attribute__((aligned(4)));   /* SWD tools read it as 32-bit words */
volatile char     dbg_name[68] __attribute__((aligned(4)));
volatile uint32_t dbg_fw_version;          /* set in main(): SWD-readable version */
volatile uint32_t dbg_button, dbg_xfer_result, dbg_xfer_count, dbg_usb_ready;

static void buttons_init(void)
{
  gpio_init_type gi;
  crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
  gpio_default_para_init(&gi);
  gi.gpio_pins = BTN_LEFT | BTN_RIGHT;
  gi.gpio_mode = GPIO_MODE_INPUT;
  gi.gpio_pull = GPIO_PULL_UP;
  gpio_init(GPIOC, &gi);
}

static uint32_t s_blinks;                  /* LED result pattern: toggles left */
static uint32_t s_blink_ms = 80;           /* ... and its toggle period */
static bool s_blink_new;                   /* a pattern was just set: start it now */
static uint32_t s_blink_next;              /* ... and when its next toggle is due (ms) */
static uint32_t s_fault_next;              /* next toggle of the flash-fault blink (ms) */

static xfer_result_t run(xfer_result_t (*op)(void))
{
  led_activity(true);                      /* flashes red while copying */
  xfer_result_t r = op();
  led_activity(false);
  dbg_xfer_result = r;
  dbg_xfer_count++;
  /* 2 quick blinks: nothing to load; 3 SLOW blinks: stick format not supported
   * (reformat as FAT32 / exFAT); 6 quick blinks: any other error */
  /* ... and one long (1 s) blink: cancelled by its own button (docs/11) */
  s_blink_ms = (r == XFER_BAD_FORMAT) ? 400u : (r == XFER_CANCELLED) ? 1000u : 80u;
  s_blinks = (r == XFER_OK) ? 0 : (r == XFER_NOTHING) ? 4 : (r == XFER_BAD_FORMAT) ? 6
           : (r == XFER_CANCELLED) ? 2 : 12;
  s_blink_new = true;
  return r;
}

/* Stack high-water mark: the free RAM between the heap and the stack is painted
 * at boot; dbg_stack_free = bytes the stack has never reached (read over SWD
 * after a transfer, the deepest code path). Keep it well above 0. */
#define STACK_PAINT 0xA5A5A5A5u
extern uint32_t _ebss;
extern void *_sbrk(int incr);
volatile uint32_t dbg_stack_free;
extern volatile uint32_t flpy_dbg_ms;          /* 1 ms tick (floppy.c) */

static void stack_paint(void)
{
  uint32_t sp;
  __asm volatile("mov %0, sp" : "=r"(sp));
  for(uint32_t *p = &_ebss; p < (uint32_t *)(sp - 64u); p++) *p = STACK_PAINT;
}

static uint32_t stack_free(void)
{
  uint32_t *p = (uint32_t *)(((uint32_t)_sbrk(0) + 3u) & ~3u);   /* above the heap */
  uint32_t n = 0;
  while(p[n] == STACK_PAINT) n++;
  return n * 4u;
}

int main(void)
{
  SCB->VTOR = APP_BASE;                    /* linked after the bootloader (SystemInit set 0x08000000) */
  __DSB();
  stack_paint();
  dbg_fw_version = FW_VERSION;
  {                                        /* build ID: CRC-32 of the image, padded like UPDATE.UPD */
    uint32_t len = (uint32_t)&_sidata + ((uint32_t)&_edata - (uint32_t)&_sdata) - APP_BASE;
    dbg_fw_build = upd_crc32(0, (const void *)APP_BASE, (len + 3u) & ~3u);
  }
  system_clock_config();
  nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);
  board_init();
  buttons_init();
  dbg_reset_cause = CRM->ctrlsts;          /* why we started (a watchdog reset shows here) */
  CRM->ctrlsts |= 1u << 24;                /* rstfc: clear the reset flags */
  watchdog_start();

  spiflash_init();
  bool flash_ok = spiflash_selftest();
  if(flash_ok)
  {
    xfer_update_ack();                     /* a completed update: withdraw its record */
    buffer_init();                         /* restore a cut-short write-back; first boot: format */
  }
  flpy_init();
  if(!flash_ok) flpy_no_disk();            /* failed flash: serve nothing rather than garbage */
  usbh_app_init();

  bool was_ready = false;
  buttons_t btn = {0};
  uint32_t stack_ms = 0;
  uint32_t btn_ms = flpy_dbg_ms;
  bool led_armed = false;
  while(1)
  {
    watchdog_feed();
    flpy_poll();
    usbh_app_poll();
    if(flpy_dbg_ms - stack_ms >= 1000u) { stack_ms = flpy_dbg_ms; dbg_stack_free = stack_free(); }  /* 1 Hz */

    bool unlocked = dbg_unlock == DBG_UNLOCK;
    if(dbg_flash_req == 2 && unlocked) { if(flpy_eject(NULL)) buffer_format(); NVIC_SystemReset(); }
    if(dbg_flash_req == 1)
    {
      spiflash_read(dbg_flash_addr, (void *)dbg_flash_buf, sizeof dbg_flash_buf);
      dbg_flash_req = 0;
    }

    bool ready = usbh_app_ready();
    dbg_usb_ready = ready;
    if(!ready && was_ready) xfer_reset_paging();
    was_ready = ready;

    if(dbg_flash_req == 5 && unlocked)     /* test hook: program bytes into the SPI flash */
    {                                      /* (e.g. damage the marker: docs/10) */
      uint32_t n = dbg_flash_len > 0 && dbg_flash_len <= 512 ? (uint32_t)dbg_flash_len : 0u;
      if(n) spiflash_program(dbg_flash_addr, (const void *)dbg_flash_buf, n);
      dbg_flash_req = 0;
    }
    if(dbg_flash_req == 4 || (dbg_flash_req == 3 && unlocked))
    {
      dbg_name[sizeof dbg_name - 1] = 0;
      dbg_flash_len = xfer_dbg_stick(dbg_flash_req == 3, (const char *)dbg_name, dbg_flash_addr,
                                     (void *)dbg_flash_buf, dbg_flash_len > 512 ? 512u : (uint32_t)dbg_flash_len);
      dbg_flash_req = 0;
    }

    /* buttons: one gesture step per elapsed millisecond, whatever the loop took */
    uint16_t b = 0;
    uint16_t raw = buttons_raw();
    while(btn_ms != flpy_dbg_ms) { btn_ms++; if(!b) b = buttons_step(&btn, raw); }
    if(buttons_armed(&btn) != led_armed)                 /* red LED solid while armed */
    {
      led_armed = buttons_armed(&btn);
      if(led_armed) led_red_on(); else led_red_off();
    }
    if(unlocked && dbg_button == 1) b = BUTTON_RIGHT;
    if(unlocked && dbg_button == 2) b = BUTTON_LEFT;
    if(unlocked && dbg_button == 3) b = BUTTON_UPDATE;
    dbg_button = 0;
    if(!flash_ok) b = 0;                                 /* nothing to transfer to or from */
    if(b == BUTTON_RIGHT) run(xfer_in);
    else if(b == BUTTON_LEFT) run(xfer_out);
    else if(b == BUTTON_UPDATE && run(xfer_update) == XFER_OK)
    {
      led_red_off();
      NVIC_SystemReset();                  /* the bootloader installs the staged image */
    }
    if(b)                                  /* a transfer ran (seconds): start the buttons afresh */
    {                                      /* rather than replay that time as one long press */
      btn_ms = flpy_dbg_ms;
      buttons_resync(&btn, buttons_raw());
    }
    dbg_fault = buffer_fault();

    /* a result pattern first (it starts at once), then any flash-fault blink;
     * timed in real milliseconds, whatever the loop takes */
    if(s_blink_new) { s_blink_new = false; s_blink_next = flpy_dbg_ms; }
    if(s_blinks)
    {
      if((int32_t)(flpy_dbg_ms - s_blink_next) >= 0)
      {
        s_blink_next += s_blink_ms;
        led_red_toggle();
        if(--s_blinks == 0) led_red_off();
      }
    }
    else if(!flash_ok || buffer_fault())
    {
      if((int32_t)(flpy_dbg_ms - s_fault_next) >= 0) { s_fault_next = flpy_dbg_ms + 100u; led_red_toggle(); }
    }

    delay_ms(1);
  }
}
