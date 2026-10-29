# VHF report-submission design

Why the TextDeck HID driver submits reports the way it does, and what was wrong
before. This page exists because the previous design was self-contradictory in a
way that only reading the official contract makes obvious.

## The two documented contracts

From [`VhfReadReportSubmit`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/nf-vhf-vhfreadreportsubmit),
Remarks:

> The HID source driver can choose to implement its buffering policy or let
> Virtual HID Framework (VHF) handle buffering.
>
> If the driver uses its own buffering policy, then it must implement and
> register an `EvtVhfReadyForNextReadReport` callback function in its call to
> `VhfCreate`. It must call `VhfReadReportSubmit` only once after VHF has invoked
> `EvtVhfReadyForNextReadReport`. After the callback has been invoked, the driver
> can reuse the transfer buffer pointed to by `HidTransferPacket`. The driver
> must wait for the next time that VHF invokes
> `EvtVhfReadyForNextReadReport` before calling this method again.
>
> If the HID source driver does not implement the `EvtVhfReadyForNextReadReport`
> callback, then there are no restrictions on calling this method. VHF uses the
> default buffering policy. The driver can reuse the transfer buffer after the
> call returns.

So there are exactly two valid modes:

| | Callback registered | Submit rule | Buffer lifetime |
|---|---|---|---|
| **A. Driver buffering** | yes | **exactly once** per callback | must stay valid until the *next* callback |
| **B. VHF default buffering** | no | unrestricted | reusable **as soon as the call returns** |

Mode A is a state machine. Mode B is a plain function call.

## What was wrong before

The earlier driver did all of the following at once:

1. registered `EvtVhfReadyForNextReadReport`, putting it in mode A;
2. called `VhfReadReportSubmit` in a retry loop of up to `TEXTDECK_SUBMIT_MAX_ATTEMPTS`
   iterations per IOCTL — violating "only once after VHF has invoked" it;
3. passed `HID_XFER_PACKET.reportBuffer = report`, a **local stack array** in
   `TextDeckHandleSubmitKey`. Under mode A the buffer must remain valid until the
   next readiness callback, so it could be read after the stack frame was gone;
4. declared the callback parameter as `VHFHANDLE VhfHandle` and compared it to
   `context->VhfHandle`. The documented parameter is `PVOID VhfClientContext` —
   the pointer supplied in `VHF_CONFIG.VhfClientContext`. Making that comparison
   work required a module-level global (`g_TextDeckCallbackContext`) purely to
   recover the context the API was already handing over;
5. reasoned — in comments and in the test suite — that "submit first, wait after"
   was needed to avoid a first-report deadlock. That rationale only applies to a
   readiness-gated design. Under mode B there is nothing to wait for, so the rule
   was protecting against a hazard the code itself created.

Separately, the device context contained a `KSPIN_LOCK SubmitLock` that was
initialised, documented as "serialises report submission", and then **never
acquired**, while the default IOCTL queue is `WdfIoQueueDispatchParallel`. Two
concurrent clients could therefore have their down/up reports reordered and latch
a key. A spinlock would also have been the wrong primitive here: the critical
section spans `VhfReadReportSubmit`, which may block, and teardown runs at
PASSIVE_LEVEL and must wait.

## What the driver does now

**Buffering: VHF default (mode B).** `EvtVhfReadyForNextReadReport` is not
registered, so the documented rule is "no restrictions" and the buffer is
reusable the moment the call returns. `TextDeckSubmitReport` still copies the
caller's report into a local buffer first — cheap, and it makes the guarantee
local rather than inherited from `METHOD_BUFFERED`.

**Ordering: a KMUTEX, not the queue.** Mode B removes the readiness state machine
but says nothing about ordering, and "unrestricted" is not "unordered". Every
path that touches VHF (`submit`, `release-all`, teardown release) runs under
`Context->SubmitMutex`. A mutex because:

- it is held across `VhfReadReportSubmit`, which may block — illegal for a spinlock;
- teardown (`EvtDeviceCleanup`, PASSIVE_LEVEL) must acquire it and cannot spin.

The default queue stays `WdfIoQueueDispatchParallel` on purpose: serialising the
queue would let one stuck client block every other client. Ordering is a property
of the submit path, not of the queue.

**Teardown order** (`TextDeckEvtDeviceCleanup`):

1. acquire `SubmitMutex` — no submit is mid-flight;
2. `InterlockedExchange(&VhfStarted, FALSE)` — new submits fail fast;
3. submit a final all-zero report *while VHF is still alive*, directly under the
   already-held mutex;
4. `VhfDelete(handle, TRUE)` — waits for outstanding VHF callbacks, including the
   output-report callback, so the context it references is provably unused;
5. release the mutex.

`VhfDelete(..., TRUE)` is what makes it safe to hand `context` to `VhfCreate` as
`VhfClientContext` without a separate refcount.

## LED / output reports

The descriptor declares an Output report (0x91 items: five LED usages plus three
constant padding bits). A declared-but-unhandled output report leaves the OS write
pending, so the driver registers
`EvtVhfAsyncOperationWriteReport`. That callback:

- runs at `<= DISPATCH_LEVEL` and therefore neither blocks nor allocates;
- decodes through `TextDeckDecodeLedReport`, a pure function the host-side logic
  test extracts and exercises verbatim;
- records the bitmap in `LedState` (published with `InterlockedExchange`);
- **always** calls `VhfAsyncOperationComplete`, even for a packet it rejects.

The driver is a transport, so it does not itself toggle Caps Lock;
`IOCTL_TEXTDECK_GET_LED_STATE` exposes the observed state to the client.

## What the tests now assert

`windows/driver-tests/validate_ioctl_contract.py` previously required
"submit is attempted before waiting (no first-report deadlock)" — a property that
only a readiness-gated design needs, asserted here without justification from the
API contract. It is replaced by checks that encode the actual contract:

- the buffering mode is unambiguous (no callback registered while submitting freely);
- no callback treats `VhfClientContext` as a `VHFHANDLE`;
- the submitted packet points at driver-owned storage, not caller memory;
- submission is guarded by a mutex, and **no** spinlock is acquired anywhere;
- the mutex is never held while waiting on a kernel event;
- teardown marks VHF unusable before `VhfDelete`, releases keys first, and waits;
- the descriptor's output report and the driver's handler agree.

## What this still does not prove

These are static and host-side checks. They cannot prove runtime behaviour of
VHF, KMDF queueing, IRQL handling or the device lifecycle. Those require loading
the driver on a machine that permits it — see `docs/SIGNING.md`.
