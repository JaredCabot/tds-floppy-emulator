# TDS Floppy Emulator Firmware

Clean-room replacement firmware for the SFR1M44-DU26 USB floppy emulator
(Artery **AT32F415**, PCB SFRC2D.B, 26-pin slim floppy connector) as fitted to
Tektronix TDS 500/600/700-series oscilloscopes. These units are commonly sold
under the Gotek name; this is an independent project, not affiliated with or
endorsed by that brand or the original firmware's authors, and it contains none
of their code.

It reproduces the stock "buffer" workflow (the scope sees a 1.44 MB floppy
whose contents live in the unit's SPI flash, and a USB stick moves files in and
out) with better reliability and a few improvements. Written from the
observable behaviour, the drive specification and bench measurements.
**Status:** working on a **TDS 794D** (firmware v8.0e) and a **TDS 784D**
(firmware v7.4e), each with its own emulator. Read, write, format, USB
transfers, the buttons and LED, the first installation on a factory unit and
the update from an earlier version are verified on the hardware: every file
saved by the scope reads back byte-identical, through the internal disk and a
USB round trip.

## Manual and firmware

- **Manual:** [TFE-0001-00 Floppy Disk Drive Emulator Instructions](docs/manual/TFE-0001-00_TDS_Floppy_Emulator_Instructions.pdf)
  (PDF), in the style of the Tektronix TDS manuals. Covers loading the
  firmware, fitting the emulator in the instrument, the buttons and
  indicators, USB flash drives, firmware updates, troubleshooting and
  specifications.
- **Firmware, first installation:** [release/tdsfloppy_install.hex](release/tdsfloppy_install.hex)
  (version 1.0.0, bootloader and firmware in one file; checksum in
  [release/README.md](release/README.md)). Program it once over USB with the
  Artery ISP Programmer, archived in [third-party/](third-party/), as
  described in the manual (*Load the Firmware*, page 2-1). Later versions
  install from a USB flash drive.

## Using it
The scope sees `fd0:` as an ordinary 1.44 MB floppy: read, save, format.

| Control | Action |
|---|---|
| **Right (lower as installed) button** - DATA IN, *put a disk in* | Replaces the internal disk with the next batch of files from the root of the USB stick (alphabetical, as many as fit in 1.44 MB; each sector verified as it is written). Press again for the next batch; wraps to the first after the last. Skips folders, hidden/system files and files over 1.44 MB. Long names appear on the scope as 8.3 short names (`TEK000~1.BMP`). |
| **Left (upper as installed) button** - DATA OUT, *take the disk out* | Copies every file on the internal disk to the stick, keeping the original dates and any folders the scope made. Never overwrites: a clash is saved as `NAME_1.EXT`, `NAME_2.EXT`, ...; a file already there and identical is not copied twice. Each file is read back and compared; only if all are safely on the stick is the internal disk erased, blank for new data. Any failure leaves it untouched. |
| **The same button again, while the LED flashes** - *cancel* | Stops the transfer at the next safe point; the LED gives one long blink. DATA OUT: any partly copied file is deleted, files already copied stay on the stick, the internal disk is not erased. DATA IN: the disk keeps the files loaded so far and the next press loads the same batch again (the disk's previous contents are gone: DATA IN erases it first). A firmware update cannot be cancelled. |
| Inserting a stick | Does nothing until a button is pressed. Sticks may be FAT12/16/32 or exFAT, MBR or GPT partitioned; on exFAT, long names get Windows-style 8.3 names (`SCOPEC~1.BMP`). |
| **Both buttons held 3 s** - *firmware update* | Red LED goes solid; release to install `UPDATE.UPD` from the stick (~5 s, safe against power loss). See [docs/13](docs/13-firmware-update.md). |
| Pressing a button while the scope is saving | Waits (LED flashing) until the scope has finished with the drive, then copies. Refused with the error blink if it is still busy after 15 s. |

| LED | Meaning |
|---|---|
| Red, off | Idle |
| Red, flashing once every 0.25 s | Copying files (either direction) - don't remove the stick |
| Red, 6 quick blinks | Error (no stick / USB error / verify failed / unreadable disk / scope busy for 15 s); internal disk unchanged |
| Red, 2 quick blinks | Right button: nothing on the stick to load; internal disk unchanged |
| Red, 3 slow blinks | Stick format not supported (NTFS, unformatted, or unusual sector size): reformat it as exFAT or FAT32. Internal disk unchanged |
| Red, one long blink (1 s) | Transfer cancelled: the button that started it was pressed again while the LED was flashing |
| Red, fast continuous | The internal SPI flash has failed (self-test at power-up, or a track could not be stored). Kept after power-off until the disk is rebuilt: save the files with DATA OUT. If the self-test fails, the unit shows no disk |
| Red, solid | Both buttons held long enough: release to update the firmware |
| Red, fast double blink, repeating | Bootloader: no runnable firmware; load the firmware again (manual, page 2-1) |
| Green (if fitted) | Hard-wired to the scope's drive-select line: lit while the scope accesses the drive. Not fitted on some units |

Jumpers as for the stock unit in a TDS: **S1 only** (drive select); MO (the
other position of the same three-pin header), JE and JD off. Details: the
manual (*Set the Jumper*, Table 2-2) and [docs/01](docs/01-hardware.md);
buttons, LED and transfers: [docs/11-usb.md](docs/11-usb.md).

## Improvements over the stock firmware
- The left button can copy out **everything** on the internal disk (stock could
  not copy out data on internal memory that was saved before the USB was
  inserted), verifies it on the stick and never overwrites stick files.
  Folders the scope made are copied too, and files already on the stick are
  recognised rather than copied twice.
- Nothing happens on stick insertion (stock auto-loaded, wiping the disk).
- Power-safe storage: every track goes through a journal before it replaces
  the old one, so a power cut loses at most the write in progress, never the
  rest of the disk (proven at every step in a simulated-power-cut test). The
  head never waits for flash on a track change.
- Every sector loaded from the stick is verified as it is written.
- A transfer can be cancelled with its own button.
- A button press waits for the scope to finish with the drive instead of
  pulling the disk from under a save.
- exFAT and GPT-partitioned sticks work; an unsupported stick (NTFS) is
  recognised and reported.
- A watchdog recovers from any hang; a failing flash is reported and the
  warning survives power-off.

## Building
Needs Arm GNU Toolchain (arm-none-eabi-gcc 14.2 used), GNU make, and for the
host unit tests a native gcc (MinGW on Windows). OpenOCD (xPack, with the
`artery` flash driver) and an ST-Link for SWD flashing.
```
cd firmware
make            # -> build/tdsfloppy.elf / .bin / .hex, UPDATE.UPD, tdsfloppy_install.hex
make test       # host unit tests: MFM, write decoder, FAT12 builder/reader and
                # folder walk, buttons, and the write-back journal against a
                # simulated flash that loses power at every step
make flash-swd  # program bootloader + app over SWD (ST-Link), verify, run
```
`make` also writes `build/UPDATE.UPD`: after the first flash, updates only
need a USB stick and the two buttons (docs/13). It prints the build ID, which
a running unit reports over SWD (`tools/button.ps1 status`); after a release,
every change gets a new version number (docs/06).
A factory unit is read-protected: the **first** flash must go through the
AT32's USB-DFU bootloader with Artery ISP Programmer (a USB-A-to-A cable and the
J3 BOOT0 jumper), which also removes the protection. Program
`build/tdsfloppy_install.hex` (bootloader + firmware; the released copy is
`release/tdsfloppy_install.hex`). After that, SWD works.
See [docs/08](docs/08-unlock-and-flash-via-isp.md) and
[docs/06](docs/06-build-and-flash.md).

## Repository layout
```
firmware/
  src/, include/   our code (see docs/04 for the module map)
  test/            host unit tests (make test)
  vendor/          Artery AT32F415 library subset + FatFs (see vendor/README.md)
  Makefile
docs/              design notes, measurements and findings (index below)
docs/manual/       the user manual (PDF)
release/           firmware for the first installation (tdsfloppy_install.hex)
Bracket/           3D-printable adapter bracket (STL, STEP), for instruments without one
third-party/       Artery ISP Programmer, archived (not our work; see its README)
tools/             bench and test tools (see tools/README.md)
```
Not in the repository (third-party or bench data; obtain separately):
Artery firmware library zip (arterychip.com), AT32F415 /
SST25VF016B / TEAC FD-05HF datasheets, Tektronix TDS manuals and firmware
images, the SFRC2D.B schematic
([PDF](https://github.com/liveboxandy/Gotek-SFRC2DB/blob/main/Gotek.pdf), by LiveBoxAndy),
and bench captures.

## Documentation
| Doc | Contents |
|---|---|
| [01-hardware](docs/01-hardware.md) | Board, parts, headers, LED wiring (red is active-low) |
| [02-pinmap.csv](docs/02-pinmap.csv) | Authoritative MCU pin -> net map |
| [03-original-behaviour](docs/03-original-behaviour.md) | The stock behaviour, and where this firmware differs |
| [04-architecture](docs/04-architecture.md) | Modules, data flow, interrupts, RAM |
| [05-firmware-extraction](docs/05-firmware-extraction.md) | Why the factory binary can't be read (FAP) |
| [06-build-and-flash](docs/06-build-and-flash.md) | Toolchain, build, SWD wiring and flashing |
| [07-floppy-format](docs/07-floppy-format.md) | 1.44 MB IBM MFM track format and bus signals |
| [08-unlock-and-flash-via-isp](docs/08-unlock-and-flash-via-isp.md) | One-time unlock + first flash over USB-DFU |
| [09-fdd-interface-spec](docs/09-fdd-interface-spec.md) | Drive behaviour per the TEAC spec and a real drive; why the TDS rejected early versions |
| [10-read-write-path](docs/10-read-write-path.md) | Track buffers, on-the-fly MFM, write capture, background write-back, formatting |
| [11-usb](docs/11-usb.md) | USB host, the buttons, LEDs, transfers |
| [12-scope-floppy-driver](docs/12-scope-floppy-driver.md) | TDS firmware analysis: supported disk sizes (1.44 MB max) |
| [13-firmware-update](docs/13-firmware-update.md) | USB-stick firmware update: bootloader, file format, failure handling |

## Credits
- SFRC2D.B schematic and PCB by LiveBoxAndy: [schematic (PDF)](https://github.com/liveboxandy/Gotek-SFRC2DB/blob/main/Gotek.pdf),
  [KiCad project, Gerbers and BOM](https://github.com/liveboxandy/Gotek-SFRC2DB)
- [FlashFloppy](https://github.com/keirf/flashfloppy) (public domain) - used as a
  reference for drive behaviour and for the JTAG/PB3 lesson
- Artery AT32F415 Firmware Library (USB host, MSC, drivers) and FatFs by ChaN

## Licence
MIT - see [LICENSE](LICENSE). This covers the project's own code, docs and
tools. Third-party code under `firmware/vendor/` keeps its own licences (Artery
library notice, FatFs BSD-style; see `firmware/vendor/README.md`).
