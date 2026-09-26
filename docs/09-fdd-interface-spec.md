# FDD Interface Spec (reference: TEAC FD-05HF-8830)

Source: `Datasheets/TEAC FD-05HF-8830 spec.pdf` (26-pin slim, 1mm FFC - same
pinout as the emulator P1 and, per the HxC forum, the drive family used in TDS
7xx scopes). All signals active LOW unless stated.

## Pinout (matches docs/02-pinmap.csv P1)
1/3/5 +5V, 2 INDEX, 4 DRIVE SELECT, 6 DISK CHANGE, 7 NC, 8 READY,
9 HD OUT (**HIGH = 2HD disk or no disk**, LOW = 2DD), 10 MOTOR ON, 11 NC,
12 DIRECTION, 13 NC, 14 STEP, 16 WDATA, 18 WGATE, 20 TRACK 00,
22 WRITE PROTECT, 24 READ DATA, 26 SIDE ONE SELECT, 15/17/23/25 0V.
Measured on the TDS: pins 9 and 11 sit at 5.0 V (host pull-ups) -> "2HD".

## Behaviours that matter for emulation
| Spec | Requirement | Firmware status |
|---|---|---|
| 8.2 | Outputs are CMOS **3-state, driven only while DRIVE SELECT** | outputs_apply(): deselected -> all released |
| 8.3.1 | All I/O except MOTOR ON valid only while selected (0.5 us) | SEL on EXINT0 (both edges); STEP ignored unless selected |
| 8.3.3/4 | Motion on **trailing** edge of STEP; DIR valid 0.8 us before it | STEP EXINT on rising edge |
| 8.3.4 | Step rate >= 3 ms same dir, >= 4 ms reversal; settle to 18 ms | TDS uses ~4-5 ms steps |
| 8.3.8 | TRACK 00 valid 2.8 ms after STEP | immediate (fine) |
| 8.3.9 | INDEX = index * selected * ready * seek-complete; width 1.5-5 ms, period 197-203 ms | 3 ms (2.3 measured), 200 ms, gated by select + ready + seek |
| 8.3.9 | seek-complete = 15.8-17.9 ms after the last STEP | SEEK_COMPLETE_MS = 17 masks INDEX + RDATA |
| 8.3.10 | READ DATA pulse 0.15-0.8 us, same gating as INDEX | 400 ns at MCU, 480 ns at connector |
| 8.3.7 | READ DATA valid 100 us after SIDE change | data hidden until track rebuilt (~20 ms) |
| 8.3.12 | DISK CHANGE true at power-on / eject; cleared by a **selected** STEP with disk in | done |
| 8.3.13 | READY: disk in + 480 ms after MOTOR ON + index seen; false 0.3 ms after motor off | asserted 480 ms after select (MOTOR ON rises with select on the TDS), dropped on deselect; RDATA/INDEX gated on it. Needs JTAG-DP off (PB3 = JTDO) |
| 4.4 | Index 200 ms +/-1.5% | 200.0 ms |
| (mech.) | Head travel beyond cylinder 79 | head may step to 82; the TDS formats + verifies cylinder 80 |
| 4.8 | Window margin 300 ns (2 MB mode) | exact 1 us cells |

## Result against the TDS 794D (2026-09-23)
Implementing the spec behaviours alone changed nothing, because READY was never
reaching the pin: **PB3 (READY) is JTDO while the JTAG port is enabled (the
reset default), so GPIO writes to it were ignored**. Fix: `gpio_pin_remap_config
(SWJTAG_GMUX_010)` = JTAG-DP off, SW-DP kept (FlashFloppy does the same).

Reference captures against the TDS's real TEAC-type drive (tools/sds_seq.py,
captures/seq_real_p3.npz vs seq_gotek_p3b.npz) showed what the TDS needs:
the drive stays silent (no READY, INDEX or READ DATA) until its motor has spun
up, then READY asserts ~487 ms after MOTOR ON and data starts. The TDS raises
MOTOR ON and DRIVE SELECT together, so the firmware starts a 480 ms spin-up
timer on select, asserts READY when it expires, gates INDEX/RDATA on READY, and
drops READY on deselect.

With both fixes the TDS runs the same sequence as with the real drive (2 rounds,
READY at +480 ms, reads head 1 -> 0 -> 1), lists TEST.BIN, and
`tools/verify_testbin.py` reads all 65,536 bytes back through the TDS's floppy
controller with **0 mismatches** (cylinders 0-4, both heads).

Lesson: check that an output actually toggles at the connector before trusting
experiments that vary it. Four "READY" experiments were testing a dead pin.
