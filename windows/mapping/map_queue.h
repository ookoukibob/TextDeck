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
#include <functional>
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
    // Event items only: the stop epoch in force when the event was posted (0 =
    // none). Kept for diagnostics and for callers that want to reason about which
    // stop an event belongs to. It is NOT the decision variable: the authoritative
    // pre-stop/post-stop test is the enqueue sequence compared against the barrier
    // applyStopBarrier() takes, because a stamp read from engine state can be stale
    // by the time the event is actually enqueued.
    uint64_t postedStopEpoch = 0;
    // Enqueue sequence, assigned by EventQueue::post() under the queue mutex.
    // Compared against the barrier taken by applyStopBarrier() to decide, without
    // any engine-state read, whether this event predates a stop.
    uint64_t seq = 0;
};

class EventQueue {
public:
    explicit EventQueue(size_t maxNormal = 2048) : maxNormal_(maxNormal) {}

    // Returns true if the event was accepted without dropping anything; false if
    // an older non-Released event had to be dropped to make room.
    // `postedStopEpoch` is recorded for diagnostics only; see QueueItem.
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
    // Discard queued Event items (control requests are kept). Used only when the
    // runtime is being torn down, where everything really is pre-stop.
    size_t dropPendingEvents();
    // CancelAll semantics for pending actions, with an unambiguous linearization
    // point. Under the queue mutex this takes the current enqueue head as a
    // barrier and erases every event enqueued at or before it; events enqueued
    // afterwards keep their place. Returns the number erased.
    //
    // Because post() assigns its sequence number under the same mutex, an event
    // is either wholly before the barrier (pre-stop, discarded) or wholly after
    // it (posted while the stop was in progress, or after it). There is no window
    // in which a not-yet-enqueued event can be mistaken for pre-stop work, and no
    // read of engine state is involved in deciding that.
    size_t applyStopBarrier();

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
    uint64_t postSeq_ = 0;         // last assigned enqueue sequence (guarded by mu_)
    mutable std::mutex mu_;
    std::condition_variable cv_;
};

// ---------------------------------------------------------------------------
// MappingRuntime: engine + queue + executor thread. The single owner of Engine.
//
// Lifecycle invariant (see map_queue.cpp):
//   engine_ may be touched by a producer thread ONLY while no executor thread
//   exists or can start. That is enforced by `lifeMu_`, which is held for the
//   whole stop() / start() transition, and which a producer must hold before it
//   takes the "no executor is running, do the cleanup inline" path. Producers
//   never hold it while blocking on output; the executor never takes it at all.
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
    // True while an executor thread exists (including while stop() is joining it).
    bool executorRunning() const { return running_.load(); }

    // Test hook: process one queued item synchronously on the calling thread
    // (returns false if nothing was available within timeoutMs). Used by tests
    // that want deterministic single-threaded execution.
    bool processOne(int timeoutMs);
    // Test hooks for the stop's linearization point, so a test can drive the
    // three ordered steps (release, barrier, acknowledge) by hand and inject an
    // event at the exact boundary the concurrency fix is about.
    size_t queueApplyStopBarrier() { return queue_.applyStopBarrier(); }
    // Number of times a producer took the "no executor, mutate Engine inline"
    // path. The lifecycle invariant is that this can never happen while
    // executorRunning() is true; a test asserts it stays at zero across a
    // concurrent stop/join window.
    size_t inlineCleanups() const { return inlineCleanups_.load(); }
    // Test-only: observes the state DURING stop(), after the stop was requested
    // and before join(). A producer that runs its stop request here must be
    // unable to take the inline Engine path, because an executor still exists.
    void setOnStopJoining(std::function<void()> fn) { onStopJoining_ = std::move(fn); }
    // Invoked immediately AFTER the stop becomes visible as complete (the
    // acknowledgement), on the executor thread. A test posts an event here to
    // prove that whatever a producer does once it can see completion is never
    // destroyed by a cleanup step that runs later.
    void setOnStopAcknowledged(std::function<void()> fn) { onStopAcked_ = std::move(fn); }
    // Invoked after key release and BEFORE the queue barrier, with the state the
    // stop would have if it were already acknowledged. A correct implementation
    // must still report the stop as pending here; that is what proves the stop is
    // not acknowledged before the FIFO is cleaned.
    void setOnBeforeBarrier(std::function<void()> fn) { onBeforeBarrier_ = std::move(fn); }

private:
    void loop();
    void handle(const QueueItem& it);
    // Apply a stop's FIFO cleanup and acknowledgement as one queue-ordered step.
    void applyStopLocked(uint64_t epoch);

    Engine engine_;
    EventQueue queue_;
    std::thread executor_;
    // Two distinct facts, deliberately not one flag:
    //   stopRequested_  the executor loop must exit (set before join)
    //   running_        an executor thread exists (true until join() completes)
    // Producers may only touch engine_ inline when running_ is false, which is
    // guaranteed to mean "the thread is gone" because stop() clears it after join.
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    // Serialises the lifecycle transition and guards the "no executor, so a
    // producer may touch engine_ directly" decision. Held across join() in
    // stop(), which is why producers must never hold it while blocking.
    std::mutex lifeMu_;
    // Test-only observer, called right after a stop is acknowledged.
    std::function<void()> onStopAcked_;
    // Test-only observer, called between key release and the queue barrier.
    std::function<void()> onBeforeBarrier_;
    // Test-only observer, called inside stop() between request and join.
    std::function<void()> onStopJoining_;
    // How many producers performed an inline Engine mutation. Must stay 0 while
    // an executor thread exists (see the lifecycle invariant above).
    std::atomic<size_t> inlineCleanups_{0};
    // Queued events discarded because an emergency stop was in force when the
    // executor was ready to run them (CancelAll semantics, observable).
    std::atomic<size_t> eventsDroppedByCancel_{0};
};

} // namespace map
