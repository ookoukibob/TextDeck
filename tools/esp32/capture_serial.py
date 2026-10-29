#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""抓取 COM 口串口日志到文件 (验证用)。

用法: python tools/capture_serial.py --port COM4 --seconds 5 --out test_logs/xxx.log
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--out", default=None)
    ap.add_argument("--reset", action="store_true",
                    help="开始抓取前先发一次 RTS 复位脉冲 (等效按 RESET)")
    ap.add_argument("--ts", action="store_true",
                    help="为每个读取块附加到达时间戳 [HH:MM:SS.mmm] (用于测量轮次耗时)")
    args = ap.parse_args()

    s = serial.Serial(args.port, args.baud, timeout=0.2)
    s.reset_input_buffer()
    if args.reset:
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.15)
        s.setRTS(False)

    t0 = time.time()
    buf = b""
    while time.time() - t0 < args.seconds:
        d = s.read(4096)
        if d:
            if args.ts:
                stamp = time.strftime("%H:%M:%S", time.localtime()) + \
                    ".%03d" % (int((time.time() % 1) * 1000))
                buf += ("[%s] " % stamp).encode("utf-8") + d
            else:
                buf += d
    s.close()

    text = buf.decode("utf-8", errors="replace")
    print(text)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"[saved] {args.out} ({len(buf)} bytes)", file=sys.stderr)


if __name__ == "__main__":
    main()
