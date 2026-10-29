#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"
#include "text_store.h"
#include "hid_typer.h"

// 配置/控制链路: 解析协议帧, 校验后写入/读取 TextStore, 驱动输入状态机,
// 返回带 CRC 的响应帧。
//
// v1.2 同时支持两个独立实例:
//   * 原生 USB CDC (ControlSerial): 日常单线控制通道, 只传输二进制协议帧,
//     绝不写入调试日志。
//   * UART0 / FTDI (Serial): 保留给刷写、调试日志和旧桌面工具; 协议帧与
//     日志共享同一物理串口 (与 v1.1 行为一致)。
class ConfigLink {
public:
  void begin(TextStore *store, Stream *io, HidTyper *typer, bool usbPort);
  void poll();   // 在 loop() 中持续调用

private:
  enum State { S_SYNC0, S_SYNC1, S_VER, S_CMD, S_LEN0, S_LEN1, S_CRC, S_PAYLOAD, S_CHECK };

  void handleFrame(uint8_t cmd, const uint8_t *payload, uint16_t len);
  void finishFrame();
  void sendFrame(uint8_t cmd, const uint8_t *payload, uint16_t len);
  void resetParser();
  void resync(uint8_t b);
  void fillTypingStatus(uint8_t *pl, uint8_t requestStatus);
  void logLine(const char *fmt, ...);

  State _state = S_SYNC0;
  uint8_t _cmd = 0;
  uint16_t _len = 0;
  uint8_t _crcBytes[4];
  uint8_t _crcIdx = 0;
  uint32_t _crc = 0;
  uint16_t _got = 0;
  // 接收缓冲: 随对象静态存在 (ConfigLink 是全局对象), 不在栈上。
  // 10253 B; 每个实例一份。USB 与 UART 各一个实例, 共约 20.5 KiB。
  uint8_t _payload[PROTO_MAX_PAYLOAD];
  uint32_t _frameCount = 0;

  TextStore *_store = nullptr;
  Stream *_io = nullptr;
  HidTyper *_typer = nullptr;
  bool _usbPort = false;
};
