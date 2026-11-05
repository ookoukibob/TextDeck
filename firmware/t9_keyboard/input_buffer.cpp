#include "input_buffer.h"
#include <string.h>

void InputBuffer::clear() {
  _len = 0;
  _buf[0] = 0;
  _overflowed = false;
}

void InputBuffer::dropOldest(int n) {
  if (n <= 0 || n > _len) return;
  memmove(_buf, _buf + n, (size_t)(_len - n));
  _len -= n;
  _buf[_len] = 0;
  _overflowed = true;
}

bool InputBuffer::append(char c) {
  // 只接受可打印字符与换行/制表（与 HID 能力保持一致）
  if (!((c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\t')) return false;

  if (_len >= CAP) {
    // 丢掉最旧的 1/4，留出空间，避免每来一个字符就搬一次内存
    dropOldest(CAP / 4);
  }
  _buf[_len++] = c;
  _buf[_len] = 0;
  return true;
}

bool InputBuffer::backspace() {
  if (_len == 0) return false;
  _len--;
  _buf[_len] = 0;
  return true;
}

bool InputBuffer::newline() {
  return append('\n');
}
