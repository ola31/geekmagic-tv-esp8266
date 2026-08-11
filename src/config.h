#ifndef CONFIG_H
#define CONFIG_H

// Pin definitions
#define PIN_BACKLIGHT 5
#define PIN_BUTTON 4

// Display settings
#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 240

// WiFi settings
#define WIFI_AP_NAME "SmartClock-Setup"
// AP password is auto-generated (8-digit numeric) at runtime
#define WIFI_TIMEOUT 180
#define WIFI_RETRY_ATTEMPTS 5
#define WIFI_RETRY_DELAY_MS 2000
#define WIFI_CONNECTION_TIMEOUT 30000  // 30 seconds per attempt
#define WIFI_MONITOR_INTERVAL 60000    // Check WiFi every 60 seconds
#define WIFI_RECONNECT_INTERVAL 300000 // Try to reconnect every 5 minutes in AP mode

// mDNS settings
#define MDNS_HOSTNAME "smartclock"

// OTA settings
#define OTA_HOSTNAME "smartclock"

// Web server
#define WEB_SERVER_PORT 80

// Time synchronization
#define NTP_SERVER "pool.ntp.org"

// Update intervals
#define SCROLL_INTERVAL 30
// Five frames a second: enough for the river page's water to read as moving
// rather than stepping. Pages without animation only repaint when their
// content hash changes, so the faster tick costs them nothing.
#define DISPLAY_UPDATE_INTERVAL 200

// Button settings
#define BUTTON_DEBOUNCE_MS 50
#define BUTTON_SHORT_PRESS_MAX_MS 800
#define BUTTON_LONG_PRESS_MIN_MS 2000

// Filesystem
#define IMAGE_DIR "/image/"

// Preferences namespace
#define PREF_NAMESPACE "smartclock"

#endif
