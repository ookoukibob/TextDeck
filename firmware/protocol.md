# 串口配置/控制协议 v1 (PC <-> ESP32-S3)

所有多字节字段均为**小端序 (Little Endian)**。

- 协议版本 (`PROTO_VERSION`) 仍为 **0x01**：v1.2 固件只新增命令，不改变已有命令的线上格式。
- 固件版本 (`FW_VERSION`) 为 **1.2.0 (0x0102)**。
- 日常单线连接：原生 USB OTG 上的 **CDC 控制端口 + HID 键盘复合设备**。
  该 CDC 端口只传输二进制协议帧，不输出调试日志。
- 备用/兼容：UART0 / FTDI 串口仍可收发同一协议（日志也走该串口），用于刷写、
  调试和旧桌面工具。

## 帧格式（双向一致）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 1 | magic0 | `0xE7` |
| 1 | 1 | magic1 | `0x53` |
| 2 | 1 | 版本 | `0x01` |
| 3 | 1 | cmd | 命令; 响应 = 请求 cmd \| `0x80` |
| 4 | 2 | payload_len | u16, 最大 `10253` (13 + 10240) |
| 6 | 4 | crc32 | payload 的 CRC32 (IEEE 802.3, 多项式 0xEDB88320, 与 Python `zlib.crc32` 一致) |
| 10 | n | payload | 命令数据 |

接收方必须能从任意垃圾字节（如固件启动日志）中重新同步到 magic，并在
版本/长度/CRC 校验失败时丢弃重扫，不得把垃圾当响应。

### 帧长上限（容量 10 KiB 后）

| 项 | 计算 | 字节 |
|---|---|---|
| 单条文本上限 `MAX_TEXT_LEN` | — | **10,240** |
| READ 响应 payload 上限 | 13 + 10240 | **10,253** |
| 最大完整帧 | 10 (header) + 10,253 | **10,263** |
| WRITE 请求 payload 上限 | — | 10,240 |
| WRITE 响应 payload | 固定 | 14 |
| READ 响应 payload | 13 + saved_len | 13 … 10,253 |
| PING 响应 payload | 固定 | 16 |
| START/CANCEL/GET_STATUS 响应 payload | 固定 | **16** |

- `payload_len` 为 u16 小端，10,253 远小于 65,535，无需改字段宽度。
- 设备端接收缓冲 `ConfigLink::_payload` 与响应缓冲 `s_out` 均按
  `PROTO_MAX_PAYLOAD` = 10,253 静态分配（CDC 与 UART 各一个解析器，约 20.5 KiB，
  **不在栈上**）。
- 两端都必须按上表校验：桌面端拒绝 `payload_len > 10253` 的帧头，
  设备端收到 `payload_len > PROTO_MAX_PAYLOAD` 时静默重新同步。

## 命令

### 0x01 CMD_WRITE_TEXT — 写入文本（v1 行为不变）
- 请求 payload = 文本字节 (ASCII)
- 响应 payload (14 字节):

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 1 | status (0=OK) |
| 1 | 2 | detail (charset 错误时 = 首个不支持字符下标) |
| 3 | 2 | saved_len (设备当前保存长度) |
| 5 | 4 | saved_crc |
| 9 | 4 | seq (版本号, 每次实际写入 +1) |
| 13 | 1 | written (1=本次新写入, 0=内容相同未写介质) |

- 长度校验**先于**任何存储动作：`len == 0` 或 `len > 10240` 一律返回
  status=2 (ST_ERR_LENGTH)，且**不写槽位、不递增 seq、不破坏已存文本**。
- 响应中的 `saved_len/saved_crc/seq` 始终取自设备**当前保存状态**，
  因此被拒绝时这三个字段就是原有文本的值（可作为“未改动”的证据）。
- **v1.2 新增**：如果当前有输入任务（PREPARING/TYPING），WRITE 返回
  status=6 (ST_ERR_BUSY)，不修改文本；否则状态机可能读到被替换的缓冲区。

### 0x02 CMD_READ_TEXT — 读取文本（v1 行为不变）
- 请求 payload = 空
- 响应 payload (13 + saved_len 字节):
  status(1), detail(2), saved_len(2), saved_crc(4), seq(4), text(saved_len)
- 无已存文本时 status=OK、saved_len=0、text 为空

### 0x03 CMD_PING — 查询设备（v1 行为/长度不变）
- 请求 payload = 空
- 响应 payload (**16 字节**, 固定长度):

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 1 | status (0=OK) |
| 1 | 1 | proto_ver (协议版本, 当前 0x01) |
| 2 | 2 | fw_ver u16 (当前 0x0102 = 1.2.0) |
| 4 | 2 | max_text u16 (单条文本上限) |
| 6 | 1 | has_text (0/1) |
| 7 | 2 | saved_len u16 |
| 9 | 4 | saved_crc u32 |
| 13 | 2 | seq **低 16 位** u16 |
| 15 | 1 | hid_ready (0/1, 设备侧 USB HID 是否已挂载) |

- **seq 位宽区分**: WRITE/READ 响应返回完整 **32 位** seq；PING 因固定 16 字节
  payload 仅返回其**低 16 位** (`seq & 0xFFFF`)，需要完整值时用 READ。
- 桌面端要求响应 payload **恰好 16 字节**，并对 proto_ver / has_text / hid_ready
  及 saved_len≤max_text 做合法性校验，长度或字段异常时明确报错。
- Windows 端用 `fw_ver >= 0x0102` 判断是否支持新增远程输入命令；旧固件对
  0x04/0x05/0x06 返回 ST_ERR_CMD，Windows 必须提示升级固件，**不得**回退到
  SendInput 或内核驱动。

### 0x04 CMD_START_TYPING — 开始输入（v1.2 新增）
- 请求 payload = 空（使用设备中已保存的文本）
- 响应 payload 固定 **16 字节**，布局见“输入状态响应”。
- 行为:
  1. 无有效文本 → status=8 (ST_ERR_NO_TEXT)，不启动。
  2. HID 未枚举/不可用 → status=9 (ST_ERR_HID_NOT_READY)。
  3. 已有任务在 PREPARING/TYPING → status=6 (ST_ERR_BUSY)，**不重头输入**。
  4. 成功 → status=0，state=PREPARING；`beginTypeText()` 立即返回，真正的
     字符输入由 `loop()` 中的非阻塞状态机执行。**响应绝不等待整段文本输入完**。
- 成功响应中的 `total` = 设备保存的文本长度，`session_id` 每次接受 START +1。
- START 成功即启用 **主机租约**：Windows 必须每 250–500ms 发送任意有效协议帧
  （通常 GET_STATUS）续租；连续 3000ms 无有效帧，设备自动中止并释放按键。

### 0x05 CMD_CANCEL_TYPING — 取消输入（v1.2 新增）
- 请求 payload = 空
- 响应 payload 固定 **16 字节**。
- 行为:
  - 立即停止推进状态机，**在发送响应之前同步执行 `releaseAll()`**（全零 HID 报告）。
    因此收到 status=0 的响应即可确认“停止已执行并释放按键”，而不只是“命令已排队”。
  - 有活动任务 → state=CANCELLED，`cancel_reason=1` (CANCEL_HOST)，
    `processed` 保留停止时的进度；停止后绝不续打剩余文本。
  - 没有活动任务 → status=0，原状态保持不变（IDLE/COMPLETED/CANCELLED/FAILED），
    同样执行一次 `releaseAll()` 作为安全清理。可重复调用。
  - 若旧任务已结束，迟到的 CANCEL 不会重新激活它；新的 START 才创建新任务。

### 0x06 CMD_GET_STATUS — 查询状态/心跳（v1.2 新增）
- 请求 payload = 空
- 响应 payload 固定 **16 字节**，status 恒为 0。
- 每次有效请求都刷新主机租约（PING/READ/WRITE 同样刷新）。
- 输入期间建议 250–500ms 查询一次；查询绝不阻塞 HID 状态机。

### 输入状态响应（0x04 / 0x05 / 0x06 响应 payload，固定 16 字节）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 1 | status | 请求处理状态码 (ST_*) |
| 1 | 1 | state | 0=IDLE, 1=PREPARING, 2=TYPING, 3=COMPLETED, 4=CANCELLED, 5=FAILED |
| 2 | 2 | processed | 已处理字符数 u16 |
| 4 | 2 | total | 本任务总字符数 u16 |
| 6 | 1 | last_error | 最近错误码 (TY_ERR_*) |
| 7 | 1 | cancel_reason | 0=NONE, 1=HOST, 2=LOCAL, 3=LEASE, 4=USB_LOST |
| 8 | 1 | flags | bit0=主机发起, bit1=已发送全释放, bit2=HID ready |
| 9 | 1 | reserved | 0 |
| 10 | 2 | lease_remaining_ms | 主机租约剩余毫秒 (非主机任务=0) |
| 12 | 2 | session_id | 任务会话号 (每次接受 START +1) |
| 14 | 2 | capabilities | bit0=远程输入, bit1=主机租约, bit2=原生 USB CDC |

`last_error`:

| 值 | 含义 |
|---|---|
| 0 | 无错误 |
| 1 | 无有效文本 |
| 2 | HID 未就绪 |
| 3 | 忙碌 |
| 4 | 不支持字符 |
| 5 | HID press() 拒绝 |
| 6 | USB HID 断连 |
| 7 | 主机租约超时 |
| 8 | 被取消 |

## 状态码（ST_*）

| 值 | 含义 |
|---|---|
| 0 | OK |
| 1 | CRC 校验失败 |
| 2 | 长度非法/超出上限 |
| 3 | 含不支持字符 (detail = 首个坏字符下标) |
| 4 | 设备存储失败 |
| 5 | 未知命令 |
| 6 | 设备忙碌 (输入中) |
| 7 | 帧格式错误 |
| 8 | 无有效文本 (v1.2) |
| 9 | HID 未就绪 (v1.2) |

## 文本约束（两端一致校验）

- 长度: 1..**10,240** 字节（10 KiB）
- 字符: ASCII `0x20-0x7E` + `\n` (0x0A) + `\t` (0x09)，与固件使用的
  `KeyboardLayout_en_US` HID 映射严格一致
- 拒绝: 中文/Emoji/任意非 ASCII、CR (`\r`)、其它控制字符
- 设备端保存前再次独立校验；不支持的字符返回 status=3，**不静默替换**
- Windows 多行编辑框的 CRLF 由主机规范化为单个 LF，保留换行数量；
  不支持的字符必须在发送 START 前整体拒绝，不能部分开始输入。

## 非阻塞输入与停止保障

- 输入节奏与 v1.1 硬件验证值相同：首字符前 80ms、按住 8ms、字符间隔 15ms
  （每字符总计约 23ms，另加 80ms 起始等待）。
- `HidTyper::poll()` 在 `loop()` 中持续运行；START/CANCEL/GET_STATUS 不会
  被整段文本输入阻塞。
- 每个字符前后都检查 USB HID 是否持续就绪；连续 10ms 不就绪即判定断连，
  立即 `releaseAll()`、状态置 FAILED、`last_error=6`，重连后**不会自动续打**。
- 主机发起的任务有 3000ms 租约；主机进程崩溃/控制端口失联时设备自动中止，
  不依赖主机成功发送 CANCEL。
- 本地 BOOT 触发的任务 `host_initiated=0`，不受主机租约影响；与远程任务共享
  同一个互斥/状态机，同一时刻只能有一个任务。

## 持久化

### 新格式（主存储）: SPIFFS 双槽位文件

- 分区: 分区表 `default.csv` 的 `spiffs`（subtype 0x82），偏移 `0x290000`，
  大小 **1,441,792 字节**。**不修改分区表**，直接复用这块旧固件从未访问的分区。
- 文件: `/textA.bin` 与 `/textB.bin`，各含 20 字节文件头 + 正文。

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | magic | `"HIDF"` (`0x46444948` 小端) |
| 4 | 2 | ver | 格式版本，当前 `1` |
| 6 | 2 | rsvd | 保留，写 0 |
| 8 | 4 | seq | 版本号，每次**实际**写入 +1 |
| 12 | 4 | len | 正文字节数，1..10240 |
| 16 | 4 | crc | 正文 CRC32 |
| 20 | len | data | 正文 |

- 写入流程（掉电安全）：写**另一**槽位 → 关闭文件 → 重新打开回读，
  逐项验证 magic/ver/文件大小 == 20+len/正文 CRC32/正文逐字节 → 全部通过
  才把它认定为最新有效版本。
- 上电选择 seq 最大且**校验通过**的槽位；另一槽损坏/半截时自动回退。
- 内容与当前完全一致时直接返回 `written=0`，**不写介质、不递增 seq**。
- 挂载失败时**绝不无条件格式化**：先确认 spiffs 分区存在、再原始扫描分区，
  只有“确认不存在任何本项目数据”才允许对该分区做一次性初始化。

### 旧格式（只读兼容 + 迁移源）: NVS

- Preferences 命名空间 `hidtext`，键 `slotA` / `slotB`
- 槽位结构（**与 1.0 逐字节一致，不得修改**）:
  `magic("HIDT") + seq u32 + len u16 + rsvd u16 + crc32 u32 + data[1024]` = **1040 字节**
  （固件内有 `static_assert(sizeof(LegacyNvsSlot) == 1040)` 守护）
- 本固件对该命名空间**只读**：从不写入、从不删除，因此旧文本永远可恢复。
- 迁移规则：
  1. 启动优先恢复有效的新格式文件；有有效新文件时**不会**被旧 NVS 覆盖。
  2. 新格式无效/不存在时，才读取旧 NVS 中 seq 最大且 CRC 有效的槽位。
  3. 迁移 = 按新格式写入 + 回读验证；**验证通过才算迁移成功**。
  4. 迁移失败时保留旧 NVS 作为当前有效文本（`usingLegacy`），并打印
     `MIGRATION FAILED` 明确报告；此时设备仍可正常按键输入旧文本，
     但写入新文本会失败（不静默降级成假成功）。
  5. 迁移成功前后都不删除/覆写 NVS。
