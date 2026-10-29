/*
 * esp32_hid_text_device - ESP32-S3 独立 USB HID 文本输入器
 *
 * v1.2 架构 (单线原生 USB 复合设备):
 *   原生 USB OTG (TinyUSB)  -> 同一接口同时枚举:
 *                                - 标准 USB HID Keyboard
 *                                - USB CDC 控制端口 (二进制协议, 无日志)
 *   FTDI UART0 (可选)       -> 刷写固件、调试日志、旧桌面工具兼容
 *   BOOT 键 (GPIO0)         -> 本地触发一次 HID 文本输入 (可选功能)
 *   SPIFFS 双槽位文件       -> 断电保存文本 (单条上限 10 KiB)
 *
 * 行为约束:
 *   - 上电只恢复文本, 不自动输入, 不发送任何测试字符串
 *   - Windows 通过 CDC 发送 START/CANCEL/GET_STATUS; 输入由非阻塞状态机执行
 *   - 仅在有效 START 或本地 BOOT 后输入; 输入中忽略新的 START/BOOT
 *   - 取消/结束时立即 releaseAll(); USB 断开或主机租约超时自动中止,
 *     重连后绝不自动续打/补发
 *
 * 关键 USB 细节 (已核对 Arduino-ESP32 3.3.12 + TinyUSB 0.21.0):
 *   - USBMode=default (0) 使用 USB-OTG/TinyUSB; USBCDC 与 USBHIDKeyboard 可组成
 *     复合设备 (官方 CompositeDevice 示例同样使用显式 USBCDC 实例)。
 *   - USBCDC::enableReboot(false) 在 3.3.12 中存在, 语义是禁用 1200bps touch
 *     以及 DTR/RTS 四步序列触发的 bootloader 重启。日常打开/关闭 COM 口
 *     不会再让 ESP32 复位或让 HID 重新枚举。
 *   - CDC 端口只写协议帧; 所有调试日志仍写 UART0/Serial, 两条通道彼此独立。
 *
 * 板型配置 (arduino-cli / Arduino IDE):
 *   FQBN: esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=default,CDCOnBoot=default
 *   UploadSpeed=115200 或 921600, EraseFlash=none (禁止全片擦除)
 */

#include <Arduino.h>
#include "USB.h"
#include "USBCDC.h"
#include "USBHIDKeyboard.h"
#include "USBHID.h"
#include "protocol.h"
#include "text_store.h"
#include "hid_typer.h"
#include "trigger_button.h"
#include "config_link.h"

static USBHIDKeyboard Keyboard;
static USBHID hidProbe;          // 仅用于查询 HID 是否就绪 (tud_hid_n_ready)
static USBCDC ControlSerial;     // 原生 USB CDC 控制端口 (协议专用, 不输出日志)
static TextStore store;
static HidTyper typer;
static TriggerButton trigger;
static ConfigLink cfgUsb;
static ConfigLink cfgUart;

// 由 config_link.cpp 导出, PING 响应中返回设备侧 HID 挂载状态
extern uint8_t s_hidReadyState;

void setup() {
  // UART0 / FTDI: 调试日志与刷写/恢复通道。绝不与 USB CDC 混用。
  Serial.begin(115200);
  delay(100);
  Serial.println();
  Serial.println("[boot] esp32_hid_text_device fw " FW_VERSION_STR
                 " (native USB CDC+HID composite)");

  // 1. 恢复已保存文本 (不产生任何 HID 输出)
  bool storeOk = store.begin();
  bool hasText = storeOk && store.load();
  if (hasText) {
    Serial.printf("[store] restored: len=%u crc=%08X seq=%u\n",
                  store.length(), store.crc(), store.seq());
  } else {
    Serial.println("[store] no valid text saved yet");
  }

  // 2. 原生 USB 复合设备: CDC 控制端口 + HID 键盘
  //    先注册 CDC, 再注册 HID, 最后 USB.begin() 启动 TinyUSB。
  ControlSerial.setRxBufferSize(16384);   // 10 KiB WRITE 帧 + 状态查询余量
  ControlSerial.begin(115200);
  // 禁用 DTR/RTS/1200bps touch 触发的重启: Windows 打开/关闭串口不得复位设备。
  ControlSerial.enableReboot(false);
  Serial.printf("[usb] CDC reboot-on-line-state=%s\n",
                ControlSerial.rebootEnabled() ? "ENABLED" : "disabled");

  Keyboard.begin(KeyboardLayout_en_US);
  USB.begin();
  typer.attach(&Keyboard, &hidProbe);

  // 3. 触发按键与两条独立控制链路
  trigger.begin(TRIGGER_PIN);
  cfgUsb.begin(&store, &ControlSerial, &typer, true);
  cfgUart.begin(&store, &Serial, &typer, false);

  s_hidReadyState = hidProbe.ready() ? 1 : 0;
  Serial.printf("[boot] ready. trigger=GPIO%d (BOOT), max_text=%d, hid_ready=%d\n",
                TRIGGER_PIN, MAX_TEXT_LEN, s_hidReadyState);
}

void loop() {
  // 1) 控制协议: 原生 USB CDC (日常单线) + UART0/FTDI (兼容/调试)
  cfgUsb.poll();
  cfgUart.poll();

  // 2) 非阻塞 HID 输入状态机 (输入期间仍然持续执行本 loop)
  typer.poll();

  // 3) 记录 HID 挂载/断开状态变化 (设备侧客观证据), 并同步给 PING 响应
  static bool lastReady = false;
  bool nowReady = hidProbe.ready();
  if (nowReady != lastReady) {
    Serial.printf("[hid] %s\n", nowReady ? "enumerated (hid_ready=1)"
                                         : "disconnected (hid_ready=0)");
    lastReady = nowReady;
  }
  s_hidReadyState = nowReady ? 1 : 0;

  // 4) 本地 BOOT 触发 (可选兼容功能; 不是 Windows 开始输入的前置条件)
  if (trigger.poll()) {
    if (typer.isActive()) {
      Serial.println("[trigger] ignored (typing)");
    } else if (!store.valid() || store.length() == 0) {
      Serial.println("[trigger] no valid text stored, ignored");
    } else if (!nowReady) {
      Serial.println("[trigger] HID not enumerated, ignored (no auto retry)");
    } else {
      Serial.printf("[trigger] local BOOT start: %u chars\n", store.length());
      HidTyper::StartResult r =
          typer.beginTypeText(store.data(), store.length(), false);
      if (r != HidTyper::START_OK) {
        Serial.printf("[trigger] start rejected: %u\n", (unsigned)r);
      }
    }
  }
}
