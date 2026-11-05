// MSP2402_Touch 实现 —— XPT2046
//
// 两条路径:
//   MSP_TOUCH_SHARED_BUS=1 (默认, 本工程实际接线): 走 LCD 那套 FSPI, 只换 CS + 降频
//   MSP_TOUCH_SHARED_BUS=0: GPIO 软件位操作
//
// XPT2046 读时序 (数据手册 Fig.12):
//   发 1 字节命令 (MSB 先) -> 等 ~6us 转换 -> 读 2 字节 -> 高 12 位有效
// 共用 40MHz 总线时, 每个字节后芯片会自动拉低时钟, 但转换时间仍要显式等,
// 否则读到的是上一个通道的值。

#include "MSP2402_Touch.h"

// 这块屏的实测校准值, 由 Example_38 的九点采样拟合得出:
//   scrx = 0.0652 * raw + (-13)
//   scry = 0.0866 * raw + (-21)
//
// 巧合的是与厂商 cali_para.h 给的 663/-13/894/-30 很接近, 说明采样准确。
// 注意 raw 的轴向: 触摸膜的 X/Y 与屏幕 X/Y 是交换的 (见 MSP_TOUCH_SWAP_XY),
// 这里已经是交换之后的参数。
#ifndef MSP_TOUCH_DEF_XFAC
#define MSP_TOUCH_DEF_XFAC   652
#endif
#ifndef MSP_TOUCH_DEF_XOFF
#define MSP_TOUCH_DEF_XOFF   (-13)
#endif
#ifndef MSP_TOUCH_DEF_YFAC
#define MSP_TOUCH_DEF_YFAC   866
#endif
#ifndef MSP_TOUCH_DEF_YOFF
#define MSP_TOUCH_DEF_YOFF   (-21)
#endif

// XPT2046 命令字: 12bit, 差分, 分别是 X / Y 位置
#define XPT_CMD_X   0xD0
#define XPT_CMD_Y   0x90

MSP2402_Touch::MSP2402_Touch()
  : _bus(&SPI), _cs(MSP_TOUCH_PIN_CS), _irq(MSP_TOUCH_PIN_IRQ),
#if !MSP_TOUCH_SHARED_BUS
    _clk(MSP_TOUCH_PIN_CLK), _din(MSP_TOUCH_PIN_DIN), _dout(MSP_TOUCH_PIN_DOUT),
#endif
    _wid(240), _heig(320), _rotation(0),
    _swap_xy(MSP_TOUCH_SWAP_XY),
    _xfac(MSP_TOUCH_DEF_XFAC), _xoff(MSP_TOUCH_DEF_XOFF),
    _yfac(MSP_TOUCH_DEF_YFAC), _yoff(MSP_TOUCH_DEF_YOFF),
    raw_x(0), raw_y(0) {}

#if MSP_TOUCH_SHARED_BUS
MSP2402_Touch::MSP2402_Touch(SPIClass *bus, int8_t tcs, int8_t tirq)
  : _bus(bus ? bus : &SPI), _cs(tcs), _irq(tirq),
    _wid(240), _heig(320), _rotation(0),
    _swap_xy(MSP_TOUCH_SWAP_XY),
    _xfac(MSP_TOUCH_DEF_XFAC), _xoff(MSP_TOUCH_DEF_XOFF),
    _yfac(MSP_TOUCH_DEF_YFAC), _yoff(MSP_TOUCH_DEF_YOFF),
    raw_x(0), raw_y(0) {}
#else
MSP2402_Touch::MSP2402_Touch(int8_t tcs, int8_t tclk, int8_t tdin, int8_t tdout, int8_t tirq)
  : _bus(&SPI), _cs(tcs), _irq(tirq), _clk(tclk), _din(tdin), _dout(tdout),
    _wid(240), _heig(320), _rotation(0),
    _swap_xy(MSP_TOUCH_SWAP_XY),
    _xfac(MSP_TOUCH_DEF_XFAC), _xoff(MSP_TOUCH_DEF_XOFF),
    _yfac(MSP_TOUCH_DEF_YFAC), _yoff(MSP_TOUCH_DEF_YOFF),
    raw_x(0), raw_y(0) {}
#endif

void MSP2402_Touch::Init(uint16_t width, uint16_t height) {
#if MSP_TOUCH_SHARED_BUS
  // 总线已经由 MSP2402::Init() 的 SPI.begin() 起好了, 这里只管 CS/IRQ
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
#else
  pinMode(_cs, OUTPUT);
  pinMode(_clk, OUTPUT);
  pinMode(_din, OUTPUT);
  digitalWrite(_cs, HIGH);
  digitalWrite(_clk, HIGH);
  digitalWrite(_din, HIGH);
  pinMode(_dout, INPUT);
#endif

  // T_IRQ 是 XPT2046 的 PENIRQ, 开漏低有效, 模块上有 10K 上拉到 3V3。
  // 注意: 上电后第一次触摸之前它可能是低电平(芯片内部状态未就绪),
  // 所以这里先做一次空采样把芯片唤醒, 之后 PENIRQ 才会正确反映"是否按下"。
  if (_irq >= 0) {
    pinMode(_irq, INPUT_PULLUP);
    Read_ADC(XPT_CMD_X);
    Read_ADC(XPT_CMD_Y);
    delay(2);
  }

  _wid = width;
  _heig = height;
}

void MSP2402_Touch::Set_Rotation(uint8_t r) { _rotation = r & 3; }

void MSP2402_Touch::Set_LCD_Size(uint16_t w, uint16_t h) { _wid = w; _heig = h; }

#if !MSP_TOUCH_SHARED_BUS
void MSP2402_Touch::Write_Byte(uint8_t d) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(_din, (d >> i) & 1);
    digitalWrite(_clk, LOW);
    digitalWrite(_clk, HIGH);
  }
}

uint8_t MSP2402_Touch::Read_Byte() {
  uint8_t v = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(_clk, LOW);
    digitalWrite(_clk, HIGH);
    v <<= 1;
    if (digitalRead(_dout)) v |= 1;
  }
  return v;
}
#endif

uint16_t MSP2402_Touch::Read_ADC(uint8_t cmd) {
#if MSP_TOUCH_SHARED_BUS
  // 注意: XPT2046 在 12 位传输结束后还需要**第 25 个时钟沿**才会把
  // PENIRQ 拉回高电平。只发 24 个时钟(3 字节)会让 IRQ 永久卡在低电平,
  // 表现为 "没碰也一直检测到触摸"、坐标乱跳。所以这里多发 1 个字节的时钟。
  uint8_t tx[4] = { cmd, 0x00, 0x00, 0x00 };
  uint8_t rx[4] = { 0, 0, 0, 0 };

  _bus->beginTransaction(SPISettings(MSP_TOUCH_SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  _bus->writeBytes(tx, 1);
  delayMicroseconds(6);              // 转换时间, 少等就读到上一通道
  _bus->transferBytes(tx + 1, rx + 1, 3);   // 2 字节数据 + 1 字节复位时钟
  digitalWrite(_cs, HIGH);
  _bus->endTransaction();

  return (((uint16_t)rx[1] << 8) | rx[2]) >> 4;   // 12 位有效在高位
#else
  digitalWrite(_cs, LOW);
  digitalWrite(_clk, LOW);
  digitalWrite(_din, LOW);

  Write_Byte(cmd);
  delayMicroseconds(6);

  digitalWrite(_clk, LOW);
  delayMicroseconds(1);
  digitalWrite(_clk, HIGH);
  digitalWrite(_clk, LOW);

  uint16_t hi = Read_Byte();
  uint16_t lo = Read_Byte();
  // 多打 8 个时钟让 PENIRQ 复位
  for (int i = 0; i < 8; i++) {
    digitalWrite(_clk, LOW);
    digitalWrite(_clk, HIGH);
  }
  digitalWrite(_cs, HIGH);

  return ((hi << 8) | lo) >> 4;      // 12 位有效在高位, 移 3 会让坐标翻倍后削顶
#endif
}

// 连读 5 次, 去掉最大最小取平均 —— 和厂商 TP_Read_XY 同样的算法
uint16_t MSP2402_Touch::Read_XY_Sample(uint8_t cmd) {
  const int N = 5;
  uint16_t buf[N];
  for (int i = 0; i < N; i++) buf[i] = Read_ADC(cmd);
  for (int i = 0; i < N - 1; i++) {
    for (int j = i + 1; j < N; j++) {
      if (buf[i] > buf[j]) { uint16_t t = buf[i]; buf[i] = buf[j]; buf[j] = t; }
    }
  }
  uint32_t sum = 0;
  for (int i = 1; i < N - 1; i++) sum += buf[i];
  return sum / (N - 2);
}

bool MSP2402_Touch::Read_Raw(uint16_t *rx, uint16_t *ry) {
  if (_irq >= 0 && digitalRead(_irq) != LOW) return false;   // PENIRQ 低有效
  raw_x = Read_XY_Sample(XPT_CMD_X);
  raw_y = Read_XY_Sample(XPT_CMD_Y);
  // 对外统一给出"已按屏幕轴向校正"的原始值, 校准程序就不用关心轴序
  if (_swap_xy) {
    uint16_t t = raw_x; raw_x = raw_y; raw_y = t;
  }
  if (rx) *rx = raw_x;
  if (ry) *ry = raw_y;
  return true;
}

bool MSP2402_Touch::Read(int16_t *x, int16_t *y) {
  uint16_t rx = 0, ry = 0;
  if (!Read_Raw(&rx, &ry)) return false;

  // XPT2046 悬空/轻触时会给出 0 或满量程, 直接丢
  if (rx == 0 || ry == 0 || rx >= 4095 || ry >= 4095) return false;

  // 注意: Read_Raw() 内部已经按 _swap_xy 交换过 axios 了,
  // 这里绝对不能再交换一次 —— 两次交换等于没交换, 表现为"横画变竖画"。
  // 所以下面直接用 rx/ry 做换算。

  // 先换算到面板自然方向 (竖屏 240x320)
  int32_t px = (int32_t)rx * _xfac / 10000 + _xoff;
  int32_t py = (int32_t)ry * _yfac / 10000 + _yoff;
  if (px < 0) px = 0;
  if (py < 0) py = 0;
  if (px > MSP_TOUCH_PANEL_W - 1) px = MSP_TOUCH_PANEL_W - 1;
  if (py > MSP_TOUCH_PANEL_H - 1) py = MSP_TOUCH_PANEL_H - 1;

  // 面板自然方向 -> 当前显示方向。
  // 跨轴那一项必须用面板自身的 240/320, 不能用旋转后的屏幕宽高,
  // 否则 rot=1/3 会算到屏幕外 (厂商原库拿 wid/heig 直接代, 有这个问题)。
  int32_t tx = px, ty = py;
  switch (_rotation & 3) {
    case 0:  tx = px;                            ty = py;                            break;
    case 1:  tx = py;                            ty = (MSP_TOUCH_PANEL_W - 1) - px;   break;
    case 2:  tx = (MSP_TOUCH_PANEL_W - 1) - px;  ty = (MSP_TOUCH_PANEL_H - 1) - py;  break;
    default: tx = (MSP_TOUCH_PANEL_H - 1) - py;  ty = px;                            break;
  }

  if (tx < 0) tx = 0;
  if (ty < 0) ty = 0;
  if (tx >= _wid) tx = _wid - 1;
  if (ty >= _heig) ty = _heig - 1;
  if (x) *x = (int16_t)tx;
  if (y) *y = (int16_t)ty;
  return true;
}

bool MSP2402_Touch::Read_Stable(int16_t *x, int16_t *y) {
  int16_t x1, y1, x2, y2;
  if (!Read(&x1, &y1)) return false;
  if (!Read(&x2, &y2)) return false;
  const int16_t RANGE = 8;
  if (abs(x1 - x2) > RANGE || abs(y1 - y2) > RANGE) return false;
  if (x) *x = (x1 + x2) / 2;
  if (y) *y = (y1 + y2) / 2;
  return true;
}

bool MSP2402_Touch::Is_Pressed() const {
  if (_irq < 0) return true;                 // 没接 IRQ 只能盲目采样
  return digitalRead(_irq) == LOW;
}

void MSP2402_Touch::Set_Calibration(int32_t xfac, int32_t xoff, int32_t yfac, int32_t yoff) {
  _xfac = xfac; _xoff = xoff; _yfac = yfac; _yoff = yoff;
}

void MSP2402_Touch::Get_Calibration(int32_t *xfac, int32_t *xoff,
                                    int32_t *yfac, int32_t *yoff) const {
  if (xfac) *xfac = _xfac;
  if (xoff) *xoff = _xoff;
  if (yfac) *yfac = _yfac;
  if (yoff) *yoff = _yoff;
}

// 两点校准: fac = (屏幕差 * 10000) / 原始差, offset = 屏幕0 - raw0*fac/10000
void MSP2402_Touch::Compute_Calibration(uint16_t raw_x0, uint16_t raw_y0,
                                        int16_t scr_x0, int16_t scr_y0,
                                        uint16_t raw_x1, uint16_t raw_y1,
                                        int16_t scr_x1, int16_t scr_y1) {
  int32_t drx = (int32_t)raw_x1 - (int32_t)raw_x0;
  int32_t dry = (int32_t)raw_y1 - (int32_t)raw_y0;
  int32_t dsx = (int32_t)scr_x1 - (int32_t)scr_x0;
  int32_t dsy = (int32_t)scr_y1 - (int32_t)scr_y0;
  if (drx != 0) _xfac = dsx * 10000 / drx;
  if (dry != 0) _yfac = dsy * 10000 / dry;
  _xoff = (int32_t)scr_x0 - (int32_t)raw_x0 * _xfac / 10000;
  _yoff = (int32_t)scr_y0 - (int32_t)raw_y0 * _yfac / 10000;
}
