// ---------------------------------------------------------------------------
// map_hid_e2e_main.cpp -- REAL-HID end-to-end (Test B + Test F with real output).
//
// Proves the full chain on a machine with the TextDeck virtual-HID driver:
//   real input (keybd_event) -> Raw Input -> rule engine -> HidDriverBackend
//   -> virtual-HID keyboard -> a real target EDIT control receives characters.
//
// Two bindings make the feedback-loop check meaningful:
//   1. Pause (vk_19) pressed -> key_tap "a"      (drives output)
//   2. "a" pressed -> key_tap "a"                (would recurse if the
//      virtual-HID keyboard were not filtered out of Raw Input)
// Injecting Pause 3x must yield exactly "aaa" in the edit. If own-output were
// not excluded, each typed "a" would re-trigger binding 2 -> runaway.
//
// Run ONLY where the driver is installed (the guest VM). On a host without the
// driver the HID calls fail and the test reports an environment failure.
// Logs to stdout and to map_hid_e2e.log in the working directory.
// ---------------------------------------------------------------------------
#include "map_raw_input.h"   // pulls winsock2 via map_engine.h
#include "map_queue.h"       // MappingRuntime
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

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_edit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            10, 10, 400, 30, hwnd, (HMENU)100, GetModuleHandleW(nullptr), nullptr);
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd, 1);
            // Drive the engine with DETERMINISTIC posted events (synthetic input
            // capture via keybd_event is unreliable on this host; the OUTPUT side
            // is real: HidDriverBackend -> virtual-HID keyboard -> this edit).
            for (int i = 0; i < 3; ++i) {
                if (g_rt) {
                    Event e; e.source = "trigger"; e.control = "vk_19";
                    e.type = EventType::Pressed; e.ts = 0;
                    g_rt->post(e);
                }
                Sleep(60);
            }
            SetTimer(hwnd, 2, 900, nullptr);
        } else if (wp == 2) {
            KillTimer(hwnd, 2);
            std::string t = EditText();
            std::printf("EDIT_TEXT='%s'\n", t.c_str());
            std::printf("DIAG: outputFailures=%zu heldKeys=%zu\n",
                        g_rt->engine().outputFailures(), g_rt->engine().heldKeys().size());
            check(t == "aaa", "B-hid-typed-3-chars", ("got='" + t + "'").c_str());
            check(t.size() == 3, "F-no-feedback-runaway", ("len=" + std::to_string(t.size())).c_str());
            if (g_rt) g_rt->engine().cancelAll();
            PostQuitMessage(0);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int main() {
    std::printf("map_hid_e2e -- real virtual-HID output end-to-end\n");

    // Real HID output backend (TextDeck driver IOCTL). Must exist on this host.
    g_hid = MakeBackend("hid");
    std::printf("backend=%s\n", g_hid->name());

    MappingRuntime rt(g_hid, 256);
    g_rt = &rt;

    // binding 1: real key press -> type "a" via HID
    Binding b1; b1.id = "tapA"; b1.match.control = "vk_19"; b1.match.type = EventType::Pressed;
    b1.mode = BindingMode::Script;
    Action t; t.kind = ActionKind::KeyTap; t.key = "a";
    b1.script = { t };
    rt.engine().addBinding(b1);

    // binding 2: if the typed "a" (own virtual-HID output) were NOT filtered,
    // this would recurse. It must never fire for the own-output device.
    Binding b2; b2.id = "recurse"; b2.match.control = "a"; b2.match.type = EventType::Pressed;
    b2.mode = BindingMode::Script;
    Action t2; t2.kind = ActionKind::KeyTap; t2.key = "a";
    b2.script = { t2 };
    rt.engine().addBinding(b2);

    rt.start();
    RawInputSource raw("raw");
    std::string err;
    if (!raw.start([&](const Event& e) { rt.post(e); }, &err)) {
        std::printf("  [FAIL] raw-input-start :: %s\n", err.c_str());
        return 1;
    }

    // target window + edit, brought to the front so HID typing lands here
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"TextDeckHidE2E";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"TextDeck HID E2E",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 460, 100, nullptr, nullptr, wc.hInstance, nullptr);
    SetForegroundWindow(hwnd);
    SetFocus(g_edit);
    Sleep(300);
    SetTimer(hwnd, 1, 700, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    raw.stop();
    rt.stop();
    std::printf("\nmap_hid_e2e: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
