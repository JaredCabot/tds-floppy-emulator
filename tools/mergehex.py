"""mergehex.py - join Intel HEX files into one (run by `make`).

  python tools/mergehex.py OUT.hex IN1.hex IN2.hex ...

Used to make build/tdsfloppy_install.hex: the bootloader (0x08000000) and the
application (0x08002000) in one file, for the first installation through the
MCU's USB-DFU bootloader (docs/08), which programs a single file. Each input's
end-of-file record is dropped and one is written at the end. The inputs must
not overlap (checked).
"""
import sys

out, ins = sys.argv[1], sys.argv[2:]
lines, used = [], {}
for path in ins:
    base = 0
    for n, raw in enumerate(open(path), 1):
        rec = raw.strip()
        if not rec:
            continue
        if not rec.startswith(":"):
            sys.exit(f"mergehex: {path}:{n} is not an Intel HEX record")
        b = bytes.fromhex(rec[1:])
        if sum(b) & 0xFF:
            sys.exit(f"mergehex: {path}:{n} checksum error")
        count, addr, rtype = b[0], b[1] << 8 | b[2], b[3]
        if rtype == 0x01:                        # end of file: dropped, one added at the end
            continue
        if rtype == 0x04:                        # extended linear address
            base = (b[4] << 8 | b[5]) << 16
        elif rtype == 0x00:
            for a in range(base + addr, base + addr + count):
                if a in used and used[a] != path:
                    sys.exit(f"mergehex: {path} overlaps {used[a]} at 0x{a:08X}")
                used[a] = path
        lines.append(rec)
open(out, "w").write("\n".join(lines + [":00000001FF"]) + "\n")
print(f"{out}: {len(used)} bytes from {len(ins)} files")
