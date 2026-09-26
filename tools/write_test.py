"""write_test.py - end-to-end WRITE test using the TDS's own save commands.

1. SAVe:SETUp the current setup to hd0: (the scope's CF card) and to fd0:
   (the emulator). Both files must end up byte-identical.
2. Read both back with FILESystem:READFile and compare.
3. Wait for the emulator's write-back (500 ms after deselect), reset the emulator
   over SWD (reloads from SPI flash + raises DISK CHANGE, so the TDS must read
   the real disk again), and compare the fd0: copy again. TEST.BIN must be
   unharmed.
Optional: python tools/write_test.py wave   also saves CH1 as a waveform file
(bigger, spans several tracks) and checks it the same way.
"""
import os, subprocess, sys, time
import pyvisa
import os as _os, sys as _sys; _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
import benchlock; benchlock.acquire(_os.path.basename(__file__))   # one bench tool at a time
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
s = pyvisa.ResourceManager().open_resource(os.environ.get("TDS_ADDR", "GPIB0::1::INSTR"))
s.timeout = 120000


def q(c):
    return s.query(c).strip()


def readfile(path):
    s.write(f'FILESystem:READFile "{path}"')
    try:
        return s.read_raw()          # reads until the TDS asserts END
    except pyvisa.errors.VisaIOError as e:
        print("   read stopped:", e.abbreviation)
        return b""


def testbin(n=65536):
    return bytes((f ^ (f >> 8) ^ (f >> 16) ^ 0x5A) & 0xFF for f in range(n))


def same(label, a, b):
    ok = a == b and len(a) > 0
    diff = sum(1 for x, y in zip(a, b) if x != y) + abs(len(a) - len(b))
    print(f"   {label}: {len(a)} vs {len(b)} bytes, {diff} differences -> {'PASS' if ok else 'FAIL'}")
    return ok


items = [("SETUP", "SAVe:SETUp", "WSETUP.SET")]
if "wave" in sys.argv:
    items.append(("WAVE", "SAVe:WAVEform CH1,", "WWAVE.WFM"))

s.write("*CLS")
s.write("FILESystem:OVERWrite OFF")
s.write("ACQuire:STATE STOP")          # freeze CH1, or hd0/fd0 get different waveforms
q("*OPC?")
results = []
for label, cmd, fname in items:
    for drive in ("hd0:", "fd0:"):
        t = time.time()
        s.write(f'{cmd} "{drive}/{fname}"')
        print(f"1. {label} -> {drive}/{fname}: OPC {q('*OPC?')} ({time.time() - t:.1f} s) {q('ALLEV?')}")
s.write('FILESystem:CWD "fd0:/"')
print("   fd0 DIR:", q("FILESystem:DIR?"))

print("2. read back")
ref = {f: readfile(f"hd0:/{f}") for _, _, f in items}
for _, _, f in items:
    results.append(same(f"{f} fd0 vs hd0", readfile(f"fd0:/{f}"), ref[f]))

print("3. write-back, reset emulator, read from the disk again")
time.sleep(3)
st = subprocess.run(["powershell", "-ExecutionPolicy", "Bypass", "-File", os.path.join(ROOT, "tools", "fdstat.ps1"),
                     "-Steps", "0"], capture_output=True, text=True).stdout.splitlines()
print("   emulator before reset:", *st[:2], sep="\n     ")
subprocess.run(["openocd", "-f", "interface/stlink.cfg", "-f", "target/artery/at32f4x.cfg",
                "-c", "init; reset run; exit"], cwd=os.path.join(ROOT, "firmware"), capture_output=True)
time.sleep(3)
s.write('FILESystem:CWD "hd0:/"'); s.write('FILESystem:CWD "fd0:/"')
print("   fd0 DIR after reset:", q("FILESystem:DIR?"), "|", q("ALLEV?"))
for _, _, f in items:
    results.append(same(f"{f} fd0 (from disk) vs hd0", readfile(f"fd0:/{f}"), ref[f]))
results.append(same("TEST.BIN intact", readfile("fd0:/TEST.BIN"), testbin()))
s.write("ACQuire:STATE RUN")
print("RESULT:", "PASS" if all(results) else "FAIL")
