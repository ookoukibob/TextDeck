# TextDeck

Type a prepared text into any Windows application — three input backends, no
paid entitlement, no activation, no telemetry.

TextDeck types text you prepare in advance into whatever window currently has
focus, driven either by a kernel-level virtual HID keyboard, by a physical
ESP32-S3 USB HID device, or by the Win32 SendInput API. The repository also
contains **TextDeck Mapping**, a general-purpose input-mapping and key-script
engine that turns input events (keyboard, mouse, gamepad, serial, UDP) into
keyboard actions through the same output backends.

**中文用户文档**（与发布 ZIP 内一致）：[功能说明](docs/功能说明.md) ·
[安装说明](docs/安装说明.md) · [使用说明](docs/使用说明.md)。
安装包下载见 [Releases](https://github.com/ookoukibob/TextDeck/releases)。

---

## Three typing backends

| Backend | What it really is | Driver? | Admin? | Character coverage |
|---|---|---|---|---|
| **Virtual HID** | A KMDF/VHF kernel driver presents a virtual keyboard to Windows; the app submits HID reports over a private IOCTL | yes | yes | US-layout letters, digits, punctuation, Enter/Tab |
| **SendInput Unicode** | `SendInput` with `KEYEVENTF_UNICODE` | no | no | any Unicode, including CJK and emoji |
| **ESP32-S3 USB HID** | A physical ESP32-S3 board enumerates as a USB keyboard; the app sends text over serial | no | no | US-layout characters |

The three are independent. A user who cannot install a kernel driver can still
use the tool through SendInput or the ESP32 device. The application labels the
active backend at all times and **never silently falls back** from one to
another.

**Honesty note on "Unicode over HID":** a keyboard HID report carries *usage
ids*, not code points. Text that the selected keyboard layout cannot express
cannot be sent as native HID input — it is reported as skipped, never quietly
pasted in via another path. If you need arbitrary Unicode, use the SendInput
backend.

---

## Repository layout

```
windows/
  shared/textdeck_protocol.h        Driver <-> client ABI (single source of truth)
  driver/TextDeckHidDriver/         KMDF/VHF virtual HID keyboard driver
  driver/TextDeckHidSolution.sln    Driver solution
  driver-tools/                     build / install / uninstall + gate tests
  driver-tests/                     IOCTL contract + driver logic tests
  VirtualHIDTyper/                  Windows GUI application (typing backends)
  mapping/                          TextDeck Mapping engine + CLI + tests
firmware/
  esp32_hid_text_device/            ESP32-S3 firmware (Arduino)
  protocol.md                       Serial protocol specification
tools/
  esp32-desktop/                    Python desktop writer for the ESP32 backend
  esp32/                            Protocol edge tests, fault injection, capture
docs/
  BUILD.md                          How to build each component
  PROTOCOL.md                       Driver/client protocol notes
  SIGNING.md                        Driver signing requirements and honest status
  VHF-DESIGN.md                     Why the driver submits HID reports the way it does
  LICENSE-AND-PROVENANCE.md         License scope and provenance record
```

---

## Quick start (no driver)

1. Build the app (`docs/BUILD.md`).
2. Run `VirtualHIDTyper.exe`.
3. Choose **软件输入模式 (SendInput)**.
4. Type or paste your text, click **保存文本**.
5. Close the window — it keeps running in the tray.
6. Focus the target application and press the start hotkey (**Ctrl+Alt+Shift+F9**
   by default).

## Quick start (virtual HID backend)

Requires the driver, which requires signing — see `docs/SIGNING.md` for the
honest status (source-complete and self-signable, **not** production-signed).
Once installed:

1. Run `VirtualHIDTyper.exe` as administrator (the device ACL is SYSTEM +
   Administrators only).
2. Choose the Virtual HID mode.
3. Save your text, close to tray, press the hotkey.

## TextDeck Mapping

A standalone engine that maps normalized input events to keyboard actions and
scripts (momentary/toggle/threshold/long-press/combo/sequence triggers,
declarative JSON profiles, key-ownership management, feedback-loop protection).
See `windows/mapping/README.md` for build commands, the configuration format
and `map_app` usage.

---

## Default hotkeys

| Action | Default |
|---|---|
| Start typing | `Ctrl+Alt+Shift+F9` |
| Emergency stop | `Ctrl+Alt+Shift+F10` |

Both are rebindable in the application; the UI always shows the *actual*
registered binding, and a conflict is reported with its Win32 error rather than
silently ignored.

---

## Safety behaviour

- **Release-all on every exit path.** Cancellation, a closed client handle and
  driver teardown all submit an all-zero HID report, so an aborted run cannot
  leave a key held down.
- **Explicit stop.** Stop hotkey, tray menu and the Stop button all funnel
  through one path.
- **Repeated start is a no-op.** A start request during a running task is
  ignored; it never spawns a second task or re-sends the text.
- **No focus theft.** Starting from the hotkey never activates the main window.
- **Restricted device access.** The driver's device object grants GENERIC_ALL to
  SYSTEM and Administrators only.

---

## Known limitations

- **Driver signing.** The virtual HID driver is an independent implementation
  that builds and installs in test-signing environments. There is **no
  production-signed driver release**: distributing it generally requires an EV
  code-signing certificate and Microsoft attestation (see `docs/SIGNING.md`).
  Do not install the test-signed driver on a machine you rely on.
- **HID character coverage** is limited to what the keyboard usage table
  expresses (US layout); other characters are reported as skipped.
- **ESP32 backend** requires the physical board and its firmware; the host
  tools in `tools/` drive it over the USB CDC control port.

---

## License

This project is licensed under the **PolyForm Noncommercial License 1.0.0** —
see [`LICENSE`](LICENSE) for the full terms.

PolyForm Noncommercial 1.0.0 permits use, modification and distribution for
**noncommercial purposes** (including personal, research, educational and
charitable use as defined in the license). **Commercial use is not permitted
under these terms** and requires a separate arrangement with the copyright
holders. PolyForm Noncommercial is **not** an OSI-approved open-source license.

Provenance and license scope for each component are recorded in
`docs/LICENSE-AND-PROVENANCE.md`. TextDeck contains **no LizardByte /
libvirtualhid code**; the driver was implemented independently from Microsoft's
published documentation.
