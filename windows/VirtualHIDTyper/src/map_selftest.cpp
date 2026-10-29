// ---------------------------------------------------------------------------
// map_selftest.cpp
//
// Console self-test for the character -> HID stroke mapping plus the ESP32
// protocol codec. Runs without the driver and without any serial port:
// verifies the full supported ASCII range, uppercase/Shift handling,
// digits, punctuation, Shift+Enter newline handling, ESP32 frame build/parse,
// CRC resync, PING/WRITE/status parsing, CRLF normalisation and the 10 KiB
// boundary. Exit code 0 = all checks passed.
// ---------------------------------------------------------------------------
#include "hid_mapping.h"
#include "sw_input.h"
#include "esp32_protocol.h"
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

using namespace vhid;

static int g_failures = 0;
static int g_checks = 0;

static void Check(bool cond, const wchar_t* what)
{
    ++g_checks;
    if (!cond) {
        ++g_failures;
        wprintf(L"  [FAIL] %s\n", what);
    }
}

static void Expect(wchar_t ch, BYTE usage, BYTE mods, bool shiftEnterNl)
{
    ++g_checks;
    KeyStroke s = CharToStroke(ch, shiftEnterNl);
    if (!s.supported || s.usage != usage || s.modifiers != mods) {
        ++g_failures;
        wprintf(L"  [FAIL] char U+%04X: got usage=0x%02X mods=0x%02X supported=%d, "
                L"want usage=0x%02X mods=0x%02X\n",
                (unsigned)ch, s.usage, s.modifiers, (int)s.supported, usage, mods);
    }
}

int wmain()
{
    wprintf(L"VirtualHIDTyper mapping self-test (US English layout 00000409)\n");
    wprintf(L"=========================================================\n");

    // 1) Lowercase a-z: usages 0x04..0x1D, no modifier.
    for (wchar_t c = L'a'; c <= L'z'; ++c)
        Expect(c, (BYTE)(0x04 + (c - L'a')), 0, true);

    // 2) Uppercase A-Z: same usages + Left Shift.
    for (wchar_t c = L'A'; c <= L'Z'; ++c)
        Expect(c, (BYTE)(0x04 + (c - L'A')), kModLeftShift, true);

    // 3) Digits 1..9,0 usages 0x1E..0x27 and shifted symbols.
    const wchar_t* digits = L"1234567890";
    const wchar_t* shiftedDigits = L"!@#$%^&*()";
    for (int i = 0; i < 10; ++i) {
        Expect(digits[i], (BYTE)(0x1E + i), 0, true);
        Expect(shiftedDigits[i], (BYTE)(0x1E + i), kModLeftShift, true);
    }

    // 4) Space / Tab.
    Expect(L' ', kUsageSpace, 0, true);
    Expect(L'\t', kUsageTab, 0, true);

    // 5) Punctuation pairs (base / shifted) per US layout.
    struct P { wchar_t base, shifted; BYTE usage; };
    const P punct[] = {
        {L'-', L'_', 0x2D}, {L'=', L'+', 0x2E}, {L'[', L'{', 0x2F},
        {L']', L'}', 0x30}, {L'\\', L'|', 0x31}, {L'\'', L'"', 0x34},  // 0x34 = ' and " (0x32 is Non-US #)
        {L';', L':', 0x33}, {L'`', L'~', 0x35}, {L',', L'<', 0x36},
        {L'.', L'>', 0x37}, {L'/', L'?', 0x38},
    };
    for (const P& p : punct) {
        Expect(p.base, p.usage, 0, true);
        Expect(p.shifted, p.usage, kModLeftShift, true);
    }

    // 6) Newline: Shift+Enter by default, plain Enter when disabled.
    Expect(L'\n', kUsageEnter, kModLeftShift, true);
    Expect(L'\r', kUsageEnter, kModLeftShift, true);
    Expect(L'\n', kUsageEnter, 0, false);

    // 7) Unsupported characters must be flagged, not mapped.
    const wchar_t unsupported[] = {0x00E9 /*é*/, 0x4E2D /*中*/, 0x2014 /*—*/,
                                   0x2026 /*…*/, 0x0416 /*Ж*/};
    for (wchar_t c : unsupported) {
        KeyStroke s = CharToStroke(c, true);
        Check(!s.supported, DescribeUnmapped(c).c_str());
    }
    Check(!CharToStroke(0x0000, true).supported, L"NUL must be unsupported");

    // 8) End-to-end sequence for the acceptance string "Hello, World! 123".
    {
        const wchar_t* text = L"Hello, World! 123";
        struct Ev { BYTE usage; BYTE mods; };
        std::vector<Ev> got;
        for (const wchar_t* p = text; *p; ++p) {
            KeyStroke s = CharToStroke(*p, true);
            Check(s.supported, L"all chars of 'Hello, World! 123' must be supported");
            got.push_back({s.usage, s.modifiers});
        }
        const Ev want[] = {
            {0x0B, 0x02}, // H
            {0x08, 0x00}, // e
            {0x0F, 0x00}, // l
            {0x0F, 0x00}, // l
            {0x12, 0x00}, // o
            {0x36, 0x00}, // ,
            {0x2C, 0x00}, // space
            {0x1A, 0x02}, // W
            {0x12, 0x00}, // o
            {0x15, 0x00}, // r
            {0x0F, 0x00}, // l
            {0x07, 0x00}, // d
            {0x1E, 0x02}, // !
            {0x2C, 0x00}, // space
            {0x1E, 0x00}, // 1
            {0x1F, 0x00}, // 2
            {0x20, 0x00}, // 3
        };
        Check(got.size() == sizeof(want) / sizeof(want[0]), L"'Hello, World! 123' length");
        for (size_t i = 0; i < got.size() && i < sizeof(want) / sizeof(want[0]); ++i) {
            if (got[i].usage != want[i].usage || got[i].mods != want[i].mods) {
                ++g_failures;
                wprintf(L"  [FAIL] acceptance seq[%zu]: got usage=0x%02X mods=0x%02X, "
                        L"want usage=0x%02X mods=0x%02X\n",
                        i, got[i].usage, got[i].mods, want[i].usage, want[i].mods);
            }
        }
    }

    // -----------------------------------------------------------------
    // Software-input mode planning (pure logic; no SendInput / no driver)
    // -----------------------------------------------------------------
    {
        // newline normalization
        Check(NormalizeNewlines(L"a\r\nb") == L"a\nb", L"norm CRLF -> LF");
        Check(NormalizeNewlines(L"a\rb") == L"a\nb", L"norm lone CR -> LF");
        Check(NormalizeNewlines(L"a\nb\nc") == L"a\nb\nc", L"norm keeps LF");
        Check(NormalizeNewlines(L"") == L"", L"norm empty");

        // empty text plans nothing
        Check(PlanSwEvents(L"", false).empty(), L"plan empty -> 0 events");

        // plain ASCII -> one CodePoint event per character
        {
            std::vector<SwEvent> ev = PlanSwEvents(L"abc", false);
            Check(ev.size() == 3, L"'abc' -> 3 events");
            Check(ev.size() == 3 && ev[0].kind == SwKind::CodePoint
                  && ev[0].cp == L'a' && ev[2].cp == L'c',
                  L"'abc' code points preserved");
        }

        // astral character (U+1F600 = D83D DE00) is ONE event, never split
        {
            std::wstring t;
            t += L'a';
            t += (wchar_t)0xD83D;
            t += (wchar_t)0xDE00;
            t += L'b';
            std::vector<SwEvent> ev = PlanSwEvents(t, false);
            Check(ev.size() == 3, L"a+emoji+b -> 3 events (pair not split)");
            Check(ev.size() == 3 && ev[1].kind == SwKind::CodePoint
                  && ev[1].cp == 0x1F600u, L"emoji planned as single cp 1F600");
            Check(ev.size() == 3 && ev[2].cp == (unsigned int)L'b',
                  L"char after pair intact");
        }

        // unpaired surrogates are skipped & counted, never sent
        // (0xD800 followed by 0xDC00 WOULD be a valid pair = U+10000, so the
        //  lone high must not touch a low, and vice versa)
        {
            std::wstring t;
            t += L'a';
            t += (wchar_t)0xD800;    // lone high surrogate (next = 'b')
            t += L'b';
            t += (wchar_t)0xDC00;    // lone low surrogate (prev = 'b')
            std::vector<SwEvent> ev = PlanSwEvents(t, false);
            Check(ev.size() == 4, L"lone surrogates -> 4 events");
            Check(ev.size() == 4 && ev[1].kind == SwKind::SkipInvalid
                  && ev[3].kind == SwKind::SkipInvalid,
                  L"lone high & lone low -> SkipInvalid");
        }

        // NUL is never typed
        {
            std::wstring t(L"a\0b", 3);
            std::vector<SwEvent> ev = PlanSwEvents(t, false);
            Check(ev.size() == 3 && ev[1].kind == SwKind::SkipInvalid,
                  L"embedded NUL -> SkipInvalid");
        }

        // newline -> Enter / ShiftEnter per option; tab -> Tab (not Unicode)
        {
            std::vector<SwEvent> plain = PlanSwEvents(L"\n", false);
            Check(plain.size() == 1 && plain[0].kind == SwKind::Enter,
                  L"LF -> Enter when Shift+Enter off");
            std::vector<SwEvent> shift = PlanSwEvents(L"\n", true);
            Check(shift.size() == 1 && shift[0].kind == SwKind::ShiftEnter,
                  L"LF -> ShiftEnter when Shift+Enter on");
            std::vector<SwEvent> tab = PlanSwEvents(L"\t", false);
            Check(tab.size() == 1 && tab[0].kind == SwKind::Tab,
                  L"TAB -> Tab event (never a Unicode char)");
        }

        // CRLF in real input: normalize first, then ONE Enter is planned
        {
            std::wstring norm = NormalizeNewlines(L"x\r\ny");
            std::vector<SwEvent> ev = PlanSwEvents(norm, false);
            Check(ev.size() == 3 && ev[1].kind == SwKind::Enter,
                  L"x<CRLF>y -> x, Enter, y (one break)");
        }

        // CJK plain-BMP char
        {
            std::vector<SwEvent> ev = PlanSwEvents(L"\u4E2D", false);
            Check(ev.size() == 1 && ev[0].kind == SwKind::CodePoint
                  && ev[0].cp == 0x4E2Du, L"CJK U+4E2D -> CodePoint");
        }

        // long text: event count == code-point count (per-event granularity)
        {
            std::wstring longTxt(2000, L'z');
            std::vector<SwEvent> ev = PlanSwEvents(longTxt, false);
            Check(ev.size() == 2000, L"2000 chars -> 2000 events");
        }
    }

    // -----------------------------------------------------------------
    // ESP32 protocol codec (pure logic; no Win32, no serial I/O)
    // -----------------------------------------------------------------
    {
        using namespace esp32;
        // CRC32/IEEE must match zlib.crc32: "123456789" -> 0xCBF43926.
        {
            const char* v = "123456789";
            Check(Crc32((const uint8_t*)v, 9) == 0xCBF43926u,
                  L"esp32 CRC32 test vector");
            Check(Crc32(nullptr, 0) == 0, L"esp32 CRC32 empty");
        }
        // Frame round-trip for every new command.
        for (uint8_t cmd : {kCmdWriteText, kCmdReadText, kCmdPing,
                            kCmdStartTyping, kCmdCancelTyping, kCmdGetStatus}) {
            const uint8_t body[] = {0x41, 0x0A, 0x09};
            std::vector<uint8_t> f = BuildFrame(cmd, body, 3);
            Check(!f.empty() && f.size() == kHeaderLen + 3, L"esp32 build frame");
            std::vector<uint8_t> buf = f;
            uint8_t got = 0;
            std::vector<uint8_t> pl;
            Check(ExtractFrame(buf, &got, &pl) && got == cmd && pl.size() == 3 &&
                  pl[0] == 0x41 && buf.empty(), L"esp32 extract frame");
        }
        // Garbage bytes (e.g. firmware boot logs) are skipped; a half frame is
        // retained until complete; a bad-CRC candidate resynchronises.
        {
            std::vector<uint8_t> buf = {'b', 'o', 'o', 't', 0xE7};
            uint8_t got = 0;
            std::vector<uint8_t> pl;
            Check(!ExtractFrame(buf, &got, &pl), L"esp32 garbage held, not parsed");
            std::vector<uint8_t> f = BuildFrame(kCmdPing, nullptr, 0);
            buf.insert(buf.end(), f.begin() + 1, f.end());
            Check(ExtractFrame(buf, &got, &pl) && got == kCmdPing && pl.empty(),
                  L"esp32 resync past garbage");
            std::vector<uint8_t> bad = BuildFrame(kCmdPing, nullptr, 0);
            bad[6] ^= 0xFF;
            std::vector<uint8_t> good = BuildFrame(kCmdPing, nullptr, 0);
            buf = bad;
            buf.insert(buf.end(), good.begin(), good.end());
            Check(ExtractFrame(buf, &got, &pl) && got == kCmdPing,
                  L"esp32 bad CRC resync to next frame");
        }
        // PING payload: exact 16 bytes, field layout per protocol.md.
        {
            std::vector<uint8_t> p(16, 0);
            p[0] = 0; p[1] = 0x01; p[2] = 0x02; p[3] = 0x01;
            p[4] = 0x00; p[5] = 0x28; p[6] = 0x01;
            p[7] = 0x05; p[8] = 0x00;
            p[9] = 0x11; p[10] = 0x22; p[11] = 0x33; p[12] = 0x44;
            p[13] = 0x07; p[14] = 0x00; p[15] = 0x01;
            PingInfo pi;
            std::wstring err;
            Check(ParsePing(p, &pi, &err) && pi.fw == 0x0102 &&
                  pi.maxText == 0x2800 && pi.hasText && pi.savedLen == 5 &&
                  pi.savedCrc == 0x44332211u && pi.hidReady,
                  L"esp32 parse PING");
            Check(!ParsePing(std::vector<uint8_t>(15, 0), nullptr, nullptr),
                  L"esp32 reject short PING");
        }
        // WRITE ack: exact 14 bytes.
        {
            std::vector<uint8_t> p(14, 0);
            p[0] = 0; p[1] = 0x03; p[2] = 0x00; p[3] = 0x02; p[4] = 0x00;
            p[5] = 0x78; p[6] = 0x56; p[7] = 0x34; p[8] = 0x12;
            p[9] = 0x09; p[10] = 0x00; p[11] = 0x00; p[12] = 0x00; p[13] = 0x01;
            WriteAck ack;
            Check(ParseWriteAck(p, &ack, nullptr) && ack.status == 0 &&
                  ack.detail == 3 && ack.savedLen == 2 &&
                  ack.savedCrc == 0x12345678u && ack.seq == 9 && ack.written,
                  L"esp32 parse WRITE ack");
        }
        // Typing status: exact 16 bytes for START/CANCEL/GET_STATUS.
        {
            std::vector<uint8_t> p(16, 0);
            p[0] = 0; p[1] = 2; p[2] = 0x0A; p[3] = 0x00; p[4] = 0x64; p[5] = 0x00;
            p[6] = 0; p[7] = 0; p[8] = 0x05; p[9] = 0x00;
            p[10] = 0xE8; p[11] = 0x03; p[12] = 0x02; p[13] = 0x00;
            p[14] = 0x07; p[15] = 0x00;
            TypingStatus s;
            Check(ParseTypingStatus(p, &s, nullptr) && s.state == kTyTyping &&
                  s.processed == 10 && s.total == 100 &&
                  (s.flags & kFlagHostInitiated) && s.sessionId == 2 &&
                  s.capabilities == 7, L"esp32 parse typing status");
            p[2] = 0x65; p[3] = 0x00;   // processed > total must be rejected
            Check(!ParseTypingStatus(p, nullptr, nullptr),
                  L"esp32 reject processed>total");
        }
        // Text preparation: CRLF -> LF, lone CR -> LF, ASCII set, 10 KiB cap.
        {
            std::string ascii;
            std::wstring err;
            size_t bad = 0;
            wchar_t ch = 0;
            Check(PrepareText(L"a\r\nb\rc", &ascii, &err) && ascii == "a\nb\nc",
                  L"esp32 CRLF/lone-CR normalise");
            Check(PrepareText(L"A\tZ 0-9 !~", &ascii, &err), L"esp32 ASCII ok");
            Check(!PrepareText(L"", &ascii, &err), L"esp32 reject empty");
            Check(!PrepareText(L"ab\u4E2Dd", &ascii, &err, &bad, &ch) &&
                  bad == 2 && ch == 0x4E2D, L"esp32 reject CJK with index");
            Check(!PrepareText(std::wstring(10241, L'a'), &ascii, &err),
                  L"esp32 reject over 10 KiB");
            Check(PrepareText(std::wstring(10240, L'a'), &ascii, &err) &&
                  ascii.size() == 10240, L"esp32 accept exactly 10 KiB");
        }
    }

    if (g_failures == 0) {
        wprintf(L"\nRESULT: ALL CHECKS PASSED (%d checks)\n", g_checks);
        return 0;
    }
    wprintf(L"\nRESULT: %d CHECK(S) FAILED\n", g_failures);
    return 1;
}
