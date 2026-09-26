"""check_image_end.py ELF BIN - fail the build if the firmware's own idea of its
image length differs from the .bin size.

The firmware computes its build ID (dbg_fw_build, docs/06) over the image as the
linker symbols describe it: .data's load image is the last thing in flash. If
a future change places a section after it, the build ID would silently stop
matching the `crc` make prints; this check turns that into a build error.
"""
import os
import subprocess
import sys

APP_BASE = 0x08002000                      # firmware/include/update.h

elf, binf = sys.argv[1:3]
sym = {}
out = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True, check=True).stdout
for line in out.splitlines():
    p = line.split()
    if len(p) == 3:
        sym[p[2]] = int(p[0], 16)
end = sym["_sidata"] + (sym["_edata"] - sym["_sdata"]) - APP_BASE
size = os.path.getsize(binf)
if end != size:
    sys.exit(f"check_image_end: the linker symbols say the image is {end} bytes but {binf} is "
             f"{size} bytes, so the build ID would be wrong (a section placed after .data?)")
print(f"check_image_end: {size} bytes, image end consistent with the build ID")
