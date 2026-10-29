#include <Arduino.h>
#include "trigger_button.h"

#define DEBOUNCE_MS 30

void TriggerButton::begin(uint8_t pin) {
  _pin = pin;
  pinMode(_pin, INPUT_PULLUP);        // BOOT 键: 按下 = LOW
  bool released = digitalRead(_pin) == HIGH;
  _candidate = released;
  _stable = released;
  _armed = false;                     // 上电后必须先看到释放才允许触发
  _lastChange = millis();
}

bool TriggerButton::poll() {
  bool raw = (digitalRead(_pin) == HIGH);
  unsigned long now = millis();

  if (raw != _candidate) {
    _candidate = raw;
    _lastChange = now;
  }
  if (_candidate != _stable && (now - _lastChange) >= DEBOUNCE_MS) {
    _stable = _candidate;
    if (_stable == false) {           // 消抖后的按下
      if (_armed) {
        _armed = false;               // 触发一次后上锁，直到释放
        return true;
      }
    } else {                          // 消抖后的释放
      _armed = true;
    }
  }
  return false;
}
