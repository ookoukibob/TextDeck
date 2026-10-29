// map_raw_input.cpp -- Windows Raw Input adapter implementation.
// Includes map_raw_input.h FIRST (pulls winsock2 via map_engine.h), then
// <windows.h> -- that ordering avoids the winsock2/windows.h conflict.
#include "map_raw_input.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdio>

namespace map {

// ---------------------------------------------------------------------------
// Pure normalizers (platform-neutral)
// ---------------------------------------------------------------------------
static std::string VkControlName(unsigned short vk) {
    const KeyInfo* ki = FindKeyByVk(vk);
    if (ki && ki->name && ki->name[0]) return ki->name;
    return "vk_" + std::to_string((unsigned)vk);
}

bool NormalizeKeyboard(unsigned short makeCode, unsigned short flags,
                       unsigned short vk, const std::string& source, Event& out) {
    (void)makeCode;
    out.source = source;
    out.control = VkControlName(vk);
    out.type = (flags & kRiKeyBreak) ? EventType::Released : EventType::Pressed;
    out.value = (double)vk;
    out.text.clear();
    out.ts = 0;
    out.sourceKind = "rawinput";
    return true;
}

void NormalizeMouseButtons(unsigned buttonFlags, const std::string& source,
                           std::vector<Event>& out) {
    struct Map { unsigned flag; const char* ctl; bool down; };
    static const Map kMaps[] = {
        { kRiMouseLeftDown,   "mouse_left",   true  }, { kRiMouseLeftUp,   "mouse_left",   false },
        { kRiMouseRightDown,  "mouse_right",  true  }, { kRiMouseRightUp,  "mouse_right",  false },
        { kRiMouseMiddleDown, "mouse_middle", true  }, { kRiMouseMiddleUp, "mouse_middle", false },
        { kRiMouseX1Down,     "mouse_x1",     true  }, { kRiMouseX1Up,     "mouse_x1",     false },
        { kRiMouseX2Down,     "mouse_x2",     true  }, { kRiMouseX2Up,     "mouse_x2",     false },
    };
    for (const Map& m : kMaps) {
        if (buttonFlags & m.flag) {
            Event e;
            e.source = source;
            e.control = m.ctl;
            e.type = m.down ? EventType::Pressed : EventType::Released;
            e.sourceKind = "rawinput";
            out.push_back(e);
        }
    }
}

void NormalizeMouseMove(long dx, long dy, const std::string& source,
                        std::vector<Event>& out) {
    if (dx) {
        Event e; e.source = source; e.control = "mouse_dx";
        e.type = EventType::ValueChanged; e.value = (double)dx; e.sourceKind = "rawinput";
        out.push_back(e);
    }
    if (dy) {
        Event e; e.source = source; e.control = "mouse_dy";
        e.type = EventType::ValueChanged; e.value = (double)dy; e.sourceKind = "rawinput";
        out.push_back(e);
    }
}

// ---------------------------------------------------------------------------
// RawInputSource
// ---------------------------------------------------------------------------
#ifdef _WIN32
static RawInputSource* g_active = nullptr;

static std::string WideToUtf8(const wchar_t* w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string s((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

static LRESULT CALLBACK RawWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_INPUT && g_active) {
        g_active->onRawInput(reinterpret_cast<void*>(lParam));
        // Let the system release the input buffer.
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (msg == WM_INPUT_DEVICE_CHANGE && g_active) {
        // GIDC_ARRIVAL=1, GIDC_REMOVAL=2 (RIDEV_DEVNOTIFY). lParam = device handle.
        std::string path;
        wchar_t name[512]; UINT cb = sizeof(name);
        if (GetRawInputDeviceInfoW((HANDLE)lParam, RIDI_DEVICENAME, name, &cb) > 0)
            path = WideToUtf8(name);
        bool arrival = (wParam == 1);
        Event e;
        e.source = path.empty() ? "raw" : path;
        e.control = "device";
        e.type = arrival ? EventType::DeviceAdded : EventType::DeviceRemoved;
        e.sourceKind = "rawinput";
        if (g_active) g_active->emit(e);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
#endif // _WIN32

RawInputSource::RawInputSource(std::string sourcePrefix) : prefix_(std::move(sourcePrefix)) {}
RawInputSource::~RawInputSource() { stop(); }
const char* RawInputSource::sourceId() const { return prefix_.c_str(); }

bool RawInputSource::start(Sink sink, std::string* err) {
#ifdef _WIN32
    (void)err;
    if (running_) return true;
    sink_ = std::move(sink);
    g_active = this;
    running_ = true;
    worker_ = std::thread([this] { threadMain(); });
    for (int i = 0; i < 400 && threadId_.load() == 0; ++i) Sleep(5);
    return true;
#else
    (void)sink; if (err) *err = "Raw Input unsupported on this platform"; return false;
#endif
}

void RawInputSource::stop() {
#ifdef _WIN32
    if (!running_) return;
    running_ = false;
    if (threadId_.load()) PostThreadMessageW(threadId_.load(), WM_QUIT, 0, 0);
    if (worker_.joinable()) worker_.join();
    hwnd_ = nullptr;
    threadId_ = 0;
    if (g_active == this) g_active = nullptr;
    Sink empty; sink_ = empty;
#else
    running_ = false;
#endif
}

void RawInputSource::addDiagnostic(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    diags_.push_back(s);
}

void RawInputSource::emit(const Event& e) {
    // Feedback-loop prevention: never map our own virtual-HID output back into
    // input. Uses the device VID/PID (structural), not a fuzzy name match.
    if (!e.source.empty() && IsOwnOutputDevice(e.source)) { ++ignoredOwn_; return; }
    Sink s;
    {
        std::lock_guard<std::mutex> lk(mu_);
        s = sink_;
    }
    if (s) s(e);
}

std::string RawInputSource::keyControlName(unsigned short vk) const { return VkControlName(vk); }

// ---- test hooks (same normalize+emit path as live WM_INPUT) ----------------
void RawInputSource::emitKeyboard(bool down, unsigned short vk, const std::string& source) {
    Event e;
    if (NormalizeKeyboard(0, down ? 0 : kRiKeyBreak, vk, source, e)) emit(e);
}
void RawInputSource::emitMouseButton(int button, bool down, const std::string& source) {
    unsigned flag = 0;
    switch (button) {
        case 0: flag = down ? kRiMouseLeftDown   : kRiMouseLeftUp;   break;
        case 1: flag = down ? kRiMouseRightDown  : kRiMouseRightUp;  break;
        case 2: flag = down ? kRiMouseMiddleDown : kRiMouseMiddleUp; break;
        case 3: flag = down ? kRiMouseX1Down     : kRiMouseX1Up;     break;
        case 4: flag = down ? kRiMouseX2Down     : kRiMouseX2Up;     break;
        default: return;
    }
    std::vector<Event> evs;
    NormalizeMouseButtons(flag, source, evs);
    for (auto& e : evs) emit(e);
}
void RawInputSource::emitMouseMove(long dx, long dy, const std::string& source) {
    std::vector<Event> evs;
    NormalizeMouseMove(dx, dy, source, evs);
    for (auto& e : evs) emit(e);
}
void RawInputSource::emitUnknownHid(const std::string& source, unsigned reportBytes) {
    ++undecoded_;
    addDiagnostic("generic HID report from " + source + " (" + std::to_string(reportBytes) +
                  " bytes) not decoded; needs a dedicated adapter");
}

// ---- live path -------------------------------------------------------------
void RawInputSource::onRawInput(void* hRawInput) {
#ifdef _WIN32
    HRAWINPUT h = reinterpret_cast<HRAWINPUT>(hRawInput);
    UINT size = 0;
    GetRawInputData(h, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
    if (size == 0) return;
    std::vector<BYTE> buf(size);
    if (GetRawInputData(h, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) != size) return;
    RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(buf.data());

    // Stable source id = device PATH (VID/PID/instance), never the raw handle.
    std::string path;
    {
        wchar_t name[512]; UINT cb = sizeof(name);
        if (GetRawInputDeviceInfoW(ri->header.hDevice, RIDI_DEVICENAME, name, &cb) > 0)
            path = WideToUtf8(name);
    }
    std::string src = path.empty() ? prefix_ : path;

    // Record the raw identity of this event for the physical-vs-synthetic
    // audit: the raw hDevice, the resolved path, and whether it was injected
    // (hDevice == NULL => SendInput/keybd_event origin).
    {
        bool injected = (ri->header.hDevice == NULL);
        bool realDev = (!injected && !path.empty());
        char hb[32]; snprintf(hb, sizeof(hb), "0x%p", (void*)ri->header.hDevice);
        std::string detail = (ri->header.dwType == RIM_TYPEKEYBOARD)
            ? ("KEY " + std::string((ri->data.keyboard.Flags & RI_KEY_BREAK) ? "UP" : "DOWN") +
               " vk=0x" + std::to_string(ri->data.keyboard.VKey))
            : (ri->header.dwType == RIM_TYPEMOUSE ? "MOUSE" : "HID");
        std::string line = std::string("hDevice=") + hb + " path=" + (path.empty() ? "(none)" : path) +
            " injected=" + std::to_string(injected) + " realDevice=" + std::to_string(realDev) +
            " | " + detail;
        std::lock_guard<std::mutex> lk(mu_);
        captures_.push_back(line);
        // Bounded diagnostic buffer: mouse moves arrive at high rate and must
        // not grow memory without limit in a long-running session.
        if (captures_.size() > 512) captures_.erase(captures_.begin());
    }

    if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        Event e;
        if (NormalizeKeyboard(ri->data.keyboard.MakeCode, ri->data.keyboard.Flags,
                              ri->data.keyboard.VKey, src, e)) emit(e);
    } else if (ri->header.dwType == RIM_TYPEMOUSE) {
        std::vector<Event> evs;
        NormalizeMouseButtons(ri->data.mouse.usButtonFlags, src, evs);
        NormalizeMouseMove(ri->data.mouse.lLastX, ri->data.mouse.lLastY, src, evs);
        for (auto& e : evs) emit(e);
    } else if (ri->header.dwType == RIM_TYPEHID) {
        unsigned bytes = ri->data.hid.dwSizeHid * ri->data.hid.dwCount;
        emitUnknownHid(src, bytes);
    }
#endif
}

void RawInputSource::enumerateDevices() {
#ifdef _WIN32
    UINT n = 0;
    GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST));
    std::vector<RAWINPUTDEVICELIST> list(n);
    UINT got = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
    std::lock_guard<std::mutex> lk(mu_);
    devs_.clear();
    for (UINT i = 0; i < got; ++i) {
        RawDeviceInfo d;
        wchar_t name[512]; UINT cb = sizeof(name);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICENAME, name, &cb) > 0)
            d.path = WideToUtf8(name);
        ParseVidPid(d.path, d.vid, d.pid);
        RID_DEVICE_INFO info{}; info.cbSize = sizeof(info); UINT icb = sizeof(info);
        if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &icb) > 0) {
            if (info.dwType == RIM_TYPEKEYBOARD) { d.type = RawDevType::Keyboard; d.usagePage = 0x01; d.usage = 0x06; }
            else if (info.dwType == RIM_TYPEMOUSE) { d.type = RawDevType::Mouse; d.usagePage = 0x01; d.usage = 0x02; }
            else if (info.dwType == RIM_TYPEHID) { d.type = RawDevType::Hid; d.usagePage = info.hid.usUsagePage; d.usage = info.hid.usUsage; }
        }
        d.ownOutput = IsOwnOutputDevice(d.path);
        devs_.push_back(d);
    }
#endif
}

void RawInputSource::threadMain() {
#ifdef _WIN32
    threadId_ = GetCurrentThreadId();
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &RawWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TextDeckRawInputWnd";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, wc.hInstance, this);
    hwnd_ = hwnd;
    RAWINPUTDEVICE rid[4] = {};
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x06; rid[0].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY; rid[0].hwndTarget = hwnd;
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x02; rid[1].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY; rid[1].hwndTarget = hwnd;
    rid[2].usUsagePage = 0x01; rid[2].usUsage = 0x05; rid[2].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY; rid[2].hwndTarget = hwnd;
    rid[3].usUsagePage = 0x01; rid[3].usUsage = 0x04; rid[3].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY; rid[3].hwndTarget = hwnd;
    RegisterRawInputDevices(rid, 4, sizeof(RAWINPUTDEVICE));
    enumerateDevices();

    MSG msg;
    while (running_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
#endif
}

// ---- accessors -------------------------------------------------------------
std::vector<RawDeviceInfo> RawInputSource::devices() const {
    std::lock_guard<std::mutex> lk(mu_);
    return devs_;
}
std::vector<std::string> RawInputSource::diagnostics() const {
    std::lock_guard<std::mutex> lk(mu_);
    return diags_;
}
std::vector<std::string> RawInputSource::captureLog() const {
    std::lock_guard<std::mutex> lk(mu_);
    return captures_;
}
size_t RawInputSource::ignoredOwnOutputCount() const { return ignoredOwn_; }
size_t RawInputSource::undecodedReportCount() const { return undecoded_; }

} // namespace map
