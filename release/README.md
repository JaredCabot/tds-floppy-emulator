# Release files

| File | Firmware | Build ID | Size | SHA-256 |
|---|---|---|---|---|
| `tdsfloppy_install.hex` | 1.1.0 | `3b0ee413` | 136,091 bytes | `bfb9d4e849b7455261bb45d8715dbc3d09e72a2dd2ed59d3fae8f0de57994ec3` |
| `UPDATE.UPD` | 1.1.0 | `3b0ee413` | 45,228 bytes | `b062d0379aa06ab43dd8732143e3c931f0bd6ae988259bd7d5e4fb0e0bbda02f` |

These are also attached to each release on the
[Releases page](https://github.com/JaredCabot/tds-floppy-emulator/releases); the
changes are in [CHANGELOG.md](../CHANGELOG.md). The **Build ID** identifies the
exact firmware image (docs/06): it is what a running unit reports over SWD
(`tools/button.ps1 status`).

**`tdsfloppy_install.hex`** is the first-installation image: the bootloader and
the firmware in one Intel HEX file, programmed once through the emulator's
USB-DFU bootloader with the Artery ISP Programmer. Follow *Load the Firmware*
in the [manual](../docs/manual/TFE-0001-00_TDS_Floppy_Emulator_Instructions.pdf)
(page 2-1), or [docs/08](../docs/08-unlock-and-flash-via-isp.md). The tool is
archived in [third-party/](../third-party/).

**`UPDATE.UPD`** updates an emulator that already runs this firmware (any
earlier version): copy it to the root of a USB flash drive, insert it, hold
both buttons for 3 seconds and release (manual page 3-3). The internal disk
and its files are kept.

**Use these files, from this folder or the Releases page.** The files a build
writes to `firmware/build/` are development builds: `build/tdsfloppy.hex` holds
the firmware without the bootloader and does not start on its own, and the
others change with every build. Check the SHA-256 above if in doubt (Windows:
`certutil -hashfile tdsfloppy_install.hex SHA256`).

Both files are built by `make` in `firmware/`; `tools/release_refresh.py`
copies them here.
