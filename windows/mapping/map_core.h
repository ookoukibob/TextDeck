// ---------------------------------------------------------------------------
// map_core.h -- TextDeck input-mapping engine: normalized events, keys,
// output backends and key ownership. PLATFORM-NEUTRAL (no Win32 in this header;
// the Win32 backends live in map_core.cpp and only run on Windows).
//
// Written from scratch for TextDeck. No third-party source was copied. The
// event/action vocabulary is informed by the public READMEs of MIT-licensed
// remappers (DeviceMapper, FlexInput, UCR); see the audit notes in this module.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace map {

// ---------------------------------------------------------------------------
// Normalized, device-agnostic input event. Adapters translate their concrete
// source (HID, XInput, MIDI, serial, UDP, timer, ...) into this shape; the rule
// engine never sees VID/PID, driver names or a particular gamepad protocol.
// ---------------------------------------------------------------------------
enum class EventType : int {
    Pressed = 0,        // button / key / switch pressed
    Released = 1,       // ... released
    ValueChanged = 2,   // analog axis / knob / pressure / sensor value
    ThresholdCrossed = 3, // synthesized by the engine when a value crosses a threshold
    MessageReceived = 4,  // serial / network / app message
    TimerElapsed = 5,     // periodic timer
    DeviceAdded = 6,      // device arrival (lifecycle)
    DeviceRemoved = 7,    // device removal (lifecycle)
};

struct Event {
    std::string source;      // stable source id, e.g. "pad1", "udp:9000", "timer1"
    std::string control;     // control id, e.g. "button_a", "axis_lx", "msg"
    EventType   type = EventType::Pressed;
    double      value = 0.0; // ValueChanged magnitude; ThresholdCrossed level
    bool        above = false; // ThresholdCrossed direction (value >= threshold)
    std::string text;        // MessageReceived payload (optional)
    uint64_t    ts = 0;      // monotonic milliseconds
    std::string sourceKind;  // adapter kind, for UI/logging only
};

// ---------------------------------------------------------------------------
// Key tables. A "key" is a canonical lowercase name; backends resolve it to a
// concrete code. Keeping resolution data platform-neutral lets the mock/record
// backend and the unit tests run without Win32.
// ---------------------------------------------------------------------------
struct KeyInfo {
    const char* name;
    unsigned short vk;       // Windows virtual-key code (0 => no VK)
    unsigned char hidUsage;  // USB HID keyboard usage id
    unsigned char hidMod;    // HID modifier bitmap (0 for non-modifier keys)
    bool isModifier;
};
const KeyInfo* FindKey(const std::string& name);
const KeyInfo* FindKeyByVk(unsigned short vk); // reverse lookup, for Raw Input

// ASCII char -> (key name, needsShift) for the type_text action. Returns false
// for characters outside the supported US-ASCII printable set (plus \n,\t).
bool CharToStroke(char c, std::string& keyName, bool& needShift);

// ---------------------------------------------------------------------------
// Unified keyboard output. Every mapping action goes through this interface,
// never through a device-specific sender.
// ---------------------------------------------------------------------------
class OutputBackend {
public:
    virtual ~OutputBackend() = default;
    virtual bool keyDown(const std::string& key) = 0;   // false = key unknown/unsupported
    virtual bool keyUp(const std::string& key) = 0;
    virtual bool typeText(const std::string& text) = 0; // whole string, char by char
    virtual bool releaseAll() = 0;                       // emergency: drop everything
    virtual const char* name() const = 0;
};

// Recording backend: proves *what* would be sent, and tracks the live held set.
// Used by the unit tests and by the MVP demo for deterministic assertion.
class RecordOutput final : public OutputBackend {
public:
    std::vector<std::string> ops;          // e.g. "down:space", "up:space", "text:hi"
    bool keyDown(const std::string& key) override;
    bool keyUp(const std::string& key) override;
    bool typeText(const std::string& text) override;
    bool releaseAll() override;
    const char* name() const override { return "record"; }
    std::vector<std::string> heldKeys() const; // currently-down keys, sorted
    bool isHeld(const std::string& key) const;
private:
    std::map<std::string, int> held_;
};

// ---------------------------------------------------------------------------
// Key ownership: multiple sources (holders) may hold the same key at once; the
// physical key is released only when the last holder releases it. This is what
// stops a second device releasing "Space" from yanking it away from a first
// device that is still holding it.
// ---------------------------------------------------------------------------
class KeyOwnership {
public:
    // press returns true only on the physical up->down transition.
    bool press(const std::string& key, const std::string& holder, OutputBackend& out);
    // release returns true only on the physical down->up transition.
    bool release(const std::string& key, const std::string& holder, OutputBackend& out);
    // Drop every key held by one holder (device disconnect, script cancel,
    // profile switch, mapping disable, process exit, emergency stop).
    void releaseAllByHolder(const std::string& holder, OutputBackend& out);
    void releaseAll(OutputBackend& out); // emergency: drop everything
    bool isHeld(const std::string& key) const;
    std::vector<std::string> heldKeys() const;
    size_t heldCount() const { return keyRefs_.size(); }
    // Number of physical down/up transitions the backend refused (unknown key,
    // device missing, ...). Never resets; lets callers observe output failure
    // instead of silently pretending the key moved.
    size_t outputFailures() const { return outputFailures_; }
private:
    std::map<std::string, int> keyRefs_;                       // key -> total holders
    std::map<std::pair<std::string, std::string>, int> perHolder_; // (key,holder)->count
    size_t outputFailures_ = 0;
};

// Backend factory: "record" (deterministic, test/demo), "sendinput" (software
// mode, no driver), "hid" (TextDeck virtual-HID driver). Unknown => record.
std::unique_ptr<OutputBackend> MakeBackend(const std::string& kind);

// ---------------------------------------------------------------------------
// Device identity & feedback-loop prevention.
//
// Raw Input reports a device interface PATH (e.g.
// "\\?\HID#VID_1209&PID_0001&REV_0100#<instance>"). We extract the hardware
// VID/PID from that path - a structured, verifiable identifier - NOT a fuzzy
// display-name match. TextDeck's own virtual-HID keyboard output is
// HID\VID_1209&PID_0001 (see windows/driver/TextDeckHidDriver/Device.c), so
// any input device carrying that signature is our own output and must be
// excluded from mapping, otherwise typing re-triggers its own mapping (loop).
// ---------------------------------------------------------------------------
bool ParseVidPid(const std::string& devicePath, unsigned& vid, unsigned& pid);
bool IsOwnOutputDevice(const std::string& devicePath);  // VID_1209 & PID_0001

} // namespace map
