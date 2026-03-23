/**
 * @file TaskLoRaLeafRelay.cpp
 * @brief FreeRTOS Task: LoRa (Core 0) — Leaf/Relay mode
 *
 * Extracted from main.cpp. Contains taskLoRaLeafRelay().
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "Globals.h"

// =============================================================
// FreeRTOS Task: LoRa (Core 0) — Leaf/Relay mode
// =============================================================

/**
 * Leaf/Relay LoRa task: beacon TX, LoRa RX dispatch,
 * AODV routing ticks, election ticks.
 *
 * Relay HARVEST_CMD packets are forwarded to Core 1 via xRelayHarvestQueue.
 */
void taskLoRaLeafRelay(void* param) {
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
                              len, beacon.ssid, beacon.imageCount);
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

            // Switch WiFi to gateway AP mode so leaves can connect as STA
            uint8_t gwMac[6];
            WiFi.macAddress(gwMac);
            char gwSSID[32];
            BeaconPacket::macToSsid(gwMac, NODE_ROLE_GATEWAY, gwSSID, sizeof(gwSSID));
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_AP);
            WiFi.softAP(gwSSID, AP_PASS);
            delay(100);
            strncpy(_apSSID, gwSSID, sizeof(_apSSID));  // Update for OLED/logs
            Serial.printf("[Promoted GW] WiFi AP switched to: %s\n", gwSSID);

            // Start CoAP server for /announce handling from leaves
            if (coapServer.begin()) {
                Serial.printf("[Promoted GW] CoAP server started on port %u\n", COAP_DEFAULT_PORT);
            }

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
