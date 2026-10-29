# License and provenance

TextDeck is licensed under the **PolyForm Noncommercial License 1.0.0**
([`LICENSE`](../LICENSE)). This file records what that grant covers, where the
code comes from, and which third-party materials are *referenced* but not
included.

PolyForm Noncommercial 1.0.0 permits use, modification and distribution for
noncommercial purposes only (including personal, research, educational and
charitable use as defined in the license). It is **not** an OSI-approved
open-source license; commercial use requires a separate arrangement with the
copyright holders.

## Component status

| Component | Location | License | Provenance |
|---|---|---|---|
| TextDeck virtual HID driver | `windows/driver/TextDeckHidDriver/` | PolyForm Noncommercial 1.0.0 | Written from scratch for TextDeck against published Microsoft documentation (VHF reference, KMDF guide, WDK IOCTL docs) and the USB HID Usage Tables. See "Driver provenance" below. |
| Shared driver/client ABI | `windows/shared/textdeck_protocol.h` | PolyForm Noncommercial 1.0.0 | Original TextDeck work. |
| Windows application | `windows/VirtualHIDTyper/` | PolyForm Noncommercial 1.0.0 | Original TextDeck work. |
| Driver build/install tooling | `windows/driver-tools/`, `windows/driver-tests/` | PolyForm Noncommercial 1.0.0 | Original TextDeck work. |
| TextDeck Mapping engine | `windows/mapping/` | PolyForm Noncommercial 1.0.0 | Original TextDeck work, written from scratch (see "Mapping provenance"). |
| ESP32-S3 firmware | `firmware/esp32_hid_text_device/` | PolyForm Noncommercial 1.0.0 | Original TextDeck work. The Arduino-ESP32 core is a build-time dependency (LGPL-2.1) and is **not** vendored here. |
| ESP32 protocol specification | `firmware/protocol.md` | PolyForm Noncommercial 1.0.0 | Original TextDeck work. |
| Desktop/host tools | `tools/` | PolyForm Noncommercial 1.0.0 | Original TextDeck work. |
| Documentation | `README.md`, `docs/`, `SECURITY.md` | PolyForm Noncommercial 1.0.0 | Original TextDeck documentation. |

Every component listed above is original work of the TextDeck project and is
covered by the repository-level `LICENSE`. No third-party source code is
vendored in this repository. No legacy `keyboard/` driver source and no
LizardByte / libvirtualhid code is included or referenced.

## Driver provenance

The kernel driver is an **independent implementation**. It was written from
scratch against publicly published sources only:

- Microsoft Windows Driver Frameworks (KMDF) documentation and the
  Virtual HID Framework (VHF) reference (`vhf.h` in the WDK):
  <https://learn.microsoft.com/windows-hardware/drivers/hid/virtual-hid-framework---vhf->
- "Defining I/O control codes" (`CTL_CODE`, `METHOD_BUFFERED`,
  `FILE_ANY_ACCESS`);
- the USB HID Usage Tables 1.12, section 10 (Keyboard/Keypad) — the usage
  *numbers* are a published standard;
- the WDK INF reference (CopyFiles, SourceDisksNames, AddService, Security).

TextDeck contains **no LizardByte / libvirtualhid / any virtual-HID vendor
code**. Nothing was copied from a GPL or otherwise restrictively licensed
driver.

## Mapping provenance

`windows/mapping/` is a clean-room implementation. The event/action vocabulary
was informed by the public READMEs of MIT-licensed remappers (DeviceMapper,
FlexInput, UCR); **no source code from those projects was copied**, and none of
their code is included here. AutoHotkey (GPL) and AntiMicroX (GPL) were
deliberately **excluded** as code sources to keep this repository's licensing
clean. The mapping module depends only on Windows system APIs.

## Build-time dependencies (not vendored)

| Dependency | Used by | License | Notes |
|---|---|---|---|
| MSVC / Windows SDK / WDK | app, driver, mapping | Microsoft proprietary toolchain | Required to build; not redistributed here. |
| Arduino-ESP32 core | firmware | LGPL-2.1 | Pulled by the Arduino/PlatformIO build; not vendored. |
| Python 3 (+ pyserial for device tools) | `tools/` | PSF / BSD-3 | Runtime tools only. |

## What this repository does not include

- No production-signed driver binaries and no code-signing certificates,
  private keys or PFX/P12 material of any kind.
- No third-party virtual-HID driver source.
- No generated build outputs (EXE/OBJ/PDB), test logs or machine-specific
  evidence files.

## Obligations when redistributing

Redistribution of the software (in source or binary form) for noncommercial
purposes must give recipients the `LICENSE` text (and any plain-text lines
beginning with `Required Notice:`) along with the copies. See the license
itself for the complete obligations — this file is a provenance record, not a
substitute for the license.
