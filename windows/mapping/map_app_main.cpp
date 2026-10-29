// ---------------------------------------------------------------------------
// map_app_main.cpp -- TextDeck Mapping V1 command-line entry point.
//
//   map_app example                     print a ready-to-run example config
//   map_app validate <config.json>      validate a config, report field errors
//   map_app devices                     list detected input devices
//   map_app run <config.json> [opts]    start the mapping engine
//         --output hid|sendinput|record   output backend (default: from config,
//                                         falling back to record = safe/no keys)
//         --udp <port>                    also listen on a UDP JSON port
//         --seconds <n>                   auto-stop after n seconds (0 = until Enter)
//
// The engine core is independent of any GUI. This CLI loads/validates a config,
// wires real input adapters, runs the mapping engine and cleans up (releasing
// every held key) on stop.
// ---------------------------------------------------------------------------
#include "map_config.h"
#include "map_core.h"
#include "map_engine.h"
#include "map_queue.h"
#include "map_raw_input.h"
#include "map_adapters.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>

using namespace map;

static void PrintErrors(const std::vector<ConfigError>& errs) {
    std::printf("config INVALID: %zu error(s)\n", errs.size());
    for (auto& e : errs) std::printf("  - %s\n", e.toText().c_str());
}

static int CmdExample() {
    std::printf("%s\n", ExampleConfigJson().c_str());
    return 0;
}

static int CmdValidate(const std::string& path) {
    std::shared_ptr<OutputBackend> out(MakeBackend("record"));
    Engine e(out);
    std::vector<ConfigError> errs;
    if (LoadConfigFile(path, e, errs)) {
        std::printf("config OK: %s\n  profiles loaded, active='%s', bindings=%zu\n",
                    path.c_str(), e.activeProfile().c_str(), e.bindingCount());
        return 0;
    }
    PrintErrors(errs);
    return 1;
}

static int CmdDevices() {
    std::printf("== Raw Input devices ==\n");
    RawInputSource raw("raw");
    std::string err;
    if (raw.start([](const Event&) {}, &err)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        for (auto& d : raw.devices()) {
            std::printf("  path=%s vid=%04X pid=%04X type=%d ownOutput=%d\n",
                        d.path.c_str(), d.vid, d.pid, (int)d.type, (int)d.ownOutput);
        }
        raw.stop();
    } else {
        std::printf("  (raw input unavailable: %s)\n", err.c_str());
    }
    std::printf("== XInput controllers ==\n");
    XInputSource xi("xinput");
    std::string xerr;
    if (xi.start([](const Event&) {}, &xerr)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        std::printf("  connected: %d\n", xi.connectedCount());
        xi.stop();
    } else {
        std::printf("  (xinput unavailable: %s)\n", xerr.c_str());
    }
    return 0;
}

static int CmdRun(const std::string& path, const std::string& outputOpt,
                  int udpPort, int seconds) {
    // 1. resolve output backend: --output wins; otherwise honor the config's
    //    "output" field; safe default = record (sends no keys).
    std::string backendKind = outputOpt;
    if (backendKind.empty()) {
        FILE* f = fopen(path.c_str(), "rb");
        if (f) {
            std::string s; char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
            fclose(f);
            std::string perr;
            JsonPtr root = ParseJson(s, &perr);
            if (root && root->isObject()) {
                std::string o = root->get("output")->s();
                if (!o.empty() && o != "default") backendKind = o;
            }
        }
    }
    if (backendKind.empty()) backendKind = "record";
    std::shared_ptr<OutputBackend> out(MakeBackend(backendKind));
    std::printf("output backend: %s\n", out->name());
    if (backendKind != "record")
        std::printf("NOTE: real keyboard output is enabled; keys will be sent to the focused window.\n");

    // 2. load + validate config
    Engine engine(out);
    std::vector<ConfigError> errs;
    if (!LoadConfigFile(path, engine, errs)) { PrintErrors(errs); return 1; }
    std::printf("config loaded: %s (active profile '%s', %zu bindings)\n",
                path.c_str(), engine.activeProfile().c_str(), engine.bindingCount());

    MappingRuntime rt(out, 2048);
    rt.engine().setRealtime(true);

    // reload config into the runtime's engine (single owner of Engine is the runtime)
    {
        std::vector<ConfigError> e2;
        if (!LoadConfigFile(path, rt.engine(), e2)) { PrintErrors(e2); return 1; }
    }

    // 3. start adapters -> post events into the runtime
    RawInputSource raw("raw");
    std::string aerr;
    if (!raw.start([&](const Event& ev) { rt.post(ev); }, &aerr))
        std::printf("warn: raw input not started (%s)\n", aerr.c_str());
    else
        std::printf("raw input: started (%zu devices)\n", raw.devices().size());

    XInputSource xin("xinput");
    if (xin.start([&](const Event& ev) { rt.post(ev); }, &aerr))
        std::printf("xinput: started\n");
    else
        std::printf("xinput: not started (%s)\n", aerr.c_str());

    std::unique_ptr<UdpJsonSource> udp;
    if (udpPort > 0) {
        udp.reset(new UdpJsonSource("udp", udpPort));
        if (udp->start([&](const Event& ev) { rt.post(ev); }, &aerr))
            std::printf("udp-json: listening on 127.0.0.1:%d\n", udpPort);
        else
            std::printf("udp-json: not started (%s)\n", aerr.c_str());
    }

    rt.start();
    std::printf("mapping engine RUNNING. Press Enter to stop...\n");

    if (seconds > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
    } else {
        std::string line; std::getline(std::cin, line);
    }

    // 4. clean shutdown: stop adapters, cancel + release keys
    std::printf("stopping...\n");
    rt.postCancelAll();
    raw.stop();
    xin.stop();
    if (udp) udp->stop();
    rt.stop();
    std::printf("stopped; all held keys released. events dropped=%zu\n", rt.dropped());
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("TextDeck Mapping V1\n");
        std::printf("usage:\n");
        std::printf("  map_app example\n");
        std::printf("  map_app validate <config.json>\n");
        std::printf("  map_app devices\n");
        std::printf("  map_app run <config.json> [--output hid|sendinput|record] [--udp <port>] [--seconds <n>]\n");
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "example") return CmdExample();
    if (cmd == "validate" && argc >= 3) return CmdValidate(argv[2]);
    if (cmd == "devices") return CmdDevices();
    if (cmd == "run" && argc >= 3) {
        std::string output; int udpPort = 0, seconds = 0;
        for (int i = 3; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--output") == 0) output = argv[++i];
            else if (std::strcmp(argv[i], "--udp") == 0) udpPort = std::atoi(argv[++i]);
            else if (std::strcmp(argv[i], "--seconds") == 0) seconds = std::atoi(argv[++i]);
        }
        // also honor "output" field in config when --output not given
        return CmdRun(argv[2], output, udpPort, seconds);
    }
    std::printf("unknown command. use: example | validate | devices | run\n");
    return 2;
}
