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
    , _relayCmdIdCounter(0)
    , _pendingCmdId(0)
    , _relayAckReceived(false)
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
    _enterState(HARVEST_START);
}

void HarvestLoop::onHarvestAck(const HarvestAckPacket& ack) {
    if (_state == HARVEST_RELAY_WAIT && ack.cmdId == _pendingCmdId) {
        _lastRelayAck = ack;
        _relayAckReceived = true;
        Serial.printf("[%s] Relay ACK received: status=%s, images=%u\n",
                      TAG, HarvestAckPacket::statusToString(ack.status), ack.imageCount);
    }
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
    log_d("%s: %s -> %s", TAG, stateStr(),
          [&]() { _state = newState; return stateStr(); }());
    _stateEnteredMs = millis();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: START
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doStart() {
    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println("║     HARVEST CYCLE STARTING           ║");
    Serial.printf( "║     Nodes registered: %u              ║\n", _registry.activeCount());
    Serial.println("╚══════════════════════════════════════╝\n");

    _stats.reset();
    _cycleStartMs = millis();
    _registry.resetHarvestFlags();
    _relayHarvesting = false;

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
    static bool rreqSent = false;
    if (!rreqSent) {
        Serial.printf("[%s] Broadcasting RREQ for topology discovery...\n", TAG);
        _aodvRouter->discoverAll();
        rreqSent = true;
    }

    // Wait for RREP responses
    if (millis() - _stateEnteredMs >= HARVEST_ROUTE_DISC_WAIT_MS) {
        Serial.printf("[%s] Route discovery period complete (%u routes found)\n",
                      TAG, _aodvRouter->routeCount());
        rreqSent = false;  // Reset for next cycle
        _enterState(HARVEST_DISCONNECT);
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: DISCONNECT
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doDisconnect() {
    _coapClient.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_STA);
    delay(500);

    log_d("%s: WiFi disconnected", TAG);
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
            delay(250);
            Serial.print(".");
        }
        Serial.println();

        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("[ERROR] WiFi connect to relay %s FAILED\n", _relaySSID);
            _registry.markHarvested(_currentNode.nodeId);
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

    // Get next unharvested node
    if (!_registry.getNextToHarvest(_currentNode)) {
        log_i("%s: No more nodes to harvest", TAG);
        _enterState(HARVEST_DONE);
        return;
    }

    _stats.nodesAttempted++;

    // ── Check if this node needs multi-hop harvesting ────────
    if (_aodvRouter && _registry.isMultiHop(_currentNode.nodeId)) {
        Serial.printf("\n--- Node %s is MULTI-HOP (%u hops) — using relay ---\n",
                      _currentNode.ssid, _currentNode.hopCount);
        _enterState(HARVEST_RELAY_CMD);
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
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[ERROR] WiFi connect to %s FAILED (timeout)\n", _currentNode.ssid);
        _registry.markHarvested(_currentNode.nodeId);
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
        _registry.markHarvested(_currentNode.nodeId);
        _stats.nodesFailed++;
        _enterState(HARVEST_DISCONNECT);
        return;
    }

    // Get the relay node info (nextHop from the route)
    // We need the relay's SSID to connect to it later
    RouteEntry route;
    if (!_aodvRouter->getRoute(_currentNode.nodeId, route)) {
        Serial.printf("[%s] No AODV route to %s — skipping\n", TAG, _currentNode.ssid);
        _registry.markHarvested(_currentNode.nodeId);
        _stats.nodesFailed++;
        _enterState(HARVEST_DISCONNECT);
        return;
    }

    // Find the relay in the registry to get its SSID
    NodeEntry relayEntry;
    bool relayFound = false;
    for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
        if (_registry.getNode(i, relayEntry) &&
            memcmp(relayEntry.nodeId, route.nextHopId, 6) == 0) {
            relayFound = true;
            break;
        }
    }

    if (!relayFound) {
        Serial.printf("[%s] Relay node not in registry — skipping\n", TAG);
        _registry.markHarvested(_currentNode.nodeId);
        _stats.nodesFailed++;
        _enterState(HARVEST_DISCONNECT);
        return;
    }

    // Save relay SSID for later connection
    strncpy(_relaySSID, relayEntry.ssid, sizeof(_relaySSID) - 1);
    _relaySSID[sizeof(_relaySSID) - 1] = '\0';

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
        _loraRadio->send(buf, len);
        _loraRadio->startReceive();
        Serial.printf("[%s] HARVEST_CMD sent to relay %s for leaf %s (cmdId=%u)\n",
                      TAG, _relaySSID, _currentNode.ssid, _pendingCmdId);
    }

    _relayAckReceived = false;
    _relayHarvesting = true;
    _enterState(HARVEST_RELAY_WAIT);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: RELAY_WAIT — Wait for HARVEST_ACK
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doRelayWait() {
    // Check if ACK received (set by onHarvestAck callback)
    if (_relayAckReceived) {
        if (_lastRelayAck.status == HARVEST_STATUS_OK && _lastRelayAck.imageCount > 0) {
            Serial.printf("[%s] Relay harvested %u images — connecting to relay AP %s\n",
                          TAG, _lastRelayAck.imageCount, _relaySSID);
            // Now connect to the relay to download the cached images
            _enterState(HARVEST_DISCONNECT);
            return;
        } else {
            Serial.printf("[%s] Relay harvest failed (status=%s) — skipping node\n",
                          TAG, HarvestAckPacket::statusToString(_lastRelayAck.status));
            _registry.markHarvested(_currentNode.nodeId);
            _stats.nodesFailed++;
            _relayHarvesting = false;
            _enterState(HARVEST_DISCONNECT);
            return;
        }
    }

    // Timeout check
    if (millis() - _stateEnteredMs >= HARVEST_RELAY_TIMEOUT_MS) {
        Serial.printf("[%s] Relay ACK TIMEOUT for %s\n", TAG, _currentNode.ssid);
        _registry.markHarvested(_currentNode.nodeId);
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
        _registry.markHarvested(_currentNode.nodeId);
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
    IPAddress leafIP(192, 168, 4, 1);
    uint16_t  leafPort = COAP_DEFAULT_PORT;

    // ── Fetch /info to get image count ──────────────────
    Serial.printf("=== GET /info from %s ===\n",
                  _relayHarvesting ? _relaySSID : _currentNode.ssid);

    uint8_t infoBuf[512];
    size_t  infoLen = sizeof(infoBuf);
    CoapClientError err = _coapClient.get(leafIP, leafPort, "info", infoBuf, infoLen);

    uint8_t imageCount = 0;
    if (err == COAP_CLIENT_OK) {
        infoBuf[infoLen] = '\0';
        Serial.printf("Response: %s\n", (char*)infoBuf);

        const char* countKey = strstr((char*)infoBuf, "\"count\":");
        if (countKey) {
            imageCount = (uint8_t)atoi(countKey + 8);
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

        char outPath[64];
        const char* savePath = nullptr;
        if (sdAvailable) {
            snprintf(outPath, sizeof(outPath), "%s/node_%s_img_%03u.jpg",
                     HARVEST_SAVE_DIR, nodePrefix, i);
            savePath = outPath;
        }

        TransferStats stats;
        err = _coapClient.downloadImage(leafIP, leafPort, i, savePath, stats);

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
    _registry.markHarvested(_currentNode.nodeId);
    _relayHarvesting = false;
    _enterState(HARVEST_DISCONNECT);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// State: DONE
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void HarvestLoop::_doDone() {
    _stats.totalTimeMs = millis() - _cycleStartMs;

    _coapClient.stop();
    WiFi.disconnect(true);

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
