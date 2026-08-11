#include "feeds.h"

#include "display.h"
#include "logger.h"
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <WiFiClientSecureBearSSL.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>

FeedConfig feedConfig;
FeedRuntimeState feedRuntime;

namespace {

constexpr uint16_t kWeatherMinRefreshMinutes = 5;
constexpr uint16_t kWeatherMaxRefreshMinutes = 240;
// The stations report hourly, so polling faster than every few minutes is waste.
constexpr uint16_t kRiverMinRefreshMinutes = 5;
constexpr uint16_t kRiverMaxRefreshMinutes = 240;
// Anonymous GitHub API access allows 60 requests per hour; keep a safety margin.
constexpr uint16_t kGithubMinRefreshMinutes = 2;
constexpr uint16_t kGithubMaxRefreshMinutes = 240;
// "Seonyu" sits on the Han River mainstream near Seonyudo; the other automatic
// stations (Tancheon, Jungnangcheon, Anyangcheon) are tributaries.
constexpr char kRiverDefaultStation[] = "선유";
constexpr uint32_t kHttpTimeoutMs = 12000;
constexpr uint32_t kFeedStartupGracePeriodMs = 15000UL;
constexpr uint32_t kHttpsPreferredFreeHeapBytes = 24000UL;
// Heap a TLS request needs on top of its receive buffer: the BearSSL session
// state, the HTTP client, and the filtered JSON document. The margin is
// generous because a sync triggered from a web request runs while the server
// still holds its own buffers, and running out mid-handshake resets the device.
constexpr uint32_t kHttpsWorkingHeapBytes = 9500UL;
constexpr uint16_t kHttpsDefaultRecvBufferBytes = 16384;
constexpr uint16_t kHttpsCompactRecvBufferBytes = 4096;
constexpr uint16_t kHttpsCompactXmitBufferBytes = 512;
constexpr uint8_t kHttpsHostProfileCount = 4;

FeedConfig savedFeedConfig;
bool feedDraftActive = false;
uint32_t feedsStartupReadyAtMs = 0;

struct HttpsHostProfile {
    bool used;
    bool probed;
    char host[40];
    uint16_t port;
    uint16_t fragmentLength;
};

struct HttpJsonRequestOptions {
    const char *authorizationBearer;
};

HttpsHostProfile httpsHostProfiles[kHttpsHostProfileCount] = {};

bool feedConfigEquals(const FeedConfig &left, const FeedConfig &right);
void updateDraftState();
void copyString(char *destination, size_t destinationSize, const char *source);

String trimStringCopy(const char *value) {
    String normalized = value != nullptr ? String(value) : String("");
    normalized.trim();
    return normalized;
}

String trimmedLowercaseString(const String &value) {
    String normalized = value;
    normalized.trim();
    normalized.toLowerCase();
    return normalized;
}


uint16_t clampRiverRefresh(uint16_t minutes);
uint16_t clampGithubRefresh(uint16_t minutes);
void clearRiverRuntime();
void clearGithubRuntime();
void fillRiverStatusJson(JsonObject root, const RiverFeedRuntime &runtime);
void fillGithubStatusJson(JsonObject root, const GithubFeedRuntime &runtime);
void applyRiverObject(JsonObjectConst root);
void applyGithubObject(JsonObjectConst root);
bool syncRiver(String *error);
bool syncGithub(String *error);
bool syncRiverIfDue(uint32_t now);
bool syncGithubIfDue(uint32_t now);
void seedKnownHttpsProfiles();

bool extractUrlHostPort(const String &url, String &host, uint16_t &port) {
    int schemeEnd = url.indexOf("://");
    if (schemeEnd < 0) {
        return false;
    }

    int hostStart = schemeEnd + 3;
    if (hostStart >= static_cast<int>(url.length())) {
        return false;
    }

    int pathStart = url.indexOf('/', hostStart);
    String authority = pathStart >= 0 ? url.substring(hostStart, pathStart) : url.substring(hostStart);
    if (authority.length() == 0) {
        return false;
    }

    int portSeparator = authority.indexOf(':');
    if (portSeparator >= 0) {
        host = authority.substring(0, portSeparator);
        int parsedPort = authority.substring(portSeparator + 1).toInt();
        port = parsedPort > 0 ? static_cast<uint16_t>(parsedPort) : 443;
    } else {
        host = authority;
        port = url.startsWith("https://") ? 443 : 80;
    }

    host.trim();
    return host.length() > 0;
}

HttpsHostProfile* findHttpsHostProfile(const String &host, uint16_t port) {
    HttpsHostProfile *freeSlot = nullptr;

    for (uint8_t index = 0; index < kHttpsHostProfileCount; ++index) {
        HttpsHostProfile &profile = httpsHostProfiles[index];
        if (!profile.used) {
            if (freeSlot == nullptr) {
                freeSlot = &profile;
            }
            continue;
        }

        if (profile.port == port && host.equalsIgnoreCase(profile.host)) {
            return &profile;
        }
    }

    if (freeSlot != nullptr) {
        memset(freeSlot, 0, sizeof(*freeSlot));
        freeSlot->used = true;
        freeSlot->port = port;
        copyString(freeSlot->host, sizeof(freeSlot->host), host.c_str());
    }

    return freeSlot;
}

uint16_t detectHttpsFragmentLength(const String &host, uint16_t port) {
    HttpsHostProfile *profile = findHttpsHostProfile(host, port);
    if (profile != nullptr && profile->probed) {
        return profile->fragmentLength;
    }

    static const uint16_t candidateLengths[] = {512, 1024, 2048, 4096};
    uint16_t fragmentLength = 0;
    for (uint8_t index = 0; index < sizeof(candidateLengths) / sizeof(candidateLengths[0]); ++index) {
        uint16_t candidate = candidateLengths[index];
        if (BearSSL::WiFiClientSecure::probeMaxFragmentLength(host, port, candidate)) {
            fragmentLength = candidate;
            break;
        }
    }

    if (profile != nullptr) {
        profile->probed = true;
        profile->fragmentLength = fragmentLength;
    }

    if (fragmentLength > 0) {
        logPrintf("HTTPS MFLN detected: %s:%u -> %u", host.c_str(), port, fragmentLength);
    } else {
        logPrintf("HTTPS MFLN unavailable: %s:%u", host.c_str(), port);
    }

    return fragmentLength;
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

    strncpy(destination, source, destinationSize - 1);
    destination[destinationSize - 1] = '\0';
}

void lowercaseCString(char *value) {
    if (value == nullptr) {
        return;
    }

    for (size_t index = 0; value[index] != '\0'; ++index) {
        value[index] = static_cast<char>(tolower(value[index]));
    }
}

uint16_t clampWeatherRefresh(uint16_t minutes) {
    return constrain(minutes, kWeatherMinRefreshMinutes, kWeatherMaxRefreshMinutes);
}

bool weatherCoordinatesValid(float latitude, float longitude) {
    return isfinite(latitude) &&
           isfinite(longitude) &&
           latitude >= -90.0f &&
           latitude <= 90.0f &&
           longitude >= -180.0f &&
           longitude <= 180.0f &&
           (fabsf(latitude) > 0.0001f || fabsf(longitude) > 0.0001f);
}

const char* weatherSourceToString(uint8_t source) {
    switch (source) {
        case WEATHER_FEED_OPEN_METEO:
            return "open-meteo";
        case WEATHER_FEED_DISABLED:
        default:
            return "disabled";
    }
}

uint8_t weatherSourceFromVariant(JsonVariantConst value) {
    if (value.isNull()) {
        return feedConfig.weather.source;
    }

    if (value.is<const char*>()) {
        String source = value.as<const char*>();
        source.toLowerCase();

        if (source == "open-meteo" || source == "openmeteo" || source == "meteo") {
            return WEATHER_FEED_OPEN_METEO;
        }
        return WEATHER_FEED_DISABLED;
    }

    return static_cast<uint8_t>(value.as<int>());
}

String urlEncode(const String &value) {
    static const char hex[] = "0123456789ABCDEF";
    String encoded;
    encoded.reserve(value.length() * 3);

    for (size_t index = 0; index < value.length(); ++index) {
        uint8_t character = static_cast<uint8_t>(value.charAt(index));
        if (isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~') {
            encoded += static_cast<char>(character);
            continue;
        }

        encoded += '%';
        encoded += hex[(character >> 4) & 0x0F];
        encoded += hex[character & 0x0F];
    }

    return encoded;
}

void trimAndCopyString(char *destination, size_t destinationSize, const char *source) {
    String normalized = trimStringCopy(source);
    copyString(destination, destinationSize, normalized.c_str());
}

bool isHexDigitChar(char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'A' && value <= 'F') ||
           (value >= 'a' && value <= 'f');
}

bool baseUrlValid(const String &baseUrl) {
    return baseUrl.startsWith("http://") || baseUrl.startsWith("https://");
}

String humanizeIdentifier(const String &value) {
    String label = value;
    int entitySeparator = label.lastIndexOf('.');
    if (entitySeparator >= 0 && entitySeparator < static_cast<int>(label.length()) - 1) {
        label = label.substring(entitySeparator + 1);
    }

    label.replace("_", " ");
    bool capitalizeNext = true;
    for (size_t index = 0; index < label.length(); ++index) {
        char character = label.charAt(index);
        if (capitalizeNext && character >= 'a' && character <= 'z') {
            label.setCharAt(index, static_cast<char>(toupper(character)));
            capitalizeNext = false;
        } else if (character == ' ') {
            capitalizeNext = true;
        } else {
            capitalizeNext = false;
        }
    }

    label.trim();
    return label;
}

String buildWeatherLocationLabel(JsonObjectConst item) {
    String label = item["name"].isNull() ? String("") : String(item["name"].as<const char*>());
    if (!item["admin1"].isNull() && String(item["admin1"].as<const char*>()).length() > 0) {
        label += ", " + String(item["admin1"].as<const char*>());
    }
    if (!item["country"].isNull() && String(item["country"].as<const char*>()).length() > 0) {
        label += ", " + String(item["country"].as<const char*>());
    }
    return label;
}

void buildWeatherSearchFilter(JsonDocument &filter) {
    JsonArray results = filter["results"].to<JsonArray>();
    JsonObject item = results.add<JsonObject>();
    item["name"] = true;
    item["country"] = true;
    item["admin1"] = true;
    item["latitude"] = true;
    item["longitude"] = true;
    item["timezone"] = true;
}

void buildWeatherForecastFilter(JsonDocument &filter) {
    JsonObject current = filter["current"].to<JsonObject>();
    current["temperature_2m"] = true;
    current["weather_code"] = true;

    JsonObject daily = filter["daily"].to<JsonObject>();
    daily["temperature_2m_max"] = true;
    daily["temperature_2m_min"] = true;
    daily["precipitation_probability_max"] = true;
}

void setConfigDefaults() {
    memset(&feedConfig, 0, sizeof(feedConfig));
    feedConfig.version = FEEDS_CONFIG_VERSION;

    // Weather is on by default, pointed at Seoul, so a fresh device shows a
    // live forecast without any setup (matching the river and github feeds).
    feedConfig.weather.source = WEATHER_FEED_OPEN_METEO;
    feedConfig.weather.refreshMinutes = 30;
    feedConfig.weather.useFahrenheit = false;
    copyString(feedConfig.weather.query, sizeof(feedConfig.weather.query), "Seoul");
    copyString(feedConfig.weather.label, sizeof(feedConfig.weather.label), "Seoul");
    feedConfig.weather.latitude = 37.5665f;
    feedConfig.weather.longitude = 126.9780f;

    feedConfig.river.enabled = true;
    copyString(feedConfig.river.apiKey, sizeof(feedConfig.river.apiKey), "sample");
    copyString(feedConfig.river.station, sizeof(feedConfig.river.station), kRiverDefaultStation);
    feedConfig.river.refreshMinutes = 20;

    feedConfig.github.enabled = true;
    feedConfig.github.useGlobalFeed = false;
    feedConfig.github.refreshMinutes = 3;
    // Organisations rather than single repositories: their feeds mix every
    // repository they own, so the page does not sit on one repo's star stream.
    copyString(feedConfig.github.repos[0], sizeof(feedConfig.github.repos[0]), "robotis-git");
    copyString(feedConfig.github.repos[1], sizeof(feedConfig.github.repos[1]), "ros2");
    copyString(feedConfig.github.repos[2], sizeof(feedConfig.github.repos[2]), "huggingface");
    copyString(feedConfig.github.repos[3], sizeof(feedConfig.github.repos[3]), "nvidia");
}

void clearWeatherRuntime() {
    memset(&feedRuntime.weather, 0, sizeof(feedRuntime.weather));
}

void clearAllRuntime() {
    clearWeatherRuntime();
    clearRiverRuntime();
    clearGithubRuntime();
}

void normalizeConfig() {
    feedConfig.version = FEEDS_CONFIG_VERSION;

    feedConfig.weather.source = constrain(feedConfig.weather.source, WEATHER_FEED_DISABLED, WEATHER_FEED_OPEN_METEO);
    if (feedConfig.weather.source == WEATHER_FEED_MANUAL) {
        feedConfig.weather.source = WEATHER_FEED_DISABLED;
    }
    feedConfig.weather.query[sizeof(feedConfig.weather.query) - 1] = '\0';
    feedConfig.weather.label[sizeof(feedConfig.weather.label) - 1] = '\0';
    feedConfig.weather.refreshMinutes = clampWeatherRefresh(feedConfig.weather.refreshMinutes);
    if (!isfinite(feedConfig.weather.latitude) || feedConfig.weather.latitude < -90.0f || feedConfig.weather.latitude > 90.0f) {
        feedConfig.weather.latitude = 0.0f;
    }
    if (!isfinite(feedConfig.weather.longitude) || feedConfig.weather.longitude < -180.0f || feedConfig.weather.longitude > 180.0f) {
        feedConfig.weather.longitude = 0.0f;
    }

    RiverFeedConfig &river = feedConfig.river;
    trimAndCopyString(river.apiKey, sizeof(river.apiKey), river.apiKey);
    trimAndCopyString(river.station, sizeof(river.station), river.station);
    if (river.apiKey[0] == '\0') {
        copyString(river.apiKey, sizeof(river.apiKey), "sample");
    }
    if (river.station[0] == '\0') {
        copyString(river.station, sizeof(river.station), kRiverDefaultStation);
    }
    river.refreshMinutes = clampRiverRefresh(river.refreshMinutes);

    GithubFeedConfig &github = feedConfig.github;
    trimAndCopyString(github.token, sizeof(github.token), github.token);
    for (uint8_t index = 0; index < GITHUB_REPO_SLOT_COUNT; ++index) {
        trimAndCopyString(github.repos[index], sizeof(github.repos[index]), github.repos[index]);
    }
    github.refreshMinutes = clampGithubRefresh(github.refreshMinutes);
}

void clearRuntimeForInactiveSources() {
    if (feedConfig.weather.source != WEATHER_FEED_OPEN_METEO || !feedsWeatherConfigured()) {
        clearWeatherRuntime();
    }

    if (!feedsRiverConfigured()) {
        clearRiverRuntime();
    }

    if (!feedsGithubConfigured()) {
        clearGithubRuntime();
    }
}

bool weatherIdentityChanged(const FeedConfig &before, const FeedConfig &after) {
    return before.weather.source != after.weather.source ||
           strcmp(before.weather.query, after.weather.query) != 0 ||
           strcmp(before.weather.label, after.weather.label) != 0 ||
           fabsf(before.weather.latitude - after.weather.latitude) > 0.0001f ||
           fabsf(before.weather.longitude - after.weather.longitude) > 0.0001f ||
           before.weather.useFahrenheit != after.weather.useFahrenheit;
}

bool weatherConfigEquals(const WeatherFeedConfig &left, const WeatherFeedConfig &right) {
    return left.source == right.source &&
           strcmp(left.query, right.query) == 0 &&
           strcmp(left.label, right.label) == 0 &&
           fabsf(left.latitude - right.latitude) <= 0.0001f &&
           fabsf(left.longitude - right.longitude) <= 0.0001f &&
           left.refreshMinutes == right.refreshMinutes &&
           left.useFahrenheit == right.useFahrenheit;
}

bool feedConfigEquals(const FeedConfig &left, const FeedConfig &right) {
    if (left.version != right.version || !weatherConfigEquals(left.weather, right.weather)) {
        return false;
    }

    if (left.river.enabled != right.river.enabled ||
        strcmp(left.river.apiKey, right.river.apiKey) != 0 ||
        strcmp(left.river.station, right.river.station) != 0 ||
        left.river.refreshMinutes != right.river.refreshMinutes) {
        return false;
    }

    if (left.github.enabled != right.github.enabled ||
        left.github.useGlobalFeed != right.github.useGlobalFeed ||
        strcmp(left.github.token, right.github.token) != 0 ||
        left.github.refreshMinutes != right.github.refreshMinutes) {
        return false;
    }

    for (uint8_t index = 0; index < GITHUB_REPO_SLOT_COUNT; ++index) {
        if (strcmp(left.github.repos[index], right.github.repos[index]) != 0) {
            return false;
        }
    }

    return true;
}

void updateDraftState() {
    feedDraftActive = !feedConfigEquals(feedConfig, savedFeedConfig);
}

void captureSavedConfig() {
    memcpy(&savedFeedConfig, &feedConfig, sizeof(savedFeedConfig));
    feedDraftActive = false;
}

void fillWeatherDataJson(JsonObject root, const WeatherData &weather) {
    root["location"] = weather.location;
    root["condition"] = weather.condition;
    root["temperature"] = weather.temperature;
    root["high"] = weather.high;
    root["low"] = weather.low;
    root["rainChance"] = weather.rainChance;
}

void fillWeatherStatusJson(JsonObject root, const WeatherFeedRuntime &runtime) {
    root["syncing"] = runtime.syncing;
    root["hasData"] = runtime.hasData;
    root["lastError"] = runtime.lastError;
    root["lastAttemptAgeSec"] = runtime.lastAttemptMs > 0 ? static_cast<long>((millis() - runtime.lastAttemptMs) / 1000UL) : -1;
    root["lastSuccessAgeSec"] = runtime.lastSuccessMs > 0 ? static_cast<long>((millis() - runtime.lastSuccessMs) / 1000UL) : -1;

    JsonObject data = root["data"].to<JsonObject>();
    if (runtime.hasData) {
        fillWeatherDataJson(data, runtime.data);
    }
}

void fillConfigJson(JsonObject root) {
    root["version"] = feedConfig.version;

    JsonObject weather = root["weather"].to<JsonObject>();
    weather["source"] = weatherSourceToString(feedConfig.weather.source);
    weather["query"] = feedConfig.weather.query;
    weather["label"] = feedConfig.weather.label;
    weather["latitude"] = feedConfig.weather.latitude;
    weather["longitude"] = feedConfig.weather.longitude;
    weather["refreshMinutes"] = feedConfig.weather.refreshMinutes;
    weather["useFahrenheit"] = feedConfig.weather.useFahrenheit;

    JsonObject river = root["river"].to<JsonObject>();
    river["enabled"] = feedConfig.river.enabled;
    river["apiKey"] = feedConfig.river.apiKey;
    river["station"] = feedConfig.river.station;
    river["refreshMinutes"] = feedConfig.river.refreshMinutes;

    JsonObject github = root["github"].to<JsonObject>();
    github["enabled"] = feedConfig.github.enabled;
    github["useGlobalFeed"] = feedConfig.github.useGlobalFeed;
    github["token"] = feedConfig.github.token;
    github["refreshMinutes"] = feedConfig.github.refreshMinutes;

    JsonArray githubRepos = github["repos"].to<JsonArray>();
    for (uint8_t index = 0; index < GITHUB_REPO_SLOT_COUNT; ++index) {
        githubRepos.add(feedConfig.github.repos[index]);
    }
}

void fillStatusJson(JsonObject root) {
    JsonObject weather = root["weather"].to<JsonObject>();
    fillWeatherStatusJson(weather, feedRuntime.weather);

    JsonObject river = root["river"].to<JsonObject>();
    fillRiverStatusJson(river, feedRuntime.river);

    JsonObject github = root["github"].to<JsonObject>();
    fillGithubStatusJson(github, feedRuntime.github);
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

const char* weatherCodeToText(int code) {
    switch (code) {
        case 0:
            return "Clear";
        case 1:
            return "Mostly clear";
        case 2:
            return "Partly cloudy";
        case 3:
            return "Overcast";
        case 45:
        case 48:
            return "Fog";
        case 51:
        case 53:
        case 55:
        case 56:
        case 57:
            return "Drizzle";
        case 61:
        case 63:
        case 65:
        case 66:
        case 67:
            return "Rain";
        case 71:
        case 73:
        case 75:
        case 77:
            return "Snow";
        case 80:
        case 81:
        case 82:
            return "Showers";
        case 85:
        case 86:
            return "Snow showers";
        case 95:
            return "Thunderstorm";
        case 96:
        case 99:
            return "Storm";
        default:
            return "Weather";
    }
}

bool httpGetJson(const String &url,
                 JsonDocument &doc,
                 String *error,
                 JsonDocument *filter = nullptr,
                 const HttpJsonRequestOptions *options = nullptr) {
    auto finishJsonRequest = [&](HTTPClient &http, const char *transportLabel) {
        http.addHeader("Accept", "application/json");
        if (options != nullptr &&
            options->authorizationBearer != nullptr &&
            options->authorizationBearer[0] != '\0') {
            http.addHeader("Authorization", String("Bearer ") + options->authorizationBearer);
        }
        int responseCode = http.GET();
        if (responseCode != HTTP_CODE_OK) {
            String httpError = responseCode < 0 ? HTTPClient::errorToString(responseCode) : "";
            if (error != nullptr) {
                *error = responseCode < 0
                             ? ("HTTP " + String(responseCode) + " " + httpError)
                             : ("HTTP " + String(responseCode));
            }
            if (responseCode < 0 && httpError.length() > 0) {
                logPrintf("%s GET failed: %d %s (%s)",
                          transportLabel,
                          responseCode,
                          httpError.c_str(),
                          url.c_str());
            } else {
                logPrintf("%s GET failed: %d (%s)", transportLabel, responseCode, url.c_str());
            }
            http.end();
            return false;
        }

        doc.clear();
        DeserializationError jsonError =
            filter == nullptr
                ? deserializeJson(doc, http.getStream())
                : deserializeJson(doc,
                                  http.getStream(),
                                  DeserializationOption::Filter(filter->as<JsonVariantConst>()));
        http.end();

        if (jsonError) {
            if (error != nullptr) {
                *error = jsonError == DeserializationError::NoMemory
                             ? "JSON memory exhausted"
                             : String(jsonError.c_str());
            }
            logPrintf("%s JSON parse failed: %s (%s)", transportLabel, jsonError.c_str(), url.c_str());
            return false;
        }

        return true;
    };

    HTTPClient http;
    http.setTimeout(kHttpTimeoutMs);
    http.useHTTP10(true);
    http.setUserAgent(F("SmartClock/1.0"));

    if (url.startsWith("http://")) {
        WiFiClient client;
        if (!http.begin(client, url)) {
            if (error != nullptr) {
                *error = "HTTP begin failed";
            }
            return false;
        }

        return finishJsonRequest(http, "HTTP");
    }

    if (!url.startsWith("https://")) {
        if (error != nullptr) {
            *error = "Unsupported URL scheme";
        }
        return false;
    }

    struct DisplayHeapGuard {
        DisplayHeapGuard() {
            displaySuspendDynamicResources();
        }

        ~DisplayHeapGuard() {
            displayResumeDynamicResources();
        }
    } displayHeapGuard;

    uint32_t freeHeap = ESP.getFreeHeap();

    // Work out the receive buffer before checking the heap. A host that
    // negotiates a small TLS fragment needs a fraction of the 16 kB default, so
    // a flat requirement would reject every request on a device that in
    // practice never has that much free.
    String host;
    uint16_t port = 443;
    uint16_t recvBufferSize = kHttpsDefaultRecvBufferBytes;
    if (extractUrlHostPort(url, host, port)) {
        HttpsHostProfile *profile = findHttpsHostProfile(host, port);
        if (profile != nullptr && profile->probed) {
            if (profile->fragmentLength > 0) {
                recvBufferSize = profile->fragmentLength;
            }
        } else if (freeHeap >= kHttpsPreferredFreeHeapBytes) {
            uint16_t fragmentLength = detectHttpsFragmentLength(host, port);
            if (fragmentLength > 0) {
                recvBufferSize = fragmentLength;
            }
        }
    }

    uint32_t requiredHeap = static_cast<uint32_t>(recvBufferSize) + kHttpsWorkingHeapBytes;
    if (freeHeap < requiredHeap) {
        if (error != nullptr) {
            *error = "Low heap, retrying later";
        }
        logPrintf("Skipping HTTPS request, free heap %u below required %u (%s)",
                  freeHeap,
                  requiredHeap,
                  url.c_str());
        return false;
    }

    std::unique_ptr<BearSSL::WiFiClientSecure> client(new BearSSL::WiFiClientSecure());
    if (!client) {
        if (error != nullptr) {
            *error = "TLS client allocation failed";
        }
        return false;
    }

    // Only api.github.com is reached over TLS now, and it is a public endpoint,
    // so the certificate is not pinned.
    client->setInsecure();
    client->setBufferSizes(recvBufferSize, kHttpsCompactXmitBufferBytes);
    logPrintf("HTTPS buffers: recv=%u xmit=%u (%s)",
              recvBufferSize,
              kHttpsCompactXmitBufferBytes,
              url.c_str());

    if (!http.begin(*client, url)) {
        if (error != nullptr) {
            *error = "HTTP begin failed";
        }
        return false;
    }

    return finishJsonRequest(http, "HTTPS");
}

bool fetchWeatherSearchJson(const String &query, JsonDocument &providerDoc, String *error) {
    String trimmedQuery = query;
    trimmedQuery.trim();

    if (trimmedQuery.length() < 2) {
        if (error != nullptr) {
            *error = "Enter at least 2 characters";
        }
        return false;
    }

    String url = "http://geocoding-api.open-meteo.com/v1/search?name=" + urlEncode(trimmedQuery) +
                 "&count=6&language=en&format=json";
    JsonDocument filter;
    buildWeatherSearchFilter(filter);
    return httpGetJson(url, providerDoc, error, &filter);
}

bool resolveWeatherLocationFromQuery(String *error) {
    JsonDocument providerDoc;
    String requestError;
    if (!fetchWeatherSearchJson(feedConfig.weather.query, providerDoc, &requestError)) {
        if (error != nullptr) {
            *error = requestError;
        }
        return false;
    }

    JsonArrayConst providerResults = providerDoc["results"].as<JsonArrayConst>();
    if (providerResults.isNull() || providerResults.size() == 0) {
        if (error != nullptr) {
            *error = "City not found";
        }
        return false;
    }

    JsonObjectConst item = providerResults[0].as<JsonObjectConst>();
    if (item.isNull() || item["latitude"].isNull() || item["longitude"].isNull()) {
        if (error != nullptr) {
            *error = "Location payload incomplete";
        }
        return false;
    }

    feedConfig.weather.latitude = item["latitude"].as<float>();
    feedConfig.weather.longitude = item["longitude"].as<float>();

    String resolvedLabel = buildWeatherLocationLabel(item);
    if (!resolvedLabel.isEmpty()) {
        copyString(feedConfig.weather.label, sizeof(feedConfig.weather.label), resolvedLabel.c_str());
    }

    updateDraftState();
    return true;
}

bool syncWeather(String *error) {
    if (WiFi.status() != WL_CONNECTED) {
        if (error != nullptr) {
            *error = "WiFi disconnected";
        }
        return false;
    }

    if (feedConfig.weather.source != WEATHER_FEED_OPEN_METEO || !feedsWeatherConfigured()) {
        if (error != nullptr) {
            *error = "Weather feed not configured";
        }
        return false;
    }

    WeatherFeedRuntime &runtime = feedRuntime.weather;
    runtime.syncing = true;
    runtime.lastAttemptMs = millis();
    runtime.lastError[0] = '\0';

    if (!weatherCoordinatesValid(feedConfig.weather.latitude, feedConfig.weather.longitude)) {
        String locationError;
        if (!resolveWeatherLocationFromQuery(&locationError)) {
            runtime.syncing = false;
            copyString(runtime.lastError, sizeof(runtime.lastError), locationError.c_str());
            if (error != nullptr) {
                *error = locationError;
            }
            return false;
        }
    }

    String url = "http://api.open-meteo.com/v1/forecast?latitude=" + String(feedConfig.weather.latitude, 4) +
                 "&longitude=" + String(feedConfig.weather.longitude, 4) +
                 "&current=temperature_2m,weather_code" +
                 "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max" +
                 "&forecast_days=1&timezone=auto";
    if (feedConfig.weather.useFahrenheit) {
        url += "&temperature_unit=fahrenheit";
    } else {
        url += "&temperature_unit=celsius";
    }

    JsonDocument doc;
    String requestError;
    JsonDocument filter;
    buildWeatherForecastFilter(filter);
    if (!httpGetJson(url, doc, &requestError, &filter)) {
        runtime.syncing = false;
        copyString(runtime.lastError, sizeof(runtime.lastError), requestError.c_str());
        if (error != nullptr) {
            *error = requestError;
        }
        return false;
    }

    JsonObjectConst current = doc["current"].as<JsonObjectConst>();
    JsonObjectConst daily = doc["daily"].as<JsonObjectConst>();
    JsonArrayConst highs = daily["temperature_2m_max"].as<JsonArrayConst>();
    JsonArrayConst lows = daily["temperature_2m_min"].as<JsonArrayConst>();
    JsonArrayConst rain = daily["precipitation_probability_max"].as<JsonArrayConst>();

    if (current.isNull() || current["temperature_2m"].isNull()) {
        runtime.syncing = false;
        copyString(runtime.lastError, sizeof(runtime.lastError), "Weather payload incomplete");
        if (error != nullptr) {
            *error = runtime.lastError;
        }
        return false;
    }

    memset(&runtime.data, 0, sizeof(runtime.data));
    const char *locationLabel = feedConfig.weather.label[0] != '\0' ? feedConfig.weather.label : feedConfig.weather.query;
    copyString(runtime.data.location, sizeof(runtime.data.location), locationLabel);
    copyString(runtime.data.condition, sizeof(runtime.data.condition), weatherCodeToText(current["weather_code"] | 0));
    runtime.data.temperature = static_cast<int>(roundf(current["temperature_2m"].as<float>()));
    runtime.data.high = highs.isNull() || highs.size() == 0
                            ? runtime.data.temperature
                            : static_cast<int>(roundf(highs[0].as<float>()));
    runtime.data.low = lows.isNull() || lows.size() == 0
                           ? runtime.data.temperature
                           : static_cast<int>(roundf(lows[0].as<float>()));
    runtime.data.rainChance = rain.isNull() || rain.size() == 0
                                  ? 0
                                  : constrain(static_cast<int>(roundf(rain[0].as<float>())), 0, 100);

    runtime.hasData = true;
    runtime.syncing = false;
    runtime.lastSuccessMs = millis();
    runtime.lastError[0] = '\0';
    logPrintf("Weather sync OK: %s %d", runtime.data.location, runtime.data.temperature);
    return true;
}

void applyWeatherObject(JsonObjectConst weather) {
    if (weather.isNull()) {
        return;
    }

    if (!weather["source"].isNull()) {
        feedConfig.weather.source = weatherSourceFromVariant(weather["source"]);
    }
    if (!weather["query"].isNull()) {
        copyString(feedConfig.weather.query, sizeof(feedConfig.weather.query), weather["query"]);
    }
    if (!weather["label"].isNull()) {
        copyString(feedConfig.weather.label, sizeof(feedConfig.weather.label), weather["label"]);
    }
    if (!weather["latitude"].isNull()) {
        feedConfig.weather.latitude = weather["latitude"].as<float>();
    }
    if (!weather["longitude"].isNull()) {
        feedConfig.weather.longitude = weather["longitude"].as<float>();
    }
    if (!weather["refreshMinutes"].isNull()) {
        feedConfig.weather.refreshMinutes = weather["refreshMinutes"].as<uint16_t>();
    }
    if (!weather["useFahrenheit"].isNull()) {
        feedConfig.weather.useFahrenheit = weather["useFahrenheit"].as<bool>();
    }
}

void applyConfigObject(JsonObjectConst root) {
    applyWeatherObject(root["weather"].as<JsonObjectConst>());
    applyRiverObject(root["river"].as<JsonObjectConst>());
    applyGithubObject(root["github"].as<JsonObjectConst>());
    normalizeConfig();
}

bool syncWeatherIfDue(uint32_t now) {
    if (feedConfig.weather.source != WEATHER_FEED_OPEN_METEO || !feedsWeatherConfigured()) {
        return false;
    }

    uint32_t intervalMs = static_cast<uint32_t>(feedConfig.weather.refreshMinutes) * 60UL * 1000UL;
    if (feedRuntime.weather.syncing) {
        return false;
    }

    if (feedRuntime.weather.lastAttemptMs != 0 && now - feedRuntime.weather.lastAttemptMs < intervalMs) {
        return false;
    }

    String error;
    bool success = syncWeather(&error);
    if (!success && error.length() > 0) {
        logPrintf("Weather sync failed: %s", error.c_str());
    }
    return success;
}

// The API reports "점검중" (under maintenance) instead of a number whenever a
// station is offline, so every reading has to be validated before use.
bool parseWaterTemperature(const char *raw, float &celsius) {
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }

    char *end = nullptr;
    float value = strtof(raw, &end);
    if (end == raw || !isfinite(value)) {
        return false;
    }

    if (value < -5.0f || value > 45.0f) {
        return false;
    }

    celsius = value;
    return true;
}

void riverStationRomanised(const char *station, char *out, size_t outSize) {
    struct StationName {
        const char *korean;
        const char *roman;
    };

    static const StationName kNames[] = {
        {"선유", "SEONYU"},
        {"노량진", "NORYANGJIN"},
        {"탄천", "TANCHEON"},
        {"중랑천", "JUNGNANG"},
        {"안양천", "ANYANG"},
    };

    for (const StationName &name : kNames) {
        if (strcmp(station, name.korean) == 0) {
            copyString(out, outSize, name.roman);
            return;
        }
    }

    copyString(out, outSize, "HAN RIVER");
}

bool syncRiver(String *error) {
    RiverFeedRuntime &runtime = feedRuntime.river;
    runtime.syncing = true;
    runtime.lastAttemptMs = millis();
    runtime.lastError[0] = '\0';

    // The shared sample key is limited to five rows per request.
    bool sampleKey = strcmp(feedConfig.river.apiKey, "sample") == 0;
    uint8_t rows = sampleKey ? 5 : 10;
    String url = String("http://openapi.seoul.go.kr:8088/") + feedConfig.river.apiKey +
                 "/json/WPOSInformationTime/1/" + String(rows) + "/";

    JsonDocument filter;
    JsonObject filterRow = filter["WPOSInformationTime"]["row"][0].to<JsonObject>();
    filterRow["MSRSTN_NM"] = true;
    filterRow["WATT"] = true;
    filterRow["HR"] = true;

    JsonDocument doc;
    String requestError;
    if (!httpGetJson(url, doc, &requestError, &filter)) {
        runtime.syncing = false;
        copyString(runtime.lastError, sizeof(runtime.lastError), requestError.c_str());
        if (error != nullptr) {
            *error = requestError;
        }
        return false;
    }

    JsonArrayConst entries = doc["WPOSInformationTime"]["row"].as<JsonArrayConst>();
    if (entries.isNull() || entries.size() == 0) {
        // An invalid key makes the service answer with an XML error document,
        // which leaves the filtered result empty.
        runtime.syncing = false;
        copyString(runtime.lastError, sizeof(runtime.lastError), "No station rows (check API key)");
        if (error != nullptr) {
            *error = runtime.lastError;
        }
        return false;
    }

    // Rows arrive newest first. Prefer the configured station and fall back to
    // any station that is currently reporting a usable number.
    const char *chosenStation = nullptr;
    const char *chosenHour = nullptr;
    float chosenTemperature = 0.0f;
    bool found = false;

    for (JsonObjectConst entry : entries) {
        const char *station = entry["MSRSTN_NM"] | "";
        float celsius = 0.0f;
        if (!parseWaterTemperature(entry["WATT"] | "", celsius)) {
            continue;
        }

        bool preferred = strcmp(station, feedConfig.river.station) == 0;
        if (!found || preferred) {
            chosenStation = station;
            chosenHour = entry["HR"] | "";
            chosenTemperature = celsius;
            found = true;
        }

        if (preferred) {
            break;
        }
    }

    if (!found) {
        runtime.syncing = false;
        copyString(runtime.lastError, sizeof(runtime.lastError), "All stations under maintenance");
        if (error != nullptr) {
            *error = runtime.lastError;
        }
        return false;
    }

    memset(&runtime.data, 0, sizeof(runtime.data));
    riverStationRomanised(chosenStation, runtime.data.station, sizeof(runtime.data.station));
    copyString(runtime.data.observedAt, sizeof(runtime.data.observedAt), chosenHour != nullptr ? chosenHour : "");
    runtime.data.temperature = chosenTemperature;
    runtime.data.hasTemperature = true;

    runtime.hasData = true;
    runtime.syncing = false;
    runtime.lastSuccessMs = millis();
    runtime.lastError[0] = '\0';
    logPrintf("River sync OK: %s %.1fC", runtime.data.station, runtime.data.temperature);
    return true;
}

// A stream of "STAR torvalds/linux" reads as if the same repository is being
// starred over and over. Showing the running total makes it obvious that each
// event is one person adding to a large number. Only fetched for star events,
// so it costs one extra call on the events that would otherwise say nothing.
bool fetchStarCount(const char *repo, String &formatted) {
    if (repo == nullptr || strchr(repo, '/') == nullptr) {
        return false;
    }

    JsonDocument filter;
    filter["stargazers_count"] = true;

    HttpJsonRequestOptions options = {};
    if (feedConfig.github.token[0] != '\0') {
        options.authorizationBearer = feedConfig.github.token;
    }

    JsonDocument doc;
    String url = String("https://api.github.com/repos/") + repo;
    if (!httpGetJson(url, doc, nullptr, &filter, &options)) {
        return false;
    }

    if (doc["stargazers_count"].isNull()) {
        return false;
    }

    long stars = doc["stargazers_count"].as<long>();
    String digits = String(stars);
    formatted = "";
    for (int index = 0; index < static_cast<int>(digits.length()); ++index) {
        if (index > 0 && ((digits.length() - index) % 3) == 0) {
            formatted += ',';
        }
        formatted += digits.charAt(index);
    }
    formatted += " stars";
    return true;
}


// GitHub timestamps are ISO 8601 in UTC. Converting here keeps the display code
// free of date handling and lets the age be recomputed as the screen redraws.
uint32_t parseGithubTimestamp(const char *value) {
    if (value == nullptr) {
        return 0;
    }

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(value, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &minute, &second) != 6) {
        return 0;
    }

    // Days from the civil epoch, per Howard Hinnant's algorithm.
    int shiftedYear = year - (month <= 2 ? 1 : 0);
    int era = (shiftedYear >= 0 ? shiftedYear : shiftedYear - 399) / 400;
    unsigned yearOfEra = static_cast<unsigned>(shiftedYear - era * 400);
    unsigned dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    long days = static_cast<long>(era) * 146097 + static_cast<long>(dayOfEra) - 719468;

    return static_cast<uint32_t>((days * 86400L) + (hour * 3600) + (minute * 60) + second);
}


void githubEventKind(const char *type, char *out, size_t outSize) {
    struct EventKind {
        const char *type;
        const char *label;
    };

    static const EventKind kKinds[] = {
        {"WatchEvent", "STAR"},
        {"ForkEvent", "FORK"},
        {"PushEvent", "PUSH"},
        {"PullRequestEvent", "PULL REQ"},
        {"PullRequestReviewEvent", "REVIEW"},
        {"PullRequestReviewCommentEvent", "REVIEW"},
        {"IssuesEvent", "ISSUE"},
        {"IssueCommentEvent", "COMMENT"},
        {"CommitCommentEvent", "COMMENT"},
        {"CreateEvent", "CREATE"},
        {"DeleteEvent", "DELETE"},
        {"ReleaseEvent", "RELEASE"},
        {"GollumEvent", "WIKI"},
        {"PublicEvent", "PUBLIC"},
        {"MemberEvent", "MEMBER"},
    };

    for (const EventKind &kind : kKinds) {
        if (strcmp(type, kind.type) == 0) {
            copyString(out, outSize, kind.label);
            return;
        }
    }

    copyString(out, outSize, "EVENT");
}

bool syncRiverIfDue(uint32_t now) {
    if (!feedsRiverConfigured()) {
        return false;
    }

    RiverFeedRuntime &runtime = feedRuntime.river;
    uint32_t intervalMs = static_cast<uint32_t>(feedConfig.river.refreshMinutes) * 60UL * 1000UL;
    if (runtime.syncing) {
        return false;
    }

    if (runtime.lastAttemptMs != 0 && now - runtime.lastAttemptMs < intervalMs) {
        return false;
    }

    String error;
    bool success = syncRiver(&error);
    if (!success && error.length() > 0) {
        logPrintf("River sync failed: %s", error.c_str());
    }
    return success;
}

bool syncGithubIfDue(uint32_t now) {
    if (!feedsGithubConfigured()) {
        return false;
    }

    GithubFeedRuntime &runtime = feedRuntime.github;
    uint32_t intervalMs = static_cast<uint32_t>(feedConfig.github.refreshMinutes) * 60UL * 1000UL;
    if (runtime.syncing) {
        return false;
    }

    if (runtime.lastAttemptMs != 0 && now - runtime.lastAttemptMs < intervalMs) {
        return false;
    }

    String error;
    bool success = syncGithub(&error);
    if (!success && error.length() > 0) {
        logPrintf("GitHub sync failed: %s", error.c_str());
    }
    return success;
}

// api.github.com negotiates the TLS max_fragment_length extension, which lets
// BearSSL work with a 512 byte receive buffer instead of the default 16 kB.
// The runtime probe that would discover this only runs while the free heap is
// above kHttpsPreferredFreeHeapBytes, and this build sits close enough to the
// RAM ceiling that it may never reach that level, so the known result is seeded
// instead. If GitHub ever stops offering the extension the handshake fails and
// the feed reports an error rather than exhausting the heap.
void seedKnownHttpsProfiles() {
    HttpsHostProfile *profile = findHttpsHostProfile(String("api.github.com"), 443);
    if (profile == nullptr) {
        return;
    }

    profile->probed = true;
    profile->fragmentLength = 512;
}

void fillRiverStatusJson(JsonObject root, const RiverFeedRuntime &runtime) {
    root["syncing"] = runtime.syncing;
    root["hasData"] = runtime.hasData;
    root["lastError"] = runtime.lastError;
    root["lastAttemptAgeSec"] = runtime.lastAttemptMs > 0 ? static_cast<long>((millis() - runtime.lastAttemptMs) / 1000UL) : -1;
    root["lastSuccessAgeSec"] = runtime.lastSuccessMs > 0 ? static_cast<long>((millis() - runtime.lastSuccessMs) / 1000UL) : -1;

    JsonObject data = root["data"].to<JsonObject>();
    if (runtime.hasData) {
        data["station"] = runtime.data.station;
        data["observedAt"] = runtime.data.observedAt;
        data["temperature"] = runtime.data.temperature;
    }
}

void fillGithubStatusJson(JsonObject root, const GithubFeedRuntime &runtime) {
    root["syncing"] = runtime.syncing;
    root["hasData"] = runtime.hasData;
    root["lastError"] = runtime.lastError;
    root["lastAttemptAgeSec"] = runtime.lastAttemptMs > 0 ? static_cast<long>((millis() - runtime.lastAttemptMs) / 1000UL) : -1;
    root["lastSuccessAgeSec"] = runtime.lastSuccessMs > 0 ? static_cast<long>((millis() - runtime.lastSuccessMs) / 1000UL) : -1;

    JsonArray events = root["events"].to<JsonArray>();
    for (uint8_t index = 0; index < runtime.data.count; ++index) {
        JsonObject event = events.add<JsonObject>();
        event["kind"] = runtime.data.events[index].kind;
        event["repo"] = runtime.data.events[index].repo;
        event["actor"] = runtime.data.events[index].actor;
        event["detail"] = runtime.data.events[index].detail;
    }
}

void applyRiverObject(JsonObjectConst root) {
    if (root.isNull()) {
        return;
    }

    if (!root["enabled"].isNull()) {
        feedConfig.river.enabled = root["enabled"].as<bool>();
    }
    if (!root["apiKey"].isNull()) {
        copyString(feedConfig.river.apiKey, sizeof(feedConfig.river.apiKey), root["apiKey"] | "");
    }
    if (!root["station"].isNull()) {
        copyString(feedConfig.river.station, sizeof(feedConfig.river.station), root["station"] | "");
    }
    if (!root["refreshMinutes"].isNull()) {
        feedConfig.river.refreshMinutes = root["refreshMinutes"].as<uint16_t>();
    }
}

void applyGithubObject(JsonObjectConst root) {
    if (root.isNull()) {
        return;
    }

    if (!root["enabled"].isNull()) {
        feedConfig.github.enabled = root["enabled"].as<bool>();
    }
    if (!root["useGlobalFeed"].isNull()) {
        feedConfig.github.useGlobalFeed = root["useGlobalFeed"].as<bool>();
    }
    if (!root["token"].isNull()) {
        copyString(feedConfig.github.token, sizeof(feedConfig.github.token), root["token"] | "");
    }
    if (!root["refreshMinutes"].isNull()) {
        feedConfig.github.refreshMinutes = root["refreshMinutes"].as<uint16_t>();
    }

    JsonArrayConst repos = root["repos"].as<JsonArrayConst>();
    if (repos.isNull()) {
        return;
    }

    for (uint8_t index = 0; index < GITHUB_REPO_SLOT_COUNT; ++index) {
        const char *value = index < repos.size() ? (repos[index] | "") : "";
        copyString(feedConfig.github.repos[index], sizeof(feedConfig.github.repos[index]), value);
    }
}

void clearRiverRuntime() {
    memset(&feedRuntime.river, 0, sizeof(feedRuntime.river));
}

void clearGithubRuntime() {
    memset(&feedRuntime.github, 0, sizeof(feedRuntime.github));
}

uint16_t clampRiverRefresh(uint16_t minutes) {
    return constrain(minutes, kRiverMinRefreshMinutes, kRiverMaxRefreshMinutes);
}

uint16_t clampGithubRefresh(uint16_t minutes) {
    return constrain(minutes, kGithubMinRefreshMinutes, kGithubMaxRefreshMinutes);
}

bool syncGithub(String *error) {
    GithubFeedRuntime &runtime = feedRuntime.github;
    runtime.syncing = true;
    runtime.lastAttemptMs = millis();
    runtime.lastError[0] = '\0';

    String url;
    if (feedConfig.github.useGlobalFeed) {
        url = "https://api.github.com/events?per_page=3";
    } else {
        // Walk the configured repositories one per refresh so a single request
        // stays small while the page still shows a variety of projects.
        const char *repo = nullptr;
        for (uint8_t attempt = 0; attempt < GITHUB_REPO_SLOT_COUNT; ++attempt) {
            uint8_t index = (runtime.nextRepoIndex + attempt) % GITHUB_REPO_SLOT_COUNT;
            if (feedConfig.github.repos[index][0] != '\0') {
                repo = feedConfig.github.repos[index];
                runtime.nextRepoIndex = (index + 1) % GITHUB_REPO_SLOT_COUNT;
                break;
            }
        }

        if (repo == nullptr) {
            runtime.syncing = false;
            copyString(runtime.lastError, sizeof(runtime.lastError), "No repositories configured");
            if (error != nullptr) {
                *error = runtime.lastError;
            }
            return false;
        }

        // A slash means a single repository; a bare name is an organisation,
        // whose feed mixes every repository it owns and so repeats far less.
        // One event per request either way: a pull-request entry alone can be
        // several kilobytes, and the JSON parser has to share what little heap
        // is left once the TLS session has taken its share.
        bool isRepository = strchr(repo, '/') != nullptr;
        url = String("https://api.github.com/") + (isRepository ? "repos/" : "orgs/") + repo +
              "/events?per_page=1";
    }

    JsonDocument filter;
    JsonObject filterEntry = filter[0].to<JsonObject>();
    filterEntry["type"] = true;
    filterEntry["actor"]["login"] = true;
    filterEntry["repo"]["name"] = true;
    filterEntry["payload"]["action"] = true;
    filterEntry["payload"]["ref"] = true;
    filterEntry["created_at"] = true;

    HttpJsonRequestOptions options = {};
    if (feedConfig.github.token[0] != '\0') {
        options.authorizationBearer = feedConfig.github.token;
    }

    JsonDocument doc;
    String requestError;
    if (!httpGetJson(url, doc, &requestError, &filter, &options)) {
        runtime.syncing = false;
        copyString(runtime.lastError, sizeof(runtime.lastError), requestError.c_str());
        if (error != nullptr) {
            *error = requestError;
        }
        return false;
    }

    JsonArrayConst entries = doc.as<JsonArrayConst>();
    if (entries.isNull() || entries.size() == 0) {
        runtime.syncing = false;
        copyString(runtime.lastError, sizeof(runtime.lastError), "Empty event list");
        if (error != nullptr) {
            *error = runtime.lastError;
        }
        return false;
    }

    memset(&runtime.data, 0, sizeof(runtime.data));
    for (JsonObjectConst entry : entries) {
        if (runtime.data.count >= DASHBOARD_GITHUB_EVENT_COUNT) {
            break;
        }

        GithubEventEntry &slot = runtime.data.events[runtime.data.count];
        githubEventKind(entry["type"] | "", slot.kind, sizeof(slot.kind));
        copyString(slot.repo, sizeof(slot.repo), entry["repo"]["name"] | "");
        copyString(slot.actor, sizeof(slot.actor), entry["actor"]["login"] | "");

        // "started" (a star) and "created" (a comment or review) only repeat
        // what the event kind already says, so they are dropped in favour of
        // the branch name when there is one.
        slot.createdAt = parseGithubTimestamp(entry["created_at"] | "");

        const char *action = entry["payload"]["action"] | "";
        const char *ref = entry["payload"]["ref"] | "";
        bool actionIsInformative = action[0] != '\0' &&
                                   strcmp(action, "started") != 0 &&
                                   strcmp(action, "created") != 0;
        if (actionIsInformative) {
            copyString(slot.detail, sizeof(slot.detail), action);
        } else if (ref[0] != '\0') {
            const char *shortRef = strrchr(ref, '/');
            copyString(slot.detail, sizeof(slot.detail), shortRef != nullptr ? shortRef + 1 : ref);
        }

        if (slot.repo[0] != '\0') {
            runtime.data.count++;
        }
    }

    if (runtime.data.count == 0) {
        runtime.syncing = false;
        copyString(runtime.lastError, sizeof(runtime.lastError), "No usable events");
        if (error != nullptr) {
            *error = runtime.lastError;
        }
        return false;
    }

    // Stars carry no useful action word, so the slot is spent on the total.
    // The count is only fetched when the repository actually changed: polling
    // returns the same event for minutes at a time, and each lookup is a second
    // request against an hourly budget of sixty.
    static char lastStarRepo[40] = {0};
    static char lastStarText[32] = {0};

    for (uint8_t index = 0; index < runtime.data.count; ++index) {
        GithubEventEntry &slot = runtime.data.events[index];
        if (strcmp(slot.kind, "STAR") != 0 || slot.detail[0] != '\0') {
            continue;
        }

        if (strcmp(slot.repo, lastStarRepo) == 0 && lastStarText[0] != '\0') {
            copyString(slot.detail, sizeof(slot.detail), lastStarText);
            break;
        }

        String starText;
        if (fetchStarCount(slot.repo, starText)) {
            copyString(slot.detail, sizeof(slot.detail), starText.c_str());
            copyString(lastStarRepo, sizeof(lastStarRepo), slot.repo);
            copyString(lastStarText, sizeof(lastStarText), starText.c_str());
        }
        break;
    }

    runtime.hasData = true;
    runtime.syncing = false;
    runtime.lastSuccessMs = millis();
    runtime.lastError[0] = '\0';
    logPrintf("GitHub sync OK: %u events (%s)", runtime.data.count, runtime.data.events[0].repo);
    return true;
}

}  // namespace

void feedsResetToDefaults() {
    setConfigDefaults();
    clearAllRuntime();
    feedsStartupReadyAtMs = 0;
}

void feedsInit() {
    feedsResetToDefaults();
    seedKnownHttpsProfiles();
    if (!feedsLoadConfig()) {
        feedsSaveConfig();
    }
}

bool feedsLoadConfig() {
    setConfigDefaults();
    clearAllRuntime();

    bool success = loadJsonFile(FEEDS_CONFIG_PATH, [](JsonObjectConst root) {
        applyConfigObject(root);
    });

    normalizeConfig();
    clearRuntimeForInactiveSources();
    captureSavedConfig();
    return success;
}

bool feedsSaveConfig() {
    normalizeConfig();

    JsonDocument doc;
    fillConfigJson(doc.to<JsonObject>());
    bool success = writeJsonToFile(FEEDS_CONFIG_PATH, doc);
    if (success) {
        captureSavedConfig();
    }
    return success;
}

bool feedsPreviewConfigJson(const String &json, String *error) {
    JsonDocument doc;
    DeserializationError deserializeError = deserializeJson(doc, json);
    if (deserializeError) {
        if (error != nullptr) {
            *error = deserializeError.c_str();
        }
        return false;
    }

    FeedConfig previousConfig = feedConfig;
    memcpy(&feedConfig, &savedFeedConfig, sizeof(feedConfig));
    applyConfigObject(doc.as<JsonObjectConst>());
    clearRuntimeForInactiveSources();

    if (weatherIdentityChanged(previousConfig, feedConfig)) {
        clearWeatherRuntime();
    }

    feedDraftActive = !feedConfigEquals(feedConfig, savedFeedConfig);
    return true;
}

bool feedsApplyConfigJson(const String &json, String *error) {
    if (!feedsPreviewConfigJson(json, error)) {
        return false;
    }

    if (!feedsSaveConfig()) {
        if (error != nullptr) {
            *error = "Failed to write feed config";
        }
        return false;
    }

    return true;
}

void feedsDiscardDraftChanges() {
    FeedConfig previousConfig = feedConfig;
    memcpy(&feedConfig, &savedFeedConfig, sizeof(feedConfig));
    clearRuntimeForInactiveSources();

    if (weatherIdentityChanged(previousConfig, feedConfig)) {
        clearWeatherRuntime();
    }

    feedDraftActive = false;
}

bool feedsHasDraftChanges() {
    return feedDraftActive;
}

void feedsBuildStateJson(String &json) {
    JsonDocument doc;
    fillConfigJson(doc["config"].to<JsonObject>());
    fillStatusJson(doc["status"].to<JsonObject>());
    JsonObject meta = doc["meta"].to<JsonObject>();
    meta["hasDraft"] = feedDraftActive;
    json = "";
    serializeJson(doc, json);
}

bool feedsSyncNow(const char *scope, String *error) {
    String scopeValue = scope != nullptr ? String(scope) : String("all");
    scopeValue.toLowerCase();

    bool wantWeather = scopeValue == "all" || scopeValue == "weather";
    bool wantRiver = scopeValue == "all" || scopeValue == "river";
    bool wantGithub = scopeValue == "all" || scopeValue == "github";

    bool anyConfigured = false;
    bool anySuccess = false;
    String firstError;

    if (wantWeather && feedConfig.weather.source == WEATHER_FEED_OPEN_METEO && feedsWeatherConfigured()) {
        anyConfigured = true;
        String weatherError;
        bool success = syncWeather(&weatherError);
        anySuccess = anySuccess || success;
        if (!success && firstError.isEmpty()) {
            firstError = weatherError;
        }
        yield();
    }

    if (wantRiver && feedsRiverConfigured()) {
        anyConfigured = true;
        String riverError;
        bool success = syncRiver(&riverError);
        anySuccess = anySuccess || success;
        if (!success && firstError.isEmpty()) {
            firstError = riverError;
        }
        yield();
    }

    if (wantGithub && feedsGithubConfigured()) {
        // Opening a TLS session while the web server still holds its request
        // buffers is what made manual syncs fail on a device this tight for
        // heap. Clearing the timer hands the work to the background loop, which
        // runs a moment later with the memory to spare.
        anyConfigured = true;
        anySuccess = true;
        feedRuntime.github.lastAttemptMs = 0;
        logPrint(F("GitHub sync queued for the next loop pass"));
    }

    if (!anyConfigured) {
        if (error != nullptr) {
            *error = "No live feeds configured";
        }
        return false;
    }

    if (!anySuccess && error != nullptr) {
        *error = firstError.isEmpty() ? "Feed sync failed" : firstError;
    }

    return anySuccess;
}

bool feedsSearchWeatherLocations(const String &query, String &json, String *error) {
    JsonDocument providerDoc;
    if (!fetchWeatherSearchJson(query, providerDoc, error)) {
        return false;
    }

    JsonDocument resultDoc;
    JsonArray results = resultDoc["results"].to<JsonArray>();
    JsonArrayConst providerResults = providerDoc["results"].as<JsonArrayConst>();

    for (JsonObjectConst item : providerResults) {
        JsonObject result = results.add<JsonObject>();
        String label = buildWeatherLocationLabel(item);

        result["name"] = item["name"] | "";
        result["label"] = label;
        result["country"] = item["country"] | "";
        result["admin1"] = item["admin1"] | "";
        result["latitude"] = item["latitude"] | 0.0f;
        result["longitude"] = item["longitude"] | 0.0f;
        result["timezone"] = item["timezone"] | "";
    }

    json = "";
    serializeJson(resultDoc, json);
    return true;
}

void feedsLoop() {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    uint32_t now = millis();
    if (feedsStartupReadyAtMs == 0) {
        feedsStartupReadyAtMs = now + kFeedStartupGracePeriodMs;
        logPrintf("Delaying automatic feed sync for %u ms after boot", kFeedStartupGracePeriodMs);
        return;
    }

    if (static_cast<int32_t>(now - feedsStartupReadyAtMs) < 0) {
        return;
    }

    bool anyUpdated = syncWeatherIfDue(now);
    yield();
    anyUpdated = syncRiverIfDue(now) || anyUpdated;
    yield();
    anyUpdated = syncGithubIfDue(now) || anyUpdated;

    if (anyUpdated) {
        logPrint(F("Feeds refreshed"));
    }
}

uint8_t feedsWeatherSource() {
    return feedConfig.weather.source;
}

bool feedsWeatherConfigured() {
    if (feedConfig.weather.source != WEATHER_FEED_OPEN_METEO) {
        return false;
    }

    if (weatherCoordinatesValid(feedConfig.weather.latitude, feedConfig.weather.longitude)) {
        return true;
    }

    String query = feedConfig.weather.query;
    query.trim();
    return query.length() >= 2;
}

bool feedsHasWeatherData() {
    return feedRuntime.weather.hasData;
}

bool feedsWeatherUsesFahrenheit() {
    return feedConfig.weather.source == WEATHER_FEED_OPEN_METEO && feedConfig.weather.useFahrenheit;
}

const WeatherData* feedsWeatherData() {
    return feedRuntime.weather.hasData ? &feedRuntime.weather.data : nullptr;
}


bool feedsRiverConfigured() {
    return feedConfig.river.enabled && feedConfig.river.apiKey[0] != '\0';
}

bool feedsHasRiverData() {
    return feedRuntime.river.hasData;
}

const RiverData* feedsRiverData() {
    if (!feedRuntime.river.hasData) {
        return nullptr;
    }

    return &feedRuntime.river.data;
}

bool feedsGithubConfigured() {
    if (!feedConfig.github.enabled) {
        return false;
    }

    if (feedConfig.github.useGlobalFeed) {
        return true;
    }

    for (uint8_t index = 0; index < GITHUB_REPO_SLOT_COUNT; ++index) {
        if (feedConfig.github.repos[index][0] != '\0') {
            return true;
        }
    }

    return false;
}

bool feedsHasGithubData() {
    return feedRuntime.github.hasData && feedRuntime.github.data.count > 0;
}

const GithubData* feedsGithubData() {
    if (!feedsHasGithubData()) {
        return nullptr;
    }

    return &feedRuntime.github.data;
}

void feedsGithubAdvanceCursor() {
    GithubData &data = feedRuntime.github.data;
    if (data.count == 0) {
        return;
    }

    data.cursor = (data.cursor + 1) % data.count;
}
