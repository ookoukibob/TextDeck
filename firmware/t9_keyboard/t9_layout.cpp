#include "t9_layout.h"

namespace t9 {

// ---------------- 数字键内容 ----------------
// 标签为显示文本，letters 为九键循环字母
const DigitKey DIGITS[12] = {
  { '1', nullptr,    0, "1"    },
  { '2', "abc",      3, "2 ABC" },
  { '3', "def",      3, "3 DEF" },
  { '4', "ghi",      3, "4 GHI" },
  { '5', "jkl",      3, "5 JKL" },
  { '6', "mno",      3, "6 MNO" },
  { '7', "pqrs",     4, "7 PQRS" },
  { '8', "tuv",      3, "8 TUV" },
  { '9', "wxyz",     4, "9 WXYZ" },
  { '*', nullptr,    0, "*"    },
  { '0', nullptr,    0, "0 _"   },
  { '#', nullptr,    0, "#"    },
};

// ---------------- 功能键 ----------------
// 全部用英文短标签：字库覆盖、宽度够、无缺字方框
const FuncKey FUNCS[] = {
  { F_SHIFT,     "SHIFT" },
  { F_CAPS,      "CAPS"  },
  { F_BACKSPACE, "DEL"   },
  { F_SPACE,     "SPACE" },
  { F_ENTER,     "ENTER" },
  { F_LAYER,     "SYM"   },
};
const int FUNC_COUNT = sizeof(FUNCS) / sizeof(FUNCS[0]);

// ---------------- 几何 ----------------

Rect digitRect(int index) {
  // index 0..11 -> 列 = index % 3, 行 = index / 3
  int col = index % 3;
  int row = index / 3;

  // 总宽 = 3*CELL_W + 4*CELL_GAP = 240，正好铺满
  int x = CELL_GAP + col * (CELL_W + CELL_GAP);
  int y = PAD_Y + row * (CELL_H + CELL_GAP);

  Rect r;
  r.x = (int16_t)x;
  r.y = (int16_t)y;
  r.w = (int16_t)CELL_W;
  r.h = (int16_t)CELL_H;
  return r;
}

Rect funcRect(int index) {
  // 功能键两行三列，每键 77x21
  //   row0: SHIFT  CAPS   DEL
  //   row1: SPACE  ENTER  SYM
  int perRow = FUNC_PER_ROW;
  int w = (SCREEN_W - FUNC_GAP * (perRow + 1)) / perRow;   // (240-8)/3 = 77

  int row = index / perRow;
  int col = index % perRow;

  Rect r;
  r.x = (int16_t)(FUNC_GAP + col * (w + FUNC_GAP));
  r.y = (int16_t)(FUNC_Y + row * (FUNC_H + FUNC_GAP));
  r.w = (int16_t)w;
  r.h = (int16_t)FUNC_H;
  return r;
}

bool hitDigit(int16_t px, int16_t py, int *index) {
  if (py < PAD_Y) return false;
  for (int i = 0; i < 12; i++) {
    Rect r = digitRect(i);
    if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) {
      if (index) *index = i;
      return true;
    }
  }
  return false;
}

bool hitFunc(int16_t px, int16_t py, int *index) {
  if (py < FUNC_Y) return false;
  for (int i = 0; i < FUNC_COUNT; i++) {
    Rect r = funcRect(i);
    if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) {
      if (index) *index = i;
      return true;
    }
  }
  return false;
}

} // namespace t9
