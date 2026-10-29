// map_queue.cpp -- see map_queue.h.
#include "map_queue.h"

#include <chrono>

namespace map {

// ---------------------------------------------------------------------------
// EventQueue
// ---------------------------------------------------------------------------
bool EventQueue::post(const Event& ev) {
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
    fifo_.push_back(item);
    cv_.notify_one();
    return !droppedSomething;
}

void EventQueue::postCancelSource(const std::string& source) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return;
        QueueItem item;
        item.kind = QueueItemKind::CancelSource;
        item.source = source;
        ctrl_.push_back(item);
    }
    cv_.notify_one();
}

void EventQueue::postCancelAll() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return;
        QueueItem item;
        item.kind = QueueItemKind::CancelAll;
        ctrl_.push_back(item);
    }
    cv_.notify_one();
}

void EventQueue::postDeviceRemoved(const std::string& source) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutdown_) return;
        QueueItem item;
        item.kind = QueueItemKind::DeviceRemoved;
        item.source = source;
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
}

void MappingRuntime::post(const Event& ev) { queue_.post(ev); }

void MappingRuntime::postCancelSource(const std::string& source) {
    engine_.requestCancel();
    queue_.postCancelSource(source);
}
void MappingRuntime::postCancelAll() {
    engine_.requestCancel();
    queue_.postCancelAll();
}
void MappingRuntime::postDeviceRemoved(const std::string& source) {
    engine_.requestCancel();
    queue_.postDeviceRemoved(source);
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
    case QueueItemKind::Event:        engine_.dispatch(it.ev); break;
    case QueueItemKind::CancelSource: engine_.cancelSource(it.source); break;
    case QueueItemKind::CancelAll:    engine_.cancelAll(); break;
    case QueueItemKind::DeviceRemoved: engine_.cancelSource(it.source); break;
    }
}

bool MappingRuntime::processOne(int timeoutMs) {
    QueueItem it;
    if (!queue_.pop(it, timeoutMs)) return false;
    handle(it);
    return true;
}

} // namespace map
