#include "TFT_eSPI.h"

#include <cstdio>
#include <cstdlib>

// glcdfont.c keeps its table static, so it is pulled into this translation unit
// rather than linked.
#include <Fonts/glcdfont.c>
static const unsigned char *const kGlcdFont = font;

// Font tables linked straight from the TFT_eSPI distribution so glyph shapes,
// widths and heights are identical to the device.
extern "C" {
extern const unsigned char widtbl_f16[96];
extern const unsigned char *const chrtbl_f16[96];
extern const unsigned char widtbl_f32[96];
extern const unsigned char *const chrtbl_f32[96];
extern const unsigned char widtbl_f64[96];
extern const unsigned char *const chrtbl_f64[96];
extern const unsigned char widtbl_f72[96];
extern const unsigned char *const chrtbl_f72[96];
}

namespace {

constexpr int kHeightF16 = 16;
constexpr int kHeightF32 = 26;
constexpr int kHeightF64 = 48;
constexpr int kHeightF72 = 75;

struct BitmapFont {
    const unsigned char *widths;
    const unsigned char *const *glyphs;
    int height;
    bool runLengthEncoded;
};

bool bitmapFontFor(uint8_t font, BitmapFont &out) {
    switch (font) {
        case 2: out = {widtbl_f16, chrtbl_f16, kHeightF16, false}; return true;
        case 4: out = {widtbl_f32, chrtbl_f32, kHeightF32, true}; return true;
        case 6: out = {widtbl_f64, chrtbl_f64, kHeightF64, true}; return true;
        case 8: out = {widtbl_f72, chrtbl_f72, kHeightF72, true}; return true;
        default: return false;
    }
}

}  // namespace

static uint32_t simMillisValue = 0;
uint32_t millis() { return simMillisValue; }
void simSetMillis(uint32_t value) { simMillisValue = value; }

TFT_eSPI::TFT_eSPI(int width, int height)
    : canvasWidth(width), canvasHeight(height), pixels(static_cast<size_t>(width * height), 0) {}

void TFT_eSPI::drawPixel(int32_t x, int32_t y, uint16_t color) {
    if (x < 0 || y < 0 || x >= canvasWidth || y >= canvasHeight) {
        return;
    }
    pixels[static_cast<size_t>(y * canvasWidth + x)] = color;
}

void TFT_eSPI::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t *data) {
    for (int32_t row = 0; row < h; ++row) {
        for (int32_t column = 0; column < w; ++column) {
            drawPixel(x + column, y + row, data[(row * w) + column]);
        }
    }
}

void TFT_eSPI::fillScreen(uint16_t color) {
    for (auto &pixel : pixels) {
        pixel = color;
    }
}

void TFT_eSPI::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
    for (int32_t row = y; row < y + h; ++row) {
        for (int32_t column = x; column < x + w; ++column) {
            drawPixel(column, row, color);
        }
    }
}

void TFT_eSPI::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
    drawFastHLine(x, y, w, color);
    drawFastHLine(x, y + h - 1, w, color);
    drawFastVLine(x, y, h, color);
    drawFastVLine(x + w - 1, y, h, color);
}

void TFT_eSPI::drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color) {
    for (int32_t column = x; column < x + w; ++column) {
        drawPixel(column, y, color);
    }
}

void TFT_eSPI::drawFastVLine(int32_t x, int32_t y, int32_t h, uint16_t color) {
    for (int32_t row = y; row < y + h; ++row) {
        drawPixel(x, row, color);
    }
}

void TFT_eSPI::drawLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t color) {
    int32_t dx = abs(x1 - x0);
    int32_t dy = -abs(y1 - y0);
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t error = dx + dy;

    while (true) {
        drawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int32_t doubled = error * 2;
        if (doubled >= dy) { error += dy; x0 += sx; }
        if (doubled <= dx) { error += dx; y0 += sy; }
    }
}

void TFT_eSPI::fillCircle(int32_t x, int32_t y, int32_t radius, uint16_t color) {
    for (int32_t row = -radius; row <= radius; ++row) {
        for (int32_t column = -radius; column <= radius; ++column) {
            if ((column * column) + (row * row) <= radius * radius) {
                drawPixel(x + column, y + row, color);
            }
        }
    }
}

void TFT_eSPI::drawCircle(int32_t x, int32_t y, int32_t radius, uint16_t color) {
    for (int angle = 0; angle < 3600; ++angle) {
        float radians = angle * 3.14159265f / 1800.0f;
        drawPixel(x + static_cast<int32_t>(lroundf(cosf(radians) * radius)),
                  y + static_cast<int32_t>(lroundf(sinf(radians) * radius)),
                  color);
    }
}

void TFT_eSPI::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint16_t color) {
    fillRect(x + radius, y, w - (2 * radius), h, color);
    fillRect(x, y + radius, radius, h - (2 * radius), color);
    fillRect(x + w - radius, y + radius, radius, h - (2 * radius), color);
    fillCircle(x + radius, y + radius, radius, color);
    fillCircle(x + w - radius - 1, y + radius, radius, color);
    fillCircle(x + radius, y + h - radius - 1, radius, color);
    fillCircle(x + w - radius - 1, y + h - radius - 1, radius, color);
}

void TFT_eSPI::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint16_t color) {
    drawFastHLine(x + radius, y, w - (2 * radius), color);
    drawFastHLine(x + radius, y + h - 1, w - (2 * radius), color);
    drawFastVLine(x, y + radius, h - (2 * radius), color);
    drawFastVLine(x + w - 1, y + radius, h - (2 * radius), color);
    drawCircle(x + radius, y + radius, radius, color);
    drawCircle(x + w - radius - 1, y + radius, radius, color);
    drawCircle(x + radius, y + h - radius - 1, radius, color);
    drawCircle(x + w - radius - 1, y + h - radius - 1, radius, color);
}

void TFT_eSPI::fillTriangle(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint16_t color) {
    int32_t minX = min(x0, min(x1, x2));
    int32_t maxX = max(x0, max(x1, x2));
    int32_t minY = min(y0, min(y1, y2));
    int32_t maxY = max(y0, max(y1, y2));

    auto edge = [](int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t px, int32_t py) {
        return ((bx - ax) * (py - ay)) - ((by - ay) * (px - ax));
    };

    for (int32_t y = minY; y <= maxY; ++y) {
        for (int32_t x = minX; x <= maxX; ++x) {
            int32_t w0 = edge(x1, y1, x2, y2, x, y);
            int32_t w1 = edge(x2, y2, x0, y0, x, y);
            int32_t w2 = edge(x0, y0, x1, y1, x, y);
            bool inside = (w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0);
            if (inside) {
                drawPixel(x, y, color);
            }
        }
    }
}

int TFT_eSPI::glyphAdvance(char character, uint8_t font) const {
    if (gfxFont != nullptr && font == 1) {
        if (static_cast<uint16_t>(character) < gfxFont->first || static_cast<uint16_t>(character) > gfxFont->last) {
            return 0;
        }
        return gfxFont->glyph[static_cast<uint16_t>(character) - gfxFont->first].xAdvance * textSize;
    }

    if (font == 1) {
        return 6 * textSize;
    }

    BitmapFont table;
    if (!bitmapFontFor(font, table) || character < 32 || character > 127) {
        return 0;
    }
    return table.widths[character - 32] * textSize;
}

int TFT_eSPI::textWidth(const String &text) { return textWidth(text, textFont); }

int TFT_eSPI::textWidth(const String &text, uint8_t font) {
    int total = 0;
    for (char character : text.text) {
        total += glyphAdvance(character, font);
    }
    return total;
}

int TFT_eSPI::fontHeight() { return fontHeight(textFont); }

int TFT_eSPI::fontHeight(uint8_t font) {
    if (gfxFont != nullptr && font == 1) {
        return gfxFont->yAdvance * textSize;
    }
    if (font == 1) {
        return 8 * textSize;
    }
    BitmapFont table;
    return bitmapFontFor(font, table) ? table.height * textSize : 8;
}

void TFT_eSPI::drawGlyphGfx(char character, int x, int baselineY) {
    uint16_t code = static_cast<uint16_t>(character);
    if (code < gfxFont->first || code > gfxFont->last) {
        return;
    }

    const GFXglyph &glyph = gfxFont->glyph[code - gfxFont->first];
    const uint8_t *bitmap = gfxFont->bitmap + glyph.bitmapOffset;
    uint8_t bits = 0;
    uint8_t bit = 0;

    for (uint8_t row = 0; row < glyph.height; ++row) {
        for (uint8_t column = 0; column < glyph.width; ++column) {
            if ((bit++ & 7) == 0) {
                bits = *bitmap++;
            }
            if (bits & 0x80) {
                for (int sy = 0; sy < textSize; ++sy) {
                    for (int sx = 0; sx < textSize; ++sx) {
                        drawPixel(x + ((glyph.xOffset + column) * textSize) + sx,
                                  baselineY + ((glyph.yOffset + row) * textSize) + sy,
                                  textColor);
                    }
                }
            }
            bits <<= 1;
        }
    }
}

// Mirrors the library's two glyph paths: font 2 is bit packed, fonts 4/6/8 are
// run length encoded with the high bit marking a run of foreground pixels.
void TFT_eSPI::drawGlyph(char character, int x, int y, uint8_t font) {
    if (font == 1) {
        // The 5x7 GLCD face, stored column-wise like Adafruit's original.
        if (!transparent) {
            fillRect(x, y, 6 * textSize, 8 * textSize, textBgColor);
        }
        for (int column = 0; column < 5; ++column) {
            uint8_t bits = kGlcdFont[(static_cast<uint8_t>(character) * 5) + column];
            for (int row = 0; row < 8; ++row) {
                if (bits & (1 << row)) {
                    for (int sy = 0; sy < textSize; ++sy) {
                        for (int sx = 0; sx < textSize; ++sx) {
                            drawPixel(x + (column * textSize) + sx, y + (row * textSize) + sy, textColor);
                        }
                    }
                }
            }
        }
        return;
    }

    BitmapFont table;
    if (!bitmapFontFor(font, table) || character < 32 || character > 127) {
        return;
    }

    int width = table.widths[character - 32];
    int height = table.height;
    const unsigned char *data = table.glyphs[character - 32];
    if (data == nullptr) {
        return;
    }

    if (!transparent) {
        fillRect(x, y, width * textSize, height * textSize, textBgColor);
    }

    if (!table.runLengthEncoded) {
        int bytesPerRow = (width + 6) / 8;
        for (int row = 0; row < height; ++row) {
            for (int byteIndex = 0; byteIndex < bytesPerRow; ++byteIndex) {
                uint8_t line = data[(bytesPerRow * row) + byteIndex];
                for (int bit = 0; bit < 8; ++bit) {
                    if (line & (0x80 >> bit)) {
                        int column = (byteIndex * 8) + bit;
                        if (column < width) {
                            for (int sy = 0; sy < textSize; ++sy) {
                                for (int sx = 0; sx < textSize; ++sx) {
                                    drawPixel(x + (column * textSize) + sx, y + (row * textSize) + sy, textColor);
                                }
                            }
                        }
                    }
                }
            }
        }
        return;
    }

    int total = width * height;
    int painted = 0;
    while (painted < total) {
        uint8_t line = *data++;
        bool foreground = (line & 0x80) != 0;
        int run = foreground ? ((line & 0x7F) + 1) : (line + 1);
        for (int index = 0; index < run && painted < total; ++index, ++painted) {
            if (!foreground) {
                continue;
            }
            int column = painted % width;
            int row = painted / width;
            for (int sy = 0; sy < textSize; ++sy) {
                for (int sx = 0; sx < textSize; ++sx) {
                    drawPixel(x + (column * textSize) + sx, y + (row * textSize) + sy, textColor);
                }
            }
        }
    }
}

int TFT_eSPI::renderAt(const String &text, int x, int y, uint8_t font) {
    int cursor = x;
    if (gfxFont != nullptr && font == 1) {
        for (char character : text.text) {
            drawGlyphGfx(character, cursor, y);
            cursor += glyphAdvance(character, font);
        }
        return cursor - x;
    }

    for (char character : text.text) {
        drawGlyph(character, cursor, y, font);
        cursor += glyphAdvance(character, font);
    }
    return cursor - x;
}

int TFT_eSPI::drawString(const String &text, int32_t x, int32_t y) {
    return drawString(text, x, y, textFont);
}

int TFT_eSPI::drawString(const String &text, int32_t x, int32_t y, uint8_t font) {
    int stringWidth = textWidth(text, font);

    // Free fonts position on the baseline, so ascent and descent of the actual
    // string decide the block height, exactly as the library computes it.
    int ascent = 0;
    int descent = 0;
    bool freeFont = gfxFont != nullptr && font == 1;
    if (freeFont) {
        for (char character : text.text) {
            uint16_t code = static_cast<uint16_t>(character);
            if (code < gfxFont->first || code > gfxFont->last) {
                continue;
            }
            const GFXglyph &glyph = gfxFont->glyph[code - gfxFont->first];
            ascent = max(ascent, -glyph.yOffset * textSize);
            descent = max(descent, (glyph.height + glyph.yOffset) * textSize);
        }
    }

    int blockHeight = freeFont ? (ascent + descent) : fontHeight(font);

    int left = x;
    int top = y;
    switch (textDatum) {
        case TC_DATUM: left = x - (stringWidth / 2); break;
        case TR_DATUM: left = x - stringWidth; break;
        case ML_DATUM: top = y - (blockHeight / 2); break;
        case MC_DATUM: left = x - (stringWidth / 2); top = y - (blockHeight / 2); break;
        case MR_DATUM: left = x - stringWidth; top = y - (blockHeight / 2); break;
        case BL_DATUM: top = y - blockHeight; break;
        case BC_DATUM: left = x - (stringWidth / 2); top = y - blockHeight; break;
        case BR_DATUM: left = x - stringWidth; top = y - blockHeight; break;
        default: break;
    }

    if (textPadding > stringWidth && !transparent) {
        fillRect(left - ((textPadding - stringWidth) / 2), top, textPadding, blockHeight, textBgColor);
    }

    return renderAt(text, left, freeFont ? top + ascent : top, font);
}

bool TFT_eSPI::writePpm(const std::string &path) const {
    FILE *file = fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }

    fprintf(file, "P6\n%d %d\n255\n", canvasWidth, canvasHeight);
    for (uint16_t pixel : pixels) {
        uint8_t rgb[3] = {
            static_cast<uint8_t>(((pixel >> 11) & 0x1F) << 3),
            static_cast<uint8_t>(((pixel >> 5) & 0x3F) << 2),
            static_cast<uint8_t>((pixel & 0x1F) << 3),
        };
        fwrite(rgb, 1, 3, file);
    }

    fclose(file);
    return true;
}
