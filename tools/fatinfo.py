"""fatinfo.py - decode a FAT12 floppy image (boot sector/BPB, FAT, root dir).

Usage: python tools/fatinfo.py IMAGE [IMAGE2]
With two images, prints the boot-sector fields side by side and lists every
differing byte in the first sectors, so we can see exactly what the TDS
accepts (stock-firmware image) versus what we generated.
"""
import struct, sys

FIELDS = [("jump", 0, "3s"), ("oem", 3, "8s"), ("bytes/sec", 11, "<H"), ("sec/clus", 13, "B"),
          ("reserved", 14, "<H"), ("nFATs", 16, "B"), ("rootEnts", 17, "<H"), ("totSec16", 19, "<H"),
          ("media", 21, "B"), ("sec/FAT", 22, "<H"), ("sec/trk", 24, "<H"), ("heads", 26, "<H"),
          ("hidden", 28, "<I"), ("totSec32", 32, "<I"), ("drive#", 36, "B"), ("resv", 37, "B"),
          ("bootSig", 38, "B"), ("volID", 39, "<I"), ("label", 43, "11s"), ("fsType", 54, "8s"),
          ("sig510", 510, "<H")]


def bpb(img):
    return {n: struct.unpack_from(f, img, o)[0] for n, o, f in FIELDS}


def listing(img):
    b = bpb(img)
    bps = b["bytes/sec"] or 512
    root = (b["reserved"] + b["nFATs"] * b["sec/FAT"]) * bps
    out = []
    for i in range(b["rootEnts"] or 224):
        e = img[root + 32 * i: root + 32 * i + 32]
        if len(e) < 32 or e[0] == 0:
            break
        if e[0] == 0xE5:
            continue
        name = e[:8].decode("latin1").rstrip() + ("." + e[8:11].decode("latin1").rstrip() if e[8:11].strip() else "")
        cl, size = struct.unpack_from("<HI", e, 26)
        out.append(f"  {name:12s} attr 0x{e[11]:02X}  cluster {cl:4d}  size {size}")
    return out


imgs = [open(p, "rb").read() for p in sys.argv[1:]]
bs = [bpb(i) for i in imgs]
for n, _, _ in FIELDS:
    vals = [b[n] for b in bs]
    fmt = [v if isinstance(v, bytes) else (hex(v) if n in ("media", "sig510", "volID", "bootSig", "drive#") else v) for v in vals]
    mark = "   <-- differs" if len(set(map(str, vals))) > 1 else ""
    print(f"{n:10s} " + "   ".join(f"{str(v):22s}" for v in fmt) + mark)

for k, img in enumerate(imgs):
    print(f"\n[{sys.argv[1 + k]}] FAT1 first 16 bytes: {img[512:528].hex(' ')}")
    print("root directory:")
    print("\n".join(listing(img)) or "  (empty)")

if len(imgs) == 2:
    a, b = imgs
    diffs = [i for i in range(512) if a[i] != b[i]]
    print(f"\nboot sector: {len(diffs)} differing bytes")
    print("  offsets:", diffs[:80])
    print("  boot code/area 62..509 of image 1 non-zero bytes:", sum(1 for x in a[62:510] if x))
