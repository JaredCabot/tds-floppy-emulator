# Changelog

All notable changes to the TDS Floppy Emulator firmware. Versions follow
[semantic versioning](https://semver.org/); every change released after 1.0.0
gets a new version (docs/06). The build ID of each release is in
[release/README.md](release/README.md); downloads are on the
[Releases page](https://github.com/JaredCabot/tds-floppy-emulator/releases).

## [1.2.0] - 2026-09-29

### Added
- **Status report.** Holding both buttons with no `UPDATE.UPD` on the flash
  drive writes `EMUSTAT.TXT` instead: firmware version and build ID, the
  unit's unique serial number and clock source, the internal disk's density
  and contents, health counters, the flash drive's file system, and the last
  transfer.

### Changed
- **Faster transfers.** A full 1.44 MB DATA IN takes about 15 s instead of
  19 s; DATA OUT about a third less time (1 MB written and read back in
  6.2-6.9 s instead of 10.6 s). The internal flash driver was the bottleneck
  and is now much leaner; USB data also moves in chunks of up to 8 KB. All
  verification and cancelling work as before.
- **Faster track loads:** the host waits 11 ms for a new track instead of
  27 ms.
- **Smaller firmware:** built with link-time optimisation, 44 KB instead of
  49 KB, leaving room for future features. It also makes transfers a little
  faster: a full 1.44 MB DATA IN in about 15 s.
- With no `UPDATE.UPD`, both buttons no longer give 2 quick blinks: they write
  the status report.
- **Interface inputs no longer use the MCU's internal pull-ups.** The board
  already has pull-up resistors to 5 V on every host line, and the datasheet
  requires internal pull-ups off on inputs driven above 3.6 V.
- **Crystal fallback:** if the 8 MHz crystal fails to start, the emulator now
  runs from the internal oscillator instead of stopping, and the status report
  says so.

### Fixed
- Reads and writes beyond the end of the disk (sector 1440 on a 720 KB disk,
  2880 on a 1.44 MB disk) are now ignored instead of reaching other parts of
  the internal flash.

### Removed
- The unused debug serial output.

### Documentation
- **New general user manual, TFE-0002-00**, for any equipment with a 26-pin
  slim floppy drive, with a technical reference (interface, timing, disk and
  update file formats).
- **TDS manual revised to TFE-0001-01:** 720 KB disks and jumper JE, the
  status report, 1.2.0 transfer rates, the TDS 784D as a verified
  instrument, a new Figure 2-1 showing all jumpers, headers and controls, and
  Figure 2-2 showing the S1 jumper fitted. Jumper JD is now described
  correctly: it holds interface pin 11 (density select) low and is not used.
- **Hardware notes** (docs/01): measured on a TDS 794D, the host drives SEL,
  STEP and DIR high enough to lift the board's 3.3 V rail to 3.8 V through
  the MCU's protection diodes; the fix is a series resistor on each line.
  Any firmware is affected, including the original.

## [1.1.0] - 2026-09-27

### Added
- **720 KB double-density (DD) disks.** The internal disk has a density, as a
  real floppy does: 1.44 MB HD or 720 KB DD. It changes the way a real disk's
  does, when the host formats it at the other density: fit jumper JE (the
  host's density line) and format the disk in the scope, and it becomes a
  720 KB disk; remove JE and format again, and it is 1.44 MB again. No settings
  file, no reboot. The emulator recognises a format at the other density from
  the data rate of the host's writes (250 or 500 kbit/s). The density survives
  power-off, firmware updates, DATA IN and DATA OUT.
- DATA IN and DATA OUT on 720 KB disks: 1 KB clusters, 112 root entries,
  files up to 730,112 bytes; a DATA IN page holds what fits on the disk.
- `tools/button.ps1 status` shows the disk's density; `tools/release_refresh.py`
  also publishes `UPDATE.UPD`.

### Changed
- The disk and FAT code take their geometry from the disk (HD or DD) instead of
  fixed 1.44 MB constants. 1.44 MB behaviour is unchanged.

### Notes
- **Downgrading:** firmware 1.0.0 does not know 720 KB disks. After going back
  to 1.0.0, a 720 KB disk reads as unformatted (its data is kept): update to
  1.1.0 again, or format the disk at 1.44 MB.
- Tested on a TDS 784D (firmware v7.4e): HD to DD and back by the jumper and
  the scope's format only, saves, a folder tree, DATA OUT and DATA IN (up to a
  716,800-byte file), 24 stress saves at 720 KB with 0 bad CRCs and 0 lost
  sectors; and an update from 1.0.0 on both test units.

## [1.0.0] - 2026-09-27

First release.

- Clean-room firmware for the SFR1M44-DU26 (PCB SFRC2D.B, Artery AT32F415)
  emulating the 1.44 MB floppy disk drive of Tektronix TDS 500/600/700
  oscilloscopes.
- Internal 1.44 MB disk in SPI flash, written back through a power-safe journal:
  a power cut loses at most the write in progress.
- DATA IN (lower button): load the next batch of files from a USB flash drive
  (FAT12/16/32 or exFAT; MBR or GPT), every sector verified.
- DATA OUT (upper button): copy everything on the internal disk, folders
  included, to the flash drive, verify each file, never overwrite, then erase
  the internal disk. Identical files already on the flash drive are not copied
  twice.
- Cancel a transfer with its own button; firmware update from a USB flash drive
  with both buttons (a bootloader installs it; a bad file changes nothing).
- Watchdog, persistent flash-fault indication, build ID; the user manual
  TFE-0001-00 in the Tektronix house style.

[1.2.0]: https://github.com/JaredCabot/tds-floppy-emulator/releases/tag/v1.2.0
[1.1.0]: https://github.com/JaredCabot/tds-floppy-emulator/releases/tag/v1.1.0
[1.0.0]: https://github.com/JaredCabot/tds-floppy-emulator/releases/tag/v1.0.0
