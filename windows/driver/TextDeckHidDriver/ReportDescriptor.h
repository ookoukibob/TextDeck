/*++
TextDeck - HID report descriptor
================================

A minimal, standard 8-byte boot-keyboard style report:

  byte 0    modifier bitmap (8 one-bit fields, Usage Page 0x07, usages 0xE0-0xE7)
  byte 1    reserved / OEM (constant, always 0)
  byte 2-7  six key slots, each 0x00..0x65 on Usage Page 0x07

The layout follows the "Keyboard/Keypad" section of the USB HID Usage Tables
(1.12) and the HID 1.11 report-descriptor grammar. It is deliberately the same
shape Windows already understands as a boot keyboard, so no custom driver is
needed on the receiving side - the OS's own kbdhid/hidclass stack binds it.

Written for TextDeck from the HID specification. No third-party descriptor was
copied.
--*/

#pragma once
#include <ntddk.h>

/* 63 bytes: modifiers, reserved, 6 key slots, LED output. */
#define TEXTDECK_REPORT_DESCRIPTOR_SIZE  63

static const UCHAR g_TextDeckReportDescriptor[TEXTDECK_REPORT_DESCRIPTOR_SIZE] = {
    /* --- Usage Page (Generic Desktop) --- */
    0x05, 0x01,
    /* --- Usage (Keyboard) --- */
    0x09, 0x06,
    /* --- Collection (Application) --- */
    0xA1, 0x01,

        /* ---- Modifier byte: 8 single-bit fields, usages 0xE0..0xE7 ---- */
        0x05, 0x07,        /*   Usage Page (Keyboard/Keypad)          */
        0x19, 0xE0,        /*   Usage Minimum (Left Control)          */
        0x29, 0xE7,        /*   Usage Maximum (Right GUI)             */
        0x15, 0x00,        /*   Logical Minimum (0)                   */
        0x25, 0x01,        /*   Logical Maximum (1)                   */
        0x75, 0x01,        /*   Report Size (1)                       */
        0x95, 0x08,        /*   Report Count (8)                      */
        0x81, 0x02,        /*   Input (Data, Variable, Absolute)      */

        /* ---- Reserved byte, always 0 ---- */
        0x95, 0x01,        /*   Report Count (1)                      */
        0x75, 0x08,        /*   Report Size (8)                       */
        0x81, 0x01,        /*   Input (Constant)                      */

        /* ---- Six key slots ---- */
        0x95, 0x06,        /*   Report Count (6)                      */
        0x75, 0x08,        /*   Report Size (8)                       */
        0x15, 0x00,        /*   Logical Minimum (0)                   */
        0x25, 0x65,        /*   Logical Maximum (101)                 */
        0x05, 0x07,        /*   Usage Page (Keyboard/Keypad)          */
        0x19, 0x00,        /*   Usage Minimum (0)                     */
        0x29, 0x65,        /*   Usage Maximum (101)                   */
        0x81, 0x00,        /*   Input (Data, Array)                   */

        /* ---- LED output report (Caps/Num/Scroll lock), 5 bits + padding ---- */
        0x95, 0x05,        /*   Report Count (5)                      */
        0x75, 0x01,        /*   Report Size (1)                       */
        0x05, 0x08,        /*   Usage Page (LEDs)                     */
        0x19, 0x01,        /*   Usage Minimum (Num Lock)              */
        0x29, 0x05,        /*   Usage Maximum (Kana)                  */
        0x91, 0x02,        /*   Output (Data, Variable, Absolute)     */
        0x95, 0x01,        /*   Report Count (1)                      */
        0x75, 0x03,        /*   Report Size (3)                       */
        0x91, 0x01,        /*   Output (Constant)                     */

    /* --- End Collection --- */
    0xC0
};
