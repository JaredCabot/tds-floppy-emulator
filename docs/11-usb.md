# USB Stick Transfers

Status 2026-09-24: working on hardware. Verified with a SanDisk Cruzer Blade:
29 files (147 KB) internal disk -> stick -> rebuilt disk, every file
byte-identical to the scope's own CF-card copies (tools/verify_disk.py).

## Behaviour
| Action | What happens |
|---|---|
| **Top button as installed, left when horizontal (PC8) - DATA IN** | The internal disk is replaced by the next "page" of the stick's root files: alphabetical regardless of case, as many as fit in 1.44 MB. Further presses page on, then wrap to the first page. Skips directories, hidden/system files and files > 1.44 MB. Every sector is read back from the internal flash and compared as it is written, and the finished disk is re-checked (file count, cluster chains). If a load fails, the next press reloads the same page. A stick with no copyable files is refused (disk untouched). |
| **Both buttons held 3 s - FIRMWARE UPDATE** | Installs `UPDATE.UPD` from the stick: see docs/13. |
| **Bottom button as installed, right when horizontal (PC7) - DATA OUT ("take the disk out")** | Every file on the internal disk is copied to the stick, keeping the original dates, **folders included**: folders the scope made (`FILESystem:MKDir`) are recreated on the stick (an existing folder of that name is used, the contents merge), up to 8 levels deep. **Nothing on the stick is ever overwritten**: if `NAME.EXT` exists the file is saved as `NAME_1.EXT`, then `NAME_2.EXT`, ... (up to `_999`; files are created with FA_CREATE_NEW), except that a file already there **byte-for-byte identical** is recognised and not copied again, so saving the same disk twice, or retrying after a failure, makes no duplicates. Each copy is **read back from the stick and compared byte-for-byte**; only if every file is safely there is the internal disk **erased to a blank volume**, ready for new data. A stick failure (pulled, write error, full, mismatch) stops at once. A fault on the internal disk itself (a corrupt file or folder, folders deeper than 8) skips just that part, saves everything else, and keeps the disk. Either way the error blinks and the disk is not erased. Anything the scope saved but not yet committed is committed first. (The stock firmware could not copy out existing contents; 1.0.0 before the 2026-09-25 review copied the root only and so lost files in folders.) |
| **Stick inserted** | **Nothing happens** until a button is pressed (Jared's requirement, 2026-09-24; the stock unit auto-loaded on insertion, and an earlier version here auto-loaded onto an empty disk). Removing the stick resets DATA IN paging to the first page. |
| **Red LED (PB10)** | Flashes once every 0.25 s while copying (SysTick-driven, `LED_ACTIVITY_PERIOD_MS` in board.h; measured 125 ms on / 125 ms off); 12 fast toggles = error; 4 = nothing to load; 6 slow toggles = stick format not supported; 2 long (1 s) toggles = cancelled; off when idle; continuous 5 Hz = the internal SPI flash failed (self-test at boot: the unit then presents no disk and refuses transfers; or a track that could not be stored after 3 tries, docs/10). |
| **720 KB disks** (1.1.0) | DATA IN and DATA OUT work the same on a 720 KB DD disk (docs/10): DATA IN builds a 720 KB volume (1 KB clusters, 112 root entries) and loads as many files as fit in 730,112 bytes, skipping larger ones; DATA OUT reads the volume the host formatted; the blank disk after DATA OUT stays 720 KB. |
| **Transfer size** (1.2.0) | USB data moves in chunks of up to 8 KB (16 sectors in one mass-storage command, instead of one command per sector), held in the floppy's second track buffer, which is idle while the disk is ejected (`flpy_scratch2`). Every sector written to the internal disk is still read back and compared, every copy on the stick is still read back and compared, and cancel is checked at least once per chunk. |
| **Cancel** | Pressing the button that started a transfer again (held 50 ms) while the red LED flashes stops it at the next safe point: between sectors, or while waiting for the scope. DATA OUT: a partial or unverified copy is deleted, files already copied stay on the stick, the internal disk is **not** erased, and a later DATA OUT recognises what is already there. DATA IN: the disk keeps the files loaded so far and the next press loads that page again (the disk's previous contents are gone: DATA IN erases it first). The firmware update is not cancellable. The LED gives one long (1 s) blink. The other button is ignored during a transfer. `tools/button.ps1 -CancelAt N` cancels after N bytes (SWD hook `xfer_dbg_cancel_at`). |
| **Green LED (if fitted)** | Hard-wired to the scope's drive-select line. |

**Waiting for the scope.** A button press only starts a transfer once the scope
has left the drive alone for 1.5 s. The TDS keeps the drive selected for the
whole of a save plus its follow-up accesses (~12 s measured), so a press right
after saving waits (LED already flashing) and then proceeds. If the scope is
still using the drive after 15 s the transfer is refused: error blink, nothing
touched, press again. While waiting the emulator keeps serving the scope normally.
Why: pulling the disk out from under a save lets the TDS write its cached FAT
and directory over the new image (happened 2026-09-24 when a test tool was
saving during a DATA IN: corrupt FAT, and the scope hung).

During a transfer the floppy is "ejected" (no READY, DISK CHANGE set), so the
scope sees no disk and then a changed disk.

**Stick formats.** FAT12/16/32 and **exFAT**. exFAT keeps no 8.3 names, so DATA
IN makes one the way Windows does: the name itself if it fits 8.3 (upper-cased),
else up to 6 characters + `~1`, `~2`, ... unique on the internal disk
(`Scope capture 1.bmp` -> `SCOPEC~1.BMP`); on FAT sticks the stick's own short
name is used. Names starting with `.` are skipped (hidden by convention, e.g.
the `._*` files macOS writes). DATA OUT writes long names on any format.

## Implementation
- `usbhost.c` - Artery USB host core + MSC class (vendor/usb), FatFs bridge
  (disk_* on LUN 0), a ready flag. OTGFS on PA11/PA12; 48 MHz = 144 MHz PLL / 3.
  Board constraints: no VBUS sensing (PA9 is the debug UART), no VBUS switch
  (PB3 is floppy READY), no SOF output (PA8 is WDATA). USB interrupt priority 2,
  below the floppy's DMA/EXINT interrupts, so USB never delays the flux stream.
  `usbh_msc_read/write` block (with timeout) - fine, the floppy is ejected.
- FatFs (vendor/fatfs, config include/ffconf.h): long file names ON (needed
  for the `_1` renames; FF_USE_LFN 1, static 256-char buffer, ffunicode.c for
  CP437). DATA IN uses each file's short 8.3 name throughout (order, paging,
  name on the internal disk): `sfn()` = FILINFO.altname if set, else fname
  (FatFs leaves altname empty when a file has no long name). So `TEK00000_1.BMP`
  appears on the scope as `TEK000~1.BMP`. Code page 437, FF_FS_TINY, no RTC (fixed stamp,
  then `f_utime` copies the real date), exFAT ON (FF_FS_EXFAT 1). 8.3 names:
  `fatimg_short_name()` / `fatimg_name_used()` (host-tested). The paging
  bookmark and the DATA IN path hold full 255-character names.
- `fatimg.c` - builds / reads the 1.44 MB FAT12 image through an I/O interface
  (host-tested against a RAM "flash" that only allows 1->0 programming). The
  builder writes a file's data first and its directory entry only when the data
  is complete; FATs are generated at the end from the committed extents. The
  reader bounds-checks every cluster chain (the TDS hangs on corrupt media, so
  don't make any).
- `xfer.c` - DATA IN / DATA OUT. Its working memory (builder or reader + a
  sector buffer) is borrowed from the floppy's spare track buffer while the disk
  is ejected (`flpy_scratch()`).
- `buffer.c` - rebuild = chip erase, build, then write the "image complete"
  marker last, so an interrupted rebuild is re-formatted at the next boot.

## Test tools
- `tools/button.ps1 in|out|status` - press a button over SWD (same code path as
  the real buttons) and report result, files, bytes.
- `tools/save_both.py`, `tools/stress_save.py` - make the TDS save files to its
  CF card and to fd0: for comparison; `tools/verify_disk.py` - dump the SPI
  flash and compare every file with the CF-card copy.

## Next
- Test a real exFAT stick (enabled, host-tested, not yet on hardware). Try
  several sticks (brands, sizes); check a stick pulled mid-transfer fails cleanly (LED error, internal
  disk intact).
- Speed: done. SPI flash now uses AAI word programming: DATA IN of 34 files /
  470 KB takes 6.3 s (was 12.7 s); a full 1.44 MB page ~10 s.

## Limits and edge cases (audit 2026-09-24)
- **DATA IN replaces the internal disk.** If the stick fails part-way, the disk
  holds the files copied so far (a valid disk; error blink) and its previous
  contents are gone. Take the disk out (DATA OUT) first if it holds
  anything not yet on the stick.
- **DATA OUT re-sends the whole disk.** If it fails at file 5 of 10, files 1-4
  are already on the stick; pressing again saves them once more as `_1`
  copies. A file whose write or read-back fails is deleted from the stick, so
  a partial or wrong copy never looks valid. The internal disk is kept.
- `UPDATE.UPD` (any case) and names starting with `.` are never copied in.
- Sticks: FAT12/16/32 or exFAT, MBR, GPT or unpartitioned, 512-byte logical
  sectors (virtually all), up to 2 TB. GPT support (FF_LBA64) was added after a
  GPT-partitioned FAT32 Transcend JetFlash 16 GB failed to mount (2026-09-24).
  NTFS, unformatted sticks and sector sizes other than 512 bytes give their
  own signal, **3 slow blinks** (`XFER_BAD_FORMAT`): reformat as exFAT or
  FAT32. (A 4 KB-sector stick would otherwise have overrun FatFs's 512-byte
  buffer: `disk_read`/`disk_write` refuse any block size but 512.)
- Tested by hand (2026-09-24, real buttons, every byte checked): SanDisk
  Cruzer Blade 16 GB as FAT32/MBR and as exFAT/MBR, Transcend JetFlash 16 GB as
  FAT32/GPT. Test set: zero-length, odd-size, lower-case and long names, two
  pages (disk full after the first), and skip cases (too big, hidden, dot
  file, UPDATE.UPD, folder). On exFAT the 8.3 names are generated by the
  firmware and match what Windows chose on FAT32 (`SCOPEC~1.BMP`, `~2`).
  Repeat it with `tools/testset.py write E:` then `tools/testset.py check`.
- Rebuilding the disk (DATA IN, or the erase after DATA OUT) erases the whole
  SPI flash, including the firmware-update staging area; by then any update
  record has already been acknowledged (docs/13).
- If the SPI flash itself failed permanently, the background write-back would
  retry indefinitely and a button press would wait for it. The boot self-test
  (fast LED blink) catches a dead chip.

## Transfer speed (1.2.0)
Measured on the TDS 794D with a FAT32 (Transcend, GPT) and an exFAT (SanDisk,
MBR) stick; times from the firmware's own counters (`dbg_xfer_ms`, split into
`xfer_dbg_ms_wait/_usb/_flash`, printed by `tools/button.ps1`):

| Operation | 1.1.0 | 1.2.0 |
|---|---|---|
| DATA IN, 1,433,600 bytes | 19.1 s | 15.0 s (USB 2.0 s, internal flash 12.7 s), both sticks |
| DATA OUT writing 1,002,120 bytes, with read-back | 10.6 s | 6.9 s FAT32, 6.2 s exFAT |
| DATA OUT, files already on the stick (compare only) | 6.4 s | 4.1-4.3 s |
| Floppy track load (9 KB, what the host waits for) | 27.5 ms | 12.2 ms |

(1.1.0 times are end-to-end from the tool, +-2 s.) The USB side was never the
bottleneck: the stick reads at about 700 KB/s. The time was in the SPI flash
driver, whose every byte went through two library calls in another file
(`gpio_bits_*`, `spi_i2s_*`, never inlined). 1.2.0 does the chip select and
byte transfer at register level, inlined only in the hot loops (bulk read,
AAI programming, the busy poll) to keep the image small. DATA IN is now
bound by the flash chip's own word-program time (about 12 us per 2 bytes,
near its specified maximum). USB data also moves in chunks of up to 8 KB
(below); on these sticks that alone changed little, but it costs nothing.
Verified after the change: benchmark files byte-identical from the flash, a
full scope format (3,026 sectors, 0 bad CRC, 0 lost), cancel mid-chunk, and
an update installed by 1.2.0's own staging.
