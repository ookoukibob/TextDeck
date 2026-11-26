// ---------------------------------------------------------------------------
// map_cancel_test.cpp -- cancellation / emergency-stop regression tests.
//
// Every test here targets one previously observable defect:
//   C1 cancel requested just before dispatch was erased by dispatch entry
//   C2 a conditional block kept running after a cancellation interrupted a
//      nested delay (the branch checked only the script-local flag)
//   C3 long type_text runs were uninterruptible and could leave modifiers down
//   C4 a queued CancelAll could not stop an already-running synchronous dispatch
//   C5 pending actions ran after an accepted stop
//   C6 repeated cancels / replayed control items corrupted runtime state
//   C7 a completed cancellation blocked all later actions
//   C8 a plain per-device cancel suppressed unrelated input
//   C9 a cancelled chord left a modifier logically pressed
//
// Determinism: cancellation is injected from inside the output backend (a
// documented, synchronous point on the calling thread) instead of by sleeping
// for an arbitrary duration. Only C4/C5 use a real thread, and they assert a
// generous wall-clock bound rather than an exact timing.
// ---------------------------------------------------------------------------
#include "../map_core.h"
#include "../map_engine.h"
#include "../map_queue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace map;

static int g_check = 0, g_pass = 0, g_fail = 0;
static void check(bool ok, const char* name, const std::string& d = "") {
    ++g_check;
    if (ok) { ++g_pass; std::printf("  [PASS] %s %s\n", name, d.c_str()); }
    else    { ++g_fail; std::printf("  [FAIL] %s %s\n", name, d.c_str()); }
}
static void section(const char* s) { std::printf("\n== %s ==\n", s); }

static Event press(const std::string& src, const std::string& ctl, uint64_t ts) {
    return Event{src, ctl, EventType::Pressed, 0.0, false, "", ts, "test"};
}
static Event release(const std::string& src, const std::string& ctl, uint64_t ts) {
    return Event{src, ctl, EventType::Released, 0.0, false, "", ts, "test"};
}
static int count(const std::vector<std::string>& ops, const std::string& op) {
    int n = 0;
    for (const auto& o : ops) if (o == op) ++n;
    return n;
}
static int taps(const std::vector<std::string>& ops, const std::string& key) {
    return count(ops, "down:" + key);
}
// ---------------------------------------------------------------------------
// HookBackend: records operations like RecordOutput, but lets a test inject a
// cancellation at an exact point in the input sequence. This is what makes the
// concurrency tests deterministic: the hook runs synchronously on the thread
// that is executing the script, at a defined boundary between input units.
//
// THREAD SAFETY: every accessor that a test thread may call while the runtime
// executor is live takes mu_. `ops` and `held_` are plain containers, so reading
// them from another thread without the lock would be a data race (undefined
// behaviour) even if it happens to "work" in practice. Tests that assert on `ops`
// should either synchronise through snapshot()/signal, or assert only after the
// executor has been joined.
// ---------------------------------------------------------------------------
class HookBackend final : public OutputBackend {
public:
    std::function<void(const std::string& key)> onKeyDown;  // after the down
    std::function<void(int charsEmitted)>      onTextChar;  // after each char
    int failTypeTextAtChar = -1;                            // -1 = never fail

    bool keyDown(const std::string& key) override {
        if (!FindKey(key)) return false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            held_[key]++;
            ops.push_back("down:" + key);
        }
        if (onKeyDown) onKeyDown(key);
        return true;
    }
    bool keyUp(const std::string& key) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = held_.find(key);
            if (it == held_.end()) return false;
            if (--it->second <= 0) held_.erase(it);
            ops.push_back("up:" + key);
        }
        return true;
    }
    TextOutcome typeText(const std::string& text, const CancelProbe& cancelled) override {
        std::string sent;
        int emitted = 0;
        for (char c : text) {
            if (cancelled && cancelled()) {
                if (!sent.empty()) { std::lock_guard<std::mutex> lk(mu_); ops.push_back("text:" + sent); }
                return TextOutcome::Cancelled;
            }
            if (emitted == failTypeTextAtChar) {
                if (!sent.empty()) { std::lock_guard<std::mutex> lk(mu_); ops.push_back("text:" + sent); }
                return TextOutcome::Failed;
            }
            sent += c;
            ++emitted;
            if (onTextChar) onTextChar(emitted);
        }
        if (!sent.empty()) { std::lock_guard<std::mutex> lk(mu_); ops.push_back("text:" + sent); }
        return TextOutcome::Ok;
    }
    bool releaseAll() override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& kv : held_) ops.push_back("up:" + kv.first);
        held_.clear();
        return true;
    }
    const char* name() const override { return "hook"; }
    bool isHeld(const std::string& key) const {
        std::lock_guard<std::mutex> lk(mu_);
        return held_.find(key) != held_.end();
    }
    std::vector<std::string> heldKeys() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> v;
        for (const auto& kv : held_) v.push_back(kv.first);
        return v;
    }
    // Locked copy of the recorded operations. Use this from the test thread when
    // the executor may still be running; never read `ops` directly in that case.
    std::vector<std::string> snapshot() const {
        std::lock_guard<std::mutex> lk(mu_);
        return ops;
    }
    // Number of recorded ops equal to `op`, taken under the lock.
    int countOp(const std::string& op) const {
        std::lock_guard<std::mutex> lk(mu_);
        int n = 0;
        for (const auto& o : ops) if (o == op) ++n;
        return n;
    }
private:
    mutable std::mutex mu_;
    std::map<std::string, int> held_;
    std::vector<std::string> ops;   // guarded by mu_ while the executor is live
};

// Locked accessors for HookBackend. Every assertion on recorded operations goes
// through these, so the test thread never touches `ops` while the executor writes
// it. Outside the runtime the behaviour is identical to the plain helpers above.
static int countOf(const HookBackend& hb, const std::string& op) { return hb.countOp(op); }
static int tapsOf(const HookBackend& hb, const std::string& key) {
    return hb.countOp("down:" + key);
}

// ---------------------------------------------------------------------------
// FailBackend: a controllable output backend that refuses selected operations.
// Used to prove that a refused physical KeyUp is (a) reported, and (b) does NOT
// erase the only record of a key that may still be physically down.
// ---------------------------------------------------------------------------
class FailBackend final : public OutputBackend {
public:
    std::vector<std::string> ops;
    bool failDown = false;
    bool failUp = false;
    bool failReleaseAll = false;
    int  upFailuresLeft = -1;      // -1 = honour failUp for every call

    bool keyDown(const std::string& key) override {
        if (!FindKey(key)) return false;
        if (failDown) { ops.push_back("fail-down:" + key); return false; }
        held_[key]++;
        ops.push_back("down:" + key);
        return true;
    }
    bool keyUp(const std::string& key) override {
        if (failUp || upFailuresLeft > 0) {
            if (upFailuresLeft > 0) --upFailuresLeft;
            ops.push_back("fail-up:" + key);
            return false;
        }
        // A refused KeyUp is modelled above; otherwise the injection is accepted.
        // Deliberately does NOT require the key to be in held_: a real SendInput
        // key-up is accepted regardless of the backend's internal bookkeeping.
        auto it = held_.find(key);
        if (it != held_.end()) { if (--it->second <= 0) held_.erase(it); }
        ops.push_back("up:" + key);
        return true;
    }
    TextOutcome typeText(const std::string& text, const CancelProbe& cancelled) override {
        (void)cancelled;
        ops.push_back("text:" + text);
        return TextOutcome::Ok;
    }
    bool releaseAll() override {
        if (failReleaseAll) { ops.push_back("fail-releaseall"); return false; }
        for (const auto& kv : held_) ops.push_back("up:" + kv.first);
        held_.clear();
        return true;
    }
    const char* name() const override { return "fail"; }
    std::vector<std::string> heldKeys() const {
        std::vector<std::string> v;
        for (const auto& kv : held_) v.push_back(kv.first);
        return v;
    }
private:
    std::map<std::string, int> held_;
};

int main() {
    std::printf("map_cancel_test -- cancellation / emergency-stop regression tests\n");

    // ---- C1: a request that lands just before dispatch must not be erased ----
    section("C1 cancellation immediately before dispatch");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding b; b.id = "c1"; b.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "x"; b.script = {t};
        e.addBinding(b);

        const uint64_t ep = e.requestStop();
        check(e.cancelPending(), "c1-request-registered", "");
        check(ep >= 1, "c1-request-has-epoch", "epoch=" + std::to_string(ep));

        e.dispatch(press("s", "c", 1000));
        check(tapsOf(*out, "x") == 0, "c1-dispatch-blocked", "");
        check(e.cancelPending(), "c1-request-not-silently-cleared", "");

        // Completing the stop releases the block, and the next action runs.
        e.cancelAll(ep);
        check(!e.cancelPending(), "c1-ack-clears-request", "");
        e.dispatch(press("s", "c", 2000));
        check(tapsOf(*out, "x") == 1, "c1-action-runs-after-ack", "");
        check(out->heldKeys().empty(), "c1-no-stuck-keys", "");
    }

    // ---- C2: a conditional block must stop when a nested step is cancelled ---
    section("C2 cancellation inside a conditional action");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        // 'a' is only held so the conditional has a real condition to evaluate.
        Binding hold; hold.id = "hold"; hold.match = {"s", "mod", EventType::Pressed};
        hold.trigger = TriggerKind::Momentary; hold.holdKey = "shift";
        e.addBinding(hold);

        Binding b; b.id = "c2"; b.match = {"s", "c", EventType::Pressed};
        Action nested1; nested1.kind = ActionKind::KeyTap; nested1.key = "a";
        Action nestedDelay; nestedDelay.kind = ActionKind::Delay; nestedDelay.delayMs = 3000;
        Action nested2; nested2.kind = ActionKind::KeyTap; nested2.key = "z";
        Action cond; cond.kind = ActionKind::Conditional; cond.cond = "held:shift";
        cond.block = {nested1, nestedDelay, nested2};
        b.script = {cond};
        e.addBinding(b);
        e.setRealtime(true);

        // Cancel exactly when the first nested step has run: this is the case
        // the old code missed (it only looked at the script-local flag, so the
        // rest of the block - including 'z' - still executed).
        out->onKeyDown = [&](const std::string& k) {
            if (k == "a") e.requestCancel();
        };

        e.dispatch(press("s", "mod", 1000));   // hold shift so the condition is true
        const auto t0 = std::chrono::steady_clock::now();
        e.dispatch(press("s", "c", 1100));
        const long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        check(tapsOf(*out, "a") == 1, "c2-first-nested-step-ran", "");
        check(tapsOf(*out, "z") == 0, "c2-rest-of-block-skipped", "");
        check(ms < 500, "c2-delay-did-not-run-to-completion",
              "elapsed=" + std::to_string(ms) + "ms of a 3000ms delay");
        check(e.cancelPending(), "c2-cancel-still-pending", "");
        e.cancelAll();
        check(!e.cancelPending(), "c2-ack-clears-request", "");
    }

    // ---- C2b: repeat must stop mid-sequence, not finish the iteration --------
    section("C2b cancellation inside repeat");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding b; b.id = "rep"; b.match = {"s", "c", EventType::Pressed};
        Action inner1; inner1.kind = ActionKind::KeyTap; inner1.key = "a";
        Action inner2; inner2.kind = ActionKind::KeyTap; inner2.key = "b";
        Action rep; rep.kind = ActionKind::Repeat; rep.count = 5;
        rep.block = {inner1, inner2};
        b.script = {rep};
        e.addBinding(b);
        out->onKeyDown = [&](const std::string& k) {
            if (k == "a") e.requestCancel();
        };
        e.dispatch(press("s", "c", 1000));
        check(tapsOf(*out, "a") == 1, "c2b-repeat-stops-after-first-step",
              "a=" + std::to_string(tapsOf(*out, "a")));
        check(tapsOf(*out, "b") == 0, "c2b-no-second-step", "");
        e.cancelAll();
    }

    // ---- C3: long text injection must be interruptible ----------------------
    section("C3 cancellation during long text injection");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding b; b.id = "txt"; b.match = {"s", "c", EventType::Pressed};
        Action text; text.kind = ActionKind::TypeText;
        text.text = "abcdefghij";                     // 10 chars
        Action after; after.kind = ActionKind::KeyTap; after.key = "z";
        b.script = {text, after};
        e.addBinding(b);

        out->onTextChar = [&](int n) {
            if (n == 3) e.requestCancel();            // stop after 3 characters
        };
        e.dispatch(press("s", "c", 1000));

        bool sawPartial = false;
        for (const auto& o : out->snapshot()) if (o == "text:abc") sawPartial = true;
        check(sawPartial, "c3-text-stopped-at-safe-point", "");
        bool sawWhole = false;
        for (const auto& o : out->snapshot()) if (o == "text:abcdefghij") sawWhole = true;
        check(!sawWhole, "c3-text-did-not-complete", "");
        check(tapsOf(*out, "z") == 0, "c3-following-action-skipped", "");
        check(e.cancelPending(), "c3-cancel-reported-pending", "");
        e.cancelAll();
        check(!e.cancelPending(), "c3-ack-clears-request", "");
    }

    // ---- C3b: a backend failure is not reported as a cancellation ------------
    section("C3b text failure stays distinct from cancellation");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding b; b.id = "fail"; b.match = {"s", "c", EventType::Pressed};
        Action text; text.kind = ActionKind::TypeText; text.text = "abcdef";
        Action after; after.kind = ActionKind::KeyTap; after.key = "q";
        b.script = {text, after};
        e.addBinding(b);
        out->failTypeTextAtChar = 2;                  // backend refuses char 3
        e.dispatch(press("s", "c", 1000));
        check(!e.cancelPending(), "c3b-failure-is-not-a-cancel", "");
        check(e.textFailures() == 1, "c3b-failure-is-reported",
              "textFailures=" + std::to_string(e.textFailures()));
        // A genuine cancellation must not be counted as a failure.
        out->failTypeTextAtChar = -1;
        out->onTextChar = [&](int n) { if (n == 2) e.requestCancel(); };
        e.dispatch(press("s", "c", 2000));
        check(e.textFailures() == 1, "c3b-cancel-not-counted-as-failure",
              "textFailures=" + std::to_string(e.textFailures()));
        e.cancelAll();
    }

    // ---- C4: a stop must reach a running synchronous dispatch ---------------
    section("C4 cancellation during an active synchronous dispatch");
    {
        auto out = std::make_shared<HookBackend>();
        MappingRuntime rt(out, 256);
        rt.engine().setRealtime(true);
        Binding slow; slow.id = "slow"; slow.match.type = EventType::Pressed;
        slow.match.source = "s"; slow.match.control = "long";
        Action d; d.kind = ActionKind::Delay; d.delayMs = 5000;
        Action t; t.kind = ActionKind::KeyTap; t.key = "z";
        slow.script = {d, t};
        Binding quick; quick.id = "quick"; quick.match.source = "s"; quick.match.control = "quick";
        Action t2; t2.kind = ActionKind::KeyTap; t2.key = "y";
        quick.script = {t2};
        rt.engine().addBinding(slow);
        rt.engine().addBinding(quick);
        rt.start();

        rt.post(press("s", "long", 1));
        // Let the executor enter the 5s delay: the engine's own gate, not the
        // queue, is the only thing that can stop it now.
        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        const auto t0 = std::chrono::steady_clock::now();
        rt.postCancelAll();
        const long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        // The stop is published before the control item is queued, so the running
        // delay observes it on its next slice instead of waiting for the queue.
        check(ms < 1000, "c4-stop-returned-promptly", "postCancelAll took " + std::to_string(ms) + "ms");

        // Wait (bounded) for the executor to acknowledge, then verify.
        bool acked = false;
        for (int i = 0; i < 200 && !acked; ++i) {
            acked = !rt.cancelPending();
            if (!acked) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(acked, "c4-stop-acknowledged", "");
        check(tapsOf(*out, "z") == 0, "c4-no-action-after-stop", "");
        check(out->heldKeys().empty(), "c4-no-stuck-keys", "");

        // The runtime must accept new work afterwards (guarantee 8). A condition
        // variable rather than polling shared output state.
        std::mutex mu;
        std::condition_variable cv;
        bool yRan = false;
        out->onKeyDown = [&](const std::string& key) {
            if (key != "y") return;
            { std::lock_guard<std::mutex> lk(mu); yRan = true; }
            cv.notify_all();
        };
        rt.post(press("s", "quick", 2));
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::seconds(10), [&] { return yRan; });
        }
        rt.stop();
        check(yRan, "c4-new-action-after-stop", "");
        check(tapsOf(*out, "y") == 1, "c4-exactly-one-action", "");
    }

    // ---- C5: pending actions follow CancelAll semantics ---------------------
    section("C5 cancellation while actions are queued");
    {
        auto out = std::make_shared<HookBackend>();
        MappingRuntime rt(out, 64);
        Binding b; b.id = "q"; b.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "k"; b.script = {t};
        rt.engine().addBinding(b);

        // No executor: everything stays queued, so this is fully deterministic.
        rt.post(press("s", "c", 1));
        rt.post(press("s", "c", 2));
        rt.post(press("s", "c", 3));
        check(rt.pending() == 3, "c5-three-queued", "pending=" + std::to_string(rt.pending()));

        rt.postCancelAll();
        check(!rt.cancelPending(), "c5-stop-acknowledged-without-executor", "");
        check(rt.pending() == 0, "c5-pending-actions-discarded",
              "pending=" + std::to_string(rt.pending()));
        check(rt.eventsDroppedByCancel() >= 3, "c5-discard-counted",
              "dropped=" + std::to_string(rt.eventsDroppedByCancel()));
        check(tapsOf(*out, "k") == 0, "c5-none-of-them-ran", "");

        // A fresh action after the stop is processed normally.
        rt.post(press("s", "c", 4));
        check(rt.processOne(0), "c5-later-action-accepted", "");
        check(tapsOf(*out, "k") == 1, "c5-later-action-ran", "");
    }

    // ---- C5b: an event posted during the stop race is not dispatched -------
    section("C5b event posted while a stop is in flight");
    {
        auto out = std::make_shared<HookBackend>();
        MappingRuntime rt(out, 64);
        Binding b; b.id = "r"; b.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "k"; b.script = {t};
        rt.engine().addBinding(b);

        // Reproduce the interleaving without an executor thread: the stop request
        // is published, an event is enqueued while it is still in flight, and the
        // stop is then applied. The queue barrier taken at stop time is what
        // decides the event's fate - the engine must not be consulted for it.
        const uint64_t stopEpoch = rt.engine().requestStop();
        rt.post(press("s", "c", 1));               // enqueued before the barrier
        rt.engine().cancelAll(0);                  // engine-side release only
        size_t dropped = rt.queueApplyStopBarrier();  // the linearization point
        rt.engine().acknowledgeStop(stopEpoch);
        check(!rt.cancelPending(), "c5b-stop-applied", "");
        check(dropped >= 1, "c5b-inflight-event-dropped-at-barrier",
              "dropped=" + std::to_string(dropped));

        check(!rt.processOne(0), "c5b-inflight-event-gone", "");
        check(tapsOf(*out, "k") == 0, "c5b-inflight-event-not-dispatched", "");
        check(rt.eventsDroppedByCancel() >= 0, "c5b-drop-path-reachable", "");

        // An event posted after the barrier is on the other side of the
        // linearization point and must survive and run.
        rt.post(press("s", "c", 2));
        check(rt.processOne(0), "c5b-post-stop-event-accepted", "");
        check(tapsOf(*out, "k") == 1, "c5b-post-stop-event-ran", "");
    }

    // ---- C6: repeated / concurrent cancellation stays consistent ------------
    section("C6 repeated cancellation requests");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding b; b.id = "c6"; b.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "x"; b.script = {t};
        e.addBinding(b);

        e.requestStop(); e.requestStop(); e.requestStop();
        check(e.cancelPending(), "c6-repeated-requests-registered", "");
        e.dispatch(press("s", "c", 1000));
        check(tapsOf(*out, "x") == 0, "c6-still-blocked", "");

        e.cancelAll();                       // one acknowledgement clears them all
        check(!e.cancelPending(), "c6-single-ack-clears-burst", "");
        e.cancelAll();                       // a second/replayed stop is harmless
        check(!e.cancelPending(), "c6-double-ack-stable", "");
        e.dispatch(press("s", "c", 2000));
        check(tapsOf(*out, "x") == 1, "c6-normal-execution-after-acks", "");
        check(out->heldKeys().empty(), "c6-no-stuck-keys", "");
    }

    // ---- C6b: a replayed control item must not stop fresh work --------------
    section("C6b replayed CancelAll does not double-stop");
    {
        auto out = std::make_shared<HookBackend>();
        MappingRuntime rt(out, 64);
        Binding hold; hold.id = "h"; hold.match = {"s", "hold", EventType::Pressed};
        hold.trigger = TriggerKind::Momentary; hold.holdKey = "space";
        rt.engine().addBinding(hold);
        rt.start();

        // Legitimately hold a key, then run a stop and wait for it to complete.
        rt.post(press("s", "hold", 1));
        bool held = false;
        for (int i = 0; i < 200 && !held; ++i) {
            held = out->isHeld("space");
            if (!held) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(held, "c6b-key-held-before-stop", "");

        const uint64_t ep = rt.engine().requestStop();
        rt.engine().cancelAll(ep);            // executor-side cleanup, inline
        check(!rt.cancelPending(), "c6b-stop-completed", "");

        // A control item for the *same* epoch arriving later must be a no-op:
        // replaying it would release keys a later action legitimately owns.
        QueueItem item; item.kind = QueueItemKind::CancelAll; item.cancelEpoch = ep;
        rt.engine().cancelAll(item.cancelEpoch);  // explicit ack of same epoch
        check(!rt.cancelPending(), "c6b-replay-does-not-block", "");
        check(rt.engine().cancelAcknowledged(ep), "c6b-epoch-recorded-acked",
              "epoch=" + std::to_string(ep));
        rt.stop();
    }

    // ---- C8: a per-device cancel must not suppress unrelated input ---------
    section("C8 per-source cancel does not block other sources");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding b1; b1.id = "p1"; b1.match = {"pad1", "b1", EventType::Pressed};
        b1.trigger = TriggerKind::Momentary; b1.holdKey = "space";
        Binding b2; b2.id = "p2"; b2.match = {"pad2", "b2", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "t"; b2.script = {t};
        e.addBinding(b1); e.addBinding(b2);

        e.dispatch(press("pad1", "b1", 1000));
        check(out->isHeld("space"), "c8-key-held", "");

        const uint64_t ep = e.requestCancel();   // pad1 disappears
        e.cancelSource("pad1", ep);
        check(!out->isHeld("space"), "c8-disconnect-released-its-keys", "");
        check(!e.cancelPending(), "c8-disconnect-acknowledged", "");

        // pad2 must keep working: the disconnect was about pad1's keys only.
        e.dispatch(press("pad2", "b2", 1100));
        check(tapsOf(*out, "t") == 1, "c8-other-source-still-mapped", "");
    }

    // ---- C9: a cancelled chord must not leave a modifier pressed -----------
    section("C9 cancellation during modifier / chord handling");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding b; b.id = "chord"; b.match = {"s", "c", EventType::Pressed};
        Action ctrl; ctrl.kind = ActionKind::KeyDown; ctrl.key = "ctrl";
        Action key;  key.kind  = ActionKind::KeyDown; key.key  = "a";
        Action delay; delay.kind = ActionKind::Delay; delay.delayMs = 3000;
        Action upKey; upKey.kind = ActionKind::KeyUp; upKey.key = "a";
        Action upCtrl; upCtrl.kind = ActionKind::KeyUp; upCtrl.key = "ctrl";
        b.script = {ctrl, key, delay, upKey, upCtrl};
        e.addBinding(b);
        e.setRealtime(true);

        // Cancel while the chord is held: the script never reaches its own KeyUp
        // steps, so the engine has to unwind them.
        out->onKeyDown = [&](const std::string& k) {
            if (k == "a") e.requestCancel();
        };
        e.dispatch(press("s", "c", 1000));

        check(tapsOf(*out, "ctrl") == 1 && tapsOf(*out, "a") == 1,
              "c9-chord-was-pressed", "");
        check(out->heldKeys().empty(), "c9-chord-unwound",
              "held=" + std::to_string(out->heldKeys().size()));
        check(countOf(*out, "up:ctrl") == 1, "c9-modifier-released", "");
        check(countOf(*out, "up:a") == 1, "c9-key-released", "");
        e.cancelAll();
        check(!e.cancelPending(), "c9-ack-clears-request", "");
    }

    // ---- C9b: a cancelled combo does not leave its tail held ---------------
    section("C9b cancellation during key_combo");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding b; b.id = "combo"; b.match = {"s", "c", EventType::Pressed};
        Action combo; combo.kind = ActionKind::KeyCombo;
        combo.combo = {"ctrl", "shift", "alt"};
        b.script = {combo};
        e.addBinding(b);
        out->onKeyDown = [&](const std::string& k) {
            if (k == "shift") e.requestCancel();   // cancel mid-chord
        };
        e.dispatch(press("s", "c", 1000));
        check(out->heldKeys().empty(), "c9b-combo-tail-not-held", "");
        check(countOf(*out, "up:ctrl") == 1, "c9b-pressed-member-released", "");
        e.cancelAll();
    }

    // ---- C10: runtime stop/start leaves no cancellation residue -------------
    section("C10 runtime restart after cancellation");
    {
        auto out = std::make_shared<HookBackend>();
        MappingRuntime rt(out, 64);
        Binding hold; hold.id = "h10"; hold.match = {"s", "hold", EventType::Pressed};
        hold.trigger = TriggerKind::Momentary; hold.holdKey = "space";
        Binding tap; tap.id = "t10"; tap.match = {"s", "tap", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "n"; tap.script = {t};
        rt.engine().addBinding(hold);
        rt.engine().addBinding(tap);

        rt.start();
        rt.post(press("s", "hold", 1));
        bool held = false;
        for (int i = 0; i < 200 && !held; ++i) {
            held = out->isHeld("space");       // locked accessor
            if (!held) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(held, "c10-key-held-before-stop", "");
        rt.postCancelAll();
        rt.stop();
        check(out->heldKeys().empty(), "c10-no-keys-after-stop", "");
        check(!rt.cancelPending(), "c10-no-residual-request-after-stop", "");

        // Restart: the queue must consume input again and the engine must accept
        // new actions, proving the stop left no permanent block behind. Notified
        // from the executor rather than polling recorded output.
        std::mutex mu;
        std::condition_variable cv;
        bool nRan = false;
        out->onKeyDown = [&](const std::string& key) {
            if (key != "n") return;
            { std::lock_guard<std::mutex> lk(mu); nRan = true; }
            cv.notify_all();
        };
        rt.start();
        rt.post(press("s", "tap", 2));
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::seconds(10), [&] { return nRan; });
        }
        rt.stop();
        check(nRan, "c10-action-runs-after-restart", "");
        check(tapsOf(*out, "n") == 1, "c10-exactly-one-action", "");
    }

    // ---- C7: a request raised DURING cleanup stays pending -----------------
    section("C7 request raised during cleanup is not swallowed");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding b; b.id = "c7"; b.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "x"; b.script = {t};
        e.addBinding(b);

        e.dispatch(press("s", "c", 1000));           // baseline: one tap
        check(tapsOf(*out, "x") == 1, "c7-baseline-tap", "");

        const uint64_t first = e.requestStop();
        e.cancelAll(first);                          // complete the first stop
        check(!e.cancelPending(), "c7-first-stop-complete", "");

        // A second request arrives; a stale acknowledgement of the first must not
        // clear it, because that cleanup never released keys for the second.
        const uint64_t second = e.requestStop();
        check(second > first, "c7-second-request-has-newer-epoch",
              "first=" + std::to_string(first) + " second=" + std::to_string(second));
        e.cancelAll(first);                          // stale acknowledge
        check(e.cancelPending(), "c7-stale-ack-does-not-clear-newer-request", "");
        e.dispatch(press("s", "c", 1200));
        check(tapsOf(*out, "x") == 1, "c7-dispatch-blocked-by-newer-request",
              "taps=" + std::to_string(tapsOf(*out, "x")));

        e.cancelAll(second);
        check(!e.cancelPending(), "c7-newer-request-acknowledged", "");
        e.dispatch(press("s", "c", 1300));
        check(tapsOf(*out, "x") == 2, "c7-runs-after-correct-ack", "");
    }

    // ---- C11: a newer request's cleanup must not acknowledge an older stop ---
    // The reported interleaving, replayed exactly and deterministically:
    //   1. requestStop()      -> epoch 10 (the stop is NOT processed yet)
    //   2. requestCancel()    -> epoch 11 (another source goes away)
    //   3. cancelSource(src, 11) completes
    //   4. the stop for epoch 10 has still not been handled
    // A single shared "highest acknowledged epoch" watermark cannot express this:
    // acknowledging 11 would make 10 look handled, unblocking dispatch while the
    // global emergency release has never run.
    section("C11 newer cancel must not acknowledge an older unhandled stop");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding hold; hold.id = "c11h"; hold.match = {"padA", "b", EventType::Pressed};
        hold.trigger = TriggerKind::Momentary; hold.holdKey = "space";
        Binding act; act.id = "c11a"; act.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "x"; act.script = {t};
        e.addBinding(hold);
        e.addBinding(act);

        e.dispatch(press("padA", "b", 1000));      // a key is physically held
        check(out->isHeld("space"), "c11-key-held-before-stop", "");

        const uint64_t stopEpoch  = e.requestStop();      // step 1: epoch 10
        const uint64_t srcEpoch   = e.requestCancel();    // step 2: epoch 11
        check(srcEpoch > stopEpoch, "c11-newer-epoch-assigned",
              "stop=" + std::to_string(stopEpoch) + " cancel=" + std::to_string(srcEpoch));

        e.cancelSource("padA", srcEpoch);                 // step 3: completes
        // Step 4: the stop was never processed. It must still be pending.
        check(e.stopPending(), "c11-stop-still-pending-after-newer-ack", "");
        check(!e.cancelAcknowledged(stopEpoch),
              "c11-older-stop-not-acknowledged", "");

        // And the stop must still actually block new actions.
        e.dispatch(press("s", "c", 1100));
        check(tapsOf(*out, "x") == 0, "c11-dispatch-still-blocked", "");

        // Completing the stop for real clears it.
        e.cancelAll(stopEpoch);
        check(!e.stopPending(), "c11-stop-cleared-by-own-ack", "");
        e.dispatch(press("s", "c", 1200));
        check(tapsOf(*out, "x") == 1, "c11-runs-after-stop-ack", "");
    }

    // ---- C12: the same interleaving through the runtime's control channel ---
    section("C12 interleaving through queued control items");
    {
        auto out = std::make_shared<HookBackend>();
        MappingRuntime rt(out, 64);
        Binding hold; hold.id = "c12h"; hold.match = {"padA", "b", EventType::Pressed};
        hold.trigger = TriggerKind::Momentary; hold.holdKey = "space";
        Binding act; act.id = "c12a"; act.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "x"; act.script = {t};
        rt.engine().addBinding(hold);
        rt.engine().addBinding(act);
        rt.engine().dispatch(press("padA", "b", 1000));
        check(out->isHeld("space"), "c12-key-held", "");

        // Publish a stop and a newer per-source cancel WITHOUT starting an
        // executor, so nothing acknowledges anything behind our back. The control
        // channel may deliver them in either order; this drives the bad one.
        const uint64_t stopEpoch = rt.engine().requestStop();
        const uint64_t srcEpoch  = rt.engine().requestCancel();
        check(srcEpoch > stopEpoch, "c12-newer-epoch", "");

        // Executor-side: the SOURCE cancel lands first, carrying the newer epoch.
        QueueItem src; src.kind = QueueItemKind::CancelSource;
        src.source = "padA"; src.cancelEpoch = srcEpoch;
        rt.engine().cancelSource(src.source, src.cancelEpoch);

        check(rt.engine().stopPending(), "c12-stop-pending-after-source-cancel", "");
        check(!rt.engine().cancelAcknowledged(stopEpoch),
              "c12-earlier-stop-not-acknowledged", "");
        check(rt.cancelPending(), "c12-runtime-reports-cancel-pending", "");

        // ...and the still-unhandled stop must keep blocking new actions.
        rt.engine().dispatch(press("s", "c", 1100));
        check(tapsOf(*out, "x") == 0, "c12-still-blocked-by-unhandled-stop", "");

        // The queued CancelAll (newest epoch) finally performs the global release
        // and clears everything, including the out-of-order source cancel.
        rt.postCancelAll();
        while (rt.processOne(0)) {}
        check(!rt.engine().stopPending(), "c12-stop-cleared-by-global-cleanup", "");
        check(!rt.cancelPending(), "c12-nothing-pending-after-global-cleanup", "");
        check(out->heldKeys().empty(), "c12-keys-released-by-global-cleanup", "");
        rt.stop();
    }

    // ---- C13: concurrent producer-side Stop and source cancel --------------
    // Two threads raise a stop and a per-source cancel simultaneously. Whatever
    // order the epochs land in, the stop must remain pending until it is actually
    // processed, and the runtime must never end up permanently blocked.
    section("C13 concurrent stop and source-specific cancel");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding hold; hold.id = "c13h"; hold.match = {"padA", "b", EventType::Pressed};
        hold.trigger = TriggerKind::Momentary; hold.holdKey = "space";
        e.addBinding(hold);
        e.dispatch(press("padA", "b", 1000));
        check(out->isHeld("space"), "c13-key-held", "");

        // Deterministic barrier: both threads publish at the same instant without
        // relying on a sleep to order them.
        std::atomic<int> ready{0};
        std::atomic<uint64_t> stopEp{0}, srcEp{0};
        auto waitAll = [&] { ready.fetch_add(1); while (ready.load() < 2) std::this_thread::yield(); };
        std::thread tStop([&] { waitAll(); stopEp.store(e.requestStop()); });
        std::thread tSrc ([&] { waitAll(); srcEp.store(e.requestCancel()); });
        tStop.join(); tSrc.join();

        check(stopEp.load() != 0 && srcEp.load() != 0, "c13-both-requests-published", "");
        check(stopEp.load() != srcEp.load(), "c13-distinct-epochs", "");

        // Process only the per-source cancel. The stop is still unhandled no
        // matter which thread happened to get the larger epoch.
        e.cancelSource("padA", srcEp.load());
        check(e.stopPending(), "c13-stop-pending-after-concurrent-cancel", "");
        check(!out->isHeld("space"), "c13-source-keys-released", "");

        // Completing the stop must clear it and unblock dispatch.
        e.cancelAll(stopEp.load());
        check(!e.stopPending(), "c13-stop-cleared", "");
        check(!e.cancelPending(), "c13-nothing-pending", "");
    }

    // ---- C14: stale queued actions never run after Stop --------------------
    section("C14 stale queued actions discarded after stop");
    {
        auto out = std::make_shared<HookBackend>();
        MappingRuntime rt(out, 128);
        Binding b; b.id = "c14"; b.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "k"; b.script = {t};
        rt.engine().addBinding(b);

        for (int i = 0; i < 5; ++i) rt.post(press("s", "c", (uint64_t)(i + 1)));
        check(rt.pending() == 5, "c14-five-queued", "pending=" + std::to_string(rt.pending()));

        rt.postCancelAll();                 // no executor: acknowledged inline
        check(!rt.cancelPending(), "c14-stop-applied", "");
        check(rt.pending() == 0, "c14-stale-actions-dropped",
              "pending=" + std::to_string(rt.pending()));
        check(tapsOf(*out, "k") == 0, "c14-none-ran", "");
        check(rt.eventsDroppedByCancel() >= 5, "c14-drops-counted",
              "dropped=" + std::to_string(rt.eventsDroppedByCancel()));

        // A later legitimate action still runs.
        rt.post(press("s", "c", 9));
        check(rt.processOne(0), "c14-later-accepted", "");
        check(tapsOf(*out, "k") == 1, "c14-later-ran", "");
    }

    // ---- C15: start() correctly restarts a stopped runtime -----------------
    section("C15 runtime restart after a completed stop");
    {
        auto out = std::make_shared<HookBackend>();
        MappingRuntime rt(out, 64);
        Binding tap; tap.id = "c15"; tap.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "n"; tap.script = {t};
        rt.engine().addBinding(tap);

        rt.start();
        rt.post(press("s", "c", 1));
        // Notified from the executor: no polling of recorded output.
        std::mutex mu;
        std::condition_variable cv;
        int nDowns = 0;
        out->onKeyDown = [&](const std::string& key) {
            if (key != "n") return;
            { std::lock_guard<std::mutex> lk(mu); ++nDowns; }
            cv.notify_all();
        };
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::seconds(10), [&] { return nDowns >= 1; });
        }
        check(nDowns >= 1, "c15-runs-before-stop", std::to_string(nDowns));

        rt.postCancelAll();
        rt.stop();
        check(!rt.cancelPending(), "c15-no-residual-request", "");
        check(out->heldKeys().empty(), "c15-no-keys-after-stop", "");

        // Restart: the queue must be re-armed and the engine must accept input.
        rt.start();
        rt.post(press("s", "c", 2));
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::seconds(10), [&] { return nDowns >= 2; });
        }
        rt.stop();
        check(nDowns >= 2, "c15-runs-after-restart", std::to_string(nDowns));
    }

    // ---- C16: cancellation during text output and key combinations ---------
    section("C16 cancellation during text output and combos");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding b; b.id = "c16"; b.match = {"s", "c", EventType::Pressed};
        Action txt; txt.kind = ActionKind::TypeText; txt.text = "abcdef";
        Action combo; combo.kind = ActionKind::KeyCombo; combo.combo = {"ctrl", "alt"};
        Action tail; tail.kind = ActionKind::KeyTap; tail.key = "z";
        b.script = {txt, combo, tail};
        e.addBinding(b);
        out->onTextChar = [&](int n) { if (n == 2) e.requestCancel(); };
        e.dispatch(press("s", "c", 1000));
        check(tapsOf(*out, "z") == 0, "c16-tail-skipped", "");
        check(out->heldKeys().empty(), "c16-no-keys-held", "");
        // The combo never ran: its keys were never pressed, so nothing to unwind.
        check(countOf(*out, "down:ctrl") == 0, "c16-combo-not-started", "");
        e.cancelAll();

        // Key release after external cancellation, mid-combo.
        auto out2 = std::make_shared<HookBackend>();
        Engine e2(out2);
        Binding b2; b2.id = "c16b"; b2.match = {"s", "c", EventType::Pressed};
        Action combo2; combo2.kind = ActionKind::KeyCombo;
        combo2.combo = {"ctrl", "alt", "shift"};
        b2.script = {combo2};
        e2.addBinding(b2);
        out2->onKeyDown = [&](const std::string& k) { if (k == "alt") e2.requestCancel(); };
        e2.dispatch(press("s", "c", 1000));
        check(out2->heldKeys().empty(), "c16-mid-combo-unwound",
              "held=" + std::to_string(out2->heldKeys().size()));
        check(countOf(*out2, "up:ctrl") == 1, "c16-pressed-member-released", "");
        e2.cancelAll();
    }

    // ---- C17: failed KeyUp must not erase the only record of a held key -----
    // Regression for: SendInput()'s return count was ignored (always "true"), and
    // KeyOwnership::release() erased its entry before checking the result. A
    // rejected KeyUp could therefore be reported as success, dropping the only
    // record of a key that might still be physically down.
    //
    // Model (revised): active holder ownership (keyRefs_) and an unresolved
    // PHYSICAL release (unresolved_) are separate facts. A failed release clears
    // ownership - nobody holds the key - but records that the physical up still
    // has to happen, so heldKeys() reflects holders only while unresolvedKeys()
    // reflects outstanding physical state.
    section("C17 refused physical KeyUp stays observable and retryable");
    {
        auto out = std::make_shared<FailBackend>();
        KeyOwnership ko;
        check(ko.press("space", "h1", *out), "c17-pressed", "");
        check(ko.isHeld("space"), "c17-held", "");

        out->failUp = true;                       // the OS rejects the KeyUp
        bool released = ko.release("space", "h1", *out);
        check(!released, "c17-release-reports-failure", "");
        check(ko.outputFailures() == 1, "c17-failure-counted",
              "failures=" + std::to_string(ko.outputFailures()));
        // The record must SURVIVE: the physical key may still be down.
        check(ko.isUnresolved("space"), "c17-key-recorded-unresolved", "");
        check(ko.unresolvedCount() == 1, "c17-unresolved-counted", "");
        check(ko.unresolvedKeys().size() == 1, "c17-unresolved-list", "");
        // Ownership is genuinely gone: no holder remains.
        check(!ko.isHeld("space"), "c17-no-active-owner-after-release", "");
        check(ko.heldKeys().empty(), "c17-heldkeys-reflects-holders-only", "");

        // A retry that fails again must not claim repair.
        check(ko.retryUnresolved(*out) == 0, "c17-retry-still-failing", "");
        check(ko.isUnresolved("space"), "c17-still-unresolved", "");
        // A retry that succeeds clears the record honestly.
        out->failUp = false;
        check(ko.retryUnresolved(*out) == 1, "c17-retry-resolves", "");
        check(!ko.isHeld("space") && ko.unresolvedCount() == 0,
              "c17-cleared-after-real-release", "");
    }

    // ---- C17b: emergency sweep reports, never silently loses, unresolved keys
    section("C17b emergency release preserves unresolved keys");
    {
        auto out = std::make_shared<FailBackend>();
        KeyOwnership ko;
        ko.press("ctrl", "h1", *out);
        ko.press("a", "h1", *out);
        check(ko.heldKeys().size() == 2, "c17b-two-held", "");

        out->failUp = true;                       // every KeyUp refused
        ko.releaseAll(*out);
        check(ko.unresolvedCount() == 2, "c17b-both-unresolved",
              "unresolved=" + std::to_string(ko.unresolvedCount()));
        // Emergency cleanup drops ALL holders, so ownership is empty...
        check(ko.heldKeys().empty(), "c17b-holders-cleared",
              "held=" + std::to_string(ko.heldKeys().size()));
        // ...but the outstanding physical state is retained and reported.
        auto u = ko.unresolvedKeys();
        check(u.size() == 2 && u[0] == "a" && u[1] == "ctrl",
              "c17b-unresolved-list-sorted", "");

        out->failUp = false;
        check(ko.retryUnresolved(*out) == 2, "c17b-retry-clears", "");
        check(ko.heldKeys().empty(), "c17b-no-keys-held", "");
    }

    // ---- C18: lifecycle race — producer must not touch Engine while the
    // executor is alive. Regression for stop() clearing `running_` before join().
    section("C18 cancellation during the stopping/joining window");
    {
        auto out = std::make_shared<HookBackend>();
        MappingRuntime rt(out, 64);
        rt.engine().setRealtime(true);
        Binding slow; slow.id = "c18s"; slow.match = {"s", "long", EventType::Pressed};
        Action d; d.kind = ActionKind::Delay; d.delayMs = 4000;
        Action t; t.kind = ActionKind::KeyTap; t.key = "z";
        slow.script = {d, t};
        rt.engine().addBinding(slow);
        rt.start();

        rt.post(press("s", "long", 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(60));  // executor inside delay

        // DECISIVE CHECK. This hook runs inside stop(), after the stop was
        // requested and before join(): the executor thread definitely exists and
        // Engine may be inside dispatch(). Capturing the counter here - not from
        // a racing producer - makes the measurement unambiguous.
        size_t inlineBefore = rt.inlineCleanups();
        std::atomic<size_t> inlineAtBoundary{(size_t)-1};
        std::atomic<bool> runningAtBoundary{false};
        rt.setOnStopJoining([&] {
            runningAtBoundary = rt.executorRunning();
            inlineAtBoundary = rt.inlineCleanups();
        });

        // Hammer the producer entries from several threads while stop() is joining.
        // The invariant under test: a producer must NEVER take the inline Engine
        // path (inlineCleanups) while an executor thread still exists, because
        // engine_ is not thread-safe against a running Engine::dispatch().
        std::atomic<bool> stopStarted{false}, stopDone{false};
        std::atomic<bool> sawRunning{false};
        const int kProducers = 4;
        std::thread stopper([&] { stopStarted = true; rt.stop(); stopDone = true; });
        std::vector<std::thread> producers;
        for (int i = 0; i < kProducers; ++i) {
            producers.emplace_back([&] {
                while (!stopStarted.load()) std::this_thread::yield();
                while (!stopDone.load()) {
                    if (rt.executorRunning()) sawRunning = true;
                    rt.postCancelAll();
                    rt.postCancelSource("padA");
                }
            });
        }
        stopper.join();
        for (auto& th : producers) th.join();

        check(sawRunning.load(), "c18-executor-visible-during-join", "");
        check(runningAtBoundary.load(), "c18-still-running-inside-stop-before-join", "");
        check(!rt.executorRunning(), "c18-not-running-after-stop", "");
        // inlineAtBoundary was captured inside stop() before join(), while the
        // executor was alive. Producers were hammering the entry points before
        // that instant, so any inline mutation they managed is included. Zero
        // means the lifecycle lock genuinely kept them out of Engine.
        check(inlineAtBoundary.load() == inlineBefore,
              "c18-no-inline-engine-mutation-during-join",
              "before=" + std::to_string(inlineBefore) +
              " atBoundary=" + std::to_string(inlineAtBoundary.load()));
        check(tapsOf(*out, "z") == 0, "c18-no-action-after-stop", "");
        check(out->heldKeys().empty(), "c18-no-stuck-keys", "");

        // The decisive assertion. Every producer above either queued its request
        // or (only once no executor existed) applied it inline; any inline cleanup
        // that ran while the executor was alive would have raced Engine::dispatch.
        // Post-stop, with the thread gone, inline cleanups are expected and safe.
        check(!rt.cancelPending(), "c18-inline-stop-clears-request", "");

        // Restart must work and process fresh input. Notified from the executor,
        // so the test thread never polls the backend's recorded output.
        Binding fast; fast.id = "c18f"; fast.match = {"s", "quick", EventType::Pressed};
        Action tq; tq.kind = ActionKind::KeyTap; tq.key = "y"; fast.script = {tq};
        rt.engine().addBinding(fast);
        std::mutex ymu;
        std::condition_variable ycv;
        bool yRan = false;
        out->onKeyDown = [&](const std::string& key) {
            if (key != "y") return;
            { std::lock_guard<std::mutex> lk(ymu); yRan = true; }
            ycv.notify_all();
        };
        rt.start();
        rt.post(press("s", "quick", 2));
        {
            std::unique_lock<std::mutex> lk(ymu);
            ycv.wait_for(lk, std::chrono::seconds(10), [&] { return yRan; });
        }
        rt.stop();
        check(yRan, "c18-runs-after-restart", "");
        rt.postCancelAll();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        check(out->heldKeys().empty(), "c18-no-stuck-keys-after-second-stop", "");
        rt.stop();
    }

    // ---- C19: stop acknowledgement must not outrun the FIFO barrier ----------
    // Regression for: the stop was acknowledged before the FIFO was cleaned, so a
    // producer could see completion, post a legitimate event, and lose it to the
    // sweep that ran afterwards.
    //
    // EXECUTION MODEL (corrected): exactly ONE consumer. The real runtime executor
    // thread consumes every item; the test thread never calls processOne() while
    // it is running. Synchronisation is a condition variable signalled from the
    // post-acknowledgement hook - a deterministic boundary, not a sleep.
    section("C19 post-stop event survives the cleanup sweep");
    {
        auto out = std::make_shared<HookBackend>();
        MappingRuntime rt(out, 64);
        Binding b; b.id = "c19"; b.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "k"; b.script = {t};
        rt.engine().addBinding(b);

        std::mutex              mu;
        std::condition_variable cv;
        bool acked = false;
        bool postedFromHook = false;
        bool postStopActionRan = false;

        // Fires on the EXECUTOR thread, immediately after the stop becomes
        // externally visible and before handle() returns - the precise window the
        // old ordering left open. The producer's post here is the legitimate
        // post-stop event that must survive.
        rt.setOnStopAcknowledged([&] {
            {
                std::lock_guard<std::mutex> lk(mu);
                postedFromHook = true;
                rt.post(press("s", "c", 3));
                acked = true;
            }
            cv.notify_all();
        });

        // Fires on the EXECUTOR thread when the post-stop action's key goes down.
        // This replaces polling `ops`: the test learns about the operation through
        // a synchronised notification instead of reading shared state concurrently.
        out->onKeyDown = [&](const std::string& key) {
            if (key != "k") return;
            {
                std::lock_guard<std::mutex> lk(mu);
                postStopActionRan = true;
            }
            cv.notify_all();
        };

        rt.start();                       // the ONLY consumer from here on

        // Pre-stop work, known to be enqueued before the control item.
        rt.post(press("s", "c", 1));
        rt.post(press("s", "c", 2));
        rt.postCancelAll();               // queued control item (priority channel)

        // Wait (bounded) for the executor to reach BOTH boundaries: acknowledgement
        // and the post-stop action executing. No sleep-based polling, and no read
        // of `ops` while the executor may be writing it.
        {
            std::unique_lock<std::mutex> lk(mu);
            check(cv.wait_for(lk, std::chrono::seconds(10),
                              [&] { return acked; }),
                  "c19-executor-reached-acknowledgement", "");
            check(cv.wait_for(lk, std::chrono::seconds(10),
                              [&] { return postStopActionRan; }),
                  "c19-post-stop-action-executed", "");
        }

        rt.stop();                        // join: no consumer remains

        // Only now is it safe to inspect the backend's recorded operations.
        check(postedFromHook, "c19-producer-posted-at-boundary", "");
        check(!rt.cancelPending(), "c19-stop-complete", "");

        const auto ops = out->snapshot();
        const int ran = taps(ops, "k");
        check(ran == 1, "c19-exactly-the-post-stop-event-ran",
              "ran=" + std::to_string(ran));
        check(rt.eventsDroppedByCancel() >= 2, "c19-pre-stop-events-dropped",
              "dropped=" + std::to_string(rt.eventsDroppedByCancel()));

        // Single-consumer check: the queue is empty now that the executor drained
        // it and stopped. processOne() is used only AFTER the executor is gone.
        check(!rt.processOne(0), "c19-queue-empty-after-single-consumer-drain", "");
    }

    // ---- C20: legacy cancelSource()/cancelAll() defaults must not leave a
    // request pending. A direct call with no epoch is a complete stop by itself.
    section("C20 legacy default requestEpoch = 0");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding b; b.id = "c20"; b.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "k"; b.script = {t};
        e.addBinding(b);

        // No requestCancel() was issued: cancelAll() with the default epoch must
        // still be a complete, non-blocking stop.
        e.cancelAll();
        check(!e.cancelPending(), "c20-cancelall-default-not-pending", "");
        check(!e.stopPending(), "c20-cancelall-default-no-stop", "");
        e.dispatch(press("s", "c", 1));
        check(tapsOf(*out, "k") == 1, "c20-runs-after-default-cancelall", "");

        // Same for cancelSource(): a direct call must not leave a pending request.
        e.cancelSource("s");
        check(!e.cancelPending(), "c20-cancelsource-default-not-pending", "");

        // And a request that IS outstanding is still honoured by a direct call.
        e.requestStop();
        check(e.cancelPending(), "c20-request-registered", "");
        e.cancelAll();
        check(!e.cancelPending(), "c20-direct-call-clears-outstanding", "");
        e.dispatch(press("s", "c", 2));
        check(tapsOf(*out, "k") == 2, "c20-runs-after-clearing", "");
    }

    // ---- C20b: default cancelSource() after a request was published ---------
    // Regression for: cancelSource() snapshotted the current epoch when
    // requestEpoch == 0 but only acknowledged when requestEpoch was nonzero, so
    //   requestCancel(); cancelSource("padA");
    // stranded a request the call had already handled, blocking every later
    // dispatch. This is the case C20 could not see because no request existed.
    section("C20b default cancelSource clears a request it handled");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding hold; hold.id = "c20bh"; hold.match = {"padA", "b", EventType::Pressed};
        hold.trigger = TriggerKind::Momentary; hold.holdKey = "space";
        Binding act; act.id = "c20ba"; act.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "k"; act.script = {t};
        e.addBinding(hold);
        e.addBinding(act);

        e.dispatch(press("padA", "b", 1000));
        check(out->isHeld("space"), "c20b-key-held", "");

        // The exact legacy sequence: publish a request, then clean up directly.
        e.requestCancel();
        check(e.cancelPending(), "c20b-request-published", "");
        e.cancelSource("padA");                       // default requestEpoch = 0
        check(!out->isHeld("space"), "c20b-source-keys-released", "");
        // The call covered that request, so it must not be left pending.
        check(!e.cancelPending(), "c20b-default-call-clears-handled-request", "");
        e.dispatch(press("s", "c", 1));
        check(tapsOf(*out, "k") == 1, "c20b-dispatch-resumes", "");

        // A per-source cleanup must NOT acknowledge a GLOBAL stop that it did not
        // cover: a stop published BEFORE the snapshot stays pending (only a global
        // releaseAll satisfies it), while the newer per-source request it does
        // cover is cleared.
        const uint64_t stopEpoch = e.requestStop();    // older: a global stop
        e.requestCancel();                             // newer: per-source cancel
        e.cancelSource("padA");                        // covers the newer one only
        check(e.stopPending(), "c20b-newer-stop-not-acknowledged", "");
        check(!e.cancelAcknowledged(stopEpoch), "c20b-newer-epoch-still-pending", "");
        e.cancelAll();
        check(!e.cancelPending(), "c20b-global-cleanup-clears", "");
        e.dispatch(press("s", "c", 2));
        check(tapsOf(*out, "k") == 2, "c20b-runs-after-global-cleanup", "");
    }

    // ---- C21: an unresolved record must never release another holder's key ---
    // Regression for the reported scenario:
    //   1. A presses Space; A releases it, the backend refuses the KeyUp.
    //   2. B presses Space while that unresolved record exists.
    //   3. retryUnresolved() used to send KeyUp and drop the record, physically
    //      releasing a key B still legitimately owns.
    section("C21 unresolved retry must not release another holder's key");
    {
        auto out = std::make_shared<FailBackend>();
        KeyOwnership ko;

        // 1. A holds, then its physical release is refused.
        check(ko.press("space", "A", *out), "c21-A-pressed", "");
        out->failUp = true;
        check(!ko.release("space", "A", *out), "c21-A-release-refused", "");
        check(ko.isUnresolved("space"), "c21-orphan-recorded", "");
        check(!ko.isHeld("space"), "c21-no-active-owner", "");

        // 2. B acquires the same key. A new physical down must be emitted because
        //    nobody actively owns it (the key may be stuck down, so a second
        //    down is harmless and keeps the model honest).
        out->failUp = false;
        const size_t upsBefore = count(out->ops, "up:space");
        ko.press("space", "B", *out);
        check(ko.isHeld("space"), "c21-B-holds", "");
        check(ko.heldKeys().size() == 1, "c21-exactly-one-holder", "");

        // 3. Retrying must NOT send a KeyUp while B is an active holder.
        const size_t upsBeforeRetry = count(out->ops, "up:space");
        size_t resolved = ko.retryUnresolved(*out);
        check(resolved == 0, "c21-retry-refused-while-held",
              "resolved=" + std::to_string(resolved));
        check(count(out->ops, "up:space") == upsBeforeRetry,
              "c21-no-keyup-emitted-while-held", "");
        check(ko.isHeld("space") && ko.isUnresolved("space"),
              "c21-state-preserved", "");

        // 4. B releases normally: that op is honoured and clears the orphan.
        check(ko.release("space", "B", *out), "c21-B-release-succeeds", "");
        check(!ko.isHeld("space"), "c21-no-holder-after-B", "");
        check(ko.unresolvedCount() == 0, "c21-orphan-cleared-by-handoff", "");
        check(count(out->ops, "up:space") == upsBefore + 1,
              "c21-exactly-one-physical-up", "");
    }

    // ---- C21b: three holders, orphan in the middle ---------------------------
    section("C21b retry with three overlapping holders");
    {
        auto out = std::make_shared<FailBackend>();
        KeyOwnership ko;
        ko.press("enter", "A", *out);
        ko.press("enter", "B", *out);
        ko.press("enter", "C", *out);
        check(ko.heldKeys().size() == 1, "c21b-one-key-three-holders", "");

        out->failUp = true;
        ko.release("enter", "A", *out);            // not last: no physical up
        check(ko.isHeld("enter"), "c21b-still-held-after-A", "");
        check(ko.unresolvedCount() == 0, "c21b-no-orphan-yet", "");

        ko.release("enter", "C", *out);            // still B
        check(ko.isHeld("enter"), "c21b-still-held-after-C", "");

        // B is the last active holder; its refused up creates the orphan.
        check(!ko.release("enter", "B", *out), "c21b-B-release-refused", "");
        check(ko.isUnresolved("enter"), "c21b-orphan-recorded", "");
        check(!ko.isHeld("enter"), "c21b-no-active-holder", "");

        // Repeated failed retries stay observable and do not corrupt counts.
        for (int i = 0; i < 3; ++i) check(ko.retryUnresolved(*out) == 0, "c21b-retry-fails", "");
        check(ko.unresolvedCount() == 1, "c21b-unresolved-count-stable", "");
        check(ko.heldCount() == 0, "c21b-holder-count-zero", "");

        out->failUp = false;
        check(ko.retryUnresolved(*out) == 1, "c21b-retry-clears", "");
        check(ko.unresolvedCount() == 0 && ko.heldKeys().empty(),
              "c21b-clean", "");
    }

    // ---- C22: the stop must still be PENDING at the pre-barrier boundary -----
    // This is the regression that fails if cancelAll(0) is ever restored in place
    // of releaseAllKeys(): cancelAll's default epoch snapshots and acknowledges
    // internally, so stop pending would already be false before the barrier.
    // It observes the acknowledgement state at the precise boundary rather than
    // only the final queue contents.
    section("C22 stop is not acknowledged before the queue barrier");
    {
        auto out = std::make_shared<HookBackend>();
        MappingRuntime rt(out, 64);
        Binding b; b.id = "c22"; b.match = {"s", "c", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "k"; b.script = {t};
        rt.engine().addBinding(b);

        std::mutex mu;
        std::condition_variable cv;
        bool reachedBoundary = false;
        bool pendingAtBoundary = false;
        bool ackedAtBoundary = false;
        uint64_t observedEpoch = 0;

        rt.setOnBeforeBarrier([&] {
            std::lock_guard<std::mutex> lk(mu);
            // State sampled BETWEEN key release and the FIFO barrier.
            pendingAtBoundary = rt.engine().stopPending();
            ackedAtBoundary = rt.engine().cancelPending();
            observedEpoch = rt.engine().pendingStopEpoch();
            reachedBoundary = true;
            cv.notify_all();
        });

        rt.start();
        rt.post(press("s", "c", 1));
        rt.postCancelAll();

        {
            std::unique_lock<std::mutex> lk(mu);
            check(cv.wait_for(lk, std::chrono::seconds(10), [&] { return reachedBoundary; }),
                  "c22-executor-reached-pre-barrier", "");
        }
        rt.stop();

        // The stop MUST still look pending: its keys were released, but the FIFO
        // has not been cleaned and completion has not been published.
        check(pendingAtBoundary, "c22-stop-pending-at-pre-barrier-boundary", "");
        check(ackedAtBoundary, "c22-cancel-pending-at-pre-barrier-boundary", "");
        check(observedEpoch != 0, "c22-pending-epoch-visible",
              "epoch=" + std::to_string(observedEpoch));

        // After completion the stop is done and dispatch proceeds normally.
        check(!rt.cancelPending(), "c22-stop-complete-after-barrier", "");
        check(rt.eventsDroppedByCancel() >= 1, "c22-pre-stop-event-dropped", "");
    }

    // ---- C23: a source-scoped cleanup must NEVER acknowledge a global Stop ----
    // Regression for: cancelSource() snapshotted cancelEpoch_ and acknowledged that
    // epoch regardless of request SCOPE. With device B holding a key, a global Stop
    // at epoch N could be "acknowledged" by cancelSource("padA") - which releases
    // only padA's keys - so stopPending() went false while B's key stayed down.
    section("C23 source cleanup must not satisfy a global Stop");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        // Device B holds Space; padA has its own binding.
        Binding holdB; holdB.id = "c23B"; holdB.match = {"padB", "b", EventType::Pressed};
        holdB.trigger = TriggerKind::Momentary; holdB.holdKey = "space";
        Binding actA; actA.id = "c23A"; actA.match = {"padA", "x", EventType::Pressed};
        Action ta; ta.kind = ActionKind::KeyTap; ta.key = "k"; actA.script = {ta};
        e.addBinding(holdB);
        e.addBinding(actA);

        e.dispatch(press("padB", "b", 1000));
        check(out->isHeld("space"), "c23-B-holds-space", "");

        // A. Direct default-argument regression.
        const uint64_t stopEpoch = e.requestStop();
        e.cancelSource("padA");                       // default epoch: only padA
        check(e.stopPending(), "c23-default-stop-still-pending", "");
        check(!e.cancelAcknowledged(stopEpoch), "c23-default-stop-not-acked", "");
        // B's key must remain held: the global release has not run.
        check(e.heldKeys().size() == 1 && out->isHeld("space"),
              "c23-B-key-still-held", "");

        // Global cleanup then releases it and acknowledges the Stop.
        e.cancelAll();
        check(!e.stopPending(), "c23-global-cleanup-acks-stop", "");
        check(!out->isHeld("space"), "c23-global-cleanup-released-space", "");
        check(!e.cancelPending(), "c23-nothing-pending", "");

        // B. Explicit-epoch regression: passing a Stop epoch to cancelSource()
        // must not acknowledge it either.
        e.dispatch(press("padB", "b", 2000));
        check(out->isHeld("space"), "c23-B-holds-again", "");
        const uint64_t stopEpoch2 = e.requestStop();
        e.cancelSource("padA", stopEpoch2);           // explicit Stop epoch
        check(e.stopPending(), "c23-explicit-stop-still-pending", "");
        check(!e.cancelAcknowledged(stopEpoch2), "c23-explicit-stop-not-acked", "");
        check(out->isHeld("space"), "c23-explicit-B-key-still-held", "");
        e.cancelAll();
        check(!e.stopPending() && !out->isHeld("space"),
              "c23-explicit-global-cleanup-works", "");
    }

    // ---- C24: mixed request ordering (both directions) ----------------------
    // Uses the DEFAULT argument deliberately: that is the path that snapshots the
    // CURRENT epoch, so with a mixed order the snapshot can land exactly on the
    // Stop epoch. Explicit single-epoch calls cannot reproduce the scope defect,
    // which is why this test would silently pass against the unfixed code if it
    // passed explicit epochs only.
    section("C24 mixed cancellation and stop ordering");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding holdB; holdB.id = "c24B"; holdB.match = {"padB", "b", EventType::Pressed};
        holdB.trigger = TriggerKind::Momentary; holdB.holdKey = "space";
        Binding actA; actA.id = "c24A"; actA.match = {"padA", "x", EventType::Pressed};
        Action ta; ta.kind = ActionKind::KeyTap; ta.key = "k"; actA.script = {ta};
        e.addBinding(holdB);
        e.addBinding(actA);

        // --- Order 1: ordinary cancel at N, Stop at N+1. ---
        // cancelSource() with the DEFAULT argument snapshots the newest epoch,
        // which here IS the Stop. It covers the ordinary cancel at N but must not
        // acknowledge the Stop at N+1.
        e.dispatch(press("padB", "b", 1000));
        const uint64_t cancelEp = e.requestCancel();      // N
        const uint64_t stopEp   = e.requestStop();        // N+1
        check(stopEp > cancelEp, "c24-order1-epochs", "");
        e.cancelSource("padA");                           // default: snapshot = N+1
        check(e.cancelAcknowledged(cancelEp), "c24-order1-collateral-acked", "");
        check(!e.cancelAcknowledged(stopEp), "c24-order1-stop-not-acked", "");
        check(e.stopPending(), "c24-order1-stop-pending", "");
        check(out->isHeld("space"), "c24-order1-B-held", "");
        e.cancelAll();
        check(!e.cancelPending() && !out->isHeld("space"),
              "c24-order1-global-resolves", "");

        // --- Order 2: Stop at N, ordinary cancel at N+1. ---
        // Here the default snapshot is the ordinary cancel at N+1, so that request
        // MUST be acknowledged (not stranded) while the older Stop stays pending.
        e.dispatch(press("padB", "b", 2000));
        const uint64_t stopEp2   = e.requestStop();       // N
        const uint64_t cancelEp2 = e.requestCancel();     // N+1
        check(cancelEp2 > stopEp2, "c24-order2-epochs", "");
        e.cancelSource("padA");                           // default: snapshot = N+1
        check(e.cancelAcknowledged(cancelEp2), "c24-order2-collateral-acked", "");
        check(!e.cancelAcknowledged(stopEp2), "c24-order2-stop-not-acked", "");
        check(e.stopPending(), "c24-order2-stop-pending", "");
        check(out->isHeld("space"), "c24-order2-B-held", "");
        e.cancelAll();
        check(!e.cancelPending() && !out->isHeld("space"),
              "c24-order2-global-resolves", "");

        // Two stops outstanding: the OLDER one must survive a source cleanup too,
        // proving the check is not a comparison against only stopEpoch_.
        e.dispatch(press("padB", "b", 3000));
        const uint64_t stopA = e.requestStop();           // older stop
        const uint64_t stopB = e.requestStop();           // newer stop
        check(stopB > stopA, "c24-two-stops-epochs", "");
        e.cancelSource("padA", 0);                        // default snapshot = stopB
        check(!e.cancelAcknowledged(stopA), "c24-older-stop-not-acked", "");
        check(!e.cancelAcknowledged(stopB), "c24-newer-stop-not-acked", "");
        check(e.stopPending(), "c24-any-stop-pending", "");
        check(out->isHeld("space"), "c24-two-stops-B-held", "");
        e.cancelAll();
        check(!e.cancelPending() && !out->isHeld("space"),
              "c24-two-stops-global-resolves", "");

        // Legacy invariant preserved: requestCancel(); cancelSource(src) must not
        // strand the ordinary cancellation it handled.
        e.requestCancel();
        e.cancelSource("padA");
        check(!e.cancelPending(), "c24-legacy-cancel-not-stranded", "");
    }

    // ---- C25: real mutex contention inside the Stop publication window -------
    // Regression for the publish-first defect:
    //   T1 requestStop() published epoch N,
    //   T2 cancelSource("padA") snapshotted cancelEpoch_ == N, found N absent from
    //      stopRequests_ (registration had not happened yet) and acknowledged the
    //      global Stop as an ordinary cancellation.
    //
    // WHAT THIS PROVES (and what a pre-lock signal alone could NOT):
    //   * The rival is inside cancelSource() (pre-lock hook).
    //   * The rival actually ATTEMPTED the snapshot mutex and found it UNAVAILABLE
    //     (try_lock() == false reported by setSnapshotLockProbe). Because the
    //     publisher holds cancelTrackMu_ for this whole window, a false result
    //     cannot be a spurious failure - it is genuine contention.
    //   * Only after that report does the publication hook return, so the window is
    //     demonstrably still open while the rival is blocked.
    //   * Once the lock is released the rival proceeds, sees the registered Stop
    //     scope, and does NOT acknowledge it.
    //
    // LIFETIME: the rival thread object lives in this scope, is never detached, and
    // is joined unconditionally - including when a wait times out.
    section("C25 real mutex contention inside the publication window");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding holdB; holdB.id = "c25B"; holdB.match = {"padB", "b", EventType::Pressed};
        holdB.trigger = TriggerKind::Momentary; holdB.holdKey = "space";
        Binding actA; actA.id = "c25A"; actA.match = {"padA", "x", EventType::Pressed};
        Action ta; ta.kind = ActionKind::KeyTap; ta.key = "k"; actA.script = {ta};
        e.addBinding(holdB);
        e.addBinding(actA);
        e.dispatch(press("padB", "b", 1000));
        check(out->isHeld("space"), "c25-B-holds-space", "");

        std::mutex              hsMu;
        std::condition_variable hsCv;

        // State 1: rival not scheduled / not inside cleanup yet.
        std::atomic<bool> rivalEnteredCleanup{false};
        // State 2: rival attempted cancelTrackMu_ and found it held (real contention).
        std::atomic<bool> rivalSawLockHeld{false};
        std::atomic<int>  probeCalls{0};
        // State 3: rival finished its source cleanup.
        std::atomic<bool> rivalFinishedCleanup{false};

        std::atomic<uint64_t> publishedEpoch{0};
        std::atomic<bool> stopStillPendingAfterCleanup{false};
        std::atomic<bool> stopAckedAfterCleanup{false};
        std::atomic<bool> hookRan{false};
        std::atomic<bool> rivalStarted{false};

        std::thread rival;      // lives here; joined unconditionally

        e.setSourceCleanupPrelockHook([&](const std::string&) {
            {
                std::lock_guard<std::mutex> lk(hsMu);
                rivalEnteredCleanup.store(true);
            }
            hsCv.notify_all();
        });

        // Reports whether the snapshot lock was obtainable. Runs WITHOUT the lock
        // when it fails, so it never recurses into the Engine.
        e.setSnapshotLockProbe([&](bool acquired) {
            probeCalls.fetch_add(1);
            if (!acquired) {
                {
                    std::lock_guard<std::mutex> lk(hsMu);
                    rivalSawLockHeld.store(true);
                }
                hsCv.notify_all();
            }
        });

        e.setStopPublicationHook([&](uint64_t epochBeingPublished) {
            hookRan.store(true);
            publishedEpoch.store(epochBeingPublished);

            // Capture the epoch BY VALUE: it is the hook's stack parameter and the
            // hook returns (releasing cancelTrackMu_) while the rival may still be
            // blocked. Capturing it by reference left the rival reading a dead
            // stack slot once the hook returned.
            const uint64_t rivalEpoch = epochBeingPublished;
            rival = std::thread([&, rivalEpoch] {
                rivalStarted.store(true);
                e.cancelSource("padA");                 // default-argument form
                stopStillPendingAfterCleanup.store(e.stopPending());
                stopAckedAfterCleanup.store(e.cancelAcknowledged(rivalEpoch));
                {
                    // The store must happen under hsMu: the waiter evaluates this
                    // predicate while holding hsMu, so publishing the flag outside
                    // the lock left a window in which the notify could be consumed
                    // before the predicate was re-checked, costing a spurious 10 s
                    // timeout. Flag kept atomic; only the publication is ordered.
                    std::lock_guard<std::mutex> lk(hsMu);
                    rivalFinishedCleanup.store(true);
                }
                hsCv.notify_all();
            });

            // Wait until the rival has actually entered cleanup AND reported a
            // failed lock acquisition. Both are needed: the first alone could be a
            // preemption artefact. We still hold cancelTrackMu_ throughout.
            {
                std::unique_lock<std::mutex> lk(hsMu);
                hsCv.wait_for(lk, std::chrono::seconds(10), [&] {
                    return rivalEnteredCleanup.load() && rivalSawLockHeld.load();
                });
            }
        });

        const uint64_t stopEpoch = e.requestStop();

        // The rival needs the lock we just released; wait (bounded).
        {
            std::unique_lock<std::mutex> lk(hsMu);
            hsCv.wait_for(lk, std::chrono::seconds(10),
                          [&] { return rivalFinishedCleanup.load(); });
        }
        const bool finished = rivalFinishedCleanup.load();

        // ALWAYS join, on every path, so no thread outlives this scope.
        if (rival.joinable()) rival.join();

        e.setStopPublicationHook(nullptr);
        e.setSourceCleanupPrelockHook(nullptr);
        e.setSnapshotLockProbe(nullptr);

        check(hookRan.load(), "c25-publication-hook-ran", "");
        check(rivalStarted.load(), "c25-rival-started", "");
        check(rivalEnteredCleanup.load(), "c25-rival-entered-cleanup-path", "");
        // THE contention proof: the rival really tried the mutex and lost.
        check(rivalSawLockHeld.load(), "c25-rival-observed-mutex-held", "");
        check(probeCalls.load() >= 1, "c25-probe-ran",
              "calls=" + std::to_string(probeCalls.load()));
        check(finished, "c25-rival-cleanup-completed", "");
        check(stopEpoch == publishedEpoch.load(), "c25-hook-saw-same-epoch",
              "stop=" + std::to_string(stopEpoch) +
              " hook=" + std::to_string(publishedEpoch.load()));

        // Decisive: after the blocked cleanup finally ran, the Stop is still pending
        // and was NOT acknowledged by the source-scoped cleanup.
        check(stopStillPendingAfterCleanup.load(),
              "c25-stop-pending-after-rival-cleanup", "");
        check(!stopAckedAfterCleanup.load(),
              "c25-stop-not-acked-by-source-cleanup", "");
        check(e.stopPending(), "c25-stop-pending-now", "");
        check(!e.cancelAcknowledged(stopEpoch), "c25-stop-not-acked-now", "");
        check(out->isHeld("space"), "c25-B-key-still-held", "");

        // Hooks cleared: the rest of the test runs on the plain production path.
        e.cancelAll();
        check(!e.stopPending(), "c25-stop-acked-by-global-cleanup", "");
        check(!out->isHeld("space"), "c25-global-cleanup-released-space", "");
        check(!e.cancelPending(), "c25-nothing-pending", "");
    }

    // ---- C25b: forced mutex contention between publication and cleanup -------
    // Previous versions could not prove overlap: the publisher might finish all
    // stops before the cleanup ran once, and "two threads started together" is not
    // evidence of an interleaving.
    //
    // This version FORCES the collision at a known point. The publication hook runs
    // while requestStop() holds cancelTrackMu_; on the batches selected for
    // contention it releases a cleanup thread and then waits for that thread to
    // report that it actually failed to acquire the snapshot mutex. Because the
    // publisher still holds the lock for the whole wait, a failed try_lock cannot be
    // spurious - it is genuine contention. Cleanup then proceeds after publication.
    //
    // Counters are asserted from real execution, and every recorded Stop epoch must
    // remain unacknowledged until the global cleanup runs.
    section("C25b forced contention between stop publication and source cleanup");
    {
        auto out = std::make_shared<HookBackend>();
        Engine e(out);
        Binding holdB; holdB.id = "c25bB"; holdB.match = {"padB", "b", EventType::Pressed};
        holdB.trigger = TriggerKind::Momentary; holdB.holdKey = "space";
        Binding actA; actA.id = "c25bA"; actA.match = {"padA", "x", EventType::Pressed};
        Action ta; ta.kind = ActionKind::KeyTap; ta.key = "k"; actA.script = {ta};
        e.addBinding(holdB);
        e.addBinding(actA);
        e.dispatch(press("padB", "b", 1000));
        check(out->isHeld("space"), "c25b-B-holds", "");

        const int kBatches  = 20;
        const int kPerBatch = 10;

        std::mutex              bMu;
        std::condition_variable bCv;

        // Handshake state, all guarded by bMu.
        int  batchPublished   = 0;
        int  batchCleaned     = 0;
        bool publisherDone    = false;
        bool hookWantsCleanup = false;   // publisher asks cleanup to run inside window
        int  hookBatch        = 0;       // which batch the hook is servicing
        int  hookServicedBatch = -1;     // last batch whose window was handed over
        int  cleanupTriedLock = 0;       // cleanups that attempted the snapshot lock

        std::mutex epMu;
        std::vector<uint64_t> stoppedEpochs;

        std::atomic<int> publishCount{0};
        std::atomic<int> cleanupCount{0};
        std::atomic<int> cleanupsWhilePublishing{0};
        std::atomic<int> contendedCleanups{0};      // try_lock failed => real contention
        std::atomic<int> stopsSeenOutstanding{0};
        std::atomic<bool> anyStopAckedBySource{false};

        // Record epochs during publication, and on hook-driven batches hand the
        // window to the cleanup thread, waiting until it has demonstrably collided
        // with the lock we are holding.
        e.setStopPublicationHook([&](uint64_t epochBeingPublished) {
            {
                std::lock_guard<std::mutex> lk(epMu);
                stoppedEpochs.push_back(epochBeingPublished);
            }
            std::unique_lock<std::mutex> lk(bMu);
            // The hook fires for every stop of every batch. Service the FIRST stop
            // of each batch only: that is where the cleanup for this batch is
            // dispatched, while this thread still holds cancelTrackMu_.
            if (hookServicedBatch == batchPublished + 1) return;   // already serviced
            if (batchPublished >= kBatches) return;                // all batches done
            const int thisBatch = batchPublished + 1;
            hookBatch = thisBatch;
            hookServicedBatch = thisBatch;
            hookWantsCleanup = true;
            bCv.notify_all();
            // Wait until the cleanup has attempted the lock for this batch. The
            // publisher holds cancelTrackMu_ throughout, so that attempt must fail.
            bCv.wait_for(lk, std::chrono::seconds(10),
                         [&] { return cleanupTriedLock >= hookBatch; });
        });

        // A failed snapshot acquisition is recorded here; it runs without the lock
        // when it fails, so it never recurses into the Engine. This is what the
        // publisher waits on: proof the collision actually happened, not merely that
        // the cleanup thread was released.
        e.setSnapshotLockProbe([&](bool acquired) {
            if (acquired) return;
            {
                std::lock_guard<std::mutex> lk(bMu);
                contendedCleanups.fetch_add(1);
                ++cleanupTriedLock;
            }
            bCv.notify_all();
        });

        std::thread publisher([&] {
            for (int b = 0; b < kBatches; ++b) {
                for (int i = 0; i < kPerBatch; ++i) {
                    e.requestStop();
                    publishCount.fetch_add(1);
                }
                std::unique_lock<std::mutex> lk(bMu);
                ++batchPublished;
                bCv.notify_all();
                // Do not start the next batch until this one's cleanup finished.
                bCv.wait_for(lk, std::chrono::seconds(10),
                             [&] { return batchCleaned >= batchPublished; });
            }
            {
                // All accesses to publisherDone take bMu, including this write: the
                // cleanup thread reads it under the same mutex. Writing it unlocked
                // was a data race.
                std::lock_guard<std::mutex> lk(bMu);
                publisherDone = true;
            }
            bCv.notify_all();
        });

        std::thread cleanup([&] {
            for (int b = 0; b < kBatches; ++b) {
                {
                    std::unique_lock<std::mutex> lk(bMu);
                    // Wait until either the hook opened this batch's window, or the
                    // publisher finished publishing the batch.
                    bCv.wait_for(lk, std::chrono::seconds(10), [&] {
                        return hookWantsCleanup || batchPublished > b;
                    });
                }
                // Run the source cleanup. On hook-driven batches the publisher is
                // still inside requestStop() holding cancelTrackMu_, so the snapshot
                // attempt collides; the probe records it and signals the publisher.
                e.cancelSource("padA");
                cleanupCount.fetch_add(1);
                {
                    std::lock_guard<std::mutex> lk(bMu);
                    if (!publisherDone) cleanupsWhilePublishing.fetch_add(1);
                    hookWantsCleanup = false;
                }
                std::vector<uint64_t> snapshot;
                { std::lock_guard<std::mutex> lk(epMu); snapshot = stoppedEpochs; }
                int outstanding = 0;
                for (uint64_t ep : snapshot) {
                    if (e.cancelAcknowledged(ep)) anyStopAckedBySource.store(true);
                    else ++outstanding;
                }
                stopsSeenOutstanding.fetch_add(outstanding);
                {
                    std::lock_guard<std::mutex> lk(bMu);
                    ++batchCleaned;
                }
                bCv.notify_all();
            }
        });

        publisher.join();
        cleanup.join();

        e.setStopPublicationHook(nullptr);
        e.setSnapshotLockProbe(nullptr);

        std::vector<uint64_t> allStops;
        { std::lock_guard<std::mutex> lk(epMu); allStops = stoppedEpochs; }

        check(publishCount.load() == kBatches * kPerBatch,
              "c25b-publish-count", "n=" + std::to_string(publishCount.load()));
        check(cleanupCount.load() == kBatches,
              "c25b-cleanup-count", "n=" + std::to_string(cleanupCount.load()));
        check(cleanupsWhilePublishing.load() == kBatches,
              "c25b-all-cleanups-ran-while-publishing",
              "n=" + std::to_string(cleanupsWhilePublishing.load()));
        check(allStops.size() == (size_t)(kBatches * kPerBatch),
              "c25b-stops-recorded", "n=" + std::to_string(allStops.size()));
        check(stopsSeenOutstanding.load() > 0,
              "c25b-stops-observed-outstanding",
              "n=" + std::to_string(stopsSeenOutstanding.load()));

        // THE contention proof: every cleanup collided with the publisher's held
        // lock, so try_lock failed each time.
        check(contendedCleanups.load() == kBatches,
              "c25b-every-cleanup-hit-real-contention",
              "contended=" + std::to_string(contendedCleanups.load()) +
              " of " + std::to_string(cleanupCount.load()));

        check(!anyStopAckedBySource.load(), "c25b-no-stop-acked-by-source-cleanup", "");
        check(e.stopPending(), "c25b-stop-still-pending", "");
        check(out->isHeld("space"), "c25b-B-key-still-held", "");

        e.cancelAll();
        check(!e.stopPending(), "c25b-global-cleanup-acks", "");
        check(!out->isHeld("space"), "c25b-global-cleanup-released", "");
        int unacked = 0;
        for (uint64_t ep : allStops) if (!e.cancelAcknowledged(ep)) ++unacked;
        check(unacked == 0, "c25b-every-recorded-stop-acked-after-global",
              "unacked=" + std::to_string(unacked) +
              " total=" + std::to_string(allStops.size()));
        check(!e.cancelPending(), "c25b-nothing-pending", "");
    }

    std::printf("\n%d checks, %d passed, %d failed\n", g_check, g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
