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

struct QueueItem {
    QueueItemKind kind = QueueItemKind::Event;
    Event ev;
    std::string source; // CancelSource / DeviceRemoved
};

class EventQueue {
public:
    explicit EventQueue(size_t maxNormal = 2048) : maxNormal_(maxNormal) {}

    // Returns true if the event was accepted without dropping anything; false if
    // an older non-Released event had to be dropped to make room.
    bool post(const Event& ev);
    void postCancelSource(const std::string& source);   // priority, never dropped
    void postCancelAll();                               // priority, never dropped
    void postDeviceRemoved(const std::string& source);  // priority, never dropped

    // Blocking pop; processes CONTROL items first. Returns false on timeout or
    // after shutdown().
    bool pop(QueueItem& out, int timeoutMs);
    void shutdown();        // wake all waiters; subsequent pop() returns false

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

    size_t dropped() const { return queue_.dropped(); }
    size_t pending() const { return queue_.pending(); }

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
};

} // namespace map
