#ifndef USBHOST_H
#define USBHOST_H

#include <stdbool.h>
#include <stdint.h>

/*
 * USB host for the stick in the emulator's USB-A port (OTGFS on PA11/PA12),
 * using Artery's host core + MSC class, with FatFs on top ("0:" = the stick).
 */

void usbh_app_init(void);     /* clocks, pins, host stack */
void usbh_app_poll(void);     /* call from the main loop */
bool usbh_app_ready(void);

/* Logical block size the stick reported (READ CAPACITY); 0 if not ready. Only
 * 512 is supported: FatFs is built for 512-byte sectors. */
uint32_t usbh_app_block_size(void);    /* stick enumerated and ready for FatFs */

#endif /* USBHOST_H */
