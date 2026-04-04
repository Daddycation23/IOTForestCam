/**
 * @file NodeRegistry.cpp
 * @brief Gateway-side node registry implementation
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "NodeRegistry.h"

static const char* TAG = "NodeRegistry";

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Constructor
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

NodeRegistry::NodeRegistry() {
    for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
        _nodes[i].active     = false;
        _nodes[i].harvested  = false;
        _nodes[i].hopCount   = 1;
        _nodes[i].routeKnown = false;
        memset(_nodes[i].nextHopId, 0, 6);
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Update / Insert
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool NodeRegistry::update(const BeaconPacket& beacon, float rssi) {
    int8_t idx = _findNode(beacon.nodeId);

    if (idx >= 0) {
        // ── Existing node: refresh fields ────────────────────
        NodeEntry& node = _nodes[idx];
        strncpy(node.ssid, beacon.ssid, BEACON_MAX_SSID);
        node.ssid[BEACON_MAX_SSID] = '\0';
        node.imageCount = beacon.imageCount;
        node.nodeRole   = beacon.nodeRole;
        node.lastSeenMs = millis();

        // Keep the strongest RSSI (most optimistic)
        if (rssi > node.rssi) {
            node.rssi = rssi;
        }

        log_d("%s: Updated node %s (RSSI=%.0f, images=%u)",
              TAG, node.ssid, node.rssi, node.imageCount);
        return false;  // Existing refresh — not a new node
    }

    // ── New node: find a slot ────────────────────────────
    idx = _findEmptySlot();

    if (idx < 0) {
        // Registry full — replace the weakest node
        idx = _findWeakestSlot();
        if (idx < 0) return false;  // Should never happen

        log_w("%s: Registry full — replacing weakest node in slot %d", TAG, idx);
    }

    // ── Populate the slot ────────────────────────────────
    NodeEntry& node = _nodes[idx];
    memcpy(node.nodeId, beacon.nodeId, 6);
    node.nodeRole   = beacon.nodeRole;
    strncpy(node.ssid, beacon.ssid, BEACON_MAX_SSID);
    node.ssid[BEACON_MAX_SSID] = '\0';
    node.imageCount = beacon.imageCount;
    node.rssi       = rssi;
    node.lastSeenMs = millis();
    node.harvested  = false;
    node.active     = true;
    memset(node.announcedIP, 0, 4);  // No announce yet — beacon-discovered
    node.hopCount   = 1;       // Assume direct until AODV says otherwise
    node.routeKnown = false;
    memset(node.nextHopId, 0, 6);

    char idStr[24];
    beacon.nodeIdToString(idStr, sizeof(idStr));
    log_i("%s: NEW node [%d] %s (%s) — %s, %u images, RSSI=%.0f",
          TAG, idx, node.ssid, idStr,
          BeaconPacket::roleToString(node.nodeRole),
          node.imageCount, node.rssi);

    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Update from Announce (leaf-initiated harvest)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool NodeRegistry::updateFromAnnounce(const uint8_t nodeId[6], const uint8_t ip[4],
                                       uint8_t imageCount)
{
    int8_t idx = _findNode(nodeId);

    if (idx >= 0) {
        // Existing node: update IP and mark active
        NodeEntry& node = _nodes[idx];
        memcpy(node.announcedIP, ip, 4);
        node.imageCount = imageCount;
        node.lastSeenMs = millis();
        node.active     = true;
        node.harvested  = false;  // Reset for this cycle
        log_i("%s: Announce update [%d] %s — IP=%u.%u.%u.%u, %u images",
              TAG, idx, node.ssid, ip[0], ip[1], ip[2], ip[3], imageCount);
        return false;  // Not new
    }

    // New node from announce
    idx = _findEmptySlot();
    if (idx < 0) {
        idx = _findWeakestSlot();
        if (idx < 0) return false;
    }

    NodeEntry& node = _nodes[idx];
    memcpy(node.nodeId, nodeId, 6);
    node.nodeRole   = NODE_ROLE_LEAF;
    snprintf(node.ssid, sizeof(node.ssid), "ForestCam-%02X%02X", nodeId[4], nodeId[5]);
    node.imageCount = imageCount;
    node.rssi       = 0;
    node.lastSeenMs = millis();
    node.harvested  = false;
    node.active     = true;
    memcpy(node.announcedIP, ip, 4);
    node.hopCount   = 1;
    node.routeKnown = false;
    memset(node.nextHopId, 0, 6);

    log_i("%s: Announce NEW [%d] %s — IP=%u.%u.%u.%u, %u images",
          TAG, idx, node.ssid, ip[0], ip[1], ip[2], ip[3], imageCount);
    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Expiry
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void NodeRegistry::expireStale() {
    uint32_t now = millis();

    for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
        if (_nodes[i].active && (now - _nodes[i].lastSeenMs) > REGISTRY_EXPIRY_MS) {
            log_i("%s: Node %s expired (no beacon for %lu ms)",
                  TAG, _nodes[i].ssid, now - _nodes[i].lastSeenMs);
            _nodes[i].active = false;
        }
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Harvest Control
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void NodeRegistry::resetHarvestFlags() {
    for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
        _nodes[i].harvested = false;
    }
}

void NodeRegistry::reset() {
    for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
        _nodes[i].active     = false;
        _nodes[i].harvested  = false;
        _nodes[i].routeKnown = false;
        _nodes[i].hopCount   = 0;
        _nodes[i].imageCount = 0;
        _nodes[i].rssi       = -999.0f;
        _nodes[i].lastSeenMs = 0;
        memset(_nodes[i].nodeId, 0, 6);
        memset(_nodes[i].nextHopId, 0, 6);
        memset(_nodes[i].ssid, 0, sizeof(_nodes[i].ssid));
    }
    Serial.println("[Registry] Reset — all slots cleared");
}

bool NodeRegistry::getNextToHarvest(NodeEntry& entry) {
    int8_t bestIdx = -1;
    float  bestRSSI = -999.0f;

    for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
        if (_nodes[i].active && !_nodes[i].harvested) {
            if (_nodes[i].rssi > bestRSSI) {
                bestRSSI = _nodes[i].rssi;
                bestIdx  = i;
            }
        }
    }

    if (bestIdx < 0) return false;

    entry = _nodes[bestIdx];
    return true;
}

bool NodeRegistry::getStrongestLeaf(NodeEntry& entry) const {
    int8_t bestIdx = -1;
    float  bestRSSI = -999.0f;

    for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
        if (!_nodes[i].active) continue;
        if (_nodes[i].nodeRole != NODE_ROLE_LEAF) continue;

        if (_nodes[i].rssi > bestRSSI) {
            bestRSSI = _nodes[i].rssi;
            bestIdx  = i;
        } else if (_nodes[i].rssi == bestRSSI && bestIdx >= 0) {
            // Tiebreak: higher MAC value wins
            if (memcmp(_nodes[i].nodeId, _nodes[bestIdx].nodeId, 6) > 0) {
                bestIdx = i;
            }
        }
    }

    if (bestIdx < 0) return false;
    entry = _nodes[bestIdx];
    return true;
}

void NodeRegistry::markHarvested(const uint8_t nodeId[6]) {
    int8_t idx = _findNode(nodeId);
    if (idx >= 0) {
        _nodes[idx].harvested = true;
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Accessors
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

uint8_t NodeRegistry::activeCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
        if (_nodes[i].active) count++;
    }
    return count;
}

bool NodeRegistry::getNode(uint8_t index, NodeEntry& entry) const {
    if (index >= REGISTRY_MAX_NODES) return false;
    if (!_nodes[index].active) return false;
    entry = _nodes[index];
    return true;
}

void NodeRegistry::dump() const {
    Serial.println("┌──────────────────────────────────────────────────────────────────────────────┐");
    Serial.println("│  Node Registry                                                               │");
    Serial.println("├────┬──────────────────┬───────┬───────┬────────┬────────┬───────┬──────┬─────┤");
    Serial.println("│ #  │ SSID             │ Role  │ Imgs  │ RSSI   │ Age(s) │ Harv? │ Hops │ Rte │");
    Serial.println("├────┼──────────────────┼───────┼───────┼────────┼────────┼───────┼──────┼─────┤");

    uint32_t now = millis();
    for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
        if (!_nodes[i].active) continue;

        uint32_t ageSec = (now - _nodes[i].lastSeenMs) / 1000;
        Serial.printf("│ %u  │ %-16s │ %-5s │  %3u  │ %5.0f  │  %4lu  │  %s  │  %2u  │  %s │\n",
                       i,
                       _nodes[i].ssid,
                       BeaconPacket::roleToString(_nodes[i].nodeRole),
                       _nodes[i].imageCount,
                       _nodes[i].rssi,
                       ageSec,
                       _nodes[i].harvested ? "YES" : " NO",
                       _nodes[i].hopCount,
                       _nodes[i].routeKnown ? "Y" : "N");
    }

    Serial.println("└────┴──────────────────┴───────┴───────┴────────┴────────┴───────┴──────┴─────┘");
    Serial.printf("Active nodes: %u\n\n", activeCount());
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// AODV Route Integration
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool NodeRegistry::updateFromRoute(const uint8_t nodeId[6],
                                    const uint8_t nextHopId[6],
                                    uint8_t hopCount) {
    int8_t idx = _findNode(nodeId);
    if (idx < 0) return false;

    NodeEntry& node = _nodes[idx];
    node.hopCount   = hopCount;
    node.routeKnown = true;
    memcpy(node.nextHopId, nextHopId, 6);

    log_i("%s: Route updated for %s — %u hop(s), route known",
          TAG, node.ssid, hopCount);

    return true;
}

bool NodeRegistry::isMultiHop(const uint8_t nodeId[6]) const {
    int8_t idx = _findNode(nodeId);
    if (idx < 0) return false;
    return _nodes[idx].routeKnown && _nodes[idx].hopCount > 1;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Private Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

int8_t NodeRegistry::_findNode(const uint8_t nodeId[6]) const {
    for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
        if (_nodes[i].active && memcmp(_nodes[i].nodeId, nodeId, 6) == 0) {
            return i;
        }
    }
    return -1;
}

int8_t NodeRegistry::_findEmptySlot() const {
    for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
        if (!_nodes[i].active) return i;
    }
    return -1;
}

int8_t NodeRegistry::_findWeakestSlot() const {
    int8_t weakestIdx = -1;
    float  weakestRSSI = 0.0f;

    for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
        if (_nodes[i].active) {
            if (weakestIdx < 0 || _nodes[i].rssi < weakestRSSI) {
                weakestRSSI = _nodes[i].rssi;
                weakestIdx  = i;
            }
        }
    }

    return weakestIdx;
}
