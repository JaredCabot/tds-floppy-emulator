"""release_refresh.py - refresh release/ after a final build (run from anywhere): the install image from the current build, its SHA-256 and
the build ID in release/README.md (the table gains a Build ID column)."""
import hashlib, os, re, shutil, sys, zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # tools/..
FW = os.path.join(ROOT, "firmware")
B = os.path.join(FW, "build")
import subprocess                                   # noqa: E402
for step in (["make"], ["make", "test"]):           # never publish a stale or failing build
    r = subprocess.run(step, cwd=FW, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"release_refresh: '{' '.join(step)}' failed; release/ not changed\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
REL = os.path.join(ROOT, "release")
shutil.copyfile(os.path.join(B, "tdsfloppy_install.hex"), os.path.join(REL, "tdsfloppy_install.hex"))

data, base = {}, 0
for line in open(os.path.join(REL, "tdsfloppy_install.hex")).read().split():
    b = bytes.fromhex(line[1:]); n, a, t = b[0], b[1] << 8 | b[2], b[3]
    if t == 4:
        base = (b[4] << 8 | b[5]) << 16
    elif t == 0:
        for i in range(n):
            data[base + a + i] = b[4 + i]
app = open(os.path.join(B, "tdsfloppy.bin"), "rb").read()
boot = open(os.path.join(B, "boot.bin"), "rb").read()
assert bytes(data.get(0x08000000 + i, 0xFF) for i in range(len(boot))) == boot, "bootloader region differs"
assert bytes(data.get(0x08002000 + i, 0xFF) for i in range(len(app))) == app, "firmware region differs"
padded = app + b"\xFF" * (-len(app) % 4)
build = f"{zlib.crc32(padded):08x}"
raw = open(os.path.join(REL, "tdsfloppy_install.hex"), "rb").read()
sha, size = hashlib.sha256(raw).hexdigest(), len(raw)

f = os.path.join(REL, "README.md")
s = open(f, encoding="utf-8").read().replace("\r\n", "\n")
ver = re.search(r"FW_VERSION_STR\s+\"([^\"]+)\"", open(os.path.join(ROOT, "firmware", "include", "version.h")).read()).group(1)
s, n = re.subn(r"\| File \|[^\n]*\n\|---[^\n]*\n\| `tdsfloppy_install.hex` \|[^\n]*",
               f"| File | Firmware | Build ID | Size | SHA-256 |\n|---|---|---|---|---|\n"
               f"| `tdsfloppy_install.hex` | {ver} | `{build}` | {size:,} bytes | `{sha}` |", s)
if n != 1:
    raise SystemExit("release table not found; README not changed")
if "**Build ID**" not in s:
    s = s.replace("**`tdsfloppy_install.hex`** is the first-installation image",
                  "The **Build ID** identifies the exact firmware image (docs/06): it is what a\n"
                  "running unit reports over SWD (`tools/button.ps1 status`).\n\n"
                  "**`tdsfloppy_install.hex`** is the first-installation image")
if "\u2014" in s:
    raise SystemExit("em dash")
open(f, "w", encoding="utf-8", newline="\n").write(s)
print(f"release: {size:,} bytes, sha256 {sha}, build ID {build} (bootloader + firmware regions verified)")
