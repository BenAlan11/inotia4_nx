"""Put the emulator window on a chosen monitor without touching the others.

The agent launches Eden to test the port, and a window that pops up on the
primary screen (or grabs focus) interrupts whatever the user is doing there.
This helper enumerates monitors, finds the emulator's top-level window by
process name and moves it onto the requested monitor -- with SWP_NOACTIVATE, so
the window does not steal focus and nothing else gets minimised.

Usage:
    python tools/win_place.py                 # move eden windows to monitor 2
    python tools/win_place.py --monitor 2 --watch 20
    python tools/win_place.py --list          # just print monitors/windows
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import sys
import time

user32 = ctypes.windll.user32

SWP_NOZORDER = 0x0004
SWP_NOACTIVATE = 0x0010
SWP_SHOWWINDOW = 0x0040
SWP_FRAMECHANGED = 0x0020

MONITORENUMPROC = ctypes.WINFUNCTYPE(
    ctypes.c_int, ctypes.c_ulong, ctypes.c_ulong, ctypes.POINTER(wt.RECT),
    ctypes.c_double)
WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_ulong, wt.LPARAM)


class MONITORINFO(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT),
                ("rcWork", wt.RECT), ("dwFlags", wt.DWORD)]


def monitors():
    found = []

    def cb(hmon, hdc, lprect, data):
        mi = MONITORINFO()
        mi.cbSize = ctypes.sizeof(MONITORINFO)
        if user32.GetMonitorInfoW(hmon, ctypes.byref(mi)):
            found.append((mi.rcMonitor.left, mi.rcMonitor.top,
                          mi.rcMonitor.right - mi.rcMonitor.left,
                          mi.rcMonitor.bottom - mi.rcMonitor.top,
                          bool(mi.dwFlags & 1)))
        return 1

    user32.EnumDisplayMonitors(0, None, MONITORENUMPROC(cb), 0)
    # the user counts the primary screen as #1, so put it first regardless of
    # the order Windows happens to enumerate in
    found.sort(key=lambda m: (0 if m[4] else 1, m[0]))
    return found


def windows(exe_names):
    """visible top-level windows whose process image matches one of the names"""
    import os
    out = []
    pids = {}
    for name in exe_names:
        os.system("")  # keep subprocess output clean
    import subprocess
    task = subprocess.run(["tasklist", "/FO", "CSV", "/NH"],
                          capture_output=True, text=True).stdout
    for line in task.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower() in exe_names:
            pids[int(parts[1])] = parts[0]

    def cb(hwnd, data):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            out.append((hwnd, pid.value, buf.value))
        return 1

    user32.EnumWindows(WNDENUMPROC(cb), 0)
    return out


def place(hwnd, x, y, w, h):
    user32.SetWindowPos(hwnd, 0, int(x), int(y), int(w), int(h),
                        SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW |
                        SWP_FRAMECHANGED)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--monitor", type=int, default=2,
                    help="1-based monitor index to move the window to")
    ap.add_argument("--exe", default="eden-cli.exe,eden.exe")
    ap.add_argument("--watch", type=float, default=0,
                    help="keep watching for this many seconds")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    mons = monitors()
    wins = windows([e.strip().lower() for e in args.exe.split(",")])
    if args.list:
        for i, m in enumerate(mons, 1):
            print("monitor %d: %dx%d at %d,%d primary=%s" %
                  (i, m[2], m[3], m[0], m[1], m[4]))
        for hwnd, pid, title in wins:
            r = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(r))
            print("window 0x%x pid %d '%s' at %d,%d %dx%d" %
                  (hwnd, pid, title, r.left, r.top, r.right - r.left,
                   r.bottom - r.top))
        return 0

    if len(mons) < args.monitor:
        print("only %d monitor(s) present, nothing to do" % len(mons))
        return 1
    mx, my, mw, mh, _ = mons[args.monitor - 1]

    deadline = time.time() + args.watch
    moved = set()
    while True:
        wins = windows([e.strip().lower() for e in args.exe.split(",")])
        for hwnd, pid, title in wins:
            if hwnd in moved:
                continue
            r = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(r))
            w = r.right - r.left
            h = r.bottom - r.top
            if w <= 0 or h <= 0 or w > mw * 2 or h > mh * 2:
                w, h = min(mw, 1280), min(mh, 720)
            place(hwnd, mx + 40, my + 40, min(w, mw - 80), min(h, mh - 80))
            moved.add(hwnd)
            print("moved 0x%x '%s' to monitor %d at %d,%d" %
                  (hwnd, title, args.monitor, mx + 40, my + 40))
        if time.time() >= deadline:
            break
        time.sleep(0.5)
    return 0


if __name__ == "__main__":
    sys.exit(main())
