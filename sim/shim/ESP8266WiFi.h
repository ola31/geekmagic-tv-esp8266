#pragma once
#include "Arduino.h"
struct SimIp { String toString() const { return String("192.168.60.204"); } };
struct SimWiFi {
    int status() const { return 3; }
    SimIp localIP() const { return SimIp(); }
    String SSID() const { return String("wifi"); }
};
static SimWiFi WiFi;
#define WL_CONNECTED 3
