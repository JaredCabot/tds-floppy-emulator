/*
 * boot.c - emulator bootloader (8 KB at 0x08000000). See include/update.h and
 * docs/13-firmware-update.md.
 *
 * At every reset: if the SPI flash holds a committed update (valid header) and
 * the installed application differs from it, check the staged image's CRC,
 * install it (erase + program + verify, up to 3 attempts), then start the
 * application at APP_BASE. An install interrupted by power loss just happens
 * again at the next power-up: the staged image stays until the next update.
 *
 * Deliberately minimal: no USB, no PLL (runs on the 8 MHz reset clock), no
 * interrupts. It leaves the MCU as it found it (peripherals reset) before
 * jumping, so the application starts as if from reset.
 */
#include "at32f415_conf.h"
#include "spiflash.h"
#include "update.h"
#include <string.h>

#define LED_PIN   GPIO_PINS_10            /* PB10, red, ACTIVE-LOW */
#define CHUNK     256u

static uint8_t s_buf[CHUNK];

static void led_init(void)
{
  gpio_init_type gi;
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  gpio_default_para_init(&gi);
  gi.gpio_pins = LED_PIN;
  gi.gpio_mode = GPIO_MODE_OUTPUT;
  gi.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init(GPIOB, &gi);
  gpio_bits_set(GPIOB, LED_PIN);          /* off */
}
static void led_toggle(void) { GPIOB->odt ^= LED_PIN; }
static void delay_loops(uint32_t n) { while(n--) __NOP(); }

/* The flash status flags are sticky and the Artery driver reports ANY set error
 * flag as the result of the current operation, so a flag left over from before
 * (e.g. an earlier SWD programming session) failed the first real install.
 * Clear them before every erase / program. */
static void flash_flags_clear(void)
{
  flash_flag_clear(FLASH_ODF_FLAG | FLASH_PRGMERR_FLAG | FLASH_EPPERR_FLAG);
}

/* Does the installed application equal the staged image, byte for byte? */
static bool installed_matches(const upd_header_t *h)
{
  for(uint32_t off = 0; off < h->length; off += CHUNK)
  {
    uint32_t n = h->length - off < CHUNK ? h->length - off : CHUNK;
    spiflash_read(UPD_STAGE_DATA + off, s_buf, n);
    if(memcmp(s_buf, (const void *)(APP_BASE + off), n) != 0) return false;
  }
  return true;
}

static bool staged_crc_ok(const upd_header_t *h)
{
  uint32_t crc = 0;
  for(uint32_t off = 0; off < h->length; off += CHUNK)
  {
    uint32_t n = h->length - off < CHUNK ? h->length - off : CHUNK;
    spiflash_read(UPD_STAGE_DATA + off, s_buf, n);
    if(off == 0 && !upd_vectors_ok(s_buf, h->length)) return false;
    crc = upd_crc32(crc, s_buf, n);
  }
  return crc == h->image_crc;
}

/* Erase the sectors the image covers and program it from the SPI flash. The
 * vector table (first 8 bytes) is programmed LAST: until the whole image is
 * in, the application area reads as erased there and is never started. */
static bool install(const upd_header_t *h)
{
  bool ok = true;
  uint32_t vec[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
  flash_unlock();
  for(uint32_t a = APP_BASE; a < APP_BASE + h->length && ok; a += APP_SECTOR_SIZE)
  {
    WDT->cmd = 0xAAAAu;                   /* in case the app's watchdog survived the reset */
    led_toggle();
    flash_flags_clear();
    ok = flash_sector_erase(a) == FLASH_OPERATE_DONE;
  }
  for(uint32_t off = 0; off < h->length && ok; off += CHUNK)
  {
    uint32_t n = h->length - off < CHUNK ? h->length - off : CHUNK;   /* multiple of 4 */
    spiflash_read(UPD_STAGE_DATA + off, s_buf, n);
    WDT->cmd = 0xAAAAu;
    for(uint32_t i = 0; i < n && ok; i += 4)
    {
      uint32_t w = (uint32_t)s_buf[i] | (uint32_t)s_buf[i + 1] << 8 | (uint32_t)s_buf[i + 2] << 16 | (uint32_t)s_buf[i + 3] << 24;
      if(off + i < 8u) { vec[(off + i) / 4u] = w; continue; }       /* later */
      flash_flags_clear();
      ok = flash_word_program(APP_BASE + off + i, w) == FLASH_OPERATE_DONE;
    }
  }
  if(ok) { flash_flags_clear(); ok = flash_word_program(APP_BASE + 4u, vec[1]) == FLASH_OPERATE_DONE; }
  if(ok) { flash_flags_clear(); ok = flash_word_program(APP_BASE, vec[0]) == FLASH_OPERATE_DONE; }
  flash_lock();
  return ok && installed_matches(h);
}

static bool app_runnable(void)
{
  return upd_vectors_ok((const uint8_t *)APP_BASE, APP_MAX);
}

static void jump_to_app(void)
{
  /* put back what we touched: the application initialises from a clean slate */
  crm_periph_reset(CRM_SPI2_PERIPH_RESET, TRUE);  crm_periph_reset(CRM_SPI2_PERIPH_RESET, FALSE);
  crm_periph_reset(CRM_GPIOB_PERIPH_RESET, TRUE); crm_periph_reset(CRM_GPIOB_PERIPH_RESET, FALSE);
  crm_periph_clock_enable(CRM_SPI2_PERIPH_CLOCK, FALSE);
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, FALSE);

  uint32_t sp = *(volatile uint32_t *)APP_BASE;
  uint32_t pc = *(volatile uint32_t *)(APP_BASE + 4u);
  SCB->VTOR = APP_BASE;
  __DSB(); __ISB();
  __asm volatile("msr msp, %0\n bx %1" :: "r"(sp), "r"(pc));
  for(;;) { }
}

int main(void)
{
  led_init();
  if(spiflash_init())
  {
    upd_header_t h;
    spiflash_read(UPD_STAGE_ADDR, &h, sizeof h);
    if(upd_header_ok(&h) && !installed_matches(&h) && staged_crc_ok(&h))
    {
      bool done = false;
      for(int attempt = 0; attempt < 3 && !done; attempt++) done = install(&h);
      gpio_bits_set(GPIOB, LED_PIN);      /* off */
    }
  }
  if(app_runnable()) jump_to_app();

  /* nothing runnable (only possible if the install failed 3 times, or after a
   * bad SWD flash): fast double blink forever; recover over SWD. */
  for(;;)
  {
    led_toggle(); delay_loops(150000); led_toggle(); delay_loops(150000);
    led_toggle(); delay_loops(150000); led_toggle(); delay_loops(900000);
  }
}
