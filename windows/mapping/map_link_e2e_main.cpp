// ---------------------------------------------------------------------------
// map_link_e2e_main.cpp -- INTEGRATION end-to-end: one live MappingRuntime
// carrying a REAL input adapter, rule matching, action execution and REAL
// TextDeck virtual-HID output into a real target window.
//
// Chain under test (all inside one running runtime):
//   UdpJsonSource (real UDP socket, external event source)
//     -> MappingRuntime bounded queue (single executor)
//     -> Engine rule match -> action script
//     -> HidDriverBackend (IOCTL_TEXTDECK_SUBMIT_KEY on the real driver)
//     -> virtual-HID keyboard -> the EDIT control of the test window
//
// INPUT ORIGIN: UDP datagrams sent by this program over the loopback socket --
// a real adapter path (not RecordOutput, not a direct engine call), but NOT a
// physical keyboard. Real HID output is separately verified by
// map_hid_e2e.exe. This program closes the loop between a live adapter and
// real output.
//
// Assertions:
//   L1  UDP message -> 3x key_tap "a" -> EDIT == "aaa"
//   L2  own virtual-HID output is filtered (recursive binding never fires)
//   L3  momentary hold via UDP press/release leaves no stuck key
//   L4  after cancel + stop, engine holds no keys
// Requires the TextDeck driver (guest VM). Logs to stdout.
// ---------------------------------------------------------------------------
#include "map_raw_input.h"   // pulls winsock2 via map_engine.h
#include "map_queue.h"
#include "map_adapters.h"
#include "map_config.h"
#include <windows.h>         // after winsock2

#include <cstdio>
#include <memory>
#include <string>

using namespace map;

static std::shared_ptr<OutputBackend> g_hid;
static MappingRuntime* g_rt = nullptr;
static HWND g_edit = nullptr;
static int g_fail = 0, g_pass = 0;

static void check(bool ok, const char* name, const std::string& detail = "") {
    if (ok) { ++g_pass; std::printf("  [PASS] %s %s\n", name, detail.c_str()); }
    else    { ++g_fail; std::printf("  [FAIL] %s %s\n", name, detail.c_str()); }
}

static std::string EditText() {
    wchar_t buf[256] = L"";
    GetWindowTextW(g_edit, buf, 255);
    int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, s.data(), len, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

static bool SendUdp(int port, const std::string& json) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((u_short)port);
    int r = sendto(s, json.c_str(), (int)json.size(), 0, (sockaddr*)&a, sizeof(a));
    closesocket(s);
    return r == (int)json.size();
}

static int g_stage = 0;
static int g_udpPort = 0;
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_edit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            10, 10, 400, 30, hwnd, (HMENU)100, GetModuleHandleW(nullptr), nullptr);
        return 0;
    case WM_TIMER: {
        KillTimer(hwnd, (UINT_PTR)wp);
        int stage = g_stage++;
        if (stage == 0) {
            // L1: external message -> 3 taps of "a" through the whole chain
            std::printf("LINK: sending UDP message 'link-go' (input = real UDP adapter)\n");
            SendUdp(g_udpPort, "{\"event\":\"message\",\"text\":\"link-go\"}");
            SetTimer(hwnd, 2, 1200, nullptr);
        } else if (stage == 1) {
            std::string t = EditText();
            HWND fg = GetForegroundWindow();
            std::printf("EDIT after message script: '%s' (outputFailures=%zu)\n",
                        t.c_str(), g_rt->engine().outputFailures());
            std::printf("DIAG: foreground=0x%p ourHwnd=0x%p focus=0x%p edit=0x%p\n",
                        (void*)fg, (void*)hwnd, (void*)GetFocus(), (void*)g_edit);
            check(t == "aaa", "L1-udp-message-typed-3-chars", ("got='" + t + "'").c_str());
            check(t.size() == 3, "L2-no-feedback-runaway", ("len=" + std::to_string(t.size())).c_str());
            // L3: momentary hold of "space" via UDP press/release
            SendUdp(g_udpPort, "{\"event\":\"press\",\"control\":\"holdbtn\"}");
            SetTimer(hwnd, 3, 500, nullptr);
        } else if (stage == 2) {
            check(g_rt->engine().heldKeys().size() == 1, "L3-space-held-during-press",
                  "held=" + std::to_string(g_rt->engine().heldKeys().size()));
            SendUdp(g_udpPort, "{\"event\":\"release\",\"control\":\"holdbtn\"}");
            SetTimer(hwnd, 4, 500, nullptr);
        } else if (stage == 3) {
            check(g_rt->engine().heldKeys().empty(), "L3-space-released-after-release", "");
            // L4: cancel + verify no stuck keys
            g_rt->postCancelAll();
            SetTimer(hwnd, 5, 300, nullptr);
        } else {
            check(g_rt->engine().heldKeys().empty(), "L4-no-keys-after-cancel", "");
            PostQuitMessage(0);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main() {
    std::printf("map_link_e2e -- integration: UDP adapter -> engine -> real HID -> window\n");

    g_hid = MakeBackend("hid");
    std::printf("backend=%s\n", g_hid->name());
    if (std::string(g_hid->name()) != "textdeck-hid") {
        std::printf("  [FAIL] hid-backend-unavailable\n");
        return 1;
    }

    MappingRuntime rt(g_hid, 256);
    g_rt = &rt;
    rt.engine().setRealtime(true); // honor inter-tap delays in scripts

    // binding 1: UDP message containing "link-go" -> tap "a" three times
    // (60 ms apart so each HID report is delivered distinctly, as a typist).
    Binding b1; b1.id = "msg3a";
    b1.match = {"udp", "msg", EventType::MessageReceived};
    b1.match.msgText = "link-go";
    b1.trigger = TriggerKind::Press;
    Action t; t.kind = ActionKind::KeyTap; t.key = "a";
    Action d; d.kind = ActionKind::Delay; d.delayMs = 60;
    b1.script = { t, d, t, d, t };
    rt.engine().addBinding(b1);

    // binding 2: recursion guard -- if own virtual-HID output ("a" press)
    // were not filtered, this would type more "a"s and run away.
    Binding b2; b2.id = "recurse";
    b2.match = {"", "a", EventType::Pressed};
    b2.trigger = TriggerKind::Press;
    Action t2; t2.kind = ActionKind::KeyTap; t2.key = "a";
    b2.script = { t2 };
    rt.engine().addBinding(b2);

    // binding 3: momentary hold -- UDP press/release on "holdbtn" holds Space
    Binding b3; b3.id = "holdspace";
    b3.match = {"udp", "holdbtn", EventType::Pressed};
    b3.trigger = TriggerKind::Momentary; b3.holdKey = "space";
    rt.engine().addBinding(b3);

    rt.start();

    // REAL input adapter: UDP JSON on an ephemeral loopback port
    UdpJsonSource udp("udp", 0);
    std::string err;
    if (!udp.bind(&err)) {
        std::printf("  [FAIL] udp-bind :: %s\n", err.c_str());
        return 1;
    }
    g_udpPort = udp.port();
    std::printf("udp-json: bound on 127.0.0.1:%d (real adapter)\n", g_udpPort);
    if (!udp.start([&](const Event& e) { rt.post(e); }, &err)) {
        std::printf("  [FAIL] udp-start :: %s\n", err.c_str());
        return 1;
    }

    // Real Raw Input adapter is also live (feedback filter path active)
    RawInputSource raw("raw");
    if (!raw.start([&](const Event& e) { rt.post(e); }, &err))
        std::printf("  [WARN] raw-input not started (%s)\n", err.c_str());

    // target window + edit brought to the front so HID typing lands here
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"TextDeckLinkE2E";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"TextDeck Link E2E",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 460, 100, nullptr, nullptr, wc.hInstance, nullptr);
    // Foreground acquisition can be denied transiently (focus-steal lock);
    // retry until this window really is the foreground window.
    for (int i = 0; i < 20 && GetForegroundWindow() != hwnd; ++i) {
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        Sleep(100);
    }
    SetFocus(g_edit);
    std::printf("focus-check: foreground=0x%p ourHwnd=0x%p edit=0x%p\n",
                (void*)GetForegroundWindow(), (void*)hwnd, (void*)g_edit);
    Sleep(300);
    SetTimer(hwnd, 1, 500, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    raw.stop();
    udp.stop();
    rt.stop();
    std::printf("\nmap_link_e2e: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
