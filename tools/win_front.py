"""Bring the Eden window to the foreground (no monitor move, no focus-stealing hacks).

    python tools/win_front.py

Only activates the window; it does NOT move it to another screen and does not
change the emulator configuration.
"""
import ctypes
import ctypes.wintypes as wt
import subprocess

u = ctypes.windll.user32
WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_ulong, wt.LPARAM)


def eden_pids():
    out = subprocess.check_output(
        ['tasklist', '/FI', 'IMAGENAME eq eden.exe', '/FO', 'CSV', '/NH'],
        text=True, errors='replace')
    pids = set()
    for line in out.splitlines():
        parts = [p.strip('" ') for p in line.split('","')]
        if len(parts) > 1 and parts[1].isdigit():
            pids.add(int(parts[1]))
    return pids


def main():
    pids = eden_pids()
    found = []
    pid = wt.DWORD()

    def cb(hwnd, _):
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value not in pids or not u.IsWindowVisible(hwnd):
            return 1
        n = u.GetWindowTextLengthW(hwnd)
        if n <= 0:
            return 1
        buf = ctypes.create_unicode_buffer(n + 1)
        u.GetWindowTextW(hwnd, buf, n + 1)
        r = wt.RECT()
        u.GetWindowRect(hwnd, ctypes.byref(r))
        found.append((hwnd, buf.value, (r.left, r.top, r.right, r.bottom)))
        return 1

    u.EnumWindows(WNDENUMPROC(cb), 0)
    if not found:
        print('no visible Eden window (pids=%s)' % sorted(pids))
        return 1
    for hwnd, title, rect in found:
        print('hwnd=%s title=%r rect=%s' % (hwnd, title, rect))
    # prefer the largest window: the main render window, not a helper dialog
    hwnd, title, _ = max(found, key=lambda f: (f[2][2] - f[2][0]) *
                         (f[2][3] - f[2][1]))
    if u.IsIconic(hwnd):
        u.ShowWindow(hwnd, 9)          # SW_RESTORE
    u.BringWindowToTop(hwnd)
    ok = u.SetForegroundWindow(hwnd)
    print('activated hwnd=%s title=%r SetForegroundWindow=%s' %
          (hwnd, title, bool(ok)))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
