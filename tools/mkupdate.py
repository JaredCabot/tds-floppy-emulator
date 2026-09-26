"""mkupdate.py - build UPDATE.UPD (USB-stick firmware update) from the app binary.

Usage (run by `make`): python tools/mkupdate.py build/tdsfloppy.bin include/version.h build/UPDATE.UPD
Format: firmware/include/update.h (48-byte header + image padded to 4 bytes).
"""
import re, struct, sys, zlib

APP_BASE = 0x08002000
IMAGE_MAX = 0x1FF000 - (0x1F2000 + 256)           # UPD_IMAGE_MAX in update.h

bin_path, ver_path, out_path = sys.argv[1:4]
image = open(bin_path, "rb").read()
image += b"\xFF" * (-len(image) % 4)               # multiple of 4 (erased-flash padding)
version = int(re.search(r"#define\s+FW_VERSION\s+(0x[0-9A-Fa-f]+)", open(ver_path).read()).group(1), 16)

if len(image) > IMAGE_MAX:
    sys.exit(f"mkupdate: image {len(image)} bytes > {IMAGE_MAX} (staging area); see docs/13")
sp, pc = struct.unpack_from("<II", image, 0)
if not (0x20000000 < sp <= 0x20008000 and pc & 1 and APP_BASE < pc < APP_BASE + len(image)):
    sys.exit(f"mkupdate: vector table SP={sp:#x} PC={pc:#x} is not an app linked at {APP_BASE:#x}")

head = struct.pack("<8s8sIIII3I", b"TDSFLUPD", b"SFRC2D.B", 1, version, len(image),
                   zlib.crc32(image), 0, 0, 0)
head += struct.pack("<I", zlib.crc32(head))
open(out_path, "wb").write(head + image)
print(f"{out_path}: version {version >> 16}.{version >> 8 & 255}.{version & 255}, "
      f"{len(image)} bytes ({100 * len(image) // IMAGE_MAX}% of the {IMAGE_MAX}-byte limit), crc {zlib.crc32(image):08x}"
      f" (= build ID)")
