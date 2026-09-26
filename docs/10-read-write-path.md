# Floppy Read/Write Path and Disk Buffer

Status 2026-09-23: **read and write verified end-to-end in a TDS 794D.**
- Read: `tools/verify_testbin.py` - TEST.BIN (64 KB, cyl 0-4, both heads) read
  back through the TDS's floppy controller, 0 mismatches.
- Write: `tools/write_test.py wave` - the TDS saves a setup file and a waveform
  (`SAVe:SETUp` / `SAVe:WAVEform`, including overwrite) to fd0: and to its CF
  card; the fd0: copies are byte-identical, also after the emulator restarts and
  reloads everything from SPI flash. 105 write gates -> 105 sectors, 0 bad CRC.

## Disk buffer (buffer.c)
The 1.44 MB image lives in the SST25VF016B, one **12 KB-aligned slot per
track** (slot = cyl*2 + head; 162 slots for cylinders 0-80). A track is
18 x 512 = 9216 bytes, so storing a track = erase its 3 sectors + program: no
read-modify-write buffer in RAM.

| SPI flash | Holds |
|---|---|
| 0x000000 | 162 track slots, 12 KB each |
| 0x1E6000 | 3 journal slots, 12 KB each |
| 0x1EF000 | meta sector: the "complete image" marker `TDSFBUF2` |
| 0x1F2000 | firmware-update staging (docs/13) |
| 0x1FF000 | boot self-test scratch |

**Power-safe write-back (journal).** Erasing a slot destroys the old track
before the new one is in, and track 0 holds the boot sector and both FATs, so
a power cut at that moment used to be able to lose the whole disk (review
2026-09-25). Now each track is first written to the next of 3 journal slots
(rotating, which spreads the wear), read back, and committed by a 16-byte
header written last (magic, track, sequence number, CRC-32). Only then is the
track's own slot erased and programmed. At boot, `buffer_init()` takes the
newest committed entry and, if its slot differs (the power went while the slot
was being rewritten), restores the slot from it. A power cut can lose only the
write in progress. The journal slot a write-back will use next is erased in
the background while the drive is idle, so the extra copy costs no erase time.
A track whose data has not changed is not written at all (less wear).

`firmware/test/test_buffer.c` proves it against a simulated flash that cuts
the power at any operation, leaving that operation half done (a half-erased
sector is noise, a half-programmed run a prefix): for **every one** of the 158
operations of an idle pre-erase plus two write-backs, the reboot finds each
track entirely old or entirely new, the rest of the disk intact, and later
write-backs normal (78 of the cut points needed the journal).

**Failures.** Every step is read back. A failure is retried (a journal failure
moves to the next journal slot); after 3 failures the write-back gives up
instead of looping, and `buffer_fault()` turns the red LED to the continuous
fast blink (the flash is failing). The drive never freezes. The fault is also
recorded in the meta sector (`TDSFAULT`, 64 bytes after the marker), so it
survives a power cycle: the LED keeps warning until a full rebuild (DATA IN, or
the reformat after a successful DATA OUT) erases it. Transfers stay allowed, so
the user can rescue the files with DATA OUT; result blinks take priority over
the fault blink. SWD test hook: `buffer_dbg_inject = 0xFA11FA11` makes every
track read-back "fail" (the data is really written) to exercise all of this.
`dbg_flash_req = 5` (with `dbg_unlock`) programs `dbg_flash_len` bytes from
`dbg_flash_buf` at `dbg_flash_addr` in the SPI flash, e.g. to damage the marker.

**Marker.** The meta sector's marker is written last after any rebuild, so a
half-built image is never served; without it (first boot, or an interrupted
rebuild) `buffer_format()` writes an empty FAT12 volume. Because that erases
everything, one bad read must not trigger it (review 2026-09-27): the marker is
read up to 3 times, and before formatting `fatimg_valid()` checks whether a
sound FAT12 volume is there after all (boot sector, BPB, both FATs starting
with the media descriptor). If it is, the meta sector is rewritten (keeping a
recorded fault) instead of the disk being wiped: `buffer_dbg_marker_repaired`
counts it. An interrupted rebuild has no boot sector yet, so it is still
formatted. Proven on hardware by zeroing the marker over SWD
(`dbg_flash_req = 5`, below): after a reset the disk and its files were intact.

**Wear.** The SST25VF016B is rated for 100,000 erase cycles per sector. Every
write-back erases one of the 3 journal slots as well as the track's own slot,
so the journal slots are the most-erased sectors: each takes a third of all
write-backs. A typical small save (FAT, directory and 1-2 data tracks: 3-5
write-backs) costs 1-2 erases per journal slot, so roughly 50,000-100,000 such
saves; a large waveform (up to ~1 MB, ~57 tracks) costs ~20 per slot. Unchanged
tracks are not written at all. When a sector does wear out, the write-back
gives up after 3 tries and the persistent fault (above) tells the user to
rescue the files. A lifetime erase counter is not possible here: every DATA IN
and DATA OUT erases the whole chip, counter included. `buffer_dbg_writebacks`
counts journal writes since power-on (`tools/button.ps1 status`). An image from 1.0.0
and before (marker `TDSFBUF1` or `GTKBUF01` in slot 0's spare area) is taken
over as it is. `tools/reformat.ps1` re-formats over SWD.

## Track in RAM (floppy.c)
Only the track under the head is live in RAM, **raw** (9216 B + 18 data CRCs),
not pre-encoded MFM (which took 25.6 KB and left no room for writes or USB).

**Two track buffers; the head never waits for flash.** When the head leaves a
track with unsaved writes, that buffer becomes the *spare* and is written back
to flash in the background (buffer_wb_*: through the journal above, 256 B
(AAI word programming) per main-loop pass, every step read back). The
new track loads into the other buffer at once (~18 ms, like a real drive's
settle time). The idle flush (100 ms after the last write) copies the live
track to the spare (~10 us, interrupts off) and writes that back, so the live
track stays usable. Returning to a track still being written back loads it
from the spare buffer, not from half-written flash.

Why it matters (bug found 2026-09-24 with tools/stress_save.py +
tools/verify_disk.py): with a single buffer, leaving a dirty track blocked the
main loop ~310 ms in the flash store before the next track could load. The
TDS's controller, looking for sector 1 on the new cylinder, gave up after two
index pulses; the TDS driver's error recovery then continued from a later
sector *without rewriting the skipped ones* and still reported success. Same
LBAs every time for the same file layout (e.g. 216-221, 324-325), all internal
checks green, because the emulator was never sent those sectors. After the fix the
head waits 0.45 ms on a track change, and a 24-file stress run takes 144 s
instead of ~190 s (no hidden retries).

Self-checks (tools/fdstat.ps1, must stay 0): store read-back failures, a load
over unsaved writes, the RAM track changing during a write.
- Dirty tracks are also flushed 100 ms after the last write ends. The emulator is
  powered by the scope: data still only in RAM at power-off is lost. (Seen
  once with a longer delay: a torn FAT after a scope power-cycle, which later
  made the TDS hang reading the cross-linked file. The TDS firmware does not
  cope with corrupt media; keep the flush prompt.)
- RAM: the spare buffer doubles as the USB transfer workspace while the disk is
  ejected (flpy_scratch()), keeping RAM at 84 % incl. stack reserve.

## Formatting by the scope (verified 2026-09-24)
`FILESystem:FORMat "fd0:"` (or the front panel) low-level formats the emulator:
191 s, no errors, 3026 sectors written, then saves work and USB copy-out reads
the result. What the TDS does (from its firmware, `floppyFormat` ->
`floppyFormatAndVerifyTracks` twice): format + verify cylinders **40 -> 80**,
then **40 -> 0**. It formats and verifies **cylinder 80**, one past the disk,
which a real 3.5" mechanism can reach. With the head clamped at 79 the format
failed ("Mass storage error; osError") before ever reaching cylinders 0-39. Now
the head stops at cylinder 80 (`FLPY_CYL_LIMIT` 81), like a mechanism's end
stop: the TDS never goes further, and the buffer has slots for 81 cylinders.
(Cylinders 81-82 were allowed until 2026-09-25; their space now holds the
journal.) Each format track arrives as one
write gate per revolution carrying all 18 IDs + data fields; the decoder files
them by the IDs' sector numbers. The TDS's boot sector: OEM `TEK_TDS`, jump
`E9 00 00`, standard 1.44 MB BPB (fatimg accepts it).
`tools/format_test.py` runs it; `tools/gatelog.ps1` shows the first 64 write
gates and steps (ID/data counts per gate).

## Read: MFM generated on the fly
`mfm_track_byte(track, p)` describes byte p of the 12500-byte IBM track (gaps,
sync, IDs from the head's current cyl/head, data, CRCs); `mfm_encode_byte()`
turns it into 16 cells. The TMR3 DMA-ring refill interrupt runs these and
converts cells to flux intervals (TMR3 ch2 PWM, 400 ns pulses). INDEX is
raised when the generator wraps to byte 0.

## Write: WDATA capture + streaming decoder
- WGATE (PB9, EXINT9, both edges) starts/stops a write while selected; READ
  DATA is silenced for the duration.
- TMR1 ch1 (PA8) captures every WDATA falling edge at 144 MHz; DMA1 ch2 (flex
  TMR1_CH1) fills a 512-entry ring **continuously**; its half/full interrupts
  decode while a write is in progress and skip otherwise. (Starting the capture
  at WGATE once lost the first A1 when the interrupt was late.)
- Interval -> cells: `n = round(ticks / 144)`, valid 2..4 (anything else resets
  the decoder). Host test: tolerates +/-125 ns precompensation-sized jitter.
- `mfm_dec_*`: syncs on A1 (>= 1 needed, so a late start still works; the CRC
  covers A1 A1 A1 + mark as written), frames bytes, delivers ID (FE) and data
  (FB) fields. Data fields with a good CRC replace the sector in the RAM track.
- Target sector: an ID field in the same write (FORMAT) wins; otherwise the
  exact byte under the head when WGATE rose (`playback_byte()`: generator
  position minus the READ-DMA entries not yet played). Normal writes start
  ~44 bytes into the sector's 658-byte slot; `sector = (pos + 300 - 146) / 658`
  leaves ~300 bytes of margin either side.

## Debug (tools/fdstat.ps1)
Track loads/stores with timestamps, load/store time, write gates / sectors /
bad CRC / lost, step log. Counters reset with the MCU, so read them before a
reset.
