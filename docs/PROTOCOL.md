# TextDeck keyboard protocol

The contract between the TextDeck application and its virtual HID driver: what a
`TEXTDECK_KEY_EVENT` means, what HID report it produces, and what a client has to
send to express a chord.

Current version: **1.1** (`TEXTDECK_PROTOCOL_MAJOR` 1, `TEXTDECK_PROTOCOL_MINOR` 1).

---

## 1. The one rule

> `Modifiers` is the **complete set of modifiers that must be held down in the
> report this event produces**. It is not "the modifiers attached to this event".

Every accepted event yields exactly one 8-byte HID input report:

| byte | meaning |
|---|---|
| 0 | modifier bitmap (`TEXTDECK_MOD_*`) — taken verbatim from `Modifiers` |
| 1 | reserved, always 0 (the descriptor declares it Constant) |
| 2..7 | key slots; `Down` writes `Usage` into slot 0, everything else leaves them 0 |

**Only one key can be down at a time, and `MaxConcurrentKeys` now says so.**

Two different numbers exist and are deliberately kept apart:

| Macro | Value | Meaning |
|---|---|---|
| `TEXTDECK_REPORT_KEY_SLOTS` | 6 | how many key slots the **descriptor** declares (wire capacity) |
| `TEXTDECK_MAX_CONCURRENT_KEYS` | 1 | how many non-modifier keys the **driver** actually holds down |

`TextDeckBuildReport` writes exactly one slot (slot 0) and leaves the rest zero,
so the real capability is 1. `TEXTDECK_GET_INFO.MaxConcurrentKeys` reports
`TEXTDECK_MAX_CONCURRENT_KEYS`, because the field name promises concurrency, not
descriptor capacity.

Before this correction the field reported 6 — the slot count — which overstated
the driver by 6×. No client read it, so nothing broke, but a client that had
trusted it would have tried to hold two keys and silently lost one. The contract
test now asserts all three facts independently: the descriptor declares six
slots, the macro says one, and `GET_INFO` reports the concurrency macro rather
than the slot macro. The logic test additionally asserts that only slot 0 is ever
written, so a future change that starts using more slots fails a test instead of
silently disagreeing with the reported number.

Modifiers are unaffected by this limit: they live in byte 0 and any combination
can be held at once.

Because the modifier byte is *state* rather than *decoration*, a modifier stays
held for exactly as long as the client keeps setting its bit. That is what makes
a chord expressible: a chord is a **sequence of reports**, and the modifier bit is
present in each one until the client clears it.

## 2. Actions

| value | name | usage | report produced |
|---|---|---|---|
| 0 | `Down` | required, `0x04..0x65` | `Modifiers` + `Usage` in slot 0 |
| 1 | `Up` | `0` or `0x04..0x65` | `Modifiers` only; every key slot cleared |
| 2 | `ReleaseAll` | ignored | all-zero |
| 3 | `Modifiers` | **must be 0** | `Modifiers` only; every key slot cleared |

`Modifiers` (action 3) and `Up` with an empty key slot produce the same report
shape. `Modifiers` exists because "hold these modifiers and press nothing" is a
real state a keyboard reports, and spelling it as `Up` with a fake usage would
make the wire format ambiguous.

## 3. What changed from 1.0, and why

Protocol 1.0 could not express a held modifier. Two independent defects:

1. **`Up` always emitted an all-zero report.** Releasing the letter of Ctrl+A
   therefore released Ctrl as well, so the modifier never survived a key release.
2. **`Down` with `Usage = 0` was rejected.** A real keyboard sends exactly that
   report when Ctrl goes down on its own, so "Ctrl is held" had no encoding at
   all.

Together those meant every chord that needs a modifier across reports —
Ctrl+C, Ctrl+V, Alt+Tab, Ctrl+Shift+… — was unrepresentable. Shift+A still
worked, because the modifier and the letter fit in a single report, which is why
the defect looked like "Ctrl is broken" rather than "the protocol cannot hold a
modifier".

The 1.1 fix is two lines of behaviour:

- `Up` writes `Report[0] = Event->Modifiers` instead of leaving it zero;
- action 3 is added, and `Down` keeps its `Usage` requirement.

## 4. Compatibility

The struct, its size (4 bytes) and actions 0..2 are unchanged, so a 1.0 client
keeps working:

- a 1.0 client sends `Modifiers = 0` on `Up`, so it still gets the all-zero report
  it always got;
- a 1.0 client never sends action 3.

Only a client that wants to hold a modifier starts filling `Modifiers` on `Up`, or
uses action 3. The minor version is bumped so the application can tell the two
drivers apart; the application refuses to type when the **major** version differs,
which is unchanged.

## 5. Sending a chord

Ctrl+A, as a real keyboard produces it:

| # | `Modifiers` | `Usage` | `Action` | report |
|---|---|---|---|---|
| 1 | `0x01` (LCtrl) | `0x00` | 3 (`Modifiers`) | `01 00 00 00 00 00 00 00` |
| 2 | `0x01` | `0x04` (A) | 0 (`Down`) | `01 00 04 00 00 00 00 00` |
| 3 | `0x01` | `0x04` | 1 (`Up`) | `01 00 00 00 00 00 00 00` |
| 4 | `0x00` | `0x00` | 3 (`Modifiers`) | `00 00 00 00 00 00 00 00` |

Step 3 is the one 1.0 could not do: the key is released while the modifier stays.

Two modifiers (Ctrl+Shift+A) is the same sequence with `Modifiers = 0x03`.

To release everything without tracking state, call `IOCTL_TEXTDECK_RELEASE_ALL`;
it submits the all-zero report and is also run when the client handle closes and
during device cleanup, so an aborted run cannot leave a key held.

## 6. Validation the driver performs

Rejected with `STATUS_INVALID_PARAMETER` (user-mode `ERROR_INVALID_PARAMETER`):

- `Reserved != 0` — the byte is declared Constant; a caller filling it is
  speaking a different protocol;
- `Usage` outside `0x04..0x65` on `Down`; outside `0` or that range on `Up`;
- `Usage != 0` on `Modifiers` — a non-zero usage there would be a second spelling
  of `Down`;
- an unknown `Action`;
- an input buffer that is not exactly `sizeof(TEXTDECK_KEY_EVENT)`.

`ReleaseAll` accepts anything, because its whole purpose is to clear state when
the caller may not know what is held.

Every rejection happens before the report is built, so a rejected event cannot
leave a partially-applied key state.

## 7. Where the modifier state lives

In the **client's** head, not the driver's. The driver is a transport: it holds no
keyboard state and remembers nothing between IOCTLs. A client that wants to hold
Ctrl must therefore keep sending `Modifiers = 0x01` on every event until it
releases Ctrl.

This is deliberate. Driver-side state would need locking against concurrent
clients, would have to be reset on every teardown path, and would make the
protocol non-idempotent — the same event would mean different things depending on
history. Making the report self-describing keeps every event independently
verifiable, which is also why the tests can assert one report at a time.

The cost: a client that forgets to clear a modifier leaves it held until it calls
`ReleaseAll` or closes the handle. Both paths are covered.
