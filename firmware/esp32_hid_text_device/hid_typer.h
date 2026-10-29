#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

// USB HID 键盘逐字符输入 (原生 USB OTG / TinyUSB)
//
// v1.2: 输入不再是阻塞式 typeText()。beginTypeText() 只做校验并立即返回，
// loop() 中的 poll() 按原有节奏 (首字符 80ms / 按住 8ms / 字符间隔 15ms)
// 推进状态机，因此串口 CANCEL/STATUS 命令在输入期间仍能被持续处理。
class HidTyper {
public:
  enum StartResult : uint8_t {
    START_OK = 0,
    START_BUSY,
    START_NO_TEXT,
    START_HID_NOT_READY,
    START_INVALID,
  };

  struct Status {
    uint8_t  state;            // TY_*
    uint16_t processed;
    uint16_t total;
    uint8_t  lastError;        // TY_ERR_*
    uint8_t  cancelReason;     // CANCEL_*
    uint8_t  flags;            // TY_FLAG_*
    uint16_t leaseRemainingMs;
    uint16_t sessionId;
  };

  // keyboard: USBHIDKeyboard 实例; hidProbe: 用于查询 HID 是否就绪
  void attach(void *keyboard, void *hidProbe);

  // 校验并启动一次输入。立即返回；hostInitiated=true 时启用主机租约，
  // 需要主机在输入期间持续发送任何有效协议帧 (状态查询即可) 来续租。
  StartResult beginTypeText(const uint8_t *data, uint16_t len, bool hostInitiated);

  // 在 loop() 中持续调用，推进输入状态机并检测 USB/租约异常。
  void poll();

  // 立即停止新按键并释放全部按键；返回 true 表示确实停止了活动任务。
  // 本调用在返回前完成 releaseAll()，因此 CANCEL 响应可以确认已释放。
  bool cancel(uint8_t reason);

  bool isActive() const {
    return _state == TY_PREPARING || _state == TY_TYPING;
  }

  Status status() const;

  // 主机每发送一帧有效协议请求时调用，刷新租约。
  void noteHostContact();

private:
  enum Phase : uint8_t {
    PH_IDLE = 0,
    PH_PRE,
    PH_PRESS,
    PH_RELEASE,
    PH_GAP,
  };

  void abortRound(uint8_t state, uint8_t err, uint8_t reason);
  void completeRound();
  void releaseAll();
  void pressCurrent(uint32_t now);

  uint8_t  _state = TY_IDLE;
  uint8_t  _phase = PH_IDLE;
  uint8_t  _lastError = TY_ERR_NONE;
  uint8_t  _cancelReason = CANCEL_NONE;
  uint8_t  _flags = 0;

  const uint8_t *_data = nullptr;
  uint16_t _len = 0;
  uint16_t _idx = 0;

  bool     _hostInitiated = false;
  bool     _keyDown = false;

  uint32_t _nextAt = 0;
  uint32_t _leaseDeadline = 0;
  uint32_t _notReadySince = 0xFFFFFFFFu;
  uint16_t _sessionId = 0;
};
