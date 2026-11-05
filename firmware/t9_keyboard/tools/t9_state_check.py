#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
T9 状态机静态验证 —— 用纯 Python 复刻逻辑，穷举需求表里的场景。
不参与固件构建，仅用于人工审查前自检。

对照 t9_keyboard.ino 的真实实现：
  commitPending()  只清候选状态，不重发字符、不碰 SHIFT
  pressDigit()     新候选：先 wantUpper = g_shift != g_caps，再 commit，再建候选，最后清 SHIFT
                   同键循环：用 g_t9Upper，不重读 g_shift
  超时 / SPACE / ENTER / DEL / 1-0-*-#  各自消费 SHIFT
  CAPS 任何路径都不清
"""

LETTERS = {
    '2': 'abc', '3': 'def', '4': 'ghi', '5': 'jkl',
    '6': 'mno', '7': 'pqrs', '8': 'tuv', '9': 'wxyz',
}
TIMEOUT = 1400


class T9:
    def __init__(self):
        self.shift = False
        self.caps = False
        self.last_key = None
        self.cycle = 0
        self.deadline = -10**9
        self.pending = None      # 已发出但未确认的字符
        self.upper = False       # 候选锁定的大小写
        self.hid = ""            # HID 实际输出流（含退格效果）
        self.buf = ""            # 屏幕回显
        self.now = 0

    # ---- 内部 ----
    def commit_pending(self):
        if self.pending is None:
            return
        self.pending = None
        self.last_key = None
        self.cycle = 0

    def emit(self, c):
        self.hid += c

    def backspace(self):
        self.hid = self.hid[:-1]

    def tick(self, ms):
        self.now += ms

    def check_timeout(self):
        if self.pending is not None and self.now >= self.deadline:
            self.commit_pending()
            self.shift = False

    # ---- 输入 ----
    def press(self, key):
        self.check_timeout()

        if key not in LETTERS:
            self.commit_pending()
            ch = ' ' if key == '0' else key
            self.emit(ch)
            self.buf += ch
            self.shift = False
            return

        letters = LETTERS[key]

        # 同键循环
        if key == self.last_key and self.now < self.deadline:
            self.cycle = (self.cycle + 1) % len(letters)
            self.backspace()
            self.buf = self.buf[:-1]
            c = letters[self.cycle]
            out = c.upper() if self.upper else c
            self.emit(out)
            self.buf += out
            self.pending = out
            self.deadline = self.now + TIMEOUT
            return

        # 新候选：先锁定大小写，再确认旧候选
        want_upper = (self.shift != self.caps)
        self.commit_pending()
        self.last_key = key
        self.cycle = 0
        self.deadline = self.now + TIMEOUT
        self.upper = want_upper
        c = letters[0]
        out = c.upper() if self.upper else c
        self.emit(out)
        self.buf += out
        self.pending = out
        self.shift = False

    def press_shift(self):
        self.check_timeout()
        self.commit_pending()
        self.shift = not self.shift

    def press_caps(self):
        self.check_timeout()
        self.commit_pending()
        self.caps = not self.caps

    def press_space(self):
        self.check_timeout()
        self.commit_pending()
        self.emit(' ')
        self.buf += ' '
        self.shift = False

    def press_del(self):
        self.check_timeout()
        if self.pending is not None:
            self.backspace()
            self.buf = self.buf[:-1]
            self.pending = None
            self.last_key = None
            self.cycle = 0
            self.shift = False
            return
        self.backspace()
        self.buf = self.buf[:-1]
        self.shift = False


def case(name, actions, expect_hid):
    """actions: [(key, delay_ms), ...]"""
    t = T9()
    for key, delay in actions:
        t.tick(delay)
        t.check_timeout()
        if key == 'SHIFT':
            t.press_shift()
        elif key == 'CAPS':
            t.press_caps()
        elif key == 'SPACE':
            t.press_space()
        elif key == 'DEL':
            t.press_del()
        else:
            t.press(key)
    ok = (t.hid == expect_hid)
    mark = "PASS" if ok else "FAIL"
    print(f"  [{mark}] {name:38s} hid={t.hid!r:12s} expect={expect_hid!r}")
    if ok and t.buf != t.hid.replace('\b', ''):
        print(f"         注意: 回显 {t.buf!r} 与 hid 不一致")
    return ok


def main():
    print("=" * 70)
    print("T9 状态机验证 —— 对照需求表")
    print("=" * 70)

    results = []

    print("\n[P1-1] 重复发送（本次修复的核心）")
    results.append(case("按 2 再按 3", [('2', 0), ('3', 100)], "ad"))
    results.append(case("按 2 等超时再按 3", [('2', 0), ('3', 2000)], "ad"))

    print("\n[P1-2] 同键连击")
    results.append(case("2 连按两次 -> b", [('2', 0), ('2', 100)], "b"))
    results.append(case("2 连按三次 -> c", [('2', 0), ('2', 100), ('2', 100)], "c"))
    results.append(case("5 连按三次 -> l", [('5', 0), ('5', 100), ('5', 100)], "l"))
    results.append(case("超时后再按 2 -> 新候选", [('2', 0), ('2', 2000)], "aa"))

    print("\n[P1-3] SHIFT 一次性")
    results.append(case("默认 2 -> a", [('2', 0)], "a"))
    results.append(case("SHIFT+2 -> A", [('SHIFT', 0), ('2', 100)], "A"))
    results.append(case("SHIFT+2 连击 -> B", [('SHIFT', 0), ('2', 100), ('2', 100)], "B"))
    results.append(case("SHIFT+2 连击三次 -> C",
                        [('SHIFT', 0), ('2', 100), ('2', 100), ('2', 100)], "C"))
    results.append(case("SHIFT+2 后再按 2 -> a",
                        [('SHIFT', 0), ('2', 100), ('2', 2000)], "Aa"))

    print("\n[P1-4] CAPS")
    results.append(case("CAPS+2 -> A", [('CAPS', 0), ('2', 100)], "A"))
    results.append(case("CAPS 连输两个 -> AA",
                        [('CAPS', 0), ('2', 2000), ('2', 2000)], "AA"))
    results.append(case("CAPS+SHIFT+2 -> a",
                        [('CAPS', 0), ('SHIFT', 0), ('2', 100)], "a"))
    results.append(case("CAPS 不被候选确认清除",
                        [('CAPS', 0), ('2', 100), ('3', 2000)], "AD"))

    print("\n[P1-5] 其他路径")
    results.append(case("2 然后 SPACE", [('2', 0), ('SPACE', 100)], "a "))
    results.append(case("SHIFT 后 SPACE -> 空格不受影响",
                        [('SHIFT', 0), ('SPACE', 100)], " "))
    results.append(case("SHIFT+2 然后 SPACE 再 2",
                        [('SHIFT', 0), ('2', 100), ('SPACE', 100), ('2', 2000)], "A a"))
    results.append(case("2 然后 DEL 删除候选", [('2', 0), ('DEL', 100)], ""))
    results.append(case("SHIFT+2 然后 DEL", [('SHIFT', 0), ('2', 100), ('DEL', 100)], ""))
    results.append(case("SHIFT 后 1（数字键）", [('SHIFT', 0), ('1', 100)], "1"))

    print("\n" + "=" * 70)
    passed = sum(1 for r in results if r)
    print(f"结果: {passed}/{len(results)} 通过")
    print("=" * 70)
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
