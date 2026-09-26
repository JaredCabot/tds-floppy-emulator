"""stress_save.py - make the TDS save N setup files (and optionally waveforms)
to both hd0: and fd0:, for tools/verify_disk.py to check.
Usage: python tools/stress_save.py 12 [--prefix ST] [--wave]"""
import sys, time
import pyvisa
import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import benchlock; benchlock.acquire(_os.path.basename(__file__))   # one bench tool at a time
n = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 12
prefix = sys.argv[sys.argv.index("--prefix") + 1] if "--prefix" in sys.argv else "ST"
s = pyvisa.ResourceManager().open_resource(__import__("os").environ.get("TDS_ADDR", "GPIB0::1::INSTR"))
s.timeout = 120000
s.write("*CLS"); s.write("FILESystem:OVERWrite OFF"); s.write("ACQuire:STATE STOP"); s.query("*OPC?")
t0 = time.time()
for i in range(n):
    jobs = [("SAVe:SETUp", f"{prefix}{i:02d}.SET")]
    if "--wave" in sys.argv:
        jobs.append(("SAVe:WAVEform CH1,", f"{prefix}{i:02d}.WFM"))
    for cmd, name in jobs:
        for drv in ("hd0:", "fd0:"):
            s.write(f'{cmd} "{drv}/{name}"')
            s.query("*OPC?")
    print(f"{i + 1}/{n} saved ({time.time() - t0:.0f} s) {s.query('ALLEV?').strip()}", flush=True)
s.write("ACQuire:STATE RUN")
s.write('FILESystem:CWD "fd0:/"')
print("fd0 DIR:", s.query("FILESystem:DIR?").strip())
