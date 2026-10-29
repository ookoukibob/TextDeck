// ---------------------------------------------------------------------------
// esp32_protocol.h
//
// Pure, testable implementation of the ESP32-S3 v1.2 binary protocol.
// No Win32 / no serial I/O here: map_selftest.exe compiles this file directly.
//
// Wire format (little endian, identical to the firmware's protocol.h):
//   [0]      0xE7 magic0
//   [1]      0x53 magic1
//   [2]      0x01 protocol version
//   [3]      command; responses use command | 0x80
//   [4..5]   payload length u16
//   [6..9]   CRC32 of payload u32
//   [10..]   payload
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esp32 {

constexpr uint8_t kMagic0 = 0xE7;
constexpr uint8_t kMagic1 = 0x53;
constexpr uint8_t kProtocolVersion = 0x01;
constexpr size_t  kHeaderLen = 10;
constexpr uint16_t kMaxText = 10240;
constexpr uint16_t kMaxPayload = 13 + kMaxText;   // READ response maximum

constexpr uint8_t kCmdWriteText = 0x01;
constexpr uint8_t kCmdReadText = 0x02;
constexpr uint8_t kCmdPing = 0x03;
constexpr uint8_t kCmdStartTyping = 0x04;
constexpr uint8_t kCmdCancelTyping = 0x05;
constexpr uint8_t kCmdGetStatus = 0x06;

constexpr uint8_t kStOk = 0;
constexpr uint8_t kStErrCrc = 1;
constexpr uint8_t kStErrLength = 2;
constexpr uint8_t kStErrCharset = 3;
constexpr uint8_t kStErrStorage = 4;
constexpr uint8_t kStErrCmd = 5;
constexpr uint8_t kStErrBusy = 6;
constexpr uint8_t kStErrFrame = 7;
constexpr uint8_t kStErrNoText = 8;
constexpr uint8_t kStErrHidNotReady = 9;

constexpr uint8_t kTyIdle = 0;
constexpr uint8_t kTyPreparing = 1;
constexpr uint8_t kTyTyping = 2;
constexpr uint8_t kTyCompleted = 3;
constexpr uint8_t kTyCancelled = 4;
constexpr uint8_t kTyFailed = 5;

constexpr uint8_t kTyErrNone = 0;
constexpr uint8_t kTyErrNoText = 1;
constexpr uint8_t kTyErrHidNotReady = 2;
constexpr uint8_t kTyErrBusy = 3;
constexpr uint8_t kTyErrBadChar = 4;
constexpr uint8_t kTyErrPressReject = 5;
constexpr uint8_t kTyErrUsbLost = 6;
constexpr uint8_t kTyErrHostLost = 7;
constexpr uint8_t kTyErrCancelled = 8;

constexpr uint8_t kCancelNone = 0;
constexpr uint8_t kCancelHost = 1;
constexpr uint8_t kCancelLocal = 2;
constexpr uint8_t kCancelLease = 3;
constexpr uint8_t kCancelUsbLost = 4;

constexpr uint8_t kFlagHostInitiated = 0x01;
constexpr uint8_t kFlagReleaseSent = 0x02;
constexpr uint8_t kFlagHidReady = 0x04;

constexpr uint16_t kCapRemoteTyping = 0x0001;
constexpr uint16_t kCapHostLease = 0x0002;
constexpr uint16_t kCapNativeCdc = 0x0004;

constexpr uint16_t kFirmwareV12 = 0x0102;

struct PingInfo {
    uint8_t  status = 0xFF;
    uint8_t  proto = 0;
    uint16_t fw = 0;
    uint16_t maxText = 0;
    bool     hasText = false;
    uint16_t savedLen = 0;
    uint32_t savedCrc = 0;
    uint16_t seq16 = 0;
    bool     hidReady = false;
};

struct WriteAck {
    uint8_t  status = 0xFF;
    uint16_t detail = 0;
    uint16_t savedLen = 0;
    uint32_t savedCrc = 0;
    uint32_t seq = 0;
    bool     written = false;
};

struct TypingStatus {
    uint8_t  status = 0xFF;
    uint8_t  state = 0;
    uint16_t processed = 0;
    uint16_t total = 0;
    uint8_t  lastError = 0;
    uint8_t  cancelReason = 0;
    uint8_t  flags = 0;
    uint16_t leaseRemainingMs = 0;
    uint16_t sessionId = 0;
    uint16_t capabilities = 0;
};

// CRC32/IEEE (reflected 0xEDB88320, init/final xor 0xFFFFFFFF).
uint32_t Crc32(const uint8_t* data, size_t len);

// Build one complete frame. Returns an empty vector when len > kMaxPayload.
std::vector<uint8_t> BuildFrame(uint8_t cmd, const uint8_t* payload, uint16_t len);

// Extract one complete, CRC-valid frame from the front of `buffer`.
// Garbage (including firmware boot logs) is skipped. If no complete frame is
// available, bytes that may still become one are retained and false is
// returned. Bad-CRC candidates are discarded one byte at a time so the parser
// can resynchronise on a later magic sequence.
bool ExtractFrame(std::vector<uint8_t>& buffer, uint8_t* cmd,
                  std::vector<uint8_t>* payload);

bool ParsePing(const std::vector<uint8_t>& payload, PingInfo* out,
               std::wstring* err);
bool ParseWriteAck(const std::vector<uint8_t>& payload, WriteAck* out,
                   std::wstring* err);
bool ParseTypingStatus(const std::vector<uint8_t>& payload, TypingStatus* out,
                       std::wstring* err);

// Normalize CRLF / lone CR to LF, then validate the device's exact character
// set (printable ASCII 0x20..0x7E, LF, TAB) and 10 KiB limit. On failure no
// partial text is returned. `badIndex` (0-based) and `badChar` describe the
// first rejected character when applicable.
bool PrepareText(const std::wstring& text, std::string* ascii,
                 std::wstring* err, size_t* badIndex = nullptr,
                 wchar_t* badChar = nullptr);

const wchar_t* StatusText(uint8_t status);
const wchar_t* TypingErrorText(uint8_t err);
const wchar_t* CancelReasonText(uint8_t reason);

} // namespace esp32
