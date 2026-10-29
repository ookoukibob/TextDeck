// ---------------------------------------------------------------------------
// sw_input.h
//
// Software-input mode (SendInput) text planning -- PURE LOGIC, no UI, no
// driver, no Win32 calls: unit-testable in map_selftest.exe.
//
// The software mode sends Unicode keyboard events (KEYEVENTF_UNICODE) so any
// layout / CJK / astral-plane character works. Newlines and Tab are NEVER
// sent as plain Unicode characters: '\n' becomes VK_RETURN (optionally with
// Shift) and '\t' becomes VK_TAB, matching what real keys do. A surrogate
// pair is planned as ONE code-point event so non-BMP characters are never
// split across checks; unpaired surrogates are skipped and counted, never
// sent as garbage.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

namespace vhid {

enum class SwKind : int {
    CodePoint  = 0, // one Unicode code point -> KEYEVENTF_UNICODE (astral
                    // characters stay as a single event; the sender expands
                    // them into a surrogate-pair sequence in ONE SendInput)
    Enter      = 1, // '\n' -> VK_RETURN (plain Enter)
    ShiftEnter = 2, // '\n' in Shift+Enter mode -> Shift + VK_RETURN
    Tab        = 3, // '\t' -> VK_TAB (never a Unicode character)
    SkipInvalid= 4, // unpaired surrogate or NUL: counted as skipped, not sent
};

struct SwEvent {
    SwKind        kind;
    unsigned int  cp; // code point when kind==CodePoint, otherwise 0
};

// CRLF and lone CR each collapse into a single '\n'; existing LFs stay.
std::wstring NormalizeNewlines(const std::wstring& text);

// Plan the send sequence for text (UTF-16). Iterates by CODE POINTS: a
// surrogate pair yields ONE CodePoint event with the full code point, an
// unpaired high/low surrogate yields SkipInvalid. '\n'/'\r' -> Enter or
// ShiftEnter per shiftEnterForNewline; '\t' -> Tab.
std::vector<SwEvent> PlanSwEvents(const std::wstring& text,
                                  bool shiftEnterForNewline);

} // namespace vhid
