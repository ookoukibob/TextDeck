// input_buffer.h —— 屏幕输入预览缓冲
//
// 只保存"用户通过触摸键盘输入的内容"，用于屏幕回显。
// 不参与 HID 输出（HID 是独立的即时行为）。
//
// 容量取 512 字节：够显示几行，又不吃 RAM。
// 超出后从头丢弃最旧的内容（环形语义），并显示提示。

#pragma once
#include <stdint.h>
#include <stddef.h>

class InputBuffer {
public:
  static const int CAP = 512;

  void clear();
  bool append(char c);
  bool backspace();
  bool newline();

  const char *data() const { return _buf; }
  int length() const { return _len; }
  bool empty() const { return _len == 0; }

  // 是否发生过截断（旧内容被丢弃）
  bool overflowed() const { return _overflowed; }
  void clearOverflowFlag() { _overflowed = false; }

  // 光标是否可以显示为"闪烁块"（本实现光标固定在末尾）
  int cursorPos() const { return _len; }

private:
  void dropOldest(int n);

  char _buf[CAP + 1];
  int  _len = 0;
  bool _overflowed = false;
};
