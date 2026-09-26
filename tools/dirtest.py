"""dirtest.py - DATA OUT with folders (review item 1) and without duplicates
(item 7), on the real scope.

  python tools/dirtest.py make    the scope makes folders on fd0: and saves files
                                  into them; each file is read back as reference
  (press DATA OUT: tools/button.ps1 out)
  python tools/dirtest.py check   every file is on the stick, byte for byte, in
                                  its folder; the internal disk is empty
  python tools/dirtest.py again   the scope saves ROOT01.SET again (identical);
                                  after a second DATA OUT, `check` must find no
                                  ROOT01_1.SET: an identical copy is recognised
References go to captures/dirtest/.
"""
import os
import subprocess
import sys

import pyvisa

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REF = os.path.join(ROOT, "captures", "dirtest")
sys.path.insert(0, HERE)
import benchlock  # noqa: E402

FILES = [("SAVe:SETUp", "ROOT01.SET"),
         ("SAVe:SETUp", "TESTDIR/IN01.SET"),
         ("SAVe:WAVEform CH1,", "TESTDIR/IN02.WFM"),
         ("SAVe:SETUp", "TESTDIR/SUB2/DEEP01.SET")]
DIRS = ["TESTDIR", "TESTDIR/SUB2", "TESTDIR/EMPTY"]


def scope():
    s = pyvisa.ResourceManager().open_resource(__import__("os").environ.get("TDS_ADDR", "GPIB0::1::INSTR"))
    s.timeout = 120000
    s.write("*CLS"); s.write("FILESystem:OVERWrite OFF"); s.write("ACQuire:STATE STOP"); s.query("*OPC?")
    return s


def readfile(s, path):
    s.write(f'FILESystem:READFile "fd0:/{path}"')
    return s.read_raw()


def ref_path(name):
    return os.path.join(REF, name.replace("/", "__"))


def stick_get(name):
    out = os.path.join(REF, "stick__" + name.replace("/", "__"))
    if os.path.exists(out):
        os.remove(out)
    benchlock.release()                      # stick.ps1 is part of this test: hand it the lock
    r = subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File",
                        os.path.join(HERE, "stick.ps1"), "get", name, out],
                       capture_output=True, text=True)
    benchlock.acquire(os.path.basename(__file__))
    return open(out, "rb").read() if os.path.exists(out) else None, r.stdout.strip().splitlines()[:1]


def make():
    os.makedirs(REF, exist_ok=True)
    s = scope()
    for d in DIRS:
        s.write(f'FILESystem:MKDir "fd0:/{d}"'); s.query("*OPC?")
    for cmd, name in FILES:
        s.write(f'{cmd} "fd0:/{name}"'); s.query("*OPC?")
    for _, name in FILES:
        data = readfile(s, name)
        open(ref_path(name), "wb").write(data)
        print(f"  fd0:/{name}: {len(data)} bytes (reference saved)")
    s.write('FILESystem:CWD "fd0:/TESTDIR"'); print("  fd0:/TESTDIR:", s.query("FILESystem:DIR?").strip())
    s.write('FILESystem:CWD "fd0:/"'); print("  fd0:/:", s.query("FILESystem:DIR?").strip())
    print("  errors:", s.query("ALLEV?").strip())
    s.write("ACQuire:STATE RUN")


def check():
    bad = 0
    for _, name in FILES:
        ref = open(ref_path(name), "rb").read()
        got, msg = stick_get(name)
        ok = got == ref
        bad += not ok
        print(f"  {name:26s} {'MATCH' if ok else 'MISMATCH / MISSING'} ({len(got) if got else 0} of {len(ref)} bytes) {msg}")
    dup, _ = stick_get("ROOT01_1.SET")
    print(f"  ROOT01_1.SET on the stick: {'YES (duplicate)' if dup is not None else 'no'}")
    s = scope()
    s.write('FILESystem:CWD "fd0:/"'); print("  internal disk after DATA OUT:", s.query("FILESystem:DIR?").strip())
    s.write("ACQuire:STATE RUN")
    print("PASS" if not bad else f"FAIL: {bad} file(s)")


def again():
    s = scope()
    s.write('SAVe:SETUp "fd0:/ROOT01.SET"'); s.query("*OPC?")
    data = readfile(s, "ROOT01.SET")
    same = data == open(ref_path("ROOT01.SET"), "rb").read()
    print(f"  fd0:/ROOT01.SET saved again: {len(data)} bytes, identical to the first: {same}")
    s.write("ACQuire:STATE RUN")


if __name__ == "__main__":
    benchlock.acquire(os.path.basename(__file__))
    {"make": make, "check": check, "again": again}.get(sys.argv[1] if len(sys.argv) > 1 else "", lambda: sys.exit(__doc__))()
