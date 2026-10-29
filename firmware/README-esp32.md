# TextDeck ESP32-S3 Firmware

ESP32-S3 **独立运行的 USB HID 文本输入器**：文本持久化在 ESP32 内部
（**SPIFFS 双槽位文件**，单条上限 **10 KiB / 10,240 字节**），设备本身就是一个
标准 USB HID 键盘。

**v1.2 起为单线方案**：一根原生 USB 线同时提供
**CDC 控制端口**（二进制协议）和 **HID 键盘**（复合设备）。Windows 托盘程序
通过该 CDC 端口下发文本并远程开始/取消输入，用户按自己键盘上的
全局快捷键即可触发——不需要内核驱动、不需要测试签名、不需要管理员权限。

板上的 **BOOT 键**仍可本地触发一次输入（可选兼容功能），但**不再是**日常使用的前提。

**明确不做的事**：不用 Windows 剪贴板、不用 Ctrl+V 等快捷键、无 AutoHotkey /
PowerShell 模拟 / 后台服务 / 网页配置页 / Wi-Fi 依赖。

## 1. 总体架构

```
┌──────────────────────────────┐                    ┌─────────────────────────────┐
│ Windows 托盘程序             │  原生 USB (单线)   │        ESP32-S3 固件        │
│ TextDeck                     │                    │ firmware/esp32_hid_text_device/ │
│  - 编辑/保存文本             │  CDC 控制端口  ───►│  - config_link: 协议解析    │
│  - 全局快捷键                │  (二进制协议帧)    │  - text_store: SPIFFS 双槽位│
│  - 开始/取消/状态查询        │                    │  - hid_typer: 非阻塞状态机  │
│  - 目标窗口焦点保护          │  HID 键盘      ◄───│  - trigger_button: BOOT键   │
└──────────────────────────────┘  (真实按键输出)    └──────────────┬──────────────┘
                                                                   │
                                            目标应用直接收到标准 USB HID 键盘输入
```

同一根原生 USB 线上同时存在两个接口：

| 接口 | 枚举结果（Windows） | 用途 |
|---|---|---|
| HID Keyboard | `HID\VID_303A&PID_1001&MI_00` | 真实按键输出（系统自带 HID 驱动） |
| USB CDC | `USB\VID_303A&PID_1001&MI_01` → `COMx` | 文本同步 / START / CANCEL / STATUS（系统自带 usbser 驱动） |

FTDI USB-UART（`VID_0403/PID_6001`）仍然保留，但只用于**刷写固件、调试日志、
恢复**，日常输入时**不需要**连接。


## 2. 硬件与开发环境

- 目标硬件：ESP32-S3 开发板，建议使用具备 **16MB Flash / 8MB PSRAM** 的 ESP32-N16R8 类板卡。
- 工具链：Arduino IDE 2、Arduino-ESP32 核心（3.x 系列）以及 Python 3；桌面端串口通信依赖见 `tools/esp32-desktop/requirements.txt`。
- 分区方案：使用 Arduino-ESP32 核心提供的 **Default 4MB with spiffs** 分区方案；`FlashSize=16M` 用于声明芯片 Flash 容量，**不代表需要修改分区表**。固件使用该方案中的 `spiffs` 分区，不要求改写自定义分区表。
- 触发按键：**BOOT 键 = GPIO0**（内部上拉，低电平有效）；不同开发板的板载按键接线可能不同，请以实际硬件为准。

## 3. 运行 Windows 桌面程序

```powershell
cd <本项目目录>\tools\esp32-desktop
python .\hid_text_writer.py               # 图形界面
```

依赖：`pyserial==3.5`（见 `tools/esp32-desktop/requirements.txt`）。

命令行方式（与 GUI 按钮共用同一套逻辑）：

```powershell
python .\hid_text_writer.py --port COMx --ping                 # 查询设备（将 COMx 替换为实际端口）
python .\hid_text_writer.py --port COMx --write "要写入的文本"  # 写入
python .\hid_text_writer.py --port COMx --read                   # 读回校验
```

### 写入流程
1. 打开程序 → 自动枚举可用串口；选择开发板的 USB CDC 控制端口（可用“刷新串口”重选）。
2. 在文本框输入内容 → 界面实时显示字节数/上限/字符支持范围。
3. 点击“写入设备” → 程序先做字符与长度校验，再发协议帧。
4. 成功判定：**必须收到设备有效响应帧**，状态栏显示
   `写入成功 (OK) — 已保存 N 字节, CRC32=0x…, 版本号 seq=…`。
   超时/断开/CRC 错/字符不支持/存储失败都会显示明确失败信息。
5. 可点“读取设备”二次核对（本地重算 CRC 与设备返回比对）。
6. 关闭程序即可，无驻留进程、无后台服务、不占用串口。

### 字符支持范围（界面同样有提示）
- 支持：英文字母、数字、常见英文标点、空格、换行 `\n`、制表符 `\t`
  （ASCII `0x20-0x7E` + `\n` + `\t`，US 键盘布局）
- 不支持：中文、Emoji、其它 Unicode、CR (`\r`) → **写入前直接拒绝并明确提示**，
  不静默替换、不走剪贴板绕过
- 上限：**10,240 字节 (10 KiB)**，按字节计（ASCII 每字符 1 字节）。
  超长/空文本在两端都被明确拒绝，不做静默截断

## 4. 连接（单线）

1. 用一根 USB 数据线把开发板的**原生 USB 口**（GPIO19=D-, GPIO20=D+）接到电脑。
   日常输入只需要这一根线。
2. Windows 会同时枚举出两个设备（均使用系统自带驱动，无需安装任何东西）：
   - **HID Keyboard Device** — `HID\VID_303A&PID_1001&MI_00`
   - **USB 串行设备 (COMx)** — `USB\VID_303A&PID_1001&MI_01`
3. 打开/关闭该 COM 口**不会**复位设备：固件调用 `USBCDC::enableReboot(false)`，
   关闭了 1200bps touch 和 DTR/RTS 四步序列触发的 bootloader 重启。
   实测反复开关串口后 HID 设备不消失、不重新枚举。
4. 串口日志（FTDI，可选）中 `[boot] ready ... hid_ready=1` 表示固件检测到 HID 已挂载。

> FTDI 口与原生口**不要**同时接入不同电源。刷写/看日志时接 FTDI 即可；
> 日常输入时只接原生口。

## 5. 触发输入

### 5.1 通过 Windows 托盘程序（日常用法）

1. 启动 `VirtualHIDTyper.exe`（普通用户权限即可），编辑并保存文本。
2. 切换到目标窗口，让编辑区获得焦点。
3. 按用户键盘上的**开始快捷键**（默认 `Ctrl+Alt+Shift+F9`）：
   程序通过 CDC 同步文本并发送 `CMD_START_TYPING`，ESP32 用真实 HID 键盘逐字符输入。
4. 按**停止快捷键**（默认 `Ctrl+Alt+Shift+F10`）：程序发送 `CMD_CANCEL_TYPING`，
   固件立即停止推进状态机并 `releaseAll()`，剩余文本**不会**补发。

协议细节见 `protocol.md`（0x04 / 0x05 / 0x06）。

### 5.2 通过板上 BOOT 键（可选，本地）

1. 打开目标应用并让编辑区获得键盘焦点。
2. 按一下 **BOOT 键**（GPIO0，按一下即松开）。
3. 设备逐字符输入已保存文本（首字符前 80ms，按住 8ms，字符间隔 15ms），
   结束时释放全部按键。
4. 一轮输入完成前再按键会被忽略；输入中按 BOOT 不会打断（请用 Windows 停止快捷键，
   或等待本轮结束）。
5. USB 未连接/未枚举时按键无效（串口会打印原因），**不会自动补发**。
6. 输入过程中拔出原生 USB 会**中止本轮**并释放按键，重连后**不会自动续打**。

### 5.3 v1.2 远程控制命令一览

| 命令 | ID | 响应 payload | 说明 |
|---|---:|---:|---|
| `CMD_WRITE_TEXT` | 0x01 | 14 B | v1 行为完全不变；输入中返回 BUSY 且不改文本 |
| `CMD_READ_TEXT` | 0x02 | 13 + n | v1 行为完全不变 |
| `CMD_PING` | 0x03 | 16 B | v1 长度/字段不变，`fw_ver=0x0102` |
| `CMD_START_TYPING` | 0x04 | 16 B | 用设备已存文本开始输入，立即返回接受/拒绝 |
| `CMD_CANCEL_TYPING` | 0x05 | 16 B | 同步 `releaseAll()` 后再响应，可重复调用 |
| `CMD_GET_STATUS` | 0x06 | 16 B | 进度/状态/错误/租约剩余/会话号/能力位 |

状态响应 16 字节布局、状态枚举与错误码见 `protocol.md`。

安全语义：

- **非阻塞状态机**：`beginTypeText()` 只校验并返回，字符由 `loop()` 中的
  `HidTyper::poll()` 按原节奏（80/8/15 ms）推进，因此输入期间串口命令照常处理。
- **取消及时性**：收到有效 CANCEL 命令后会停止推进输入状态机并释放所有按键；
  实际响应时间取决于 USB 与主机调度环境。
- **单一任务**：输入中再次 START 返回 `ST_ERR_BUSY`，不重头输入、`session_id` 不变。
- **主机租约**：主机发起的任务若 3000 ms 内没有任何有效协议帧，固件自动中止
  （`state=FAILED, reason=LEASE`）并释放按键；本地 BOOT 任务不受租约影响。
- **断连/错误**：USB HID 不就绪时 START 返回 `ST_ERR_HID_NOT_READY`；输入中
  断连立即中止且不自动续打。

### 5.4 固件测试

```powershell
# 主机侧状态机单元测试: 原样编译生产 hid_typer.cpp (HID_TYPER_HOST_TEST)
powershell -NoProfile -ExecutionPolicy Bypass -File firmware\test\build_state_test.ps1
# 结果: firmware state tests: 59 checks, 0 failures
```

实机验收（协议 + USB HID 打字）需要连接 ESP32-S3 开发板，使用
`tools/esp32/` 下的设备测试脚本和 `windows/VirtualHIDTyper/e2e_device.py`
驱动真实记事本回读。DTR 说明：本固件的 TinyUSB CDC 仅在 DTR 置位后接收主机
数据（`tools/esp32-desktop/hid_text_writer.py` 已在打开串口后恢复 DTR=True）；
USB HID 输出与 DTR 无关。

## 6. 编译与烧录固件

```powershell
$cli = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$FQBN = "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=default,CDCOnBoot=default"

& $cli compile --fqbn $FQBN <项目>\firmware\esp32_hid_text_device
& $cli upload --port COMx --fqbn $FQBN <项目>\firmware\esp32_hid_text_device  # 将 COMx 替换为实际刷写端口
```

- `USBMode=default` = USB-OTG (TinyUSB)，HID 与 CDC 复合设备必需；
  `CDCOnBoot=default`（关闭核心自带的 `Serial`-over-USB）是**刻意的**：
  固件自己实例化 `USBCDC ControlSerial` 作为控制端口，调试日志继续走 UART0，
  两条通道互不混流。
- **禁止**设置 `EraseFlash=all`（全片擦除）；普通上传只替换应用固件，
  SPIFFS 中已保存的文本会保留（实测：升级后首启日志
  `[store] loaded from filesystem /textB.bin: len=50 ... seq=15`）。
- 不修改分区表，不碰 eFuse。

Arduino IDE 2 图形操作：打开 `firmware/esp32_hid_text_device`，
开发板选 `ESP32S3 Dev Module`，菜单设置 Flash Size=16MB、PSRAM=OPI PSRAM、
USB Mode=USB-OTG (TinyUSB)、USB CDC On Boot=Disabled，并选择开发板对应的实际端口 → 验证/上传。

## 7. 项目结构

```
firmware/
├── README-esp32.md                  本文件
├── protocol.md                      串口帧协议规范 (两端实现依据)
└── esp32_hid_text_device/           ESP32 固件 (Arduino sketch)
    ├── esp32_hid_text_device.ino    主流程 (setup/loop, 触发编排)
    ├── protocol.h                   帧格式/状态码/CRC32/字符集规则
    ├── config_link.h/.cpp           帧解析、命令分发、响应封装
    ├── text_store.h/.cpp            SPIFFS 双槽位持久化 + 回读校验 + NVS 旧数据迁移
    ├── trigger_button.h/.cpp        BOOT 键消抖/单次触发/需释放再按
    ├── hid_typer.h/.cpp             USB HID 非阻塞输入状态机、节奏、失败中止
    └── test/                        主机侧状态机单元测试 (不依赖硬件)
        ├── build_state_test.ps1     编译并运行 (原样编译生产 hid_typer.cpp)
        ├── state_test.cpp           59 项断言: 完成/取消/BUSY/断连/租约/心跳/非法字符
        └── host_test_hal.h/.cpp     假时钟 + 假键盘 HAL
tools/
├── esp32-desktop/
│   ├── hid_text_writer.py           Tkinter GUI + CLI (共用写入/读取逻辑)
│   ├── requirements.txt             依赖清单
│   └── test_control_lines.py        CDC 控制线回归测试
└── esp32/
    ├── capture_serial.py            串口日志抓取工具 (--ts 可附时间戳)
    ├── read_notepad.ps1             UI Automation 只读取记事本内容 (HID 输入客观证据)
    ├── protocol_edge_tests.py       设备端协议边界测试 (绕过桌面端预校验, 29 项断言)
    └── fault_inject_tests.py        故障注入测试 (仅 TEXT_STORE_FAULT_INJECT 测试构建)
```

## 8. 已知问题与恢复方法

| 问题 | 处理 |
|---|---|
| 打开串口时板子可能复位一次 | 正常（自动复位电路）；程序内先做 RTS 复位脉冲 + 等待 `[boot] ready`，再由 PING 重试兜底 |
| 长文本写入/读取偶尔超时 | 超时按帧长计算（见 `protocol.md` 与下方“超时机制”），10 KiB 在 115200 baud 下单程约 0.9 s，单次期限约 2.8 s；若仍超时通常是串口被占用或波特率不符，**不要**靠盲目加大固定超时解决 |

| 想清空已存文本 | 目前无删除命令，写入新文本覆盖即可（不需擦除整片 Flash） |
| 升级后旧文本还在吗 | 在。旧 NVS 数据本固件**只读**，开机自动迁移到 `/textA.bin` 并回读验证；迁移成功也不删除 NVS，可反复恢复 |
| 按键无反应 | 看串口日志：`HID not enumerated`=原生 USB 未连；`no valid text`=未写入文本 |
| 烧录失败 | 先确认端口未被占用、板型 FQBN 与实际端口选择正确；**不要**用全片擦除“解决” |
| 固件异常需恢复 | 重新 `arduino-cli upload` 正常固件即可；NVS 文本与应用分区分离，重新烧录不会删除已存文本 |

## 9. 串口超时机制（10 KiB 后不再用固定短窗）

10 KiB 文本在 115200 baud、8N1（10 bit/字节 = 11,520 字节/秒）下的线路时间：

| 操作 | 请求帧 | 响应帧 | 单程传输 | 计算出的单次期限 |
|---|---|---|---|---|
| PING | 10 | 26 | ~0.003 s | 1.0 + 2×36/11520 ≈ **1.01 s** |
| WRITE 10 KiB | 10,250 | 24 | ~0.89 s | 1.0 + 2×10,274/11520 ≈ **2.80 s** |
| READ 10 KiB | 10 | 10,263 | ~0.89 s | 1.0 + 2×10,273/11520 ≈ **2.80 s** |

```
期限 = DEADLINE_FIXED (1.0s 设备处理预算) + 2 × (请求帧 + 响应帧) / 11520
```

- 系数 2 覆盖“请求发出”与“响应回传”两个方向的线路时间，再留 1 s 给设备
  解析 + SPIFFS 写入 + 回读校验。
- **只有整个期限都没收到匹配响应帧才重试**（WRITE 2 次、READ/PING 4 次），
  不会在正常传输途中误判失败后重复发送。
- WRITE 重发是安全的：设备对完全相同的内容去重（`written=0`、seq 不变、
  不写介质），因此重试不会产生重复写入或 seq 虚增。
- 该机制**不替代**协议校验：CRC、长度、字符集、状态码检查全部保留。

## 10. 限制说明（诚实边界）

- HID 发送的是**按键码**，不是任意 Unicode：中文/Emoji 无法直接输入（已拒绝并提示）。
  将来如需中文需单独方案（依赖输入法/布局），本次未实现也未声称支持。
- 要求目标应用接受 **US 键盘布局** 的按键事件。
- HID 枚举与记事本实际输入需在真实开发板上验证（使用 `tools/esp32/` 与
  `windows/VirtualHIDTyper/e2e_device.py`）。
