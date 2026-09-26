# Unlock + Flash via the on-chip Bootloader (Artery ISP)

The factory chip has access protection (FAP) set, and SWD/OpenOCD CANNOT relieve
it (docs/05 records the proof). The working route is Artery's ISP tool through
the chip's own bootloader, which disables read protection from inside the chip
(with a mass-erase) and then programs our firmware.

VERIFIED PROCEDURE SOURCE: HxC2001's step-by-step for an ARTERY emulator over a
USB-A-to-USB-A cable (screenshots): hxc2001.com/docs/gotek-floppy-emulator-hxc-
firmware/pages/flash-a-gotek-with-a-usb-cable.html#artery-usb-fw-installation.
Our steps below mirror it, substituting our own build/tdsfloppy_install.hex for their file.

## Hardware needed
- A USB-A to USB-A (male-male) cable. THAT IS ALL - no probe, no serial adapter,
  no other wiring.

## Get the tool (pinned version that the HxC guide uses)
1. Download `Artery_ISP_Programmer_V2.0.05.zip` from Artery:
   https://www.arterychip.com/download/TOOL/Artery_ISP_Programmer_V2.0.05.zip
   (mirror: hxc2001.com/download/floppy_drive_emulator/Artery_ISP_Programmer_V2.0.05.zip)
2. Unpack it. Run `Artery_DFU_DriverInstall.exe` to install the DFU USB driver.
   (Win10: no reboot needed.)
3. `ArteryISPProgrammer.exe` is the tool. Defender may warn "unrecognised app"
   -> More info -> Run anyway. If UI is Chinese, pick English on the lower tab.

## Enter the bootloader
1. Strap **BOOT0 high**: the rear jumper block the HxC guide calls "pins 1 and 2"
   IS the **J3 header** on this board. J3.1 = BOOT0, J3.2 = +3.3V (see docs/01),
   so jumpering J3 pins 1-2 pulls BOOT0 high (RN13 is a pulldown, so it must be
   actively strapped high). Jumper or wire, no soldering.
2. Connect the emulator to the PC with the USB-A-to-A cable (into P3). No other
   connection.
3. Windows should detect the DFU device (appears in Device Manager).
   (No manual power-cycle called out in the HxC guide - plugging in with BOOT0
   strapped enumerates it in DFU. If it doesn't appear, unplug/replug.)

## In ArteryISPProgrammer - exact click path (from HxC guide)
1. Port type: **USB DFU**. The emulator appears in the list -> select it -> **Next**.
2. **Next** (device info page).
3. **Next** again.
4. **Enable/Disable Protection** -> choose **DISABLE** and **Access Protection**
   (the label in V2.0.26; HxC's guide, written for an older version, says Read Protection)
   -> **Next**.
5. **Yes** to confirm. This relieves FAP and **mass-erases** the flash (the
   factory image is destroyed - expected; we can't read it and don't need it).
6. When the erase completes -> **Back**.
7. **Download to device** -> **Add** -> select **release/tdsfloppy_install.hex** (the
   released image; a developer can use **build/tdsfloppy_install.hex** from their own
   build) -> **Next**.
   (That file holds the bootloader AND the application. `build/tdsfloppy.hex` is the
   application alone, at 0x08002000: programmed on its own it leaves no bootloader
   and nothing runs. Later updates use UPDATE.UPD from a USB stick, docs/13.)
8. **Ok**. The emulator is programmed (and verified).
9. **Close** when done. Remove the BOOT0 strap. Power-cycle -> our firmware runs.

> Note: the HxC procedure only disables *read protection* (which mass-erases and
> is enough for a stock factory emulator). If the tool separately shows erase/program
> protection still set and blocks programming, disable that too (User system data
> / Protection -> uncheck all EPP-protected sectors -> Apply), then retry step 7.

## After this, SWD works normally
Once FAP is relieved, the ST-Link + OpenOCD path (docs/06 `make flash-swd`,
`make debug-server`) will work for all future flashing and debugging - the
protection only has to be cleared once (until/unless something re-enables it).
So: unlock once via ISP, then go back to the fast SWD loop for development.

## If DISABLE fails / device won't unlock
- Re-seat the BOOT0 strap and power-cycle; confirm the tool actually entered the
  bootloader (it must read the device ID before any unlock).
- Try the other transport (DFU vs UART).
- If the tool reports the part is high-level/permanently protected, this specific
  chip is locked for good -> replace with a fresh AT32F415RBT7 (LQFP64). Our
  firmware is chip-generic; a blank AT32F415RBT7 flashes with no unlock dance.

## Programmer to buy (for robustness + future SWD debug)
- **Artery AT-Link** (official, ~US$6-12) - native for ICP/ISP + SWD debug. Best.
- **Segger J-Link** (EDU Mini ~US$18) - works with Artery ICP and J-Flash
  (documented AT32 auto-unlock).
Not required if the ISP-over-DFU/UART route above succeeds.
