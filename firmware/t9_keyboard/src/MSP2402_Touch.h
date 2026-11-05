// MSP2402_Touch —— 模块上 XPT2046 电阻触摸控制器驱动 (ESP32-S3)
//
// ★ 本工程的实际接线是"触摸与 LCD 共用 SPI 总线" ★
//   T_CLK  -> GPIO12 (= LCD SCK)
//   T_DIN  -> GPIO11 (= LCD MOSI)
//   T_DO   -> GPIO13 (= LCD MISO)
//   T_CS   -> GPIO6
//   T_IRQ  -> GPIO5
//
// 既然 CLK/DIN/DO 三个脚和屏共用, 触摸就必须走硬件 FSPI 并与刷屏共用同一个
// SPIClass —— 用软件位操作会把 GPIO11/12 从 SPI 外设手里抢走, 之后刷屏全黑。
// 共用同一条总线时靠 CS 区分: T_CS 拉低期间是触摸, LCD 的 CS 拉低期间是屏。
//
// XPT2046 的 DOUT 和 ILI9341 的 SDO 都在 CS 抬高时高阻, 并到同一个 MISO 不会打架。
//
// 如果你的板子是触摸单独 4 线接的, 把 MSP_TOUCH_SHARED_BUS 设成 0,
// 然后按下面的 MSP_TOUCH_PIN_* 单独接。

#ifndef _MSP2402_TOUCH_H_
#define _MSP2402_TOUCH_H_

#include <Arduino.h>
#include <SPI.h>
#include "MSP2402.h"

// 1 = 触摸与 LCD 共用 FSPI 总线
// 0 = 触摸用独立 GPIO 做软件 SPI
//
// 本工程实测用 0: T_DO 单独接在 GPIO7, 不和 LCD 的 SDO 共用一根 MISO。
// 共用总线时 XPT2046 的 2MHz 时序会和 LCD 的 40MHz 残留配置打架,
// 读出来恒为 0/4095; 独立引脚 + 软件 SPI 一次就通。
#ifndef MSP_TOUCH_SHARED_BUS
#define MSP_TOUCH_SHARED_BUS 0
#endif

// ---- 共用总线时只需要这两根 ----
#ifndef MSP_TOUCH_PIN_CS
#define MSP_TOUCH_PIN_CS    6
#endif
#ifndef MSP_TOUCH_PIN_IRQ
#define MSP_TOUCH_PIN_IRQ   5
#endif

// ---- 独立 4 线 ----
// 触摸完全独立，不和 LCD 共用任何信号线。
// 原因: 软件 SPI 必须 pinMode(CLK/DIN, OUTPUT)，如果这两个脚是 LCD 的
// SCK/MOSI，会立刻把 FSPI 外设抢走，屏幕直接黑。
#ifndef MSP_TOUCH_PIN_CLK
#define MSP_TOUCH_PIN_CLK   4
#endif
#ifndef MSP_TOUCH_PIN_DIN
#define MSP_TOUCH_PIN_DIN   15
#endif
// T_DO 单独接 GPIO7, 不与 LCD 的 SDO(GPIO13) 共用
#ifndef MSP_TOUCH_PIN_DOUT
#define MSP_TOUCH_PIN_DOUT  7
#endif

// 触摸膜的 X/Y 是否需要交换。
// 这块面板的 GRAM 原生是 320x240(横), 驱动用 MV 位把它转成 240x320 竖屏,
// 行列一交换, 触摸膜的物理轴向也跟着变了 —— 实测必须置 1,
// 否则手指水平划动会在屏幕上画出竖线。
#ifndef MSP_TOUCH_SWAP_XY
#define MSP_TOUCH_SWAP_XY 1
#endif

// XPT2046 时钟上限约 2MHz, 共用 40MHz 的 LCD 总线时必须每次降频
#ifndef MSP_TOUCH_SPI_HZ
#define MSP_TOUCH_SPI_HZ    2000000UL
#endif

// 面板物理分辨率 (竖屏), 与显示方向无关
#define MSP_TOUCH_PANEL_W  240
#define MSP_TOUCH_PANEL_H  320

class MSP2402_Touch {
public:
  MSP2402_Touch();
#if MSP_TOUCH_SHARED_BUS
  // 共用总线: 传 LCD 正在用的那条 SPIClass
  explicit MSP2402_Touch(SPIClass *bus, int8_t tcs = MSP_TOUCH_PIN_CS,
                         int8_t tirq = MSP_TOUCH_PIN_IRQ);
#else
  MSP2402_Touch(int8_t tcs, int8_t tclk, int8_t tdin, int8_t tdout, int8_t tirq);
#endif

  void Init(uint16_t width, uint16_t height);
  // 共用总线时必须绑定到 LCD 正在用的那条 SPIClass。
  // 默认构造后忘了绑会连到全局 SPI —— 大多数例程里其实就是同一条,
  // 但如果你给 LCD 传了自定义 SPIClass, 这里就必须显式绑一次。
  void Set_SPI_Bus(SPIClass *bus) { if (bus) _bus = bus; }
  // 必须和 LCD 的 Set_Rotation 设成同一个值, 触摸坐标才对得上
  void Set_Rotation(uint8_t r);
  void Set_LCD_Size(uint16_t w, uint16_t h);

  bool Read(int16_t *x, int16_t *y);          // 读一次屏幕坐标
  bool Read_Stable(int16_t *x, int16_t *y);   // 连读两次比对, 防抖
  bool Read_Raw(uint16_t *rx, uint16_t *ry);  // 原始 ADC 值 (校准用)
  bool Is_Pressed() const;                    // 只看 IRQ, 不采样

  // 校准: 屏幕 = raw * fac / 10000 + offset
  // 运行时切换轴交换 (校准程序里可以自动探测)
  void Set_Swap_XY(bool swap) { _swap_xy = swap; }
  bool Get_Swap_XY() const { return _swap_xy; }

  void Set_Calibration(int32_t xfac, int32_t xoff, int32_t yfac, int32_t yoff);
  void Get_Calibration(int32_t *xfac, int32_t *xoff, int32_t *yfac, int32_t *yoff) const;
  void Compute_Calibration(uint16_t raw_x0, uint16_t raw_y0, int16_t scr_x0, int16_t scr_y0,
                           uint16_t raw_x1, uint16_t raw_y1, int16_t scr_x1, int16_t scr_y1);

  uint16_t raw_x, raw_y;          // 最近一次的原始值

private:
  uint16_t Read_ADC(uint8_t cmd);
  uint16_t Read_XY_Sample(uint8_t cmd);
#if !MSP_TOUCH_SHARED_BUS
  void Write_Byte(uint8_t d);
  uint8_t Read_Byte();
#endif

  SPIClass *_bus;
  int8_t _cs, _irq;
#if !MSP_TOUCH_SHARED_BUS
  int8_t _clk, _din, _dout;
#endif
  uint16_t _wid, _heig;
  uint8_t _rotation;
  bool _swap_xy;
  int32_t _xfac, _xoff, _yfac, _yoff;
};

#endif // _MSP2402_TOUCH_H_
