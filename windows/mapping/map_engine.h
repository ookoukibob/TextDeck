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
#include <mutex>
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

    void cancelSource(const std::string& source, uint64_t requestEpoch = 0); // device disconnect
    void cancelAll(uint64_t requestEpoch = 0);                              // emergency stop
    // Split forms, for callers that must order cleanup before making the stop
    // externally visible. releaseAllKeys() releases every key and clears trigger
    // latches but acknowledges nothing; acknowledgeStop(epoch) then publishes
    // completion. The executor uses this so the FIFO barrier sits between them.
    void releaseAllKeys();
    void acknowledgeStop(uint64_t epoch);
    // Async, any thread. requestCancel() interrupts a running script (used by a
    // single device disconnect); requestStop() also forbids any NEW action from
    // starting (emergency stop). Both return the epoch that identifies the
    // request, so the executor can acknowledge exactly it and not a later one.
    uint64_t requestCancel();
    uint64_t requestStop();
    // Test-only observation hook, empty in production. Invoked from requestStop()
    // while it holds cancelTrackMu_, i.e. at the exact point where the epoch has
    // been allocated and its Stop scope registered but NOT yet published. A test
    // uses it to run a concurrent source cleanup inside that window and prove it
    // cannot acknowledge the Stop. It must not call back into the engine.
    void setStopPublicationHook(std::function<void(uint64_t)> fn) { stopPublishHook_ = std::move(fn); }
    // True while an accepted request has not been acknowledged yet. Producers may
    // poll these (read-only); only cancelAll()/cancelSource() acknowledge.
    bool cancelPending() const;
    bool stopPending() const;
    // True once THIS request has been fully handled, so a queued control item
    // whose stop already ran is not replayed (no double stop).
    //
    // Requests are tracked, not merely counted: a single "highest acknowledged
    // epoch" watermark cannot answer this. A newer request (a per-source cancel,
    // epoch 11) can be acknowledged while an older stop (epoch 10) is still
    // unhandled, and a watermark would then claim 10 was handled too, unblocking
    // dispatch while the global emergency release had never run. A request
    // acknowledged out of order therefore stays unhandled until its own cleanup.
    bool cancelAcknowledged(uint64_t epoch) const;
    // Newest stop epoch. Producers stamp queued events with it, so the executor
    // can tell an event that predates a stop from one posted after it instead of
    // guessing from arrival order. Returns 0 when no stop is outstanding, so a
    // completed stop never stamps (and so never discards) later input.
    uint64_t pendingStopEpoch() const;
    void updateClock(uint64_t nowMs);
    void setRealtime(bool r) { realtime_ = r; }

    std::vector<std::string> heldKeys() const;
    // Keys whose physical KeyUp the backend refused. They stay tracked (see
    // KeyOwnership::unresolvedKeys) so an emergency sweep can report and retry
    // them instead of silently forgetting a key that may still be down.
    std::vector<std::string> unresolvedKeyReleases() const;
    size_t unresolvedReleaseCount() const { return ownership_.unresolvedCount(); }
    // Retry unresolved physical releases once; returns how many were resolved.
    size_t retryUnresolvedReleases();
    // Output transitions the backend refused (unknown key / device missing).
    // Observable so callers never mistake a failed send for a successful one.
    size_t outputFailures() const;
    // Text injections the backend refused to complete for a reason other than
    // cancellation. Kept apart from a cancelled run, which is an expected stop
    // and not an error.
    size_t textFailures() const { return textFailures_; }
    size_t bindingCount() const { return activeBindings_().size(); }
    double lastValue(const std::string& source, const std::string& control) const;

private:
    struct CancelState { bool cancelled = false; };

    // ---- cancellation state machine ---------------------------------------
    // Two independent things must be interruptible, and conflating them lost
    // key releases:
    //
    //   cancel  - "abort the script that is running". Interrupts sleeps, text
    //             injection and nested blocks. A device disconnect raises this,
    //             because only a running script's keys need dropping.
    //   stop    - "no new action may begin". The emergency stop raises this.
    //
    // Both are versions of one monotonic request counter (epoch). A request is
    // acknowledged when the executor has done its cleanup for that epoch:
    // cancelAll() for a stop, cancelSource()/cancelAll() for a cancel. Dispatch
    // and every action gate read the epoch, never a flag the dispatcher can
    // clear, so a request arriving during dispatch initialization is honoured.
    void acknowledgeCancel(uint64_t epoch);
    // Acknowledge every request up to and including `epoch`. Only a GLOBAL
    // cleanup (cancelAll: release every key, clear every trigger) may do this,
    // because its work is a superset of what those requests needed. A scoped
    // cancelSource must use acknowledgeSourceCleanup() instead.
    void acknowledgeCancelThrough(uint64_t epoch);
    // Acknowledge the requests a SOURCE-SCOPED cleanup actually covers: every
    // ordinary cancellation at or below `coveredEpoch`, but NEVER a global Stop,
    // even when the caller reached this path through the default-argument form.
    // A source cleanup releases only that source's keys, so it cannot satisfy a
    // request that demands a global release.
    void acknowledgeSourceCleanup(uint64_t coveredEpoch);
    // True when this epoch was published by requestStop() (global scope).
    // Caller must hold cancelTrackMu_.
    bool isStopLocked(uint64_t epoch) const;
    // True when this epoch is covered by the in-order watermark or was recorded
    // out of order. Caller must hold cancelTrackMu_.
    bool ackCoveredLocked(uint64_t epoch) const;
    // Advance the in-order watermark over every contiguously acknowledged request.
    // Caller must hold cancelTrackMu_.
    void foldAckedLocked();
    bool runBlock(const std::vector<Action>& block, const std::string& holder, CancelState& cs);

    void handle(const Binding& b, const Event& e);
    void runScript(const std::vector<Action>& script, const std::string& holder, CancelState& cs);
    void runAction(const Action& a, const std::string& holder, CancelState& cs);
    void releaseCancelKeys(const std::string& holder, CancelState& cs);
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
    // Async cancellation handshake. See the state-machine note above.
    //   nextEpoch_    the last epoch HANDED OUT (guarded by cancelTrackMu_)
    //   cancelEpoch_  the newest epoch PUBLISHED as accepted. Publishing may lag
    //                 allocation by one request: requestStop() takes an epoch under
    //                 the lock, registers its Stop scope, and only then publishes.
    //                 Readers therefore never observe an epoch whose scope is not
    //                 yet recorded.
    //   cancelAcked_  every request up to here has been handled, in order. Requests
    //                 handled out of order are NOT folded into it (see
    //                 ackOutOfOrder_) because doing so would also "acknowledge"
    //                 older requests that were never cleaned up.
    //   stopEpoch_    the newest request that also forbids new actions
    std::atomic<uint64_t> cancelEpoch_{0};
    std::atomic<uint64_t> cancelAcked_{0};
    std::atomic<uint64_t> stopEpoch_{0};
    // Guards the composite state transitions (epoch allocation, scope
    // registration, Stop publication, acknowledgement). Being atomic is not enough
    // for those: "allocate + register + publish" and "snapshot + classify" must each
    // be indivisible with respect to the other.
    mutable std::mutex cancelTrackMu_;
    uint64_t nextEpoch_ = 0;             // guarded by cancelTrackMu_
    // Epochs of requests cleaned up while an older request was still unhandled.
    // Consulted by cancelAcknowledged()/stopPending(); superseded by cancelAcked_
    // once the in-order watermark catches up. Guarded by cancelTrackMu_.
    std::vector<uint64_t> ackOutOfOrder_;
    // Epochs published by requestStop(). A source-scoped cleanup must never
    // acknowledge one of these: releasing a single source's keys is not the global
    // release a Stop demands. Kept as a sorted set because several stops can be
    // outstanding at once and producers may publish concurrently. Guarded by
    // cancelTrackMu_; entries at or below cancelAcked_ are pruned.
    std::vector<uint64_t> stopRequests_;
    // Kept for diagnostics; never the decision variable (that is the epoch logic).
    std::atomic<bool> cancelRequested_{false};
    // Test-only observation hook for the publication window. Never set in
    // production, so it does not affect the production path's semantics.
    std::function<void(uint64_t)> stopPublishHook_;
    size_t textFailures_ = 0;   // executor-thread only, like the rest of the state

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
