/**
 * @file HarvestLoop.cpp
 * @brief Gateway harvest state machine implementation
 *
 * Sequentially connects to each discovered node's WiFi AP and
 * downloads all images via the existing CoAP Block2 client.
 * Supports multi-hop harvesting via relay nodes using AODV routes.
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "HarvestLoop.h"
#include "TaskConfig.h"
#include "DeepSleepManager.h"   // rtcBootCount

static const char* TAG = "Harvest";

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Constructor
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

HarvestLoop::HarvestLoop(NodeRegistry& registry, CoapClient& coapClient)
    : _registry(registry)
    , _coapClient(coapClient)
    , _aodvRouter(nullptr)
    , _loraRadio(nullptr)
    , _state(HARVEST_IDLE)
    , _stateEnteredMs(0)
    , _cycleStartMs(0)
    , _globalImageCounter(0)
    , _relayHarvesting(false)
    , _routeDiscRreqSent(false)
    , _relayCmdIdCounter(0)
    , _pendingCmdId(0)
    , _relayAckReceived{false}
    , _nodeBlockedCb(nullptr)
{
    _stats.reset();
    memset(_relaySSID, 0, sizeof(_relaySSID));
}

void HarvestLoop::setAodv(AodvRouter* router, LoRaRadio* radio) {
    _aodvRouter = router;
    _loraRadio  = radio;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Public Interface
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::startCycle() {
    if (_state != HARVEST_IDLE) {
        log_w("%s: Cannot start — already in state %s", TAG, stateStr());
        return;
    }
    _routeDiscRreqSent = false;
    _enterState(HARVEST_START);
}

void HarvestLoop::onHarvestAck(const HarvestAckPacket& ack) {
    if (_state == HARVEST_RELAY_WAIT && ack.cmdId == _pendingCmdId) {
        portENTER_CRITICAL(&_relayAckMux);
        _lastRelayAck = ack;
        _relayAckReceived = true;
        portEXIT_CRITICAL(&_relayAckMux);
        Serial.printf("[%s] Relay ACK received: status=%s, images=%u\n",
                      TAG, HarvestAckPacket::statusToString(ack.status), ack.imageCount);
    }
}

void HarvestLoop::abortCycle() {
    if (_state == HARVEST_IDLE) return;

    Serial.println("[Harvest] Aborting cycle — marking remaining nodes as failed");

    // Mark all remaining unharvested nodes as failed
    if (registryLock()) {
        NodeEntry node;
        while (_registry.getNextToHarvest(node)) {
            _registry.markHarvested(node.nodeId);
            _stats.nodesFailed++;
        }
        registryUnlock();
    }

    _relayHarvesting = false;
    _coapClient.stop();
    _enterState(HARVEST_IDLE);
}

void HarvestLoop::tick() {
    switch (_state) {
        case HARVEST_IDLE:
            break;
        case HARVEST_START:
            _doStart();
            break;
        case HARVEST_ROUTE_DISCOVERY:
            _doRouteDiscovery();
            break;
        case HARVEST_DISCONNECT:
            _doDisconnect();
            break;
        case HARVEST_WAKE_NODE:
            // WAKE_NODE removed (LoRa wake unreliable). Fall through to CONNECT.
            _enterState(HARVEST_CONNECT);
            break;
        case HARVEST_CONNECT:
            _doConnect();
            break;
        case HARVEST_COAP_INIT:
            _doCoapInit();
            break;
        case HARVEST_DOWNLOAD:
            _doDownload();
            break;
        case HARVEST_NEXT:
            _doNext();
            break;
        case HARVEST_RELAY_CMD:
            _doRelayCmd();
            break;
        case HARVEST_RELAY_WAIT:
            _doRelayWait();
            break;
        case HARVEST_DONE:
            _doDone();
            break;
    }
}

const char* HarvestLoop::stateStr() const {
    switch (_state) {
        case HARVEST_IDLE:            return "IDLE";
        case HARVEST_START:           return "START";
        case HARVEST_ROUTE_DISCOVERY: return "ROUTE_DISC";
        case HARVEST_DISCONNECT:      return "DISCONNECT";
        case HARVEST_WAKE_NODE:       return "WAKE_NODE";
        case HARVEST_CONNECT:         return "CONNECT";
        case HARVEST_COAP_INIT:       return "COAP_INIT";
        case HARVEST_DOWNLOAD:        return "DOWNLOAD";
        case HARVEST_NEXT:            return "NEXT";
        case HARVEST_RELAY_CMD:       return "RELAY_CMD";
        case HARVEST_RELAY_WAIT:      return "RELAY_WAIT";
        case HARVEST_DONE:            return "DONE";
        default:                      return "???";
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State Transitions
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_enterState(HarvestState newState) {
    _state = newState;
    _stateEnteredMs = millis();
    log_d("%s: -> %s", TAG, stateStr());
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: START
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doStart() {
    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println("║     HARVEST CYCLE STARTING           ║");
    if (registryLock()) {
        Serial.printf( "║     Nodes registered: %u              ║\n", _registry.activeCount());
        registryUnlock();
    }
    Serial.println("╚══════════════════════════════════════╝\n");

    _stats.reset();
    _cycleStartMs = millis();

    // Check if registry has any nodes before starting
    uint8_t activeNodes = 0;
    if (registryLock()) {
        activeNodes = _registry.activeCount();
        _registry.resetHarvestFlags();
        registryUnlock();
    }
    _relayHarvesting = false;

    if (activeNodes == 0) {
        Serial.printf("[%s] No active nodes in registry — skipping cycle\n", TAG);
        _enterState(HARVEST_DONE);
        return;
    }

    // If AODV is available, do a route discovery first
    if (_aodvRouter) {
        _enterState(HARVEST_ROUTE_DISCOVERY);
    } else {
        _enterState(HARVEST_DISCONNECT);
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: ROUTE_DISCOVERY
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doRouteDiscovery() {
    // Broadcast RREQ once at the start of this state
    if (!_routeDiscRreqSent) {
        Serial.printf("[%s] Broadcasting RREQ for topology discovery...\n", TAG);
        _aodvRouter->discoverAll();
        _routeDiscRreqSent = true;
    }

    // Wait for RREP responses
    if (millis() - _stateEnteredMs >= HARVEST_ROUTE_DISC_WAIT_MS) {
        Serial.printf("[%s] Route discovery period complete (%u routes found)\n",
                      TAG, _aodvRouter->routeCount());
        _routeDiscRreqSent = false;  // Reset for next cycle
        _enterState(HARVEST_DISCONNECT);
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: DISCONNECT
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doDisconnect() {
    _coapClient.stop();

    // Put LoRa into standby to reduce power draw before WiFi activates.
    // This prevents brownout crashes when WiFi + LoRa RX + SD are all active.
    if (_loraRadio) {
        if (xSemaphoreTake(xLoRaMutex, MUTEX_TIMEOUT) == pdTRUE) {
            _loraRadio->standby();
            xSemaphoreGive(xLoRaMutex);
        }
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    vTaskDelay(pdMS_TO_TICKS(500));

    log_d("%s: WiFi disconnected", TAG);

    // Gateway-as-AP: no WAKE_NODE needed. Leaves connect to gateway AP
    // on timer wake and announce themselves. Go directly to CONNECT.
    _enterState(HARVEST_CONNECT);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: CONNECT
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doConnect() {
    // ── Relay phase 2: connect to relay AP for cached images ─
    if (_relayHarvesting) {
        Serial.printf("\n--- Connecting to relay %s for cached images ---\n", _relaySSID);
        WiFi.begin(_relaySSID, HARVEST_WIFI_PASSWORD);

        uint32_t wifiStart = millis();
        while (WiFi.status() != WL_CONNECTED &&
               millis() - wifiStart < HARVEST_WIFI_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(250));
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() != WL_CONNECTED) {
            WiFi.disconnect(true);  // Force cleanup of failed connection attempt
            Serial.printf("[ERROR] WiFi connect to relay %s FAILED\n", _relaySSID);
            if (registryLock()) {
                _registry.markHarvested(_currentNode.nodeId);
                registryUnlock();
            }
            _stats.nodesFailed++;
            _relayHarvesting = false;
            _enterState(HARVEST_DISCONNECT);
            return;
        }

        Serial.printf("[OK] Connected to relay %s (IP: %s)\n",
                      _relaySSID, WiFi.localIP().toString().c_str());
        _enterState(HARVEST_COAP_INIT);
        return;
    }

    // Get next unharvested node (registry is shared with Core 0 beacon updates)
    bool hasNext = false;
    bool isMultiHop = false;
    if (registryLock()) {
        hasNext = _registry.getNextToHarvest(_currentNode);
        if (hasNext && _aodvRouter) {
            isMultiHop = _registry.isMultiHop(_currentNode.nodeId);
        }
        registryUnlock();
    }
    if (!hasNext) {
        log_i("%s: No more nodes to harvest", TAG);
        _enterState(HARVEST_DONE);
        return;
    }

    // Skip blocked nodes
    if (_nodeBlockedCb && _nodeBlockedCb(_currentNode.nodeId)) {
        Serial.printf("[%s] Node %s is BLOCKED — skipping\n", TAG, _currentNode.ssid);
        if (registryLock()) {
            _registry.markHarvested(_currentNode.nodeId);
            registryUnlock();
        }
        _enterState(HARVEST_DISCONNECT);
        return;
    }

    _stats.nodesAttempted++;

    // ── Check if this node needs multi-hop harvesting ────────
    if (isMultiHop) {
        Serial.printf("\n--- Node %s is MULTI-HOP (%u hops) — using relay ---\n",
                      _currentNode.ssid, _currentNode.hopCount);
        _enterState(HARVEST_RELAY_CMD);
        return;
    }

    // ── Announced node: already connected to gateway AP as STA ──
    if (_currentNode.announcedIP[0] != 0) {
        Serial.printf("\n--- Announced node %s (IP=%u.%u.%u.%u, images=%u) ---\n",
                      _currentNode.ssid,
                      _currentNode.announcedIP[0], _currentNode.announcedIP[1],
                      _currentNode.announcedIP[2], _currentNode.announcedIP[3],
                      _currentNode.imageCount);
        _enterState(HARVEST_COAP_INIT);  // Skip WiFi — leaf is on our AP
        return;
    }

    // ── Direct connection to leaf ────────────────────────────
    Serial.printf("\n--- Connecting to %s (RSSI=%.0f, images=%u, hops=%u) ---\n",
                  _currentNode.ssid, _currentNode.rssi,
                  _currentNode.imageCount, _currentNode.hopCount);

    WiFi.begin(_currentNode.ssid, HARVEST_WIFI_PASSWORD);

    uint32_t wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - wifiStart < HARVEST_WIFI_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect(true);  // Force cleanup of failed connection attempt
        Serial.printf("[ERROR] WiFi connect to %s FAILED (timeout)\n", _currentNode.ssid);
        if (registryLock()) {
            _registry.markHarvested(_currentNode.nodeId);
            registryUnlock();
        }
        _stats.nodesFailed++;
        _enterState(HARVEST_DISCONNECT);
        return;
    }

    Serial.printf("[OK] Connected to %s (IP: %s)\n",
                  _currentNode.ssid,
                  WiFi.localIP().toString().c_str());

    _enterState(HARVEST_COAP_INIT);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: RELAY_CMD — Send HARVEST_CMD to relay
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doRelayCmd() {
    if (!_loraRadio || !_aodvRouter) {
        Serial.printf("[%s] No LoRa/AODV — cannot relay, skipping node\n", TAG);
        if (registryLock()) {
            _registry.markHarvested(_currentNode.nodeId);
            registryUnlock();
        }
        _stats.nodesFailed++;
        _enterState(HARVEST_DISCONNECT);
        return;
    }

    // Get the relay node info (nextHop from the route)
    // We need the relay's SSID to connect to it later
    RouteEntry route;
    if (!_aodvRouter->getRoute(_currentNode.nodeId, route)) {
        Serial.printf("[%s] No AODV route to %s — skipping\n", TAG, _currentNode.ssid);
        if (registryLock()) {
            _registry.markHarvested(_currentNode.nodeId);
            registryUnlock();
        }
        _stats.nodesFailed++;
        _enterState(HARVEST_DISCONNECT);
        return;
    }

    // Find the relay in the registry to get its SSID (thread-safe)
    NodeEntry relayEntry;
    bool relayFound = false;
    if (registryLock()) {
        for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
            if (_registry.getNode(i, relayEntry) &&
                memcmp(relayEntry.nodeId, route.nextHopId, 6) == 0) {
                relayFound = true;
                break;
            }
        }
        registryUnlock();
    }

    if (!relayFound) {
        Serial.printf("[%s] Relay node not in registry — skipping\n", TAG);
        if (registryLock()) {
            _registry.markHarvested(_currentNode.nodeId);
            registryUnlock();
        }
        _stats.nodesFailed++;
        _enterState(HARVEST_DISCONNECT);
        return;
    }

    // Save relay SSID for later connection
    strncpy(_relaySSID, relayEntry.ssid, sizeof(_relaySSID) - 1);
    _relaySSID[sizeof(_relaySSID) - 1] = '\0';

    // Validate relay SSID
    if (_relaySSID[0] == '\0') {
        Serial.printf("[%s] Relay SSID is empty — skipping node\n", TAG);
        if (registryLock()) {
            _registry.markHarvested(_currentNode.nodeId);
            registryUnlock();
        }
        _stats.nodesFailed++;
        _enterState(HARVEST_DISCONNECT);
        return;
    }

    // Build HARVEST_CMD
    _relayCmdIdCounter++;
    _pendingCmdId = _relayCmdIdCounter;

    HarvestCmdPacket cmd;
    cmd.cmdId = _pendingCmdId;
    memcpy(cmd.relayId, route.nextHopId, 6);
    memcpy(cmd.targetLeafId, _currentNode.nodeId, 6);
    cmd.ssidLen = strlen(_currentNode.ssid);
    if (cmd.ssidLen > HARVEST_CMD_MAX_SSID) cmd.ssidLen = HARVEST_CMD_MAX_SSID;
    memcpy(cmd.ssid, _currentNode.ssid, cmd.ssidLen);
    cmd.ssid[cmd.ssidLen] = '\0';

    // Send via LoRa
    uint8_t buf[64];
    uint8_t len = cmd.serialize(buf, sizeof(buf));
    if (len > 0) {
        loraSendSafe(buf, len);
        loraStartReceiveSafe();
        Serial.printf("[%s] HARVEST_CMD sent to relay %s for leaf %s (cmdId=%u)\n",
                      TAG, _relaySSID, _currentNode.ssid, _pendingCmdId);
    }

    portENTER_CRITICAL(&_relayAckMux);
    _relayAckReceived = false;
    portEXIT_CRITICAL(&_relayAckMux);
    _relayHarvesting = true;
    _enterState(HARVEST_RELAY_WAIT);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: RELAY_WAIT — Wait for HARVEST_ACK
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doRelayWait() {
    // Check if ACK received (set by onHarvestAck callback on another core)
    HarvestAckPacket localAck;
    bool ackReceived = false;
    portENTER_CRITICAL(&_relayAckMux);
    if (_relayAckReceived) {
        ackReceived = true;
        localAck = _lastRelayAck;
    }
    portEXIT_CRITICAL(&_relayAckMux);

    if (ackReceived) {
        if (localAck.status == HARVEST_STATUS_OK && localAck.imageCount > 0) {
            Serial.printf("[%s] Relay harvested %u images — connecting to relay AP %s\n",
                          TAG, localAck.imageCount, _relaySSID);
            // Now connect to the relay to download the cached images
            _enterState(HARVEST_DISCONNECT);
            return;
        } else {
            Serial.printf("[%s] Relay harvest failed (status=%s) — skipping node\n",
                          TAG, HarvestAckPacket::statusToString(localAck.status));
            if (registryLock()) {
                _registry.markHarvested(_currentNode.nodeId);
                registryUnlock();
            }
            _stats.nodesFailed++;
            _relayHarvesting = false;
            _enterState(HARVEST_DISCONNECT);
            return;
        }
    }

    // Timeout check
    if (millis() - _stateEnteredMs >= HARVEST_RELAY_TIMEOUT_MS) {
        Serial.printf("[%s] Relay ACK TIMEOUT for %s\n", TAG, _currentNode.ssid);
        if (registryLock()) {
            _registry.markHarvested(_currentNode.nodeId);
            registryUnlock();
        }
        _stats.nodesFailed++;
        _relayHarvesting = false;
        _enterState(HARVEST_DISCONNECT);
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: COAP_INIT
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doCoapInit() {
    if (!_coapClient.begin()) {
        Serial.printf("[ERROR] CoAP client init failed for %s\n", _currentNode.ssid);
        if (_relayHarvesting) {
            _relayHarvesting = false;
        }
        if (registryLock()) {
            _registry.markHarvested(_currentNode.nodeId);
            registryUnlock();
        }
        _stats.nodesFailed++;
        _enterState(HARVEST_DISCONNECT);
        return;
    }

    log_d("%s: CoAP client ready on %s", TAG, _currentNode.ssid);
    _enterState(HARVEST_DOWNLOAD);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: DOWNLOAD
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doDownload() {
    // Use announced STA IP if available, otherwise default AP IP
    IPAddress leafIP;
    if (_currentNode.announcedIP[0] != 0) {
        leafIP = IPAddress(_currentNode.announcedIP[0], _currentNode.announcedIP[1],
                           _currentNode.announcedIP[2], _currentNode.announcedIP[3]);
    } else {
        leafIP = IPAddress(192, 168, 4, 1);
    }
    uint16_t  leafPort = COAP_DEFAULT_PORT;

    // ── Fetch /info to get image count ──────────────────
    Serial.printf("=== GET /info from %s ===\n",
                  _relayHarvesting ? _relaySSID : _currentNode.ssid);

    uint8_t infoBuf[513];
    size_t  infoLen = sizeof(infoBuf) - 1;
    CoapClientError err = _coapClient.get(leafIP, leafPort, "info", infoBuf, infoLen);

    uint8_t imageCount = 0;
    if (err == COAP_CLIENT_OK && infoLen > 0) {
        infoBuf[infoLen] = '\0';
        Serial.printf("Response: %s\n", (char*)infoBuf);

        const char* countKey = strstr((char*)infoBuf, "\"count\":");
        if (countKey) {
            int parsed = atoi(countKey + 8);
            if (parsed > 0 && parsed <= 255) {
                imageCount = (uint8_t)parsed;
            } else {
                Serial.printf("[WARN] Invalid image count: %d\n", parsed);
            }
        } else {
            Serial.println("[WARN] /info response missing \"count\" field");
        }
    } else {
        Serial.printf("[ERROR] GET /info failed: %s\n", coapClientErrorStr(err));
    }

    if (imageCount == 0) {
        Serial.printf("[WARN] No images on %s, skipping\n",
                      _relayHarvesting ? _relaySSID : _currentNode.ssid);
        _enterState(HARVEST_NEXT);
        return;
    }

    Serial.printf("Downloading %u image(s)...\n\n", imageCount);

    // ── Build node-specific output prefix ────────────────
    char nodePrefix[16];
    snprintf(nodePrefix, sizeof(nodePrefix), "%02X%02X",
             _currentNode.nodeId[4], _currentNode.nodeId[5]);

    if (!SD.exists(HARVEST_SAVE_DIR)) {
        SD.mkdir(HARVEST_SAVE_DIR);
    }

    // ── Download each image ─────────────────────────────
    bool sdAvailable = SD.exists(HARVEST_SAVE_DIR);
    uint8_t passCount = 0;
    uint8_t failCount = 0;

    for (uint8_t i = 0; i < imageCount; i++) {
        Serial.printf("  === Download /image/%u ===\n", i);

        char outPath[96];
        const char* savePath = nullptr;
        if (sdAvailable) {
            uint32_t uptimeSec = millis() / 1000;
            snprintf(outPath, sizeof(outPath),
                     "%s/node_%s_boot%03lu_%06lus_img_%03u.jpg",
                     HARVEST_SAVE_DIR, nodePrefix,
                     (unsigned long)rtcBootCount,
                     (unsigned long)uptimeSec, i);
            savePath = outPath;
        }

        TransferStats stats;
        err = _coapClient.downloadImagePipelined(leafIP, leafPort, i, savePath, stats);

        if (err == COAP_CLIENT_OK) {
            Serial.printf("    Bytes: %lu | Blocks: %lu | Time: %lu ms | Speed: %.1f KB/s\n",
                          stats.totalBytes, stats.totalBlocks,
                          stats.elapsedMs, stats.throughputKBps);

            if (savePath) {
                Serial.printf("    Saved: %s\n", savePath);
            }

            // Verify checksum (skip for relay-cached images)
            if (!_relayHarvesting) {
                bool match = false;
                err = _coapClient.verifyChecksum(leafIP, leafPort, i,
                                                  stats.computedChecksum, match);
                if (err == COAP_CLIENT_OK && match) {
                    Serial.printf("    Checksum: 0x%04X — PASS\n\n", stats.computedChecksum);
                    passCount++;
                } else {
                    Serial.printf("    Checksum: FAIL\n\n");
                    failCount++;
                }
            } else {
                // Trust relay's download (checksum was verified at relay)
                passCount++;
                Serial.printf("    (via relay — checksum verified at relay)\n\n");
            }

            _stats.totalBytes += stats.totalBytes;
            _stats.totalImages++;
        } else {
            Serial.printf("    [ERROR] Download failed: %s\n\n", coapClientErrorStr(err));
            // Remove partial/corrupt file from SD
            if (savePath && SD.exists(savePath)) {
                SD.remove(savePath);
                Serial.printf("    Removed partial file: %s\n", savePath);
            }
            failCount++;
        }

        _globalImageCounter++;
    }

    Serial.printf("  Node %s: %u/%u images OK\n\n",
                  _currentNode.ssid, passCount, imageCount);

    if (failCount == 0) {
        _stats.nodesSucceeded++;
    } else {
        _stats.nodesFailed++;
    }

    _enterState(HARVEST_NEXT);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: NEXT
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doNext() {
    if (registryLock()) {
        _registry.markHarvested(_currentNode.nodeId);
        registryUnlock();
    }
    _relayHarvesting = false;
    _enterState(HARVEST_DISCONNECT);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: DONE
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_selfCopyImages() {
    File imgDir = SD.open("/images");
    if (!imgDir || !imgDir.isDirectory()) {
        Serial.printf("[%s] Self-copy: /images/ not found — skipping\n", TAG);
        if (imgDir) imgDir.close();
        return;
    }

    if (!SD.exists(HARVEST_SAVE_DIR)) {
        SD.mkdir(HARVEST_SAVE_DIR);
    }

    char nodePrefix[8] = "SELF";

    uint16_t copied = 0;
    File entry;
    while ((entry = imgDir.openNextFile())) {
        if (entry.isDirectory()) { entry.close(); continue; }

        const char* name = entry.name();
        size_t nameLen = strlen(name);
        if (nameLen < 4) { entry.close(); continue; }
        const char* ext = name + nameLen - 4;
        bool isJpg = (strcasecmp(ext, ".jpg") == 0);
        if (!isJpg && nameLen >= 5) {
            isJpg = (strcasecmp(name + nameLen - 5, ".jpeg") == 0);
        }
        if (!isJpg) { entry.close(); continue; }

        // Build output path
        char outPath[96];
        uint32_t uptimeSec = millis() / 1000;
        snprintf(outPath, sizeof(outPath),
                 "%s/node_%s_boot%03lu_%06lus_img_%03u.jpg",
                 HARVEST_SAVE_DIR, nodePrefix,
                 (unsigned long)rtcBootCount,
                 (unsigned long)uptimeSec, copied);

        File dst = SD.open(outPath, FILE_WRITE);
        if (!dst) {
            Serial.printf("[%s] Self-copy: cannot create %s\n", TAG, outPath);
            entry.close();
            continue;
        }

        // Copy in chunks
        uint8_t buf[1024];
        size_t totalBytes = 0;
        bool writeErr = false;
        while (entry.available()) {
            size_t n = entry.read(buf, sizeof(buf));
            if (n == 0) break;
            size_t written = dst.write(buf, n);
            if (written != n) {
                Serial.printf("[%s] Self-copy: write error on %s (%u of %u)\n",
                              TAG, outPath, (unsigned)written, (unsigned)n);
                writeErr = true;
                break;
            }
            totalBytes += written;
        }
        dst.close();
        entry.close();

        if (writeErr) {
            SD.remove(outPath);
            continue;
        }

        Serial.printf("[%s] Self-copy: %s -> %s (%lu bytes)\n",
                      TAG, name, outPath, (unsigned long)totalBytes);
        _stats.totalImages++;
        _stats.totalBytes += totalBytes;
        copied++;
    }
    imgDir.close();

    Serial.printf("[%s] Self-copy complete: %u image(s) copied\n", TAG, copied);
}

void HarvestLoop::_doDone() {
    _stats.totalTimeMs = millis() - _cycleStartMs;

    // Explicit resource cleanup with error logging
    _coapClient.stop();
    WiFi.disconnect(true);
    _relayHarvesting = false;  // Ensure relay state is clean

    // ── Self-copy: gateway's own /images/ → /received/ ──────
    _selfCopyImages();

    // Restart LoRa RX for beacon listening after harvest
    if (_loraRadio) {
        loraStartReceiveSafe();
    }

    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println("║     HARVEST CYCLE COMPLETE           ║");
    Serial.printf( "║     Nodes: %u attempted, %u OK, %u fail ║\n",
                   _stats.nodesAttempted, _stats.nodesSucceeded, _stats.nodesFailed);
    Serial.printf( "║     Images: %lu total                  ║\n", _stats.totalImages);
    Serial.printf( "║     Data:   %lu bytes                  ║\n", _stats.totalBytes);
    Serial.printf( "║     Time:   %lu ms                     ║\n", _stats.totalTimeMs);
    Serial.println("╚══════════════════════════════════════╝\n");

    _enterState(HARVEST_IDLE);
}
