/**
 * @file main.cpp
 * @brief IOT Forest Cam — Dual-mode: Leaf (CoAP Server) or Gateway (CoAP Client)
 *
 * Build with:
 *   pio run -e esp32s3           → Leaf node (WiFi AP + serves images)
 *   pio run -e esp32s3_gateway   → Gateway node (WiFi STA + downloads images)
 *
 * Hardware: LILYGO T3-S3 V1.2
 *   - OLED: SSD1306 128x64 (I2C: SDA=18, SCL=17)
 *   - SD Card: HSPI (MOSI=11, MISO=2, CLK=14, CS=13)
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

#ifdef GATEWAY_MODE
  #include <SD.h>
  #include "CoapClient.h"
#else
  #include "StorageReader.h"
  #include "CoapServer.h"
#endif

// ─── LILYGO T3-S3 V1.2 Pin Definitions ──────────────────────
#define OLED_SDA 18
#define OLED_SCL 17

// ─── Shared OLED ─────────────────────────────────────────────
Adafruit_SSD1306 display(128, 64, &Wire, -1);

void displayStatus(const char* line) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("IOT Forest Cam");
    display.println(line);
    display.display();
}

// Use HSPI bus for SD card (same as StorageReader)
static SPIClass gwSPI(HSPI);

// =============================================================
// GATEWAY MODE — WiFi STA + CoAP Client
// =============================================================
#ifdef GATEWAY_MODE

// ─── Gateway Configuration ──────────────────────────────────
// Change LEAF_SSID to match your leaf node's SSID shown on its OLED
static const char*      LEAF_SSID = "ForestCam-79D8";
static const char*      LEAF_PASS = "forestcam123";
static const IPAddress  LEAF_IP(192, 168, 4, 1);
static const uint16_t   LEAF_PORT = COAP_DEFAULT_PORT;

CoapClient coapClient;

void setup() {
    Serial.begin(115200);
    delay(1000);

    // ── OLED Init ────────────────────────────────────────────
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    displayStatus("Gateway Mode");

    Serial.println("\n===========================");
    Serial.println("  IOT Forest Cam — Gateway");
    Serial.println("===========================\n");

    // ── SD Card Init (for saving received images) ────────────
    displayStatus("Mounting SD...");

    pinMode(VSENSOR_SD_CS, OUTPUT);
    digitalWrite(VSENSOR_SD_CS, HIGH);
    delay(100);
    gwSPI.begin(VSENSOR_SD_CLK, VSENSOR_SD_MISO, VSENSOR_SD_MOSI, VSENSOR_SD_CS);

    if (!SD.begin(VSENSOR_SD_CS, gwSPI, 4000000)) {   // 4 MHz SPI for reliable writes
        Serial.println("[WARN] SD card not available — checksum-only mode");
    } else {
        Serial.println("[OK] SD card mounted for receiving images");
        SD.mkdir("/received");
    }

    // ── Connect to Leaf's WiFi AP ────────────────────────────
    displayStatus("Connecting WiFi...");
    Serial.printf("Connecting to %s...\n", LEAF_SSID);

    WiFi.mode(WIFI_STA);
    WiFi.begin(LEAF_SSID, LEAF_PASS);

    uint32_t wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
        delay(500);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        displayStatus("WiFi FAILED!");
        Serial.printf("[ERROR] Cannot connect to %s\n", LEAF_SSID);
        while (true) delay(1000);
    }

    Serial.printf("[OK] Connected to %s\n", LEAF_SSID);
    Serial.printf("[OK] Gateway IP: %s\n", WiFi.localIP().toString().c_str());

    // ── Start CoAP Client ────────────────────────────────────
    if (!coapClient.begin()) {
        displayStatus("CoAP FAILED!");
        Serial.println("[ERROR] CoAP client failed to start");
        while (true) delay(1000);
    }
    Serial.println("[OK] CoAP client ready\n");

    // ── Display Status ───────────────────────────────────────
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Gateway Mode");
    display.printf("Leaf: %s\n", LEAF_SSID);
    display.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    display.println("────────────────────");
    display.println("Fetching images...");
    display.display();

    // ── Fetch /info to discover image count ────────────────────
    Serial.println("=== GET /info ===");
    uint8_t infoBuf[512];
    size_t infoLen = sizeof(infoBuf);
    CoapClientError err = coapClient.get(LEAF_IP, LEAF_PORT, "info",
                                          infoBuf, infoLen);

    uint8_t imageCount = 0;
    if (err == COAP_CLIENT_OK) {
        infoBuf[infoLen] = '\0';
        Serial.printf("Response: %s\n\n", (char*)infoBuf);

        // Parse "count":N from JSON
        const char* countKey = strstr((char*)infoBuf, "\"count\":");
        if (countKey) {
            imageCount = (uint8_t)atoi(countKey + 8);
        }
    } else {
        Serial.printf("[ERROR] GET /info failed: %s\n\n", coapClientErrorStr(err));
    }

    if (imageCount == 0) {
        displayStatus("No images found!");
        Serial.println("[ERROR] No images available on leaf node.");
        Serial.println("\n=== Gateway idle ===");
        return;
    }

    Serial.printf("Found %u image(s) — downloading all...\n\n", imageCount);

    // ── Download all images ─────────────────────────────────
    bool sdAvailable = SD.exists("/received");
    uint8_t passCount = 0;
    uint8_t failCount = 0;
    uint32_t totalBytes = 0;
    uint32_t totalTimeMs = 0;

    for (uint8_t i = 0; i < imageCount; i++) {
        Serial.printf("=== Download /image/%u ===\n", i);

        // Build output path: /received/img_000.jpg, img_001.jpg, ...
        char outPath[32];
        const char* savePath = nullptr;
        if (sdAvailable) {
            snprintf(outPath, sizeof(outPath), "/received/img_%03u.jpg", i);
            savePath = outPath;
        }

        TransferStats stats;
        err = coapClient.downloadImage(LEAF_IP, LEAF_PORT, i, savePath, stats);

        if (err == COAP_CLIENT_OK) {
            Serial.printf("  Bytes: %lu | Blocks: %lu | Time: %lu ms | Speed: %.1f KB/s | Retries: %lu\n",
                          stats.totalBytes, stats.totalBlocks, stats.elapsedMs,
                          stats.throughputKBps, stats.retryCount);
            if (savePath) {
                Serial.printf("  Saved to: %s\n", savePath);
            }

            // Verify checksum
            bool match = false;
            err = coapClient.verifyChecksum(LEAF_IP, LEAF_PORT, i,
                                             stats.computedChecksum, match);
            if (err == COAP_CLIENT_OK && match) {
                Serial.printf("  Checksum: 0x%04X — PASS\n\n", stats.computedChecksum);
                passCount++;
            } else {
                Serial.printf("  Checksum: FAIL\n\n");
                failCount++;
            }

            totalBytes += stats.totalBytes;
            totalTimeMs += stats.elapsedMs;
        } else {
            Serial.printf("  [ERROR] Download failed: %s\n\n", coapClientErrorStr(err));
            failCount++;
        }

        // Update OLED progress
        display.clearDisplay();
        display.setCursor(0, 0);
        display.printf("Downloading %u/%u\n", i + 1, imageCount);
        display.printf("Image %u: %lu B\n", i, stats.totalBytes);
        display.printf("Speed: %.1f KB/s\n", stats.throughputKBps);
        display.printf("Pass:%u Fail:%u\n", passCount, failCount);
        display.display();
    }

    // ── Final summary ───────────────────────────────────────
    float avgSpeed = (totalTimeMs > 0)
        ? (float)totalBytes / (float)totalTimeMs * 1000.0f / 1024.0f
        : 0.0f;

    Serial.println("=============================");
    Serial.printf("  All transfers complete\n");
    Serial.printf("  Images:  %u/%u passed\n", passCount, imageCount);
    Serial.printf("  Total:   %lu bytes\n", totalBytes);
    Serial.printf("  Time:    %lu ms\n", totalTimeMs);
    Serial.printf("  Avg:     %.1f KB/s\n", avgSpeed);
    Serial.println("=============================");

    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Gateway — Done");
    display.printf("Images: %u/%u OK\n", passCount, imageCount);
    display.printf("Total: %lu B\n", totalBytes);
    display.printf("Time: %lu ms\n", totalTimeMs);
    display.printf("Avg: %.1f KB/s\n", avgSpeed);
    display.printf("Fail: %u\n", failCount);
    display.display();

    Serial.println("\n=== Gateway idle ===");
}

void loop() {
    // Gateway is idle after one-shot test.
    // Future: periodic polling or LoRa-triggered downloads.
    delay(10000);
}

// =============================================================
// LEAF MODE — WiFi AP + CoAP Server (default)
// =============================================================
#else

static const char* AP_SSID_PREFIX = "ForestCam";
static const char* AP_PASS        = "forestcam123";
static char _apSSID[32];

StorageReader storage;
CoapServer    coapServer(storage);

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
        while (true) delay(1000);
    }

    Serial.printf("[OK] SD card: %d image(s) found\n", storage.imageCount());

    if (storage.imageCount() == 0) {
        displayStatus("No images on SD!");
        Serial.println("[ERROR] No JPEG files in /images/ directory.");
        while (true) delay(1000);
    }

    for (uint8_t i = 0; i < storage.imageCount(); i++) {
        ImageInfo info;
        if (storage.getImageInfo(i, info)) {
            Serial.printf("  [%u] %s — %lu bytes (%lu blocks)\n",
                          i, info.filename, info.fileSize, info.totalBlocks);
        }
    }

    // ── WiFi AP Init ─────────────────────────────────────────
    displayStatus("Starting WiFi AP...");

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

void loop() {
    coapServer.loop();

    static uint32_t lastDisplayUpdate = 0;
    if (millis() - lastDisplayUpdate > 2000) {
        lastDisplayUpdate = millis();
        display.fillRect(0, 56, 128, 8, SSD1306_BLACK);
        display.setCursor(0, 56);
        display.printf("Reqs:%-4lu Blks:%-5lu",
                       coapServer.requestCount(), coapServer.blocksSent());
        display.display();
    }
}

#endif // GATEWAY_MODE
