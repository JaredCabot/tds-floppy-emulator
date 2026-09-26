# Extracting the Original Firmware (optional reference only)

We are doing a clean-room clone of observable behaviour, so the original binary
is NOT needed. This note exists only if you want a dump to diff against. If you
extract it, keep it read-only reference and do not paste its code into the clone.

## Read protection on the AT32F415
Artery's read-out protection is called **Access Protection (FAP)** - the analogue
of ST's RDP level 1. When set (the factory almost certainly sets it), SWD/JTAG
and the serial/USB bootloader all refuse to read flash; disabling it triggers a
mass erase. So you cannot just connect a probe and read it out.

## OBSERVED on this board (2026-09-22) - the factory chip IS protected
Connected a genuine ST-Link/V2 via SWD (OpenOCD 0.12 + target/artery/at32f4x.cfg).
Results:
- Chip positively identified: Device ID 0x700301c4 = **AT32F415RBT7**, 128 KiB flash.
- `FLASH_OBR` (0x4002201C) = 0x07FFFFFE -> **bit1 RDPRT = 1 (access protection ON)**.
- `FLASH_EPPS` (0x40022020) = 0xFFFFFFFF (no per-sector erase/program protection).
- Any erase/program over SWD -> "Sector 0 is write protected / failed erasing".
- Tried to relieve FAP three ways, ALL blocked:
  1. artery driver `artery fap disable 0` -> no error but USD unchanged after reload.
  2. `stm32f1x unlock 0` -> driver refuses: "Cannot identify target as a STM32
     family" (won't act on the AT32 device ID).
  3. Manual USD erase via flash-controller registers (unlock 0x04/0x08 keys ok,
     CTRL 0x080->0x200 confirming oplk cleared + usdulks set) -> the USD ERASE
     step sets `FLASH_STS` bit4 **epperr** = the hardware refuses to erase the
     User System Data over the debug interface. Correct 0x5AA5 (nFAP:FAP) value
     doesn't matter because the erase never completes.
- Conclusion: **SWD/OpenOCD cannot recover this chip.** The USD is erase-locked
  from the debugger (high-level-access-protection signature). This is NOT a bug
  in the sequence; it is the protection working as designed against debug access.

## The path that works: the on-chip bootloader (ISP), not SWD
The bootloader runs from system memory with privileged flash access the external
debugger lacks. Its "disable access protection" + "disable erase/program
protection" commands relieve FAP+EPP from inside the chip (with mass-erase). The
FlashFloppy community unlocks these exact AT32F415 emulators this way routinely.
See docs/08-unlock-and-flash-via-isp.md for the step-by-step.
- F415 high-level protection is the REVERSIBLE variant in the AT32F415 manuals
  (the irreversible warning applies to F425/L021/F423/A423/F402/F405/... NOT
  F415). So the bootloader relieve is expected to succeed.
- Caveat: if this specific part were set to the truly-irreversible mode, even the
  bootloader refuses and the only fix is a fresh AT32F415RBT7. Unknown until tried.

## Realistic options for a DUMP (still optional - clone doesn't need it)
1. **Don't.** Clone from the spec (docs/03). This is the recommended path.
2. **Voltage/SWD glitch (RDP-1 bypass).** The Obermaier/Tatschner WOOT'17
   cold-boot-stepping attack and its descendants dump RDP-1 STM32F1 parts one
   word at a time by racing the debug interface after reset, controlling reset +
   power. Reference implementations:
   - CTXz/stm32f1-picopwner (Pi Pico, STM32F1 glitch+FPB) - closest analogue.
   - racerxdl/stm32f0-pico-dump (SWD race PoC).
   These target STM32; the AT32F415 is a pin/register-compatible Cortex-M4 clone,
   so the same class of attack is plausible but **not verified on Artery**. FAP
   may differ in detail. Expect to adapt timing and confirm on your own part.
3. **Bootloader command quirks.** Some vendor bootloaders leak memory-read via
   unintended commands; unknown for Artery ISP. Low confidence, cheap to probe.

## If you attempt option 2 - practical notes
- Wire SWDIO=PA13 (J10.4), SWCLK=PA14 (J10.3), NRST, and switch VDD via a
  MOSFET/relay so the attacker board can power-cycle the target each word.
- BOOT0 header (J3.1) selects the bootloader; useful for the bootloader-based
  variants of the attack.
- Success is per-part and finicky. Budget it as a research side-quest, not a
  blocker - the clone does not wait on it.

## Ethics/scope
Cloning behaviour of hardware you own for interoperability/repair is the intent
here. Keep the clone derived from the documented spec, not from a decompiled
factory binary.
