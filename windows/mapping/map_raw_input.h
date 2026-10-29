// ---------------------------------------------------------------------------
// map_raw_input.h -- Windows Raw Input adapter.
//
// Captures real keyboard/mouse input via the Win32 Raw Input API (WM_INPUT) on
// a dedicated message-only window thread, and normalizes it into the engine's
// device-agnostic Event. Also enumerates input devices and EXCLUDES TextDeck's
// own virtual-HID keyboard output (VID_1209&PID_0001) so typed output cannot
// re-trigger its own mapping (feedback loop).
//
// Scope (reliably decoded):
//   * Keyboard (RIM_TYPEKEYBOARD)  -> Pressed/Released per key.
//   * Mouse    (RIM_TYPEMOUSE)     -> button Pressed/Released + movement ValueChanged.
//   * Generic HID (RIM_TYPEHID)    -> enumerated + diagnosed, NOT guessed into
//                                     buttons/axes (needs a dedicated adapter).
//
// The device identity used for `Event::source` is the stable device PATH
// (VID/PID/instance), never the session-local handle.
// ---------------------------------------------------------------------------
#pragma once

#include "map_core.h"
#include "map_engine.h"   // IInputSource

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace map {

// Fixed Win32 Raw Input flag values, redeclared so the normalize helpers are
// unit-testable without pulling in <windows.h>. These are stable ABI constants.
constexpr unsigned kRiKeyBreak         = 0x0001;
constexpr unsigned kRiMouseLeftDown    = 0x0001;
constexpr unsigned kRiMouseLeftUp      = 0x0002;
constexpr unsigned kRiMouseRightDown   = 0x0004;
constexpr unsigned kRiMouseRightUp     = 0x0008;
constexpr unsigned kRiMouseMiddleDown  = 0x0010;
constexpr unsigned kRiMouseMiddleUp    = 0x0020;
constexpr unsigned kRiMouseX1Down      = 0x0040;
constexpr unsigned kRiMouseX1Up        = 0x0080;
constexpr unsigned kRiMouseX2Down      = 0x0100;
constexpr unsigned kRiMouseX2Up        = 0x0200;

enum class RawDevType : int { Hid = 0, Keyboard = 1, Mouse = 2, Unknown = -1 };

struct RawDeviceInfo {
    std::string  path;       // stable device path (VID/PID/instance), not a handle
    unsigned     vid = 0, pid = 0;
    unsigned short usagePage = 0, usage = 0;
    RawDevType   type = RawDevType::Unknown;
    bool         ownOutput = false;   // TextDeck's own virtual-HID keyboard
};

// ---- pure normalizers (unit-testable, no Win32) ----------------------------
// Keyboard: out.control = canonical key name ("a","space","left",...) or
// "vk_<n>"; out.type = Released if (flags & kRiKeyBreak) else Pressed.
bool NormalizeKeyboard(unsigned short makeCode, unsigned short flags,
                       unsigned short vk, const std::string& source, Event& out);

// Mouse buttons: appends Pressed/Released events for each button edge.
void NormalizeMouseButtons(unsigned buttonFlags, const std::string& source,
                           std::vector<Event>& out);

// Mouse movement: appends ValueChanged for nonzero dx/dy.
void NormalizeMouseMove(long dx, long dy, const std::string& source,
                        std::vector<Event>& out);

// ---------------------------------------------------------------------------
class RawInputSource final : public IInputSource {
public:
    explicit RawInputSource(std::string sourcePrefix = "raw");
    ~RawInputSource() override;

    bool start(Sink sink, std::string* err = nullptr) override;
    void stop() override;
    const char* kind() const override { return "rawinput"; }
    const char* sourceId() const override;

    std::vector<RawDeviceInfo> devices() const;
    std::vector<std::string>   diagnostics() const;
    std::vector<std::string>   captureLog() const; // per-event hDevice/path/injected
    size_t ignoredOwnOutputCount() const;
    size_t undecodedReportCount() const;
    bool running() const { return running_; }

    // Test hooks: drive the SAME normalize+emit path the live WM_INPUT handler
    // uses, so behaviour is identical whether events come from real devices or
    // from an injected synthetic input.
    void emitKeyboard(bool down, unsigned short vk, const std::string& source);
    void emitMouseButton(int button, bool down, const std::string& source);
    void emitMouseMove(long dx, long dy, const std::string& source);
    void emitUnknownHid(const std::string& source, unsigned reportBytes);

    // Live-path entry (called from the window thread on WM_INPUT).
    void onRawInput(void* hRawInput);
    void emit(const Event& e);   // also used for device-change lifecycle events

private:
    void addDiagnostic(const std::string& s);
    std::string keyControlName(unsigned short vk) const;

    std::string prefix_;
    Sink sink_;
    std::vector<RawDeviceInfo> devs_;
    std::vector<std::string>   diags_;
    std::vector<std::string>   captures_;   // formatted hDevice/path/injected lines
    size_t ignoredOwn_ = 0;
    size_t undecoded_ = 0;
    std::atomic<bool> running_{false};
    std::thread worker_;
    mutable std::mutex mu_;

    // Window-thread plumbing (opaque to keep this header free of <windows.h>).
    void* hwnd_ = nullptr;
    std::atomic<unsigned> threadId_{0};
    void threadMain();
    void enumerateDevices();
};

} // namespace map
