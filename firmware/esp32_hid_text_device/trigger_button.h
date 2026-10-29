#pragma once
#include <stdint.h>
#include <stdbool.h>

// 实体按键触发 (BOOT/GPIO0, 内部上拉, 低有效)
// - 消抖 30ms
// - 每次有效按下只触发一次 (触发后必须先释放才允许下一次)
// - 上电时若按键已被按住，不会视为触发 (需先释放)
class TriggerButton {
public:
  void begin(uint8_t pin);
  bool poll();   // 返回 true 表示出现一次新的有效按下
private:
  uint8_t _pin = 0;
  bool _candidate = true;     // 原始采样 (true=释放)
  bool _stable = true;        // 消抖后的状态
  bool _armed = false;        // 是否允许触发 (上电先要求释放一次)
  unsigned long _lastChange = 0;
};
