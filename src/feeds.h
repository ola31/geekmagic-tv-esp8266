#ifndef FEEDS_H
#define FEEDS_H

#include <Arduino.h>
#include "dashboard.h"

#define FEEDS_CONFIG_PATH "/feeds-config.json"
#define FEEDS_CONFIG_VERSION 4
#define GITHUB_REPO_SLOT_COUNT 4

enum WeatherFeedSource : uint8_t {
    WEATHER_FEED_DISABLED = 0,
    WEATHER_FEED_MANUAL,
    WEATHER_FEED_OPEN_METEO
};

struct WeatherFeedConfig {
    uint8_t source;
    char query[40];
    char label[24];
    float latitude;
    float longitude;
    uint16_t refreshMinutes;
    bool useFahrenheit;
};

// Seoul Open Data Plaza publishes hourly readings from the automatic water
// quality stations on the Han River and its tributaries. The "sample" key works
// without registration but is capped at five rows.
struct RiverFeedConfig {
    bool enabled;
    char apiKey[40];
    char station[24];   // preferred station name as the API spells it (UTF-8)
    uint16_t refreshMinutes;
};

// The global api.github.com/events feed only carries PushEvent entries from
// throwaway repositories, so watching a handful of well-known repositories
// gives far better content. useGlobalFeed switches back to the firehose.
struct GithubFeedConfig {
    bool enabled;
    bool useGlobalFeed;
    char token[64];     // optional; raises the rate limit from 60 to 5000/hour
    char repos[GITHUB_REPO_SLOT_COUNT][40];
    uint16_t refreshMinutes;
};

struct FeedConfig {
    uint16_t version;
    WeatherFeedConfig weather;
    RiverFeedConfig river;
    GithubFeedConfig github;
};

struct WeatherFeedRuntime {
    bool hasData;
    bool syncing;
    uint32_t lastAttemptMs;
    uint32_t lastSuccessMs;
    char lastError[64];
    WeatherData data;
};

struct RiverFeedRuntime {
    bool hasData;
    bool syncing;
    uint32_t lastAttemptMs;
    uint32_t lastSuccessMs;
    char lastError[64];
    RiverData data;
};

struct GithubFeedRuntime {
    bool hasData;
    bool syncing;
    uint32_t lastAttemptMs;
    uint32_t lastSuccessMs;
    uint8_t nextRepoIndex;   // rotates through the configured repositories
    char lastError[64];
    GithubData data;
};

struct FeedRuntimeState {
    WeatherFeedRuntime weather;
    RiverFeedRuntime river;
    GithubFeedRuntime github;
};

extern FeedConfig feedConfig;
extern FeedRuntimeState feedRuntime;

void feedsInit();
void feedsLoop();
void feedsResetToDefaults();
bool feedsLoadConfig();
bool feedsSaveConfig();
bool feedsPreviewConfigJson(const String &json, String *error = nullptr);
bool feedsApplyConfigJson(const String &json, String *error = nullptr);
void feedsDiscardDraftChanges();
bool feedsHasDraftChanges();
void feedsBuildStateJson(String &json);
bool feedsSyncNow(const char *scope = "all", String *error = nullptr);
bool feedsSearchWeatherLocations(const String &query, String &json, String *error = nullptr);

uint8_t feedsWeatherSource();
bool feedsWeatherConfigured();
bool feedsHasWeatherData();
bool feedsWeatherUsesFahrenheit();
const WeatherData* feedsWeatherData();

bool feedsRiverConfigured();
bool feedsHasRiverData();
const RiverData* feedsRiverData();
bool feedsGithubConfigured();
bool feedsHasGithubData();
const GithubData* feedsGithubData();
void feedsGithubAdvanceCursor();

#endif
