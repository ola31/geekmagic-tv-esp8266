// Renders the firmware's real dashboard pages on the host so a layout can be
// checked before it is flashed. display.cpp is included rather than linked so
// the page renderers, which live in an anonymous namespace, stay reachable.

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include "shim/Arduino.h"

#include "../src/dashboard.h"
#include "../src/feeds.h"

// State the firmware owns in other translation units.
DashboardConfig dashboardConfig;
DashboardData dashboardData;
FeedConfig feedConfig;
FeedRuntimeState feedRuntime;

void logPrint(const String &) {}
void logPrintf(const char *, ...) {}

bool authCanRevealPassword() { return false; }
const char *authProvisionedPassword() { return ""; }

uint32_t dashboardCurrentEpoch() { return 1785000000u; }
int32_t dashboardFocusRemainingSeconds() { return 0; }
int32_t dashboardEventRemainingSeconds() { return 0; }
bool dashboardNightModeActive() { return false; }
bool dashboardPageEnabled(uint8_t pageId) {
    return pageId < DASHBOARD_PAGE_COUNT && dashboardConfig.enabledPages[pageId];
}
uint8_t dashboardFirstEnabledPage() { return DASHBOARD_PAGE_CLOCK; }

uint8_t feedsWeatherSource() { return feedConfig.weather.source; }
bool feedsWeatherConfigured() { return true; }
bool feedsHasWeatherData() { return feedRuntime.weather.hasData; }
bool feedsWeatherUsesFahrenheit() { return false; }
const WeatherData *feedsWeatherData() {
    return feedRuntime.weather.hasData ? &feedRuntime.weather.data : nullptr;
}

bool feedsRiverConfigured() { return true; }
bool feedsHasRiverData() { return feedRuntime.river.hasData; }
const RiverData *feedsRiverData() {
    return feedRuntime.river.hasData ? &feedRuntime.river.data : nullptr;
}
bool feedsGithubConfigured() { return true; }
bool feedsHasGithubData() { return feedRuntime.github.hasData && feedRuntime.github.data.count > 0; }
const GithubData *feedsGithubData() { return feedsHasGithubData() ? &feedRuntime.github.data : nullptr; }
void feedsGithubAdvanceCursor() {}

#include "../src/display.cpp"

namespace {

void copyInto(char *destination, size_t size, const char *source) {
    snprintf(destination, size, "%s", source);
}

void seedSampleData() {
    for (uint8_t page = 0; page < DASHBOARD_PAGE_COUNT; ++page) {
        dashboardConfig.enabledPages[page] = false;
    }
    dashboardConfig.enabledPages[DASHBOARD_PAGE_CLOCK] = true;
    dashboardConfig.enabledPages[DASHBOARD_PAGE_WEATHER] = true;
    dashboardConfig.enabledPages[DASHBOARD_PAGE_RIVER] = true;
    dashboardConfig.enabledPages[DASHBOARD_PAGE_GITHUB] = true;

    dashboardConfig.theme = 0;
    dashboardConfig.customThemeEnabled = true;
    // Overridable so palettes can be compared without editing the source.
    const char *background = getenv("SIM_BG");
    const char *surface = getenv("SIM_SURFACE");
    copyInto(dashboardConfig.customBackground, sizeof(dashboardConfig.customBackground),
             background != nullptr ? background : "#000410");
    copyInto(dashboardConfig.customSurface, sizeof(dashboardConfig.customSurface),
             surface != nullptr ? surface : "#000C20");
    copyInto(dashboardConfig.customAccent, sizeof(dashboardConfig.customAccent), "#35C8E8");
    copyInto(dashboardConfig.customText, sizeof(dashboardConfig.customText), "#EAF2F6");
    dashboardConfig.use24Hour = true;
    dashboardConfig.showSeconds = false;
    dashboardConfig.showIp = false;

    feedConfig.weather.source = WEATHER_FEED_OPEN_METEO;
    feedRuntime.weather.hasData = true;
    copyInto(feedRuntime.weather.data.location, sizeof(feedRuntime.weather.data.location), "SEOUL");
    copyInto(feedRuntime.weather.data.condition, sizeof(feedRuntime.weather.data.condition), "Clear sky");
    feedRuntime.weather.data.temperature = 34;
    feedRuntime.weather.data.high = 36;
    feedRuntime.weather.data.low = 26;
    feedRuntime.weather.data.rainChance = 10;

    feedRuntime.river.hasData = true;
    copyInto(feedRuntime.river.data.station, sizeof(feedRuntime.river.data.station), "SEONYU");
    copyInto(feedRuntime.river.data.observedAt, sizeof(feedRuntime.river.data.observedAt), "15:00");
    feedRuntime.river.data.temperature = 28.3f;
    feedRuntime.river.data.hasTemperature = true;

    feedRuntime.github.hasData = true;
    feedRuntime.github.data.count = 1;
    feedRuntime.github.data.cursor = 0;
    copyInto(feedRuntime.github.data.events[0].kind, sizeof(feedRuntime.github.data.events[0].kind), "STAR");
    copyInto(feedRuntime.github.data.events[0].repo, sizeof(feedRuntime.github.data.events[0].repo), "torvalds/linux");
    copyInto(feedRuntime.github.data.events[0].actor, sizeof(feedRuntime.github.data.events[0].actor), "crs-alzerd");
    copyInto(feedRuntime.github.data.events[0].detail, sizeof(feedRuntime.github.data.events[0].detail), "3 min ago");
}

void renderTo(const char *path, void (*render)()) {
    render();
    if (!tft.writePpm(path)) {
        fprintf(stderr, "failed to write %s\n", path);
    }
}

}  // namespace

int main() {
    // Pin the clock so renders are reproducible and match the design sheet.
    setenv("TZ", "UTC-9", 1);
    tzset();
    simSetMillis(4000);
    seedSampleData();

    renderTo("out/river.ppm", renderRiverPage);
    renderTo("out/weather.ppm", renderWeatherPage);
    renderTo("out/github.ppm", renderGithubPage);
    renderTo("out/clock.ppm", renderClockPage);

    printf("rendered 4 screens\n");
    return 0;
}
