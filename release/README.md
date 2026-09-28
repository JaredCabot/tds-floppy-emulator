# Release files

| File | Firmware | Build ID | Size | SHA-256 |
|---|---|---|---|---|
| `tdsfloppy_install.hex` | 1.2.0 | `1c27c478` | 133,621 bytes | `c3eeafb19c4d53ba14a7985e2fe69ab5c2feaaede94e662c3bf27f58cfe983f7` |
| `UPDATE.UPD` | 1.2.0 | `1c27c478` | 44,324 bytes | `e5572dd42f79856e3d67afb98a094a32a209734cc645fe349c88fc9c92f2a63c` |

These are also attached to each release on the
[Releases page](https://github.com/JaredCabot/tds-floppy-emulator/releases); the
changes are in [CHANGELOG.md](../CHANGELOG.md). The **Build ID** identifies the
exact firmware image (docs/06): it is what a running unit reports over SWD
(`tools/button.ps1 status`).

**`tdsfloppy_install.hex`** is the first-installation image: the bootloader and
the firmware in one Intel HEX file, programmed once through the emulator's
USB-DFU bootloader with the Artery ISP Programmer. Follow *Load the Firmware*
in the [manual](../docs/manual/TFE-0001-01_TDS_Floppy_Emulator_Instructions.pdf)
(page 2-1), or [docs/08](../docs/08-unlock-and-flash-via-isp.md). The tool is
archived in [third-party/](../third-party/).

**`UPDATE.UPD`** updates an emulator that already runs this firmware (any
earlier version): copy it to the root of a USB flash drive, insert it, hold
both buttons for 3 seconds and release (manual page 3-4). The internal disk
and its files are kept.

**Use these files, from this folder or the Releases page.** The files a build
writes to `firmware/build/` are development builds: `build/tdsfloppy.hex` holds
the firmware without the bootloader and does not start on its own, and the
others change with every build. Check the SHA-256 above if in doubt (Windows:
`certutil -hashfile tdsfloppy_install.hex SHA256`).

Both files are built by `make` in `firmware/`; `tools/release_refresh.py`
copies them here.
