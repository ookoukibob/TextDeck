#pragma once
// Host-side test HAL for firmware/esp32_hid_text_device/hid_typer.cpp.
// It implements only the small surface the production state machine uses.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

struct FakeSerial {
  void printf(const char *fmt, ...) {
    (void)fmt;
  }
  void println(const char *s) {
    (void)s;
  }
};

extern FakeSerial Serial;

uint32_t millis();

class USBHIDKeyboard {
public:
  size_t press(uint8_t k);
  size_t release(uint8_t k);
  void releaseAll();

  // test observability
  int pressCount = 0;
  int releaseCount = 0;
  int releaseAllCount = 0;
  int lastPressed = -1;
  int lastReleased = -1;
  bool anyKeyDown = false;
};

class USBHID {
public:
  bool ready();

  // test control
  bool isReady = true;
};

// Test-side controls.
extern uint32_t g_fakeMillis;
extern bool g_fakeHidReady;
void fakeAdvance(uint32_t ms, class HidTyper *typer);
