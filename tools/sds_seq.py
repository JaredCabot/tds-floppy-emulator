"""sds_seq.py - capture the TDS's floppy access sequence (Pass 1) and print a
timeline. Probes: C1 STEP (14), C2 INDEX (2), C3 SIDE1 (26), C4 DRIVE SELECT (4).
Usage:
  python tools/sds_seq.py <tag>            arm on SEL, access fd0: over GPIB, fetch
  python tools/sds_seq.py <tag> --arm      arm only, triggering on the first STEP
                                           (then eject/re-insert the disk)
  python tools/sds_seq.py <tag> --fetch    access fd0: over GPIB, wait, fetch
  python tools/sds_seq.py <tag> --analyse  re-analyse a saved capture
Saves captures/seq_<tag>.npz.
"""
import os, sys, time
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TAG = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else "run"
NPZ = os.path.join(ROOT, "captures", f"seq_{TAG}.npz")
CH = {"C1": "STEP", "C2": "INDEX", "C3": "SIDE1", "C4": "SEL"}
LONG = "--p2" in sys.argv or "--p3" in sys.argv   # 10 s window
if "--p2" in sys.argv:
    CH = {"C1": "STEP", "C2": "MOTOR", "C3": "DSKCHG", "C4": "SEL"}
if "--p3" in sys.argv:   # what decides whether the host reads: READY, INDEX, RDATA
    CH = {"C1": "RDATA", "C2": "INDEX", "C3": "READY", "C4": "SEL"}


def capture():
    import pyvisa
    rm = pyvisa.ResourceManager()
    s = rm.open_resource("USB0::0xF4EC::0x100C::SDS2HBAD6R0515::0::INSTR"); s.timeout = 60000
    s.clear()

    def w(c):
        s.write(c); time.sleep(0.15)

    if "--fetch" not in sys.argv:                     # set up and arm
        for c in CH:
            n = c[1]
            w(f":CHANnel{n}:SWITch ON"); w(f":CHANnel{n}:PROBe VALue,10")
            w(f":CHANnel{n}:SCALe 1"); w(f":CHANnel{n}:OFFSet -2.5")
        if LONG:   # 1 s/div, trigger 3 s from the left: -3 .. +7 s, 2 MS/s
            w(":TIMebase:SCALe 1"); w(":TIMebase:DELay 2"); w(":ACQuire:MDEPth 20M")
        else:      # 0.5 s/div: -0.2 .. +4.8 s, 2 MS/s
            w(":TIMebase:SCALe 0.5"); w(":TIMebase:DELay 2.3"); w(":ACQuire:MDEPth 10M")
        src = "C1" if ("--arm" in sys.argv and CH["C1"] == "STEP") else "C4"   # first STEP, else SELECT
        w(":TRIGger:TYPE EDGE"); w(f":TRIGger:EDGE:SOURce {src}")
        w(":TRIGger:EDGE:SLOPe FALLing"); w(":TRIGger:EDGE:LEVel 1.5")
        w(":TRIGger:MODE SINGle")
        time.sleep(1)
        print(f"armed on {CH[src]}:", s.query(":TRIGger:STATus?").strip())
        if "--arm" in sys.argv:
            return None

    t = rm.open_resource(__import__("os").environ.get("TDS_ADDR", "GPIB0::1::INSTR")); t.timeout = 60000
    t.write("*CLS"); t.write('FILESystem:CWD "hd0:/"'); t.write('FILESystem:CWD "fd0:/"')
    print("TDS DIR:", t.query("FILESystem:DIR?").strip()[:300])
    print("TDS events:", t.query("ALLEV?").strip())

    for _ in range(40):
        st = s.query(":TRIGger:STATus?").strip()
        if st.lower().startswith("stop"):
            break
        time.sleep(0.5)
    print("trigger:", st)
    srate = float(s.query(":ACQuire:SRATe?"))
    w(":WAVeform:WIDTh BYTE")
    data = {}
    for c in CH:
        w(f":WAVeform:SOURce {c}")
        total = int(float(s.query(":ACQuire:POINts?")))
        buf, start = bytearray(), 0
        while start < total:
            n = min(5_000_000, total - start)
            w(f":WAVeform:STARt {start}"); w(f":WAVeform:POINt {n}"); w(":WAVeform:DATA?")
            raw = s.read_raw()
            i = raw.index(b"#"); nd = int(raw[i + 1:i + 2]); ln = int(raw[i + 2:i + 2 + nd])
            buf += raw[i + 2 + nd:i + 2 + nd + ln]; start += n
        data[c] = np.frombuffer(bytes(buf), dtype=np.int8)
    os.makedirs(os.path.dirname(NPZ), exist_ok=True)
    np.savez_compressed(NPZ, srate=srate, **data)
    return srate, data


def analyse(srate, data):
    dt = 1.0 / srate
    low = {c: (data[c].astype(np.int16) / 30.0 + 2.5) < 1.5 for c in data}   # asserted = low
    def edges(b):
        f = np.flatnonzero(~b[:-1] & b[1:]); r = np.flatnonzero(b[:-1] & ~b[1:])
        return f, r
    ms = lambda i: i * dt * 1e3
    by = {name: c for c, name in CH.items()}
    t0 = 3000.0 if LONG else 200.0            # trigger position -> report times relative to it
    ev = []
    for name, c in by.items():
        if name in ("STEP", "INDEX", "RDATA"):
            continue
        f, r = edges(low[c])
        on, off = ("-> head 1", "-> head 0") if name == "SIDE1" else ("on", "off")
        ev += [(ms(x) - t0, f"{name} {on}") for x in f] + [(ms(x) - t0, f"{name} {off}") for x in r]
    f, _ = edges(low[by["STEP"]]) if "STEP" in by else ([], []); steps = [ms(x) - t0 for x in f]
    b = []
    for t in steps:
        if b and t - b[-1][-1] < 30: b[-1].append(t)
        else: b.append([t])
    ev += [(g[0], f"STEP x{len(g)} ({(g[-1]-g[0]):.0f} ms, interval {np.mean(np.diff(g)) if len(g)>1 else 0:.1f} ms)") for g in b]
    print(f"\nwindow {len(data['C1']) * dt:.2f} s @ {dt*1e9:.0f} ns, times relative to the trigger; "
          "initial levels (low=asserted): " + ", ".join(f"{CH[c]}={'LOW' if low[c][0] else 'high'}" for c in CH))
    if "INDEX" in by:
        fi, ri = edges(low[by["INDEX"]]); idx = [ms(x) - t0 for x in fi]
        print(f"INDEX pulses: {len(idx)}" + (f", period {np.median(np.diff(idx)):.1f} ms, width "
              f"{np.median([(ri[ri > x][0] - x) * dt * 1e3 for x in fi if (ri > x).any()]):.2f} ms" if len(idx) > 1 else ""))
        if idx:
            print("INDEX runs (start ms):", [round(idx[0])] + [round(idx[k]) for k in range(1, len(idx)) if idx[k] - idx[k-1] > 300])
    if "RDATA" in by:   # activity periods: pulses closer than 5 ms belong together
        fr, _ = edges(low[by["RDATA"]]); tr = [ms(x) - t0 for x in fr]
        per = []
        for t in tr:
            if per and t - per[-1][1] < 5: per[-1][1] = t; per[-1][2] += 1
            else: per.append([t, t, 1])
        ev += [(p[0], f"RDATA active until {p[1]:.1f} ms ({p[2]} pulses)") for p in per if p[2] > 20]
    print("\ntimeline:")
    for t, e in sorted(ev):
        print(f"  {t:9.1f} ms  {e}")


if __name__ == "__main__":
    if "--analyse" in sys.argv:
        z = np.load(NPZ); analyse(float(z["srate"]), {c: z[c] for c in CH})
    else:
        r = capture()
        if r:
            analyse(*r)
