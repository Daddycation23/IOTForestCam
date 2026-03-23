/**
 * @file main.cpp
 * @brief IOT Forest Cam — Unified firmware with FreeRTOS task separation
 *
 * Role is selected at boot via a 5-second OLED menu using the built-in
 * BOOT button (GPIO 0).  The selection is persisted to NVS so it survives
 * power cycles — no recompilation needed to change roles.
 *
 * FreeRTOS Architecture:
 *   Core 0: taskLoRa — LoRa RX/TX, beacon, AODV routing, election
 *   Core 1: taskHarvest — WiFi connect, CoAP download (gateway)
 *           taskCoapServer — CoAP server (leaf/relay)
 *   OLED updates run in the Arduino loop() at low priority.
 *
 * Build: pio run -e esp32s3_unified
 *
 * Hardware: LILYGO T3-S3 V1.2
 *   - OLED:    SSD1306 128x64 (I2C: SDA=18, SCL=17)
 *   - SD Card: HSPI (MOSI=11, MISO=2, CLK=14, CS=13)
 *   - LoRa:    SX1280 FSPI (MOSI=6, MISO=3, SCK=5, CS=7, DIO1=9, BUSY=36, RST=8, RXEN=21, TXEN=10)
 *   - BOOT:    GPIO 0 (active-low, used for role selection at boot)
 *
 * @author  CS Group 2
 * @date    2026
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_system.h>
#include <atomic>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// ── LoRa + AODV (all roles) ──────────────────────────────────
#include "LoRaRadio.h"
#include "LoRaBeacon.h"
#include "LoRaDispatch.h"
#include "AodvPacket.h"
#include "AodvRouter.h"

// ── Gateway role ─────────────────────────────────────────────
#include "CoapClient.h"
#include "NodeRegistry.h"
#include "HarvestLoop.h"

// ── Leaf / Relay role ────────────────────────────────────────
#include "StorageReader.h"
#include "CoapServer.h"

// ── Dynamic role selection ───────────────────────────────────
#include "RoleConfig.h"
#include "ElectionManager.h"

// ── FreeRTOS task configuration ─────────────────────────────
#include "TaskConfig.h"

// ── Deep sleep management ───────────────────────────────────
#include "DeepSleepManager.h"
#include <driver/gpio.h>     // gpio_hold_dis/gpio_deep_sleep_hold_dis on wake

// ── Serial command parsing ──────────────────────────────────
#include "SerialCmd.h"

// ─── LILYGO T3-S3 V1.2 Pin Definitions ──────────────────────
#define OLED_SDA 18
#define OLED_SCL 17

// ─── Runtime role ─────────────────────────────────────────────
/// Set once in setup() by RoleConfig::selectRole(); never modified after that.
NodeRole g_role = NODE_ROLE_LEAF;
/// Runtime role — may differ from g_role when a relay promotes to gateway.
/// Checked every loop() iteration to select loopGateway() vs loopLeafRelay().
/// Atomic: written by electionMgr on Core 0, read by loop() on Core 1.
std::atomic<uint8_t> _activeRole{NODE_ROLE_LEAF};

// ─── Shared hardware ──────────────────────────────────────────
Adafruit_SSD1306 display(128, 64, &Wire, -1);
LoRaRadio         loraRadio;
AodvRouter        aodvRouter(loraRadio);   // loraRadio must be declared first

// ─── Beacon timing ────────────────────────────────────────────
static constexpr uint32_t BEACON_INTERVAL_MS = 30000;   // 30 seconds
static constexpr uint32_t BEACON_JITTER_MS   = 2000;    // +/- 2 s jitter

// ─── SD SPI bus ───────────────────────────────────────────────
static SPIClass gwSPI(HSPI);

// ─── Gateway objects ──────────────────────────────────────────
// IMPORTANT: declaration order is significant — C++ initialises file-scope
// objects in declaration order within a translation unit.
// HarvestLoop stores NodeRegistry& and CoapClient& so they must come first.
NodeRegistry registry;
CoapClient   coapClient;
HarvestLoop  harvestLoop(registry, coapClient);
// ─── Election manager ────────────────────────────────────────
// MUST be after registry, harvestLoop, aodvRouter — C++ file-scope init order
ElectionManager electionMgr(loraRadio, registry, harvestLoop, aodvRouter);

// ─── Serial command handler ──────────────────────────────────
SerialCmd serialCmd;

// ─── Leaf / Relay objects ─────────────────────────────────────
// CoapServer stores StorageReader& so storage must come first.
StorageReader storage;                        // Leaf: serves /images/
CoapServer    coapServer(storage);

// ─── Relay cached-image objects ─────────────────────────────
// After relay harvest, cached images go to /cached/ on SD.
// These objects let the relay serve those files to the gateway.
StorageReader cachedStorage("/cached");        // Relay: indexes /cached/
CoapServer    cachedCoapServer(cachedStorage); // Relay: serves /cached/ via CoAP

// ─── Leaf / Relay state ───────────────────────────────────────
static const char* AP_SSID_PREFIX = "ForestCam";
static const char* AP_PASS        = "forestcam123";
static char _apSSID[32];

static volatile bool _loraReady    = false;  // Written in setup(), read in tasks
static volatile bool _gwLoraReady  = false;  // Gateway LoRa status (set in initGateway)
static std::atomic<bool> _relayBusy{false};        // Accessed from Core 0 + Core 1
static std::atomic<uint8_t> _relayCmdId{0};

// ─── Deep sleep manager (leaf/relay only) ────────────────────
DeepSleepManager deepSleepMgr;

// ─── Relay cached-image serving state ────────────────────────
static std::atomic<bool>     _relayCachedServing{false};   // Accessed from Core 0 + Core 1
static std::atomic<uint32_t> _relayCachedStartMs{0};       // When we started serving
static constexpr uint32_t RELAY_CACHED_TIMEOUT_MS = 120000;  // Auto-cleanup after 2 min

// ─── Gateway timing ───────────────────────────────────────────
static constexpr uint32_t HARVEST_LISTEN_PERIOD_MS = 180000; // 3 min max between harvest cycles
static constexpr uint32_t HARVEST_REACTIVE_DELAY_MS = 15000; // 15s collect window after first new beacon
static constexpr uint32_t ROUTE_DISCOVERY_DELAY_MS = 15000;  // RREQ 15 s after boot

// Beacon-reactive harvest: set by beacon RX when a new/reappeared node is detected
static volatile uint32_t _lastNewBeaconMs = 0;


// =============================================================
// Forward declarations
// =============================================================
static const char* getResetReasonStr();
static bool wasCleanBoot();

// =============================================================
// Shared helpers
// =============================================================

void displayStatus(const char* line) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("IOT Forest Cam");
    display.println(line);
    display.display();
}

// =============================================================
// Relay cached-image cleanup
// =============================================================

/**
 * Clean up after the gateway has downloaded (or timed out downloading)
 * cached images from the relay.  Deletes /cached/ files, stops the
 * CoAP server serving them, and frees the relay for the next harvest.
 */
static void relayCachedCleanup() {
    Serial.println("[Relay] Cleaning up cached images...");

    // Guard first — prevent any re-entry from loopLeafRelay()
    _relayCachedServing = false;

    // Stop serving
    cachedCoapServer.stop();
    cachedStorage.endScanOnly();

    // Delete all files in /cached/
    File dir = SD.open("/cached");
    if (dir && dir.isDirectory()) {
        File entry = dir.openNextFile();
        while (entry) {
            char path[64];
            snprintf(path, sizeof(path), "/cached/%s", entry.name());
            entry.close();
            SD.remove(path);
            Serial.printf("[Relay]   Deleted %s\n", path);
            entry = dir.openNextFile();
        }
        dir.close();
    }

    // Return to AP mode after serving on gateway network
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(_apSSID, AP_PASS);
    vTaskDelay(pdMS_TO_TICKS(200));

    _relayBusy = false;
    Serial.printf("[Relay] Cached image cleanup complete — back to AP %s\n", _apSSID);
}

// =============================================================
// Gateway helpers
// =============================================================

// ── AODV route-discovered callback ───────────────────────────
static void onRouteDiscovered(const uint8_t destId[6], uint8_t hopCount) {
    RouteEntry route;
    if (aodvRouter.getRoute(destId, route)) {
        if (registryLock()) {
            registry.updateFromRoute(destId, route.nextHopId, route.hopCount);
            registryUnlock();
        }
    }
}

// =============================================================
// Relay helper — blocking store-and-forward harvest
// =============================================================

/**
 * Execute a relay harvest: switch to AP_STA, connect to the target leaf,
 * download images to /cached/ on SD, send HARVEST_ACK via LoRa.
 *
 * Runs on Core 1 (network task) — LoRa continues on Core 0 unblocked.
 *
 * Only valid when g_role == NODE_ROLE_RELAY.
 */
static void relayHarvest(const HarvestCmdPacket& cmd) {
    if (g_role != NODE_ROLE_RELAY) return;

    Serial.printf("\n╔══════════════════════════════════════╗\n");
    Serial.printf("║  RELAY HARVEST: %-20s║\n", cmd.ssid);
    Serial.printf("╚══════════════════════════════════════╝\n\n");

    _relayBusy  = true;
    _relayCmdId = cmd.cmdId;

    HarvestAckPacket ack;
    ack.cmdId = cmd.cmdId;
    WiFi.macAddress(ack.relayId);
    ack.status     = HARVEST_STATUS_OK;
    ack.imageCount = 0;
    ack.totalBytes = 0;

    // ── Switch to AP+STA — keeps relay AP running ────────────
    WiFi.mode(WIFI_AP_STA);
    vTaskDelay(pdMS_TO_TICKS(200));

    // ── Connect STA to the target leaf's AP ─────────────────
    Serial.printf("[Relay] Connecting to %s...\n", cmd.ssid);
    WiFi.begin(cmd.ssid, AP_PASS);

    uint32_t connectStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - connectStart < 15000) {
        vTaskDelay(pdMS_TO_TICKS(250));
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[Relay] WiFi connect to %s FAILED\n", cmd.ssid);
        ack.status = HARVEST_STATUS_WIFI_FAIL;
    } else {
        Serial.printf("[Relay] Connected to %s (IP: %s)\n",
                      cmd.ssid, WiFi.localIP().toString().c_str());

        // ── Download images from leaf via CoAP ──────────────
        CoapClient relayCoap;
        if (!relayCoap.begin()) {
            Serial.println("[Relay] CoAP client init failed");
            ack.status = HARVEST_STATUS_COAP_FAIL;
        } else {
            IPAddress leafIP(192, 168, 4, 1);
            uint16_t  leafPort = COAP_DEFAULT_PORT;

            // Get image count from /info endpoint
            uint8_t infoBuf[513];
            size_t  infoLen = sizeof(infoBuf) - 1;
            CoapClientError err = relayCoap.get(leafIP, leafPort,
                                                 "info", infoBuf, infoLen);

            uint8_t imageCount = 0;
            if (err == COAP_CLIENT_OK) {
                infoBuf[infoLen] = '\0';
                const char* countKey = strstr((char*)infoBuf, "\"count\":");
                if (countKey) imageCount = (uint8_t)atoi(countKey + 8);
            }

            if (imageCount > 0) {
                if (!SD.exists("/cached")) SD.mkdir("/cached");

                for (uint8_t i = 0; i < imageCount; i++) {
                    char outPath[64];
                    snprintf(outPath, sizeof(outPath), "/cached/relay_img_%03u.jpg", i);

                    if (SD.exists(outPath)) SD.remove(outPath);

                    TransferStats stats;
                    err = relayCoap.downloadImagePipelined(leafIP, leafPort, i, outPath, stats);

                    if (err == COAP_CLIENT_OK) {
                        ack.imageCount++;
                        ack.totalBytes += stats.totalBytes;
                        Serial.printf("[Relay] Cached img %u: %lu bytes -> %s\n",
                                      i, stats.totalBytes, outPath);
                    } else {
                        Serial.printf("[Relay] Failed to download image %u\n", i);
                    }
                }
            } else {
                Serial.println("[Relay] No images to cache");
            }

            relayCoap.stop();
        }

        WiFi.disconnect(true);
    }

    // ── Phase 2: Connect to gateway AP as STA to serve cached images ──
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(200));

    // ── Send HARVEST_ACK via LoRa (thread-safe) ──────────────
    Serial.printf("[Relay] ACK: status=%s, images=%u, bytes=%lu\n",
                  HarvestAckPacket::statusToString(ack.status),
                  ack.imageCount, ack.totalBytes);

    uint8_t ackBuf[64];
    uint8_t ackLen = ack.serialize(ackBuf, sizeof(ackBuf));
    if (ackLen > 0) {
        loraSendSafe(ackBuf, ackLen);
        loraStartReceiveSafe();
    }

    // ── Start serving cached images via gateway's WiFi network ──
    if (ack.status == HARVEST_STATUS_OK && ack.imageCount > 0 &&
        rtcGatewayKnown && rtcGatewaySSID[0] != '\0') {
        // Connect to gateway AP as STA
        WiFi.mode(WIFI_STA);
        WiFi.begin(rtcGatewaySSID, AP_PASS);

        uint32_t gwConnStart = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - gwConnStart < HARVEST_WIFI_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(250));
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[Relay] Connected to gateway AP %s (IP: %s)\n",
                          rtcGatewaySSID, WiFi.localIP().toString().c_str());

            if (cachedStorage.beginScanOnly()) {
                if (cachedCoapServer.begin()) {
                    _relayCachedServing = true;
                    _relayCachedStartMs = millis();
                    Serial.printf("[Relay] Serving %u cached image(s) via CoAP on gateway network\n",
                                  cachedStorage.imageCount());
                    Serial.println("[Relay] Harvest command complete — waiting for gateway download\n");
                    return;  // _relayBusy stays true
                } else {
                    Serial.println("[Relay] Failed to start cached CoAP server");
                    cachedStorage.endScanOnly();
                }
            } else {
                Serial.println("[Relay] Failed to scan /cached/ directory");
            }
        } else {
            WiFi.disconnect(true);
            Serial.printf("[Relay] Failed to connect to gateway AP %s\n", rtcGatewaySSID);
        }
    }

    // ── Fallback: return to AP mode ──────────────────────────
    WiFi.mode(WIFI_AP);
    WiFi.softAP(_apSSID, AP_PASS);
    vTaskDelay(pdMS_TO_TICKS(200));
    Serial.printf("[Relay] Back to AP mode: %s\n", _apSSID);

    _relayBusy = false;
    Serial.println("[Relay] Harvest command complete\n");
}


// =============================================================
// Role-specific init helpers
// =============================================================

static void initGateway() {
    Serial.println("\n===================================");
    Serial.println("  IOT Forest Cam — Gateway (LoRa)");
    Serial.println("  FreeRTOS + AODV Routing Enabled");
    Serial.println("===================================\n");

    // ── SD Card ──────────────────────────────────────────────
    displayStatus("Mounting SD...");
    pinMode(VSENSOR_SD_CS, OUTPUT);
    digitalWrite(VSENSOR_SD_CS, HIGH);
    delay(100);
    gwSPI.begin(VSENSOR_SD_CLK, VSENSOR_SD_MISO, VSENSOR_SD_MOSI, VSENSOR_SD_CS);

    if (!SD.begin(VSENSOR_SD_CS, gwSPI, 4000000)) {
        Serial.println("[WARN] SD card not available — checksum-only mode");
    } else {
        Serial.println("[OK] SD card mounted for receiving images");
        SD.mkdir("/received");
    }

    // ── LoRa ─────────────────────────────────────────────────
    displayStatus("LoRa Init...");
    if (!loraRadio.begin()) {
        Serial.println("[WARN] LoRa SX1280 init failed — gateway running without LoRa");
        Serial.println("       (No beacon RX / AODV — WiFi-only harvest still possible)");
        displayStatus("LoRa FAILED (warn)");
        delay(1500);
    } else {
        _gwLoraReady = true;
        if (!loraRadio.startReceive()) {
            Serial.println("[WARN] LoRa startReceive failed — will retry in loop");
        }
    }

    // ── AODV Router ──────────────────────────────────────────
    uint8_t myMac[6];
    WiFi.macAddress(myMac);
    if (_gwLoraReady) {
        aodvRouter.begin(myMac);
        aodvRouter.setRouteDiscoveredCallback(onRouteDiscovered);
        harvestLoop.setAodv(&aodvRouter, &loraRadio);
        harvestLoop.setNodeBlockedCallback([](const uint8_t nodeId[6]) -> bool {
            return serialCmd.isNodeBlocked(nodeId);
        });
        electionMgr.begin(myMac, NODE_ROLE_GATEWAY);
    }

    // ── Broadcast GW_RECLAIM to reclaim from any promoted relay ──
    if (_gwLoraReady) {
        Serial.println("[GW] Broadcasting GW_RECLAIM...");
        ElectionPacket reclaim;
        reclaim.type = PKT_TYPE_GW_RECLAIM;
        memcpy(reclaim.senderId, myMac, 6);
        reclaim.electionId = 0;

        uint8_t buf[ELECTION_PACKET_SIZE];
        uint8_t len = reclaim.serialize(buf, sizeof(buf));
        for (uint8_t i = 0; i < GW_RECLAIM_TX_REPEAT; i++) {
            if (len > 0) loraRadio.send(buf, len);
            delay(GW_RECLAIM_TX_GAP_MS);
        }
        loraRadio.startReceive();
    }

    // ── WiFi AP (persistent — leaves connect to us as STA) ───
    uint8_t gwMac[6];
    WiFi.macAddress(gwMac);
    static char _gwAPSSID[32];
    snprintf(_gwAPSSID, sizeof(_gwAPSSID), "ForestCam-GW-%02X%02X", gwMac[4], gwMac[5]);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(_gwAPSSID, AP_PASS);
    delay(100);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    IPAddress gwIP = WiFi.softAPIP();
    Serial.printf("[OK] Gateway WiFi AP: %s (pass: %s)\n", _gwAPSSID, AP_PASS);
    Serial.printf("[OK] Gateway IP: %s\n", gwIP.toString().c_str());

    // ── Gateway CoAP server (receives /announce from leaves) ──
    // Gateway also runs a CoAP server to handle leaf announcements.
    // The gateway's own images (if any) are served from here too.
    if (coapServer.begin()) {
        Serial.printf("[OK] Gateway CoAP server on port %u (for /announce)\n", COAP_DEFAULT_PORT);
    } else {
        Serial.println("[WARN] Gateway CoAP server failed — announce-based harvest unavailable");
    }

    // ── OLED ready screen ────────────────────────────────────
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Gateway (RTOS+AODV)");
    display.printf("AP: %s\n", _gwAPSSID);
    display.printf("IP: %s\n", gwIP.toString().c_str());
    display.println();
    display.println("Nodes: 0  Rtes: 0");
    display.println("State: IDLE");
    display.display();

    Serial.println("[OK] Gateway listening for LoRa beacons + AODV...\n");
}

static void initLeafRelay() {
    if (g_role == NODE_ROLE_RELAY) {
        displayStatus("Relay Mode");
        Serial.println("\n======================================");
        Serial.println("  IOT Forest Cam — Relay (LoRa+CoAP)");
        Serial.println("  FreeRTOS + AODV Routing Enabled");
        Serial.println("======================================\n");
    } else {
        displayStatus("Leaf Mode");
        Serial.println("\n====================================");
        Serial.println("  IOT Forest Cam — Leaf (LoRa+CoAP)");
        Serial.println("  FreeRTOS + AODV Routing Enabled");
        Serial.println("====================================\n");
    }

    // ── SD Card (images stored in /images/) ──────────────────
    bool _sdReady = false;

    if (g_role == NODE_ROLE_RELAY) {
        displayStatus("Mounting SD...");
        pinMode(VSENSOR_SD_CS, OUTPUT);
        digitalWrite(VSENSOR_SD_CS, HIGH);
        delay(100);
        gwSPI.begin(VSENSOR_SD_CLK, VSENSOR_SD_MISO, VSENSOR_SD_MOSI, VSENSOR_SD_CS);

        if (!SD.begin(VSENSOR_SD_CS, gwSPI, 4000000)) {
            Serial.println("[WARN] SD card not available — relay harvest will be disabled");
        } else {
            _sdReady = true;
            SD.mkdir("/cached");
            Serial.println("[OK] SD card mounted for relay caching");
        }
    } else {
        displayStatus("Mounting SD...");

        if (!wasCleanBoot()) {
            Serial.println("[SD] Non-clean reset — power-cycling SD CS pin");
            pinMode(VSENSOR_SD_CS, OUTPUT);
            digitalWrite(VSENSOR_SD_CS, LOW);
            delay(100);
            digitalWrite(VSENSOR_SD_CS, HIGH);
            delay(500);
        }

        if (!storage.begin()) {
            displayStatus("SD FAILED - no CoAP");
            Serial.println("[WARN] SD card mount failed. CoAP server will not start.");
            Serial.println("       Check: SD card inserted? FAT32? /images/ folder with .jpg files?");
        } else {
            _sdReady = true;
            Serial.printf("[OK] SD card: %d image(s) found\n", storage.imageCount());

            if (storage.imageCount() == 0) {
                displayStatus("No images on SD!");
                Serial.println("[WARN] No JPEG files in /images/ directory. CoAP server will not start.");
                _sdReady = false;
            }

            for (uint8_t i = 0; i < storage.imageCount(); i++) {
                ImageInfo info;
                if (storage.getImageInfo(i, info)) {
                    Serial.printf("  [%u] %s — %lu bytes (%lu blocks)\n",
                                  i, info.filename, info.fileSize, info.totalBlocks);
                }
            }
        }
    }

    // ── WiFi Setup ──────────────────────────────────────────
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(_apSSID, sizeof(_apSSID), "%s-%02X%02X",
             AP_SSID_PREFIX, mac[4], mac[5]);

    // On timer wake with known gateway: connect as STA for announce-based harvest
    bool _leafStaMode = false;
    if ((DeepSleepManager::wasWokenByTimer() || DeepSleepManager::wasWokenByLoRa())
        && rtcGatewayKnown && rtcGatewaySSID[0] != '\0') {

        displayStatus("STA connect...");
        Serial.printf("[Leaf] Connecting to gateway AP: %s\n", rtcGatewaySSID);
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_8_5dBm);
        WiFi.begin(rtcGatewaySSID, AP_PASS);

        uint32_t connectStart = millis();
        while (WiFi.status() != WL_CONNECTED &&
               millis() - connectStart < HARVEST_WIFI_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(250));
            Serial.print(".");
        }

        if (WiFi.status() == WL_CONNECTED) {
            _leafStaMode = true;
            Serial.printf("\n[Leaf] Connected to gateway! STA IP: %s\n",
                          WiFi.localIP().toString().c_str());
            deepSleepMgr.onActivity();
        } else {
            Serial.println("\n[Leaf] Gateway connect failed — falling back to AP mode");
            WiFi.disconnect(true);
        }
    }

    // Normal AP mode (cold boot, relay, or STA connect failed)
    if (!_leafStaMode) {
        displayStatus("Starting WiFi AP...");
        WiFi.mode(WIFI_AP);
        WiFi.softAP(_apSSID, AP_PASS);
        delay(100);
        WiFi.setTxPower(WIFI_POWER_8_5dBm);

        IPAddress ip = WiFi.softAPIP();
        Serial.printf("[OK] WiFi AP: %s (pass: %s)\n", _apSSID, AP_PASS);
        Serial.printf("[OK] IP: %s\n", ip.toString().c_str());
    }

    // ── CoAP Server (leaf only — relay uses cachedCoapServer on demand) ──
    // Binds to 0.0.0.0 — works on both AP and STA interfaces
    if (_sdReady && g_role != NODE_ROLE_RELAY) {
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
        if (_leafStaMode) {
            Serial.println("  POST /announce        — Leaf-initiated harvest");
        }
        Serial.println();

        // Send announce to gateway if in STA mode
        if (_leafStaMode) {
            uint8_t announcePayload[7];
            memcpy(announcePayload, mac, 6);
            announcePayload[6] = storage.imageCount();

            CoapClient announceClient;
            announceClient.begin();
            CoapClientError err = announceClient.post(
                IPAddress(192, 168, 4, 1), COAP_DEFAULT_PORT,
                "announce", announcePayload, 7);
            announceClient.stop();

            if (err == COAP_CLIENT_OK) {
                Serial.println("[Leaf] Announce sent — waiting for gateway to download");
                deepSleepMgr.onActivity();  // Fresh 120s from now
            } else {
                Serial.printf("[Leaf] Announce failed (err=%d)\n", err);
            }
        }
    } else {
        Serial.println("[WARN] CoAP server skipped — no SD card available");
    }

    // ── LoRa ─────────────────────────────────────────────────
    displayStatus("LoRa Init...");
    if (!loraRadio.begin()) {
        Serial.println("[WARN] LoRa init failed — running without beacons");
    } else {
        _loraReady = true;
        if (!loraRadio.startReceive()) {
            Serial.println("[WARN] LoRa startReceive failed — will retry in loop");
        }
        Serial.println("[OK] LoRa beacon TX + RX enabled (AODV routing)");
    }

    // ── AODV Router ──────────────────────────────────────────
    if (_loraReady) {
        aodvRouter.begin(mac);
        harvestLoop.setAodv(&aodvRouter, &loraRadio);
        harvestLoop.setNodeBlockedCallback([](const uint8_t nodeId[6]) -> bool {
            return serialCmd.isNodeBlocked(nodeId);
        });
        // On timer wake, pass RTC gateway knowledge to skip unnecessary election
        bool gwFromRtc = (DeepSleepManager::wasWokenByTimer() || DeepSleepManager::wasWokenByLoRa())
                         && rtcGatewayKnown;
        electionMgr.begin(mac, g_role, gwFromRtc);
    }

    // ── OLED ready screen ────────────────────────────────────
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(g_role == NODE_ROLE_RELAY ? "Relay (AODV)" : "Leaf (AODV)");
    display.printf("AP: %s\n", _apSSID);
    IPAddress displayIP = _leafStaMode ? WiFi.localIP() : WiFi.softAPIP();
    display.printf("IP: %s\n", displayIP.toString().c_str());
    display.printf("CoAP: %s\n", _sdReady ? ":5683" : "OFF");
    display.printf("Imgs: %d  LoRa:%s\n",
                   _sdReady ? storage.imageCount() : 0, _loraReady ? "OK" : "NO");
    display.println("────────────────────");
    display.println("Reqs: 0  Blks: 0");
    display.display();
}


// =============================================================
// FreeRTOS Task: LoRa (Core 0) — Gateway mode
// =============================================================

/**
 * Gateway LoRa task: handles all LoRa RX/TX, beacon broadcasting,
 * AODV routing ticks, election ticks, and processes the LoRa TX queue
 * for requests from Core 1.
 *
 * Also triggers harvest cycles by enqueuing to xHarvestCmdQueue.
 */
static void taskLoRaGateway(void* param) {
    // ── Timing state ─────────────────────────────────────────
    uint32_t lastDiag           = 0;
    uint32_t lastGwBeacon       = 0;
    uint32_t gwBeaconInterval   = BEACON_INTERVAL_MS;
    uint32_t lastExpireMs       = 0;
    bool     routeDiscoveryDone = false;
    uint32_t bootMs             = millis();
    uint32_t listenStartMs      = millis();
    bool     firstHarvestDone   = false;
    HarvestState prevState      = HARVEST_IDLE;

    for (;;) {
        // ── LoRa TX queue: drain requests from Core 1 ────────
        LoRaTxRequest txReq;
        while (xQueueReceive(xLoRaTxQueue, &txReq, 0) == pdTRUE) {
            if (xSemaphoreTake(xLoRaMutex, MUTEX_TIMEOUT) == pdTRUE) {
                loraRadio.send(txReq.data, txReq.length);
                loraRadio.startReceive();
                xSemaphoreGive(xLoRaMutex);
            }
        }

        if (!_gwLoraReady && !_loraReady) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // ── Periodic LoRa RX recovery (every 5 s) ──────────────
        if (millis() - lastDiag >= 5000) {
            lastDiag = millis();
            if (xSemaphoreTake(xLoRaMutex, MUTEX_TIMEOUT) == pdTRUE) {
                uint8_t st = loraRadio.getStatus();
                uint8_t mode = (st >> 4) & 0x07;

                if (mode != 5 && mode != 6) {
                    if (!loraRadio.startReceive()) {
                        uint16_t irq = loraRadio.getIrqFlags();
                        Serial.printf("[LoRa] RX recovery FAILED status=0x%02X mode=%u IRQ=0x%04X\n",
                                      st, mode, irq);
                    }
                }
                xSemaphoreGive(xLoRaMutex);
            }
        }

        // ── Gateway beacon TX (for election liveness detection) ──
        if (millis() - lastGwBeacon >= gwBeaconInterval) {
            BeaconPacket gwBeacon;
            gwBeacon.packetType = BEACON_TYPE_BEACON;
            gwBeacon.ttl = 2;
            WiFi.macAddress(gwBeacon.nodeId);
            gwBeacon.nodeRole   = NODE_ROLE_GATEWAY;
            gwBeacon.imageCount = 0;
            gwBeacon.batteryPct = 0xFF;  // USB powered
            gwBeacon.uptimeMin  = (uint16_t)(millis() / 60000);
            gwBeacon.deriveSsid();  // Populate ssid from MAC for logging

            uint8_t buf[BEACON_MAX_SIZE];
            uint8_t len = gwBeacon.serialize(buf, sizeof(buf));
            if (len > 0) {
                loraSendSafe(buf, len);
                loraStartReceiveSafe();
                Serial.println("[GW] Beacon TX (liveness)");
            }

            lastGwBeacon = millis();
            gwBeaconInterval = BEACON_INTERVAL_MS
                             + random(-(int32_t)BEACON_JITTER_MS, (int32_t)BEACON_JITTER_MS);
        }

        // ── LoRa RX: dispatch packets ───────────────────────
        LoRaRxResult rx;
        if (loraCheckReceiveSafe(rx)) {
            uint8_t pktType = getLoRaPacketType(rx.data, rx.length);

            switch (pktType) {
                case BEACON_TYPE_BEACON:
                case BEACON_TYPE_BEACON_RELAY: {
                    BeaconPacket beacon;
                    if (beacon.parse(rx.data, rx.length)) {
                        bool isNew = false;
                        if (registryLock()) {
                            isNew = registry.update(beacon, rx.rssi);
                            registryUnlock();
                        }
                        if (isNew && _lastNewBeaconMs == 0) {
                            _lastNewBeaconMs = millis();
                        }
                        electionMgr.onBeacon(beacon);

                        char idStr[24];
                        beacon.nodeIdToString(idStr, sizeof(idStr));
                        Serial.printf("[LoRa] Beacon from %s (%s) — %s, %u images, RSSI=%.0f dBm\n",
                                      beacon.ssid, idStr,
                                      BeaconPacket::roleToString(beacon.nodeRole),
                                      beacon.imageCount, rx.rssi);
                    }
                    break;
                }
                case PKT_TYPE_RREP: {
                    RrepPacket rrep;
                    if (rrep.parse(rx.data, rx.length)) {
                        aodvRouter.handleRREP(rrep);
                    }
                    break;
                }
                case PKT_TYPE_RERR: {
                    RerrPacket rerr;
                    if (rerr.parse(rx.data, rx.length)) {
                        aodvRouter.handleRERR(rerr);
                    }
                    break;
                }
                case PKT_TYPE_RREQ: {
                    RreqPacket rreq;
                    if (rreq.parse(rx.data, rx.length)) {
                        aodvRouter.handleRREQ(rreq, rx.rssi);
                    }
                    break;
                }
                case PKT_TYPE_HARVEST_ACK: {
                    HarvestAckPacket ack;
                    if (ack.parse(rx.data, rx.length)) {
                        Serial.printf("[LoRa] HARVEST_ACK from relay — status=%s, images=%u, bytes=%lu\n",
                                      HarvestAckPacket::statusToString(ack.status),
                                      ack.imageCount, ack.totalBytes);
                        harvestLoop.onHarvestAck(ack);
                    }
                    break;
                }
                case PKT_TYPE_ELECTION:
                case PKT_TYPE_SUPPRESS:
                case PKT_TYPE_COORDINATOR:
                case PKT_TYPE_GW_RECLAIM:
                    electionMgr.onElectionPacket(rx.data, rx.length);
                    break;
                default:
                    log_d("Unknown LoRa packet type 0x%02X (%u bytes)", pktType, rx.length);
                    break;
            }

            loraStartReceiveSafe();
        }

        // ── AODV periodic tick ──────────────────────────────
        aodvRouter.tick();

        // ── Expire stale nodes + trigger RERR ────────────────
        if (millis() - lastExpireMs >= 10000) {
            lastExpireMs = millis();

            if (registryLock()) {
                for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
                    NodeEntry entry;
                    if (registry.getNode(i, entry)) {
                        if ((millis() - entry.lastSeenMs) > REGISTRY_EXPIRY_MS) {
                            aodvRouter.notifyLinkBreak(entry.nodeId);
                        }
                    }
                }
                registry.expireStale();
                registryUnlock();
            }
        }

        // ── Auto-broadcast RREQ for topology discovery ───────
        uint8_t activeNodes = 0;
        if (registryLock()) {
            activeNodes = registry.activeCount();
            registryUnlock();
        }

        if (!routeDiscoveryDone && millis() - bootMs >= ROUTE_DISCOVERY_DELAY_MS &&
            activeNodes > 0) {
            Serial.println("\n[AODV] Broadcasting RREQ for all nodes (topology discovery)...");
            aodvRouter.discoverAll();
            routeDiscoveryDone = true;
        }

        // ── Start harvest (reactive or max-wait) ─────────────
        HarvestState curState = harvestLoop.state();

        // Reactive: start 15s after first new beacon, OR after 180s max
        bool reactiveReady = _lastNewBeaconMs > 0 &&
                             (millis() - _lastNewBeaconMs >= HARVEST_REACTIVE_DELAY_MS);
        bool maxWaitReady  = millis() - listenStartMs >= HARVEST_LISTEN_PERIOD_MS;

        if (curState == HARVEST_IDLE &&
            activeNodes > 0 &&
            (reactiveReady || maxWaitReady))
        {
            _lastNewBeaconMs = 0;  // Reset for next cycle
            if (!aodvRouter.isDiscoveryPending()) {
                Serial.println("\n[AODV] Pre-harvest route discovery...");
                aodvRouter.discoverAll();
            }

            Serial.printf("\n[Harvest] Starting cycle — %u node(s), %u routes\n",
                          activeNodes, aodvRouter.routeCount());
            if (registryLock()) {
                registry.dump();
                registryUnlock();
            }
            aodvRouter.dumpRoutes();

            // Signal the harvest task to begin
            uint8_t cmd = 1;
            xQueueSend(xHarvestCmdQueue, &cmd, 0);
            firstHarvestDone = true;
        }

        // ── Reset listen timer after harvest completes ───────
        if (curState == HARVEST_IDLE && prevState == HARVEST_DONE) {
            listenStartMs = millis();
            routeDiscoveryDone = false;
            loraStartReceiveSafe();
            Serial.println("[Gateway] Resuming beacon listening...\n");
        }
        prevState = curState;

        // ── Election state machine ──────────────────────────
        electionMgr.tick();

        // ── Yield to other tasks ────────────────────────────
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// =============================================================
// FreeRTOS Task: LoRa (Core 0) — Leaf/Relay mode
// =============================================================

// Forward declaration — taskHarvestGateway is created lazily on promotion
static void taskHarvestGateway(void* param);

/**
 * Leaf/Relay LoRa task: beacon TX, LoRa RX dispatch,
 * AODV routing ticks, election ticks.
 *
 * Relay HARVEST_CMD packets are forwarded to Core 1 via xRelayHarvestQueue.
 */
static void taskLoRaLeafRelay(void* param) {
    uint32_t lastDiag      = 0;
    uint32_t lastBeaconMs  = 0;
    uint32_t nextInterval  = 0;  // Fire first beacon immediately on boot/wake

    // Stored HARVEST_CMD for relay queue (persists across loop iterations)
    static HarvestCmdPacket pendingCmd;

    for (;;) {
        // ── LoRa TX queue: drain requests from Core 1 ────────
        LoRaTxRequest txReq;
        while (xQueueReceive(xLoRaTxQueue, &txReq, 0) == pdTRUE) {
            if (xSemaphoreTake(xLoRaMutex, MUTEX_TIMEOUT) == pdTRUE) {
                loraRadio.send(txReq.data, txReq.length);
                loraRadio.startReceive();
                xSemaphoreGive(xLoRaMutex);
            }
        }

        if (!_loraReady) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // ── Periodic LoRa RX recovery (every 5 s) ──────────────
        if (millis() - lastDiag >= 5000) {
            lastDiag = millis();
            if (xSemaphoreTake(xLoRaMutex, MUTEX_TIMEOUT) == pdTRUE) {
                uint8_t st = loraRadio.getStatus();
                uint8_t mode = (st >> 4) & 0x07;

                if (mode != 5 && mode != 6) {
                    if (!loraRadio.startReceive()) {
                        uint16_t irq = loraRadio.getIrqFlags();
                        Serial.printf("[LoRa] RX recovery FAILED status=0x%02X mode=%u IRQ=0x%04X\n",
                                      st, mode, irq);
                    }
                }
                xSemaphoreGive(xLoRaMutex);
            }
        }

        // ── LoRa Beacon Broadcast (every ~30 s with jitter) ──
        if (millis() - lastBeaconMs >= nextInterval) {
            lastBeaconMs = millis();
            nextInterval = BEACON_INTERVAL_MS + random(-BEACON_JITTER_MS, BEACON_JITTER_MS);

            BeaconPacket beacon;
            beacon.packetType = BEACON_TYPE_BEACON;
            beacon.ttl = 2;
            WiFi.macAddress(beacon.nodeId);
            if (electionMgr.isPromoted()) {
                beacon.nodeRole = NODE_ROLE_GATEWAY;
            } else if (g_role == NODE_ROLE_RELAY || aodvRouter.isRelaying()) {
                beacon.nodeRole = NODE_ROLE_RELAY;
            } else {
                beacon.nodeRole = NODE_ROLE_LEAF;
            }
            beacon.imageCount = (g_role == NODE_ROLE_RELAY && _relayCachedServing)
                                    ? cachedStorage.imageCount()
                                    : storage.imageCount();
            beacon.deriveSsid();  // Populate ssid from MAC for logging
            beacon.batteryPct = 0xFF;
            beacon.uptimeMin  = (uint16_t)(millis() / 60000);

            uint8_t buf[BEACON_MAX_SIZE];
            uint8_t len = beacon.serialize(buf, sizeof(buf));
            if (len > 0) {
                loraSendSafe(buf, len);
                Serial.printf("[LoRa] Beacon TX (%u bytes) — %s, %u images\n",
                              len, _apSSID, beacon.imageCount);
            }

            loraStartReceiveSafe();
        }

        // ── LoRa RX: Dispatch all packet types ───────────────
        LoRaRxResult rx;
        if (loraCheckReceiveSafe(rx)) {
            uint8_t pktType = getLoRaPacketType(rx.data, rx.length);

            switch (pktType) {
                case BEACON_TYPE_BEACON:
                case BEACON_TYPE_BEACON_RELAY: {
                    BeaconPacket received;
                    if (received.parse(rx.data, rx.length)) {
                        // All nodes must see beacons for gateway detection
                        electionMgr.onBeacon(received);

                        // Only relay/promoted nodes do registry update and beacon forwarding
                        if (g_role == NODE_ROLE_RELAY || electionMgr.isPromoted()) {
                            uint8_t myMac[6];
                            WiFi.macAddress(myMac);

                            if (electionMgr.isPromoted()) {
                                if (registryLock()) {
                                    bool isNewPromoted = registry.update(received, rx.rssi);
                                    registryUnlock();
                                    if (isNewPromoted && _lastNewBeaconMs == 0) {
                                        _lastNewBeaconMs = millis();
                                    }
                                }
                            }

                            if (memcmp(received.nodeId, myMac, 6) != 0 && received.ttl > 1) {
                                received.ttl--;
                                received.packetType = BEACON_TYPE_BEACON_RELAY;

                                uint8_t relayBuf[BEACON_MAX_SIZE];
                                uint8_t relayLen = received.serialize(relayBuf, sizeof(relayBuf));

                                if (relayLen > 0) {
                                    vTaskDelay(pdMS_TO_TICKS(random(100, 500)));
                                    loraSendSafe(relayBuf, relayLen);

                                    char idStr[24];
                                    received.nodeIdToString(idStr, sizeof(idStr));
                                    Serial.printf("[LoRa] RELAYED beacon from %s (%s), TTL=%u\n",
                                                  received.ssid, idStr, received.ttl);
                                }
                            }
                        }
                    }
                    break;
                }

                case PKT_TYPE_RREQ: {
                    RreqPacket rreq;
                    if (rreq.parse(rx.data, rx.length)) {
                        aodvRouter.handleRREQ(rreq, rx.rssi);
                    }
                    break;
                }

                case PKT_TYPE_RREP: {
                    RrepPacket rrep;
                    if (rrep.parse(rx.data, rx.length)) {
                        aodvRouter.handleRREP(rrep);
                    }
                    break;
                }

                case PKT_TYPE_RERR: {
                    RerrPacket rerr;
                    if (rerr.parse(rx.data, rx.length)) {
                        aodvRouter.handleRERR(rerr);
                    }
                    break;
                }

                case PKT_TYPE_HARVEST_CMD: {
                    // Relay only: forward to Core 1 via queue
                    if (g_role == NODE_ROLE_RELAY) {
                        if (pendingCmd.parse(rx.data, rx.length)) {
                            uint8_t myMac[6];
                            WiFi.macAddress(myMac);
                            if (memcmp(pendingCmd.relayId, myMac, 6) == 0 && !_relayBusy) {
                                deepSleepMgr.onActivity();  // Reset sleep timer
                                xQueueSend(xRelayHarvestQueue, &pendingCmd, 0);
                            }
                        }
                    }
                    // Any HARVEST_CMD means gateway is actively harvesting — stay awake
                    deepSleepMgr.onActivity();
                    break;
                }

                // WAKE_PING (0x40) and WAKE_BEACON_REQ (0x41) removed:
                // SX1280 LoRa wake from deep sleep is unreliable (commit 8665061).
                // Timer-based wake (180s) is the only wake mechanism.

                case PKT_TYPE_ELECTION:
                case PKT_TYPE_SUPPRESS:
                case PKT_TYPE_COORDINATOR:
                case PKT_TYPE_GW_RECLAIM:
                    electionMgr.onElectionPacket(rx.data, rx.length);
                    break;

                default:
                    break;
            }

            loraStartReceiveSafe();
        }

        // ── AODV periodic tick ──────────────────────────────
        aodvRouter.tick();

        // ── Election state machine ──────────────────────────
        electionMgr.tick();

        // ── Harvest trigger (promoted gateway only) ─────────
        static bool wasPromoted = false;
        static uint32_t promoteListenStartMs = 0;
        static HarvestState prevPromoteState = HARVEST_IDLE;
        static bool promoteHarvestTriggered = false;
        bool nowPromoted = electionMgr.isPromoted();

        if (nowPromoted && !wasPromoted) {
            // Fresh promotion — reset all harvest timing
            promoteListenStartMs = millis();
            prevPromoteState = HARVEST_IDLE;
            promoteHarvestTriggered = false;
            SD.mkdir("/received");

            // Create harvest task on-demand (first promotion only)
            if (hTaskHarvest == nullptr) {
                xTaskCreatePinnedToCore(taskHarvestGateway, "Harvest_GW",
                    STACK_HARVEST, nullptr, PRIORITY_HARVEST,
                    &hTaskHarvest, CORE_NETWORK);
                Serial.println("[Promoted GW] Harvest task created");
            }
            Serial.println("[Promoted GW] Harvest capability enabled, listening...");
        }
        wasPromoted = nowPromoted;

        if (nowPromoted) {
            uint8_t activeNodes = 0;
            if (registryLock()) {
                activeNodes = registry.activeCount();
                registryUnlock();
            }

            HarvestState curState = harvestLoop.state();

            // Reactive: start 15s after first new beacon, OR after 180s max
            bool promReactive = _lastNewBeaconMs > 0 &&
                                (millis() - _lastNewBeaconMs >= HARVEST_REACTIVE_DELAY_MS);
            bool promMaxWait  = millis() - promoteListenStartMs >= HARVEST_LISTEN_PERIOD_MS;

            if (curState == HARVEST_IDLE &&
                !promoteHarvestTriggered &&
                activeNodes > 0 &&
                (promReactive || promMaxWait))
            {
                _lastNewBeaconMs = 0;  // Reset for next cycle
                if (!aodvRouter.isDiscoveryPending()) {
                    aodvRouter.discoverAll();
                }
                Serial.printf("\n[Harvest] Starting cycle — %u node(s)\n", activeNodes);
                uint8_t cmd = 1;
                xQueueSend(xHarvestCmdQueue, &cmd, 0);
                promoteHarvestTriggered = true;
            }

            // Reset listen timer after harvest completes
            if (curState == HARVEST_IDLE && prevPromoteState == HARVEST_DONE) {
                promoteListenStartMs = millis();
                promoteHarvestTriggered = false;
            }
            prevPromoteState = curState;
        }

        // ── Deep sleep check (leaf/relay only) ──────────────
        // Never sleep while promoted, during election, or when gateway is missing
        // (node must stay awake to run the election and potentially promote)
        if (g_role != NODE_ROLE_GATEWAY && !electionMgr.isPromoted()
            && !electionMgr.isElectionActive() && !electionMgr.isGatewayMissing()) {
            deepSleepMgr.setCoapBusy(_relayCachedServing);
            deepSleepMgr.setHarvestInProgress(_relayBusy);

            // Reset sleep timer if CoAP is actively serving (blocks or requests)
            static uint32_t lastBlockCount = 0;
            static uint32_t lastRequestCount = 0;
            uint32_t curBlockCount = coapServer.blocksSent();
            uint32_t curRequestCount = coapServer.requestCount();
            if (curBlockCount != lastBlockCount || curRequestCount != lastRequestCount) {
                lastBlockCount = curBlockCount;
                lastRequestCount = curRequestCount;
                deepSleepMgr.onActivity();
            }

            if (deepSleepMgr.shouldSleep(millis())) {
                Serial.println("\n[DeepSleep] Active timeout expired — entering deep sleep");
                Serial.printf("[DeepSleep] Boot count: %lu\n", deepSleepMgr.bootCount());

                // Save state for fast-path wake
                deepSleepMgr.saveState(g_role, _apSSID);

                // Stop CoAP server and WiFi before sleeping
                coapServer.stop();
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);

                // Turn off OLED to save power during sleep
                display.ssd1306_command(SSD1306_DISPLAYOFF);

                // Prepare radio for DIO1 wake
                if (_loraReady) {
                    deepSleepMgr.prepareRadioForSleep(loraRadio);
                }

                deepSleepMgr.enterSleep();
                // ── Does not return ──
            }
        }

        // ── Yield to other tasks ────────────────────────────
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


// =============================================================
// FreeRTOS Task: Harvest (Core 1) — Gateway mode
// =============================================================

/**
 * Gateway harvest task: waits for harvest trigger from Core 0,
 * then runs the harvest state machine until completion.
 * WiFi connect + CoAP download happen here without blocking LoRa.
 */
static void taskHarvestGateway(void* param) {
    for (;;) {
        // ── Check announce queue (leaf-initiated harvest) ─────
        AnnounceMessage announce;
        bool hasAnnounce = false;
        while (xQueueReceive(xAnnounceQueue, &announce, 0) == pdTRUE) {
            if (registryLock()) {
                registry.updateFromAnnounce(announce.nodeId, announce.ip, announce.imageCount);
                registryUnlock();
            }
            hasAnnounce = true;
        }

        if (hasAnnounce && harvestLoop.state() == HARVEST_IDLE) {
            Serial.println("[Harvest Task] Announce-triggered harvest on Core 1");
            harvestLoop.startCycle();
            while (harvestLoop.state() != HARVEST_IDLE) {
                harvestLoop.tick();
                // Drain any new announces that arrive during harvest
                AnnounceMessage late;
                while (xQueueReceive(xAnnounceQueue, &late, 0) == pdTRUE) {
                    if (registryLock()) {
                        registry.updateFromAnnounce(late.nodeId, late.ip, late.imageCount);
                        registryUnlock();
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            Serial.println("[Harvest Task] Harvest cycle complete");
            continue;
        }

        // ── Legacy: block on harvest command queue ────────────
        uint8_t cmd;
        if (xQueueReceive(xHarvestCmdQueue, &cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
            Serial.println("[Harvest Task] Starting harvest cycle on Core 1");
            harvestLoop.startCycle();
            while (harvestLoop.state() != HARVEST_IDLE) {
                harvestLoop.tick();
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            Serial.println("[Harvest Task] Harvest cycle complete");
        }
    }
}


// =============================================================
// FreeRTOS Task: CoAP Server (Core 1) — Leaf/Relay mode
// =============================================================

/**
 * Leaf/Relay CoAP server task: serves images via CoAP Block2 transfer.
 * Also handles relay cached-image serving and cleanup.
 */
static void taskCoapServerLoop(void* param) {
    for (;;) {
        // ── CoAP Server (leaf serves /images/) ───────────────
        coapServer.loop();

        // ── Relay: serve cached images to gateway ────────────
        if (_relayCachedServing) {
            cachedCoapServer.loop();

            if (millis() - _relayCachedStartMs >= RELAY_CACHED_TIMEOUT_MS) {
                Serial.println("[Relay] Cached serving timeout — cleaning up");
                relayCachedCleanup();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}


// =============================================================
// FreeRTOS Task: Relay Harvest (Core 1) — Relay mode
// =============================================================

/**
 * Relay harvest task: waits for HARVEST_CMD from Core 0,
 * then executes the blocking relay harvest on Core 1.
 */
static void taskRelayHarvest(void* param) {
    HarvestCmdPacket cmd;
    for (;;) {
        if (xQueueReceive(xRelayHarvestQueue, &cmd, portMAX_DELAY) == pdTRUE) {
            Serial.println("[Relay Task] Executing relay harvest on Core 1");
            relayHarvest(cmd);
        }
    }
}


// =============================================================
// Arduino entry points
// =============================================================

// ── Reset reason helper ──────────────────────────────────────
static const char* getResetReasonStr() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "Power-on";
        case ESP_RST_SW:       return "Software";
        case ESP_RST_PANIC:    return "Panic/crash";
        case ESP_RST_INT_WDT:  return "Interrupt WDT";
        case ESP_RST_TASK_WDT: return "Task WDT";
        case ESP_RST_WDT:      return "Other WDT";
        case ESP_RST_DEEPSLEEP:return "Deep sleep";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "Unknown";
    }
}

static bool wasCleanBoot() {
    esp_reset_reason_t r = esp_reset_reason();
    return (r == ESP_RST_POWERON || r == ESP_RST_SW || r == ESP_RST_DEEPSLEEP);
}

void setup() {
    // ── Disable brownout detector ─────────────────────────────
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(1000);

    // ── Log reset reason ─────────────────────────────────────
    Serial.printf("\n[Boot] Reset reason: %s (boot #%lu)\n", getResetReasonStr(), rtcBootCount);

    // ── OLED ─────────────────────────────────────────────────
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    // ── Fast-path wake: skip role menu if woken by timer or LoRa ─
    if (DeepSleepManager::wasWokenByTimer() || DeepSleepManager::wasWokenByLoRa()) {
        // Release GPIO holds set before deep sleep so RadioLib can manage pins
        gpio_hold_dis(GPIO_NUM_21);   // RXEN
        gpio_hold_dis(GPIO_NUM_10);   // TXEN
        gpio_hold_dis(GPIO_NUM_7);    // CS
        gpio_hold_dis(GPIO_NUM_5);    // SCK
        gpio_hold_dis(GPIO_NUM_6);    // MOSI
        gpio_hold_dis(GPIO_NUM_3);    // MISO
        gpio_deep_sleep_hold_dis();

        char restoredSSID[32];
        NodeRole restoredRole;
        if (deepSleepMgr.restoreState(restoredRole, restoredSSID)) {
            g_role = restoredRole;
            strncpy(_apSSID, restoredSSID, sizeof(_apSSID));
            Serial.printf("[Wake] Fast-path: restored role=%s, SSID=%s\n",
                          RoleConfig::roleName(g_role), _apSSID);

            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("LoRa Wake!");
            display.printf("Role: %s\n", RoleConfig::roleName(g_role));
            display.printf("Boot #%lu\n", rtcBootCount);
            display.display();
        } else {
            // RTC state invalid — fall through to normal boot
            Serial.println("[Wake] No valid RTC state — doing normal boot");
            goto normal_boot;
        }
    } else {
normal_boot:
        if (!wasCleanBoot()) {
            Serial.println("[Boot] Non-clean reset detected — adding extra stabilisation delay");
            delay(2000);
        }

        // ── Role selection (auto-negotiate or manual BOOT override) ──
        bool manualOverride = false;
        g_role = RoleConfig::determineRole(display, manualOverride);
        Serial.printf("\n[Role] Booting as: %s (%s)\n\n",
                      RoleConfig::roleName(g_role),
                      manualOverride ? "manual" : "auto-negotiate");
    }

    // ── Initialize FreeRTOS primitives ───────────────────────
    initRTOS();

    // ── Role-specific hardware init ──────────────────────────
    if (g_role == NODE_ROLE_GATEWAY) {
        initGateway();
    } else {
        initLeafRelay();
    }

    _activeRole = g_role;

    // ── Create FreeRTOS tasks based on role ──────────────────
    if (g_role == NODE_ROLE_GATEWAY) {
        xTaskCreatePinnedToCore(taskLoRaGateway,  "LoRa_GW",     STACK_LORA,        nullptr, PRIORITY_LORA,        &hTaskLoRa,       CORE_LORA);
        xTaskCreatePinnedToCore(taskHarvestGateway, "Harvest_GW", STACK_HARVEST,     nullptr, PRIORITY_HARVEST,     &hTaskHarvest,    CORE_NETWORK);
        // Gateway also runs CoAP server to receive /announce from leaves
        xTaskCreatePinnedToCore(taskCoapServerLoop, "CoAP_Srv",  STACK_COAP_SERVER, nullptr, PRIORITY_COAP_SERVER, &hTaskCoapServer, CORE_NETWORK);
    } else {
        xTaskCreatePinnedToCore(taskLoRaLeafRelay, "LoRa_LR",    STACK_LORA,        nullptr, PRIORITY_LORA,        &hTaskLoRa,       CORE_LORA);
        xTaskCreatePinnedToCore(taskCoapServerLoop, "CoAP_Srv",  STACK_COAP_SERVER, nullptr, PRIORITY_COAP_SERVER, &hTaskCoapServer, CORE_NETWORK);
        // taskHarvestGateway created lazily on promotion (saves 32KB heap)

        if (g_role == NODE_ROLE_RELAY) {
            xTaskCreatePinnedToCore(taskRelayHarvest, "Relay_H",  STACK_HARVEST,    nullptr, PRIORITY_HARVEST,     &hTaskRelayHarvest,    CORE_NETWORK);
        }
    }

    Serial.printf("[RTOS] Tasks created — Core 0: LoRa, Core 1: %s\n",
                  g_role == NODE_ROLE_GATEWAY ? "Harvest" : "CoAP Server");
}

void loop() {
    // ── Serial commands (block/unblock/list) ─────────────────
    serialCmd.tick();

    // ── Update active role from election manager ─────────────
    _activeRole = electionMgr.activeRole();

    // ── OLED Update (every 2 s) — runs at low priority ───────
    static uint32_t lastDisplayMs = 0;
    if (millis() - lastDisplayMs < 2000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        return;
    }
    lastDisplayMs = millis();

    // Track display mode so we can do a full clear on gateway→leaf transition
    static bool lastDisplayWasGateway = (g_role == NODE_ROLE_GATEWAY);

    if (_activeRole == NODE_ROLE_GATEWAY) {
        lastDisplayWasGateway = true;
        // ── Gateway OLED ─────────────────────────────────────
        uint8_t activeNodes = 0;
        if (registryLock()) {
            activeNodes = registry.activeCount();
            registryUnlock();
        }

        HarvestState curState = harvestLoop.state();
        bool firstHarvestDone = (curState != HARVEST_IDLE) ||
                                (harvestLoop.lastCycleStats().nodesAttempted > 0);

        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("Gateway (RTOS+AODV)");
        display.printf("Nodes:%u Rtes:%u\n",
                       activeNodes, aodvRouter.routeCount());
        display.printf("State: %s\n", harvestLoop.stateStr());

        if (curState >= HARVEST_CONNECT && curState <= HARVEST_DOWNLOAD) {
            display.printf("-> %s\n", harvestLoop.currentNodeSSID());
        } else if (firstHarvestDone) {
            const HarvestCycleStats& stats = harvestLoop.lastCycleStats();
            display.printf("Last: %u OK, %u fail\n",
                           stats.nodesSucceeded, stats.nodesFailed);
            display.printf("Imgs: %lu\n", stats.totalImages);
        }

        // Stack high-water marks for debugging
        if (hTaskLoRa) {
            display.printf("Stk L:%u H:%u\n",
                           uxTaskGetStackHighWaterMark(hTaskLoRa),
                           hTaskHarvest ? uxTaskGetStackHighWaterMark(hTaskHarvest) : 0);
        }

        display.display();

    } else {
        // ── Leaf/Relay OLED ──────────────────────────────────
        // Full clear on gateway→leaf/relay transition (old header persists otherwise)
        if (lastDisplayWasGateway) {
            lastDisplayWasGateway = false;
            display.clearDisplay();
        }

        // Always redraw static header fields (rows 0-40)
        display.fillRect(0, 0, 128, 48, SSD1306_BLACK);
        display.setCursor(0, 0);
        display.println(_activeRole == NODE_ROLE_RELAY ? "Relay (AODV)" : "Leaf (AODV)");
        display.printf("AP: %s\n", _apSSID);
        display.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());
        display.printf("CoAP: %s\n", coapServer.requestCount() > 0 ? ":5683" : "OFF");
        display.printf("Imgs: %d  LoRa:%s\n",
                       storage.imageCount(), _loraReady ? "OK" : "NO");
        display.println("────────────────────");

        if (_relayCachedServing) {
            display.fillRect(0, 48, 128, 16, SSD1306_BLACK);
            display.setCursor(0, 48);
            uint32_t elapsed = (millis() - _relayCachedStartMs) / 1000;
            display.printf("Serving:%u %lus",
                           cachedStorage.imageCount(), elapsed);
            display.setCursor(0, 56);
            display.printf("CReqs:%-3lu CBlk:%-4lu",
                           cachedCoapServer.requestCount(),
                           cachedCoapServer.blocksSent());
        } else {
            display.fillRect(0, 56, 128, 8, SSD1306_BLACK);
            display.setCursor(0, 56);
            display.printf("Reqs:%-4lu Blks:%-5lu",
                           coapServer.requestCount(), coapServer.blocksSent());
        }

        // Show election state on OLED if not idle
        ElectionState es = electionMgr.state();
        if (es == ELECT_ELECTION_START || es == ELECT_WAITING) {
            display.fillRect(0, 40, 128, 8, SSD1306_BLACK);
            display.setCursor(0, 40);
            display.println("ELECTING...");
        } else if (es == ELECT_ACTING_GATEWAY || es == ELECT_PROMOTED) {
            display.fillRect(0, 0, 128, 8, SSD1306_BLACK);
            display.setCursor(0, 0);
            display.println("ACTING GW");
        } else if (es == ELECT_STOOD_DOWN) {
            display.fillRect(0, 40, 128, 8, SSD1306_BLACK);
            display.setCursor(0, 40);
            display.println("ELECTION LOST");
        } else if (es == ELECT_RECLAIMED) {
            display.fillRect(0, 40, 128, 8, SSD1306_BLACK);
            display.setCursor(0, 40);
            display.println("RECLAIMED -> LEAF");
        }

        display.display();
    }
}
