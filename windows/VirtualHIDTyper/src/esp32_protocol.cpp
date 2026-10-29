// ---------------------------------------------------------------------------
// esp32_protocol.cpp -- pure protocol codec; see esp32_protocol.h.
// ---------------------------------------------------------------------------
#include "esp32_protocol.h"

#include <cstdio>
#include <cwchar>

namespace esp32 {
namespace {

uint16_t GetU16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t GetU32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void PutU16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)(x >> 8));
}

void PutU32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 24) & 0xFF));
}

std::wstring DescribeChar(wchar_t c)
{
    wchar_t buf[64];
    if (c >= 0x20 && c <= 0x7E) {
        swprintf_s(buf, L"'%c' (U+%04X)", c, (unsigned)c);
    } else {
        swprintf_s(buf, L"U+%04X", (unsigned)c);
    }
    return buf;
}

} // namespace

uint32_t Crc32(const uint8_t* data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

std::vector<uint8_t> BuildFrame(uint8_t cmd, const uint8_t* payload, uint16_t len)
{
    std::vector<uint8_t> out;
    if (len > kMaxPayload) return out;
    out.reserve(kHeaderLen + len);
    out.push_back(kMagic0);
    out.push_back(kMagic1);
    out.push_back(kProtocolVersion);
    out.push_back(cmd);
    PutU16(out, len);
    PutU32(out, Crc32(payload, len));
    if (len && payload) out.insert(out.end(), payload, payload + len);
    return out;
}

bool ExtractFrame(std::vector<uint8_t>& buffer, uint8_t* cmd,
                  std::vector<uint8_t>* payload)
{
    size_t i = 0;
    while (i < buffer.size()) {
        if (buffer[i] != kMagic0) {
            ++i;
            continue;
        }
        if (i + 2 > buffer.size()) break;          // magic0 at the very end
        if (buffer[i + 1] != kMagic1) {
            ++i;
            continue;
        }
        if (i + kHeaderLen > buffer.size()) break; // incomplete header
        if (buffer[i + 2] != kProtocolVersion) {
            ++i;
            continue;
        }
        uint16_t len = GetU16(&buffer[i + 4]);
        if (len > kMaxPayload) {
            ++i;
            continue;
        }
        size_t total = kHeaderLen + (size_t)len;
        if (i + total > buffer.size()) break;      // incomplete body
        uint32_t want = GetU32(&buffer[i + 6]);
        uint32_t got = Crc32(len ? &buffer[i + kHeaderLen] : nullptr, len);
        if (want != got) {
            ++i;                                   // bad CRC: resync later
            continue;
        }
        if (cmd) *cmd = buffer[i + 3];
        if (payload) {
            payload->assign(buffer.begin() + (ptrdiff_t)(i + kHeaderLen),
                            buffer.begin() + (ptrdiff_t)(i + total));
        }
        buffer.erase(buffer.begin(), buffer.begin() + (ptrdiff_t)(i + total));
        return true;
    }

    // Keep only the possible start of an incomplete frame. Everything before
    // it is known garbage and is discarded.
    if (i > 0) buffer.erase(buffer.begin(), buffer.begin() + (ptrdiff_t)i);
    if (!buffer.empty() && buffer[0] != kMagic0) {
        if (buffer.back() == kMagic0) {
            uint8_t b = buffer.back();
            buffer.assign(1, b);
        } else {
            buffer.clear();
        }
    }
    return false;
}

bool ParsePing(const std::vector<uint8_t>& payload, PingInfo* out,
               std::wstring* err)
{
    if (payload.size() != 16) {
        if (err) *err = L"PING 响应长度不是 16 字节（固件协议不匹配）";
        return false;
    }
    PingInfo p;
    p.status = payload[0];
    p.proto = payload[1];
    p.fw = GetU16(&payload[2]);
    p.maxText = GetU16(&payload[4]);
    p.hasText = payload[6] != 0;
    p.savedLen = GetU16(&payload[7]);
    p.savedCrc = GetU32(&payload[9]);
    p.seq16 = GetU16(&payload[13]);
    p.hidReady = payload[15] != 0;

    if (p.proto != kProtocolVersion) {
        if (err) *err = L"PING 协议版本不是 0x01（固件协议不匹配）";
        return false;
    }
    if (p.maxText == 0 || p.maxText > kMaxText) {
        if (err) *err = L"PING 返回的文本上限非法";
        return false;
    }
    if (payload[6] > 1 || payload[15] > 1) {
        if (err) *err = L"PING 布尔字段非法";
        return false;
    }
    if (p.savedLen > p.maxText) {
        if (err) *err = L"PING saved_len 超过 max_text";
        return false;
    }
    if (!p.hasText && p.savedLen != 0) {
        if (err) *err = L"PING 无文本但 saved_len 非零";
        return false;
    }
    if (out) *out = p;
    return true;
}

bool ParseWriteAck(const std::vector<uint8_t>& payload, WriteAck* out,
                   std::wstring* err)
{
    if (payload.size() != 14) {
        if (err) *err = L"WRITE 响应长度不是 14 字节";
        return false;
    }
    WriteAck w;
    w.status = payload[0];
    w.detail = GetU16(&payload[1]);
    w.savedLen = GetU16(&payload[3]);
    w.savedCrc = GetU32(&payload[5]);
    w.seq = GetU32(&payload[9]);
    w.written = payload[13] != 0;
    if (payload[13] > 1) {
        if (err) *err = L"WRITE 响应 written 字段非法";
        return false;
    }
    if (out) *out = w;
    return true;
}

bool ParseTypingStatus(const std::vector<uint8_t>& payload, TypingStatus* out,
                       std::wstring* err)
{
    if (payload.size() != 16) {
        if (err) *err = L"输入状态响应长度不是 16 字节";
        return false;
    }
    TypingStatus s;
    s.status = payload[0];
    s.state = payload[1];
    s.processed = GetU16(&payload[2]);
    s.total = GetU16(&payload[4]);
    s.lastError = payload[6];
    s.cancelReason = payload[7];
    s.flags = payload[8];
    s.leaseRemainingMs = GetU16(&payload[10]);
    s.sessionId = GetU16(&payload[12]);
    s.capabilities = GetU16(&payload[14]);
    if (s.state > kTyFailed) {
        if (err) *err = L"输入状态响应的 state 字段非法";
        return false;
    }
    if (s.processed > s.total) {
        if (err) *err = L"输入状态响应的 processed 超过 total";
        return false;
    }
    if (out) *out = s;
    return true;
}

bool PrepareText(const std::wstring& text, std::string* ascii,
                 std::wstring* err, size_t* badIndex, wchar_t* badChar)
{
    if (ascii) ascii->clear();
    if (badIndex) *badIndex = 0;
    if (badChar) *badChar = 0;

    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        wchar_t c = text[i];
        if (c == L'\r') {
            if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
            c = L'\n';
        }
        bool ok = (c >= 0x20 && c <= 0x7E) || c == L'\n' || c == L'\t';
        if (!ok) {
            if (badIndex) *badIndex = out.size();
            if (badChar) *badChar = c;
            if (err) {
                *err = L"ESP32 HID 模式仅支持 ASCII 0x20-0x7E、换行和 Tab；"
                       L"发现不支持字符 " + DescribeChar(c);
            }
            return false;
        }
        out.push_back((char)c);
    }

    if (out.empty()) {
        if (err) *err = L"文本为空，未发送。";
        return false;
    }
    if (out.size() > kMaxText) {
        wchar_t buf[160];
        swprintf_s(buf, L"文本为 %u 字节，超过 ESP32 单条上限 %u 字节（10 KiB）。",
                   (unsigned)out.size(), (unsigned)kMaxText);
        if (err) *err = buf;
        return false;
    }
    if (ascii) *ascii = std::move(out);
    return true;
}

const wchar_t* StatusText(uint8_t status)
{
    switch (status) {
    case kStOk:             return L"成功";
    case kStErrCrc:         return L"CRC 校验失败";
    case kStErrLength:      return L"长度非法";
    case kStErrCharset:     return L"含不支持的字符";
    case kStErrStorage:     return L"设备存储失败";
    case kStErrCmd:         return L"固件不支持该命令";
    case kStErrBusy:        return L"设备忙碌（已有输入任务）";
    case kStErrFrame:       return L"帧格式错误";
    case kStErrNoText:      return L"设备中没有可输入的文本";
    case kStErrHidNotReady: return L"设备 HID 未枚举/不可用";
    default:                return L"未知状态";
    }
}

const wchar_t* TypingErrorText(uint8_t err)
{
    switch (err) {
    case kTyErrNone:         return L"无错误";
    case kTyErrNoText:       return L"设备中没有可输入的文本";
    case kTyErrHidNotReady:  return L"HID 未就绪";
    case kTyErrBusy:         return L"设备忙碌";
    case kTyErrBadChar:      return L"遇到不支持的字符";
    case kTyErrPressReject:  return L"HID 按键发送被拒绝";
    case kTyErrUsbLost:      return L"USB HID 连接断开";
    case kTyErrHostLost:     return L"主机控制租约超时";
    case kTyErrCancelled:    return L"输入已被取消";
    default:                 return L"未知输入错误";
    }
}

const wchar_t* CancelReasonText(uint8_t reason)
{
    switch (reason) {
    case kCancelNone:    return L"无";
    case kCancelHost:    return L"主机请求停止";
    case kCancelLocal:   return L"设备本地停止";
    case kCancelLease:   return L"主机租约超时";
    case kCancelUsbLost: return L"USB 断开";
    default:             return L"未知原因";
    }
}

} // namespace esp32
