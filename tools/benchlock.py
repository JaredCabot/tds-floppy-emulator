"""benchlock.py - only one bench tool drives the scope / emulator at a time.

Two tools saving to the scope while another rebuilds the emulator's disk corrupted
the disk and hung the scope (2026-09-24). Tools that make the scope write, or
change the emulator's disk, call acquire() first: it writes captures/bench.lock
(PID + tool name) and refuses if another LIVE process holds it. A lock left by a
crashed run is ignored. Released automatically at exit.
The PowerShell tools do the same check (see button.ps1, reformat.ps1).
"""
import atexit, ctypes, os, sys

LOCK = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "captures", "bench.lock")


def _alive(pid):
    # os.kill(pid, 0) would TERMINATE the process on Windows; ask Win32 instead
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x1000, False, pid)        # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return False
    code = ctypes.c_ulong()
    ok = k.GetExitCodeProcess(h, ctypes.byref(code))
    k.CloseHandle(h)
    return bool(ok) and code.value == 259         # STILL_ACTIVE


def acquire(name):
    try:
        pid, other = open(LOCK).read().split(" ", 1)
        if int(pid) != os.getpid() and _alive(int(pid)):
            sys.exit(f"BENCH BUSY: '{other.strip()}' (pid {pid}) is using the scope/emulator. Not starting {name}.")
    except (FileNotFoundError, ValueError):
        pass
    os.makedirs(os.path.dirname(LOCK), exist_ok=True)
    open(LOCK, "w").write(f"{os.getpid()} {name}")
    atexit.register(release)


def release():
    try:
        if open(LOCK).read().split(" ", 1)[0] == str(os.getpid()):
            os.remove(LOCK)
    except (FileNotFoundError, ValueError):
        pass
