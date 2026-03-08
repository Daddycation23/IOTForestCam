/**
 * @file main.cpp
 * @brief IOT Forest Cam — Tri-mode: Leaf, Gateway, or Relay
 *
 * Build with:
 *   pio run -e esp32s3           -> Leaf node   (WiFi AP + CoAP Server + LoRa Beacon TX)
 *   pio run -e esp32s3_gateway   -> Gateway     (LoRa Beacon RX + WiFi STA + CoAP Client)
 *   pio run -e esp32s3_relay     -> Relay node  (Leaf + LoRa Beacon Relay)
 *
 * Hardware: LILYGO T3-S3 V1.2
 *   - OLED:    SSD1306 128x64 (I2C: SDA=18, SCL=17)
 *   - SD Card: HSPI (MOSI=11, MISO=2, CLK=14, CS=13)
 *   - LoRa:    SX1262 FSPI (MOSI=6, MISO=3, SCK=5, CS=7, DIO1=33, BUSY=34, RST=8)
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

// LoRa + AODV (all modes)
#include "LoRaRadio.h"
#include "LoRaBeacon.h"
#include "LoRaDispatch.h"
#include "AodvPacket.h"
#include "AodvRouter.h"

#ifdef GATEWAY_MODE
  #include <SD.h>
  #include "CoapClient.h"
  #include "NodeRegistry.h"
  #include "HarvestLoop.h"
#else
  #include "StorageReader.h"
  #include "CoapServer.h"
  #ifdef RELAY_MODE
    #include <SD.h>
    #include "CoapClient.h"
  #endif
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

// ─── Shared LoRa Radio ──────────────────────────────────────
LoRaRadio loraRadio;

// ─── Shared AODV Router ────────────────────────────────────
AodvRouter aodvRouter(loraRadio);

// ─── Beacon Timing ──────────────────────────────────────────
static constexpr uint32_t BEACON_INTERVAL_MS = 30000;   // 30 seconds
static constexpr uint32_t BEACON_JITTER_MS   = 2000;    // +/- 2s random jitter

// ─── HSPI bus for SD card (shared instance) ─────────────────
static SPIClass gwSPI(HSPI);


// =============================================================
// GATEWAY MODE — LoRa Beacon RX + WiFi STA + CoAP Client
// =============================================================
#ifdef GATEWAY_MODE

// ─── Gateway Configuration ──────────────────────────────────
static constexpr uint32_t HARVEST_LISTEN_PERIOD_MS = 60000;  // Listen 60s before harvest
static constexpr uint32_t ROUTE_DISCOVERY_DELAY_MS = 15000;  // Broadcast RREQ 15s after boot

NodeRegistry registry;
CoapClient   coapClient;
HarvestLoop  harvestLoop(registry, coapClient);

// ─── AODV route-discovered callback ─────────────────────────
static void onRouteDiscovered(const uint8_t destId[6], uint8_t hopCount) {
    RouteEntry route;
    if (aodvRouter.getRoute(destId, route)) {
        registry.updateFromRoute(destId, route.nextHopId, route.hopCount);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    // ── OLED Init ────────────────────────────────────────────
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    displayStatus("Gateway Mode");

    Serial.println("\n===================================");
    Serial.println("  IOT Forest Cam — Gateway (LoRa)");
    Serial.println("  AODV Routing Enabled");
    Serial.println("===================================\n");

    // ── SD Card Init (for saving received images) ────────────
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

    // ── LoRa Init ────────────────────────────────────────────
    displayStatus("LoRa Init...");
    if (!loraRadio.begin()) {
        displayStatus("LoRa FAILED!");
        Serial.println("[ERROR] LoRa SX1262 init failed — check wiring");
        while (true) delay(1000);
    }

    // Enter continuous receive mode for beacons + AODV
    loraRadio.startReceive();

    // ── AODV Router Init ─────────────────────────────────────
    uint8_t myMac[6];
    WiFi.macAddress(myMac);
    aodvRouter.begin(myMac);
    aodvRouter.setRouteDiscoveredCallback(onRouteDiscovered);
    harvestLoop.setAodv(&aodvRouter, &loraRadio);

    // ── WiFi in STA mode (but don't connect yet) ─────────────
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);

    // ── Display Ready Status ─────────────────────────────────
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

void loop() {
    // ── Poll for LoRa packets (dispatch by type) ────────────
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
                // Gateway shouldn't normally receive its own RREQ back,
                // but relay might forward others' RREQs
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

        // Check which nodes expired and notify AODV
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
    static bool routeDiscoveryDone = false;
    static uint32_t bootMs = millis();

    if (!routeDiscoveryDone && millis() - bootMs >= ROUTE_DISCOVERY_DELAY_MS &&
        registry.activeCount() > 0) {
        Serial.println("\n[AODV] Broadcasting RREQ for all nodes (topology discovery)...");
        aodvRouter.discoverAll();
        routeDiscoveryDone = true;
    }

    // ── Re-run route discovery before each harvest ───────────
    static uint32_t listenStartMs = millis();
    static bool firstHarvestDone = false;

    if (harvestLoop.state() == HARVEST_IDLE &&
        registry.activeCount() > 0 &&
        millis() - listenStartMs >= HARVEST_LISTEN_PERIOD_MS)
    {
        // Trigger route discovery if not recently done
        if (!aodvRouter.isDiscoveryPending()) {
            Serial.println("\n[AODV] Pre-harvest route discovery...");
            aodvRouter.discoverAll();
        }

        Serial.printf("\n[Harvest] Starting cycle — %u node(s) registered, %u routes\n",
                      registry.activeCount(), aodvRouter.routeCount());
        registry.dump();
        aodvRouter.dumpRoutes();
        harvestLoop.startCycle();
        firstHarvestDone = true;
    }

    // ── Harvest state machine tick ───────────────────────────
    harvestLoop.tick();

    // ── Reset listen timer after harvest completes ───────────
    static HarvestState prevState = HARVEST_IDLE;
    HarvestState curState = harvestLoop.state();

    if (curState == HARVEST_IDLE && prevState == HARVEST_DONE) {
        listenStartMs = millis();
        routeDiscoveryDone = false;   // Re-discover routes next cycle
        loraRadio.startReceive();
        Serial.println("[Gateway] Resuming beacon listening...\n");
    }
    prevState = curState;

    // ── OLED Update ──────────────────────────────────────────
    static uint32_t lastDisplayMs = 0;
    if (millis() - lastDisplayMs >= 2000) {
        lastDisplayMs = millis();

        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("Gateway (AODV)");
        display.printf("Nodes:%u Rtes:%u\n", registry.activeCount(), aodvRouter.routeCount());
        display.printf("State: %s\n", harvestLoop.stateStr());

        if (curState >= HARVEST_CONNECT && curState <= HARVEST_DOWNLOAD) {
            display.printf("-> %s\n", harvestLoop.currentNodeSSID());
        } else if (firstHarvestDone) {
            const HarvestCycleStats& stats = harvestLoop.lastCycleStats();
            display.printf("Last: %u OK, %u fail\n", stats.nodesSucceeded, stats.nodesFailed);
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


// =============================================================
// LEAF / RELAY MODE — WiFi AP + CoAP Server + LoRa Beacons
// =============================================================
#else

static const char* AP_SSID_PREFIX = "ForestCam";
static const char* AP_PASS        = "forestcam123";
static char _apSSID[32];

StorageReader storage;
CoapServer    coapServer(storage);

// LoRa beacon state
static bool _loraReady = false;

#ifdef RELAY_MODE
// ── Relay-specific: harvest command handling ─────────────────

static bool     _relayBusy      = false;    // Currently executing a harvest command
static uint8_t  _relayCmdId     = 0;        // Active command ID

/**
 * Execute a relay harvest: connect to the target leaf, download images to /cached/,
 * then send HARVEST_ACK back to the gateway.
 *
 * This is a BLOCKING operation — called from the main loop when a HARVEST_CMD arrives.
 * The relay switches to AP_STA mode to keep its AP running while connecting to the leaf.
 */
static void relayHarvest(const HarvestCmdPacket& cmd) {
    Serial.printf("\n╔══════════════════════════════════════╗\n");
    Serial.printf("║  RELAY HARVEST: %s  ║\n", cmd.ssid);
    Serial.printf("╚══════════════════════════════════════╝\n\n");

    _relayBusy = true;
    _relayCmdId = cmd.cmdId;

    HarvestAckPacket ack;
    ack.cmdId = cmd.cmdId;
    WiFi.macAddress(ack.relayId);
    ack.status     = HARVEST_STATUS_OK;
    ack.imageCount = 0;
    ack.totalBytes = 0;

    // ── Switch to AP+STA mode (keep AP running) ─────────────
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
            uint16_t leafPort = COAP_DEFAULT_PORT;

            // Get image count
            uint8_t infoBuf[512];
            size_t infoLen = sizeof(infoBuf);
            CoapClientError err = relayCoap.get(leafIP, leafPort, "info", infoBuf, infoLen);

            uint8_t imageCount = 0;
            if (err == COAP_CLIENT_OK) {
                infoBuf[infoLen] = '\0';
                const char* countKey = strstr((char*)infoBuf, "\"count\":");
                if (countKey) {
                    imageCount = (uint8_t)atoi(countKey + 8);
                }
            }

            if (imageCount > 0) {
                // Create /cached directory on SD
                if (!SD.exists("/cached")) {
                    SD.mkdir("/cached");
                }

                // Download each image
                for (uint8_t i = 0; i < imageCount; i++) {
                    char outPath[64];
                    snprintf(outPath, sizeof(outPath), "/cached/relay_img_%03u.jpg", i);

                    if (SD.exists(outPath)) {
                        SD.remove(outPath);
                    }

                    TransferStats stats;
                    err = relayCoap.downloadImage(leafIP, leafPort, i, outPath, stats);

                    if (err == COAP_CLIENT_OK) {
                        ack.imageCount++;
                        ack.totalBytes += stats.totalBytes;
                        Serial.printf("[Relay] Cached image %u: %lu bytes -> %s\n",
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

        // ── Disconnect STA, return to AP-only ────────────────
        WiFi.disconnect(true);
    }

    WiFi.mode(WIFI_AP);
    WiFi.softAP(_apSSID, AP_PASS);
    delay(200);
    Serial.printf("[Relay] Back to AP mode: %s\n", _apSSID);

    // ── Send HARVEST_ACK via LoRa ────────────────────────────
    Serial.printf("[Relay] Sending HARVEST_ACK: status=%s, images=%u, bytes=%lu\n",
                  HarvestAckPacket::statusToString(ack.status),
                  ack.imageCount, ack.totalBytes);

    uint8_t ackBuf[64];
    uint8_t ackLen = ack.serialize(ackBuf, sizeof(ackBuf));
    if (ackLen > 0) {
        loraRadio.send(ackBuf, ackLen);
        loraRadio.startReceive();
    }

    _relayBusy = false;
    Serial.println("[Relay] Harvest command complete\n");
}
#endif // RELAY_MODE

void setup() {
    Serial.begin(115200);
    delay(1000);

    // ── OLED Init ────────────────────────────────────────────
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

#ifdef RELAY_MODE
    displayStatus("Relay Mode");
    Serial.println("\n======================================");
    Serial.println("  IOT Forest Cam — Relay (LoRa+CoAP)");
    Serial.println("  AODV Routing Enabled");
    Serial.println("======================================\n");
#else
    displayStatus("Leaf Mode");
    Serial.println("\n====================================");
    Serial.println("  IOT Forest Cam — Leaf (LoRa+CoAP)");
    Serial.println("  AODV Routing Enabled");
    Serial.println("====================================\n");
#endif

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

    // ── LoRa Init ────────────────────────────────────────────
    displayStatus("LoRa Init...");
    if (!loraRadio.begin()) {
        Serial.println("[WARN] LoRa init failed — running without beacons");
        // Non-fatal: leaf still works for direct WiFi connection
    } else {
        _loraReady = true;
        Serial.println("[OK] LoRa beacon TX enabled (30s interval)");

        // All nodes need RX for AODV (leaf listens for RREQ)
        loraRadio.startReceive();
        Serial.println("[OK] LoRa RX enabled (AODV routing)");
    }

    // ── AODV Router Init ─────────────────────────────────────
    if (_loraReady) {
        aodvRouter.begin(mac);
    }

    // ── Display Ready Status ─────────────────────────────────
    display.clearDisplay();
    display.setCursor(0, 0);
#ifdef RELAY_MODE
    display.println("Relay (AODV)");
#else
    display.println("Leaf (AODV)");
#endif
    display.printf("AP: %s\n", _apSSID);
    display.printf("IP: %s\n", ip.toString().c_str());
    display.printf("CoAP: :%u\n", COAP_DEFAULT_PORT);
    display.printf("Imgs: %d  LoRa:%s\n",
                   storage.imageCount(),
                   _loraReady ? "OK" : "NO");
    display.println("────────────────────");
    display.println("Reqs: 0  Blks: 0");
    display.display();
}

void loop() {
    // ── CoAP Server ──────────────────────────────────────────
    coapServer.loop();

    // ── LoRa Beacon Broadcast (every ~30s with jitter) ───────
    if (_loraReady) {
        static uint32_t lastBeaconMs = 0;
        static uint32_t nextInterval = BEACON_INTERVAL_MS;

        if (millis() - lastBeaconMs >= nextInterval) {
            lastBeaconMs = millis();
            // Add random jitter to avoid synchronized collisions
            nextInterval = BEACON_INTERVAL_MS + random(-BEACON_JITTER_MS, BEACON_JITTER_MS);

            // Build beacon packet
            BeaconPacket beacon;
            beacon.packetType = BEACON_TYPE_BEACON;
            beacon.ttl = 2;
            WiFi.macAddress(beacon.nodeId);
#ifdef RELAY_MODE
            beacon.nodeRole = NODE_ROLE_RELAY;
#else
            beacon.nodeRole = NODE_ROLE_LEAF;
#endif
            beacon.ssidLen = strlen(_apSSID);
            if (beacon.ssidLen > BEACON_MAX_SSID) beacon.ssidLen = BEACON_MAX_SSID;
            memcpy(beacon.ssid, _apSSID, beacon.ssidLen);
            beacon.ssid[beacon.ssidLen] = '\0';
            beacon.imageCount = storage.imageCount();
            beacon.batteryPct = 0xFF;       // USB powered
            beacon.uptimeMin = (uint16_t)(millis() / 60000);

            // Serialize and transmit
            uint8_t buf[BEACON_MAX_SIZE];
            uint8_t len = beacon.serialize(buf, sizeof(buf));
            if (len > 0) {
                loraRadio.send(buf, len);
                Serial.printf("[LoRa] Beacon TX (%u bytes) — %s, %u images\n",
                              len, _apSSID, beacon.imageCount);
            }

            // Resume receive mode after transmitting
            loraRadio.startReceive();
        }
    }

    // ── LoRa RX: Dispatch all packet types ──────────────────
    if (_loraReady) {
        LoRaRxResult rx;
        if (loraRadio.checkReceive(rx)) {
            uint8_t pktType = getLoRaPacketType(rx.data, rx.length);

            switch (pktType) {
#ifdef RELAY_MODE
                case BEACON_TYPE_BEACON:
                case BEACON_TYPE_BEACON_RELAY: {
                    // Relay: re-broadcast other nodes' beacons
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
                    break;
                }
#endif // RELAY_MODE

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

#ifdef RELAY_MODE
                case PKT_TYPE_HARVEST_CMD: {
                    HarvestCmdPacket cmd;
                    if (cmd.parse(rx.data, rx.length)) {
                        uint8_t myMac[6];
                        WiFi.macAddress(myMac);
                        if (memcmp(cmd.relayId, myMac, 6) == 0 && !_relayBusy) {
                            loraRadio.startReceive();  // Resume RX before blocking harvest
                            relayHarvest(cmd);
                        }
                    }
                    break;
                }
#endif // RELAY_MODE

                default:
                    // Non-beacon, non-AODV packet (or leaf receiving beacons — ignore)
                    break;
            }

            // Resume listening after processing
            loraRadio.startReceive();
        }

        // ── AODV periodic tick ──────────────────────────────
        aodvRouter.tick();
    }

    // ── OLED Update ──────────────────────────────────────────
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
