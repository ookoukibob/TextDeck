// ---------------------------------------------------------------------------
// hid_mapping.cpp
// ---------------------------------------------------------------------------
#include "hid_mapping.h"

namespace vhid {
namespace {

struct AsciiKey {
    wchar_t ch;
    BYTE    usage;
    bool    shift;
};

// US QWERTY layout -> HID usage ids (USB HID Usage Tables 1.12, section 10).
const AsciiKey kTable[] = {
    // lowercase letters: usage 0x04..0x1D
    {L'a', 0x04, false}, {L'b', 0x05, false}, {L'c', 0x06, false},
    {L'd', 0x07, false}, {L'e', 0x08, false}, {L'f', 0x09, false},
    {L'g', 0x0A, false}, {L'h', 0x0B, false}, {L'i', 0x0C, false},
    {L'j', 0x0D, false}, {L'k', 0x0E, false}, {L'l', 0x0F, false},
    {L'm', 0x10, false}, {L'n', 0x11, false}, {L'o', 0x12, false},
    {L'p', 0x13, false}, {L'q', 0x14, false}, {L'r', 0x15, false},
    {L's', 0x16, false}, {L't', 0x17, false}, {L'u', 0x18, false},
    {L'v', 0x19, false}, {L'w', 0x1A, false}, {L'x', 0x1B, false},
    {L'y', 0x1C, false}, {L'z', 0x1D, false},
    // uppercase letters (Shift)
    {L'A', 0x04, true},  {L'B', 0x05, true},  {L'C', 0x06, true},
    {L'D', 0x07, true},  {L'E', 0x08, true},  {L'F', 0x09, true},
    {L'G', 0x0A, true},  {L'H', 0x0B, true},  {L'I', 0x0C, true},
    {L'J', 0x0D, true},  {L'K', 0x0E, true},  {L'L', 0x0F, true},
    {L'M', 0x10, true},  {L'N', 0x11, true},  {L'O', 0x12, true},
    {L'P', 0x13, true},  {L'Q', 0x14, true},  {L'R', 0x15, true},
    {L'S', 0x16, true},  {L'T', 0x17, true},  {L'U', 0x18, true},
    {L'V', 0x19, true},  {L'W', 0x1A, true},  {L'X', 0x1B, true},
    {L'Y', 0x1C, true},  {L'Z', 0x1D, true},
    // digits and their shifted symbols
    {L'1', 0x1E, false}, {L'2', 0x1F, false}, {L'3', 0x20, false},
    {L'4', 0x21, false}, {L'5', 0x22, false}, {L'6', 0x23, false},
    {L'7', 0x24, false}, {L'8', 0x25, false}, {L'9', 0x26, false},
    {L'0', 0x27, false},
    {L'!', 0x1E, true},  {L'@', 0x1F, true},  {L'#', 0x20, true},
    {L'$', 0x21, true},  {L'%', 0x22, true},  {L'^', 0x23, true},
    {L'&', 0x24, true},  {L'*', 0x25, true},  {L'(', 0x26, true},
    {L')', 0x27, true},
    // whitespace
    {L'\t', kUsageTab,   false},   // Tab
    {L' ',  kUsageSpace, false},   // Space
    // punctuation (US layout)
    {L'-', 0x2D, false}, {L'_', 0x2D, true},
    {L'=', 0x2E, false}, {L'+', 0x2E, true},
    {L'[', 0x2F, false}, {L'{', 0x2F, true},
    {L']', 0x30, false}, {L'}', 0x30, true},
    {L'\\', 0x31, false}, {L'|', 0x31, true},
    // USB HID Usage Tables 1.12, section 10:
    //   0x32 = Keyboard Non-US # and ~   (Windows renders this as '\')
    //   0x34 = Keyboard ' and "          <-- the US apostrophe/quote key
    // Using 0x32 here made every ' and " arrive as \ and |.
    {L'\'', 0x34, false}, {L'"', 0x34, true},
    {L';', 0x33, false}, {L':', 0x33, true},
    {L'`', 0x35, false}, {L'~', 0x35, true},
    {L',', 0x36, false}, {L'<', 0x36, true},
    {L'.', 0x37, false}, {L'>', 0x37, true},
    {L'/', 0x38, false}, {L'?', 0x38, true},
};

} // namespace

KeyStroke CharToStroke(wchar_t ch, bool shiftEnterForNewline)
{
    KeyStroke out = {kUsageNone, 0, false};

    if (ch == L'\n' || ch == L'\r') {
        out.usage = kUsageEnter;
        out.modifiers = shiftEnterForNewline ? kModLeftShift : 0;
        out.supported = true;
        return out;
    }

    for (const AsciiKey& k : kTable) {
        if (k.ch == ch) {
            out.usage = k.usage;
            out.modifiers = k.shift ? kModLeftShift : 0;
            out.supported = true;
            return out;
        }
    }

    return out; // unsupported
}

std::wstring DescribeUnmapped(wchar_t ch)
{
    wchar_t buf[64];
    if (ch < 0x20 || ch == 0x7F) {
        swprintf_s(buf, L"U+%04X", (unsigned)ch);
    } else {
        swprintf_s(buf, L"'%c' (U+%04X)", ch, (unsigned)ch);
    }
    return buf;
}

} // namespace vhid
