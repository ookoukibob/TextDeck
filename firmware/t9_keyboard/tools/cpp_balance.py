#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
C/C++ 源码括号平衡检查 —— 剥离注释与字符串字面量后再统计。
只做静态检查，不参与固件构建、不触发编译。
"""
import sys
import io


def strip_noise(src):
    """移除 // 注释、/* */ 注释、字符串和字符字面量，返回干净代码。"""
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        # 行注释
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
            continue
        # 块注释
        if c == '/' and i + 1 < n and src[i + 1] == '*':
            i += 2
            while i + 1 < n and not (src[i] == '*' and src[i + 1] == '/'):
                i += 1
            i += 2
            continue
        # 字符串
        if c == '"':
            i += 1
            while i < n:
                if src[i] == '\\':
                    i += 2
                    continue
                if src[i] == '"':
                    i += 1
                    break
                i += 1
            out.append('""')
            continue
        # 字符字面量
        if c == "'":
            i += 1
            while i < n:
                if src[i] == '\\':
                    i += 2
                    continue
                if src[i] == "'":
                    i += 1
                    break
                i += 1
            out.append("''")
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def check(path):
    with io.open(path, encoding='utf-8') as f:
        src = f.read()
    clean = strip_noise(src)

    pairs = {'(': ')', '[': ']', '{': '}'}
    stack = []
    line = 1
    errors = []

    for ch in clean:
        if ch == '\n':
            line += 1
            continue
        if ch in pairs:
            stack.append((ch, line))
        elif ch in ')]}':
            if not stack:
                errors.append(f"  行 {line}: 多余的 '{ch}'")
                continue
            op, ol = stack.pop()
            if pairs[op] != ch:
                errors.append(f"  行 {line}: '{ch}' 与行 {ol} 的 '{op}' 不匹配")

    for op, ol in stack:
        errors.append(f"  行 {ol}: '{op}' 未闭合")

    counts = {k: clean.count(k) for k in '()[]{}'}
    status = "OK" if not errors else "FAIL"
    print(f"[{status}] {path}")
    print(f"       ( {counts['(']} ) {counts[')']}   "
          f"[ {counts['[']} ] {counts[']']}   "
          f"{{ {counts['{']} }} {counts['}']}")
    for e in errors:
        print(e)
    return len(errors) == 0


def main():
    files = sys.argv[1:]
    if not files:
        print("用法: python cpp_balance.py <file> [...]")
        return 2
    ok = all(check(f) for f in files)
    print()
    print("全部通过" if ok else "存在问题")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
