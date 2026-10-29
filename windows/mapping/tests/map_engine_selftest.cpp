// ---------------------------------------------------------------------------
// map_engine_selftest.cpp -- unit tests for the input-mapping engine core:
// event normalization & matching, press/release pairing, multi-source key
// ownership, debounce, threshold+hysteresis, delay/conditional/cancel/repeat
// actions, JSON profile loading, and disconnect / emergency cleanup.
//
// Runs without the kernel driver (uses the recording output backend).
// ---------------------------------------------------------------------------
#include "../map_core.h"
#include "../map_engine.h"
#include "../map_queue.h"
#include "../map_raw_input.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace map;

static int g_fail = 0;
static int g_pass = 0;
static int g_check = 0;

static void check(bool ok, const char* name, const char* detail = "") {
    ++g_check;
    if (ok) { ++g_pass; std::printf("  [PASS] %s %s\n", name, detail); }
    else    { ++g_fail; std::printf("  [FAIL] %s %s\n", name, detail); }
}

static void section(const char* s) { std::printf("\n== %s ==\n", s); }

static Event press(const std::string& src, const std::string& ctl, uint64_t ts) {
    return Event{ src, ctl, EventType::Pressed, 0.0, false, "", ts, "test" };
}
static Event release(const std::string& src, const std::string& ctl, uint64_t ts) {
    return Event{ src, ctl, EventType::Released, 0.0, false, "", ts, "test" };
}
static Event value(const std::string& src, const std::string& ctl, double v, uint64_t ts) {
    return Event{ src, ctl, EventType::ValueChanged, v, false, "", ts, "test" };
}

int main() {
    std::printf("map_engine_selftest\n");

    // 1. matching + normalization
    section("event matching");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "b1"; b.match.source = "pad1"; b.match.control = "button_a";
        b.match.type = EventType::Pressed; b.mode = BindingMode::Momentary; b.holdKey = "space";
        e.addBinding(b);
        e.dispatch(press("pad1", "button_a", 100));   // matches
        check(rec->isHeld("space"), "match-press-holds", "");
        e.dispatch(press("pad2", "button_a", 101));   // wrong source
        check(rec->heldKeys().size() == 1, "no-wrong-source-release", "");
        e.dispatch(press("pad1", "button_b", 102));   // wrong control
        check(rec->isHeld("space"), "still-held", "");
    }

    // 2. press/release pairing (momentary)
    section("press/release pairing");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "m1"; b.match = { "pad1", "button_a", EventType::Pressed };
        b.mode = BindingMode::Momentary; b.holdKey = "space";
        e.addBinding(b);
        e.dispatch(press("pad1", "button_a", 100));
        check(rec->isHeld("space"), "down", "");
        e.dispatch(release("pad1", "button_a", 101));
        check(!rec->isHeld("space"), "up", "");
        check(rec->ops.size() == 2 && rec->ops[0] == "down:space" && rec->ops[1] == "up:space",
              "balanced-ops", "");
    }

    // 3. multi-source key ownership
    section("multi-source key ownership");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b1; b1.id = "h1"; b1.match = { "pad1", "b1", EventType::Pressed }; b1.mode = BindingMode::Momentary; b1.holdKey = "space";
        Binding b2; b2.id = "h2"; b2.match = { "pad2", "b2", EventType::Pressed }; b2.mode = BindingMode::Momentary; b2.holdKey = "space";
        e.addBinding(b1); e.addBinding(b2);
        e.dispatch(press("pad1", "b1", 100));   // physical down
        e.dispatch(press("pad2", "b2", 101));   // already down, no new down
        e.dispatch(release("pad1", "b1", 102)); // still held by pad2
        check(rec->isHeld("space"), "not-prematurely-released", "");
        e.dispatch(release("pad2", "b2", 103)); // last holder -> up
        check(!rec->isHeld("space"), "released-when-last-holder-releases", "");
        // exactly one down and one up
        int downs = 0, ups = 0;
        for (auto& o : rec->ops) { if (o == "down:space") ++downs; if (o == "up:space") ++ups; }
        check(downs == 1 && ups == 1, "exactly-one-down-up", "");
    }

    // 4. debounce
    section("debounce");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "d1"; b.match = { "pad1", "b1", EventType::Pressed };
        b.mode = BindingMode::Script; b.debounceMs = 50;
        Action a; a.kind = ActionKind::KeyTap; a.key = "x";
        b.script.push_back(a);
        e.addBinding(b);
        e.dispatch(press("pad1", "b1", 100));   // fires (tap x)
        e.dispatch(press("pad1", "b1", 120));   // within 50ms -> ignored
        int taps = 0; for (auto& o : rec->ops) if (o == "down:x") ++taps;
        check(taps == 1, "debounce-suppresses-bounce", "");
        e.dispatch(press("pad1", "b1", 200));   // outside window -> fires
        taps = 0; for (auto& o : rec->ops) if (o == "down:x") ++taps;
        check(taps == 2, "debounce-passes-later-event", "");
    }

    // 5. threshold + hysteresis
    section("threshold + hysteresis");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "t1"; b.match = { "stick", "axis_ly", EventType::ValueChanged };
        b.mode = BindingMode::Threshold; b.threshold = 0.6; b.hysteresis = 0.1; b.aboveKey = "w";
        e.addBinding(b);
        e.dispatch(value("stick", "axis_ly", 0.3, 100));  // below
        check(!rec->isHeld("w"), "below-holds-nothing", "");
        e.dispatch(value("stick", "axis_ly", 0.7, 101));  // crosses up
        check(rec->isHeld("w"), "crossed-above-holds", "");
        e.dispatch(value("stick", "axis_ly", 0.55, 102)); // within hysteresis
        check(rec->isHeld("w"), "hysteresis-keeps-held", "");
        e.dispatch(value("stick", "axis_ly", 0.4, 103));  // below (threshold - hysteresis)
        check(!rec->isHeld("w"), "released-below-hysteresis", "");
    }

    // 6. script actions: combo, type_text, delay, repeat, conditional, cancel
    section("script actions");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "s1"; b.match = { "net", "msg", EventType::MessageReceived }; b.match.msgText = "launch";
        Action combo; combo.kind = ActionKind::KeyCombo; combo.combo = { "ctrl", "shift", "p" };
        Action txt; txt.kind = ActionKind::TypeText; txt.text = "hi";
        Action rep; rep.kind = ActionKind::Repeat; rep.count = 3;
        Action t; t.kind = ActionKind::KeyTap; t.key = "q"; rep.block.push_back(t);
        Action cond; cond.kind = ActionKind::Conditional; cond.cond = "held:nothing";
        Action condTap; condTap.kind = ActionKind::KeyTap; condTap.key = "z"; cond.block.push_back(condTap);
        b.script = { combo, txt, rep, cond };
        e.addBinding(b);
        e.dispatch(Event{ "net", "msg", EventType::MessageReceived, 0.0, false, "please launch now", 100, "udp" });
        int ctrlDowns = 0, qTaps = 0, zTaps = 0; bool sawText = false;
        for (auto& o : rec->ops) {
            if (o == "down:ctrl") ++ctrlDowns;
            if (o == "down:q") ++qTaps;
            if (o == "down:z") ++zTaps;
            if (o == "text:hi") sawText = true;
        }
        check(ctrlDowns == 1, "combo-pressed-ctrl", "");
        check(qTaps == 3, "repeat-ran-three-times", "");
        check(zTaps == 0, "conditional-false-block-skipped", "");
        check(sawText, "type-text-emitted", "");
        check(rec->heldKeys().empty(), "combo-released-everything", "");
    }

    // 7. cancel_sequence
    section("cancel sequence");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "c1"; b.match = { "pad1", "b1", EventType::Pressed };
        Action t1; t1.kind = ActionKind::KeyTap; t1.key = "a";
        Action cancel; cancel.kind = ActionKind::CancelSequence;
        Action t2; t2.kind = ActionKind::KeyTap; t2.key = "b";
        b.script = { t1, cancel, t2 };
        e.addBinding(b);
        e.dispatch(press("pad1", "b1", 100));
        bool sawA = false, sawB = false;
        for (auto& o : rec->ops) { if (o == "down:a") sawA = true; if (o == "down:b") sawB = true; }
        check(sawA && !sawB, "cancel-stops-remaining-steps", "");
    }

    // 8. toggle
    section("toggle");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "g1"; b.match = { "pad1", "b1", EventType::Pressed };
        b.mode = BindingMode::Toggle; b.holdKey = "shift";
        e.addBinding(b);
        e.dispatch(press("pad1", "b1", 100));
        check(rec->isHeld("shift"), "toggle-on", "");
        e.dispatch(press("pad1", "b1", 101));
        check(!rec->isHeld("shift"), "toggle-off", "");
    }

    // 9. disconnect cleanup (no stuck keys)
    section("disconnect cleanup");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "k1"; b.match = { "pad1", "b1", EventType::Pressed }; b.mode = BindingMode::Momentary; b.holdKey = "space";
        e.addBinding(b);
        e.dispatch(press("pad1", "b1", 100));
        check(rec->isHeld("space"), "held-before-disconnect", "");
        e.cancelSource("pad1");
        check(!rec->isHeld("space"), "released-after-disconnect", "");
        check(e.heldKeys().empty(), "no-stuck-keys", "");
    }

    // 10. emergency cancelAll
    section("emergency cancelAll");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b1; b1.id = "e1"; b1.match = { "pad1", "b1", EventType::Pressed }; b1.mode = BindingMode::Momentary; b1.holdKey = "space";
        Binding b2; b2.id = "e2"; b2.match = { "pad2", "b2", EventType::Pressed }; b2.mode = BindingMode::Momentary; b2.holdKey = "w";
        e.addBinding(b1); e.addBinding(b2);
        e.dispatch(press("pad1", "b1", 100));
        e.dispatch(press("pad2", "b2", 101));
        check(rec->heldKeys().size() == 2, "two-held", "");
        e.cancelAll();
        check(rec->heldKeys().empty(), "all-released", "");
    }

    // 11. JSON profile loading + unknown-field tolerance + profile switch
    section("json profile");
    {
        const char* json =
            "{\"profiles\":{"
            "\"default\":[{\"id\":\"a\",\"mode\":\"momentary\",\"source\":\"pad1\","
            "  \"control\":\"b1\",\"type\":\"pressed\",\"holdKey\":\"space\"}],"
            "\"game\":[{\"id\":\"g\",\"mode\":\"threshold\",\"source\":\"stick\","
            "  \"control\":\"axis_ly\",\"type\":\"value\",\"aboveKey\":\"w\","
            "  \"threshold\":0.6,\"hysteresis\":0.1}]"
            "}}";
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        std::string err;
        check(e.loadProfileText(json, &err), "profile-loads", err.c_str());
        e.activateProfile("game");
        check(e.activeProfile() == "game", "profile-switch", "");
        e.dispatch(value("stick", "axis_ly", 0.9, 100));
        check(rec->isHeld("w"), "game-profile-threshold-active", "");
        e.activateProfile("default");
        check(!rec->isHeld("w"), "switch-releases-keys", "");
        // malformed JSON must fail cleanly
        check(!e.loadProfileText("{not json", &err), "bad-json-fails", err.c_str());
    }

    // 12. two sources share one key via ownership manager directly
    section("key ownership manager");
    {
        auto rec = std::make_shared<RecordOutput>();
        KeyOwnership ko;
        ko.press("space", "srcA", *rec);
        ko.press("space", "srcB", *rec);
        check(rec->isHeld("space"), "held-by-two", "");
        ko.release("space", "srcA", *rec);
        check(rec->isHeld("space"), "still-held-by-B", "");
        ko.release("space", "srcB", *rec);
        check(!rec->isHeld("space"), "released", "");
        ko.releaseAllByHolder("srcA", *rec); // nothing held by srcA now
        check(rec->heldKeys().empty(), "releaseAllByHolder-idempotent", "");
    }

    // 13. device identity / feedback-loop prevention (VID/PID, not fuzzy name)
    section("device identity / own-output");
    {
        unsigned vid = 0, pid = 0;
        check(ParseVidPid("\\\\?\\HID#VID_1209&PID_0001&REV_0100#abc", vid, pid) &&
              vid == 0x1209 && pid == 0x0001, "parse-own-vidpid", "");
        check(ParseVidPid("\\\\?\\HID#VID_046D&PID_C077&MI_00#xy", vid, pid) &&
              vid == 0x046D && pid == 0xC077, "parse-logitech-vidpid", "");
        check(IsOwnOutputDevice("\\\\?\\HID#VID_1209&PID_0001&REV_0100#z"), "own-output-detected", "");
        check(!IsOwnOutputDevice("\\\\?\\HID#VID_046D&PID_C077#z"), "real-keyboard-not-own", "");
        check(!IsOwnOutputDevice("\\\\?\\ROOT#HIDCLASS#0000"), "no-vidpid-not-own", "");
    }

    // 14. event queue: order + control priority + never-drop-release + overflow
    section("event queue");
    {
        EventQueue q(4); // tiny cap to force overflow
        Event e; e.type = EventType::Pressed; e.control = "a";
        check(q.post(e), "post-ok", "");
        e.control = "b"; q.post(e);
        e.control = "c"; q.post(e);
        e.control = "d"; q.post(e);
        // now at cap; pushing a non-release should drop the OLDEST non-release ("a")
        e.control = "e"; bool clean = q.post(e);
        check(!clean, "overflow-dropped-something", "");
        check(q.dropped() == 1, "drop-counted", "");
        // FIFO order preserved minus the dropped "a": b,c,d,e
        QueueItem it; std::string order;
        while (q.pop(it, 0)) order += it.ev.control;
        check(order == "bcde", "fifo-order-preserved", order.c_str());

        // releases are never dropped under pressure
        EventQueue q2(2);
        Event r; r.type = EventType::Released; r.control = "x";
        for (int i = 0; i < 5; ++i) { Event s = r; s.control = "r" + std::to_string(i); q2.post(s); }
        check(q2.dropped() == 0, "releases-never-dropped", "");
        size_t cnt = 0; while (q2.pop(it, 0)) ++cnt;
        check(cnt == 5, "all-releases-retained", "");

        // control channel is processed before queued events
        EventQueue q3;
        Event p1; p1.type = EventType::Pressed; p1.control = "pending";
        q3.post(p1);
        q3.postCancelAll();
        QueueItem first;
        check(q3.pop(first, 0) && first.kind == QueueItemKind::CancelAll, "control-first", "");
    }

    // 15. mapping runtime: queue isolation (delay script must not block collector)
    section("runtime queue isolation");
    {
        auto rec = std::make_shared<RecordOutput>();
        MappingRuntime rt(rec, 256);
        // a script that holds a key then delays then releases (realtime so it sleeps)
        Binding b; b.id = "iso"; b.match = { "src", "btn", EventType::Pressed };
        Action d; d.kind = ActionKind::KeyDown; d.key = "space";
        Action del; del.kind = ActionKind::Delay; del.delayMs = 60;
        Action up; up.kind = ActionKind::KeyUp; up.key = "space";
        b.script = { d, del, up };
        rt.engine().addBinding(b);
        rt.engine().setRealtime(true);
        rt.start();
        // post press + many events quickly; the collector (this thread) is never blocked
        Event press; press.source = "src"; press.control = "btn"; press.type = EventType::Pressed;
        rt.post(press);
        // simulate concurrent collection while the script is sleeping
        for (int i = 0; i < 200; ++i) {
            Event v; v.source = "noise"; v.control = "n"; v.type = EventType::ValueChanged; v.value = i;
            rt.post(v);
        }
        Event rel; rel.source = "src"; rel.control = "btn"; rel.type = EventType::Released;
        rt.post(rel);
        // give the executor time to finish
        for (int i = 0; i < 200 && rt.pending() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        check(rt.dropped() == 0, "no-drops-under-load", ("dropped=" + std::to_string(rt.dropped())).c_str());
        check(!rec->isHeld("space"), "key-not-stuck-after-script", "");
        rt.stop();
    }

    // 16. raw input normalizers (pure)
    section("raw input normalizers");
    {
        Event e;
        check(NormalizeKeyboard(0, 0, 0x20, "kb1", e) && e.type == EventType::Pressed &&
              e.control == "space", "kb-down-space", e.control.c_str());
        check(NormalizeKeyboard(0, kRiKeyBreak, 0x20, "kb1", e) && e.type == EventType::Released,
              "kb-up-space", "");
        check(NormalizeKeyboard(0, 0, 0x41, "kb1", e) && e.control == "a", "kb-down-a", e.control.c_str());
        std::vector<Event> evs;
        NormalizeMouseButtons(kRiMouseLeftDown | kRiMouseRightUp, "m1", evs);
        check(evs.size() == 2 && evs[0].control == "mouse_left" && evs[0].type == EventType::Pressed &&
              evs[1].control == "mouse_right" && evs[1].type == EventType::Released, "mouse-buttons", "");
        evs.clear();
        NormalizeMouseMove(5, -3, "m1", evs);
        check(evs.size() == 2 && evs[0].control == "mouse_dx" && evs[1].control == "mouse_dy", "mouse-move", "");
    }

    // 17. RawInputSource own-output filtering (feedback-loop prevention)
    section("raw input feedback-loop filter");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "fb"; b.match.type = EventType::Pressed; b.mode = BindingMode::Momentary; b.holdKey = "space";
        b.match.source = ""; b.match.control = ""; // match any keyboard
        e.addBinding(b);
        RawInputSource raw("raw");
        raw.start([&](const Event& ev) { e.dispatch(ev); });
        // a REAL keyboard (different VID/PID) must still map
        raw.emitKeyboard(true, 0x20, "\\\\?\\HID#VID_046D&PID_C077#real");
        check(rec->isHeld("space"), "real-keyboard-maps", "");
        raw.emitKeyboard(false, 0x20, "\\\\?\\HID#VID_046D&PID_C077#real");
        // our OWN virtual-HID keyboard output must NOT map (else infinite loop)
        raw.emitKeyboard(true, 0x41, "\\\\?\\HID#VID_1209&PID_0001&REV_0100#own");
        check(!rec->isHeld("a"), "own-output-not-mapped", "");
        check(raw.ignoredOwnOutputCount() >= 1, "own-output-counted", "");
        raw.emitUnknownHid("\\\\?\\HID#VID_1234&PID_5678#unk", 8);
        check(raw.undecodedReportCount() == 1 && raw.diagnostics().size() == 1,
              "unknown-hid-diagnosed-not-guessed", "");
        raw.stop();
    }

    std::printf("\n%d checks, %d passed, %d failed\n", g_check, g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
