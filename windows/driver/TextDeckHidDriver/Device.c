/*++
TextDeck - Module: Device.c - device creation, VHF lifetime, report submission.

Independently implemented for TextDeck from the published Microsoft interfaces
(VHF reference, KMDF programming guide, USB HID Usage Tables). No third-party
driver source was copied.

Report path
-----------
  IOCTL (Queue.c)  ->  TextDeckBuildReport  ->  TextDeckSubmitReport  ->  VHF

Buffering model: VHF default
---------------------------
This driver deliberately does NOT register EvtVhfReadyForNextReadReport. The
documented consequence (VhfReadReportSubmit, Remarks):

    "If the HID source driver does not implement the
     EvtVhfReadyForNextReadReport callback, then there are no restrictions on
     calling this method. VHF uses the default buffering policy. The driver can
     reuse the transfer buffer after the call returns."

The alternative, registered-callback mode, is far stricter: submit exactly once
per callback, and keep the report buffer valid until the next callback fires.
The previous revision registered the callback AND called VhfReadReportSubmit in
a bounded retry loop, which satisfies neither rule, and passed a stack buffer
that went out of scope while VHF might still have been reading it. Because the
callback also treated its VhfClientContext argument as a VHFHANDLE, its
readiness signal could never work as intended either.

Default buffering removes all three problems: VHF copies the report before
VhfReadReportSubmit returns, so a stack buffer is safe, no readiness event is
needed, and submission carries no per-callback state machine.

Ordering
--------
"Default buffering" does not mean "unordered". Down/up pairs arrive as separate
IOCTLs from possibly several threads, and the KMDF default queue is parallel, so
without serialisation two submits could reach VHF in the opposite order and
leave a key latched. Every path that talks to VHF therefore runs under
SubmitMutex (a KMUTEX). A mutex rather than a spinlock because it is held across
VhfReadReportSubmit, which may block, and because the teardown path must wait.

Release-all
-----------
TextDeckReleaseAll submits an all-zero report, which releases every modifier and
every key slot at once. It is called on file-handle close and during device
cleanup, so an aborted run cannot leave a key held down.
--*/

#define INITGUID          /* one definition of the interface GUID, here */
#include "Driver.h"

/* Single definition of the device interface GUID (declared in Driver.h). */
DEFINE_GUID(GUID_DEVINTERFACE_TEXTDECK_HID_OBJ,
            0x6f1c4a2e, 0x9b73, 0x4c8d,
            0x8f, 0x10, 0x2d, 0x5b, 0x7a, 0x9e, 0x3c, 0x41);

/* -------------------------------------------------------------------------
 * TextDeckDiag -- see Driver.h.
 *
 * Deliberately minimal: one volatile-option key under HKLM\SOFTWARE, two
 * values (Stage, Status). Never throws work into a failing path and never
 * changes control flow; a failure to create/write the key is silently ignored.
 * ------------------------------------------------------------------------- */
VOID
TextDeckDiag(
    _In_ ULONG    Stage,
    _In_ NTSTATUS Status
    )
{
    HANDLE            key = NULL;
    UNICODE_STRING    keyName;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING    stageName;
    UNICODE_STRING    statusName;
    ULONG             stageValue = Stage;
    ULONG             statusValue = (ULONG)Status;

    RtlInitUnicodeString(&keyName, L"\\Registry\\Machine\\SOFTWARE\\TextDeckDiag");
    InitializeObjectAttributes(&oa,
                               &keyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    if (!NT_SUCCESS(ZwCreateKey(&key,
                                KEY_SET_VALUE | KEY_QUERY_VALUE,
                                &oa,
                                0,
                                NULL,
                                REG_OPTION_VOLATILE,
                                NULL)))
    {
        return;
    }

    RtlInitUnicodeString(&stageName,  L"Stage");
    RtlInitUnicodeString(&statusName, L"Status");

    (void)ZwSetValueKey(key, &stageName,  0, REG_DWORD, &stageValue,  sizeof(stageValue));
    (void)ZwSetValueKey(key, &statusName, 0, REG_DWORD, &statusValue, sizeof(statusValue));

    /* Keep a human-readable history of the last stage for post-mortem. */
    {
        UNICODE_STRING lastName;
        WCHAR          buf[96];
        RtlInitUnicodeString(&lastName, L"LastStage");
        (void)RtlStringCchPrintfW(buf,
                                  RTL_NUMBER_OF(buf),
                                  L"stage=%lu status=0x%08X",
                                  Stage,
                                  statusValue);
        (void)ZwSetValueKey(key,
                            &lastName,
                            0,
                            REG_SZ,
                            buf,
                            (ULONG)((wcslen(buf) + 1) * sizeof(WCHAR)));
    }

    ZwClose(key);
}

/* Records one extra named DWORD under the same volatile key. Used for values
 * that are evidence about a documented precondition (e.g. the IRQL VhfCreate
 * was called at), never for tuning a parameter at run time. */
VOID
TextDeckDiagValue(
    _In_ PCWSTR Name,
    _In_ ULONG  Value
    )
{
    HANDLE            key = NULL;
    UNICODE_STRING    keyName;
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING    valueName;
    ULONG             v = Value;

    if (Name == NULL) {
        return;
    }

    RtlInitUnicodeString(&keyName, L"\\Registry\\Machine\\SOFTWARE\\TextDeckDiag");
    InitializeObjectAttributes(&oa,
                               &keyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL,
                               NULL);

    if (!NT_SUCCESS(ZwCreateKey(&key,
                                KEY_SET_VALUE,
                                &oa,
                                0,
                                NULL,
                                REG_OPTION_VOLATILE,
                                NULL)))
    {
        return;
    }

    RtlInitUnicodeString(&valueName, Name);
    (void)ZwSetValueKey(key, &valueName, 0, REG_DWORD, &v, sizeof(v));
    ZwClose(key);
}

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, TextDeckEvtDeviceAdd)
#pragma alloc_text (PAGE, TextDeckEvtDeviceFileCreate)
#pragma alloc_text (PAGE, TextDeckEvtFileClose)
#pragma alloc_text (PAGE, TextDeckEvtDeviceCleanup)
#pragma alloc_text (PAGE, TextDeckSubmitReport)
#pragma alloc_text (PAGE, TextDeckReleaseAll)
#endif

/* -------------------------------------------------------------------------
 * TextDeckSubmitReport
 *
 * The only place that calls VhfReadReportSubmit. Caller-supplied reports are
 * copied into a local buffer first: the packet must describe memory that is
 * valid for the duration of the call and that a user-mode caller cannot mutate
 * underneath us (METHOD_BUFFERED already copies, but the copy here makes the
 * guarantee local and independent of the transport).
 * ------------------------------------------------------------------------- */
NTSTATUS
TextDeckSubmitReport(
    _In_ PTEXTDECK_DEVICE_CONTEXT Context,
    _In_reads_bytes_(TEXTDECK_REPORT_SIZE) const UCHAR* Report
)
{
    UCHAR            local[TEXTDECK_REPORT_SIZE];
    HID_XFER_PACKET  packet;
    LARGE_INTEGER    delay;
    NTSTATUS         status;
    int              attempt;

    PAGED_CODE();

    if (Context == NULL || Report == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(local, Report, sizeof(local));

    packet.reportBuffer    = local;
    packet.reportBufferLen = TEXTDECK_REPORT_SIZE;
    packet.reportId        = 0;

    delay.QuadPart = TEXTDECK_SUBMIT_RETRY_DELAY_100NS;

    /* Serialise against every other VHF user for the whole submit, so the
     * order in which reports reach VHF is the order in which they were
     * accepted here. Released on every exit path. */
    KeWaitForSingleObject(&Context->SubmitMutex,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);

    if (!Context->VhfStarted || Context->VhfHandle == NULL) {
        KeReleaseMutex(&Context->SubmitMutex, FALSE);
        return STATUS_DEVICE_NOT_READY;
    }

    status = STATUS_DEVICE_BUSY;
    for (attempt = 0; attempt < TEXTDECK_SUBMIT_MAX_ATTEMPTS; attempt++) {

        status = VhfReadReportSubmit(Context->VhfHandle, &packet);

        if (status != STATUS_DEVICE_BUSY && status != STATUS_DEVICE_NOT_READY) {
            /* Delivered, or a real error the caller must see. */
            break;
        }

        /* VHF's ring is momentarily full. Yield and retry rather than fail a
         * keystroke outright; the wait is bounded and the mutex is still held
         * so ordering is preserved. */
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

    KeReleaseMutex(&Context->SubmitMutex, FALSE);
    return status;
}

/* -------------------------------------------------------------------------
 * TextDeckReleaseAll
 *
 * Best-effort by design: teardown paths must not fail or block indefinitely.
 * The all-zero report is the HID-defined "no keys pressed" state, and it also
 * clears every modifier bit.
 * ------------------------------------------------------------------------- */
NTSTATUS
TextDeckReleaseAll(
    _In_ PTEXTDECK_DEVICE_CONTEXT Context
)
{
    UCHAR report[TEXTDECK_REPORT_SIZE];

    PAGED_CODE();

    if (Context == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(report, sizeof(report));
    return TextDeckSubmitReport(Context, report);
}

/* -------------------------------------------------------------------------
 * TextDeckBuildReport
 *
 * The single validation point. Anything this function rejects never reaches
 * VHF, so a malformed request cannot produce a stuck key.
 *
 * Layout (TEXTDECK_REPORT_SIZE = 8, boot-keyboard compatible):
 *   [0] modifier bitmap
 *   [1] reserved - MUST be 0 (the descriptor declares it Constant)
 *   [2..7] key slots, 0 = unused
 * ------------------------------------------------------------------------- */
NTSTATUS
TextDeckBuildReport(
    _In_ const TEXTDECK_KEY_EVENT* Event,
    _Out_writes_bytes_(TEXTDECK_REPORT_SIZE) UCHAR* Report
)
{
    if (Event == NULL || Report == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /* The reserved byte is part of the contract: a caller that fills it with
     * something is talking a different protocol, and guessing would mask the
     * mismatch. Reject before touching the output buffer. */
    if (Event->Reserved != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Report, TEXTDECK_REPORT_SIZE);

    switch (Event->Action) {

    case TextDeckKeyActionDown:
        /* Press `Usage` while holding the complete modifier set in Modifiers. */
        if (Event->Usage < TEXTDECK_USAGE_MIN || Event->Usage > TEXTDECK_USAGE_MAX) {
            return STATUS_INVALID_PARAMETER;
        }
        Report[0] = Event->Modifiers;
        Report[2] = Event->Usage;   /* first key slot */
        /* Report[1] stays 0: the reserved byte is never written. */
        return STATUS_SUCCESS;

    case TextDeckKeyActionUp:
        /* Release `Usage` but KEEP the modifiers in Modifiers held.
         *
         * Protocol 1.0 emitted an all-zero report here, which meant releasing
         * the letter of Ctrl+A also released Ctrl -- the modifier could never
         * survive a key release. Carrying Modifiers through is what makes a
         * held modifier expressible at all.
         *
         * A 1.0 client sends Modifiers=0, so it still gets the all-zero report
         * it always got: the change is additive, not a break. */
        if (Event->Usage != 0 &&
            (Event->Usage < TEXTDECK_USAGE_MIN || Event->Usage > TEXTDECK_USAGE_MAX)) {
            return STATUS_INVALID_PARAMETER;
        }
        Report[0] = Event->Modifiers;
        return STATUS_SUCCESS;

    case TextDeckKeyActionModifiers:
        /* Hold exactly `Modifiers` with no key pressed -- the report a real
         * keyboard sends when Ctrl/Alt/Shift/GUI goes down on its own.
         *
         * Usage must be 0: a non-zero usage here would mean the caller is
         * describing a key press, which is what Action=Down is for. Rejecting
         * it keeps one report shape per action instead of two spellings. */
        if (Event->Usage != 0) {
            return STATUS_INVALID_PARAMETER;
        }
        Report[0] = Event->Modifiers;
        return STATUS_SUCCESS;

    case TextDeckKeyActionReleaseAll:
        /* All-zero report; Usage and Modifiers are both ignored. */
        return STATUS_SUCCESS;

    default:
        return STATUS_INVALID_PARAMETER;
    }
}

/* -------------------------------------------------------------------------
 * TextDeckEvtVhfAsyncOperationWriteReport
 *
 * Called by VHF when the OS writes an output report, i.e. when the LED state
 * changes (Caps Lock, Num Lock, Scroll Lock, Compose, Kana).
 *
 * This driver is a transport: it does not act on LED state, but it MUST
 * complete the operation, otherwise the OS write never finishes and the
 * keyboard stack can stall. The state is recorded so a client can query it.
 *
 * Runs at <= DISPATCH_LEVEL: no paged code, no blocking, no allocation.
 * ------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------
 * TextDeckDecodeLedReport
 *
 * The descriptor declares exactly one output report byte (5 LED usages plus 3
 * padding bits) and no report ids, so a valid packet is a single byte. Any
 * other shape means the OS and this descriptor disagree; the previous LED state
 * is then kept rather than being overwritten with garbage.
 * ------------------------------------------------------------------------- */
NTSTATUS
TextDeckDecodeLedReport(
    _In_reads_bytes_opt_(Length) const UCHAR* Buffer,
    _In_ ULONG Length,
    _Out_ ULONG* Leds
)
{
    if (Buffer == NULL || Leds == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Length != TEXTDECK_LED_REPORT_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Only the five defined LED usages are meaningful; the padding bits are
     * declared Constant and must read as zero. Masking keeps a junk padding
     * bit from being reported as a real LED state. */
    *Leds = (ULONG)(Buffer[0] & TEXTDECK_LED_VALID_MASK);
    return STATUS_SUCCESS;
}

VOID
TextDeckEvtVhfAsyncOperationWriteReport(
    _In_     PVOID               VhfClientContext,
    _In_     VHFOPERATIONHANDLE  VhfOperationHandle,
    _In_opt_ PVOID               VhfOperationContext,
    _In_     PHID_XFER_PACKET    HidTransferPacket
)
{
    ULONG leds = 0;

    UNREFERENCED_PARAMETER(VhfOperationContext);

    if (VhfClientContext != NULL && HidTransferPacket != NULL) {
        PTEXTDECK_DEVICE_CONTEXT context = (PTEXTDECK_DEVICE_CONTEXT)VhfClientContext;

        /* reportBuffer excludes the report id; with a single output report and
         * no declared report ids the first byte is the LED bitmap. */
        if (NT_SUCCESS(TextDeckDecodeLedReport(HidTransferPacket->reportBuffer,
                                               HidTransferPacket->reportBufferLen,
                                               &leds))) {
            InterlockedExchange(&context->LedState, (LONG)leds);
        }
    }

    /* Always complete, even for a packet we did not understand: an
     * uncompleted output operation would wedge the caller. */
    (void)VhfAsyncOperationComplete(VhfOperationHandle, STATUS_SUCCESS);
}

/* -------------------------------------------------------------------------
 * TextDeckEvtDeviceFileCreate
 *
 * One handle per client is expected, but concurrent handles are harmless: the
 * submit path is serialised by SubmitMutex. Nothing is allocated per handle.
 * ------------------------------------------------------------------------- */
VOID
TextDeckEvtDeviceFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject
)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(FileObject);

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

/* -------------------------------------------------------------------------
 * TextDeckEvtFileClose
 *
 * A client disappearing must never leave a key held down. Release everything
 * before the handle goes away. Failure is not actionable here (the device may
 * already be tearing down), so the status is deliberately ignored.
 * ------------------------------------------------------------------------- */
VOID
TextDeckEvtFileClose(
    _In_ WDFFILEOBJECT FileObject
)
{
    WDFDEVICE device;
    PTEXTDECK_DEVICE_CONTEXT context;

    PAGED_CODE();

    device = WdfFileObjectGetDevice(FileObject);
    if (device == NULL) {
        return;
    }
    context = TextDeckGetDeviceContext(device);
    if (context == NULL) {
        return;
    }

    (void)TextDeckReleaseAll(context);
}

/* -------------------------------------------------------------------------
 * TextDeckEvtDeviceCleanup
 *
 * Runs when the WDFDEVICE is being destroyed (PASSIVE_LEVEL). Order matters:
 *   1. take the submit mutex so no in-flight submit is using VHF,
 *   2. mark VHF unusable so new submits fail fast instead of hanging,
 *   3. release every key while VHF is still alive,
 *   4. delete the VHF instance, waiting for outstanding callbacks,
 *   5. drop the mutex.
 *
 * VhfDelete(Wait = TRUE) is used deliberately: it guarantees the VHF callbacks
 * (including the output-report callback) have finished before the device
 * context they reference is torn down, which is what makes passing the context
 * to VhfCreate safe without a separate refcount.
 * ------------------------------------------------------------------------- */
VOID
TextDeckEvtDeviceCleanup(
    _In_ WDFOBJECT DeviceObject
)
{
    WDFDEVICE device = (WDFDEVICE)DeviceObject;
    PTEXTDECK_DEVICE_CONTEXT context;
    UCHAR zero[TEXTDECK_REPORT_SIZE];

    PAGED_CODE();

    context = TextDeckGetDeviceContext(device);
    if (context == NULL) {
        return;
    }

    KeWaitForSingleObject(&context->SubmitMutex,
                          Executive,
                          KernelMode,
                          FALSE,
                          NULL);

    /* New submits must fail fast from here on. */
    InterlockedExchange(&context->VhfStarted, FALSE);

    if (context->VhfHandle != NULL) {
        /* Best effort: never leave a key held down on the way out. Submitted
         * directly rather than through TextDeckReleaseAll because the mutex is
         * already held (this function is the lock owner). */
        HID_XFER_PACKET packet;
        RtlZeroMemory(zero, sizeof(zero));
        packet.reportBuffer    = zero;
        packet.reportBufferLen = TEXTDECK_REPORT_SIZE;
        packet.reportId        = 0;
        (void)VhfReadReportSubmit(context->VhfHandle, &packet);

        /* Waits for outstanding VHF callbacks to drain. */
        VhfDelete(context->VhfHandle, TRUE);
        context->VhfHandle = NULL;
    }

    KeReleaseMutex(&context->SubmitMutex, FALSE);
}

/* -------------------------------------------------------------------------
 * TextDeckEvtDeviceAdd
 *
 * Creates the device object, registers the device interface and the Win32
 * symbolic link, then brings VHF up.
 * ------------------------------------------------------------------------- */
NTSTATUS
TextDeckEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE device;
    PTEXTDECK_DEVICE_CONTEXT context;
    UNICODE_STRING symbolicLink;
    VHF_CONFIG vhfConfig;
    NTSTATUS status;

    PAGED_CODE();

    TextDeckDiag(100, STATUS_SUCCESS);   /* EvtDeviceAdd entered */

    UNREFERENCED_PARAMETER(Driver);

    /* --- file-object callbacks so a vanished client releases its keys --- */
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig,
                               TextDeckEvtDeviceFileCreate,
                               TextDeckEvtFileClose,
                               WDF_NO_EVENT_CALLBACK);
    WDF_OBJECT_ATTRIBUTES_INIT(&fileAttributes);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &fileAttributes);

    /* Leave the device type at the KMDF default (FILE_DEVICE_UNKNOWN).
     * FILE_DEVICE_KEYBOARD is a protected type that VHF rejects with
     * STATUS_INVALID_DEVICE_REQUEST from VhfCreate. */

    /* Name the device object in the \Device namespace. WdfDeviceCreateSymbolicLink
     * below resolves its target through that name; without it the call fails
     * with STATUS_INVALID_DEVICE_REQUEST (this was the Code 31 cause). */
    {
        DECLARE_CONST_UNICODE_STRING(deviceName, TEXTDECK_DEVICE_KERNEL_NAME);
        status = WdfDeviceInitAssignName(DeviceInit, &deviceName);
        if (!NT_SUCCESS(status)) {
            TextDeckDiag(120, status);
            return status;
        }
    }

    /* --- device object --- */
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes,
                                            TEXTDECK_DEVICE_CONTEXT);
    /* VHF must be torn down when the device object goes away, otherwise the
     * virtual keyboard stays registered and a reinstall sees a stale HID node. */
    deviceAttributes.EvtCleanupCallback = TextDeckEvtDeviceCleanup;

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        TextDeckDiag(130, status);
        return status;
    }

    context = TextDeckGetDeviceContext(device);
    RtlZeroMemory(context, sizeof(*context));
    KeInitializeMutex(&context->SubmitMutex, 0);
    InterlockedExchange(&context->LedState, 0L);
    InterlockedExchange(&context->VhfStarted, FALSE);
    context->VhfHandle = NULL;

    /* --- user-mode reachability --- */
    status = WdfDeviceCreateDeviceInterface(device,
                                            &GUID_DEVINTERFACE_TEXTDECK_HID_OBJ,
                                            NULL);
    if (!NT_SUCCESS(status)) {
        TextDeckDiag(140, status);
        return status;
    }

    RtlInitUnicodeString(&symbolicLink, TEXTDECK_DEVICE_SYMBOLIC_LINK);
    status = WdfDeviceCreateSymbolicLink(device, &symbolicLink);
    if (!NT_SUCCESS(status)) {
        /* The device interface above is the documented open path; the symbolic
         * link is a convenience alias. Failing the whole device because the alias
         * could not be created would take the working interface down with it, so
         * this is logged and tolerated instead of returned. */
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL,
                   "TextDeck: WdfDeviceCreateSymbolicLink failed 0x%08X (interface still usable)\n",
                   status);
    }

    /* --- VHF --- */
    VHF_CONFIG_INIT(&vhfConfig,
                    WdfDeviceWdmGetDeviceObject(device),
                    TEXTDECK_REPORT_DESCRIPTOR_SIZE,
                    (PUCHAR)g_TextDeckReportDescriptor);

    /* The device context travels to every VHF callback as VhfClientContext.
     * This is the ONLY legitimate way a VHF callback receives driver state:
     * the callbacks are documented to take VhfClientContext, not the VHFHANDLE
     * returned by VhfCreate. VhfDelete(Wait=TRUE) in cleanup guarantees VHF is
     * finished with this pointer before the context is freed. */
    vhfConfig.VhfClientContext = context;

    /* No EvtVhfReadyForNextReadReport: default buffering policy (see the file
     * header). Output reports (LEDs) ARE handled. */
    vhfConfig.EvtVhfAsyncOperationWriteReport = TextDeckEvtVhfAsyncOperationWriteReport;

    /* pid.codes 1209:0001 is reserved for private testing only, is not globally
     * unique, and must not be used on a redistributed/sold/manufactured device.
     * This virtual identity is for controlled development/testing only; choose
     * an appropriate production identity before any production distribution.
     * https://pid.codes/1209/0001/ */
    vhfConfig.VendorID      = 0x1209;
    vhfConfig.ProductID     = 0x0001;
    vhfConfig.VersionNumber = 0x0100;

    /* ContainerID groups the virtual device with its children. A fixed GUID is
     * used so repeated installs map to the same container. This is device
     * identity, not a workaround: nothing here is claimed to affect whether
     * VhfCreate succeeds. */
    {
        static const GUID textDeckContainer =
            { 0x7c9e1a44, 0x2b6f, 0x4d18, { 0x9a, 0x3c, 0x51, 0x8e, 0x2f, 0x60, 0xb4, 0xd7 } };
        vhfConfig.ContainerID = textDeckContainer;
    }

    /* A per-instance identifier keeps two virtual keyboards distinguishable. */
    {
        static const WCHAR instanceId[] = L"TEXTDECK-HID-0001";
        vhfConfig.InstanceIDLength = (USHORT)(sizeof(instanceId) - sizeof(WCHAR));
        vhfConfig.InstanceID = (PWSTR)instanceId;
    }

    /* HardwareIDs let the HID class installer pick its own driver. The list is
     * REG_MULTI_SZ-shaped: entries separated by NUL, terminated by an extra NUL,
     * so the byte count includes the terminator. */
    {
        static const WCHAR hwIds[] = L"HID\\VID_1209&PID_0001&REV_0100";
        vhfConfig.HardwareIDsLength = (USHORT)sizeof(hwIds);
        vhfConfig.HardwareIDs = (PWSTR)hwIds;
    }

    /* VhfCreate is documented as PASSIVE_LEVEL only, and this is the only place
     * the driver calls it. EvtDeviceAdd runs at PASSIVE_LEVEL, so the assertion
     * below states the precondition the contract asks for rather than hoping. */
    ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    TextDeckDiagValue(L"VhfCreateIrql", (ULONG)KeGetCurrentIrql());

    /* The VHF_CONFIG handed to VHF is exactly what VHF_CONFIG_INIT produced,
     * plus the documented fields set above. VHF_CONFIG.Size is never adjusted:
     * the struct's own size is the only value the framework is promised, and a
     * "try a different Size until it works" sweep turns a documented contract
     * into a guess. The real return status is recorded so a failure is evidence,
     * not a hypothesis. */
    status = VhfCreate(&vhfConfig, &context->VhfHandle);
    TextDeckDiagValue(L"VhfCreateStatus", (ULONG)status);
    TextDeckDiag(160, status);   /* the real VhfCreate NTSTATUS */
    if (!NT_SUCCESS(status)) {
        context->VhfHandle = NULL;
        return status;
    }

    status = VhfStart(context->VhfHandle);
    if (!NT_SUCCESS(status)) {
        VhfDelete(context->VhfHandle, TRUE);
        context->VhfHandle = NULL;
        TextDeckDiag(170, status);
        return status;
    }
    InterlockedExchange(&context->VhfStarted, TRUE);

    /* --- default queue: parallel dispatch so a slow client cannot stall others.
     * Ordering is guaranteed by SubmitMutex in the submit path, not by
     * serialising the queue (which would let one stuck client block all). --- */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                           WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = TextDeckEvtIoDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&context->VhfStarted, FALSE);
        VhfDelete(context->VhfHandle, TRUE);
        context->VhfHandle = NULL;
        TextDeckDiag(180, status);
        return status;
    }

    TextDeckDiag(190, STATUS_SUCCESS);
    return STATUS_SUCCESS;
}
