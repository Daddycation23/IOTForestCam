/**
 * @file InitNodes.cpp
 * @brief Role-specific hardware initialization for gateway and leaf/relay nodes
 *
 * Extracted from main.cpp for maintainability.
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "Globals.h"
#include "ElectionPacket.h"

// Forward declaration from main.cpp
void displayStatus(const char* line);
bool wasCleanBoot();

void initGateway() {
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
    strncpy(_apSSID, _gwAPSSID, sizeof(_apSSID));  // Populate global for consistency

    WiFi.mode(WIFI_AP);
    WiFi.softAP(_gwAPSSID, AP_PASS);
    delay(100);
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    IPAddress gwIP = WiFi.softAPIP();
    Serial.printf("[OK] Gateway WiFi AP: %s (pass: %s)\n", _gwAPSSID, AP_PASS);
    Serial.printf("[OK] Gateway IP: %s\n", gwIP.toString().c_str());

    // ── Gateway CoAP server (receives /announce from leaves) ──
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

void initLeafRelay() {
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
        WiFi.setTxPower(WIFI_POWER_19_5dBm);
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
        WiFi.setTxPower(WIFI_POWER_19_5dBm);

        IPAddress ip = WiFi.softAPIP();
        Serial.printf("[OK] WiFi AP: %s (pass: %s)\n", _apSSID, AP_PASS);
        Serial.printf("[OK] IP: %s\n", ip.toString().c_str());
    }

    // ── CoAP Server (all nodes with SD — serves images to whoever connects) ──
    if (_sdReady) {
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
                deepSleepMgr.onActivity();
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
