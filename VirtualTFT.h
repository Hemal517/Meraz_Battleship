/* =====================================================================
   VirtualTFT.h - a stand-in for TFT_eSPI that needs NO display hardware

   Use it while you're waiting for the real TFT to arrive. Every draw
   call the UI makes is turned into one text line on USB Serial, and
   virtual_display.py (on your laptop) draws those lines onto a 240x320
   screen in your browser. Clicking that screen sends a "touch" back to
   the board, which getTouch() hands to the UI exactly like a real tap.

   The real UI code in participant_node.ino runs unchanged - it just
   talks to this class instead of the real driver.

   HOW TO USE
     1. In participant_node.ino, uncomment:  #define USE_VIRTUAL_DISPLAY
     2. Keep this file in the same folder as participant_node.ino
     3. Upload as usual. Close the Arduino Serial Monitor afterwards
        (only one program can own the USB port).
     4. On your laptop:  python virtual_display.py COM6   (your port)
        then open http://localhost:5001

   TO GO BACK TO THE REAL DISPLAY: comment the #define out again.

   WIRE FORMAT (one line per call, always starting with "@TFT "):
     @TFT INIT
     @TFT F  color                        fillScreen
     @TFT R  x y w h color                fillRect
     @TFT r  x y w h color                drawRect
     @TFT Q  x y w h radius color         fillRoundRect
     @TFT q  x y w h radius color         drawRoundRect
     @TFT T  x y size fg bg text          text at (x,y); bg = -1 means
                                          transparent. Colors are RGB565.
   Lines coming back from the host:
     @TOUCH x y                           a tap at pixel (x,y)
     @REDRAW                              "repaint the current screen"

   LIMITS (be honest with yourself about what this does NOT test)
     - Text uses the built-in 6x8 font metrics, like the real driver's
       default font, but glyph shapes in the browser are approximate.
     - It doesn't test your wiring, SPI speed, backlight, or touch
       calibration. Those only show up on the real display.
     - Drawing over USB Serial at 115200 baud is slower than SPI; a full
       screen takes roughly a tenth of a second.
   ===================================================================== */

#ifndef VIRTUAL_TFT_H
#define VIRTUAL_TFT_H

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Same RGB565 values TFT_eSPI uses, so the sketch's color names match.
#define TFT_BLACK       0x0000
#define TFT_NAVY        0x000F
#define TFT_DARKGREEN   0x03E0
#define TFT_DARKCYAN    0x03EF
#define TFT_MAROON      0x7800
#define TFT_PURPLE      0x780F
#define TFT_OLIVE       0x7BE0
#define TFT_LIGHTGREY   0xD69A
#define TFT_DARKGREY    0x7BEF
#define TFT_BLUE        0x001F
#define TFT_GREEN       0x07E0
#define TFT_CYAN        0x07FF
#define TFT_RED         0xF800
#define TFT_MAGENTA     0xF81F
#define TFT_YELLOW      0xFFE0
#define TFT_WHITE       0xFFFF
#define TFT_ORANGE      0xFDA0
#define TFT_GREENYELLOW 0xB7E0
#define TFT_PINK        0xFE19
#define TFT_BROWN       0x9A60
#define TFT_GOLD        0xFEA0
#define TFT_SILVER      0xC618
#define TFT_SKYBLUE     0x867D
#define TFT_VIOLET      0x915C

class VirtualTFT {
 public:
  // --- setup-type calls (same names as TFT_eSPI) ---
  void init() { emit("@TFT INIT"); }
  void setRotation(uint8_t) {}            // the virtual screen is always portrait
  void setTouch(uint16_t *) {}            // no calibration needed for mouse clicks

  // --- drawing ---
  void fillScreen(uint32_t color) { emit("@TFT F %lu", (unsigned long)color); }
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    emit("@TFT R %ld %ld %ld %ld %lu", (long)x, (long)y, (long)w, (long)h, (unsigned long)color);
  }
  void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    emit("@TFT r %ld %ld %ld %ld %lu", (long)x, (long)y, (long)w, (long)h, (unsigned long)color);
  }
  void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color) {
    emit("@TFT Q %ld %ld %ld %ld %ld %lu", (long)x, (long)y, (long)w, (long)h, (long)r, (unsigned long)color);
  }
  void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color) {
    emit("@TFT q %ld %ld %ld %ld %ld %lu", (long)x, (long)y, (long)w, (long)h, (long)r, (unsigned long)color);
  }

  // --- text (built-in font: each character cell is 6*size x 8*size) ---
  void setTextSize(uint8_t s) { textSize_ = (s == 0) ? 1 : s; }
  void setTextColor(uint16_t fg) { fg_ = fg; fillBg_ = false; }            // transparent background
  void setTextColor(uint16_t fg, uint16_t bg) { fg_ = fg; bg_ = bg; fillBg_ = true; }
  void setCursor(int16_t x, int16_t y) { cx_ = x; cy_ = y; }

  int16_t textWidth(const char *s) { return (int16_t)(6 * textSize_ * strlen(s)); }
  int16_t textWidth(const String &s) { return textWidth(s.c_str()); }

  size_t print(const char *s) {
    char clean[100];
    size_t n = 0;
    for (; s[n] && n < sizeof(clean) - 1; n++) {
      clean[n] = (s[n] == '\n' || s[n] == '\r') ? ' ' : s[n];
    }
    clean[n] = '\0';
    if (n == 0) return 0;
    emit("@TFT T %d %d %u %u %ld %s", cx_, cy_, (unsigned)textSize_, (unsigned)fg_,
         fillBg_ ? (long)bg_ : -1L, clean);
    cx_ += (int16_t)(6 * textSize_ * n);   // the real driver advances the cursor too
    return n;
  }
  size_t print(const String &s) { return print(s.c_str()); }
  size_t print(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); return print(b); }
  size_t print(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); return print(b); }

  // --- touch ---
  // Called by the sketch when the host sends "@TOUCH x y". One tap is
  // reported exactly once (the UI has its own debounce on top of this).
  void feedTouch(uint16_t x, uint16_t y) { touchX_ = x; touchY_ = y; touchPending_ = true; }

  bool getTouch(uint16_t *x, uint16_t *y, uint16_t /*threshold*/ = 600) {
    if (!touchPending_) return false;
    touchPending_ = false;
    *x = touchX_;
    *y = touchY_;
    return true;
  }

 private:
  uint8_t textSize_ = 1;
  uint16_t fg_ = TFT_WHITE, bg_ = TFT_BLACK;
  bool fillBg_ = false;
  int16_t cx_ = 0, cy_ = 0;
  uint16_t touchX_ = 0, touchY_ = 0;
  volatile bool touchPending_ = false;

  // One single write() per line, so a log message printed from another
  // task can't land in the middle of a draw command.
  void emit(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    Serial.write((const uint8_t *)buf, n);
  }
};

#endif  // VIRTUAL_TFT_H
