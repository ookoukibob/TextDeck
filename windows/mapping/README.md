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
| `map_cancel_test.exe` | cancellation / emergency-stop regression tests |
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

## Cancellation and emergency stop

Cancellation is a two-part handshake over one monotonic *epoch* counter, so a
request can never be lost to a race with dispatch and a stale request can never
cancel a later action.

| Concept | Meaning |
|---|---|
| `requestCancel()` | Abort the script that is running (per-device disconnect). Returns the epoch. |
| `requestStop()` | The above **plus** "no new action may begin" (emergency stop). |
| `cancelAll(epoch)` | Executor-side cleanup: release every key, clear trigger latches, acknowledge `epoch`. |
| `cancelSource(src, epoch)` | Per-device cleanup: release only that source's keys, acknowledge `epoch`. |
| `stopPending()` | True while an accepted stop has not been acknowledged. |

Rules the implementation holds:

- `Engine::dispatch()` never clears cancellation state. An outstanding stop
  refuses the dispatch entirely; a request that arrives between two matching
  rules stops the loop mid-dispatch.
- Every nested block (repeat, conditional) runs through one `runBlock()` gate
  that checks cancellation **before and after** each step, so an interrupted
  `delay` cannot let the rest of the block continue.
- `typeText()` receives a `CancelProbe` and polls it between characters only —
  never between the down/up legs of one stroke — so an interrupted run cannot
  leave a key or modifier pressed. `TextOutcome` keeps *cancelled* (expected)
  and *failed* (reported via `textFailures()`) distinct.
- A script cancelled from outside has its own key ownership released, so a
  half-issued chord does not stay held.
- `MappingRuntime::postCancelAll()` publishes the request **before** queueing the
  control item, so an already-running synchronous dispatch observes it without
  waiting for the queue. With no executor running, the request is acknowledged
  inline (nothing would ever pop the item).
- Actions queued before an applied stop are discarded and counted
  (`eventsDroppedByCancel()`); actions posted after it completes run normally.
  The boundary is a queue-level **enqueue sequence**: `post()` takes its sequence
  number under the queue mutex and a stop takes a barrier under that same mutex,
  so an event is unambiguously before or after the stop. No engine state is read
  to decide it, which is what keeps a producer that is mid-`post()` safe.
- Replaying a stop whose cleanup already ran is a no-op, so a duplicate control
  item cannot wipe the key state of a later, accepted action.

### Runtime lifecycle

`MappingRuntime` distinguishes two facts that must not be conflated:

| Flag | Meaning |
|---|---|
| `stopRequested_` | the executor loop must exit (set before `join()`) |
| `running_` | an executor thread exists (true until `join()` completes) |

A producer may only mutate `Engine` inline (the "no executor" fast path) when
`running_` is false, which by construction means the thread is gone. `stop()` and
`start()` hold the lifecycle mutex across the whole transition, so the inline
decision cannot be evaluated while a join is in progress.

Lock order: `lifeMu_` -> queue mutex -> engine. Neither the executor nor `Engine`
ever takes `lifeMu_`, so the order cannot invert. No blocking output call is made
while holding the queue mutex.

### Stop ordering (linearization point)

Inside one stop step the executor does exactly this, in this order:

1. release every key and clear every trigger latch;
2. take the queue barrier, dropping events enqueued at or before it;
3. only then acknowledge the stop, which is what makes it externally "complete".

Acknowledging before step 2 let a producer observe completion, post a legitimate
event, and lose it to the sweep that ran afterwards. The no-executor path in
`postCancelAll()` uses the same three-step order.

### Output release accounting

`SendInputBackend::send()` checks the value returned by `SendInput()`. A physical
KeyUp that Windows did not accept is reported as a failure. A `SendInput` success
only means Windows accepted the INPUT structure; it is not a claim about what the
target application did.

Active holder ownership and an outstanding physical release are **separate
facts**:

| State | Meaning | Accessor |
|---|---|---|
| `keyRefs_` | active holders (refcounted) | `heldKeys()`, `isHeld()` |
| `unresolved_` | a physical KeyUp that was refused | `unresolvedKeys()`, `isUnresolved()` |

A refused release clears the holder count (nobody owns the key any more) but
records the outstanding physical up, so `heldKeys()` never reports a key as held
by someone who released it. `retryUnresolved()` refuses to send a KeyUp while any
active holder still owns that key, because doing so would physically release
another input source's key. Once the last holder releases, the normal release
path retries the orphan itself; an explicit retry also clears it. Emergency
`releaseAll()` drops every holder but retains and reports any key whose physical
up could not be delivered.

### Stop ordering: cleanup vs acknowledgement APIs

`Engine::cancelAll(epoch)` is *cleanup plus acknowledgement*: with its default
epoch it snapshots the current epoch and acknowledges it internally. It must
therefore **never** be used as the first step of a queue-managed stop, because
that would make the stop observable before the FIFO barrier runs. The queued and
no-executor paths both use the split form:

```
engine_.releaseAllKeys();      // (1) cleanup only, no acknowledgement state
queue_.applyStopBarrier();     // (2) discard events enqueued at or before now
engine_.acknowledgeStop(ep);   // (3) publish completion
```

A direct `Engine::cancelSource()` / `cancelAll()` with the default
`requestEpoch == 0` acknowledges exactly the epoch it snapshotted at entry: a
request it handled is never left pending, and a newer request it did not cover
stays pending. Explicit-epoch calls behave the same way, which is what queued
control items rely on.

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
