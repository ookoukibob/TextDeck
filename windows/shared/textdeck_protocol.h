/*++
TextDeck - shared driver/client ABI
===================================

This header is the SINGLE source of truth for the kernel driver <-> user mode
contract. It is included by the KMDF driver and by the Win32 client, so the two
sides can never drift apart.

Provenance
----------
Written from scratch for TextDeck against the published Microsoft interfaces:

  * Virtual HID Framework (VHF)
    https://learn.microsoft.com/windows-hardware/drivers/hid/virtual-hid-framework--vhf-
  * Defining I/O control codes (CTL_CODE, METHOD_BUFFERED, FILE_ANY_ACCESS)
    https://learn.microsoft.com/windows-hardware/drivers/kernel/defining-i-o-control-codes
  * USB HID Usage Tables 1.12, section 10 (Keyboard/Keypad)

No third-party driver source was consulted or copied.

Design notes
------------
* One IOCTL carries one complete key event: an optional modifier bitmap plus a
  HID keyboard usage id, and an explicit down/up action. Sending "down" and
  "up" as separate requests (rather than a press/release pair inside one call)
  lets the caller decide the inter-key timing, which is what makes the output
  look like a real typist and lets a stop request interrupt between keys.
* Every request is validated before it reaches the VHF queue: unknown actions,
  out-of-range usages and modifier bits outside the defined mask are rejected
  with STATUS_INVALID_PARAMETER rather than silently ignored.
* The device object's security descriptor (see the INF) grants access to
  SYSTEM and Administrators only. Nothing here widens that.
--*/

#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

/* -------------------------------------------------------------------------
 * Device interface
 *
 * The driver registers GUID_DEVINTERFACE_TEXTDECK_HID and also creates a
 * symbolic link so a client can open the device with a plain CreateFile path.
 * ------------------------------------------------------------------------- */
#define TEXTDECK_DEVICE_SYMBOLIC_LINK   L"\\DosDevices\\TextDeckHidDevice"

/* Kernel object name. WdfDeviceCreateSymbolicLink requires the device object to
 * be named in the \Device namespace, otherwise the call fails with
 * STATUS_INVALID_DEVICE_REQUEST. Assigned in EvtDeviceAdd before WdfDeviceCreate. */
#define TEXTDECK_DEVICE_KERNEL_NAME     L"\\Device\\TextDeckHidDevice"

/* User-mode open path: \\.\TextDeckHidDevice */
#define TEXTDECK_DEVICE_WIN32_PATH      L"\\\\.\\TextDeckHidDevice"

/* {6f1c4a2e-9b73-4c8d-8f10-2d5b7a9e3c41} */
#define GUID_DEVINTERFACE_TEXTDECK_HID \
    { 0x6f1c4a2e, 0x9b73, 0x4c8d, { 0x8f, 0x10, 0x2d, 0x5b, 0x7a, 0x9e, 0x3c, 0x41 } }

/* -------------------------------------------------------------------------
 * Protocol version
 *
 * Bumped whenever the structures or their semantics change. The client sends
 * its expected version in TEXTDECK_GET_INFO and refuses to type if the driver
 * reports a different major version, so a stale driver can never be driven
 * with a mismatched struct layout.
 * ------------------------------------------------------------------------- */
#define TEXTDECK_PROTOCOL_MAJOR   1
#define TEXTDECK_PROTOCOL_MINOR   1

/* -------------------------------------------------------------------------
 * Keyboard report layout (8 bytes, boot-keyboard compatible)
 *
 *   byte 0 : modifier bitmap
 *   byte 1 : reserved (0)
 *   byte 2..7 : six key slots, 0 = unused
 *
 * TWO DIFFERENT NUMBERS, deliberately kept apart:
 *
 *   TEXTDECK_REPORT_KEY_SLOTS  (6)  how many key slots the DESCRIPTOR declares.
 *                                   This is wire capacity: the descriptor is a
 *                                   standard boot-keyboard shape and carries six
 *                                   slots. It is not a promise that six keys can
 *                                   be held at once.
 *
 *   TEXTDECK_MAX_CONCURRENT_KEYS (1) how many non-modifier keys this DRIVER
 *                                   actually keeps down simultaneously.
 *                                   TextDeckBuildReport writes exactly one slot
 *                                   (slot 0) and leaves the rest zero, so one is
 *                                   the real capability.
 *
 * TEXTDECK_DEVICE_INFO.MaxConcurrentKeys reports the SECOND number, because that
 * is what the field name promises a client. Until protocol 1.1 it reported the
 * first, which overstated the driver by 6x; no client read it, so nothing broke,
 * but a client that had trusted it would have silently lost keys. The distinction
 * is asserted by the contract and logic tests so the two cannot drift apart again.
 *
 * Modifiers are NOT limited by this number: they live in byte 0 and any
 * combination can be held at once.
 * ------------------------------------------------------------------------- */
#define TEXTDECK_REPORT_SIZE          8
#define TEXTDECK_REPORT_KEY_SLOTS     6   /* descriptor capacity (byte 2..7) */
#define TEXTDECK_MAX_CONCURRENT_KEYS  1   /* actual simultaneous non-modifier keys */

/* Backwards-compatible alias for the descriptor capacity. Kept because the name
 * appears in existing comments and tools; it has never meant "concurrency". */
#define TEXTDECK_MAX_KEYS_PER_REPORT  TEXTDECK_REPORT_KEY_SLOTS

/* HID modifier bitmap bits (USB HID Usage Tables 1.12, 10: Keyboard/Keypad) */
#define TEXTDECK_MOD_LEFT_CTRL        0x01
#define TEXTDECK_MOD_LEFT_SHIFT       0x02
#define TEXTDECK_MOD_LEFT_ALT         0x04
#define TEXTDECK_MOD_LEFT_GUI         0x08
#define TEXTDECK_MOD_RIGHT_CTRL       0x10
#define TEXTDECK_MOD_RIGHT_SHIFT      0x20
#define TEXTDECK_MOD_RIGHT_ALT        0x40
#define TEXTDECK_MOD_RIGHT_GUI        0x80
#define TEXTDECK_MOD_VALID_MASK       0xFF

/* Usage id bounds the driver accepts. 0x04..0x65 covers the alphanumeric,
 * punctuation, editing and keypad ranges of the HID keyboard page; anything
 * outside it (consumer keys, vendor pages) is not representable by this
 * descriptor and is rejected. */
#define TEXTDECK_USAGE_MIN            0x04
#define TEXTDECK_USAGE_MAX            0x65

/* -------------------------------------------------------------------------
 * LED output report (Caps Lock / Num Lock / Scroll Lock / Compose / Kana)
 *
 * The report descriptor declares ONE output report byte holding five 1-bit
 * LED usages (Usage Page 0x08, usages 0x01..0x05) followed by three constant
 * padding bits (HID 1.11). The OS writes this report whenever the LED state
 * changes; the driver records it and the client may query it.
 * ------------------------------------------------------------------------- */
#define TEXTDECK_LED_REPORT_SIZE     1
#define TEXTDECK_LED_NUM_LOCK        0x01
#define TEXTDECK_LED_CAPS_LOCK       0x02
#define TEXTDECK_LED_SCROLL_LOCK     0x04
#define TEXTDECK_LED_COMPOSE         0x08
#define TEXTDECK_LED_KANA            0x10
#define TEXTDECK_LED_VALID_MASK      0x1F

/* -------------------------------------------------------------------------
 * TEXTDECK_KEY_EVENT - one keyboard state transition
 *
 * SEMANTICS (protocol 1.1; see docs/PROTOCOL.md)
 * ----------------------------------------------
 * `Modifiers` is NOT "the modifiers attached to this event". It is the COMPLETE
 * set of modifiers that must be held down in the report this event produces.
 * Every accepted event yields exactly one HID input report whose modifier byte
 * is `Modifiers` and whose key slots are derived from `Action`.
 *
 * That single rule is what makes chords expressible. Under protocol 1.0 `Up`
 * always emitted an all-zero report, so releasing the letter of Ctrl+A also
 * released Ctrl, and a modifier could not be held at all (a `Down` with
 * Usage=0 was rejected). Both are fixed here without changing the struct:
 *
 *   1. Modifiers alone down     Action=Modifiers, Modifiers=0x01, Usage=0
 *   2. Ctrl held, A pressed     Action=Down,      Modifiers=0x01, Usage=0x04
 *   3. A released, Ctrl held    Action=Up,        Modifiers=0x01, Usage=0x04
 *   4. Ctrl released            Action=Modifiers, Modifiers=0x00, Usage=0
 *
 * COMPATIBILITY
 * -------------
 * The struct layout, its size (4 bytes) and actions 0..2 are unchanged, so a
 * protocol-1.0 client keeps working: it sends Modifiers=0 on Up, which still
 * produces the all-zero report it always got. Only clients that WANT to hold a
 * modifier start filling `Modifiers` on Up, or use action 3.
 * ------------------------------------------------------------------------- */
typedef enum _TEXTDECK_KEY_ACTION {
    TextDeckKeyActionDown = 0,      /* press Usage while holding Modifiers */
    TextDeckKeyActionUp   = 1,      /* release Usage, keep Modifiers held */
    TextDeckKeyActionReleaseAll = 2,/* release every key and modifier (all-zero) */
    TextDeckKeyActionModifiers = 3  /* hold exactly Modifiers, no key pressed */
} TEXTDECK_KEY_ACTION;

#pragma pack(push, 1)
typedef struct _TEXTDECK_KEY_EVENT {
    UCHAR  Modifiers;   /* TEXTDECK_MOD_* bitmap: the COMPLETE held set */
    UCHAR  Usage;       /* HID usage for Down/Up; MUST be 0 for Modifiers/ReleaseAll */
    UCHAR  Action;      /* TEXTDECK_KEY_ACTION */
    UCHAR  Reserved;    /* must be 0 */
} TEXTDECK_KEY_EVENT;
#pragma pack(pop)

/* -------------------------------------------------------------------------
 * TEXTDECK_DEVICE_INFO - returned by TEXTDECK_GET_INFO
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct _TEXTDECK_DEVICE_INFO {
    USHORT ProtocolMajor;
    USHORT ProtocolMinor;
    UCHAR  ReportDescriptorVersion;
    /* How many NON-MODIFIER keys this driver keeps down at once. This is the
     * driver's real capability (TEXTDECK_MAX_CONCURRENT_KEYS = 1), NOT the
     * descriptor's six-slot capacity. See the report-layout note above. */
    UCHAR  MaxConcurrentKeys;
    UCHAR  Reserved[2];
} TEXTDECK_DEVICE_INFO;
#pragma pack(pop)

/* -------------------------------------------------------------------------
 * TEXTDECK_LED_STATE - returned by TEXTDECK_GET_LED_STATE
 * ------------------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct _TEXTDECK_LED_STATE {
    UCHAR Leds;       /* TEXTDECK_LED_* bitmap as last written by the OS */
    UCHAR Reserved[3];
} TEXTDECK_LED_STATE;
#pragma pack(pop)

/* -------------------------------------------------------------------------
 * IOCTL codes
 *
 * FILE_DEVICE_UNKNOWN with a vendor function range keeps these clear of any
 * Microsoft-defined device type. All are METHOD_BUFFERED + FILE_ANY_ACCESS:
 * the INF's device ACL is what restricts who may open the handle.
 *
 *   0x800  TEXTDECK_GET_INFO       - read device/protocol identity
 *   0x801  TEXTDECK_SUBMIT_KEY     - one TEXTDECK_KEY_EVENT
 *   0x802  TEXTDECK_RELEASE_ALL    - release every key (no input buffer)
 *   0x803  TEXTDECK_GET_LED_STATE  - out: TEXTDECK_LED_STATE
 * ------------------------------------------------------------------------- */
#define TEXTDECK_DEVICE_TYPE  FILE_DEVICE_UNKNOWN

#define IOCTL_TEXTDECK_GET_INFO \
    CTL_CODE(TEXTDECK_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TEXTDECK_SUBMIT_KEY \
    CTL_CODE(TEXTDECK_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TEXTDECK_RELEASE_ALL \
    CTL_CODE(TEXTDECK_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_TEXTDECK_GET_LED_STATE \
    CTL_CODE(TEXTDECK_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* -------------------------------------------------------------------------
 * Driver-reported NTSTATUS values the client maps to user-facing text.
 * Listed here so the client does not have to include ntddk.h.
 * ------------------------------------------------------------------------- */
#define TEXTDECK_STATUS_OK                  ((LONG)0x00000000L)
#define TEXTDECK_STATUS_INVALID_PARAMETER   ((LONG)0x8000000DL)  /* STATUS_INVALID_PARAMETER */
#define TEXTDECK_STATUS_INVALID_DEVICE_REQUEST ((LONG)0x8000020FL) /* STATUS_INVALID_DEVICE_REQUEST */
#define TEXTDECK_STATUS_BUFFER_TOO_SMALL    ((LONG)0x80000023L)  /* STATUS_BUFFER_TOO_SMALL */
#define TEXTDECK_STATUS_DEVICE_BUSY         ((LONG)0x80000021L)  /* STATUS_DEVICE_BUSY */
#define TEXTDECK_STATUS_DEVICE_NOT_READY    ((LONG)0xC00000A3L)  /* STATUS_DEVICE_NOT_READY */
#define TEXTDECK_STATUS_INSUFFICIENT_RESOURCES ((LONG)0xC000009AL)
#define TEXTDECK_STATUS_DEVICE_REMOVED      ((LONG)0xC00002B6L)
