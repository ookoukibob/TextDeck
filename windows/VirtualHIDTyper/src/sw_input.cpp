// ---------------------------------------------------------------------------
// sw_input.cpp
//
// Pure planning logic for the software-input mode. See sw_input.h.
// ---------------------------------------------------------------------------
#include "sw_input.h"

namespace vhid {

std::wstring NormalizeNewlines(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i; // CRLF -> one break
            out.push_back(L'\n');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

static bool IsHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
static bool IsLowSurrogate(wchar_t c)  { return c >= 0xDC00 && c <= 0xDFFF; }

std::vector<SwEvent> PlanSwEvents(const std::wstring& text,
                                  bool shiftEnterForNewline)
{
    std::vector<SwEvent> ev;
    ev.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c == L'\n' || c == L'\r') {
            // '\r' is defensive: the caller normally normalizes first.
            ev.push_back({shiftEnterForNewline ? SwKind::ShiftEnter
                                               : SwKind::Enter, 0});
        } else if (c == L'\t') {
            ev.push_back({SwKind::Tab, 0});
        } else if (IsHighSurrogate(c)) {
            if (i + 1 < text.size() && IsLowSurrogate(text[i + 1])) {
                unsigned int cp = 0x10000u
                    + (((unsigned int)c - 0xD800u) << 10)
                    + ((unsigned int)text[i + 1] - 0xDC00u);
                ev.push_back({SwKind::CodePoint, cp});
                ++i; // the pair is consumed together -- never split
            } else {
                ev.push_back({SwKind::SkipInvalid, 0}); // lone high surrogate
            }
        } else if (IsLowSurrogate(c)) {
            ev.push_back({SwKind::SkipInvalid, 0});     // lone low surrogate
        } else if (c == 0) {
            ev.push_back({SwKind::SkipInvalid, 0});     // NUL cannot be typed
        } else {
            ev.push_back({SwKind::CodePoint, (unsigned int)c});
        }
    }
    return ev;
}

} // namespace vhid
