#include "host_test_hal.h"
#include "hid_typer.h"

FakeSerial Serial;
uint32_t g_fakeMillis = 0;
bool g_fakeHidReady = true;

uint32_t millis() {
  return g_fakeMillis;
}

size_t USBHIDKeyboard::press(uint8_t k) {
  pressCount++;
  lastPressed = k;
  anyKeyDown = true;
  return 1;
}

size_t USBHIDKeyboard::release(uint8_t k) {
  releaseCount++;
  lastReleased = k;
  anyKeyDown = false;
  return 1;
}

void USBHIDKeyboard::releaseAll() {
  releaseAllCount++;
  anyKeyDown = false;
}

bool USBHID::ready() {
  return g_fakeHidReady;
}
