// map_adapters.cpp -- XInput / Serial / UDP-JSON adapter implementations.
#include "map_adapters.h"
#include "map_json.h"

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <chrono>
#include <cstdio>
#include <cstring>

namespace map {

#ifdef _WIN32
static std::string WideToUtf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
#endif

// ===========================================================================
// XInput
// ===========================================================================
#ifdef _WIN32
namespace {
#pragma pack(push, 1)
struct XGamepad { unsigned short wButtons; unsigned char bLeftTrigger, bRightTrigger;
                  short sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XState { unsigned long dwPacketNumber; XGamepad Gamepad; };
#pragma pack(pop)
const unsigned long kDeviceNotConnected = 1167;
const unsigned kBtnA=0x1000,kBtnB=0x2000,kBtnX=0x4000,kBtnY=0x8000,
               kDpadUp=0x0001,kDpadDown=0x0002,kDpadLeft=0x0004,kDpadRight=0x0008,
               kStart=0x0010,kBack=0x0020,kLThumb=0x0040,kRThumb=0x0080,
               kLShoulder=0x0100,kRShoulder=0x0200;
struct BtnMap { unsigned bit; const char* name; };
const BtnMap kBtns[] = {
    {kDpadUp,"dpad_up"},{kDpadDown,"dpad_down"},{kDpadLeft,"dpad_left"},{kDpadRight,"dpad_right"},
    {kStart,"start"},{kBack,"back"},{kLThumb,"l3"},{kRThumb,"r3"},
    {kLShoulder,"lb"},{kRShoulder,"rb"},{kBtnA,"a"},{kBtnB,"b"},{kBtnX,"x"},{kBtnY,"y"},
};
} // namespace
#endif

XInputSource::XInputSource(std::string p) : prefix_(std::move(p)) {}
XInputSource::~XInputSource() { stop(); }
const char* XInputSource::sourceId() const { return prefix_.c_str(); }
int XInputSource::connectedCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0; for (auto& s : slots_) if (s.present) ++n; return n;
}

bool XInputSource::start(Sink sink, std::string* err) {
#ifdef _WIN32
    if (running_) return true;
    sink_ = std::move(sink);
    xinputLib_ = (void*)LoadLibraryW(L"xinput1_4.dll");
    if (!xinputLib_) xinputLib_ = (void*)LoadLibraryW(L"xinput9_1_0.dll");
    if (!xinputLib_) { if (err) *err = "xinput dll not found"; return false; }
    getState_ = (int(*)(unsigned, void*))GetProcAddress((HMODULE)xinputLib_, "XInputGetState");
    if (!getState_) { if (err) *err = "XInputGetState not found"; return false; }
    running_ = true;
    worker_ = std::thread([this] {
        while (running_) { pollOnce(); std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
    });
    return true;
#else
    (void)sink; if (err) *err = "XInput unsupported on this platform"; return false;
#endif
}

void XInputSource::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
#ifdef _WIN32
    if (xinputLib_) { FreeLibrary((HMODULE)xinputLib_); xinputLib_ = nullptr; }
#endif
    sink_ = nullptr;
}

void XInputSource::pollOnce() {
#ifdef _WIN32
    if (!getState_) return;
    for (int i = 0; i < 4; ++i) {
        XState st{};
        unsigned long r = (unsigned long)getState_((unsigned)i, &st);
        std::lock_guard<std::mutex> lk(mu_);
        Slot& sl = slots_[i];
        std::string src = prefix_ + ":" + std::to_string(i);
        if (r == kDeviceNotConnected) {
            if (sl.present) {
                sl.present = false;
                if (sink_) sink_(Event{src, "device", EventType::DeviceRemoved, 0, false, "", 0, "xinput"});
            }
            continue;
        }
        if (!sl.present) {
            sl.present = true;
            if (sink_) sink_(Event{src, "device", EventType::DeviceAdded, 0, false, "", 0, "xinput"});
        }
        XGamepad& g = st.Gamepad;
        // buttons: emit edges
        for (auto& bm : kBtns) {
            bool now = (g.wButtons & bm.bit) != 0;
            bool was = (sl.buttons & bm.bit) != 0;
            if (now != was && sink_) {
                sink_(Event{src, bm.name, now ? EventType::Pressed : EventType::Released,
                            0, false, "", 0, "xinput"});
            }
        }
        sl.buttons = g.wButtons;
        auto emitAxis = [&](const char* ctl, float now, float& prev) {
            if (now != prev && sink_) sink_(Event{src, ctl, EventType::ValueChanged, now, false, "", 0, "xinput"});
            prev = now;
        };
        emitAxis("lx", g.sThumbLX / 32767.0f, sl.lx);
        emitAxis("ly", g.sThumbLY / 32767.0f, sl.ly);
        emitAxis("rx", g.sThumbRX / 32767.0f, sl.rx);
        emitAxis("ry", g.sThumbRY / 32767.0f, sl.ry);
        emitAxis("lt", g.bLeftTrigger / 255.0f, sl.lt);
        emitAxis("rt", g.bRightTrigger / 255.0f, sl.rt);
    }
#endif
}

// ===========================================================================
// Serial
// ===========================================================================
SerialSource::SerialSource(std::string id, std::string port, int baud)
    : id_(std::move(id)), port_(std::move(port)), baud_(baud) {}
SerialSource::~SerialSource() { stop(); }
const char* SerialSource::sourceId() const { return id_.c_str(); }
bool SerialSource::connected() const { std::lock_guard<std::mutex> lk(mu_); return connected_; }

bool SerialSource::openPort(std::string* err) {
#ifdef _WIN32
    std::wstring path;
    if (port_.rfind("\\\\.\\", 0) == 0) {
        int n = MultiByteToWideChar(CP_UTF8, 0, port_.c_str(), -1, nullptr, 0);
        path.assign((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, port_.c_str(), -1, &path[0], n);
    } else {
        path = L"\\\\.\\" + std::wstring(port_.begin(), port_.end());
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = "open " + port_ + " failed"; return false; }
    DCB dcb{}; dcb.DCBlength = sizeof(dcb);
    GetCommState(h, &dcb);
    dcb.BaudRate = (DWORD)baud_; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); if (err) *err = "SetCommState failed"; return false; }
    SetupComm(h, 4096, 4096);
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 50; to.ReadTotalTimeoutConstant = 50; to.ReadTotalTimeoutMultiplier = 10;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);
    handle_ = (void*)h;
    std::lock_guard<std::mutex> lk(mu_); connected_ = true;
    return true;
#else
    (void)err; return false;
#endif
}
void SerialSource::closePort() {
#ifdef _WIN32
    if (handle_) { CloseHandle((HANDLE)handle_); handle_ = nullptr; }
    std::lock_guard<std::mutex> lk(mu_); connected_ = false;
#endif
}

bool SerialSource::start(Sink sink, std::string* err) {
    if (running_) return true;
    sink_ = std::move(sink);
    if (!openPort(err)) return false;
    running_ = true;
    worker_ = std::thread([this] { readLoop(); });
    return true;
}
void SerialSource::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    closePort();
    sink_ = nullptr;
}

void SerialSource::readLoop() {
#ifdef _WIN32
    const size_t kMaxLine = 4096;
    std::string acc;
    bool wasConnected = false;
    while (running_) {
        HANDLE h = (HANDLE)handle_;
        if (!h) {
            std::string e2;
            if (!openPort(&e2)) { std::this_thread::sleep_for(std::chrono::milliseconds(500)); continue; }
            h = (HANDLE)handle_;
            if (!wasConnected) {
                wasConnected = true;
                if (sink_) sink_(Event{id_, "device", EventType::DeviceAdded, 0, false, "", 0, "serial"});
            }
        }
        char buf[256]; DWORD n = 0;
        OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL ok = ReadFile(h, buf, sizeof(buf), &n, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                WaitForSingleObject(ov.hEvent, 200);
                ok = GetOverlappedResult(h, &ov, &n, FALSE);
            } else {
                ok = FALSE; // hard failure: device gone / handle dead
            }
        }
        CloseHandle(ov.hEvent);
        if (!running_) break;
        if (!ok) {
            // Hard read failure (unplugged adapter, dead handle): report the
            // disconnect and drop the handle so the loop reconnects instead of
            // spinning on a broken HANDLE forever.
            closePort();
            acc.clear();
            if (wasConnected) {
                wasConnected = false;
                if (sink_) sink_(Event{id_, "device", EventType::DeviceRemoved, 0, false, "", 0, "serial"});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        if (n == 0) continue;
        acc.append(buf, n);
        // split lines; cap line length
        size_t pos;
        while ((pos = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            acc.erase(0, pos + 1);
            if (line.size() > kMaxLine) { ++dropped_; continue; }
            if (sink_ && !line.empty())
                sink_(Event{id_, "line", EventType::MessageReceived, 0, false, line, 0, "serial"});
        }
        if (acc.size() > kMaxLine) { acc.clear(); ++dropped_; }
    }
#endif
}

// ===========================================================================
// UDP JSON
// ===========================================================================
UdpJsonSource::UdpJsonSource(std::string id, int port) : id_(std::move(id)), port_(port) {}
UdpJsonSource::~UdpJsonSource() { stop(); }
const char* UdpJsonSource::sourceId() const { return id_.c_str(); }
size_t UdpJsonSource::droppedMessages() const { return dropped_.load(); }

bool UdpJsonSource::bind(std::string* err) {
#ifdef _WIN32
    static bool wsa = false; if (!wsa) { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); wsa = true; }
    sock_ = (uintptr_t)::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == 0 || sock_ == (uintptr_t)INVALID_SOCKET) { sock_ = 0; if (err) *err = "socket failed"; return false; }
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)port_);
    if (::bind((SOCKET)sock_, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        if (err) *err = "bind failed"; closesocket((SOCKET)sock_); sock_ = 0; return false;
    }
    if (port_ == 0) { // ephemeral: report the assigned port
        sockaddr_in got{}; int glen = sizeof(got);
        if (getsockname((SOCKET)sock_, (sockaddr*)&got, &glen) == 0) port_ = ntohs(got.sin_port);
    }
    return true;
#else
    (void)err; return false;
#endif
}
void UdpJsonSource::close() {
#ifdef _WIN32
    if (sock_ && sock_ != (uintptr_t)INVALID_SOCKET) { closesocket((SOCKET)sock_); }
    sock_ = 0;
#endif
}

bool UdpJsonSource::recvOne(int timeoutMs) {
#ifdef _WIN32
    if (!sock_) return false;
    fd_set fds; FD_ZERO(&fds); FD_SET((SOCKET)sock_, &fds);
    timeval tv{ timeoutMs/1000, (timeoutMs%1000)*1000 };
    if (select(0, &fds, nullptr, nullptr, &tv) <= 0) return false;
    char buf[4096];
    int n = recv((SOCKET)sock_, buf, sizeof(buf)-1, 0);
    if (n <= 0) return false;
    if (n >= 4000) { ++dropped_; return false; }  // oversized
    buf[n] = 0;
    std::string err;
    JsonPtr j = ParseJson(std::string(buf, n), &err);
    if (!j || !j->isObject()) { ++dropped_; return false; }
    std::string ev = j->get("event")->s();
    std::string ctl = j->get("control")->s();
    double val = j->get("value")->n();
    std::string text = j->get("text")->s();
    if (sink_) {
        if (ev == "press") sink_(Event{id_, ctl, EventType::Pressed, 0, false, "", 0, "udpjson"});
        else if (ev == "release") sink_(Event{id_, ctl, EventType::Released, 0, false, "", 0, "udpjson"});
        else if (ev == "value") sink_(Event{id_, ctl, EventType::ValueChanged, val, false, "", 0, "udpjson"});
        else if (ev == "message") sink_(Event{id_, "msg", EventType::MessageReceived, 0, false, text, 0, "udpjson"});
        else { ++dropped_; return false; }
    }
    return true;
#else
    (void)timeoutMs; return false;
#endif
}

bool UdpJsonSource::start(Sink sink, std::string* err) {
    if (running_) return true;
    sink_ = std::move(sink);
    // bind() is also a public API (tests use it to learn the ephemeral port);
    // do not re-bind an already-open socket.
    if (!sock_ && !bind(err)) return false;
    running_ = true;
    worker_ = std::thread([this] { threadMain(); });
    return true;
}
void UdpJsonSource::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    close();
    sink_ = nullptr;
}
void UdpJsonSource::threadMain() {
    while (running_) recvOne(100);
}

} // namespace map
