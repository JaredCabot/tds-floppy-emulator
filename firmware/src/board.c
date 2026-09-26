/**
 * board.c - Phase 1a board bring-up: LED, debug UART, delay.
 *
 * Pins per docs/02-pinmap.csv. Keep this the ONLY place that knows physical
 * pin assignments (SOLID: board is the hardware-abstraction seam; the rest of
 * the firmware talks to the led and dbg helpers, never to raw GPIO).
 */
#include "board.h"
#include "buttons.h"
#include <stdio.h>

/* ---- cycle-counter delay (DWT), exact at any core clock ---- */
static uint32_t cycles_per_ms;

static void delay_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  cycles_per_ms = system_core_clock / 1000u;
}

void delay_ms(uint32_t ms)
{
  while(ms--)
  {
    uint32_t start = DWT->CYCCNT;
    while((DWT->CYCCNT - start) < cycles_per_ms) { }
  }
}

void delay_us(uint32_t us)
{
  uint32_t start = DWT->CYCCNT, n = us * (cycles_per_ms / 1000u);
  while((DWT->CYCCNT - start) < n) { }
}

/* ---- LED (red side, PB10) ---- */
static void led_init(void)
{
  gpio_init_type gi;
  crm_periph_clock_enable(LED_RED_CRM_CLK, TRUE);
  gpio_default_para_init(&gi);
  gi.gpio_pins = LED_RED_PIN;
  gi.gpio_mode = GPIO_MODE_OUTPUT;
  gi.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gi.gpio_pull = GPIO_PULL_NONE;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init(LED_RED_GPIO, &gi);
  gpio_bits_set(LED_RED_GPIO, LED_RED_PIN);        /* off (active-low, see below) */
}

/* The bi-colour LED has a common anode (net L4-A, to +3.3 V); the red cathode is
 * on PB10 and the green cathode on the drive-select line (lit when the scope
 * selects the drive). So red is ACTIVE-LOW: pin low = lit. (A symmetric blink
 * hid this; a steady "off" showed it as steady red.) */
void led_red_on(void)     { gpio_bits_reset(LED_RED_GPIO, LED_RED_PIN); }
void led_red_off(void)    { gpio_bits_set(LED_RED_GPIO, LED_RED_PIN); }
void led_red_toggle(void)
{
  if(LED_RED_GPIO->odt & LED_RED_PIN) led_red_on();
  else                                led_red_off();
}

/* Activity flashing, driven from the 1 ms tick so it keeps going while the
 * main loop is busy in a blocking transfer. */
static volatile bool     s_act;
static volatile uint32_t s_act_ms;

void led_activity(bool on)
{
  s_act_ms = 0;
  s_act = on;
  if(on) led_red_on(); else led_red_off();
}

void board_tick_1ms(void)
{
  if(s_act && ++s_act_ms >= LED_ACTIVITY_PERIOD_MS / 2u) { s_act_ms = 0; led_red_toggle(); }
}

/* ---- debug UART (USART1 TX = PA9), 115200 8N1 ---- */
static void dbg_uart_init(void)
{
  gpio_init_type gi;
  crm_periph_clock_enable(DBG_USART_GPIO_CLK, TRUE);
  crm_periph_clock_enable(DBG_USART_CRM_CLK, TRUE);

  gpio_default_para_init(&gi);
  gi.gpio_pins = DBG_USART_TX_PIN;      /* PA9, USART1 default mapping */
  gi.gpio_mode = GPIO_MODE_MUX;
  gi.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gi.gpio_pull = GPIO_PULL_NONE;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
  gpio_init(DBG_USART_GPIO, &gi);

  usart_init(DBG_USART, DBG_USART_BAUD, USART_DATA_8BITS, USART_STOP_1_BIT);
  usart_transmitter_enable(DBG_USART, TRUE);
  usart_enable(DBG_USART, TRUE);
}

void dbg_putc(char c)
{
  while(usart_flag_get(DBG_USART, USART_TDBE_FLAG) == RESET) { }
  usart_data_transmit(DBG_USART, (uint16_t)c);
  while(usart_flag_get(DBG_USART, USART_TDC_FLAG) == RESET) { }
}

void dbg_puts(const char *s) { while(*s) dbg_putc(*s++); }

/* retarget printf() to the debug UART (newlib syscall) */
int _write(int fd, char *ptr, int len);   /* newlib stdout hook (unused since printf was dropped) */
int _write(int fd, char *ptr, int len)
{
  (void)fd;
  for(int i = 0; i < len; i++) dbg_putc(ptr[i]);
  return len;
}

void board_init(void)
{
  delay_init();
  led_init();
  dbg_uart_init();
}

uint16_t buttons_raw(void)
{
  uint32_t low = ~GPIOC->idt;
  return (uint16_t)(((low & BTN_LEFT) ? BUTTON_LEFT : 0u) | ((low & BTN_RIGHT) ? BUTTON_RIGHT : 0u));
}

void watchdog_start(void)
{
  *(volatile uint32_t *)0xE0042004u |= 0x100u;   /* DEBUG ctrl wdt_pause: stop while halted by SWD */
  wdt_register_write_enable(TRUE);
  wdt_divider_set(WDT_CLK_DIV_256);              /* LICK ~40 kHz / 256 = ~156 Hz */
  wdt_reload_value_set(2500u - 1u);              /* ~16 s */
  wdt_counter_reload();
  wdt_enable();
}
