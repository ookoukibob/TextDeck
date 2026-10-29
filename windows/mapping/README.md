# TextDeck Mapping

A general-purpose input-mapping and key-script engine: any input signal
(physical keyboard/mouse, gamepad, serial line, UDP message, timer) can be
mapped to keyboard events, text input or multi-step key scripts through the
same output backends the TextDeck app uses (virtual HID driver, SendInput, or a
recording backend for tests).

## Build

Requires Visual Studio Build Tools (MSVC) and a Windows SDK. From this
directory:

```powershell
powershell -File build_mapping.ps1
```

All binaries are written to `bin/` (not tracked in git):

| Binary | Purpose |
|---|---|
| `map_app.exe` | CLI entry point (validate configs, list devices, run mappings) |
| `map_engine_selftest.exe` | engine unit tests |
| `map_v1_selftest.exe` | trigger/config/adapter acceptance tests |
| `map_audit_test.exe` | regression tests for cancel/ownership/lifecycle fixes |
| `map_raw_input_test.exe` | Raw Input adapter tests |
| `map_mvp.exe` | six-scenario demo of the engine |
| `map_hid_e2e.exe` | end-to-end: engine -> real virtual-HID driver -> target window |
| `map_link_e2e.exe` | end-to-end: UDP adapter -> engine -> real HID -> target window |

## Run

```powershell
bin\map_app.exe example > my.json     # print a ready-to-run example config
bin\map_app.exe validate my.json      # validate; errors name the offending field
bin\map_app.exe devices               # list real Raw Input devices + XInput pads
bin\map_app.exe run my.json --udp 39999 --seconds 30
```

`run` options: `--output hid|sendinput|record` (default: the config's `output`
field, else `record`, which sends no keys), `--udp <port>` to accept UDP JSON
events, `--seconds <n>` to auto-stop (`0` = until Enter).

## Configuration format (JSON, version 1)

```json
{
  "version": 1,
  "output": "record",
  "profiles": {
    "default": [
      {
        "id": "space-on-button",
        "trigger": "momentary",
        "source": "xinput:0",
        "control": "a",
        "type": "pressed",
        "holdKey": "space"
      },
      {
        "id": "macro-on-double-tap",
        "trigger": "double_tap",
        "source": "xinput:0",
        "control": "b",
        "type": "pressed",
        "tapWindowMs": 300,
        "script": [
          { "action": "key_tap", "key": "a" },
          { "action": "delay", "ms": 50 },
          { "action": "type_text", "text": "hi" }
        ]
      }
    ]
  }
}
```

- **Triggers**: `press`, `release`, `momentary`, `toggle`, `threshold`
  (with hysteresis), `long_press`, `hold_repeat`, `double_tap`, `multi_tap`,
  `sequence` (ordered steps with timeout), `combo` (cross-device chords),
  `cooldown` (rate limit); per-binding `debounceMs`.
- **Actions**: `key_down`, `key_up`, `key_tap`, `key_combo`, `type_text`,
  `delay`, `repeat`, `conditional`, `profile_switch`, `cancel_sequence`.
- **Key names** are validated at load time (`a`-`z`, `0`-`9`, `f1`-`f24`,
  `ctrl`/`shift`/`alt`/`gui`, `enter`/`esc`/`tab`/`space`, arrows, US
  punctuation). Unknown names are rejected with a field-level error.
- A profile is either an array of bindings or `{"bindings":[...]}`. The
  `combo` trigger accepts full `source|control` ids (cross-device precision)
  or bare control names.

## Input adapters

Windows Raw Input (keyboard, mouse buttons/movement, device hot-plug), XInput
gamepads (buttons, sticks, triggers), serial line (newline-framed messages,
bounded reconnect), UDP JSON (`{"event":"press|release|value|message",...}`),
and timers. TextDeck's own virtual-HID output device (VID `1209`, PID `0001`)
is filtered out by structured device identity, so mappings cannot feed back
into themselves.

## Limitations

- `type_text` on the HID backend covers printable US-ASCII plus `\n`/`\t`;
  unsupported characters fail visibly instead of being dropped silently.
- Generic HID devices are enumerated and diagnosed but not decoded without a
  dedicated adapter (unknown report formats are never guessed).
- Hardware-dependent adapters (serial, XInput) require the respective device.
- The engine is a console/CLI product; there is no GUI in this module.

## License

PolyForm Noncommercial License 1.0.0 — see the repository root `LICENSE` and
`docs/LICENSE-AND-PROVENANCE.md`.
