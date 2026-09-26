"""dis68k.py - disassemble TDS 700D firmware functions by symbol name.

The image (scope firmware/TDS_v8.0e_*.bin) is mapped at 0x05001000; its VxWorks
symbol table has 16-byte entries {0, name*, value, type<<8}. Code symbols have
type 0x05, data 0x09 (RAM, not in the image). Calls/addresses are annotated with
symbol names.
Usage: python tools/dis68k.py <symbol|0xaddr> [more...] [--max N] [--grep TEXT]
"""
import bisect, glob, os, re, struct, sys
import capstone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG = glob.glob(os.path.join(ROOT, "scope firmware", "TDS_v8.0e_*.bin"))[0]
BASE = 0x05001000
d = open(IMG, "rb").read()

def cstr(addr):
    o = addr - BASE
    if not 0 <= o < len(d): return None
    e = d.find(b"\0", o, o + 80)
    s = d[o:e]
    return s.decode("latin1") if e > o and all(32 <= c < 127 for c in s) else None

syms = {}
for i in range(0, len(d) - 16, 4):                      # scan for symbol entries
    z, name, val, typ = struct.unpack_from(">IIII", d, i)
    if z == 0 and BASE <= name < BASE + len(d) and typ in (0x500, 0x700, 0x900, 0x800):
        n = cstr(name)
        if n and n.startswith("_") and len(n) > 2:
            syms.setdefault(n, (val, typ))
by_addr = sorted((v, n) for n, (v, t) in syms.items() if t == 0x500)
addrs = [a for a, _ in by_addr]

def name_of(a):
    for n, (v, t) in syms.items():
        if v == a: return n
    return None

def near(a):
    k = bisect.bisect_right(addrs, a) - 1
    return f"{by_addr[k][1]}+0x{a - addrs[k]:X}" if k >= 0 and a - addrs[k] < 0x4000 else None

md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_M68K_020)
args = [a for a in sys.argv[1:] if not a.startswith("--")]
mx = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 400
for a in args:
    if a.startswith("0x"): start = int(a, 16)
    else:
        start = syms[a if a.startswith("_") else "_" + a][0]
    k = bisect.bisect_right(addrs, start)
    end = addrs[k] if k < len(addrs) else start + 0x400
    print(f"\n==== {near(start) or hex(start)} @ {start:08X} .. {end:08X}")
    n = 0
    for ins in md.disasm(d[start - BASE:end - BASE], start):
        note = []
        for m in re.finditer(r"\$([0-9a-f]{7,8})", ins.op_str):
            v = int(m.group(1), 16)
            s = name_of(v) or (cstr(v) and repr(cstr(v))) or near(v)
            if s: note.append(s)
        print(f"  {ins.address:08X}  {ins.mnemonic:8s} {ins.op_str:40s} {'; ' + ', '.join(note) if note else ''}")
        n += 1
        if n >= mx: break
if "--grep" in sys.argv:
    t = sys.argv[sys.argv.index("--grep") + 1].lower()
    for n, (v, ty) in sorted(syms.items()):
        if t in n.lower(): print(f"{n:40s} {v:08X} type {ty:X}")
