// ---------------------------------------------------------------------------
// map_mvp_main.cpp -- runnable MVP demo for the TextDeck input-mapping engine.
//
// Wires REAL input-source adapters (digital button, analog axis, UDP message)
// to the engine, which drives the unified output through a RECORDING backend
// so the demo never types into an arbitrary host window. It demonstrates and
// asserts the six required scenarios, then prints a summary and exits non-zero
// on any failure.
//
// Run on the host:  map_mvp.exe        (no driver, no elevation required)
// ---------------------------------------------------------------------------
#include "map_core.h"
#include "map_engine.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace map;

static int g_fail = 0;
static int g_pass = 0;

static void demo(bool ok, const char* name, const std::string& detail = "") {
    if (ok) { ++g_pass; std::printf("  [PASS] %s %s\n", name, detail.c_str()); }
    else    { ++g_fail; std::printf("  [FAIL] %s %s\n", name, detail.c_str()); }
}

#ifdef _WIN32
static bool udpSend(const char* host, int port, const std::string& msg) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((u_short)port);
    int r = sendto(s, msg.c_str(), (int)msg.size(), 0, (sockaddr*)&a, sizeof(a));
    closesocket(s);
    return r == (int)msg.size();
}
#endif

int main() {
    std::printf("map_mvp -- TextDeck input-mapping engine demo\n");
#ifdef _WIN32
    WSADATA wd; WSAStartup(MAKEWORD(2, 2), &wd);
#endif

    // --- scenario 1: button press -> Space held, release -> Space released ---
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        ButtonSource pad1("pad1");
        Binding b; b.id = "s1"; b.match = { "pad1", "button_a", EventType::Pressed };
        b.mode = BindingMode::Momentary; b.holdKey = "space";
        e.addBinding(b);
        pad1.start([&](const Event& ev) { e.dispatch(ev); });
        pad1.press("button_a", 100);
        demo(rec->isHeld("space"), "1-press-holds-space", "");
        pad1.release("button_a", 101);
        demo(!rec->isHeld("space"), "1-release-lifts-space", "");
        pad1.stop();
    }

    // --- scenario 2: analog axis threshold -> W held, back below -> released ---
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        AnalogSource stick("stick");
        Binding b; b.id = "s2"; b.match = { "stick", "axis_ly", EventType::ValueChanged };
        b.mode = BindingMode::Threshold; b.threshold = 0.6; b.hysteresis = 0.1; b.aboveKey = "w";
        e.addBinding(b);
        stick.start([&](const Event& ev) { e.dispatch(ev); });
        stick.set("axis_ly", 0.2, 100);
        demo(!rec->isHeld("w"), "2-below-holds-nothing", "");
        stick.set("axis_ly", 0.85, 101);
        demo(rec->isHeld("w"), "2-crossed-above-holds-w", "");
        stick.set("axis_ly", 0.30, 102);
        demo(!rec->isHeld("w"), "2-back-below-releases-w", "");
        stick.stop();
    }

    // --- scenario 3: custom UDP message -> Ctrl+Shift+P combo ---
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "s3"; b.match = { "udp", "msg", EventType::MessageReceived };
        b.match.msgText = "launch";
        Action combo; combo.kind = ActionKind::KeyCombo; combo.combo = { "ctrl", "shift", "p" };
        b.script = { combo };
        e.addBinding(b);

        UdpMessageSource udp("udp", 0); // ephemeral port
        std::string err;
        bool bound = udp.bind(&err);
        demo(bound, "3-udp-bound", err);
        if (bound) {
#ifdef _WIN32
            bool sent = udpSend("127.0.0.1", udp.port(), "please launch now");
#else
            bool sent = false;
#endif
            demo(sent, "3-udp-sent", "port=" + std::to_string(udp.port()));
            Event ev;
            bool got = udp.recvOne(2000, ev);
            demo(got, "3-udp-received", ev.text);
            if (got) e.dispatch(ev);
            bool comboSeen = false;
            for (const auto& o : rec->ops) if (o == "down:ctrl") comboSeen = true;
            demo(comboSeen, "3-combo-executed", "");
            demo(rec->heldKeys().empty(), "3-combo-released-everything", "");
        }
        udp.stop();
    }

    // --- scenario 4: one input -> multi-step script (down, delay, combo, text) ---
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b; b.id = "s4"; b.match = { "pad1", "macro", EventType::Pressed };
        Action d; d.kind = ActionKind::KeyDown; d.key = "a";
        Action delay; delay.kind = ActionKind::Delay; delay.delayMs = 5;
        Action up; up.kind = ActionKind::KeyUp; up.key = "a";
        Action combo; combo.kind = ActionKind::KeyCombo; combo.combo = { "ctrl", "shift", "p" };
        Action txt; txt.kind = ActionKind::TypeText; txt.text = "hello";
        b.script = { d, delay, up, combo, txt };
        e.addBinding(b);
        ButtonSource pad1("pad1");
        pad1.start([&](const Event& ev) { e.dispatch(ev); });
        pad1.press("macro", 100);
        bool sawText = false, sawCtrl = false;
        for (const auto& o : rec->ops) { if (o == "text:hello") sawText = true; if (o == "down:ctrl") sawCtrl = true; }
        demo(sawText && sawCtrl, "4-multi-step-script-executed", "");
        demo(rec->heldKeys().empty(), "4-no-keys-left-held", "");
        pad1.stop();
    }

    // --- scenario 5: two sources -> same key, no premature release ---
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b1; b1.id = "s5a"; b1.match = { "pad1", "b1", EventType::Pressed }; b1.mode = BindingMode::Momentary; b1.holdKey = "space";
        Binding b2; b2.id = "s5b"; b2.match = { "pad2", "b2", EventType::Pressed }; b2.mode = BindingMode::Momentary; b2.holdKey = "space";
        e.addBinding(b1); e.addBinding(b2);
        ButtonSource pad1("pad1"), pad2("pad2");
        pad1.start([&](const Event& ev) { e.dispatch(ev); });
        pad2.start([&](const Event& ev) { e.dispatch(ev); });
        pad1.press("b1", 100);
        pad2.press("b2", 101);
        pad1.release("b1", 102);
        demo(rec->isHeld("space"), "5-no-premature-release", "");
        pad2.release("b2", 103);
        demo(!rec->isHeld("space"), "5-released-when-last-holder-releases", "");
        pad1.stop(); pad2.stop();
    }

    // --- scenario 6: disconnect / cancel -> no stuck keys ---
    {
        auto rec = std::make_shared<RecordOutput>();
        Engine e(rec);
        Binding b1; b1.id = "s6a"; b1.match = { "pad1", "b1", EventType::Pressed }; b1.mode = BindingMode::Momentary; b1.holdKey = "space";
        Binding b2; b2.id = "s6b"; b2.match = { "pad2", "b2", EventType::Pressed }; b2.mode = BindingMode::Momentary; b2.holdKey = "w";
        e.addBinding(b1); e.addBinding(b2);
        ButtonSource pad1("pad1"), pad2("pad2");
        pad1.start([&](const Event& ev) { e.dispatch(ev); });
        pad2.start([&](const Event& ev) { e.dispatch(ev); });
        pad1.press("b1", 100);
        pad2.press("b2", 101);
        demo(rec->heldKeys().size() == 2, "6-two-keys-held", "");
        e.cancelSource("pad1");           // pad1 "disconnects"
        demo(!rec->isHeld("space") && rec->isHeld("w"), "6-disconnect-releases-only-its-keys", "");
        e.cancelAll();                    // emergency
        demo(rec->heldKeys().empty(), "6-emergency-releases-all", "");
        pad1.stop(); pad2.stop();
    }

    // --- backend factory sanity ---
    {
        auto sb = MakeBackend("sendinput");
        demo(std::string(sb->name()) == "sendinput", "backend-factory-sendinput", sb->name());
        auto hb = MakeBackend("hid");
        demo(std::string(hb->name()) == "textdeck-hid", "backend-factory-hid", hb->name());
    }

    std::printf("\nMVP demo: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
