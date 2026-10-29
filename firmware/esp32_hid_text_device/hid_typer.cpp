#ifdef HID_TYPER_HOST_TEST
// 主机侧状态机单元测试: 用假时钟/假键盘原样编译生产状态机代码。
#include "host_test_hal.h"
#else
#include <Arduino.h>
#include "USBHIDKeyboard.h"
#include "USBHID.h"
#endif
#include "hid_typer.h"
#include "protocol.h"

// 输入节奏 (毫秒), 与 v1.1 已通过硬件验证的取值保持一致
#define PRE_DELAY_MS   80    // 触发后、输入首个字符前的等待
#define HELD_MS         8    // 按下保持
#define CHAR_GAP_MS    15    // 相邻字符间隔

// ---- 断连检测宽限期 (依据已核实的 Arduino-ESP32 3.3.12 + TinyUSB 0.21.0 源码) ----
// USBHID::ready() -> tud_hid_n_ready(0):
//     return tud_ready() && (ep_in != 0) && !usbd_edpt_busy(rhport, ep_in);
// 其中 ep-in 端点"忙"表示上一份报告仍在途 (USB 全速下约 1ms 量级), 属正常瞬态;
// 真实拔线/挂起会使 tud_ready() 变 false 并持续为 false。
// 状态机每个 loop 都复查, 只有连续 10ms 不就绪才判定断连, 不阻塞主循环。
#define READY_GRACE_MS  10

static USBHIDKeyboard *s_kb = nullptr;
static USBHID *s_hid = nullptr;

static inline bool timeReached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

void HidTyper::attach(void *keyboard, void *hidProbe) {
  s_kb = (USBHIDKeyboard *)keyboard;
  s_hid = (USBHID *)hidProbe;
}

void HidTyper::releaseAll() {
  if (s_kb) s_kb->releaseAll();
  _keyDown = false;
}

HidTyper::StartResult HidTyper::beginTypeText(const uint8_t *data, uint16_t len,
                                              bool hostInitiated) {
  if (!s_kb || !s_hid) {
    return START_HID_NOT_READY;
  }
  if (isActive()) {
    Serial.println("[hid] START rejected: busy");
    return START_BUSY;
  }
  if (!data || len == 0) {
    Serial.println("[hid] START rejected: no text");
    return START_NO_TEXT;
  }
  if (len > MAX_TEXT_LEN) {
    Serial.println("[hid] START rejected: text too long");
    return START_INVALID;
  }
  for (uint16_t i = 0; i < len; i++) {
    if (!proto_char_supported(data[i])) {
      Serial.printf("[hid] START rejected: unsupported char 0x%02X at %u\n",
                    data[i], (unsigned)i);
      return START_INVALID;
    }
  }
  if (!s_hid->ready()) {
    Serial.println("[hid] START rejected: USB HID not ready");
    return START_HID_NOT_READY;
  }

  // 清除上一轮可能遗留的内部键态 (发一份全释放报告)
  releaseAll();

  uint32_t now = millis();
  _data = data;
  _len = len;
  _idx = 0;
  _hostInitiated = hostInitiated;
  _state = TY_PREPARING;
  _phase = PH_PRE;
  _lastError = TY_ERR_NONE;
  _cancelReason = CANCEL_NONE;
  _flags = hostInitiated ? TY_FLAG_HOST_INITIATED : 0;
  _nextAt = now + PRE_DELAY_MS;
  _leaseDeadline = hostInitiated ? (now + HOST_LEASE_TIMEOUT_MS) : 0;
  _notReadySince = 0xFFFFFFFFu;
  _sessionId++;
  if (_sessionId == 0) _sessionId = 1;

  Serial.printf("[hid] START accepted: session=%u len=%u host=%d, first char in %dms\n",
                (unsigned)_sessionId, (unsigned)len, hostInitiated ? 1 : 0,
                PRE_DELAY_MS);
  return START_OK;
}

void HidTyper::poll() {
  if (!isActive()) return;

  uint32_t now = millis();

  // 主机控制租约: 只对 Windows 发起的任务生效; 本地 BOOT 任务不受影响。
  if (_hostInitiated && timeReached(now, _leaseDeadline)) {
    abortRound(TY_FAILED, TY_ERR_HOST_LOST, CANCEL_LEASE);
    return;
  }

  // USB HID 断连检测 (非阻塞宽限)
  if (!s_hid || !s_hid->ready()) {
    if (_notReadySince == 0xFFFFFFFFu) {
      _notReadySince = now;
    } else if ((uint32_t)(now - _notReadySince) >= READY_GRACE_MS) {
      abortRound(TY_FAILED, TY_ERR_USB_LOST, CANCEL_USB_LOST);
      return;
    }
    return;
  }
  _notReadySince = 0xFFFFFFFFu;

  if (!timeReached(now, _nextAt)) return;

  switch (_phase) {
    case PH_PRE:
      _state = TY_TYPING;
      // 首字符发送前再次确认 HID (上面已检查); 直接进入按下阶段
      _phase = PH_PRESS;
      _nextAt = now;
      pressCurrent(now);
      break;

    case PH_PRESS:
      pressCurrent(now);
      break;

    case PH_RELEASE: {
      uint8_t c = _data[_idx];
      size_t released = s_kb ? s_kb->release(c) : 0;
      if (!released) {
        abortRound(TY_FAILED, TY_ERR_PRESS_REJECT, CANCEL_NONE);
        return;
      }
      _keyDown = false;
      _idx++;
      _phase = PH_GAP;
      _nextAt = now + CHAR_GAP_MS;
      break;
    }

    case PH_GAP:
      if (_idx >= _len) {
        completeRound();
      } else {
        _phase = PH_PRESS;
        _nextAt = now;
        pressCurrent(now);
      }
      break;

    default:
      break;
  }
}

// 在调用前已确认 HID ready 且到达时间点。
void HidTyper::pressCurrent(uint32_t now) {
  if (_idx >= _len) {
    completeRound();
    return;
  }
  uint8_t c = _data[_idx];
  if (!proto_char_supported(c)) {
    abortRound(TY_FAILED, TY_ERR_BAD_CHAR, CANCEL_NONE);
    return;
  }
  if (!s_kb || s_kb->press(c) == 0) {
    abortRound(TY_FAILED, TY_ERR_PRESS_REJECT, CANCEL_NONE);
    return;
  }
  _keyDown = true;
  _phase = PH_RELEASE;
  _nextAt = now + HELD_MS;
}

void HidTyper::completeRound() {
  releaseAll();
  _flags |= TY_FLAG_RELEASE_SENT;
  _state = TY_COMPLETED;
  _phase = PH_IDLE;
  _leaseDeadline = 0;
  Serial.printf("[hid] COMPLETED: %u/%u chars, keys released\n",
                (unsigned)_idx, (unsigned)_len);
}

void HidTyper::abortRound(uint8_t state, uint8_t err, uint8_t reason) {
  // 尽力释放键盘状态; 断连时报告发不出去, 但内部键态会被清零,
  // 重连后不会遗留旧按键。本轮退出后不会自动续打/补发。
  releaseAll();
  _flags |= TY_FLAG_RELEASE_SENT;
  _state = state;
  _lastError = err;
  _cancelReason = reason;
  _phase = PH_IDLE;
  _leaseDeadline = 0;
  Serial.printf("[hid] %s at char %u/%u: err=%u reason=%u; typing stopped, "
                "remaining text will NOT be typed, no auto-resend\n",
                state == TY_CANCELLED ? "CANCELLED" : "FAILED",
                (unsigned)_idx, (unsigned)_len, (unsigned)err, (unsigned)reason);
}

bool HidTyper::cancel(uint8_t reason) {
  bool active = isActive();

  // 即使没有活动任务也发一份全释放报告: CANCEL 的语义是"确保没有按下的键"。
  releaseAll();
  _flags |= TY_FLAG_RELEASE_SENT;
  _phase = PH_IDLE;
  _leaseDeadline = 0;

  if (active) {
    _state = TY_CANCELLED;
    _cancelReason = reason;
    _lastError = (reason == CANCEL_USB_LOST) ? TY_ERR_USB_LOST
               : (reason == CANCEL_LEASE)    ? TY_ERR_HOST_LOST
                                             : TY_ERR_CANCELLED;
    Serial.printf("[hid] CANCEL: stopped at char %u/%u, reason=%u, keys released\n",
                  (unsigned)_idx, (unsigned)_len, (unsigned)reason);
  } else {
    Serial.printf("[hid] CANCEL: no active task (state=%u), keys released\n",
                  (unsigned)_state);
  }
  return active;
}

HidTyper::Status HidTyper::status() const {
  Status st;
  st.state = _state;
  st.processed = _idx;
  st.total = _len;
  st.lastError = _lastError;
  st.cancelReason = _cancelReason;
  st.flags = _flags;
  if (s_hid && s_hid->ready()) st.flags |= TY_FLAG_HID_READY;

  uint32_t remain = 0;
  if (_hostInitiated && isActive() && _leaseDeadline) {
    uint32_t now = millis();
    if (!timeReached(now, _leaseDeadline)) remain = _leaseDeadline - now;
  }
  if (remain > 0xFFFFu) remain = 0xFFFFu;
  st.leaseRemainingMs = (uint16_t)remain;
  st.sessionId = _sessionId;
  return st;
}

void HidTyper::noteHostContact() {
  if (_hostInitiated && isActive()) {
    _leaseDeadline = millis() + HOST_LEASE_TIMEOUT_MS;
  }
}
