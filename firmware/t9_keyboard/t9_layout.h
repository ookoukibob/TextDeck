// t9_layout.h —— 240x320 竖屏九键输入布局
//
// 竖屏 240 宽 x 320 高，rotation=0（驱动默认 MADCTL=0xE0）。
//
// 分区:
//   y   0.. 17   状态栏   18px   USB / Caps / 模式
//   y  19.. 73   输入预览 55px   约 3 行 ASCII
//   y  75.. 95   候选条   20px   九键输入的候选字母/词组
//   y  97..319   键盘区  223px
//
// 九键区 = 3x4 网格（数字 1-9 + 0），每格 76x52
//   1      2 ABC  3 DEF
//   4 GHI  5 JKL  6 MNO
//   7 PQRS 8 TUV  9 WXYZ
//   *      0 ␣    #
//
// 底部独立功能行（y 313 附近不够，改为在数字网格右侧/下方扩展）
// 实际布局：3 列 x 5 行（4 行数字 + 1 行功能）
//   每格宽 80，高 44
//
// 字母输入方式：连按同一个数字键循环选中该键上的字母
//   例如按 2 一次 -> A，再按 -> B，再按 -> C，超时后确认
// 这与传统手机九键一致，电阻屏点击可靠性最高。

#pragma once
#include <stdint.h>
#include "keymap.h"

namespace t9 {

const int SCREEN_W = 240;
const int SCREEN_H = 320;

const int STATUS_Y = 0;
const int STATUS_H = 18;

const int PREVIEW_Y = 19;
const int PREVIEW_H = 54;

const int CAND_Y = 73;
const int CAND_H = 22;

const int PAD_Y = 97;           // 数字键盘起点

// 3 列 4 行数字区
// 76*3 + 2*4 = 228 + 8 = 236，留 4px 边距
const int COLS = 3;
const int PAD_ROWS = 4;
const int CELL_W = 76;
const int CELL_H = 41;
const int CELL_GAP = 2;

// 功能行：两行三列（SHIFT CAPS DEL / SPACE ENTER SYM）
const int FUNC_Y = PAD_Y + PAD_ROWS * (CELL_H + CELL_GAP);   // = 97 + 172 = 269
const int FUNC_H = 21;
const int FUNC_GAP = 2;
const int FUNC_PER_ROW = 3;

// 单个数字键的内容
struct DigitKey {
  char digit;              // '1'..'9', '0', '*', '#'
  const char *letters;     // 该键对应字母串，无则 nullptr
  int letterCount;
  const char *label;       // 显示标签
};

// 12 个数字键（3x4）
extern const DigitKey DIGITS[12];

// 功能键
enum FuncKind : uint8_t {
  F_SHIFT,
  F_CAPS,
  F_BACKSPACE,
  F_SPACE,
  F_ENTER,
  F_MODE,       // 数字/字母模式切换
  F_LAYER,      // 符号层
};

struct FuncKey {
  FuncKind kind;
  const char *label;
};

extern const FuncKey FUNCS[];
extern const int FUNC_COUNT;

// 几何：数字键矩形
struct Rect { int16_t x, y, w, h; };
Rect digitRect(int index);     // index 0..11
Rect funcRect(int index);      // index 0..FUNC_COUNT-1

// 命中测试
bool hitDigit(int16_t px, int16_t py, int *index);
bool hitFunc(int16_t px, int16_t py, int *index);

} // namespace t9
