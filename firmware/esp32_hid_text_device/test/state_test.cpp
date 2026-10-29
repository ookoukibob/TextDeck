// Host-side unit tests for the firmware HidTyper non-blocking state machine.
// Compiled with HID_TYPER_HOST_TEST against the real production hid_typer.cpp.
#include <stdio.h>
#include <string.h>
#include "host_test_hal.h"
#include "hid_typer.h"

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond) do { \
  g_checks++; \
  if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fail++; \
  } \
} while (0)

static void resetHal() {
  g_fakeMillis = 0;
  g_fakeHidReady = true;
}

struct Fixture {
  USBHIDKeyboard kb;
  USBHID hid;
  HidTyper typer;
  Fixture() {
    resetHal();
    typer.attach(&kb, &hid);
  }
  void at(uint32_t t) {
    g_fakeMillis = t;
    typer.poll();
  }
};

static void testNormalCompletion() {
  Fixture f;
  const uint8_t text[] = "Ab";
  CHECK(f.typer.beginTypeText(text, 2, false) == HidTyper::START_OK);
  CHECK(f.typer.status().state == TY_PREPARING);

  f.at(79);
  CHECK(f.kb.pressCount == 0);
  f.at(80);
  CHECK(f.kb.pressCount == 1);
  CHECK(f.kb.lastPressed == 'A');
  f.at(88);
  CHECK(f.kb.releaseCount == 1);
  CHECK(f.kb.lastReleased == 'A');
  f.at(103);
  CHECK(f.kb.pressCount == 2);
  CHECK(f.kb.lastPressed == 'b');
  f.at(111);
  CHECK(f.kb.releaseCount == 2);
  f.at(126);

  HidTyper::Status st = f.typer.status();
  CHECK(st.state == TY_COMPLETED);
  CHECK(st.processed == 2);
  CHECK(st.total == 2);
  CHECK((st.flags & TY_FLAG_RELEASE_SENT) != 0);
  CHECK(f.kb.releaseAllCount >= 1);
  CHECK(!f.kb.anyKeyDown);
  CHECK(!f.typer.isActive());
}

static void testCancelStopsImmediately() {
  Fixture f;
  const uint8_t text[] = "abcdef";
  CHECK(f.typer.beginTypeText(text, 6, true) == HidTyper::START_OK);
  f.at(80);
  CHECK(f.kb.pressCount == 1);
  CHECK(f.kb.anyKeyDown);

  CHECK(f.typer.cancel(CANCEL_HOST) == true);
  HidTyper::Status st = f.typer.status();
  CHECK(st.state == TY_CANCELLED);
  CHECK(st.cancelReason == CANCEL_HOST);
  CHECK(st.processed == 0);
  CHECK(!f.kb.anyKeyDown);
  CHECK(f.kb.releaseAllCount >= 1);

  int presses = f.kb.pressCount;
  f.at(500);
  CHECK(f.kb.pressCount == presses);   // no new key reports after CANCEL
  CHECK(f.typer.cancel(CANCEL_HOST) == false);  // repeatable no-op
}

static void testSecondStartBusy() {
  Fixture f;
  const uint8_t text[] = "abc";
  CHECK(f.typer.beginTypeText(text, 3, true) == HidTyper::START_OK);
  CHECK(f.typer.beginTypeText(text, 3, true) == HidTyper::START_BUSY);
  f.at(80);
  CHECK(f.kb.pressCount == 1);
}

static void testRejectNoTextAndHidNotReady() {
  Fixture f;
  CHECK(f.typer.beginTypeText(nullptr, 0, true) == HidTyper::START_NO_TEXT);

  const uint8_t text[] = "x";
  g_fakeHidReady = false;
  CHECK(f.typer.beginTypeText(text, 1, false) == HidTyper::START_HID_NOT_READY);
}

static void testDisconnectAbortsWithoutResume() {
  Fixture f;
  const uint8_t text[] = "abcd";
  CHECK(f.typer.beginTypeText(text, 4, false) == HidTyper::START_OK);
  f.at(80);
  CHECK(f.kb.pressCount == 1);

  g_fakeHidReady = false;
  f.at(81);                       // start grace window
  CHECK(f.typer.status().state == TY_TYPING);
  f.at(91);                       // grace expired -> abort
  HidTyper::Status st = f.typer.status();
  CHECK(st.state == TY_FAILED);
  CHECK(st.lastError == TY_ERR_USB_LOST);
  CHECK(st.cancelReason == CANCEL_USB_LOST);
  CHECK(!f.kb.anyKeyDown);
  CHECK(f.kb.releaseAllCount >= 1);

  int presses = f.kb.pressCount;
  g_fakeHidReady = true;          // reconnect must NOT resume
  for (uint32_t t = 100; t < 2000; t += 10) f.at(t);
  CHECK(f.kb.pressCount == presses);
  CHECK(f.typer.status().state == TY_FAILED);
}

static void testHostLeaseTimeout() {
  Fixture f;
  uint8_t text[300];
  memset(text, 'a', sizeof(text));
  CHECK(f.typer.beginTypeText(text, sizeof(text), true) == HidTyper::START_OK);
  for (uint32_t t = 0; t <= 2999; t += 10) f.at(t);
  CHECK(f.typer.status().state == TY_TYPING || f.typer.status().state == TY_PREPARING);

  f.at(3001);
  HidTyper::Status st = f.typer.status();
  CHECK(st.state == TY_FAILED);
  CHECK(st.lastError == TY_ERR_HOST_LOST);
  CHECK(st.cancelReason == CANCEL_LEASE);
  CHECK(!f.kb.anyKeyDown);
}

static void testHostHeartbeatKeepsAlive() {
  Fixture f;
  uint8_t text[300];
  memset(text, 'a', sizeof(text));
  CHECK(f.typer.beginTypeText(text, sizeof(text), true) == HidTyper::START_OK);
  for (uint32_t t = 0; t <= 4001; t += 10) {
    if (t == 1000 || t == 2000 || t == 3000 || t == 4000) f.typer.noteHostContact();
    f.at(t);
  }
  CHECK(f.typer.status().state == TY_TYPING);
  CHECK(f.typer.status().lastError == TY_ERR_NONE);
}

static void testUnsupportedCharRejectedBeforeAnyKey() {
  Fixture f;
  const uint8_t text[] = { 'a', 0xE9, 'b' };
  CHECK(f.typer.beginTypeText(text, 3, true) == HidTyper::START_INVALID);
  CHECK(f.kb.pressCount == 0);
  CHECK(f.typer.status().state == TY_IDLE);
}

static void testCancelWithoutActiveTask() {
  Fixture f;
  CHECK(f.typer.cancel(CANCEL_HOST) == false);
  CHECK(f.typer.status().state == TY_IDLE);
  CHECK(!f.kb.anyKeyDown);
  CHECK(f.kb.releaseAllCount >= 1);
}

int main() {
  testNormalCompletion();
  testCancelStopsImmediately();
  testSecondStartBusy();
  testRejectNoTextAndHidNotReady();
  testDisconnectAbortsWithoutResume();
  testHostLeaseTimeout();
  testHostHeartbeatKeepsAlive();
  testUnsupportedCharRejectedBeforeAnyKey();
  testCancelWithoutActiveTask();

  printf("firmware state tests: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
