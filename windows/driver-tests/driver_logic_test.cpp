/*++
TextDeck - driver logic test (host-side, no driver install required)
====================================================================

Compiles the driver's PURE logic (TextDeckBuildReport) for the host and asserts
its behaviour. This is not a substitute for loading the driver, but it does
exercise the real validation code rather than grepping for it.

What is covered:
  * key-down produces the documented report layout (modifier byte + slot 0)
  * key-up and release-all produce the all-zero report
  * key-down followed by key-up and by release-all leaves nothing asserted
  * out-of-range usages are rejected (both ends of the range)
  * an unknown action is rejected
  * a non-zero Reserved byte is rejected (the descriptor declares it Constant)
  * a null buffer is rejected
  * the report buffer is fully zeroed before use (no stale bytes leak out)
  * every value the driver accepts maps to a report the HID descriptor can carry
  * LED output reports are decoded, range-checked and masked

This is host-side logic coverage only. It does NOT exercise VHF itself, KMDF
queue ordering, IRQL behaviour, or the device lifecycle - those need a loaded
driver and are outside the scope of this host-side test.

The file includes the driver's Device.c directly with the kernel types stubbed
out, so the function under test is the shipping implementation, not a copy.
--*/

// The driver's validation function is pure C with no WDF/VHF calls, so it can be
// compiled on the host. Windows.h supplies the real UCHAR/USHORT/ULONG/NTSTATUS
// types, so nothing is re-typedef'd here (redefining them collides with
// wtypesbase.h / winsmcrd.h).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>
#include <string.h>

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS              ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER    ((NTSTATUS)0x8000000DL)
#endif

// The driver calls RtlZeroMemory; on the host it is plain memset.
#ifndef RtlZeroMemory
#define RtlZeroMemory(dst, n) memset((dst), 0, (n))
#endif

// NTSTATUS and the STATUS_* codes live in the kernel/DDK headers, which are not
// available (and not wanted) for a host build. windows.h does not define
// NTSTATUS, so declare exactly the pieces the extracted function touches.
#ifndef _NTDEF_
typedef LONG NTSTATUS;
#endif
typedef NTSTATUS* PNTSTATUS;

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS              ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER    ((NTSTATUS)0x8000000DL)
#endif

// Pull in the ABI only (no WDF headers): the constants and the event struct.
#include "../shared/textdeck_protocol.h"

// The function under test is NOT re-implemented here. build_logic_test.ps1
// extracts TextDeckBuildReport() verbatim from the driver's Device.c into
// driver_logic_extracted.inc, and that extracted code is what compiles below.
// If the driver's validation changes, this test changes with it.

static int g_checks = 0;
static int g_failures = 0;

static void check(const char* name, bool ok, const char* detail = "")
{
    ++g_checks;
    printf("  [%s] %s :: %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) ++g_failures;
}

static bool report_is_all_zero(const UCHAR* r)
{
    for (int i = 0; i < TEXTDECK_REPORT_SIZE; ++i) {
        if (r[i] != 0) return false;
    }
    return true;
}


#include "driver_logic_extracted.inc"

int main(void)
{
    UCHAR report[TEXTDECK_REPORT_SIZE];
    NTSTATUS st;

    printf("TextDeck driver logic test (host-side)\n");
    printf("=======================================================\n");

    // --- 1. key down: modifier byte + first key slot ---------------------
    {
        TEXTDECK_KEY_EVENT ev = {};
        ev.Modifiers = TEXTDECK_MOD_LEFT_SHIFT;
        ev.Usage     = 0x04;            // 'a'
        ev.Action    = TextDeckKeyActionDown;
        memset(report, 0xAA, sizeof(report));   // poison
        st = TextDeckBuildReport(&ev, report);
        check("down: returns success", st == STATUS_SUCCESS, "STATUS_SUCCESS");
        check("down: modifier byte carries Shift",
              report[0] == TEXTDECK_MOD_LEFT_SHIFT, "byte0 == MOD_LEFT_SHIFT");
        check("down: reserved byte is zero", report[1] == 0, "byte1 == 0");
        check("down: usage lands in slot 0", report[2] == 0x04, "byte2 == 0x04");
        bool restZero = true;
        for (int i = 3; i < TEXTDECK_REPORT_SIZE; ++i) if (report[i] != 0) restZero = false;
        check("down: remaining slots cleared (poison overwritten)",
              restZero, "bytes 3..7 == 0");
    }

    // --- 2. key up: releases the key, KEEPS the held modifiers -----------
    // Protocol 1.0 emitted an all-zero report here, which is exactly the defect
    // this round fixes: releasing the letter of Ctrl+A also released Ctrl. The
    // expected report now carries the caller's modifier set.
    {
        TEXTDECK_KEY_EVENT ev = {};
        ev.Usage  = 0x04;
        ev.Action = TextDeckKeyActionUp;
        memset(report, 0xAA, sizeof(report));
        st = TextDeckBuildReport(&ev, report);
        check("up: returns success", st == STATUS_SUCCESS, "STATUS_SUCCESS");
        check("up: with no modifiers still releases everything (all-zero)",
              report_is_all_zero(report), "all 8 bytes zero");

        // Holding Ctrl across the release is the whole point of 1.1.
        ev.Modifiers = TEXTDECK_MOD_LEFT_CTRL;
        memset(report, 0xAA, sizeof(report));
        st = TextDeckBuildReport(&ev, report);
        check("up: keeps Ctrl held", st == STATUS_SUCCESS && report[0] == TEXTDECK_MOD_LEFT_CTRL,
              "byte0 == MOD_LEFT_CTRL");
        bool slotsClear = true;
        for (int i = 1; i < TEXTDECK_REPORT_SIZE; ++i) if (report[i] != 0) slotsClear = false;
        check("up: clears every key slot while keeping the modifier",
              slotsClear, "bytes 1..7 == 0");
    }

    // --- 2b. modifiers-only action (protocol 1.1) ------------------------
    {
        TEXTDECK_KEY_EVENT ev = {};
        ev.Action    = TextDeckKeyActionModifiers;
        ev.Modifiers = TEXTDECK_MOD_LEFT_CTRL;
        ev.Usage     = 0;
        memset(report, 0xAA, sizeof(report));
        st = TextDeckBuildReport(&ev, report);
        check("modifiers-only: accepted", st == STATUS_SUCCESS, "STATUS_SUCCESS");
        check("modifiers-only: modifier byte set", report[0] == TEXTDECK_MOD_LEFT_CTRL,
              "byte0 == MOD_LEFT_CTRL");
        bool noKeys = true;
        for (int i = 1; i < TEXTDECK_REPORT_SIZE; ++i) if (report[i] != 0) noKeys = false;
        check("modifiers-only: no key slot set", noKeys, "bytes 1..7 == 0");

        // Usage must be 0: a non-zero usage here would be a second spelling of
        // Action=Down, and two spellings of one report shape is how protocols rot.
        ev.Usage = 0x04;
        check("modifiers-only: non-zero usage rejected",
              TextDeckBuildReport(&ev, report) == STATUS_INVALID_PARAMETER,
              "STATUS_INVALID_PARAMETER");

        // All-zero modifier set is the "release the modifiers" transition.
        ev.Usage = 0;
        ev.Modifiers = 0;
        st = TextDeckBuildReport(&ev, report);
        check("modifiers-only: empty set yields the all-zero report",
              st == STATUS_SUCCESS && report_is_all_zero(report), "all 8 bytes zero");
    }

    // --- 2c. the full Ctrl+A sequence, report by report ------------------
    // This is the behaviour the previous round recorded as FAIL. It is asserted
    // here at the report level so a regression is caught without a VM.
    {
        struct Step { UCHAR mods; UCHAR usage; UCHAR action; UCHAR expectMods; UCHAR expectKey; };
        const Step seq[] = {
            { TEXTDECK_MOD_LEFT_CTRL, 0x00, TextDeckKeyActionModifiers, TEXTDECK_MOD_LEFT_CTRL, 0x00 }, // Ctrl down
            { TEXTDECK_MOD_LEFT_CTRL, 0x04, TextDeckKeyActionDown,      TEXTDECK_MOD_LEFT_CTRL, 0x04 }, // A down
            { TEXTDECK_MOD_LEFT_CTRL, 0x04, TextDeckKeyActionUp,        TEXTDECK_MOD_LEFT_CTRL, 0x00 }, // A up, Ctrl held
            { 0x00,                   0x00, TextDeckKeyActionModifiers, 0x00,                   0x00 }, // Ctrl up
        };
        bool allOk = true;
        char detail[128] = {0};
        for (int i = 0; i < 4; ++i) {
            TEXTDECK_KEY_EVENT ev = {};
            ev.Modifiers = seq[i].mods;
            ev.Usage     = seq[i].usage;
            ev.Action    = seq[i].action;
            memset(report, 0xAA, sizeof(report));
            if (TextDeckBuildReport(&ev, report) != STATUS_SUCCESS) { allOk = false; continue; }
            if (report[0] != seq[i].expectMods) allOk = false;
            if (report[2] != seq[i].expectKey)  allOk = false;
        }
        sprintf_s(detail, sizeof(detail), "4 reports: Ctrl/ Ctrl+A / A-up / Ctrl-up");
        check("Ctrl+A sequence produces the documented report at every step",
              allOk, detail);
    }

    // --- 3. release all ---------------------------------------------------
    {
        TEXTDECK_KEY_EVENT ev = {};
        ev.Action = TextDeckKeyActionReleaseAll;
        memset(report, 0xAA, sizeof(report));
        st = TextDeckBuildReport(&ev, report);
        check("release-all: returns success", st == STATUS_SUCCESS, "STATUS_SUCCESS");
        check("release-all: all-zero report",
              report_is_all_zero(report), "all 8 bytes zero");
    }

    // --- 4. usage range boundaries ---------------------------------------
    {
        TEXTDECK_KEY_EVENT ev = {};
        ev.Action = TextDeckKeyActionDown;

        ev.Usage = TEXTDECK_USAGE_MIN - 1;
        check("down: usage below range rejected",
              TextDeckBuildReport(&ev, report) == STATUS_INVALID_PARAMETER,
              "STATUS_INVALID_PARAMETER");

        ev.Usage = TEXTDECK_USAGE_MAX + 1;
        check("down: usage above range rejected",
              TextDeckBuildReport(&ev, report) == STATUS_INVALID_PARAMETER,
              "STATUS_INVALID_PARAMETER");

        ev.Usage = 0;
        check("down: usage 0 rejected (no key to press)",
              TextDeckBuildReport(&ev, report) == STATUS_INVALID_PARAMETER,
              "STATUS_INVALID_PARAMETER");

        ev.Usage = TEXTDECK_USAGE_MIN;
        check("down: minimum usage accepted",
              TextDeckBuildReport(&ev, report) == STATUS_SUCCESS, "0x04");

        ev.Usage = TEXTDECK_USAGE_MAX;
        check("down: maximum usage accepted",
              TextDeckBuildReport(&ev, report) == STATUS_SUCCESS, "0x65");
    }

    // --- 5. unknown action ------------------------------------------------
    {
        TEXTDECK_KEY_EVENT ev = {};
        ev.Action = 99;
        check("unknown action rejected",
              TextDeckBuildReport(&ev, report) == STATUS_INVALID_PARAMETER,
              "STATUS_INVALID_PARAMETER");
    }

    // --- 6. null buffers --------------------------------------------------
    {
        TEXTDECK_KEY_EVENT ev = {};
        ev.Action = TextDeckKeyActionReleaseAll;
        check("null event rejected",
              TextDeckBuildReport(NULL, report) == STATUS_INVALID_PARAMETER,
              "STATUS_INVALID_PARAMETER");
        check("null report rejected",
              TextDeckBuildReport(&ev, NULL) == STATUS_INVALID_PARAMETER,
              "STATUS_INVALID_PARAMETER");
    }

    // --- 7. every modifier bit is accepted (full 8-bit bitmap) -----------
    {
        bool allOk = true;
        for (int bit = 0; bit < 8; ++bit) {
            TEXTDECK_KEY_EVENT ev = {};
            ev.Modifiers = (UCHAR)(1u << bit);
            ev.Usage     = 0x04;
            ev.Action    = TextDeckKeyActionDown;
            if (TextDeckBuildReport(&ev, report) != STATUS_SUCCESS) allOk = false;
            if (report[0] != (UCHAR)(1u << bit)) allOk = false;
        }
        check("all 8 modifier bits accepted and preserved", allOk, "bits 0..7");
    }

    // --- 7b. the advertised concurrency matches what is actually written --
    // GET_INFO.MaxConcurrentKeys promises "how many non-modifier keys can be held
    // at once". The descriptor carries six slots, but this function writes only
    // slot 0, so the honest number is 1. Asserting the behaviour here (rather
    // than only in the contract test) means a future change that starts writing
    // more slots will fail a test instead of silently disagreeing with the
    // reported number.
    {
        TEXTDECK_KEY_EVENT ev = {};
        ev.Modifiers = 0;
        ev.Usage     = 0x04;
        ev.Action    = TextDeckKeyActionDown;
        memset(report, 0xAA, sizeof(report));
        st = TextDeckBuildReport(&ev, report);

        check("down: exactly one key slot carries the usage",
              st == STATUS_SUCCESS && report[2] == 0x04, "byte2 == 0x04");
        bool otherSlotsEmpty = true;
        for (int i = 3; i < TEXTDECK_REPORT_SIZE; ++i) if (report[i] != 0) otherSlotsEmpty = false;
        check("down: all other key slots are zero (one key at a time)",
              otherSlotsEmpty, "bytes 3..7 == 0");
        check("advertised concurrency equals the number of slots written",
              TEXTDECK_MAX_CONCURRENT_KEYS == 1, "TEXTDECK_MAX_CONCURRENT_KEYS == 1");
        check("descriptor capacity is documented separately from concurrency",
              TEXTDECK_REPORT_KEY_SLOTS == 6 && TEXTDECK_REPORT_KEY_SLOTS != TEXTDECK_MAX_CONCURRENT_KEYS,
              "slots=6 concurrency=1");
    }

    // --- 8. reserved byte must be zero -----------------------------------
    // The descriptor declares byte 1 as Constant, so a caller that fills it is
    // speaking a different protocol. Rejecting is the only safe answer.
    {
        TEXTDECK_KEY_EVENT ev = {};
        ev.Modifiers = TEXTDECK_MOD_LEFT_SHIFT;
        ev.Usage     = 0x04;
        ev.Action    = TextDeckKeyActionDown;
        ev.Reserved  = 1;
        check("down: non-zero Reserved rejected",
              TextDeckBuildReport(&ev, report) == STATUS_INVALID_PARAMETER,
              "Reserved must be 0");

        ev.Reserved = 0xFF;
        check("down: Reserved 0xFF rejected",
              TextDeckBuildReport(&ev, report) == STATUS_INVALID_PARAMETER,
              "Reserved must be 0");

        ev.Action = TextDeckKeyActionUp;
        ev.Usage  = 0x04;
        check("up: non-zero Reserved rejected",
              TextDeckBuildReport(&ev, report) == STATUS_INVALID_PARAMETER,
              "Reserved must be 0");

        ev.Action = TextDeckKeyActionUp;
        ev.Reserved = 0;
        check("up: Reserved 0 accepted",
              TextDeckBuildReport(&ev, report) == STATUS_SUCCESS, "Reserved 0");
    }

    // --- 9. down -> up -> release-all leaves nothing asserted ------------
    {
        TEXTDECK_KEY_EVENT ev = {};
        ev.Modifiers = TEXTDECK_MOD_LEFT_CTRL;
        ev.Usage     = 0x06;             // 'c'
        ev.Action    = TextDeckKeyActionDown;
        st = TextDeckBuildReport(&ev, report);
        bool downOk = (st == STATUS_SUCCESS) && report[0] == TEXTDECK_MOD_LEFT_CTRL &&
                      report[2] == 0x06;

        ev.Action = TextDeckKeyActionUp;
        st = TextDeckBuildReport(&ev, report);
        // Protocol 1.1: releasing the key keeps the caller's modifier set held.
        // Asserting "all-zero" here was asserting the defect.
        bool upOk = (st == STATUS_SUCCESS) &&
                    report[0] == TEXTDECK_MOD_LEFT_CTRL && report[2] == 0x00;

        ev.Action = TextDeckKeyActionReleaseAll;
        ev.Modifiers = 0;
        st = TextDeckBuildReport(&ev, report);
        bool raOk = (st == STATUS_SUCCESS) && report_is_all_zero(report);

        check("sequence: Ctrl+C down asserts modifier + key", downOk, "byte0=Ctrl byte2=0x06");
        check("sequence: up clears the key but keeps Ctrl held", upOk,
              "byte0=Ctrl byte2=0x00");
        check("sequence: release-all clears everything", raOk, "all-zero");
    }

    // --- 10. LED output report decoding ----------------------------------
    {
        UCHAR buf[4];
        ULONG leds = 0xFFFFFFFFu;

        check("led: null buffer rejected",
              TextDeckDecodeLedReport(NULL, 1, &leds) == STATUS_INVALID_PARAMETER,
              "STATUS_INVALID_PARAMETER");
        check("led: null out rejected",
              TextDeckDecodeLedReport(buf, 1, NULL) == STATUS_INVALID_PARAMETER,
              "STATUS_INVALID_PARAMETER");

        buf[0] = TEXTDECK_LED_CAPS_LOCK;
        check("led: zero-length packet rejected",
              TextDeckDecodeLedReport(buf, 0, &leds) == STATUS_INVALID_PARAMETER,
              "length must be 1");
        check("led: two-byte packet rejected (descriptor declares one byte)",
              TextDeckDecodeLedReport(buf, 2, &leds) == STATUS_INVALID_PARAMETER,
              "length must be 1");

        leds = 0xDEADBEEF;
        check("led: rejected packet leaves state untouched",
              TextDeckDecodeLedReport(buf, 2, &leds) == STATUS_INVALID_PARAMETER &&
              leds == 0xDEADBEEF, "state preserved");

        buf[0] = TEXTDECK_LED_CAPS_LOCK | TEXTDECK_LED_NUM_LOCK;
        st = TextDeckDecodeLedReport(buf, TEXTDECK_LED_REPORT_SIZE, &leds);
        check("led: Caps|Num decoded",
              st == STATUS_SUCCESS &&
              leds == (TEXTDECK_LED_CAPS_LOCK | TEXTDECK_LED_NUM_LOCK),
              "0x03");

        buf[0] = 0xFF;   // padding bits set: descriptor says they are Constant 0
        st = TextDeckDecodeLedReport(buf, TEXTDECK_LED_REPORT_SIZE, &leds);
        check("led: padding bits masked out",
              st == STATUS_SUCCESS && leds == TEXTDECK_LED_VALID_MASK, "0x1F");

        buf[0] = 0x00;
        st = TextDeckDecodeLedReport(buf, TEXTDECK_LED_REPORT_SIZE, &leds);
        check("led: all-off decoded as 0",
              st == STATUS_SUCCESS && leds == 0, "0x00");

        // Each declared LED usage must round-trip on its own.
        bool eachOk = true;
        for (int bit = 0; bit < 5; ++bit) {
            buf[0] = (UCHAR)(1u << bit);
            if (TextDeckDecodeLedReport(buf, TEXTDECK_LED_REPORT_SIZE, &leds) != STATUS_SUCCESS)
                eachOk = false;
            if (leds != (ULONG)(1u << bit)) eachOk = false;
        }
        check("led: all 5 declared LED usages round-trip", eachOk, "bits 0..4");
    }

    printf("=======================================================\n");
    printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    if (g_failures) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: ALL DRIVER LOGIC CHECKS PASSED\n");
    return 0;
}
