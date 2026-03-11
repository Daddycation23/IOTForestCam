/**
 * @file main.cpp
 * @brief IOT Forest Cam — Unified firmware: Leaf, Gateway, or Relay
 *
 * Role is selected at boot via a 5-second OLED menu using the built-in
 * BOOT button (GPIO 0).  The selection is persisted to NVS so it survives
 * power cycles — no recompilation needed to change roles.
 *
 * Build: pio run -e esp32s3_unified
 *
 * Hardware: LILYGO T3-S3 V1.2
 *   - OLED:    SSD1306 128x64 (I2C: SDA=18, SCL=17)
 *   - SD Card: HSPI (MOSI=11, MISO=2, CLK=14, CS=13)
 *   - LoRa:    SX1262 FSPI (MOSI=6, MISO=3, SCK=5, CS=7, DIO1=33, BUSY=34, RST=8)
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

// ─── LILYGO T3-S3 V1.2 Pin Definitions ──────────────────────
#define OLED_SDA 18
#define OLED_SCL 17

// ─── Runtime role ─────────────────────────────────────────────
/// Set once in setup() by RoleConfig::selectRole(); never modified after that.
NodeRole g_role = NODE_ROLE_LEAF;

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

static bool    _loraReady    = false;
static bool    _gwLoraReady  = false;   // Gateway LoRa status (set in initGateway)
static bool    _relayBusy    = false;
static uint8_t _relayCmdId   = 0;

// ─── Relay cached-image serving state ────────────────────────
static bool     _relayCachedServing  = false;   // Currently serving cached images?
static uint32_t _relayCachedStartMs  = 0;       // When we started serving
static constexpr uint32_t RELAY_CACHED_TIMEOUT_MS = 120000;  // Auto-cleanup after 2 min

// ─── Gateway timing ───────────────────────────────────────────
static constexpr uint32_t HARVEST_LISTEN_PERIOD_MS = 60000;  // Listen 60 s before harvest
static constexpr uint32_t ROUTE_DISCOVERY_DELAY_MS = 15000;  // RREQ 15 s after boot


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

    _relayBusy = false;
    Serial.println("[Relay] Cached image cleanup complete — ready for next harvest\n");
}

// =============================================================
// Gateway helpers
// =============================================================

// ── AODV route-discovered callback ───────────────────────────
static void onRouteDiscovered(const uint8_t destId[6], uint8_t hopCount) {
    RouteEntry route;
    if (aodvRouter.getRoute(destId, route)) {
        registry.updateFromRoute(destId, route.nextHopId, route.hopCount);
    }
}

// =============================================================
// Relay helper — blocking store-and-forward harvest
// =============================================================

/**
 * Execute a relay harvest: switch to AP_STA, connect to the target leaf,
 * download images to /cached/ on SD, send HARVEST_ACK via LoRa.
 *
 * This is a BLOCKING call (up to ~120 s).  The LoRa radio is resumed
 * before entering the WiFi-blocking section.
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
    delay(200);

    // ── Connect STA to the target leaf's AP ─────────────────
    Serial.printf("[Relay] Connecting to %s...\n", cmd.ssid);
    WiFi.begin(cmd.ssid, AP_PASS);

    uint32_t connectStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - connectStart < 15000) {
        delay(250);
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
            uint8_t infoBuf[512];
            size_t  infoLen = sizeof(infoBuf);
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
                    err = relayCoap.downloadImage(leafIP, leafPort, i, outPath, stats);

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

    // ── Return to AP-only mode ───────────────────────────────
    WiFi.mode(WIFI_AP);
    WiFi.softAP(_apSSID, AP_PASS);
    delay(200);
    Serial.printf("[Relay] Back to AP mode: %s\n", _apSSID);

    // ── Send HARVEST_ACK via LoRa ────────────────────────────
    Serial.printf("[Relay] ACK: status=%s, images=%u, bytes=%lu\n",
                  HarvestAckPacket::statusToString(ack.status),
                  ack.imageCount, ack.totalBytes);

    uint8_t ackBuf[64];
    uint8_t ackLen = ack.serialize(ackBuf, sizeof(ackBuf));
    if (ackLen > 0) {
        loraRadio.send(ackBuf, ackLen);
        loraRadio.startReceive();
    }

    // ── Start serving cached images for the gateway ─────────
    // The gateway will connect to our AP and download via CoAP.
    // _relayBusy stays true until cached serving completes.
    if (ack.status == HARVEST_STATUS_OK && ack.imageCount > 0) {
        if (cachedStorage.beginScanOnly()) {
            if (cachedCoapServer.begin()) {
                _relayCachedServing = true;
                _relayCachedStartMs = millis();
                Serial.printf("[Relay] Serving %u cached image(s) via CoAP — "
                              "gateway can connect to %s\n",
                              cachedStorage.imageCount(), _apSSID);
                Serial.println("[Relay] Harvest command complete — waiting for gateway download\n");
                return;  // _relayBusy stays true
            } else {
                Serial.println("[Relay] Failed to start cached CoAP server");
                cachedStorage.endScanOnly();
            }
        } else {
            Serial.println("[Relay] Failed to scan /cached/ directory");
        }
    }

    _relayBusy = false;
    Serial.println("[Relay] Harvest command complete\n");
}


// =============================================================
// Role-specific init helpers
// =============================================================

static void initGateway() {
    Serial.println("\n===================================");
    Serial.println("  IOT Forest Cam — Gateway (LoRa)");
    Serial.println("  AODV Routing Enabled");
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
        Serial.println("[WARN] LoRa SX1262 init failed — gateway running without LoRa");
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
    }

    // ── WiFi STA (connect only during harvest) ───────────────
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);

    // ── OLED ready screen ────────────────────────────────────
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Gateway (AODV)");
    display.println("Listening for");
    display.println("beacons...");
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
        Serial.println("  AODV Routing Enabled");
        Serial.println("======================================\n");
    } else {
        displayStatus("Leaf Mode");
        Serial.println("\n====================================");
        Serial.println("  IOT Forest Cam — Leaf (LoRa+CoAP)");
        Serial.println("  AODV Routing Enabled");
        Serial.println("====================================\n");
    }

    // ── SD Card (images stored in /images/) ──────────────────
    bool _sdReady = false;

    if (g_role == NODE_ROLE_RELAY) {
        // Mount SD for relay — needed for store-and-forward image caching.
        // Uses gwSPI (same pattern as gateway) since the relay's StorageReader
        // is not used for /images/ scanning.
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

        // After a brownout/crash the SD card may need extra time to recover.
        // Toggle CS pin to reset the card's SPI state machine.
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
            // Continue without CoAP — don't hang the board
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

    // ── WiFi AP ──────────────────────────────────────────────
    displayStatus("Starting WiFi AP...");

    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(_apSSID, sizeof(_apSSID), "%s-%02X%02X",
             AP_SSID_PREFIX, mac[4], mac[5]);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(_apSSID, AP_PASS);
    delay(100);

    // Lower TX power to reduce current spikes that cause brownout resets
    // when WiFi + SD card are active simultaneously.
    // WIFI_POWER_8_5dBm is plenty for close-range laptop testing.
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    IPAddress ip = WiFi.softAPIP();
    Serial.printf("[OK] WiFi AP: %s (pass: %s)\n", _apSSID, AP_PASS);
    Serial.printf("[OK] IP: %s\n", ip.toString().c_str());

    // ── CoAP Server (leaf only — relay uses cachedCoapServer on demand) ──
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
        Serial.println();
    } else {
        Serial.println("[WARN] CoAP server skipped — no SD card available");
    }

    // ── LoRa ─────────────────────────────────────────────────
    displayStatus("LoRa Init...");
    if (!loraRadio.begin()) {
        Serial.println("[WARN] LoRa init failed — running without beacons");
        // Non-fatal: leaf/relay still serves CoAP for direct connections
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
    }

    // ── OLED ready screen ────────────────────────────────────
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(g_role == NODE_ROLE_RELAY ? "Relay (AODV)" : "Leaf (AODV)");
    display.printf("AP: %s\n", _apSSID);
    display.printf("IP: %s\n", ip.toString().c_str());
    display.printf("CoAP: %s\n", _sdReady ? ":5683" : "OFF");
    display.printf("Imgs: %d  LoRa:%s\n",
                   _sdReady ? storage.imageCount() : 0, _loraReady ? "OK" : "NO");
    display.println("────────────────────");
    display.println("Reqs: 0  Blks: 0");
    display.display();
}


// =============================================================
// Role-specific loop bodies
// =============================================================

static void loopGateway() {
    // ── Poll for LoRa packets (dispatch by type) ────────────
    if (!_gwLoraReady) { delay(100); return; }   // No LoRa — nothing to do

    // ── Periodic LoRa diagnostic + RX recovery (every 15 s) ───
    static uint32_t lastDiag = 0;
    if (millis() - lastDiag >= 15000) {
        lastDiag = millis();
        uint8_t st = loraRadio.getStatus();
        uint8_t mode = (st >> 4) & 0x07;
        uint16_t irq = loraRadio.getIrqFlags();
        Serial.printf("[LoRa DIAG] status=0x%02X mode=%u(%s) IRQ=0x%04X DIO1=%d\n",
                      st, mode,
                      mode == 2 ? "STBY_RC" : mode == 3 ? "STBY_XOSC" :
                      mode == 4 ? "FS" : mode == 5 ? "RX" :
                      mode == 6 ? "TX" : "??",
                      irq, digitalRead(LORA_DIO1));

        // Auto-recover if radio dropped out of RX mode
        if (mode != 5 && mode != 6) {
            Serial.println("[LoRa] Radio not in RX — attempting recovery...");
            if (loraRadio.startReceive()) {
                Serial.println("[LoRa] RX recovery OK");
            } else {
                Serial.println("[LoRa] RX recovery FAILED");
            }
        }
    }

    LoRaRxResult rx;
    if (loraRadio.checkReceive(rx)) {
        uint8_t pktType = getLoRaPacketType(rx.data, rx.length);

        switch (pktType) {
            case BEACON_TYPE_BEACON:
            case BEACON_TYPE_BEACON_RELAY: {
                BeaconPacket beacon;
                if (beacon.parse(rx.data, rx.length)) {
                    registry.update(beacon, rx.rssi);

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
            default:
                log_d("Unknown LoRa packet type 0x%02X (%u bytes)", pktType, rx.length);
                break;
        }

        // Resume listening after processing
        loraRadio.startReceive();
    }

    // ── AODV periodic tick ───────────────────────────────────
    aodvRouter.tick();

    // ── Expire stale nodes + trigger RERR ────────────────────
    static uint32_t lastExpireMs = 0;
    if (millis() - lastExpireMs >= 10000) {
        lastExpireMs = millis();

        for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
            NodeEntry entry;
            if (registry.getNode(i, entry)) {
                if ((millis() - entry.lastSeenMs) > REGISTRY_EXPIRY_MS) {
                    aodvRouter.notifyLinkBreak(entry.nodeId);
                }
            }
        }
        registry.expireStale();
    }

    // ── Auto-broadcast RREQ for topology discovery ───────────
    static bool     routeDiscoveryDone = false;
    static uint32_t bootMs = millis();

    if (!routeDiscoveryDone && millis() - bootMs >= ROUTE_DISCOVERY_DELAY_MS &&
        registry.activeCount() > 0) {
        Serial.println("\n[AODV] Broadcasting RREQ for all nodes (topology discovery)...");
        aodvRouter.discoverAll();
        routeDiscoveryDone = true;
    }

    // ── Start harvest after listen period ────────────────────
    static uint32_t listenStartMs  = millis();
    static bool     firstHarvestDone = false;
    static HarvestState prevState  = HARVEST_IDLE;
    HarvestState curState = harvestLoop.state();

    if (curState == HARVEST_IDLE &&
        registry.activeCount() > 0 &&
        millis() - listenStartMs >= HARVEST_LISTEN_PERIOD_MS)
    {
        if (!aodvRouter.isDiscoveryPending()) {
            Serial.println("\n[AODV] Pre-harvest route discovery...");
            aodvRouter.discoverAll();
        }

        Serial.printf("\n[Harvest] Starting cycle — %u node(s), %u routes\n",
                      registry.activeCount(), aodvRouter.routeCount());
        registry.dump();
        aodvRouter.dumpRoutes();
        harvestLoop.startCycle();
        firstHarvestDone = true;
    }

    // ── Harvest state machine tick ───────────────────────────
    harvestLoop.tick();

    // ── Reset listen timer after harvest completes ───────────
    if (curState == HARVEST_IDLE && prevState == HARVEST_DONE) {
        listenStartMs = millis();
        routeDiscoveryDone = false;
        loraRadio.startReceive();
        Serial.println("[Gateway] Resuming beacon listening...\n");
    }
    prevState = curState;

    // ── OLED Update (every 2 s) ───────────────────────────────
    static uint32_t lastDisplayMs = 0;
    if (millis() - lastDisplayMs >= 2000) {
        lastDisplayMs = millis();

        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("Gateway (AODV)");
        display.printf("Nodes:%u Rtes:%u\n",
                       registry.activeCount(), aodvRouter.routeCount());
        display.printf("State: %s\n", harvestLoop.stateStr());

        if (curState >= HARVEST_CONNECT && curState <= HARVEST_DOWNLOAD) {
            display.printf("-> %s\n", harvestLoop.currentNodeSSID());
        } else if (firstHarvestDone) {
            const HarvestCycleStats& stats = harvestLoop.lastCycleStats();
            display.printf("Last: %u OK, %u fail\n",
                           stats.nodesSucceeded, stats.nodesFailed);
            display.printf("Imgs: %lu\n", stats.totalImages);
        } else {
            uint32_t remaining = 0;
            if (millis() - listenStartMs < HARVEST_LISTEN_PERIOD_MS) {
                remaining = (HARVEST_LISTEN_PERIOD_MS - (millis() - listenStartMs)) / 1000;
            }
            display.printf("Harvest in: %lus\n", remaining);
        }

        display.display();
    }
}

static void loopLeafRelay() {
    // ── CoAP Server (leaf serves /images/) ───────────────────
    coapServer.loop();

    // ── Relay: serve cached images to gateway ────────────────
    if (_relayCachedServing) {
        cachedCoapServer.loop();

        // Auto-cleanup after timeout (gateway should be done by now)
        if (millis() - _relayCachedStartMs >= RELAY_CACHED_TIMEOUT_MS) {
            Serial.println("[Relay] Cached serving timeout — cleaning up");
            relayCachedCleanup();
        }
    }

    // ── Periodic LoRa diagnostic + RX recovery (every 15 s) ───
    if (_loraReady) {
        static uint32_t lastDiag = 0;
        if (millis() - lastDiag >= 15000) {
            lastDiag = millis();
            uint8_t st = loraRadio.getStatus();
            uint8_t mode = (st >> 4) & 0x07;
            uint16_t irq = loraRadio.getIrqFlags();
            Serial.printf("[LoRa DIAG] status=0x%02X mode=%u(%s) IRQ=0x%04X DIO1=%d\n",
                          st, mode,
                          mode == 2 ? "STBY_RC" : mode == 3 ? "STBY_XOSC" :
                          mode == 4 ? "FS" : mode == 5 ? "RX" :
                          mode == 6 ? "TX" : "??",
                          irq, digitalRead(LORA_DIO1));

            // Auto-recover if radio dropped out of RX mode
            if (mode != 5 && mode != 6) {
                Serial.println("[LoRa] Radio not in RX — attempting recovery...");
                if (loraRadio.startReceive()) {
                    Serial.println("[LoRa] RX recovery OK");
                } else {
                    Serial.println("[LoRa] RX recovery FAILED");
                }
            }
        }
    }

    // ── LoRa Beacon Broadcast (every ~30 s with jitter) ──────
    if (_loraReady) {
        static uint32_t lastBeaconMs  = 0;
        static uint32_t nextInterval  = BEACON_INTERVAL_MS;

        if (millis() - lastBeaconMs >= nextInterval) {
            lastBeaconMs = millis();
            nextInterval = BEACON_INTERVAL_MS + random(-BEACON_JITTER_MS, BEACON_JITTER_MS);

            BeaconPacket beacon;
            beacon.packetType = BEACON_TYPE_BEACON;
            beacon.ttl = 2;
            WiFi.macAddress(beacon.nodeId);
            beacon.nodeRole   = (g_role == NODE_ROLE_RELAY)
                                    ? NODE_ROLE_RELAY : NODE_ROLE_LEAF;
            beacon.ssidLen    = strlen(_apSSID);
            if (beacon.ssidLen > BEACON_MAX_SSID) beacon.ssidLen = BEACON_MAX_SSID;
            memcpy(beacon.ssid, _apSSID, beacon.ssidLen);
            beacon.ssid[beacon.ssidLen] = '\0';
            beacon.imageCount = (g_role == NODE_ROLE_RELAY && _relayCachedServing)
                                    ? cachedStorage.imageCount()
                                    : storage.imageCount();
            beacon.batteryPct = 0xFF;         // USB powered
            beacon.uptimeMin  = (uint16_t)(millis() / 60000);

            uint8_t buf[BEACON_MAX_SIZE];
            uint8_t len = beacon.serialize(buf, sizeof(buf));
            if (len > 0) {
                loraRadio.send(buf, len);
                Serial.printf("[LoRa] Beacon TX (%u bytes) — %s, %u images\n",
                              len, _apSSID, beacon.imageCount);
            }

            loraRadio.startReceive();
        }
    }

    // ── LoRa RX: Dispatch all packet types ───────────────────
    if (_loraReady) {
        LoRaRxResult rx;
        if (loraRadio.checkReceive(rx)) {
            uint8_t pktType = getLoRaPacketType(rx.data, rx.length);

            switch (pktType) {
                case BEACON_TYPE_BEACON:
                case BEACON_TYPE_BEACON_RELAY: {
                    // Relay only: re-broadcast other nodes' beacons
                    if (g_role == NODE_ROLE_RELAY) {
                        BeaconPacket received;
                        if (received.parse(rx.data, rx.length)) {
                            uint8_t myMac[6];
                            WiFi.macAddress(myMac);

                            if (memcmp(received.nodeId, myMac, 6) != 0 && received.ttl > 1) {
                                received.ttl--;
                                received.packetType = BEACON_TYPE_BEACON_RELAY;

                                uint8_t relayBuf[BEACON_MAX_SIZE];
                                uint8_t relayLen = received.serialize(relayBuf, sizeof(relayBuf));

                                if (relayLen > 0) {
                                    delay(random(100, 500));
                                    loraRadio.send(relayBuf, relayLen);

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
                    // Relay only: gateway telling us to fetch from a leaf
                    if (g_role == NODE_ROLE_RELAY) {
                        HarvestCmdPacket cmd;
                        if (cmd.parse(rx.data, rx.length)) {
                            uint8_t myMac[6];
                            WiFi.macAddress(myMac);
                            if (memcmp(cmd.relayId, myMac, 6) == 0 && !_relayBusy) {
                                loraRadio.startReceive();  // Resume RX before blocking
                                relayHarvest(cmd);
                            }
                        }
                    }
                    break;
                }

                default:
                    break;
            }

            loraRadio.startReceive();
        }

        // ── AODV periodic tick ──────────────────────────────
        aodvRouter.tick();
    }

    // ── OLED Update (every 2 s) ───────────────────────────────
    static uint32_t lastDisplayUpdate = 0;
    if (millis() - lastDisplayUpdate > 2000) {
        lastDisplayUpdate = millis();

        if (_relayCachedServing) {
            // Show cached-serving status on bottom two lines
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

        display.display();
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
    // WiFi TX + SD card reads together can spike current draw on the
    // T3-S3, triggering a false brownout reset.  Disable the detector
    // so the board stays up during heavy WiFi+SPI traffic.
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(1000);

    // ── Log reset reason (helps diagnose brownouts / crashes) ─
    Serial.printf("\n[Boot] Reset reason: %s\n", getResetReasonStr());
    if (!wasCleanBoot()) {
        Serial.println("[Boot] Non-clean reset detected — adding extra stabilisation delay");
        delay(2000);   // Let power rail + SD card fully settle
    }

    // ── OLED must be ready before role-selection menu ────────
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    // ── 5-second boot menu: BOOT button cycles role ──────────
    // Reads NVS default, lets user change it, saves and returns.
    g_role = RoleConfig::selectRole(display);

    Serial.printf("\n[Role] Booting as: %s\n\n", RoleConfig::roleName(g_role));

    // ── Role-specific hardware init ──────────────────────────
    if (g_role == NODE_ROLE_GATEWAY) {
        initGateway();
    } else {
        initLeafRelay();   // handles both LEAF and RELAY
    }
}

void loop() {
    if (g_role == NODE_ROLE_GATEWAY) {
        loopGateway();
    } else {
        loopLeafRelay();   // handles both LEAF and RELAY
    }
}
