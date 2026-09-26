"""verify_disk.py - check every file on the emulator's internal disk against the
TDS's copy of the same name on its CF card (hd0:), straight from SPI flash.

Dumps the flash over SWD (tools/flashdump.py), rebuilds the linear 1.44 MB image
from the 12 KB track slots, walks the FAT12 root directory, and compares each
file with hd0:/<name> read over GPIB. Reports bad sectors by file and by LBA.
Usage: python tools/verify_disk.py [--no-dump]   (--no-dump reuses the last dump)
"""
import os, struct, subprocess, sys
import pyvisa
import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import benchlock; benchlock.acquire(_os.path.basename(__file__))   # one bench tool at a time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DUMP = os.path.join(ROOT, "captures", "spiflash_0k_1920k.bin")
if "--no-dump" not in sys.argv:
    if os.path.exists(DUMP):
        os.remove(DUMP)                    # never compare a stale dump
    d = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "flashdump.py"), "0", "1920"],
                       capture_output=True, text=True)
    if d.returncode != 0 or not os.path.exists(DUMP):
        sys.exit("verify_disk: flash dump FAILED - " + (d.stdout + d.stderr).strip().splitlines()[-1:][0] if (d.stdout + d.stderr).strip() else "verify_disk: flash dump FAILED")
raw = open(DUMP, "rb").read()
img = b"".join(raw[(n // 18) * 12288 + (n % 18) * 512:][:512] for n in range(2880))
fat = img[512:512 * 10]


def fe(n):
    k = n * 3 // 2
    v = fat[k] | fat[k + 1] << 8
    return (v >> 4) if n & 1 else (v & 0xFFF)


tds = pyvisa.ResourceManager().open_resource(__import__("os").environ.get("TDS_ADDR", "GPIB0::1::INSTR"))
tds.timeout = 60000
root = img[19 * 512:33 * 512]
nfiles = nbad = 0
print(f"FAT1 == FAT2: {fat == img[512 * 10:512 * 19]}")
for i in range(0, len(root), 32):
    e = root[i:i + 32]
    if e[0] == 0:
        break
    if e[0] == 0xE5 or e[11] & 0x18:
        continue
    name = e[:8].decode("latin1").rstrip() + ("." + e[8:11].decode("latin1").rstrip() if e[8:11].strip() else "")
    cl, size = struct.unpack_from("<HI", e, 26)
    chain, c = [], cl
    while 2 <= c < 0xFF8 and len(chain) < 3000:
        chain.append(c)
        c = fe(c)
    data = b"".join(img[(33 + x - 2) * 512:(33 + x - 1) * 512] for x in chain)[:size]
    tds.write(f'FILESystem:READFile "hd0:/{name}"')
    try:
        ref = tds.read_raw()
    except pyvisa.errors.VisaIOError:
        print(f"  {name:12s} (no hd0 copy)")
        continue
    bad = sorted({j // 512 for j in range(max(len(ref), len(data))) if j >= len(data) or j >= len(ref) or ref[j] != data[j]})
    nfiles += 1
    nbad += bool(bad)
    lbas = [33 + chain[k] - 2 for k in bad if k < len(chain)]
    print(f"  {name:12s} LBA {33 + cl - 2:4d}+{len(chain):<3d} size {size:6d}  " +
          ("MATCH" if not bad else f"BAD sectors {bad} (LBA {lbas})"))
print(f"RESULT: {nfiles - nbad}/{nfiles} files match" + ("" if nbad else " -> PASS"))
