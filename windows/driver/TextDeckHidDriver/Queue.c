/*++
TextDeck - Module: Queue.c - IOCTL dispatch and request validation.

Independently implemented for TextDeck. References: WDK "Defining I/O control
codes", "Handling IOCTLs in KMDF drivers", and the VHF reference.

Contract (see windows/shared/textdeck_protocol.h):

  IOCTL_TEXTDECK_GET_INFO     out: TEXTDECK_DEVICE_INFO
  IOCTL_TEXTDECK_SUBMIT_KEY   in : TEXTDECK_KEY_EVENT   (exactly 4 bytes)
  IOCTL_TEXTDECK_RELEASE_ALL  -   no buffers
  IOCTL_TEXTDECK_GET_LED_STATE out: TEXTDECK_LED_STATE

Every branch validates the buffer length and the payload BEFORE touching VHF.
A rejected request returns a specific NTSTATUS and never sends a report, so a
malformed or malicious request cannot leave a key pressed.

Buffers are copied out of the WDF request into local storage first: the report
we submit must not alias a caller-owned buffer that could change underneath the
asynchronous VHF submit.
--*/

#include "Driver.h"

/* -------------------------------------------------------------------------
 * TextDeckHandleGetInfo
 * ------------------------------------------------------------------------- */
static VOID
TextDeckHandleGetInfo(
    _In_ WDFREQUEST Request
)
{
    TEXTDECK_DEVICE_INFO info;
    WDFMEMORY outputMemory;
    PVOID outputBuffer;
    size_t outputSize;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputMemory(Request, &outputMemory);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    outputBuffer = WdfMemoryGetBuffer(outputMemory, &outputSize);
    if (outputBuffer == NULL || outputSize < sizeof(TEXTDECK_DEVICE_INFO)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    RtlZeroMemory(&info, sizeof(info));
    info.ProtocolMajor           = TEXTDECK_PROTOCOL_MAJOR;
    info.ProtocolMinor           = TEXTDECK_PROTOCOL_MINOR;
    info.ReportDescriptorVersion = 1;
    /* The driver's real concurrency, not the descriptor's slot count. The
     * descriptor carries six key slots, but TextDeckBuildReport writes only
     * slot 0, so one non-modifier key can be held at a time. Reporting 6 here
     * would let a client believe it can hold two keys and silently lose one. */
    info.MaxConcurrentKeys       = TEXTDECK_MAX_CONCURRENT_KEYS;

    RtlCopyMemory(outputBuffer, &info, sizeof(info));
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(info));
}

/* -------------------------------------------------------------------------
 * TextDeckHandleSubmitKey
 * ------------------------------------------------------------------------- */
static VOID
TextDeckHandleSubmitKey(
    _In_ WDFREQUEST Request,
    _In_ PTEXTDECK_DEVICE_CONTEXT Context
)
{
    WDFMEMORY inputMemory;
    PVOID inputBuffer;
    size_t inputSize;
    TEXTDECK_KEY_EVENT event;
    UCHAR report[TEXTDECK_REPORT_SIZE];
    NTSTATUS status;

    status = WdfRequestRetrieveInputMemory(Request, &inputMemory);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    inputBuffer = WdfMemoryGetBuffer(inputMemory, &inputSize);
    if (inputBuffer == NULL || inputSize != sizeof(TEXTDECK_KEY_EVENT)) {
        /* Exact size: a shorter or longer buffer means the caller and the
         * driver disagree about the protocol, which must not be guessed at. */
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    /* Copy out: the submit below must not alias caller memory. */
    RtlCopyMemory(&event, inputBuffer, sizeof(event));

    status = TextDeckBuildReport(&event, report);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    status = TextDeckSubmitReport(Context, report);
    WdfRequestComplete(Request, status);
}

/* -------------------------------------------------------------------------
 * TextDeckHandleGetLedState
 *
 * Reports the LED bitmap the OS last wrote. Observation only: this driver is a
 * transport, and toggling Caps Lock itself would fight the OS keyboard stack.
 * ------------------------------------------------------------------------- */
static VOID
TextDeckHandleGetLedState(
    _In_ WDFREQUEST Request,
    _In_ PTEXTDECK_DEVICE_CONTEXT Context
)
{
    WDFMEMORY outputMemory;
    PVOID outputBuffer;
    size_t outputSize;
    TEXTDECK_LED_STATE state;
    LONG leds;
    NTSTATUS status;

    status = WdfRequestRetrieveOutputMemory(Request, &outputMemory);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    outputBuffer = WdfMemoryGetBuffer(outputMemory, &outputSize);
    if (outputBuffer == NULL || outputSize < sizeof(TEXTDECK_LED_STATE)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    /* Published by the output-report callback at <= DISPATCH_LEVEL; a plain
     * atomic read gives a consistent single byte without a lock. */
    leds = InterlockedCompareExchange(&Context->LedState, 0, 0);
    RtlZeroMemory(&state, sizeof(state));
    state.Leds = (UCHAR)(leds & TEXTDECK_LED_VALID_MASK);

    RtlCopyMemory(outputBuffer, &state, sizeof(state));
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(state));
}

/* -------------------------------------------------------------------------
 * TextDeckEvtIoDeviceControl
 * ------------------------------------------------------------------------- */
VOID
TextDeckEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    WDFDEVICE device;
    PTEXTDECK_DEVICE_CONTEXT context;

    device = WdfIoQueueGetDevice(Queue);
    context = TextDeckGetDeviceContext(device);

    if (context == NULL || !context->VhfStarted) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    switch (IoControlCode) {

    case IOCTL_TEXTDECK_GET_INFO:
        /* GET_INFO has no input payload; reject unexpected request bytes. */
        if (InputBufferLength != 0) {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            break;
        }
        TextDeckHandleGetInfo(Request);
        break;

    case IOCTL_TEXTDECK_SUBMIT_KEY:
        /* SUBMIT_KEY is input-only; no output buffer is part of this contract. */
        if (OutputBufferLength != 0) {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            break;
        }
        TextDeckHandleSubmitKey(Request, context);
        break;

    case IOCTL_TEXTDECK_RELEASE_ALL:
        /* RELEASE_ALL is a no-buffer command. */
        if (InputBufferLength != 0 || OutputBufferLength != 0) {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            break;
        }
        WdfRequestComplete(Request, TextDeckReleaseAll(context));
        break;

    case IOCTL_TEXTDECK_GET_LED_STATE:
        /* GET_LED_STATE has an output payload but no input payload. */
        if (InputBufferLength != 0) {
            WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
            break;
        }
        TextDeckHandleGetLedState(Request, context);
        break;

    default:
        /* Unknown code: explicit failure, never a silent success. */
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        break;
    }
}
