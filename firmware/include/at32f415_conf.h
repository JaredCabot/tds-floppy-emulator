/**
 * at32f415_conf.h - driver configuration for the emulator clone firmware.
 *
 * Enable only the peripheral driver modules we actually compile. Add modules
 * here as later phases need them (SPI for the NOR buffer, TMR/EXINT/DMA for the
 * floppy bus, USB for the stick). KISS: don't enable what we don't use.
 */
#ifndef __AT32F415_CONF_H
#define __AT32F415_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* clock source values (board fits an 8 MHz crystal, see docs/01-hardware.md) */
#define HEXT_VALUE            ((uint32_t)8000000)
#define HEXT_STARTUP_TIMEOUT  ((uint16_t)0x3000)
#define HICK_VALUE            ((uint32_t)8000000)
#define LEXT_VALUE            ((uint32_t)32768)

/* --- Phase 1a: clocks, GPIO (LED), USART (debug log) --- */
#define CRM_MODULE_ENABLED
#define GPIO_MODULE_ENABLED
#define USART_MODULE_ENABLED
#define FLASH_MODULE_ENABLED   /* flash_psr_set() in the clock config */
#define PWC_MODULE_ENABLED     /* pulled in by crm_reset() path */
#define MISC_MODULE_ENABLED    /* nvic helpers */
#define SPI_MODULE_ENABLED     /* Phase 1b: SST25VF016B on SPI2 */
#define TMR_MODULE_ENABLED     /* Phase 1c: RDATA stream timer (TMR3) */
#define DMA_MODULE_ENABLED     /* Phase 1c: DMA feeds TMR3 periods */
#define EXINT_MODULE_ENABLED   /* Phase 1c: STEP interrupt */
#define USB_MODULE_ENABLED     /* USB host (OTGFS) for the stick */
#define WDT_MODULE_ENABLED     /* independent watchdog: a hang resets the unit */

#include "at32f415.h"

/* pull in the driver headers for the enabled modules */
#ifdef CRM_MODULE_ENABLED
  #include "at32f415_crm.h"
#endif
#ifdef GPIO_MODULE_ENABLED
  #include "at32f415_gpio.h"
#endif
#ifdef USART_MODULE_ENABLED
  #include "at32f415_usart.h"
#endif
#ifdef SPI_MODULE_ENABLED
  #include "at32f415_spi.h"
#endif
#ifdef TMR_MODULE_ENABLED
  #include "at32f415_tmr.h"
#endif
#ifdef DMA_MODULE_ENABLED
  #include "at32f415_dma.h"
#endif
#ifdef EXINT_MODULE_ENABLED
  #include "at32f415_exint.h"
#endif
#ifdef PWC_MODULE_ENABLED
  #include "at32f415_pwc.h"
#endif
#ifdef FLASH_MODULE_ENABLED
  #include "at32f415_flash.h"
#endif
#ifdef USB_MODULE_ENABLED
#include "at32f415_usb.h"
#endif
#ifdef MISC_MODULE_ENABLED
  #include "at32f415_misc.h"
#endif
#ifdef WDT_MODULE_ENABLED
  #include "at32f415_wdt.h"
#endif

/* driver parameter-check macro (drivers reference assert_param) */
#ifdef USE_STDPERIPH_DRIVER
  #define assert_param(expr) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __AT32F415_CONF_H */
