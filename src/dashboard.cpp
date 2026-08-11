#include "dashboard.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <LittleFS.h>
#include <time.h>

DashboardConfig dashboardConfig;
DashboardData dashboardData;

namespace {

constexpr uint32_t kValidEpochFloor = 946684800UL;  // 2000-01-01T00:00:00Z
constexpr char kDefaultCustomBackground[] = "#080F1D";
constexpr char kDefaultCustomSurface[] = "#122035";
constexpr char kDefaultCustomAccent[] = "#58DAC1";
constexpr char kDefaultCustomText[] = "#F0F7FF";
constexpr char kDefaultNightCustomBackground[] = "#050B17";
constexpr char kDefaultNightCustomSurface[] = "#101A28";
constexpr char kDefaultNightCustomAccent[] = "#A8C7FA";
constexpr char kDefaultNightCustomText[] = "#F3F6FC";

DashboardConfig savedDashboardConfig;
DashboardData savedDashboardData;
bool dashboardConfigDraftActive = false;
bool dashboardDataDraftActive = false;

uint16_t normalizeMinutesOfDay(int value) {
    int normalized = value % (24 * 60);
    if (normalized < 0) {
        normalized += 24 * 60;
    }
    return static_cast<uint16_t>(normalized);
}

void enableAllPages() {
    for (uint8_t page = 0; page < DASHBOARD_PAGE_COUNT; ++page) {
        dashboardConfig.enabledPages[page] = true;
    }
}

bool anyPageEnabled() {
    for (uint8_t page = 0; page < DASHBOARD_PAGE_COUNT; ++page) {
        if (dashboardConfig.enabledPages[page]) {
            return true;
        }
    }

    return false;
}

void ensureAtLeastOnePageEnabled() {
    if (anyPageEnabled()) {
        return;
    }

    dashboardConfig.enabledPages[DASHBOARD_PAGE_CLOCK] = true;
}

bool buildTempPath(const char *path, char *tempPath, size_t tempPathSize) {
    int tempPathLength = snprintf(tempPath, tempPathSize, "%s.tmp", path);
    return tempPathLength > 0 && static_cast<size_t>(tempPathLength) < tempPathSize;
}

void copyString(char *destination, size_t destinationSize, const char *source) {
    if (destinationSize == 0) {
        return;
    }

    if (source == nullptr) {
        destination[0] = '\0';
        return;
    }

    snprintf(destination, destinationSize, "%s", source);
}

bool isHexColorChar(char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

void normalizeHexColor(char *destination, size_t destinationSize, const char *fallback) {
    if (destinationSize < 8) {
        return;
    }

    char normalized[8];
    normalized[0] = '#';
    normalized[7] = '\0';

    const char *source = destination;
    if (source == nullptr || source[0] == '\0') {
        copyString(destination, destinationSize, fallback);
        return;
    }

    if (source[0] == '#') {
        ++source;
    }

    for (uint8_t index = 0; index < 6; ++index) {
        char value = source[index];
        if (value == '\0' || !isHexColorChar(value)) {
            copyString(destination, destinationSize, fallback);
            return;
        }

        normalized[index + 1] = static_cast<char>(toupper(value));
    }

    if (source[6] != '\0') {
        copyString(destination, destinationSize, fallback);
        return;
    }

    copyString(destination, destinationSize, normalized);
}

void setConfigDefaults() {
    memset(&dashboardConfig, 0, sizeof(dashboardConfig));
    dashboardConfig.version = DASHBOARD_CONFIG_VERSION;
    dashboardConfig.theme = DASHBOARD_THEME_AURORA;
    dashboardConfig.customThemeEnabled = false;
    copyString(dashboardConfig.customBackground, sizeof(dashboardConfig.customBackground), kDefaultCustomBackground);
    copyString(dashboardConfig.customSurface, sizeof(dashboardConfig.customSurface), kDefaultCustomSurface);
    copyString(dashboardConfig.customAccent, sizeof(dashboardConfig.customAccent), kDefaultCustomAccent);
    copyString(dashboardConfig.customText, sizeof(dashboardConfig.customText), kDefaultCustomText);
    dashboardConfig.nightModeEnabled = false;
    dashboardConfig.nightStartMinutes = 22 * 60;
    dashboardConfig.nightEndMinutes = 7 * 60;
    dashboardConfig.nightBrightness = 14;
    dashboardConfig.nightThemeEnabled = false;
    dashboardConfig.nightTheme = DASHBOARD_THEME_AURORA;
    dashboardConfig.nightCustomThemeEnabled = false;
    copyString(dashboardConfig.nightCustomBackground,
               sizeof(dashboardConfig.nightCustomBackground),
               kDefaultNightCustomBackground);
    copyString(dashboardConfig.nightCustomSurface,
               sizeof(dashboardConfig.nightCustomSurface),
               kDefaultNightCustomSurface);
    copyString(dashboardConfig.nightCustomAccent,
               sizeof(dashboardConfig.nightCustomAccent),
               kDefaultNightCustomAccent);
    copyString(dashboardConfig.nightCustomText,
               sizeof(dashboardConfig.nightCustomText),
               kDefaultNightCustomText);
    dashboardConfig.rotationEnabled = true;
    dashboardConfig.rotationIntervalSec = 10;
    dashboardConfig.use24Hour = true;
    dashboardConfig.showSeconds = false;
    dashboardConfig.showIp = false;
    enableAllPages();
}

void setDataDefaults() {
    memset(&dashboardData, 0, sizeof(dashboardData));

    dashboardData.weather.temperature = 21;
    dashboardData.weather.high = 24;
    dashboardData.weather.low = 18;
    dashboardData.weather.rainChance = 10;
}

void normalizeConfig() {
    dashboardConfig.version = DASHBOARD_CONFIG_VERSION;
    dashboardConfig.theme = constrain(dashboardConfig.theme, 0, DASHBOARD_THEME_COUNT - 1);
    normalizeHexColor(dashboardConfig.customBackground, sizeof(dashboardConfig.customBackground), kDefaultCustomBackground);
    normalizeHexColor(dashboardConfig.customSurface, sizeof(dashboardConfig.customSurface), kDefaultCustomSurface);
    normalizeHexColor(dashboardConfig.customAccent, sizeof(dashboardConfig.customAccent), kDefaultCustomAccent);
    normalizeHexColor(dashboardConfig.customText, sizeof(dashboardConfig.customText), kDefaultCustomText);
    dashboardConfig.nightStartMinutes = normalizeMinutesOfDay(dashboardConfig.nightStartMinutes);
    dashboardConfig.nightEndMinutes = normalizeMinutesOfDay(dashboardConfig.nightEndMinutes);
    dashboardConfig.nightBrightness = constrain(dashboardConfig.nightBrightness, 1, 100);
    dashboardConfig.nightTheme = constrain(dashboardConfig.nightTheme, 0, DASHBOARD_THEME_COUNT - 1);
    normalizeHexColor(dashboardConfig.nightCustomBackground,
                      sizeof(dashboardConfig.nightCustomBackground),
                      kDefaultNightCustomBackground);
    normalizeHexColor(dashboardConfig.nightCustomSurface,
                      sizeof(dashboardConfig.nightCustomSurface),
                      kDefaultNightCustomSurface);
    normalizeHexColor(dashboardConfig.nightCustomAccent,
                      sizeof(dashboardConfig.nightCustomAccent),
                      kDefaultNightCustomAccent);
    normalizeHexColor(dashboardConfig.nightCustomText,
                      sizeof(dashboardConfig.nightCustomText),
                      kDefaultNightCustomText);
    dashboardConfig.rotationIntervalSec = constrain(dashboardConfig.rotationIntervalSec, 3, 120);
    ensureAtLeastOnePageEnabled();
}

void normalizeData() {
    dashboardData.weather.rainChance = constrain(dashboardData.weather.rainChance, 0, 100);
    dashboardData.weather.location[sizeof(dashboardData.weather.location) - 1] = '\0';
    dashboardData.weather.condition[sizeof(dashboardData.weather.condition) - 1] = '\0';
}

bool weatherEquals(const WeatherData &left, const WeatherData &right) {
    return strcmp(left.location, right.location) == 0 &&
           strcmp(left.condition, right.condition) == 0 &&
           left.temperature == right.temperature &&
           left.high == right.high &&
           left.low == right.low &&
           left.rainChance == right.rainChance;
}

bool configEquals(const DashboardConfig &left, const DashboardConfig &right) {
    if (left.version != right.version ||
        left.theme != right.theme ||
        left.customThemeEnabled != right.customThemeEnabled ||
        strcmp(left.customBackground, right.customBackground) != 0 ||
        strcmp(left.customSurface, right.customSurface) != 0 ||
        strcmp(left.customAccent, right.customAccent) != 0 ||
        strcmp(left.customText, right.customText) != 0 ||
        left.nightModeEnabled != right.nightModeEnabled ||
        left.nightStartMinutes != right.nightStartMinutes ||
        left.nightEndMinutes != right.nightEndMinutes ||
        left.nightBrightness != right.nightBrightness ||
        left.nightThemeEnabled != right.nightThemeEnabled ||
        left.nightTheme != right.nightTheme ||
        left.nightCustomThemeEnabled != right.nightCustomThemeEnabled ||
        strcmp(left.nightCustomBackground, right.nightCustomBackground) != 0 ||
        strcmp(left.nightCustomSurface, right.nightCustomSurface) != 0 ||
        strcmp(left.nightCustomAccent, right.nightCustomAccent) != 0 ||
        strcmp(left.nightCustomText, right.nightCustomText) != 0 ||
        left.rotationEnabled != right.rotationEnabled ||
        left.rotationIntervalSec != right.rotationIntervalSec ||
        left.use24Hour != right.use24Hour ||
        left.showSeconds != right.showSeconds ||
        left.showIp != right.showIp) {
        return false;
    }

    for (uint8_t page = 0; page < DASHBOARD_PAGE_COUNT; ++page) {
        if (left.enabledPages[page] != right.enabledPages[page]) {
            return false;
        }
    }

    return true;
}

bool dataEquals(const DashboardData &left, const DashboardData &right) {
    return weatherEquals(left.weather, right.weather);
}

void captureSavedConfig() {
    memcpy(&savedDashboardConfig, &dashboardConfig, sizeof(savedDashboardConfig));
    dashboardConfigDraftActive = false;
}

void captureSavedData() {
    memcpy(&savedDashboardData, &dashboardData, sizeof(savedDashboardData));
    dashboardDataDraftActive = false;
}

void fillConfigJson(JsonObject root) {
    root["version"] = dashboardConfig.version;
    root["theme"] = dashboardConfig.theme;
    root["customThemeEnabled"] = dashboardConfig.customThemeEnabled;
    root["rotationEnabled"] = dashboardConfig.rotationEnabled;
    root["rotationIntervalSec"] = dashboardConfig.rotationIntervalSec;
    root["use24Hour"] = dashboardConfig.use24Hour;
    root["showSeconds"] = dashboardConfig.showSeconds;
    root["showIp"] = dashboardConfig.showIp;

    JsonObject customTheme = root["customTheme"].to<JsonObject>();
    customTheme["background"] = dashboardConfig.customBackground;
    customTheme["surface"] = dashboardConfig.customSurface;
    customTheme["accent"] = dashboardConfig.customAccent;
    customTheme["text"] = dashboardConfig.customText;

    JsonObject nightMode = root["nightMode"].to<JsonObject>();
    nightMode["enabled"] = dashboardConfig.nightModeEnabled;
    nightMode["startMinutes"] = dashboardConfig.nightStartMinutes;
    nightMode["endMinutes"] = dashboardConfig.nightEndMinutes;
    nightMode["brightness"] = dashboardConfig.nightBrightness;
    nightMode["themeEnabled"] = dashboardConfig.nightThemeEnabled;
    nightMode["theme"] = dashboardConfig.nightTheme;
    nightMode["customThemeEnabled"] = dashboardConfig.nightCustomThemeEnabled;

    JsonObject nightCustomTheme = nightMode["customTheme"].to<JsonObject>();
    nightCustomTheme["background"] = dashboardConfig.nightCustomBackground;
    nightCustomTheme["surface"] = dashboardConfig.nightCustomSurface;
    nightCustomTheme["accent"] = dashboardConfig.nightCustomAccent;
    nightCustomTheme["text"] = dashboardConfig.nightCustomText;

    JsonObject pages = root["pages"].to<JsonObject>();
    pages["clock"] = dashboardConfig.enabledPages[DASHBOARD_PAGE_CLOCK];
    pages["weather"] = dashboardConfig.enabledPages[DASHBOARD_PAGE_WEATHER];
    pages["river"] = dashboardConfig.enabledPages[DASHBOARD_PAGE_RIVER];
    pages["github"] = dashboardConfig.enabledPages[DASHBOARD_PAGE_GITHUB];
}

void fillDataJson(JsonObject root) {
    JsonObject weather = root["weather"].to<JsonObject>();
    weather["location"] = dashboardData.weather.location;
    weather["condition"] = dashboardData.weather.condition;
    weather["temperature"] = dashboardData.weather.temperature;
    weather["high"] = dashboardData.weather.high;
    weather["low"] = dashboardData.weather.low;
    weather["rainChance"] = dashboardData.weather.rainChance;
}

void applyPagesObject(JsonObjectConst pages) {
    if (pages.isNull()) {
        return;
    }

    struct PageKey {
        const char *name;
        uint8_t page;
    };
    static const PageKey kPageKeys[] = {
        {"clock", DASHBOARD_PAGE_CLOCK},
        {"weather", DASHBOARD_PAGE_WEATHER},
        {"river", DASHBOARD_PAGE_RIVER},
        {"github", DASHBOARD_PAGE_GITHUB},
    };

    for (const PageKey &key : kPageKeys) {
        if (!pages[key.name].isNull()) {
            dashboardConfig.enabledPages[key.page] = pages[key.name].as<bool>();
        }
    }
}

bool writeJsonToFile(const char *path, JsonDocument &doc) {
    char tempPath[64];
    if (!buildTempPath(path, tempPath, sizeof(tempPath))) {
        return false;
    }

    LittleFS.remove(tempPath);

    File file = LittleFS.open(tempPath, "w");
    if (!file) {
        return false;
    }

    bool success = serializeJson(doc, file) > 0;
    file.flush();
    file.close();
    if (!success) {
        LittleFS.remove(tempPath);
        return false;
    }

    if (LittleFS.exists(path)) {
        LittleFS.remove(path);
    }

    if (!LittleFS.rename(tempPath, path)) {
        LittleFS.remove(tempPath);
        return false;
    }

    return true;
}

template <typename TObject>
bool loadJsonFile(const char *path, TObject object) {
    auto tryLoad = [&](const char *candidatePath) {
        File file = LittleFS.open(candidatePath, "r");
        if (!file) {
            return false;
        }

        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, file);
        file.close();
        if (error) {
            return false;
        }

        object(doc.as<JsonObjectConst>());
        return true;
    };

    if (tryLoad(path)) {
        return true;
    }

    char tempPath[64];
    if (!buildTempPath(path, tempPath, sizeof(tempPath))) {
        return false;
    }

    if (!LittleFS.exists(tempPath) || !tryLoad(tempPath)) {
        return false;
    }

    LittleFS.remove(path);
    LittleFS.rename(tempPath, path);
    return true;
}

void applyConfigObject(JsonObjectConst root) {
    if (!root["theme"].isNull()) {
        dashboardConfig.theme = root["theme"].as<uint8_t>();
    }
    if (!root["customThemeEnabled"].isNull()) {
        dashboardConfig.customThemeEnabled = root["customThemeEnabled"].as<bool>();
    }
    if (!root["rotationEnabled"].isNull()) {
        dashboardConfig.rotationEnabled = root["rotationEnabled"].as<bool>();
    }
    if (!root["rotationIntervalSec"].isNull()) {
        dashboardConfig.rotationIntervalSec = root["rotationIntervalSec"].as<uint16_t>();
    }
    if (!root["use24Hour"].isNull()) {
        dashboardConfig.use24Hour = root["use24Hour"].as<bool>();
    }
    if (!root["showSeconds"].isNull()) {
        dashboardConfig.showSeconds = root["showSeconds"].as<bool>();
    }
    if (!root["showIp"].isNull()) {
        dashboardConfig.showIp = root["showIp"].as<bool>();
    }

    JsonObjectConst customTheme = root["customTheme"].as<JsonObjectConst>();
    if (!customTheme.isNull()) {
        if (!customTheme["background"].isNull()) {
            copyString(dashboardConfig.customBackground, sizeof(dashboardConfig.customBackground), customTheme["background"]);
        }
        if (!customTheme["surface"].isNull()) {
            copyString(dashboardConfig.customSurface, sizeof(dashboardConfig.customSurface), customTheme["surface"]);
        }
        if (!customTheme["accent"].isNull()) {
            copyString(dashboardConfig.customAccent, sizeof(dashboardConfig.customAccent), customTheme["accent"]);
        }
        if (!customTheme["text"].isNull()) {
            copyString(dashboardConfig.customText, sizeof(dashboardConfig.customText), customTheme["text"]);
        }
    }

    JsonObjectConst nightMode = root["nightMode"].as<JsonObjectConst>();
    if (!nightMode.isNull()) {
        if (!nightMode["enabled"].isNull()) {
            dashboardConfig.nightModeEnabled = nightMode["enabled"].as<bool>();
        }
        if (!nightMode["startMinutes"].isNull()) {
            dashboardConfig.nightStartMinutes = nightMode["startMinutes"].as<uint16_t>();
        }
        if (!nightMode["endMinutes"].isNull()) {
            dashboardConfig.nightEndMinutes = nightMode["endMinutes"].as<uint16_t>();
        }
        if (!nightMode["brightness"].isNull()) {
            dashboardConfig.nightBrightness = nightMode["brightness"].as<uint8_t>();
        }
        if (!nightMode["themeEnabled"].isNull()) {
            dashboardConfig.nightThemeEnabled = nightMode["themeEnabled"].as<bool>();
        }
        if (!nightMode["theme"].isNull()) {
            dashboardConfig.nightTheme = nightMode["theme"].as<uint8_t>();
        }
        if (!nightMode["customThemeEnabled"].isNull()) {
            dashboardConfig.nightCustomThemeEnabled = nightMode["customThemeEnabled"].as<bool>();
        }

        JsonObjectConst nightCustomTheme = nightMode["customTheme"].as<JsonObjectConst>();
        if (!nightCustomTheme.isNull()) {
            if (!nightCustomTheme["background"].isNull()) {
                copyString(dashboardConfig.nightCustomBackground,
                           sizeof(dashboardConfig.nightCustomBackground),
                           nightCustomTheme["background"]);
            }
            if (!nightCustomTheme["surface"].isNull()) {
                copyString(dashboardConfig.nightCustomSurface,
                           sizeof(dashboardConfig.nightCustomSurface),
                           nightCustomTheme["surface"]);
            }
            if (!nightCustomTheme["accent"].isNull()) {
                copyString(dashboardConfig.nightCustomAccent,
                           sizeof(dashboardConfig.nightCustomAccent),
                           nightCustomTheme["accent"]);
            }
            if (!nightCustomTheme["text"].isNull()) {
                copyString(dashboardConfig.nightCustomText,
                           sizeof(dashboardConfig.nightCustomText),
                           nightCustomTheme["text"]);
            }
        }
    }

    applyPagesObject(root["pages"].as<JsonObjectConst>());
    normalizeConfig();
}

void applyWeatherObject(JsonObjectConst weather) {
    if (weather.isNull()) {
        return;
    }

    if (!weather["location"].isNull()) {
        copyString(dashboardData.weather.location, sizeof(dashboardData.weather.location), weather["location"]);
    }
    if (!weather["condition"].isNull()) {
        copyString(dashboardData.weather.condition, sizeof(dashboardData.weather.condition), weather["condition"]);
    }
    if (!weather["temperature"].isNull()) {
        dashboardData.weather.temperature = weather["temperature"].as<int>();
    }
    if (!weather["high"].isNull()) {
        dashboardData.weather.high = weather["high"].as<int>();
    }
    if (!weather["low"].isNull()) {
        dashboardData.weather.low = weather["low"].as<int>();
    }
    if (!weather["rainChance"].isNull()) {
        dashboardData.weather.rainChance = weather["rainChance"].as<int>();
    }
}

void applyDataObject(JsonObjectConst root) {
    JsonObjectConst weather = root["weather"].as<JsonObjectConst>();
    if (!weather.isNull()) {
        if (!weather["location"].isNull()) {
            copyString(dashboardData.weather.location, sizeof(dashboardData.weather.location), weather["location"] | "");
        }
        if (!weather["condition"].isNull()) {
            copyString(dashboardData.weather.condition, sizeof(dashboardData.weather.condition), weather["condition"] | "");
        }
        if (!weather["temperature"].isNull()) {
            dashboardData.weather.temperature = weather["temperature"].as<int>();
        }
        if (!weather["high"].isNull()) {
            dashboardData.weather.high = weather["high"].as<int>();
        }
        if (!weather["low"].isNull()) {
            dashboardData.weather.low = weather["low"].as<int>();
        }
        if (!weather["rainChance"].isNull()) {
            dashboardData.weather.rainChance = weather["rainChance"].as<int>();
        }
    }

    normalizeData();
}

}  // namespace

void dashboardResetToDefaults() {
    setConfigDefaults();
    setDataDefaults();
}

void dashboardInit() {
    dashboardResetToDefaults();

    bool configLoaded = dashboardLoadConfig();
    bool dataLoaded = dashboardLoadData();

    if (!configLoaded) {
        dashboardSaveConfig();
    }
    if (!dataLoaded) {
        dashboardSaveData();
    }
}

bool dashboardLoadConfig() {
    setConfigDefaults();
    bool success = loadJsonFile(DASHBOARD_CONFIG_PATH, [](JsonObjectConst root) {
        applyConfigObject(root);
    });

    normalizeConfig();
    captureSavedConfig();
    return success;
}

bool dashboardLoadData() {
    setDataDefaults();
    bool success = loadJsonFile(DASHBOARD_DATA_PATH, [](JsonObjectConst root) {
        applyDataObject(root);
    });

    normalizeData();
    captureSavedData();
    return success;
}

bool dashboardSaveConfig() {
    normalizeConfig();

    JsonDocument doc;
    fillConfigJson(doc.to<JsonObject>());
    bool success = writeJsonToFile(DASHBOARD_CONFIG_PATH, doc);
    if (success) {
        captureSavedConfig();
    }
    return success;
}

bool dashboardSaveData() {
    normalizeData();

    JsonDocument doc;
    fillDataJson(doc.to<JsonObject>());
    bool success = writeJsonToFile(DASHBOARD_DATA_PATH, doc);
    if (success) {
        captureSavedData();
    }
    return success;
}

bool dashboardSaveAll() {
    return dashboardSaveConfig() && dashboardSaveData();
}

bool dashboardApplyConfigJson(const String &json, String *error) {
    JsonDocument doc;
    DeserializationError deserializeError = deserializeJson(doc, json);
    if (deserializeError) {
        if (error != nullptr) {
            *error = deserializeError.c_str();
        }
        return false;
    }

    applyConfigObject(doc.as<JsonObjectConst>());
    if (!dashboardSaveConfig()) {
        if (error != nullptr) {
            *error = "Failed to write config";
        }
        return false;
    }

    return true;
}

bool dashboardApplyDataJson(const String &json, String *error) {
    JsonDocument doc;
    DeserializationError deserializeError = deserializeJson(doc, json);
    if (deserializeError) {
        if (error != nullptr) {
            *error = deserializeError.c_str();
        }
        return false;
    }

    applyDataObject(doc.as<JsonObjectConst>());
    if (!dashboardSaveData()) {
        if (error != nullptr) {
            *error = "Failed to write data";
        }
        return false;
    }

    return true;
}

bool dashboardPreviewConfigJson(const String &json, String *error) {
    JsonDocument doc;
    DeserializationError deserializeError = deserializeJson(doc, json);
    if (deserializeError) {
        if (error != nullptr) {
            *error = deserializeError.c_str();
        }
        return false;
    }

    memcpy(&dashboardConfig, &savedDashboardConfig, sizeof(dashboardConfig));
    applyConfigObject(doc.as<JsonObjectConst>());
    dashboardConfigDraftActive = !configEquals(dashboardConfig, savedDashboardConfig);
    return true;
}

bool dashboardPreviewDataJson(const String &json, String *error) {
    JsonDocument doc;
    DeserializationError deserializeError = deserializeJson(doc, json);
    if (deserializeError) {
        if (error != nullptr) {
            *error = deserializeError.c_str();
        }
        return false;
    }

    memcpy(&dashboardData, &savedDashboardData, sizeof(dashboardData));
    applyDataObject(doc.as<JsonObjectConst>());
    dashboardDataDraftActive = !dataEquals(dashboardData, savedDashboardData);
    return true;
}

void dashboardDiscardDraftChanges() {
    memcpy(&dashboardConfig, &savedDashboardConfig, sizeof(dashboardConfig));
    memcpy(&dashboardData, &savedDashboardData, sizeof(dashboardData));
    dashboardConfigDraftActive = false;
    dashboardDataDraftActive = false;
}

bool dashboardHasDraftChanges() {
    return dashboardConfigDraftActive || dashboardDataDraftActive;
}

void dashboardBuildConfigJson(String &json) {
    JsonDocument doc;
    fillConfigJson(doc.to<JsonObject>());
    json = "";
    serializeJson(doc, json);
}

void dashboardBuildDataJson(String &json) {
    JsonDocument doc;
    fillDataJson(doc.to<JsonObject>());
    json = "";
    serializeJson(doc, json);
}

void dashboardBuildFullJson(String &json) {
    JsonDocument doc;
    fillConfigJson(doc["config"].to<JsonObject>());
    fillDataJson(doc["data"].to<JsonObject>());
    JsonObject meta = doc["meta"].to<JsonObject>();
    meta["configDraft"] = dashboardConfigDraftActive;
    meta["dataDraft"] = dashboardDataDraftActive;
    meta["hasDraft"] = dashboardHasDraftChanges();
    json = "";
    serializeJson(doc, json);
}

bool dashboardSyncRuntimeState() {
    // Nothing on the dashboard counts down any more; the pages read straight
    // from the feeds, so there is no runtime state to reconcile.
    return false;
}

bool dashboardPageEnabled(uint8_t pageId) {
    if (pageId >= DASHBOARD_PAGE_COUNT) {
        return false;
    }

    return dashboardConfig.enabledPages[pageId];
}

uint8_t dashboardFirstEnabledPage() {
    for (uint8_t page = 0; page < DASHBOARD_PAGE_COUNT; ++page) {
        if (dashboardPageEnabled(page)) {
            return page;
        }
    }

    return DASHBOARD_PAGE_CLOCK;
}

uint8_t dashboardNextEnabledPage(uint8_t currentPage) {
    for (uint8_t offset = 1; offset <= DASHBOARD_PAGE_COUNT; ++offset) {
        uint8_t page = (currentPage + offset) % DASHBOARD_PAGE_COUNT;
        if (dashboardPageEnabled(page)) {
            return page;
        }
    }

    return DASHBOARD_PAGE_CLOCK;
}

const char* dashboardPageName(uint8_t pageId) {
    switch (pageId) {
        case DASHBOARD_PAGE_WEATHER:
            return "weather";
        case DASHBOARD_PAGE_RIVER:
            return "river";
        case DASHBOARD_PAGE_GITHUB:
            return "github";
        default:
            return "clock";
    }
}

uint32_t dashboardCurrentEpoch() {
    time_t now = time(nullptr);
    if (now < kValidEpochFloor) {
        return 0;
    }

    return static_cast<uint32_t>(now);
}

bool dashboardNightModeActive() {
    if (!dashboardConfig.nightModeEnabled || dashboardCurrentEpoch() == 0) {
        return false;
    }

    time_t now = time(nullptr);
    tm localTimeInfo;
    localtime_r(&now, &localTimeInfo);

    uint16_t currentMinutes = static_cast<uint16_t>((localTimeInfo.tm_hour * 60) + localTimeInfo.tm_min);
    uint16_t startMinutes = dashboardConfig.nightStartMinutes;
    uint16_t endMinutes = dashboardConfig.nightEndMinutes;

    if (startMinutes == endMinutes) {
        return true;
    }

    if (startMinutes < endMinutes) {
        return currentMinutes >= startMinutes && currentMinutes < endMinutes;
    }

    return currentMinutes >= startMinutes || currentMinutes < endMinutes;
}

int dashboardEffectiveBrightness(int dayBrightness) {
    int normalizedDayBrightness = constrain(dayBrightness, 0, 100);
    if (!dashboardNightModeActive()) {
        return normalizedDayBrightness;
    }

    return constrain(static_cast<int>(dashboardConfig.nightBrightness), 1, 100);
}

