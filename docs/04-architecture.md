# Architecture (as built)

Clean-room firmware for the AT32F415 SFR1M44-DU26: a 1.44 MB floppy for the
TDS scope, backed by the on-board SPI flash, with a USB stick for moving files
in and out. Super-loop plus interrupts; no RTOS. KISS / YAGNI.

## Modules (firmware/src, one job each)
| Module | Job |
|---|---|
| `boot/boot.c` | **Bootloader** (8 KB at 0x08000000): installs a staged update from the SPI flash, then starts the app (docs/13) |
| `main.c` | Init, super-loop, button actions, LED patterns, SWD debug hooks |
| `buttons.c` | Pure C, host-tested: debounce + gestures (single press on release; both held 3 s = update) |
| `update.c` | Pure C, host-tested: update-file header, CRC-32, vector-table checks (shared with the bootloader) |
| `board.c` / `clock.c` | 144 MHz clock from the 8 MHz crystal, DWT delays, UART log (PA9), red LED (active-low, SysTick-driven activity flashing) |
| `floppy.c` | **Hard real-time floppy bus slave**: drive-select gating, spin-up/READY, STEP/DIR, SIDE1, DISK CHANGE, INDEX, READ DATA stream, WDATA capture + write decoding, two track buffers, background write-back, eject/insert |
| `mfm.c` | Pure C, host-tested: IBM MFM track generator (per byte), encoder, streaming write decoder, CRC-CCITT |
| `buffer.c` | The disk image in SPI flash: one 12 KB slot per track (166 slots), load/store/verify, background write-back state machine, format |
| `spiflash.c` | SST25VF016B driver on SPI2 (18 MHz): AAI word programming (2x faster than per-byte), blocking and non-blocking erase |
| `fatimg.c` | Pure C, host-tested: build and read a 1.44 MB FAT12 image through an I/O interface; Windows-style 8.3 names for long / exFAT names |
| `fat12.c` | Standard DOS 1.44 MB boot sector / BPB |
| `usbhost.c` | Artery USB host core + MSC class, FatFs disk bridge, stick-ready flag |
| `xfer.c` | DATA IN (stick -> disk, paged) and DATA OUT (disk -> stick, rename on clash, verify, erase); firmware-update staging + acknowledge; SWD stick hook |

(`test/testimg.c`: a test image generator, host tests only.)

Dependencies: `main -> {floppy, usbhost, xfer, buffer}`; `xfer -> {usbhost,
buffer, floppy, fatimg}`; `floppy -> {buffer, mfm}`; `buffer -> {spiflash,
fatimg}`. Nothing depends on `main` or `xfer`.

## The floppy side (details: docs/09, docs/10)
- The track under the head is held **raw** in RAM (18 x 512 B). READ DATA is
  generated **on the fly**: the TMR3 DMA ring-refill interrupt turns track bytes
  into MFM cells and cells into flux intervals (TMR3 ch2 PWM, 400 ns pulses).
  IDs always carry the head's current cylinder/head; while the RAM track doesn't
  match (just stepped, loading) the track has no address marks, so the host
  simply retries - like a real drive settling.
- Writes: WDATA edges are captured continuously by TMR1 ch1 + DMA; while WGATE
  is active the interrupt decodes MFM and drops each good data field into the
  RAM track (sector from the ID in the stream, or the exact rotational
  position).
- **Two track buffers.** Leaving a track with unsaved writes hands its buffer
  to a background write-back (erase, 256 B programmed per main-loop pass,
  read-back verify); the next track loads into the other buffer at once
  (~18 ms). The idle flush (100 ms after the last write) copies and writes back
  the same way.
- Drive behaviour per the TEAC FD-05HF spec and a real drive in the TDS:
  outputs only while selected, READY ~480 ms after select, INDEX/READ DATA only
  when ready and seek-complete, STEP on its trailing edge, head travel to
  cylinder 82 (the TDS formats cylinder 80).

## Interrupts (priority: lower number wins)
| IRQ | Prio | Job |
|---|---|---|
| DMA1 ch3 (TMR3 READ DATA ring) | 0 | Generate the next 256 flux intervals; INDEX |
| DMA1 ch2 (TMR1 WDATA capture ring) | 0 | Decode written data while WGATE is active |
| EXINT0 (SELECT), EXINT1 (STEP), EXINT9 (WGATE) | 1 | Drive select/ready, head movement, write start/stop |
| OTGFS (USB host) | 2 | USB - never delays the flux stream |
| SysTick (1 ms) | lowest | INDEX pulse width, seek-complete, spin-up, LED flashing |

**Watchdog.** The independent watchdog (`watchdog_start()` in board.c, about
16 s, 11-21 s over the LICK tolerance) is fed by the main loop, every USB
sector transfer and the eject wait, so a hang anywhere resets the unit instead
of needing a power cycle. The longest legitimate unfed stretch is a USB sector
that times out (2 s + 50 ms per sector since the vendor driver change, see
firmware/vendor/README.md). It pauses while SWD holds the core halted.
`dbg_reset_cause` (SWD) keeps the reset flags of the last start: bit 29 means
the watchdog fired.

The main loop runs track loads, write-back steps, the USB host state machine,
buttons and transfers. Transfers block the loop, but the floppy is ejected
while they run. Ejecting (`flpy_eject`) first waits for the host to leave the
drive alone for 1.5 s (at most 15 s, refusing after that), servicing the floppy
meanwhile, then marks the disk removed atomically with that check, and only
then commits the pending writes.

## Memory
- Internal flash: bootloader 3 KB (of 8 KB) at 0x08000000, app ~39 KB at
  0x08002000 (limit 52 KB: the update staging area). Real-time code -O2, the
  rest -Os. RAM ~89 % of 32 KB including the reserved stack and heap (no heap
  is used). The stack is **measured**, not estimated: free RAM is painted at boot
  and `dbg_stack_free` (SWD) reports what the stack has never reached; the worst seen is ~4.9 KB (1.1.x: no printf, no heap). The two track buffers are 18 KB;
  the spare one doubles as the USB transfer workspace (builder/reader, name
  table, sector and path buffers) while the disk is ejected (`flpy_scratch()`).
- SPI flash: 166 x 12 KB track slots (2.04 MB) + the top 4 KB for the boot
  self-test. A marker in slot 0, written last, means "complete image present".

## Board constraints that shaped the design
- PB3 (READY) and PB4 (SIDE1) are JTAG pins: JTAG-DP must be switched off
  (SWD kept) or PB3 ignores GPIO writes.
- PA9 is the debug UART (no USB VBUS sensing); PB3 is READY (no VBUS switch);
  PA8 is WDATA (no USB SOF output).
- The red LED is active-low (common anode with the green one, whose cathode is
  the drive-select line).

## Testing
- `make test`: MFM track round trip and clock-rule check, streaming decoder
  (clean, +/-125 ns jitter, late start), FAT12 builder/reader against a RAM
  "flash" that only allows 1->0 programming, test image.
- Bench (tools/, docs/10, docs/11): the TDS saves files over GPIB to its CF card
  and to the emulator; the SPI flash is dumped over SWD and every file compared.
  Self-check counters (store verify failures, loads while dirty, track changed
  during a write) must stay 0.
