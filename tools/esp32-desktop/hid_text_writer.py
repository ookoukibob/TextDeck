#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
hid_text_writer - ESP32-S3 HID 文本输入器 的 Windows 桌面写入程序

功能:
  - 枚举/选择串口, 把文本通过自定义帧协议写入设备存储 (SPIFFS 双槽位文件;
    升级前的旧 NVS 文本由固件开机自动迁移) 并读取回校验
  - 只负责“写入设备”, 不触发任何键盘输入 (不存在剪贴板/快捷键/模拟输入)
  - 关闭程序后不驻留、不占用串口

用法:
  图形界面:      python hid_text_writer.py
  命令行写入:    python hid_text_writer.py --port COM4 --write "ESP32-S3 HID TEST 123!"
  命令行读取:    python hid_text_writer.py --port COM4 --read
  查询设备:      python hid_text_writer.py --port COM4 --ping
  GUI 自测:      python hid_text_writer.py --self-test --port COM4 --write "..."
"""

import argparse
import sys
import time
import zlib

import serial
from serial.tools import list_ports

# ---------------- 协议常量 (与固件 protocol.h 完全一致) ----------------
MAGIC0 = 0xE7
MAGIC1 = 0x53
VERSION = 0x01
HEADER_LEN = 10
MAX_TEXT_LEN = 10240                    # 10 KiB, 与固件 protocol.h 一致

CMD_WRITE = 0x01
CMD_READ = 0x02
CMD_PING = 0x03

ST_OK = 0
ST_ERR_CRC = 1
ST_ERR_LENGTH = 2
ST_ERR_CHARSET = 3
ST_ERR_STORAGE = 4
ST_ERR_CMD = 5
ST_ERR_BUSY = 6
ST_ERR_FRAME = 7
STATUS_TEXT = {
    0: "OK",
    1: "CRC 校验失败",
    2: "长度非法/超出上限",
    3: "含不支持的字符",
    4: "设备存储失败",
    5: "未知命令",
    6: "设备忙碌",
    7: "帧格式错误",
}

CHARSET_NOTE = ("支持字符: 英文字母、数字、常见英文标点、空格、换行(Enter)、制表符(Tab) "
                "—— 即 ASCII 0x20-0x7E + \\n + \\t (US 键盘布局)。"
                "不支持: 中文/Emoji/其他 Unicode、CR(\\r)。")

MAX_READ_PAYLOAD = 13 + MAX_TEXT_LEN     # 10253; 最大完整帧 = 10 + 10253 = 10263
PING_PAYLOAD_LEN = 16

# ---- 串口传输超时: 按帧长计算, 不用固定短窗, 也不放大到数十秒 ----
# 115200 baud 8N1 = 10 bit/字节 → 线路 11520 字节/秒
SERIAL_BYTES_PER_SEC = 11520.0
# 各命令的预期响应帧长 (header + payload)
RESP_FRAME_LEN = {
    CMD_WRITE: HEADER_LEN + 14,               # 24
    CMD_READ:  HEADER_LEN + MAX_READ_PAYLOAD, # 10263 (10 KiB 文本时)
    CMD_PING:  HEADER_LEN + PING_PAYLOAD_LEN, # 26
}
# 单次操作期限 = 固定处理预算 1.0s (设备解析/存储回读) + 2×(请求帧+响应帧)传输时间
#   PING  ≈ 1.0 + 2×36/11520   ≈ 1.01 s
#   WRITE 10 KiB ≈ 1.0 + 2×(10250+24)/11520 ≈ 2.80 s
#   READ  10 KiB ≈ 1.0 + 2×(10+10263)/11520 ≈ 2.80 s
DEADLINE_FIXED = 1.0
DEADLINE_WIRE_FACTOR = 2.0
# 重试次数 (整个期限耗尽才重试):
#   WRITE=2 — 设备端对完全相同的内容去重 (不重复写介质/不重复递增 seq), 重发安全;
#             长度/字符集错误是确定性响应, 重试不会产生错误报告
#   READ/PING=4 — 幂等, 且覆盖开串口复位后固件启动 (含首次开机文件系统初始化) 的等待
REQUEST_RETRIES = {CMD_WRITE: 2, CMD_READ: 4, CMD_PING: 4}


class DeviceError(Exception):
    """通信或设备返回错误"""


# ---------------- 协议帧 ----------------
def build_frame(cmd: int, payload: bytes) -> bytes:
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    return (bytes([MAGIC0, MAGIC1, VERSION, cmd])
            + len(payload).to_bytes(2, "little")
            + crc.to_bytes(4, "little")
            + payload)


# ---------------- 协议帧扫描 (跳过固件启动日志/FTDI 复位回放等垃圾字节) ----------------
def scan_next_frame(buf: bytearray):
    """旧式一次性扫描 (保留兼容)。新代码请用 FrameScanner (增量, 无 O(n^2))。"""
    while True:
        idx = -1
        for i in range(len(buf) - 1):
            if buf[i] == MAGIC0 and buf[i + 1] == MAGIC1:
                idx = i
                break
        if idx < 0:
            if buf and buf[-1] == MAGIC0:
                del buf[:-1]
            else:
                buf.clear()
            return None
        if idx > 0:
            del buf[:idx]
        if len(buf) < HEADER_LEN:
            return None
        ver = buf[2]
        ln = buf[4] | (buf[5] << 8)
        if ver != VERSION or ln > MAX_READ_PAYLOAD:
            del buf[0]
            continue
        total = HEADER_LEN + ln
        if len(buf) < total:
            return None
        crc = int.from_bytes(buf[6:10], "little")
        payload = bytes(buf[HEADER_LEN:total])
        if zlib.crc32(payload) & 0xFFFFFFFF != crc:
            del buf[0:2]
            continue
        cmd = buf[3]
        del buf[:total]
        return (cmd, payload)


class FrameScanner:
    """增量帧扫描器：语义与 scan_next_frame 一致，但只在新数据到达时推进。

    复位回放洪水 (FTDI 驱动层, 可达数万字节) 会让旧式全缓冲重扫呈 O(n^2) 开销，
    在长洪流下把响应帧埋住。这里 magic 查找用 C 实现的 bytes.find、垃圾字节
    用索引跳过（定期压缩），内存与 CPU 都保持恒定。
    """

    def __init__(self):
        self.buf = bytearray()
        self.skip = 0    # 已判定为垃圾/待压缩的前缀长度
        self.scanpos = 0  # 下次从该偏移继续找 magic（增量推进，避免 O(n²)）

    def feed(self, data: bytes):
        self.buf += data

    def _compact(self):
        if self.skip:
            if self.scanpos >= self.skip:
                self.scanpos -= self.skip
            else:
                self.scanpos = 0
            del self.buf[:self.skip]
            self.skip = 0

    def pop(self):
        """取出下一个完整有效帧 (cmd, payload)，无则返回 None（垃圾已被消费）"""
        buf = self.buf
        magic = bytes((MAGIC0, MAGIC1))
        while True:
            i = buf.find(magic, self.scanpos)
            if i < 0:
                # 保留尾部可能的半个 magic
                self.scanpos = 0
                if buf and buf[-1] == MAGIC0:
                    self.skip = len(buf) - 1
                else:
                    self.skip = len(buf)
                self._compact()
                return None
            if len(buf) - i < HEADER_LEN:
                self.skip = i
                self.scanpos = 0
                self._compact()  # 压缩它前面的垃圾，半帧等待后续数据
                return None
            ver = buf[i + 2]
            ln = buf[i + 4] | (buf[i + 5] << 8)
            if ver != VERSION or ln > MAX_READ_PAYLOAD:
                self.scanpos = i + 1  # 等价旧逻辑 del buf[0]（逐字节重同步）
                continue
            total = HEADER_LEN + ln
            if len(buf) - i < total:
                self.skip = i
                self.scanpos = 0
                self._compact()
                return None
            crc = int.from_bytes(buf[i + 6:i + 10], "little")
            payload = bytes(buf[i + HEADER_LEN:i + total])
            if zlib.crc32(payload) & 0xFFFFFFFF != crc:
                self.scanpos = i + 2  # 等价旧逻辑 del buf[0:2]
                continue
            cmd = buf[i + 3]
            self.scanpos = 0
            self.skip = 0
            del buf[:i + total]
            return (cmd, payload)


# ---------------- 设备客户端 ----------------
class DeviceClient:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 0.15):
        self.port = port
        self.baud = baud
        self.timeout = timeout
        self.ser = None

    def open(self):
        # 打开时可能触发自动复位电路, 因此 open 后用复位 + 启动等待确保固件就绪
        self.ser = serial.Serial(self.port, self.baud, timeout=self.timeout,
                                 write_timeout=2.0)
        time.sleep(0.2)
        try:
            self.ser.reset_input_buffer()
        except Exception:
            pass
        self._hard_reset()
        # 复位脉冲结束时 DTR 停在 False。本固件的 TinyUSB CDC 只在 DTR 置位后
        # 才接收主机数据, 因此这里必须显式恢复 DTR=True, 否则后续 PING/READ/WRITE
        # 全部超时 (实测: DTR=False 时 PING 无响应; DTR=True 时正常)。
        #
        # 关于 RTS: 对照实验显示 RTS=True / False 都能正常通信, 它不是必要条件。
        # 这里一并置位只是为了与已验证可用的历史脚本 e2e_device.py 保持相同的
        # 空闲状态, 避免再引入一个未验证的差异; 不表示两者都必须为 True。
        #
        # 时序: setDTR 写入的是 pyserial 的 _dtr_state, 只有在端口打开时的
        # _reconfigure_port 才会把它写进 DCB; 端口已打开时 setDTR 会立即调用
        # EscapeCommFunction 生效。此处端口已打开, 所以该调用立即生效, 且发生在
        # _wait_boot_ready() 与第一次 PING 之前。
        self._assert_control_lines()
        self._wait_boot_ready()
        return self

    def _assert_control_lines(self):
        """把控制线置于已验证可通信的状态 (DTR=True, RTS=True)。

        只在 open() 中、第一次发送任何协议帧之前调用一次。失败时直接抛出,
        不静默吞掉: 控制线没建立就继续发送, 只会得到一堆超时错误。
        """
        self.ser.setDTR(True)
        self.ser.setRTS(True)
        time.sleep(0.1)          # 给 CDC 端点一点时间完成 line-state 同步

    def _hard_reset(self):
        """RTS 复位脉冲（等效按 RESET）：清空 FTDI 侧回放积压、重启固件。

        背景 (upgrade10k_replay_flood_evidence.log / diag)：仅打开串口不一定复位；
        而长时间静置后 FTDI 侧可能堆积数 MB 回放数据（实测 120KB/s 持续数十秒）。
        复位后真正的启动发生在脉冲后 ~0.4s，窗口清晰可辨，随后的
        _wait_boot_ready 能可靠对齐。复位时 NVS/文件系统/应用分区不受影响。
        """
        try:
            self.ser.setDTR(False)
            self.ser.setRTS(True)
            time.sleep(0.15)
            self.ser.setRTS(False)
            time.sleep(0.1)
            self.ser.reset_input_buffer()
        except Exception:
            pass

    def _wait_boot_ready(self, max_wait: float = 20.0):
        """open 后等待固件第二次启动完成（`[boot] ready ... max_text=…`）。

        背景 (见 README 已知问题与 upgrade10k_replay_flood_evidence.log)：
        open/reboot 后的复位窗口里 FTDI 驱动会以满速回放数万字节旧日志
        (`ready=1)` / `ESP-ROM` 重复，常见于连续复位造成的积压）。
        该阶段未发送任何请求，读到的都是启动日志与回放垃圾，直接按行丢弃；
        等到启动完成或超时后返回，随后 PING 重试继续兜底。
        回放行、启动行、panic/错误行一律保留在 serial 侧的可观察性（抓包工具可见），
        此处只决定“何时可以开始发请求”，不隐藏协议层报错。
        """
        tail = b""
        quiet_since = None
        t_end = time.time() + max_wait
        while time.time() < t_end:
            try:
                d = self.ser.read(4096)
            except Exception:
                return False
            now = time.time()
            if d:
                tail = (tail + d)[-64:]
                if b"[boot] ready" in tail:
                    return True
                quiet_since = None
            else:
                # read 超时才返回空：连续 quiet 秒无数据 = 缓冲已排空/线路安静，
                # 此时可直接开始发请求，不必等满 20s
                if quiet_since is None:
                    quiet_since = now
                elif now - quiet_since >= 1.0:
                    return True
        return False   # 超时：在无日志的安静板上也继续（PING 重试兜底）

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def __enter__(self):
        return self.open()

    def __exit__(self, *a):
        self.close()

    def _deadline_for(self, cmd: int, req_payload_len: int) -> float:
        """按帧长计算单次操作期限 (见模块头部超时机制说明)"""
        resp = RESP_FRAME_LEN.get(cmd, HEADER_LEN + 14)
        wire = (HEADER_LEN + req_payload_len + resp) / SERIAL_BYTES_PER_SEC
        return DEADLINE_FIXED + DEADLINE_WIRE_FACTOR * wire

    def _request(self, cmd: int, payload: bytes) -> bytes:
        """发送请求并等待匹配响应帧; 处理半包/垃圾数据/设备复位。

        等待窗口 = _deadline_for() 按实际请求/响应帧长计算的期限 (10 KiB WRITE/READ
        ≈2.8s, PING ≈1.0s), 不使用固定短窗, 也不会把期限放大到数十秒;
        只有整个期限内完全没收到响应帧才重试 (缓冲跨重试累积)。
        """
        if not self.ser:
            raise DeviceError("串口未打开")
        expect = cmd | 0x80
        retries = REQUEST_RETRIES.get(cmd, 3)
        each = self._deadline_for(cmd, len(payload))
        scanner = FrameScanner()
        for attempt in range(retries):
            try:
                self.ser.write(build_frame(cmd, payload))
                self.ser.flush()
            except Exception as e:
                raise DeviceError(f"串口写入失败: {e}")
            deadline = time.time() + each
            while time.time() < deadline:
                chunk = self.ser.read(4096)
                if chunk:
                    scanner.feed(chunk)
                    while True:
                        fr = scanner.pop()
                        if fr is None:
                            break
                        rcmd, rpayload = fr
                        if rcmd == expect:
                            return rpayload
                        # 其它响应帧 (如上一次的残留), 继续等
            # 期限耗尽才重试: WRITE 重发因设备端内容去重不会重复写介质/递增 seq
            time.sleep(0.3)
        raise DeviceError(
            f"等待设备响应超时 (单次期限 {each:.2f}s × {retries} 次; "
            f"端口/波特率/固件是否正确?)\n"
            f"排查提示：复位窗口的 FTDI 回放日志属驱动层已知行为（见 README），"
            f"设备侧日志可用 tools/capture_serial.py 单独观察；"
            f"本报错只证明协议层在期限内没收到匹配响应帧。")

    def ping(self) -> dict:
        pl = self._request(CMD_PING, b"")
        # PING 响应为固定 16 字节 payload (协议 v1, 见 protocol.md):
        # status(1) proto_ver(1) fw_ver(2) max_text(2) has_text(1)
        # saved_len(2) saved_crc(4) seq低16位(2) hid_ready(1, 偏移15)
        if len(pl) != PING_PAYLOAD_LEN:
            raise DeviceError(
                f"PING 响应长度异常: 收到 {len(pl)} 字节, 预期 {PING_PAYLOAD_LEN} 字节")
        if pl[0] != ST_OK:
            raise DeviceError(f"PING 失败: {STATUS_TEXT.get(pl[0], pl[0])}")
        if pl[1] != VERSION:
            raise DeviceError(f"PING 协议版本不符: 设备 v{pl[1]}, 本程序 v{VERSION}")
        has_text = pl[6]
        if has_text not in (0, 1):
            raise DeviceError(f"PING has_text 字段非法: {has_text}")
        hid_ready_raw = pl[15]
        if hid_ready_raw not in (0, 1):
            raise DeviceError(f"PING hid_ready 字段非法: {hid_ready_raw}")
        info = {
            "proto": pl[1],
            "fw": f"{pl[3]}.{pl[2]}",          # u16 LE: 低字节=主版本
            "fw_ver": pl[2] | (pl[3] << 8),
            "max_text": pl[4] | (pl[5] << 8),
            "has_text": bool(has_text),
            "saved_len": pl[7] | (pl[8] << 8),
            "saved_crc": int.from_bytes(pl[9:13], "little"),
            "seq_lo": pl[13] | (pl[14] << 8),  # 仅低 16 位 (完整 seq 见 READ)
            "hid_ready": bool(hid_ready_raw),
            "payload_len": len(pl),
        }
        if info["saved_len"] > info["max_text"]:
            raise DeviceError(
                f"PING 字段不自洽: saved_len={info['saved_len']} > max_text={info['max_text']}")
        return info

    def write_text(self, text: str) -> dict:
        """校验 + 写入。返回设备确认信息; 失败抛 DeviceError"""
        err = validate_text(text)
        if err:
            raise DeviceError(f"内容校验失败: {err}")
        data = text.encode("ascii")
        pl = self._request(CMD_WRITE, data)
        if len(pl) < 14:
            raise DeviceError(f"WRITE 响应长度异常: {len(pl)}")
        status = pl[0]
        detail = pl[1] | (pl[2] << 8)
        info = {
            "status": status,
            "status_text": STATUS_TEXT.get(status, str(status)),
            "saved_len": pl[3] | (pl[4] << 8),
            "saved_crc": int.from_bytes(pl[5:9], "little"),
            "seq": int.from_bytes(pl[9:13], "little"),
            "written": bool(pl[13]),
            "detail": detail,
        }
        if status == ST_ERR_CHARSET:
            bad = text[detail] if detail < len(text) else "?"
            raise DeviceError(f"设备拒绝: 含不支持字符 {bad!r} (位置 {detail})")
        if status != ST_OK:
            raise DeviceError(f"设备返回错误: {info['status_text']}")
        return info

    def read_text(self) -> dict:
        pl = self._request(CMD_READ, b"")
        if len(pl) < 13:
            raise DeviceError(f"READ 响应长度异常: {len(pl)}")
        status = pl[0]
        if status != ST_OK:
            raise DeviceError(f"READ 失败: {STATUS_TEXT.get(status, status)}")
        saved_len = pl[3] | (pl[4] << 8)
        if saved_len > MAX_TEXT_LEN:
            raise DeviceError(
                f"READ saved_len={saved_len} 超出上限 {MAX_TEXT_LEN} (响应异常/固件不一致?)")
        saved_crc = int.from_bytes(pl[5:9], "little")
        seq = int.from_bytes(pl[9:13], "little")
        text_bytes = pl[13:13 + saved_len]
        if len(text_bytes) != saved_len:
            raise DeviceError("READ 响应文本不完整 (半包)")
        crc_ok = (zlib.crc32(text_bytes) & 0xFFFFFFFF) == saved_crc
        return {
            "text": text_bytes.decode("ascii", errors="replace"),
            "len": saved_len,
            "crc": saved_crc,
            "seq": seq,
            "crc_ok": crc_ok,
        }


def validate_text(text: str):
    """返回错误说明, None 表示通过 (与固件 proto_char_supported 一致)"""
    if not isinstance(text, str):
        return "文本必须是字符串"
    bad = []
    for i, ch in enumerate(text):
        o = ord(ch)
        if (0x20 <= o <= 0x7E) or ch in ("\n", "\t"):
            continue
        if ch == "\r":
            bad.append((i, "CR(\\r)"))
        elif o > 127:
            bad.append((i, f"非 ASCII {ch!r}"))
        else:
            bad.append((i, f"不支持的控制字符 0x{o:02X}"))
    if bad:
        samples = ", ".join(f"{desc}@{i}" for i, desc in bad[:5])
        more = "" if len(bad) <= 5 else f" 等 {len(bad)} 处"
        return f"含不受支持的字符: {samples}{more}。{CHARSET_NOTE}"
    n = len(text.encode("ascii"))
    if n > MAX_TEXT_LEN:
        return f"文本 {n} 字节超过上限 {MAX_TEXT_LEN} 字节"
    if n == 0:
        return "文本为空"
    return None


def list_serial_ports():
    ports = list(list_ports.comports())
    ports.sort(key=lambda p: 0 if p.device.upper() == "COM4" else 1)
    return ports


# ---------------- CLI ----------------
def run_cli(args) -> int:
    if not args.port:
        pl = list_serial_ports()
        if not pl:
            print("未发现串口", file=sys.stderr)
            return 2
        args.port = pl[0].device
    try:
        with DeviceClient(args.port) as dev:
            if args.ping:
                info = dev.ping()
                print(f"[PING] {args.port} 固件协议v{info['proto']} fw=0x{info['fw_ver']:04X} "
                      f"max={info['max_text']}B has_text={info['has_text']} "
                      f"saved_len={info['saved_len']} crc=0x{info['saved_crc']:08X} "
                      f"hid_ready={info.get('hid_ready')} payload_len={info.get('payload_len')}")
            if args.write is not None:
                info = dev.write_text(args.write)
                print(f"[WRITE] {info['status_text']} saved_len={info['saved_len']} "
                      f"crc=0x{info['saved_crc']:08X} seq={info['seq']} "
                      f"changed={info['written']}")
            if args.read:
                r = dev.read_text()
                print(f"[READ] len={r['len']} crc=0x{r['crc']:08X} seq={r['seq']} "
                      f"crc_ok={r['crc_ok']}")
                print("--- text begin ---")
                print(r["text"])
                print("--- text end ---")
    except DeviceError as e:
        print(f"失败: {e}", file=sys.stderr)
        return 1
    except serial.SerialException as e:
        print(f"串口错误: {e}", file=sys.stderr)
        return 1
    return 0


# ---------------- GUI ----------------
def run_gui(args, self_test=False):
    import tkinter as tk
    from tkinter import ttk, messagebox

    root = tk.Tk()
    root.title("ESP32 HID 文本写入器 v1.1 (10 KiB, 仅写入, 不触发输入)")
    root.minsize(640, 480)

    main = ttk.Frame(root, padding=10)
    main.pack(fill=tk.BOTH, expand=True)

    # 串口行
    row1 = ttk.Frame(main)
    row1.pack(fill=tk.X, pady=(0, 6))
    ttk.Label(row1, text="串口:").pack(side=tk.LEFT)
    port_var = tk.StringVar()
    port_cb = ttk.Combobox(row1, textvariable=port_var, width=18, state="readonly")
    port_cb.pack(side=tk.LEFT, padx=6)

    def refresh_ports():
        ports = list_serial_ports()
        vals = []
        for p in ports:
            desc = f"{p.device}  {p.description}"
            vals.append(desc)
        port_cb["values"] = vals
        if vals:
            # 优先 COM4, 否则第一个
            for i, p in enumerate(ports):
                if p.device.upper() == "COM4":
                    port_cb.current(i)
                    return
            port_cb.current(0)

    def selected_port():
        v = port_var.get()
        return v.split()[0] if v else None

    ttk.Button(row1, text="刷新串口", command=refresh_ports).pack(side=tk.LEFT, padx=4)
    info_lbl = ttk.Label(row1, text="")
    info_lbl.pack(side=tk.RIGHT)

    # 文本编辑区
    ttk.Label(main, text="要写入设备的文本 (写入前会自动校验字符与长度):").pack(anchor=tk.W)
    text_frame = ttk.Frame(main)
    text_frame.pack(fill=tk.BOTH, expand=True, pady=4)
    txt = tk.Text(text_frame, height=12, wrap=tk.WORD, undo=True)
    sb = ttk.Scrollbar(text_frame, command=txt.yview)
    txt.configure(yscrollcommand=sb.set)
    sb.pack(side=tk.RIGHT, fill=tk.Y)
    txt.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)

    # 按钮行
    row2 = ttk.Frame(main)
    row2.pack(fill=tk.X, pady=6)
    status_var = tk.StringVar(value="就绪。仅负责写入文本; HID 输入由设备按键独立完成。")
    write_btn = ttk.Button(row2, text="写入设备")
    read_btn = ttk.Button(row2, text="读取设备")
    ping_btn = ttk.Button(row2, text="查询设备")
    write_btn.pack(side=tk.LEFT, padx=(0, 6))
    read_btn.pack(side=tk.LEFT, padx=6)
    ping_btn.pack(side=tk.LEFT, padx=6)

    # 状态区
    status_lbl = ttk.Label(main, textvariable=status_var, foreground="#005500",
                           wraplength=620, justify=tk.LEFT)
    status_lbl.pack(anchor=tk.W, fill=tk.X)
    ttk.Label(main, text=CHARSET_NOTE + f" 最大长度 {MAX_TEXT_LEN} 字节。",
              foreground="#666666", wraplength=620, justify=tk.LEFT).pack(anchor=tk.W, pady=(4, 0))
    len_var = tk.StringVar(value="当前文本: 0 字节")
    ttk.Label(main, textvariable=len_var, foreground="#666666").pack(anchor=tk.W)

    def update_len(*_):
        s = txt.get("1.0", "end-1c")
        n = len(s.encode("ascii", errors="replace"))
        note = "" if s.isascii() else "  (含非 ASCII 字符, 写入将被拒绝)"
        len_var.set(f"当前文本: {n} 字节 / 上限 {MAX_TEXT_LEN}{note}")
    txt.bind("<<Modified>>", lambda e: (update_len(), txt.edit_modified(False)))

    def set_status(msg, ok=True):
        status_var.set(msg)
        status_lbl.configure(foreground="#005500" if ok else "#aa0000")

    def do_write():
        """写入主流程 (GUI 按钮与自测共用)"""
        port = selected_port()
        if not port:
            set_status("请先选择串口", ok=False)
            return False
        text = txt.get("1.0", "end-1c")
        err = validate_text(text)
        if err:
            set_status(f"校验失败, 未发送: {err}", ok=False)
            return False
        n = len(text.encode("ascii"))
        try:
            with DeviceClient(port) as dev:
                info = dev.write_text(text)
        except DeviceError as e:
            set_status(f"写入失败: {e}", ok=False)
            return False
        except Exception as e:
            set_status(f"串口异常: {e}", ok=False)
            return False
        changed = "已写入新内容" if info["written"] else "内容与设备一致, 未重复写入"
        set_status(f"写入成功 ({info['status_text']}) — 已保存 {info['saved_len']} 字节, "
                   f"CRC32=0x{info['saved_crc']:08X}, 版本号 seq={info['seq']}, {changed}。"
                   f"本程序可安全关闭, 文本已存入设备存储 (SPIFFS 双槽位文件)。")
        return True

    def do_read():
        port = selected_port()
        if not port:
            set_status("请先选择串口", ok=False)
            return
        try:
            with DeviceClient(port) as dev:
                r = dev.read_text()
        except Exception as e:
            set_status(f"读取失败: {e}", ok=False)
            return
        set_status(f"读取成功: {r['len']} 字节, CRC32=0x{r['crc']:08X} "
                   f"(本地重算 {'一致' if r['crc_ok'] else '不一致!'}), seq={r['seq']}")
        if r["crc_ok"]:
            txt.delete("1.0", tk.END)
            txt.insert("1.0", r["text"])
            update_len()

    def do_ping():
        port = selected_port()
        if not port:
            set_status("请先选择串口", ok=False)
            return
        try:
            with DeviceClient(port) as dev:
                info = dev.ping()
        except Exception as e:
            set_status(f"查询失败: {e}", ok=False)
            return
        set_status(f"设备在线: 协议v{info['proto']}, 固件0x{info['fw_ver']:04X}, "
                   f"最大{info['max_text']}B, 已存文本={info['has_text']} "
                   f"({info['saved_len']}字节, CRC=0x{info['saved_crc']:08X}), "
                   f"设备侧HID挂载={info.get('hid_ready')}")

    write_btn.configure(command=do_write)
    read_btn.configure(command=do_read)
    ping_btn.configure(command=do_ping)

    refresh_ports()
    if args.port:
        for i, v in enumerate(port_cb["values"]):
            if v.startswith(args.port.upper()):
                port_cb.current(i)
                break
    update_len()

    # GUI 自测: 程序化调用与按钮完全相同的处理函数, 记录证据 (不做任何键盘模拟)
    self_test_log = {}
    if self_test:
        if args.write is not None:
            txt.delete("1.0", tk.END)
            txt.insert("1.0", args.write)
            update_len()
        def step1():
            do_ping()
            root.after(600, step2)
        def step2():
            ok = do_write()
            self_test_log["write_ok"] = ok
            root.after(600, step3)
        def step3():
            do_read()
            self_test_log["status"] = status_var.get()
            import os
            logdir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "..", "test_logs")
            os.makedirs(logdir, exist_ok=True)
            with open(os.path.join(logdir, "gui_self_test.log"), "a",
                      encoding="utf-8") as f:
                f.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] "
                        f"write_ok={self_test_log.get('write_ok')} "
                        f"status={self_test_log.get('status')}\n")
            root.after(400, root.destroy)   # 自测完成后自动关闭, 不驻留
        root.after(800, step1)

    root.mainloop()
    return 0


def main():
    ap = argparse.ArgumentParser(description="ESP32 HID 文本写入器 (桌面程序)")
    ap.add_argument("--port", help="串口, 如 COM4 (默认自动选择, 优先 COM4)")
    ap.add_argument("--write", help="写入指定文本到设备")
    ap.add_argument("--read", action="store_true", help="读取设备已保存文本")
    ap.add_argument("--ping", action="store_true", help="查询设备状态")
    ap.add_argument("--gui", action="store_true", help="强制打开图形界面")
    ap.add_argument("--self-test", action="store_true",
                    help="打开图形界面并自动调用查询/写入/读取按钮逻辑, 记录到 test_logs/")
    args = ap.parse_args()

    if args.gui or args.self_test:
        return run_gui(args, self_test=args.self_test)
    if not (args.write is not None or args.read or args.ping):
        return run_gui(args)
    return run_cli(args)


if __name__ == "__main__":
    sys.exit(main())
