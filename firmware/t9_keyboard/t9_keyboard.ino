// t9_keyboard.ino —— ESP32-S3 USB HID 九键触摸键盘
//
// 屏幕: 240x320 竖屏（MSP2402 / ILI9341 兼容）
// 输入: 九键 T9 风格，连按同键循环切换字母
// 输出: USB HID Keyboard（标准键盘，无需电脑端软件）
//
// USB 配置:
//   FQBN: esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=default,CDCOnBoot=default
//
// 接线（沿用已验证驱动默认值）:
//   LCD   CS=10 DC=9 RST=8 MOSI=11 SCK=12 MISO=13  VCC=5V(J1断开) LED=3V3
//   Touch T_CLK=4 T_DIN=15 T_DO=7 T_CS=6 T_IRQ=5
//
// 安全约束:
//   - 每次触摸只产生一次按下+释放
//   - 所有分支结束都 releaseAll()
//   - 修饰键用过即清，不会卡住

#include <Arduino.h>
#include "USB.h"
#include "USBHIDKeyboard.h"

#include "src/MSP2402.h"
#include "src/MSP2402_Touch.h"
#include "keymap.h"
#include "t9_layout.h"
#include "input_buffer.h"

// ---------------- 全局 ----------------
static USBHIDKeyboard Keyboard;
static MSP2402        lcd;
static MSP2402_Touch  tp;
static InputBuffer    input;

static bool g_shift = false;
static bool g_caps  = false;
static bool g_symbolLayer = false;    // 符号层

static bool g_hidReady = false;

// 九键连击状态
static int      g_t9LastKey = -1;     // 上次按的数字键 index
static int      g_t9Cycle   = 0;      // 当前循环到的字母序号
static uint32_t g_t9Deadline = 0;     // 连击有效期
static char     g_t9PendingChar = 0;  // 尚未提交的字母

// 视觉反馈
static int g_pressIdx = -1;
static bool g_pressIsFunc = false;
static uint32_t g_pressUntil = 0;

// ---------------- 颜色 ----------------
// 整体走"深底浅字"，避免面板在高亮时被摄像头拍成过曝白块
static const uint16_t C_BG        = 0x0000;
static const uint16_t C_STATUS    = 0x2104;
static const uint16_t C_PREVIEW   = 0x1082;
static const uint16_t C_CAND      = 0x18C3;
static const uint16_t C_KEY       = 0x4A69;   // 数字键：中灰
static const uint16_t C_KEY_DARK  = 0x2945;   // 功能键：深蓝灰（比数字键更暗，区分层级）
static const uint16_t C_KEY_ACT   = 0x07E0;   // 按下：绿
static const uint16_t C_KEY_ON    = 0xFD20;   // 激活：橙
static const uint16_t C_TEXT      = 0xFFFF;
static const uint16_t C_TEXT_DIM  = 0xC618;   // 副标签：亮灰
static const uint16_t C_ACCENT    = 0x07FF;
static const uint16_t C_BORDER    = 0x18E3;

static const uint32_t T9_TIMEOUT_MS = 1400;

// ---------------- 绘制 ----------------

static void drawStatusBar() {
  lcd.Fill_Rect(0, t9::STATUS_Y, t9::SCREEN_W, t9::STATUS_H, C_STATUS);
  lcd.Set_Text_Size(1);
  lcd.Set_Text_Transparent();

  lcd.Print_String(g_hidReady ? "USB OK" : "USB --", 3, 3,
                   g_hidReady ? 0x07E0 : 0xF800, C_STATUS);
  lcd.Print_String(g_shift ? "SH" : "--", 58, 3,
                   g_shift ? C_KEY_ON : C_TEXT_DIM, C_STATUS);
  lcd.Print_String(g_caps ? "CAPS" : "----", 82, 3,
                   g_caps ? C_KEY_ON : C_TEXT_DIM, C_STATUS);
  lcd.Print_String(g_symbolLayer ? "SYM" : "ABC", 124, 3,
                   g_symbolLayer ? C_ACCENT : C_TEXT_DIM, C_STATUS);

  char buf[12];
  snprintf(buf, sizeof(buf), "%d", input.length());
  lcd.Print_String(buf, t9::SCREEN_W - 26, 3, C_ACCENT, C_STATUS);
}

// 输入预览：显示最后几行，光标在末尾
static void drawPreview() {
  lcd.Fill_Rect(0, t9::PREVIEW_Y, t9::SCREEN_W, t9::PREVIEW_H, C_PREVIEW);
  lcd.Set_Text_Size(1);
  lcd.Set_Text_Transparent();

  const int lineH = 15;
  const int maxLines = t9::PREVIEW_H / lineH;      // 3 行
  const int charsPerLine = (t9::SCREEN_W - 10) / 6;

  const char *s = input.data();
  int len = input.length();

  // 统计总行数
  int totalLines = 1, cur = 0;
  for (int i = 0; i < len; i++) {
    if (s[i] == '\n') { totalLines++; cur = 0; }
    else if (++cur > charsPerLine) { totalLines++; cur = 1; }
  }
  int skip = (totalLines > maxLines) ? (totalLines - maxLines) : 0;

  // 跳过前 skip 行
  int i = 0, line = 0;
  cur = 0;
  while (i < len && line < skip) {
    if (s[i] == '\n') { line++; cur = 0; }
    else if (++cur > charsPerLine) { line++; cur = 1; }
    i++;
  }

  // 绘制
  int x = 5, y = t9::PREVIEW_Y + 2;
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
    // 空状态提示（全英文，字号 2 便于看清）
    lcd.Set_Text_Size(2);
    lcd.Print_String("Ready", 8, t9::PREVIEW_Y + 6, C_TEXT_DIM, C_PREVIEW);
    lcd.Set_Text_Size(1);
    lcd.Print_String("tap keys to type", 8, t9::PREVIEW_Y + 30, 0x7BEF, C_PREVIEW);
  }

  // 光标
  if ((millis() / 500) & 1) {
    if (lastY <= t9::PREVIEW_Y + t9::PREVIEW_H - lineH) {
      lcd.Fill_Rect(lastX, lastY + 1, 2, 12, C_ACCENT);
    }
  }
}

// 候选条：显示当前数字键可循环的字母，高亮当前项
static void drawCandidates() {
  lcd.Fill_Rect(0, t9::CAND_Y, t9::SCREEN_W, t9::CAND_H, C_CAND);
  lcd.Set_Text_Size(1);
  lcd.Set_Text_Transparent();

  if (g_t9LastKey < 0 || g_t9LastKey >= 12) {
    lcd.Print_String("TAP TWICE TO CYCLE", 5, t9::CAND_Y + 7, C_TEXT_DIM, C_CAND);
    return;
  }

  const t9::DigitKey &dk = t9::DIGITS[g_t9LastKey];
  if (dk.letterCount == 0) {
    return;
  }

  // 显示该键所有字母，当前循环位置高亮
  int x = 5;
  for (int k = 0; k < dk.letterCount; k++) {
    char up = dk.letters[k];
    if (g_shift != g_caps) up = (char)(up - 32);   // 大写显示

    char one[2] = { up, 0 };
    uint16_t fg = (k == g_t9Cycle) ? 0x0000 : C_TEXT;
    uint16_t bg = (k == g_t9Cycle) ? C_ACCENT : C_CAND;
    if (k == g_t9Cycle) {
      lcd.Fill_Rect(x - 2, t9::CAND_Y + 4, 14, 14, bg);
    }
    lcd.Print_String(one, x, t9::CAND_Y + 7, fg, bg);
    x += 16;
  }
}

// 绘制一个数字键
static void drawDigit(int index, bool pressed) {
  t9::Rect r = t9::digitRect(index);
  const t9::DigitKey &dk = t9::DIGITS[index];

  uint16_t bg = pressed ? C_KEY_ACT : C_KEY;
  lcd.Fill_Rect(r.x, r.y, r.w, r.h, bg);
  lcd.Draw_Rect(r.x, r.y, r.w, r.h, C_BORDER);

  // ---- 主标签：大号数字（2 倍字号，单个字符宽 12px）----
  char main[4];
  main[0] = dk.digit;
  main[1] = 0;
  lcd.Set_Text_Size(2);
  lcd.Set_Text_Transparent();
  lcd.Print_String(main, r.x + (r.w - 12) / 2, r.y + 2, C_TEXT, bg);

  // ---- 副标签：字母串（1 倍字号，每字符 6px）----
  if (dk.letters && dk.letterCount > 0) {
    char lbl[6];
    int n = dk.letterCount;
    if (n > 5) n = 5;
    for (int i = 0; i < n; i++) {
      char c = dk.letters[i];
      if (c >= 'a' && c <= 'z') c = (char)(c - 32);   // 转大写
      lbl[i] = c;
    }
    lbl[n] = 0;

    lcd.Set_Text_Size(1);
    int lw = n * 6;
    int lx = r.x + (r.w - lw) / 2;
    if (lx < r.x + 1) lx = r.x + 1;
    lcd.Print_String(lbl, lx, r.y + r.h - 11, C_TEXT_DIM, bg);
  }

  lcd.Set_Text_Size(1);
}

// 绘制功能键
static void drawFunc(int index, bool pressed) {
  t9::Rect r = t9::funcRect(index);
  const t9::FuncKey &fk = t9::FUNCS[index];

  uint16_t bg = pressed ? C_KEY_ACT : C_KEY_DARK;
  if (!pressed) {
    switch (fk.kind) {
      case t9::F_SHIFT: if (g_shift) bg = C_KEY_ON; break;
      case t9::F_CAPS:  if (g_caps)  bg = C_KEY_ON; break;
      case t9::F_LAYER: if (g_symbolLayer) bg = C_ACCENT; break;
      default: break;
    }
  }

  lcd.Fill_Rect(r.x, r.y, r.w, r.h, bg);
  lcd.Draw_Rect(r.x, r.y, r.w, r.h, C_BORDER);

  // 文字颜色：亮底用深字，暗底用白字，保证对比度
  uint16_t fg = C_TEXT;
  if (bg == C_KEY_ACT || bg == C_KEY_ON || bg == C_ACCENT) {
    fg = 0x0000;          // 亮底 -> 黑字
  }

  lcd.Set_Text_Size(1);
  lcd.Set_Text_Transparent();
  int tw = (int)strlen(fk.label) * 6;
  int tx = r.x + (r.w - tw) / 2;
  if (tx < r.x + 1) tx = r.x + 1;
  int ty = r.y + (r.h - 8) / 2;
  lcd.Print_String(fk.label, tx, ty, fg, bg);
}

static void drawPad(int pressIdx = -1, bool pressIsFunc = false) {
  lcd.Fill_Rect(0, t9::PAD_Y, t9::SCREEN_W, t9::SCREEN_H - t9::PAD_Y, C_BG);
  for (int i = 0; i < 12; i++) {
    drawDigit(i, !pressIsFunc && pressIdx == i);
  }
  for (int i = 0; i < t9::FUNC_COUNT; i++) {
    drawFunc(i, pressIsFunc && pressIdx == i);
  }
}

static void drawAll() {
  drawStatusBar();
  drawPreview();
  drawCandidates();
  drawPad();
}

// ---------------- HID ----------------
//
// 关键：必须用 pressRaw()/releaseRaw() 并传【真正的 HID usage】。
//
// USBHIDKeyboard::press(k) 会把 k 当成 ASCII 字符，用内部布局表转换成
// usage+modifier。传 0x1E（想表示数字键 1）会被当成控制字符 0x1E，
// 查表失败后回退，实际输出的字符完全不对（实测 "!@#" 全变成 "((("）。
//
// pressRaw(k) 直接把 k 当 usage 发出去，这才是我们要的。
// 修饰键同样要传 usage：LeftShift = 0xE1，LeftCtrl = 0xE0，LeftAlt = 0xE2。

#define HID_MOD_LCTRL   0xE0
#define HID_MOD_LSHIFT  0xE1
#define HID_MOD_LALT    0xE2
#define HID_MOD_LGUI    0xE3

// HID 按键的保持时间。太短主机会丢键，太长会拖慢输入。
static const uint16_t HID_HOLD_MS = 22;
static const uint16_t HID_GAP_MS  = 18;

static void hidReleaseAll() {
  Keyboard.releaseAll();
}

// 发一个键（可带修饰键），这是整个固件唯一的 HID 出口
static void emitKeyRaw(uint8_t usage, bool shift, bool ctrl, bool alt) {
  if (usage == 0) return;

  if (shift) Keyboard.pressRaw(HID_MOD_LSHIFT);
  if (ctrl)  Keyboard.pressRaw(HID_MOD_LCTRL);
  if (alt)   Keyboard.pressRaw(HID_MOD_LALT);
  if (shift || ctrl || alt) delay(6);

  Keyboard.pressRaw(usage);
  delay(HID_HOLD_MS);

  Keyboard.releaseRaw(usage);
  delay(6);

  if (shift) Keyboard.releaseRaw(HID_MOD_LSHIFT);
  if (ctrl)  Keyboard.releaseRaw(HID_MOD_LCTRL);
  if (alt)   Keyboard.releaseRaw(HID_MOD_LALT);

  // 显式发一份空报告，确保主机看到"全部释放"
  Keyboard.releaseAll();
  delay(HID_GAP_MS);
}

// 按字符发送（自动处理 Shift）
static void emitChar(char c) {
  uint8_t usage = 0;
  bool needShift = false;
  if (!keymap::charToHid(c, &usage, &needShift)) return;
  emitKeyRaw(usage, needShift, false, false);
}

// 按 usage 发送（功能键：回车/退格/方向键等）
static void emitUsage(uint8_t usage) {
  emitKeyRaw(usage, false, false, false);
}

// ---------------- 九键逻辑 ----------------

// 把待提交的字母真正发出去
static void commitPending() {
  if (g_t9PendingChar == 0) return;
  char c = g_t9PendingChar;
  g_t9PendingChar = 0;
  g_t9LastKey = -1;
  g_t9Cycle = 0;

  emitChar(c);
  input.append(c);
}

// 按数字键
static void pressDigit(int index) {
  const t9::DigitKey &dk = t9::DIGITS[index];

  // 没字母的键（1 * # 0）：先提交待定，再直接输出
  if (dk.letterCount == 0) {
    commitPending();

    switch (dk.digit) {
      case '1': emitChar('1'); input.append('1'); break;
      case '0': emitChar(' '); input.append(' '); break;
      case '*': emitChar('*'); input.append('*'); break;
      case '#': emitChar('#'); input.append('#'); break;
      default: break;
    }
    return;
  }

  uint32_t now = millis();

  // 同一个键且在有效期内 -> 循环下一个字母
  if (index == g_t9LastKey && (int32_t)(now - g_t9Deadline) < 0) {
    // 已经发出去的字符要"改写"：用退格删掉再重发
    g_t9Cycle = (g_t9Cycle + 1) % dk.letterCount;

    // 从输入缓冲删掉上一个字符
    // 注意：HID 侧用退格删除
    emitUsage(keymap::U_BACKSPACE);
    input.backspace();

    char upper = false;
    char c = dk.letters[g_t9Cycle];
    char out = ((g_shift != g_caps)) ? (char)(c - 32) : c;

    emitChar(out);
    input.append(out);

    g_t9PendingChar = out;
    g_t9Deadline = now + T9_TIMEOUT_MS;
    return;
  }

  // 不同键 -> 提交上一个，开始新的循环
  commitPending();

  g_t9LastKey = index;
  g_t9Cycle = 0;
  g_t9Deadline = now + T9_TIMEOUT_MS;

  char c = dk.letters[0];
  char out = ((g_shift != g_caps)) ? (char)(c - 32) : c;

  emitChar(out);
  input.append(out);
  g_t9PendingChar = out;
}

// 按功能键
static void pressFunc(int index) {
  const t9::FuncKey &fk = t9::FUNCS[index];

  switch (fk.kind) {
    case t9::F_SHIFT:
      commitPending();
      g_shift = !g_shift;
      return;

    case t9::F_CAPS:
      commitPending();
      g_caps = !g_caps;
      return;

    case t9::F_BACKSPACE:
      // 有待定字母时先取消它
      if (g_t9PendingChar != 0) {
        emitUsage(keymap::U_BACKSPACE);
        input.backspace();
        g_t9PendingChar = 0;
        g_t9LastKey = -1;
        g_t9Cycle = 0;
        return;
      }
      emitUsage(keymap::U_BACKSPACE);
      input.backspace();
      return;

    case t9::F_SPACE:
      commitPending();
      emitChar(' ');
      input.append(' ');
      return;

    case t9::F_ENTER:
      commitPending();
      emitUsage(keymap::U_ENTER);
      input.newline();
      return;

    case t9::F_LAYER:
      commitPending();
      g_symbolLayer = !g_symbolLayer;
      return;

    default:
      return;
  }
}

// ---------------- setup / loop ----------------

void setup() {
  // 1. 显示：竖屏 240x320
  //    驱动 MADCTL 表里 rot2 = 0x20 (MV)，是 240x320 竖屏且内容正立的方向。
  //    rot0 = 0xE0 会上下颠倒（实测），rot2 才是我们要的。
  lcd.Init();
  lcd.Set_Rotation(2);

  // 2. 触摸：与 LCD 同方向，否则坐标对不上
  tp.Init(lcd.Get_Width(), lcd.Get_Height());
  tp.Set_Rotation(2);

  lcd.Fill_Screen(C_BG);
  input.clear();

  // 3. USB HID
  Keyboard.begin(KeyboardLayout_en_US);
  USB.begin();
  delay(600);
  g_hidReady = true;

  drawAll();
}

void loop() {
  uint32_t now = millis();

  // ---- 1. 九键连击超时 -> 提交 ----
  if (g_t9PendingChar != 0 && (int32_t)(now - g_t9Deadline) >= 0) {
    // 超时后只是清除待定状态（字符已经发出去了，不需要重发）
    g_t9PendingChar = 0;
    g_t9LastKey = -1;
    g_t9Cycle = 0;
    drawCandidates();
  }

  // ---- 2. 按键高亮复位 ----
  if (g_pressIdx >= 0 && (int32_t)(now - g_pressUntil) >= 0) {
    if (g_pressIsFunc) drawFunc(g_pressIdx, false);
    else               drawDigit(g_pressIdx, false);
    g_pressIdx = -1;
  }

  // ---- 3. 触摸 ----
  int16_t tx, ty;
  if (!tp.Read_Stable(&tx, &ty)) {
    delay(6);
    return;
  }

  int idx;
  bool isFunc = false;
  bool hit = false;

  if (t9::hitFunc(tx, ty, &idx)) {
    isFunc = true;
    hit = true;
  } else if (t9::hitDigit(tx, ty, &idx)) {
    isFunc = false;
    hit = true;
  }

  if (!hit) {
    delay(6);
    return;
  }

  // 视觉反馈
  g_pressIdx = idx;
  g_pressIsFunc = isFunc;
  g_pressUntil = now + 110;
  if (isFunc) drawFunc(idx, true);
  else        drawDigit(idx, true);

  // 执行
  if (isFunc) pressFunc(idx);
  else        pressDigit(idx);

  // 刷新受影响的区域
  drawPreview();
  drawCandidates();
  drawStatusBar();

  // 修饰键状态变化需要重绘功能键
  if (isFunc) {
    for (int i = 0; i < t9::FUNC_COUNT; i++) drawFunc(i, false);
  }

  // 等松手，避免一次触摸触发多次
  uint32_t t0 = millis();
  while (tp.Is_Pressed() && (millis() - t0) < 700) {
    delay(8);
  }
  delay(25);
}
