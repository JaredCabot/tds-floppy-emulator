# TDS 700D Floppy Driver - What Disk Sizes Does It Support?

Question (2026-09-24): could the emulator present a USB stick to the scope as one
very large floppy? Answered from the scope firmware itself
(`scope firmware/TDS_v8.0e_9f176f8e.bin`, the version in Jared's TDS 794D).

## Method
- The image is a raw flash dump of a VxWorks system on a **Motorola 68k** CPU
  (functions end `4E 5E 4E 75` = unlk/rts). It carries its VxWorks **symbol
  table** (16-byte entries `{0, name*, value, type<<8}`, text 0x05, data 0x09).
- Load address **0x05001000** (derived by matching symbol-name pointers to
  string offsets; 5 of 6 names agree).
- `tools/dis68k.py <symbol>` disassembles any function by name (Capstone m68k)
  and annotates calls/addresses with symbol names.

## Findings
Floppy driver functions: `floppyInit`, `floppyDriveInit`, `floppySetForMedia`,
`floppySetFromBps`, `floppyReadBlocks/WriteBlocks`, `floppyFormat`, `fdc*`
(82077-class controller), tables `_rateTable`, `_eots`, `_byteGaps`,
`_formatGaps`, `_bytesPerSectorFromCode`.

`_rateTable` (0x05365214) - the only media the driver knows:

| index | size | rate | sectors/track (512 B) |
|---|---|---|---|
| 0 | 1440 KB | 500 kbit/s | 18 |
| 1 | 760 KB | 300 kbit/s | 9 |
| 2 | 720 KB | 250 kbit/s | 9 |
| 3 | 2880 KB | 1000 kbit/s | 36 |

- `floppySetForMedia(drive, sizeKB, heads, bpsCode, ...)` looks the size up in
  `_rateTable`; `floppySetFromBps` then loads sectors/track and gaps for that
  media and sector size ("Bps" = bytes per sector, not BPB).
- `floppySetForMedia` has exactly **one caller** (0x0500FEA6): it reads bit 0 of
  a drive status register (the density line, pin 9 HD OUT) and asks for
  **720 KB** (bit 0 = 0) or **1440 KB** (bit 0 = 1), 2 heads. That function is
  called from 5 places (drive init, media change, ...).
- The value 2880 (0xB40) never appears as an immediate: the **2.88 MB entry is
  unreachable**. Cylinders are fixed at 80 (`move.b #80` in drive init).

**Conclusion: through the floppy connector the scope can only ever use a
720 KB or 1.44 MB disk.** Geometry comes from the drive's density signal, not
from the disk's boot sector, and the block count follows from it (2880 blocks
max). A "huge floppy" is not possible without modifying the scope firmware.

## Other storage in the same firmware (for reference)
Drivers exist for a SCSI **Zip** drive (`zipDevCreate`, `isZipInstalled`,
`scsi*`), **PCMCIA** ATA cards (`pcmciaDevCreate`, "PCMCIA: Media Detected: %u
Byte Disk") and an **NFS** client (`nfsMount`). Whether a given model has the
hardware for them is a separate question; none of them is reachable from the
emulator's floppy connector.

## What this means for the "USB stick as the disk" idea
Still possible, at 1.44 MB per disk: the emulator maps the scope's floppy sectors
straight onto a 2880-sector region of the stick (a small FAT12 partition, or an
image file), with no internal copy. Not implemented; a possible future mode
("direct bridge"), to be decided after hands-on use of the current firmware.
