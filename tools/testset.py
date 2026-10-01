"""testset.py - repeatable hands-on USB test (docs/11 "Tested by hand").

  python tools/testset.py write E:   put the 10 test files on the stick at E:
  python tools/testset.py check      dump the emulator's internal disk over SWD
                                     and check every file on it against the set

Workflow: `write`, eject the stick safely, insert it in the emulator, press the
DATA IN, run `check` (expects page 1); press DATA IN again, `check` (page 2).
Contents are pseudo-random from fixed seeds, so nothing else needs storing.

Expected on the emulator's disk (FAT32, GPT or exFAT sticks alike):
  page 1: EMPTY.TXT PAGE1.BIN PAGE2.BIN               (disk then full)
  page 2: SCOPEC~1.BMP SCOPEC~2.BMP TEST01.BIN TEST02.DAT
  never:  BIGFILE.BIN (too big), HIDDEN.TXT (hidden), .dotfile.txt (dot file)
"""
import ctypes, hashlib, os, random, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# (name on the stick, size, name expected on the emulator's disk or None = skipped)
FILES = [("EMPTY.TXT", 0, "EMPTY.TXT"), ("PAGE1.BIN", 716800, "PAGE1.BIN"),
         ("PAGE2.BIN", 716800, "PAGE2.BIN"), ("Scope capture 1.bmp", 71680, "SCOPEC~1.BMP"),
         ("Scope capture 2.bmp", 71680, "SCOPEC~2.BMP"), ("TEST01.BIN", 1000, "TEST01.BIN"),
         ("test02.dat", 512, "TEST02.DAT"), ("BIGFILE.BIN", 1536000, None),
         (".dotfile.txt", 100, None), ("HIDDEN.TXT", 100, None)]


def content(i, size):
    return random.Random(1000 + i).randbytes(size)


def write(drive):
    for i, (name, size, _) in enumerate(FILES):
        path = os.path.join(drive + "\\", name)
        if os.path.exists(path):
            ctypes.windll.kernel32.SetFileAttributesW(path, 0x80)   # clear hidden to overwrite
        open(path, "wb").write(content(i, size))
    ctypes.windll.kernel32.SetFileAttributesW(os.path.join(drive + "\\", "HIDDEN.TXT"), 0x02)
    print(f"{len(FILES)} test files written to {drive}\\ - eject the stick safely before use")


def check():
    dump = os.path.join(ROOT, "captures", "spiflash_0k_1920k.bin")
    if os.path.exists(dump):
        os.remove(dump)                                   # never check a stale dump
    d = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "flashdump.py"), "0", "1920"],
                       capture_output=True, text=True)
    if d.returncode != 0 or not os.path.exists(dump):
        sys.exit("flash dump FAILED: " + (d.stdout + d.stderr).strip()[-200:])
    raw = open(dump, "rb").read()
    img = b"".join(raw[(n // 18) * 12288 + (n % 18) * 512:][:512] for n in range(2880))
    fat = img[512:512 * 10]

    def nxt(n):
        k = n * 3 // 2
        v = fat[k] | fat[k + 1] << 8
        return (v >> 4) if n & 1 else (v & 0xFFF)

    expect = {disk: (i, size) for i, (_, size, disk) in enumerate(FILES) if disk}
    print("FAT1 == FAT2:", fat == img[512 * 10:512 * 19])
    root = img[19 * 512:33 * 512]
    good = total = 0
    for off in range(0, len(root), 32):
        e = root[off:off + 32]
        if e[0] == 0:
            break
        if e[0] == 0xE5 or e[11] & 0x08:
            continue
        name = (e[:8].decode("latin1").strip() + "." + e[8:11].decode("latin1").strip()).rstrip(".")
        cl, size = struct.unpack_from("<HI", e, 26)
        chain, c = [], cl
        while 2 <= c < 0xFF8 and len(chain) < 3000:
            chain.append(c)
            c = nxt(c)
        data = b"".join(img[(33 + x - 2) * 512:(33 + x - 1) * 512] for x in chain)[:size]
        total += 1
        ok = name in expect and hashlib.sha1(data).digest() == hashlib.sha1(content(*expect[name])).digest()
        good += ok
        print(f"  {name:13s} {size:8d} B  {'MATCH' if ok else 'NOT IN THE SET / MISMATCH'}")
    print(f"{good}/{total} files match the test set")


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "write":
        write(sys.argv[2].rstrip(":\\") + ":")
    elif len(sys.argv) == 2 and sys.argv[1] == "check":
        check()
    else:
        sys.exit(__doc__)
