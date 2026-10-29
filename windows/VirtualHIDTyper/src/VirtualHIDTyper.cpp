// ---------------------------------------------------------------------------
// VirtualHIDTyper.cpp
//
// Types text into the currently focused window using one of THREE explicit,
// user-selected modes (never silently switched):
//
//   * Virtual HID mode -- key events go to the TextDeck virtual HID driver
//     (\\.\TextDeckHidDevice) through the private IOCTLs declared in
//     windows/shared/textdeck_protocol.h. US English QWERTY only; requires the
//     (signed) driver to be installed.
//
//   * ESP32 USB HID mode -- the app writes the text over the ESP32-S3's
//     native USB CDC control port; the ESP32 itself is a real USB HID
//     keyboard and types the characters. No kernel driver and no admin
//     rights. ASCII-only (same US-layout character set as HID mode).
//
//   * Software input mode -- SendInput with KEYEVENTF_UNICODE; works with NO
//     driver installed, any Unicode text (CJK, emoji, ...), newlines as
//     VK_RETURN (optionally Shift+Enter), Tab as VK_TAB. This is a regular
//     user-level input path, NOT a virtual/physical keyboard.
//
// Body text is never injected with clipboard paste, SendKeys or hidden
// paste mechanisms. RegisterHotKey is used only for the start/stop control
// hotkeys (Ctrl+Alt+T / Ctrl+Alt+X). Stop aborts immediately.
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <winioctl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <mutex>

#include "hid_mapping.h"
#include "sw_input.h"
#include "esp32_link.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")

// ----- protocol shared with the kernel driver -------------------------------
// The ABI is NOT declared here. It comes from windows/shared/textdeck_protocol.h,
// which the driver includes too, so the two sides cannot drift apart. Keeping a
// private copy of the struct in this file is exactly how a client and a driver
// end up disagreeing about a buffer layout.
#include "textdeck_protocol.h"

// Win32 open path for the driver's symbolic link, taken from the shared header
// so the client and the INF cannot disagree about the device name.
static const wchar_t* kDevicePath = TEXTDECK_DEVICE_WIN32_PATH;

// ----- UI ids ---------------------------------------------------------------
enum : int {
    IDC_EDIT_TEXT     = 1001,
    IDC_EDIT_INTERVAL = 1002,
    IDC_CHK_SHIFT_NL  = 1003,
    IDC_BTN_START     = 1004,
    IDC_BTN_STOP      = 1005,
    IDC_STATIC_PROG   = 1006,
    IDC_STATIC_STATUS = 1007,
    IDC_RADIO_HID     = 1008,
    IDC_RADIO_SW      = 1009,
    IDC_BTN_SAVE      = 1010,
    IDC_BTN_HOTKEYS   = 1011,
    IDC_STATIC_HK     = 1012,
    IDC_STATIC_INTERVAL = 1013,
    IDC_RADIO_ESP32   = 1014,
    IDC_STATIC_PORT   = 1015,
    IDC_COMBO_PORT    = 1016,
    IDC_BTN_REFRESH   = 1017,
    IDC_BTN_TEST      = 1018,
};
enum : UINT_PTR { TIMER_COUNTDOWN = 1, TIMER_SMOKE = 2 };
enum : int { ID_HOTKEY_START = 1, ID_HOTKEY_STOP = 2,
             ID_HOTKEY_TMP_START = 3, ID_HOTKEY_TMP_STOP = 4 };

// tray icon + its context menu
#define TRAY_UID        1
enum : int {
    IDM_TRAY_OPEN    = 2001,
    IDM_TRAY_START   = 2002,
    IDM_TRAY_STOP    = 2003,
    IDM_TRAY_HOTKEYS = 2004,
    IDM_TRAY_EXIT    = 2005,
};

// hotkey settings dialog controls
enum : int {
    IDC_HK_INFO   = 3001,
    IDC_HK_START  = 3002,
    IDC_HK_STOP   = 3003,
    IDC_HK_OK     = 3004,
    IDC_HK_CANCEL = 3005,
    IDC_HK_DEFAULT = 3006,
};

#define WM_APP_STATUS   (WM_APP + 1) // lParam = LPWSTR (malloc'd, freed by receiver)
#define WM_APP_PROGRESS (WM_APP + 2) // wParam = sent count, lParam = total count
#define WM_APP_DONE     (WM_APP + 3) // wParam = DoneReason, lParam = win32 error
#define WM_APP_TRAY     (WM_APP + 4) // tray callback (Shell_NotifyIcon dwCallbackMessage)
#define WM_APP_SHOWME   (WM_APP + 5) // a second instance asked us to show the window

enum DoneReason { DONE_COMPLETED = 0, DONE_STOPPED = 1, DONE_FOCUS = 2, DONE_ERROR = 3 };

// Explicit task lifecycle. Only IDLE / COMPLETED / FAILED accept a new start
// request, so a hotkey that repeats quickly (or a busy message pump) can never
// spawn a second typing thread or restart the text from the beginning.
enum TaskState {
    TS_IDLE = 0, TS_STARTING, TS_TYPING, TS_STOPPING, TS_COMPLETED, TS_FAILED
};

// A global hotkey binding: modifier mask (MOD_*) + virtual-key code.
struct HotkeySpec {
    UINT mods;
    UINT vk;
};

// ----- global state ---------------------------------------------------------
enum InputMode { MODE_HID = 0, MODE_SOFTWARE = 1, MODE_ESP32 = 2 };

static HWND g_hwndMain = nullptr;
static HWND g_editText = nullptr;
static HWND g_editInterval = nullptr;
static HWND g_chkShiftNl = nullptr;
static HWND g_radioHid = nullptr;
static HWND g_radioSw = nullptr;
static HWND g_radioEsp32 = nullptr;
static HWND g_btnStart = nullptr;
static HWND g_btnStop = nullptr;
static HWND g_staticProg = nullptr;
static HWND g_staticStatus = nullptr;
static HWND g_btnSave = nullptr;
static HWND g_btnHotkeys = nullptr;
static HWND g_staticHotkeys = nullptr;
static HWND g_staticPort = nullptr;
static HWND g_comboPort = nullptr;
static HWND g_btnRefreshPort = nullptr;
static HWND g_btnTestPort = nullptr;

static InputMode         g_mode = MODE_ESP32;    // selected via radio / --mode
static InputMode         g_runMode = MODE_ESP32; // mode of the CURRENT run
static bool              g_modeOverride = false;
static InputMode         g_modeOverrideValue = MODE_ESP32;

// ESP32 control channel. One instance owns the COM handle; it stays open
// across start/stop and between runs so line-state never resets the device.
static esp32::Link      g_esp32;
static std::wstring     g_esp32Port;
static std::vector<esp32::PortInfo> g_portList;
// Button-countdown stash for ESP32 mode: the 3 s timer needs the validated
// payload after the user has clicked into the target.
static std::string      g_esp32PendingText;
static std::wstring     g_esp32PendingPort;
static HWND             g_esp32PendingTarget = nullptr;
static bool             g_esp32CountdownArmed = false;
static std::mutex       g_lastErrorLock;
static std::wstring     g_lastErrorDetail;

// Isolated data directory (settings.ini + text.txt). Tests pass
// --data-dir=<temp> or set VHIDT_DATA_DIR; the default is the user's
// %APPDATA%\VirtualHIDTyper. A custom directory also gets its own single
// instance mutex and window class, so it can run beside the real tray app
// without touching its settings or hotkeys.
static std::wstring     g_dataDir;
static bool             g_customDataDir = false;
static std::wstring     g_windowClass = L"VirtualHIDTyperWnd";
static std::wstring     g_mutexName = L"VirtualHIDTyper.SingleInstance.v1";

static std::atomic<bool>  g_running{false};
static std::atomic<bool>  g_inCountdown{false};
static int                g_countdownLeft = 0;
static HANDLE             g_stopEvent = nullptr;
static std::thread        g_worker;
static std::atomic<long>  g_sent{0};
static std::atomic<long>  g_skipped{0};
static bool               g_smokeMode = false;
static bool               g_elevatedArg = false;
static std::wstring       g_smokeLogPath;
static wchar_t            g_exePath[MAX_PATH] = L"";

// ----- tray / global hotkeys / task state -----------------------------------
static NOTIFYICONDATAW    g_nid = {};
static bool               g_trayAdded = false;
static HICON              g_iconApp   = nullptr;   // normal app icon
static HICON              g_iconIdle  = nullptr;   // tray: idle
static HICON              g_iconBusy  = nullptr;   // tray: typing
static HICON              g_iconErr   = nullptr;   // tray: error

static std::atomic<int>   g_task{TS_IDLE};
// Mutex protecting "may I start?" so two near-simultaneous hotkey events cannot
// both pass the state check and spawn parallel workers.
static std::mutex         g_taskLock;

// Defaults; overridable in the hotkey dialog and persisted in settings.ini.
// Ctrl+Alt+Shift+F9/F10 are deliberately obscure: plain Ctrl+Alt+F9 and
// Ctrl+Alt+X collide with common applications, and adding Shift drops the
// overlap dramatically. Neither combination is reserved by Windows.
static const HotkeySpec   kDefaultHkStart = { MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F9 };
static const HotkeySpec   kDefaultHkStop  = { MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F10 };
static HotkeySpec         g_hkStart = kDefaultHkStart;   // Ctrl+Alt+Shift+F9
static HotkeySpec         g_hkStop  = kDefaultHkStop;    // Ctrl+Alt+Shift+F10
static bool               g_hkStartOk = false;   // registered successfully?
static bool               g_hkStopOk  = false;
static DWORD              g_hkStartErr = 0;      // last RegisterHotKey error
static DWORD              g_hkStopErr  = 0;
// Id pair currently holding the ACTIVE bindings (0 = none registered yet).
// ApplyHotkeys alternates between {ID_HOTKEY_START,ID_HOTKEY_STOP} and
// {ID_HOTKEY_TMP_START,ID_HOTKEY_TMP_STOP} so a conflicting change can never
// release the working pair before the replacement is provably registrable.
static int                g_hkIdStart = 0;
static int                g_hkIdStop  = 0;
static std::wstring       g_trayTip;             // current tray tooltip text
static bool               g_hotkeyDlgOpen = false;

static int GetWindowTextInt(HWND h, int fallback)
{
    wchar_t buf[32] = L"";
    GetWindowTextW(h, buf, 32);
    wchar_t* end = nullptr;
    long v = wcstol(buf, &end, 10);
    if (end == buf) return fallback;
    return (int)v;
}

static void AppendSmokeLog(const std::wstring& line)
{
    if (!g_smokeMode || g_smokeLogPath.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_smokeLogPath.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(f, L"[%02u:%02u:%02u.%03u] %s\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line.c_str());
        fclose(f);
    }
}

static bool EnsureDirectory(const std::wstring& dir)
{
    if (dir.empty()) return false;
    int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS) return true;
    DWORD attr = GetFileAttributesW(dir.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring DefaultDataDir()
{
    wchar_t appdata[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(nullptr, 0x001a /*CSIDL_APPDATA*/, nullptr,
                               0, appdata)) || !appdata[0]) {
        DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return L"";
    }
    std::wstring dir = appdata;
    dir += L"\\VirtualHIDTyper";
    return dir;
}

static std::wstring SettingsDir()
{
    if (g_dataDir.empty()) {
        g_dataDir = DefaultDataDir();
        if (!g_dataDir.empty()) EnsureDirectory(g_dataDir);
    }
    return g_dataDir;
}

static unsigned Fnv1a(const std::wstring& s)
{
    unsigned h = 2166136261u;
    for (wchar_t c : s) {
        h ^= (unsigned)c;
        h *= 16777619u;
    }
    return h;
}

// Parse the command line ONCE, before the single-instance mutex or any file
// access. Supports:
//   --data-dir=<path> / --data-dir <path>   (also VHIDT_DATA_DIR)
//   --mode=esp32|hid|software
//   --smoke [logpath]
//   --elevated
static bool ParseCommandLine()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return true;

    std::wstring dataDirArg;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (_wcsnicmp(a.c_str(), L"--data-dir=", 11) == 0) {
            dataDirArg = a.substr(11);
        } else if (a == L"--data-dir" && i + 1 < argc) {
            dataDirArg = argv[++i];
        } else if (a == L"--smoke") {
            g_smokeMode = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                g_smokeLogPath = argv[++i];
            } else {
                g_smokeLogPath = L"smoke.log";
            }
        } else if (a == L"--elevated") {
            g_elevatedArg = true;
        } else if (_wcsnicmp(a.c_str(), L"--mode=", 7) == 0) {
            std::wstring m = a.substr(7);
            if (_wcsicmp(m.c_str(), L"software") == 0) {
                g_modeOverride = true;
                g_modeOverrideValue = MODE_SOFTWARE;
            } else if (_wcsicmp(m.c_str(), L"hid") == 0) {
                g_modeOverride = true;
                g_modeOverrideValue = MODE_HID;
            } else if (_wcsicmp(m.c_str(), L"esp32") == 0) {
                g_modeOverride = true;
                g_modeOverrideValue = MODE_ESP32;
            }
        }
    }
    LocalFree(argv);

    if (dataDirArg.empty()) {
        wchar_t env[32768] = L"";
        DWORD n = GetEnvironmentVariableW(L"VHIDT_DATA_DIR", env, 32768);
        if (n > 0 && n < 32768) dataDirArg = env;
    }

    if (!dataDirArg.empty()) {
        wchar_t full[32768] = L"";
        DWORD n = GetFullPathNameW(dataDirArg.c_str(), 32768, full, nullptr);
        if (n == 0 || n >= 32768) {
            MessageBoxW(nullptr, L"--data-dir 路径无效。", L"VirtualHIDTyper",
                        MB_OK | MB_ICONERROR);
            return false;
        }
        g_dataDir = full;
        g_customDataDir = true;
        if (!EnsureDirectory(g_dataDir)) {
            std::wstring msg = L"无法创建数据目录：\n" + g_dataDir;
            MessageBoxW(nullptr, msg.c_str(), L"VirtualHIDTyper",
                        MB_OK | MB_ICONERROR);
            return false;
        }
        std::wstring tag = g_dataDir;
        if (!tag.empty()) CharLowerBuffW(&tag[0], (DWORD)tag.size());
        wchar_t hex[16] = L"";
        swprintf_s(hex, L"%08X", Fnv1a(tag));
        g_mutexName = std::wstring(L"VirtualHIDTyper.SingleInstance.v1_") + hex;
        g_windowClass = std::wstring(L"VirtualHIDTyperWnd_") + hex;
    } else {
        g_dataDir = DefaultDataDir();
        if (g_dataDir.empty() || !EnsureDirectory(g_dataDir)) {
            MessageBoxW(nullptr, L"无法创建 %APPDATA%\\VirtualHIDTyper 数据目录。",
                        L"VirtualHIDTyper", MB_OK | MB_ICONERROR);
            return false;
        }
    }
    return true;
}

// Forward declarations: LoadSettings (below) validates the persisted hotkey
// pair, and the tray/menu handlers reference these before their definitions.
static bool SameHotkey(const HotkeySpec& a, const HotkeySpec& b);
static std::wstring HotkeyText(const HotkeySpec& hk);
static bool SaveHotkeySettings();
static void RefreshActionLabels();
static void SaveWindowState(HWND hwnd);
static void SetTrayDetail(const std::wstring& detail);
static void UpdateTrayIcon();

// ----- profile (INI) normalisation ------------------------------------------
// The Win32 profile API has quirks that were MEASURED on this platform, not
// assumed, because they decide which value the app actually reads:
//
//   * a UTF-8 byte-order mark is NOT stripped by the parser. The three BOM
//     bytes end up glued to the first section name, so the very first header
//     is not recognised as a section at all. A file that starts with a BOM and
//     holds exactly one [Main] therefore yields NO values: every lookup misses.
//   * a file that starts with a BOM and holds TWO [Main] sections exposes only
//     the second one, because the first header was swallowed by the BOM.
//   * a file with no BOM resolves to the first matching section.
//
// So a settings.ini carrying a BOM is not a cosmetic problem: the section the
// app reads is not the section it writes. Writes go to the first [Main], reads
// come from the last recognised one, and every save appears to be ignored.
//
// NormalizeIniSections() repairs the file once and for all: it collapses
// duplicate sections, keeps the value the platform would have returned for the
// file as it stands (so normalising never changes a setting the user could
// observe), and rewrites the file WITHOUT a BOM so the surviving [Main] is
// parsed correctly from then on.
namespace {

bool IniIsSectionHeader(const std::wstring& line, std::wstring* name)
{
    size_t a = line.find_first_not_of(L" \t");
    if (a == std::wstring::npos || line[a] != L'[') return false;
    size_t b = line.find_last_not_of(L" \t\r");
    if (b == std::wstring::npos || b <= a || line[b] != L']') return false;
    if (name) *name = line.substr(a + 1, b - a - 1);
    return true;
}

bool IniIsKeyLine(const std::wstring& line, std::wstring* key)
{
    size_t a = line.find_first_not_of(L" \t");
    if (a == std::wstring::npos) return false;
    if (line[a] == L';' || line[a] == L'[') return false;
    size_t eq = line.find(L'=', a);
    if (eq == std::wstring::npos) return false;
    std::wstring k = line.substr(a, eq - a);
    size_t e = k.find_last_not_of(L" \t");
    k = (e == std::wstring::npos) ? std::wstring() : k.substr(0, e + 1);
    if (k.empty()) return false;
    if (key) *key = k;
    return true;
}

bool IniSameName(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

} // namespace

static void NormalizeIniSections(const std::wstring& iniPath)
{
    HANDLE h = CreateFileW(iniPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string raw;
    char chunk[4096];
    DWORD got = 0;
    while (ReadFile(h, chunk, sizeof(chunk), &got, nullptr) && got > 0)
        raw.append(chunk, got);
    CloseHandle(h);
    if (raw.empty()) return;

    // A UTF-16 file is left untouched: rewriting it is not needed for the bug
    // above and the profile API's UTF-16 handling is not worth guessing at.
    if (raw.size() >= 2 &&
        (((unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) ||
         ((unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF))) {
        return;
    }

    const bool hadBom = raw.size() >= 3 &&
                        (unsigned char)raw[0] == 0xEF &&
                        (unsigned char)raw[1] == 0xBB &&
                        (unsigned char)raw[2] == 0xBF;
    const char* body = raw.data() + (hadBom ? 3 : 0);
    const int bodyLen = (int)(raw.size() - (hadBom ? 3 : 0));

    std::wstring text;
    int need = MultiByteToWideChar(CP_UTF8, 0, body, bodyLen, nullptr, 0);
    if (need <= 0) return;
    text.resize((size_t)need);
    MultiByteToWideChar(CP_UTF8, 0, body, bodyLen, &text[0], need);

    // --- split into a preamble plus an ordered list of sections --------------
    std::vector<std::wstring> lines;
    {
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find(L'\n', pos);
            std::wstring line = (nl == std::wstring::npos)
                                    ? text.substr(pos)
                                    : text.substr(pos, nl - pos);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            lines.push_back(line);
            if (nl == std::wstring::npos) break;
            pos = nl + 1;
        }
    }

    struct IniSection { std::wstring name; std::vector<std::wstring> body; };
    std::vector<std::wstring> preamble;
    std::vector<IniSection> sections;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::wstring name;
        if (IniIsSectionHeader(lines[i], &name)) {
            IniSection s;
            s.name = name;
            sections.push_back(s);
        } else if (sections.empty()) {
            preamble.push_back(lines[i]);
        } else {
            sections.back().body.push_back(lines[i]);
        }
    }
    if (sections.empty()) return;

    // Which occurrence wins for a duplicated key: the one the platform returns
    // for the file AS IT IS NOW. With a BOM the first header is invisible, so
    // the last recognised section wins; without one the first section wins.
    const bool lastWins = hadBom;

    // --- collapse duplicates -------------------------------------------------
    bool changed = hadBom;
    std::vector<IniSection> merged;
    std::vector<std::wstring> emitted;
    for (size_t i = 0; i < sections.size(); ++i) {
        bool dup = false;
        for (size_t q = 0; q < emitted.size(); ++q)
            if (IniSameName(emitted[q], sections[i].name)) { dup = true; break; }
        if (dup) { changed = true; continue; }
        emitted.push_back(sections[i].name);

        std::vector<size_t> members;
        for (size_t j = i; j < sections.size(); ++j)
            if (IniSameName(sections[j].name, sections[i].name)) members.push_back(j);

        std::vector<std::wstring> keys;                 // order of first appearance
        std::vector<std::pair<size_t, size_t> > winner; // (member index, line index)
        for (size_t m = 0; m < members.size(); ++m) {
            const std::vector<std::wstring>& b = sections[members[m]].body;
            for (size_t li = 0; li < b.size(); ++li) {
                std::wstring k;
                if (!IniIsKeyLine(b[li], &k)) continue;
                size_t idx = keys.size();
                for (size_t q = 0; q < keys.size(); ++q)
                    if (IniSameName(keys[q], k)) { idx = q; break; }
                if (idx == keys.size()) {
                    keys.push_back(k);
                    winner.push_back(std::make_pair(m, li));
                } else if (lastWins) {
                    winner[idx] = std::make_pair(m, li);
                }
            }
        }

        IniSection out;
        out.name = sections[i].name;
        for (size_t m = 0; m < members.size(); ++m) {
            const std::vector<std::wstring>& b = sections[members[m]].body;
            for (size_t li = 0; li < b.size(); ++li) {
                std::wstring k;
                if (!IniIsKeyLine(b[li], &k)) { out.body.push_back(b[li]); continue; }
                for (size_t q = 0; q < keys.size(); ++q) {
                    if (!IniSameName(keys[q], k)) continue;
                    if (winner[q].first == m && winner[q].second == li)
                        out.body.push_back(b[li]);
                    break;
                }
            }
        }
        while (!out.body.empty() && out.body.back().find_first_not_of(L" \t\r") == std::wstring::npos)
            out.body.pop_back();
        merged.push_back(out);
    }
    if (!changed) return;

    // --- rebuild and write back WITHOUT a BOM --------------------------------
    std::wstring rebuilt;
    for (size_t i = 0; i < preamble.size(); ++i) {
        if (preamble[i].find_first_not_of(L" \t\r") == std::wstring::npos) continue;
        rebuilt += preamble[i];
        rebuilt += L"\r\n";
    }
    for (size_t s = 0; s < merged.size(); ++s) {
        rebuilt += L"[" + merged[s].name + L"]\r\n";
        for (size_t li = 0; li < merged[s].body.size(); ++li) {
            rebuilt += merged[s].body[li];
            rebuilt += L"\r\n";
        }
    }

    int bytesNeeded = WideCharToMultiByte(CP_UTF8, 0, rebuilt.data(), (int)rebuilt.size(),
                                          nullptr, 0, nullptr, nullptr);
    if (bytesNeeded <= 0) return;
    std::string bytes((size_t)bytesNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, rebuilt.data(), (int)rebuilt.size(),
                        &bytes[0], bytesNeeded, nullptr, nullptr);

    // NEVER truncate the live settings file. CREATE_ALWAYS on the real path
    // destroys the user's only profile before the replacement is known to be
    // complete, so a crash, a full disk or a killed process would leave them
    // with nothing. Write a sibling temp file, prove every byte landed and
    // reached the disk, then replace the original in one atomic step.
    std::wstring tempPath = iniPath + L".tmp";
    HANDLE w = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) return;   // original untouched

    DWORD written = 0;
    BOOL ok = WriteFile(w, bytes.data(), (DWORD)bytes.size(), &written, nullptr);
    if (!ok || written != (DWORD)bytes.size()) {
        CloseHandle(w);
        DeleteFileW(tempPath.c_str());
        return;                              // original untouched
    }
    // Flush to disk: a replacement that only lives in the cache is not a
    // replacement after a power loss.
    if (!FlushFileBuffers(w)) {
        CloseHandle(w);
        DeleteFileW(tempPath.c_str());
        return;
    }
    CloseHandle(w);

    // MoveFileEx with REPLACE_EXISTING + WRITE_THROUGH swaps the file in one
    // step on the same volume; if the move fails the original survives.
    if (!MoveFileExW(tempPath.c_str(), iniPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath.c_str());
        return;
    }
}

static void SaveSettings()
{
    std::wstring dir = SettingsDir();
    std::wstring ini = dir + L"\\settings.ini";
    NormalizeIniSections(ini);   // see the note above: keep [Main] unique
    wchar_t buf[32];
    int interval = GetWindowTextInt(g_editInterval, 30);
    swprintf_s(buf, L"%d", interval);
    WritePrivateProfileStringW(L"Main", L"IntervalMs", buf, ini.c_str());
    WritePrivateProfileStringW(L"Main", L"ShiftEnter",
                               Button_GetCheck(g_chkShiftNl) ? L"1" : L"0", ini.c_str());
    const wchar_t* modeValue = g_mode == MODE_SOFTWARE ? L"software"
                             : g_mode == MODE_ESP32    ? L"esp32"
                                                       : L"hid";
    WritePrivateProfileStringW(L"Main", L"InputMode", modeValue, ini.c_str());
    WritePrivateProfileStringW(L"Main", L"Esp32Port", g_esp32Port.c_str(), ini.c_str());
    // hotkey bindings: stored as "<mods>,<vk>" so they survive restarts
    wchar_t hk[64];
    swprintf_s(hk, L"%u,%u", g_hkStart.mods, g_hkStart.vk);
    WritePrivateProfileStringW(L"Main", L"HotkeyStart", hk, ini.c_str());
    swprintf_s(hk, L"%u,%u", g_hkStop.mods, g_hkStop.vk);
    WritePrivateProfileStringW(L"Main", L"HotkeyStop", hk, ini.c_str());

    int len = GetWindowTextLengthW(g_editText);
    std::wstring text((size_t)len + 1, L'\0');
    GetWindowTextW(g_editText, &text[0], len + 1);
    text.resize(wcslen(text.c_str()));
    std::wstring path = dir + L"\\text.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, L"\xFEFF", 2, &written, nullptr);
        WriteFile(h, text.data(), (DWORD)(text.size() * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(h);
    }
}

static const wchar_t* ModeShortName(InputMode m)
{
    switch (m) {
    case MODE_SOFTWARE: return L"软件输入模式 SendInput";
    case MODE_ESP32:    return L"ESP32 USB HID 模式";
    default:            return L"TextDeck HID 模式";
    }
}

static const wchar_t* ModeStatusText()
{
    // Focus-race advisory: the foreground check before each
    // send is not atomic with the send, so focus can still change in that
    // window. We cannot promise zero stray/lost chars on switch; the
    // advisory tells the user how to stay safe instead.
    static wchar_t buf[768];
    if (g_mode == MODE_SOFTWARE) {
        swprintf_s(buf,
            L"[软件输入模式] 就绪。SendInput Unicode，无需驱动；支持任意 Unicode 文字"
            L"（含中文/emoji）、换行、Tab。全局快捷键: %s 开始, %s 停止。"
            L"关闭主窗口会隐藏到系统托盘继续运行（双击托盘图标可重新打开）。",
            HotkeyText(g_hkStart).c_str(), HotkeyText(g_hkStop).c_str());
    } else if (g_mode == MODE_ESP32) {
        const wchar_t* port = g_esp32Port.empty() ? L"未选择串口" : g_esp32Port.c_str();
        swprintf_s(buf,
            L"[ESP32 USB HID 模式] 就绪。原生 USB 单线（CDC 控制 + HID 键盘），"
            L"无需内核驱动/管理员；US 英文布局 ASCII，换行固定为普通 Enter。"
            L"串口: %s。全局快捷键: %s 开始, %s 停止。"
            L"关闭主窗口会隐藏到系统托盘继续运行。",
            port, HotkeyText(g_hkStart).c_str(), HotkeyText(g_hkStop).c_str());
    } else {
        swprintf_s(buf,
            L"[TextDeck HID 模式] 就绪。需已安装虚拟 HID 驱动（管理员权限）；"
            L"US 英文布局字母/数字/常见标点。全局快捷键: %s 开始, %s 停止。"
            L"关闭主窗口会隐藏到系统托盘继续运行（双击托盘图标可重新打开）。",
            HotkeyText(g_hkStart).c_str(), HotkeyText(g_hkStop).c_str());
    }
    return buf;
}

static void SetMode(InputMode m, bool updateStatus)
{
    g_mode = m;
    if (g_radioHid && g_radioSw && g_radioEsp32) {
        Button_SetCheck(g_radioHid,   m == MODE_HID ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(g_radioSw,    m == MODE_SOFTWARE ? BST_CHECKED : BST_UNCHECKED);
        Button_SetCheck(g_radioEsp32, m == MODE_ESP32 ? BST_CHECKED : BST_UNCHECKED);
    }

    const bool idle = !g_running && !g_inCountdown;
    const bool esp = (m == MODE_ESP32);
    if (g_comboPort) EnableWindow(g_comboPort, esp && idle);
    if (g_btnRefreshPort) EnableWindow(g_btnRefreshPort, esp && idle);
    if (g_btnTestPort) EnableWindow(g_btnTestPort, esp && idle);
    if (g_editInterval) EnableWindow(g_editInterval, !esp && idle);
    if (g_chkShiftNl) EnableWindow(g_chkShiftNl, !esp && idle);
    if (g_staticPort) SetWindowTextW(g_staticPort, L"ESP32 串口:");

    if (g_hwndMain) {
        // The title shows the ACTUAL registered bindings, not a hard-coded
        // default, so it can never claim a hotkey that failed to register.
        wchar_t title[256];
        swprintf_s(title, L"VirtualHIDTyper - [%s] (%s 开始 / %s 停止)",
                   ModeShortName(m),
                   HotkeyText(g_hkStart).c_str(), HotkeyText(g_hkStop).c_str());
        SetWindowTextW(g_hwndMain, title);
    }
    if (updateStatus && g_staticStatus && !g_running && !g_inCountdown) {
        SetWindowTextW(g_staticStatus, ModeStatusText());
    }
}

static void LoadSettings()
{
    std::wstring dir = SettingsDir();
    std::wstring ini = dir + L"\\settings.ini";
    NormalizeIniSections(ini);   // collapse duplicate sections before reading
    wchar_t buf[32] = L"";
    GetPrivateProfileStringW(L"Main", L"IntervalMs", L"30", buf, 32, ini.c_str());
    SetWindowTextW(g_editInterval, buf);
    Button_SetCheck(g_chkShiftNl,
        GetPrivateProfileIntW(L"Main", L"ShiftEnter", 1, ini.c_str()) ? BST_CHECKED : BST_UNCHECKED);

    // Mode:
    //   * an explicit InputMode=hid/software/esp32 always wins (old HID users
    //     are NEVER silently migrated);
    //   * a missing key in an EXISTING settings file is an old profile that
    //     never recorded a mode. Ask once, explicitly, and persist the answer.
    //   * a fresh profile (no settings.ini) defaults to the new ESP32 mode.
    //   * smoke/custom-data runs never prompt: automated tests must not touch
    //     or depend on the user's real settings.
    wchar_t modeBuf[32] = L"";
    DWORD modeLen = GetPrivateProfileStringW(L"Main", L"InputMode", L"",
                                             modeBuf, 32, ini.c_str());
    if (modeLen == 0 || !modeBuf[0]) {
        const bool iniExisted =
            GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (iniExisted && !g_smokeMode && !g_customDataDir) {
            int choice = MessageBoxW(g_hwndMain,
                L"检测到旧版本设置文件，但没有记录输入模式。\n\n"
                L"是 = 切换到【ESP32 USB HID 模式】\n"
                L"     原生 USB 单线（CDC 控制 + HID 键盘），无需内核驱动，"
                L"普通用户即可运行；仅支持 ASCII。\n\n"
                L"否 = 保留【TextDeck HID 模式】\n"
                L"     继续使用现有 VirtualHIDDriver，可能需要管理员权限。\n\n"
                L"本程序不会静默改变旧配置；本次选择会保存，之后可在主窗口切换。",
                L"选择输入模式", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
            g_mode = (choice == IDYES) ? MODE_ESP32 : MODE_HID;
            const wchar_t* chosen = (g_mode == MODE_ESP32) ? L"esp32" : L"hid";
            WritePrivateProfileStringW(L"Main", L"InputMode", chosen,
                                       ini.c_str());
        } else {
            g_mode = MODE_ESP32;
        }
    } else if (_wcsicmp(modeBuf, L"software") == 0) {
        g_mode = MODE_SOFTWARE;
    } else if (_wcsicmp(modeBuf, L"esp32") == 0) {
        g_mode = MODE_ESP32;
    } else {
        g_mode = MODE_HID;   // "hid" and any unknown legacy value
    }

    wchar_t portBuf[64] = L"";
    GetPrivateProfileStringW(L"Main", L"Esp32Port", L"", portBuf, 64, ini.c_str());
    g_esp32Port = portBuf;
    SetMode(g_mode, false);

    // --- hotkey bindings ---------------------------------------------------
    // Absent -> built-in defaults.
    // LEGACY MIGRATION: earlier builds shipped Ctrl+Alt+F9 / Ctrl+Alt+X, which
    // collided with common applications. A settings file that still holds
    // exactly that OLD pair was never customised by the user, so it is upgraded
    // to the new defaults once (recorded by HotkeySchema). Any other stored
    // pair is a real user choice and is preserved verbatim.
    bool haveStart = false, haveStop = false;
    HotkeySpec fileStart = g_hkStart, fileStop = g_hkStop;
    wchar_t hkBuf[64] = L"";
    GetPrivateProfileStringW(L"Main", L"HotkeyStart", L"", hkBuf, 64, ini.c_str());
    if (hkBuf[0]) {
        unsigned m = 0, v = 0;
        if (swscanf_s(hkBuf, L"%u,%u", &m, &v) == 2 && v != 0) {
            fileStart.mods = m; fileStart.vk = v; haveStart = true;
        }
    }
    GetPrivateProfileStringW(L"Main", L"HotkeyStop", L"", hkBuf, 64, ini.c_str());
    if (hkBuf[0]) {
        unsigned m = 0, v = 0;
        if (swscanf_s(hkBuf, L"%u,%u", &m, &v) == 2 && v != 0) {
            fileStop.mods = m; fileStop.vk = v; haveStop = true;
        }
    }

    const HotkeySpec legacyStart = { MOD_CONTROL | MOD_ALT, VK_F9 };
    const HotkeySpec legacyStop  = { MOD_CONTROL | MOD_ALT, 'X' };
    int schema = GetPrivateProfileIntW(L"Main", L"HotkeySchema", 1, ini.c_str());
    bool isLegacyDefault = haveStart && haveStop &&
                           SameHotkey(fileStart, legacyStart) &&
                           SameHotkey(fileStop, legacyStop);

    if (isLegacyDefault && schema < 2) {
        // Upgrade the untouched legacy defaults to the new, less collision-prone
        // pair and persist the migration so the user can still customise later.
        g_hkStart = kDefaultHkStart;
        g_hkStop  = kDefaultHkStop;
        SaveHotkeySettings();
    } else {
        if (haveStart) g_hkStart = fileStart;
        if (haveStop)  g_hkStop  = fileStop;
    }

    if (SameHotkey(g_hkStart, g_hkStop)) {
        // corrupted settings must never leave start/stop identical
        g_hkStart = kDefaultHkStart;
        g_hkStop  = kDefaultHkStop;
    }

    std::wstring path = dir + L"\\text.txt";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        if (sz.QuadPart > 0 && sz.QuadPart < 4 * 1024 * 1024) {
            std::string raw((size_t)sz.QuadPart, '\0');
            DWORD rd = 0;
            ReadFile(h, &raw[0], (DWORD)raw.size(), &rd, nullptr);
            raw.resize(rd);
            if (raw.size() >= 2 && (BYTE)raw[0] == 0xFF && (BYTE)raw[1] == 0xFE) {
                std::wstring text((const wchar_t*)(raw.data() + 2),
                                  (raw.size() - 2) / sizeof(wchar_t));
                SetWindowTextW(g_editText, text.c_str());
            }
        }
        CloseHandle(h);
    }
}

static void SavePortSetting()
{
    std::wstring ini = SettingsDir() + L"\\settings.ini";
    WritePrivateProfileStringW(L"Main", L"Esp32Port", g_esp32Port.c_str(),
                               ini.c_str());
}

static void CancelEsp32Countdown()
{
    if (g_esp32CountdownArmed) {
        g_esp32CountdownArmed = false;
        g_inCountdown = false;
        KillTimer(g_hwndMain, TIMER_COUNTDOWN);
        g_task.store(TS_IDLE);
        g_esp32PendingText.clear();
        g_esp32PendingPort.clear();
        g_esp32PendingTarget = nullptr;
        UpdateTrayIcon();
    }
}

static void SetLastErrorDetail(const std::wstring& detail)
{
    std::lock_guard<std::mutex> lk(g_lastErrorLock);
    g_lastErrorDetail = detail;
}

static std::wstring TakeLastErrorDetail()
{
    std::lock_guard<std::mutex> lk(g_lastErrorLock);
    std::wstring s;
    s.swap(g_lastErrorDetail);
    return s;
}

static std::wstring SelectedPortFromCombo()
{
    if (!g_comboPort) return L"";
    int sel = (int)SendMessageW(g_comboPort, CB_GETCURSEL, 0, 0);
    if (sel < 0) return L"";
    int idx = (int)SendMessageW(g_comboPort, CB_GETITEMDATA, (WPARAM)sel, 0);
    if (idx < 0 || (size_t)idx >= g_portList.size()) return L"";
    return g_portList[(size_t)idx].port;
}

// Enumerate present COM ports and select the saved/auto-detected one. This
// never opens a port: opening only happens on EXPLICIT test/start, so startup
// cannot reset the device and cannot steal the port from another tool.
static void RefreshPortCombo()
{
    std::wstring previous = g_esp32Port.empty() ? SelectedPortFromCombo()
                                                : g_esp32Port;
    g_portList.clear();
    esp32::Link::ListPorts(&g_portList);

    if (g_comboPort) {
        SendMessageW(g_comboPort, CB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < g_portList.size(); ++i) {
            const esp32::PortInfo& p = g_portList[i];
            std::wstring label = p.port;
            if (p.espressif) label += L"  [ESP32]";
            if (!p.friendly.empty() && p.friendly != p.port)
                label += L"  " + p.friendly;
            int item = (int)SendMessageW(g_comboPort, CB_ADDSTRING, 0,
                                         (LPARAM)label.c_str());
            if (item >= 0)
                SendMessageW(g_comboPort, CB_SETITEMDATA, (WPARAM)item,
                             (LPARAM)i);
        }

        std::wstring pick = esp32::Link::ChoosePort(g_portList, previous);
        // Never silently pick an unrelated serial device: ChoosePort returns
        // empty when neither the saved port nor an Espressif VID_303A port is
        // present. The user can still select any listed port manually.
        g_esp32Port = pick;

        int select = -1;
        for (size_t i = 0; i < g_portList.size(); ++i) {
            if (_wcsicmp(g_portList[i].port.c_str(), pick.c_str()) == 0) {
                select = (int)i;
                break;
            }
        }
        if (select >= 0) SendMessageW(g_comboPort, CB_SETCURSEL, (WPARAM)select, 0);
    }

    if (g_mode == MODE_ESP32 && !g_running && !g_inCountdown && g_staticStatus)
        SetWindowTextW(g_staticStatus, ModeStatusText());
}

// Explicit connectivity test. Opens the selected COM port (only on this
// explicit user action or on START), PINGs the firmware and, for v1.2+,
// fetches the idle typing status. The handle stays open for the next run.
// No text is written and no HID key is pressed.
static void TestEsp32Connection()
{
    if (g_running || g_inCountdown) return;
    RefreshPortCombo();

    const std::wstring port = g_esp32Port;
    if (port.empty()) {
        const wchar_t* msg = L"未检测到 ESP32 串口。请连接开发板原生 USB 口，"
                             L"待 Windows 枚举出 CDC 串口后点“刷新串口”，"
                             L"或在列表中手动选择正确的 COM 口。";
        if (g_staticStatus) SetWindowTextW(g_staticStatus, msg);
        SetTrayDetail(msg);
        return;
    }

    std::wstring err;
    if (!g_esp32.IsOpen() ||
        _wcsicmp(g_esp32.PortName().c_str(), port.c_str()) != 0) {
        g_esp32.Close();
        if (!g_esp32.Open(port, &err)) {
            if (g_staticStatus) SetWindowTextW(g_staticStatus, err.c_str());
            SetTrayDetail(err);
            return;
        }
    }

    esp32::PingInfo ping;
    if (!g_esp32.Ping(&ping, &err)) {
        g_esp32.Close();
        std::wstring why = L"连接 ESP32 失败：" + err;
        if (g_staticStatus) SetWindowTextW(g_staticStatus, why.c_str());
        SetTrayDetail(why);
        return;
    }
    SavePortSetting();

    wchar_t buf[768];
    if (ping.fw < esp32::kFirmwareV12) {
        swprintf_s(buf,
            L"[ESP32 USB HID 模式] 已连接 %s，固件 0x%04X 低于 v1.2 (0x0102)，"
            L"HID %s。PING 可用，但 START/CANCEL/GET_STATUS 远程输入命令不可用；"
            L"请先烧录 v1.2 固件。",
            port.c_str(), ping.fw,
            ping.hidReady ? L"已枚举" : L"未枚举 (hid_ready=0)");
    } else {
        esp32::TypingStatus st{};
        std::wstring statusErr;
        const bool haveStatus = g_esp32.GetStatus(&st, &statusErr);
        if (haveStatus) {
            swprintf_s(buf,
                L"[ESP32 USB HID 模式] 已连接 %s：固件 1.2.0 (0x%04X)，协议 0x%02X，"
                L"HID %s，文本上限 %u 字节，能力 0x%04X，状态 %s，会话 %u，"
                L"租约剩余 %u ms。未写入文本，未发送按键。",
                port.c_str(), ping.fw, ping.proto,
                ping.hidReady ? L"已枚举" : L"未枚举 (hid_ready=0)",
                (unsigned)ping.maxText, (unsigned)st.capabilities,
                st.state == esp32::kTyIdle ? L"空闲" :
                st.state == esp32::kTyCompleted ? L"上次已完成" :
                st.state == esp32::kTyCancelled ? L"上次已取消" :
                st.state == esp32::kTyFailed ? L"上次失败" : L"进行中",
                (unsigned)st.sessionId, (unsigned)st.leaseRemainingMs);
        } else {
            swprintf_s(buf,
                L"[ESP32 USB HID 模式] 已连接 %s：固件 1.2.0 (0x%04X)，HID %s。"
                L"但 GET_STATUS 失败：%s",
                port.c_str(), ping.fw,
                ping.hidReady ? L"已枚举" : L"未枚举 (hid_ready=0)",
                statusErr.c_str());
        }
    }
    if (g_staticStatus) SetWindowTextW(g_staticStatus, buf);
    SetTrayDetail(buf);
    UpdateTrayIcon();
}

// ----- text persistence -----------------------------------------------------
// The body text lives in %APPDATA%\VirtualHIDTyper\text.txt (UTF-16LE with a
// 0xFEFF byte-order mark). Save is explicit (Save button / tray menu / on
// exit) so a partially-typed edit is never silently written.
static bool SaveTextToDisk(std::wstring* errOut)
{
    std::wstring dir = SettingsDir();
    std::wstring path = dir + L"\\text.txt";
    int len = GetWindowTextLengthW(g_editText);
    std::wstring text((size_t)len + 1, L'\0');
    if (len > 0) GetWindowTextW(g_editText, &text[0], len + 1);
    text.resize(wcslen(text.c_str()));

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (errOut) *errOut = L"无法写入文本文件（错误码 " + std::to_wstring(GetLastError()) + L"）";
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, L"\xFEFF", 2, &written, nullptr);
    if (ok && !text.empty()) {
        ok = WriteFile(h, text.data(), (DWORD)(text.size() * sizeof(wchar_t)),
                       &written, nullptr);
    }
    DWORD writeErr = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) {
        if (errOut) *errOut = L"写入文本失败（错误码 " + std::to_wstring(writeErr) + L"）";
        return false;
    }
    return true;
}

// ----- tray icon ------------------------------------------------------------
// The tray is the primary UI surface once the window is hidden, so it is kept
// in sync with the task state at every transition. Nothing here touches the
// foreground window: tooltips and icon changes never steal focus.
static const wchar_t* TaskStateText(TaskState s)
{
    switch (s) {
    case TS_STARTING:  return L"正在预检…";
    case TS_TYPING:    return L"正在输入…";
    case TS_STOPPING:  return L"正在停止…";
    case TS_COMPLETED: return L"输入已完成";
    case TS_FAILED:    return L"输入失败";
    default:           return L"待机";
    }
}

static HICON IconForState(TaskState s)
{
    if (s == TS_FAILED) return g_iconErr ? g_iconErr : g_iconApp;
    if (s == TS_TYPING || s == TS_STARTING || s == TS_STOPPING)
        return g_iconBusy ? g_iconBusy : g_iconApp;
    return g_iconIdle ? g_iconIdle : g_iconApp;
}

// Extra detail appended to the tooltip (mode, progress, last error, hotkey
// health). Kept short: the shell truncates long tooltips.
static std::wstring g_trayDetail;

static void UpdateTrayIcon()
{
    if (!g_trayAdded) return;
    TaskState s = (TaskState)g_task.load();
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.hIcon = IconForState(s);
    std::wstring tip = L"VirtualHIDTyper - ";
    tip += TaskStateText(s);
    // Persistent health warning, independent of the changing detail line: a
    // registration failure must stay visible for as long as it is true, so
    // the user can never believe an emergency-stop hotkey is live when it is
    // not (requirement: continuous visible notice on failed registration).
    if (!g_hkStartOk || !g_hkStopOk) {
        tip += L" [快捷键未注册:";
        if (!g_hkStartOk && !g_hkStopOk) tip += L"开始+停止";
        else if (!g_hkStopOk)            tip += L"紧急停止";
        else                             tip += L"开始";
        tip += L"失效,请托盘菜单操作]";
    }
    if (!g_trayDetail.empty()) {
        tip += L"\n";
        tip += g_trayDetail;
    }
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_trayTip = tip;
}

static void SetTrayDetail(const std::wstring& detail)
{
    g_trayDetail = detail;
    UpdateTrayIcon();
}

static void AddTrayIcon(HWND hwnd)
{
    if (g_trayAdded) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_UID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = IconForState((TaskState)g_task.load());
    wcscpy_s(g_nid.szTip, L"VirtualHIDTyper - 待机");
    if (Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        g_trayAdded = true;
        UpdateTrayIcon();
    }
}

static void RemoveTrayIcon()
{
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    g_trayAdded = false;
}

// ----- hotkey formatting / registration -------------------------------------
static std::wstring VkName(UINT vk)
{
    if (vk >= (UINT)'A' && vk <= (UINT)'Z') return std::wstring(1, (wchar_t)vk);
    if (vk >= (UINT)'0' && vk <= (UINT)'9') return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    switch (vk) {
    case VK_SPACE:     return L"Space";
    case VK_RETURN:    return L"Enter";
    case VK_TAB:       return L"Tab";
    case VK_ESCAPE:    return L"Esc";
    case VK_OEM_3:     return L"Grave";
    case VK_OEM_MINUS: return L"-";
    case VK_OEM_PLUS:  return L"=";
    case VK_OEM_4:     return L"[";
    case VK_OEM_5:     return L"\\";
    case VK_OEM_6:     return L"]";
    case VK_OEM_1:     return L";";
    case VK_OEM_7:     return L"'";
    case VK_OEM_COMMA: return L",";
    case VK_OEM_PERIOD:return L".";
    case VK_OEM_2:     return L"/";
    default: break;
    }
    wchar_t buf[32];
    swprintf_s(buf, L"VK%02X", vk);
    return buf;
}

static std::wstring HotkeyText(const HotkeySpec& hk)
{
    std::wstring s;
    if (hk.mods & MOD_CONTROL) s += L"Ctrl+";
    if (hk.mods & MOD_ALT)     s += L"Alt+";
    if (hk.mods & MOD_SHIFT)   s += L"Shift+";
    if (hk.mods & MOD_WIN)     s += L"Win+";
    s += VkName(hk.vk);
    return s;
}

static bool SameHotkey(const HotkeySpec& a, const HotkeySpec& b)
{
    return a.mods == b.mods && a.vk == b.vk;
}

// Per-half binding switch that can NEVER leave the user with no working
// hotkeys. Each half toggles between its OWN two ids (start: 1<->3, stop:
// 2<->4):
//   * a half whose chord did not change is NOT re-registered at all - the old
//     registration keeps working (this also makes a start-only or stop-only
//     change conflict-free against our OWN still-registered other half);
//   * a changed half is first registered on its FREE id, and its old id is
//     only released after that succeeded (the shell has no "peek", so this
//     take-then-release order is the only conflict-free one);
//   * a mixed result (one half taken, the other refused) is rolled back to
//     the EXACT previous pair, so UI text, persistence and the live
//     registrations can never disagree.
// Returns true only when the requested pair is fully in effect (including
// "already in effect"). A false return means at least one half was refused
// by the shell and everything was rolled back; g_hk*Ok keeps describing the
// LIVE registrations (the previous pair stays ok after a rollback), so
// callers MUST use the return value - never the ok flags - to detect the
// refusal. g_hk*Err carries the failing GetLastError() of the refused half.
static bool ApplyHotkeys(HWND hwnd, const HotkeySpec& start, const HotkeySpec& stop)
{
    if (SameHotkey(start, stop)) {
        // Refuse the pair outright: two identical bindings are ambiguous and
        // the shell would only let one of them register anyway. Whatever is
        // registered right now stays registered.
        g_hkStartErr = g_hkStopErr = ERROR_INVALID_PARAMETER;
        return false;
    }

    const bool firstRun = !g_hkStartOk && !g_hkStopOk &&
                          g_hkIdStart == 0 && g_hkIdStop == 0;
    const bool startUnchanged = g_hkStartOk && SameHotkey(start, g_hkStart);
    const bool stopUnchanged  = g_hkStopOk  && SameHotkey(stop,  g_hkStop);
    g_hkStartErr = g_hkStopErr = 0;

    if (startUnchanged && stopUnchanged) return true;   // nothing to do

    const HotkeySpec prevStart = g_hkStart, prevStop = g_hkStop;
    const bool prevStartOk = g_hkStartOk, prevStopOk = g_hkStopOk;

    // MOD_NOREPEAT: holding the chord produces ONE WM_HOTKEY, so an
    // auto-repeating key can never queue a burst of start/stop requests.
    bool startTaken = startUnchanged, stopTaken = stopUnchanged;
    int candStart = 0, candStop = 0;

    if (!startUnchanged) {
        candStart = (g_hkIdStart == ID_HOTKEY_START) ? ID_HOTKEY_TMP_START
                                                     : ID_HOTKEY_START;
        if (RegisterHotKey(hwnd, candStart, start.mods | MOD_NOREPEAT, start.vk)) {
            startTaken = true;
        } else {
            g_hkStartErr = GetLastError();
            candStart = 0;
        }
    }
    if (!stopUnchanged) {
        candStop = (g_hkIdStop == ID_HOTKEY_STOP) ? ID_HOTKEY_TMP_STOP
                                                  : ID_HOTKEY_STOP;
        if (RegisterHotKey(hwnd, candStop, stop.mods | MOD_NOREPEAT, stop.vk)) {
            stopTaken = true;
        } else {
            g_hkStopErr = GetLastError();
            candStop = 0;
        }
    }

    if (firstRun) {
        // No previous pair to protect: keep whichever halves registered, so
        // at least the emergency stop stays hotkey-able whenever possible.
        g_hkStart = start;
        g_hkStop  = stop;
        if (startTaken) { g_hkIdStart = candStart; g_hkStartOk = true; }
        if (stopTaken)  { g_hkIdStop  = candStop;  g_hkStopOk  = true; }
        return g_hkStartOk && g_hkStopOk;   // a half that never registered
                                             // must still be reported
    }

    if (!startTaken || !stopTaken) {
        // Roll back the half that WAS taken so the previous pair remains
        // exactly registered - a failed change costs the user nothing.
        if (candStart) UnregisterHotKey(hwnd, candStart);
        if (candStop)  UnregisterHotKey(hwnd, candStop);
        g_hkStart = prevStart;  g_hkStartOk = prevStartOk;
        g_hkStop  = prevStop;   g_hkStopOk  = prevStopOk;
        // false = the caller must TELL the user the change was refused: the
        // restored ok flags describe the still-live old pair and would make
        // any flag-based check report success (a previous flag-check bug).
        return false;
    }

    // Fully successful: release the old id of each changed half and adopt.
    if (!startUnchanged) {
        if (g_hkIdStart) UnregisterHotKey(hwnd, g_hkIdStart);
        g_hkIdStart = candStart;
    }
    if (!stopUnchanged) {
        if (g_hkIdStop) UnregisterHotKey(hwnd, g_hkIdStop);
        g_hkIdStop = candStop;
    }
    g_hkStart = start;
    g_hkStop  = stop;
    g_hkStartOk = g_hkStopOk = true;
    return true;
}

// Full teardown used on real shutdown paths (both id pairs, plus whatever a
// partially-failed start-up may have registered).
static void UnregisterAllHotkeys(HWND hwnd)
{
    if (!hwnd) return;
    UnregisterHotKey(hwnd, ID_HOTKEY_START);
    UnregisterHotKey(hwnd, ID_HOTKEY_STOP);
    UnregisterHotKey(hwnd, ID_HOTKEY_TMP_START);
    UnregisterHotKey(hwnd, ID_HOTKEY_TMP_STOP);
    g_hkIdStart = g_hkIdStop = 0;
}

static std::wstring HotkeyStatusText()
{
    std::wstring s;
    s += L"开始: " + HotkeyText(g_hkStart);
    s += g_hkStartOk ? L" (已注册)" : L" (注册失败)";
    s += L"    停止: " + HotkeyText(g_hkStop);
    s += g_hkStopOk ? L" (已注册)" : L" (注册失败)";
    return s;
}

// Persist only the hotkey pair (used by the settings dialog so a successful
// rebind survives a restart even if the user never presses Save again).
// Returns false when ANY write failed so the caller can tell the user the
// binding is live for THIS run only, instead of pretending it is permanent.
static bool SaveHotkeySettings()
{
    std::wstring dir = SettingsDir();
    std::wstring ini = dir + L"\\settings.ini";
    NormalizeIniSections(ini);   // see the note above: keep [Main] unique
    wchar_t hk[64];
    bool ok = true;
    swprintf_s(hk, L"%u,%u", g_hkStart.mods, g_hkStart.vk);
    ok = (WritePrivateProfileStringW(L"Main", L"HotkeyStart", hk, ini.c_str()) != 0) && ok;
    swprintf_s(hk, L"%u,%u", g_hkStop.mods, g_hkStop.vk);
    ok = (WritePrivateProfileStringW(L"Main", L"HotkeyStop", hk, ini.c_str()) != 0) && ok;
    // Schema 2 = the Ctrl+Alt+Shift defaults. Stamped so the legacy-default
    // migration in LoadSettings never overwrites a real user choice.
    ok = (WritePrivateProfileStringW(L"Main", L"HotkeySchema", L"2", ini.c_str()) != 0) && ok;
    return ok;
}

// ----- HID device (client adapter for the TextDeck driver) ------------------
// Thin adapter over the IOCTL contract in windows/shared/textdeck_protocol.h.
// It deliberately contains no typing policy: it opens the device, verifies the
// protocol version, and submits key events. Timing, text mapping and stop
// handling live above it.
class HidDevice {
public:
    enum class Result { Ok, NotFound, AccessDenied, Other };

    // Populated by Open(): the driver's reported protocol version (diagnostics).
    unsigned ProtocolMajor = 0;
    unsigned ProtocolMinor = 0;

    Result Open(DWORD* err)
    {
        Close();
        h_ = CreateFileW(kDevicePath, GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (err) *err = e;
            switch (e) {
            case ERROR_FILE_NOT_FOUND: return Result::NotFound;
            case ERROR_ACCESS_DENIED:  return Result::AccessDenied;
            default:                   return Result::Other;
            }
        }

        // Refuse to drive a driver whose ABI we do not understand. A version
        // mismatch is far more likely to be a stale driver than a broken one,
        // and typing through a mismatched struct layout would send garbage.
        TEXTDECK_DEVICE_INFO info{};
        DWORD returned = 0;
        if (!DeviceIoControl(h_, IOCTL_TEXTDECK_GET_INFO, nullptr, 0,
                             &info, sizeof(info), &returned, nullptr) ||
            returned < sizeof(info)) {
            if (err) *err = GetLastError();
            Close();
            return Result::Other;
        }
        ProtocolMajor = info.ProtocolMajor;
        ProtocolMinor = info.ProtocolMinor;
        if (info.ProtocolMajor != TEXTDECK_PROTOCOL_MAJOR) {
            if (err) *err = ERROR_REVISION_MISMATCH;
            Close();
            return Result::Other;
        }
        return Result::Ok;
    }

    void Close()
    {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

    bool IsOpen() const { return h_ != INVALID_HANDLE_VALUE; }

    // Submit one keyboard state transition. The driver validates the payload; a
    // rejected request comes back as a real error rather than being retried
    // blindly. ERROR_BUSY means VHF still owns the previous report, so a short
    // bounded retry is the correct response to that one code only.
    //
    // `modifiers` is the COMPLETE set of modifiers that must be held in the
    // report this call produces (protocol 1.1), not "the modifiers attached to
    // this event". For a plain character that is the character's own modifier
    // set; for the release that follows, it must be the same set again or the
    // modifier would be dropped before the OS finished the chord.
    bool SendKey(BYTE modifiers, BYTE usage, bool down, DWORD* err)
    {
        return SendEvent(modifiers, usage,
                         down ? TextDeckKeyActionDown : TextDeckKeyActionUp, err);
    }

    // Hold exactly `modifiers` with no key pressed. This is the report a real
    // keyboard sends when Ctrl/Alt/Shift/GUI goes down on its own, and the only
    // way to express "Ctrl is held" before a letter is pressed.
    bool SendModifiers(BYTE modifiers, DWORD* err)
    {
        return SendEvent(modifiers, 0, TextDeckKeyActionModifiers, err);
    }

    bool SendEvent(BYTE modifiers, BYTE usage, BYTE action, DWORD* err)
    {
        TEXTDECK_KEY_EVENT ev{};
        ev.Modifiers = modifiers;
        ev.Usage     = usage;
        ev.Action    = action;
        ev.Reserved  = 0;

        for (int attempt = 0; attempt < 20; ++attempt) {
            DWORD returned = 0;
            BOOL ok = DeviceIoControl(h_, IOCTL_TEXTDECK_SUBMIT_KEY, &ev, sizeof(ev),
                                      nullptr, 0, &returned, nullptr);
            if (ok) return true;
            DWORD e = GetLastError();
            if (err) *err = e;
            if (e == ERROR_BUSY /*170*/) {
                Sleep(10);
                continue;
            }
            return false;
        }
        return false;
    }

    // Dedicated release-all IOCTL: clears every modifier and key slot even when
    // the caller does not know which keys are currently held.
    bool SendReleaseAll(DWORD* err)
    {
        DWORD returned = 0;
        BOOL ok = DeviceIoControl(h_, IOCTL_TEXTDECK_RELEASE_ALL, nullptr, 0,
                                  nullptr, 0, &returned, nullptr);
        if (!ok && err) *err = GetLastError();
        return ok != FALSE;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

static HidDevice g_device;

// ----- typing worker --------------------------------------------------------
static void TypingThread(std::wstring text, int intervalMs, bool shiftEnter, HWND target)
{
    DoneReason reason = DONE_COMPLETED;
    DWORD lastErr = 0;

    int holdMs = intervalMs / 3;
    if (holdMs < 5) holdMs = 5;
    if (holdMs > 15) holdMs = 15;
    int gapMs = intervalMs - holdMs;
    if (gapMs < 0) gapMs = 0;

    g_sent = 0;
    g_skipped = 0;

    for (size_t i = 0; i < text.size(); ++i) {
        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) {
            reason = DONE_STOPPED;
            break;
        }
        if (GetForegroundWindow() != target) {
            reason = DONE_FOCUS;
            break;
        }

        vhid::KeyStroke ks = vhid::CharToStroke(text[i], shiftEnter);
        if (!ks.supported) {
            g_skipped.fetch_add(1);
            continue;
        }

        // press must be paired with a release even if the press fails, so a
        // half-sent chord cannot leave a key stuck
        if (!g_device.SendKey(ks.modifiers, ks.usage, true, &lastErr)) {
            g_device.SendReleaseAll(nullptr);
            reason = DONE_ERROR;
            break;
        }
        // The release repeats the SAME modifier set: under protocol 1.1 the
        // Modifiers field is the complete held set, so sending 0 here would
        // drop Shift before the OS had acted on the shifted key. The report
        // that follows releases everything, so the modifier does not linger.
        if (!g_device.SendKey(ks.modifiers, ks.usage, false, &lastErr)) {
            reason = DONE_ERROR;
            break;
        }
        // Now that the character is delivered, let go of its modifier.
        if (ks.modifiers != 0 && !g_device.SendModifiers(0, &lastErr)) {
            reason = DONE_ERROR;
            break;
        }
        g_sent.fetch_add(1);
        PostMessageW(g_hwndMain, WM_APP_PROGRESS, (WPARAM)g_sent.load(),
                     (WPARAM)text.size());

        if (WaitForSingleObject(g_stopEvent, (DWORD)holdMs) == WAIT_OBJECT_0) {
            reason = DONE_STOPPED;
            break;
        }
        if (gapMs > 0 &&
            WaitForSingleObject(g_stopEvent, (DWORD)gapMs) == WAIT_OBJECT_0) {
            reason = DONE_STOPPED;
            break;
        }
    }

    // Always release everything (normally the per-key release already sent an
    // all-zero report; this guarantees no residual Shift/keys after Stop).
    DWORD relErr = 0;
    if (g_device.IsOpen() && !g_device.SendReleaseAll(&relErr) && reason == DONE_COMPLETED) {
        reason = DONE_ERROR;
        lastErr = relErr;
    }

    PostMessageW(g_hwndMain, WM_APP_DONE, (WPARAM)reason, (LPARAM)lastErr);
}

// ----- software-mode sender (SendInput) ------------------------------------
// One planned event -> one SendInput batch. An astral character is sent as a
// high+low KEYEVENTF_UNICODE pair inside a SINGLE SendInput call, so the
// surrogate pair can never be split across checks or interleaved with the
// stop key. Newline -> VK_RETURN (+Shift when selected), Tab -> VK_TAB:
// control characters are never typed as plain Unicode text. Nothing ever
// goes through the clipboard. A short/failed SendInput (typically UIPI when
// the target runs at a higher integrity level) stops immediately - no retry
// loop, no bypass attempt.
static bool SendSoftwareEvent(const vhid::SwEvent& e, DWORD* err)
{
    INPUT inputs[4];
    ZeroMemory(inputs, sizeof(inputs));
    int n = 0;
    auto key = [&inputs, &n](WORD vk, bool down) {
        INPUT& in = inputs[n++];
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    };
    auto uni = [&inputs, &n](wchar_t u, bool down) {
        INPUT& in = inputs[n++];
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = u;
        in.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP);
    };

    switch (e.kind) {
    case vhid::SwKind::CodePoint:
        if (e.cp >= 0x10000u) {
            wchar_t hi = (wchar_t)(0xD800 + ((e.cp - 0x10000u) >> 10));
            wchar_t lo = (wchar_t)(0xDC00 + (e.cp & 0x3FFu));
            uni(hi, true); uni(hi, false);
            uni(lo, true); uni(lo, false);
        } else {
            uni((wchar_t)e.cp, true);
            uni((wchar_t)e.cp, false);
        }
        break;
    case vhid::SwKind::Enter:
        key(VK_RETURN, true);  key(VK_RETURN, false);
        break;
    case vhid::SwKind::ShiftEnter:
        key(VK_SHIFT, true);   key(VK_RETURN, true);
        key(VK_RETURN, false); key(VK_SHIFT, false);
        break;
    case vhid::SwKind::Tab:
        key(VK_TAB, true);     key(VK_TAB, false);
        break;
    default:
        return true; // SkipInvalid never reaches here (filtered by the worker)
    }

    UINT sentCount = SendInput(n, inputs, sizeof(INPUT));
    if (sentCount != (UINT)n) {
        if (err) *err = GetLastError();
        return false;
    }
    return true;
}

// Software-mode typing worker: same stop / focus-loss / progress protocol as
// the HID worker. One planned event counts as one typed character.
static void SoftwareTypingThread(std::vector<vhid::SwEvent> events,
                                 int intervalMs, HWND target)
{
    DoneReason reason = DONE_COMPLETED;
    DWORD lastErr = 0;

    g_sent = 0;
    g_skipped = 0;
    long total = (long)events.size();

    for (size_t i = 0; i < events.size(); ++i) {
        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) {
            reason = DONE_STOPPED;
            break;
        }
        if (GetForegroundWindow() != target) {
            reason = DONE_FOCUS;
            break;
        }
        const vhid::SwEvent& e = events[i];
        if (e.kind == vhid::SwKind::SkipInvalid) {
            g_skipped.fetch_add(1);
            continue;
        }
        if (!SendSoftwareEvent(e, &lastErr)) {
            reason = DONE_ERROR;
            break;
        }
        g_sent.fetch_add(1);
        PostMessageW(g_hwndMain, WM_APP_PROGRESS, (WPARAM)g_sent.load(),
                     (WPARAM)total);

        if (WaitForSingleObject(g_stopEvent, (DWORD)intervalMs) == WAIT_OBJECT_0) {
            reason = DONE_STOPPED;
            break;
        }
    }

    if (reason == DONE_ERROR) {
        // best effort: a partially-failed SendInput batch could leave a key
        // down; release the keys this mode ever presses. Harmless if blocked.
        INPUT rel[3];
        ZeroMemory(rel, sizeof(rel));
        const WORD vks[3] = { VK_SHIFT, VK_RETURN, VK_TAB };
        for (int k = 0; k < 3; ++k) {
            rel[k].type = INPUT_KEYBOARD;
            rel[k].ki.wVk = vks[k];
            rel[k].ki.dwFlags = KEYEVENTF_KEYUP;
        }
        SendInput(3, rel, sizeof(INPUT));
    }

    PostMessageW(g_hwndMain, WM_APP_DONE, (WPARAM)reason, (LPARAM)lastErr);
}

// ----- ESP32 worker ---------------------------------------------------------
// The UI thread never blocks on serial I/O. It only validates text and starts
// this worker, which opens/reuses the CDC port, writes the text if needed,
// sends START, polls GET_STATUS for the 3 s device lease, and sends CANCEL
// immediately on stop/focus loss/disconnect. COM is deliberately NOT closed
// at the end of a run.
static bool Esp32WaitStop(DWORD ms)
{
    if (!g_stopEvent) return false;
    return WaitForSingleObject(g_stopEvent, ms) == WAIT_OBJECT_0;
}

static bool Esp32Cancel(std::wstring* err, esp32::TypingStatus* out = nullptr)
{
    esp32::TypingStatus st;
    std::wstring local;
    if (!g_esp32.CancelTyping(&st, &local)) {
        if (err) *err = local;
        return false;
    }
    if (out) *out = st;
    return true;
}

static void Esp32TypingThread(std::string text, std::wstring port, HWND target)
{
    g_sent = 0;
    g_skipped = 0;
    SetLastErrorDetail(L"");

    auto done = [](DoneReason reason, DWORD err = 0) {
        PostMessageW(g_hwndMain, WM_APP_DONE, (WPARAM)reason, (LPARAM)err);
    };
    auto fail = [&](const std::wstring& why, DWORD err = 0) {
        SetLastErrorDetail(why);
        done(DONE_ERROR, err);
    };

    if (Esp32WaitStop(0)) { done(DONE_STOPPED); return; }

    // Reuse the open handle across runs. Close only after a real link error
    // (or when the user selected a different port).
    if (!g_esp32.IsOpen() ||
        _wcsicmp(g_esp32.PortName().c_str(), port.c_str()) != 0) {
        g_esp32.Close();
        std::wstring err;
        if (!g_esp32.Open(port, &err)) {
            fail(err);
            return;
        }
    }

    esp32::PingInfo ping;
    std::wstring err;
    if (!g_esp32.Ping(&ping, &err)) {
        fail(L"连接 ESP32 失败：" + err);
        return;
    }
    if (ping.fw < esp32::kFirmwareV12) {
        wchar_t ver[96];
        swprintf_s(ver, L"设备固件版本为 0x%04X，低于远程输入所需的 1.2.0 (0x0102)。"
                        L"请先烧录 v1.2 固件；本模式不会回退到驱动或 SendInput。",
                   ping.fw);
        fail(ver);
        return;
    }
    if (!ping.hidReady) {
        fail(L"ESP32 已连接，但 USB HID 键盘未枚举（hid_ready=0）。"
             L"请确认原生 USB 线已连接且设备未被其他主机占用。");
        return;
    }
    if (text.size() > ping.maxText) {
        wchar_t msg[160];
        swprintf_s(msg, L"文本为 %u 字节，超过设备上限 %u 字节。",
                   (unsigned)text.size(), (unsigned)ping.maxText);
        fail(msg);
        return;
    }

    const uint32_t wantCrc = esp32::Crc32(
        (const uint8_t*)text.data(), text.size());
    const bool needWrite = !ping.hasText ||
                           ping.savedLen != (uint16_t)text.size() ||
                           ping.savedCrc != wantCrc;
    if (needWrite) {
        if (Esp32WaitStop(0)) { done(DONE_STOPPED); return; }
        esp32::WriteAck ack;
        if (!g_esp32.WriteText(text, &ack, &err)) {
            fail(L"写入 ESP32 文本失败：" + err);
            return;
        }
        if (ack.savedLen != text.size() || ack.savedCrc != wantCrc) {
            fail(L"ESP32 写入回执校验失败：设备保存的长度/CRC 与发送内容不一致。");
            return;
        }
    }

    if (Esp32WaitStop(0)) { done(DONE_STOPPED); return; }
    if (GetForegroundWindow() != target) {
        done(DONE_FOCUS);
        return;
    }

    esp32::TypingStatus st;
    if (!g_esp32.StartTyping(&st, &err)) {
        fail(L"启动 ESP32 输入失败：" + err);
        return;
    }
    if ((st.capabilities & esp32::kCapRemoteTyping) == 0) {
        std::wstring cancelErr;
        Esp32Cancel(&cancelErr);
        fail(L"固件未声明远程输入能力（capabilities 缺少 bit0）。");
        return;
    }
    if (st.state == esp32::kTyCompleted) {
        g_sent = st.total;
        done(DONE_COMPLETED);
        return;
    }
    if (st.state == esp32::kTyCancelled || st.state == esp32::kTyFailed) {
        fail(std::wstring(L"ESP32 启动后立即结束：") +
             esp32::TypingErrorText(st.lastError));
        return;
    }

    PostMessageW(g_hwndMain, WM_APP_PROGRESS, 0, (WPARAM)st.total);

    for (;;) {
        if (Esp32WaitStop(0)) {
            esp32::TypingStatus cs;
            std::wstring cerr;
            if (!Esp32Cancel(&cerr, &cs)) {
                fail(L"停止时发送 CANCEL 失败：" + cerr +
                     L"。设备将在主机租约 3 秒后自动停下并释放按键。");
            } else if (cs.state == esp32::kTyCompleted) {
                g_sent = cs.processed;
                done(DONE_COMPLETED);
            } else {
                g_sent = cs.processed;
                done(DONE_STOPPED);
            }
            return;
        }

        if (GetForegroundWindow() != target) {
            // Focus left the target: stop device-side typing at once. The
            // remaining text is never resumed.
            std::wstring cerr;
            esp32::TypingStatus cs{};
            if (!Esp32Cancel(&cerr, &cs)) {
                SetLastErrorDetail(L"焦点已离开目标窗口，且 CANCEL 发送失败：" +
                                   cerr + L"。设备将在 3 秒租约后自动停止。");
            }
            g_sent = cs.processed;
            done(DONE_FOCUS);
            return;
        }

        // 250 ms poll keeps the 3 s host lease alive with a wide margin and
        // keeps stop/cancel latency well under a quarter second.
        if (Esp32WaitStop(250)) continue;

        if (!g_esp32.GetStatus(&st, &err)) {
            if (Esp32WaitStop(0)) continue;
            // One failed status read is not enough to declare the device
            // dead. Try a CANCEL so a still-live device stops immediately;
            // otherwise its own 3 s lease is the final safety net.
            esp32::TypingStatus cs;
            std::wstring cerr;
            const bool cancelled = Esp32Cancel(&cerr, &cs);
            if (cancelled) {
                g_sent = cs.processed;
                fail(L"读取 ESP32 状态失败：" + err +
                     L"。已尽力发送 CANCEL 停止设备。");
            } else {
                fail(L"读取 ESP32 状态失败：" + err +
                     L"；CANCEL 也失败：" + cerr +
                     L"。设备将在主机租约 3 秒后自动停止。");
            }
            return;
        }

        g_sent = st.processed;
        PostMessageW(g_hwndMain, WM_APP_PROGRESS, (WPARAM)st.processed,
                     (WPARAM)st.total);

        if (st.state == esp32::kTyCompleted) {
            if (st.processed != st.total) {
                wchar_t msg[160];
                swprintf_s(msg, L"设备报告完成，但进度为 %u/%u。",
                           (unsigned)st.processed, (unsigned)st.total);
                fail(msg);
            } else {
                done(DONE_COMPLETED);
            }
            return;
        }
        if (st.state == esp32::kTyCancelled) {
            if (st.cancelReason == esp32::kCancelHost)
                done(DONE_STOPPED);
            else
                fail(L"ESP32 输入被设备取消：" +
                     std::wstring(esp32::CancelReasonText(st.cancelReason)));
            return;
        }
        if (st.state == esp32::kTyFailed) {
            std::wstring why = L"ESP32 输入中止：" +
                               std::wstring(esp32::TypingErrorText(st.lastError));
            if (st.cancelReason)
                why += std::wstring(L"（") +
                       esp32::CancelReasonText(st.cancelReason) + L"）";
            fail(why);
            return;
        }
        if (st.state == esp32::kTyIdle) {
            fail(L"ESP32 在输入过程中回到空闲状态，未收到完成/失败状态。");
            return;
        }
        // PREPARING / TYPING: poll again.
    }
}

// ----- start / stop ---------------------------------------------------------
// ----- task lifecycle -------------------------------------------------------
// Forward declarations (the tray/menu handlers below drive the same task).
static void RequestStart(bool viaHotkey);
static void RequestStop();
static void SaveTextOrReport();
static void ShowHotkeyDialog();
static void ShowMainWindow();
static void BeginTyping(bool viaHotkey);
static void BeginEsp32Typing(const std::string& ascii, const std::wstring& port,
                             HWND target);

// Returns true when a new task may begin. Guarded by g_taskLock so two hotkey
// events arriving back-to-back can never both pass.
static bool TryEnterStarting()
{
    std::lock_guard<std::mutex> lk(g_taskLock);
    int s = g_task.load();
    if (s == TS_STARTING || s == TS_TYPING || s == TS_STOPPING) return false;
    if (g_worker.joinable()) return false;   // previous worker not reaped yet
    g_task.store(TS_STARTING);
    return true;
}

static void FailStart(const std::wstring& why)
{
    g_task.store(TS_FAILED);
    if (g_staticStatus) SetWindowTextW(g_staticStatus, why.c_str());
    SetTrayDetail(why);
}

// The foreground window is the intended target. We never show or activate our
// own window here: doing so would take the focus away from the user's target
// and the text would land in the wrong place.
static bool IsUsableTarget(HWND target)
{
    if (!target || !IsWindow(target)) return false;
    if (target == g_hwndMain || IsChild(g_hwndMain, target)) return false;
    // Owned dialogs of our process are also unusable as typing targets.
    DWORD pid = 0;
    GetWindowThreadProcessId(target, &pid);
    if (pid == GetCurrentProcessId()) return false;
    return true;
}

// Shows a tray balloon / notification without taking focus. Used for preflight
// failures so the user learns why nothing was typed while their target window
// keeps the foreground.
static void NotifyNoFocus(const std::wstring& title, const std::wstring& text)
{
    if (!g_trayAdded) return;
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    UpdateTrayIcon();   // restore the normal flag set
}

static void CancelCountdown(const wchar_t* why)
{
    CancelEsp32Countdown();
    KillTimer(g_hwndMain, TIMER_COUNTDOWN);
    g_inCountdown = false;
    g_task.store(TS_IDLE);
    if (g_staticStatus) SetWindowTextW(g_staticStatus, why);
    UpdateTrayIcon();
}

// Runs on the UI thread after the countdown. `viaHotkey` selects the
// no-focus-stealing path: no countdown, no activation, no modal dialog.
static void BeginTyping(bool viaHotkey)
{
    g_inCountdown = false;

    HWND target = GetForegroundWindow();
    if (!IsUsableTarget(target)) {
        FailStart(viaHotkey
            ? L"未开始：前台窗口不可用于输入（当前是本程序窗口或系统界面）。请切到目标窗口后重试。"
            : L"焦点仍在本程序窗口，未开始。请重新开始并在倒计时内把光标放进目标窗口。");
        if (viaHotkey) NotifyNoFocus(L"无法开始输入",
                                     L"当前前台窗口不可用于输入。请切换到目标窗口后按开始快捷键。");
        return;
    }

    int interval = GetWindowTextInt(g_editInterval, 30);
    if (interval < 5) interval = 5;
    if (interval > 2000) interval = 2000;
    bool shiftEnter = Button_GetCheck(g_chkShiftNl) == BST_CHECKED;

    int len = GetWindowTextLengthW(g_editText);
    if (len <= 0) {
        FailStart(L"文本为空，请先在主窗口输入并保存要键入的文本。");
        if (viaHotkey) NotifyNoFocus(L"无法开始输入", L"当前文本为空。请先编辑并保存文本。");
        return;
    }
    std::wstring text((size_t)len + 1, L'\0');
    GetWindowTextW(g_editText, &text[0], len + 1);
    text.resize(wcslen(text.c_str()));

    // Prepare the payload for the SELECTED mode. ESP32 has the device's exact
    // ASCII-only character set and a hard 10 KiB limit, so the whole text is
    // rejected before START (never partially typed).
    std::wstring norm;
    std::string espText;
    if (g_mode == MODE_ESP32) {
        size_t badIndex = 0;
        wchar_t badChar = 0;
        std::wstring why;
        if (!esp32::PrepareText(text, &espText, &why, &badIndex, &badChar)) {
            FailStart(why);
            if (viaHotkey) NotifyNoFocus(L"无法开始 ESP32 输入", why);
            return;
        }
        // Enumerate on every explicit start so a newly plugged device is found.
        RefreshPortCombo();
        if (g_esp32Port.empty()) {
            std::wstring msg = L"未检测到 ESP32 串口。请连接开发板原生 USB 口，"
                               L"待 Windows 枚举出 CDC 串口后点“刷新串口”。";
            FailStart(msg);
            if (viaHotkey) NotifyNoFocus(L"无法开始 ESP32 输入", msg);
            return;
        }
    } else {
        // normalize CRLF / lone CR into a single newline (pure, shared with tests)
        norm = vhid::NormalizeNewlines(text);
    }

    // ESP32 mode diverges here: the countdown/hotkey settle below applies to the
    // legacy HID/software paths. ESP32 validates the port now (no-focus UI on
    // failure), then either starts immediately (hotkey) or stashes the payload
    // for the 3 s button countdown.
    if (g_mode == MODE_ESP32) {
        if (viaHotkey) {
            Sleep(150);
            HWND now = GetForegroundWindow();
            if (now != target || !IsUsableTarget(now)) {
                FailStart(L"未开始：目标窗口在开始前失去了前台焦点。请重新切到目标窗口后按开始快捷键。");
                NotifyNoFocus(L"无法开始输入", L"目标窗口在开始前失去前台焦点。请重试。");
                return;
            }
            BeginEsp32Typing(espText, g_esp32Port, target);
            return;
        }
        // Button path: reuse the countdown timer; the payload rides in statics.
        g_esp32PendingText = espText;
        g_esp32PendingPort = g_esp32Port;
        g_esp32PendingTarget = target;
        g_esp32CountdownArmed = true;
        g_inCountdown = true;
        g_countdownLeft = 3;
        wchar_t st[220];
        swprintf_s(st, L"倒计时 %d 秒（ESP32 USB HID 模式）—— 请立即把光标点击到目标窗口…",
                   g_countdownLeft);
        if (g_staticStatus) SetWindowTextW(g_staticStatus, st);
        SetTimer(g_hwndMain, TIMER_COUNTDOWN, 1000, nullptr);
        return;
    }

    // --- acquire the input channel for the SELECTED mode. The mode is never
    //     silently switched. In HID mode a failure NEVER falls back to
    //     SendInput: the task fails and the reason is reported. ESP32 opens
    //     its port in the worker, not here.
    if (g_mode == MODE_HID) {
        DWORD err = 0;
        HidDevice::Result r = g_device.Open(&err);
        if (r != HidDevice::Result::Ok) {
            std::wstring why;
            if (r == HidDevice::Result::NotFound)
                why = L"无法打开 \\\\.\\TextDeckHidDevice：驱动未安装或设备未创建（错误码 "
                      + std::to_wstring(err) + L"）";
            else if (r == HidDevice::Result::AccessDenied)
                why = L"无法打开 \\\\.\\TextDeckHidDevice：访问被拒绝（错误码 "
                      + std::to_wstring(err) + L"）。请以管理员身份运行本程序。";
            else
                why = L"无法打开 \\\\.\\TextDeckHidDevice（错误码 "
                      + std::to_wstring(err) + L"）";

            FailStart(why);
            // The interactive (button) path keeps the explicit three-way
            // choice; the hotkey path must NOT open a modal dialog because
            // that would steal the target window's focus.
            if (!viaHotkey) {
                wchar_t msg[640];
                swprintf_s(msg,
                    L"%s\n\n是 = 改用【软件输入模式】继续（SendInput，非虚拟 HID）\n"
                    L"否 = 留在 TextDeck HID 模式（解决后重试）\n取消 = 不发送任何字符",
                    why.c_str());
                int mb = MessageBoxW(g_hwndMain, msg, L"虚拟 HID 设备不可用",
                                     MB_YESNOCANCEL | MB_ICONWARNING);
                if (mb == IDYES) {
                    SetMode(MODE_SOFTWARE, true);
                    g_task.store(TS_STARTING);
                    BeginTyping(false);
                } else if (mb == IDNO) {
                    g_task.store(TS_IDLE);
                } else {
                    SetWindowTextW(g_staticStatus, L"已取消（未发送任何字符）。");
                    g_task.store(TS_IDLE);
                }
            } else {
                NotifyNoFocus(L"无法开始输入", why);
            }
            UpdateTrayIcon();
            return;
        }
    }

    // Hotkey path only: let the hotkey's own key-up events and the window
    // manager settle before the first report is submitted. Without this the
    // first one or two characters can be swallowed by the still-in-flight
    // Ctrl/Alt releases (observed: "Round12" arriving as "und12").
    if (viaHotkey) {
        Sleep(150);
        HWND now = GetForegroundWindow();
        if (now != target || !IsUsableTarget(now)) {
            FailStart(L"未开始：目标窗口在开始前失去了前台焦点。请重新切到目标窗口后按开始快捷键。");
            NotifyNoFocus(L"无法开始输入", L"目标窗口在开始前失去前台焦点。请重试。");
            return;
        }
    }

    if (g_worker.joinable()) g_worker.join();
    if (g_stopEvent) CloseHandle(g_stopEvent);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    g_runMode = g_mode;   // freeze the mode of THIS run for status/result text
    g_running = true;
    g_task.store(TS_TYPING);
    if (g_btnStart) EnableWindow(g_btnStart, FALSE);
    if (g_btnStop) EnableWindow(g_btnStop, TRUE);
    if (g_radioHid) EnableWindow(g_radioHid, FALSE);   // mode fixed during a run
    if (g_radioSw) EnableWindow(g_radioSw, FALSE);

    wchar_t st[240];
    swprintf_s(st, L"%s输入中… 目标窗口 0x%p。中止：%s 或托盘“停止输入”。"
               L"切换焦点将自动停止。",
               g_runMode == MODE_SOFTWARE ? L"【软件输入模式】" : L"【TextDeck HID 模式】",
               target, HotkeyText(g_hkStop).c_str());
    if (g_staticStatus) SetWindowTextW(g_staticStatus, st);
    SetTrayDetail(std::wstring(g_runMode == MODE_SOFTWARE
                    ? L"软件输入模式" : L"TextDeck HID 模式") + L" - 正在输入");
    UpdateTrayIcon();

    if (g_runMode == MODE_SOFTWARE) {
        std::vector<vhid::SwEvent> events = vhid::PlanSwEvents(norm, shiftEnter);
        if (events.empty()) {
            g_running = false;
            g_task.store(TS_IDLE);
            if (g_btnStart) EnableWindow(g_btnStart, TRUE);
            if (g_btnStop) EnableWindow(g_btnStop, FALSE);
            if (g_radioHid) EnableWindow(g_radioHid, TRUE);
            if (g_radioSw) EnableWindow(g_radioSw, TRUE);
            SetWindowTextW(g_staticStatus, L"文本为空，未开始。");
            return;
        }
        g_worker = std::thread([ev = std::move(events), interval, target]() {
            SoftwareTypingThread(std::move(ev), interval, target);
        });
    } else {
        g_worker = std::thread([text = std::move(norm), interval, shiftEnter, target]() {
            TypingThread(text, interval, shiftEnter, target);
        });
    }
}

static void BeginTyping(bool viaHotkey);

// ESP32 button path: the button keeps the legacy 3 s countdown so the user can
// click into the target; the hotkey path starts immediately against the live
// foreground window. The worker owns the serial handshake (ping / write /
// START / status poll / CANCEL).
static void BeginEsp32Typing(const std::string& ascii, const std::wstring& port,
                             HWND target)
{
    if (g_worker.joinable()) g_worker.join();
    if (g_stopEvent) CloseHandle(g_stopEvent);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    g_runMode = MODE_ESP32;
    g_running = true;
    g_task.store(TS_TYPING);
    if (g_btnStart) EnableWindow(g_btnStart, FALSE);
    if (g_btnStop) EnableWindow(g_btnStop, TRUE);
    if (g_radioHid) EnableWindow(g_radioHid, FALSE);
    if (g_radioSw) EnableWindow(g_radioSw, FALSE);
    if (g_radioEsp32) EnableWindow(g_radioEsp32, FALSE);
    if (g_comboPort) EnableWindow(g_comboPort, FALSE);
    if (g_btnRefreshPort) EnableWindow(g_btnRefreshPort, FALSE);
    if (g_btnTestPort) EnableWindow(g_btnTestPort, FALSE);

    wchar_t st[256];
    swprintf_s(st, L"【ESP32 USB HID 模式】输入中… 目标窗口 0x%p。中止：%s 或托盘“停止输入”。"
               L"切换焦点将自动停止。",
               target, HotkeyText(g_hkStop).c_str());
    if (g_staticStatus) SetWindowTextW(g_staticStatus, st);
    SetTrayDetail(L"ESP32 USB HID 模式 - 正在输入");
    UpdateTrayIcon();

    g_worker = std::thread([text = ascii, port, target]() {
        Esp32TypingThread(std::move(text), port, target);
    });
}

// Entry point for BOTH the button and the global hotkey.
//  * viaHotkey == true : no countdown, no window activation, no modal dialog.
//    The task starts against the window that is in front right now.
//  * viaHotkey == false: legacy 3-second countdown for the on-screen button.
static void RequestStart(bool viaHotkey)
{
    if (!TryEnterStarting()) {
        // A second start while a task is live must be a no-op, never a restart.
        if (g_staticStatus)
            SetWindowTextW(g_staticStatus, L"已有输入任务在进行，已忽略重复的开始请求。");
        return;
    }
    UpdateTrayIcon();

    if (viaHotkey) {
        BeginTyping(true);
        return;
    }

    g_inCountdown = true;
    g_countdownLeft = 3;
    wchar_t st[220];
    swprintf_s(st, L"倒计时 %d 秒（%s）—— 请立即把光标点击到目标窗口…",
               g_countdownLeft, ModeShortName(g_mode));
    if (g_staticStatus) SetWindowTextW(g_staticStatus, st);
    SetTimer(g_hwndMain, TIMER_COUNTDOWN, 1000, nullptr);
}

// Single stop entry point for the hotkey, the tray menu and the button.
static void RequestStop()
{
    if (g_inCountdown || g_esp32CountdownArmed) {
        CancelCountdown(L"已取消（未发送任何按键）。");
        return;
    }
    int s = g_task.load();
    if ((s == TS_TYPING || s == TS_STARTING) && g_stopEvent) {
        g_task.store(TS_STOPPING);
        if (g_staticStatus)
            SetWindowTextW(g_staticStatus, L"正在停止…（释放所有按键，不再发送后续字符）");
        UpdateTrayIcon();
        // Wake the worker immediately AND interrupt any blocking serial read so
        // CANCEL jumps ahead of the next status poll instead of queueing.
        g_esp32.AbortPendingIo();
        SetEvent(g_stopEvent);
        return;
    }
    if (g_staticStatus) SetWindowTextW(g_staticStatus, L"当前没有进行中的输入任务。");
}

static void OnStart() { RequestStart(false); }
static void OnStop()  { RequestStop(); }

// Persist the editor text and report the outcome without stealing focus.
static void SaveTextOrReport()
{
    std::wstring err;
    if (SaveTextToDisk(&err)) {
        if (g_staticStatus)
            SetWindowTextW(g_staticStatus, L"文本已保存，可用于全局快捷键输入。");
        SetTrayDetail(L"文本已保存");
    } else {
        if (g_staticStatus) SetWindowTextW(g_staticStatus, err.c_str());
        SetTrayDetail(err);
    }
    UpdateTrayIcon();
}

// ----- tray menu ------------------------------------------------------------
// Brings the main window back. Only ever called for an EXPLICIT user request
// (tray double-click / menu item) - never on a hotkey start, because that would
// pull the foreground away from the user's typing target.
static void ShowMainWindow()
{
    if (!g_hwndMain) return;
    ShowWindow(g_hwndMain, SW_SHOW);
    ShowWindow(g_hwndMain, SW_RESTORE);
    SetForegroundWindow(g_hwndMain);
}

static void HideMainWindowToTray()
{
    if (!g_hwndMain) return;
    ShowWindow(g_hwndMain, SW_HIDE);
    if (!g_trayAdded) AddTrayIcon(g_hwndMain);
    SetTrayDetail(L"已隐藏到托盘 - 双击图标打开");
}

// Builds and shows the tray context menu. The Stop entry is disabled while no
// task is running, so the menu never offers a no-op action.
static void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    int s = g_task.load();
    bool running = (s == TS_TYPING || s == TS_STARTING || s == TS_STOPPING);

    AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, L"打开 / 编辑文本(&O)...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (running ? MF_GRAYED : 0u), IDM_TRAY_START,
                L"开始输入(&S)");
    AppendMenuW(menu, MF_STRING | (running ? 0u : MF_GRAYED), IDM_TRAY_STOP,
                L"停止输入(&T)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_HOTKEYS, L"快捷键设置(&K)...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出程序(&X)");

    SetMenuDefaultItem(menu, IDM_TRAY_OPEN, FALSE);
    // Required so the menu closes correctly when the user clicks elsewhere.
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// Central tray callback: everything here runs on the UI thread, so the message
// pump stays responsive while the worker types (the worker only posts progress
// and completion messages).
static void OnTrayMessage(LPARAM lParam)
{
    switch (LOWORD(lParam)) {
    case WM_LBUTTONDBLCLK:
        ShowMainWindow();
        break;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        ShowTrayMenu(g_hwndMain);
        break;
    default:
        break;
    }
}

// Handles the tray menu commands.
static void OnTrayCommand(int id)
{
    switch (id) {
    case IDM_TRAY_OPEN:
        ShowMainWindow();
        break;
    case IDM_TRAY_START:
        RequestStart(true);   // same focus-preserving path as the global hotkey
        break;
    case IDM_TRAY_STOP:
        RequestStop();
        break;
    case IDM_TRAY_HOTKEYS:
        ShowMainWindow();
        ShowHotkeyDialog();
        break;
    case IDM_TRAY_EXIT:
        // Full shutdown: stop any task (releasing keys), persist the text,
        // unregister the hotkeys and remove the tray icon. The DRIVER is
        // deliberately left installed - quitting the app never touches it.
        CancelEsp32Countdown();
        if (g_running && g_stopEvent) {
            g_esp32.AbortPendingIo();
            SetEvent(g_stopEvent);
        }
        if (g_worker.joinable()) g_worker.join();
        if (g_device.IsOpen()) { DWORD e = 0; g_device.SendReleaseAll(&e); }
        g_device.Close();
        // Best effort: tell a live ESP32 to release keys before we exit. The
        // device also self-releases on host-lease timeout if we are gone.
        if (g_esp32.IsOpen()) { std::wstring dummy; g_esp32.CancelTyping(nullptr, &dummy); }
        g_esp32.Close();
        SaveTextToDisk(nullptr);
        // Exiting while the window is still VISIBLE must persist settings and
        // window geometry too - not only the hide-to-tray WM_CLOSE path.
        SaveSettings();
        SaveWindowState(g_hwndMain);
        UnregisterAllHotkeys(g_hwndMain);
        RemoveTrayIcon();
        DestroyWindow(g_hwndMain);
        break;
    default:
        break;
    }
}

// ----- hotkey settings dialog ----------------------------------------------
// A small modal dialog that captures an actual key chord (not a text field), so
// the user can never enter a combination Windows cannot represent. The dialog
// only reports what was registered successfully.
struct HotkeyDlgState {
    HotkeySpec start;
    HotkeySpec stop;
    int        captureTarget;   // 0 = none, 1 = start, 2 = stop
    HHOOK      kbHook;
    HWND       hwnd;
    bool       done;            // ends the modal pump (set by the dialog wndproc)
    WNDPROC    origProc;        // #32770's original proc (subclass fallback)
};

static HotkeyDlgState g_hkDlg = {};

// Modifier state tracked BY THE HOOK ITSELF. The hook swallows modifier
// keydowns (return 1), which keeps them out of the thread's message queue AND
// out of the system's async key state - both GetKeyState and even
// GetAsyncKeyState read "up" for them, so every captured chord came out bare.
// The hook DOES see those events (that is how it can swallow them), so it
// records them here and clears them on the matching keyup. Reset when the
// dialog opens so a modifier held across sessions cannot leak in.
static DWORD g_hkModsTracked = 0;

static DWORD ModBitForVk(UINT vk)
{
    switch (vk) {
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return MOD_CONTROL;
    case VK_MENU:    case VK_LMENU:    case VK_RMENU:    return MOD_ALT;
    case VK_SHIFT:   case VK_LSHIFT:   case VK_RSHIFT:   return MOD_SHIFT;
    case VK_LWIN:    case VK_RWIN:                        return MOD_WIN;
    default:                                              return 0;
    }
}

static LRESULT CALLBACK HotkeyKbHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION &&
        (wParam == WM_KEYDOWN   || wParam == WM_SYSKEYDOWN ||
         wParam == WM_KEYUP     || wParam == WM_SYSKEYUP)) {
        KBDLLHOOKSTRUCT* k = (KBDLLHOOKSTRUCT*)lParam;
        UINT vk = (UINT)k->vkCode;
        DWORD bit = ModBitForVk(vk);
        if (bit) {
            if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                g_hkModsTracked &= ~bit;   // matching release (keyups pass on)
            } else {
                g_hkModsTracked |= bit;    // record, THEN swallow the press
                return 1;   // swallow pure modifier presses (pre-existing)
            }
        } else if (wParam != WM_KEYUP && wParam != WM_SYSKEYUP &&
                   g_hkDlg.captureTarget != 0) {
            UINT mods = g_hkModsTracked;
            // Async state as a fallback for modifiers that were already held
            // before the hook was armed; for everything this hook swallowed
            // the tracked bits above are the only source that stays correct.
            if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
            if (GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
            if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;
            if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))
                mods |= MOD_WIN;

            HotkeySpec spec = { mods, vk };
            HWND edit = GetDlgItem(g_hkDlg.hwnd,
                          g_hkDlg.captureTarget == 1 ? IDC_HK_START : IDC_HK_STOP);
            if (edit) SetWindowTextW(edit, HotkeyText(spec).c_str());
            if (g_hkDlg.captureTarget == 1) g_hkDlg.start = spec;
            else                            g_hkDlg.stop  = spec;
            g_hkDlg.captureTarget = 0;
            return 1;   // consumed: the chord must not reach other apps
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// ----- hotkey dialog command handlers --------------------------------------
// These run from HotkeyDlgWndProc, the ONLY place that sees every delivery
// mode: a real button click sends WM_COMMAND straight to the window
// procedure, automation may post it (the modal pump dispatches it here), and
// Enter/Esc arrive as IDOK/IDCANCEL. Handling commands only in the pump's
// message filter silently dropped the sent ones - the buttons looked dead
// and CloseHotkeyDialog could never cancel the dialog.

static void HotkeyDlgOnOk(HWND dlg)
{
    if (SameHotkey(g_hkDlg.start, g_hkDlg.stop)) {
        MessageBoxW(dlg, L"开始和停止不能使用相同的快捷键组合。",
                    L"快捷键冲突", MB_OK | MB_ICONWARNING);
        return;
    }
    // Two-phase switch: the candidate pair is registered under the INACTIVE
    // id pair first; on failure the previous bindings were never released,
    // so nothing has to be rolled back and the user keeps full control.
    // NOTE: the ok flags keep describing the LIVE pair - a refused change
    // rolls back to them - so the refusal MUST be read from the return
    // value. Checking the flags here silently swallowed every conflict
    // The dialog re-saved the unchanged pair and closed
    // with no conflict box and nothing committed.
    if (!ApplyHotkeys(g_hwndMain, g_hkDlg.start, g_hkDlg.stop)) {
        DWORD e = g_hkStartErr ? g_hkStartErr : g_hkStopErr;
        wchar_t m[560];
        if (e == ERROR_HOTKEY_ALREADY_REGISTERED) {
            swprintf_s(m,
                L"该组合已被其他程序占用（错误码 %lu："
                L"全局热键已被注册）。\n\n"
                L"原来的快捷键保持有效，未做任何更改。"
                L"请换一个组合再试。", e);
        } else {
            swprintf_s(m,
                L"快捷键注册失败（错误码 %lu）。\n\n"
                L"原来的快捷键保持有效，未做任何更改。"
                L"请换一个组合再试。", e);
        }
        MessageBoxW(dlg, m, L"快捷键注册失败", MB_OK | MB_ICONWARNING);
        return;
    }
    if (!SaveHotkeySettings()) {
        MessageBoxW(dlg,
            L"快捷键已生效并可立即使用，但写入配置文件失败：\n"
            L"重启应用后会恢复为之前的快捷键设置。\n\n"
            L"请检查磁盘空间与权限，然后在本窗口重新保存一次。",
            L"保存失败", MB_OK | MB_ICONWARNING);
    }
    // Every surface that shows a binding is derived from the
    // registered values, so they can never disagree.
    RefreshActionLabels();
    SetMode(g_mode, true);   // title also carries the bindings
    g_hkDlg.done = true;
}

static void HotkeyDlgOnDefault(HWND dlg)
{
    // Restore the built-in bindings into the dialog fields;
    // nothing is committed until 确定, so cancelling still
    // leaves the current bindings untouched.
    g_hkDlg.start = kDefaultHkStart;
    g_hkDlg.stop  = kDefaultHkStop;
    SetWindowTextW(GetDlgItem(dlg, IDC_HK_START),
                   HotkeyText(g_hkDlg.start).c_str());
    SetWindowTextW(GetDlgItem(dlg, IDC_HK_STOP),
                   HotkeyText(g_hkDlg.stop).c_str());
}

static LRESULT CALLBACK HotkeyDlgWndProc(HWND dlg, UINT msg,
                                          WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_COMMAND) {
        int id  = LOWORD(wParam);
        int evt = HIWORD(wParam);
        // Arm capture on a real user click (the read-only edit reports
        // EN_SETFOCUS when focused) or on an explicit command (BN_CLICKED =
        // 0: button press, automation, Enter-on-default). EN_CHANGE (0x300)
        // fired by our own SetWindowText below must NOT re-arm, hence the
        // explicit event gate - edit notifications are >= 0x100.
        if (id == IDC_HK_START && (evt == BN_CLICKED || evt == EN_SETFOCUS)) {
            g_hkDlg.captureTarget = 1;
            return 0;
        }
        if (id == IDC_HK_STOP && (evt == BN_CLICKED || evt == EN_SETFOCUS)) {
            g_hkDlg.captureTarget = 2;
            return 0;
        }
        if (evt == BN_CLICKED) {
            if (id == IDC_HK_OK || id == IDOK) { HotkeyDlgOnOk(dlg); return 0; }
            if (id == IDC_HK_DEFAULT) { HotkeyDlgOnDefault(dlg); return 0; }
            if (id == IDC_HK_CANCEL || id == IDCANCEL) {
                g_hkDlg.done = true;
                return 0;
            }
        }
        // Anything else (EN_CHANGE, paints, focus, unknown ids) falls
        // through to the original dialog procedure.
    }
    if (msg == WM_CLOSE)   { g_hkDlg.done = true; return 0; }  // X button
    if (msg == WM_DESTROY) { g_hkDlg.done = true; return 0; }  // app exit / owner teardown
    return CallWindowProcW(g_hkDlg.origProc, dlg, msg, wParam, lParam);
}

static void ShowHotkeyDialog()
{
    if (g_hotkeyDlgOpen) return;      // never stack two dialogs
    g_hotkeyDlgOpen = true;
    g_hkDlg.start = g_hkStart;
    g_hkDlg.stop  = g_hkStop;

    // Build the dialog from a template at runtime (no .rc file needed).
    struct { DLGTEMPLATE tmpl; WORD menu, cls, title; } tpl = {};
    tpl.tmpl.style = DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    tpl.tmpl.dwExtendedStyle = 0;
    tpl.tmpl.cdit = 0;
    tpl.tmpl.x = 100; tpl.tmpl.y = 100; tpl.tmpl.cx = 300; tpl.tmpl.cy = 150;

    // A tiny runtime dialog template with 5 children would need byte layout
    // work; a plain owned window is simpler and behaves the same for our use.
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        L"#32770", L"快捷键设置",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 580, 300,
        g_hwndMain, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dlg) { g_hotkeyDlgOpen = false; return; }

    // Subclass: #32770 has no dialog procedure of ours, so sent WM_COMMANDs
    // (real button clicks, Enter/Esc) landed directly in DefDlgProc and were
    // dropped - while the pump filter only ever saw POSTED commands. Routing
    // everything through HotkeyDlgWndProc gives commands exactly one path.
    g_hkDlg.origProc = (WNDPROC)SetWindowLongPtrW(dlg, GWLP_WNDPROC,
                                                   (LONG_PTR)HotkeyDlgWndProc);

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                  int x, int y, int w, int h, int id) {
        HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style,
                               x, y, w, h, dlg, (HMENU)(INT_PTR)id, nullptr, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    mk(L"STATIC", L"开始输入:", 0, 16, 20, 90, 22, -1);
    HWND eStart = mk(L"EDIT", L"", WS_BORDER | ES_READONLY, 112, 18, 380, 24, IDC_HK_START);
    mk(L"STATIC", L"紧急停止:", 0, 16, 56, 90, 22, -1);
    HWND eStop  = mk(L"EDIT", L"", WS_BORDER | ES_READONLY, 112, 54, 380, 24, IDC_HK_STOP);
    mk(L"STATIC",
       L"点击输入框后直接按组合键（需含 Ctrl/Alt/Shift/Win 之一）。\r\n"
       L"开始与停止不能相同；组合被其他程序占用时会提示并保留原设置。",
       0, 16, 96, 540, 44, IDC_HK_INFO);
    HWND ok = mk(L"BUTTON", L"确定", BS_DEFPUSHBUTTON, 300, 220, 90, 32, IDC_HK_OK);
    HWND cancel = mk(L"BUTTON", L"取消", 0, 400, 220, 90, 32, IDC_HK_CANCEL);
    HWND deflt = mk(L"BUTTON", L"恢复默认", 0, 16, 220, 100, 32, IDC_HK_DEFAULT);
    (void)ok; (void)cancel; (void)deflt;

    SetWindowTextW(eStart, HotkeyText(g_hkDlg.start).c_str());
    SetWindowTextW(eStop,  HotkeyText(g_hkDlg.stop).c_str());

    g_hkDlg.hwnd = dlg;
    g_hkDlg.captureTarget = 0;
    g_hkDlg.done = false;
    g_hkModsTracked = 0;   // no stale modifiers from a previous dialog session
    g_hkDlg.kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, HotkeyKbHook,
                                       GetModuleHandleW(nullptr), 0);

    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);

    // Modal loop: the main window keeps processing its own messages (the tray
    // and the typing worker stay alive while this dialog is open). Commands
    // go through HotkeyDlgWndProc (subclass above): posted WM_COMMANDs are
    // dispatched to it by this loop, sent ones (button clicks, Enter/Esc)
    // arrive directly - one code path for every delivery mode.
    MSG dmsg = {};
    BOOL gr = 1;
    while (!g_hkDlg.done && (gr = GetMessageW(&dmsg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&dmsg);
        DispatchMessageW(&dmsg);
    }
    // GetMessage()==0 means WM_QUIT. A nested loop MUST hand the quit back:
    // consuming it here would leave the outer wWinMain loop waiting forever
    // - a zombie process that still holds the single-instance mutex, turning
    // every later launch into a silent no-op second instance.
    if (gr == 0) PostQuitMessage((int)dmsg.wParam);

    if (g_hkDlg.kbHook) { UnhookWindowsHookEx(g_hkDlg.kbHook); g_hkDlg.kbHook = nullptr; }
    DestroyWindow(dlg);
    g_hkDlg.hwnd = nullptr;
    g_hkDlg.origProc = nullptr;
    g_hotkeyDlgOpen = false;
}

// ----- responsive layout ----------------------------------------------------
// Every control is positioned from the CLIENT RECT in WM_SIZE, so the window can
// be resized freely and the text editor (the whole point of the app) absorbs
// the extra space. The old build placed controls once at fixed pixel offsets and
// had no WM_SIZE at all, so the editor stayed 496x186 no matter how big the
// window was.
//
// DPI: all constants below are LOGICAL units and are multiplied by the window's
// current DPI scale at layout time, so the same code is correct at 100%, 125%,
// 150% and 200%.

static UINT g_dpi = 96;

static int Dpi(int logical)
{
    return MulDiv(logical, (int)g_dpi, 96);
}

// Updates g_dpi from the window's monitor and re-lays out if it changed.
static void RefreshDpi(HWND hwnd)
{
    UINT dpi = 96;
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    typedef UINT (WINAPI *GetDpiForSystemFn)(void);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        GetDpiForWindowFn fn = (GetDpiForWindowFn)(void*)GetProcAddress(user32, "GetDpiForWindow");
        if (fn) {
            dpi = fn(hwnd);
        } else {
            GetDpiForSystemFn sys = (GetDpiForSystemFn)(void*)GetProcAddress(user32, "GetDpiForSystem");
            if (sys) dpi = sys();
        }
    }
    if (dpi == 0) dpi = 96;
    g_dpi = dpi;
}

// Minimum client size that keeps the control rows from overlapping.
// Seven stacked rows now (editor + options + port + mode + buttons + progress
// + hotkey + status), so the floor grows past the old two-radio layout.
static void GetMinClientSize(int* w, int* h)
{
    *w = Dpi(620);
    *h = Dpi(500);
}

// Preferred first-run size: large enough to read a real paragraph, but clamped
// to the work area of the monitor the window is on.
static void GetPreferredWindowSize(HWND hwnd, int* w, int* h)
{
    int cw = Dpi(1100);
    int ch = Dpi(800);

    // Convert the desired CLIENT size into a WINDOW size for this style.
    RECT rc = { 0, 0, cw, ch };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
    int ww = rc.right - rc.left;
    int wh = rc.bottom - rc.top;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        int availW = (mi.rcWork.right - mi.rcWork.left);
        int availH = (mi.rcWork.bottom - mi.rcWork.top);
        // Never exceed ~92% of the work area: leaves room for the taskbar and
        // keeps the window fully on screen on small displays.
        int maxW = availW - Dpi(40);
        int maxH = availH - Dpi(40);
        if (maxW > Dpi(480) && ww > maxW) ww = maxW;
        if (maxH > Dpi(360) && wh > maxH) wh = maxH;
    }
    *w = ww;
    *h = wh;
}

// Clamps a saved window rect so a window from a disconnected monitor (or a
// resolution change) still lands fully inside a visible work area.
static bool RectOnAnyMonitor(const RECT* r)
{
    if (r->right - r->left < Dpi(200) || r->bottom - r->top < Dpi(150)) return false;
    HMONITOR mon = MonitorFromRect(r, MONITOR_DEFAULTTONULL);
    if (!mon) return false;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    RECT inter = {};
    if (!IntersectRect(&inter, r, &mi.rcWork)) return false;
    // require a decent portion to be visible, not just 1 pixel
    return (inter.right - inter.left) >= Dpi(200) &&
           (inter.bottom - inter.top) >= Dpi(150);
}

static void LayoutControls(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    const int pad      = Dpi(14);   // outer margin
    const int gap      = Dpi(8);    // vertical gap between rows
    const int rowH     = Dpi(26);   // standard control height
    const int btnH     = Dpi(32);   // primary button height
    const int statusH  = Dpi(52);   // multi-line status block

    // Bottom-up: status (2 lines) + hotkey row + progress, then the controls,
    // then the ESP32 serial row + mode row, then the editor takes the rest.
    int yStatus  = H - pad - statusH;
    int yHotkey  = yStatus - gap - Dpi(30);
    int yProgress= yHotkey - gap - Dpi(20);
    int yButtons = yProgress - gap - btnH;
    int yMode    = yButtons - gap - Dpi(24);
    int yPort    = yMode - gap - Dpi(24);
    int yOptions = yPort - gap - Dpi(24);
    int yEditor  = pad;
    int editorH  = yOptions - gap - yEditor;
    if (editorH < Dpi(80)) editorH = Dpi(80);   // defensive: never negative

    // --- editor: full width, all remaining vertical space ---
    MoveWindow(g_editText, pad, yEditor, W - 2 * pad, editorH, TRUE);

    // --- options row: interval + shift-enter checkbox ---
    int optW = W - 2 * pad;
    int lblW = Dpi(72);
    int ivlW = Dpi(64);
    HWND hLbl = GetDlgItem(hwnd, IDC_STATIC_INTERVAL);
    if (hLbl) MoveWindow(hLbl, pad, yOptions + Dpi(3), lblW, rowH, TRUE);
    MoveWindow(g_editInterval, pad + lblW, yOptions, ivlW, rowH, TRUE);
    MoveWindow(g_chkShiftNl, pad + lblW + ivlW + Dpi(14), yOptions,
               optW - lblW - ivlW - Dpi(14), rowH, TRUE);

    // --- mode radios: three across (legacy HID / software / ESP32) ---
    int third = (W - 2 * pad - 2 * Dpi(10)) / 3;
    MoveWindow(g_radioHid, pad, yMode, third, Dpi(24), TRUE);
    MoveWindow(g_radioSw, pad + third + Dpi(10), yMode, third, Dpi(24), TRUE);
    MoveWindow(g_radioEsp32, pad + 2 * (third + Dpi(10)), yMode, third, Dpi(24), TRUE);

    // --- ESP32 serial row: label + combo + refresh + test ---
    int portLblW = Dpi(72);
    int portBtnW = Dpi(88);
    int comboW = W - 2 * pad - portLblW - 2 * portBtnW - 3 * Dpi(8);
    if (comboW < Dpi(120)) comboW = Dpi(120);
    MoveWindow(g_staticPort, pad, yPort + Dpi(3), portLblW, rowH, TRUE);
    MoveWindow(g_comboPort, pad + portLblW + Dpi(4), yPort, comboW, Dpi(200), TRUE);
    MoveWindow(g_btnRefreshPort, pad + portLblW + Dpi(4) + comboW + Dpi(8), yPort,
               portBtnW, rowH, TRUE);
    MoveWindow(g_btnTestPort, pad + portLblW + Dpi(4) + comboW + Dpi(8) + portBtnW + Dpi(8),
               yPort, portBtnW, rowH, TRUE);

    // --- primary actions: 开始 / 停止 / 保存, equal thirds ---
    int bw = (W - 2 * pad - 2 * Dpi(10)) / 3;
    MoveWindow(g_btnStart, pad, yButtons, bw, btnH, TRUE);
    MoveWindow(g_btnStop, pad + bw + Dpi(10), yButtons, bw, btnH, TRUE);
    MoveWindow(g_btnSave, pad + 2 * (bw + Dpi(10)), yButtons, bw, btnH, TRUE);

    // --- progress line ---
    MoveWindow(g_staticProg, pad, yProgress, W - 2 * pad, Dpi(20), TRUE);

    // --- hotkey row: settings button + current bindings ---
    int hkW = Dpi(150);
    MoveWindow(g_btnHotkeys, pad, yHotkey, hkW, Dpi(30), TRUE);
    MoveWindow(g_staticHotkeys, pad + hkW + Dpi(12), yHotkey + Dpi(6),
               W - 2 * pad - hkW - Dpi(12), Dpi(20), TRUE);

    // --- status block ---
    MoveWindow(g_staticStatus, pad, yStatus, W - 2 * pad, statusH, TRUE);
}

// ----- labels that follow the real bindings --------------------------------
// The Start/Stop buttons and the hotkey line must never disagree with what is
// actually registered. Everything is derived from g_hkStart / g_hkStop, so a
// successful rebind updates every surface at once.
static void RefreshActionLabels()
{
    std::wstring st = L"开始 (" + HotkeyText(g_hkStart) + L")";
    std::wstring sp = L"停止 (" + HotkeyText(g_hkStop) + L")";
    if (g_btnStart) SetWindowTextW(g_btnStart, st.c_str());
    if (g_btnStop)  SetWindowTextW(g_btnStop, sp.c_str());
    if (g_staticHotkeys) SetWindowTextW(g_staticHotkeys, HotkeyStatusText().c_str());
}

// ----- window geometry persistence -----------------------------------------
// The last NORMAL (non-maximised) rect is stored in settings.ini together with
// a maximised flag. Anything unreadable, out of range, or off-screen falls back
// to the default size instead of preventing startup.
struct WindowState {
    RECT normal;          // normal (restored) rect in screen coords
    bool maximized;
    bool valid;           // a saved rect was loaded
};

static WindowState g_winState = {};

static void SaveWindowState(HWND hwnd)
{
    if (!hwnd) return;
    std::wstring dir = SettingsDir();
    std::wstring ini = dir + L"\\settings.ini";

    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) return;

    // rcNormalPosition is the restored rect regardless of current state.
    RECT r = wp.rcNormalPosition;
    wchar_t buf[128];
    swprintf_s(buf, L"%ld,%ld,%ld,%ld", r.left, r.top,
               r.right - r.left, r.bottom - r.top);
    WritePrivateProfileStringW(L"Window", L"Rect", buf, ini.c_str());
    WritePrivateProfileStringW(L"Window", L"Maximized",
        (wp.showCmd == SW_SHOWMAXIMIZED) ? L"1" : L"0", ini.c_str());
}

static void LoadWindowState()
{
    g_winState.valid = false;
    g_winState.maximized = false;
    ZeroMemory(&g_winState.normal, sizeof(g_winState.normal));

    std::wstring dir = SettingsDir();
    std::wstring ini = dir + L"\\settings.ini";
    wchar_t buf[128] = L"";
    GetPrivateProfileStringW(L"Window", L"Rect", L"", buf, 128, ini.c_str());
    if (!buf[0]) return;

    long l = 0, t = 0, w = 0, h = 0;
    if (swscanf_s(buf, L"%ld,%ld,%ld,%ld", &l, &t, &w, &h) != 4) return;
    // Reject nonsense early: a corrupt ini must never break startup.
    if (w < 200 || h < 150 || w > 20000 || h > 20000) return;
    if (l < -30000 || t < -30000 || l > 30000 || t > 30000) return;

    RECT r = { l, t, l + w, t + h };
    if (!RectOnAnyMonitor(&r)) return;   // monitor gone / resolution changed

    g_winState.normal = r;
    g_winState.valid = true;
    g_winState.maximized =
        GetPrivateProfileIntW(L"Window", L"Maximized", 0, ini.c_str()) != 0;
}

// ----- window ---------------------------------------------------------------
static void CreateControls(HWND hwnd)
{
    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    // Controls are created with placeholder geometry; LayoutControls() below
    // (and on every WM_SIZE) computes the real positions from the client rect.
    g_editText = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        // Word wrap is implicit for a multiline edit WITHOUT ES_AUTOHSCROLL;
        // WS_HSCROLL is deliberately absent (it would be ignored anyway).
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
        ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_EDIT_TEXT, nullptr, nullptr);

    CreateWindowW(L"STATIC", L"间隔(ms):",
        WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd,
        (HMENU)(INT_PTR)IDC_STATIC_INTERVAL, nullptr, nullptr);
    g_editInterval = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"30",
        WS_CHILD | WS_VISIBLE | ES_NUMBER,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_EDIT_INTERVAL, nullptr, nullptr);

    g_chkShiftNl = CreateWindowW(L"BUTTON", L"换行发送 Shift+Enter（防误发送）",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_CHK_SHIFT_NL, nullptr, nullptr);

    // explicit input-mode selector: never switched silently; ESP32 and software
    // modes start and work with NO driver installed
    g_radioHid = CreateWindowW(L"BUTTON", L"TextDeck HID 模式（需已装驱动）",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_RADIO_HID, nullptr, nullptr);
    g_radioSw = CreateWindowW(L"BUTTON", L"软件输入模式（SendInput，无需驱动）",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_RADIO_SW, nullptr, nullptr);
    g_radioEsp32 = CreateWindowW(L"BUTTON", L"ESP32 USB HID 模式（原生 USB 单线，无需驱动）",
        WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_RADIO_ESP32, nullptr, nullptr);

    g_staticPort = CreateWindowW(L"STATIC", L"ESP32 串口:",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_STATIC_PORT, nullptr, nullptr);
    g_comboPort = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_COMBO_PORT, nullptr, nullptr);
    g_btnRefreshPort = CreateWindowW(L"BUTTON", L"刷新串口",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_BTN_REFRESH, nullptr, nullptr);
    g_btnTestPort = CreateWindowW(L"BUTTON", L"测试连接",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_BTN_TEST, nullptr, nullptr);

    // Button captions are filled in by RefreshActionLabels() so they always
    // show the ACTUAL registered hotkeys.
    g_btnStart = CreateWindowW(L"BUTTON", L"开始",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_BTN_START, nullptr, nullptr);
    g_btnStop = CreateWindowW(L"BUTTON", L"停止",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_BTN_STOP, nullptr, nullptr);
    EnableWindow(g_btnStop, FALSE);

    g_btnSave = CreateWindowW(L"BUTTON", L"保存文本 (Ctrl+S)",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_BTN_SAVE, nullptr, nullptr);

    g_staticProg = CreateWindowW(L"STATIC", L"进度: -",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_STATIC_PROG, nullptr, nullptr);

    g_btnHotkeys = CreateWindowW(L"BUTTON", L"快捷键设置…",
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_BTN_HOTKEYS, nullptr, nullptr);

    g_staticHotkeys = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_STATIC_HK, nullptr, nullptr);

    g_staticStatus = CreateWindowW(L"STATIC", ModeStatusText(),
        WS_CHILD | WS_VISIBLE,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)IDC_STATIC_STATUS, nullptr, nullptr);

    for (HWND c : {g_editText, g_editInterval, g_chkShiftNl, g_radioHid, g_radioSw,
                   g_radioEsp32, g_staticPort,
                   g_btnStart, g_btnStop, g_btnSave, g_btnHotkeys,
                   g_staticProg, g_staticStatus, g_staticHotkeys}) {
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    }
    if (g_comboPort) SendMessageW(g_comboPort, WM_SETFONT, (WPARAM)font, TRUE);
    if (g_btnRefreshPort) SendMessageW(g_btnRefreshPort, WM_SETFONT, (WPARAM)font, TRUE);
    if (g_btnTestPort) SendMessageW(g_btnTestPort, WM_SETFONT, (WPARAM)font, TRUE);
    HWND lblInterval = GetDlgItem(hwnd, IDC_STATIC_INTERVAL);
    if (lblInterval) SendMessageW(lblInterval, WM_SETFONT, (WPARAM)font, TRUE);

    // Generous limit: the editor is meant for real paragraphs, not one-liners.
    SendMessageW(g_editText, EM_LIMITTEXT, 1000000, 0);
    // Word wrap ON (no ES_AUTOHSCROLL) so long lines stay readable; the
    // vertical scrollbar handles long documents.

    RefreshDpi(hwnd);
    LayoutControls(hwnd);
    RefreshActionLabels();

    // reflect the REAL registration result, never an optimistic default
    SetWindowTextW(g_staticHotkeys, HotkeyStatusText().c_str());
}
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        g_hwndMain = hwnd;
        CreateControls(hwnd);
        return 0;

    case WM_SIZE:
        // Re-layout on every resize: the editor absorbs the extra space and
        // the button/status rows stay pinned to the bottom without overlap.
        if (wParam != SIZE_MINIMIZED) LayoutControls(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        // Refuse to shrink below the size where the control rows would
        // collide (the editor has a floor of its own).
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        int cw = 0, ch = 0;
        GetMinClientSize(&cw, &ch);
        RECT rc = { 0, 0, cw, ch };
        AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
        mmi->ptMinTrackSize.x = rc.right - rc.left;
        mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }

    case WM_DPICHANGED: {
        // The user dragged the window to a monitor with a different scale:
        // adopt the suggested rect and re-layout with the new DPI.
        g_dpi = HIWORD(wParam);
        RECT* suggested = (RECT*)lParam;
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutControls(hwnd);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_START && HIWORD(wParam) == BN_CLICKED) OnStart();
        if (LOWORD(wParam) == IDC_BTN_STOP && HIWORD(wParam) == BN_CLICKED) OnStop();
        if (LOWORD(wParam) == IDC_BTN_SAVE && HIWORD(wParam) == BN_CLICKED) SaveTextOrReport();
        if (LOWORD(wParam) == IDC_BTN_HOTKEYS && HIWORD(wParam) == BN_CLICKED) ShowHotkeyDialog();
        // Tray context-menu commands arrive as WM_COMMAND with the menu id.
        if (LOWORD(wParam) >= IDM_TRAY_OPEN && LOWORD(wParam) <= IDM_TRAY_EXIT)
            OnTrayCommand(LOWORD(wParam));
        if (LOWORD(wParam) == IDC_RADIO_HID && HIWORD(wParam) == BN_CLICKED && !g_running)
            SetMode(MODE_HID, true);
        if (LOWORD(wParam) == IDC_RADIO_SW && HIWORD(wParam) == BN_CLICKED && !g_running)
            SetMode(MODE_SOFTWARE, true);
        if (LOWORD(wParam) == IDC_RADIO_ESP32 && HIWORD(wParam) == BN_CLICKED && !g_running) {
            SetMode(MODE_ESP32, true);
            RefreshPortCombo();
        }
        if (LOWORD(wParam) == IDC_BTN_REFRESH && HIWORD(wParam) == BN_CLICKED && !g_running)
            RefreshPortCombo();
        if (LOWORD(wParam) == IDC_BTN_TEST && HIWORD(wParam) == BN_CLICKED && !g_running)
            TestEsp32Connection();
        if (LOWORD(wParam) == IDC_COMBO_PORT && HIWORD(wParam) == CBN_SELCHANGE && !g_running) {
            g_esp32Port = SelectedPortFromCombo();
            SavePortSetting();
            if (g_mode == MODE_ESP32 && g_staticStatus)
                SetWindowTextW(g_staticStatus, ModeStatusText());
        }
        return 0;

    case WM_HOTKEY:
        // The global start hotkey must NOT show or activate this window: the
        // foreground window at this instant is the user typing target. At any
        // moment only ONE of each id pair is registered (the switching
        // scheme), so both ids can safely map to the same action.
        if (wParam == ID_HOTKEY_START || wParam == ID_HOTKEY_TMP_START)
            RequestStart(true);
        if (wParam == ID_HOTKEY_STOP || wParam == ID_HOTKEY_TMP_STOP)
            RequestStop();
        return 0;

    case WM_APP_SHOWME:
        ShowMainWindow();
        return 0;

    case WM_APP_TRAY:
        OnTrayMessage(lParam);
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_COUNTDOWN) {
            --g_countdownLeft;
            if (g_countdownLeft > 0) {
                wchar_t st[128];
                swprintf_s(st, L"倒计时 %d 秒 —— 请把光标点击到目标窗口…", g_countdownLeft);
                SetWindowTextW(g_staticStatus, st);
            } else {
                KillTimer(hwnd, TIMER_COUNTDOWN);
                if (g_esp32CountdownArmed) {
                    g_esp32CountdownArmed = false;
                    // The countdown target is whatever window is in front NOW,
                    // not the window captured before the countdown.
                    HWND target = GetForegroundWindow();
                    if (!IsUsableTarget(target)) {
                        FailStart(L"焦点仍在本程序窗口，未开始。请重新开始并在倒计时内把光标放进目标窗口。");
                    } else {
                        BeginEsp32Typing(g_esp32PendingText, g_esp32PendingPort, target);
                    }
                } else {
                    BeginTyping(false);   // button path (countdown finished)
                }
            }
            return 0;
        }
        if (wParam == TIMER_SMOKE) {
            AppendSmokeLog(L"smoke: closing");
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;

    case WM_APP_STATUS:
        if (lParam) {
            SetWindowTextW(g_staticStatus, (const wchar_t*)lParam);
            free((void*)lParam);
        }
        return 0;

    case WM_APP_PROGRESS: {
        long skipped = g_skipped.load();
        wchar_t buf[128];
        swprintf_s(buf, L"进度: %u / %u 字符   跳过不支持: %ld",
                   (unsigned)wParam, (unsigned)lParam, skipped);
        SetWindowTextW(g_staticProg, buf);
        return 0;
    }

    case WM_APP_DONE: {
        DoneReason reason = (DoneReason)wParam;
        DWORD err = (DWORD)lParam;
        g_running = false;
        EnableWindow(g_btnStart, TRUE);
        EnableWindow(g_btnStop, FALSE);
        EnableWindow(g_radioHid, TRUE);   // mode selectable again after a run
        EnableWindow(g_radioSw, TRUE);
        EnableWindow(g_radioEsp32, TRUE);
        if (g_comboPort) EnableWindow(g_comboPort, g_mode == MODE_ESP32);
        if (g_btnRefreshPort) EnableWindow(g_btnRefreshPort, g_mode == MODE_ESP32);
        if (g_btnTestPort) EnableWindow(g_btnTestPort, g_mode == MODE_ESP32);
        if (g_editInterval) EnableWindow(g_editInterval, g_mode != MODE_ESP32);
        if (g_chkShiftNl) EnableWindow(g_chkShiftNl, g_mode != MODE_ESP32);
        if (g_worker.joinable()) g_worker.join();
        g_esp32CountdownArmed = false;

        const wchar_t* modeTag = g_runMode == MODE_SOFTWARE ? L"软件输入模式"
                              : g_runMode == MODE_ESP32    ? L"ESP32 USB HID 模式"
                                                           : L"TextDeck HID 模式";
        long sent = g_sent.load(), skipped = g_skipped.load();
        wchar_t buf[352];
        switch (reason) {
        case DONE_COMPLETED:
            swprintf_s(buf,
                       L"完成【%s】：本次实际输入 %ld 字符，跳过 %ld 字符，无错误。"
                       L"（模式已标明，软件模式=SendInput，非虚拟 HID 设备）",
                       modeTag, sent, skipped);
            break;
        case DONE_STOPPED:
            swprintf_s(buf, L"已停止【%s】：已输入 %ld 字符，队列中后续字符未发送。",
                       modeTag, sent);
            break;
        case DONE_FOCUS:
            swprintf_s(buf, L"焦点离开目标窗口，已自动停止【%s】：已输入 %ld 字符，"
                       L"后续未发送。", modeTag, sent);
            break;
        default:
            if (g_runMode == MODE_SOFTWARE) {
                swprintf_s(buf,
                           L"错误【软件输入模式】：事件发送失败（Win32 错误码 %lu），"
                           L"已立即停止；已输入 %ld 字符。若错误码为 5 (拒绝访问)，"
                           L"通常是系统 UIPI/完整性级别限制，不会尝试绕过。", err, sent);
            } else if (g_runMode == MODE_ESP32) {
                std::wstring detail = TakeLastErrorDetail();
                if (detail.empty()) detail = L"未知错误";
                swprintf_s(buf,
                           L"错误【ESP32 USB HID 模式】：%s（已输入 %ld 字符）。"
                           L"设备已释放按键，不会自动续打；不会回退到驱动或 SendInput。",
                           detail.c_str(), sent);
                // A link error means the handle may be stale; the next run
                // re-opens it. The device keeps typing only until its 3 s host
                // lease expires, then releases keys by itself.
                g_esp32.Close();
            } else {
                swprintf_s(buf,
                           L"驱动通信失败【TextDeck HID 模式】，已停止输入（错误码 %lu，"
                           L"已键入 %ld 字符）。后续字符未发送，按键已释放。", err, sent);
                g_device.Close();   // treat as disconnect; next run re-opens
            }
            break;
        }
        SetWindowTextW(g_staticStatus, buf);
        // Terminal task state + tray feedback. The window is NOT brought to
        // the front: the user is still working in their target application.
        g_task.store(reason == DONE_COMPLETED ? TS_COMPLETED :
                     (reason == DONE_STOPPED ? TS_IDLE : TS_FAILED));
        wchar_t detail[256];
        swprintf_s(detail, L"%s - 已输入 %ld 字符，跳过 %ld",
                   modeTag, sent, skipped);
        SetTrayDetail(detail);
        UpdateTrayIcon();
        AppendSmokeLog(L"run finished");
        return 0;
    }

    case WM_CLOSE:
        // Closing the main window HIDES it: the app keeps running in the tray
        // with its hotkeys and any in-flight typing task. Only the tray menu
        // "退出程序" really terminates the process.
        if (g_inCountdown || g_esp32CountdownArmed) CancelCountdown(L"已取消。");
        SaveSettings();
        SaveWindowState(hwnd);
        SaveTextToDisk(nullptr);
        HideMainWindowToTray();
        return 0;

    case WM_DESTROY:
        UnregisterAllHotkeys(hwnd);
        CancelEsp32Countdown();
        if (g_running && g_stopEvent) {
            g_esp32.AbortPendingIo();
            SetEvent(g_stopEvent);
        }
        if (g_worker.joinable()) g_worker.join();
        if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
        if (g_esp32.IsOpen()) { std::wstring dummy; g_esp32.CancelTyping(nullptr, &dummy); }
        g_esp32.Close();
        g_device.Close();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR cmdLine, int)
{
    // Parse --data-dir / --mode / --smoke FIRST: the single-instance mutex and
    // the window class both derive from the data directory, and the mode
    // override must win over the persisted value below.
    if (!ParseCommandLine()) return 1;
    if (g_smokeMode && g_smokeLogPath.empty()) g_smokeLogPath = L"smoke.log";

    // --- single instance -----------------------------------------------------
    // A named mutex makes a second double-click a no-op: without it the user
    // would get two tray icons and two sets of hotkeys fighting over the same
    // combinations. The already-running instance is asked to show its window
    // through a registered message, so an in-flight typing task is NOT
    // interrupted.
    HANDLE singleMutex = CreateMutexW(nullptr, FALSE, g_mutexName.c_str());
    if (singleMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(g_windowClass.c_str(), nullptr);
        if (existing) {
            DWORD_PTR unused = 0;
            // Do not force the window forward if a task is running: the running
            // instance decides how to react to WM_APP_SHOWME.
            SendMessageTimeoutW(existing, WM_APP_SHOWME, 0, 0,
                                SMTO_ABORTIFHUNG, 2000, &unused);
        }
        if (singleMutex) CloseHandle(singleMutex);
        return 0;
    }

    g_elevatedArg = cmdLine && wcsstr(cmdLine, L"--elevated") != nullptr;
    GetModuleFileNameW(nullptr, g_exePath, MAX_PATH);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = g_windowClass.c_str();
    RegisterClassExW(&wc);

    // --- initial geometry -------------------------------------------------
    // 1. a saved NORMAL rect (validated against the current monitors) wins;
    // 2. otherwise use a comfortable 1100x800 logical size clamped to the
    //    work area of the monitor the window will appear on.
    LoadWindowState();
    int winW = 0, winH = 0;
    int winX = CW_USEDEFAULT, winY = CW_USEDEFAULT;
    if (g_winState.valid) {
        winX = g_winState.normal.left;
        winY = g_winState.normal.top;
        winW = g_winState.normal.right - g_winState.normal.left;
        winH = g_winState.normal.bottom - g_winState.normal.top;
    }
    // No saved rect: the preferred size CANNOT be computed yet - g_dpi is
    // still the 96-DPI placeholder until the window exists (WM_CREATE ->
    // CreateControls -> RefreshDpi reads the real monitor DPI). Sizing here
    // produced a window 2/3 of the intended logical size on 150%-scaled
    // displays. Create with the system default placement/size instead and
    // apply the real preferred size right after CreateWindowEx returns.

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
        L"VirtualHIDTyper - 虚拟HID逐字符输入",
        WS_OVERLAPPEDWINDOW,   // resizable + minimise + maximise + system menu
        winX, winY, winW, winH,
        nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    if (!g_winState.valid) {
        int pw = 0, ph = 0;
        GetPreferredWindowSize(hwnd, &pw, &ph);   // g_dpi is real by now
        SetWindowPos(hwnd, nullptr, 0, 0, pw, ph,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        // The CW_USEDEFAULT placement was picked for the placeholder size;
        // after resizing to the real preferred size keep the window fully
        // inside the work area (spec: never start off-screen).
        RECT wr{};
        if (GetWindowRect(hwnd, &wr)) {
            MONITORINFO mi{ sizeof(mi) };
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                int nx = wr.left, ny = wr.top;
                if (wr.right > mi.rcWork.right) nx -= (wr.right - mi.rcWork.right);
                if (wr.bottom > mi.rcWork.bottom) ny -= (wr.bottom - mi.rcWork.bottom);
                if (nx < mi.rcWork.left) nx = mi.rcWork.left;
                if (ny < mi.rcWork.top) ny = mi.rcWork.top;
                if (nx != wr.left || ny != wr.top)
                    SetWindowPos(hwnd, nullptr, nx, ny, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }

    LoadSettings();
    // explicit start-up mode override (used by automated E2E runs); applied
    // AFTER LoadSettings so it always wins over the persisted value.
    // ParseCommandLine already honoured --mode=; this keeps the legacy
    // --mode=software suffix working when callers bypass ParseCommandLine.
    if (g_modeOverride) SetMode(g_modeOverrideValue, false);
    if (cmdLine && wcsstr(cmdLine, L"--mode=software"))
        SetMode(MODE_SOFTWARE, false);
    SetMode(g_mode, true);   // refresh title + status bar with the active mode
    RefreshPortCombo();      // enumerate CDC ports without opening any of them

    // Real global hotkeys (RegisterHotKey/WM_HOTKEY). The persisted bindings
    // are applied as a PAIR so a failure on either one is reported precisely
    // instead of silently leaving a half-configured pair.
    ApplyHotkeys(hwnd, g_hkStart, g_hkStop);
    if (g_staticHotkeys) SetWindowTextW(g_staticHotkeys, HotkeyStatusText().c_str());
    // Re-render the mode banner and title now that the real bindings are known.
    SetMode(g_mode, true);

    if (!g_hkStartOk || !g_hkStopOk) {
        std::wstring why;
        if (!g_hkStartOk && !g_hkStopOk)
            why = L"开始和停止快捷键都注册失败（可能已被其他程序占用）。";
        else if (!g_hkStartOk)
            why = L"开始快捷键 " + HotkeyText(g_hkStart) + L" 注册失败（错误码 "
                  + std::to_wstring(g_hkStartErr) + L"），可能已被其他程序占用。";
        else
            why = L"停止快捷键 " + HotkeyText(g_hkStop) + L" 注册失败（错误码 "
                  + std::to_wstring(g_hkStopErr) + L"），可能已被其他程序占用。";
        why += L" 仍可通过托盘菜单和按钮开始/停止，请在“快捷键设置”中更换组合。";
        if (g_staticStatus) SetWindowTextW(g_staticStatus, why.c_str());
        SetTrayDetail(why);
    } else {
        SetTrayDetail(std::wstring(L"快捷键 ") + HotkeyText(g_hkStart)
                      + L" / " + HotkeyText(g_hkStop));
    }

    AddTrayIcon(hwnd);
    UpdateTrayIcon();

    // The main window is shown on first launch so the user can edit and save
    // the text; afterwards it can live in the tray indefinitely.
    // Restore the saved maximised state AFTER the window exists, so the
    // restored rect stays correct for the next normal (un-maximised) size.
    if (g_winState.valid && g_winState.maximized) {
        ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    } else {
        ShowWindow(hwnd, SW_SHOW);
    }
    UpdateWindow(hwnd);

    if (g_smokeMode) {
        AppendSmokeLog(L"smoke: window created");
        wchar_t hkline[256];
        swprintf_s(hkline, L"smoke: start hotkey %s registered=%d err=%lu",
                   HotkeyText(g_hkStart).c_str(), (int)g_hkStartOk, g_hkStartErr);
        AppendSmokeLog(hkline);
        swprintf_s(hkline, L"smoke: stop hotkey %s registered=%d err=%lu",
                   HotkeyText(g_hkStop).c_str(), (int)g_hkStopOk, g_hkStopErr);
        AppendSmokeLog(hkline);
        AppendSmokeLog(g_trayAdded ? L"smoke: tray icon added"
                                   : L"smoke: tray icon FAILED");
        // ESP32 CDC enumeration is part of the smoke contract: it proves the
        // port list populates without opening (and resetting) any device.
        RefreshPortCombo();
        wchar_t portline[256];
        swprintf_s(portline, L"smoke: esp32 ports=%u selected=%s",
                   (unsigned)g_portList.size(),
                   g_esp32Port.empty() ? L"(none)" : g_esp32Port.c_str());
        AppendSmokeLog(portline);
        DWORD err = 0;
        HidDevice probe;
        HidDevice::Result r = probe.Open(&err);
        wchar_t line[160];
        swprintf_s(line, L"smoke: device open -> result=%d win32error=%lu", (int)r, err);
        AppendSmokeLog(line);
        probe.Close();
        SetTimer(hwnd, TIMER_SMOKE, 2000, nullptr);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
