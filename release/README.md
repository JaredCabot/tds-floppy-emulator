# Release files

| File | Firmware | Build ID | Size | SHA-256 |
|---|---|---|---|---|
| `tdsfloppy_install.hex` | 1.0.0 | `484f5c1d` | 132,234 bytes | `23ff4e0a3b60914b373489bb3c3f6964f5e4abacb44399b3893e028cde9b168d` |

The **Build ID** identifies the exact firmware image (docs/06): it is what a
running unit reports over SWD (`tools/button.ps1 status`).

**`tdsfloppy_install.hex`** is the first-installation image: the bootloader and
the firmware in one Intel HEX file, programmed once through the emulator's
USB-DFU bootloader with the Artery ISP Programmer. Follow *Load the Firmware*
in the [manual](../docs/manual/TFE-0001-00_TDS_Floppy_Emulator_Instructions.pdf)
(page 2-1), or [docs/08](../docs/08-unlock-and-flash-via-isp.md). The tool is
archived in [third-party/](../third-party/).

**Use this file, from this folder.** The files a build writes to
`firmware/build/` are development builds: `build/tdsfloppy.hex` holds the
firmware without the bootloader and does not start on its own, and
`build/tdsfloppy_install.hex` changes with every build. Check the SHA-256 above
if in doubt (Windows: `certutil -hashfile tdsfloppy_install.hex SHA256`).

Later versions are installed from a USB flash drive: the release will then
include `UPDATE.UPD` for that version (manual page 3-3). Version 1.0.0 is the
first release, so there is no update file yet.

Both files are built by `make` in `firmware/` (`build/tdsfloppy_install.hex`,
`build/UPDATE.UPD`).
