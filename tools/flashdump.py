"""flashdump.py - dump the emulator's SST25VF016B SPI flash over SWD.

Uses the dbg_flash_* window in firmware/src/main.c (SWD can only see RAM, so the
running firmware copies each 512-byte flash block into a RAM buffer on request).
Usage: python tools/flashdump.py [start_kb] [length_kb]   (default: whole 2 MB)
Output: captures/spiflash_<start>_<len>.bin
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "firmware")
start = int(sys.argv[1]) * 1024 if len(sys.argv) > 1 else 0
length = int(sys.argv[2]) * 1024 if len(sys.argv) > 2 else 2 * 1024 * 1024

syms = {}
_b, _f = os.path.join(FW, "build", "tdsfloppy.bin"), os.path.join(FW, "build", "flashed.bin")
if not os.path.exists(_f) or open(_b, "rb").read() != open(_f, "rb").read():
    sys.exit("FIRMWARE MISMATCH: build/tdsfloppy.bin is not the image last flashed - run make flash-swd first.")
for line in subprocess.run(["arm-none-eabi-nm", "build/tdsfloppy.elf"], cwd=FW,
                           capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and p[2].startswith("dbg_flash_"):
        syms[p[2]] = "0x" + p[0]

txt = os.path.join(FW, "build", "flashdump.txt").replace("\\", "/")
tcl = f"""
init
set fp [open "{txt}" w]
for {{set a {start}}} {{$a < {start + length}}} {{incr a 512}} {{
  mww {syms['dbg_flash_addr']} $a
  mww {syms['dbg_flash_req']} 1
  set n 0
  while {{[read_memory {syms['dbg_flash_req']} 32 1] != 0 && $n < 200}} {{ incr n }}
  puts $fp [read_memory {syms['dbg_flash_buf']} 32 128]
}}
close $fp
exit
"""
cfg = os.path.join(FW, "build", "flashdump.cfg")
open(cfg, "w").write(tcl)
print(f"dumping {length // 1024} KB from 0x{start:06X} ...")
ocd = subprocess.run(["openocd", "-f", "interface/stlink.cfg", "-f", "target/artery/at32f4x.cfg",
                      "-f", "build/flashdump.cfg"], cwd=FW, capture_output=True, text=True)
errors = [l for l in (ocd.stdout + ocd.stderr).splitlines() if l.startswith("Error")]
if ocd.returncode != 0 or errors:
    sys.exit("flashdump FAILED (OpenOCD): " + "; ".join(errors[:3]))

data = bytearray()
for line in open(txt):
    for w in line.split():
        data += int(w, 16).to_bytes(4, "little")
os.makedirs(os.path.join(ROOT, "captures"), exist_ok=True)
if len(data) != length:
    sys.exit(f"flashdump FAILED: got {len(data)} of {length} bytes")
out = os.path.join(ROOT, "captures", f"spiflash_{start // 1024}k_{len(data) // 1024}k.bin")
open(out, "wb").write(data)
print(f"wrote {len(data)} bytes -> {out}")

# quick survey: blank vs used 4 KB blocks, and any FAT boot sectors (55 AA at 510)
used = [i for i in range(0, len(data), 4096) if data[i:i + 4096] != b"\xff" * 4096]
print(f"non-blank 4 KB blocks: {len(used)} of {len(data) // 4096}"
      + (f" (first at 0x{start + used[0]:06X}, last at 0x{start + used[-1]:06X})" if used else ""))
for off in range(0, len(data) - 511, 512):
    s = data[off:off + 512]
    if s[510:512] == b"\x55\xaa" and s[0] in (0xEB, 0xE9):
        print(f"boot sector candidate at flash 0x{start + off:06X}: OEM {bytes(s[3:11])!r}")
