// map_engine.cpp -- rule engine and script runner implementation, plus the
// JSON profile loader. (The concrete adapters are header-only in map_engine.h.)
#include "map_engine.h"

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

void Engine::dispatch(const Event& e) {
    cancelRequested_.store(false); // consume any stale cancel; real mid-run cancels still work
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
    for (const Action& a : script) {
        if (cs.cancelled || cancelRequested_.load()) break;
        runAction(a, holder, cs);
    }
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
        for (const auto& k : a.combo) ownership_.press(k, holder, *out_);
        for (auto it = a.combo.rbegin(); it != a.combo.rend(); ++it)
            ownership_.release(*it, holder, *out_);
        break;
    case ActionKind::TypeText: out_->typeText(a.text); break;
    case ActionKind::Delay: sleepMs(a.delayMs); break;
    case ActionKind::Repeat:
        for (int i = 0; i < a.count && !cs.cancelled && !cancelRequested_.load(); ++i) {
            for (const auto& s : a.block) {
                if (cs.cancelled || cancelRequested_.load()) break;
                runAction(s, holder, cs);
            }
        }
        break;
    case ActionKind::Conditional:
        if (evalCond(a.cond)) {
            for (const auto& s : a.block) { if (cs.cancelled) break; runAction(s, holder, cs); }
        }
        break;
    case ActionKind::ProfileSwitch: activateProfile(a.profile); break;
    case ActionKind::CancelSequence: cs.cancelled = true; break;
    }
}

void Engine::sleepMs(int ms) {
    if (ms <= 0) return;
    if (!realtime_) return;
    int left = ms;
    while (left > 0) {
        if (cancelRequested_.load()) return;   // interruptible delay
        int slice = left < 10 ? left : 10;
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        left -= slice;
    }
}

void Engine::cancelSource(const std::string& source) {
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
}

void Engine::cancelAll() {
    cancelRequested_.store(false);
    ownership_.releaseAll(*out_);
    for (auto& kv : controls_) { kv.second.above = false; kv.second.toggle = false; }
    // Every trigger latch is invalid after an emergency stop: without this,
    // a stale held=true would make tick() re-fire LongPress/HoldRepeat.
    triggerStates_.clear();
}

void Engine::requestCancel() {
    // Safe to call from any thread (producer/UI/emergency): only flips an atomic
    // so a long `delay`/`repeat` script bails out early. The actual key release
    // is performed by the executor via cancelAll()/cancelSource().
    cancelRequested_.store(true);
}

std::vector<std::string> Engine::heldKeys() const { return ownership_.heldKeys(); }
size_t Engine::outputFailures() const { return ownership_.outputFailures(); }

double Engine::lastValue(const std::string& source, const std::string& control) const {
    auto it = controls_.find(ctrlKey(source, control));
    return it == controls_.end() ? 0.0 : it->second.value;
}

} // namespace map
