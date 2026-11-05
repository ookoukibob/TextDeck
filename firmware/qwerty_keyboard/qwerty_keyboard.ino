// qwerty_keyboard.ino —— ESP32-S3 USB HID 全功能 QWERTY 触摸键盘
//
// 320x240 横屏，完整 QWERTY 布局 + 数字 + 符号 + 功能键。
// 每次触摸产生一次完整的 HID 按下/释放，不会卡键。
//
// USB 配置（必须严格一致，理由见 README）:
//   FQBN: esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=default,CDCOnBoot=cdc
//
// 接线（沿用已验证驱动默认值）:
//   LCD   CS=10 DC=9 RST=8 MOSI=11 SCK=12 MISO=13  VCC=5V(J1断开) LED=3V3
//   Touch T_CLK=4 T_DIN=15 T_DO=7 T_CS=6 T_IRQ=5

#include <Arduino.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

#include "src/MSP2402.h"
#include "src/MSP2402_Touch.h"
#include "keymap.h"
#include "layout.h"
#include "input_buffer.h"

// ---------------- 全局 ----------------
static USBHIDKeyboard Keyboard;
static MSP2402        lcd;
static MSP2402_Touch  tp;
static InputBuffer    input;

static bool g_shift = false;
static bool g_caps  = false;
static bool g_ctrl  = false;
static bool g_symbol = false;      // 符号层
static bool g_hidReady = false;

// 视觉反馈
static int  g_pressRow = -1;
static int  g_pressCol = -1;
static uint32_t g_pressUntil = 0;

// ---------------- 配色 ----------------
static const uint16_t C_BG       = 0x0000;
static const uint16_t C_STATUS   = 0x2104;
static const uint16_t C_PREVIEW  = 0x1082;
static const uint16_t C_KEY      = 0x4A69;
static const uint16_t C_KEY_FN   = 0x2945;
static const uint16_t C_KEY_ACT  = 0x07E0;
static const uint16_t C_KEY_ON   = 0xFD20;
static const uint16_t C_TEXT     = 0xFFFF;
static const uint16_t C_TEXT_DIM = 0xC618;
static const uint16_t C_ACCENT   = 0x07FF;
static const uint16_t C_BORDER   = 0x18E3;

// ---------------- HID ----------------
//
// 必须用 pressRaw()：press() 会把参数当 ASCII 查表，
// 传 HID usage 会得到完全错误的字符（实测 "!@#" 变成 "((("）。
// 修饰键是 usage 而不是 report 的 modifier 位：
//   LeftCtrl=0xE0  LeftShift=0xE1  LeftAlt=0xE2  LeftGUI=0xE3

#define HID_MOD_LCTRL   0xE0
#define HID_MOD_LSHIFT  0xE1
#define HID_MOD_LALT    0xE2

static const uint16_t HID_HOLD_MS = 22;
static const uint16_t HID_GAP_MS  = 18;

static void emitKeyRaw(uint8_t usage, bool shift, bool ctrl) {
  if (usage == 0) return;

  if (shift) Keyboard.pressRaw(HID_MOD_LSHIFT);
  if (ctrl)  Keyboard.pressRaw(HID_MOD_LCTRL);
  if (shift || ctrl) delay(6);

  Keyboard.pressRaw(usage);
  delay(HID_HOLD_MS);

  Keyboard.releaseRaw(usage);
  delay(6);

  if (shift) Keyboard.releaseRaw(HID_MOD_LSHIFT);
  if (ctrl)  Keyboard.releaseRaw(HID_MOD_LCTRL);

  Keyboard.releaseAll();
  delay(HID_GAP_MS);
}

static void emitChar(char c) {
  uint8_t usage = 0;
  bool needShift = false;
  if (!keymap::charToHid(c, &usage, &needShift)) return;
  emitKeyRaw(usage, needShift, g_ctrl);
}

static void emitUsage(uint8_t usage) {
  emitKeyRaw(usage, false, g_ctrl);
}

// ---------------- 绘制 ----------------

static void drawStatusBar() {
  lcd.Fill_Rect(0, kb::STATUS_Y, kb::SCREEN_W, kb::STATUS_H, C_STATUS);
  lcd.Set_Text_Size(1);
  lcd.Set_Text_Transparent();

  lcd.Print_String(g_hidReady ? "USB OK" : "USB --", 3, 3,
                   g_hidReady ? 0x07E0 : 0xF800, C_STATUS);
  lcd.Print_String(g_shift ? "SH" : "--", 58, 3,
                   g_shift ? C_KEY_ON : C_TEXT_DIM, C_STATUS);
  lcd.Print_String(g_caps ? "CAPS" : "----", 82, 3,
                   g_caps ? C_KEY_ON : C_TEXT_DIM, C_STATUS);
  lcd.Print_String(g_ctrl ? "CTL" : "---", 124, 3,
                   g_ctrl ? C_KEY_ON : C_TEXT_DIM, C_STATUS);
  lcd.Print_String(g_symbol ? "SYM" : "ABC", 156, 3,
                   g_symbol ? C_ACCENT : C_TEXT_DIM, C_STATUS);

  char buf[12];
  snprintf(buf, sizeof(buf), "%d", input.length());
  lcd.Print_String(buf, kb::SCREEN_W - 26, 3, C_ACCENT, C_STATUS);
}

static void drawPreview() {
  lcd.Fill_Rect(0, kb::PREVIEW_Y, kb::SCREEN_W, kb::PREVIEW_H, C_PREVIEW);
  lcd.Set_Text_Size(1);
  lcd.Set_Text_Transparent();

  const int lineH = 15;
  const int maxLines = kb::PREVIEW_H / lineH;         // 3 行
  const int charsPerLine = (kb::SCREEN_W - 10) / 6;   // 51 字符

  const char *s = input.data();
  int len = input.length();

  int totalLines = 1, cur = 0;
  for (int i = 0; i < len; i++) {
    if (s[i] == '\n') { totalLines++; cur = 0; }
    else if (++cur > charsPerLine) { totalLines++; cur = 1; }
  }
  int skip = (totalLines > maxLines) ? (totalLines - maxLines) : 0;

  int i = 0, line = 0;
  cur = 0;
  while (i < len && line < skip) {
    if (s[i] == '\n') { line++; cur = 0; }
    else if (++cur > charsPerLine) { line++; cur = 1; }
    i++;
  }

  int x = 5, y = kb::PREVIEW_Y + 2;
  cur = 0;
  int drawn = 0;
  int lastX = x, lastY = y;

  for (; i < len && drawn < maxLines; i++) {
    if (s[i] == '\n') {
      x = 5; y += lineH; cur = 0; drawn++;
      lastX = x; lastY = y;
      continue;
    }
    if (cur >= charsPerLine) {
      x = 5; y += lineH; cur = 0; drawn++;
    }
    char one[2] = { s[i], 0 };
    lcd.Print_String(one, x, y, C_TEXT, C_PREVIEW);
    x += 6; cur++;
    lastX = x; lastY = y;
  }

  if (len == 0) {
    lcd.Set_Text_Size(2);
    lcd.Print_String("Ready", 6, kb::PREVIEW_Y + 4, C_TEXT_DIM, C_PREVIEW);
    lcd.Set_Text_Size(1);
    lcd.Print_String("tap keys to type", 6, kb::PREVIEW_Y + 26, 0x7BEF, C_PREVIEW);
  }

  if ((millis() / 500) & 1) {
    if (lastY <= kb::PREVIEW_Y + kb::PREVIEW_H - lineH) {
      lcd.Fill_Rect(lastX, lastY + 1, 2, 12, C_ACCENT);
    }
  }
}

static uint16_t keyColor(const keymap::KeyDef &k, bool pressed) {
  if (pressed) return C_KEY_ACT;

  switch (k.kind) {
    case keymap::K_SHIFT:     return g_shift ? C_KEY_ON : C_KEY_FN;
    case keymap::K_CAPSLOCK:  return g_caps  ? C_KEY_ON : C_KEY_FN;
    case keymap::K_CTRL:      return g_ctrl  ? C_KEY_ON : C_KEY_FN;
    case keymap::K_LAYER:     return g_symbol ? C_ACCENT : C_KEY_FN;
    case keymap::K_ENTER:
    case keymap::K_BACKSPACE: return 0x2945;
    default:                  return (k.kind == keymap::K_CHAR) ? C_KEY : C_KEY_FN;
  }
}

static void drawKey(int row, int col, bool pressed) {
  kb::Rect r = kb::keyRect(row, col);
  const keymap::KeyDef &k = kb::keyAt(row, col);

  uint16_t bg = keyColor(k, pressed);
  lcd.Fill_Rect(r.x, r.y, r.w, r.h, bg);
  lcd.Draw_Rect(r.x, r.y, r.w, r.h, C_BORDER);

  // 亮底用黑字，暗底用白字
  uint16_t fg = C_TEXT;
  if (bg == C_KEY_ACT || bg == C_KEY_ON || bg == C_ACCENT) fg = 0x0000;

  lcd.Set_Text_Size(1);
  lcd.Set_Text_Transparent();

  // 字符键按当前 Shift/Caps 显示实际会打出的字符
  char label[8];
  if (k.kind == keymap::K_CHAR) {
    char show;
    if (keymap::isAlpha(k.base)) {
      bool upper = (g_shift != g_caps);
      show = upper ? (char)(k.base - 32) : k.base;
    } else {
      show = g_shift ? k.shifted : k.base;
    }
    label[0] = show;
    label[1] = 0;
  } else {
    strncpy(label, k.label ? k.label : "", sizeof(label) - 1);
    label[sizeof(label) - 1] = 0;
  }

  int tw = (int)strlen(label) * 6;
  int tx = r.x + (r.w - tw) / 2;
  if (tx < r.x + 1) tx = r.x + 1;
  int ty = r.y + (r.h - 8) / 2;
  lcd.Print_String(label, tx, ty, fg, bg);
}

static void drawKeyboard(int pressRow = -1, int pressCol = -1) {
  lcd.Fill_Rect(0, kb::KB_Y, kb::SCREEN_W, kb::SCREEN_H - kb::KB_Y, C_BG);
  for (int r = 0; r < kb::ROW_COUNT; r++) {
    for (int c = 0; c < kb::ROWS[r].count; c++) {
      drawKey(r, c, (r == pressRow && c == pressCol));
    }
  }
}

static void drawAll() {
  drawStatusBar();
  drawPreview();
  drawKeyboard();
}

// ---------------- 按键处理 ----------------

static void handleKey(int row, int col) {
  const keymap::KeyDef &k = kb::keyAt(row, col);

  switch (k.kind) {
    case keymap::K_CHAR: {
      uint8_t usage;
      bool needShift;
      keymap::resolveCharKey(k, g_shift, g_caps, &usage, &needShift);
      emitKeyRaw(usage, needShift, g_ctrl);

      char ch;
      if (keymap::isAlpha(k.base)) {
        bool upper = (g_shift != g_caps);
        ch = upper ? (char)(k.base - 32) : k.base;
      } else {
        ch = g_shift ? k.shifted : k.base;
      }
      input.append(ch);

      if (g_shift) g_shift = false;      // Shift 一次性
      if (g_ctrl)  g_ctrl  = false;      // Ctrl 一次性，避免后续输入全被组合
      return;
    }

    case keymap::K_SHIFT:    g_shift = !g_shift; return;
    case keymap::K_CAPSLOCK: g_caps  = !g_caps;  return;
    case keymap::K_CTRL:     g_ctrl  = !g_ctrl;  return;
    case keymap::K_LAYER:    g_symbol = !g_symbol; return;

    case keymap::K_BACKSPACE:
      emitUsage(keymap::U_BACKSPACE);
      input.backspace();
      if (g_shift) g_shift = false;
      return;

    case keymap::K_ENTER:
      emitUsage(keymap::U_ENTER);
      input.newline();
      return;

    case keymap::K_TAB:
      emitUsage(keymap::U_TAB);
      input.append('\t');
      return;

    case keymap::K_SPACE:
      emitUsage(keymap::U_SPACE);
      input.append(' ');
      return;

    case keymap::K_ESC:
      emitUsage(keymap::U_ESC);
      return;

    case keymap::K_UP:    emitUsage(keymap::U_UP);    return;
    case keymap::K_DOWN:  emitUsage(keymap::U_DOWN);  return;
    case keymap::K_LEFT:  emitUsage(keymap::U_LEFT);  return;
    case keymap::K_RIGHT: emitUsage(keymap::U_RIGHT); return;
    case keymap::K_DELETE: emitUsage(keymap::U_DELETE); return;

    default:
      return;
  }
}

// ---------------- setup / loop ----------------

void setup() {
  lcd.Init();
  lcd.Set_Rotation(1);              // 320x240 横屏 (MADCTL 0xA0)

  tp.Init(lcd.Get_Width(), lcd.Get_Height());
  tp.Set_Rotation(1);

  lcd.Fill_Screen(C_BG);
  input.clear();

  Keyboard.begin(KeyboardLayout_en_US);
  USB.begin();
  delay(600);
  g_hidReady = true;

  drawAll();
}

void loop() {
  uint32_t now = millis();

  // 按键高亮复位
  if (g_pressRow >= 0 && (int32_t)(now - g_pressUntil) > 0) {
    int r = g_pressRow, c = g_pressCol;
    g_pressRow = -1;
    g_pressCol = -1;
    drawKey(r, c, false);
  }

  int16_t tx, ty;
  if (!tp.Read_Stable(&tx, &ty)) {
    delay(6);
    return;
  }

  int row, col;
  if (!kb::hitTest(tx, ty, &row, &col)) {
    delay(6);
    return;
  }

  // 视觉反馈
  g_pressRow = row;
  g_pressCol = col;
  g_pressUntil = now + 110;
  drawKey(row, col, true);

  handleKey(row, col);

  // 刷新受影响区域
  drawStatusBar();
  drawPreview();

  // 修饰键/层切换需要重绘整排（颜色变化）
  keymap::KeyKind kk = kb::keyAt(row, col).kind;
  if (kk == keymap::K_SHIFT || kk == keymap::K_CAPSLOCK ||
      kk == keymap::K_CTRL  || kk == keymap::K_LAYER) {
    drawKeyboard();
  }

  // 等松手，避免一次触摸触发多次
  uint32_t t0 = millis();
  while (tp.Is_Pressed() && (millis() - t0) < 700) {
    delay(8);
  }
  delay(25);
}
