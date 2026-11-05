// MSP2402_ESP32S3 驱动实现
//
// 总线层: ESP32-S3 的 FSPI 走 SPI.begin(sck, miso, mosi, -1), CS/DC 用普通 GPIO 手推。
// 面板初始化序列沿用厂商 Simple_test.ino 里针对这块屏 (QDtech MSP2402) 调过的参数,
// 不是通用 ILI9341 默认值 —— 直接套通用序列会出现偏色/残影。
//
// 写像素时 CS 从 Set 窗口一直保持到数据推完 (中间不抬 CS), 这是 ILI9341 的
// RAMWR 流式写入要求, 抬了 CS 有概率丢首字节, 表现为每行开头错一两个像素。

#include "MSP2402.h"
#include "msp_font_cn.h"
#include "msp_font_ascii.h"

#ifndef MSP2402_BL_PWM_HZ
#define MSP2402_BL_PWM_HZ 5000
#endif

MSP2402::MSP2402()
  : _spi(&SPI),
    _cs(MSP2402_PIN_CS), _dc(MSP2402_PIN_DC), _rst(MSP2402_PIN_RST), _bl(MSP2402_PIN_BL),
    _sck(MSP2402_PIN_SCK), _mosi(MSP2402_PIN_MOSI), _miso(MSP2402_PIN_MISO),
    _width(MSP2402_WIDTH), _height(MSP2402_HEIGHT),
    _xoff2(0), _yoff2(0),
    _rotation(0), _text_size(1),
    _text_fg(0xFFFF), _text_bg(0x0000), _text_opaque(false),
    _spi_hz(MSP2402_SPI_HZ), _bl_pwm(false), _in_write(false) {}

MSP2402::MSP2402(int8_t cs, int8_t dc, int8_t rst, int8_t bl,
                 int8_t sck, int8_t mosi, int8_t miso)
  : _spi(&SPI),
    _cs(cs), _dc(dc), _rst(rst), _bl(bl),
    _sck(sck), _mosi(mosi), _miso(miso),
    _width(MSP2402_WIDTH), _height(MSP2402_HEIGHT),
    _xoff2(0), _yoff2(0),
    _rotation(0), _text_size(1),
    _text_fg(0xFFFF), _text_bg(0x0000), _text_opaque(false),
    _spi_hz(MSP2402_SPI_HZ), _bl_pwm(false), _in_write(false) {}

// ---------------------------------------------------------------- 初始化

void MSP2402::Bus_Init() {
  pinMode(_cs, OUTPUT);
  pinMode(_dc, OUTPUT);
  digitalWrite(_cs, HIGH);
  digitalWrite(_dc, HIGH);
  if (_rst >= 0) {
    pinMode(_rst, OUTPUT);
    digitalWrite(_rst, HIGH);
  }
  if (_bl >= 0) {
    pinMode(_bl, OUTPUT);
    digitalWrite(_bl, HIGH);       // 先点亮, 后面可以再调 PWM
  }

  _spi->begin(_sck, _miso, _mosi, -1);
}

void MSP2402::Init(uint32_t spi_hz) {
  _spi_hz = spi_hz;
  Bus_Init();
  Reset();
  Init_Sequence();
  Set_Rotation(0);
  Fill_Screen(0x0000);
}

void MSP2402::Reset() {
  if (_rst >= 0) {
    digitalWrite(_rst, HIGH);
    delay(5);
    digitalWrite(_rst, LOW);
    delay(15);
    digitalWrite(_rst, HIGH);
    delay(15);
  } else {
    Write_Cmd(0x01);              // 没有 RST 脚时用软复位
    delay(50);
  }
}

// 厂商 MSP2402 面板实测参数
void MSP2402::Init_Sequence() {
  Write_Cmd(0x01); delay(5);        // SWRESET
  Write_Cmd(0x28);                  // DISPOFF

  Write_Cmd(0xCB);                  // POWER CONTROL A
  Write_Data8(0x39); Write_Data8(0x2C); Write_Data8(0x00); Write_Data8(0x34); Write_Data8(0x02);
  Write_Cmd(0xCF);                  // POWER CONTROL B
  Write_Data8(0x00); Write_Data8(0xC1); Write_Data8(0x30);
  Write_Cmd(0xE8);                  // DRIVER TIMING CONTROL A
  Write_Data8(0x85); Write_Data8(0x00); Write_Data8(0x78);
  Write_Cmd(0xEA);                  // DRIVER TIMING CONTROL B
  Write_Data8(0x00); Write_Data8(0x00);
  Write_Cmd(0xED);                  // POWER ON SEQUENCE CONTROL
  Write_Data8(0x64); Write_Data8(0x03); Write_Data8(0x12); Write_Data8(0x81);
  Write_Cmd(0xF7);                  // PUMP RATIO CONTROL
  Write_Data8(0x20);

  Write_Cmd(0xC0); Write_Data8(0x23);              // POWER CONTROL 1
  Write_Cmd(0xC1); Write_Data8(0x10);              // POWER CONTROL 2
  Write_Cmd(0xC5); Write_Data8(0x3E); Write_Data8(0x28);  // VCOM 1
  Write_Cmd(0xC7); Write_Data8(0x86);              // VCOM 2

  // MADCTL = 0x20 (MV, 不带 BGR)。两点都是实测结论:
  //   1) 这块红板 V1.3 的 GRAM 原生方向是 320x240 (横), 必须置 MV 才能当竖屏;
  //      用 0x48 (无 MV) 会得到旋转 90 度 + 底部断层。
  //   2) 它是 RGB 顺序而不是 BGR, 带 0x08 会让红蓝互换 (红显示成蓝)。
  Write_Cmd(0x36); Write_Data8(0xE0);              // MADCTL: MX|MY|ML|MV (RGB)
  Write_Cmd(0x3A); Write_Data8(0x55);              // 16bit/pixel

  Write_Cmd(0xB1); Write_Data8(0x00); Write_Data8(0x18);            // FRAME RATE
  Write_Cmd(0xB6); Write_Data8(0x08); Write_Data8(0x82); Write_Data8(0x27); // DISPLAY FUNCTION

  Write_Cmd(0x11); delay(120);                     // SLEEP OUT
  Write_Cmd(0x29);                                 // DISPLAY ON
}

// ---------------------------------------------------------------- 低层写

void MSP2402::Write_Cmd(uint8_t cmd) {
  _spi->beginTransaction(SPISettings(_spi_hz, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, LOW);
  _spi->writeBytes(&cmd, 1);
  digitalWrite(_cs, HIGH);
  _spi->endTransaction();
}

void MSP2402::Write_Data8(uint8_t d) {
  _spi->beginTransaction(SPISettings(_spi_hz, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, HIGH);
  _spi->writeBytes(&d, 1);
  digitalWrite(_cs, HIGH);
  _spi->endTransaction();
}

void MSP2402::Write_Data16(uint16_t d) {
  uint8_t buf[2] = { (uint8_t)(d >> 8), (uint8_t)d };
  _spi->beginTransaction(SPISettings(_spi_hz, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, HIGH);
  _spi->writeBytes(buf, 2);
  digitalWrite(_cs, HIGH);
  _spi->endTransaction();
}

void MSP2402::Write_Cmd_Data8(uint8_t cmd, uint8_t d) {
  _spi->beginTransaction(SPISettings(_spi_hz, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, LOW);
  _spi->writeBytes(&cmd, 1);
  digitalWrite(_dc, HIGH);
  _spi->writeBytes(&d, 1);
  digitalWrite(_cs, HIGH);
  _spi->endTransaction();
}

// 开一次事务, 发完窗口和 0x2C 后保持 CS 低, 交给 Push_* 推像素
void MSP2402::Set_Addr_Window(int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
  if (_in_write) End_Write();

  uint8_t cmd, buf[4];
  _spi->beginTransaction(SPISettings(_spi_hz, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);

  int16_t gx1 = x1 + _xoff2, gx2 = x2 + _xoff2;
  int16_t gy1 = y1 + _yoff2, gy2 = y2 + _yoff2;

  cmd = 0x2A;                       // CASET
  digitalWrite(_dc, LOW);
  _spi->writeBytes(&cmd, 1);
  digitalWrite(_dc, HIGH);
  buf[0] = gx1 >> 8; buf[1] = gx1; buf[2] = gx2 >> 8; buf[3] = gx2;
  _spi->writeBytes(buf, 4);

  cmd = 0x2B;                       // RASET
  digitalWrite(_dc, LOW);
  _spi->writeBytes(&cmd, 1);
  digitalWrite(_dc, HIGH);
  buf[0] = gy1 >> 8; buf[1] = gy1; buf[2] = gy2 >> 8; buf[3] = gy2;
  _spi->writeBytes(buf, 4);

  cmd = 0x2C;                       // RAMWR
  digitalWrite(_dc, LOW);
  _spi->writeBytes(&cmd, 1);
  digitalWrite(_dc, HIGH);

  _in_write = true;                 // CS 保持低, 等 Push_* 推完
}

void MSP2402::End_Write() {
  if (!_in_write) return;
  digitalWrite(_cs, HIGH);
  _spi->endTransaction();
  _in_write = false;
}

// 连续推 n 个同色像素
// 少量像素直接写, 不要为 1 个像素去填 512 字节的模板 —— Draw_Pixel / Draw_Line
// 全都走这里, 每次多填 512 字节会让画线慢一个数量级。
void MSP2402::Push_Color(uint16_t color, uint32_t count) {
  if (!count) return;
  uint8_t two[2] = { (uint8_t)(color >> 8), (uint8_t)color };
  if (count <= 8) {
    for (uint32_t i = 0; i < count; i++) _spi->writeBytes(two, 2);
    return;
  }
  uint8_t buf[512];
  for (int i = 0; i < 256; i++) {
    buf[i * 2] = two[0];
    buf[i * 2 + 1] = two[1];
  }
  while (count) {
    uint32_t n = count > 256 ? 256 : count;
    _spi->writeBytes(buf, n * 2);
    count -= n;
  }
}

void MSP2402::Push_Buffer(const uint8_t *buf, uint32_t len) {
  if (!len) return;
  _spi->writeBytes(buf, len);
}

// ---------------------------------------------------------------- 方向

// 面板 GRAM 原生是 320x240 (横)。要让 rotation=0 显示成 240x320 竖屏,
// 必须置 MV 位交换行列。四个方向的取值由 Example_27 实测得出:
//   rot0 -> 0xE0 (MX|MY|ML|MV) 240x320  竖   <- 默认, 与实物摆放方向一致
//   rot1 -> 0xA0 (MY|MV)       320x240  横
//   rot2 -> 0x20 (MV)          240x320  竖(倒)
//   rot3 -> 0x60 (MX|MV)       320x240  横(倒)
// 注意: 不带 BGR 位 (0x08), 因为实测这块屏是 RGB 顺序。
void MSP2402::Apply_Madctl() {
  uint8_t val;
  switch (_rotation & 3) {
    case 0:  val = 0xE0; break;     // MY | MX | MV   240x320 竖 (默认)
    case 1:  val = 0xA0; break;     // MY | MV        320x240 横
    case 2:  val = 0x20; break;     // MV             240x320 竖(倒)
    default: val = 0x60; break;     // MX | MV        320x240 横(倒)
  }
  Write_Cmd_Data8(0x36, val);
}

void MSP2402::Set_Offset(int16_t xoff, int16_t yoff) {
  _xoff2 = xoff;
  _yoff2 = yoff;
  Set_Addr_Window(0, 0, _width - 1, _height - 1);
  End_Write();
}

void MSP2402::Set_Rotation(uint8_t r) {
  _rotation = r & 3;
  _width = (_rotation & 1) ? MSP2402_HEIGHT : MSP2402_WIDTH;
  _height = (_rotation & 1) ? MSP2402_WIDTH : MSP2402_HEIGHT;
  Apply_Madctl();
  Set_Addr_Window(0, 0, _width - 1, _height - 1);
  End_Write();
}

// ---------------------------------------------------------------- 绘图

void MSP2402::Fill_Screen(uint16_t color) {
  Fill_Rect(0, 0, _width, _height, color);
}

void MSP2402::Fill_Rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  if (x >= _width || y >= _height) return;
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > _width) w = _width - x;
  if (y + h > _height) h = _height - y;
  if (w <= 0 || h <= 0) return;

  Set_Addr_Window(x, y, x + w - 1, y + h - 1);
  Push_Color(color, (uint32_t)w * h);
  End_Write();
}

void MSP2402::Draw_Pixel(int16_t x, int16_t y, uint16_t color) {
  if (x < 0 || y < 0 || x >= _width || y >= _height) return;
  Set_Addr_Window(x, y, x, y);
  Push_Color(color, 1);
  End_Write();
}

void MSP2402::Draw_Fast_HLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  Fill_Rect(x, y, w, 1, color);
}

void MSP2402::Draw_Fast_VLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  Fill_Rect(x, y, 1, h, color);
}

void MSP2402::Draw_Line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  int16_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int16_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int16_t err = dx + dy;
  while (true) {
    Draw_Pixel(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    int16_t e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void MSP2402::Draw_Rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  Draw_Fast_HLine(x, y, w, color);
  Draw_Fast_HLine(x, y + h - 1, w, color);
  Draw_Fast_VLine(x, y, h, color);
  Draw_Fast_VLine(x + w - 1, y, h, color);
}

void MSP2402::Draw_Circle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
  int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
  Draw_Pixel(x0, y0 + r, color);
  Draw_Pixel(x0, y0 - r, color);
  Draw_Pixel(x0 + r, y0, color);
  Draw_Pixel(x0 - r, y0, color);
  while (x < y) {
    if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
    x++; ddF_x += 2; f += ddF_x;
    Draw_Pixel(x0 + x, y0 + y, color);
    Draw_Pixel(x0 - x, y0 + y, color);
    Draw_Pixel(x0 + x, y0 - y, color);
    Draw_Pixel(x0 - x, y0 - y, color);
    Draw_Pixel(x0 + y, y0 + x, color);
    Draw_Pixel(x0 - y, y0 + x, color);
    Draw_Pixel(x0 + y, y0 - x, color);
    Draw_Pixel(x0 - y, y0 - x, color);
  }
}

void MSP2402::Fill_Circle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
  Draw_Fast_VLine(x0, y0 - r, 2 * r + 1, color);
  int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
  while (x < y) {
    if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
    x++; ddF_x += 2; f += ddF_x;
    Draw_Fast_VLine(x0 + x, y0 - y, 2 * y + 1, color);
    Draw_Fast_VLine(x0 - x, y0 - y, 2 * y + 1, color);
    Draw_Fast_VLine(x0 + y, y0 - x, 2 * x + 1, color);
    Draw_Fast_VLine(x0 - y, y0 - x, 2 * x + 1, color);
  }
}

void MSP2402::Draw_Round_Rect(int16_t x, int16_t y, int16_t w, int16_t h,
                              int16_t r, uint16_t color) {
  if (r <= 0) { Draw_Rect(x, y, w, h, color); return; }
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  Draw_Fast_HLine(x + r, y, w - 2 * r, color);
  Draw_Fast_HLine(x + r, y + h - 1, w - 2 * r, color);
  Draw_Fast_VLine(x, y + r, h - 2 * r, color);
  Draw_Fast_VLine(x + w - 1, y + r, h - 2 * r, color);

  int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, xx = 0, yy = r;
  while (xx < yy) {
    if (f >= 0) { yy--; ddF_y += 2; f += ddF_y; }
    xx++; ddF_x += 2; f += ddF_x;
    Draw_Pixel(x + r - xx, y + r - yy, color);
    Draw_Pixel(x + w - 1 - r + xx, y + r - yy, color);
    Draw_Pixel(x + r - xx, y + h - 1 - r + yy, color);
    Draw_Pixel(x + w - 1 - r + xx, y + h - 1 - r + yy, color);
    Draw_Pixel(x + r - yy, y + r - xx, color);
    Draw_Pixel(x + w - 1 - r + yy, y + r - xx, color);
    Draw_Pixel(x + r - yy, y + h - 1 - r + xx, color);
    Draw_Pixel(x + w - 1 - r + yy, y + h - 1 - r + xx, color);
  }
}

void MSP2402::Fill_Round_Rect(int16_t x, int16_t y, int16_t w, int16_t h,
                              int16_t r, uint16_t color) {
  if (r <= 0) { Fill_Rect(x, y, w, h, color); return; }
  if (r > w / 2) r = w / 2;
  if (r > h / 2) r = h / 2;
  Fill_Rect(x + r, y, w - 2 * r, h, color);
  int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, xx = 0, yy = r;
  while (xx < yy) {
    if (f >= 0) { yy--; ddF_y += 2; f += ddF_y; }
    xx++; ddF_x += 2; f += ddF_x;
    Draw_Fast_HLine(x + r - yy, y + r - xx, w - 2 * (r - yy), color);
    Draw_Fast_HLine(x + r - yy, y + h - 1 - r + xx, w - 2 * (r - yy), color);
  }
}

static inline void swap16(int16_t &a, int16_t &b) { int16_t t = a; a = b; b = t; }

void MSP2402::Draw_Triangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            int16_t x2, int16_t y2, uint16_t color) {
  Draw_Line(x0, y0, x1, y1, color);
  Draw_Line(x1, y1, x2, y2, color);
  Draw_Line(x2, y2, x0, y0, color);
}

void MSP2402::Fill_Triangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            int16_t x2, int16_t y2, uint16_t color) {
  if (y0 > y1) { swap16(y0, y1); swap16(x0, x1); }
  if (y1 > y2) { swap16(y2, y1); swap16(x2, x1); }
  if (y0 > y1) { swap16(y0, y1); swap16(x0, x1); }

  if (y0 == y2) {                                  // 退化成一个水平条
    int16_t a = x0, b = x0;
    if (x1 < a) a = x1; else if (x1 > b) b = x1;
    if (x2 < a) a = x2; else if (x2 > b) b = x2;
    Draw_Fast_HLine(a, y0, b - a + 1, color);
    return;
  }

  int32_t sa = 0, sb = 0;
  int16_t dx01 = x1 - x0, dy01 = y1 - y0, dx02 = x2 - x0, dy02 = y2 - y0;
  int32_t dx12 = x2 - x1, dy12 = y2 - y1;
  int16_t last = (y1 == y2) ? y1 : y1 - 1;
  int16_t y = y0;

  for (; y <= last; y++) {
    int16_t a = x0 + (dy01 ? (int16_t)(sa / dy01) : 0);
    int16_t b = x0 + (dy02 ? (int16_t)(sb / dy02) : 0);
    sa += dx01;
    sb += dx02;
    if (a > b) swap16(a, b);
    Draw_Fast_HLine(a, y, b - a + 1, color);
  }

  sa = (int32_t)dx12 * (y - y1);
  sb = (int32_t)dx02 * (y - y1);
  for (; y <= y2; y++) {
    int16_t a = x1 + (dy12 ? (int16_t)(sa / dy12) : 0);
    int16_t b = x0 + (dy02 ? (int16_t)(sb / dy02) : 0);
    sa += dx12;
    sb += dx02;
    if (a > b) swap16(a, b);
    Draw_Fast_HLine(a, y, b - a + 1, color);
  }
}

// ---------------------------------------------------------------- 位图

void MSP2402::Draw_Bitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                          const uint16_t *data, bool from_flash) {
  if (x >= _width || y >= _height || !data) return;
  uint8_t chunk[512];
  for (int16_t row = 0; row < h; row++) {
    int16_t py = y + row;
    if (py < 0 || py >= _height) continue;
    const uint16_t *line = data + (int32_t)row * w;
    int16_t x0 = x < 0 ? 0 : x;
    int16_t x1 = x + w; if (x1 > _width) x1 = _width;
    if (x1 <= x0) continue;

    Set_Addr_Window(x0, py, x1 - 1, py);
    const uint16_t *p = line + (x0 - x);
    int16_t n = x1 - x0;
    while (n > 0) {
      int16_t k = n > 256 ? 256 : n;
      for (int16_t i = 0; i < k; i++) {
        uint16_t c = from_flash ? pgm_read_word(p + i) : p[i];
        chunk[i * 2] = (uint8_t)(c >> 8);
        chunk[i * 2 + 1] = (uint8_t)c;
      }
      Push_Buffer(chunk, (uint32_t)k * 2);
      p += k;
      n -= k;
    }
    End_Write();
  }
}

void MSP2402::Draw_Bitmap_1bpp(int16_t x, int16_t y, int16_t w, int16_t h,
                               const uint8_t *data, uint16_t fg, uint16_t bg) {
  if (x >= _width || y >= _height || !data) return;
  int16_t stride = (w + 7) / 8;
  uint8_t chunk[512];
  for (int16_t row = 0; row < h; row++) {
    int16_t py = y + row;
    if (py < 0 || py >= _height) continue;
    const uint8_t *line = data + (int32_t)row * stride;
    int16_t x0 = x < 0 ? 0 : x;
    int16_t x1 = x + w; if (x1 > _width) x1 = _width;
    if (x1 <= x0) continue;

    Set_Addr_Window(x0, py, x1 - 1, py);
    int16_t n = x1 - x0, done = 0;
    while (done < n) {
      int16_t k = (n - done) > 256 ? 256 : (n - done);
      for (int16_t i = 0; i < k; i++) {
        int16_t gx = (x0 - x) + done + i;
        uint8_t bit = (line[gx >> 3] >> (7 - (gx & 7))) & 1;
        uint16_t c = bit ? fg : bg;
        chunk[i * 2] = (uint8_t)(c >> 8);
        chunk[i * 2 + 1] = (uint8_t)c;
      }
      Push_Buffer(chunk, (uint32_t)k * 2);
      done += k;
    }
    End_Write();
  }
}

// ---------------------------------------------------------------- 文字

// 中文字库 32 字节/字, 每行 2 字节, MSB 在左。透明模式逐段输出, 保证横向不错位。
// 段的起点可能落在屏幕左侧外 (跑马灯), 所以裁剪要带 chunk 内偏移, 不能只改长度。
void MSP2402::Draw_Char_CN(int16_t x, int16_t y, const uint8_t *g, uint16_t fg, uint16_t bg) {
  uint8_t s = _text_size;
  uint8_t chunk[16 * 4 * 2];
  for (int r = 0; r < 16; r++) {
    uint8_t hi = g[r * 2], lo = g[r * 2 + 1];
    for (int sr = 0; sr < s; sr++) {
      int16_t py = y + r * s + sr;
      if (py < 0 || py >= _height) continue;

      int start = -1;
      for (int c = 0; c <= 16; c++) {
        bool draw = false;
        if (c < 16) {
          uint8_t bit = (c < 8) ? ((hi >> (7 - c)) & 1) : ((lo >> (7 - (c - 8))) & 1);
          draw = _text_opaque || bit;
        }
        if (draw && start < 0) start = c;
        if (draw || start < 0) continue;

        // [start, c) 这一段要画
        int16_t px0 = x + start * s;
        int16_t cnt = (c - start) * s;
        int16_t skip = 0;                       // 左侧出界的那几个像素
        if (px0 < 0) { skip = -px0; if (skip >= cnt) { start = -1; continue; } }
        int16_t vis = cnt - skip;
        if (px0 + skip + vis > _width) vis = _width - (px0 + skip);
        if (vis > 0) {
          int16_t ox = (px0 < 0) ? 0 : px0;
          for (int i = 0; i < vis; i++) {
            int col = start + (skip + i) / s;
            uint8_t bit = (col < 8) ? ((hi >> (7 - col)) & 1)
                                    : ((lo >> (7 - (col - 8))) & 1);
            uint16_t cc = bit ? fg : bg;
            chunk[i * 2] = (uint8_t)(cc >> 8);
            chunk[i * 2 + 1] = (uint8_t)cc;
          }
          Set_Addr_Window(ox, py, ox + vis - 1, py);
          Push_Buffer(chunk, (uint32_t)vis * 2);
          End_Write();
        }
        start = -1;
      }
    }
  }
}

// ASCII 字库 5 字节/字符, 每字节一列, bit0 在最上 (7 行可见)
void MSP2402::Draw_Char_ASCII(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg) {
  if (c < MSP_FONT_ASCII_FIRST || c > MSP_FONT_ASCII_LAST) c = '?';
  const uint8_t *g = msp_font_ascii[(uint8_t)c - MSP_FONT_ASCII_FIRST];
  uint8_t s = _text_size;
  const int cols = 6;                 // 第 6 列留空当字间距
  uint8_t chunk[6 * 4 * 2];
  for (int r = 0; r < 8; r++) {
    for (int sr = 0; sr < s; sr++) {
      int16_t py = y + r * s + sr;
      if (py < 0 || py >= _height) continue;

      int start = -1;
      for (int col = 0; col <= cols; col++) {
        bool draw = false;
        if (col < cols) {
          uint8_t bit = (col < 5) ? ((g[col] >> r) & 1) : 0;
          draw = _text_opaque || bit;
        }
        if (draw && start < 0) start = col;
        if (draw || start < 0) continue;

        int16_t px0 = x + start * s;
        int16_t cnt = (col - start) * s;
        int16_t skip = 0;
        if (px0 < 0) { skip = -px0; if (skip >= cnt) { start = -1; continue; } }
        int16_t vis = cnt - skip;
        if (px0 + skip + vis > _width) vis = _width - (px0 + skip);
        if (vis > 0) {
          int16_t ox = (px0 < 0) ? 0 : px0;
          for (int i = 0; i < vis; i++) {
            int cc2 = start + (skip + i) / s;
            uint8_t bit = (cc2 < 5) ? ((g[cc2] >> r) & 1) : 0;
            uint16_t cc = bit ? fg : bg;
            chunk[i * 2] = (uint8_t)(cc >> 8);
            chunk[i * 2 + 1] = (uint8_t)cc;
          }
          Set_Addr_Window(ox, py, ox + vis - 1, py);
          Push_Buffer(chunk, (uint32_t)vis * 2);
          End_Write();
        }
        start = -1;
      }
    }
  }
}

uint16_t MSP2402::Get_Font_Count() { return MSP_FONT_CN_COUNT; }

bool MSP2402::Has_Glyph(const char *utf8_char) {
  if (!utf8_char) return false;
  const uint8_t *p = (const uint8_t *)utf8_char;
  uint32_t cp = 0;
  if ((*p & 0xE0) == 0xC0 && p[1]) {
    cp = ((*p & 0x1Fu) << 6) | (p[1] & 0x3Fu);
  } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
    cp = ((*p & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
  } else {
    return false;
  }
  for (uint16_t i = 0; i < MSP_FONT_CN_COUNT; i++) {
    if (msp_font_cn[i].unicode == cp) return true;
  }
  return false;
}

int16_t MSP2402::Get_String_Width(const char *str) const {
  if (!str) return 0;
  int16_t w = 0;
  const uint8_t *p = (const uint8_t *)str;
  while (*p) {
    if (*p < 0x80) { w += 6 * _text_size; p++; }
    else if ((*p & 0xE0) == 0xC0 && p[1]) { w += 16; p += 2; }
    else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { w += 16; p += 3; }
    else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { w += 16; p += 4; }
    else { p++; }
  }
  return w;
}

void MSP2402::Print_String(const char *str, int16_t x, int16_t y) {
  Print_String(str, x, y, _text_fg, _text_bg);
}

void MSP2402::Print_String(const char *str, int16_t x, int16_t y,
                           uint16_t fg, uint16_t bg) {
  if (!str) return;
  _text_fg = fg;
  _text_bg = bg;
  int16_t cx = x;
  if (x == MSP_CENTER) cx = (_width - Get_String_Width(str)) / 2;
  else if (x == MSP_RIGHT) cx = _width - Get_String_Width(str);

  const uint8_t *p = (const uint8_t *)str;
  while (*p) {
    if (*p < 0x80) {
      Draw_Char_ASCII(cx, y, (char)*p, fg, bg);
      cx += 6 * _text_size;
      p++;
      continue;
    }
    uint32_t cp = 0;
    int len = 1;
    if ((*p & 0xE0) == 0xC0 && p[1]) {
      cp = ((*p & 0x1Fu) << 6) | (p[1] & 0x3Fu); len = 2;
    } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
      cp = ((*p & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); len = 3;
    } else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
      cp = ((*p & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
           ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu); len = 4;
    }

    bool found = false;
    for (uint16_t i = 0; i < MSP_FONT_CN_COUNT; i++) {
      if (msp_font_cn[i].unicode == cp) {
        Draw_Char_CN(cx, y, msp_font_cn[i].data, fg, bg);
        found = true;
        break;
      }
    }
    if (!found) {                    // 缺字画方框, 不漏字也不错位
      Fill_Rect(cx + 2, y + 2, 12, 12, fg);
      if (_text_opaque) Fill_Rect(cx + 5, y + 5, 6, 6, bg);
    }
    cx += 16;
    p += len;
  }
}

// ---------------------------------------------------------------- 面板控制

void MSP2402::Backlight(bool on) {
  if (_bl < 0) return;
  if (_bl_pwm) { ledcDetach(_bl); _bl_pwm = false; pinMode(_bl, OUTPUT); }
  digitalWrite(_bl, on ? HIGH : LOW);
}

void MSP2402::Set_Backlight(uint8_t percent) {
  if (_bl < 0) return;
  if (percent > 100) percent = 100;
  if (!_bl_pwm) _bl_pwm = ledcAttach(_bl, MSP2402_BL_PWM_HZ, 10);
  if (!_bl_pwm) {                   // LEDC 起不来就退化成开关
    digitalWrite(_bl, percent > 0 ? HIGH : LOW);
    return;
  }
  ledcWrite(_bl, (uint32_t)percent * 1023 / 100);
}

void MSP2402::Invert(bool inv) {
  Write_Cmd(inv ? 0x21 : 0x20);
}

// ILI9341 硬件竖直滚动:
//   0x33 VSCRDEF  3 个 16 位参数: TFA / VSA / BFA
//   0x37 VSCRSADD 1 个 16 位参数: 起始行
// 只对竖屏 (240x320) 有意义, 面板会自己循环, CPU 不参与。
void MSP2402::Vert_Scroll(int16_t top, int16_t lines, int16_t offset) {
  int16_t bottom = MSP2402_HEIGHT - top - lines;
  if (bottom < 0) bottom = 0;
  Write_Cmd(0x33);
  Write_Data16(top);
  Write_Data16(lines);
  Write_Data16(bottom);
  Write_Cmd(0x37);
  Write_Data16(offset);
}

void MSP2402::Sleep(bool on) {
  Write_Cmd(on ? 0x10 : 0x11);
  delay(120);
}

uint16_t MSP2402::Read_ID() {
  uint8_t cmd = 0xD3, buf[4] = { 0, 0, 0, 0 };
  _spi->beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, LOW);
  _spi->writeBytes(&cmd, 1);
  digitalWrite(_dc, HIGH);
  for (int i = 0; i < 4; i++) buf[i] = _spi->transfer(0xFF);
  digitalWrite(_cs, HIGH);
  _spi->endTransaction();
  return ((uint16_t)buf[2] << 8) | buf[3];
}

uint16_t MSP2402::Read_Pixel(int16_t x, int16_t y) {
  if (x < 0 || y < 0 || x >= _width || y >= _height) return 0;
  Set_Addr_Window(x, y, x, y);
  End_Write();

  uint8_t cmd = 0x2E;                        // RAMRD
  _spi->beginTransaction(SPISettings(8000000, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, LOW);
  _spi->writeBytes(&cmd, 1);
  digitalWrite(_dc, HIGH);
  _spi->transfer(0xFF);                      // dummy
  uint8_t hi = _spi->transfer(0xFF);
  uint8_t lo = _spi->transfer(0xFF);
  digitalWrite(_cs, HIGH);
  _spi->endTransaction();
  return ((uint16_t)hi << 8) | lo;
}
