// ---------------------------------------------------------------------------
// esp32_link.h
//
// Win32 serial transport for the ESP32-S3 native USB CDC control port.
// One instance owns at most one COM handle. All request/response calls are
// serialised by an internal mutex; AbortPendingIo() is lock-free and is used
// by the emergency-stop path so a pending short request returns quickly.
//
// The port is deliberately kept OPEN across START/STOP and between runs. The
// firmware disables DTR/RTS/1200-bps reboot, and this class clears DTR/RTS on
// open, so opening/closing the daily control channel never resets the device.
// ---------------------------------------------------------------------------
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "esp32_protocol.h"

namespace esp32 {

struct PortInfo {
    std::wstring port;       // "COM7"
    std::wstring friendly;   // e.g. "USB Serial Device (COM7)"
    bool         espressif = false;
};

class Link {
public:
    Link();
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    // Enumerate present COM ports. Espressif VID_303A devices are flagged.
    static bool ListPorts(std::vector<PortInfo>* out);

    // Return `preferred` when present, otherwise the first Espressif port,
    // otherwise an empty string.
    static std::wstring ChoosePort(const std::vector<PortInfo>& ports,
                                   const std::wstring& preferred);

    bool Open(const std::wstring& port, std::wstring* err);
    void Close();
    bool IsOpen() const;
    std::wstring PortName() const;

    bool Ping(PingInfo* out, std::wstring* err);
    bool ReadText(std::string* text, uint32_t* seq, uint32_t* crc,
                  std::wstring* err);
    bool WriteText(const std::string& text, WriteAck* out, std::wstring* err);
    bool StartTyping(TypingStatus* out, std::wstring* err);
    bool CancelTyping(TypingStatus* out, std::wstring* err);
    bool GetStatus(TypingStatus* out, std::wstring* err);

    // Lock-free emergency interrupt. Only GET_STATUS/START/CANCEL/PING/READ
    // request loops check it; a WRITE frame is always completed so the device
    // parser is never left mid-frame. Cleared at the start of every request.
    void AbortPendingIo();
    void ClearAbort();

private:
    bool Request(uint8_t cmd, const uint8_t* payload, uint16_t len,
                 uint8_t expectedCmd, std::vector<uint8_t>* response,
                 DWORD timeoutMs, std::wstring* err);
    bool WriteAll(const uint8_t* data, size_t len, std::wstring* err);
    void SetError(std::wstring* err, const wchar_t* text) const;

    mutable std::mutex api_;
    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::wstring port_;
    std::vector<uint8_t> rx_;
    std::atomic<bool> abort_{false};
};

} // namespace esp32
