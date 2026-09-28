"""release_refresh.py - refresh release/ after a final build (run from anywhere):

  1. `make` and `make test` in firmware/ (stops on any failure: never publish a
     stale or failing build);
  2. copies build/tdsfloppy_install.hex and build/UPDATE.UPD into release/,
     checking the install image's bootloader and firmware regions and the
     update file's image against the build;
  3. writes release/README.md: version, build ID, sizes and SHA-256s.
"""
import hashlib, os, re, shutil, struct, subprocess, sys, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # tools/..
FW = os.path.join(ROOT, "firmware")
B = os.path.join(FW, "build")
REL = os.path.join(ROOT, "release")

for step in (["make"], ["make", "test"]):
    r = subprocess.run(step, cwd=FW, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"release_refresh: '{' '.join(step)}' failed; release/ not changed\n{r.stdout[-2000:]}{r.stderr[-2000:]}")

app = open(os.path.join(B, "tdsfloppy.bin"), "rb").read()
boot = open(os.path.join(B, "boot.bin"), "rb").read()
padded = app + b"\xFF" * (-len(app) % 4)
build = f"{zlib.crc32(padded):08x}"
ver = re.search(r'FW_VERSION_STR\s+"([^"]+)"', open(os.path.join(FW, "include", "version.h")).read()).group(1)

# install image: bootloader + firmware
data, base = {}, 0
for line in open(os.path.join(B, "tdsfloppy_install.hex")).read().split():
    b = bytes.fromhex(line[1:]); n, a, t = b[0], b[1] << 8 | b[2], b[3]
    if t == 4:
        base = (b[4] << 8 | b[5]) << 16
    elif t == 0:
        for i in range(n):
            data[base + a + i] = b[4 + i]
assert bytes(data.get(0x08000000 + i, 0xFF) for i in range(len(boot))) == boot, "install hex: bootloader region differs"
assert bytes(data.get(0x08002000 + i, 0xFF) for i in range(len(app))) == app, "install hex: firmware region differs"
# update file: its image is the firmware
upd = open(os.path.join(B, "UPDATE.UPD"), "rb").read()
assert upd[48:48 + len(app)] == app and zlib.crc32(upd[48:]) == int(build, 16), "UPDATE.UPD differs from the build"

rows = []
for name in ("tdsfloppy_install.hex", "UPDATE.UPD"):
    shutil.copyfile(os.path.join(B, name), os.path.join(REL, name))
    raw = open(os.path.join(REL, name), "rb").read()
    rows.append(f"| `{name}` | {ver} | `{build}` | {len(raw):,} bytes | `{hashlib.sha256(raw).hexdigest()}` |")

README = f"""# Release files

| File | Firmware | Build ID | Size | SHA-256 |
|---|---|---|---|---|
{chr(10).join(rows)}

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
"""
if "\u2014" in README:
    sys.exit("em dash in release/README.md")
open(os.path.join(REL, "README.md"), "w", encoding="utf-8", newline="\n").write(README)
print(f"release: {ver} build {build}")
for r in rows:
    print("  " + r)
