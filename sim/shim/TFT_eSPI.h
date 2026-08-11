// Host-side TFT_eSPI stand-in. Geometry and text metrics follow the real
// library so a rendered frame matches what the panel shows, pixel for pixel:
// the bitmap fonts are decoded from the library's own font tables, and the
// GFX free fonts from their headers.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Arduino.h"

#define TFT_BLACK 0x0000
#define TFT_WHITE 0xFFFF
#define TFT_RED 0xF800
#define TFT_GREEN 0x07E0
#define TFT_BLUE 0x001F
#define TFT_CYAN 0x07FF
#define TFT_ORANGE 0xFDA0
#define TFT_DARKGREY 0x7BEF

// Datum codes, matching the library's ordering.
#define TL_DATUM 0
#define TC_DATUM 1
#define TR_DATUM 2
#define ML_DATUM 3
#define MC_DATUM 4
#define MR_DATUM 5
#define BL_DATUM 6
#define BC_DATUM 7
#define BR_DATUM 8

struct GFXglyph {
    uint32_t bitmapOffset;
    uint8_t width;
    uint8_t height;
    uint8_t xAdvance;
    int8_t xOffset;
    int8_t yOffset;
};

struct GFXfont {
    uint8_t *bitmap;
    GFXglyph *glyph;
    uint16_t first;
    uint16_t last;
    uint8_t yAdvance;
};

// The real header pulls the free fonts in behind LOAD_GFXFF, so the firmware
// never includes them itself. Mirror that here.
#ifdef LOAD_GFXFF
#include <Fonts/GFXFF/FreeSansBold24pt7b.h>
#endif

class TFT_eSPI {
  public:
    TFT_eSPI(int width = 240, int height = 240);

    void init() {}
    void startWrite() {}
    void endWrite() {}
    void setRotation(uint8_t) {}
    void invertDisplay(bool) {}

    int width() const { return canvasWidth; }
    int height() const { return canvasHeight; }

    void fillScreen(uint16_t color);
    void drawPixel(int32_t x, int32_t y, uint16_t color);
    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
    void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
    void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint16_t color);
    void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint16_t color);
    void fillCircle(int32_t x, int32_t y, int32_t radius, uint16_t color);
    void drawCircle(int32_t x, int32_t y, int32_t radius, uint16_t color);
    void fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t color);
    void drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color);
    void drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color);
    void drawFastVLine(int32_t x, int32_t y, int32_t h, uint16_t color);
    void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t *data);

    void setTextDatum(uint8_t datum) { textDatum = datum; }
    void setTextFont(uint8_t font) { textFont = font; gfxFont = nullptr; }
    void setFreeFont(const GFXfont *font) { gfxFont = font; textFont = 1; }
    void setTextColor(uint16_t color) { textColor = color; textBgColor = color; transparent = true; }
    void setTextColor(uint16_t color, uint16_t background) {
        textColor = color;
        textBgColor = background;
        transparent = false;
    }
    void setTextPadding(uint16_t padding) { textPadding = padding; }
    void setTextSize(uint8_t size) { textSize = size; }

    int textWidth(const String &text);
    int textWidth(const String &text, uint8_t font);
    int fontHeight();
    int fontHeight(uint8_t font);

    int drawString(const String &text, int32_t x, int32_t y);
    int drawString(const String &text, int32_t x, int32_t y, uint8_t font);

    bool writePpm(const std::string &path) const;

  private:
    int glyphAdvance(char character, uint8_t font) const;
    void drawGlyph(char character, int x, int y, uint8_t font);
    void drawGlyphGfx(char character, int x, int baselineY);
    int renderAt(const String &text, int x, int y, uint8_t font);

    int canvasWidth;
    int canvasHeight;
    std::vector<uint16_t> pixels;

    uint8_t textDatum = TL_DATUM;
    uint8_t textFont = 1;
    uint8_t textSize = 1;
    uint16_t textColor = TFT_WHITE;
    uint16_t textBgColor = TFT_BLACK;
    uint16_t textPadding = 0;
    bool transparent = false;
    const GFXfont *gfxFont = nullptr;
};

// The sprite type is only referenced for the clock's optional buffer, which the
// device never allocates at this heap size; a thin alias keeps the code honest.
class TFT_eSprite : public TFT_eSPI {
  public:
    explicit TFT_eSprite(TFT_eSPI *) : TFT_eSPI(1, 1) {}
    void setColorDepth(int8_t) {}
    void *createSprite(int16_t, int16_t) { return nullptr; }
    void deleteSprite() {}
    void fillSprite(uint16_t) {}
    void pushSprite(int32_t, int32_t) {}
};
