// ---------------------------------------------------------------------------
// map_queue.h -- bounded event queue + single executor (MappingRuntime).
//
// Real device adapters must not run action scripts on the device message
// thread. The adapter does a minimal decode and posts a normalized Event here;
// the executor thread pops items and runs the rule engine / action scripts in
// order. delay/repeat/long scripts therefore never block device collection.
//
// Thread-safety boundary:
//   * producers (adapters/UI) only call post*/requestCancel (lock-free or short
//     lock); they NEVER touch Engine or KeyOwnership.
//   * the single executor thread is the ONLY caller of Engine::dispatch /
//     cancelAll / cancelSource, so all key-state mutations are single-threaded.
//   * EventQueue is fully thread-safe (mutex + condition_variable).
//
// Priority & overflow:
//   * A separate CONTROL channel (cancel / device-removed) is drained FIRST and
//     is never dropped - stop, disconnect and emergency release always win.
//   * The main FIFO preserves press/release ORDER. Released events are NEVER
//     dropped; when the FIFO is full, the OLDEST non-Released event is dropped
//     (counted + reported) so a key release can never be lost to queue pressure.
// ---------------------------------------------------------------------------
#pragma once

#include "map_core.h"
#include "map_engine.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace map {

enum class QueueItemKind : int {
    Event = 0,
    CancelSource,
    CancelAll,
    DeviceRemoved,
};

// A control request carries the cancellation epoch it belongs to, so the
// executor acknowledges exactly the requests it actually handled.
struct QueueItem {
    QueueItemKind kind = QueueItemKind::Event;
    Event ev;
    std::string source;         // CancelSource / DeviceRemoved
    uint64_t cancelEpoch = 0;   // epoch of the request this control item completes
    // Event items only: the stop epoch in force when the event was posted. An
    // event stamped with a stop that has since been applied belongs to the
    // pre-stop world and is discarded; an unstamped one (0) was posted after the
    // stop completed and runs normally. This is what distinguishes "queued before
    // the emergency stop" from "arrived just after it" without timing guesses.
    uint64_t postedStopEpoch = 0;
};

class EventQueue {
public:
    explicit EventQueue(size_t maxNormal = 2048) : maxNormal_(maxNormal) {}

    // Returns true if the event was accepted without dropping anything; false if
    // an older non-Released event had to be dropped to make room.
    // `postedStopEpoch` stamps the event with the stop in force at post time (0 =
    // none); the executor discards stamped events instead of running them after
    // that stop has been applied.
    bool post(const Event& ev, uint64_t postedStopEpoch = 0);
    void postCancelSource(const std::string& source, uint64_t epoch = 0); // priority, never dropped
    void postCancelAll(uint64_t epoch = 0);                              // priority, never dropped
    void postDeviceRemoved(const std::string& source, uint64_t epoch = 0); // priority, never dropped

    // Blocking pop; processes CONTROL items first. Returns false on timeout or
    // after shutdown().
    bool pop(QueueItem& out, int timeoutMs);
    void shutdown();        // wake all waiters; subsequent pop() returns false
    // Re-arm after shutdown() so a restarted MappingRuntime consumes items again
    // instead of spinning on a permanent "false" from pop().
    void reopen();
    // Forget queued CancelAll/CancelSource/DeviceRemoved items: their requests
    // were acknowledged by the caller, so replaying them would be a double stop.
    size_t dropPendingControl();
    // Discard queued Event items (control requests are kept). This is CancelAll
    // semantics for pending actions: an accepted stop must not be followed by
    // actions that were already in the FIFO when it was requested.
    size_t dropPendingEvents();

    size_t dropped() const;
    size_t pending() const;
    bool empty() const;

private:
    bool isRelease(const Event& e) const { return e.type == EventType::Released; }

    std::deque<QueueItem> fifo_;   // events, order preserved
    std::deque<QueueItem> ctrl_;   // control channel (priority, never dropped)
    size_t maxNormal_;
    size_t dropped_ = 0;
    bool shutdown_ = false;
    mutable std::mutex mu_;
    std::condition_variable cv_;
};

// ---------------------------------------------------------------------------
// MappingRuntime: engine + queue + executor thread. The single owner of Engine.
// ---------------------------------------------------------------------------
class MappingRuntime {
public:
    explicit MappingRuntime(std::shared_ptr<OutputBackend> out, size_t maxQueue = 2048);
    ~MappingRuntime();

    Engine& engine() { return engine_; }
    const Engine& engine() const { return engine_; }

    bool start();   // spawn the executor thread
    void stop();    // queue shutdown + join

    // Producer-safe entry points (adapters call these).
    void post(const Event& ev);
    void postCancelSource(const std::string& source);
    void postCancelAll();
    void postDeviceRemoved(const std::string& source);
    size_t eventsDroppedByCancel() const { return eventsDroppedByCancel_.load(); }

    size_t dropped() const { return queue_.dropped(); }
    size_t pending() const { return queue_.pending(); }
    // True while an accepted emergency stop has not been acknowledged yet.
    bool cancelPending() const { return engine_.cancelPending(); }

    // Test hook: process one queued item synchronously on the calling thread
    // (returns false if nothing was available within timeoutMs). Used by tests
    // that want deterministic single-threaded execution.
    bool processOne(int timeoutMs);

private:
    void loop();
    void handle(const QueueItem& it);

    Engine engine_;
    EventQueue queue_;
    std::thread executor_;
    std::atomic<bool> running_{false};
    // Queued events discarded because an emergency stop was in force when the
    // executor was ready to run them (CancelAll semantics, observable).
    std::atomic<size_t> eventsDroppedByCancel_{0};
};

} // namespace map
