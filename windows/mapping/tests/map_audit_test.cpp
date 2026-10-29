// ---------------------------------------------------------------------------
// map_audit_test.cpp -- final-audit regression tests for the TextDeck mapping
// engine. Each test targets a defect found (and fixed) during the V1 audit:
//   A1 release-edge reachability through Pressed-type matches
//   A2 combo bookkeeping consistency (bare control names + source|control)
//   A3 per-binding debounce isolation
//   A4 trigger-state staleness after cancelAll / cancelSource / profile switch
//   A5 {"bindings":[...]} object-form profiles actually load
//   A6 profile without "default" activates a defined profile
//   A7 output-backend failure is observable and does not leak ownership
//   A8 MappingRuntime::stop() interrupts a long-running delay promptly
//   A9 time-based triggers keep firing under sustained event load
//   A10 key-table completeness (f13..f24, escape) + CharToStroke mapping
//   A11 config rejects unknown key names with a readable error
// Runs on the host (RecordOutput backend only).
// ---------------------------------------------------------------------------
#include "../map_core.h"
#include "../map_engine.h"
#include "../map_queue.h"
#include "../map_config.h"

#include <atomic>
#include <chrono>
#include <cstdio>
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
static Event value(const std::string& src, const std::string& ctl, double v, uint64_t ts) {
    return Event{src, ctl, EventType::ValueChanged, v, false, "", ts, "test"};
}
static int taps(const RecordOutput& r, const std::string& key) {
    int n = 0;
    for (const auto& op : r.ops) if (op == "down:" + key) ++n;
    return n;
}
static int downs(const RecordOutput& r, const std::string& key) { return taps(r, key); }
static int ups(const RecordOutput& r, const std::string& key) {
    int n = 0;
    for (const auto& op : r.ops) if (op == "up:" + key) ++n;
    return n;
}

int main() {
    std::printf("map_audit_test -- V1 final-audit regression tests\n");

    // ---- A1: Released edges must reach Pressed-type matches ---------------
    section("A1 release-edge reachability");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "m1"; b.match = {"s", "btn", EventType::Pressed};
        b.trigger = TriggerKind::Momentary; b.holdKey = "space";
        e.addBinding(b);
        e.dispatch(press("s", "btn", 1000));
        check(rec->isHeld("space"), "a1-hold-on-press", "");
        e.dispatch(release("s", "btn", 1100));
        check(!rec->isHeld("space"), "a1-released-via-pasthrough", "");
        // onRelease script on a Press trigger
        auto rec2 = std::make_shared<RecordOutput>();
        Engine e2(rec2);
        Binding b2; b2.id = "sc"; b2.match = {"s", "btn", EventType::Pressed};
        Action t; t.kind = ActionKind::KeyTap; t.key = "r";
        b2.onRelease = { t };
        e2.addBinding(b2);
        e2.dispatch(press("s", "btn", 2000));
        check(taps(*rec2, "r") == 0, "a1-onrelease-not-on-press", "");
        e2.dispatch(release("s", "btn", 2100));
        check(taps(*rec2, "r") == 1, "a1-onrelease-fires", "");
    }

    // ---- A2: combo consistency (bare names AND source|control) -----------
    section("A2 combo bookkeeping consistency");
    {
        // bare control names
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "cb"; b.match = {"", "", EventType::Pressed};
        b.trigger = TriggerKind::Combo; b.combo = {"y", "a"}; // bare names
        Action t; t.kind = ActionKind::KeyTap; t.key = "k"; b.script = { t };
        e.addBinding(b);
        e.dispatch(press("p1", "y", 1000));
        check(taps(*rec, "k") == 0, "a2-bare-half", "");
        e.dispatch(press("p2", "a", 1100));
        check(taps(*rec, "k") == 1, "a2-bare-combo-fires", "");
        e.dispatch(release("p2", "a", 1200));
        e.dispatch(press("p2", "a", 1300));
        check(taps(*rec, "k") == 2, "a2-bare-refires-after-release", "");

        // full source|control names (cross-device precision)
        auto rec2 = std::make_shared<RecordOutput>();
        Engine e2(rec2);
        Binding b2; b2.id = "cb2"; b2.match = {"", "", EventType::Pressed};
        b2.trigger = TriggerKind::Combo; b2.combo = {"p1|y", "p2|a"};
        Action t2; t2.kind = ActionKind::KeyTap; t2.key = "k"; b2.script = { t2 };
        e2.addBinding(b2);
        e2.dispatch(press("p1", "y", 1000));
        e2.dispatch(press("p3", "a", 1100));      // wrong source: must NOT satisfy p2|a
        check(taps(*rec2, "k") == 0, "a2-full-cross-device-precision", "");
        e2.dispatch(press("p2", "a", 1200));
        check(taps(*rec2, "k") == 1, "a2-full-fires", "");
    }

    // ---- A3: per-binding debounce isolation ------------------------------
    section("A3 debounce isolation across bindings");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding bd; bd.id = "deb"; bd.match = {"s", "b", EventType::Pressed};
        bd.trigger = TriggerKind::Press; bd.debounceMs = 200;
        Action t; t.kind = ActionKind::KeyTap; t.key = "d"; bd.script = { t };
        Binding bo; bo.id = "nodeb"; bo.match = {"s", "b", EventType::Pressed};
        bo.trigger = TriggerKind::Press;
        Action t2; t2.kind = ActionKind::KeyTap; t2.key = "o"; bo.script = { t2 };
        e.addBinding(bd);
        e.addBinding(bo);
        e.dispatch(press("s", "b", 1000));
        e.dispatch(press("s", "b", 1050)); // within debounce window
        check(taps(*rec, "d") == 1, "a3-debounced-binding-fires-once", "");
        check(taps(*rec, "o") == 2, "a3-other-binding-not-suppressed", "");
    }

    // ---- A4: trigger-state staleness -------------------------------------
    section("A4 stale trigger state after cancel/disconnect/switch");
    {
        // cancelAll must not leave a live LongPress latch
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "lp"; b.match = {"s", "x", EventType::Pressed};
        b.trigger = TriggerKind::LongPress; b.longPressMs = 50;
        Action t; t.kind = ActionKind::KeyTap; t.key = "x"; b.script = { t };
        e.addBinding(b);
        e.dispatch(press("s", "x", 1000));
        e.cancelAll();
        e.tick(2000); // long after longPressMs: stale held must not fire
        check(taps(*rec, "x") == 0, "a4-no-fire-after-cancelAll", "");

        // cancelSource (disconnect) mid-hold must not fire later
        auto rec2 = std::make_shared<RecordOutput>();
        Engine e2(rec2);
        Binding b2; b2.id = "lp2"; b2.match = {"pad", "x", EventType::Pressed};
        b2.trigger = TriggerKind::LongPress; b2.longPressMs = 50;
        Action t2; t2.kind = ActionKind::KeyTap; t2.key = "x"; b2.script = { t2 };
        e2.addBinding(b2);
        e2.dispatch(press("pad", "x", 1000));
        e2.cancelSource("pad"); // device disconnects while held
        e2.tick(2000);
        check(taps(*rec2, "x") == 0, "a4-no-fire-after-disconnect", "");

        // profile switch must not carry a stale toggle/held into the new profile
        auto rec3 = std::make_shared<RecordOutput>();
        Engine e3(rec3);
        Binding b3; b3.id = "tg"; b3.match = {"s", "y", EventType::Pressed};
        b3.trigger = TriggerKind::Toggle; b3.holdKey = "w";
        Binding lp3; lp3.id = "lp3"; lp3.match = {"s", "z", EventType::Pressed};
        lp3.trigger = TriggerKind::LongPress; lp3.longPressMs = 50;
        Action t3; t3.kind = ActionKind::KeyTap; t3.key = "q"; lp3.script = { t3 };
        std::string err;
        std::string json = R"({"profiles":{"default":[
            {"id":"tg","trigger":"toggle","source":"s","control":"y","type":"pressed","holdKey":"w"},
            {"id":"lp3","trigger":"long_press","source":"s","control":"z","type":"pressed","longPressMs":50,
             "script":[{"action":"key_tap","key":"q"}]}
        ],"other":[]}})";
        check(e3.loadProfileText(json, &err), "a4-profile-loaded", err);
        e3.dispatch(press("s", "y", 1000));          // toggle ON
        e3.dispatch(press("s", "z", 1100));          // long-press armed
        e3.activateProfile("other");                  // switch away mid-hold
        e3.tick(2000);
        check(taps(*rec3, "q") == 0, "a4-no-longpress-after-switch", "");
        check(!rec3->isHeld("w"), "a4-toggle-key-released-on-switch", "");
    }

    // ---- A5/A6: profile loading forms ------------------------------------
    section("A5/A6 profile load forms");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        std::string err;
        std::string json = R"({"profiles":{"work":{"bindings":[
            {"id":"w1","trigger":"press","source":"s","control":"c","type":"pressed",
             "script":[{"action":"key_tap","key":"a"}]}
        ]}}})";
        check(e.loadProfileText(json, &err), "a5-object-form-loads", err);
        check(e.bindingCount() == 1, "a5-bindings-not-lost",
              "count=" + std::to_string(e.bindingCount()));
        check(e.activeProfile() == "work", "a6-non-default-activated", e.activeProfile());
    }

    // ---- A7: output failure observability --------------------------------
    section("A7 output failure observability");
    {
        auto rec = std::make_shared<RecordOutput>();
        KeyOwnership own;
        size_t before = own.outputFailures();
        // unknown key: backend refuses; ownership must report + not leak
        bool ok = own.press("no_such_key", "h1", *rec);
        check(!ok, "a7-press-unknown-key-fails", "");
        check(own.outputFailures() > before, "a7-failure-counted", "");
        check(!own.isHeld("no_such_key"), "a7-no-phantom-hold", "");
        // release of a never-pressed key is a clean no-op, not a failure spike
        size_t mid = own.outputFailures();
        own.release("no_such_key", "h1", *rec);
        check(own.outputFailures() == mid, "a7-release-noop-not-failure", "");
    }

    // ---- A8: stop() interrupts long delays -------------------------------
    section("A8 stop() interrupts long-running scripts");
    {
        auto rec = std::make_shared<RecordOutput>();
        MappingRuntime rt(rec, 64);
        rt.engine().setRealtime(true);
        Binding b; b.id = "slow"; b.match = {"s", "c", EventType::Pressed};
        b.trigger = TriggerKind::Press;
        Action d; d.kind = ActionKind::Delay; d.delayMs = 5000;
        Action t; t.kind = ActionKind::KeyTap; t.key = "z";
        b.script = { d, t };
        rt.engine().addBinding(b);
        rt.start();
        rt.post(press("s", "c", 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // enter the delay
        auto t0 = std::chrono::steady_clock::now();
        rt.stop();
        long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        check(ms < 1500, "a8-stop-prompt", "stop took " + std::to_string(ms) + "ms");
        check(taps(*rec, "z") == 0, "a8-script-aborted", "");
        check(rec->heldKeys().empty(), "a8-no-keys-held", "");
    }

    // ---- A9: time-based triggers under load ------------------------------
    section("A9 HoldRepeat keeps firing under sustained load");
    {
        auto rec = std::make_shared<RecordOutput>();
        MappingRuntime rt(rec, 2048);
        rt.engine().setRealtime(true);
        Binding b; b.id = "hr"; b.match = {"s", "c", EventType::Pressed};
        b.trigger = TriggerKind::HoldRepeat; b.longPressMs = 50; b.repeatMs = 50;
        Action t; t.kind = ActionKind::KeyTap; t.key = "h";
        b.script = { t };
        rt.engine().addBinding(b);
        rt.start();
        rt.post(press("s", "c", 1));
        // flood the queue with unrelated events for 400 ms so pop() never idles
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(400)) {
            rt.post(Event{"noise", "n", EventType::ValueChanged, 0.5, false, "", 0, "test"});
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        int fired = taps(*rec, "h");
        check(fired >= 2, "a9-holdrepeat-under-load",
              "fired=" + std::to_string(fired));
        rt.stop();
    }

    // ---- A10: key table + CharToStroke -----------------------------------
    section("A10 key table completeness and CharToStroke");
    {
        check(FindKey("f13") && FindKey("f24"), "a10-f13-f24-present", "");
        check(FindKey("escape") && FindKey("f13") != nullptr, "a10-escape-alias", "");
        std::string k; bool sh = false;
        check(CharToStroke('a', k, sh) && k == "a" && !sh, "a10-char-lower", k);
        check(CharToStroke('A', k, sh) && k == "a" && sh, "a10-char-upper", k);
        check(CharToStroke('!', k, sh) && k == "1" && sh, "a10-char-shifted-digit", k);
        check(CharToStroke(' ', k, sh) && k == "space" && !sh, "a10-char-space", k);
        check(CharToStroke('\n', k, sh) && k == "enter" && !sh, "a10-char-enter", k);
        check(!CharToStroke((char)0x01, k, sh), "a10-unsupported-char-rejected", "");
    }

    // ---- A11: config key validation -------------------------------------
    section("A11 config rejects unknown key names");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        std::vector<ConfigError> errs;
        bool ok = LoadConfigText(R"({"profiles":{"default":[
            {"id":"bad","trigger":"press","source":"s","control":"c","type":"pressed",
             "script":[{"action":"key_tap","key":"no_such_key"}]}
        ]}})", e, errs);
        check(!ok, "a11-unknown-script-key-rejected", "");
        check(!errs.empty() && errs[0].toText().find("no_such_key") != std::string::npos,
              "a11-error-names-the-key", errs.empty() ? "" : errs[0].toText());

        errs.clear();
        bool ok2 = LoadConfigText(R"({"profiles":{"default":[
            {"id":"bad2","trigger":"momentary","source":"s","control":"c","type":"pressed",
             "holdKey":"not_a_key"}
        ]}})", e, errs);
        check(!ok2, "a11-unknown-holdkey-rejected", "");
    }

    std::printf("\n%d checks, %d passed, %d failed\n", g_check, g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
