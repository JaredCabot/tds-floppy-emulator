/* usb_conf.h - Artery USB library configuration for the emulator (host mode only).
 * Adapted from the AT32F415 usb_host/msc_only_fat32 example. Pin conflicts on
 * this board decide the options: PA9 is the debug UART (so no VBUS sensing),
 * PB3 is the floppy READY output (so no VBUS power-switch pin; the USB-A port's
 * 5 V is always on), PA8 is WDATA (so no SOF output). */
#ifndef __USB_CONF_H
#define __USB_CONF_H
#ifdef __cplusplus
extern "C" {
#endif

#include "at32f415_usb.h"
#include "at32f415.h"
#include <stddef.h>          /* the library uses NULL (the example got it via stdio.h) */

#define USE_OTG_HOST_MODE

#define USB_ID                           0
#define OTG_CLOCK                        CRM_OTGFS1_PERIPH_CLOCK
#define OTG_IRQ                          OTGFS1_IRQn
#define OTG_IRQ_HANDLER                  OTGFS1_IRQHandler

#define OTG_PIN_GPIO                     GPIOA
#define OTG_PIN_GPIO_CLOCK               CRM_GPIOA_PERIPH_CLOCK

#ifndef USB_HOST_CHANNEL_NUM
#define USB_HOST_CHANNEL_NUM             8
#endif
#define USBH_RX_FIFO_SIZE                128
#define USBH_NP_TX_FIFO_SIZE             96
#define USBH_P_TX_FIFO_SIZE              96

#define USB_VBUS_IGNORE                  /* PA9 is USART1 TX */

#define USBH_DEBUG(...)

void usb_delay_ms(uint32_t ms);
void usb_delay_us(uint32_t us);

#ifdef __cplusplus
}
#endif
#endif
