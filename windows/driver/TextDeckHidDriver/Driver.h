/*++
TextDeck - KMDF/VHF virtual HID keyboard driver (public header)
==============================================================

Independently implemented for TextDeck. Technical references:

  * Virtual HID Framework (VHF)
    https://learn.microsoft.com/windows-hardware/drivers/hid/virtual-hid-framework--vhf-
  * VhfReadReportSubmit / EVT_VHF_READY_FOR_NEXT_READ_REPORT (vhf.h in the WDK)
  * Kernel-Mode Driver Framework (KMDF) programming guide
  * Defining I/O control codes
  * USB HID Usage Tables 1.12, section 10

No third-party driver source was used, copied, translated or mechanically
refactored. The layout below follows the Microsoft KMDF driver skeleton that the
WDK documentation presents for every KMDF driver (DriverEntry -> device add ->
queue -> IOCTL dispatch); the VHF usage follows the documented VhfCreate /
VhfStart / VhfReadReportSubmit / VhfDelete sequence.

Buffering model
---------------
This driver does NOT register EvtVhfReadyForNextReadReport, so VHF applies its
default buffering policy. The documented contract then reads:

    "If the HID source driver does not implement the EvtVhfReadyForNextReadReport
     callback, then there are no restrictions on calling this method. VHF uses
     the default buffering policy. The driver can reuse the transfer buffer
     after the call returns."

Registered-callback mode has a much stricter contract (submit exactly once per
callback, and the buffer must stay valid until the next callback). An earlier
revision of this driver registered the callback AND looped on submit, honouring
neither rule; see docs/VHF-DESIGN.md for the analysis and the decision.
--*/

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <vhf.h>
#include <ntstrsafe.h>

#include "../shared/textdeck_protocol.h"
#include "ReportDescriptor.h"

/* -------------------------------------------------------------------------
 * Device context
 *
 * Everything here has the device's lifetime.
 * ------------------------------------------------------------------------- */
typedef struct _TEXTDECK_DEVICE_CONTEXT {
    VHFHANDLE VhfHandle;

    /* Serialises every path that talks to VHF: key submit, release-all, LED
     * output handling and the teardown release. A KMUTEX (not a spinlock) is
     * required because the guard is held across VhfReadReportSubmit, which is
     * allowed to block, and because the teardown path runs at PASSIVE_LEVEL
     * and must be able to wait. A spinlock must never be held across a call
     * that can block; the previous revision did exactly that with a
     * KSPIN_LOCK that was in any case never acquired.
     *
     * Lock order: SubmitMutex is acquired in isolation and never nested. */
    KMUTEX    SubmitMutex;

    /* Latest LED state reported by the OS (Caps/Num/Scroll/Compose/Kana).
     * Written by the output-report callback (<= DISPATCH_LEVEL), so it is a
     * plain byte published with a memory barrier rather than a locked
     * structure: readers only need a single-byte consistent value. */
    volatile LONG LedState;

    /* True between VhfStart success and teardown. Read lock-free by the
     * dispatch path; the authoritative teardown serialisation is the
     * mutex plus VhfDelete(Wait=TRUE). */
    volatile LONG VhfStarted;
} TEXTDECK_DEVICE_CONTEXT, *PTEXTDECK_DEVICE_CONTEXT;

/* -------------------------------------------------------------------------
 * TextDeckDiag / TextDeckDiagValue
 *
 * Writes the current stage number and NTSTATUS to
 *   HKLM\SOFTWARE\TextDeckDiag  { Stage, Status, LastStage }
 * so a failed load can be attributed to an exact call site without a kernel
 * debugger. TextDeckDiagValue records one extra named DWORD; it is used for the
 * IRQL that VhfCreate was actually called at, which is a documented
 * precondition rather than a tunable. Both are best-effort: any failure is
 * ignored and never affects the driver's control flow.
 * ------------------------------------------------------------------------- */
VOID
TextDeckDiag(
    _In_ ULONG    Stage,
    _In_ NTSTATUS Status
    );

VOID
TextDeckDiagValue(
    _In_ PCWSTR Name,
    _In_ ULONG  Value
    );

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TEXTDECK_DEVICE_CONTEXT, TextDeckGetDeviceContext)

/* The device interface GUID as an OBJECT. The macro in the shared header is a
 * brace-initialiser, which cannot be used with the address-of operator in C.
 * Declared here, DEFINED once in Device.c (see INITGUID there). */
extern const GUID GUID_DEVINTERFACE_TEXTDECK_HID_OBJ;

/* How many times a single IOCTL may retry a busy VHF queue. With the default
 * buffering policy a busy return is transient (VHF drains its ring), so a short
 * bounded retry is enough and keeps the IOCTL from blocking a client forever. */
#define TEXTDECK_SUBMIT_MAX_ATTEMPTS   8

/* Delay between busy retries. VHF is given a chance to drain without burning
 * a core; the total worst case stays far below any client timeout. */
#define TEXTDECK_SUBMIT_RETRY_DELAY_100NS  (-1LL * 10000LL)   /* 1 ms */

/* -------------------------------------------------------------------------
 * Driver entry points (implemented in Driver.c / Device.c / Queue.c)
 * ------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD        TextDeckEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP   TextDeckEvtDriverContextCleanup;

EVT_WDF_OBJECT_CONTEXT_CLEANUP   TextDeckEvtDeviceCleanup;
EVT_WDF_DEVICE_FILE_CREATE       TextDeckEvtDeviceFileCreate;
EVT_WDF_FILE_CLOSE               TextDeckEvtFileClose;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL TextDeckEvtIoDeviceControl;

/* VHF output-report callback (Caps/Num/Scroll LED state from the OS). */
EVT_VHF_ASYNC_OPERATION TextDeckEvtVhfAsyncOperationWriteReport;

#ifdef __cplusplus
}
#endif

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Builds and submits one 8-byte keyboard report while holding SubmitMutex.
 * The report is copied into driver-owned storage for the duration of the
 * VHF call, so a caller-owned buffer can never be read asynchronously.
 * Returns the NTSTATUS from VHF, or a driver-level status if not ready. */
NTSTATUS
TextDeckSubmitReport(
    _In_ PTEXTDECK_DEVICE_CONTEXT Context,
    _In_reads_bytes_(TEXTDECK_REPORT_SIZE) const UCHAR* Report
);

/* Releases every key and modifier. Safe on teardown paths: it acquires the
 * submit mutex (never blocks indefinitely because callers are at PASSIVE_LEVEL)
 * and ignores a closed VHF instance. */
NTSTATUS
TextDeckReleaseAll(
    _In_ PTEXTDECK_DEVICE_CONTEXT Context
);

/* Validates one LED output report from the OS and extracts the LED bitmap.
 * Pure function: no locks, no blocking, safe at DISPATCH_LEVEL. On failure
 * *Leds is left untouched so the caller keeps its previous state. */
NTSTATUS
TextDeckDecodeLedReport(
    _In_reads_bytes_opt_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_ ULONG* Leds
);

/* Validates one TEXTDECK_KEY_EVENT and converts it to a report.
 * Returns STATUS_INVALID_PARAMETER for anything the driver will not send. */
NTSTATUS
TextDeckBuildReport(
    _In_ const TEXTDECK_KEY_EVENT* Event,
    _Out_writes_bytes_(TEXTDECK_REPORT_SIZE) UCHAR* Report
);
