"""identify_image.py CHIP_DUMP - which firmware does a dump of the application
area (0x08002000, as tools/button.ps1 reads it) hold?

Prints one line:  match <build id> build\tdsfloppy.bin   (the current build)
                  known <build id> <name>                (a released version)
                  unknown - -                            (neither)
and writes build/button_installed.bin: the identified image (or the whole dump
if unknown), for the SWD tools' mismatch guard. An image's length cannot be
read from the chip (an update overwrites only its own length; later bytes may
be left from an older, longer image), so images are only ever matched against
known ones, never measured: releases come from the git tags (UPDATE.UPD from
1.1.0 on, the install image's application region for 1.0.0).
"""
import os, subprocess, sys, zlib

FW = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "firmware")
APP = 0x08002000


def build_id(img):
    return "%08x" % zlib.crc32(img + b"\xff" * (-len(img) % 4))


def blob(ref):
    r = subprocess.run(["git", "cat-file", "blob", ref], capture_output=True, cwd=FW)
    return r.stdout if r.returncode == 0 else None


def hex_app(text):
    data, base = {}, 0
    for line in text.split():
        b = bytes.fromhex(line[1:]); n, a, t = b[0], b[1] << 8 | b[2], b[3]
        if t == 4:
            base = (b[4] << 8 | b[5]) << 16
        elif t == 0:
            for i in range(n):
                if base + a + i >= APP:
                    data[base + a + i - APP] = b[4 + i]
    return bytes(data.get(i, 0xFF) for i in range(max(data) + 1)) if data else None


chip = open(sys.argv[1], "rb").read()
cands = [("match", "build\\tdsfloppy.bin", open(os.path.join(FW, "build", "tdsfloppy.bin"), "rb").read())]
tags = subprocess.run(["git", "tag", "-l", "v*", "--sort=-v:refname"], capture_output=True, text=True, cwd=FW).stdout.split()
for tag in tags:
    upd = blob(f"{tag}:release/UPDATE.UPD")
    if upd:
        cands.append(("known", f"release {tag}", upd[48:48 + int.from_bytes(upd[24:28], "little")]))
    hx = blob(f"{tag}:release/tdsfloppy_install.hex")
    if hx:
        img = hex_app(hx.decode())
        if img:
            cands.append(("known", f"release {tag}", img))
out = os.path.join(FW, "build", "button_installed.bin")
for kind, name, img in cands:
    if chip[:len(img)] == img:
        open(out, "wb").write(img)
        print(kind, build_id(img), name)
        break
else:
    open(out, "wb").write(chip)
    print("unknown - -")