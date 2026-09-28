#ifndef BOARD_H
#define BOARD_H

#include "at32f415_conf.h"
#include <stdbool.h>

/*
 * Board support for the SFR1M44-DU26 floppy emulator (PCB SFRC2D.B): LED,
 * timing, buttons, watchdog. Authoritative pin map: docs/02-pinmap.csv; the
 * floppy, SPI-flash and USB pins live in their own modules.
 *
 * LED: bi-colour. Red = PB10 (ACTIVE-LOW: common anode). Green is not driven
 *      by the MCU: its cathode is the drive-select line (PA0, a host input),
 *      so it lights whenever the scope selects the drive (not fitted on some
 *      units).
 */

#define LED_RED_GPIO        GPIOB
#define LED_RED_PIN         GPIO_PINS_10
#define LED_RED_CRM_CLK     CRM_GPIOB_PERIPH_CLOCK

/* Bring up the delay timer and the LED. Call once after system_clock_config(). */
void board_init(void);

/* LED control (red side). */
void led_red_on(void);
void led_red_off(void);
void led_red_toggle(void);

/* Red LED flashing while files are being copied: one flash per period. */
#define LED_ACTIVITY_PERIOD_MS 250u
void led_activity(bool on);
void board_tick_1ms(void);          /* called from the SysTick handler */

/* Independent watchdog: started once in main(); if nothing feeds it for about
 * 16 s (11-21 s over the LICK oscillator's tolerance) the MCU resets, so any
 * hang recovers by itself. Fed by the main loop, every USB sector transfer and
 * the eject wait. It pauses while a debugger holds the core halted. */
void watchdog_start(void);
static inline void watchdog_feed(void) { WDT->cmd = 0xAAAAu; }

/* Front-panel buttons (PC7, PC8; active low, pulled up). */
#define BTN_LEFT   GPIO_PINS_7    /* PC7: DATA OUT (upper button as installed) */
#define BTN_RIGHT  GPIO_PINS_8    /* PC8: DATA IN (lower button) */
/* Pressed buttons as BUTTON_LEFT / BUTTON_RIGHT bits (buttons.h). */
uint16_t buttons_raw(void);

/* Blocking millisecond delay driven by the CPU cycle counter (DWT). */
void delay_ms(uint32_t ms);
void delay_us(uint32_t us);


#endif /* BOARD_H */
