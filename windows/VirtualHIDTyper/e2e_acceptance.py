"""VirtualHIDTyper.exe end-to-end acceptance on real ESP32-S3 hardware.

What this proves (all with the SHIPPING EXE and the real board):
  * the EXE starts as a normal, non-elevated user (no UAC, no TESTSIGNING);
  * a global hotkey pressed on the user's keyboard starts typing while the
    main window is hidden in the tray, without stealing the target's focus;
  * the ESP32's real USB HID keyboard types the exact text into a real
    application, including newlines and TAB;
  * the stop hotkey cancels mid-run, the device stops sending keys, and the
    text already delivered is a clean prefix;
  * repeat start/stop cycles leave no stuck state;
  * losing the foreground window stops typing automatically;
  * killing the app mid-run still stops the device (firmware host lease).

The hotkey chord is delivered through the OS input layer (keybd_event), which
is the same path a physical chord takes into RegisterHotKey -> WM_HOTKEY.
No physical key can be pressed by this harness; that is the one part of the
user-visible flow that is simulated rather than physically pressed.

Usage:  python e2e_acceptance.py [--port COM6] [--keep]
Exit 0 = every check passed.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time

import serial

import e2e_win as w

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "bin", "VirtualHIDTyper.exe")
DATA_DIR = os.path.join(HERE, "e2e_data")
SMOKE = os.path.join(DATA_DIR, "smoke.log")
APPDATA_DIR = os.path.join(os.environ["APPDATA"], "VirtualHIDTyper")

VK_DELETE = 0x2E
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" :: {detail}" if detail else ""))
    return ok


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def write_profile(text, port):
    os.makedirs(DATA_DIR, exist_ok=True)
    ini = (f"[Main]\nInputMode=esp32\nEsp32Port={port}\n"
           f"IntervalMs=30\nShiftEnter=1\n")
    with open(os.path.join(DATA_DIR, "settings.ini"), "w", encoding="utf-8") as f:
        f.write(ini)
    with open(os.path.join(DATA_DIR, "text.txt"), "w", encoding="utf-16-le") as f:
        f.write("\ufeff" + text)


def start_app(smoke=False):
    """Start the shipping EXE against the isolated profile.

    NOTE: --smoke makes the app close itself after 2 s (TIMER_SMOKE), so it is
    only used for the startup-diagnostics run, never for functional tests."""
    p = subprocess.Popen([EXE, f"--data-dir={DATA_DIR}"] +
                         (["--smoke", SMOKE] if smoke else []))
    hwnd = None
    for _ in range(60):
        time.sleep(0.25)
        hwnd = w.find_main_window(p.pid)
        if hwnd:
            break
    return p, hwnd


def run_smoke():
    if os.path.exists(SMOKE):
        os.remove(SMOKE)
    p, _ = start_app(smoke=True)
    try:
        p.wait(timeout=20)
    except Exception:
        p.kill()
    time.sleep(0.3)
    return smoke_lines()


def hide_to_tray(hwnd):
    """WM_CLOSE is the app's documented 'hide to tray' path (it saves state and
    keeps running with hotkeys)."""
    user32 = w.user32
    user32.PostMessageW(hwnd, 0x0010, 0, 0)   # WM_CLOSE
    time.sleep(0.8)
    return not user32.IsWindowVisible(hwnd)


def stop_app(p):
    try:
        if p.poll() is None:
            p.kill()
    except Exception:
        pass
    time.sleep(0.6)


def open_notepad():
    """Launch Notepad and return its real top-level window.

    On Windows 11 notepad.exe is a packaged app: the spawned process is only a
    launcher, so the window must be located by class name, not by PID."""
    existing = set(w.windows_by_class("Notepad"))
    p = subprocess.Popen(["notepad.exe"])
    hwnd = None
    for _ in range(40):
        time.sleep(0.25)
        new = [h for h in w.windows_by_class("Notepad") if h not in existing]
        if new:
            hwnd = new[-1]
            break
    if hwnd:
        w.activate(hwnd)
    return p, hwnd


def clear_notepad(hwnd):
    w.activate(hwnd)
    w.send_ctrl(0x41)
    time.sleep(0.08)
    w.key_event(VK_DELETE)
    w.key_event(VK_DELETE, up=True)
    time.sleep(0.08)


def read_notepad(hwnd):
    w.activate(hwnd)
    w.send_ctrl(0x41)
    time.sleep(0.1)
    w.send_ctrl(0x43)
    time.sleep(0.15)
    return w.read_clipboard().replace("\r\n", "\n")


def stable_read(hwnd, settle=0.4, tries=12):
    prev = read_notepad(hwnd)
    for _ in range(tries):
        time.sleep(settle)
        cur = read_notepad(hwnd)
        if cur == prev:
            return cur
        prev = cur
    return prev


def smoke_lines():
    if not os.path.exists(SMOKE):
        return []
    with open(SMOKE, "r", encoding="utf-8", errors="replace") as f:
        return [l.rstrip("\n") for l in f]


def guard_focus(hwnd, what):
    """Refuse to send the start chord unless the intended target owns focus.

    Typing goes to whatever is in front, so a failed activation must abort the
    test instead of spraying keystrokes into an unrelated application."""
    fg = w.foreground()
    if fg != hwnd:
        raise RuntimeError(f"{what}: target is not in the foreground "
                           f"(fg={fg} want={hwnd})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM6")
    ap.add_argument("--keep", action="store_true", help="leave the app running")
    args = ap.parse_args()

    print("== VirtualHIDTyper + ESP32-S3 USB HID acceptance ==")
    if not os.path.exists(EXE):
        print(f"EXE missing: {EXE}")
        return 1
    print(f"   EXE: {EXE} ({os.path.getsize(EXE)} bytes)")

    # --- protect the real user profile -----------------------------------
    before = {}
    for name in ("settings.ini", "text.txt"):
        p = os.path.join(APPDATA_DIR, name)
        if os.path.exists(p):
            before[name] = (sha256(p), os.path.getsize(p))
    print(f"   real profile hashes captured: {before}")

    np, np_hwnd = open_notepad()
    if not np_hwnd:
        print("FATAL: could not open the Notepad target window")
        return 1
    print(f"   target window hwnd={np_hwnd}")

    app = None
    try:
        # ---------- 1. startup diagnostics ----------
        print("\n-- 1. startup diagnostics (isolated profile, --smoke) --")
        write_profile("warmup\n", args.port)
        lines = run_smoke()
        for l in lines:
            print(f"      {l}")
        check("start hotkey registered",
              any("start hotkey" in l and "registered=1" in l for l in lines),
              "Ctrl+Alt+Shift+F9")
        check("stop hotkey registered",
              any("stop hotkey" in l and "registered=1" in l for l in lines),
              "Ctrl+Alt+Shift+F10")
        check("tray icon created", any("tray icon added" in l for l in lines))
        check("ESP32 control port auto-detected",
              any("esp32 ports=" in l and "COM" in l for l in lines),
              next((l for l in lines if "esp32 ports=" in l), ""))
        check("legacy kernel device is unavailable to a normal user "
              "(ESP32 mode must not need it)",
              any("device open -> result=2" in l for l in lines),
              next((l for l in lines if "device open" in l), ""))

        # ---------- 2. normal-user launch ----------
        print("\n-- 2. launch as a normal user (no UAC) --")
        app, hwnd = start_app()
        check("EXE starts and stays running", app.poll() is None, f"pid={app.pid}")
        check("main window created", hwnd is not None, str(hwnd))
        elev = w.is_elevated(app.pid)
        check("process runs NON-elevated (asInvoker)", elev is False, f"elevated={elev}")
        hidden = hide_to_tray(hwnd) if hwnd else False
        check("window hides to tray and keeps running", hidden and app.poll() is None,
              f"hidden={hidden} alive={app.poll() is None}")

        # ---------- 3. exact typing through the start hotkey ----------
        print("\n-- 3. start hotkey -> exact HID text (window hidden) --")
        text = ("ESP32 native USB HID 0123456789\n"
                "Line2 TAB:\there\n"
                "!@#$%^&*()_+-=[]{};':\",./<>? `~\\| end")
        stop_app(app)
        write_profile(text, args.port)
        app, hwnd = start_app()
        check("app restarted for the exact-text run", app.poll() is None)
        hide_to_tray(hwnd)
        clear_notepad(np_hwnd)
        guard_focus(np_hwnd, "exact-text run")
        check("target window holds the foreground before START", True, str(np_hwnd))
        w.send_hotkey(w.VK_F9)
        time.sleep(len(text) * 0.032 + 3.0)
        check("target window kept the foreground during typing",
              w.foreground() == np_hwnd, str(w.foreground()))
        typed = stable_read(np_hwnd)
        check("HID text matches the payload exactly", typed == text,
              f"len {len(typed)} vs {len(text)}")
        if typed != text:
            for i, (a, b) in enumerate(zip(typed, text)):
                if a != b:
                    print(f"      first diff at {i}: got {a!r} want {b!r}")
                    break
        check("newlines delivered", typed.count("\n") == text.count("\n"),
              str(typed.count("\n")))
        check("TAB delivered", typed.count("\t") == text.count("\t"),
              str(typed.count("\t")))
        check("app still alive after a completed run", app.poll() is None)

        # ---------- 4. stop hotkey cancels mid-run ----------
        print("\n-- 4. stop hotkey cancels mid-run --")
        long_text = "".join(f"{i:03d}" for i in range(120))   # 360 chars
        stop_app(app)
        write_profile(long_text, args.port)
        app, hwnd = start_app()
        hide_to_tray(hwnd)
        clear_notepad(np_hwnd)
        guard_focus(np_hwnd, "cancel run")
        w.send_hotkey(w.VK_F9)
        time.sleep(1.0)
        t_stop = time.time()
        w.send_hotkey(w.VK_F10)
        stop_rtt = (time.time() - t_stop) * 1000
        time.sleep(1.5)
        first = read_notepad(np_hwnd)
        time.sleep(1.5)
        second = read_notepad(np_hwnd)
        check("stop hotkey stopped the run", len(first) < len(long_text),
              f"{len(first)}/{len(long_text)} chars")
        check("no characters arrive after the stop", first == second,
              f"{len(first)} vs {len(second)}")
        check("delivered text is a clean prefix", long_text.startswith(first),
              repr(first[-16:]))
        check("app survived the cancel", app.poll() is None)
        print(f"   stop chord delivered in {stop_rtt:.0f} ms; "
              f"{len(first)} of {len(long_text)} chars were typed")

        # ---------- 5. repeat start/stop cycles ----------
        print("\n-- 5. repeated start/stop cycles --")
        lens = []
        ok_all = True
        for i in range(3):
            clear_notepad(np_hwnd)
            guard_focus(np_hwnd, f"cycle {i + 1}")
            w.send_hotkey(w.VK_F9)
            time.sleep(0.7)
            w.send_hotkey(w.VK_F10)
            time.sleep(1.2)
            got = read_notepad(np_hwnd)
            lens.append(len(got))
            ok_all &= long_text.startswith(got) and 0 < len(got) < len(long_text)
        check("3 start/stop cycles each typed a clean prefix", ok_all, str(lens))
        check("app still alive after cycles", app.poll() is None)
        clear_notepad(np_hwnd)
        guard_focus(np_hwnd, "full run after cycles")
        w.send_hotkey(w.VK_F9)
        time.sleep(len(long_text) * 0.032 + 3.0)
        full = stable_read(np_hwnd)
        check("full run after cycles is exact", full == long_text,
              f"{len(full)}/{len(long_text)}")

        # ---------- 6. focus-loss protection ----------
        print("\n-- 6. focus loss during typing stops the run --")
        clear_notepad(np_hwnd)
        guard_focus(np_hwnd, "focus-loss run")
        w.send_hotkey(w.VK_F9)
        time.sleep(0.8)
        other, other_hwnd = open_notepad()
        w.activate(other_hwnd)
        time.sleep(1.5)
        after_focus = read_notepad(other_hwnd)
        time.sleep(1.5)
        after_focus2 = read_notepad(other_hwnd)
        check("typing stops when the target loses focus", after_focus == after_focus2,
              f"{len(after_focus)} vs {len(after_focus2)}")
        # Real HID cannot lock a window: the app detects the switch on its next
        # 250 ms status poll, so a few characters can land in the new window.
        # What matters is that the spill is bounded and then stops completely.
        check("focus-loss spill is bounded to the detection window",
              len(after_focus) <= 12,
              f"{len(after_focus)} chars spilled before the cancel")
        print(f"   {len(after_focus)} chars reached the new foreground window "
              f"before the focus-loss cancel landed")
        check("app survived the focus-loss abort", app.poll() is None)
        try:
            other.kill()
        except Exception:
            pass

        # ---------- 7. host death -> firmware lease ----------
        print("\n-- 7. app killed mid-run -> device stops on its own --")
        clear_notepad(np_hwnd)
        guard_focus(np_hwnd, "host-death run")
        w.send_hotkey(w.VK_F9)
        time.sleep(0.8)
        app.kill()
        app.wait(timeout=5)
        time.sleep(0.4)
        # The app no longer owns the port, so the device can be read directly.
        # IMPORTANT: any valid frame refreshes the host lease, so we must stay
        # completely silent for longer than HOST_LEASE_TIMEOUT_MS (3000 ms)
        # before the first status read -- polling here would keep the orphaned
        # task alive and mask the very behaviour under test.
        from e2e_device import Link, parse_status, STATE_NAMES
        print("   waiting 4.5 s in silence for the firmware lease to expire...")
        time.sleep(4.5)
        link = Link(args.port)
        try:
            st = parse_status(link.req(0x06))
            check("device aborted the orphaned host task",
                  st and st["state"] == 5, STATE_NAMES.get(st["state"]) if st else "none")
            if st:
                check("abort reason is the host lease", st["reason"] == 3, str(st["reason"]))
                check("abort happened before the text finished",
                      st["processed"] < st["total"], f"{st['processed']}/{st['total']}")
            frozen = st["processed"] if st else 0
            time.sleep(1.2)
            st2 = parse_status(link.req(0x06))
            check("no keys after the orphaned task was aborted",
                  st2 and st2["processed"] == frozen, f"{st2['processed']} vs {frozen}")
            typed_after = read_notepad(np_hwnd)
            check("delivered text after host death matches device progress",
                  len(typed_after) == frozen, f"{len(typed_after)} vs {frozen}")
        finally:
            link.close()
        app = None

        # ---------- 8. user data untouched ----------
        print("\n-- 8. real user profile untouched --")
        after = {}
        for name in ("settings.ini", "text.txt"):
            p = os.path.join(APPDATA_DIR, name)
            if os.path.exists(p):
                after[name] = (sha256(p), os.path.getsize(p))
        check("real %APPDATA%\\VirtualHIDTyper files unchanged", before == after,
              f"{before} -> {after}")

    finally:
        if app is not None and not args.keep:
            stop_app(app)
        try:
            np.kill()
        except Exception:
            pass

    passed = sum(1 for _, ok, _ in results if ok)
    failed = [n for n, ok, _ in results if not ok]
    print(f"\n== {passed}/{len(results)} checks passed ==")
    if failed:
        print("FAILED: " + ", ".join(failed))
        return 1
    print("ALL APPLICATION ACCEPTANCE CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
