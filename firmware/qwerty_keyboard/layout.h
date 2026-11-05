// layout.h —— 320x240 横屏 QWERTY 布局
//
// 屏幕 320 宽 x 240 高，rotation=1（MADCTL 0xA0）。
//
// 分区:
//   y   0.. 16   状态栏   17px
//   y  17.. 62   输入回显 46px
//   y  64..239   键盘区   176px
//
// 横屏 320px 宽，一行可放 10 键：
//   键宽 29，间距 2，共 10*29 + 11*2 = 290 + 22 = 312，留 4px 边距。
//   键高 27，共 6 行：6*27 + 7*2 = 162 + 14 = 176，正好填满键盘区。
//
// 布局（6 行）：
//   row0: 1 2 3 4 5 6 7 8 9 0          (10 键)
//   row1: Q W E R T Y U I O P          (10 键)
//   row2: A S D F G H J K L ;          (10 键，缩进半格)
//   row3: SHIFT Z X C V B N M DEL      (Shift/DEL 加宽)
//   row4: SYM , . / SPACE  <- ->       (功能混合)
//   row5: CTRL CAPS TAB  ENTER  ESC    (功能行)
//
// 电阻屏点得准：最小键 29x27，够大。

#pragma once
#include <stdint.h>
#include "keymap.h"

namespace kb {

const int SCREEN_W = 320;
const int SCREEN_H = 240;

const int STATUS_Y = 0;
const int STATUS_H = 17;

const int PREVIEW_Y = 17;
const int PREVIEW_H = 46;

const int KB_Y = 64;

// 键尺寸：10 键/行
const int COLS = 10;
const int KEY_W = 29;
const int KEY_H = 27;
const int KEY_GAP = 2;
const int ROW_STEP = KEY_H + KEY_GAP;    // 29

struct RowDef {
  const keymap::KeyDef *keys;
  int count;
  int16_t indent;      // 左侧缩进（像素），用于 A 行、移位行
};

// 各行键定义
extern const keymap::KeyDef ROW_NUM[];     // 1..0
extern const keymap::KeyDef ROW_Q[];       // Q..P
extern const keymap::KeyDef ROW_A[];       // A..;
extern const keymap::KeyDef ROW_Z[];       // SHIFT Z..M DEL
extern const keymap::KeyDef ROW_BOT[];     // SYM , . / SPACE <- ->
extern const keymap::KeyDef ROW_FN[];      // CTRL CAPS TAB ENTER ESC

extern const RowDef ROWS[];
extern const int ROW_COUNT;

struct Rect { int16_t x, y, w, h; };

// 取某行某键的矩形。
// 特殊行（0/3/4/5）的键宽按权重分配，权重表在 layout.cpp 里。
Rect keyRect(int row, int col);

bool hitTest(int16_t px, int16_t py, int *row, int *col);
const keymap::KeyDef &keyAt(int row, int col);

} // namespace kb
