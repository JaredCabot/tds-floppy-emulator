"""format_test.py - have the TDS format fd0: (the emulator) and report the outcome.
Then checks the result is usable: directory, a save + read-back, free space.
Usage: python tools/format_test.py"""
import time
import pyvisa
import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import benchlock; benchlock.acquire(_os.path.basename(__file__))   # one bench tool at a time
s = pyvisa.ResourceManager().open_resource(__import__("os").environ.get("TDS_ADDR", "GPIB0::1::INSTR"))
s.timeout = 600000                                   # a format takes minutes
s.write("*CLS")
t = time.time()
s.write('FILESystem:FORMat "fd0:"')
print(f"FORMAT: OPC {s.query('*OPC?').strip()} after {time.time() - t:.0f} s", flush=True)
print("  ESR", s.query("*ESR?").strip(), "|", s.query("ALLEV?").strip(), flush=True)
import sys
if "--no-save" in sys.argv:
    sys.exit(0)
s.timeout = 120000
s.write('FILESystem:CWD "fd0:/"')
print("  DIR:", s.query("FILESystem:DIR?").strip(), flush=True)
print("  FREESPACE:", s.query("FILESystem:FREESpace?").strip(), flush=True)
s.write("ACQuire:STATE STOP"); s.query("*OPC?")
for drv in ("hd0:", "fd0:"):
    s.write(f'SAVe:SETUp "{drv}/AFTERFMT.SET"')
    s.query("*OPC?")
s.write("ACQuire:STATE RUN")
print("  DIR after save:", s.query("FILESystem:DIR?").strip(), "|", s.query("ALLEV?").strip(), flush=True)
