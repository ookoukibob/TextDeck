#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""故障注入测试 — 仅针对 **TEXT_STORE_FAULT_INJECT=1 测试构建** 固件 (模拟测试)。

覆盖验收项 (10.2):
  #6 写入备用槽位时失败 (写前失败 / 半截写入) → 旧有效文本仍可读、重启后仍恢复
  #7 新槽位文件头损坏或正文 CRC 错误 → 不被采信, 启动加载另一有效槽的旧内容
  #8 挂载失败时的格式化前置判据: 分区已含本项目数据 → 必须拒绝格式化 (只读探测)

说明:
  - 生产构建 (无 TEXT_STORE_FAULT_INJECT) 不存在 0x7E 命令, 本工具会因
    ST_ERR_CMD 而明确失败 — 因此它**只在故障注入测试构建上执行**。
  - 所有断言动态获取 (当前文本/seq/lastSlot), 不硬编码槽位状态。
  - 重启 = 关闭串口后重新打开 + RTS 复位脉冲, 抓取启动日志关键行。

用法: python tools/fault_inject_tests.py [--port COM4] [--log <path>]
"""
import argparse
import os
import sys
import time
import serial

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "desktop"))
from hid_text_writer import (  # noqa: E402
    DeviceClient, DeviceError, CMD_WRITE,
    ST_OK, ST_ERR_STORAGE,
)

FI_CMD = 0x7E                      # 测试构建专用命令 (生产构建不存在)
PASS, FAIL = "PASS", "FAIL"
_results = []


def log(f, msg):
    line = f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {msg}"
    print(line, flush=True)
    f.write(line + "\n")
    f.flush()


def check(f, name, cond, detail=""):
    st = PASS if cond else FAIL
    _results.append((st, name))
    log(f, f"[{st}] {name}" + (f" | {detail}" if detail else ""))
    return cond


def fi(c, op, which=None):
    """发送故障注入命令, 返回 (status, op, result/lastslot+1, failmode)

    op=6 时返回 (status, op, scan+1, fsMounted) — 只读探测, 不改动任何数据。
    """
    payload = bytes([op]) + (bytes([which]) if which is not None else b"")
    pl = c._request(FI_CMD, payload)
    if len(pl) < 4:
        raise DeviceError(f"FI 响应长度异常 {len(pl)}")
    return pl[0], pl[1], pl[2], pl[3]


def raw_write(c, text: str):
    """原始 WRITE, 不抛状态异常 — 返回解析后的 14B 响应字段"""
    pl = c._request(CMD_WRITE, text.encode("ascii"))
    if len(pl) != 14:
        raise DeviceError(f"WRITE 响应长度异常 {len(pl)}")
    return {
        "status": pl[0],
        "saved_len": pl[3] | (pl[4] << 8),
        "saved_crc": int.from_bytes(pl[5:9], "little"),
        "seq": int.from_bytes(pl[9:13], "little"),
        "written": bool(pl[13]),
    }


def reboot_capture(c, seconds: float = 8.0) -> str:
    """复位设备并抓取一次完整启动日志 (与 tools/capture_serial.py --reset 同法)。

    Windows 上同一 COM 口不能被两个句柄同时打开, 因此先关闭客户端的串口,
    抓完启动日志后再由 c.open() 重新打开并等固件就绪。
    """
    port = c.port
    c.close()
    time.sleep(0.3)
    ser = serial.Serial(port, 115200, timeout=0.2)
    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)
    time.sleep(0.1)
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        d = ser.read(4096)
        if d:
            buf += d
    ser.close()
    c.open()
    return buf.decode("utf-8", errors="replace")


def slot_name(slot: int) -> str:
    return "/textA.bin" if slot == 0 else "/textB.bin"


BANNER = "[boot] esp32_hid_text_device fw"


def boot_tail(lines: str) -> str:
    """取最后一次"启动横幅"之后的行 (即最近一次真实启动的日志)。

    FTDI 驱动复位窗口回放可能把本次复位前的历史行也灌进来
    (见 upgrade10k_replay_flood_evidence.log)，所以只分析
    最后一次启动的尾部，避免用旧启动行做断言。

    注意: 一次启动里有**两行** `[boot]` 前缀 (横幅 + ready)，
    因此不能用 `rsplit("[boot] ")` — 那样会把中间的 [fs]/[store] 行丢掉。
    这里以横幅行 `[boot] esp32_hid_text_device fw` 作为本次启动的起点。
    """
    i = lines.rfind(BANNER)
    if i >= 0:
        return lines[i:]
    j = lines.rfind("[boot] ")
    return lines[j:] if j >= 0 else lines


def loaded_slots(tail: str):
    """尾部中出现过的所有 `loaded from filesystem <path>` 的槽位路径集合"""
    out = set()
    for l in tail.splitlines():
        if "loaded from filesystem" in l:
            out.add("/textA.bin" if "/textA.bin" in l else "/textB.bin")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--log", default=None)
    args = ap.parse_args()

    log_path = args.log or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..",
        "build-logs", "esp32_fault_inject.log")
    # A fresh clone has no build-logs/ yet.
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    f = open(log_path, "w", encoding="utf-8")
    log(f, "=== 故障注入测试 (仅 TEXT_STORE_FAULT_INJECT 测试构建; 模拟测试) ===")
    log(f, f"port={args.port} 覆盖: 备用槽写失败保护 / 槽文件头损坏 / 槽 CRC 损坏 / 格式化前置判据")

    c = DeviceClient(args.port)
    try:
        c.open()
    except DeviceError as e:
        log(f, f"[FAIL] 打开设备失败: {e}")
        f.close()
        return 1

    try:
        # ---- T1: 确认测试构建在线 ----
        st, op, last1, fmode = fi(c, 0)
        if st != ST_OK:
            log(f, f"[FAIL] 0x7E 状态查询被拒 — 固件不是故障注入测试构建? status={st}")
            f.close(); c.close(); return 1
        L0 = last1 - 1
        check(f, "T1 测试构建在线 (0x7E 状态查询 OK)", L0 in (0, 1) and fmode == 0,
              f"lastSlot={L0} failMode={fmode}")

        b = c.read_text()
        B0, N0 = b["text"], b["seq"]
        check(f, "T1 基线文本就绪", b["crc_ok"] and b["len"] > 0,
              f"len={b['len']} crc=0x{b['crc']:08X} seq={N0}")

        # ---- T0: 只读探测 — 验证"挂载失败时不无条件格式化"的前置判据 ----
        # 判据链条 (固件 mountFilesystem()):
        #   ① 先 SPIFFS.begin(false) 不格式化挂载; ② 失败后确认 spiffs 分区存在;
        #   ③ 原始扫描分区, 只有"确认不存在任何本项目数据"才做一次性初始化;
        #   ④ 扫描出错 / 已发现项目数据 / 分区不存在 → 一律拒绝格式化。
        # 本测试用只读扫描验证 ③④ 所依赖的输入: 设备当前 spiffs 分区**确实**含本项目
        # 数据 (即此刻若挂载失败, 固件必须走"拒绝格式化"分支)。
        st, _, scan_p1, mounted = fi(c, 6)
        check(f, "T0 spiffs 原始扫描确认已含本项目文件数据 (挂载失败时必须拒绝格式化)",
              st == ST_OK and scan_p1 == 2 and mounted == 1,
              f"scan+1={scan_p1} (2=已发现) fsMounted={mounted}")

        # ---- T2: 写前失败注入 (备用槽尚未被碰) ----
        st, _, res, _ = fi(c, 3)
        check(f, "T2 写前失败注入已武装", st == ST_OK and res == 1, f"res={res}")
        w = raw_write(c, "FAULT pre-write fail X1")
        check(f, "T2 注入后 WRITE 返回 ST_ERR_STORAGE",
              w["status"] == ST_ERR_STORAGE and not w["written"],
              f"status={w['status']} written={w['written']}")
        r = c.read_text()
        check(f, "T2 旧文本仍完好 (运行时)", r["text"] == B0 and r["seq"] == N0,
              f"len={r['len']} seq={r['seq']}")

        # ---- T3: 半截写失败注入 (目标备用槽只写头即失败) ----
        st, _, res, _ = fi(c, 4)
        check(f, "T3 半截写失败注入已武装", st == ST_OK and res == 1, f"res={res}")
        w = raw_write(c, "FAULT partial write X2")
        check(f, "T3 注入后 WRITE 返回 ST_ERR_STORAGE",
              w["status"] == ST_ERR_STORAGE and not w["written"],
              f"status={w['status']}")
        r = c.read_text()
        check(f, "T3 旧文本仍完好 (运行时)", r["text"] == B0 and r["seq"] == N0,
              f"seq={r['seq']}")

        # ---- T4: 重启 → 半截文件必须被拒, 加载另一有效槽 ----
        lines = reboot_capture(c)
        tail = boot_tail(lines)
        check(f, "T4 重启加载未损坏槽 (半截文件被拒)",
              f"loaded from filesystem {slot_name(L0)}" in tail,
              next((l for l in tail.splitlines() if "loaded from" in l), "无加载行"))
        r = c.read_text()
        check(f, "T4 重启后旧文本恢复", r["text"] == B0 and r["seq"] == N0,
              f"len={r['len']} seq={r['seq']}")

        # ---- T5: 正常写入 → 覆盖修复损坏的备用槽 ----
        Y1 = "FAULT round repaired target slot Y1"
        info = c.write_text(Y1)
        r = c.read_text()
        check(f, "T5 正常写入成功 (同时修复损坏备用槽)",
              info["written"] and r["text"] == Y1, f"seq={r['seq']}")
        P = 1 - L0    # T5 后 active 槽 (Y1); inactive = L0 (基线)

        # ---- T6: 破坏 active 槽文件头 → 运行时不变; 重启加载另一槽旧内容 ----
        st, _, res, _ = fi(c, 1, 2)      # which=2 → active (持有 Y1 的新槽)
        check(f, "T6 active 文件头破坏成功", st == ST_OK and res == 1, f"res={res}")
        r = c.read_text()
        check(f, "T6 破坏后运行时 READ 不受影响 (RAM 副本)", r["text"] == Y1,
              f"len={r['len']} seq={r['seq']}")
        lines = reboot_capture(c)
        check(f, "T6 重启选另一有效槽 (文件头损坏的新槽被拒)",
              f"loaded from filesystem {slot_name(L0)}" in boot_tail(lines),
              next((l for l in boot_tail(lines).splitlines() if "loaded from" in l),
                   "无加载行"))
        r = c.read_text()
        check(f, "T6 恢复的是另一槽原有旧文本", r["text"] == B0 and r["seq"] == N0,
              f"len={r['len']} seq={r['seq']} (期望基线 seq={N0})")

        # ---- T7: 修复出两槽有效后, 翻转 active 正文首字节 (CRC 失效) ----
        Y3 = "FAULT crc-flip scenario Y3"
        c.write_text(Y3)                # 修复 T6 破坏的槽, 使两槽再次有效 (active=P)
        st, _, res, _ = fi(c, 2, 2)     # which=2 → active (Y3, 新槽) 正文翻转
        check(f, "T7 active 正文翻转成功 (CRC 失效)", st == ST_OK and res == 1,
              f"res={res}")
        r = c.read_text()
        check(f, "T7 破坏后运行时 READ 不受影响 (RAM 副本)", r["text"] == Y3,
              f"len={r['len']} seq={r['seq']}")
        lines = reboot_capture(c)
        check(f, "T7 重启选另一有效槽 (CRC 校验拦截损坏新槽)",
              f"loaded from filesystem {slot_name(L0)}" in boot_tail(lines),
              next((l for l in boot_tail(lines).splitlines() if "loaded from" in l),
                   "无加载行"))
        r = c.read_text()
        check(f, "T7 恢复的仍是另一槽旧文本", r["text"] == B0 and r["seq"] == N0,
              f"seq={r['seq']} (期望 {N0})")

        # ---- T8: 双槽全部损坏 → 明确日志的 legacy NVS 降级恢复 (非假装最新) ----
        st, _, res, _ = fi(c, 1, 2)     # 破坏仅存的 active 槽 (L0) 文件头 → 两槽皆坏
        check(f, "T8 第二槽文件头破坏成功 (制造双槽全坏)", st == ST_OK and res == 1,
              f"res={res}")
        lines = reboot_capture(c)
        tail = boot_tail(lines)
        has_fallback = "found legacy NVS" in tail
        has_fake_fs = loaded_slots(tail) != set()
        check(f, "T8 重启走 legacy 降级路径且日志明确 (不假装新版本)",
              has_fallback and not has_fake_fs,
              next((l for l in tail.splitlines() if "legacy NVS" in l), "无降级日志"))
        # 降级恢复的必须是 legacy NVS 里的旧文本 (22 字节基线), **不是**文件系统里的
        # 10240 字节文本。直接从启动日志解析 legacy 的 len/crc/seq 再与 READ 比对,
        # 不硬编码任何期望值。
        import re as _re
        m = _re.search(r"found legacy NVS (\w+): len=(\d+) crc=([0-9A-Fa-f]{8}) seq=(\d+)", tail)
        r = c.read_text()
        if m:
            l_len, l_crc, l_seq = int(m.group(2)), int(m.group(3), 16), int(m.group(4))
            ok = (r["len"] == l_len and r["crc"] == l_crc and r["seq"] == l_seq
                  and r["crc_ok"] and r["text"] != Y3)
            detail = (f"legacy({m.group(1)}) len={l_len} crc=0x{l_crc:08X} seq={l_seq} "
                      f"| READ len={r['len']} crc=0x{r['crc']:08X} seq={r['seq']}")
        else:
            ok = False
            detail = "启动日志中未解析到 legacy len/crc/seq"
        check(f, "T8 降级恢复的文本与 legacy NVS 原文一致 (len/CRC/seq 逐项比对)",
              ok, detail)

        # ---- T8b: 迁移必须"回读验证通过"才算成功, 且明确记录 (不静默假装) ----
        mig = [l for l in tail.splitlines()
               if "migrat" in l.lower() or "MIGRATION" in l]
        for l in mig:
            log(f, f"    [device] {l.strip()}")
        verified = any("migrated legacy NVS -> /textA.bin VERIFIED" in l for l in mig)
        failed = any("MIGRATION FAILED" in l for l in mig)
        check(f, "T8b legacy → 新格式迁移已回读验证通过 (或明确报告失败, 不静默)",
              verified and not failed,
              next((l.strip() for l in mig if "VERIFIED" in l or "FAILED" in l),
                   "无迁移结果行"))

        # ---- T9: 恢复 — 正常写入修复全部槽 → 写回原始基线 → 终检 ----
        Y4 = "FAULT full recovery Y4"
        info = c.write_text(Y4)         # 写入另一槽, 顺带修复其中一个损坏文件
        check(f, "T9 降级状态下仍可正常保存", info["written"],
              f"seq={info['seq']}")
        c.write_text(B0)                # 写回原始基线 (与 Y4 不同 → 必然落盘)
        r = c.read_text()
        st, _, last1, fmode = fi(c, 0)
        check(f, "T9 已写回原始基线且注入清零",
              r["text"] == B0 and r["crc_ok"] and fmode == 0,
              f"seq={r['seq']} failMode={fmode}")
        lines = reboot_capture(c)
        tail = boot_tail(lines)
        r2 = c.read_text()
        check(f, "T9 终检: 重启从文件系统加载基线文本",
              loaded_slots(tail) != set() and r2["text"] == B0
              and r2["crc_ok"],
              f"seq={r2['seq']}")

    except DeviceError as e:
        log(f, f"[FAIL] 测试异常中断: {e}")
    finally:
        try:
            c.close()
        except Exception:
            pass

    fails = [n for s, n in _results if s == FAIL]
    log(f, f"=== 结果: {len(_results) - len(fails)}/{len(_results)} 通过 ===")
    for n in fails:
        log(f, f"  FAILED: {n}")
    f.close()
    print(f"[saved] {log_path}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
