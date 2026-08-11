#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <Arduino.h>

#define DASHBOARD_GITHUB_EVENT_COUNT 2
#define DASHBOARD_CONFIG_PATH "/dashboard-config.json"
#define DASHBOARD_DATA_PATH "/dashboard-data.json"
#define DASHBOARD_CONFIG_VERSION 5

enum DashboardPageId : uint8_t {
    DASHBOARD_PAGE_CLOCK = 0,
    DASHBOARD_PAGE_WEATHER,
    DASHBOARD_PAGE_RIVER,
    DASHBOARD_PAGE_GITHUB,
    DASHBOARD_PAGE_COUNT
};

enum DashboardThemeId : uint8_t {
    DASHBOARD_THEME_AURORA = 0,
    DASHBOARD_THEME_SUNSET,
    DASHBOARD_THEME_TERMINAL,
    DASHBOARD_THEME_COUNT
};

struct WeatherData {
    char location[24];
    char condition[24];
    int temperature;
    int high;
    int low;
    int rainChance;
};

struct RiverData {
    char station[16];      // romanised station name, e.g. "SEONYU"
    char observedAt[8];    // hour reported by the API, e.g. "13:00"
    float temperature;     // water temperature in Celsius
    bool hasTemperature;   // false while the station reports maintenance
};

struct GithubEventEntry {
    char kind[12];         // STAR / FORK / PR / ISSUE / PUSH / COMMENT
    char repo[40];         // owner/name
    char actor[24];
    char detail[32];       // star total, branch, PR action, ...
    uint32_t createdAt;    // UTC epoch, so the age can be recomputed on screen
};

struct GithubData {
    GithubEventEntry events[DASHBOARD_GITHUB_EVENT_COUNT];
    uint8_t count;
    uint8_t cursor;        // advances every time the page is shown
};

struct DashboardConfig {
    uint16_t version;
    uint8_t theme;
    bool customThemeEnabled;
    char customBackground[8];
    char customSurface[8];
    char customAccent[8];
    char customText[8];
    bool nightModeEnabled;
    uint16_t nightStartMinutes;
    uint16_t nightEndMinutes;
    uint8_t nightBrightness;
    bool nightThemeEnabled;
    uint8_t nightTheme;
    bool nightCustomThemeEnabled;
    char nightCustomBackground[8];
    char nightCustomSurface[8];
    char nightCustomAccent[8];
    char nightCustomText[8];
    bool rotationEnabled;
    uint16_t rotationIntervalSec;
    bool use24Hour;
    bool showSeconds;
    bool showIp;
    bool enabledPages[DASHBOARD_PAGE_COUNT];
};

struct DashboardData {
    WeatherData weather;
    // River and GitHub readings live in the feed runtime rather than here; the
    // pages render straight from it, and this device has no RAM to spare for a
    // second copy.
};

extern DashboardConfig dashboardConfig;
extern DashboardData dashboardData;

void dashboardInit();
void dashboardResetToDefaults();
bool dashboardLoadConfig();
bool dashboardLoadData();
bool dashboardSaveConfig();
bool dashboardSaveData();
bool dashboardSaveAll();
bool dashboardApplyConfigJson(const String &json, String *error = nullptr);
bool dashboardApplyDataJson(const String &json, String *error = nullptr);
bool dashboardPreviewConfigJson(const String &json, String *error = nullptr);
bool dashboardPreviewDataJson(const String &json, String *error = nullptr);
void dashboardDiscardDraftChanges();
bool dashboardHasDraftChanges();
void dashboardBuildConfigJson(String &json);
void dashboardBuildDataJson(String &json);
void dashboardBuildFullJson(String &json);
bool dashboardSyncRuntimeState();
uint8_t dashboardFirstEnabledPage();
uint8_t dashboardNextEnabledPage(uint8_t currentPage);
bool dashboardPageEnabled(uint8_t pageId);
const char* dashboardPageName(uint8_t pageId);
uint32_t dashboardCurrentEpoch();
bool dashboardNightModeActive();
int dashboardEffectiveBrightness(int dayBrightness);

#endif
