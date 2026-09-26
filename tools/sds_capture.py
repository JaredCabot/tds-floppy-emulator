"""sds_capture.py - capture the floppy bus on the Siglent SDS2504X HD while the
TDS reads the emulator, then decode the MFM straight off the wire.

Probes: C1 = RDATA (P1.24, 10x), C2 = INDEX (P1.2, 10x), C3 = PA7 (MCU, 1x, optional).
Usage:  python tools/sds_capture.py            # arm, trigger a TDS read, capture, analyse
        python tools/sds_capture.py --analyse  # re-analyse the last capture only
Saves the raw capture to captures/capture.npz.
"""
import os, sys, time, subprocess
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, "firmware")
NPZ = os.path.join(ROOT, "captures", "capture.npz")   # outside firmware/build (survives make clean)
SDS = "USB0::0xF4EC::0x100C::SDS2HBAD6R0515::0::INSTR"
TDS = os.environ.get("TDS_ADDR", "GPIB0::1::INSTR")
CHANS = {"C1": "RDATA", "C2": "INDEX", "C3": "PA7"}


def capture():
    import pyvisa
    rm = pyvisa.ResourceManager()
    s = rm.open_resource(SDS); s.timeout = 30000
    s.clear()

    def w(cmd):                        # the SDS stalls on back-to-back setup writes
        s.write(cmd)
        time.sleep(0.15)
    for c, off in (("C1", -2.5), ("C2", -2.5), ("C3", -1.6)):
        w(f":CHANnel{c[1]}:SWITch ON"); w(f":CHANnel{c[1]}:PROBe VALue,10")
        w(f":CHANnel{c[1]}:SCALe 1"); w(f":CHANnel{c[1]}:OFFSet {off}")
    w(":TIMebase:SCALe 0.02")          # 20 ms/div -> 200 ms window = one revolution
    w(":TIMebase:DELay 0.09")          # put the trigger near the left edge
    w(":ACQuire:MDEPth 10M")           # 50 MS/s
    w(":TRIGger:TYPE EDGE"); w(":TRIGger:EDGE:SOURce C2")
    w(":TRIGger:EDGE:SLOPe FALLing"); w(":TRIGger:EDGE:LEVel 1.5")
    w(":TRIGger:MODE SINGle")
    time.sleep(1.0)
    print("armed:", s.query(":TRIGger:STATus?").strip())

    # force a genuine re-read: assert DSKCHG over SWD, then touch fd0: over GPIB
    env = dict(os.environ)
    subprocess.run(["openocd", "-f", "interface/stlink.cfg", "-f", "target/artery/at32f4x.cfg",
                    "-c", "init; mww 0x40010C14 0x0080; exit"], cwd=FW, capture_output=True, env=env)
    t = rm.open_resource(TDS); t.timeout = 60000
    t.write("*CLS"); t.write('FILESystem:CWD "hd0:/"'); t.write('FILESystem:CWD "fd0:/"')

    for _ in range(60):
        st = s.query(":TRIGger:STATus?").strip()
        if st.lower().startswith("stop"):
            break
        time.sleep(0.5)
    print("trigger status:", st)
    if not st.lower().startswith("stop"):
        sys.exit("no trigger: INDEX never pulsed (drive not selected, or probe/level wrong)")

    srate = float(s.query(":ACQuire:SRATe?"))
    data = {}
    w(":WAVeform:WIDTh BYTE")
    for c in CHANS:
        w(f":WAVeform:SOURce {c}")
        total = int(float(s.query(":ACQuire:POINts?")))
        buf = bytearray()
        start = 0
        while start < total:
            n = min(5_000_000, total - start)
            w(f":WAVeform:STARt {start}"); w(f":WAVeform:POINt {n}")
            w(":WAVeform:DATA?")
            raw = s.read_raw()
            i = raw.index(b"#"); nd = int(raw[i + 1:i + 2]); ln = int(raw[i + 2:i + 2 + nd])
            buf += raw[i + 2 + nd:i + 2 + nd + ln]
            start += n
        data[c] = np.frombuffer(bytes(buf), dtype=np.int8).astype(np.int16)
        print(f"{c} ({CHANS[c]}): {len(data[c])} points")
    np.savez_compressed(NPZ, srate=srate, **data)
    return srate, data


def crc16(b, crc=0xFFFF):
    for x in b:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def edges(v, falling=True):
    lo, hi = np.percentile(v, 2), np.percentile(v, 98)
    th = (lo + hi) / 2
    b = v < th
    if hi - lo < 10:
        return None, b
    return (np.flatnonzero(~b[:-1] & b[1:]) if falling else np.flatnonzero(b[:-1] & ~b[1:])), b


def analyse(srate, data):
    dt = 1.0 / srate
    print(f"\nsample interval {dt * 1e9:.0f} ns, window {len(data['C1']) * dt * 1e3:.1f} ms")
    for c in data:
        v = data[c]
        print(f"{c} {CHANS[c]:6s} codes min {v.min()} max {v.max()} (2%/98%: {np.percentile(v,2):.0f}/{np.percentile(v,98):.0f})")

    fi, bi = edges(data["C2"])
    if fi is not None:
        print("INDEX falling edges at ms:", [round(x * dt * 1e3, 2) for x in fi[:6]],
              " low width ms:", [round((np.argmax(~bi[x+1:]) + 1) * dt * 1e3, 2) for x in fi[:3]])

    for c in ("C1", "C3"):
        if c not in data:
            continue
        fe, b = edges(data[c])
        if fe is None or len(fe) < 100:
            print(f"\n{CHANS[c]}: no pulses (flat line)")
            continue
        widths = np.array([np.argmax(~b[x + 1:x + 200]) + 1 for x in fe[:5000]]) * dt * 1e9
        gaps = np.diff(fe) * dt * 1e6
        print(f"\n{CHANS[c]}: {len(fe)} falling edges; low-pulse width ns median {np.median(widths):.0f} "
              f"(min {widths.min():.0f}, max {widths.max():.0f}); fraction low {b.mean():.3f}")
        h, e = np.histogram(gaps, bins=np.arange(0, 10.25, 0.25))
        print("  gap histogram (us):", " ".join(f"{e[i]:.2f}:{h[i]}" for i in range(len(h)) if h[i]))

    # decode MFM from RDATA (C1): each gap = n cells of 1 us
    fe, _ = edges(data["C1"])
    if fe is None or len(fe) < 1000:
        return
    gaps = np.diff(fe) * dt * 1e6
    cells = []
    for g in gaps:
        n = int(round(g))
        if n < 2 or n > 4:
            cells.append(None); continue
        cells += [0] * (n - 1) + [1]
    bits = "".join("x" if c is None else str(c) for c in cells)
    ok_ids, bad_ids, found = 0, 0, []
    i = bits.find("0100010010001001")
    while i >= 0:
        j = i
        while bits[j:j + 16] == "0100010010001001":
            j += 16
        nsync = (j - i) // 16

        def byte(k):
            s = bits[k:k + 16]
            return None if len(s) < 16 or "x" in s else int(s[1::2], 2)
        if nsync >= 3:
            am = byte(j)
            if am == 0xFE:
                f = [byte(j + 16 * k) for k in range(7)]
                if None not in f:
                    good = crc16([0xA1] * 3 + f[:5]) == (f[5] << 8 | f[6])
                    found.append((f[1], f[2], f[3], good))
                    ok_ids += good; bad_ids += not good
        i = bits.find("0100010010001001", j)
    print(f"\ndecoded ID fields: {ok_ids} good CRC, {bad_ids} bad CRC")
    print("  (cyl, head, sector, crc_ok):", found[:24])

    # data fields: after each ID, the next A1A1A1 + FB, 512 bytes, CRC
    def byte_at(k):
        s = bits[k:k + 16]
        return None if len(s) < 16 or "x" in s else int(s[1::2], 2)
    SYNC = "0100010010001001"
    dres = []
    i = bits.find(SYNC * 3)
    while i >= 0:
        j = i + 48
        if byte_at(j) == 0xFE:
            idf = [byte_at(j + 16 * k) for k in range(5)]
            d = bits.find(SYNC * 3, j + 16 * 7)
            if d > 0 and byte_at(d + 48) == 0xFB:
                p = d + 64
                body = [byte_at(p + 16 * k) for k in range(514)]
                if None in body:
                    dres.append((idf[1:4], "undecodable", body.index(None)))
                else:
                    good = crc16([0xA1] * 3 + [0xFB] + body[:512]) == (body[512] << 8 | body[513])
                    dres.append((idf[1:4], "ok" if good else "BAD CRC", body[:4]))
        i = bits.find(SYNC * 3, j)
    print(f"\ndata fields: {sum(r[1] == 'ok' for r in dres)} ok, "
          f"{sum(r[1] != 'ok' for r in dres)} bad, of {len(dres)}")
    for r in dres:
        if r[1] != "ok":
            print("  ", r)

    # locate impossible gaps (MFM only allows 2/3/4 us)
    bad_g = [(k, round(g, 2)) for k, g in enumerate(gaps) if not (1.6 < g < 4.4)]
    print(f"\ngaps outside 2-4 us: {len(bad_g)}")
    for k, g in bad_g[:10]:
        t_ms = fe[k] * dt * 1e3
        print(f"   edge #{k} at {t_ms:.3f} ms: gap {g} us; neighbours {[round(x, 2) for x in gaps[max(0, k - 3):k + 4]]}")


if __name__ == "__main__":
    if "--analyse" in sys.argv:
        z = np.load(NPZ)
        analyse(float(z["srate"]), {k: z[k] for k in CHANS if k in z})
    else:
        analyse(*capture())
