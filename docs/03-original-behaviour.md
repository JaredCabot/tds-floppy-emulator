# Original Firmware Behaviour (the spec to clone)

Derived from the instructions file (`Instructions/Gotek Floppy Instructions.txt`)
and the independent teardown at blog.mbirth.uk. This is the closed factory
firmware for the SFR1M44-DU26 in its "buffer" mode - NOT FlashFloppy, NOT the
image-selector emulators. Our firmware must reproduce this behaviour exactly, then
improve only USB robustness.

## Mental model
Two independent transfer channels meet at a single 1.44 MB buffer:
- **Red LED = USB stick <-> internal buffer**
- **Green LED = host (scope) <-> internal buffer**

The buffer is a FAT12 1.44 MB floppy image held in the SST25VF016B SPI NOR.
The host always sees a normal 1.44 MB floppy (fd0) backed by that buffer.

## State machine (as observed)

### On USB stick insert
1. LED flashes red briefly.
2. Firmware reads the **first 1.44 MB** of the stick's file list into the buffer
   (FAT12 image built from the stick's root files, at floppy speed).
3. Any previous buffer contents are erased by the insert.
4. LED goes out. Host can now read those files from fd0.

### Read direction (USB -> scope), "next page"
- Right button (PC8) copies the **next 1.44 MB** of the stick's file list into
  the buffer, erasing the previous page. LED red during copy.
- Host must re-select fd0 afterwards to refresh, or it throws disk errors.
- Buffer contents persist after the stick is removed.
- Re-inserting the stick restarts paging from the first page.

### Write direction (scope -> USB), "write mode"
- Left button (PC7) toggles WRITE mode. Solid red LED = WRITE mode on.
- In WRITE mode, files the host writes to fd0 are copied straight through to the
  USB stick (at floppy speed). LED green during host access, red when committing.
- Exceeding 1.44 MB per session -> host sees a standard "disk full" error.
- Toggling WRITE mode off then on resets the buffer for another <=1.44 MB batch.
- Files written to the buffer with **no** stick present cannot later be flushed
  to a stick; they live only in the buffer.

### Host access (always)
- Green LED lights whenever the host reads/writes the buffer over the floppy bus.
- After any stick insert/removal, host must re-select fd0 to refresh the display.

## Error behaviour to preserve
- No specified file / empty stick: original shows an error code on 7-seg models;
  our DU26 has only the LED, so mirror with an LED error blink (define in impl).
- Buffer/flash fault: original E16. Mirror with a distinct LED pattern.

## What the host (floppy side) requires - the hard-real-time part
The firmware must be a *correct floppy drive* on P1 at all times, regardless of
what the USB side is doing:
- Assert TRK0 at track 0, honour STEP/DIR to move the head position.
- Generate INDEX pulses (300 RPM -> 5 Hz, 200 ms period).
- Emit MFM RDATA for the selected track/side on read.
- Capture MFM WDATA when WGATE asserted, decode, write into buffer image.
- READY / DSKCHG / WPROT per floppy convention and buffer state.
- This timing cannot stall while USB enumeration or flash writes happen - hence
  the track-cache design in 04-architecture.md.

## Explicitly out of scope for the clone (YAGNI)
- Image selection UI, OLED menus, multi-image libraries (that's FlashFloppy).
- Any config file. Original has none; we add none until asked.
- 720 KB / 2.88 MB geometries. Original DU26 for TDS scope use is 1.44 MB only.

## Where this firmware differs from the stock unit (decided 2026-09-24)
| Stock | This firmware | Why |
|---|---|---|
| Inserting a stick auto-loads its files into the buffer | Nothing happens until a button is pressed | Auto-load wiped files saved while no stick was present |
| Left button toggles WRITE mode; the second press copies files written since | Left button copies out **every** file on the internal disk, verifies each on the stick, then erases the internal disk ("take the disk out") | Stock could not copy out existing contents; mirrors the right button ("put the next disk in") |
| Same-named files on the stick are overwritten | Never overwritten: `NAME_1.EXT`, `NAME_2.EXT`, ... | Keep every copy; the stick holds the canonical files |
| Red LED solid during transfers | Red LED flashes once every 0.25 s during transfers; distinct error / nothing-to-load blinks | Requested; visible even while the copy blocks |
| (unknown) | Right button always replaces the whole internal disk (no merge) | Matches the "disk swap" model; kept deliberately for now |
Unchanged: the right button pages through the stick's root files alphabetically;
the green LED shows the scope's drive accesses (it is hard-wired).
