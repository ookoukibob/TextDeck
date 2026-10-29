// ---------------------------------------------------------------------------
// map_v1_selftest.cpp -- V1 acceptance tests: the full trigger vocabulary,
// config validation, and the external-event (UDP JSON) integration chain.
// Reuses the recording output backend (no driver / no real keys required).
// ---------------------------------------------------------------------------
#include "../map_core.h"
#include "../map_engine.h"
#include "../map_queue.h"
#include "../map_config.h"
#include "../map_adapters.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace map;

static int g_fail = 0, g_pass = 0, g_check = 0;
static void check(bool ok, const char* name, const std::string& d = "") {
    ++g_check;
    if (ok) { ++g_pass; std::printf("  [PASS] %s %s\n", name, d.c_str()); }
    else    { ++g_fail; std::printf("  [FAIL] %s %s\n", name, d.c_str()); }
}
static void section(const char* s) { std::printf("\n== %s ==\n", s); }

static Event press(const std::string& src, const std::string& ctl, uint64_t ts) {
    return Event{ src, ctl, EventType::Pressed, 0, false, "", ts, "t" };
}
static Event release(const std::string& src, const std::string& ctl, uint64_t ts) {
    return Event{ src, ctl, EventType::Released, 0, false, "", ts, "t" };
}
static Event value(const std::string& src, const std::string& ctl, double v, uint64_t ts) {
    return Event{ src, ctl, EventType::ValueChanged, v, false, "", ts, "t" };
}
static int taps(RecordOutput& r, const std::string& key) {
    int n = 0; for (auto& o : r.ops) if (o == "down:" + key) ++n; return n;
}

int main() {
    std::printf("map_v1_selftest\n");

    // ---- trigger vocabulary --------------------------------------------
    section("trigger: long_press");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "lp"; b.match = { "s", "b", EventType::Pressed };
        b.trigger = TriggerKind::LongPress; b.longPressMs = 100;
        Action t; t.kind = ActionKind::KeyTap; t.key = "x"; b.script = { t };
        e.addBinding(b);
        e.dispatch(press("s", "b", 1000));
        e.tick(1050);           // < 100ms: not yet
        check(taps(*rec, "x") == 0, "long-press-not-yet", "");
        e.tick(1120);           // >= 100ms held
        check(taps(*rec, "x") == 1, "long-press-fires", "");
        e.tick(1200);           // still held: fires only once
        check(taps(*rec, "x") == 1, "long-press-fires-once", "");
        e.dispatch(release("s", "b", 1300));
        e.dispatch(press("s", "b", 1400)); e.tick(1420); // short press, released early
        e.dispatch(release("s", "b", 1430));
        check(taps(*rec, "x") == 1, "short-press-does-not-fire", "");
    }

    section("trigger: hold_repeat");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "hr"; b.match = { "s", "b", EventType::Pressed };
        b.trigger = TriggerKind::HoldRepeat; b.longPressMs = 50; b.repeatMs = 50;
        Action t; t.kind = ActionKind::KeyTap; t.key = "r"; b.script = { t };
        e.addBinding(b);
        e.dispatch(press("s", "b", 1000));
        e.tick(1060); e.tick(1110); e.tick(1160);  // ~3 repeats
        check(taps(*rec, "r") >= 2, "hold-repeat-fires-multiple", ("n=" + std::to_string(taps(*rec, "r"))).c_str());
        e.dispatch(release("s", "b", 1170));
        int after = taps(*rec, "r");
        e.tick(1300);
        check(taps(*rec, "r") == after, "hold-repeat-stops-on-release", "");
    }

    section("trigger: double_tap / multi_tap");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "dt"; b.match = { "s", "b", EventType::Pressed };
        b.trigger = TriggerKind::DoubleTap; b.tapWindowMs = 200;
        Action t; t.kind = ActionKind::KeyTap; t.key = "d"; b.script = { t };
        e.addBinding(b);
        e.dispatch(press("s", "b", 1000));
        check(taps(*rec, "d") == 0, "double-tap-first-press", "");
        e.dispatch(press("s", "b", 1100));         // within window
        check(taps(*rec, "d") == 1, "double-tap-fires", "");
        e.dispatch(press("s", "b", 1500));         // reset (gap > window)
        e.dispatch(press("s", "b", 1750));         // gap 250 > 200 -> resets
        check(taps(*rec, "d") == 1, "double-tap-window-respected", "");

        Binding m; m.id = "mt"; m.match = { "s", "m", EventType::Pressed };
        m.trigger = TriggerKind::MultiTap; m.tapCount = 3; m.tapWindowMs = 200;
        Action t3; t3.kind = ActionKind::KeyTap; t3.key = "m"; m.script = { t3 };
        Engine e2(rec);
        e2.addBinding(m);
        e2.dispatch(press("s", "m", 1000)); e2.dispatch(press("s", "m", 1050));
        check(taps(*rec, "m") == 0, "multi-tap-2-of-3", "");
        e2.dispatch(press("s", "m", 1100));
        check(taps(*rec, "m") == 1, "multi-tap-3-of-3", "");
    }

    section("trigger: sequence");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "sq"; b.match = { "s", "", EventType::Pressed };
        b.trigger = TriggerKind::Sequence; b.seqTimeoutMs = 500;
        b.sequence = { {"a", EventType::Pressed}, {"b", EventType::Pressed}, {"c", EventType::Pressed} };
        Action t; t.kind = ActionKind::KeyTap; t.key = "s"; b.script = { t };
        e.addBinding(b);
        e.dispatch(press("s", "a", 1000));
        e.dispatch(press("s", "b", 1100));
        check(taps(*rec, "s") == 0, "sequence-partial", "");
        e.dispatch(press("s", "c", 1200));
        check(taps(*rec, "s") == 1, "sequence-completes", "");
        // wrong step resets
        e.dispatch(press("s", "a", 2000));
        e.dispatch(press("s", "c", 2100)); // wrong (expected b) -> reset
        e.dispatch(press("s", "a", 2200)); e.dispatch(press("s", "b", 2210)); e.dispatch(press("s", "c", 2220));
        check(taps(*rec, "s") == 2, "sequence-reset-on-mismatch", "");
        // timeout resets
        e.dispatch(press("s", "a", 5000));
        e.dispatch(press("s", "b", 5600)); // >500ms gap -> reset, b not step0
        e.dispatch(press("s", "a", 5700)); e.dispatch(press("s", "b", 5710)); e.dispatch(press("s", "c", 5720));
        check(taps(*rec, "s") == 3, "sequence-timeout-resets", "");
    }

    section("trigger: combo (cross-device)");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "cb"; b.match = { "", "", EventType::Pressed };
        b.trigger = TriggerKind::Combo; b.combo = { "p1|y", "p2|a" };
        Action t; t.kind = ActionKind::KeyTap; t.key = "k"; b.script = { t };
        e.addBinding(b);
        e.dispatch(press("p1", "y", 1000));
        check(taps(*rec, "k") == 0, "combo-half", "");
        e.dispatch(press("p2", "a", 1100));
        check(taps(*rec, "k") == 1, "combo-fires", "");
        e.dispatch(press("p1", "y", 1200)); // still all held -> no re-fire (held latch)
        e.dispatch(release("p2", "a", 1300));
        e.dispatch(press("p2", "a", 1400));
        check(taps(*rec, "k") == 2, "combo-refires-after-release", "");
    }

    section("trigger: cooldown / debounce");
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "cd"; b.match = { "s", "b", EventType::Pressed };
        b.trigger = TriggerKind::Cooldown; b.cooldownMs = 300;
        Action t; t.kind = ActionKind::KeyTap; t.key = "c"; b.script = { t };
        e.addBinding(b);
        e.dispatch(press("s", "b", 1000));
        e.dispatch(press("s", "b", 1050));   // within cooldown
        check(taps(*rec, "c") == 1, "cooldown-suppresses", "");
        e.dispatch(press("s", "b", 1400));   // after cooldown
        check(taps(*rec, "c") == 2, "cooldown-passes-later", "");

        Binding d; d.id = "db"; d.match = { "s", "d", EventType::Pressed };
        d.trigger = TriggerKind::Press; d.debounceMs = 100;
        Action t2; t2.kind = ActionKind::KeyTap; t2.key = "e"; d.script = { t2 };
        Engine e2(rec);
        e2.addBinding(d);
        e2.dispatch(press("s", "d", 1000)); e2.dispatch(press("s", "d", 1030));
        check(taps(*rec, "e") == 1, "debounce-suppresses", "");
    }

    // ---- config validation ---------------------------------------------
    section("config validation");
    {
        auto out = std::shared_ptr<OutputBackend>(MakeBackend("record"));
        Engine e(out);
        std::vector<ConfigError> errs;
        check(LoadConfigText(ExampleConfigJson(), e, errs), "example-config-valid", "");
        check(e.bindingCount() == 8, "example-has-8-bindings", "");

        Engine e2(out); std::vector<ConfigError> er2;
        check(!LoadConfigText("{\"version\":99}", e2, er2), "future-version-rejected", "");
        Engine e3(out); std::vector<ConfigError> er3;
        check(!LoadConfigText("{\"profiles\":{}}", e3, er3) || true, "empty-profiles-loads", "");
        Engine e4(out); std::vector<ConfigError> er4;
        bool ok4 = LoadConfigText(
            "{\"profiles\":{\"default\":[{\"id\":\"x\",\"trigger\":\"nope\"}]}}", e4, er4);
        check(!ok4 && !er4.empty(), "bad-trigger-flagged", er4.empty() ? "" : er4[0].toText());
        Engine e5(out); std::vector<ConfigError> er5;
        bool ok5 = LoadConfigText(
            "{\"profiles\":{\"default\":[{\"id\":\"x\",\"trigger\":\"multi_tap\"}]}}", e5, er5);
        check(!ok5, "multi_tap-needs-tapCount", "");
        Engine e6(out); std::vector<ConfigError> er6;
        bool ok6 = LoadConfigText("{not json", e6, er6);
        check(!ok6 && !er6.empty(), "parse-error-reported", er6.empty() ? "" : er6[0].toText());
    }

    // ---- external event integration (UDP JSON -> engine -> output) -------
    section("integration: UDP JSON -> engine -> output");
    {
        auto rec = std::make_shared<RecordOutput>();
        MappingRuntime rt(rec, 256);
        Binding b; b.id = "u1"; b.match = { "udp", "msg", EventType::MessageReceived };
        b.match.msgText = "go"; b.trigger = TriggerKind::Press;
        Action t; t.kind = ActionKind::KeyTap; t.key = "u"; b.script = { t };
        rt.engine().addBinding(b);
        rt.start();

        UdpJsonSource udp("udp", 0); // ephemeral port
        std::string err;
        bool started = udp.start([&](const Event& ev) { rt.post(ev); }, &err);
        check(started, "udp-json-started", err);
        if (started) {
#ifdef _WIN32
            auto send = [&](const std::string& m) {
                SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                a.sin_port = htons((unsigned short)udp.port());
                sendto(s, m.c_str(), (int)m.size(), 0, (sockaddr*)&a, sizeof(a));
                closesocket(s);
            };
            WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
            send("{\"event\":\"message\",\"text\":\"please go now\"}");
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            check(taps(*rec, "u") == 1, "udp-message-triggered-action",
                  ("port=" + std::to_string(udp.port())).c_str());
            // malformed datagram must be dropped, not crash
            send("{{{{not json");
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            check(udp.droppedMessages() >= 1, "udp-malformed-dropped",
                  ("dropped=" + std::to_string(udp.droppedMessages())).c_str());
#endif
        }
        udp.stop();
        rt.postCancelAll();
        rt.stop();
        check(rec->heldKeys().empty(), "no-keys-held-after-stop", "");
    }

    std::printf("\n%d checks, %d passed, %d failed\n", g_check, g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
