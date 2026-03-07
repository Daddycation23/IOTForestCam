/**
 * @file main.cpp
 * @brief IOT Forest Cam — CoAP Image Server
 *
 * Sets up the ESP32-S3 as a WiFi Access Point running a CoAP server
 * that serves JPEG images from the SD card using Block-Wise Transfer.
 *
 * Hardware: LILYGO T3-S3 V1.2
 *   - OLED: SSD1306 128x64 (I2C: SDA=18, SCL=17)
 *   - SD Card: HSPI (MOSI=11, MISO=2, CLK=14, CS=13)
 *
 * Testing with Python (aiocoap):
 *   1. Connect to WiFi AP shown on OLED
 *   2. pip install aiocoap
 *   3. aiocoap-client coap://192.168.4.1/info
 *   4. aiocoap-client coap://192.168.4.1/image/0 -o image.jpg
 *
 * Testing with libcoap:
 *   coap-client -m get coap://192.168.4.1/info
 *   coap-client -m get -b 512 coap://192.168.4.1/image/0 -o image.jpg
 *
 * @author  CS Group 2
 * @date    2026
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "StorageReader.h"
#include "CoapServer.h"

// ─── LILYGO T3-S3 V1.2 Pin Definitions ──────────────────────
#define OLED_SDA 18
#define OLED_SCL 17

// ─── WiFi AP Configuration ──────────────────────────────────
// In the final mesh system, WiFi will be managed by the mesh layer.
// For now, we use AP mode so a gateway/laptop can connect and test.
static const char* AP_SSID_PREFIX = "ForestCam";
static const char* AP_PASS        = "forestcam123";

// ─── Global Objects ──────────────────────────────────────────
Adafruit_SSD1306 display(128, 64, &Wire, -1);
StorageReader    storage;
CoapServer       coapServer(storage);

// ─── OLED Helper ─────────────────────────────────────────────
static char _apSSID[32];  // Store SSID for display updates

void displayStatus(const char* line) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("IOT Forest Cam");
    display.println(line);
    display.display();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void setup() {
    Serial.begin(115200);
    delay(1000);

    // ── OLED Init ────────────────────────────────────────────
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    displayStatus("Booting...");

    Serial.println("\n=============================");
    Serial.println("  IOT Forest Cam — CoAP Server");
    Serial.println("=============================\n");

    // ── SD Card Init ─────────────────────────────────────────
    displayStatus("Mounting SD...");

    if (!storage.begin()) {
        displayStatus("SD Mount FAILED!");
        Serial.println("[ERROR] SD card mount failed. Check wiring and /images/ folder.");
        while (true) delay(1000);  // Halt — no point continuing without images
    }

    Serial.printf("[OK] SD card: %d image(s) found\n", storage.imageCount());

    if (storage.imageCount() == 0) {
        displayStatus("No images on SD!");
        Serial.println("[ERROR] No JPEG files in /images/ directory.");
        while (true) delay(1000);
    }

    // Log image catalogue
    for (uint8_t i = 0; i < storage.imageCount(); i++) {
        ImageInfo info;
        if (storage.getImageInfo(i, info)) {
            Serial.printf("  [%u] %s — %lu bytes (%lu blocks)\n",
                          i, info.filename, info.fileSize, info.totalBlocks);
        }
    }

    // ── WiFi AP Init ─────────────────────────────────────────
    displayStatus("Starting WiFi AP...");

    // Generate unique SSID from MAC address
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(_apSSID, sizeof(_apSSID), "%s-%02X%02X",
             AP_SSID_PREFIX, mac[4], mac[5]);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(_apSSID, AP_PASS);
    delay(100);

    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[OK] WiFi AP: %s (pass: %s)\n", _apSSID, AP_PASS);
    Serial.printf("[OK] IP: %s\n", ip.toString().c_str());

    // ── CoAP Server Init ─────────────────────────────────────
    if (!coapServer.begin()) {
        displayStatus("CoAP FAILED!");
        Serial.println("[ERROR] CoAP server failed to start.");
        while (true) delay(1000);
    }

    Serial.printf("[OK] CoAP server on port %u\n", COAP_DEFAULT_PORT);
    Serial.println();
    Serial.println("Resources:");
    Serial.println("  GET /info             — Image catalogue (JSON)");
    Serial.println("  GET /image/{n}        — Image via Block2 transfer");
    Serial.println("  GET /checksum/{n}     — Fletcher-16 checksum (JSON)");
    Serial.println("  GET /.well-known/core — Resource discovery");
    Serial.println();
    Serial.println("Test with:");
    Serial.printf("  aiocoap-client coap://%s/info\n", ip.toString().c_str());
    Serial.printf("  aiocoap-client coap://%s/image/0 -o image.jpg\n",
                  ip.toString().c_str());
    Serial.println();

    // ── Display Ready Status ─────────────────────────────────
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("IOT Forest Cam");
    display.printf("AP: %s\n", _apSSID);
    display.printf("IP: %s\n", ip.toString().c_str());
    display.printf("CoAP: :%u\n", COAP_DEFAULT_PORT);
    display.printf("Images: %d\n", storage.imageCount());
    display.println("────────────────────");
    display.println("GET /image/{n}");
    display.println("Reqs: 0  Blks: 0");
    display.display();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void loop() {
    // Process incoming CoAP requests
    coapServer.loop();

    // Update OLED stats every 2 seconds
    static uint32_t lastDisplayUpdate = 0;
    if (millis() - lastDisplayUpdate > 2000) {
        lastDisplayUpdate = millis();

        // Only update the bottom line to avoid flicker
        display.fillRect(0, 56, 128, 8, SSD1306_BLACK);
        display.setCursor(0, 56);
        display.printf("Reqs:%-4lu Blks:%-5lu",
                       coapServer.requestCount(), coapServer.blocksSent());
        display.display();
    }
}
