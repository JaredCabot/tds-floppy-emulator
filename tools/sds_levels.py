"""sds_levels.py - capture all 4 Siglent channels during a forced TDS read and
report the voltage levels on each (for static/strap lines like pins 9 and 11).

Probe map (edit LABELS to suit): C1 RDATA (P1.24), C2 INDEX (P1.2),
C3 pin 11 (DINST/JD), C4 pin 9 (HDOUT/JE). All 10x probes.
Voltage from byte codes: 1 V/div, offset -2.5 V -> V = code/30 + 2.5 (measured).
"""
import os, subprocess, time
import numpy as np
import pyvisa

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "firmware")
LABELS = {"C1": "RDATA p24", "C2": "INDEX p2", "C3": "pin 11 (JD)", "C4": "pin 9 (JE/HDOUT)"}

rm = pyvisa.ResourceManager()
s = rm.open_resource("USB0::0xF4EC::0x100C::SDS2HBAD6R0515::0::INSTR"); s.timeout = 30000
s.clear()


def w(c):
    s.write(c); time.sleep(0.15)


for c in LABELS:
    n = c[1]
    w(f":CHANnel{n}:SWITch ON"); w(f":CHANnel{n}:PROBe VALue,10")
    w(f":CHANnel{n}:SCALe 1"); w(f":CHANnel{n}:OFFSet -2.5")
w(":TIMebase:SCALe 0.1"); w(":TIMebase:DELay 0.45")      # 1 s window after the trigger
w(":ACQuire:MDEPth 10M")
w(":TRIGger:TYPE EDGE"); w(":TRIGger:EDGE:SOURce C2")
w(":TRIGger:EDGE:SLOPe FALLing"); w(":TRIGger:EDGE:LEVel 1.5")
w(":TRIGger:MODE SINGle")
time.sleep(1)

subprocess.run(["openocd", "-f", "interface/stlink.cfg", "-f", "target/artery/at32f4x.cfg",
                "-c", "init; mww 0x40010C14 0x0080; exit"], cwd=FW, capture_output=True)
t = rm.open_resource(__import__("os").environ.get("TDS_ADDR", "GPIB0::1::INSTR")); t.timeout = 60000
t.write('FILESystem:CWD "hd0:/"'); t.write('FILESystem:CWD "fd0:/"')

for _ in range(60):
    st = s.query(":TRIGger:STATus?").strip()
    if st.lower().startswith("stop"):
        break
    time.sleep(0.5)
print("trigger:", st)

w(":WAVeform:WIDTh BYTE")
for c, lab in LABELS.items():
    w(f":WAVeform:SOURce {c}")
    w(":WAVeform:STARt 0"); w(":WAVeform:POINt 5000000")    # first 0.5 s is plenty
    w(":WAVeform:DATA?")
    raw = s.read_raw()
    i = raw.index(b"#"); nd = int(raw[i + 1:i + 2]); ln = int(raw[i + 2:i + 2 + nd])
    v = np.frombuffer(raw[i + 2 + nd:i + 2 + nd + ln], dtype=np.int8) / 30.0 + 2.5
    lo, hi = np.percentile(v, 1), np.percentile(v, 99)
    frac_low = np.mean(v < 1.5)
    print(f"{c} {lab:18s} min {v.min():5.2f} V  p1 {lo:5.2f}  median {np.median(v):5.2f}  "
          f"p99 {hi:5.2f}  max {v.max():5.2f} V   time below 1.5 V: {100 * frac_low:5.1f}%")
