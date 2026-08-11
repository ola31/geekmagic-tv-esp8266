// Stepping-stone firmware for the GeekMagic SmallTV Ultra.
//
// Its only job is to replace the stock image with something small that declares
// a 1 MB filesystem instead of 3 MB. That moves the filesystem start address far
// enough up the flash map to leave room for the real firmware, which is roughly
// 900 KB and cannot fit in the ~520 KB of staging space the stock image leaves.
//
// Once this is running: join WiFi through the setup portal, then upload the full
// firmware at http://<device-ip>/update.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Updater.h>
#include <ESP8266mDNS.h>
#include <TFT_eSPI.h>
#include <WiFiManager.h>

namespace {

constexpr uint8_t kBacklightPin = 5;
constexpr char kSetupApName[] = "SmallTV-Setup";
constexpr char kHostname[] = "smalltv";

TFT_eSPI tft;
ESP8266WebServer server(80);
bool strictUpdate = false;

void showLines(const char *title, const String &line1, const String &line2, uint16_t accent) {
    tft.fillScreen(TFT_BLACK);

    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(accent, TFT_BLACK);
    tft.drawString(title, 120, 30, 4);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(line1, 120, 100, 4);

    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString(line2, 120, 150, 2);
}

void onPortalStarted(WiFiManager *manager) {
    showLines("SETUP", kSetupApName, "join this WiFi to configure", TFT_ORANGE);
    (void)manager;
}

}  // namespace

void setup() {
    Serial.begin(115200);

    tft.init();
    tft.setRotation(0);
    tft.invertDisplay(true);   // this ST7789 panel ships inverted
    tft.fillScreen(TFT_BLACK);

    // The backlight has to be driven after tft.init(), because TFT_eSPI claims
    // TFT_BL during init and would undo an earlier setting.
    //
    // This panel's backlight is ACTIVE-LOW: PWM 0 is full brightness and PWM
    // 1023 is off (the main firmware inverts brightness the same way). Writing
    // 1023 here is what left the screen dark while WiFi and /update still
    // worked, so drive it to 0 for full brightness.
    pinMode(kBacklightPin, OUTPUT);
    analogWriteFreq(1000);
    analogWriteRange(1023);
    analogWrite(kBacklightPin, 0);

    showLines("BOOTSTRAP", "starting", "", TFT_CYAN);

    WiFiManager manager;
    manager.setAPCallback(onPortalStarted);
    manager.setConfigPortalTimeout(300);

    if (!manager.autoConnect(kSetupApName)) {
        showLines("NO WIFI", "restarting", "setup timed out", TFT_RED);
        delay(3000);
        ESP.restart();
    }

    WiFi.hostname(kHostname);
    MDNS.begin(kHostname);

    // A hand written updater rather than ESP8266HTTPUpdateServer: that one
    // finishes with Update.end(true), which accepts a truncated upload and
    // writes it to the boot slot. This one refuses anything that does not
    // arrive whole and match its checksum.
    server.on("/update", HTTP_POST,
        []() {
            bool ok = Update.isFinished() && !Update.hasError();
            server.send(200, "text/plain", ok ? "OK - Rebooting..." : "FAILED - not flashed");
            if (ok) {
                delay(500);
                ESP.restart();
            }
        },
        []() {
            HTTPUpload &upload = server.upload();
            if (upload.status == UPLOAD_FILE_START) {
                // The declared size is what makes verification meaningful: with
                // only the free space to go on, the updater cannot tell a
                // complete image from a truncated one.
                uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
                // Never stage past the bootable app slot (0x001000..0x100000):
                // a larger image cannot boot anyway.
                constexpr uint32_t kAppSlotSize = 0xFF000u;
                if (maxSketchSpace > kAppSlotSize) {
                    maxSketchSpace = kAppSlotSize;
                }
                uint32_t declared = server.hasArg("size") ? server.arg("size").toInt() : 0;
                strictUpdate = declared > 0 && declared <= maxSketchSpace;
                if (!Update.begin(strictUpdate ? declared : maxSketchSpace)) {
                    Update.printError(Serial);
                    return;
                }
                if (server.hasArg("md5")) {
                    String expected = server.arg("md5");
                    expected.trim();
                    expected.toLowerCase();
                    if (expected.length() != 32 || !Update.setMD5(expected.c_str())) {
                        Serial.println(F("rejected: malformed md5"));
                        Update.end(false);
                    }
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                // Strict only when the client told us how big the image is.
                if (!Update.end(!strictUpdate)) {
                    Update.printError(Serial);
                }
            } else if (upload.status == UPLOAD_FILE_ABORTED) {
                Update.end(false);
            }
        });
    server.on("/", []() {
        server.send(200,
                    "text/html",
                    F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                      "<h2>SmallTV bootstrap</h2>"
                      "<p>Upload the full firmware at <a href='/update'>/update</a>.</p>"));
    });
    server.begin();
    MDNS.addService("http", "tcp", 80);

    showLines("READY", WiFi.localIP().toString(), "upload firmware at /update", TFT_GREEN);
    Serial.printf("Bootstrap ready at http://%s/update\n", WiFi.localIP().toString().c_str());
    Serial.printf("Free sketch space: %u bytes\n", ESP.getFreeSketchSpace());
}

void loop() {
    server.handleClient();
    MDNS.update();
}
