"""verify_testbin.py - end-to-end read test through the TDS floppy controller.

Makes the scope read fd0:/TEST.BIN (served by the emulator test image) and send it
over GPIB, then checks every byte against the firmware's pattern
(testimg_pattern in firmware/include/testimg.h). Any mismatch means the scope
read wrong data off our emulated disk.
"""
import os, time
import pyvisa

SIZE = 65536


def pattern(f):
    return (f ^ (f >> 8) ^ (f >> 16) ^ 0x5A) & 0xFF


s = pyvisa.ResourceManager().open_resource(os.environ.get("TDS_ADDR", "GPIB0::1::INSTR"))
s.timeout = 120000
s.write("*CLS")
s.write('FILESystem:CWD "fd0:/"')
print("CWD:", s.query("FILESystem:CWD?").strip())
print("DIR:", s.query("FILESystem:DIR?").strip())

t = time.time()
s.write('FILESystem:READFile "fd0:/TEST.BIN"')
data = b""
try:
    while len(data) < SIZE:
        data += s.read_raw()
except pyvisa.errors.VisaIOError as e:
    print("read stopped:", e.abbreviation)
dt = time.time() - t

print(f"received {len(data)} bytes in {dt:.1f} s")
n = min(len(data), SIZE)
bad = [i for i in range(n) if data[i] != pattern(i)]
print(f"compared {n} bytes: {len(bad)} mismatches")
for i in bad[:10]:
    print(f"  offset {i:6d} (LBA {33 + i // 512}, byte {i % 512}): got 0x{data[i]:02X} want 0x{pattern(i):02X}")
if len(data) > SIZE:
    print("trailing bytes:", data[SIZE:SIZE + 16])
print("ESR:", s.query("*ESR?").strip(), "|", s.query("ALLEV?").strip())
print("RESULT:", "PASS" if n == SIZE and not bad else "FAIL")
