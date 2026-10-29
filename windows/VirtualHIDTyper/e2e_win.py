"""Win32 helpers for the VirtualHIDTyper end-to-end acceptance tests.

Everything here uses ctypes only: this environment blocks PowerShell
Add-Type / COM object creation, so the older .ps1 harness could not run.
No third-party packages are required.
"""
import ctypes
import ctypes.wintypes as wt
import time

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)

user32.FindWindowW.restype = wt.HWND
user32.FindWindowW.argtypes = [wt.LPCWSTR, wt.LPCWSTR]
user32.GetForegroundWindow.restype = wt.HWND
user32.SetForegroundWindow.argtypes = [wt.HWND]
user32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
user32.IsWindow.argtypes = [wt.HWND]
user32.GetWindowThreadProcessId.argtypes = [wt.HWND, ctypes.POINTER(wt.DWORD)]
user32.GetWindowThreadProcessId.restype = wt.DWORD
user32.AttachThreadInput.argtypes = [wt.DWORD, wt.DWORD, wt.BOOL]
user32.BringWindowToTop.argtypes = [wt.HWND]
user32.SetFocus.argtypes = [wt.HWND]
user32.GetWindowTextW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]
user32.GetWindowTextLengthW.argtypes = [wt.HWND]
user32.GetClassNameW.argtypes = [wt.HWND, wt.LPWSTR, ctypes.c_int]

SW_RESTORE = 9
VK_CONTROL, VK_MENU, VK_SHIFT = 0x11, 0x12, 0x10
VK_F9, VK_F10 = 0x78, 0x79
KEYEVENTF_KEYUP = 0x0002


def foreground():
    return user32.GetForegroundWindow()


def activate(hwnd):
    """Bring `hwnd` to the foreground, working around the foreground lock."""
    if not hwnd:
        return False
    user32.ShowWindow(hwnd, SW_RESTORE)
    fg = user32.GetForegroundWindow()
    if fg == hwnd:
        return True
    cur_tid = kernel32.GetCurrentThreadId()
    fg_tid = user32.GetWindowThreadProcessId(fg, None) if fg else 0
    tgt_tid = user32.GetWindowThreadProcessId(hwnd, None)
    attached = []
    for tid in (fg_tid, tgt_tid):
        if tid and tid != cur_tid and tid not in attached:
            if user32.AttachThreadInput(cur_tid, tid, True):
                attached.append(tid)
    try:
        user32.BringWindowToTop(hwnd)
        user32.SetForegroundWindow(hwnd)
        user32.SetFocus(hwnd)
    finally:
        for tid in attached:
            user32.AttachThreadInput(cur_tid, tid, False)
    time.sleep(0.15)
    return user32.GetForegroundWindow() == hwnd


def key_event(vk, up=False):
    user32.keybd_event(vk, 0, KEYEVENTF_KEYUP if up else 0, 0)


def send_hotkey(vk):
    """Ctrl+Alt+Shift+<vk> through the OS input layer (same path a physical
    chord takes: the shell turns it into WM_HOTKEY for the registered app)."""
    for k in (VK_CONTROL, VK_MENU, VK_SHIFT):
        key_event(k)
    time.sleep(0.03)
    key_event(vk)
    time.sleep(0.05)
    key_event(vk, up=True)
    for k in (VK_SHIFT, VK_MENU, VK_CONTROL):
        key_event(k, up=True)
    time.sleep(0.05)


def send_ctrl(vk):
    key_event(VK_CONTROL)
    time.sleep(0.02)
    key_event(vk)
    time.sleep(0.03)
    key_event(vk, up=True)
    key_event(VK_CONTROL, up=True)


def send_keys(text):
    """Type ASCII text with keybd_event (used only for harness self-checks)."""
    for ch in text:
        vk = user32.VkKeyScanW(ord(ch)) & 0xFF
        shift = (user32.VkKeyScanW(ord(ch)) >> 8) & 0xFF
        if shift & 1:
            key_event(VK_SHIFT)
        key_event(vk)
        key_event(vk, up=True)
        if shift & 1:
            key_event(VK_SHIFT, up=True)
        time.sleep(0.01)


def window_text(hwnd):
    n = user32.GetWindowTextLengthW(hwnd)
    buf = ctypes.create_unicode_buffer(n + 2)
    user32.GetWindowTextW(hwnd, buf, n + 2)
    return buf.value


CF_UNICODETEXT = 13


def read_clipboard():
    user32.OpenClipboard.argtypes = [wt.HWND]
    user32.GetClipboardData.restype = wt.HANDLE
    user32.GetClipboardData.argtypes = [wt.UINT]
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = [wt.HANDLE]
    kernel32.GlobalUnlock.argtypes = [wt.HANDLE]

    for _ in range(20):
        if user32.OpenClipboard(None):
            break
        time.sleep(0.05)
    else:
        raise RuntimeError("OpenClipboard failed")
    try:
        h = user32.GetClipboardData(CF_UNICODETEXT)
        if not h:
            return ""
        p = kernel32.GlobalLock(h)
        if not p:
            return ""
        try:
            return ctypes.wstring_at(p)
        finally:
            kernel32.GlobalUnlock(h)
    finally:
        user32.CloseClipboard()


# ---- process elevation -----------------------------------------------------
TOKEN_QUERY = 0x0008
TokenElevation = 20
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


class TOKEN_ELEVATION(ctypes.Structure):
    _fields_ = [("TokenIsElevated", wt.DWORD)]


def is_elevated(pid):
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return None
    try:
        tok = wt.HANDLE()
        if not advapi32.OpenProcessToken(h, TOKEN_QUERY, ctypes.byref(tok)):
            return None
        try:
            info = TOKEN_ELEVATION()
            size = wt.DWORD(ctypes.sizeof(info))
            ok = advapi32.GetTokenInformation(
                tok, TokenElevation, ctypes.byref(info), size, ctypes.byref(size))
            if not ok:
                return None
            return bool(info.TokenIsElevated)
        finally:
            kernel32.CloseHandle(tok)
    finally:
        kernel32.CloseHandle(h)


# ---- process / window discovery -------------------------------------------
def find_main_window(pid, exclude=()):
    """First visible top-level window owned by `pid`."""
    found = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        wpid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return found[0] if found else None


def windows_by_class(cls_name):
    """All visible top-level windows whose class name matches `cls_name`.

    notepad.exe on Windows 11 is a packaged app: the launched process is only a
    launcher, so the window belongs to a different PID. Matching by class is the
    only reliable way to find the real window.
    """
    out = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        if buf.value == cls_name:
            out.append(hwnd)
        return True

    user32.EnumWindows(cb, 0)
    return out
