/**
 * @file TaskLoRaGateway.cpp
 * @brief FreeRTOS Task: LoRa (Core 0) — Gateway mode
 *
 * Extracted from main.cpp. Contains taskLoRaGateway() and the
 * onRouteDiscovered() AODV callback.
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "Globals.h"

// ── AODV route-discovered callback ───────────────────────────
void onRouteDiscovered(const uint8_t destId[6], uint8_t hopCount) {
    RouteEntry route;
    if (aodvRouter.getRoute(destId, route)) {
        if (registryLock()) {
            registry.updateFromRoute(destId, route.nextHopId, route.hopCount);
            registryUnlock();
        }
    }
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
void taskLoRaGateway(void* param) {
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
                g_beaconTxCount++;
                Serial.println("[GW] Beacon TX (liveness)");
            }

            lastGwBeacon = millis();
            gwBeaconInterval = BEACON_INTERVAL_MS
                             + random(-(int32_t)BEACON_JITTER_MS, (int32_t)BEACON_JITTER_MS);
        }

        // ── LoRa RX: dispatch packets ───────────────────────
        LoRaRxResult rx;
        if (loraCheckReceiveSafe(rx)) {
            // ── LoRa-level blocklist: drop packets from blocked senders ──
            uint8_t senderMac[6];
            if (extractSenderMac(rx.data, rx.length, senderMac) &&
                serialCmd.isNodeBlocked(senderMac)) {
                loraStartReceiveSafe();
                continue;
            }

            uint8_t pktType = getLoRaPacketType(rx.data, rx.length);

            switch (pktType) {
                case BEACON_TYPE_BEACON:
                case BEACON_TYPE_BEACON_RELAY: {
                    BeaconPacket beacon;
                    if (beacon.parse(rx.data, rx.length)) {
                        // Count any valid beacon heard (including our own echo, before filtering)
                        g_beaconRxCount++;

                        // Skip our own beacons (relayed back to us)
                        uint8_t gwMac[6];
                        WiFi.macAddress(gwMac);
                        if (memcmp(beacon.nodeId, gwMac, 6) == 0) break;

                        bool isNew = false;
                        if (registryLock()) {
                            isNew = registry.update(beacon, rx.rssi);
                            registryUnlock();
                        }
                        if (isNew && _lastNewBeaconMs == 0) {
                            _lastNewBeaconMs = millis();
                        }
                        // Re-evaluate relay assignment when a new node appears
                        if (isNew) {
                            electionMgr.assignRelayByRssi(true);
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
                case PKT_TYPE_RELAY_ASSIGN:
                    electionMgr.onRelayAssign(rx.data, rx.length);
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
            // Phase 2: Assign relay based on RSSI before route discovery
            electionMgr.assignRelayByRssi();

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
            electionMgr.assignRelayByRssi();
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
