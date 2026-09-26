"""save_both.py - have the TDS save the current setup (and optionally CH1) to
both hd0: and fd0: under the given name, for comparisons after USB transfers.
Usage: python tools/save_both.py NEWSAVE.SET [--wave NEWWAVE.WFM]"""
import sys, time
import pyvisa
import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import benchlock; benchlock.acquire(_os.path.basename(__file__))   # one bench tool at a time
s = pyvisa.ResourceManager().open_resource(__import__("os").environ.get("TDS_ADDR", "GPIB0::1::INSTR"))
s.timeout = 120000
s.write("*CLS"); s.write("FILESystem:OVERWrite OFF"); s.write("ACQuire:STATE STOP"); s.query("*OPC?")
jobs = [("SAVe:SETUp", sys.argv[1])]
if "--wave" in sys.argv:
    jobs.append(("SAVe:WAVEform CH1,", sys.argv[sys.argv.index("--wave") + 1]))
for cmd, name in jobs:
    for drv in ("hd0:", "fd0:"):
        t = time.time()
        s.write(f'{cmd} "{drv}/{name}"')
        print(f"{cmd} {drv}/{name}: OPC {s.query('*OPC?').strip()} ({time.time() - t:.1f} s)")
s.write("ACQuire:STATE RUN")
s.write('FILESystem:CWD "fd0:/"')
print("fd0 DIR:", s.query("FILESystem:DIR?").strip(), "|", s.query("ALLEV?").strip())
