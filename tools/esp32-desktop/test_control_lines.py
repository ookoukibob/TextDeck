#!/usr/bin/env python3
"""Regression test for the CDC control-line initialisation in hid_text_writer.

The defect this guards against: ``DeviceClient.open()`` performs an RTS reset
pulse whose last action is ``setDTR(False)``. This firmware's TinyUSB CDC only
accepts host data once DTR is asserted, so without a later ``setDTR(True)`` every
protocol request times out. The symptom was a bare "等待设备响应超时" with no hint
that the control line was the cause.

The test uses a fake serial object, so it needs no hardware and cannot touch the
device. It asserts four things:

  1. open() leaves DTR and RTS asserted, and does so *after* the reset pulse
  2. the first protocol frame is written only after the control lines are set
  3. existing error paths still report: port cannot open, PING times out
  4. open()/ping() never write a text payload (no WRITE, no storage change)

Run:  python tools/esp32-desktop/test_control_lines.py
Exit: 0 = all checks passed, 1 = at least one failed
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import hid_text_writer as hw   # noqa: E402


class FakeSerial:
    """Records every control-line change and every byte written."""

    def __init__(self, *a, **kw):
        self.events = []          # ordered log of what happened
        self.written = []
        self._dtr = None
        self._rts = None
        self.closed = False
        self.reply = b""          # bytes the fake device "sends back"
        self.fail_on_open = False

    # --- pyserial surface used by DeviceClient ---
    def setDTR(self, v):
        self._dtr = bool(v)
        self.events.append(("dtr", bool(v)))

    def setRTS(self, v):
        self._rts = bool(v)
        self.events.append(("rts", bool(v)))

    def reset_input_buffer(self):
        self.events.append(("reset_input_buffer", None))

    def write(self, data):
        self.events.append(("write", bytes(data)))
        self.written.append(bytes(data))
        return len(data)

    def flush(self):
        pass

    def read(self, n=1):
        if self.reply:
            out, self.reply = self.reply, b""
            return out
        return b""

    def close(self):
        self.closed = True

    @property
    def in_waiting(self):
        return len(self.reply)


CHECKS = []
def check(name, ok, detail=""):
    CHECKS.append((name, bool(ok), detail))
    print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name,
                           (" :: " + detail) if detail else ""))


def ping_payload():
    """A valid 16-byte PING response payload."""
    p = bytearray(16)
    p[0] = hw.ST_OK           # status
    p[1] = hw.VERSION         # proto
    p[2] = 0x02               # fw minor
    p[3] = 0x01               # fw major -> 1.2
    p[4] = 0x00; p[5] = 0x28  # max_text = 10240
    p[6] = 1                  # has_text
    p[7] = 0xE7; p[8] = 0x25  # saved_len = 9703
    p[9:13] = (0x2D2E11B8).to_bytes(4, "little")
    p[13] = 0x3A; p[14] = 0x00
    p[15] = 1                 # hid_ready
    return bytes(p)


def install_fake(monkey_target="serial"):
    """Point DeviceClient at FakeSerial and return the instance it will use."""
    holder = {}

    def factory(*a, **kw):
        s = FakeSerial(*a, **kw)
        holder["ser"] = s
        return s

    hw.serial.Serial = factory
    return holder


print("== 1. control lines are asserted after the reset pulse ==")
orig_serial = hw.serial.Serial
try:
    holder = install_fake()
    dev = hw.DeviceClient("COM_TEST")
    dev._wait_boot_ready = lambda *a, **kw: True   # no real boot log in the fake
    dev.open()
    s = holder["ser"]

    dtr_events = [i for i, (k, v) in enumerate(s.events) if k == "dtr"]
    last_dtr = s.events[dtr_events[-1]][1] if dtr_events else None
    check("DTR is asserted after open()", last_dtr is True, "last dtr=%s" % last_dtr)
    check("RTS is asserted after open()", s._rts is True, "rts=%s" % s._rts)

    # the reset pulse must still happen, and must end before the assertion
    saw_reset = any(k == "dtr" and v is False for k, v in s.events)
    check("the reset pulse still runs (dtr is driven low at least once)", saw_reset,
          "dtr events=%d" % len(dtr_events))
    after_reset = dtr_events[-1] > max(i for i, (k, v) in enumerate(s.events)
                                       if k == "dtr" and v is False)
    check("the asserting setDTR happens after the reset pulse", after_reset,
          "ordering ok")

    print("\n== 2. the first protocol frame goes out after the control lines ==")
    # queue a valid PING response, then make the first request
    s.reply = hw.build_frame(hw.CMD_PING | 0x80, ping_payload())
    info = dev.ping()

    first_write_idx = next(i for i, (k, _) in enumerate(s.events) if k == "write")
    first_dtr_high = next(i for i, (k, v) in enumerate(s.events)
                          if k == "dtr" and v is True)
    check("first write occurs after DTR was asserted",
          first_write_idx > first_dtr_high,
          "write@%d dtr-high@%d" % (first_write_idx, first_dtr_high))

    check("PING parses a valid response", info.get("fw_ver") == 0x0102,
          "fw_ver=0x%04X" % info.get("fw_ver", 0))
    check("PING reports the device's stored length", info.get("saved_len") == 9703,
          "saved_len=%s" % info.get("saved_len"))

    print("\n== 4. open()+ping() never write a text payload ==")
    written_cmds = []
    for w in s.written:
        if len(w) >= 4:
            written_cmds.append(w[3])
    check("no WRITE_TEXT command was sent",
          hw.CMD_WRITE not in written_cmds,
          "cmds sent: %s" % [hex(c) for c in written_cmds])
    check("only PING was sent during open()+ping()",
          set(written_cmds) <= {hw.CMD_PING},
          "cmds sent: %s" % [hex(c) for c in written_cmds])
    dev.close()
    check("close() releases the port", s.closed is True, "closed")

    print("\n== 3. existing error paths still report ==")
    # 3a. port cannot be opened
    def factory_raises(*a, **kw):
        raise hw.serial.SerialException("could not open port 'COM_NOPE'")
    hw.serial.Serial = factory_raises
    dev2 = hw.DeviceClient("COM_NOPE")
    try:
        dev2.open()
        raised = False
    except Exception:
        raised = True
    check("a port that cannot open raises instead of being swallowed", raised,
          "SerialException propagated")

    # 3b. PING timeout still raises DeviceError with diagnostic text
    holder2 = install_fake()
    dev3 = hw.DeviceClient("COM_TEST")
    dev3._wait_boot_ready = lambda *a, **kw: True
    dev3.open()
    holder2["ser"].reply = b""            # no response at all
    orig_retries = dict(hw.REQUEST_RETRIES)
    hw.REQUEST_RETRIES[hw.CMD_PING] = 1   # keep the test quick
    try:
        dev3.ping()
        timed_out = False
        msg = ""
    except hw.DeviceError as e:
        timed_out = True
        msg = str(e)
    finally:
        hw.REQUEST_RETRIES.clear()
        hw.REQUEST_RETRIES.update(orig_retries)
    check("PING timeout still raises DeviceError", timed_out, "raised")
    check("timeout message still names the failure", "超时" in msg or "timeout" in msg.lower(),
          msg.splitlines()[0][:60] if msg else "")
finally:
    hw.serial.Serial = orig_serial

passed = sum(1 for _, ok, _ in CHECKS if ok)
print()
print("control-line regression: %d checks, %d passed, %d failed"
      % (len(CHECKS), passed, len(CHECKS) - passed))
print("RESULT:", "PASS" if passed == len(CHECKS) else "FAIL")
sys.exit(0 if passed == len(CHECKS) else 1)
