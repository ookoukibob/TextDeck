/*++
TextDeck - KMDF/VHF virtual HID keyboard driver
===============================================

Module: Driver.c - DriverEntry and the WDFDRIVER lifecycle.

Independently implemented for TextDeck from the published Microsoft interfaces
(VHF reference, KMDF programming guide, WDK IOCTL documentation). No third-party
driver source was copied, translated or mechanically refactored.

What this driver is
-------------------
A small root-enumerated KMDF function driver that exposes ONE virtual HID
keyboard to the operating system through the Virtual HID Framework. User mode
sends key events over a private IOCTL; VHF hands the resulting HID reports to
the OS, which binds its own keyboard stack to them. The driver contains no
typing policy, no text buffering and no timing logic - it is a transport.

Security posture
----------------
* The device object's security descriptor comes from the INF
  (D:P(A;;GA;;;SY)(A;;GA;;;BA)): SYSTEM and Administrators only. The driver
  does not widen it.
* Every IOCTL is validated before anything reaches VHF (see Queue.c).
* Unrecognised IOCTLs return STATUS_INVALID_DEVICE_REQUEST; they are not
  silently completed as success.
--*/

#include "Driver.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, TextDeckEvtDriverContextCleanup)
#endif

/* -------------------------------------------------------------------------
 * TextDeckEvtDriverContextCleanup
 *
 * Runs when the WDFDRIVER object is destroyed. The framework guarantees no
 * device objects remain at this point, so there is nothing device-scoped to
 * release here; it exists so the lifecycle is explicit and traceable.
 * ------------------------------------------------------------------------- */
VOID
TextDeckEvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(DriverObject);

    /* No driver-scoped allocations are held. */
}

/* -------------------------------------------------------------------------
 * DriverEntry
 *
 * The canonical KMDF entry point: initialise the driver config, register the
 * device-add and cleanup callbacks, and hand control to the framework.
 * ------------------------------------------------------------------------- */
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES driverAttributes;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, TextDeckEvtDeviceAdd);

    TextDeckDiag(10, STATUS_SUCCESS);   /* DriverEntry entered */

    /* The cleanup callback is an OBJECT attribute of the WDFDRIVER, not a
     * member of WDF_DRIVER_CONFIG (WDF_DRIVER_CONFIG only carries the device
     * add callback and the driver flags). */
    WDF_OBJECT_ATTRIBUTES_INIT(&driverAttributes);
    driverAttributes.EvtCleanupCallback = TextDeckEvtDriverContextCleanup;

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &driverAttributes,
                             &config,
                             WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        TextDeckDiag(12, status);   /* WdfDriverCreate failed */
        return status;
    }

    TextDeckDiag(19, STATUS_SUCCESS);   /* DriverEntry completed */
    return STATUS_SUCCESS;
}
