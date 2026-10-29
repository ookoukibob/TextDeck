// ---------------------------------------------------------------------------
// map_adapters.h -- additional input adapters: XInput gamepad, serial line,
// and UDP JSON. Each implements IInputSource (start/stop/error/cleanup) and
// emits normalized Events into the shared sink. Written from scratch.
// ---------------------------------------------------------------------------
#pragma once

#include "map_core.h"
#include "map_engine.h"   // IInputSource, Event

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace map {

// ---------------------------------------------------------------------------
// XInput gamepad adapter. Polls up to 4 controllers; emits Pressed/Released
// for digital buttons, ValueChanged for thumbsticks (normalized -1..1) and
// triggers (0..1), plus DeviceAdded/DeviceRemoved on connect/disconnect.
// Uses xinput1_4.dll loaded dynamically (no static dependency).
// ---------------------------------------------------------------------------
class XInputSource final : public IInputSource {
public:
    explicit XInputSource(std::string sourcePrefix = "xinput");
    ~XInputSource() override;
    bool start(Sink sink, std::string* err = nullptr) override;
    void stop() override;
    const char* kind() const override { return "xinput"; }
    const char* sourceId() const override;
    int connectedCount() const;
private:
    void pollOnce();
    std::string prefix_;
    Sink sink_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    mutable std::mutex mu_;
    // per-slot last button mask + axis values
    struct Slot { bool present = false; unsigned buttons = 0; float lx=0,ly=0,rx=0,ry=0,lt=0,rt=0; };
    Slot slots_[4];
    void* xinputLib_ = nullptr;      // HMODULE
    int (*getState_)(unsigned, void*) = nullptr; // XInputGetState
};

// ---------------------------------------------------------------------------
// Serial line adapter. Opens a COM port (overlapped), reads newline-terminated
// lines and emits MessageReceived(text). On read failure / device loss it emits
// DeviceRemoved and attempts bounded reconnect. Message size is capped.
// ---------------------------------------------------------------------------
class SerialSource final : public IInputSource {
public:
    SerialSource(std::string sourceId, std::string port, int baud = 115200);
    ~SerialSource() override;
    bool start(Sink sink, std::string* err = nullptr) override;
    void stop() override;
    const char* kind() const override { return "serial"; }
    const char* sourceId() const override;
    bool connected() const;
private:
    void readLoop();
    bool openPort(std::string* err);
    void closePort();
    std::string id_, port_;
    int baud_;
    Sink sink_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    void* handle_ = nullptr;  // HANDLE
    mutable std::mutex mu_;
    bool connected_ = false;
    std::atomic<size_t> dropped_{0};   // oversized / malformed lines dropped
};

// ---------------------------------------------------------------------------
// UDP JSON adapter. Receives datagrams on a port and parses a small JSON
// envelope into Events:
//   {"event":"press","control":"btn_a"}     -> Pressed
//   {"event":"release","control":"btn_a"}   -> Released
//   {"event":"value","control":"axis","value":0.5} -> ValueChanged
//   {"event":"message","text":"..."}        -> MessageReceived
// Malformed / oversized datagrams are dropped with a diagnostic, never crash.
// ---------------------------------------------------------------------------
class UdpJsonSource final : public IInputSource {
public:
    UdpJsonSource(std::string sourceId, int port);
    ~UdpJsonSource() override;
    bool start(Sink sink, std::string* err = nullptr) override;
    void stop() override;
    const char* kind() const override { return "udpjson"; }
    const char* sourceId() const override;
    size_t droppedMessages() const;
    int port() const { return port_; }
    bool bind(std::string* err = nullptr);
    bool recvOne(int timeoutMs);   // parse one datagram -> emit
    void close();
private:
    void threadMain();
    std::string id_;
    int port_;
    Sink sink_;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::atomic<size_t> dropped_{0};
#ifdef _WIN32
    uintptr_t sock_ = 0;
#else
    int sock_ = -1;
#endif
};

} // namespace map
