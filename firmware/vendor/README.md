# Vendor code

Third-party code needed to build the firmware, unmodified unless noted. Each
file keeps its original copyright notice.

| Folder | What | Source | Licence |
|---|---|---|---|
| `cmsis/` | Arm CMSIS core + AT32F415 device support, startup, linker scripts | Artery AT32F415 Firmware Library V2.1.8 | Arm (Apache-2.0) / Artery notice in each file |
| `drivers/` | AT32F415 peripheral drivers (CRM, GPIO, SPI, TMR, DMA, EXINT, USB, ...) | Artery AT32F415 Firmware Library V2.1.8 | Artery notice in each file |
| `usb/` | USB OTG core, host core, MSC host class (+ device headers the core includes) | Artery AT32F415 Firmware Library V2.1.8 (`middlewares/usb_drivers`, `middlewares/usbh_class/usbh_msc`) | Artery notice in each file |
| `fatfs/` | FatFs R0.15 (ChaN) | Artery AT32F415 Firmware Library V2.1.8 (`middlewares/3rd_party/fatfs`) | BSD-style, see `fatfs/LICENSE.txt` |

The Artery notice authorises customers "to use, copy, and distribute the BSP
software and its related documentation for the purpose of design and
development in conjunction with Artery microcontrollers", which is what this
project is (firmware for an AT32F415).

Not taken from the library: `fatfs/diskio.c` and `fatfs/ffconf.h` (the FatFs
disk bridge is `src/usbhost.c`; the configuration is `include/ffconf.h`).

The full library is available from Artery (arterychip.com) as
`AT32F415_Firmware_Library_V2.1.8.zip`.

## Local changes

- `usb/msc/usbh_msc_class.c`: the read/write timeout was `len * 10000` ms
  (10 s per sector); it is now `MSC_RW_TIMEOUT_MS(len)` = 2 s + 50 ms per sector,
  so a stick that stops answering fails a transfer in seconds, not minutes.
