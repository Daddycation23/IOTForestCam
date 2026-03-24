/**
 * @file TaskRelayHarvest.cpp
 * @brief FreeRTOS Task: Relay Harvest (Core 1) — Relay mode
 *
 * Extracted from main.cpp. Contains taskRelayHarvest(),
 * relayHarvest(), and relayCachedCleanup().
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "Globals.h"

// =============================================================
// Relay cached-image cleanup
// =============================================================

/**
 * Clean up after the gateway has downloaded (or timed out downloading)
 * cached images from the relay.  Deletes /cached/ files, stops the
 * CoAP server serving them, and frees the relay for the next harvest.
 */
void relayCachedCleanup() {
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
void relayHarvest(const HarvestCmdPacket& cmd) {
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
    // WiFi already disconnected above (line 159) — just wait for cleanup
    vTaskDelay(pdMS_TO_TICKS(200));

    // ── Send HARVEST_ACK via LoRa (thread-safe) ──────────────
    Serial.printf("[Relay] ACK: status=%s, images=%u, bytes=%lu\n",
                  HarvestAckPacket::statusToString(ack.status),
                  ack.imageCount, ack.totalBytes);

    uint8_t ackBuf[64];
    uint8_t ackLen = ack.serialize(ackBuf, sizeof(ackBuf));
    if (ackLen > 0) {
        // Use TX queue instead of direct SPI access from Core 1
        loraTxEnqueue(ackBuf, ackLen);
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
// FreeRTOS Task: Relay Harvest (Core 1) — Relay mode
// =============================================================

/**
 * Relay harvest task: waits for HARVEST_CMD from Core 0,
 * then executes the blocking relay harvest on Core 1.
 */
void taskRelayHarvest(void* param) {
    HarvestCmdPacket cmd;
    for (;;) {
        if (xQueueReceive(xRelayHarvestQueue, &cmd, portMAX_DELAY) == pdTRUE) {
            Serial.println("[Relay Task] Executing relay harvest on Core 1");
            relayHarvest(cmd);
        }
    }
}
