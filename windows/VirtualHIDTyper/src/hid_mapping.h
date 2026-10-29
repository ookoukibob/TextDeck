// ---------------------------------------------------------------------------
// hid_mapping.h
//
// Character -> HID keyboard stroke mapping for the VirtualHIDTyper app.
//
// SUPPORTED KEYBOARD LAYOUT: US English QWERTY (Windows layout 00000409).
// The produced HID usage ids are raw keyboard scan codes; Windows translates
// them according to the *active keyboard layout of the focused session*.
// Characters below are guaranteed correct only when the target machine's
// active layout is US English. Other layouts may produce different glyphs
// for shifted punctuation. Characters that have no US key are reported as
// unsupported (never silently mistyped).
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>
#include <string>

namespace vhid {

// HID modifier bitmap bits (USB HID Usage Tables, section 10 Keyboard/Keypad)
constexpr BYTE kModLeftCtrl   = 0x01;
constexpr BYTE kModLeftShift  = 0x02;
constexpr BYTE kModLeftAlt    = 0x04;
constexpr BYTE kModLeftGui    = 0x08;
constexpr BYTE kModRightCtrl  = 0x10;
constexpr BYTE kModRightShift = 0x20;
constexpr BYTE kModRightAlt   = 0x40;
constexpr BYTE kModRightGui   = 0x80;

// Well-known HID keyboard usages
constexpr BYTE kUsageNone   = 0x00;
constexpr BYTE kUsageEnter  = 0x28;
constexpr BYTE kUsageTab    = 0x2B;
constexpr BYTE kUsageSpace  = 0x2C;

struct KeyStroke {
    BYTE usage;       // HID keyboard usage id (valid only when supported==true)
    BYTE modifiers;   // HID modifier bitmap (usually 0 or kModLeftShift)
    bool supported;   // false => no US-layout key produces this character
};

// Convert one wide character to a HID keystroke.
//  - '\n' and '\r' both mean "newline": usage=Enter; when shiftEnterForNewline
//    is true (default behaviour of the app) the Shift modifier is added, i.e.
//    Shift+Enter, which chat applications typically treat as a line break
//    instead of "send message".
//  - All printable ASCII (letters, digits, space, common punctuation) is
//    mapped per US QWERTY, including Shift for uppercase/symbols.
//  - Any other character returns supported=false; the caller must count and
//    report it, never send a wrong key.
KeyStroke CharToStroke(wchar_t ch, bool shiftEnterForNewline);

// Human-readable description of an unsupported character, e.g. L"'é' (U+00E9)".
std::wstring DescribeUnmapped(wchar_t ch);

} // namespace vhid
