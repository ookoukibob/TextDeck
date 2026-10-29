// map_core.cpp -- implementation of key resolution, recording backend, key
// ownership, and the two real Windows output backends (SendInput and the
// TextDeck virtual-HID driver IOCTL).
#include "map_core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace map {

// ---------------------------------------------------------------------------
// Key tables. US QWERTY layout, matching the existing vhid::CharToStroke.
// hidUsage/hidMod follow USB HID Usage Tables 1.12 (Keyboard/Keypad).
// ---------------------------------------------------------------------------
static const KeyInfo kKeys[] = {
    // modifiers
    {"ctrl",  0x11, 0xE0, 0x01, true},
    {"shift", 0x10, 0xE1, 0x02, true},
    {"alt",   0x12, 0xE2, 0x04, true},
    {"gui",   0x5B, 0xE3, 0x08, true},
    {"win",   0x5B, 0xE3, 0x08, true},
    // letters a-z (HID usage 0x04..0x1D)
    {"a",0x41,0x04,0,false},{"b",0x42,0x05,0,false},{"c",0x43,0x06,0,false},
    {"d",0x44,0x07,0,false},{"e",0x45,0x08,0,false},{"f",0x46,0x09,0,false},
    {"g",0x47,0x0A,0,false},{"h",0x48,0x0B,0,false},{"i",0x49,0x0C,0,false},
    {"j",0x4A,0x0D,0,false},{"k",0x4B,0x0E,0,false},{"l",0x4C,0x0F,0,false},
    {"m",0x4D,0x10,0,false},{"n",0x4E,0x11,0,false},{"o",0x4F,0x12,0,false},
    {"p",0x50,0x13,0,false},{"q",0x51,0x14,0,false},{"r",0x52,0x15,0,false},
    {"s",0x53,0x16,0,false},{"t",0x54,0x17,0,false},{"u",0x55,0x18,0,false},
    {"v",0x56,0x19,0,false},{"w",0x57,0x1A,0,false},{"x",0x58,0x1B,0,false},
    {"y",0x59,0x1C,0,false},{"z",0x5A,0x1D,0,false},
    // digits 1..9,0
    {"1",0x31,0x1E,0,false},{"2",0x32,0x1F,0,false},{"3",0x33,0x20,0,false},
    {"4",0x34,0x21,0,false},{"5",0x35,0x22,0,false},{"6",0x36,0x23,0,false},
    {"7",0x37,0x24,0,false},{"8",0x38,0x25,0,false},{"9",0x39,0x26,0,false},
    {"0",0x30,0x27,0,false},
    // whitespace / editing
    {"enter",0x0D,0x28,0,false},{"esc",0x1B,0x29,0,false},
    {"backspace",0x08,0x2A,0,false},{"tab",0x09,0x2B,0,false},
    {"space",0x20,0x2C,0,false},{"caps",0x14,0x39,0,false},
    // function keys
    {"f1",0x70,0x3A,0,false},{"f2",0x71,0x3B,0,false},{"f3",0x72,0x3C,0,false},
    {"f4",0x73,0x3D,0,false},{"f5",0x74,0x3E,0,false},{"f6",0x75,0x3F,0,false},
    {"f7",0x76,0x40,0,false},{"f8",0x77,0x41,0,false},{"f9",0x78,0x42,0,false},
    {"f10",0x79,0x43,0,false},{"f11",0x7A,0x44,0,false},{"f12",0x7B,0x45,0,false},
    {"f13",0x7C,0x68,0,false},{"f14",0x7D,0x69,0,false},{"f15",0x7E,0x6A,0,false},
    {"f16",0x7F,0x6B,0,false},{"f17",0x80,0x6C,0,false},{"f18",0x81,0x6D,0,false},
    {"f19",0x82,0x6E,0,false},{"f20",0x83,0x6F,0,false},{"f21",0x84,0x70,0,false},
    {"f22",0x85,0x71,0,false},{"f23",0x86,0x72,0,false},{"f24",0x87,0x73,0,false},
    // aliases (common spellings)
    {"escape",0x1B,0x29,0,false},{"return",0x0D,0x28,0,false},{"del",0x2E,0x4C,0,false},
    // arrows
    {"right",0x27,0x4F,0,false},{"left",0x25,0x50,0,false},
    {"down",0x28,0x51,0,false},{"up",0x26,0x52,0,false},
    // punctuation (US)
    {"-",0xBD,0x2D,0,false},{"=",0xBB,0x2E,0,false},
    {"[",0xDB,0x2F,0,false},{"]",0xDD,0x30,0,false},
    {";",0xBA,0x33,0,false},{"'",0xDE,0x34,0,false},
    {",",0xBC,0x36,0,false},{".",0xBE,0x37,0,false},
    {"/",0xBF,0x38,0,false},{"`",0xC0,0x35,0,false},
    {"\\",0xDC,0x31,0,false},
};

const KeyInfo* FindKey(const std::string& name) {
    for (const KeyInfo& k : kKeys) {
        if (name == k.name) return &k;
    }
    return nullptr;
}

const KeyInfo* FindKeyByVk(unsigned short vk) {
    if (vk == 0) return nullptr;
    for (const KeyInfo& k : kKeys) {
        if (k.vk == vk) return &k;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// CharToStroke: ASCII char -> (key, shift) for the type_text action. US QWERTY,
// the same layout assumption as kKeys / vhid::CharToStroke.
// ---------------------------------------------------------------------------
bool CharToStroke(char c, std::string& keyName, bool& needShift) {
    needShift = false;
    if (c >= 'a' && c <= 'z') { keyName = std::string(1, c); return true; }
    if (c >= 'A' && c <= 'Z') { keyName = std::string(1, (char)(c - 'A' + 'a')); needShift = true; return true; }
    if (c >= '1' && c <= '9') { keyName = std::string(1, c); return true; }
    if (c == '0') { keyName = "0"; return true; }
    if (c == ' ' ) { keyName = "space"; return true; }
    if (c == '\n') { keyName = "enter"; return true; }
    if (c == '\t') { keyName = "tab"; return true; }
    // unshifted punctuation
    switch (c) {
        case '-': keyName = "-"; return true;
        case '=': keyName = "="; return true;
        case '[': keyName = "["; return true;
        case ']': keyName = "]"; return true;
        case ';': keyName = ";"; return true;
        case '\'': keyName = "'"; return true;
        case ',': keyName = ","; return true;
        case '.': keyName = "."; return true;
        case '/': keyName = "/"; return true;
        case '`': keyName = "`"; return true;
        case '\\': keyName = "\\"; return true;
    }
    // shifted punctuation -> base key + shift
    struct Shifted { char ch; const char* key; };
    static const Shifted kShifted[] = {
        {'!',"1"},{'@',"2"},{'#',"3"},{'$',"4"},{'%',"5"},{'^',"6"},{'&',"7"},{'*',"8"},{'(',"9"},{')',"0"},
        {'_',"-"},{'+',"="},{'{',"["},{'}',"]"},{':',";"},{'"',"'"},{'<',","},{'>',"."},{'?',"/"},{'~',"`"},{'|',"\\"},
    };
    for (const Shifted& s : kShifted) {
        if (c == s.ch) { keyName = s.key; needShift = true; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// RecordOutput
// ---------------------------------------------------------------------------
bool RecordOutput::keyDown(const std::string& key) {
    if (!FindKey(key)) return false;
    held_[key]++;
    ops.push_back("down:" + key);
    return true;
}
bool RecordOutput::keyUp(const std::string& key) {
    auto it = held_.find(key);
    if (it == held_.end()) return false;
    if (--it->second <= 0) held_.erase(it);
    ops.push_back("up:" + key);
    return true;
}
bool RecordOutput::typeText(const std::string& text) {
    ops.push_back("text:" + text);
    return true;
}
bool RecordOutput::releaseAll() {
    for (const auto& kv : held_) ops.push_back("up:" + kv.first);
    held_.clear();
    return true;
}
bool RecordOutput::isHeld(const std::string& key) const {
    return held_.find(key) != held_.end();
}
std::vector<std::string> RecordOutput::heldKeys() const {
    std::vector<std::string> v;
    for (const auto& kv : held_) v.push_back(kv.first);
    std::sort(v.begin(), v.end());
    return v;
}

// ---------------------------------------------------------------------------
// KeyOwnership
// ---------------------------------------------------------------------------
bool KeyOwnership::press(const std::string& key, const std::string& holder, OutputBackend& out) {
    int& holderCount = perHolder_[{key, holder}];
    if (holderCount > 0) {          // already held by this holder: no-op
        return false;
    }
    holderCount = 1;
    const int total = ++keyRefs_[key];
    if (total == 1) {               // physical up->down
        if (!out.keyDown(key)) {    // backend refused: roll back, report failure
            if (--keyRefs_[key] <= 0) keyRefs_.erase(key);
            holderCount = 0;
            ++outputFailures_;
            return false;
        }
        return true;
    }
    return false;
}
bool KeyOwnership::release(const std::string& key, const std::string& holder, OutputBackend& out) {
    auto hIt = perHolder_.find({key, holder});
    if (hIt == perHolder_.end() || hIt->second == 0) return false;
    hIt->second = 0;
    auto keyIt = keyRefs_.find(key);
    if (keyIt != keyRefs_.end() && --keyIt->second <= 0) {
        keyRefs_.erase(keyIt);
        if (!out.keyUp(key)) { ++outputFailures_; return false; } // physical down->up failed
        return true;
    }
    return false;
}
void KeyOwnership::releaseAllByHolder(const std::string& holder, OutputBackend& out) {
    for (auto it = perHolder_.begin(); it != perHolder_.end();) {
        if (it->first.second == holder && it->second > 0) {
            it->second = 0;
            auto keyIt = keyRefs_.find(it->first.first);
            if (keyIt != keyRefs_.end() && --keyIt->second <= 0) {
                keyRefs_.erase(keyIt);
                if (!out.keyUp(it->first.first)) ++outputFailures_;
            }
            ++it;
        } else {
            ++it;
        }
    }
}
void KeyOwnership::releaseAll(OutputBackend& out) {
    for (auto& kv : keyRefs_) out.keyUp(kv.first);
    keyRefs_.clear();
    perHolder_.clear();
    out.releaseAll();
}
bool KeyOwnership::isHeld(const std::string& key) const {
    return keyRefs_.find(key) != keyRefs_.end();
}
std::vector<std::string> KeyOwnership::heldKeys() const {
    std::vector<std::string> v;
    for (const auto& kv : keyRefs_) v.push_back(kv.first);
    std::sort(v.begin(), v.end());
    return v;
}

// ---------------------------------------------------------------------------
// Device identity & feedback-loop prevention (platform-neutral).
// ---------------------------------------------------------------------------
bool ParseVidPid(const std::string& devicePath, unsigned& vid, unsigned& pid) {
    std::string s;
    s.reserve(devicePath.size());
    for (char c : devicePath) s += (char)tolower((unsigned char)c);
    auto vpos = s.find("vid_");
    if (vpos == std::string::npos || vpos + 8 > s.size()) return false;
    vid = (unsigned)strtoul(s.substr(vpos + 4, 4).c_str(), nullptr, 16);
    auto ppos = s.find("pid_");
    if (ppos == std::string::npos || ppos + 8 > s.size()) return false;
    pid = (unsigned)strtoul(s.substr(ppos + 4, 4).c_str(), nullptr, 16);
    return true;
}

bool IsOwnOutputDevice(const std::string& devicePath) {
    unsigned vid = 0, pid = 0;
    return ParseVidPid(devicePath, vid, pid) && vid == 0x1209u && pid == 0x0001u;
}

// ===========================================================================
// Windows backends (compiled only on Windows).
// ===========================================================================
#ifdef _WIN32
#include <windows.h>
#include "textdeck_protocol.h"
#pragma comment(lib, "user32.lib")

namespace {

// Translate an opaque key name to a VK for SendInput. Handles "up/down/left/right"
// (which are not single characters) and letters/digits/punct via FindKey.
WORD KeyNameToVk(const std::string& key) {
    const KeyInfo* ki = FindKey(key);
    if (ki) return ki->vk;
    // fallback: single ASCII char
    if (key.size() == 1) {
        char c = key[0];
        if (c >= 'a' && c <= 'z') return (WORD)(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') return (WORD)c;
        if (c >= '0' && c <= '9') return (WORD)c;
    }
    return 0;
}

} // namespace

// --- SendInput backend (software mode; no driver required) ------------------
class SendInputBackend final : public OutputBackend {
public:
    bool keyDown(const std::string& key) override { return send(key, false); }
    bool keyUp(const std::string& key) override { return send(key, true); }
    bool typeText(const std::string& text) override {
        bool ok = true;
        for (char c : text) {
            INPUT in[2] = {};
            in[0].type = INPUT_KEYBOARD; in[0].ki.wScan = (WORD)(unsigned char)c;
            in[0].ki.dwFlags = KEYEVENTF_UNICODE;
            in[1].type = INPUT_KEYBOARD; in[1].ki.wScan = (WORD)(unsigned char)c;
            in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            if (SendInput(2, in, sizeof(INPUT)) != 2) ok = false;
        }
        return ok;
    }
    bool releaseAll() override {
        // Release every non-modifier key we can enumerate; the caller (key
        // ownership) already released tracked keys. This is a best-effort sweep.
        return true;
    }
    const char* name() const override { return "sendinput"; }
private:
    bool send(const std::string& key, bool up) {
        WORD vk = KeyNameToVk(key);
        if (!vk) return false;
        INPUT in = {};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        if (up) in.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(INPUT));
        return true;
    }
};

// --- TextDeck virtual-HID driver backend ------------------------------------
// Opens \\.\TextDeckHidDevice and drives TEXTDECK_KEY_EVENT through
// IOCTL_TEXTDECK_SUBMIT_KEY. Absent/insufficient-privilege device => the calls
// fail cleanly (false), never throw; the caller decides the fallback.
class HidDriverBackend final : public OutputBackend {
public:
    HidDriverBackend() { open(); }
    ~HidDriverBackend() override { close(); }
    bool keyDown(const std::string& key) override { return send(key, true); }
    bool keyUp(const std::string& key) override { return send(key, false); }
    bool typeText(const std::string& text) override {
        // Type ASCII via per-character strokes: hold Shift when the character
        // needs it (Modifiers is the COMPLETE held set), press/release the
        // usage, then clear modifiers. Unresolvable characters fail loudly.
        if (h_ == INVALID_HANDLE_VALUE) return false;
        for (char c : text) {
            std::string keyName; bool shift = false;
            if (!CharToStroke(c, keyName, shift)) return false;
            const KeyInfo* ki = FindKey(keyName);
            if (!ki || ki->isModifier) return false;
            const UCHAR mods = shift ? (UCHAR)0x02 : (UCHAR)0; // TEXTDECK_MOD_LEFT_SHIFT
            if (shift && !submit(0, 0, TextDeckKeyActionModifiers, mods)) return false;
            if (!submit(ki->hidUsage, 0, TextDeckKeyActionDown, mods)) return false;
            if (!submit(ki->hidUsage, 0, TextDeckKeyActionUp, (UCHAR)0)) return false;
            // the Up above already produced the all-zero report when mods==0;
            // when shift was held, one more Modifiers(0) is sent by Up's 0 mods.
        }
        return true;
    }
    bool releaseAll() override {
        if (h_ == INVALID_HANDLE_VALUE) return false;
        DWORD n = 0;
        return DeviceIoControl(h_, IOCTL_TEXTDECK_RELEASE_ALL, nullptr, 0, nullptr, 0, &n, nullptr) != 0;
    }
    const char* name() const override { return "textdeck-hid"; }
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    void open() {
        h_ = CreateFileW(L"\\\\.\\TextDeckHidDevice", GENERIC_WRITE | GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    void close() { if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; } }
    bool send(const std::string& name, bool down) {
        if (h_ == INVALID_HANDLE_VALUE) return false;
        const KeyInfo* ki = FindKey(name);
        if (!ki) return false;
        // Modifier keys are held via Action=Modifiers; ordinary keys via Down/Up.
        if (ki->isModifier) {
            if (down) return submit(0, 0, TextDeckKeyActionModifiers, ki->hidMod);
            return submit(0, 0, TextDeckKeyActionModifiers, 0); // release: clear the set
        }
        return submit(ki->hidUsage, 0, down ? TextDeckKeyActionDown : TextDeckKeyActionUp,
                      ki->hidMod);
    }
    bool submit(UCHAR usage, UCHAR reserved, UCHAR action, UCHAR mods) {
        if (h_ == INVALID_HANDLE_VALUE) return false;
        TEXTDECK_KEY_EVENT ev = {};
        ev.Modifiers = mods;
        ev.Usage     = usage;
        ev.Action    = action;
        ev.Reserved  = reserved;
        DWORD n = 0;
        return DeviceIoControl(h_, IOCTL_TEXTDECK_SUBMIT_KEY, &ev, sizeof(ev), nullptr, 0, &n, nullptr) != 0;
    }
};

// --- backend factory --------------------------------------------------------
std::unique_ptr<OutputBackend> MakeBackend(const std::string& kind) {
    if (kind == "record")    return std::make_unique<RecordOutput>();
    if (kind == "sendinput") return std::make_unique<SendInputBackend>();
    if (kind == "hid")       return std::make_unique<HidDriverBackend>();
    return std::make_unique<RecordOutput>();
}

#else // _WIN32

std::unique_ptr<OutputBackend> MakeBackend(const std::string&) {
    return std::make_unique<RecordOutput>();
}

#endif // _WIN32

} // namespace map
