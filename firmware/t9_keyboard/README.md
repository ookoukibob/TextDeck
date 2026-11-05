# T9 Touch Keyboard — ESP32-S3 USB HID 九键触摸键盘

独立运行的九键（T9 风格）触摸键盘：在 240×320 电阻触摸屏上按键，通过 **标准 USB HID Keyboard** 把字符发送到主机，屏幕实时回显输入内容。不需要电脑端软件。

这是 TextDeck 的一个**可选输入设备固件**，与现有的 `esp32_hid_text_device`（CDC 协议 + 远程打字）并列，互不影响。

---

## 1. 硬件

| 项目 | 说明 |
|---|---|
| MCU | ESP32-S3-N16R8（16 MB Flash / 8 MB OPI PSRAM） |
| 屏幕 | 2.4" SPI TFT，240×320，ILI9341 兼容（实测红板 V1.3） |
| 触摸 | XPT2046 电阻触摸（与屏同模块） |

### 接线

LCD（硬件 SPI）：

| 模块 | GPIO |
|---|---|
| VCC | 5V（**J1 必须断开**，见下方警告） |
| GND | GND |
| CS | 10 |
| RESET | 8 |
| DC/RS | 9 |
| SDI(MOSI) | 11 |
| SCK | 12 |
| SDO(MISO) | 13 |
| LED | 3V3 |

触摸（独立软件 SPI，**不与 LCD 共用任何信号线**）：

| 模块 | GPIO |
|---|---|
| T_CLK | 4 |
| T_DIN | 15 |
| T_DO | 7 |
| T_CS | 6 |
| T_IRQ | 5 |

> ⚠️ **供电警告**：模块上有 XC6206 LDO。外部接 5V 时 **J1 必须断开**；J1 短接 + 5V 会直灌 3.3V 轨烧屏。

> ⚠️ **触摸必须独立走线**：XPT2046 上限 2 MHz，与 LCD 共用 40 MHz 总线时读不到数据。而改用软件 SPI 后如果 CLK/DIN 仍接在 LCD 的 SCK/MOSI 上，`pinMode()` 会把这两个脚从 FSPI 外设手里抢走，导致屏幕黑屏。所以 T_CLK/T_DIN 必须用独立引脚。

---

## 2. 编译与烧录

### FQBN（**必须严格用这个组合**）

```
esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,USBMode=default,CDCOnBoot=cdc
```

两个关键参数：

- **`USBMode=default`** — USB-OTG / TinyUSB。HID 键盘需要它。
- **`CDCOnBoot=cdc`** — 保留一个 CDC 串口用于烧录。

> 实测结论：`USBMode=default` 配 `CDCOnBoot=default` 时，设备枚举不出 HID 接口
> （`USB Composite Device` 只有 CDC + JTAG 两个接口）。加上 `CDCOnBoot=cdc`
> 后 HID 正常出现在 `HID\VID_303A&PID_1001&MI_00`。

### 烧录

```powershell
arduino-cli compile --fqbn "<上面的 FQBN>" firmware/t9_keyboard
arduino-cli upload  -p COMx --fqbn "<上面的 FQBN>" firmware/t9_keyboard
```

烧录端口是 UART 口（带桥接芯片那个）。运行时 HID 从 USB-OTG 口输出。

> ⚠️ 如果 `arduino-cli` 报
> `following symlink ...\Arduino15\packages: The system cannot find the path specified`，
> 说明 `Arduino15` 是指向别处的 junction。用 `--config-file` 显式指定数据目录即可：
>
> ```yaml
> directories:
>   data: <真实路径>/Arduino15
>   downloads: <真实路径>/Arduino15/staging
>   user: <用户目录>/Documents/Arduino
> ```

---

## 3. 界面

屏幕 240×320 竖屏（`rotation = 2`，MADCTL `0x20`）。界面**全英文**，不使用中文以免依赖字库。

```
┌─────────────────────────────┐
│ USB OK  SH  CAPS  ABC    12 │  状态栏 18px
├─────────────────────────────┤
│ Ready                       │  输入回显 54px
│ tap keys to type            │
├─────────────────────────────┤
│ TAP TWICE TO CYCLE          │  候选条 22px
├─────────────────────────────┤
│  1      2 ABC    3 DEF      │
│  4 GHI  5 JKL    6 MNO      │  数字区 3x4
│  7 PQRS 8 TUV    9 WXYZ     │  每键 76x41
│  *      0         #         │
├─────────────────────────────┤
│ SHIFT │ CAPS  │ DEL         │  功能键 2 行
│ SPACE │ ENTER │ SYM         │  每键 77x21
└─────────────────────────────┘
```

### 输入方式

九键连击（T9）：同一个数字键在 1.4 秒内连按，循环该键上的字母。

| 按键 | 字母 |
|---|---|
| 2 | a b c |
| 3 | d e f |
| 4 | g h i |
| 5 | j k l |
| 6 | m n o |
| 7 | p q r s |
| 8 | t u v |
| 9 | w x y z |

- 按 5 三下 → `j` `k` `l`，停在 `l`
- 切到别的键或等 1.4 秒，当前字母"定稿"
- 数字键 `1` `0` `*` `#` 直接输出对应字符（`0` 输出空格）

### 功能键

| 键 | 作用 |
|---|---|
| SHIFT | 一次性大写（下一个字母大写后自动复位） |
| CAPS | 大写锁定（切换） |
| DEL | 退格 |
| SPACE | 空格 |
| ENTER | 回车 |
| SYM | 符号层（预留） |

---

## 4. HID 实现要点

### 必须用 `pressRaw()`，不能用 `press()`

这是本项目调试中最重要的发现。

```cpp
// 错误：press() 把参数当 ASCII 字符，用内部布局表转换成 usage+modifier
// 传 0x1E（想表示数字键 1）会被当成控制字符查表失败，实测输出全错
Keyboard.press(0x1E);       // ✗

// 正确：pressRaw() 直接把参数当 HID usage 发出去
Keyboard.pressRaw(0x1E);    // ✓
```

修饰键同样要传 usage，**不是 HID report 里的 modifier 位掩码**：

| 键 | usage |
|---|---|
| LeftCtrl | `0xE0` |
| LeftShift | `0xE1` |
| LeftAlt | `0xE2` |
| LeftGUI | `0xE3` |

### 按键时序

```cpp
pressRaw(mod);  delay(6);
pressRaw(usage); delay(22);      // 保持 22ms，太短主机可能丢键
releaseRaw(usage); delay(6);
releaseRaw(mod);
releaseAll();   delay(18);       // 显式发空报告，确保全部释放
```

### 不卡键的保证

- 每次按键都是一次完整的 按下 → 释放 → `releaseAll()`
- 触摸丢失、页面切换、异常分支都强制 `releaseAll()`
- 九键连击修改字母时用「退格 + 重发」，不会留下按下状态

---

## 5. 代码结构

| 文件 | 职责 |
|---|---|
| `t9_keyboard.ino` | 主逻辑：初始化、UI 绘制、触摸扫描、九键状态机、HID 输出 |
| `t9_layout.h/.cpp` | 320×240 竖屏布局：数字键/功能键几何、命中测试 |
| `keymap.h/.cpp` | 字符 → HID usage 映射表（完整 ASCII 0x20–0x7E），Shift/Caps 组合规则 |
| `input_buffer.h/.cpp` | 屏幕回显缓冲（512 字节，溢出丢最旧） |
| `src/MSP2402.*` | ILI9341 显示驱动（FSPI，40 MHz） |
| `src/MSP2402_Touch.*` | XPT2046 触摸驱动（独立软件 SPI） |
| `src/msp_font_ascii.h` | 5×7 ASCII 字库（0x20–0x7E） |
| `src/msp_font_cn.h` | 16×16 中文字库（界面不使用，保留备扩展） |
| `tools/hid_test.ps1` | 自动化测试脚本（读/清空/等待记事本内容） |

---

## 6. 测试脚本

`tools/hid_test.ps1` 通过 UIAutomation 读写记事本，用于自动化验证 HID 输出。

```powershell
# 读取记事本内容
powershell -ExecutionPolicy Bypass -File tools\hid_test.ps1 -Action read

# 清空并置前（准备测试）
powershell -ExecutionPolicy Bypass -File tools\hid_test.ps1 -Action clear

# 等待出现指定内容
powershell -ExecutionPolicy Bypass -File tools\hid_test.ps1 -Action wait -Expect "abc" -Timeout 15

# 断言内容包含指定字符串
powershell -ExecutionPolicy Bypass -File tools\hid_test.ps1 -Action check -Expect "abc"
```

> 脚本必须是 **UTF-8 with BOM**，否则 PowerShell 会按 GBK 解析中文导致语法错误。

---

## 7. 已验证 / 未验证

### 已在实机验证

| 项目 | 结果 |
|---|---|
| USB HID 枚举 | ✅ `HID\VID_303A&PID_1001&MI_00`，状态 OK |
| HID 字符输出正确性 | ✅ `!Aa` — Shift+1、Shift+A、小写 a 全对 |
| 九键连击 | ✅ 按 2/3/5×3 → 输出 `adl` |
| Backspace / Caps / Enter / Space | ✅ 用户实测通过 |
| 显示方向与布局 | ✅ 截图确认 |
| 显示几何（框/圆/十字） | ✅ 像素级正确 |

### 未完成

- **未有硬件在环时的长时稳定性测试**（连续快速按键、长时间使用是否卡键）
- `SYM` 符号层为预留，未实现
- 未做触摸校准参数的运行时调整（当前用固定值 `652/-13/866/-21`）

### 已知限制

- 只支持 ASCII（0x20–0x7E + `\n` `\t`）。中文等非 ASCII 字符需要目标机输入法配合，本固件不处理。
- 九键输入不能像手机那样根据词典猜测整词，只能逐字母循环。

---

## 8. 与 TextDeck 的关系

本项目是**独立的备用固件**，不改动现有的 `esp32_hid_text_device`：

- 不修改 USB 描述符、PID/VID、CDC 协议
- 不修改 Windows 端驱动或工具
- 单独编译、单独烧录，两者互斥（同一时刻只能烧一个）

---

## 9. 调试经验（踩过的坑）

1. **`Keyboard.press()` 的坑**：它按 ASCII 查表，传 usage 会得到完全错误的结果。必须用 `pressRaw()`，修饰键用 `0xE1` 而不是 `0x02`。

2. **`USBMode=default` + `CDCOnBoot=default` 不出 HID**：设备只有 CDC + JTAG，没有 HID 接口。改用 `CDCOnBoot=cdc` 才正常。

3. **触摸不能与 LCD 共用引脚**：硬件 SPI 共用总线读不到（2 MHz 上限），软件 SPI 共用引脚会抢走 FSPI 导致黑屏。必须完全独立。

4. **`MADCTL = 0xE0`**：这块面板 GRAM 原生 320×240 横，必须置 MV 才能当竖屏；且是 RGB 顺序，不能带 BGR 位（否则红蓝互换）。

5. **调试显示不要只靠摄像头**：夜间室内光线差，笔记本摄像头在高增益下噪点严重，小字号文字会被噪点淹没，误判成渲染 bug。用主机端仿真验证渲染逻辑，用大字号/高对比度截图确认。
