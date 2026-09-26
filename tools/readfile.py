"""readfile.py - time FILESystem:READFile for one or more paths.
Usage: python tools/readfile.py fd0:/WSETUP.SET hd0:/WSETUP.SET ...
       add --save to store each file under captures/ (drive prefix in the name)."""
import os, sys, time
import pyvisa

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
s = pyvisa.ResourceManager().open_resource(__import__("os").environ.get("TDS_ADDR", "GPIB0::1::INSTR"))
s.timeout = 90000
for path in [a for a in sys.argv[1:] if not a.startswith("--")]:
    t = time.time()
    s.write(f'FILESystem:READFile "{path}"')
    try:
        d = s.read_raw()
        print(f"{path}: {len(d)} bytes in {time.time() - t:.1f} s")
        if "--save" in sys.argv:
            open(os.path.join(ROOT, "captures", path.replace(":/", "_")), "wb").write(d)
    except pyvisa.errors.VisaIOError:
        print(f"{path}: TIMEOUT after {time.time() - t:.1f} s")
print(s.query("ALLEV?").strip())
