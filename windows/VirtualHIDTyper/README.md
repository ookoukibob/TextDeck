# VirtualHIDTyper —— 逐字符输入工具（ESP32 USB HID / Virtual HID / 软件输入 三模式）

三种输入模式，全部**显式选择、绝不静默切换**：

- **ESP32 USB HID 模式（推荐）**：把文本通过 **USB CDC 串口**下发到
  ESP32-S3，由开发板以**真实 USB HID 键盘**逐字符输入。**普通用户权限**即可运行，
  不需要内核驱动、不需要 TESTSIGNING、不需要管理员。
  需要本仓库 `firmware/esp32_hid_text_device/` 的 v1.2.0 固件；编译与烧录步骤见 `firmware/README-esp32.md`。
- **TextDeck HID 模式**：通过内核驱动 `TextDeckHidDriver`（VHF 虚拟 HID 键盘）向当前焦点
  窗口注入键盘 HID 报告。需要驱动已安装且签名符合本机策略。
- **软件输入模式（SendInput，无需驱动）**：用户态 `SendInput` 直接发送 Unicode 键盘
  事件。**无需驱动、无需管理员、无需重启**，支持任意 Unicode 文字（含中文与 emoji）。

**正文输入不经过剪贴板，也不使用 SendKeys/SendEvent/SendText**；全局快捷键只用于
开始/停止控制。程序**不会静默切换模式**：模式由单选按钮显式选择，且始终显示在
标题栏与状态栏。

## 程序入口

| 文件 | 说明 |
|---|---|
| `bin\VirtualHIDTyper.exe` | 主程序（GUI，双击或命令行运行；ESP32 / 软件模式无需管理员） |
| `bin\map_selftest.exe` | 字符映射 + ESP32 协议编解码自测（控制台，不发送任何输入，退出码 0=通过） |
| `build_app.ps1` | 重新构建两个 exe 的脚本（无需管理员） |
| `e2e_acceptance.py` | **ESP32 模式端到端验收**（真实 EXE + 真实开发板 + 记事本逐字符回读、停止热键、焦点丢失、宿主进程被杀） |
| `e2e_device.py` | ESP32 设备协议 + HID 验收（真实开发板：写入/读回/CRC/拒绝/远程 START/CANCEL） |
| `e2e_win.py` | 上面两个脚本共用的 Win32 ctypes 助手 |
| `e2e_software_mode.ps1` | 软件模式端到端测试（真实窗口输入、三选一对话框、停止/焦点/倒计时/emoji/剪贴板哨兵） |
| `e2e_target_host.ps1` | 端到端测试的专用目标文本窗口宿主（被 `e2e_software_mode.ps1` 调用） |

> 自动化测试必须使用 `--data-dir=<临时目录>` 运行，绝不碰 `%APPDATA%\VirtualHIDTyper`。
> `e2e_acceptance.py` 会在开始前后对真实 `settings.ini` / `text.txt` 做 SHA-256 比对，
> 并把它作为一条验收项。

## 使用方法（ESP32 USB HID 模式）

1. 原生 USB 线连接 ESP32-S3（同一接口同时枚举 HID 键盘 + CDC 串口）。
2. 启动 `VirtualHIDTyper.exe`（普通用户），模式选 **ESP32 USB HID**；
   串口自动选中 Espressif 接口，可手动改，选择会保存。
3. 编辑并保存文本（ASCII 可打印字符 + 换行 + TAB，上限 10 KiB）。
4. 切到目标窗口，按开始快捷键（默认 `Ctrl+Alt+Shift+F9`）；停止用
   `Ctrl+Alt+Shift+F10` / 托盘“停止输入” / 主窗口 Stop。


## 使用方法（软件输入模式 —— 无需驱动即可实际输入）

1. 启动 `VirtualHIDTyper.exe`（无需管理员），点选
   **「软件输入模式（SendInput，无需驱动）」** 单选按钮。
   - 选择会记住到 `%APPDATA%\VirtualHIDTyper\settings.ini`（`InputMode=software`），
     下次启动保持；也可命令行 `VirtualHIDTyper.exe --mode=software` 指定。
2. 在多行文本框输入（或事后编辑）要发送的文本（英文/数字/标点/中文/Tab/换行/
   emoji 均可）；文本与间隔自动保存到 `%APPDATA%\VirtualHIDTyper\`（text.txt + settings.ini）。
3. 设置"间隔(ms)"（默认 30，范围 5–2000）。
4. 点击"开始"（或默认开始快捷键 Ctrl+Alt+Shift+F9）→ **3 秒倒计时内把光标点击到目标窗口**。
5. 输入完成后状态栏给出统计：`完成【软件输入模式】：本次实际输入 X 字符，跳过 Y
   字符，无错误。（模式已标明，软件模式=SendInput，非虚拟 HID 设备）`。

## 使用方法（Virtual HID 模式 —— 需驱动）

1. （如提示）以管理员身份运行——驱动设备默认 ACL 仅允许管理员打开；程序检测到
   访问被拒绝时会询问是否自动提权重启。
2. 选中「Virtual HID 模式」，输入文本与间隔（同上）。
3. 点击"开始"→ 3 秒倒计时内点击目标窗口。**驱动未安装/设备打不开时会弹三选一对话框**：
   - **是** = 改用【软件输入模式】继续（SendInput Unicode，无需驱动）
   - **否** = 留在 Virtual HID 模式（解决驱动问题后重新点击"开始"）
   - **取消** = 停止，不发送任何字符

## 快捷键

| 功能 | 组合键 | 说明 |
|---|---|---|
| 开始 | **Ctrl+Alt+Shift+F9** | 若被其他程序占用，自动回退 Ctrl+Alt+Y → Ctrl+Alt+G，实际生效组合显示在状态栏 |
| 停止 | **Ctrl+Alt+Shift+F10** | 倒计时阶段按 = 取消（不发送任何字符）；输入阶段按 = 立即停发并释放按键 |

## 本表：Windows 本机两种输入模式差异（ESP32 模式见前文）

| 维度 | Virtual HID 模式 | 软件输入模式 |
|---|---|---|
| 机制 | 内核驱动 VHF 虚拟 HID 键盘（`\\.\TextDeckHidDevice`） | 用户态 `SendInput`（`KEYEVENTF_UNICODE` + 虚拟键） |
| 前提 | 驱动已安装 + 签名过闸门 + ACL 允许 | 无驱动、无管理员、无重启 |
| 字符范围 | 仅 US QWERTY 可映射字符，其余跳过并计数 | 任意 Unicode；孤立代理/NUL 跳过并计数 |
| 回车/Tab | HID 键位（Shift+Enter 选项） | 回车→`VK_RETURN`（可选带 Shift），Tab→`VK_TAB`（不作为普通 Unicode 发送） |
| 键盘布局 | 目标布局决定字形（非 US 布局符号可能变形） | 与目标键盘布局无关 |
| 真实性 | 真实 HID 设备输入 | 普通软件键盘输入，**不是**虚拟 HID 设备 |

## 行为说明 / 安全设计

- 换行默认发送 **Shift+Enter**（复选框可关闭，改为普通 Enter）；两种模式一致。
- 软件模式每个事件一次 `SendInput` 批；**astral 字符的高+低代理对在同一批内发送，
  绝不拆开**。
- 每个字符 = 按下报告 + 释放报告；释放报告为全零（释放一切按键与修饰键）。
- 倒计时结束时记录目标前台窗口；输入过程中前台窗口一旦变化 → 自动停止并释放按键，
  防止输入串入其他窗口。
- 若焦点仍在本程序窗口则拒绝开始（防止自我输入）。
- 输入过程中模式单选按钮被禁用（一次运行内模式不漂移）；完成/错误信息均标注本次
  运行所用模式。
- 驱动通信失败（断开/卸载）→ 立即停止输入、释放按键、显示错误码，不做无限重试
  （仅对 ERROR_BUSY 做 ≤200ms 的有界重试）。
- 软件模式 `SendInput` 失败 → **立即失败中止**并报错误码；错误码 5（拒绝访问）
  如实标注为系统 UIPI/完整性级别限制，**不尝试绕过**（高完整性窗口无法由本程序输入）。
- 停止后不会再发出任何排队字符；停止时清理可能残留的按键状态。
- UI 线程不被输入任务阻塞（输入在后台线程，通过窗口消息回报进度）。

**已知固有局限**：焦点保护的"检查前台窗口 → 发送"不是原子
操作，竞态窗口本质存在于发送动作本身，**无法彻底消除**（在不绕过 UIPI、不修改
系统安全配置的前提下没有可行的消除手段）；焦点切换瞬间，在途/队列中的个别事件
可能丢失或落点已改变。程序在每个输入事件前检查前台窗口、失焦即
停；就绪状态与输入中状态均带 UI 提示：**开始前确认目标窗口；输入过程中避免切换
焦点，必要时立即停止**（Ctrl+Alt+Shift+F10 或"停止"按钮）。

## 支持的键盘布局（重要，仅 Virtual HID 模式）

字符→HID 映射按 **US 英文 QWERTY（Windows 布局 00000409）** 制作：
小写/大写字母（含 Shift）、0–9 及 Shift 符号、空格、Tab、Enter，
以及 `- = [ ] \ ; ' ` , . /` 与其 Shift 形态 `_ + { } | : " ~ < > ?`。

**明确限制**：HID 报告只携带物理按键位置；目标会话的"当前键盘布局"决定实际字形。
若目标机活动布局不是美式键盘，Shift 标点可能映射为其他字符；本工具不声称在非 US
布局下所有符号正确。无 US 键位的字符（如 é、中文、—、…）会被计数跳过并在完成时
报告，不会静默发送错误按键。**软件输入模式不受此限制**（Unicode 直发）。

## 端到端测试

```powershell
# 默认路径：绝不写剪贴板、只操作本脚本自己启动的进程（无需管理员；exit 0 = 全部通过）
powershell -NoProfile -STA -ExecutionPolicy Bypass -File VirtualHIDTyper\e2e_software_mode.ps1
# 显式选择的剪贴板哨兵变体（会主动写入剪贴板，见下方 T7 说明）：
powershell -NoProfile -STA -ExecutionPolicy Bypass -File VirtualHIDTyper\e2e_software_mode.ps1 -ClipboardTest
# 剪贴板故障注入自测（内存后端模拟；不写入不清空真实剪贴板，仅首尾只读指纹；不启动 GUI）：
powershell -NoProfile -STA -ExecutionPolicy Bypass -File VirtualHIDTyper\e2e_software_mode.ps1 -ClipboardSeamSelfTest
```

覆盖（默认路径 **8 组 73 断言**：T0=23、T1–T6=48、T7=2；跳过的检查只记 SKIPPED、
不计入通过）：

- **T0 静态卫生检查**：对脚本自身源码扫描——
  禁止无 PID 限定的同名进程枚举/强杀、禁止全局类名查找 typer 窗口、禁止任何剪贴板
  cmdlet；破坏性剪贴板调用（owner-open/Empty/Set）只能出现在事务区内、哨兵写入调用
  必须位于保护性 try/catch/finally 之内、恢复调用必须位于 finally（FLOW-END 之后）；
  全文件唯一允许的 `OpenClipboard(NULL)` 只能住在只读助手里；属主窗口校验存在且
  开窗前重查；Open/Empty/Set/Enum 四个 P/Invoke 必须 SetLastError=true；枚举必须
  先清 last-error 且 0 返回仅在 ERROR_SUCCESS 时视为正常结束；安全门单测覆盖空/
  UnicodeText 可读/UnicodeText 不可读/多文本格式/HTML/RTF/文件/图片/错误态。
- **T1–T6 功能用例**：真实目标窗口逐字符回读比对（英文+数字+标点+中文+Tab+多行）、
  三选一对话框 否/取消/是 三路、停止热键、焦点切换自动停止、倒计时取消、emoji 代理对。
- **T7 剪贴板非干扰**：默认路径 = **只读指纹**（运行前后格式列表+Unicode 文本完全
  一致，证明脚本全程未写剪贴板）；`-ClipboardTest` 显式选择哨兵变体，且仅当原
  剪贴板可**证明精确往返**（空，或恰为 UnicodeText 单格式且内容确实可读）才写入
  哨兵，否则**写入前拒绝**并如实记录原因。写入采用**属主窗口事务**：
  先创建并校验一个属于本测试进程的隐藏窗口作为剪贴板所有者（`OpenClipboard(NULL)`
  + `EmptyClipboard` 会使属主置空并令 `SetClipboardData` 失败——写路径彻底弃用该
  组合；读路径不受此限），Empty 成功即置"原数据已销毁"状态位；此后任何分配/锁定/
  写入/关闭失败都立即抛入外层 finally 的恢复流程。`Opened` 状态位仅在
  `CloseClipboard()` **明确成功**时才翻转为 false：关闭失败即保留
  真实打开状态，由恢复流程先如实关闭遗留句柄——即使 `Emptied=false` 无需恢复
  内容也绝不漏掉打开状态清理。恢复**尝试**与恢复**验证**分开
  报告：只有重读格式列表+文本并与运行前指纹逐字节一致才宣称已验证；恢复失败 =
  显式断言失败，绝不吞异常、绝不伪造成功。日志只记录格式、长度、错误码与验证结果，
  **绝不记录剪贴板文本内容**。

**剪贴板故障注入自测**（`-ClipboardSeamSelfTest`）：在
同一脚本的读/门禁/事务/恢复逻辑上挂接内存后端，以**故障队列**在每个破坏性步骤
（属主校验、Open、Empty、Set、Close、枚举、恢复中的 reopen/Empty/Set/Close）注入
一次性故障（队列支持多步故障组合按序触发）。**实测 51 断言全过（exit 0；源码 49 个
断言调用点，S7 循环×2 迭代后运行时共 51 次，以实际运行日志计数为准）**，覆盖：

- 门禁矩阵 9 态在任何写入前拒绝（空/UnicodeText 可读放行/不可读/多文本格式/
  HTML/RTF/文件/图片/错误态）；
- 空/UnicodeText 原剪贴板完整往返 + 调用顺序轨迹证明 open→empty→set→close→
  恢复 的完整次序（S1/S2）；
- SetClipboardData 失败（Empty 已成功）→ 抛异常、`Opened`/`Emptied` 状态位准确、
  恢复被触发且**验证**通过（S3）；
- CloseClipboard 失败组合矩阵（S4/S10）：写入成功后关闭失败、Empty 失败×关闭
  成功、Empty 失败×关闭失败、Set 失败×关闭失败——关闭失败时 `Opened` 保持 true
  如实反映遗留打开状态，由恢复流程先关闭遗留句柄再继续，恢复结果**验证**通过；
- **OpenClipboard 失败注入**（S9，注入=真实重试全部耗尽后的失败结果，调用方
  状态转移与真实一致）：调用方识别为失败、未发生 Empty/Set、原剪贴板完整、
  无需恢复、绝不被报告为哨兵写入成功；
- 恢复阶段失败（reopen 失败/写回失败/恢复后关闭失败）→ 显式失败上报、
  `Attempted` 与 `Verified` 分开、清理后状态位准确（S5/S11）；
- 属主校验失败 → 任何破坏性调用计数全 0（S7）；未修改时恢复为严格空操作（S8）。

三层口径严格区分：**默认 E2E** = 仅运行前后只读指纹，不写入、不清空剪贴板；
**内存接缝测试** = 在模拟后端验证异常恢复逻辑——不写入、不清空真实剪贴板，仅在
套件运行前后各取一次只读指纹并断言一致；**真实哨兵写入—恢复闭环** = 尚未在隔离的
Windows 会话中验证（测试环境中的剪贴板内容不满足精确往返门禁，因此写入前会被拒绝；真实桌面会话中的哨兵写入—恢复闭环尚未验证）。接缝自测是恢复逻辑的模拟证明，
绝不表述为真实剪贴板恢复验证。

**进程隔离保证**：脚本总是自己启动一个新实例，并全程只按
**自己的 PID** 定位窗口/对话框/清理对象；启动前已存在的同名实例（任何路径）全程
不受影响——已有实测：预存实例在完整 E2E 运行后仍存活，窗口句柄/标题/状态/文本
零变化。启动失败或窗口识别超时 → 抛错**安全失败**（exit 1，不伪装通过），`finally`
只清理本轮创建的确切 PID（宽限期后也只强杀该 PID），绝不用"杀掉其他程序"来恢复
测试环境。

证据写入 `logs\e2e-evidence\` 与 `logs\e2e-software.log`（本机，不入库）。
该测试只验证**软件输入模式**；真实虚拟 HID 设备输入依赖驱动安装，见 `..\REPORT.md`。

## 卸载驱动 / 恢复原状

驱动卸载脚本见 `..\driver-tools\uninstall_driver.ps1`。软件输入模式不安装任何驱动、不改任何系统配置，无需"卸载"。
