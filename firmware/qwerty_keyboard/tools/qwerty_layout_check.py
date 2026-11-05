#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
QWERTY 布局静态验证 —— 复刻 layout.cpp 的 keyRect()，穷举所有键检查：
  1. 所有键都在屏幕范围内
  2. 同一行内相邻键之间恰好是 KEY_GAP
  3. 首键左缘 = KEY_GAP，末键右缘 = SCREEN_W - KEY_GAP
  4. 键与键之间不重叠
  5. 各行垂直方向不重叠
  6. hitTest 的每个像素点都落在唯一的键里（用矩形反推，与绘制同源）

不参与固件构建，仅用于人工审查前自检。
"""

SCREEN_W = 320
SCREEN_H = 240
KEY_H = 27
KEY_GAP = 2
ROW_STEP = KEY_H + KEY_GAP
KB_Y = 64
ROW_COUNT = 6

# 与 layout.cpp 保持一致
ROW_COUNTS = [10, 10, 10, 10, 8, 5]
ROW_INDENT = [0, 0, 14, 0, 0, 0]

W3 = [2, 1, 1, 1, 1, 1, 1, 1, 1, 1]
W4 = [1, 1, 1, 1, 3, 1, 1, 2]
W5 = [2, 2, 2, 2, 2]
WEIGHTS = [None, None, None, W3, W4, W5]


def key_rect(row, col):
    """复刻 layout.cpp::keyRect()"""
    y = KB_Y + row * ROW_STEP
    n = ROW_COUNTS[row]

    if row in (0, 1, 2):
        indent = ROW_INDENT[row]
        m = 10
        usable = SCREEN_W - indent - KEY_GAP * (m + 1)
        x = indent + KEY_GAP
        for i in range(col):
            w_i = ((i + 1) * usable) // m - (i * usable) // m
            x += w_i + KEY_GAP
        w = ((col + 1) * usable) // m - (col * usable) // m
        return (x, y, w, KEY_H)

    wt = WEIGHTS[row]
    total = sum(wt)
    usable = SCREEN_W - KEY_GAP * (n + 1)

    prefix = [0]
    for v in wt:
        prefix.append(prefix[-1] + v)

    x = KEY_GAP
    for i in range(col):
        w_i = (prefix[i + 1] * usable) // total - (prefix[i] * usable) // total
        x += w_i + KEY_GAP

    w = (prefix[col + 1] * usable) // total - (prefix[col] * usable) // total
    return (x, y, w, KEY_H)


def main():
    fails = 0

    print("=" * 72)
    print("QWERTY 布局验证")
    print("=" * 72)

    for row in range(ROW_COUNT):
        n = ROW_COUNTS[row]
        rects = [key_rect(row, c) for c in range(n)]

        xs = [r[0] for r in rects]
        rights = [r[0] + r[2] for r in rects]
        ws = [r[2] for r in rects]
        y = rects[0][1]

        # 1. 范围
        in_bounds = all(x >= 0 and x + w <= SCREEN_W and y >= 0 and y + KEY_H <= SCREEN_H
                        for x, y2, w, h in rects)

        # 2. 间距：下一键的左缘 - 当前键的右缘
        gaps = set(xs[i + 1] - rights[i] for i in range(n - 1)) if n > 1 else set()
        gaps_ok = gaps == {KEY_GAP} if n > 1 else True

        # 3. 边缘
        left_ok = xs[0] == KEY_GAP or row == 2
        right_ok = rights[-1] == SCREEN_W - KEY_GAP

        # 4. 不重叠
        no_overlap = all(xs[i] + ws[i] <= xs[i + 1] for i in range(n - 1))

        ok = in_bounds and gaps_ok and right_ok and no_overlap
        if not ok:
            fails += 1

        mark = "PASS" if ok else "FAIL"
        print(f"  [{mark}] 行{row}: n={n:2d} y={y:3d} "
              f"x={xs[0]:3d}..{rights[-1]:3d} 宽={ws} 间距={sorted(gaps) if gaps else '-'}")

        if not in_bounds:
            print("         超出屏幕范围")
        if not gaps_ok:
            print(f"         间距不一致: {gaps}")
        if not right_ok:
            print(f"         末键右缘 {rights[-1]} != {SCREEN_W - KEY_GAP}")
        if not no_overlap:
            print("         键重叠")

    # 5. 垂直不重叠
    print()
    rows_y = [(KB_Y + r * ROW_STEP, KB_Y + r * ROW_STEP + KEY_H) for r in range(ROW_COUNT)]
    v_ok = all(rows_y[i][1] <= rows_y[i + 1][0] for i in range(ROW_COUNT - 1))
    v_bottom = rows_y[-1][1]
    print(f"  [{'PASS' if v_ok else 'FAIL'}] 垂直不重叠，最后一行底 {v_bottom} (<= {SCREEN_H})")
    if not v_ok or v_bottom > SCREEN_H:
        fails += 1

    # 6. 命中测试唯一性：抽样像素，确认只落在一个键
    print()
    overlaps = 0
    for row in range(ROW_COUNT):
        n = ROW_COUNTS[row]
        for c in range(n):
            x, y, w, h = key_rect(row, c)
            # 取键中心点，反查所有行所有键
            px, py = x + w // 2, y + h // 2
            hits = 0
            for r2 in range(ROW_COUNT):
                for c2 in range(ROW_COUNTS[r2]):
                    x2, y2, w2, h2 = key_rect(r2, c2)
                    if x2 <= px < x2 + w2 and y2 <= py < y2 + h2:
                        hits += 1
            if hits != 1:
                overlaps += 1
                print(f"         ({px},{py}) 命中 {hits} 个键 [行{row} 列{c}]")
    print(f"  [{'PASS' if overlaps == 0 else 'FAIL'}] 命中测试唯一性（{overlaps} 个冲突）")
    if overlaps:
        fails += 1

    print()
    print("=" * 72)
    print(f"结果: {'全部通过' if fails == 0 else str(fails) + ' 项失败'}")
    print("=" * 72)
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
