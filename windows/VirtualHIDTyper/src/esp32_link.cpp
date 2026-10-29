// ---------------------------------------------------------------------------
// esp32_link.cpp -- Win32 serial transport for the ESP32-S3 CDC control port.
// ---------------------------------------------------------------------------
#include "esp32_link.h"

#include <setupapi.h>
#include <devguid.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace esp32 {
namespace {

bool ContainsNoCase(const std::wstring& haystack, const wchar_t* needle)
{
    if (!needle || !*needle) return false;
    const size_t n = wcslen(needle);
    if (haystack.size() < n) return false;
    for (size_t i = 0; i + n <= haystack.size(); ++i) {
        bool same = true;
        for (size_t j = 0; j < n; ++j) {
            wchar_t a = haystack[i + j];
            wchar_t b = needle[j];
            if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
            if (b >= L'A' && b <= L'Z') b = (wchar_t)(b - L'A' + L'a');
            if (a != b) { same = false; break; }
        }
        if (same) return true;
    }
    return false;
}

int ComNumber(const std::wstring& port)
{
    if (port.size() < 4) return 0x7FFFFFFF;
    int n = 0;
    for (size_t i = 3; i < port.size(); ++i) {
        if (port[i] < L'0' || port[i] > L'9') return 0x7FFFFFFF;
        n = n * 10 + (port[i] - L'0');
    }
    return n;
}

std::wstring ReadRegistryString(HKEY key, const wchar_t* name)
{
    wchar_t buf[512] = L"";
    DWORD size = sizeof(buf);
    DWORD type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, (LPBYTE)buf, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        return buf;
    }
    return L"";
}

std::vector<BYTE> ReadDeviceProperty(HDEVINFO devs, SP_DEVINFO_DATA* dd,
                                     DWORD property)
{
    DWORD type = 0, size = 0;
    SetupDiGetDeviceRegistryPropertyW(devs, dd, property, &type, nullptr, 0, &size);
    if (size == 0) return {};
    std::vector<BYTE> buf(size + sizeof(wchar_t), 0);
    if (!SetupDiGetDeviceRegistryPropertyW(devs, dd, property, &type,
                                           buf.data(), (DWORD)buf.size(), &size)) {
        return {};
    }
    return buf;
}

std::wstring MultiSzContains(const std::vector<BYTE>& buf, const wchar_t* needle)
{
    if (buf.size() < sizeof(wchar_t)) return L"";
    const wchar_t* p = (const wchar_t*)buf.data();
    const wchar_t* end = p + buf.size() / sizeof(wchar_t);
    while (p < end && *p) {
        std::wstring id = p;
        if (ContainsNoCase(id, needle)) return id;
        p += id.size() + 1;
    }
    return L"";
}

} // namespace

Link::Link() = default;

Link::~Link()
{
    Close();
}

bool Link::ListPorts(std::vector<PortInfo>* out)
{
    if (!out) return false;
    out->clear();

    HDEVINFO devs = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr,
                                         DIGCF_PRESENT);
    if (devs != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dd{};
        dd.cbSize = sizeof(dd);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &dd); ++i) {
            HKEY key = SetupDiOpenDevRegKey(devs, &dd, DICS_FLAG_GLOBAL, 0,
                                            DIREG_DEV, KEY_READ);
            if (!key) continue;
            std::wstring port = ReadRegistryString(key, L"PortName");
            RegCloseKey(key);
            if (port.size() < 4 || _wcsnicmp(port.c_str(), L"COM", 3) != 0)
                continue;

            std::vector<BYTE> hw = ReadDeviceProperty(devs, &dd, SPDRP_HARDWAREID);
            std::vector<BYTE> compat = ReadDeviceProperty(devs, &dd, SPDRP_COMPATIBLEIDS);
            // Control port may be either:
            //   * the native-USB CDC of the ESP32-S3 (VID_303A), or
            //   * the on-board FTDI USB-UART bridge (VID_0403 / PID_6001), which
            //     is the control channel for two-wire firmware (v1.1) and for the
            //     v1.2 fallback / recovery path.
            // Real identity is always confirmed by a protocol PING before use, so
            // flagging FTDI here only affects auto-selection priority, never trust.
            bool esp = !MultiSzContains(hw, L"VID_303A").empty() ||
                       !MultiSzContains(compat, L"VID_303A").empty() ||
                       !MultiSzContains(hw, L"VID_0403").empty() ||
                       !MultiSzContains(compat, L"VID_0403").empty();

            std::vector<BYTE> friendlyBuf = ReadDeviceProperty(devs, &dd, SPDRP_FRIENDLYNAME);
            std::wstring friendly;
            if (!friendlyBuf.empty())
                friendly = (const wchar_t*)friendlyBuf.data();
            if (friendly.empty()) {
                std::vector<BYTE> desc = ReadDeviceProperty(devs, &dd, SPDRP_DEVICEDESC);
                if (!desc.empty()) friendly = (const wchar_t*)desc.data();
            }

            PortInfo info;
            info.port = port;
            info.friendly = friendly;
            info.espressif = esp;
            out->push_back(info);
        }
        SetupDiDestroyDeviceInfoList(devs);
    }

    // Fallback for unusual drivers: the classic SERIALCOMM map still lists
    // present COM ports (without VID information, so they cannot be flagged).
    if (out->empty()) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM",
                          0, KEY_READ, &key) == ERROR_SUCCESS) {
            for (DWORD i = 0;; ++i) {
                wchar_t name[256] = L"";
                DWORD nameLen = 256, type = 0;
                BYTE value[128] = {};
                DWORD valueLen = sizeof(value);
                if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type,
                                  value, &valueLen) != ERROR_SUCCESS)
                    break;
                if (type != REG_SZ) continue;
                std::wstring port = (const wchar_t*)value;
                if (port.size() >= 4 && _wcsnicmp(port.c_str(), L"COM", 3) == 0) {
                    PortInfo info;
                    info.port = port;
                    info.friendly = port;
                    info.espressif = false;
                    out->push_back(info);
                }
            }
            RegCloseKey(key);
        }
    }

    std::sort(out->begin(), out->end(), [](const PortInfo& a, const PortInfo& b) {
        if (a.espressif != b.espressif) return a.espressif;
        int na = ComNumber(a.port), nb = ComNumber(b.port);
        if (na != nb) return na < nb;
        return a.port < b.port;
    });
    return true;
}

std::wstring Link::ChoosePort(const std::vector<PortInfo>& ports,
                              const std::wstring& preferred)
{
    for (const PortInfo& p : ports) {
        if (_wcsicmp(p.port.c_str(), preferred.c_str()) == 0) return p.port;
    }
    for (const PortInfo& p : ports) {
        if (p.espressif) return p.port;
    }
    return L"";
}

bool Link::Open(const std::wstring& port, std::wstring* err)
{
    std::lock_guard<std::mutex> lk(api_);
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
    port_.clear();
    rx_.clear();

    if (port.empty()) {
        SetError(err, L"未选择 ESP32 串口。");
        return false;
    }
    if (port.size() < 4 || _wcsnicmp(port.c_str(), L"COM", 3) != 0) {
        SetError(err, L"串口名无效（应为 COMx）。");
        return false;
    }

    std::wstring path = L"\\\\.\\" + port;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (err) {
            *err = L"无法打开 " + port + L"（Win32 错误码 " +
                   std::to_wstring(e) + L"）。请确认 ESP32 原生 USB 已连接、"
                   L"串口未被其他程序占用。";
        }
        return false;
    }

    SetupComm(h, 65536, 65536);
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        if (err) *err = L"读取串口配置失败（错误码 " + std::to_wstring(e) + L"）。";
        return false;
    }
    dcb.BaudRate = 115200;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        if (err) *err = L"设置串口参数失败（错误码 " + std::to_wstring(e) + L"）。";
        return false;
    }

    COMMTIMEOUTS to{};
    // ReadFile returns as soon as one byte is available, otherwise it waits
    // 20 ms. That keeps the emergency-stop flag responsive without busy
    // spinning and without CancelIoEx races against CloseHandle.
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant = 20;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 500;
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
    // Open with DTR+RTS asserted. The native USB CDC only reports itself
    // "connected" -- and only delivers protocol responses -- once the host
    // asserts BOTH DTR and RTS; USBCDC::write() silently drops bytes until then.
    // (USBCDC::enableReboot(false) in firmware v1.2 removed the 1200-bps/DTR
    // reboot path, so opening no longer bounces the device into the bootloader;
    // verified on both COM4/FTDI and COM6/native with pySerial.)
    EscapeCommFunction(h, SETDTR);
    EscapeCommFunction(h, SETRTS);

    h_ = h;
    port_ = port;
    abort_.store(false);
    return true;
}

void Link::Close()
{
    std::lock_guard<std::mutex> lk(api_);
    if (h_ != INVALID_HANDLE_VALUE) {
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }
    port_.clear();
    rx_.clear();
}

bool Link::IsOpen() const
{
    std::lock_guard<std::mutex> lk(api_);
    return h_ != INVALID_HANDLE_VALUE;
}

std::wstring Link::PortName() const
{
    std::lock_guard<std::mutex> lk(api_);
    return port_;
}

void Link::AbortPendingIo()
{
    abort_.store(true);
}

void Link::ClearAbort()
{
    abort_.store(false);
}

bool Link::Request(uint8_t cmd, const uint8_t* payload, uint16_t len,
                   uint8_t expectedCmd, std::vector<uint8_t>* response,
                   DWORD timeoutMs, std::wstring* err)
{
    if (response) response->clear();
    if (h_ == INVALID_HANDLE_VALUE) {
        SetError(err, L"ESP32 串口尚未打开。");
        return false;
    }

    abort_.store(false);
    std::vector<uint8_t> frame = BuildFrame(cmd, payload, len);
    if (frame.empty()) {
        SetError(err, L"协议帧过长，未发送。");
        return false;
    }

    // Discard stale bytes from a previous timed-out/aborted exchange. This is
    // safe because only one request is in flight at a time.
    rx_.clear();
    PurgeComm(h_, PURGE_RXCLEAR);

    if (!WriteAll(frame.data(), frame.size(), err)) return false;

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    uint8_t tmp[4096];
    for (;;) {
        // A WRITE frame is never abandoned mid-frame; all other request loops
        // honour the emergency stop flag within one 20 ms read slice.
        if (cmd != kCmdWriteText && abort_.load()) {
            SetError(err, L"操作已取消。");
            return false;
        }

        DWORD got = 0;
        if (ReadFile(h_, tmp, sizeof(tmp), &got, nullptr)) {
            if (got) rx_.insert(rx_.end(), tmp, tmp + got);
        } else {
            DWORD e = GetLastError();
            if (e != ERROR_TIMEOUT && e != ERROR_IO_PENDING &&
                e != ERROR_OPERATION_ABORTED) {
                if (err) *err = L"读取串口失败（Win32 错误码 " +
                                std::to_wstring(e) + L"）。";
                return false;
            }
        }

        for (;;) {
            uint8_t respCmd = 0;
            std::vector<uint8_t> pl;
            if (!ExtractFrame(rx_, &respCmd, &pl)) break;
            if (respCmd == expectedCmd) {
                if (response) *response = std::move(pl);
                return true;
            }
            // A late response to an earlier request is harmless; keep looking
            // for the response that belongs to this request.
        }

        if (GetTickCount64() >= deadline) {
            if (err) {
                *err = L"等待 ESP32 响应超时（" + std::to_wstring(timeoutMs) +
                       L" ms）。设备可能已断开或固件未运行。";
            }
            return false;
        }
    }
}

bool Link::WriteAll(const uint8_t* data, size_t len, std::wstring* err)
{
    size_t off = 0;
    while (off < len) {
        DWORD chunk = (DWORD)std::min<size_t>(len - off, 4096);
        DWORD wrote = 0;
        if (!WriteFile(h_, data + off, chunk, &wrote, nullptr)) {
            DWORD e = GetLastError();
            if (err) *err = L"写入串口失败（Win32 错误码 " +
                            std::to_wstring(e) + L"）。";
            return false;
        }
        if (wrote == 0) {
            SetError(err, L"串口写入返回 0 字节。");
            return false;
        }
        off += wrote;
    }
    return true;
}

bool Link::Ping(PingInfo* out, std::wstring* err)
{
    std::lock_guard<std::mutex> lk(api_);
    std::vector<uint8_t> pl;
    if (!Request(kCmdPing, nullptr, 0, kCmdPing | 0x80, &pl, 1500, err))
        return false;
    PingInfo p;
    if (!ParsePing(pl, &p, err)) return false;
    if (p.status != kStOk) {
        if (err) *err = std::wstring(L"PING 返回错误：") + StatusText(p.status);
        return false;
    }
    if (out) *out = p;
    return true;
}

bool Link::ReadText(std::string* text, uint32_t* seq, uint32_t* crc,
                    std::wstring* err)
{
    std::lock_guard<std::mutex> lk(api_);
    std::vector<uint8_t> pl;
    if (!Request(kCmdReadText, nullptr, 0, kCmdReadText | 0x80, &pl, 3500, err))
        return false;
    if (pl.size() < 13) {
        SetError(err, L"READ 响应长度不足 13 字节。");
        return false;
    }
    uint8_t status = pl[0];
    if (status != kStOk) {
        if (err) *err = std::wstring(L"READ 返回错误：") + StatusText(status);
        return false;
    }
    uint16_t len = (uint16_t)(pl[3] | (pl[4] << 8));
    uint32_t savedCrc = (uint32_t)pl[5] | ((uint32_t)pl[6] << 8) |
                        ((uint32_t)pl[7] << 16) | ((uint32_t)pl[8] << 24);
    uint32_t savedSeq = (uint32_t)pl[9] | ((uint32_t)pl[10] << 8) |
                        ((uint32_t)pl[11] << 16) | ((uint32_t)pl[12] << 24);
    if (len > kMaxText || pl.size() != (size_t)13 + len) {
        SetError(err, L"READ 响应长度与 saved_len 不一致。");
        return false;
    }
    if (Crc32(len ? &pl[13] : nullptr, len) != savedCrc) {
        SetError(err, L"READ 文本 CRC 校验失败。");
        return false;
    }
    if (text) text->assign((const char*)&pl[13], len);
    if (seq) *seq = savedSeq;
    if (crc) *crc = savedCrc;
    return true;
}

bool Link::WriteText(const std::string& text, WriteAck* out, std::wstring* err)
{
    std::lock_guard<std::mutex> lk(api_);
    if (text.empty() || text.size() > kMaxText) {
        SetError(err, L"WRITE 文本长度非法。");
        return false;
    }
    std::vector<uint8_t> pl;
    if (!Request(kCmdWriteText, (const uint8_t*)text.data(),
                 (uint16_t)text.size(), kCmdWriteText | 0x80, &pl, 3500, err))
        return false;
    WriteAck ack;
    if (!ParseWriteAck(pl, &ack, err)) return false;
    if (ack.status != kStOk) {
        if (err) {
            *err = std::wstring(L"设备拒绝写入：") + StatusText(ack.status);
            if (ack.status == kStErrCharset)
                *err += L"（首个坏字符位置 " + std::to_wstring(ack.detail) + L"）";
        }
        return false;
    }
    uint32_t wantCrc = Crc32((const uint8_t*)text.data(), text.size());
    if (ack.savedLen != text.size() || ack.savedCrc != wantCrc) {
        if (err) *err = L"设备写入回执与发送内容不一致（回读校验失败）。";
        return false;
    }
    if (out) *out = ack;
    return true;
}

bool Link::StartTyping(TypingStatus* out, std::wstring* err)
{
    std::lock_guard<std::mutex> lk(api_);
    std::vector<uint8_t> pl;
    if (!Request(kCmdStartTyping, nullptr, 0, kCmdStartTyping | 0x80, &pl,
                 2500, err))
        return false;
    TypingStatus s;
    if (!ParseTypingStatus(pl, &s, err)) return false;
    if (s.status != kStOk) {
        if (err) {
            *err = std::wstring(L"设备拒绝开始输入：") + StatusText(s.status);
            if (s.lastError) *err += std::wstring(L"（") +
                                     TypingErrorText(s.lastError) + L"）";
        }
        return false;
    }
    if (out) *out = s;
    return true;
}

bool Link::CancelTyping(TypingStatus* out, std::wstring* err)
{
    std::lock_guard<std::mutex> lk(api_);
    std::vector<uint8_t> pl;
    if (!Request(kCmdCancelTyping, nullptr, 0, kCmdCancelTyping | 0x80, &pl,
                 1500, err))
        return false;
    TypingStatus s;
    if (!ParseTypingStatus(pl, &s, err)) return false;
    if (s.status != kStOk) {
        if (err) *err = std::wstring(L"CANCEL 返回错误：") + StatusText(s.status);
        return false;
    }
    if (out) *out = s;
    return true;
}

bool Link::GetStatus(TypingStatus* out, std::wstring* err)
{
    std::lock_guard<std::mutex> lk(api_);
    std::vector<uint8_t> pl;
    if (!Request(kCmdGetStatus, nullptr, 0, kCmdGetStatus | 0x80, &pl,
                 1200, err))
        return false;
    TypingStatus s;
    if (!ParseTypingStatus(pl, &s, err)) return false;
    if (s.status != kStOk) {
        if (err) *err = std::wstring(L"GET_STATUS 返回错误：") + StatusText(s.status);
        return false;
    }
    if (out) *out = s;
    return true;
}

void Link::SetError(std::wstring* err, const wchar_t* text) const
{
    if (err) *err = text ? text : L"未知串口错误。";
}

} // namespace esp32
