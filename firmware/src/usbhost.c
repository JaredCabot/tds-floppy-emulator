/**
 * usbhost.c - USB host for the stick: Artery host core + MSC class, FatFs disk
 * bridge (diskio), and a ready flag for the application. Adapted from the
 * AT32F415 usb_host/msc_only_fat32 example (docs/11-usb.md).
 */
#include "usbhost.h"
#include "board.h"
#include "usb_conf.h"
#include "usb_core.h"
#include "usbh_int.h"
#include "usbh_msc_class.h"
#include "ff.h"
#include "diskio.h"

static otg_core_type s_otg;
static volatile bool s_ready;

/* ---- host-stack user callbacks (usbh_core.h) ---- */
static usb_sts_type cb_ok(void)                   { return USB_OK; }
static usb_sts_type cb_disconnect(void)           { s_ready = false; return USB_OK; }
static usb_sts_type cb_speed(uint8_t s)           { (void)s; return USB_OK; }
static usb_sts_type cb_string(void *s)            { (void)s; return USB_OK; }
static usb_sts_type cb_application(void)          { s_ready = true; return USB_OK; }   /* MSC ready */
static usb_sts_type cb_vbus(void *h, confirm_state st) { (void)h; (void)st; return USB_OK; }  /* 5 V always on */

static usbh_user_handler_type s_user = {
  cb_ok, cb_ok, cb_ok, cb_disconnect, cb_speed, cb_string, cb_string, cb_string,
  cb_ok, cb_application, cb_vbus, cb_ok,
};

void usb_delay_ms(uint32_t ms) { delay_ms(ms); }
void usb_delay_us(uint32_t us) { delay_us(us); }

void OTGFS1_IRQHandler(void);                     /* vector table (startup file) */
void OTGFS1_IRQHandler(void) { usbh_irq_handler(&s_otg); }

void usbh_app_init(void)
{
  gpio_init_type gi;
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  gpio_default_para_init(&gi);                     /* PA11 D-, PA12 D+ */
  gi.gpio_pins = GPIO_PINS_11 | GPIO_PINS_12;
  gi.gpio_mode = GPIO_MODE_MUX;
  gi.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(GPIOA, &gi);

  crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_PLL);
  crm_usb_clock_div_set(CRM_USB_DIV_3);            /* 144 MHz PLL / 3 = 48 MHz */
  crm_periph_clock_enable(OTG_CLOCK, TRUE);
  /* Below the floppy's DMA/EXINT interrupts (0/1): USB timing is lenient,
   * the flux stream is not. */
  nvic_irq_enable(OTG_IRQ, 2, 0);
  usbh_init(&s_otg, USB_FULL_SPEED_CORE_ID, USB_ID, &uhost_msc_class_handler, &s_user);
}

void usbh_app_poll(void) { usbh_loop_handler(&s_otg.host); }

bool usbh_app_ready(void) { return s_ready && s_otg.host.conn_sts; }

uint32_t usbh_app_block_size(void)
{
  if(!usbh_app_ready() || !s_otg.host.class_handler) return 0;
  usbh_msc_type *m = (usbh_msc_type *)s_otg.host.class_handler->pdata;
  return m ? m->l_unit_n[0].capacity.blk_size : 0;
}

/* ---- FatFs disk bridge (physical drive 0 = LUN 0 of the stick) ---- */
DSTATUS disk_status(BYTE pdrv)
{
  return usbh_msc_is_ready(&s_otg.host, pdrv) == MSC_OK ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

/* LBA_t is 64-bit (FF_LBA64, for GPT); the MSC driver takes 32-bit sector
 * numbers, i.e. sticks up to 2 TB. Beyond that: an error, never a wrapped LBA. */
static bool lba_ok(LBA_t sector, UINT count) { return sector + count <= 0x100000000ull; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
  if(!lba_ok(sector, count)) return RES_PARERR;
  if(usbh_app_block_size() != 512) return RES_ERROR;   /* larger blocks would overrun buff */
  watchdog_feed();                                     /* a transfer is progress */
  return usbh_msc_read(&s_otg.host, (uint32_t)sector, count, buff, pdrv) == USB_OK ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
  if(!lba_ok(sector, count)) return RES_PARERR;
  if(usbh_app_block_size() != 512) return RES_ERROR;
  watchdog_feed();
  return usbh_msc_write(&s_otg.host, (uint32_t)sector, count, (uint8_t *)buff, pdrv) == USB_OK ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
  (void)pdrv; (void)buff;
  return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;
}
