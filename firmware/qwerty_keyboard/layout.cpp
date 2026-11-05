#include "layout.h"

namespace kb {

using namespace keymap;

// ---------------- 各行键定义 ----------------

const KeyDef ROW_NUM[] = {
  { K_CHAR, '1', '!', 0, "1" }, { K_CHAR, '2', '@', 0, "2" },
  { K_CHAR, '3', '#', 0, "3" }, { K_CHAR, '4', '$', 0, "4" },
  { K_CHAR, '5', '%', 0, "5" }, { K_CHAR, '6', '^', 0, "6" },
  { K_CHAR, '7', '&', 0, "7" }, { K_CHAR, '8', '*', 0, "8" },
  { K_CHAR, '9', '(', 0, "9" }, { K_CHAR, '0', ')', 0, "0" },
};

const KeyDef ROW_Q[] = {
  { K_CHAR, 'q', 'Q', 0, "Q" }, { K_CHAR, 'w', 'W', 0, "W" },
  { K_CHAR, 'e', 'E', 0, "E" }, { K_CHAR, 'r', 'R', 0, "R" },
  { K_CHAR, 't', 'T', 0, "T" }, { K_CHAR, 'y', 'Y', 0, "Y" },
  { K_CHAR, 'u', 'U', 0, "U" }, { K_CHAR, 'i', 'I', 0, "I" },
  { K_CHAR, 'o', 'O', 0, "O" }, { K_CHAR, 'p', 'P', 0, "P" },
};

const KeyDef ROW_A[] = {
  { K_CHAR, 'a', 'A', 0, "A" }, { K_CHAR, 's', 'S', 0, "S" },
  { K_CHAR, 'd', 'D', 0, "D" }, { K_CHAR, 'f', 'F', 0, "F" },
  { K_CHAR, 'g', 'G', 0, "G" }, { K_CHAR, 'h', 'H', 0, "H" },
  { K_CHAR, 'j', 'J', 0, "J" }, { K_CHAR, 'k', 'K', 0, "K" },
  { K_CHAR, 'l', 'L', 0, "L" }, { K_CHAR, ';', ':', 0, ";" },
};

const KeyDef ROW_Z[] = {
  { K_SHIFT,     0, 0, 0, "SH" },
  { K_CHAR, 'z', 'Z', 0, "Z" }, { K_CHAR, 'x', 'X', 0, "X" },
  { K_CHAR, 'c', 'C', 0, "C" }, { K_CHAR, 'v', 'V', 0, "V" },
  { K_CHAR, 'b', 'B', 0, "B" }, { K_CHAR, 'n', 'N', 0, "N" },
  { K_CHAR, 'm', 'M', 0, "M" },
  { K_CHAR, ',', '<', 0, "," }, { K_CHAR, '.', '>', 0, "." },
};

const KeyDef ROW_BOT[] = {
  { K_LAYER,  0, 0, 0, "SYM" },
  { K_CHAR, '/', '?', 0, "/" },
  { K_CHAR, '-', '_', 0, "-" },
  { K_CHAR, '=', '+', 0, "=" },
  { K_SPACE,  0, 0, 0, "SPACE" },
  { K_LEFT,   0, 0, 0, "<-" },
  { K_RIGHT,  0, 0, 0, "->" },
  { K_BACKSPACE, 0, 0, 0, "DEL" },
};

const KeyDef ROW_FN[] = {
  { K_CTRL,   0, 0, 0, "CTRL" },
  { K_CAPSLOCK, 0, 0, 0, "CAPS" },
  { K_TAB,    0, 0, 0, "TAB" },
  { K_ENTER,  0, 0, 0, "ENTER" },
  { K_ESC,    0, 0, 0, "ESC" },
};

// 行表
const RowDef ROWS[] = {
  { ROW_NUM, 10, 0 },
  { ROW_Q,   10, 0 },
  { ROW_A,   10, 14 },        // A 行缩进半格
  { ROW_Z,   10, 0 },
  { ROW_BOT,  8, 0 },
  { ROW_FN,   5, 0 },
};
const int ROW_COUNT = sizeof(ROWS) / sizeof(ROWS[0]);

// ---------------- 几何 ----------------
//
// 普通行（0/1/2）：等宽 10 键，缩进由 RowDef.indent 决定。
// 行 2 有缩进，键宽需重算：(320 - indent - gaps) / 10
//
// 特殊行（3/4/5）：按权重分配宽度，填满整行。

// 行 3（SHIFT Z..M , .）：SHIFT 权重 2，其余 1（共 11 份）
static const int8_t W3[] = { 2, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
// 行 4（SYM / - = SPACE <- -> DEL）：SYM 1, / 1, - 1, = 1, SPACE 3, <- 1, -> 1, DEL 2 = 11
static const int8_t W4[] = { 1, 1, 1, 1, 3, 1, 1, 2 };
// 行 5（CTRL CAPS TAB ENTER ESC）：各 2，共 10
static const int8_t W5[] = { 2, 2, 2, 2, 2 };

Rect keyRect(int row, int col) {
  Rect r;
  r.y = (int16_t)(KB_Y + row * ROW_STEP);
  r.h = (int16_t)KEY_H;

  const RowDef &rd = ROWS[row];

  if (row == 0 || row == 1) {
    // 10 键等宽，左右各留 KEY_GAP
    int w = (SCREEN_W - KEY_GAP * (10 + 1)) / 10;    // (320-22)/10 = 29
    r.x = (int16_t)(KEY_GAP + col * (w + KEY_GAP));
    r.w = (int16_t)w;
    return r;
  }

  if (row == 2) {
    // 有缩进，10 键
    int avail = SCREEN_W - rd.indent - KEY_GAP * 11;
    int w = avail / 10;
    r.x = (int16_t)(rd.indent + KEY_GAP + col * (w + KEY_GAP));
    r.w = (int16_t)w;
    return r;
  }

  // 行 3/4/5：按权重填满
  const int8_t *wt = (row == 3) ? W3 : (row == 4) ? W4 : W5;
  int n = rd.count;
  int total = 0;
  for (int i = 0; i < n; i++) total += wt[i];

  int usable = SCREEN_W - KEY_GAP * (total + 1);
  int unit = usable / total;

  int x = KEY_GAP;
  for (int i = 0; i < col; i++) x += unit * wt[i] + KEY_GAP;

  r.x = (int16_t)x;
  int w = unit * wt[col];
  // 最后一键贴右边缘，避免累计误差留下缝隙
  if (col == n - 1) {
    w = SCREEN_W - KEY_GAP - x;
  }
  r.w = (int16_t)w;
  return r;
}

bool hitTest(int16_t px, int16_t py, int *row, int *col) {
  if (py < KB_Y) return false;
  int r = (py - KB_Y) / ROW_STEP;
  if (r < 0 || r >= ROW_COUNT) return false;

  for (int c = 0; c < ROWS[r].count; c++) {
    Rect k = keyRect(r, c);
    if (px >= k.x && px < k.x + k.w && py >= k.y && py < k.y + k.h) {
      if (row) *row = r;
      if (col) *col = c;
      return true;
    }
  }
  return false;
}

const KeyDef &keyAt(int row, int col) {
  return ROWS[row].keys[col];
}

} // namespace kb
