// keymap.h —— 触摸按键到 USB HID 的映射
//
// 设计要点:
//   1. 每个物理键对应一个 KeyCode，不是直接对应字符。
//      因为同一个键在不同修饰状态下产生不同字符（a/A、1/!）。
//   2. 字符由 (KeyCode, shift, caps) 三者共同决定，而不是硬编码。
//   3. Ctrl/Alt/GUI 是"粘滞修饰键"：按下后 2 秒内有效，或按住不放。
//      粘滞模式避免用户因为忘记释放而卡键。
//   4. 任何情况下都能通过 releaseAll() 清空。
//
// HID Usage (USB HID Usage Tables, Keyboard/Keypad Page 0x07):
//   0x04..0x1D   a..z
//   0x1E..0x27   1..9,0
//   0x28        Enter
//   0x29        Escape
//   0x2A        Backspace
//   0x2B        Tab
//   0x2C        Space
//   0x2D        - _
//   0x2E        = +
//   0x2F        [ {
//   0x30        ] }
//   0x31        \ |
//   0x32        (非US) ; :
//   0x33        ; :
//   0x34        ' "
//   0x35        ` ~
//   0x36        , <
//   0x37        . >
//   0x38        / ?
//   0x39        Caps Lock
//   0x4F..0x52  右/左/下/上
//
// 修饰键位掩码 (HID report byte 0):
//   0x01 LeftCtrl 0x02 LeftShift 0x04 LeftAlt 0x08 LeftGUI
//   0x10 RightCtrl 0x20 RightShift 0x40 RightAlt 0x80 RightGUI

#pragma once
#include <stdint.h>

namespace keymap {

// ---------------- HID Usage 定义 ----------------
enum Usage : uint8_t {
  U_NONE      = 0x00,
  U_A         = 0x04,   // A..Z 连续 26 个
  U_1         = 0x1E,   // 1..9 连续 9 个, 0 是 0x27
  U_0         = 0x27,
  U_ENTER     = 0x28,
  U_ESC       = 0x29,
  U_BACKSPACE = 0x2A,
  U_TAB       = 0x2B,
  U_SPACE     = 0x2C,
  U_MINUS     = 0x2D,
  U_EQUAL     = 0x2E,
  U_LBRACKET  = 0x2F,
  U_RBRACKET  = 0x30,
  U_BACKSLASH = 0x31,
  U_SEMICOLON = 0x33,
  U_QUOTE     = 0x34,
  U_GRAVE     = 0x35,
  U_COMMA     = 0x36,
  U_PERIOD    = 0x37,
  U_SLASH     = 0x38,
  U_CAPSLOCK  = 0x39,
  U_DELETE    = 0x4C,
  U_RIGHT     = 0x4F,
  U_LEFT      = 0x50,
  U_DOWN      = 0x51,
  U_UP        = 0x52,
};

// ---------------- 修饰键掩码 ----------------
enum Mod : uint8_t {
  M_NONE      = 0x00,
  M_LCTRL     = 0x01,
  M_LSHIFT    = 0x02,
  M_LALT      = 0x04,
  M_LGUI      = 0x08,
  M_RCTRL     = 0x10,
  M_RSHIFT    = 0x20,
  M_RALT      = 0x40,
  M_RGUI      = 0x80,
};

// ---------------- 键盘按键标识 ----------------
// 每个虚拟键在布局表里用这个枚举描述其"语义"
enum KeyKind : uint8_t {
  K_CHAR,       // 普通字符键, 值见 Char 字段
  K_ENTER,
  K_BACKSPACE,
  K_TAB,
  K_ESC,
  K_SPACE,
  K_CAPSLOCK,
  K_SHIFT,      // 左 Shift (粘滞)
  K_CTRL,       // 左 Ctrl  (粘滞)
  K_ALT,        // 左 Alt   (粘滞)
  K_UP, K_DOWN, K_LEFT, K_RIGHT,
  K_DELETE,
  K_LAYER,      // 切换到符号层
  K_MODE,       // 切换 键盘/剪贴板/传输 页面
  K_NONE,       // 空位 (仅占位, 不响应)
};

// 一个虚拟键的完整描述
struct KeyDef {
  KeyKind kind;
  char    base;      // 未按 Shift 时的字符 (仅 K_CHAR 有效)
  char    shifted;   // 按 Shift 时的字符 (仅 K_CHAR 有效)
  uint8_t usage;     // 直接给定的 HID usage (K_CHAR 时由 base 推导)
  const char *label; // 按键上显示的短标签
};

// 把字符映射到 HID usage + 是否需要 Shift。
// 返回 false 表示该字符无法用标准 HID 键盘发出 (例如中文)。
bool charToHid(char c, uint8_t *usage, bool *needShift);

// 该字符是否可发 (与上面一致, 供预览/校验使用)
bool charSupported(char c);

// 根据当前修饰状态, 计算一个 K_CHAR 键应发的 usage 与 shift 需求
void resolveCharKey(const KeyDef &k, bool shiftActive, bool capsActive,
                    uint8_t *usage, bool *needShift);

// 判断是否是大写字母键 (供 UI 显示用)
bool isAlpha(char c);

} // namespace keymap
