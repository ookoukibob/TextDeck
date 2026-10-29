"""ESP32-S3 v1.2 control-protocol acceptance tests (real hardware, real HID).

Runs against the device's native USB CDC control port while the Windows tray
app is NOT running (it owns the port). The HID keyboard output is verified by
letting the device type into a Notepad window this script opens and keeps in
the foreground, then reading the text back through the clipboard.

Usage:
    python e2e_device.py [COM6]

Exit code 0 = every check passed.
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import subprocess
import sys
import time
import zlib

import serial

import e2e_win as w

MAGIC = bytes([0xE7, 0x53, 0x01])
CMD_WRITE, CMD_READ, CMD_PING = 0x01, 0x02, 0x03
CMD_START, CMD_CANCEL, CMD_STATUS = 0x04, 0x05, 0x06

ST_OK, ST_ERR_CRC, ST_ERR_LENGTH, ST_ERR_CHARSET = 0, 1, 2, 3
ST_ERR_STORAGE, ST_ERR_CMD, ST_ERR_BUSY, ST_ERR_FRAME = 4, 5, 6, 7
ST_ERR_NO_TEXT, ST_ERR_HID_NOT_READY = 8, 9

TY_IDLE, TY_PREPARING, TY_TYPING = 0, 1, 2
TY_COMPLETED, TY_CANCELLED, TY_FAILED = 3, 4, 5

MAX_TEXT = 10240
STATE_NAMES = {0: "IDLE", 1: "PREPARING", 2: "TYPING", 3: "COMPLETED",
               4: "CANCELLED", 5: "FAILED"}

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" :: {detail}" if detail else ""))
    return ok


def frame(cmd, payload=b""):
    return (MAGIC + bytes([cmd]) + struct.pack("<H", len(payload)) +
            struct.pack("<I", zlib.crc32(payload) & 0xFFFFFFFF) + payload)


def parse_frames(buf):
    out, i = [], 0
    while i + 10 <= len(buf):
        if buf[i] != 0xE7 or buf[i + 1] != 0x53 or buf[i + 2] != 0x01:
            i += 1
            continue
        cmd = buf[i + 3]
        ln = struct.unpack("<H", buf[i + 4:i + 6])[0]
        if ln > 13 + MAX_TEXT:
            i += 1
            continue
        total = 10 + ln
        if i + total > len(buf):
            break
        want = struct.unpack("<I", buf[i + 6:i + 10])[0]
        body = buf[i + 10:i + total]
        if want != (zlib.crc32(body) & 0xFFFFFFFF):
            i += 1
            continue
        out.append((cmd, body))
        i += total
    return out


class Link:
    def __init__(self, port):
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = 115200
        self.s.timeout = 0.02
        self.s.dtr = True
        self.s.rts = True
        self.s.open()
        time.sleep(0.4)
        self.s.reset_input_buffer()

    def close(self):
        self.s.close()

    def req(self, cmd, payload=b"", timeout=1.5):
        self.s.reset_input_buffer()
        self.s.write(frame(cmd, payload))
        self.s.flush()
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            d = self.s.read(4096)
            if d:
                buf += d
                for c, p in parse_frames(buf):
                    if c == (cmd | 0x80):
                        return p
            else:
                time.sleep(0.005)
        return None

    def raw(self, data, timeout=0.6):
        self.s.reset_input_buffer()
        self.s.write(data)
        self.s.flush()
        buf = b""
        t0 = time.time()
        while time.time() - t0 < timeout:
            d = self.s.read(4096)
            if d:
                buf += d
            else:
                time.sleep(0.005)
        return buf


def parse_status(p):
    if not p or len(p) < 16:
        return None
    return {
        "status": p[0], "state": p[1],
        "processed": struct.unpack("<H", p[2:4])[0],
        "total": struct.unpack("<H", p[4:6])[0],
        "err": p[6], "reason": p[7], "flags": p[8],
        "lease": struct.unpack("<H", p[10:12])[0],
        "session": struct.unpack("<H", p[12:14])[0],
        "caps": struct.unpack("<H", p[14:16])[0],
    }


def parse_ping(p):
    if not p or len(p) < 16:
        return None
    return {
        "status": p[0], "proto": p[1],
        "fw": struct.unpack("<H", p[2:4])[0],
        "max_text": struct.unpack("<H", p[4:6])[0],
        "has_text": p[6],
        "saved_len": struct.unpack("<H", p[7:9])[0],
        "saved_crc": struct.unpack("<I", p[9:13])[0],
        "seq16": struct.unpack("<H", p[13:15])[0],
        "hid_ready": p[15],
    }


def parse_write(p):
    if not p or len(p) < 14:
        return None
    return {
        "status": p[0], "detail": struct.unpack("<H", p[1:3])[0],
        "saved_len": struct.unpack("<H", p[3:5])[0],
        "saved_crc": struct.unpack("<I", p[5:9])[0],
        "seq": struct.unpack("<I", p[9:13])[0],
        "written": p[13],
    }


def read_notepad(hwnd):
    """Ctrl+A / Ctrl+C into the focused Notepad and read the clipboard."""
    if w.foreground() != hwnd:
        w.activate(hwnd)
    w.send_ctrl(0x41)   # Ctrl+A
    time.sleep(0.1)
    w.send_ctrl(0x43)   # Ctrl+C
    time.sleep(0.15)
    return w.read_clipboard()


def clear_notepad(hwnd):
    """Empty the target buffer so each typing test starts from a known state.

    Without this the previous test's text (and any leftover selection) makes
    the character-count comparison ambiguous."""
    if w.foreground() != hwnd:
        w.activate(hwnd)
    w.send_ctrl(0x41)   # Ctrl+A
    time.sleep(0.08)
    w.key_event(0x2E)   # VK_DELETE
    w.key_event(0x2E, up=True)
    time.sleep(0.08)
    return read_notepad(hwnd) == ""


def open_notepad():
    """Launch Notepad and return its real top-level window (class-based lookup:
    on Windows 11 the spawned notepad.exe is only a launcher)."""
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


def close_notepad(p):
    try:
        p.kill()
    except Exception:
        pass


def wait_state(link, want, timeout=30.0, interval=0.05):
    t0 = time.time()
    last = None
    while time.time() - t0 < timeout:
        last = parse_status(link.req(CMD_STATUS))
        if last and last["state"] in want:
            return last
        time.sleep(interval)
    return last


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
    print(f"== ESP32-S3 v1.2 protocol acceptance on {port} ==")
    np, np_hwnd = open_notepad()
    print(f"   notepad pid={np.pid} hwnd={np_hwnd}")
    link = Link(port)
    try:
        # ---------- 1. PING / identity ----------
        print("\n-- 1. PING / identity --")
        ping = parse_ping(link.req(CMD_PING))
        check("PING responds", ping is not None, str(ping))
        if ping:
            check("firmware is v1.2.0 (0x0102)", ping["fw"] == 0x0102, hex(ping["fw"]))
            check("protocol version 1", ping["proto"] == 0x01, str(ping["proto"]))
            check("text limit 10240", ping["max_text"] == MAX_TEXT, str(ping["max_text"]))
            check("USB HID enumerated (hid_ready=1)", ping["hid_ready"] == 1,
                  str(ping["hid_ready"]))

        # ---------- 2. GET_STATUS baseline ----------
        print("\n-- 2. GET_STATUS baseline --")
        st = parse_status(link.req(CMD_STATUS))
        check("GET_STATUS returns 16-byte status", st is not None, str(st))
        if st:
            check("remote-typing capability advertised",
                  st["caps"] & 0x0001 == 1, hex(st["caps"]))
            check("host-lease capability advertised",
                  st["caps"] & 0x0002 == 2, hex(st["caps"]))
            check("native-CDC capability advertised",
                  st["caps"] & 0x0004 == 4, hex(st["caps"]))
            baseline_state = st["state"]

        # ---------- 3. WRITE / READ round-trip (v1 compatibility) ----------
        print("\n-- 3. WRITE / READ round-trip --")
        body = "Protocol check 0123 !@#$%^&*()\n\ttab line\nend"
        w_ack = parse_write(link.req(CMD_WRITE, body.encode()))
        check("WRITE accepted", w_ack and w_ack["status"] == ST_OK,
              f"status={w_ack['status'] if w_ack else None}")
        if w_ack:
            check("WRITE ack saved_len matches",
                  w_ack["saved_len"] == len(body), str(w_ack["saved_len"]))
            check("WRITE ack saved_crc matches",
                  w_ack["saved_crc"] == (zlib.crc32(body.encode()) & 0xFFFFFFFF),
                  hex(w_ack["saved_crc"]))
        rd = link.req(CMD_READ)
        if rd and len(rd) >= 13:
            tlen = struct.unpack("<H", rd[3:5])[0]
            got = rd[13:13 + tlen].decode()
            check("READ returns identical text", got == body, repr(got[:40]))
        else:
            check("READ returns text", False, "no/short response")

        # ---------- 4. START / BUSY / progress / CANCEL ----------
        print("\n-- 4. START / BUSY / progress / CANCEL --")
        long_text = ("The quick brown fox jumps over the lazy dog 0123456789\n"
                     "Second line with TAB:\there\n"
                     "Third line ends with punctuation!?,.;:'\"()[]{}") * 2
        ack = parse_write(link.req(CMD_WRITE, long_text.encode()))
        check("WRITE long text accepted", ack and ack["status"] == ST_OK,
              str(ack and ack["status"]))
        total = len(long_text)
        clear_notepad(np_hwnd)
        t_start = time.time()
        s1 = parse_status(link.req(CMD_START))
        check("START accepted", s1 and s1["status"] == ST_OK,
              f"status={s1['status'] if s1 else None}")
        if s1:
            check("START reports task PREPARING/TYPING",
                  s1["state"] in (TY_PREPARING, TY_TYPING), STATE_NAMES.get(s1["state"]))
            check("START reports total length", s1["total"] == total,
                  f"{s1['total']} vs {total}")
            check("START flags host-initiated", s1["flags"] & 0x01 == 1,
                  hex(s1["flags"]))
        s2 = parse_status(link.req(CMD_START))
        check("second START while typing is BUSY",
              s2 and s2["status"] == ST_ERR_BUSY,
              f"status={s2['status'] if s2 else None}")
        if s2:
            check("BUSY reply does not restart the task", s2["session"] == s1["session"],
                  f"session {s2['session']} vs {s1['session']}")

        time.sleep(0.6)
        sp = parse_status(link.req(CMD_STATUS))
        check("GET_STATUS shows forward progress",
              sp and sp["state"] == TY_TYPING and 0 < sp["processed"] < total,
              f"{STATE_NAMES.get(sp['state'])} {sp['processed']}/{sp['total']}")
        check("GET_STATUS reports host lease remaining",
              sp and sp["lease"] > 0, str(sp and sp["lease"]))

        # cancel and measure
        proc_before = sp["processed"] if sp else 0
        t_cancel = time.time()
        sc = parse_status(link.req(CMD_CANCEL))
        dt_cancel = (time.time() - t_cancel) * 1000.0
        check("CANCEL acknowledged", sc and sc["status"] == ST_OK,
              f"status={sc['status'] if sc else None}")
        if sc:
            check("CANCEL leaves task CANCELLED", sc["state"] == TY_CANCELLED,
                  STATE_NAMES.get(sc["state"]))
            check("CANCEL reason = host", sc["reason"] == 1, str(sc["reason"]))
            check("CANCEL flags release-sent", sc["flags"] & 0x02 == 2,
                  hex(sc["flags"]))
            check("CANCEL stopped before the end", sc["processed"] < total,
                  f"{sc['processed']}/{total}")
        frozen = sc["processed"] if sc else 0
        time.sleep(1.2)
        after = parse_status(link.req(CMD_STATUS))
        check("no keys processed after CANCEL",
              after and after["processed"] == frozen,
              f"{after['processed']} vs {frozen}")
        check("state stays CANCELLED", after and after["state"] == TY_CANCELLED,
              STATE_NAMES.get(after["state"]))

        # Notepad content must match the device's processed count exactly.
        typed = read_notepad(np_hwnd)
        typed_norm = typed.replace("\r\n", "\n")
        check("HID output matches device progress exactly",
              len(typed_norm) == frozen,
              f"notepad={len(typed_norm)} device={frozen}")
        check("HID output is a prefix of the payload",
              long_text.startswith(typed_norm), repr(typed_norm[-30:]))
        print(f"   cancel round-trip (host measured): {dt_cancel:.0f} ms, "
              f"stopped at char {frozen}/{total}")

        # ---------- 5. cancel idempotent, then full completion ----------
        print("\n-- 5. cancel idempotent / full completion --")
        sc2 = parse_status(link.req(CMD_CANCEL))
        check("repeated CANCEL with no active task still OK",
              sc2 and sc2["status"] == ST_OK and sc2["state"] == TY_CANCELLED,
              f"status={sc2['status'] if sc2 else None}")

        short = "Done.\tTabbed line\nHello, World! 42 ?!,.;:'\"()[]{}\n"
        parse_write(link.req(CMD_WRITE, short.encode()))
        clear_notepad(np_hwnd)
        st_start = parse_status(link.req(CMD_START))
        check("START on short text accepted",
              st_start and st_start["status"] == ST_OK, str(st_start and st_start["status"]))
        fin = wait_state(link, (TY_COMPLETED, TY_FAILED, TY_CANCELLED), timeout=20)
        check("task reaches COMPLETED", fin and fin["state"] == TY_COMPLETED,
              STATE_NAMES.get(fin["state"]) if fin else "timeout")
        if fin:
            check("completed progress == total", fin["processed"] == fin["total"],
                  f"{fin['processed']}/{fin['total']}")
            check("completed task released keys", fin["flags"] & 0x02 == 2,
                  hex(fin["flags"]))
        typed2 = read_notepad(np_hwnd).replace("\r\n", "\n")
        check("completed HID output exact (incl. TAB and newline)", typed2 == short,
              repr(typed2))
        check("completed output contains newline and TAB",
              "\n" in typed2 and "\t" in typed2,
              f"newlines={typed2.count(chr(10))} tabs={typed2.count(chr(9))}")

        # ---------- 6. host-lease timeout ----------
        print("\n-- 6. host-lease timeout (no polling after START) --")
        lease_text = ("0123456789abcdefghij" * 30)
        parse_write(link.req(CMD_WRITE, lease_text.encode()))
        clear_notepad(np_hwnd)
        ls = parse_status(link.req(CMD_START))
        check("START accepted for lease test", ls and ls["status"] == ST_OK,
              str(ls and ls["status"]))
        t0 = time.time()
        # Deliberately silent: no GET_STATUS, no heartbeat.
        time.sleep(4.2)
        lf = parse_status(link.req(CMD_STATUS))
        check("lease timeout aborted the host task",
              lf and lf["state"] == TY_FAILED, STATE_NAMES.get(lf["state"]) if lf else "none")
        if lf:
            check("lease abort reason = lease", lf["reason"] == 3, str(lf["reason"]))
            check("lease abort error = host-lost", lf["err"] == 7, str(lf["err"]))
            check("lease abort stopped before the end", lf["processed"] < lf["total"],
                  f"{lf['processed']}/{lf['total']}")
        frozen2 = lf["processed"] if lf else 0
        time.sleep(1.0)
        lf2 = parse_status(link.req(CMD_STATUS))
        check("no keys after lease abort", lf2 and lf2["processed"] == frozen2,
              f"{lf2['processed']} vs {frozen2}")
        typed3 = read_notepad(np_hwnd)
        check("HID output after lease abort matches progress",
              len(typed3.replace("\r\n", "\n")) == frozen2,
              f"notepad={len(typed3)} device={frozen2}")
        print(f"   lease abort after {time.time() - t0:.1f}s wall clock "
              f"(configured timeout 3000 ms)")

        # ---------- 7. error paths ----------
        print("\n-- 7. protocol error paths --")
        a = parse_write(link.req(CMD_WRITE, b"bad\rchar"))
        check("CR is rejected as unsupported", a and a["status"] == ST_ERR_CHARSET,
              f"status={a['status'] if a else None}")
        if a:
            check("charset error detail points at the CR", a["detail"] == 3,
                  str(a["detail"]))
        a = parse_write(link.req(CMD_WRITE, b"x" * (MAX_TEXT + 1)))
        check("over-10 KiB text rejected", a and a["status"] == ST_ERR_LENGTH,
              f"status={a['status'] if a else None}")
        a = parse_write(link.req(CMD_WRITE, b"x" * MAX_TEXT))
        check("exactly 10 KiB accepted", a and a["status"] == ST_OK,
              f"status={a['status'] if a else None}")
        p = link.req(CMD_PING)
        check("device still healthy after 10 KiB write", p is not None and len(p) == 16,
              "PING ok" if p else "no response")
        a = parse_write(link.req(CMD_WRITE, b""))
        check("empty WRITE rejected", a and a["status"] == ST_ERR_LENGTH,
              f"status={a['status'] if a else None}")

        unk = link.req(0x7F)
        check("unknown command returns ST_ERR_CMD",
              unk is not None and unk[0] == ST_ERR_CMD,
              str(unk[0] if unk else None))

        bad = bytearray(frame(CMD_PING))
        bad[6] ^= 0xFF                      # corrupt the payload CRC
        r = link.raw(bytes(bad))
        fr = parse_frames(r)
        check("bad CRC is NAKed with ST_ERR_CRC",
              any(c == (CMD_PING | 0x80) and p and p[0] == ST_ERR_CRC for c, p in fr),
              str([(hex(c), p[:1].hex()) for c, p in fr]))
        p = link.req(CMD_PING)
        check("PING works right after a CRC error", p is not None, "ok" if p else "no")

        overlong = MAGIC + bytes([CMD_PING]) + struct.pack("<H", 60000) + b"\x00" * 4
        link.raw(overlong)
        p = link.req(CMD_PING)
        check("oversized length header resynchronises", p is not None,
              "ok" if p else "no")

        # garbage prefix then a valid frame in the same write
        p = link.req(CMD_PING, b"")
        check("PING after garbage/oversize recovery", p is not None, "ok" if p else "no")

        # ---------- 8. STATUS does not stall the state machine ----------
        print("\n-- 8. high-frequency STATUS during typing --")
        parse_write(link.req(CMD_WRITE, (b"abcdefghij" * 12)))
        clear_notepad(np_hwnd)
        hb = parse_status(link.req(CMD_START))
        check("START accepted for stress test", hb and hb["status"] == ST_OK,
              str(hb and hb["status"]))
        polls, seen = 0, 0
        t0 = time.time()
        while time.time() - t0 < 1.0:
            s = parse_status(link.req(CMD_STATUS, timeout=0.4))
            polls += 1
            if s and s["processed"] > seen:
                seen = s["processed"]
        check("status polling does not block typing", seen > 0,
              f"{polls} polls, progress {seen}")
        sc = parse_status(link.req(CMD_CANCEL))
        check("CANCEL still works under status flood",
              sc and sc["state"] == TY_CANCELLED, STATE_NAMES.get(sc["state"]))
        time.sleep(0.5)
        after = parse_status(link.req(CMD_STATUS))
        check("no keys after flood-cancel", after["processed"] == sc["processed"],
              f"{after['processed']} vs {sc['processed']}")

    finally:
        link.close()
        close_notepad(np)

    passed = sum(1 for _, ok, _ in results if ok)
    failed = [n for n, ok, _ in results if not ok]
    print(f"\n== {passed}/{len(results)} checks passed ==")
    if failed:
        print("FAILED: " + ", ".join(failed))
        return 1
    print("ALL DEVICE PROTOCOL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
