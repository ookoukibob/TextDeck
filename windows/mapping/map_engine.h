// ---------------------------------------------------------------------------
// map_engine.h -- rule engine + action/script model + input-source adapters
// for the TextDeck input-mapping platform.
//
// Declarative JSON profiles (see map_json.h) describe bindings; the engine
// matches normalized events and runs scripts of built-in actions through the
// unified output backend, with refcounted key ownership so multiple sources
// can share a key without premature release.
//
// The concrete adapters (ButtonSource, AnalogSource, TimerSource,
// UdpMessageSource) are header-only so the unit tests and the MVP demo can
// drive them deterministically; the engine itself is implemented in
// map_engine.cpp.
// ---------------------------------------------------------------------------
#pragma once

#include "map_core.h"
#include "map_json.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace map {

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
enum class ActionKind {
    KeyDown = 0, KeyUp, KeyTap, KeyCombo, TypeText, Delay, Repeat,
    Conditional, ProfileSwitch, CancelSequence
};

struct Action {
    ActionKind kind = ActionKind::KeyTap;
    std::string key;                       // KeyDown / KeyUp / KeyTap
    std::vector<std::string> combo;        // KeyCombo
    std::string text;                      // TypeText
    int   delayMs = 0;                     // Delay
    int   count  = 1;                      // Repeat count
    std::string cond;                      // Conditional: "held:<key>" | "above:<src>/<ctl>"
    std::string profile;                   // ProfileSwitch target
    std::vector<Action> block;             // Repeat / Conditional nested steps
};

// ---------------------------------------------------------------------------
// Binding (one rule)
// ---------------------------------------------------------------------------
struct MatchSpec {
    std::string source;   // "" = any source
    std::string control;  // "" = any control
    EventType   type = EventType::Pressed;
    std::string msgText;  // MessageReceived: substring match ("" = any)
};

enum class BindingMode { Momentary = 0, Toggle = 1, Script = 2, Threshold = 3 };

// Full trigger vocabulary. Each kind has a concrete state machine in
// map_engine.cpp; none are decorative.
enum class TriggerKind {
    Press = 0,      // fire on the matching Pressed edge
    Release,        // fire on the matching Released edge
    Momentary,      // hold key while pressed, release on Released
    Toggle,         // each press flips a held key
    Threshold,      // fire when an analog value crosses a threshold (with hysteresis)
    LongPress,      // fire once when held >= longPressMs
    HoldRepeat,     // fire while held every repeatMs (after the first)
    DoubleTap,      // fire on 2 presses within tapWindowMs
    MultiTap,       // fire on N presses within tapWindowMs
    Sequence,       // ordered multi-step (control,edge) match with per-step timeout
    Combo,          // all listed controls held together (cross-device) -> fire once
    Cooldown,       // press, rate-limited to one fire per cooldownMs
};

// One step of a Sequence trigger.
struct SeqStep {
    std::string control;          // control id ("" = any control of the source)
    EventType   type = EventType::Pressed;
};

struct Binding {
    std::string id;
    MatchSpec  match;
    BindingMode mode = BindingMode::Script;
    TriggerKind trigger = TriggerKind::Press;

    std::string holdKey;             // Momentary/Toggle/LongPress-hold key
    std::vector<Action> script;      // run when the trigger fires
    std::vector<Action> onRelease;   // run on Released (optional)

    // Threshold
    double threshold = 0.5;
    double hysteresis = 0.05;
    std::string aboveKey;

    // Timing / conditioning parameters
    int debounceMs  = 0;      // suppress edges closer than this
    int cooldownMs  = 0;      // min gap between fires (Cooldown, and optional global)
    int longPressMs = 500;    // LongPress / HoldRepeat initial delay
    int repeatMs    = 250;    // HoldRepeat interval
    int tapCount    = 2;      // MultiTap N (2 == double)
    int tapWindowMs = 300;    // DoubleTap/MultiTap window
    int seqTimeoutMs = 1000;  // Sequence: max gap between steps
    std::vector<SeqStep> sequence; // Sequence steps
    std::vector<std::string> combo; // Combo: controls that must all be held
};

// Per-binding runtime state for the trigger state machine. Owned by the engine
// and mutated only on the executor thread.
struct TriggerState {
    bool     held = false;
    bool     toggle = false;
    bool     above = false;
    bool     firedLongPress = false;
    int      tapCount = 0;
    uint64_t lastEventTs = 0;
    uint64_t pressTs = 0;
    uint64_t lastTapTs = 0;
    uint64_t lastFireTs = 0;
    uint64_t nextRepeatTs = 0;
    int      seqIndex = 0;
    uint64_t seqLastTs = 0;
    std::map<std::string, bool> comboHeld; // control -> held (for Combo)
};

// ---------------------------------------------------------------------------
// Input-source adapter. Emits normalized events into a sink; supports
// start/stop, error reporting and resource cleanup. A failing adapter must
// never crash the engine.
// ---------------------------------------------------------------------------
class IInputSource {
public:
    using Sink = std::function<void(const Event&)>;
    virtual ~IInputSource() = default;
    virtual bool start(Sink sink, std::string* err = nullptr) = 0;
    virtual void stop() = 0;
    virtual const char* kind() const = 0;
    virtual const char* sourceId() const = 0;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
class Engine {
public:
    explicit Engine(std::shared_ptr<OutputBackend> out);

    void addBinding(const Binding& b);
    bool loadProfile(const JsonPtr& root, std::string* err);
    bool loadProfileText(const std::string& json, std::string* err);
    void activateProfile(const std::string& name);
    const std::string& activeProfile() const { return active_; }

    void dispatch(const Event& e);
    void tick(uint64_t nowMs);   // drives HoldRepeat / LongPress while held

    void cancelSource(const std::string& source); // device disconnect
    void cancelAll();                             // emergency stop
    void requestCancel();                         // async: interrupt a running script
    void updateClock(uint64_t nowMs);
    void setRealtime(bool r) { realtime_ = r; }

    std::vector<std::string> heldKeys() const;
    // Output transitions the backend refused (unknown key / device missing).
    // Observable so callers never mistake a failed send for a successful one.
    size_t outputFailures() const;
    size_t bindingCount() const { return activeBindings_().size(); }
    double lastValue(const std::string& source, const std::string& control) const;

private:
    struct CancelState { bool cancelled = false; };

    void handle(const Binding& b, const Event& e);
    void runScript(const std::vector<Action>& script, const std::string& holder, CancelState& cs);
    void runAction(const Action& a, const std::string& holder, CancelState& cs);
    bool evalCond(const std::string& cond);
    void sleepMs(int ms);
    bool match(const Binding& b, const Event& e) const;
    const std::vector<Binding>& activeBindings_() const;

    std::shared_ptr<OutputBackend> out_;
    std::unordered_map<std::string, std::vector<Binding>> profiles_;
    std::string active_;
    KeyOwnership ownership_;

    struct ControlState {
        double value = 0.0;
        bool   above = false;
        bool   toggle = false;
        uint64_t lastEventTs = 0;
    };
    std::unordered_map<std::string, ControlState> controls_;
    std::unordered_map<std::string, TriggerState> triggerStates_; // key: binding id

    uint64_t now_ = 0;
    uint64_t scriptInstance_ = 0;
    bool realtime_ = false;
    std::atomic<bool> cancelRequested_{false}; // set async; aborts long scripts

    static std::string ctrlKey(const std::string& s, const std::string& c) { return s + "|" + c; }
};

// ---------------------------------------------------------------------------
// Concrete adapters (header-only).
// ---------------------------------------------------------------------------
class ButtonSource final : public IInputSource {
public:
    explicit ButtonSource(std::string id) : id_(std::move(id)) {}
    bool start(Sink sink, std::string* = nullptr) override { sink_ = std::move(sink); return true; }
    void stop() override { sink_ = nullptr; }
    const char* kind() const override { return "button"; }
    const char* sourceId() const override { return id_.c_str(); }
    void press(const std::string& control, uint64_t ts) {
        if (sink_) sink_(Event{ id_, control, EventType::Pressed, 0.0, false, "", ts, "button" });
    }
    void release(const std::string& control, uint64_t ts) {
        if (sink_) sink_(Event{ id_, control, EventType::Released, 0.0, false, "", ts, "button" });
    }
private:
    std::string id_;
    Sink sink_;
};

class AnalogSource final : public IInputSource {
public:
    explicit AnalogSource(std::string id) : id_(std::move(id)) {}
    bool start(Sink sink, std::string* = nullptr) override { sink_ = std::move(sink); return true; }
    void stop() override { sink_ = nullptr; }
    const char* kind() const override { return "analog"; }
    const char* sourceId() const override { return id_.c_str(); }
    void set(const std::string& control, double value, uint64_t ts) {
        if (sink_) sink_(Event{ id_, control, EventType::ValueChanged, value, false, "", ts, "analog" });
    }
private:
    std::string id_;
    Sink sink_;
};

class TimerSource final : public IInputSource {
public:
    TimerSource(std::string id, int intervalMs) : id_(std::move(id)), interval_(intervalMs) {}
    bool start(Sink sink, std::string* err = nullptr) override {
        (void)err;
        sink_ = std::move(sink);
        running_ = true;
        worker_ = std::thread([this] {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_));
                if (running_ && sink_) {
                    sink_(Event{ id_, "tick", EventType::TimerElapsed, 0.0, false, "", 0, "timer" });
                }
            }
        });
        return true;
    }
    void stop() override {
        running_ = false;
        if (worker_.joinable()) worker_.join();
    }
    const char* kind() const override { return "timer"; }
    const char* sourceId() const override { return id_.c_str(); }
    void tick(uint64_t ts) {
        if (sink_) sink_(Event{ id_, "tick", EventType::TimerElapsed, 0.0, false, "", ts, "timer" });
    }
private:
    std::string id_;
    int interval_;
    Sink sink_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

class UdpMessageSource final : public IInputSource {
public:
    UdpMessageSource(std::string id, int port) : id_(std::move(id)), port_(port) {}
    ~UdpMessageSource() override { stop(); }

    bool start(Sink sink, std::string* err = nullptr) override {
        sink_ = std::move(sink);
        if (!bind(err)) return false;
        running_ = true;
        worker_ = std::thread([this] {
            Event e;
            while (running_) {
                if (recvOne(200, e) && running_) sink_(e);
            }
        });
        return true;
    }
    void stop() override {
        running_ = false;
        if (worker_.joinable()) worker_.join();
        close();
    }
    const char* kind() const override { return "udp"; }
    const char* sourceId() const override { return id_.c_str(); }

    bool bind(std::string* err) {
#ifdef _WIN32
        static bool wsa = false;
        if (!wsa) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); wsa = true; }
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_ == INVALID_SOCKET) { if (err) *err = "socket() failed"; return false; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((u_short)port_);
        if (::bind(sock_, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
            if (err) *err = "bind() failed"; closesocket(sock_); sock_ = INVALID_SOCKET; return false;
        }
        if (port_ == 0) { // ephemeral: report the assigned port
            sockaddr_in got{}; int glen = sizeof(got);
            if (getsockname(sock_, (sockaddr*)&got, &glen) == 0) port_ = ntohs(got.sin_port);
        }
        return true;
#else
        if (err) *err = "UDP unsupported on this platform";
        return false;
#endif
    }
    bool recvOne(int timeoutMs, Event& out) {
#ifdef _WIN32
        if (sock_ == INVALID_SOCKET) return false;
        fd_set fds; FD_ZERO(&fds); FD_SET(sock_, &fds);
        timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        int r = select(0, &fds, nullptr, nullptr, &tv);
        if (r <= 0) return false;
        char buf[2048];
        int n = recv(sock_, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return false;
        out = Event{ id_, "msg", EventType::MessageReceived, 0.0, false, std::string(buf, n), 0, "udp" };
        return true;
#else
        (void)timeoutMs; (void)out; return false;
#endif
    }
    void close() {
#ifdef _WIN32
        if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
#endif
    }
#ifdef _WIN32
    SOCKET socket() const { return sock_; }
#else
    uintptr_t socket() const { return 0; }
#endif
    int port() const { return port_; }

private:
    std::string id_;
    int port_;
#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    uintptr_t sock_ = 0;
#endif
    Sink sink_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace map
