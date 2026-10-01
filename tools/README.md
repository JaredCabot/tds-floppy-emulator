# Tools

Bench, test and analysis tools used during development. All locate the project
relative to themselves. The TDS is expected at `GPIB0::1::INSTR` (NI-VISA +
`pyvisa`), the ST-Link via OpenOCD (`interface/stlink.cfg`,
`target/artery/at32f4x.cfg`, on PATH), the Siglent SDS2504X HD on USB-VISA.
Output files go to `captures/` (not in the repository).

**One tool at a time.** Tools that make the scope write or change the emulator's
disk (stress_save, save_both, format_test, write_test, verify_disk, button in/out,
reformat) take `captures/bench.lock` (`benchlock.py`) and refuse to start while
another live tool holds it: two of them at once corrupted the disk and hung the
scope once. `locktest.py N` holds the lock for N s (to test it).

**Firmware must match the build.** The SWD tools look up variables by address
in `firmware/build/tdsfloppy.elf`; they refuse to run unless `build/tdsfloppy.bin` equals
`build/flashed.bin` (written by `make flash-swd`). Rebuilt but not flashed ->
flash first.

**OpenOCD notes.** `verify_image` runs a checksum routine in the target's RAM
and corrupts a running application: reset afterwards (or compare a
`dump_image` on the PC). OpenOCD's `flash info` shows 2 KB sectors for the
AT32F415RB; they are 1 KB (docs/13). Desktop Commander refuses shell lines that
start with `init`, `halt` or `reg`: put OpenOCD commands in a `-f` script.

**Avoid `FILESystem:WRITEFile` on the TDS 794D** - it hung the scope
(the manual lists it for the 500C/700C only). Use `SAVe:SETUp` /
`SAVe:WAVEform` to make the scope write files.

## emulator over SWD (PowerShell, OpenOCD)
| Tool | What |
|---|---|
| `fdstat.ps1 [-Steps N]` | Floppy status: uptime, INDEX/steps/selects, current track, load/store times, write counters, self-check counters (must be 0), track load/store trace, step log |
| `button.ps1 in\|out\|both\|status` | Press the DATA IN / DATA OUT / both buttons (update, or the status report) over SWD and report the result with its time; after an update it detects the restart, identifies the installed image and keeps the SWD tools' guard truthful, split into waiting, USB and internal flash; `status` shows version, build ID, density, stick-ready and the last transfer |
| `gatelog.ps1` | First 64 write gates (track, ID/data fields decoded, visible) and first 64 steps since reset - used to debug formatting |
| `reformat.ps1` | Erase the internal disk to a blank FAT12 volume and restart the firmware |
| `tryread.ps1 [-Poke "..."]` | Raise DISK CHANGE (so the TDS re-reads the disk) and make the TDS access fd0:; optional extra OpenOCD pokes |
| `busmon.cfg` | OpenOCD script: sample the floppy input pins for 60 s, log changes |
| `flashdump.py [start_kb] [len_kb]` | Dump the SPI flash through the firmware's SWD window (whole chip ~66 s) |
| `benchlock.py` | Shared bench lock (see above); `locktest.py N` holds it for testing |
| `stick.ps1 put FILE NAME` / `get NAME FILE` | Put a PC file onto / get a file from the USB stick in the emulator, through the firmware's SWD hook (the floppy keeps running). Used to place `UPDATE.UPD` for unattended update tests |
| `mkupdate.py BIN VERSION_H OUT` | Build `UPDATE.UPD` (run by `make`) |
| `identify_image.py DUMP` | Which firmware is in a dump of the application area: the current build, a released version (from the git tags), or unrecognised. Used by `button.ps1 both` after an update restarts the emulator |
| `check_image_end.py ELF BIN` | Run by `make`: fails the build if the image length the firmware computes its build ID over differs from the `.bin` |
| `release_refresh.py` | Runs `make` and `make test` (stops on failure), then copies `tdsfloppy_install.hex` into `release/`, verify its bootloader and firmware regions against the build, and update the version, build ID, size and SHA-256 in `release/README.md` |
| `dirtest.py make` / `check` / `again` | DATA OUT with folders: the scope makes folders on fd0: and saves into them; after DATA OUT every file must be on the stick in its folder, byte for byte; `again` checks an identical re-save makes no `_1` duplicate |
| `testset.py write E:` / `check` | The hands-on USB test: put 10 test files (zero-length, odd sizes, long / lower-case names, two pages, skip cases) on a stick, then after each DATA IN press check every file on the emulator's disk (docs/11) |

## The TDS over GPIB (Python)
| Tool | What |
|---|---|
| `tds.py CMD...` | Send commands / queries, print replies with timing, then *ESR? and ALLEV? |
| `readfile.py PATH... [--save]` | Timed `FILESystem:READFile`; `--save` stores the files |
| `save_both.py NAME [--wave NAME]` | Make the TDS save the setup (and CH1) to its CF card (hd0:) and to fd0: |
| `stress_save.py N [--wave] [--prefix P]` | N such saves in a row (stress test) |
| `format_test.py [--no-save]` | `FILESystem:FORMat "fd0:"`, then directory, free space and a save |
| `write_test.py [wave]` | Save, read back, restart the emulator, read back again from the disk |
| `verify_testbin.py` | Read TEST.BIN (test image) through the TDS and compare with its pattern |
| `verify_disk.py [--no-dump]` | Dump the SPI flash, rebuild the image, compare every file with the scope's hd0: copy - the main end-to-end check |

## Oscilloscope captures (Siglent SDS2504X HD, Python + numpy)
| Tool | What |
|---|---|
| `sds_capture.py` | One revolution of RDATA/INDEX/PA7; decodes the MFM off the wire (IDs, data CRCs, gap histogram) |
| `sds_seq.py TAG [--arm/--fetch] [--p2/--p3]` | Multi-second capture of the TDS's access sequence (STEP/SEL/SIDE1/INDEX, or MOTOR/DSKCHG, or RDATA/INDEX/READY), timeline printout - used to compare against a real drive |
| `sds_levels.py` | Static levels on the floppy lines during an access |

## Analysis
| Tool | What |
|---|---|
| `fatinfo.py IMG [IMG2]` | Decode / compare FAT12 boot sectors and root directories |
| `dis68k.py SYMBOL...` | Disassemble TDS 700D firmware functions by name (Capstone m68k, using the firmware's own VxWorks symbol table); see docs/12 |
