#include "display.h"

#include "auth.h"
#include "config.h"
#include "feeds.h"
#include "logger.h"
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <time.h>
#include <vector>
#include "fonts/SansMono11Vlw.h"
#include "fonts/SansMono13Vlw.h"
#include "fonts/SansMono16Vlw.h"
#include "fonts/SansMono18Vlw.h"
#include "fonts/SansMono22Vlw.h"
#include "fonts/SansMono26Vlw.h"
#include "fonts/SansMono44Vlw.h"
#include "fonts/ClockVlw.h"
#include "fonts/HeroVlw.h"

// Type tiers. Every page text is now drawn with an anti-aliased VLW mono face,
// the same way the clock and hero temperature already were, so nothing on
// screen stair-steps. loadMono() maps a pixel size to its VLW array; loadFont
// auto-unloads the previously loaded face, so switching sizes never leaks heap.
#define FONT_INFO 13
#define FONT_LABEL 18
#define FONT_BODY 26
#define FONT_TITLE 26

static inline void loadMono(int px) {
    switch (px) {
        case 11: tft.loadFont(SansMono11Vlw); break;
        case 13: tft.loadFont(SansMono13Vlw); break;
        case 16: tft.loadFont(SansMono16Vlw); break;
        case 18: tft.loadFont(SansMono18Vlw); break;
        case 22: tft.loadFont(SansMono22Vlw); break;
        case 44: tft.loadFont(SansMono44Vlw); break;  // seconds fallback: digits + colon only
        default: tft.loadFont(SansMono26Vlw); break;
    }
}

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite clockDynamicSprite = TFT_eSprite(&tft);
DisplayState displayState;
int scrollPos = 240;

namespace {

constexpr uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3);
}

constexpr int kPageMargin = 4;
constexpr int kPageWidth = DISPLAY_WIDTH - (kPageMargin * 2);
constexpr int kHeaderTitleY = 10;
constexpr int kHeaderSubtitleY = 12;
constexpr int kHeaderRuleY = 32;
constexpr int kContentTopY = 40;
// The date sits above the time and the weather line below it, so the repainted
// region covers only the digits themselves.
constexpr int kClockDynamicRegionX = 12;
// The main time sits a little lower than centre; the region and the time share
// the same top so the digits never clip against the sprite's bottom edge. The
// accent rule below the time (in renderClockPage) is moved down by the same
// amount to keep the spacing under the clock.
constexpr int kClockDynamicRegionY = 76;
constexpr int kClockDynamicRegionWidth = DISPLAY_WIDTH - (kClockDynamicRegionX * 2);
constexpr int kClockDynamicRegionHeight = 86;
constexpr int kClockTimeY = 76;
constexpr int kClockMetaY = 38;
constexpr int kClockFooterY = 148;
constexpr int kClockFooterHeight = 78;
constexpr uint32_t kClockSpriteMinFreeHeapBytes = 30000UL;
constexpr uint8_t kPageTransitionFadeDownSteps = 2;
constexpr uint8_t kPageTransitionFadeUpSteps = 3;
constexpr uint16_t kPageTransitionFadeStepDelayMs = 12;
constexpr uint8_t kPageTransitionDimPercent = 82;
constexpr uint8_t kPageTransitionMinimumBrightness = 18;
constexpr size_t kTemporaryMessageBufferSize = 96;

struct ThemePalette {
    uint16_t background;
    uint16_t surface;
    uint16_t surfaceAlt;
    uint16_t accent;
    uint16_t accentSoft;
    uint16_t text;
    uint16_t muted;
    uint16_t positive;
    uint16_t negative;
    uint16_t warning;
};

struct Rgb888 {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

struct ThemeSelection {
    uint8_t theme;
    bool customThemeEnabled;
    const char *background;
    const char *surface;
    const char *accent;
    const char *text;
};

const ThemePalette kThemes[DASHBOARD_THEME_COUNT] = {
    {
        // This is an LCD, not the OLED the mock was viewed on: the backlight
        // leaks through, so the ground already sits at the panel's black floor
        // and colour cannot take it lower. The cards, though, sit above that
        // floor and read as visibly grey, so they are pulled well down toward
        // black - the biggest darkening colour alone can buy without touching
        // the backlight. Faintly cool, near-neutral, so the light text carries.
        rgb565(3, 5, 9),      // ground: as close to black as the panel shows
        rgb565(4, 10, 20),    // main card: dark navy - blue kept dominant so it stays navy, not grey
        rgb565(10, 18, 36),   // secondary cells: one step up, same dark navy
        rgb565(168, 199, 250),
        rgb565(44, 66, 104),
        rgb565(236, 241, 251),
        rgb565(173, 182, 197),
        rgb565(123, 214, 167),
        rgb565(255, 180, 171),
        rgb565(255, 210, 128),
    },
    {
        rgb565(33, 16, 11),
        rgb565(61, 34, 26),
        rgb565(74, 43, 32),
        rgb565(255, 183, 77),
        rgb565(118, 75, 35),
        rgb565(255, 239, 232),
        rgb565(224, 189, 173),
        rgb565(151, 221, 168),
        rgb565(255, 172, 162),
        rgb565(255, 214, 120),
    },
    {
        rgb565(9, 18, 14),
        rgb565(19, 36, 28),
        rgb565(26, 47, 37),
        rgb565(165, 230, 174),
        rgb565(47, 83, 60),
        rgb565(232, 247, 236),
        rgb565(160, 188, 170),
        rgb565(137, 230, 164),
        rgb565(255, 169, 169),
        rgb565(233, 241, 129),
    },
};

int savedBrightness = 100;
int appliedBrightness = -1;
bool backlightOn = true;

enum DisplayRenderMode : uint8_t {
    DISPLAY_MODE_DASHBOARD = 0,
    DISPLAY_MODE_TEMP_MESSAGE,
    DISPLAY_MODE_AP,
    DISPLAY_MODE_IMAGE
};

struct RenderCache {
    bool valid;
    uint8_t mode;
    uint8_t page;
    uint8_t theme;
    uint32_t staticHash;
    uint32_t dynamicHash;
    char imagePath[DISPLAY_PATH_BUFFER_SIZE];
};

RenderCache renderCache = {false, DISPLAY_MODE_DASHBOARD, 0, 0, 0, 0, {0}};

struct ClockMetaCache {
    bool valid;
    char metaLine[40];
    char timeText[16];
};

ClockMetaCache clockMetaCache = {false, {0}, {0}};
bool clockDynamicSpriteReady = false;
bool clockDynamicSpriteAttempted = false;
bool clockDynamicSpriteAllowed = false;
bool resumeClockSpriteAfterDynamicSuspend = false;
uint8_t dynamicResourceSuspendDepth = 0;

struct TemporaryMessageState {
    bool active;
    unsigned long expiresAtMs;
    char message[kTemporaryMessageBufferSize];
};

TemporaryMessageState temporaryMessage = {false, 0, {0}};

constexpr uint32_t kFnvOffset = 2166136261UL;
constexpr uint32_t kFnvPrime = 16777619UL;

void hashBytes(uint32_t &hash, const uint8_t *data, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        hash ^= data[index];
        hash *= kFnvPrime;
    }
}

template <typename TValue>
void hashValue(uint32_t &hash, const TValue &value) {
    hashBytes(hash, reinterpret_cast<const uint8_t*>(&value), sizeof(TValue));
}

void hashCString(uint32_t &hash, const char *value) {
    if (value == nullptr) {
        const uint8_t zero = 0;
        hashBytes(hash, &zero, sizeof(zero));
        return;
    }

    hashBytes(hash, reinterpret_cast<const uint8_t*>(value), strlen(value));
    const uint8_t terminator = 0;
    hashBytes(hash, &terminator, sizeof(terminator));
}

void invalidateRenderCache() {
    renderCache.valid = false;
    renderCache.imagePath[0] = '\0';
    clockMetaCache.valid = false;
    clockMetaCache.metaLine[0] = '\0';
}

void clearTemporaryMessage() {
    temporaryMessage.active = false;
    temporaryMessage.expiresAtMs = 0;
    temporaryMessage.message[0] = '\0';
}

bool temporaryMessageExpired() {
    return temporaryMessage.active &&
           static_cast<long>(millis() - temporaryMessage.expiresAtMs) >= 0;
}

bool temporaryMessageVisible() {
    return temporaryMessage.active &&
           temporaryMessage.message[0] != '\0' &&
           !temporaryMessageExpired();
}

Rgb888 rgb565ToRgb888(uint16_t color) {
    Rgb888 rgb;
    rgb.red = static_cast<uint8_t>(((color >> 11) & 0x1F) * 255 / 31);
    rgb.green = static_cast<uint8_t>(((color >> 5) & 0x3F) * 255 / 63);
    rgb.blue = static_cast<uint8_t>((color & 0x1F) * 255 / 31);
    return rgb;
}

uint16_t rgb888ToRgb565(const Rgb888 &rgb) {
    return rgb565(rgb.red, rgb.green, rgb.blue);
}

bool parseHexColor888(const char *value, Rgb888 &rgb) {
    if (value == nullptr) {
        return false;
    }

    const char *source = value[0] == '#' ? value + 1 : value;
    if (strlen(source) != 6) {
        return false;
    }

    char *end = nullptr;
    unsigned long raw = strtoul(source, &end, 16);
    if (end == nullptr || *end != '\0') {
        return false;
    }

    rgb.red = static_cast<uint8_t>((raw >> 16) & 0xFF);
    rgb.green = static_cast<uint8_t>((raw >> 8) & 0xFF);
    rgb.blue = static_cast<uint8_t>(raw & 0xFF);
    return true;
}

Rgb888 blendRgb(const Rgb888 &base, const Rgb888 &mix, uint8_t mixWeightPercent) {
    uint16_t baseWeight = 100U - constrain(mixWeightPercent, 0, 100);
    uint16_t mixWeight = constrain(mixWeightPercent, 0, 100);

    Rgb888 result;
    result.red = static_cast<uint8_t>((base.red * baseWeight + mix.red * mixWeight) / 100U);
    result.green = static_cast<uint8_t>((base.green * baseWeight + mix.green * mixWeight) / 100U);
    result.blue = static_cast<uint8_t>((base.blue * baseWeight + mix.blue * mixWeight) / 100U);
    return result;
}

ThemeSelection effectiveThemeSelection() {
    if (dashboardNightModeActive() && dashboardConfig.nightThemeEnabled) {
        return {
            dashboardConfig.nightTheme,
            dashboardConfig.nightCustomThemeEnabled,
            dashboardConfig.nightCustomBackground,
            dashboardConfig.nightCustomSurface,
            dashboardConfig.nightCustomAccent,
            dashboardConfig.nightCustomText
        };
    }

    return {
        dashboardConfig.theme,
        dashboardConfig.customThemeEnabled,
        dashboardConfig.customBackground,
        dashboardConfig.customSurface,
        dashboardConfig.customAccent,
        dashboardConfig.customText
    };
}

uint8_t activeThemeIdValue() {
    ThemeSelection selection = effectiveThemeSelection();
    return constrain(selection.theme, 0, DASHBOARD_THEME_COUNT - 1);
}

ThemePalette buildCustomTheme(const ThemePalette &base,
                              const char *backgroundColor,
                              const char *surfaceColor,
                              const char *accentColor,
                              const char *textColor) {
    Rgb888 background = rgb565ToRgb888(base.background);
    Rgb888 surface = rgb565ToRgb888(base.surface);
    Rgb888 accent = rgb565ToRgb888(base.accent);
    Rgb888 text = rgb565ToRgb888(base.text);

    parseHexColor888(backgroundColor, background);
    parseHexColor888(surfaceColor, surface);
    parseHexColor888(accentColor, accent);
    parseHexColor888(textColor, text);

    ThemePalette theme = {};
    theme.background = rgb888ToRgb565(background);
    theme.surface = rgb888ToRgb565(surface);
    theme.surfaceAlt = rgb888ToRgb565(blendRgb(surface, background, 22));
    theme.accent = rgb888ToRgb565(accent);
    theme.accentSoft = rgb888ToRgb565(blendRgb(surface, accent, 28));
    theme.text = rgb888ToRgb565(text);
    theme.muted = rgb888ToRgb565(blendRgb(text, background, 52));
    theme.positive = base.positive;
    theme.negative = base.negative;
    theme.warning = base.warning;
    return theme;
}

void hashThemeConfig(uint32_t &hash) {
    ThemeSelection selection = effectiveThemeSelection();
    hashValue(hash, selection.theme);
    hashValue(hash, selection.customThemeEnabled);
    if (!selection.customThemeEnabled) {
        return;
    }

    hashCString(hash, selection.background);
    hashCString(hash, selection.surface);
    hashCString(hash, selection.accent);
    hashCString(hash, selection.text);
}

const ThemePalette& activeTheme() {
    static ThemePalette customTheme;
    ThemeSelection selection = effectiveThemeSelection();
    uint8_t themeIndex = constrain(selection.theme, 0, DASHBOARD_THEME_COUNT - 1);
    if (!selection.customThemeEnabled) {
        return kThemes[themeIndex];
    }

    customTheme = buildCustomTheme(kThemes[themeIndex],
                                   selection.background,
                                   selection.surface,
                                   selection.accent,
                                   selection.text);
    return customTheme;
}

String safeCString(const char *value) {
    return (value != nullptr) ? String(value) : String("");
}

bool hasTimeSync() {
    return dashboardCurrentEpoch() != 0;
}

String formatTimeForTm(const tm &timeInfo, bool showSeconds, bool use24Hour, bool *isPm = nullptr) {
    char buffer[16];
    const char *format = nullptr;

    if (use24Hour) {
        format = showSeconds ? "%H:%M:%S" : "%H:%M";
    } else {
        format = showSeconds ? "%I:%M:%S" : "%I:%M";
        if (isPm != nullptr) {
            *isPm = timeInfo.tm_hour >= 12;
        }
    }

    strftime(buffer, sizeof(buffer), format, &timeInfo);
    return String(buffer);
}

String formatLocalTime(bool showSeconds, bool use24Hour, bool *isPm = nullptr) {
    if (!hasTimeSync()) {
        if (isPm != nullptr) {
            *isPm = false;
        }
        return showSeconds ? "--:--:--" : "--:--";
    }

    time_t now = time(nullptr);
    tm localTimeInfo;
    localtime_r(&now, &localTimeInfo);
    return formatTimeForTm(localTimeInfo, showSeconds, use24Hour, isPm);
}

String formatLocalDate() {
    if (!hasTimeSync()) {
        return "Waiting for time";
    }

    // "MON 3 AUG" - uppercase, no leading zero, no year, matching the design.
    char buffer[32];
    time_t now = time(nullptr);
    tm localTimeInfo;
    localtime_r(&now, &localTimeInfo);
    strftime(buffer, sizeof(buffer), "%a %b", &localTimeInfo);

    String weekday(buffer);
    int separator = weekday.indexOf(' ');
    String month = separator >= 0 ? weekday.substring(separator + 1) : String("");
    weekday = separator >= 0 ? weekday.substring(0, separator) : weekday;

    String result = weekday + " " + String(localTimeInfo.tm_mday) + " " + month;
    result.toUpperCase();
    return result;
}

String formatDuration(int32_t totalSeconds) {
    int32_t boundedSeconds = (totalSeconds > 0) ? totalSeconds : 0;
    int32_t hours = boundedSeconds / 3600;
    int32_t minutes = (boundedSeconds % 3600) / 60;
    int32_t seconds = boundedSeconds % 60;

    char buffer[16];
    if (hours > 0) {
        snprintf(buffer, sizeof(buffer), "%ld:%02ld", static_cast<long>(hours), static_cast<long>(minutes));
    } else {
        snprintf(buffer, sizeof(buffer), "%02ld:%02ld", static_cast<long>(minutes), static_cast<long>(seconds));
    }
    return String(buffer);
}

String formatUtcOffset(long offsetSeconds) {
    long absoluteSeconds = labs(offsetSeconds);
    long hours = absoluteSeconds / 3600;
    long minutes = (absoluteSeconds % 3600) / 60;
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "UTC%c%02ld:%02ld",
             offsetSeconds >= 0 ? '+' : '-',
             hours,
             minutes);
    return String(buffer);
}

String formatClockMetaLine(bool isPm) {
    String metaLine = formatLocalDate();
    if (!dashboardConfig.use24Hour && hasTimeSync()) {
        metaLine += isPm ? "  PM" : "  AM";
    }
    return metaLine;
}

void updateClockMetaCache(const String &metaLine) {
    strncpy(clockMetaCache.metaLine, metaLine.c_str(), sizeof(clockMetaCache.metaLine) - 1);
    clockMetaCache.metaLine[sizeof(clockMetaCache.metaLine) - 1] = '\0';
    clockMetaCache.valid = true;
}

template <typename TCanvas>
int chooseCanvasFittingFont(TCanvas &canvas,
                            const String &text,
                            int maxWidth,
                            const int *fontCandidates,
                            size_t fontCandidateCount) {
    if (fontCandidates == nullptr || fontCandidateCount == 0) {
        return FONT_INFO;
    }

    for (size_t index = 0; index < fontCandidateCount; ++index) {
        int font = fontCandidates[index];
        if (canvas.textWidth(text, font) <= maxWidth) {
            return font;
        }
    }

    return fontCandidates[fontCandidateCount - 1];
}

void drawClockTime(const String &timeText,
                   bool showSeconds,
                   uint16_t primaryColor,
                   uint16_t backgroundColor,
                   int centerX,
                   int topY) {
    // Drawn straight onto the 16-bit panel (not the 4-bit clock sprite) so the
    // anti-aliased VLW face can blend against the background instead of
    // stair-stepping. Digits sit either side of a hand-drawn square colon,
    // bottom-aligned to the baseline, per the design.
    int centerY = topY + 43;
    tft.setTextColor(primaryColor, backgroundColor);

    int colonIndex = timeText.indexOf(':');
    if (!showSeconds && colonIndex > 0) {
        String hours = timeText.substring(0, colonIndex);
        String minutes = timeText.substring(colonIndex + 1);
        tft.loadFont(ClockVlw);
        tft.setTextColor(primaryColor, backgroundColor);
        tft.setTextDatum(L_BASELINE);
        int hoursWidth = tft.textWidth(hours);
        const int colonHalfGap = 15;   // gap from centre to each number's inner edge
        int baselineY = centerY + 25;  // keeps the digits centred in the region
        tft.drawString(hours, centerX - colonHalfGap - hoursWidth, baselineY);
        tft.drawString(minutes, centerX + colonHalfGap, baselineY);
        tft.unloadFont();

        const int square = 9;
        const int gap = 9;
        int colonX = centerX - square / 2;
        tft.fillRect(colonX, baselineY - square, square, square, primaryColor);              // lower dot, bottom on the baseline
        tft.fillRect(colonX, baselineY - (2 * square) - gap, square, square, primaryColor);  // upper dot
        return;
    }

    loadMono(44);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(timeText, centerX, centerY);
}

String headerSubtitle() {
    if (displayState.apMode && displayState.ipInfo[0] != '\0') {
        return safeCString(displayState.ipInfo);
    }

    if (!dashboardConfig.showIp) {
        return "";
    }

    if (displayState.ipInfo[0] != '\0') {
        return safeCString(displayState.ipInfo);
    }

    return "";
}

void drawRoundedPanel(int x, int y, int width, int height, uint16_t fillColor, uint16_t borderColor) {
    tft.fillRoundRect(x, y, width, height, 18, fillColor);
    tft.drawRoundRect(x, y, width, height, 18, borderColor);
}

void drawBadge(int x, int y, int width, int height, const String &label, uint16_t fillColor, uint16_t textColor) {
    tft.fillRoundRect(x, y, width, height, height / 2, fillColor);
    tft.setTextDatum(MC_DATUM);
    loadMono(13);
    tft.setTextColor(textColor, fillColor);
    tft.drawString(label, x + (width / 2), y + (height / 2));
}

int monoForTier(int tier);

void drawWrappedCenteredText(const String &text,
                             int centerX,
                             int startY,
                             int maxWidth,
                             int font,
                             uint16_t textColor,
                             uint16_t backgroundColor,
                             int lineSpacing = 4) {
    loadMono(monoForTier(font));
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(textColor, backgroundColor);
    int lineHeight = tft.fontHeight() + lineSpacing;

    // Word-wrap against the current mono font, breaking hard on '\n'.
    std::vector<String> lines;
    String cur;
    String word;
    auto flushWord = [&]() {
        if (word.length() == 0) {
            return;
        }
        String trial = cur.length() ? cur + " " + word : word;
        if (static_cast<int>(tft.textWidth(trial)) <= maxWidth || cur.length() == 0) {
            cur = trial;
        } else {
            lines.push_back(cur);
            cur = word;
        }
        word = "";
    };
    int length = text.length();
    for (int index = 0; index <= length; ++index) {
        char character = (index < length) ? text[index] : ' ';
        if (character == '\n') {
            flushWord();
            lines.push_back(cur);
            cur = "";
        } else if (character == ' ') {
            flushWord();
        } else {
            word += character;
        }
    }
    if (cur.length()) {
        lines.push_back(cur);
    }

    for (size_t lineIndex = 0; lineIndex < lines.size(); ++lineIndex) {
        tft.drawString(lines[lineIndex], centerX, startY + (lineIndex * lineHeight));
    }
}

// Maps the old built-in font tiers (FONT_BODY/LABEL/INFO) to the sans-mono
// faces, so every caller that still asks for a tier gets the monospace face at
// a matching size.
int monoForTier(int tier) {
    if (tier >= FONT_BODY) {
        return 26;
    }
    if (tier >= FONT_LABEL) {
        return 18;
    }
    return 13;
}

void drawAdaptiveText(const String &text,
                      int x,
                      int y,
                      int maxWidth,
                      uint8_t datum,
                      const int *fontCandidates,
                      size_t fontCandidateCount,
                      uint16_t textColor,
                      uint16_t backgroundColor) {
    // Candidates arrive largest-first; pick the largest mono face that fits.
    int chosen = monoForTier(fontCandidateCount > 0
                                 ? fontCandidates[fontCandidateCount - 1]
                                 : FONT_INFO);
    for (size_t index = 0; index < fontCandidateCount; ++index) {
        int candidate = monoForTier(fontCandidates[index]);
        loadMono(candidate);
        if (static_cast<int>(tft.textWidth(text)) <= maxWidth) {
            chosen = candidate;
            break;
        }
    }
    loadMono(chosen);
    tft.setTextDatum(datum);
    tft.setTextColor(textColor, backgroundColor);
    tft.drawString(text, x, y);
}

void drawDividerLine(int x, int y, int width, uint16_t color) {
    tft.drawFastHLine(x, y, width, color);
}

void drawDividerColumn(int x, int y, int height, uint16_t color) {
    tft.drawFastVLine(x, y, height, color);
}

void drawMetricColumn(int centerX,
                      int topY,
                      const String &label,
                      const String &value,
                      uint16_t labelColor,
                      uint16_t valueColor,
                      uint16_t backgroundColor) {
    tft.setTextDatum(TC_DATUM);
    loadMono(13);
    tft.setTextColor(labelColor, backgroundColor);
    tft.drawString(label, centerX, topY);

    const int valueFonts[] = {FONT_BODY, FONT_LABEL, FONT_INFO};
    drawAdaptiveText(value,
                     centerX,
                     topY + 16,
                     58,
                     TC_DATUM,
                     valueFonts,
                     sizeof(valueFonts) / sizeof(valueFonts[0]),
                     valueColor,
                     backgroundColor);
}

void drawScreenChrome(const String &title) {
    const ThemePalette &theme = activeTheme();
    const int chromeX = 10;
    const int chromeY = 10;
    const int chromeWidth = 220;
    const int chromeHeight = 26;

    tft.fillScreen(theme.background);
    tft.fillRoundRect(chromeX, chromeY, chromeWidth, chromeHeight, 12, theme.surfaceAlt);
    tft.fillCircle(chromeX + 14, chromeY + (chromeHeight / 2), 4, theme.accent);
    tft.setTextDatum(TL_DATUM);
    loadMono(18);
    tft.setTextColor(theme.text, theme.surfaceAlt);
    tft.drawString(title, chromeX + 26, chromeY + 5);

    String subtitle = headerSubtitle();
    if (!subtitle.isEmpty()) {
        loadMono(13);
        int chipWidth = min(94, tft.textWidth(subtitle) + 16);
        int chipHeight = 18;
        int chipX = chromeX + chromeWidth - chipWidth - 6;
        int chipY = chromeY + 4;

        tft.fillRoundRect(chipX, chipY, chipWidth, chipHeight, chipHeight / 2, theme.background);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(theme.muted, theme.background);
        tft.drawString(subtitle, chipX + (chipWidth / 2), chipY + (chipHeight / 2));
    }
}

void drawPlaceholder(const String &title, const String &message) {
    const ThemePalette &theme = activeTheme();
    drawScreenChrome(title);

    drawRoundedPanel(10, 42, 220, 178, theme.surface, theme.surfaceAlt);
    tft.setTextDatum(TC_DATUM);
    tft.fillRoundRect(98, 68, 44, 44, 22, theme.accentSoft);
    tft.setTextColor(theme.accent, theme.accentSoft);
    loadMono(26);
    tft.drawString("?", 120, 78);

    tft.setTextColor(theme.text, theme.surface);
    loadMono(18);
    tft.drawString("Nothing here yet", 120, 128);
    drawWrappedCenteredText(message, 120, 154, 184, FONT_INFO, theme.muted, theme.surface, 2);
}

// ---------------------------------------------------------------------------
// Shared page furniture
//
// The rotating pages use one spacing scale so they read as a single design:
// a 16 px margin on every side, a 26 px title (the largest letters these
// bitmap fonts carry), a short accent rule under it, and cards that never sit
// closer than 16 px to each other.
// ---------------------------------------------------------------------------
constexpr int kCardMargin = 16;
constexpr int kHeadingTextY = 12;
constexpr int kAccentRuleY = 50;
constexpr int kAccentRuleWidth = 46;

uint16_t rgbToColor(uint8_t red, uint8_t green, uint8_t blue) {
    return static_cast<uint16_t>(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3));
}

// Each page carries its own accent so a rotation step is felt, not just read.
uint16_t pageAccentColor(uint8_t pageId) {
    switch (pageId) {
        case DASHBOARD_PAGE_RIVER:
            return rgbToColor(0x35, 0xC8, 0xE8);
        case DASHBOARD_PAGE_GITHUB:
            return rgbToColor(0x58, 0xA6, 0xFF);
        case DASHBOARD_PAGE_WEATHER:
            return rgbToColor(0xFF, 0xB4, 0x54);
        default:
            return activeTheme().accent;
    }
}

// These fonts stop at ASCII 126, so there is no degree glyph to print. Drawing
// the ring keeps the unit legible and lets it match the page accent.
void drawDegreeRing(int x, int y, int radius, uint16_t color, uint16_t background) {
    // At small radii a two-pixel wall closes the ring up, so the hole scales.
    tft.fillCircle(x, y, radius, color);
    tft.fillCircle(x, y, radius <= 3 ? radius - 1 : radius - 2, background);
}

void drawThickLine(int x0, int y0, int x1, int y1, int thickness, uint16_t color) {
    int half = thickness / 2;
    for (int offset = -half; offset <= half; ++offset) {
        tft.drawLine(x0 + offset, y0, x1 + offset, y1, color);
        tft.drawLine(x0, y0 + offset, x1, y1 + offset, color);
    }
}

// The hero reading uses its own larger anti-aliased VLW face, drawn from the
// vertical centre so the group sits in the middle of its card.
void drawHeroReading(const String &value,
                     char unit,
                     int centerX,
                     int centerY,
                     uint16_t textColor,
                     uint16_t accentColor,
                     uint16_t background) {
    // The reading itself is centred on the screen; the degree sign and unit hang
    // off its right edge. Centring the pair instead would push the number left
    // of centre, which is what the design avoids.
    // The big reading uses the anti-aliased VLW face (like the clock) so it
    // does not stair-step; it is drawn once per page render, never animated.
    tft.loadFont(HeroVlw);
    tft.setTextColor(textColor, background);
    int valueWidth = tft.textWidth(value);
    int startX = centerX - (valueWidth / 2);

    tft.setTextDatum(ML_DATUM);
    tft.drawString(value, startX, centerY);
    tft.unloadFont();

    int unitX = startX + valueWidth + 6;
    drawDegreeRing(unitX + 6, centerY - 14, 5, accentColor, background);
    loadMono(18);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(accentColor, background);
    tft.drawString(String(unit), unitX + 14, centerY + 4);
}

// Small readings print the degree as a drawn ring, matching the design; the
// bitmap fonts have no such glyph.
void drawSmallDegreeValue(const String &value, int centerX, int y, uint16_t color, uint16_t accentColor, uint16_t background) {
    loadMono(18);
    int valueWidth = tft.textWidth(value);
    int startX = centerX - ((valueWidth + 9) / 2);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(color, background);
    tft.drawString(value, startX, y);
    drawDegreeRing(startX + valueWidth + 4, y + 4, 3, accentColor, background);
}

void drawAccentRule(int x, uint16_t color) {
    tft.fillRect(x, kAccentRuleY, kAccentRuleWidth, 2, color);
}

void drawSunGlyph(int cx, int cy, int radius, uint16_t color) {
    tft.fillCircle(cx, cy, radius, color);
    for (int index = 0; index < 8; ++index) {
        float angle = index * PI / 4.0f;
        drawThickLine(cx + static_cast<int>(lroundf(cosf(angle) * (radius + 4))),
                      cy + static_cast<int>(lroundf(sinf(angle) * (radius + 4))),
                      cx + static_cast<int>(lroundf(cosf(angle) * (radius + 8))),
                      cy + static_cast<int>(lroundf(sinf(angle) * (radius + 8))),
                      2,
                      color);
    }
}

void drawCloudGlyph(int cx, int cy, uint16_t color) {
    tft.fillCircle(cx - 7, cy + 2, 6, color);
    tft.fillCircle(cx + 1, cy - 3, 8, color);
    tft.fillCircle(cx + 9, cy + 2, 6, color);
    tft.fillRect(cx - 7, cy + 2, 17, 6, color);
}

// Picks a glyph from the Open-Meteo condition wording rather than the raw code,
// so it keeps working whatever phrasing the feed returns.
void drawWeatherMark(const char *condition, int cx, int cy, uint16_t color) {
    String text(condition);
    text.toLowerCase();

    if (text.indexOf("thunder") >= 0 || text.indexOf("storm") >= 0) {
        drawCloudGlyph(cx, cy - 3, color);
        tft.fillTriangle(cx - 3, cy + 8, cx + 4, cy + 8, cx, cy + 16, rgbToColor(0xFF, 0xE0, 0x6A));
        return;
    }

    if (text.indexOf("snow") >= 0) {
        drawCloudGlyph(cx, cy - 3, color);
        for (int index = -1; index <= 1; ++index) {
            tft.fillCircle(cx + (index * 7), cy + 12, 2, color);
        }
        return;
    }

    if (text.indexOf("rain") >= 0 || text.indexOf("drizzle") >= 0 || text.indexOf("shower") >= 0) {
        drawCloudGlyph(cx, cy - 3, color);
        for (int index = -1; index <= 1; ++index) {
            drawThickLine(cx + (index * 7), cy + 9, cx + (index * 7) - 2, cy + 15, 2, color);
        }
        return;
    }

    if (text.indexOf("fog") >= 0) {
        for (int index = 0; index < 3; ++index) {
            drawThickLine(cx - 11, cy - 4 + (index * 6), cx + 11, cy - 4 + (index * 6), 2, color);
        }
        return;
    }

    if (text.indexOf("cloud") >= 0 || text.indexOf("overcast") >= 0) {
        drawCloudGlyph(cx, cy, color);
        return;
    }

    drawSunGlyph(cx, cy, 7, color);
}

const WeatherData* effectiveWeatherData() {
    return feedsWeatherSource() == WEATHER_FEED_OPEN_METEO ? feedsWeatherData() : nullptr;
}

bool weatherWaitingForSync() {
    return feedsWeatherSource() == WEATHER_FEED_OPEN_METEO &&
           feedsWeatherConfigured() &&
           !feedsHasWeatherData();
}

char weatherUnitSymbol() {
    return feedsWeatherUsesFahrenheit() ? 'F' : 'C';
}

String formatWeatherTemperature(int value) {
    return String(value) + weatherUnitSymbol();
}

bool hasWeatherContent() {
    const WeatherData *weather = effectiveWeatherData();
    return weather != nullptr &&
           (weather->location[0] != '\0' ||
            weather->condition[0] != '\0' ||
            feedsHasWeatherData());
}

const char* activeClockMessage() {
    if (displayState.line2[0] == '\0') {
        return nullptr;
    }

    if (strcmp(displayState.line2, "Dashboard preview ready") == 0) {
        return nullptr;
    }

    return displayState.line2;
}
String formatEventAge(uint32_t createdAt);


bool hasRiverContent() {
    return feedsHasRiverData();
}

bool riverWaitingForSync() {
    return feedsRiverConfigured() && !feedsHasRiverData();
}

bool hasGithubContent() {
    return feedsHasGithubData();
}

bool githubWaitingForSync() {
    return feedsGithubConfigured() && !feedsHasGithubData();
}

bool dashboardPageHasRenderableContent(uint8_t pageId) {
    switch (pageId) {
        case DASHBOARD_PAGE_CLOCK:
            return true;
        case DASHBOARD_PAGE_WEATHER:
            return hasWeatherContent() || weatherWaitingForSync();
        case DASHBOARD_PAGE_RIVER:
            return hasRiverContent() || riverWaitingForSync();
        case DASHBOARD_PAGE_GITHUB:
            return hasGithubContent() || githubWaitingForSync();
        default:
            return false;
    }
}

bool dashboardPageAvailable(uint8_t pageId) {
    return dashboardPageEnabled(pageId) && dashboardPageHasRenderableContent(pageId);
}

uint8_t firstAvailableDashboardPage() {
    for (uint8_t pageId = 0; pageId < DASHBOARD_PAGE_COUNT; ++pageId) {
        if (dashboardPageAvailable(pageId)) {
            return pageId;
        }
    }

    return dashboardFirstEnabledPage();
}

uint8_t nextAvailableDashboardPage(uint8_t currentPage) {
    for (uint8_t offset = 1; offset <= DASHBOARD_PAGE_COUNT; ++offset) {
        uint8_t pageId = (currentPage + offset) % DASHBOARD_PAGE_COUNT;
        if (dashboardPageAvailable(pageId)) {
            return pageId;
        }
    }

    if (dashboardPageEnabled(currentPage)) {
        return currentPage;
    }

    return dashboardFirstEnabledPage();
}

void hashWeatherData(uint32_t &hash) {
    hashValue(hash, feedsWeatherSource());
    hashValue(hash, feedsWeatherUsesFahrenheit());
    hashValue(hash, weatherWaitingForSync());

    const WeatherData *weather = effectiveWeatherData();
    bool hasData = weather != nullptr;
    hashValue(hash, hasData);
    if (!hasData) {
        return;
    }

    hashCString(hash, weather->location);
    hashCString(hash, weather->condition);
    hashValue(hash, weather->temperature);
    hashValue(hash, weather->high);
    hashValue(hash, weather->low);
    hashValue(hash, weather->rainChance);
}

void hashCommonPageState(uint32_t &hash, uint8_t pageId) {
    hashValue(hash, pageId);
    hashThemeConfig(hash);
    hashValue(hash, dashboardConfig.showIp);
    hashCString(hash, displayState.ipInfo);
}

uint32_t dashboardStaticHash(uint8_t pageId) {
    uint32_t hash = kFnvOffset;
    hashCommonPageState(hash, pageId);

    switch (pageId) {
        case DASHBOARD_PAGE_CLOCK:
            hashValue(hash, dashboardConfig.use24Hour);
            hashValue(hash, dashboardConfig.showSeconds);
            hashValue(hash, hasWeatherContent());
            hashValue(hash, weatherWaitingForSync());
            hashValue(hash, activeClockMessage() != nullptr);
            if (hasWeatherContent()) {
                hashWeatherData(hash);
            } else if (activeClockMessage() != nullptr) {
                hashCString(hash, activeClockMessage());
            }
            break;
        case DASHBOARD_PAGE_WEATHER:
            hashWeatherData(hash);
            break;
        case DASHBOARD_PAGE_RIVER: {
            const RiverData *river = feedsRiverData();
            hashValue(hash, river != nullptr);
            if (river != nullptr) {
                hashCString(hash, river->station);
                hashCString(hash, river->observedAt);
                hashValue(hash, river->temperature);
            }
            // The page also shows the air temperature next to the water value.
            hashWeatherData(hash);
            break;
        }
        case DASHBOARD_PAGE_GITHUB: {
            const GithubData *github = feedsGithubData();
            hashValue(hash, github != nullptr);
            if (github != nullptr) {
                hashValue(hash, github->cursor);
                const GithubEventEntry &event = github->events[github->cursor % github->count];
                hashCString(hash, event.kind);
                hashCString(hash, event.repo);
                hashCString(hash, event.actor);
                hashCString(hash, event.detail);
                // The relative age ("3 min ago") is deliberately not hashed: it
                // ticks every minute, and hashing it forced a full-screen
                // repaint each time, which read as a flicker. It is drawn once
                // when the page is shown (accurate then) and refreshed on the
                // next visit, when the cursor advances anyway.
            }
            break;
        }
        default:
            break;
    }

    return hash;
}

uint32_t dashboardDynamicHash(uint8_t pageId) {
    uint32_t hash = kFnvOffset;
    hashValue(hash, pageId);

    switch (pageId) {
        case DASHBOARD_PAGE_CLOCK: {
            bool isPm = false;
            String timeText = formatLocalTime(dashboardConfig.showSeconds, dashboardConfig.use24Hour, &isPm);
            String metaLine = formatClockMetaLine(isPm);
            hashCString(hash, timeText.c_str());
            hashCString(hash, metaLine.c_str());
            hashValue(hash, isPm);
            break;
        }
        case DASHBOARD_PAGE_RIVER: {
            // Mirrors riverWavePhase(); folding it into the hash is what drives
            // the water animation through the existing partial-redraw path.
            hashValue(hash, static_cast<uint8_t>((millis() / 200UL) % 32UL));
            break;
        }
        default:
            break;
    }

    return hash;
}

uint32_t apScreenHash() {
    uint32_t hash = kFnvOffset;
    hashThemeConfig(hash);
    hashCString(hash, displayState.ipInfo);
    hashCString(hash, displayState.apSSID);
    hashCString(hash, displayState.apPassword);
    hashValue(hash, authCanRevealPassword());
    hashCString(hash, authProvisionedPassword());
    return hash;
}

uint32_t temporaryMessageHash() {
    uint32_t hash = kFnvOffset;
    hashThemeConfig(hash);
    hashValue(hash, displayState.apMode);
    hashValue(hash, dashboardConfig.showIp);
    hashCString(hash, displayState.ipInfo);
    hashCString(hash, temporaryMessage.message);
    return hash;
}

bool pageUsesDynamicRefresh(uint8_t pageId) {
    return pageId == DASHBOARD_PAGE_CLOCK ||
           pageId == DASHBOARD_PAGE_RIVER;
}

bool ensureClockDynamicSprite() {
    if (!clockDynamicSpriteAllowed) {
        return false;
    }

    if (clockDynamicSpriteReady) {
        return true;
    }

    if (clockDynamicSpriteAttempted) {
        return false;
    }

    clockDynamicSpriteAttempted = true;
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < kClockSpriteMinFreeHeapBytes) {
        logPrintf("Clock sprite skipped, free heap too low: %u", freeHeap);
        return false;
    }

    clockDynamicSprite.deleteSprite();
    clockDynamicSprite.setColorDepth(4);
    clockDynamicSpriteReady =
        clockDynamicSprite.createSprite(kClockDynamicRegionWidth, kClockDynamicRegionHeight) != nullptr;

    if (clockDynamicSpriteReady) {
        logPrintf("Clock sprite ready (%dx%d), free heap now %u",
                  kClockDynamicRegionWidth,
                  kClockDynamicRegionHeight,
                  ESP.getFreeHeap());
    } else {
        logPrintf("Clock sprite allocation failed, free heap was %u", freeHeap);
    }

    return clockDynamicSpriteReady;
}

void updateClockDynamicArea() {
    const ThemePalette &theme = activeTheme();
    bool isPm = false;
    String timeText = formatLocalTime(dashboardConfig.showSeconds, dashboardConfig.use24Hour, &isPm);
    String metaLine = formatClockMetaLine(isPm);
    // The clock page no longer sits on a panel, so the repaint colour is the
    // page background rather than the card surface.
    uint16_t dynamicBackground = theme.background;

    // Redraw the time only when it changes (once a minute for HH:MM) so drawing
    // straight onto the panel does not flash. renderClockPage() clears the cache
    // to force a full repaint on page entry.
    if (!clockMetaCache.valid || strcmp(clockMetaCache.timeText, timeText.c_str()) != 0) {
        tft.fillRect(kClockDynamicRegionX,
                     kClockDynamicRegionY,
                     kClockDynamicRegionWidth,
                     kClockDynamicRegionHeight,
                     dynamicBackground);
        drawClockTime(timeText,
                      dashboardConfig.showSeconds,
                      theme.text,
                      dynamicBackground,
                      120,
                      kClockTimeY);
        strncpy(clockMetaCache.timeText, timeText.c_str(), sizeof(clockMetaCache.timeText) - 1);
        clockMetaCache.timeText[sizeof(clockMetaCache.timeText) - 1] = '\0';
    }

    if (!clockMetaCache.valid || strcmp(clockMetaCache.metaLine, metaLine.c_str()) != 0) {
        loadMono(18);
        int metaWidth = tft.textWidth(metaLine) + 12;
        if (clockMetaCache.valid) {
            metaWidth = max(metaWidth, tft.textWidth(clockMetaCache.metaLine) + 12);
        }

        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(theme.muted, dynamicBackground);
        tft.setTextPadding(metaWidth);
        tft.drawString(metaLine, 120, kClockMetaY);
        tft.setTextPadding(0);
        updateClockMetaCache(metaLine);
    }
}

void renderClockPage() {
    const ThemePalette &theme = activeTheme();
    tft.fillScreen(theme.background);
    clockMetaCache.valid = false;
    updateClockDynamicArea();

    tft.fillRect(97, 166, kAccentRuleWidth, 2, rgbToColor(0x6E, 0xE7, 0xC0));

    const char *clockMessage = activeClockMessage();
    if (hasWeatherContent()) {
        // Laid out as one centred group: weather glyph, reading, degree, unit.
        const WeatherData *weather = effectiveWeatherData();
        const uint16_t weatherAccent = pageAccentColor(DASHBOARD_PAGE_WEATHER);
        const int rowCenterY = 196;

        String valueText = String(weather->temperature);
        loadMono(18);
        int valueWidth = tft.textWidth(valueText);
        int unitWidth = 10 + tft.textWidth(String(weatherUnitSymbol()));
        int groupWidth = 30 + valueWidth + unitWidth;
        int groupX = 120 - (groupWidth / 2);

        // The design marks the weather here with a plain dot; the descriptive
        // glyph belongs on the weather page.
        tft.fillCircle(groupX + 8, rowCenterY, 7, weatherAccent);

        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(theme.text, theme.background);
        tft.drawString(valueText, groupX + 30, rowCenterY);

        int unitX = groupX + 30 + valueWidth;
        drawDegreeRing(unitX + 5, rowCenterY - 5, 4, weatherAccent, theme.background);
        tft.setTextColor(weatherAccent, theme.background);
        tft.drawString(String(weatherUnitSymbol()), unitX + 12, rowCenterY);

    } else if (weatherWaitingForSync()) {
        drawWrappedCenteredText("Syncing weather...", 120, 184, 192, FONT_LABEL, theme.muted, theme.background, 2);
    } else if (clockMessage != nullptr) {
        drawWrappedCenteredText(clockMessage, 120, 180, 192, FONT_LABEL, theme.text, theme.background, 2);
    }
}

void renderWeatherPage() {
    const ThemePalette &theme = activeTheme();
    if (!hasWeatherContent()) {
        if (weatherWaitingForSync()) {
            drawPlaceholder("Weather", "Syncing live data.");
            return;
        }
        drawPlaceholder("Weather", "Add weather data.");
        return;
    }

    const WeatherData *weather = effectiveWeatherData();
    const uint16_t accent = pageAccentColor(DASHBOARD_PAGE_WEATHER);

    tft.fillScreen(theme.background);

    drawWeatherMark(weather->condition, 30, 26, accent);
    tft.setTextDatum(TL_DATUM);
    loadMono(26);
    tft.setTextColor(theme.text, theme.background);
    tft.drawString(weather->location[0] != '\0' ? weather->location : "WEATHER",
                   kCardMargin + 40,
                   kHeadingTextY);
    drawAccentRule(kCardMargin + 2, accent);

    tft.fillRoundRect(kCardMargin, 66, 240 - (kCardMargin * 2), 94, 14, theme.surface);

    drawHeroReading(String(weather->temperature), weatherUnitSymbol(), 120, 104, theme.text, accent, theme.surface);

    // The design sets the condition in caps, e.g. CLEAR SKY.
    String conditionText(weather->condition[0] != '\0' ? weather->condition : "Updated");
    conditionText.toUpperCase();

    const int conditionFonts[] = {FONT_LABEL, FONT_INFO};
    drawAdaptiveText(conditionText,
                     120,
                     134,
                     190,
                     TC_DATUM,
                     conditionFonts,
                     sizeof(conditionFonts) / sizeof(conditionFonts[0]),
                     theme.muted,
                     theme.surface);

    struct WeatherCell {
        const char *label;
        String value;
        bool showsDegree;
    };
    const WeatherCell cells[] = {
        {"HIGH", String(weather->high), true},
        {"LOW", String(weather->low), true},
        {"RAIN", String(weather->rainChance) + "%", false},
    };

    const int cellWidth = 66;
    const int cellGap = 5;
    for (int index = 0; index < 3; ++index) {
        int cellX = kCardMargin + (index * (cellWidth + cellGap));
        tft.fillRoundRect(cellX, 176, cellWidth, 48, 10, theme.surfaceAlt);

        tft.setTextDatum(TC_DATUM);
        loadMono(13);
        tft.setTextColor(theme.muted, theme.surfaceAlt);
        tft.drawString(cells[index].label, cellX + (cellWidth / 2), 186);

        if (cells[index].showsDegree) {
            drawSmallDegreeValue(cells[index].value, cellX + (cellWidth / 2), 202, theme.text, accent, theme.surfaceAlt);
        } else {
            tft.setTextDatum(TC_DATUM);
            loadMono(18);
            tft.setTextColor(theme.text, theme.surfaceAlt);
            tft.drawString(cells[index].value, cellX + (cellWidth / 2), 202);
        }
    }
}

// ---------------------------------------------------------------------------
// Han River page
// ---------------------------------------------------------------------------
constexpr int kRiverCardX = kCardMargin;
constexpr int kRiverCardY = 68;
constexpr int kRiverCardW = 240 - (kCardMargin * 2);
constexpr int kRiverCardH = 118;
constexpr int kRiverCardRadius = 16;
// The water band starts below the label. Anything drawn inside this band is
// repainted every second by the animation, so static text must stay above it.
constexpr int kRiverWaterTop = 136;
constexpr int kRiverLabelY = 164;   // caption baseline area, sitting on the water

// One full wave cycle per 32 ticks at 5 Hz, so the surface drifts across in
// about six seconds. The step count has to divide the cycle exactly or the
// water jumps when the counter wraps.
constexpr uint8_t kRiverWavePhaseSteps = 32;

uint8_t riverWavePhase() {
    return static_cast<uint8_t>((millis() / 200UL) % kRiverWavePhaseSteps);
}

// The water is tinted by the reading itself: icy blue through winter, cyan and
// teal in the shoulder seasons, amber once the river is genuinely warm. The
// page can then be read at a glance, before the number registers.
void riverWaterColors(float celsius, uint16_t &deep, uint16_t &crest, uint16_t &accent) {
    struct TemperatureBand {
        float below;
        uint8_t red;
        uint8_t green;
        uint8_t blue;
    };

    // Two ramps. The water stays inside the blue-to-teal family, because a
    // warm hue dimmed for the surface turns muddy brown and stops reading as
    // water at all. The accent carries the full cold-to-hot range instead.
    // Blue always stays ahead of green: the moment green leads, the surface
    // reads as mint rather than water. Temperature only slides the balance.
    static const TemperatureBand kWaterBands[] = {
        {5.0f, 0x18, 0x34, 0xB4},
        {10.0f, 0x1A, 0x42, 0xAC},
        {15.0f, 0x1C, 0x4E, 0xA4},
        {20.0f, 0x1E, 0x5A, 0xA0},
        {24.0f, 0x20, 0x64, 0x9E},
        {27.0f, 0x22, 0x6C, 0x9E},
        {999.0f, 0x24, 0x74, 0xA0},
    };

    static const TemperatureBand kAccentBands[] = {
        {5.0f, 0x6E, 0xB4, 0xFF},
        {10.0f, 0x4F, 0xC4, 0xF0},
        {15.0f, 0x35, 0xC8, 0xE8},
        {20.0f, 0x46, 0xD6, 0xB4},
        {24.0f, 0x9A, 0xD8, 0x60},
        {27.0f, 0xE8, 0xC0, 0x4A},
        {999.0f, 0xFF, 0x9A, 0x50},
    };

    auto pick = [celsius](const TemperatureBand *bands, size_t count) {
        for (size_t index = 0; index < count; ++index) {
            if (celsius < bands[index].below) {
                return &bands[index];
            }
        }
        return &bands[count - 1];
    };

    const TemperatureBand *water = pick(kWaterBands, sizeof(kWaterBands) / sizeof(kWaterBands[0]));
    const TemperatureBand *tint = pick(kAccentBands, sizeof(kAccentBands) / sizeof(kAccentBands[0]));

    auto scale = [](uint8_t channel, float factor) {
        return static_cast<uint8_t>(lroundf(channel * factor));
    };

    accent = rgbToColor(tint->red, tint->green, tint->blue);
    crest = rgbToColor(scale(water->red, 0.46f), scale(water->green, 0.46f), scale(water->blue, 0.46f));
    deep = rgbToColor(scale(water->red, 0.26f), scale(water->green, 0.26f), scale(water->blue, 0.26f));
}

// Repaints only the water band. The temperature above it is left untouched, so
// the animation costs one strip of vertical lines and never flickers the value.
void drawRiverWater(uint8_t phase) {
    const ThemePalette &theme = activeTheme();
    const RiverData *river = feedsRiverData();
    uint16_t deep = 0;
    uint16_t crest = 0;
    uint16_t accent = 0;
    riverWaterColors(river != nullptr ? river->temperature : 15.0f, deep, crest, accent);

    const int left = kRiverCardX + 2;
    const int right = kRiverCardX + kRiverCardW - 2;
    const int bottom = kRiverCardY + kRiverCardH - 2;
    const int cornerRadius = kRiverCardRadius - 2;

    // The caption sits on the water, as designed. Its box is skipped during the
    // repaint so the animation never erases and redraws it - that flicker was
    // the whole reason it had been moved off the water before.
    loadMono(16);
    int labelWidth = tft.textWidth("WATER TEMP");
    // Match the caption box exactly (fillRect at 120-w/2-7, width w+14, so its
    // last column is 120+w/2+6). Keeping these in step stops the water either
    // showing the card through a seam or clipping the caption's edge letters.
    const int labelLeft = 120 - (labelWidth / 2) - 7;
    const int labelRight = 120 + (labelWidth / 2) + 6;
    const int labelTop = kRiverLabelY - 1;
    const int labelBottom = kRiverLabelY + 17;

    // Draws a vertical run, skipping the caption box where they overlap.
    auto fillColumn = [&](int x, int top, int columnBottom, uint16_t color) {
        if (top > columnBottom) {
            return;
        }
        if (x < labelLeft || x > labelRight) {
            tft.drawFastVLine(x, top, columnBottom - top + 1, color);
            return;
        }
        if (top < labelTop) {
            tft.drawFastVLine(x, top, min(labelTop, columnBottom + 1) - top, color);
        }
        if (columnBottom > labelBottom) {
            int start = max(top, labelBottom + 1);
            tft.drawFastVLine(x, start, columnBottom - start + 1, color);
        }
    };


    for (int x = left; x < right; ++x) {
        // Follow the card's rounded bottom so the water never spills outside it.
        int edgeDistance = min(x - left, right - 1 - x);
        int columnBottom = bottom;
        if (edgeDistance < cornerRadius) {
            int dx = cornerRadius - edgeDistance;
            int lift = cornerRadius -
                       static_cast<int>(lroundf(sqrtf(static_cast<float>(
                           (cornerRadius * cornerRadius) - (dx * dx)))));
            columnBottom = bottom - lift;
        }

        // Both layers advance by the same phase each frame so they wrap cleanly
        // together; only their wavelengths differ, which is what gives the
        // surface its depth.
        float travel = static_cast<float>(phase) * (2.0f * PI / kRiverWavePhaseSteps);
        float column = static_cast<float>(x - left);
        int backTop = kRiverWaterTop + 4 + static_cast<int>(lroundf(sinf((column / 34.0f) + travel + 1.9f) * 3.0f));
        int frontTop = kRiverWaterTop + 11 + static_cast<int>(lroundf(sinf((column / 24.0f) + travel) * 4.0f));

        fillColumn(x, kRiverWaterTop, columnBottom, theme.surface);
        fillColumn(x, backTop, columnBottom, deep);
        fillColumn(x, frontTop, columnBottom, crest);
    }
}

void updateRiverDynamicArea() {
    if (!hasRiverContent()) {
        return;
    }

    drawRiverWater(riverWavePhase());
}

void renderRiverPage() {
    const ThemePalette &theme = activeTheme();
    if (!hasRiverContent()) {
        if (riverWaitingForSync()) {
            drawPlaceholder("Han River", "Syncing water temp.");
            return;
        }
        drawPlaceholder("Han River", "Enable the river feed.");
        return;
    }

    const RiverData *river = feedsRiverData();

    // The whole page takes its accent from the reading, so the dot, the degree
    // sign and the water all shift together as the river warms or cools.
    uint16_t waterDeep = 0;
    uint16_t waterCrest = 0;
    uint16_t accent = 0;
    riverWaterColors(river->temperature, waterDeep, waterCrest, accent);

    tft.fillScreen(theme.background);

    tft.setTextDatum(TC_DATUM);
    loadMono(26);
    tft.setTextColor(theme.text, theme.background);
    tft.drawString("HAN RIVER", 120, kHeadingTextY);

    // Station and observation hour share one line, separated by a drawn dot
    // because the fonts have no middle-dot glyph.
    String station = river->station[0] != '\0' ? String(river->station) : String("HAN RIVER");
    String observed = river->observedAt[0] != '\0' ? String(river->observedAt) : String("--");
    loadMono(13);
    int stationWidth = tft.textWidth(station);
    int observedWidth = tft.textWidth(observed);
    int subtitleX = 120 - ((stationWidth + 16 + observedWidth) / 2);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(theme.muted, theme.background);
    tft.drawString(station, subtitleX, 42);
    tft.fillCircle(subtitleX + stationWidth + 8, 48, 2, accent);
    tft.drawString(observed, subtitleX + stationWidth + 16, 42);

    tft.fillRoundRect(kRiverCardX, kRiverCardY, kRiverCardW, kRiverCardH, kRiverCardRadius, theme.surface);

    drawHeroReading(String(river->temperature, 1), 'C', 120, 106, theme.text, accent, theme.surface);

    drawRiverWater(riverWavePhase());

    // The caption sits inside the water band, but the animation skips its box
    // entirely, so it is painted once here. Repainting it every frame is what
    // made it flicker.
    loadMono(16);
    int captionWidth = tft.textWidth("WATER TEMP");
    tft.fillRect(120 - (captionWidth / 2) - 7,
                 kRiverLabelY - 1,
                 captionWidth + 14,
                 19,
                 waterCrest);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(rgbToColor(0xD5, 0xF0, 0xF7), waterCrest);
    tft.drawString("WATER TEMP", 120, kRiverLabelY);

    // The comparison with the air temperature is the point of the page, so the
    // difference is spelled out rather than left for the reader to subtract.
    const WeatherData *weather = effectiveWeatherData();
    bool haveAir = weather != nullptr && feedsHasWeatherData();

    tft.setTextDatum(TL_DATUM);
    loadMono(13);
    tft.setTextColor(theme.muted, theme.background);
    tft.drawString("AIR", kCardMargin + 20, 200);

    if (haveAir) {
        float delta = river->temperature - static_cast<float>(weather->temperature);
        bool cooler = delta < 0.0f;
        uint16_t deltaColor = cooler ? accent : theme.warning;
        int arrowX = kCardMargin + 2;

        if (cooler) {
            tft.fillTriangle(arrowX, 200, arrowX + 12, 200, arrowX + 6, 210, deltaColor);
        } else {
            tft.fillTriangle(arrowX, 210, arrowX + 12, 210, arrowX + 6, 200, deltaColor);
        }

        // Air reading and the difference read as one phrase on the right, as
        // designed, rather than being pushed to opposite edges.
        String deltaText = String(" / ") + (delta > 0 ? String("+") : String("")) + String(delta, 1);
        String airText = String(weather->temperature);

        loadMono(18);
        int airWidth = tft.textWidth(airText);
        int deltaWidth = tft.textWidth(deltaText);
        int groupX = 240 - kCardMargin - airWidth - 11 - deltaWidth;

        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(theme.text, theme.background);
        tft.drawString(airText, groupX, 194);
        drawDegreeRing(groupX + airWidth + 5, 200, 4, accent, theme.background);
        tft.setTextColor(deltaColor, theme.background);
        tft.drawString(deltaText, groupX + airWidth + 11, 194);
    }
}

// ---------------------------------------------------------------------------
// GitHub page
// ---------------------------------------------------------------------------
void drawStarIcon(int cx, int cy, int radius, uint16_t color) {
    int16_t pointX[10];
    int16_t pointY[10];
    for (int index = 0; index < 10; ++index) {
        float pointRadius = (index % 2 == 0) ? static_cast<float>(radius) : radius * 0.44f;
        float angle = -PI / 2.0f + (index * PI / 5.0f);
        pointX[index] = cx + static_cast<int16_t>(lroundf(cosf(angle) * pointRadius));
        pointY[index] = cy + static_cast<int16_t>(lroundf(sinf(angle) * pointRadius));
    }

    for (int index = 0; index < 10; ++index) {
        int next = (index + 1) % 10;
        tft.fillTriangle(cx, cy, pointX[index], pointY[index], pointX[next], pointY[next], color);
    }
}

void drawForkIcon(int cx, int cy, uint16_t color) {
    drawThickLine(cx - 6, cy - 12, cx - 6, cy + 2, 3, color);
    drawThickLine(cx - 6, cy + 2, cx + 6, cy + 6, 3, color);
    tft.fillCircle(cx - 6, cy - 14, 4, color);
    tft.fillCircle(cx - 6, cy + 12, 4, color);
    tft.fillCircle(cx + 8, cy + 8, 4, color);
}

void drawPullRequestIcon(int cx, int cy, uint16_t color) {
    drawThickLine(cx - 7, cy - 10, cx - 7, cy + 8, 3, color);
    drawThickLine(cx + 7, cy - 8, cx + 7, cy + 6, 3, color);
    tft.fillCircle(cx - 7, cy - 13, 4, color);
    tft.fillCircle(cx + 7, cy + 10, 4, color);
    tft.fillCircle(cx + 7, cy - 12, 4, color);
}

void drawIssueIcon(int cx, int cy, uint16_t color) {
    tft.fillCircle(cx, cy, 13, color);
    tft.fillCircle(cx, cy, 9, activeTheme().surface);
    tft.fillCircle(cx, cy, 4, color);
}

void drawCommentIcon(int cx, int cy, uint16_t color) {
    tft.fillRoundRect(cx - 15, cy - 11, 30, 20, 5, color);
    tft.fillTriangle(cx - 6, cy + 8, cx + 2, cy + 8, cx - 8, cy + 16, color);
}

void drawPushIcon(int cx, int cy, uint16_t color) {
    drawThickLine(cx, cy - 2, cx, cy + 13, 4, color);
    tft.fillTriangle(cx - 10, cy - 2, cx + 10, cy - 2, cx, cy - 14, color);
}

void drawReviewIcon(int cx, int cy, uint16_t color) {
    tft.fillCircle(cx, cy, 13, color);
    tft.fillCircle(cx, cy, 10, activeTheme().surface);
    drawThickLine(cx - 6, cy, cx - 2, cy + 5, 3, color);
    drawThickLine(cx - 2, cy + 5, cx + 6, cy - 5, 3, color);
}

void drawReleaseIcon(int cx, int cy, uint16_t color) {
    tft.fillRect(cx - 12, cy - 2, 24, 14, color);
    tft.fillRect(cx - 8, cy + 1, 16, 8, activeTheme().surface);
    tft.fillTriangle(cx - 14, cy - 2, cx + 14, cy - 2, cx, cy - 15, color);
}

void drawGithubEventIcon(const char *kind, int cx, int cy) {
    if (strcmp(kind, "STAR") == 0) {
        drawStarIcon(cx, cy, 19, rgbToColor(0xF2, 0xC7, 0x44));
    } else if (strcmp(kind, "FORK") == 0) {
        drawForkIcon(cx, cy, rgbToColor(0x58, 0xA6, 0xFF));
    } else if (strcmp(kind, "PULL REQ") == 0) {
        drawPullRequestIcon(cx, cy, rgbToColor(0xA4, 0xF0, 0xB8));
    } else if (strcmp(kind, "ISSUE") == 0) {
        drawIssueIcon(cx, cy, rgbToColor(0xA4, 0xF0, 0xB8));
    } else if (strcmp(kind, "COMMENT") == 0) {
        drawCommentIcon(cx, cy, rgbToColor(0x9C, 0x8C, 0xF0));
    } else if (strcmp(kind, "REVIEW") == 0) {
        drawReviewIcon(cx, cy, rgbToColor(0xF2, 0xA3, 0x5A));
    } else if (strcmp(kind, "RELEASE") == 0) {
        drawReleaseIcon(cx, cy, rgbToColor(0xC0, 0xCA, 0xD3));
    } else {
        drawPushIcon(cx, cy, rgbToColor(0x7D, 0xE3, 0xF0));
    }
}

// The three dots and two strokes of a branch, used as the page mark.
void drawBranchMark(int x, int y, uint16_t color) {
    drawThickLine(x, y + 3, x, y + 17, 3, color);
    drawThickLine(x, y + 14, x + 12, y + 18, 3, color);
    tft.fillCircle(x, y, 4, color);
    tft.fillCircle(x, y + 20, 4, color);
    tft.fillCircle(x + 14, y + 18, 4, color);
}

// GitHub publishes its event feed a few minutes late, so the age is shown as a
// real figure rather than a vague "just now" that would overstate freshness.
String formatEventAge(uint32_t createdAt) {
    if (createdAt == 0 || !hasTimeSync()) {
        return String("recently");
    }

    uint32_t now = static_cast<uint32_t>(time(nullptr));
    if (now <= createdAt) {
        return String("just now");
    }

    uint32_t seconds = now - createdAt;
    if (seconds < 90) {
        return String("just now");
    }
    if (seconds < 3600) {
        return String(static_cast<int>(seconds / 60)) + " min ago";
    }
    if (seconds < 86400) {
        int hours = static_cast<int>(seconds / 3600);
        return String(hours) + (hours == 1 ? " hour ago" : " hours ago");
    }

    int days = static_cast<int>(seconds / 86400);
    return String(days) + (days == 1 ? " day ago" : " days ago");
}


void renderGithubPage() {
    const ThemePalette &theme = activeTheme();
    if (!hasGithubContent()) {
        if (githubWaitingForSync()) {
            drawPlaceholder("GitHub", "Syncing live events.");
            return;
        }
        drawPlaceholder("GitHub", "Enable the GitHub feed.");
        return;
    }

    const GithubData *github = feedsGithubData();
    const GithubEventEntry &event = github->events[github->cursor % github->count];
    const uint16_t accent = pageAccentColor(DASHBOARD_PAGE_GITHUB);

    // "owner/name" does not fit on one line, and the name is the part worth
    // reading, so the owner is demoted to a small line above it.
    String repo(event.repo);
    String owner;
    String name = repo;
    int slashIndex = repo.indexOf('/');
    if (slashIndex > 0) {
        owner = repo.substring(0, slashIndex);
        name = repo.substring(slashIndex + 1);
    }

    tft.fillScreen(theme.background);

    drawBranchMark(kCardMargin + 6, 14, accent);
    tft.setTextDatum(TL_DATUM);
    loadMono(26);
    tft.setTextColor(theme.text, theme.background);
    tft.drawString("GITHUB", kCardMargin + 36, kHeadingTextY);
    drawAccentRule(kCardMargin + 2, accent);

    tft.fillRoundRect(kCardMargin, 66, 240 - (kCardMargin * 2), 76, 14, theme.surface);
    drawGithubEventIcon(event.kind[0] != '\0' ? event.kind : "EVENT", 56, 104);

    const int kindFonts[] = {FONT_BODY, FONT_LABEL, FONT_INFO};
    bool hasDetail = event.detail[0] != '\0';
    drawAdaptiveText(event.kind[0] != '\0' ? event.kind : "EVENT",
                     152,
                     hasDetail ? 76 : 84,
                     130,
                     TC_DATUM,
                     kindFonts,
                     sizeof(kindFonts) / sizeof(kindFonts[0]),
                     theme.text,
                     theme.surface);

    const int detailFonts[] = {FONT_INFO};
    if (hasDetail) {
        drawAdaptiveText(event.detail,
                         152,
                         104,
                         138,
                         TC_DATUM,
                         detailFonts,
                         sizeof(detailFonts) / sizeof(detailFonts[0]),
                         accent,
                         theme.surface);
    }

    drawAdaptiveText(formatEventAge(event.createdAt),
                     152,
                     hasDetail ? 122 : 116,
                     138,
                     TC_DATUM,
                     detailFonts,
                     sizeof(detailFonts) / sizeof(detailFonts[0]),
                     theme.muted,
                     theme.surface);

    tft.setTextDatum(TL_DATUM);
    loadMono(13);
    tft.setTextColor(theme.muted, theme.background);
    tft.drawString(owner.length() > 0 ? owner + " /" : String("github /"), kCardMargin + 4, 154);

    // Repo name is the part worth reading: 22 px, dropping to 18 only when a
    // long name would overrun the card.
    loadMono(22);
    if (static_cast<int>(tft.textWidth(name)) > 240 - (kCardMargin * 2) - 8) {
        loadMono(18);
    }
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(theme.text, theme.background);
    tft.drawString(name, kCardMargin + 4, 174);

    // The account name is the human part of the event, so it gets the same
    // weight as the repository rather than being tucked away at label size.
    const int actorFonts[] = {FONT_LABEL, FONT_INFO};
    drawAdaptiveText(event.actor[0] != '\0' ? String(event.actor) : String("someone"),
                     kCardMargin + 4,
                     206,
                     240 - (kCardMargin * 2) - 8,
                     TL_DATUM,
                     actorFonts,
                     sizeof(actorFonts) / sizeof(actorFonts[0]),
                     accent,
                     theme.background);
}

void renderDashboardPage() {
    if (!dashboardPageAvailable(displayState.currentPage)) {
        displayState.currentPage = firstAvailableDashboardPage();
    }

    switch (displayState.currentPage) {
        case DASHBOARD_PAGE_CLOCK:
            renderClockPage();
            break;
        case DASHBOARD_PAGE_WEATHER:
            renderWeatherPage();
            break;
        case DASHBOARD_PAGE_RIVER:
            renderRiverPage();
            break;
        case DASHBOARD_PAGE_GITHUB:
            renderGithubPage();
            break;
        default:
            renderClockPage();
            break;
    }
}

void updateDashboardDynamicArea(uint8_t pageId) {
    switch (pageId) {
        case DASHBOARD_PAGE_CLOCK:
            updateClockDynamicArea();
            break;
        case DASHBOARD_PAGE_RIVER:
            updateRiverDynamicArea();
            break;
        default:
            break;
    }
}

void renderDashboardPageCached() {
    if (!dashboardPageAvailable(displayState.currentPage)) {
        displayState.currentPage = firstAvailableDashboardPage();
    }

    uint8_t pageId = displayState.currentPage;
    uint8_t themeId = activeThemeIdValue();
    uint32_t staticHash = dashboardStaticHash(pageId);
    uint32_t dynamicHash = dashboardDynamicHash(pageId);

    bool requiresFullRender = !renderCache.valid ||
                              renderCache.mode != DISPLAY_MODE_DASHBOARD ||
                              renderCache.page != pageId ||
                              renderCache.theme != themeId ||
                              renderCache.staticHash != staticHash;

    if (requiresFullRender) {
        renderDashboardPage();
        renderCache.valid = true;
        renderCache.mode = DISPLAY_MODE_DASHBOARD;
        renderCache.page = pageId;
        renderCache.theme = themeId;
        renderCache.staticHash = staticHash;
        renderCache.dynamicHash = dynamicHash;
        renderCache.imagePath[0] = '\0';
        return;
    }

    if (pageUsesDynamicRefresh(pageId) && renderCache.dynamicHash != dynamicHash) {
        updateDashboardDynamicArea(pageId);
        renderCache.dynamicHash = dynamicHash;
    }
}

}  // namespace

void setBacklightLevel(int brightness, bool rememberPreference, bool shouldLog = true) {
    int boundedBrightness = constrain(brightness, 0, 100);
    if (rememberPreference) {
        savedBrightness = boundedBrightness;
    }

    if (appliedBrightness == boundedBrightness) {
        backlightOn = boundedBrightness > 0;
        return;
    }

    int pwmValue = 0;
    if (boundedBrightness == 0) {
        pwmValue = 1023;
    } else if (boundedBrightness == 100) {
        pwmValue = 0;
    } else {
        pwmValue = map(boundedBrightness, 0, 100, 1023, 0);
    }

    analogWrite(PIN_BACKLIGHT, pwmValue);
    appliedBrightness = boundedBrightness;
    backlightOn = boundedBrightness > 0;
    if (shouldLog) {
        logPrintf("Brightness: %d%%, PWM: %d", boundedBrightness, pwmValue);
    }
}

void animateBacklightRamp(int fromBrightness,
                          int toBrightness,
                          uint8_t steps,
                          uint16_t stepDelayMs) {
    if (steps == 0) {
        setBacklightLevel(toBrightness, false, false);
        return;
    }

    int start = constrain(fromBrightness, 0, 100);
    int target = constrain(toBrightness, 0, 100);
    if (start == target) {
        return;
    }

    for (uint8_t step = 1; step <= steps; ++step) {
        int interpolated = start + ((target - start) * static_cast<int>(step)) / static_cast<int>(steps);
        setBacklightLevel(interpolated, false, false);
        if (stepDelayMs > 0) {
            delay(stepDelayMs);
            yield();
        }
    }
}

int beginPageTransition(int targetBrightness) {
    int startBrightness = constrain(targetBrightness, 0, 100);
    if (startBrightness < kPageTransitionMinimumBrightness) {
        return startBrightness;
    }

    int dimBrightness = max(static_cast<int>(kPageTransitionMinimumBrightness),
                            (startBrightness * static_cast<int>(kPageTransitionDimPercent)) / 100);
    if (dimBrightness >= startBrightness) {
        return startBrightness;
    }

    animateBacklightRamp(startBrightness,
                         dimBrightness,
                         kPageTransitionFadeDownSteps,
                         kPageTransitionFadeStepDelayMs);
    return dimBrightness;
}

void endPageTransition(int transitionBrightness, int targetBrightness) {
    int startBrightness = constrain(transitionBrightness, 0, 100);
    int endBrightness = constrain(targetBrightness, 0, 100);
    if (startBrightness == endBrightness) {
        return;
    }

    delay(8);
    yield();
    animateBacklightRamp(startBrightness,
                         endBrightness,
                         kPageTransitionFadeUpSteps,
                         kPageTransitionFadeStepDelayMs);
}

void displayInit() {
    logPrint(F("Display init..."));

    tft.init();
    tft.setRotation(0);

    // Shadow-crushed gamma. This is a backlit LCD, so near-black lifts into grey
    // (backlight haze). Lowering the low-grey points of the ST7789 gamma curve
    // pulls the dark end down toward the panel floor while leaving midtones and
    // highlights near stock. Only the first few params differ from the default
    // curve; delete these two writes to return to stock gamma.
    {
        static const uint8_t kGammaPos[14] =
            {0xD0, 0x00, 0x05, 0x08, 0x0B, 0x28, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
        static const uint8_t kGammaNeg[14] =
            {0xD0, 0x00, 0x05, 0x08, 0x0B, 0x29, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
        tft.writecommand(0xE0);  // GMCTRP1 - positive gamma
        for (uint8_t i = 0; i < 14; ++i) tft.writedata(kGammaPos[i]);
        tft.writecommand(0xE1);  // GMCTRN1 - negative gamma
        for (uint8_t i = 0; i < 14; ++i) tft.writedata(kGammaNeg[i]);
    }

    tft.invertDisplay(true);
    tft.fillScreen(TFT_BLACK);

    displayState.line2[0] = '\0';
    displayState.ipInfo[0] = '\0';
    displayState.showImage = false;
    displayState.imagePath[0] = '\0';
    displayState.apMode = false;
    displayState.apSSID[0] = '\0';
    displayState.apPassword[0] = '\0';
    displayState.currentPage = DASHBOARD_PAGE_CLOCK;
    clearTemporaryMessage();

    clockDynamicSprite.deleteSprite();
    clockDynamicSpriteReady = false;
    clockDynamicSpriteAttempted = false;
    clockDynamicSpriteAllowed = false;

    pinMode(PIN_BACKLIGHT, OUTPUT);
    analogWriteFreq(1000);
    analogWriteRange(1023);
    displaySetBrightness(100);

    logPrint(F("Display init complete"));
}

void displaySetBrightness(int brightness) {
    setBacklightLevel(brightness, true);
}

void displayApplyBrightness(int brightness) {
    setBacklightLevel(brightness, false);
}

void displaySetClockSpriteAllowed(bool allowed) {
    if (clockDynamicSpriteAllowed == allowed &&
        (!clockDynamicSpriteReady || allowed)) {
        return;
    }

    clockDynamicSpriteAllowed = allowed;
    clockDynamicSpriteAttempted = false;

    if (!allowed) {
        clockDynamicSprite.deleteSprite();
        clockDynamicSpriteReady = false;
    }
}

void displaySuspendDynamicResources() {
    if (dynamicResourceSuspendDepth == 0) {
        resumeClockSpriteAfterDynamicSuspend = clockDynamicSpriteAllowed;
        if (clockDynamicSpriteReady || clockDynamicSpriteAllowed) {
            clockDynamicSprite.deleteSprite();
            clockDynamicSpriteReady = false;
            clockDynamicSpriteAttempted = false;
            clockDynamicSpriteAllowed = false;
            logPrintf("Display resources suspended, free heap now %u", ESP.getFreeHeap());
        }
    }

    if (dynamicResourceSuspendDepth < 255) {
        ++dynamicResourceSuspendDepth;
    }
}

void displayResumeDynamicResources() {
    if (dynamicResourceSuspendDepth == 0) {
        return;
    }

    --dynamicResourceSuspendDepth;
    if (dynamicResourceSuspendDepth != 0) {
        return;
    }

    if (resumeClockSpriteAfterDynamicSuspend) {
        clockDynamicSpriteAllowed = true;
        clockDynamicSpriteAttempted = false;
        resumeClockSpriteAfterDynamicSuspend = false;
    }
}

void displayUpdate() {
    if (temporaryMessageVisible()) {
        uint32_t currentHash = temporaryMessageHash();
        uint8_t themeId = activeThemeIdValue();
        if (renderCache.valid &&
            renderCache.mode == DISPLAY_MODE_TEMP_MESSAGE &&
            renderCache.theme == themeId &&
            renderCache.staticHash == currentHash) {
            return;
        }

        const ThemePalette &theme = activeTheme();
        drawScreenChrome("SmartClock");
        drawRoundedPanel(12, 48, DISPLAY_WIDTH - 24, 176, theme.surface, theme.surfaceAlt);
        drawWrappedCenteredText(temporaryMessage.message, 120, 92, 192, FONT_BODY, theme.text, theme.surface, 4);

        renderCache.valid = true;
        renderCache.mode = DISPLAY_MODE_TEMP_MESSAGE;
        renderCache.page = 0;
        renderCache.theme = themeId;
        renderCache.staticHash = currentHash;
        renderCache.dynamicHash = 0;
        renderCache.imagePath[0] = '\0';
        return;
    }

    if (temporaryMessageExpired()) {
        clearTemporaryMessage();
        invalidateRenderCache();
    }

    if (displayState.apMode) {
        uint32_t currentHash = apScreenHash();
        uint8_t themeId = activeThemeIdValue();
        if (renderCache.valid &&
            renderCache.mode == DISPLAY_MODE_AP &&
            renderCache.theme == themeId &&
            renderCache.staticHash == currentHash) {
            return;
        }

        displayRenderAPMode();
        renderCache.valid = true;
        renderCache.mode = DISPLAY_MODE_AP;
        renderCache.page = 0;
        renderCache.theme = themeId;
        renderCache.staticHash = currentHash;
        renderCache.dynamicHash = 0;
        renderCache.imagePath[0] = '\0';
        return;
    }

    renderDashboardPageCached();
}

void displayRenderClock() {
    renderClockPage();
}

void displayRenderAPMode() {
    const ThemePalette &theme = activeTheme();
    bool canRevealAdminPassword = authCanRevealPassword();
    const char *adminPassword = authProvisionedPassword();
    String setupIp = displayState.ipInfo[0] != '\0' ? safeCString(displayState.ipInfo) : String("192.168.4.1");
    const int kWideFonts[] = {FONT_BODY, FONT_LABEL, FONT_INFO};
    const int kIpFonts[] = {FONT_INFO};
    const int kCompactFonts[] = {FONT_LABEL, FONT_INFO};

    tft.fillScreen(theme.background);

    tft.fillRoundRect(10, 10, 220, 24, 12, theme.surfaceAlt);
    tft.fillCircle(24, 22, 4, theme.accent);
    tft.setTextDatum(TL_DATUM);
    loadMono(13);
    tft.setTextColor(theme.text, theme.surfaceAlt);
    tft.drawString("Setup mode", 36, 16);

    drawRoundedPanel(10, 42, 220, 60, theme.accentSoft, theme.accentSoft);
    tft.setTextDatum(TC_DATUM);
    loadMono(13);
    tft.setTextColor(theme.accent, theme.accentSoft);
    tft.drawString("SETUP URL", 120, 52);
    drawAdaptiveText(setupIp,
                     120,
                     68,
                     192,
                     TC_DATUM,
                     kIpFonts,
                     sizeof(kIpFonts) / sizeof(kIpFonts[0]),
                     theme.text,
                     theme.accentSoft);
    loadMono(13);
    tft.setTextColor(theme.muted, theme.accentSoft);
    tft.drawString("Open this in browser", 120, 86);

    drawRoundedPanel(10, 108, 134, 50, theme.surface, theme.surfaceAlt);
    tft.setTextDatum(TL_DATUM);
    loadMono(13);
    tft.setTextColor(theme.muted, theme.surface);
    tft.drawString("WI-FI", 20, 118);
    drawAdaptiveText(safeCString(displayState.apSSID),
                     77,
                     136,
                     106,
                     TC_DATUM,
                     kCompactFonts,
                     sizeof(kCompactFonts) / sizeof(kCompactFonts[0]),
                     theme.text,
                     theme.surface);

    drawRoundedPanel(150, 108, 80, 50, theme.surface, theme.surfaceAlt);
    tft.setTextDatum(TC_DATUM);
    loadMono(13);
    tft.setTextColor(theme.muted, theme.surface);
    tft.drawString("PASS", 190, 118);
    drawAdaptiveText(safeCString(displayState.apPassword),
                     190,
                     136,
                     58,
                     TC_DATUM,
                     kWideFonts,
                     sizeof(kWideFonts) / sizeof(kWideFonts[0]),
                     theme.text,
                     theme.surface);

    drawRoundedPanel(10, 168, 220, 62, theme.surfaceAlt, theme.surfaceAlt);
    tft.setTextDatum(TL_DATUM);
    loadMono(13);
    tft.setTextColor(canRevealAdminPassword ? theme.accent : theme.muted, theme.surfaceAlt);
    tft.drawString("ADMIN LOGIN", 20, 178);

    if (canRevealAdminPassword && adminPassword != nullptr && adminPassword[0] != '\0') {
        drawBadge(20, 193, 48, 18, "admin", theme.accentSoft, theme.accent);
        drawAdaptiveText(safeCString(adminPassword),
                         220,
                         191,
                         138,
                         TR_DATUM,
                         kWideFonts,
                         sizeof(kWideFonts) / sizeof(kWideFonts[0]),
                         theme.text,
                         theme.surfaceAlt);

        tft.setTextDatum(TL_DATUM);
        loadMono(13);
        tft.setTextColor(theme.muted, theme.surfaceAlt);
        tft.drawString("Log in with this password.", 20, 212);
        return;
    }

    drawWrappedCenteredText("Use your saved admin password after Wi-Fi setup.",
                            120,
                            194,
                            192,
                            FONT_INFO,
                            theme.muted,
                            theme.surfaceAlt,
                            2);
}

void displayBlankScreen() {
    clearTemporaryMessage();
    tft.fillScreen(TFT_BLACK);
    invalidateRenderCache();
    logPrint(F("Display blanked to black."));
}

void displayShowMessage(const String &msg) {
    clearTemporaryMessage();
    const ThemePalette &theme = activeTheme();
    drawScreenChrome("SmartClock");
    drawRoundedPanel(18, 54, 204, 144, theme.surface, theme.surfaceAlt);
    drawWrappedCenteredText(msg, 120, 96, 170, FONT_BODY, theme.text, theme.surface, 4);
    invalidateRenderCache();
}

void displayShowTemporaryMessage(const String &msg, uint32_t durationMs) {
    strncpy(temporaryMessage.message, msg.c_str(), sizeof(temporaryMessage.message) - 1);
    temporaryMessage.message[sizeof(temporaryMessage.message) - 1] = '\0';
    temporaryMessage.active = temporaryMessage.message[0] != '\0';
    temporaryMessage.expiresAtMs = millis() + (durationMs > 0 ? durationMs : 1000UL);
    invalidateRenderCache();
    displayUpdate();
}

void displayShowAPScreen(const char *ssid, const char *password, const char *ip) {
    displayState.apMode = true;
    displayState.showImage = false;
    strncpy(displayState.apSSID, ssid, sizeof(displayState.apSSID) - 1);
    displayState.apSSID[sizeof(displayState.apSSID) - 1] = '\0';
    strncpy(displayState.apPassword, password, sizeof(displayState.apPassword) - 1);
    displayState.apPassword[sizeof(displayState.apPassword) - 1] = '\0';
    strncpy(displayState.ipInfo, ip, sizeof(displayState.ipInfo) - 1);
    displayState.ipInfo[sizeof(displayState.ipInfo) - 1] = '\0';
    invalidateRenderCache();
    displayUpdate();
}

void displayCycleNextPage(bool smoothTransition) {
    if (temporaryMessageVisible()) {
        logPrint(F("Page cycling disabled while temporary message is active"));
        return;
    }

    if (displayState.apMode) {
        logPrint(F("Page cycling disabled in AP mode"));
        return;
    }

    if (displayState.showImage) {
        displayState.showImage = false;
        displayState.currentPage = firstAvailableDashboardPage();
        invalidateRenderCache();
        displayUpdate();
        return;
    }

    uint8_t previousPage = displayState.currentPage;
    if (!dashboardPageAvailable(displayState.currentPage)) {
        displayState.currentPage = firstAvailableDashboardPage();
    } else {
        displayState.currentPage = nextAvailableDashboardPage(displayState.currentPage);
    }

    if (displayState.currentPage == previousPage) {
        return;
    }

    // Each visit to the GitHub page shows the next cached event rather than
    // repeating the newest one until the feed refreshes.
    if (displayState.currentPage == DASHBOARD_PAGE_GITHUB) {
        feedsGithubAdvanceCursor();
    }

    int targetBrightness = appliedBrightness;
    int transitionBrightness = targetBrightness;
    if (smoothTransition) {
        transitionBrightness = beginPageTransition(targetBrightness);
    }

    invalidateRenderCache();
    displayUpdate();
    if (smoothTransition) {
        endPageTransition(transitionBrightness, targetBrightness);
    }
}

void displayToggleBacklight() {
    if (backlightOn) {
        logPrint(F("Backlight OFF"));
        displayApplyBrightness(0);
    } else {
        logPrintf("Backlight ON (brightness: %d)", savedBrightness);
        displayApplyBrightness(savedBrightness > 0 ? savedBrightness : 100);
    }
}
