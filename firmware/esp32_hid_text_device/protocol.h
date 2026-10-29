#pragma once
#include <stdint.h>
#include <stddef.h>

// ============================================================
// 串口配置协议 (PC <-> ESP32)，小端序，与 desktop/ 及 Windows 端完全一致
// 帧格式:
//   [0]      magic0 = 0xE7
//   [1]      magic1 = 0x53
//   [2]      版本   = 0x01
//   [3]      cmd
//   [4..5]   payload 长度 u16 LE
//   [6..9]   payload 的 CRC32 (IEEE, 与 zlib.crc32 一致) u32 LE
//   [10..]   payload
// 响应帧: cmd = 请求 cmd | 0x80
//
// v1.2 在保持 v1 命令 (WRITE/READ/PING) 的线上格式、响应长度和 CRC 语义
// 完全不变的前提下，新增 START/CANCEL/GET_STATUS 三个远程输入控制命令。
// ============================================================

#define FW_VERSION_STR  "1.2.0"
#define FW_VERSION      0x0102

#define PROTO_MAGIC0    0xE7
#define PROTO_MAGIC1    0x53
#define PROTO_VERSION   0x01
#define PROTO_HEADER_LEN 10

#define MAX_TEXT_LEN    10240                     // 最大文本字节数 (10 KiB, ASCII 每字符 1 字节)
#define PROTO_MAX_PAYLOAD (13 + MAX_TEXT_LEN)     // READ 响应 = 状态头13B + 文本 → 10253
// 最大完整帧 = 10 (header) + 10253 (payload) = 10263 字节

// 命令 (0x01..0x03 为 v1 原有命令，行为不得改变)
#define CMD_WRITE_TEXT    0x01
#define CMD_READ_TEXT     0x02
#define CMD_PING          0x03
#define CMD_START_TYPING  0x04
#define CMD_CANCEL_TYPING 0x05
#define CMD_GET_STATUS    0x06

// 状态码 (0..7 为 v1 原有状态码，保持不变)
#define ST_OK                0
#define ST_ERR_CRC           1   // CRC 校验失败
#define ST_ERR_LENGTH        2   // 长度非法 / 超出上限
#define ST_ERR_CHARSET       3   // 含不支持的字符 (detail = 首个坏字符下标)
#define ST_ERR_STORAGE       4   // NVS/SPIFFS 存储失败
#define ST_ERR_CMD           5   // 未知命令
#define ST_ERR_BUSY          6   // 设备忙碌 (已有输入任务)
#define ST_ERR_FRAME         7   // 帧格式错误
#define ST_ERR_NO_TEXT       8   // 设备中没有可输入的文本
#define ST_ERR_HID_NOT_READY 9   // USB HID 未枚举/不可用

// ---- 远程输入任务状态 (GET_STATUS/START/CANCEL 响应 [1]) ----
#define TY_IDLE        0
#define TY_PREPARING   1
#define TY_TYPING      2
#define TY_COMPLETED   3
#define TY_CANCELLED   4
#define TY_FAILED      5

// ---- 最近错误码 (响应 [6]) ----
#define TY_ERR_NONE          0
#define TY_ERR_NO_TEXT       1
#define TY_ERR_HID_NOT_READY 2
#define TY_ERR_BUSY          3
#define TY_ERR_BAD_CHAR      4
#define TY_ERR_PRESS_REJECT  5
#define TY_ERR_USB_LOST      6
#define TY_ERR_HOST_LOST     7
#define TY_ERR_CANCELLED     8

// ---- 取消原因 (响应 [7]) ----
#define CANCEL_NONE        0
#define CANCEL_HOST        1   // Windows 托盘程序发送 CANCEL
#define CANCEL_LOCAL       2   // 本地 BOOT 触发/设备侧主动停止
#define CANCEL_LEASE       3   // 主机控制租约超时
#define CANCEL_USB_LOST    4   // USB HID 断开

// ---- 状态标志 (响应 [8]) ----
#define TY_FLAG_HOST_INITIATED 0x01
#define TY_FLAG_RELEASE_SENT   0x02
#define TY_FLAG_HID_READY      0x04

// ---- 固件能力 (状态响应 [14..15], u16) ----
#define FW_CAP_REMOTE_TYPING 0x0001
#define FW_CAP_HOST_LEASE    0x0002
#define FW_CAP_NATIVE_CDC    0x0004
#define FW_CAPABILITIES      (FW_CAP_REMOTE_TYPING | FW_CAP_HOST_LEASE | FW_CAP_NATIVE_CDC)

// 统一 16 字节输入状态响应 (START/CANCEL/GET_STATUS 响应 payload 固定 16B):
//   [0]     请求处理状态码 (ST_*)
//   [1]     任务状态 (TY_*)
//   [2..3]  已处理字符数 u16
//   [4..5]  总字符数 u16
//   [6]     最近错误码 (TY_ERR_*)
//   [7]     取消原因 (CANCEL_*)
//   [8]     标志 (TY_FLAG_*)
//   [9]     保留 (0)
//   [10..11] 主机租约剩余毫秒 u16 (非主机任务/无租约 = 0)
//   [12..13] 任务会话号 u16 (每次接受的 START +1)
//   [14..15] 固件能力位 (FW_CAP_*)
#define TY_STATUS_PAYLOAD_LEN 16

// 主机控制租约: 输入期间 Windows 每 250~500ms 查询一次状态。
// 3s 允许约 6~12 次连续丢失，远高于正常 USB CDC 传输抖动。
#define HOST_LEASE_TIMEOUT_MS 3000

// 触发按键: BOOT 键 (GPIO0, 内部上拉, 低电平有效)
#define TRIGGER_PIN     0

// ---- CRC32 (多项式 0xEDB88320, 初值/终值异或 0xFFFFFFFF) ----
// 分段版本: 供文件系统逐块校验使用 (状态跨块传递, 结果与一次性计算完全一致)
static inline uint32_t proto_crc32_cont(uint32_t state, const uint8_t *data, size_t len) {
  uint32_t crc = state;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
  }
  return crc;
}
static inline uint32_t proto_crc32(const uint8_t *data, size_t len) {
  return proto_crc32_cont(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

static inline void put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static inline void put_u32(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static inline uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 支持的字符: 可打印 ASCII 0x20..0x7E + 换行(0x0A) + 制表(0x09)
// 与 KeyboardLayout_en_US 的映射一致 (CR 0x0D 不在映射表中，故拒绝)
static inline bool proto_char_supported(uint8_t c) {
  return (c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\t';
}
