// MSP2402_ESP32S3 —— 2.4 寸 SPI 模块 (ILI9341 240x320 + XPT2046 触摸) ESP32-S3 原生驱动
//
// 为什么不是直接用厂商的 LCDWIKI_SPI:
//   厂商库里 CS_ACTIVE / CD_COMMAND / write8() 这些宏只在 __AVR__ 和
//   ARDUINO_ARCH_ESP8266 分支里定义 (见 mcu_spi_magic.h),ESP32 上全是未定义标识符;
//   构造函数里还调用了 ESP32 上不存在的 SPI.setClockDivider(SPI_CLOCK_DIV4)。
//   所以 ESP32-S3 需要重写总线层,面板初始化序列和取模方式保持和厂商一致,
//   这样画面效果和 UNO/MEGA 例程对得上。
//
// 引脚可在 examples 里用构造函数覆盖,默认值见 MSP2402_PIN_*。

#ifndef _MSP2402_H_
#define _MSP2402_H_

#include <Arduino.h>
#include <SPI.h>

// ---------------- 默认接线 (ESP32-S3 DevKit N16R8) ----------------
// 这组是实测在用的接线 (YG 的板子), 不是理论推荐值。
// 注意 LCD 和触摸共用 SCK/MOSI —— 见 MSP2402_Touch 的 SHARED_BUS 开关。
// 避开 0/3/45/46 这些 strapping 脚, 也避开 19/20 (USB) 和 43/44 (UART0)
#ifndef MSP2402_PIN_SCK
#define MSP2402_PIN_SCK   12
#endif
#ifndef MSP2402_PIN_MOSI
#define MSP2402_PIN_MOSI  11
#endif
#ifndef MSP2402_PIN_MISO
#define MSP2402_PIN_MISO  13
#endif
#ifndef MSP2402_PIN_CS
#define MSP2402_PIN_CS    10
#endif
#ifndef MSP2402_PIN_DC
#define MSP2402_PIN_DC     9
#endif
#ifndef MSP2402_PIN_RST
#define MSP2402_PIN_RST    8
#endif
// 背光直接并到 3.3V, 不做软件控制 (想调亮度就把 LED 挪到一个 GPIO 再改这里)
#ifndef MSP2402_PIN_BL
#define MSP2402_PIN_BL    (-1)
#endif

#define MSP2402_WIDTH   240
#define MSP2402_HEIGHT  320
#define MSP2402_SPI_HZ  40000000UL   // ILI9341 官方上限 40MHz

// 文字对齐
#define MSP_LEFT    0
#define MSP_CENTER  9998
#define MSP_RIGHT   9999

class MSP2402 {
public:
  // 用默认引脚
  MSP2402();
  // 自定义引脚; bl < 0 表示不做背光控制
  MSP2402(int8_t cs, int8_t dc, int8_t rst, int8_t bl,
          int8_t sck = MSP2402_PIN_SCK, int8_t mosi = MSP2402_PIN_MOSI,
          int8_t miso = MSP2402_PIN_MISO);

  // 绑定自定义 SPI 总线 (默认用全局 SPI / FSPI)
  void Set_SPI_Bus(SPIClass *bus) { _spi = bus; }

  void Init(uint32_t spi_hz = MSP2402_SPI_HZ);
  void Set_Rotation(uint8_t r);        // 0/1/2/3, 奇数时宽高互换
  uint8_t Get_Rotation() const { return _rotation; }
  int16_t Get_Width() const { return _width; }
  int16_t Get_Height() const { return _height; }

  // ---- 基础绘图 ----
  void Fill_Screen(uint16_t color);
  void Fill_Screen(uint8_t r, uint8_t g, uint8_t b) { Fill_Screen(Color_To_565(r, g, b)); }
  void Fill_Rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void Fill_Rect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b) {
    Fill_Rect(x, y, w, h, Color_To_565(r, g, b));
  }
  void Draw_Pixel(int16_t x, int16_t y, uint16_t color);
  void Draw_Line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
  void Draw_Fast_VLine(int16_t x, int16_t y, int16_t h, uint16_t color);
  void Draw_Fast_HLine(int16_t x, int16_t y, int16_t w, uint16_t color);
  void Draw_Rect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void Draw_Round_Rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t radius, uint16_t color);
  void Fill_Round_Rect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t radius, uint16_t color);
  void Draw_Circle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
  void Fill_Circle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
  void Draw_Triangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                     int16_t x2, int16_t y2, uint16_t color);
  void Fill_Triangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                     int16_t x2, int16_t y2, uint16_t color);

  // ---- 位图 (RGB565, 行优先, 无字节交换) ----
  // 从 PROGMEM / 普通 RAM 都能读, 自动按行推屏
  void Draw_Bitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                   const uint16_t *data, bool from_flash = false);
  void Draw_Bitmap_1bpp(int16_t x, int16_t y, int16_t w, int16_t h,
                        const uint8_t *data, uint16_t fg, uint16_t bg);

  // ---- 文字 ----
  void Set_Text_Size(uint8_t s) { _text_size = s ? s : 1; }
  uint8_t Get_Text_Size() const { return _text_size; }
  void Set_Text_Color(uint16_t color) { _text_fg = color; }
  void Set_Text_Bg(uint16_t color) { _text_bg = color; _text_opaque = true; }
  void Set_Text_Transparent() { _text_opaque = false; }
  // 打印 UTF-8 字符串; 中文按 16x16 字库, ASCII 按 5x7 放大 _text_size 倍
  // x 可以是 MSP_LEFT / MSP_CENTER / MSP_RIGHT
  void Print_String(const char *str, int16_t x, int16_t y);
  void Print_String(const char *str, int16_t x, int16_t y,
                    uint16_t fg, uint16_t bg);
  int16_t Get_String_Width(const char *str) const;
  // 字库里一共收了几个汉字 / 某个 UTF-8 字符是否有字模
  static uint16_t Get_Font_Count();
  static bool Has_Glyph(const char *utf8_char);

  // ---- 面板控制 ----
  void Backlight(bool on);
  void Set_Backlight(uint8_t percent);   // 0~100, 用 LEDC PWM
  void Invert(bool inv);
  void Vert_Scroll(int16_t top, int16_t lines, int16_t offset);
  void Sleep(bool on);

  // 面板 GRAM 可视区的像素偏移。
  // 有些批次的屏, GRAM 原点并不等于可视区左上角 (比如 ST7735 要 2/1),
  // ILI9341 的 240x320 一般两轴都是 0, 但厂家换屏/换 FPC 后可能不是。
  // 画面整体偏移、边上出彩条时就是这里要调。
  void Set_Offset(int16_t xoff, int16_t yoff);
  int16_t Get_Offset_X() const { return _xoff2; }
  int16_t Get_Offset_Y() const { return _yoff2; }

  uint16_t Read_ID();                     // 需要接上 SDO/MISO
  uint16_t Read_Pixel(int16_t x, int16_t y);
  uint16_t Color_To_565(uint8_t r, uint8_t g, uint8_t b) const {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }

  // 底层, 示例里想自己发命令时用
  void Write_Cmd(uint8_t cmd);
  void Write_Data8(uint8_t d);
  void Write_Data16(uint16_t d);
  void Set_Addr_Window(int16_t x1, int16_t y1, int16_t x2, int16_t y2);

private:
  void Bus_Init();
  void Reset();
  void Init_Sequence();
  void Write_Cmd_Data8(uint8_t cmd, uint8_t d);
  void Push_Color(uint16_t color, uint32_t count);
  void Push_Buffer(const uint8_t *buf, uint32_t len);
  void End_Write();
  void Draw_Char_CN(int16_t x, int16_t y, const uint8_t *glyph, uint16_t fg, uint16_t bg);
  void Draw_Char_ASCII(int16_t x, int16_t y, char c, uint16_t fg, uint16_t bg);
  void Apply_Madctl();

  SPIClass *_spi;
  int8_t _cs, _dc, _rst, _bl, _sck, _mosi, _miso;
  int16_t _width, _height;
  int16_t _xoff2, _yoff2;        // GRAM 可视区偏移
  uint8_t _rotation;
  uint8_t _text_size;
  uint16_t _text_fg, _text_bg;
  bool _text_opaque;
  uint32_t _spi_hz;
  bool _bl_pwm;
  bool _in_write;
};

#endif // _MSP2402_H_
