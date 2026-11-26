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

bool MappingRuntime::start() {
    if (running_) return true;
    // A previous stop() shut the queue down; without re-arming it the executor
    // would spin through pops that always return false and never process input.
    queue_.reopen();
    running_ = true;
    executor_ = std::thread([this] { loop(); });
    return true;
}

void MappingRuntime::stop() {
    if (!running_) return;
    running_ = false;
    engine_.requestCancel();  // interrupt a long delay/repeat so join() is prompt
    queue_.shutdown();
    if (executor_.joinable()) executor_.join();
    // The executor's final cancelAll() is a full stop: acknowledge whatever was
    // requested so a restarted runtime is not immediately blocked by it.
    engine_.cancelAll();
    // Shutdown is a stop too: work that was queued but never ran must not fire
    // after a restart as if it had just arrived.
    size_t discarded = queue_.dropPendingEvents();
    if (discarded) eventsDroppedByCancel_.fetch_add(discarded);
}
void MappingRuntime::post(const Event& ev) {
    // Stamp the event with the stop that is in force at post time (0 if none).
    // The executor discards an event whose stamp refers to a stop it has already
    // applied, so "actions queued before the stop do not run" holds without
    // guessing from arrival order; an event posted once the stop completed has no
    // stamp and is dispatched normally (a later legitimate action still runs).
    queue_.post(ev, engine_.pendingStopEpoch());
}

// Emergency-stop path. Ordering matters and is deliberate:
//
//  1. Publish the request (epoch + stop epoch + flag) FIRST. This is what stops
//     an executing synchronous dispatch, which a queued control item cannot
//     interrupt at all: the executor is inside the engine, not at the queue.
//  2. Queue the priority control item carrying the epoch, so the executor also
//     performs the key release and acknowledges the request.
//  3. If no executor is running (single-threaded callers, or a runtime between
//     start() calls), nobody would ever pop that item, so the request is
//     acknowledged here instead - otherwise the runtime would stay blocked
//     forever and no later action could run.
//
// The epoch travels with the item, so a burst of stops collapses into one
// acknowledgement without losing the newest request, and a replay of a stop whose
// cleanup already ran is skipped rather than repeated.
void MappingRuntime::postCancelSource(const std::string& source) {
    const uint64_t epoch = engine_.requestCancel();
    if (running_.load()) { queue_.postCancelSource(source, epoch); return; }
    engine_.cancelSource(source, epoch);
    queue_.dropPendingControl();
}
void MappingRuntime::postCancelAll() {
    // The stop epoch is captured BEFORE the flag is published, so events posted
    // from here on carry a stamp that identifies them as pre-stop work.
    const uint64_t epoch = engine_.requestStop();
    if (running_.load()) { queue_.postCancelAll(epoch); return; }
    // No executor to pop the item: acknowledge here and discard what was queued.
    engine_.cancelAll(epoch);
    queue_.dropPendingControl();
    size_t discarded = queue_.dropPendingEvents();
    if (discarded) eventsDroppedByCancel_.fetch_add(discarded);
}
void MappingRuntime::postDeviceRemoved(const std::string& source) {
    const uint64_t epoch = engine_.requestCancel();
    if (running_.load()) { queue_.postDeviceRemoved(source, epoch); return; }
    engine_.cancelSource(source, epoch);
    queue_.dropPendingControl();
}

void MappingRuntime::loop() {
    QueueItem it;
    auto nowMs = [] {
        return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    uint64_t lastTick = nowMs();
    while (running_) {
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
        // An event posted while an emergency stop was in force belongs to the
        // pre-stop world: CancelAll semantics say it does not run. Legitimate
        // input posted after the stop completed carries no stamp.
        if (it.postedStopEpoch && engine_.cancelAcknowledged(it.postedStopEpoch)) {
            eventsDroppedByCancel_.fetch_add(1);
            break;
        }
        // No local stop guard here: Engine::dispatch() is the single place that
        // decides what a pending stop blocks, so the two cannot drift apart.
        engine_.dispatch(it.ev);
        break;
    case QueueItemKind::CancelSource:
        // A stop already executed for this request must not run again: a second
        // releaseAll/cancelAll sweep would wipe key state that a later, already
        // accepted action legitimately owns.
        if (it.cancelEpoch && engine_.cancelAcknowledged(it.cancelEpoch)) break;
        engine_.cancelSource(it.source, it.cancelEpoch);
        break;
    case QueueItemKind::CancelAll: {
        if (it.cancelEpoch && engine_.cancelAcknowledged(it.cancelEpoch)) {
            // Replay of a stop whose cleanup already ran (e.g. a control item left
            // over from a previous run, or two producers racing). Performing it
            // again would delete key state and queued input that a later, already
            // accepted action legitimately owns, so it is a no-op.
            break;
        }
        engine_.cancelAll(it.cancelEpoch);
        // CancelAll semantics: everything still queued was posted before this
        // stop was applied, so none of it may run. Events posted from now on carry
        // this epoch and are discarded individually if they race the stop.
        size_t discarded = queue_.dropPendingEvents();
        if (discarded) eventsDroppedByCancel_.fetch_add(discarded);
        break;
    }
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
