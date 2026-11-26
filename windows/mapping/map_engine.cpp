// map_engine.cpp -- rule engine and script runner implementation, plus the
// JSON profile loader. (The concrete adapters are header-only in map_engine.h.)
#include "map_engine.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace map {

// ---------------------------------------------------------------------------
// JSON profile loading
// ---------------------------------------------------------------------------
namespace {

EventType ParseEventType(const std::string& s) {
    if (s == "pressed") return EventType::Pressed;
    if (s == "released") return EventType::Released;
    if (s == "value") return EventType::ValueChanged;
    if (s == "message") return EventType::MessageReceived;
    if (s == "timer") return EventType::TimerElapsed;
    return EventType::Pressed;
}

Action ParseAction(const JsonPtr& j) {
    Action a;
    if (!j || !j->isObject()) return a;
    const std::string kind = j->get("action")->s();
    a.text    = j->get("text")->s();
    a.key     = j->get("key")->s();
    a.delayMs = j->get("ms")->i();
    a.count   = j->get("count")->i();
    a.cond    = j->get("if")->s();
    a.profile = j->get("profile")->s();

    if (kind == "key_down")       a.kind = ActionKind::KeyDown;
    else if (kind == "key_up")    a.kind = ActionKind::KeyUp;
    else if (kind == "key_tap")   a.kind = ActionKind::KeyTap;
    else if (kind == "key_combo") a.kind = ActionKind::KeyCombo;
    else if (kind == "type_text") a.kind = ActionKind::TypeText;
    else if (kind == "delay")     a.kind = ActionKind::Delay;
    else if (kind == "repeat")    a.kind = ActionKind::Repeat;
    else if (kind == "conditional")    a.kind = ActionKind::Conditional;
    else if (kind == "profile_switch") a.kind = ActionKind::ProfileSwitch;
    else if (kind == "cancel_sequence") a.kind = ActionKind::CancelSequence;

    JsonPtr keys = j->get("keys");
    if (keys && keys->isArray()) for (const auto& k : keys->arr) a.combo.push_back(k->s());

    JsonPtr steps = j->get("steps");
    JsonPtr thenJ = j->get("then");
    const JsonPtr& nested = (steps && steps->isArray()) ? steps : thenJ;
    if (nested && nested->isArray()) for (const auto& s : nested->arr) a.block.push_back(ParseAction(s));
    return a;
}

std::vector<Action> ParseScript(const JsonPtr& j) {
    std::vector<Action> out;
    if (j && j->isArray()) for (const auto& s : j->arr) out.push_back(ParseAction(s));
    return out;
}

Binding ParseBinding(const JsonPtr& j) {
    Binding b;
    b.id           = j->get("id")->s();
    b.match.source  = j->get("source")->s();
    b.match.control = j->get("control")->s();
    b.match.type    = ParseEventType(j->get("type")->s());
    b.match.msgText = j->get("msgText")->s();
    b.holdKey   = j->get("holdKey")->s();
    b.aboveKey  = j->get("aboveKey")->s();
    b.threshold = j->get("threshold")->n();
    b.hysteresis = j->get("hysteresis")->n();
    b.debounceMs = j->get("debounceMs")->i();
    b.cooldownMs = j->get("cooldownMs")->i();
    b.longPressMs = j->get("longPressMs")->i();
    b.repeatMs = j->get("repeatMs")->i();
    b.tapCount = j->get("tapCount")->i();
    b.tapWindowMs = j->get("tapWindowMs")->i();
    b.seqTimeoutMs = j->get("seqTimeoutMs")->i();

    const std::string mode = j->get("mode")->s();
    if (mode == "momentary") b.mode = BindingMode::Momentary;
    else if (mode == "toggle") b.mode = BindingMode::Toggle;
    else if (mode == "threshold") b.mode = BindingMode::Threshold;
    else b.mode = BindingMode::Script;

    const std::string trig = j->get("trigger")->s();
    if (trig == "press") b.trigger = TriggerKind::Press;
    else if (trig == "release") b.trigger = TriggerKind::Release;
    else if (trig == "momentary") b.trigger = TriggerKind::Momentary;
    else if (trig == "toggle") b.trigger = TriggerKind::Toggle;
    else if (trig == "threshold") b.trigger = TriggerKind::Threshold;
    else if (trig == "long_press") b.trigger = TriggerKind::LongPress;
    else if (trig == "hold_repeat") b.trigger = TriggerKind::HoldRepeat;
    else if (trig == "double_tap") b.trigger = TriggerKind::DoubleTap;
    else if (trig == "multi_tap") b.trigger = TriggerKind::MultiTap;
    else if (trig == "sequence") b.trigger = TriggerKind::Sequence;
    else if (trig == "combo") b.trigger = TriggerKind::Combo;
    else if (trig == "cooldown") b.trigger = TriggerKind::Cooldown;

    JsonPtr seq = j->get("sequence");
    if (seq && seq->isArray()) {
        for (const auto& s : seq->arr) {
            SeqStep st;
            st.control = s->get("control")->s();
            st.type = ParseEventType(s->get("type")->s());
            b.sequence.push_back(st);
        }
    }
    JsonPtr combo = j->get("combo");
    if (combo && combo->isArray())
        for (const auto& c : combo->arr) b.combo.push_back(c->s());

    b.script    = ParseScript(j->get("script"));
    b.onRelease = ParseScript(j->get("onRelease"));
    return b;
}

} // namespace

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
Engine::Engine(std::shared_ptr<OutputBackend> out) : out_(out) {
    profiles_["default"] = {};
    active_ = "default";
}

const std::vector<Binding>& Engine::activeBindings_() const {
    static const std::vector<Binding> kEmpty;
    auto it = profiles_.find(active_);
    return it == profiles_.end() ? kEmpty : it->second;
}

void Engine::addBinding(const Binding& b) { profiles_[active_].push_back(b); }

void Engine::activateProfile(const std::string& name) {
    if (name == active_) return;
    for (const Binding& b : activeBindings_()) ownership_.releaseAllByHolder(b.id, *out_);
    active_ = name;
    if (profiles_.find(active_) == profiles_.end()) profiles_[active_] = {};
    // Stale trigger/control state must never leak into the new profile: a
    // half-held LongPress or an above-threshold latch would otherwise fire
    // (or hold a key) for a rule that has not seen an event yet.
    triggerStates_.clear();
    controls_.clear();
}

bool Engine::loadProfile(const JsonPtr& root, std::string* err) {
    if (!root || !root->isObject()) { if (err) *err = "profile root is not an object"; return false; }
    JsonPtr profs = root->get("profiles");
    if (!profs || !profs->isObject()) { if (err) *err = "missing 'profiles' object"; return false; }
    bool hasDefault = false;
    for (const auto& p : profs->obj) {
        std::vector<Binding> bindings;
        // A profile is either an array of bindings or {"bindings":[...]}.
        const JsonPtr& pv = p.second;
        const JsonPtr& arr = (pv && pv->isArray()) ? pv
                            : (pv && pv->isObject() ? pv->get("bindings") : pv);
        if (arr && arr->isArray())
            for (const auto& b : arr->arr) bindings.push_back(ParseBinding(b));
        profiles_[p.first] = bindings;
        if (p.first == "default") hasDefault = true;
    }
    // Activate a profile the document actually defines; never silently fall
    // back to the constructor's empty "default" when the config named others.
    if (hasDefault || profs->obj.empty()) active_ = "default";
    else active_ = profs->obj.begin()->first;
    return true;
}

bool Engine::loadProfileText(const std::string& json, std::string* err) {
    JsonPtr root = ParseJson(json, err);
    if (!root) return false;
    return loadProfile(root, err);
}

void Engine::updateClock(uint64_t nowMs) { now_ = nowMs; }

bool Engine::match(const Binding& b, const Event& e) const {
    const MatchSpec& m = b.match;
    if (!m.source.empty() && m.source != e.source) return false;
    if (!m.control.empty() && m.control != e.control) return false;
    bool typeOk = (m.type == e.type);
    if (!typeOk) {
        // A Pressed-type match must also see the matching Released edge so
        // stateful triggers (momentary/toggle/long-press/hold-repeat/combo and
        // script onRelease) can release. handle() ignores edges it does not use.
        if (m.type == EventType::Pressed && e.type == EventType::Released) typeOk = true;
    }
    if (!typeOk) return false;
    if (e.type == EventType::MessageReceived && !m.msgText.empty() &&
        e.text.find(m.msgText) == std::string::npos) return false;
    return true;
}

bool Engine::isStopLocked(uint64_t epoch) const {
    // Caller holds cancelTrackMu_.
    if (epoch == 0) return false;
    return std::binary_search(stopRequests_.begin(), stopRequests_.end(), epoch);
}

bool Engine::ackCoveredLocked(uint64_t epoch) const {
    // Caller holds cancelTrackMu_ (or passes a snapshot it already owns).
    if (epoch == 0) return true;                       // "no request" is trivially done
    if (epoch <= cancelAcked_.load(std::memory_order_acquire)) return true;
    for (uint64_t e : ackOutOfOrder_) if (e == epoch) return true;
    return false;
}

bool Engine::cancelAcknowledged(uint64_t epoch) const {
    std::lock_guard<std::mutex> lk(cancelTrackMu_);
    return ackCoveredLocked(epoch);
}

bool Engine::cancelPending() const {
    // A cancel is outstanding while ANY accepted request is still unhandled.
    // Checking only the newest epoch would hide an older request that is still
    // pending behind a newer one that has already been cleaned up.
    std::lock_guard<std::mutex> lk(cancelTrackMu_);
    const uint64_t acked = cancelAcked_.load(std::memory_order_acquire);
    const uint64_t head  = cancelEpoch_.load(std::memory_order_acquire);
    if (head > acked) return true;                     // newest request not handled
    for (uint64_t e : ackOutOfOrder_) if (e > acked) return true;
    return false;
}

bool Engine::stopPending() const {
    // A stop blocks new actions until ITS OWN epoch has been cleaned up. A newer
    // per-device cancel being acknowledged must not unblock it; that is exactly
    // the interleaving that a shared watermark got wrong.
    std::lock_guard<std::mutex> lk(cancelTrackMu_);
    const uint64_t stop = stopEpoch_.load(std::memory_order_acquire);
    if (stop == 0) return false;
    return !ackCoveredLocked(stop);
}

uint64_t Engine::pendingStopEpoch() const {
    std::lock_guard<std::mutex> lk(cancelTrackMu_);
    const uint64_t stop = stopEpoch_.load(std::memory_order_acquire);
    if (stop == 0 || ackCoveredLocked(stop)) return 0;
    return stop;
}

void Engine::foldAckedLocked() {
    // Advance the watermark over every contiguously handled request. Requests
    // handled ahead of a gap stay in ackOutOfOrder_ so the gap itself stays
    // visibly pending: folding them wholesale is precisely the bug that let a
    // newer cancel "acknowledge" an older, never-executed stop.
    uint64_t next = cancelAcked_.load(std::memory_order_acquire) + 1;
    while (true) {
        auto it = std::lower_bound(ackOutOfOrder_.begin(), ackOutOfOrder_.end(), next);
        if (it == ackOutOfOrder_.end() || *it != next) break;
        ackOutOfOrder_.erase(it);
        ++next;
    }
    if (next - 1 > cancelAcked_.load(std::memory_order_acquire)) {
        cancelAcked_.store(next - 1, std::memory_order_acq_rel);
    }
    const uint64_t covered = cancelAcked_.load(std::memory_order_acquire);
    ackOutOfOrder_.erase(
        std::remove_if(ackOutOfOrder_.begin(), ackOutOfOrder_.end(),
                       [covered](uint64_t e) { return e <= covered; }),
        ackOutOfOrder_.end());
}

void Engine::acknowledgeCancel(uint64_t epoch) {
    // Record exactly one request as handled. Contiguous ones advance the
    // watermark; a request handled ahead of a gap is remembered individually and
    // leaves the gap pending.
    if (epoch == 0) return;
    std::lock_guard<std::mutex> lk(cancelTrackMu_);
    if (epoch > cancelAcked_.load(std::memory_order_acquire)) {
        if (!ackCoveredLocked(epoch)) {
            ackOutOfOrder_.push_back(epoch);
            std::sort(ackOutOfOrder_.begin(), ackOutOfOrder_.end());
        }
        foldAckedLocked();
    }
    // The flag is purely diagnostic: clear it only when nothing at all is pending.
    const uint64_t acked = cancelAcked_.load(std::memory_order_acquire);
    bool anyPending = cancelEpoch_.load(std::memory_order_acquire) > acked;
    for (uint64_t e : ackOutOfOrder_) if (e > acked) { anyPending = true; break; }
    if (!anyPending) cancelRequested_.store(false, std::memory_order_release);
}

void Engine::acknowledgeSourceCleanup(uint64_t coveredEpoch) {
    // A SOURCE-SCOPED cleanup released one source's keys. That satisfies ordinary
    // cancellation requests at or below `coveredEpoch`, but it does NOT satisfy a
    // global Stop: a Stop demands every key released, including keys held by other
    // devices. Acknowledging a Stop here would clear stopPending() while another
    // device's key is still down, which is the defect this method exists to
    // prevent - whether the epoch arrived explicitly or through the default
    // argument.
    if (coveredEpoch == 0) return;
    std::lock_guard<std::mutex> lk(cancelTrackMu_);
    const uint64_t acked = cancelAcked_.load(std::memory_order_acquire);
    for (uint64_t e = acked + 1; e <= coveredEpoch; ++e) {
        if (isStopLocked(e)) continue;          // global scope: leave pending
        if (ackCoveredLocked(e)) continue;      // already handled
        ackOutOfOrder_.push_back(e);
    }
    std::sort(ackOutOfOrder_.begin(), ackOutOfOrder_.end());
    ackOutOfOrder_.erase(std::unique(ackOutOfOrder_.begin(), ackOutOfOrder_.end()),
                         ackOutOfOrder_.end());
    foldAckedLocked();
    // The diagnostic flag is cleared only when nothing at all is outstanding.
    const uint64_t nowAcked = cancelAcked_.load(std::memory_order_acquire);
    bool anyPending = cancelEpoch_.load(std::memory_order_acquire) > nowAcked;
    for (uint64_t e : ackOutOfOrder_) if (e > nowAcked) { anyPending = true; break; }
    if (!anyPending) cancelRequested_.store(false, std::memory_order_release);
}

void Engine::acknowledgeCancelThrough(uint64_t epoch) {
    // A global cleanup handled everything that was pending when it ran, so every
    // request up to `epoch` is complete - including a global Stop and requests a
    // scoped cancelSource had cleaned up out of order.
    if (epoch == 0) return;
    std::lock_guard<std::mutex> lk(cancelTrackMu_);
    uint64_t prev = cancelAcked_.load(std::memory_order_acquire);
    while (prev < epoch &&
           !cancelAcked_.compare_exchange_weak(prev, epoch,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
    }
    foldAckedLocked();
    // Stop records at or below the watermark can never be consulted again: a
    // source cleanup only ever asks about epochs above it. Pruning keeps the set
    // small on a long-lived engine.
    const uint64_t acked = cancelAcked_.load(std::memory_order_acquire);
    stopRequests_.erase(
        std::remove_if(stopRequests_.begin(), stopRequests_.end(),
                       [acked](uint64_t e) { return e <= acked; }),
        stopRequests_.end());
    bool anyPending = cancelEpoch_.load(std::memory_order_acquire) > acked;
    for (uint64_t e : ackOutOfOrder_) if (e > acked) { anyPending = true; break; }
    if (!anyPending) cancelRequested_.store(false, std::memory_order_release);
}

void Engine::dispatch(const Event& e) {
    // Never blind-reset cancellation state here. A request that lands just before
    // this call is a legitimate emergency stop and MUST NOT be erased by a
    // dispatch that is only now starting; it is cleared only by the executor's
    // acknowledgement. An outstanding stop blocks the whole dispatch.
    if (stopPending()) return;  // guarantee 1: no new action once a stop is accepted
    if (e.ts != 0) now_ = e.ts;
    for (const Binding& b : activeBindings_()) {
        if (!match(b, e)) continue;
        TriggerState& st = triggerStates_[b.id];
        if (b.debounceMs > 0) {
            // Debounce state is PER BINDING: two bindings on the same control
            // with different debounce windows must not suppress each other.
            if (st.lastEventTs != 0 && e.ts != 0 && e.ts - st.lastEventTs < (uint64_t)b.debounceMs)
                continue;
            st.lastEventTs = e.ts;
        }
        // A stop can also arrive between two matching rules of one dispatch.
        if (stopPending()) break;
        handle(b, e);
    }
}

void Engine::handle(const Binding& b, const Event& e) {
    const std::string holder = b.id; // stable per binding => cross-dispatch ownership works
    ControlState& cs = controls_[ctrlKey(e.source, e.control)];
    TriggerState& st = triggerStates_[b.id];

    // Legacy BindingMode maps onto the trigger vocabulary when the config used
    // the older mode field instead of an explicit trigger kind.
    TriggerKind tk = b.trigger;
    if (tk == TriggerKind::Press) {
        if (b.mode == BindingMode::Momentary) tk = TriggerKind::Momentary;
        else if (b.mode == BindingMode::Toggle) tk = TriggerKind::Toggle;
        else if (b.mode == BindingMode::Threshold) tk = TriggerKind::Threshold;
        else tk = TriggerKind::Press;
    }

    const uint64_t now = e.ts ? e.ts : now_;

    switch (tk) {
    case TriggerKind::Momentary:
        if (!b.holdKey.empty()) {
            if (e.type == EventType::Pressed) { st.held = true; ownership_.press(b.holdKey, holder, *out_); }
            else if (e.type == EventType::Released) { st.held = false; ownership_.release(b.holdKey, holder, *out_); }
        }
        break;

    case TriggerKind::Toggle:
        if (e.type == EventType::Pressed && !b.holdKey.empty()) {
            st.toggle = !st.toggle; cs.toggle = st.toggle;
            if (st.toggle) ownership_.press(b.holdKey, holder, *out_);
            else ownership_.release(b.holdKey, holder, *out_);
        }
        break;

    case TriggerKind::Threshold: {
        if (e.type != EventType::ValueChanged && e.type != EventType::ThresholdCrossed) break;
        cs.value = e.value;
        const bool above = e.value >= b.threshold;
        cs.above = above;
        if (!b.aboveKey.empty()) {
            if (above && !st.held) {
                st.held = true;
                ownership_.press(b.aboveKey, holder, *out_);
            } else if (!above && st.held && e.value < b.threshold - b.hysteresis) {
                st.held = false;
                ownership_.release(b.aboveKey, holder, *out_);
            }
        }
        break;
    }

    case TriggerKind::Press:
        if (e.type == EventType::Pressed || e.type == EventType::MessageReceived ||
            e.type == EventType::TimerElapsed) {
            if (b.cooldownMs > 0 && st.lastFireTs && now - st.lastFireTs < (uint64_t)b.cooldownMs) break;
            st.lastFireTs = now;
            CancelState c; runScript(b.script, holder, c);
        } else if (e.type == EventType::Released && !b.onRelease.empty()) {
            CancelState c; runScript(b.onRelease, holder, c);
        }
        break;

    case TriggerKind::Release:
        if (e.type == EventType::Released) {
            if (b.cooldownMs > 0 && st.lastFireTs && now - st.lastFireTs < (uint64_t)b.cooldownMs) break;
            st.lastFireTs = now;
            CancelState c; runScript(b.script, holder, c);
        }
        break;

    case TriggerKind::Cooldown:
        if (e.type == EventType::Pressed) {
            if (st.lastFireTs && now - st.lastFireTs < (uint64_t)b.cooldownMs) break;
            st.lastFireTs = now;
            CancelState c; runScript(b.script, holder, c);
        }
        break;

    case TriggerKind::LongPress:
        if (e.type == EventType::Pressed) {
            st.held = true; st.pressTs = now; st.firedLongPress = false;
        } else if (e.type == EventType::Released) {
            st.held = false; st.firedLongPress = false;
        }
        // actual fire happens in tick() once held >= longPressMs
        break;

    case TriggerKind::HoldRepeat:
        if (e.type == EventType::Pressed) {
            st.held = true; st.pressTs = now;
            st.nextRepeatTs = now + (uint64_t)b.longPressMs; // first repeat after initial delay
        } else if (e.type == EventType::Released) {
            st.held = false;
        }
        break;

    case TriggerKind::DoubleTap:
    case TriggerKind::MultiTap: {
        int need = (tk == TriggerKind::DoubleTap) ? 2 : b.tapCount;
        if (e.type == EventType::Pressed) {
            if (st.lastTapTs && now - st.lastTapTs <= (uint64_t)b.tapWindowMs) st.tapCount++;
            else st.tapCount = 1;
            st.lastTapTs = now;
            if (st.tapCount >= need) {
                st.tapCount = 0; st.lastTapTs = 0;
                if (b.cooldownMs > 0 && st.lastFireTs && now - st.lastFireTs < (uint64_t)b.cooldownMs) break;
                st.lastFireTs = now;
                CancelState c; runScript(b.script, holder, c);
            }
        }
        break;
    }

    case TriggerKind::Sequence: {
        if (b.sequence.empty()) break;
        // timeout resets progress
        if (st.seqIndex > 0 && st.seqLastTs && now - st.seqLastTs > (uint64_t)b.seqTimeoutMs) st.seqIndex = 0;
        const SeqStep& want = b.sequence[(size_t)st.seqIndex];
        bool ctrlOk = want.control.empty() || want.control == e.control;
        if (ctrlOk && want.type == e.type) {
            st.seqIndex++;
            st.seqLastTs = now;
            if (st.seqIndex >= (int)b.sequence.size()) {
                st.seqIndex = 0;
                if (b.cooldownMs > 0 && st.lastFireTs && now - st.lastFireTs < (uint64_t)b.cooldownMs) break;
                st.lastFireTs = now;
                CancelState c; runScript(b.script, holder, c);
            }
        } else if (e.type == EventType::Pressed || e.type == EventType::Released) {
            // wrong edge for the expected step resets the sequence
            st.seqIndex = 0;
        }
        break;
    }

    case TriggerKind::Combo: {
        if (b.combo.empty()) break;
        if (e.type == EventType::Pressed || e.type == EventType::Released) {
            std::string key = e.source + "|" + e.control;
            st.comboHeld[key] = (e.type == EventType::Pressed);
            // A combo entry may be the full "source|control" key (cross-device
            // precision) or a bare control name (any source). Both must agree
            // with the per-event bookkeeping above, whatever the config style.
            auto entryHeld = [&](const std::string& want) -> bool {
                if (want.find('|') != std::string::npos) {
                    auto it = st.comboHeld.find(want);
                    return it != st.comboHeld.end() && it->second;
                }
                const std::string suffix = "|" + want;
                for (const auto& kv : st.comboHeld) {
                    if (kv.second && kv.first.size() > suffix.size() &&
                        kv.first.compare(kv.first.size() - suffix.size(), suffix.size(), suffix) == 0)
                        return true;
                }
                return false;
            };
            bool all = true;
            for (const auto& c : b.combo) { if (!entryHeld(c)) { all = false; break; } }
            if (all && !st.held) {
                st.held = true; // fire once per full chord
                if (b.cooldownMs > 0 && st.lastFireTs && now - st.lastFireTs < (uint64_t)b.cooldownMs) break;
                st.lastFireTs = now;
                CancelState c; runScript(b.script, holder, c);
            } else if (!all && st.held && e.type == EventType::Released) {
                st.held = false;
            }
        }
        break;
    }
    }
}

void Engine::tick(uint64_t nowMs) {
    now_ = nowMs;
    // A time-based trigger must not fire a script while a stop is unacknowledged;
    // the latching state is left untouched so the trigger continues normally once
    // the stop has been completed.
    if (stopPending()) return;
    for (const Binding& b : activeBindings_()) {
        auto it = triggerStates_.find(b.id);
        if (it == triggerStates_.end()) continue;
        TriggerState& st = it->second;
        TriggerKind tk = b.trigger;
        if (tk == TriggerKind::Press) {
            if (b.mode == BindingMode::Momentary) tk = TriggerKind::Momentary;
            else if (b.mode == BindingMode::Toggle) tk = TriggerKind::Toggle;
            else if (b.mode == BindingMode::Threshold) tk = TriggerKind::Threshold;
            else tk = TriggerKind::Press;
        }
        const std::string holder = b.id;
        if (tk == TriggerKind::LongPress) {
            if (st.held && !st.firedLongPress && st.pressTs &&
                nowMs - st.pressTs >= (uint64_t)b.longPressMs) {
                st.firedLongPress = true;
                if (b.cooldownMs > 0 && st.lastFireTs && nowMs - st.lastFireTs < (uint64_t)b.cooldownMs) continue;
                st.lastFireTs = nowMs;
                CancelState c; runScript(b.script, holder, c);
            }
        } else if (tk == TriggerKind::HoldRepeat) {
            if (st.held && st.nextRepeatTs && nowMs >= st.nextRepeatTs) {
                st.nextRepeatTs = nowMs + (uint64_t)b.repeatMs;
                CancelState c; runScript(b.script, holder, c);
            }
        }
    }
}

void Engine::runScript(const std::vector<Action>& script, const std::string& holder, CancelState& cs) {
    // Single choke point for every script entry (press, release, tick, nested
    // blocks all funnel through here or runBlock), so cancellation is not
    // re-implemented per action kind and cannot drift out of sync.
    runBlock(script, holder, cs);
    // An externally interrupted script never reaches its own KeyUp steps, so the
    // keys it owns are dropped here. This is what keeps a cancelled chord from
    // leaving a modifier logically pressed. An explicit cancel_sequence action
    // only stops the script and deliberately leaves key state to its author.
    if (cancelPending()) releaseCancelKeys(holder, cs);
}

bool Engine::runBlock(const std::vector<Action>& block, const std::string& holder, CancelState& cs) {
    // Returns false as soon as a step refused to run, which is what stops a
    // nested sequence from resuming after a cancelled Delay interrupted it.
    for (const Action& a : block) {
        if (cs.cancelled || cancelPending()) return false;
        runAction(a, holder, cs);
        if (cs.cancelled || cancelPending()) return false; // post-step check: catches Delay text
    }
    return true;
}

void Engine::releaseCancelKeys(const std::string& holder, CancelState& cs) {
    // A cancelled script must not leave its own keys or the modifiers of a
    // half-issued chord down. Ownership is refcounted, so releasing the script's
    // holder only drops what that holder actually pressed.
    ownership_.releaseAllByHolder(holder, *out_);
    cs.cancelled = true;
}

bool Engine::evalCond(const std::string& cond) {
    if (cond.empty()) return true;
    bool negate = false;
    std::string c = cond;
    if (c.rfind("not:", 0) == 0) { negate = true; c = c.substr(4); }
    bool r = false;
    if (c.rfind("held:", 0) == 0) {
        r = ownership_.isHeld(c.substr(5));
    } else if (c.rfind("above:", 0) == 0) {
        const std::string rest = c.substr(6);
        const auto pos = rest.find('/');
        if (pos != std::string::npos) {
            r = controls_[ctrlKey(rest.substr(0, pos), rest.substr(pos + 1))].above;
        }
    }
    return negate ? !r : r;
}

void Engine::runAction(const Action& a, const std::string& holder, CancelState& cs) {
    switch (a.kind) {
    case ActionKind::KeyDown: ownership_.press(a.key, holder, *out_); break;
    case ActionKind::KeyUp: ownership_.release(a.key, holder, *out_); break;
    case ActionKind::KeyTap:
        ownership_.press(a.key, holder, *out_);
        ownership_.release(a.key, holder, *out_);
        break;
    case ActionKind::KeyCombo:
        // Presses and releases are interleaved per key, so a cancellation that
        // arrives mid-chord still unwinds every key this combo already pressed
        // instead of leaving the tail of the chord held.
        for (const auto& k : a.combo) {
            if (cs.cancelled || cancelPending()) break;
            ownership_.press(k, holder, *out_);
        }
        for (auto it = a.combo.rbegin(); it != a.combo.rend(); ++it)
            ownership_.release(*it, holder, *out_);
        break;
    case ActionKind::TypeText: {
        // The backend polls this probe between characters and unwinds its own
        // partial state; the engine only decides whether the script continues.
        TextOutcome r = out_->typeText(a.text, [this] { return cancelPending(); });
        if (r == TextOutcome::Cancelled) cs.cancelled = true;
        else if (r == TextOutcome::Failed) ++textFailures_;   // reported, not swallowed
        break;
    }
    case ActionKind::Delay: sleepMs(a.delayMs); break;
    case ActionKind::Repeat:
        for (int i = 0; i < a.count; ++i) {
            if (!runBlock(a.block, holder, cs)) break;
        }
        break;
    case ActionKind::Conditional:
        // Uses the same cancellation gate as every other nested block: the old
        // code checked only cs.cancelled, so a cancel request that interrupted
        // a nested Delay let the rest of the block run to completion.
        if (evalCond(a.cond)) runBlock(a.block, holder, cs);
        break;
    case ActionKind::ProfileSwitch:
        // A cancellation must win over a profile change: switching profiles
        // releases keys owned by the outgoing profile and clears trigger state,
        // which would make an in-flight stop look like a normal transition.
        if (!cs.cancelled && !cancelPending()) activateProfile(a.profile);
        break;
    case ActionKind::CancelSequence: cs.cancelled = true; break;
    }
}

void Engine::sleepMs(int ms) {
    if (ms <= 0) return;
    if (!realtime_) return;
    int left = ms;
    while (left > 0) {
        if (cancelPending()) return;   // interruptible delay
        int slice = left < 10 ? left : 10;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        left -= slice;
    }
}

void Engine::cancelSource(const std::string& source, uint64_t requestEpoch) {
    // Snapshot BEFORE the cleanup, never after: a request that arrives while this
    // call is releasing keys is NOT covered by this cleanup and must stay pending
    // until its own acknowledgement, otherwise its keys would never be released.
    //
    // The snapshot is taken UNDER cancelTrackMu_, the same lock that publishes an
    // epoch and registers its Stop scope. That mutual exclusion is what closes the
    // window in which an epoch is visible but its scope is not yet recorded: a
    // cleanup either snapshots before the Stop is published (seeing an older
    // epoch) or after publication, when the scope is already registered. Reading
    // cancelEpoch_ without the lock could observe the new epoch while stopRequests_
    // still lacked it, and then acknowledge a global Stop as if it were ordinary.
    // Read the test-only pre-lock observer ONCE, before taking the lock, so the
    // production path performs no extra work and no callback can run while
    // cancelTrackMu_ is held.
    const std::function<void(const std::string&)> prelock = prelockHook_;
    if (prelock) prelock(source);

    const uint64_t covered = [&] {
        // Test-only contention probe. It is installed only by tests; with no probe
        // this collapses to the plain lock_guard path below, so the production
        // linearization point is unchanged.
        //
        // try_lock() can report a spurious failure on some implementations, but the
        // test that uses this holds cancelTrackMu_ for the entire observation
        // window, so a failure is always genuine contention. The probe reports
        // BEFORE the blocking acquisition, so it cannot itself delay the lock.
        if (snapshotProbe_) {
            const bool acquired = cancelTrackMu_.try_lock();
            if (acquired) cancelTrackMu_.unlock();
            snapshotProbe_(acquired);
        }
        std::lock_guard<std::mutex> lk(cancelTrackMu_);
        return requestEpoch ? requestEpoch
                            : cancelEpoch_.load(std::memory_order_acquire);
    }();
    for (const auto& p : profiles_) {
        for (const Binding& b : p.second) {
            if (b.match.source == source) {
                ownership_.releaseAllByHolder(b.id, *out_);
                // A disconnecting source must not leave a live trigger state:
                // a half-held LongPress/HoldRepeat would keep firing in tick().
                triggerStates_.erase(b.id);
            }
        }
    }
    for (auto it = controls_.begin(); it != controls_.end();) {
        if (it->first.rfind(source + "|", 0) == 0) it = controls_.erase(it);
        else ++it;
    }
    // A per-source stop is complete once that source's keys are released and its
    // stale trigger state is gone, so the ordinary cancellation requests this
    // cleanup covers are acknowledged - for BOTH the explicit-epoch form (queued
    // control item) and the legacy default form. Leaving the default form
    // unacknowledged would let
    //   requestCancel(); cancelSource("padA");
    // strand a request that this call already handled, blocking every later
    // dispatch forever.
    //
    // It must NOT acknowledge a GLOBAL Stop. A source cleanup releases one source's
    // keys; a Stop requires every key released, including other devices'. That
    // holds whether the epoch was passed explicitly or snapshotted here, so the
    // scope check lives in acknowledgeSourceCleanup() rather than in a comparison
    // against the current stop epoch (which would be wrong with several stops
    // pending).
    acknowledgeSourceCleanup(covered);
}

void Engine::releaseAllKeys() {
    ownership_.releaseAll(*out_);
    for (auto& kv : controls_) { kv.second.above = false; kv.second.toggle = false; }
    // Every trigger latch is invalid after an emergency stop: without this,
    // a stale held=true would make tick() re-fire LongPress/HoldRepeat.
    triggerStates_.clear();
}

void Engine::acknowledgeStop(uint64_t epoch) {
    // Global cleanup is a superset of every earlier request, so the whole prefix
    // is complete. Kept separate from releaseAllKeys() so the caller can put the
    // queue barrier in between: until this runs, producers still see the stop as
    // pending and therefore cannot post an event that the barrier would miss.
    acknowledgeCancelThrough(epoch);
}

void Engine::cancelAll(uint64_t requestEpoch) {
    // Snapshot BEFORE the cleanup: see cancelSource(). Reading after the release
    // would acknowledge a request whose keys were never released.
    const uint64_t covered = requestEpoch ? requestEpoch
                                          : cancelEpoch_.load(std::memory_order_acquire);
    releaseAllKeys();
    // Releasing every key and clearing every trigger is a superset of whatever any
    // earlier pending request needed, so the whole prefix up to `covered` is done.
    acknowledgeCancelThrough(covered);
}

uint64_t Engine::requestCancel() {
    // Safe to call from any thread (producer/UI/device teardown): it allocates an
    // epoch and publishes it so a long delay/repeat/type_text script bails out
    // early. The actual key release is performed by the executor via
    // cancelAll()/cancelSource().
    //
    // Allocation and publication happen under cancelTrackMu_ together, because an
    // epoch must never be observable before its scope is decided. For an ordinary
    // cancellation the scope is "ordinary" by definition, so registering and
    // publishing in one critical section is enough; requestStop() additionally
    // records the Stop scope in the SAME section (see below).
    std::lock_guard<std::mutex> lk(cancelTrackMu_);
    const uint64_t epoch = ++nextEpoch_;
    cancelEpoch_.store(epoch, std::memory_order_release);
    cancelRequested_.store(true, std::memory_order_release);
    return epoch;
}

uint64_t Engine::requestStop() {
    // An emergency stop is a cancel plus "and do not start anything new".
    //
    // CRITICAL: allocation, scope registration, publication and the stopEpoch_
    // update form ONE indivisible operation under cancelTrackMu_. A cleanup thread
    // takes the same lock to snapshot the epoch and to classify it, so it is
    // impossible for it to observe this epoch without also observing its Stop
    // scope. Publishing first and registering afterwards (the previous ordering)
    // left exactly that window open: a default-argument cancelSource() could read
    // the new epoch, find it absent from stopRequests_, and acknowledge a global
    // Stop as though it were an ordinary cancellation.
    std::lock_guard<std::mutex> lk(cancelTrackMu_);
    const uint64_t epoch = ++nextEpoch_;
    // Register the scope BEFORE publishing, so publication is the last step a
    // concurrent reader can observe.
    if (!isStopLocked(epoch)) {
        stopRequests_.push_back(epoch);
        std::sort(stopRequests_.begin(), stopRequests_.end());
    }
    // Monotonic max: two concurrent stops must not let the lower epoch win. Safe
    // to compute under the lock because every stop publication does the same.
    uint64_t prev = stopEpoch_.load(std::memory_order_acquire);
    while (prev < epoch &&
           !stopEpoch_.compare_exchange_weak(prev, epoch,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
    }
    // Test-only: expose the window between scope registration and publication. The
    // hook runs while this thread holds cancelTrackMu_, so any cleanup it triggers
    // on another thread must block on that lock - which is precisely the property
    // under test. Empty in production.
    if (stopPublishHook_) stopPublishHook_(epoch);
    cancelEpoch_.store(epoch, std::memory_order_release);
    cancelRequested_.store(true, std::memory_order_release);
    return epoch;
}

std::vector<std::string> Engine::heldKeys() const { return ownership_.heldKeys(); }
std::vector<std::string> Engine::unresolvedKeyReleases() const { return ownership_.unresolvedKeys(); }
size_t Engine::retryUnresolvedReleases() { return ownership_.retryUnresolved(*out_); }
size_t Engine::outputFailures() const { return ownership_.outputFailures(); }

double Engine::lastValue(const std::string& source, const std::string& control) const {
    auto it = controls_.find(ctrlKey(source, control));
    return it == controls_.end() ? 0.0 : it->second.value;
}

} // namespace map
