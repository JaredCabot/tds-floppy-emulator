# Firmware Update from a USB Stick

Status 2026-09-24: working on hardware (1.0.1 -> 1.0.2 -> 1.1.0 installed from
the stick, 5 s each; bad files rejected; interrupted installs repaired).

## Using it
1. Put `UPDATE.UPD` (made by `make`: `firmware/build/UPDATE.UPD`) in the root of
   a USB stick and insert it in the emulator.
2. Hold **both buttons together for 3 seconds**. The red LED goes **solid**.
3. Release. The LED flashes while the file is checked and copied (~2 s), the
   emulator restarts, the bootloader installs the new firmware (LED blinking,
   ~1 s) and starts it. Total ~5 s. The internal disk is not touched.

| Outcome | LED |
|---|---|
| Updated | Flashing, then the restart |
| No `UPDATE.UPD` on the stick | 2 quick blinks, nothing changed |
| Corrupt / truncated file, other hardware, or scope busy | 6 quick blinks, nothing changed |
| Stick format not supported (e.g. NTFS) | 3 slow blinks, nothing changed |

Releasing before the LED goes solid does nothing (and never triggers DATA IN /
DATA OUT: single-button actions happen on release, and only if the other
button was not pressed). Pressing both buttons again with the same file on the
stick simply re-installs the same firmware.

## Design
Internal flash (128 KB, **128 sectors of 1 KB**, RM_AT32F415 5.1):
```
0x08000000  bootloader  8 KB   boot/boot.c, installed once over SWD
0x08002000  application 120 KB this firmware (ld/app.ld), max image 52 KB
```
SPI flash staging area: `0x1F2000`-`0x1FEFFF` (52 KB) between the 166 track
slots and the self-test sector: header at `0x1F2000`, image at `0x1F2100`.

**UPDATE.UPD** = 48-byte header + application binary (padded to 4 bytes),
`include/update.h`: magic `TDSFLUPD`, board `SFRC2D.B`, format 1, version,
length, CRC-32 of the image, CRC-32 of the header. `tools/mkupdate.py` builds
it and refuses images that are too big or not linked at `0x08002000`.

**Application side** (`xfer_update`, both buttons 3 s):
1. Header checks (magic, board, format, header CRC, length, file size).
2. Waits for the scope to leave the drive alone (as for every transfer), ejects.
3. Pass 1 over the file: image CRC and vector-table sanity. **Nothing is
   touched until the whole file checks out.**
4. Erase the staging area (this also withdraws any older record), write the
   image, read it back and check the CRC.
5. Write the header last: the **commit record**. Restart.

**Bootloader** (every reset, 3 KB, 8 MHz reset clock, no interrupts, no USB):
if a valid record exists and the installed app differs from the staged image,
check the staged CRC, then erase and program the app (the **vector table is
written last**, so a half-written app is never started), verify, retry up to
3 times. Then start the app if its vector table is sane; otherwise blink a fast
double-flash forever (recover over SWD).

**Acknowledge:** at startup the application withdraws the record once the
installed app *is* the staged image. So the record lives exactly until the new
firmware has started once: long enough to repair an interrupted install, and
it cannot later "restore" the staged image over a newer SWD-flashed build.

## Integrity, not authenticity
The header CRC, the image CRC and the vector check prove that a file is intact
and built for this board. They do not prove who built it: the file is not
signed, so anyone with a USB stick and physical access can install any image
built for SFRC2D.B (as they could over SWD). That is a deliberate choice for a
hobby instrument accessory, where anyone may build their own firmware; a
signature would need a key only the publisher holds.

## Failure cases (tested)
| Case | Result |
|---|---|
| No file | `NOTHING` (2 blinks) |
| 1 bit flipped in the image | rejected in pass 1 (image CRC), nothing touched |
| Header for another board (valid header CRC) | rejected |
| Truncated file | rejected (size) |
| Power lost early in an install (vector table + 4 KB erased) | bootloader reinstalls at the next boot |
| App body damaged, vector table intact | bootloader's full compare catches it, reinstalls |
| Install failed partway (the 2 KB/1 KB sector bug, below) | once the bootloader was fixed, it completed the install by itself |

## Lessons (for anyone changing this)
- **Sector size is 1 KB** on the 128 KB AT32F415. OpenOCD's `flash info`
  reports 2 KB for it. Erasing in 2 KB steps erased every other sector.
- The Artery flash driver **never clears the status error flags** and reports
  any set flag as the result of the current operation: clear them before each
  erase/program (`flash_flags_clear` in boot.c).
- OpenOCD's `verify_image` runs a checksum routine in the target's RAM and
  corrupts a running application: reset afterwards (or compare a
  `dump_image` on the PC instead).

## Size budget
The staging area limits the application to 52,992 bytes (1.1.0: 38.9 KB, 73%).
Hard real-time code (floppy, mfm, buffer, spiflash) is compiled `-O2`, the
rest `-Os`. If it ever outgrows the area, the area could take the slots of the
spare format cylinders 81-82, which the TDS never writes.

## Development
`make` builds `boot.elf`, `tdsfloppy.elf` and `UPDATE.UPD`; `make flash-swd`
programs bootloader + app. Bump `FW_VERSION` in `include/version.h` for each
release; the running version is `dbg_fw_version` (SWD). Bench tools:
`tools/stick.ps1 put|get` moves files to/from the stick in the emulator over SWD;
`dbg_button = 3` triggers an update like the buttons.
