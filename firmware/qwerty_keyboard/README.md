# QWERTY Touch Keyboard — ESP32-S3 USB HID 全功能触摸键盘

320×240 横屏完整 QWERTY 键盘：数字、字母、符号、功能键齐全，通过**标准 USB HID Keyboard** 输出到主机，屏幕实时回显。

与 `t9_keyboard` 是**同一套硬件、两套输入方式**：

| | qwerty_keyboard（本目录） | t9_keyboard |
|---|---|---|
| 屏幕方向 | 横屏 320×240 | 竖屏 240×320 |
| 输入方式 | 每键一个字符，直接输入 | 九键连击循环选字母 |
| 适合 | 快速输入、符号多 | 手指粗/屏幕小、单手操作 |
| 键数 | 每行 10 键 | 3×4 数字网格 |

两者共用 `src/` 里的显示与触摸驱动、`keymap.*` 字符映射、`input_buffer.*` 回显缓冲。

---

## 1. 硬件与接线

和 `t9_keyboard` **完全相同**，见 [../t9_keyboard/README.md](../t9_keyboard/README.md)。

要点复述：

- LCD 用硬件 SPI：CS=10 DC=9 RST=8 MOSI=11 SCK=12 MISO=13
- 触摸用**独立**软件 SPI：T_CLK=4 T_DIN=15 T_DO=7 T_CS=6 T_IRQ=5
- **VCC 接 5V 时 J1 必须断开**（否则 5V 直灌 3.3V 轨）
- 触摸不能与 LCD 共用引脚（共用时读不到数据，用软件 SPI 共用又会抢走 FSPI 导致黑屏）

---

## 2. 编译与烧录

FQBN **必须**是：

```
esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=default,CDCOnBoot=cdc
```

```powershell
arduino-cli compile --fqbn "<FQBN>" firmware/qwerty_keyboard
arduino-cli upload  -p COMx --fqbn "<FQBN>" firmware/qwerty_keyboard
```

`USBMode=default` 是 TinyUSB（HID 必需），`CDCOnBoot=cdc` 保留烧录串口。
两个参数缺一不可 —— 实测 `CDCOnBoot=default` 时 HID 接口不会出现。

---

## 3. 布局

横屏 320 宽，每行 10 键，键宽 29px、高 27px，共 6 行。

```
┌──────────────────────────────────────────────┐
│ USB OK  SH --  CAPS  CTL  ABC              0 │  状态栏
├──────────────────────────────────────────────┤
│ Ready                                        │  输入回显
│ tap keys to type                             │  3 行
├──────────────────────────────────────────────┤
│ 1 2 3 4 5 6 7 8 9 0                          │
│ Q W E R T Y U I O P                          │
│  A S D F G H J K L ;                         │  缩进半格
│ SHIFT Z X C V B N M , .                      │  SHIFT 加宽
│ SYM / - = SPACE  <- ->  DEL                  │  SPACE/DEL 加宽
│ CTRL CAPS TAB ENTER ESC                      │
└──────────────────────────────────────────────┘
```

### 功能键

| 键 | 作用 | 说明 |
|---|---|---|
| SHIFT | 一次性大写 | 打完一个字符自动复位 |
| CAPS | 大写锁定 | 切换，状态栏显示 |
| CTRL | 一次性 Ctrl | 可与字母组合成 Ctrl+A/C/V；用完自动清 |
| SYM | 符号层 | 预留 |
| TAB | 制表符 | |
| ENTER | 回车 | 回显区换行 |
| DEL | 退格 | 同时删除显示字符 |
| <- -> | 左右方向键 | 只发 HID，不影响回显 |
| ESC | Esc | |

---

## 4. HID 实现（关键坑）

### 必须用 `pressRaw()` 而不是 `press()`

```cpp
Keyboard.press(0x1E);      // ✗ 把 0x1E 当 ASCII 控制字符查表，输出完全错误
Keyboard.pressRaw(0x1E);   // ✓ 直接按 HID usage 发送
```

调试时实测：`Keyboard.press()` 发送 `!@#` 结果是 `(((`。

### 修饰键传 usage，不是 report 位掩码

| 键 | usage |
|---|---|
| LeftCtrl | `0xE0` |
| LeftShift | `0xE1` |
| LeftAlt | `0xE2` |

### 时序

```cpp
pressRaw(mod);   delay(6);
pressRaw(usage); delay(22);    // 保持
releaseRaw(usage); delay(6);
releaseRaw(mod);
releaseAll();    delay(18);    // 显式空报告
```

### 防卡键

- 每次按键 = 完整「按下 → 释放 → releaseAll()」
- SHIFT / CTRL 都是**一次性**，用完立即清标志，避免后续输入全变成组合键
- 触摸丢失、越界、异常分支都不停留按下状态

---

## 5. 文件结构

| 文件 | 职责 |
|---|---|
| `qwerty_keyboard.ino` | 主逻辑：初始化、UI、触摸扫描、HID 输出 |
| `layout.h/.cpp` | 320×240 横屏布局、键宽权重、命中测试 |
| `keymap.h/.cpp` | 字符 → HID usage 映射、Shift/Caps 组合 |
| `input_buffer.h/.cpp` | 回显缓冲 |
| `src/MSP2402.*` | ILI9341 显示驱动 |
| `src/MSP2402_Touch.*` | XPT2046 触摸驱动 |
| `tools/hid_test.ps1` | 自动化测试（读写记事本） |

---

## 6. 测试

```powershell
# 清空记事本并置前
powershell -ExecutionPolicy Bypass -File tools\hid_test.ps1 -Action clear

# 读内容
powershell -ExecutionPolicy Bypass -File tools\hid_test.ps1 -Action read
```

> 脚本须为 **UTF-8 with BOM**，否则 PowerShell 按 GBK 解析中文会语法错误。

### 已验证（实机）

- USB HID 枚举：`HID\VID_303A&PID_1001&MI_00`，状态 OK
- 字符输出：`!Aa` 全对（Shift+1、Shift+A、小写 a）
- 九键版已验证：连击、Backspace、Caps、Enter、Space

### 未验证

- **QWERTY 版的触摸实际输入**（编写时无硬件在线，仅确认编译通过）
- 长时连续按键稳定性
- SYM 符号层（未实现）

---

## 7. 调试经验

见 [../t9_keyboard/README.md](../t9_keyboard/README.md) 第 9 节，两个项目共享同样的坑：

1. `press()` vs `pressRaw()`
2. `USBMode` / `CDCOnBoot` 组合决定 HID 能否枚举
3. 触摸必须独立引脚
4. `MADCTL` 与屏幕朝向（横屏用 `rotation=1` → `0xA0`）
5. 不要只靠摄像头判断渲染结果
