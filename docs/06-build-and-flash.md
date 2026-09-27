# Build & Flash

## Toolchain (installed)
- **Arm GNU Toolchain 14.2.Rel1** - `arm-none-eabi-gcc` etc.
  `C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin`
- **GNU make 4.4.1** (ezwinports) - on PATH as `make`.

If `make` / `arm-none-eabi-gcc` aren't found in a new shell, log out/in once (the
installers added them to PATH), or prepend that bin dir.

## Build
```
cd "D:\Documents\Projects\Gotek Floppy\firmware"
make            # -> build/tdsfloppy.elf, .bin, .hex  (+ size report)
make test       # host unit tests (native gcc, e.g. MinGW)
make flash-swd  # program + verify over SWD; records build/flashed.bin
                # (make also writes build/tdsfloppy_install.hex: bootloader + app for
                #  the first install over USB-DFU, docs/08, and build/UPDATE.UPD)
make clean
```
Current size: ~45 KB flash of 128 KB; RAM ~90 % of 32 KB including the reserved
stack/heap; measured stack headroom ~2.7 KB (see docs/04).

`flash-swd` copies the image it flashed to `build/flashed.bin`. The SWD tools
(tools/*.ps1, flashdump.py) refuse to run when `build/tdsfloppy.bin` differs from
it: after a rebuild without flashing, their symbol addresses would be wrong
(a "button press" once wrote to the wrong RAM location that way).

Host tests need a native gcc on PATH (MinGW-w64 / WinLibs gcc used). The bench
tools in `tools/` need Python 3 with `pyvisa` (+ NI-VISA/GPIB for the TDS),
`numpy` (Siglent captures), `pymupdf` (manual pages) and `capstone` (68k
disassembly of the scope firmware); see tools/README.md.

Key build facts (learned, don't relitigate):
- AT32F415 is Cortex-M4 **without FPU** -> `-mfloat-abi=soft`, no `-mfpu`.
- Part define `-DAT32F415RBT7` selects 128 KB-flash "xB" density + LQFP64.
- Linker script `AT32F415xB_FLASH.ld` (128 KB @ 0x08000000, 32 KB RAM).
- The `_close/_read/... not implemented` and `RWX segment` linker warnings are
  benign (newlib-nano nosys stubs, GC'd).

## First flash of a factory unit: USB-DFU (one time)
Factory units are read-protected (FAP), which SWD cannot remove. Flash the first
time through the AT32's ROM bootloader over USB-DFU with Artery ISP Programmer
(USB-A-to-A cable, J3 pins 1-2 jumpered for BOOT0), disabling the read
protection in the process: [docs/08](08-unlock-and-flash-via-isp.md). After that,
use SWD for everything.

(A UART route through the ROM bootloader also exists - PA9/PA10 on J3/J4 - but
was not needed; the USB-to-serial adapter tried turned out to be a counterfeit
that Windows refuses to drive.)

## Flash and debug: ST-Link V2 + SWD (installed, best for dev)
OpenOCD (xpack 0.12.0+dev) is installed and on PATH.

**Use the Artery driver, NOT stm32f1x.** The AT32F415 is an Artery part; modern
OpenOCD ships a dedicated `artery` flash driver and target config that knows the
AT32 flash layout and FAP. Using `target/stm32f1x.cfg` mis-detects the flash and
can fail or corrupt. Correct config: `target/artery/at32f4x.cfg`.

1. Wire the genuine ST-Link/V2 20-pin JTAG/SWD ribbon (UM1075 Table 4, SWD
   column) to the emulator. Only 3 wires are required; NRST is optional.
   J10 pinout (from PCB netlist): 1=GND, 2=5V, 3=SWCLK, 4=SWDIO.
   | ST-Link CN3 pin | Signal        | emulator                         |
   |-----------------|---------------|-------------------------------|
   | 7               | TMS_SWDIO     | SWDIO = PA13 (J10 pin 4)      |
   | 9               | TCK_SWCLK     | SWCLK = PA14 (J10 pin 3)      |
   | 4/6/8/20 (any)  | GND           | GND (J10 pin 1)               |
   | 1               | VAPP (SENSE)  | +3.3 V on J3 pin 2 - ref only |
   | 15              | NRST (opt.)   | NRST (J4 pin 2)               |
   WARNING: J10 pin 2 is the 5V rail, NOT 3.3 V. Do NOT connect ST-Link VAPP
   (pin 1) to J10 pin 2 - VAPP must sense the MCU's 3.3 V I/O level. The 3.3 V
   reference is J3 pin 2 (+3.3V). (5 V on VAPP would make the probe drive its
   SWD lines at the wrong level / risk the 3.6 V-max JTAG inputs.)
   POWER: the emulator runs from its OWN USB (or the scope). Do NOT power it from
   the ST-Link. Pin 1 (VAPP) is a voltage-SENSE input only, not a supply; pin 19
   (VDD 3.3 V) is "not connected" for SWD - leave it. Never source power into
   the board from the probe while it is USB/scope powered (two supplies fighting
   on one rail). Ribbon pin 1 is keyed; odd pins one row, even the other
   (UM1075 Fig 10).
   If you prefer to skip VAPP entirely: SWDIO + SWCLK + GND alone will work for
   flashing on a 5 V-tolerant target; VAPP just makes the probe match I/O level.
2. Sanity check the link (no flashing):
   ```
   make probe          # connects, halts, lists flash, resets, exits
   ```
   Expect a Cortex-M4 detected and an `at32f4x.flash` bank at 0x08000000.
3. Flash:
   ```
   make flash-swd      # program build/tdsfloppy.elf, verify, reset, run
   ```
4. Live debug:
   ```
   make debug-server   # leaves OpenOCD gdb server on :3333
   # in another shell:
   arm-none-eabi-gdb build/tdsfloppy.elf -ex "target extended-remote :3333"
   ```

If OpenOCD reports "open failed", the ST-Link isn't reachable: check it is
plugged in (Windows Device Manager, VID 0483) and bound to WinUSB (Zadig), and
that no other OpenOCD instance holds it.

> Note on FAP/read protection: if a board ever comes up read-protected, the
> artery driver exposes `artery fap disable <bank>` (mass-erases, then reset).
> Not needed for our own boards.

## Verify a build without hardware
The first 8 bytes of `tdsfloppy.bin` are the vector table: initial SP then reset PC.
A good build shows SP = 0x20008000 and reset PC in 0x08000xxx.

## SPI-flash self-test (every boot)
On boot the firmware runs `spiflash_selftest()` and reports on the UART:
- `spiflash: JEDEC ID = 0xBF2541 (ok)` - SST25VF016B detected on SPI2.
- `spiflash: erase/program/verify ok` - scratch sector (last 4 KB of the chip)
  erased to 0xFF, 16 bytes programmed and read back.

LED without a serial console: **red LED off = flash OK** (the old 1 Hz
heartbeat is gone; the red LED now only shows file activity), **~5 Hz
continuous blink = flash self-test FAILED** (bad JEDEC ID or verify mismatch - check SPI2 wiring /
CE# on PB12, or that U8 is really an SST25VF016B).

If JEDEC ID reads 0x000000 or 0xFFFFFF: MISO not returning data (PB14) or CE#
never asserting. If ID is right but verify fails: block-protect not cleared - 
confirm the EWSR+WRSR(0x00) in `spiflash_init()` ran (some SST clones need WP#
high; WP# is not on a header here, it's tied on the PCB).

Build note: the Makefile now uses `-MMD -MP` so editing a header (e.g.
`at32f415_conf.h` to enable a driver module) correctly rebuilds dependent
objects. Before this, a stale object could drop a driver and cause link errors.

## Versions and build ID

`include/version.h` holds the version (`FW_VERSION`, e.g. 1.0.0). **After a
release, every change that is flashed to a unit or published gets a new
version**: two different images must never share a number (during the
2026-09-26 tests two quite different builds both said 1.0.0).

The **build ID** identifies an exact image regardless of version: the CRC-32 of
the firmware image padded to 4 bytes with 0xFF, which is the `crc` that `make`
prints ("(= build ID)"), the image CRC inside `UPDATE.UPD`, and what the
firmware computes over itself at boot into `dbg_fw_build` (SWD). It is the same
whether the image was installed from `UPDATE.UPD` or over SWD, since unused
flash reads 0xFF. `tools/button.ps1 status` shows version, build ID and the
flash-fault flag. `release/README.md` lists the build ID of each release;
`tools/release_refresh.py` updates `release/` from a final build.

## Publishing a release

1. Set the new version in `include/version.h` (both `FW_VERSION` and
   `FW_VERSION_STR`) and add its entry to `CHANGELOG.md`.
2. `python tools/release_refresh.py`: builds, runs the host tests, copies
   `tdsfloppy_install.hex` and `UPDATE.UPD` into `release/` (checked against
   the build) and writes their checksums into `release/README.md`.
3. Commit and push `main`, tag it `vX.Y.Z`, and create the GitHub Release from
   the tag with the changelog entry as notes and both files (and the manual)
   attached: `gh release create vX.Y.Z release/tdsfloppy_install.hex
   release/UPDATE.UPD docs/manual/... --notes-file ...`.
4. Download the attached files anonymously and compare their SHA-256 with
   `release/README.md`.
