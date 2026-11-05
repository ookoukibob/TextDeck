#include "keymap.h"
#include <stddef.h>

namespace keymap {

// ---------------- 字符 -> HID ----------------
//
// 完整覆盖可打印 ASCII 0x20..0x7E。
// 表里每项是 {usage, 是否需要Shift}。
// 用静态表而不是 switch，方便审计和扩充。
struct CharMap {
  char    c;
  uint8_t usage;
  bool    shift;
};

static const CharMap kCharTable[] = {
  // 数字行 (Shift 出符号)
  { '1', U_1, false }, { '!', U_1, true },
  { '2', 0x1F, false }, { '@', 0x1F, true },
  { '3', 0x20, false }, { '#', 0x20, true },
  { '4', 0x21, false }, { '$', 0x21, true },
  { '5', 0x22, false }, { '%', 0x22, true },
  { '6', 0x23, false }, { '^', 0x23, true },
  { '7', 0x24, false }, { '&', 0x24, true },
  { '8', 0x25, false }, { '*', 0x25, true },
  { '9', 0x26, false }, { '(', 0x26, true },
  { '0', U_0,  false }, { ')', U_0,  true },

  // 符号键
  { '-', U_MINUS,    false }, { '_', U_MINUS,    true },
  { '=', U_EQUAL,    false }, { '+', U_EQUAL,    true },
  { '[', U_LBRACKET, false }, { '{', U_LBRACKET, true },
  { ']', U_RBRACKET, false }, { '}', U_RBRACKET, true },
  { '\\', U_BACKSLASH, false }, { '|', U_BACKSLASH, true },
  { ';', U_SEMICOLON, false }, { ':', U_SEMICOLON, true },
  { '\'', U_QUOTE,   false }, { '"', U_QUOTE,    true },
  { '`', U_GRAVE,    false }, { '~', U_GRAVE,    true },
  { ',', U_COMMA,    false }, { '<', U_COMMA,    true },
  { '.', U_PERIOD,   false }, { '>', U_PERIOD,   true },
  { '/', U_SLASH,    false }, { '?', U_SLASH,    true },

  // 空白
  { ' ', U_SPACE, false },
  { '\t', U_TAB,  false },
  { '\n', U_ENTER, false },
};

bool charToHid(char c, uint8_t *usage, bool *needShift) {
  // 小写字母 -> 0x04..0x1D
  if (c >= 'a' && c <= 'z') {
    *usage = (uint8_t)(U_A + (c - 'a'));
    *needShift = false;
    return true;
  }
  // 大写字母 -> 同样 usage, 但要 Shift
  if (c >= 'A' && c <= 'Z') {
    *usage = (uint8_t)(U_A + (c - 'A'));
    *needShift = true;
    return true;
  }
  for (size_t i = 0; i < sizeof(kCharTable) / sizeof(kCharTable[0]); i++) {
    if (kCharTable[i].c == c) {
      *usage = kCharTable[i].usage;
      *needShift = kCharTable[i].shift;
      return true;
    }
  }
  return false;
}

bool charSupported(char c) {
  uint8_t u;
  bool s;
  return charToHid(c, &u, &s);
}

bool isAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// ---------------- 组合逻辑 ----------------
//
// Caps Lock 只影响字母；Shift 对所有键生效。
// 最终要发的字符:
//   字母 : shift XOR caps 决定大小写
//   其它 : shift 决定 base/shifted
//
// 这里返回的是"要发给 HID 的 usage + 是否附加 Shift 修饰"。
void resolveCharKey(const KeyDef &k, bool shiftActive, bool capsActive,
                    uint8_t *usage, bool *needShift) {
  *usage = 0;
  *needShift = false;

  if (k.kind != K_CHAR) {
    // 非字符键直接用 usage 字段
    *usage = k.usage;
    return;
  }

  // 目标字符: 字母受 caps 影响, 其它只看 shift
  char target;
  if (isAlpha(k.base)) {
    bool upper = (shiftActive != capsActive);   // 异或: 一个生效就大写
    char lower = (k.base >= 'A' && k.base <= 'Z') ? (char)(k.base + 32) : k.base;
    target = upper ? (char)(lower - 32) : lower;
  } else {
    target = shiftActive ? k.shifted : k.base;
  }

  charToHid(target, usage, needShift);
}

} // namespace keymap
