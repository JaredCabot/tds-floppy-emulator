"""tds.py - send commands to the TDS scope over GPIB and print the replies.

Usage:  python tools/tds.py "CMD1" "CMD2?" ...
Commands ending in '?' are queried; others are written. Prints timing, then
*ESR? and ALLEV? so any error the scope raised is visible.
Address override: set TDS_ADDR (default GPIB0::1::INSTR, the TDS 794D).
"""
import os, sys, time
import pyvisa

addr = os.environ.get("TDS_ADDR", "GPIB0::1::INSTR")
s = pyvisa.ResourceManager().open_resource(addr)
s.timeout = 60000  # floppy operations are slow


def run(cmd):
    t = time.time()
    try:
        if cmd.rstrip().endswith("?"):
            r = s.query(cmd).strip()
        else:
            s.write(cmd)
            s.query("*OPC?")  # wait for the command to finish
            r = "(ok)"
    except Exception as e:
        r = f"** {type(e).__name__}: {str(e)[:100]}"
    print(f"{cmd:32s} -> {r[:300]}   ({time.time() - t:.1f}s)")


s.write("*CLS")
for c in sys.argv[1:]:
    run(c)
run("*ESR?")
run("ALLEV?")
