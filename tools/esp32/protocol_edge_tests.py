#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fix_round 设备端协议边界测试 (10 KiB 升级版)。

与桌面程序共用 build_frame/帧扫描/DeviceClient, 但**故意绕过桌面端 validate_text**,
直接构造原始 payload 发给设备 —— 以设备端固件的实际响应为准。
覆盖:
  1. PING: 实际响应长度=16, 字段合法, max_text=10240, hid_ready 取值
  2. 正常写入 → 读回校验 (CRC/文本一致)
  3. 重复写入 → written=0, seq 不变
  4. 空 payload WRITE → 设备端拒绝 (ST_ERR_LENGTH), 不改写存储
  5. 非法字符 WRITE → ST_ERR_CHARSET, detail=首个坏字符下标, 不改写存储
  6. 超长 WRITE (MAX_TEXT_LEN+1 = 10241B) → ST_ERR_LENGTH, 不改写存储
  7. 容量边界: 1024 / 1025 / 4096 / 恰 10240 字节经真实协议写入+读回逐字节比对;
     桌面端 10241 字节预校验拒绝; 10240 字节重复写不递增 seq
  8. 结束时恢复基线文本

用法: python tools/protocol_edge_tests.py [--port COM4] [--log <path>]
"""
import argparse
import os
import sys
import time
import zlib

# The desktop writer lives in tools/esp32-desktop (sibling of tools/esp32).
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "esp32-desktop"))
from hid_text_writer import (  # noqa: E402
    DeviceClient, DeviceError,
    CMD_WRITE, CMD_READ, CMD_PING,
    ST_OK, ST_ERR_LENGTH, ST_ERR_CHARSET, MAX_TEXT_LEN, validate_text,
)

PASS, FAIL = "PASS", "FAIL"
_results = []


def log(f, msg):
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    print(line)
    f.write(line + "\n")
    f.flush()


def check(f, name, cond, detail=""):
    st = PASS if cond else FAIL
    _results.append((st, name, detail))
    log(f, f"[{st}] {name}" + (f" | {detail}" if detail else ""))
    return cond


def parse_write_resp(pl):
    return {
        "status": pl[0],
        "detail": pl[1] | (pl[2] << 8),
        "saved_len": pl[3] | (pl[4] << 8),
        "saved_crc": int.from_bytes(pl[5:9], "little"),
        "seq": int.from_bytes(pl[9:13], "little"),
        "written": pl[13],
        "raw_len": len(pl),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--log", default=None)
    args = ap.parse_args()

    log_path = args.log or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..",
        "build-logs", "esp32_protocol_edge.log")
    # A fresh clone has no build-logs/ yet.
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    f = open(log_path, "w", encoding="utf-8")
    log(f, "=== 设备端协议边界测试 (10 KiB; 绕过桌面端预校验, 以设备端行为为准) ===")
    log(f, f"源码基线: 10 KiB 升级 (基于 dd1de8b) | MAX_TEXT_LEN={MAX_TEXT_LEN} | port={args.port}")

    c = DeviceClient(args.port)
    try:
        c.open()
    except DeviceError as e:
        log(f, f"[FAIL] 打开设备失败: {e}")
        f.close()
        return 1

    try:
        # ---- 1. PING ----
        info = c.ping()
        check(f, "PING 响应长度 == 16", info.get("payload_len") == 16,
              f"payload_len={info.get('payload_len')}")
        check(f, "PING hid_ready 为合法 0/1 且已解析",
              isinstance(info.get("hid_ready"), bool),
              f"hid_ready={info.get('hid_ready')}")
        check(f, "PING 协议/固件版本 (1.1.0)", info["proto"] == 1 and info["fw_ver"] == 0x0101,
              f"proto=v{info['proto']} fw=0x{info['fw_ver']:04X}")
        check(f, "PING max_text == 10240", info["max_text"] == 10240,
              f"max_text={info['max_text']}")

        # ---- 2. 基线读取 ----
        base = c.read_text()
        base_text, base_crc, base_seq = base["text"], base["crc"], base["seq"]
        log(f, f"baseline: len={base['len']} crc=0x{base_crc:08X} seq={base_seq} "
               f"text={base_text!r}")
        check(f, "基线读回 CRC 自洽", base["crc_ok"])
        if base["len"] == 0:
            log(f, "[FAIL] 设备当前无已存文本, 无法做拒绝后保持性断言, 中止")
            c.close(); f.close(); return 1

        def expect_unchanged(tag, resp_pl):
            w = parse_write_resp(resp_pl)
            ok = (w["status"] != ST_OK
                  and w["saved_len"] == base["len"]
                  and w["saved_crc"] == base_crc
                  and w["seq"] == base_seq)
            check(f, f"{tag}: 拒绝且 saved_len/crc/seq 保持原状",
                  ok, f"status={w['status']} saved_len={w['saved_len']} "
                      f"crc=0x{w['saved_crc']:08X} seq={w['seq']}")
            return w

        # ---- 3. 空文本 WRITE (原始帧, 桌面端不参与) ----
        pl = c._request(CMD_WRITE, b"")
        w1 = expect_unchanged("空文本 WRITE 被设备拒绝", pl)
        check(f, "空文本拒绝状态码 == ST_ERR_LENGTH(2)",
              w1["status"] == ST_ERR_LENGTH, f"status={w1['status']}")
        check(f, "空文本拒绝响应长度 == 14", w1["raw_len"] == 14,
              f"raw_len={w1['raw_len']}")
        check(f, "空文本拒绝 written=0 (未写 Flash)", w1["written"] == 0,
              f"written={w1['written']}")

        # 拒绝后读回: 原文本必须完好
        r = c.read_text()
        check(f, "空文本拒绝后 READ 原文本完好",
              r["text"] == base_text and r["crc"] == base_crc and r["seq"] == base_seq,
              f"len={r['len']} crc=0x{r['crc']:08X} seq={r['seq']}")

        # 第二次空拒绝: seq 仍不得递增
        pl = c._request(CMD_WRITE, b"")
        expect_unchanged("重复空文本 WRITE (seq 仍不变)", pl)

        # ---- 4. 非法字符 WRITE ----
        raw = b"AB\xFFCD"
        pl = c._request(CMD_WRITE, raw)
        w = expect_unchanged("非法字符 WRITE 被设备拒绝", pl)
        check(f, "非法字符状态码 == ST_ERR_CHARSET(3), detail=2",
              w["status"] == ST_ERR_CHARSET and w["detail"] == 2,
              f"status={w['status']} detail={w['detail']}")

        # ---- 5. 超长 WRITE (MAX_TEXT_LEN+1 = 10241B, 帧本身合法 → 由设备长度校验拒绝) ----
        pl = c._request(CMD_WRITE, b"A" * (MAX_TEXT_LEN + 1))
        w = expect_unchanged("超长 WRITE 被设备拒绝", pl)
        check(f, "超长状态码 == ST_ERR_LENGTH(2)",
              w["status"] == ST_ERR_LENGTH, f"status={w['status']}")

        # ---- 6. 正常写入 + 读回 ----
        test_text = "FIX ROUND OK abc123"
        info_w = c.write_text(test_text)
        check(f, "正常写入 status=OK, written=1, seq+1",
              info_w["status"] == ST_OK and info_w["written"]
              and info_w["seq"] == base_seq + 1,
              f"seq={info_w['seq']} (base={base_seq}) crc=0x{info_w['saved_crc']:08X}")
        r = c.read_text()
        check(f, "正常写入读回一致", r["text"] == test_text and r["crc_ok"],
              f"len={r['len']} crc=0x{r['crc']:08X}")

        # ---- 7. 重复写入 ----
        info_d = c.write_text(test_text)
        check(f, "重复写入 written=0, seq 不变",
              info_d["written"] is False and info_d["seq"] == base_seq + 1,
              f"written={info_d['written']} seq={info_d['seq']}")

        # ---- 8. 容量边界测试 (10 KiB 升级, 全部经真实协议往返) ----
        log(f, f"=== 容量边界测试 (MAX_TEXT_LEN={MAX_TEXT_LEN} 字节) ===")
        cur = c.read_text()
        cur_seq = cur["seq"]

        def roundtrip(name, text: str):
            nonlocal cur_seq
            raw = text.encode("ascii")
            exp_len = len(raw)
            exp_crc = zlib.crc32(raw) & 0xFFFFFFFF
            try:
                w = c.write_text(text)
                r = c.read_text()
            except DeviceError as e:
                check(f, name, False, f"异常: {e}")
                raise
            ok = (r["text"] == text and r["crc_ok"] and r["crc"] == exp_crc
                  and r["len"] == exp_len and w["saved_len"] == exp_len)
            check(f, name, ok,
                  f"len={r['len']} crc=0x{r['crc']:08X} seq={r['seq']}")
            cur_seq = r["seq"]
            return r

        t1024 = "A" * 1023 + "\n"                        # 恰 1024 字节
        t1025 = "B" * 1024 + "\t"                        # 1025 = 旧上限+1
        unit4096 = "0123456789" * 4 + "\n"               # 41 字节/行
        t4096 = (unit4096 * 120)[:4096]                  # 多行 + 换行
        unit10k = "ABCDEFGHIJabcdefghij0123456789 \t*\n"  # 34 字节, 含 \t 与 \n
        t10240 = (unit10k * 400)[:10240]
        check(f, "测试数据长度自检 (1024/1025/4096/10240)",
              (len(t1024.encode()), len(t1025.encode()),
               len(t4096.encode()), len(t10240.encode())) == (1024, 1025, 4096, 10240),
              f"{len(t1024)}/{len(t1025)}/{len(t4096)}/{len(t10240)}")

        roundtrip("1024 字节文本 写入+读回逐字节一致", t1024)
        roundtrip("1025 字节文本 (旧上限+1) 写入+读回逐字节一致", t1025)
        roundtrip("4096 字节多行文本 写入+读回逐字节一致", t4096)
        roundtrip("恰 10240 字节 (10 KiB, 含换行/制表符) 写入+读回逐字节一致", t10240)

        # 桌面端预校验: 10241 字节在客户端即拒绝 (设备端 10241 拒绝见第 5 节)
        check(f, "桌面端预校验拒绝 10241 字节",
              validate_text("C" * (MAX_TEXT_LEN + 1)) is not None,
              f"limit={MAX_TEXT_LEN}")

        # PING 视角确认设备侧已按 10240 保存, 且 PING 仍为固定 16 字节
        info = c.ping()
        check(f, "PING saved_len=10240 / max_text=10240 / payload=16",
              info["payload_len"] == 16 and info["saved_len"] == 10240
              and info["max_text"] == 10240,
              f"payload_len={info['payload_len']} saved_len={info['saved_len']} "
              f"max_text={info['max_text']}")

        # 10240 字节内容重复写入 → 设备去重: 不写介质, seq 不变
        w2 = c.write_text(t10240)
        check(f, "10240 字节重复写入 written=0, seq 不变",
              w2["written"] is False and w2["seq"] == cur_seq,
              f"written={w2['written']} seq={w2['seq']} (期望 {cur_seq})")

        # 在 10240 已保存状态下再发一次空 payload WRITE → 拒绝且不破坏
        w = parse_write_resp(c._request(CMD_WRITE, b""))
        check(f, "10240 保存状态下空 WRITE 仍被拒绝且存储不变",
              w["status"] == ST_ERR_LENGTH and w["saved_len"] == 10240
              and w["seq"] == cur_seq,
              f"status={w['status']} saved_len={w['saved_len']} seq={w['seq']}")
        r = c.read_text()
        check(f, "拒绝后读回仍为完整 10240 文本 (CRC 一致)",
              r["text"] == t10240 and r["crc_ok"],
              f"len={r['len']} crc=0x{r['crc']:08X}")

        # ---- 9. 恢复基线文本 ----
        c.write_text(base_text)
        r = c.read_text()
        check(f, "已恢复基线文本",
              r["text"] == base_text and r["crc"] == base_crc,
              f"seq={r['seq']} (原 {base_seq}, 写入导致 +1 属预期)")

        info = c.ping()
        log(f, f"final ping: payload_len={info['payload_len']} "
               f"hid_ready={info['hid_ready']} saved_len={info['saved_len']} "
               f"crc=0x{info['saved_crc']:08X}")
    except DeviceError as e:
        log(f, f"[FAIL] 测试异常中断: {e}")
        c.close(); f.close(); return 1
    finally:
        try:
            c.close()
        except Exception:
            pass

    fails = [r for r in _results if r[0] == FAIL]
    log(f, f"=== 结果: {len(_results) - len(fails)}/{len(_results)} 通过 ===")
    if fails:
        for st, name, detail in fails:
            log(f, f"  FAILED: {name} | {detail}")
    f.close()
    print(f"[saved] {log_path}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
