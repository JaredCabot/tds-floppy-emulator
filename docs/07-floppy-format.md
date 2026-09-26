# Floppy Format & Bus Signals (reference)

How the design uses this: [docs/10](10-read-write-path.md). Drive behaviour
(timing, READY, gating): [docs/09](09-fdd-interface-spec.md).

## Geometry (1.44 MB HD, IBM System 34 MFM)
- 80 cylinders x 2 heads x 18 sectors x 512 B = 1,474,560 B = 2880 LBAs.
  (The head can step to cylinder 82; the TDS formats cylinder 80 too.)
- 500 kbit/s data rate, 300 RPM -> 200 ms/rev -> 12500 data bytes/track.
- Each data byte -> 16 MFM cells; 1 cell = 1 us (144 timer ticks at 144 MHz).
- LBA map: lba = (cyl*2 + head)*18 + sector_index(0..17).

## Track layout (per revolution), gap/sync bytes as used for 1.44 MB
```
Gap4a 80x4E | Sync 12x00 | IAM(3x C2*, FC) | Gap1 50x4E |
  18x: Sync 12x00 | IDAM(3x A1*, FE) | C H R N | CRC(2) | Gap2 22x4E |
       Sync 12x00 | DAM(3x A1*, FB)  | 512 data | CRC(2) | Gap3 84x4E
| Gap4b 4E to the end (12500 bytes)
```
- `*` = missing-clock sync mark: A1 -> cells 0x4489, C2 -> 0x5224.
- N = 0x02 (512-byte sectors). Sector numbers R are 1..18.
- CRC = CRC-CCITT (poly 0x1021, init 0xFFFF) over the 3 sync bytes + the
  address-mark byte + the field, stored big-endian. Check value:
  "123456789" -> 0x29B1.
- MFM clock rule: a clock cell is 1 only when the data bits either side of it
  are both 0. Flux intervals are therefore 2, 3 or 4 us.

Implemented in `src/mfm.c` (`mfm_track_byte`, `mfm_encode_byte`, streaming
decoder `mfm_dec_*`) and checked by `make test`: every sector's ID and data CRC
and data after a round trip, every clock cell against the rule, exactly 111 sync
words per track, and the write decoder with jitter and a late start.

## Bus signals (docs/02-pinmap.csv)
Inputs from the host: SEL (PA0), STEP (PA1), DIR (PB0), SIDE1 (PB4),
WGATE (PB9), WDATA (PA8).
Outputs to the host: INDEX (PB8), TRK0 (PB6), WPROT (PB5), READY (PB3),
RDATA (PA7), DSKCHG (PB7).

Every output goes MCU -> 74AHC04 inverter -> NPN open collector -> 1 k pull-up
to 5 V, so **driving the MCU pin LOW asserts the (active-low) host line**.
`floppy.c` wraps this as outB_assert/outB_deassert. Senses confirmed on the
bench: STEP direction (DIR low = step in), SIDE1 low = head 1, pin 6 = DISK
CHANGE and pin 8 = READY for the TDS, pin 9 HD OUT high = 2HD (host pull-up;
jumper JE off).

## Blank FAT12 image
Standard DOS 1.44 MB BPB (`src/fat12.c`): LBA0 boot/BPB, 1..9 FAT1, 10..18
FAT2, 19..32 root directory (224 entries), 33.. data (1 sector per cluster).
The TDS's own format writes the same layout (OEM name `TEK_TDS`).
