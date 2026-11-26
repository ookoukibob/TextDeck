// map_queue.cpp -- see map_queue.h.
#include "map_queue.h"

#include <chrono>

namespace map {

// ---------------------------------------------------------------------------
// EventQueue
// ---------------------------------------------------------------------------
bool EventQueue::post(const Event& ev, uint64_t postedStopEpoch) {
    std::lock_guard<std::mutex> lk(mu_);
    if (shutdown_) return false;
    bool droppedSomething = false;
    // Released events are NEVER dropped: they must survive queue pressure or a
    // key would stay stuck. Only non-Released events participate in the
    // overflow policy (drop oldest non-Released to make room).
    if (!isRelease(ev)) {
        while (fifo_.size() >= maxNormal_) {
            auto it = fifo_.begin();
            for (; it != fifo_.end(); ++it) {
                if (it->kind == QueueItemKind::Event && !isRelease(it->ev)) break;
            }
            if (it != fifo_.end()) {
                fifo_.erase(it);
                ++dropped_;
                droppedSomething = true;
            } else {
                break; // only releases in the queue: let it grow rather than drop one
            }
        }
    }
    QueueItem item;
    item.kind = QueueItemKind::Event;
    item.ev = ev;
    item.postedStopEpoch = postedStopEpoch;
    item.seq = ++postSeq_;              // assigned under mu_: the linearization point
    fifo_.push_back(item);
    cv_.notify_one();
    return !droppedSomething;
}

void EventQueue::postCancelSource(const std::string& source, uint64_t epoch) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return;
        QueueItem item;
        item.kind = QueueItemKind::CancelSource;
        item.source = source;
        item.cancelEpoch = epoch;
        ctrl_.push_back(item);
    }
    cv_.notify_one();
}

void EventQueue::postCancelAll(uint64_t epoch) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return;
        QueueItem item;
        item.kind = QueueItemKind::CancelAll;
        item.cancelEpoch = epoch;
        ctrl_.push_back(item);
    }
    cv_.notify_one();
}

void EventQueue::postDeviceRemoved(const std::string& source, uint64_t epoch) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return;
        QueueItem item;
        item.kind = QueueItemKind::DeviceRemoved;
        item.source = source;
        item.cancelEpoch = epoch;
        ctrl_.push_back(item);
    }
    cv_.notify_one();
}

bool EventQueue::pop(QueueItem& out, int timeoutMs) {
    std::unique_lock<std::mutex> lk(mu_);
    auto ready = [this] { return shutdown_ || !ctrl_.empty() || !fifo_.empty(); };
    if (timeoutMs <= 0) {
        if (!ready()) return false;
    } else {
        if (!cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), ready)) return false;
    }
    if (shutdown_) return false;
    // CONTROL channel always wins (stop / disconnect / emergency).
    if (!ctrl_.empty()) { out = ctrl_.front(); ctrl_.pop_front(); return true; }
    if (!fifo_.empty()) { out = fifo_.front(); fifo_.pop_front(); return true; }
    return false;
}

void EventQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        shutdown_ = true;
    }
    cv_.notify_all();
}

void EventQueue::reopen() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        shutdown_ = false;
    }
    cv_.notify_all();
}

size_t EventQueue::dropPendingControl() {
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        n = ctrl_.size();
        ctrl_.clear();
    }
    return n;
}

size_t EventQueue::dropPendingEvents() {
    // Everything in the FIFO at the moment a stop is applied was posted before
    // that stop took effect, so none of it may run. Control items are untouched:
    // a per-source disconnect still has to release its own keys.
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        n = fifo_.size();
        fifo_.clear();
    }
    return n;
}

size_t EventQueue::applyStopBarrier() {
    // The barrier and the enqueue sequence are both taken under mu_, so the
    // order between "this event was enqueued" and "this stop was applied" is a
    // total order with no gap. An event whose seq is <= the barrier was queued
    // before the stop took effect and is erased. An event enqueued afterwards,
    // including one from a producer that was still inside its own post() call,
    // has a higher seq and is preserved. No engine state is consulted, so a
    // producer cannot make its own event look post-stop by reading stale state.
    size_t n = 0;
    uint64_t barrier = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        barrier = postSeq_;
        for (auto it = fifo_.begin(); it != fifo_.end();) {
            if (it->kind == QueueItemKind::Event && it->seq <= barrier) {
                it = fifo_.erase(it);
                ++n;
            } else {
                ++it;
            }
        }
    }
    return n;
}

size_t EventQueue::dropped() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dropped_;
}
size_t EventQueue::pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ctrl_.size() + fifo_.size();
}
bool EventQueue::empty() const {
    std::lock_guard<std::mutex> lk(mu_);
    return ctrl_.empty() && fifo_.empty();
}

// ---------------------------------------------------------------------------
// MappingRuntime: single executor over Engine.
// ---------------------------------------------------------------------------
MappingRuntime::MappingRuntime(std::shared_ptr<OutputBackend> out, size_t maxQueue)
    : engine_(out), queue_(maxQueue) {}

MappingRuntime::~MappingRuntime() { stop(); }

// Lifecycle and lock order:
//
//   lifeMu_  ->  queue mutex (EventQueue::mu_)  ->  engine (no lock)
//
//   * lifeMu_ is held for the whole start()/stop() transition and by a producer
//     only while it decides (and, if there is no executor, performs) an inline
//     cleanup. That is what makes "no executor thread exists" a fact a producer
//     can rely on: stop() cannot be midway through joining while a producer holds
//     it, because stop() needs it to join.
//   * Neither the executor nor engine_ ever takes lifeMu_, so it cannot invert.
//   * No blocking output call is made while holding the queue mutex; engine_
//     work happens outside it.
bool MappingRuntime::start() {
    std::lock_guard<std::mutex> life(lifeMu_);
    if (running_) return true;
    // A previous stop() shut the queue down; without re-arming it the executor
    // would spin through pops that always return false and never process input.
    queue_.reopen();
    stopRequested_ = false;
    running_ = true;          // a thread exists from here until join() completes
    executor_ = std::thread([this] { loop(); });
    return true;
}

void MappingRuntime::stop() {
    std::unique_lock<std::mutex> life(lifeMu_);
    if (!running_) return;
    // Tell the LOOP to exit, but keep running_ true: a producer that sees
    // running_ == false must be able to conclude no executor exists. Clearing it
    // before join() let a producer take the inline-cleanup path and touch engine_
    // while the executor was still inside Engine::dispatch().
    stopRequested_ = true;
    engine_.requestCancel();  // interrupt a long delay/repeat so join() is prompt
    queue_.shutdown();
    // Test-only: at this instant the executor is still alive and running_ is
    // still true, which is exactly the window the lifecycle fix is about.
    if (onStopJoining_) onStopJoining_();
    if (executor_.joinable()) executor_.join();
    running_ = false;         // executor is gone (joined): only now is that true
    // Final sweep, now that the executor cannot touch engine_ concurrently.
    engine_.cancelAll();
    // Shutdown is a stop too: work queued but never run must not fire after a
    // restart as if it had just arrived.
    size_t discarded = queue_.dropPendingEvents();
    if (discarded) eventsDroppedByCancel_.fetch_add(discarded);
}

void MappingRuntime::post(const Event& ev) {
    // Stamp the event with the stop that is in force at post time (0 if none).
    // The stamp is only a hint for diagnostics; the authoritative pre/post-stop
    // decision is the enqueue sequence compared against the stop barrier inside
    // EventQueue, so a producer cannot be fooled by reading stale stop state.
    queue_.post(ev, engine_.pendingStopEpoch());
}

// Emergency-stop path. Ordering is deliberate:
//
//  1. Publish the request (epoch + stop epoch + flag) FIRST. This is what stops
//     an executing synchronous dispatch, which a queued control item cannot
//     interrupt: the executor is inside the engine, not at the queue.
//  2. Queue the priority control item carrying the epoch, so the executor
//     performs the key release, applies the FIFO barrier, and acknowledges.
//  3. If no executor exists, nobody would ever pop that item. That decision is
//     taken under lifeMu_, together with stop()'s transition, so a producer can
//     never run the inline cleanup while an executor is still alive.
void MappingRuntime::postCancelSource(const std::string& source) {
    const uint64_t epoch = engine_.requestCancel();
    {
        std::lock_guard<std::mutex> life(lifeMu_);
        if (!running_) {
            inlineCleanups_.fetch_add(1);
            engine_.cancelSource(source, epoch);
            queue_.dropPendingControl();
            return;
        }
    }
    queue_.postCancelSource(source, epoch);
}
void MappingRuntime::postCancelAll() {
    const uint64_t epoch = engine_.requestStop();
    {
        std::lock_guard<std::mutex> life(lifeMu_);
        if (!running_) {
            inlineCleanups_.fetch_add(1);
            // No executor: perform the same three-step order as the queued path.
            // releaseAllKeys() (not cancelAll()) so the stop is NOT acknowledged
            // until the FIFO barrier has been applied.
            engine_.releaseAllKeys();
            queue_.dropPendingControl();
            size_t discarded = queue_.applyStopBarrier();
            if (discarded) eventsDroppedByCancel_.fetch_add(discarded);
            engine_.acknowledgeStop(epoch);
            return;
        }
    }
    queue_.postCancelAll(epoch);
}
void MappingRuntime::postDeviceRemoved(const std::string& source) {
    const uint64_t epoch = engine_.requestCancel();
    {
        std::lock_guard<std::mutex> life(lifeMu_);
        if (!running_) {
            inlineCleanups_.fetch_add(1);
            engine_.cancelSource(source, epoch);
            queue_.dropPendingControl();
            return;
        }
    }
    queue_.postDeviceRemoved(source, epoch);
}

void MappingRuntime::loop() {
    QueueItem it;
    auto nowMs = [] {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    uint64_t lastTick = nowMs();
    while (!stopRequested_) {
        if (!queue_.pop(it, 20)) {
            // idle: drive time-based triggers (HoldRepeat / LongPress)
            engine_.tick(nowMs());
            lastTick = nowMs();
            continue;
        }
        handle(it);
        // Also tick under sustained load: time-based triggers must not starve
        // just because events keep arriving (bounded to ~every 15 ms).
        uint64_t t = nowMs();
        if (t - lastTick >= 15) { engine_.tick(t); lastTick = t; }
    }
    // On exit make sure no key is left stuck.
    engine_.cancelAll();
}

void MappingRuntime::handle(const QueueItem& it) {
    switch (it.kind) {
    case QueueItemKind::Event:
        // CancelAll semantics are enforced by the queue barrier at stop time, so
        // an event still in the FIFO is either genuinely after the stop or was
        // posted while it was in flight; in the latter case the engine's own
        // stopPending() gate refuses it. No local stamp test here.
        engine_.dispatch(it.ev);
        break;
    case QueueItemKind::CancelSource:
        // A stop already executed for this request must not run again: a second
        // releaseAll/cancelAll sweep would wipe key state that a later, already
        // accepted action legitimately owns.
        if (it.cancelEpoch && engine_.cancelAcknowledged(it.cancelEpoch)) break;
        engine_.cancelSource(it.source, it.cancelEpoch);
        break;
    case QueueItemKind::CancelAll:
        // Replay of a stop whose cleanup already ran (a control item left over
        // from a previous run, or two producers racing) must not wipe key state
        // that a later, already accepted action legitimately owns.
        if (it.cancelEpoch && engine_.cancelAcknowledged(it.cancelEpoch)) break;
        // Ordering inside a step is load bearing:
        //   1. release every key and clear every trigger;
        //   2. take the FIFO barrier, dropping events enqueued before this point;
        //   3. ONLY THEN acknowledge, which is what makes the stop observably
        //      "complete" to producers.
        // Acknowledging before the barrier let a producer see the stop as done,
        // post a legitimate event, and lose it to the sweep that ran afterwards.
        //
        // (1) MUST be releaseAllKeys(), NOT cancelAll(). cancelAll() with its
        // default epoch snapshots the current epoch and acknowledges it
        // internally, so it would make the stop observable BEFORE the barrier -
        // exactly the defect this ordering exists to prevent. releaseAllKeys() is
        // cleanup only and touches no acknowledgement state.
        engine_.releaseAllKeys();                  // (1) engine-side release only
        // Test-only: at this instant the stop must NOT be observable as complete.
        // If an implicit acknowledgement happened in step (1), stopPending() is
        // already false here and the regression test fails at this boundary.
        if (onBeforeBarrier_) onBeforeBarrier_();
        {
            size_t discarded = queue_.applyStopBarrier();   // (2)
            if (discarded) eventsDroppedByCancel_.fetch_add(discarded);
        }
        engine_.acknowledgeStop(it.cancelEpoch);   // (3) now visibly complete
        // Test-only: a producer that reacts to completion by posting an event
        // does so here, i.e. immediately after the stop is externally visible and
        // before this handler returns. It must never be deleted afterwards.
        if (onStopAcked_) onStopAcked_();
        break;
    case QueueItemKind::DeviceRemoved:
        if (it.cancelEpoch && engine_.cancelAcknowledged(it.cancelEpoch)) break;
        engine_.cancelSource(it.source, it.cancelEpoch);
        break;
    }
}

bool MappingRuntime::processOne(int timeoutMs) {
    QueueItem it;
    if (!queue_.pop(it, timeoutMs)) return false;
    handle(it);
    return true;
}

} // namespace map
