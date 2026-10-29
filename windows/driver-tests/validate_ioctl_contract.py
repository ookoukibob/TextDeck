#!/usr/bin/env python3
"""Static contract validation for the TextDeck virtual HID driver.

This does NOT install or load the driver. It parses the ABI header, the driver
sources and the INF, and asserts the invariants the design depends on:

  * the IOCTL contract is complete and consistently declared
  * every IOCTL is handled, and unknown codes fail explicitly
  * input is validated and copied before it reaches VHF
  * release-all runs on every teardown path
  * the VHF buffering contract is honoured (see docs/VHF-DESIGN.md)
  * report ordering is serialised, and no spinlock is held across a blocking call
  * LED output reports are handled because the descriptor declares them
  * the INF restricts the device and declares one hardware ID
  * the client preserves the same contract

Exit code 0 = all checks passed.
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PROTO = os.path.join(ROOT, "windows", "shared", "textdeck_protocol.h")
DRV = os.path.join(ROOT, "windows", "driver", "TextDeckHidDriver")
QUEUE = os.path.join(DRV, "Queue.c")
DEVICE = os.path.join(DRV, "Device.c")
DRIVER = os.path.join(DRV, "Driver.c")
INF = os.path.join(DRV, "TextDeckHidDriver.inf")
DESC = os.path.join(DRV, "ReportDescriptor.h")
APP = os.path.join(ROOT, "windows", "VirtualHIDTyper", "src", "VirtualHIDTyper.cpp")
INSTALLER = os.path.join(ROOT, "windows", "driver-tools", "install_driver.ps1")

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))


def read(p):
    with open(p, encoding="utf-8") as f:
        return f.read()


def main():
    for p in (PROTO, QUEUE, DEVICE, DRIVER, INF, DESC, APP, INSTALLER):
        if not os.path.exists(p):
            print(f"FATAL: missing source file {p}")
            return 2

    proto = read(PROTO)
    queue = read(QUEUE)
    device = read(DEVICE)
    inf = read(INF)
    desc = read(DESC)

    # ---- ABI declarations -------------------------------------------------
    check("protocol major version is 1",
          re.search(r"#define\s+TEXTDECK_PROTOCOL_MAJOR\s+1\b", proto) is not None,
          "TEXTDECK_PROTOCOL_MAJOR 1")

    ctl = re.findall(r"CTL_CODE\(\s*TEXTDECK_DEVICE_TYPE,\s*(0x[0-9A-Fa-f]+),\s*"
                     r"(METHOD_\w+),\s*(FILE_\w+)\s*\)", proto)
    check("exactly four IOCTLs are declared", len(ctl) == 4, f"found {len(ctl)}")
    check("the four function codes are 0x800/0x801/0x802/0x803",
          [c[0].lower() for c in ctl] == ["0x800", "0x801", "0x802", "0x803"],
          ", ".join(c[0] for c in ctl))
    check("every IOCTL uses METHOD_BUFFERED",
          len(ctl) == 4 and all(c[1] == "METHOD_BUFFERED" for c in ctl),
          ", ".join(c[1] for c in ctl))
    check("every IOCTL uses FILE_ANY_ACCESS (access is governed by the device ACL)",
          len(ctl) == 4 and all(c[2] == "FILE_ANY_ACCESS" for c in ctl),
          ", ".join(c[2] for c in ctl))
    check("LED state struct is declared for the LED query IOCTL",
          "TEXTDECK_LED_STATE" in proto and "IOCTL_TEXTDECK_GET_LED_STATE" in proto,
          "TEXTDECK_GET_LED_STATE")
    check("LED bitmap constants match the descriptor's five usages",
          all(f"TEXTDECK_LED_{n}" in proto for n in
              ("NUM_LOCK", "CAPS_LOCK", "SCROLL_LOCK", "COMPOSE", "KANA")) and
          "TEXTDECK_LED_VALID_MASK      0x1F" in proto,
          "5 usages, mask 0x1F")

    check("report size is 8 bytes",
          re.search(r"#define\s+TEXTDECK_REPORT_SIZE\s+8\b", proto) is not None, "8")
    check("key event struct is packed",
          "#pragma pack(push, 1)" in proto, "pack(1)")
    check("usage bounds are 0x04..0x65",
          re.search(r"#define\s+TEXTDECK_USAGE_MIN\s+0x04", proto) is not None and
          re.search(r"#define\s+TEXTDECK_USAGE_MAX\s+0x65", proto) is not None,
          "0x04..0x65")
    check("three key actions are defined",
          "TextDeckKeyActionDown = 0" in proto and
          "TextDeckKeyActionUp   = 1" in proto and
          "TextDeckKeyActionReleaseAll = 2" in proto,
          "Down/Up/ReleaseAll")

    # ---- protocol 1.1: modifier-holding semantics -------------------------
    # The defect this round fixes: Up used to emit an all-zero report, so
    # releasing the letter of Ctrl+A also released Ctrl, and a Down with
    # Usage=0 was rejected so a modifier could not be held on its own.
    check("a modifiers-only action exists",
          re.search(r"TextDeckKeyActionModifiers\s*=\s*3", proto) is not None,
          "Modifiers = 3")
    check("the Modifiers field is documented as the complete held set",
          # The phrase wraps across a comment line ("COMPLETE\n * set of ..."),
          # so the continuation marker has to be part of the pattern.
          re.search(r"COMPLETE\s*\*?\s*set\s+of\s+modifiers", proto) is not None,
          "semantics documented in the header")
    check("protocol minor version was bumped for the semantic change",
          re.search(r"#define\s+TEXTDECK_PROTOCOL_MINOR\s+1\b", proto) is not None,
          "MINOR = 1")
    check("the event struct size is unchanged (ABI compatibility)",
          "UCHAR  Reserved;    /* must be 0 */" in proto and
          "#pragma pack(pop)" in proto,
          "4-byte packed struct, no fields added")

    # The driver must actually implement the new semantics, not just declare it.
    # Anchor on the switch cases and require the modifier write inside each one;
    # the comment blocks in between are long, so the window has to be generous.
    _up_case = re.search(r"case TextDeckKeyActionUp:([\s\S]*?)case TextDeckKeyActionModifiers:",
                         device)
    check("driver keeps modifiers on Up",
          _up_case is not None and "Report[0] = Event->Modifiers" in _up_case.group(1),
          "Up writes the caller's modifier byte")
    _mod_case = re.search(r"case TextDeckKeyActionModifiers:([\s\S]*?)case TextDeckKeyActionReleaseAll:",
                          device)
    check("driver implements the modifiers-only action",
          _mod_case is not None and "Report[0] = Event->Modifiers" in _mod_case.group(1),
          "Modifiers writes the modifier byte")
    check("driver rejects a non-zero usage on the modifiers-only action",
          _mod_case is not None and
          "Event->Usage != 0" in _mod_case.group(1) and
          "STATUS_INVALID_PARAMETER" in _mod_case.group(1),
          "usage must be 0")
    check("driver still rejects unknown actions",
          re.search(r"default:\s*\n\s*return STATUS_INVALID_PARAMETER", device) is not None,
          "default -> invalid parameter")
    check("device symbolic link is a DOS device path",
          "DosDevices\\\\TextDeckHidDevice" in proto, "\\DosDevices\\TextDeckHidDevice")
    check("the ABI contains no test-signing automation",
          re.search(r"bcdedit|testsigning", proto, re.I) is None, "none")

    # ---- report descriptor ------------------------------------------------
    body = desc[desc.index("{"):desc.index("};")]
    # Strip comments first: the doc comments quote example bytes (0xE0..0xE7)
    # that are NOT part of the descriptor array.
    body = re.sub(r"/\*[\s\S]*?\*/", "", body)
    body = re.sub(r"//[^\n]*", "", body)
    bytes_in_desc = re.findall(r"0x[0-9A-Fa-f]{2}", body)
    declared = re.search(r"TEXTDECK_REPORT_DESCRIPTOR_SIZE\s+(\d+)", desc)
    check("report descriptor byte count matches its declared size",
          declared is not None and len(bytes_in_desc) == int(declared.group(1)),
          f"{len(bytes_in_desc)} bytes, declared {declared.group(1) if declared else '?'}")
    check("descriptor ends with End Collection (0xC0)",
          bytes_in_desc and bytes_in_desc[-1].lower() == "0xc0", "0xC0")

    # ---- advertised concurrency must equal real capability ----------------
    # GET_INFO.MaxConcurrentKeys names "how many non-modifier keys can be held at
    # once". The descriptor declares six key slots, but TextDeckBuildReport writes
    # only slot 0, so the honest answer is 1. This used to report 6 -- the slot
    # count -- which no client read, so nothing broke, but a client that trusted
    # it would have silently lost the second key. The three numbers are asserted
    # separately so they cannot drift apart again.
    slots_macro = re.search(r"#define\s+TEXTDECK_REPORT_KEY_SLOTS\s+(\d+)", proto)
    conc_macro  = re.search(r"#define\s+TEXTDECK_MAX_CONCURRENT_KEYS\s+(\d+)", proto)
    check("the descriptor's slot count is declared explicitly",
          slots_macro is not None, slots_macro.group(0) if slots_macro else "missing")
    check("the driver's real concurrency is declared explicitly",
          conc_macro is not None, conc_macro.group(0) if conc_macro else "missing")

    # The descriptor must actually declare that many 8-bit key slots: Report
    # Count (0x95, N) followed by Report Size (0x75, 0x08) then Input(Data,Array).
    slots = int(slots_macro.group(1)) if slots_macro else -1
    check("the report descriptor declares the documented number of key slots",
          re.search(r"0x95,\s*0x0?%X\b" % slots, body) is not None and slots == 6,
          f"descriptor slots={slots}")

    # GET_INFO must report the concurrency macro, not the slot macro.
    check("GET_INFO reports the driver's real concurrency, not the slot count",
          re.search(r"info\.MaxConcurrentKeys\s*=\s*TEXTDECK_MAX_CONCURRENT_KEYS\s*;", queue)
          is not None,
          "MaxConcurrentKeys = TEXTDECK_MAX_CONCURRENT_KEYS")
    check("GET_INFO does not report the descriptor slot count as concurrency",
          re.search(r"info\.MaxConcurrentKeys\s*=\s*TEXTDECK_MAX_KEYS_PER_REPORT\s*;", queue)
          is None,
          "not the slot count")

    # And the number must be honest about what BuildReport does: if it only ever
    # writes slot 0, the advertised concurrency has to be 1.
    writes_slot0_only = ("Report[2] = Event->Usage" in device and
                         re.search(r"Report\[3\]\s*=\s*Event->Usage", device) is None)
    advertised = int(conc_macro.group(1)) if conc_macro else -1
    check("advertised concurrency matches the slots BuildReport actually writes",
          (writes_slot0_only and advertised == 1) or (not writes_slot0_only),
          f"writes_slot0_only={writes_slot0_only} advertised={advertised}")

    # ---- LED output reports ----------------------------------------------
    # The descriptor declares an Output report (0x91 items). VHF will only
    # deliver those if EvtVhfAsyncOperationWriteReport is registered, so either
    # both sides exist or neither does - a declared-but-unhandled output report
    # stalls the OS write.
    declares_output = "0x91, 0x02" in body and "0x91, 0x01" in body
    handles_output = "EvtVhfAsyncOperationWriteReport" in device
    check("descriptor output report and driver handler agree",
          declares_output == handles_output,
          f"declares_output={declares_output} handles_output={handles_output}")
    check("output-report callback completes the operation",
          "VhfAsyncOperationComplete" in device,
          "never leaves the OS write pending")
    check("LED report is decoded by a pure, testable function",
          "TextDeckDecodeLedReport" in device,
          "extracted and covered by the logic test")

    # ---- IOCTL dispatch ---------------------------------------------------
    for name in ("IOCTL_TEXTDECK_GET_INFO", "IOCTL_TEXTDECK_SUBMIT_KEY",
                 "IOCTL_TEXTDECK_RELEASE_ALL", "IOCTL_TEXTDECK_GET_LED_STATE"):
        check(f"driver handles {name}", f"case {name}:" in queue, "case present")

    def dispatch_case(case_name, end_marker):
        start_at = queue.find(f"case {case_name}:")
        end_at = queue.find(end_marker, start_at + 1) if start_at >= 0 else -1
        return queue[start_at:end_at] if start_at >= 0 and end_at >= 0 else ""

    get_info_case = dispatch_case("IOCTL_TEXTDECK_GET_INFO", "case IOCTL_TEXTDECK_SUBMIT_KEY:")
    submit_key_case = dispatch_case("IOCTL_TEXTDECK_SUBMIT_KEY", "case IOCTL_TEXTDECK_RELEASE_ALL:")
    release_all_case = dispatch_case("IOCTL_TEXTDECK_RELEASE_ALL", "case IOCTL_TEXTDECK_GET_LED_STATE:")
    get_led_case = dispatch_case("IOCTL_TEXTDECK_GET_LED_STATE", "default:")

    check("GET_INFO rejects unexpected input bytes",
          "if (InputBufferLength != 0)" in get_info_case,
          "input payload must be empty")
    check("SUBMIT_KEY rejects an unexpected output buffer",
          "if (OutputBufferLength != 0)" in submit_key_case,
          "output buffer must be empty")
    check("RELEASE_ALL rejects either buffer",
          "if (InputBufferLength != 0 || OutputBufferLength != 0)" in release_all_case,
          "both buffer lengths must be zero")
    check("GET_LED_STATE rejects unexpected input bytes",
          "if (InputBufferLength != 0)" in get_led_case,
          "input payload must be empty")

    check("unknown IOCTL fails with STATUS_INVALID_DEVICE_REQUEST",
          re.search(r"default:[\s\S]{0,220}STATUS_INVALID_DEVICE_REQUEST", queue) is not None,
          "explicit rejection")
    check("input buffer length is validated exactly",
          "inputSize != sizeof(TEXTDECK_KEY_EVENT)" in queue, "exact-size check")
    check("input is copied out of caller memory before submit",
          "RtlCopyMemory(&event, inputBuffer, sizeof(event))" in queue, "copied")
    check("payload is validated before it reaches VHF",
          queue.index("TextDeckBuildReport") < queue.index("TextDeckSubmitReport"),
          "build-then-submit")

    # ---- teardown paths ---------------------------------------------------
    check("release-all runs when a client closes its handle",
          re.search(r"TextDeckEvtFileClose[\s\S]{0,700}TextDeckReleaseAll", device) is not None,
          "EvtFileClose")
    check("release-all runs during device cleanup",
          re.search(r"TextDeckEvtDeviceCleanup[\s\S]{0,900}TextDeckReleaseAll", device) is not None,
          "EvtDeviceCleanup")
    check("VHF instance is deleted during cleanup",
          re.search(r"TextDeckEvtDeviceCleanup[\s\S]{0,1100}VhfDelete", device) is not None,
          "VhfDelete")
    # The old design stored the context in a module-level global and cleared it
    # before VhfDelete. The context now travels through VHF_CONFIG.VhfClientContext
    # and VhfDelete(Wait=TRUE) drains outstanding callbacks, so there is no
    # global to clear. Assert the safer mechanism instead.
    check("no module-level callback context global remains",
          "g_TextDeckCallbackContext" not in device,
          "context passed via VHF_CONFIG.VhfClientContext")

    # ---- VHF buffering contract ------------------------------------------
    # The two mutually exclusive VHF contracts (VhfReadReportSubmit remarks):
    #
    #   A. default buffering  - NO EvtVhfReadyForNextReadReport registered, and
    #      "the driver can reuse the transfer buffer after the call returns".
    #   B. driver buffering   - EvtVhfReadyForNextReadReport registered; submit
    #      EXACTLY ONCE per callback, and keep the buffer valid until the next
    #      callback.
    #
    # Registering the callback while submitting in a free-running loop breaks
    # both, which is exactly the defect this validator now guards against.
    # Strip comments first: the file header explains WHY the callback is not
    # registered, and a raw substring search would match that explanation.
    device_code = re.sub(r"/\*[\s\S]*?\*/", "", device)
    device_code = re.sub(r"//[^\n]*", "", device_code)
    registers_ready = "EvtVhfReadyForNextReadReport" in device_code
    check("VHF buffering mode is unambiguous (default OR callback, not a mix)",
          not registers_ready,
          "no EvtVhfReadyForNextReadReport registered -> VHF default buffering")
    check("the default-buffering choice is documented where it is made",
          "EvtVhfReadyForNextReadReport" in device,
          "rationale recorded in Device.c")

    check("VHF_CONFIG carries VhfClientContext for the output-report callback",
          re.search(r"vhfConfig\.VhfClientContext\s*=\s*context\s*;", device) is not None,
          "VhfClientContext = context")

    # The readiness callback, if it ever comes back, must take VhfClientContext.
    # The documented parameter is a driver-defined context pointer, NOT the
    # VHFHANDLE returned by VhfCreate. Any code comparing the callback argument
    # against the stored handle is treating it as the wrong type.
    check("no callback treats VhfClientContext as a VHFHANDLE",
          re.search(r"VhfHandle\s*!=\s*context->VhfHandle", device) is None and
          re.search(r"EventVhf\w*\s*==\s*context->VhfHandle", device) is None,
          "callback argument is a context pointer, never compared to the handle")

    # A report buffer that is a local (stack) array is only safe under default
    # buffering, where VHF copies before returning. Assert the copy exists so
    # the code does not rely on the caller's memory.
    m = re.search(r"^TextDeckSubmitReport\(", device, re.M)
    if not m:
        check("submit path located", False, "TextDeckSubmitReport definition not found")
        submit_body = ""
    else:
        submit_body = device[m.start():]
        nxt = re.search(r"^TextDeckReleaseAll\(", submit_body[1:], re.M)
        submit_body = submit_body[:nxt.start() + 1] if nxt else submit_body

    check("submit copies the report into driver-owned storage",
          "RtlCopyMemory(local, Report, sizeof(local))" in submit_body,
          "local buffer copy")
    check("packet points at the driver-owned copy, not caller memory",
          "packet.reportBuffer    = local;" in submit_body,
          "reportBuffer = local")
    check("submit retries are bounded",
          "TEXTDECK_SUBMIT_MAX_ATTEMPTS" in submit_body, "bounded loop")
    check("a busy VHF queue is retried, not reported as success",
          submit_body.count("VhfReadReportSubmit") >= 1 and
          "STATUS_DEVICE_BUSY" in submit_body,
          "busy is retried")
    check("VhfReadReportSubmit result is never discarded",
          re.search(r"status\s*=\s*VhfReadReportSubmit", submit_body) is not None,
          "status captured")

    # ---- serialisation ----------------------------------------------------
    check("report submission is serialised by a mutex, not a spinlock",
          "KeInitializeMutex" in device and "KeInitializeSpinLock" not in device,
          "KMUTEX only")
    check("the submit path actually acquires and releases the mutex",
          "KeWaitForSingleObject(&Context->SubmitMutex" in submit_body and
          submit_body.count("KeReleaseMutex(&Context->SubmitMutex") >= 2,
          "acquire + release on every exit path")
    check("no spinlock is acquired anywhere in the driver",
          not re.search(r"KeAcquireSpinLock|KeAcquireInStackQueuedSpinLock", device + queue),
          "no spinlock held across a blocking call")
    check("the mutex is not held while waiting on a kernel event",
          "KeWaitForSingleObject(&Context->ReportReadyEvent" not in device,
          "no readiness event wait")

    # ---- teardown ordering ------------------------------------------------
    check("teardown marks VHF unusable before deleting it",
          re.search(r"InterlockedExchange\(&context->VhfStarted, FALSE\)[\s\S]{0,900}VhfDelete", device) is not None,
          "stop accepting, then VhfDelete")
    check("teardown releases keys while VHF is still alive",
          re.search(r"VhfReadReportSubmit[\s\S]{0,400}VhfDelete", device) is not None,
          "release-all before VhfDelete")
    check("VhfDelete waits for outstanding VHF callbacks",
          re.search(r"VhfDelete\(context->VhfHandle, TRUE\)", device) is not None,
          "VhfDelete(..., TRUE)")

    # ---- validation rules -------------------------------------------------
    mb = re.search(r"^TextDeckBuildReport\(", device, re.M)
    if not mb:
        check("validation path located", False, "TextDeckBuildReport definition not found")
        build_body = ""
    else:
        build_body = device[mb.start():]
        nb = re.search(r"^TextDeckEvtDeviceFileCreate\(", build_body[1:], re.M)
        build_body = build_body[:nb.start() + 1] if nb else build_body
    check("out-of-range usage is rejected",
          "TEXTDECK_USAGE_MIN" in build_body and "TEXTDECK_USAGE_MAX" in build_body,
          "range enforced")
    check("unknown action is rejected",
          "default:" in build_body and "STATUS_INVALID_PARAMETER" in build_body,
          "default -> invalid parameter")
    check("report buffer is zeroed before use",
          "RtlZeroMemory(Report, TEXTDECK_REPORT_SIZE)" in build_body, "zeroed")

    # ---- INF --------------------------------------------------------------
    check("INF restricts the device object to SYSTEM + Administrators",
          "D:P(A;;GA;;;SY)(A;;GA;;;BA)" in inf, "SDDL SY+BA")
    check("INF grants nothing to interactive users or Everyone",
          not re.search(r";;;WD\)|;;;BU\)|;;;AU\)", inf), "no WD/BU/AU ACE")
    check("INF declares exactly one hardware ID",
          len(re.findall(r"Root\\TextDeckHidDriver", inf)) == 1,
          "Root\\TextDeckHidDriver")
    check("INF does not enable test signing or touch BCD",
          re.search(r"bcdedit|testsigning", inf, re.I) is None, "none")

    # The VHF contract: vhf.sys must be loaded as a LOWER FILTER below the HID
    # source driver. A package that only declares Class=HIDClass can install and
    # report "Started" while VhfCreate has nothing to attach to, so the entry is
    # asserted explicitly -- section placement, value type and service name.
    # 0x00010000 is FLG_ADDREG_TYPE_MULTI_SZ (REG_MULTI_SZ), which is the
    # documented type for LowerFilters. 0x00010008 would OR in FLG_ADDREG_APPEND
    # and change the meaning, so it is rejected rather than accepted as a variant.
    hw_sections = re.findall(r"\[([^\]]*\.HW)\]", inf, re.I)
    check("INF declares a DDInstall .HW section", len(hw_sections) >= 1,
          ",".join(hw_sections) if hw_sections else "none")
    hw_body = ""
    if hw_sections:
        start = inf.find("[%s]" % hw_sections[0])
        if start >= 0:
            rest = inf[start + len(hw_sections[0]) + 2:]
            nxt = rest.find("\n[")
            hw_body = rest if nxt < 0 else rest[:nxt]
    check("the .HW section is the one that AddRegs the filter list",
          "AddReg" in hw_body, "AddReg in .HW")

    lf = re.search(r"^\s*HKR\s*,\s*,\s*LowerFilters\s*,\s*0x([0-9A-Fa-f]+)\s*,\s*\"([^\"]*)\"",
                   inf, re.I | re.M)
    check("INF sets LowerFilters to vhf in an AddReg section",
          lf is not None, lf.group(0).strip() if lf else "no LowerFilters entry")
    check("LowerFilters is REG_MULTI_SZ (0x00010000), not an append flag",
          lf is not None and lf.group(1).lower() == "00010000",
          ("0x" + lf.group(1)) if lf else "n/a")
    check("LowerFilters names the in-box vhf service",
          lf is not None and lf.group(2).strip().lower() == "vhf",
          lf.group(2) if lf else "n/a")
    check("LowerFilters is not written from the plain DDInstall section",
          re.search(r"\[TextDeck_Device\.NT\][^\[]*LowerFilters", inf, re.I) is None,
          "only from .HW")

    # ---- client/driver ABI agreement --------------------------------------
    app = read(APP)
    check("client includes the shared ABI header (no private copy)",
          '#include "textdeck_protocol.h"' in app,
          "includes textdeck_protocol.h")
    check("client does not re-declare the key-event struct",
          "struct KEYBOARD_INPUT_EX_DATA" not in app and
          "struct TEXTDECK_KEY_EVENT" not in app,
          "no local struct definition")
    check("client takes the device path from the shared header",
          "TEXTDECK_DEVICE_WIN32_PATH" in app,
          "kDevicePath = TEXTDECK_DEVICE_WIN32_PATH")
    check("client submits via IOCTL_TEXTDECK_SUBMIT_KEY",
          "IOCTL_TEXTDECK_SUBMIT_KEY" in app, "submit key")
    check("client releases via IOCTL_TEXTDECK_RELEASE_ALL",
          "IOCTL_TEXTDECK_RELEASE_ALL" in app, "release all")
    check("client queries the protocol version before typing",
          "IOCTL_TEXTDECK_GET_INFO" in app and
          "TEXTDECK_PROTOCOL_MAJOR" in app,
          "GET_INFO + version compare")
    check("client carries no legacy driver symbols",
          "IOCTL_KEYBOARD_INPUT_EX" not in app and
          "\\\\.\\VirtualHIDDevice" not in app,
          "no legacy IOCTL or device path")

    # ---- installer identity ------------------------------------------------
    inst = read(INSTALLER)
    check("installer targets the TextDeck hardware ID",
          "Root\\\\TextDeckHidDriver" in inst or "Root\\TextDeckHidDriver" in inst,
          "Root\\TextDeckHidDriver")
    check("installer does not target the legacy driver",
          "VirtualHIDDriver" not in inst, "no VirtualHIDDriver")
    check("installer default package dir points at the built package",
          "TextDeckHidDriver" in inst and "x64\\Debug" in inst,
          "windows/driver/TextDeckHidDriver/x64/Debug/...")

    # ---- report -----------------------------------------------------------
    print("TextDeck driver IOCTL contract validation")
    print("=" * 66)
    failed = 0
    for name, ok, detail in results:
        print(f"  [{'PASS' if ok else 'FAIL'}] {name} :: {detail}")
        if not ok:
            failed += 1
    print("=" * 66)
    print(f"{len(results)} checks, {failed} failure(s)")
    if failed:
        print("RESULT: FAIL")
        return 1
    print("RESULT: ALL CONTRACT CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
