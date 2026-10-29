# Building TextDeck

All paths below are relative to the repository root. Every build script derives
its own location from `$PSScriptRoot`, so the repository can live anywhere.

## Prerequisites

| Component | Requires |
|---|---|
| Windows app | Visual Studio 2022 Build Tools (MSVC v143), Windows SDK |
| Virtual HID driver | the above **plus** the Windows Driver Kit (WDK) 10.0.26100 |
| ESP32 firmware | Arduino IDE 2.x or arduino-cli, ESP32 board support (ESP32-S3) |
| Host tools | Python 3.9+ (`pyserial` for the desktop writer) |

The driver build script discovers installed Visual Studio/MSBuild toolchains rather than
calling `vcvarsall.bat`. The application build script currently expects the default
Visual Studio 2022 Build Tools path (`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`)
and the standard Windows SDK path; if your installation uses another location, update
`windows/VirtualHIDTyper/build_app.ps1` before building.

## Windows application

```powershell
pwsh windows/VirtualHIDTyper/build_app.ps1
```

Outputs:

```
windows/VirtualHIDTyper/bin/VirtualHIDTyper.exe   (GUI)
windows/VirtualHIDTyper/bin/map_selftest.exe      (console self-test)
```

The EXE embeds `VirtualHIDTyper.manifest` through `VirtualHIDTyper.rc`. The
manifest requests **`asInvoker`**, not `requireAdministrator`: the app runs
unelevated so it can be used by a normal user, and only the driver
install/uninstall steps need an Administrator console. (Earlier revisions of
this page said `requireAdministrator`, which contradicted the manifest.)

## Virtual HID driver

```powershell
pwsh windows/driver-tools/build_driver.ps1
```

The script **detects** the toolchain instead of hardcoding a path: it asks
`vswhere` for every Visual Studio installation that provides MSBuild, prefers one
that also carries the `WindowsKernelModeDriver10.0` platform toolset (a newer VS
without the WDK VSIX would otherwise win and then fail on `ntddk.h`), and picks
the newest MSVC toolset present. The detected MSBuild, `cl.exe`, MSVC version and
WDK/SDK version are printed and recorded in `build-logs/<LogName>.log.meta`.

Override with `-MSBuildPath <path>` or `-ToolsetVersion <version>` when needed.
Therefore a specific compiler build number (such as `14.44.35207`) is never
required.

Or directly with MSBuild:

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" `
  windows/driver/TextDeckHidSolution.sln `
  /t:Build /p:Configuration=Debug /p:Platform=x64 `
  /p:SignMode=Off /p:Inf2CatUseLocalTime=true
```

Outputs (unsigned):

```
windows/driver/TextDeckHidDriver/x64/Debug/TextDeckHidDriver.sys
windows/driver/TextDeckHidDriver/x64/Debug/TextDeckHidDriver/textdeckhiddriver.cat
windows/driver/TextDeckHidDriver/x64/Debug/TextDeckHidDriver/TextDeckHidDriver.inf
```

**Signing is off by default.** Enabling test signing or importing a certificate
is an explicit operator action — see `docs/SIGNING.md`.

Note on `/p:Inf2CatUseLocalTime=true`: `stampinf` writes `DriverVer` from the
local clock while `Inf2Cat` defaults to UTC, so on machines east of UTC the
build fails during the hours where the local date is already "tomorrow" in UTC.
The switch makes the verifier use the same clock the stamper used; it does not
relax any signability check.

## Driver install / uninstall

```powershell
# Read-only preflight: verifies package signature, target policy, and that no
# matching device instance already exists. Changes nothing.
pwsh windows/driver-tools/install_driver.ps1 -CheckOnly

# Install (first-time only; refuses if an instance already exists)
pwsh windows/driver-tools/install_driver.ps1

# Remove the device and the staged package (never touches other devices)
pwsh windows/driver-tools/uninstall_driver.ps1
```

The installer fails closed: if the catalog signature does not validate, or does
not cover the INF and SYS, it refuses and stages nothing.

## ESP32-S3 firmware

The board must be configured with the menu options below — the bare
`esp32:esp32:esp32s3` FQBN compiles a *different* device (4 MB flash, PSRAM off,
core USB CDC on boot) and is not the shipping configuration.
`USBMode=default` selects USB-OTG/TinyUSB, which the CDC + HID composite
requires; `CDCOnBoot=default` is deliberate so the core does not claim the USB
serial port the firmware drives itself.

```powershell
$FQBN = "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=default,CDCOnBoot=default"
arduino-cli compile --fqbn $FQBN firmware/esp32_hid_text_device
arduino-cli upload  --fqbn $FQBN --port COMx firmware/esp32_hid_text_device
```

Host-side state-machine tests (no hardware needed):

```powershell
pwsh firmware/esp32_hid_text_device/test/build_state_test.ps1
```

## Host tools

```powershell
python -m pip install -r tools/esp32-desktop/requirements.txt
python tools/esp32-desktop/hid_text_writer.py --help
```

Protocol conformance tests:

```powershell
python tools/esp32/protocol_edge_tests.py
python tools/esp32/fault_inject_tests.py
```

## TextDeck Mapping

```powershell
cd windows/mapping
powershell -NoProfile -ExecutionPolicy Bypass -File build_mapping.ps1
```

Binaries land in `windows/mapping/bin/` (see `windows/mapping/README.md` for
`map_app` usage and the configuration format).

## Tests

| Suite | Command |
|---|---|
| HID mapping self-test | `windows/VirtualHIDTyper/bin/map_selftest.exe` |
| Installer gate regression | `pwsh windows/driver-tools/tests/test_install_gate.ps1` |
| Uninstaller fail-closed source checks | `pwsh windows/driver-tools/tests/test_uninstall_fail_closed.ps1` |
| Driver IOCTL + ABI contract | `python windows/driver-tests/validate_ioctl_contract.py` |
| Driver logic (host-side) | `pwsh windows/driver-tests/build_logic_test.ps1` |
| Firmware state machine | `pwsh firmware/esp32_hid_text_device/test/build_state_test.ps1` |
| ESP32 protocol edges | `python tools/esp32/protocol_edge_tests.py` |
| ESP32 fault injection | `python tools/esp32/fault_inject_tests.py` |
| App E2E (HID) | `pwsh windows/VirtualHIDTyper/e2e_hid_mode.ps1` |
| App E2E (tray + hotkeys) | `pwsh windows/VirtualHIDTyper/e2e_tray_hotkey.ps1` |
| App E2E (SendInput) | `pwsh windows/VirtualHIDTyper/e2e_software_mode.ps1` |
| Mapping engine unit tests | `windows/mapping/bin/map_engine_selftest.exe` |
| Mapping acceptance tests | `windows/mapping/bin/map_v1_selftest.exe` |
| Mapping audit regressions | `windows/mapping/bin/map_audit_test.exe` |
| Mapping Raw Input tests | `windows/mapping/bin/map_raw_input_test.exe` |
| Mapping E2E (real HID output) | `windows/mapping/bin/map_hid_e2e.exe` (needs the driver) |
| Mapping E2E (UDP -> HID link) | `windows/mapping/bin/map_link_e2e.exe` (needs the driver) |

`build_logic_test.ps1` extracts `TextDeckBuildReport()` verbatim from the
driver's `Device.c` into a generated `driver_logic_extracted.inc` and compiles
that, so the test exercises the shipping validation code rather than a copy.

The E2E suites that exercise the virtual HID backend need the driver installed
and an elevated shell. They are reported as NOT RUN when those prerequisites are
absent — never as passing.
