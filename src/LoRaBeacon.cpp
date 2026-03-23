/**
 * @file LoRaBeacon.cpp
 * @brief LoRa beacon packet serialization and parsing (v2: no SSID on wire)
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "LoRaBeacon.h"

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Serialize (v2 — fixed 15 bytes, no SSID)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

uint8_t BeaconPacket::serialize(uint8_t* buf, uint8_t maxLen) const {
    if (maxLen < BEACON_MIN_SIZE) return 0;

    uint8_t pos = 0;

    // Fixed header
    buf[pos++] = BEACON_MAGIC;          // offset 0
    buf[pos++] = BEACON_VERSION;        // offset 1 (0x02 = v2)
    buf[pos++] = packetType;            // offset 2
    buf[pos++] = ttl;                   // offset 3

    // Node ID (6 bytes MAC)
    memcpy(&buf[pos], nodeId, 6);       // offset 4-9
    pos += 6;

    // Node role
    buf[pos++] = nodeRole;              // offset 10

    // Payload (no SSID in v2)
    buf[pos++] = imageCount;            // offset 11
    buf[pos++] = batteryPct;            // offset 12

    // Uptime (uint16 little-endian)
    buf[pos++] = uptimeMin & 0xFF;      // offset 13 (low byte)
    buf[pos++] = (uptimeMin >> 8) & 0xFF; // offset 14 (high byte)

    return pos;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Parse (supports both v1 with SSID and v2 without)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool BeaconPacket::parse(const uint8_t* buf, uint8_t len) {
    // Minimum check: need at least magic + version + type + ttl + nodeId(6) + role = 11 bytes
    if (len < 11) return false;

    uint8_t pos = 0;

    // Validate magic
    if (buf[pos++] != BEACON_MAGIC) return false;

    // Version (0x01 = v1 with SSID, 0x02 = v2 without)
    uint8_t version = buf[pos++];
    if (version != 0x01 && version != BEACON_VERSION) return false;

    // Packet type
    packetType = buf[pos++];
    if (packetType != BEACON_TYPE_BEACON &&
        packetType != BEACON_TYPE_BEACON_RELAY) {
        return false;
    }

    // TTL
    ttl = buf[pos++];

    // Node ID (6 bytes)
    memcpy(nodeId, &buf[pos], 6);
    pos += 6;

    // Node role
    nodeRole = buf[pos++];

    if (version == 0x01) {
        // v1: SSID on wire (backward compat)
        if (pos >= len) return false;
        ssidLen = buf[pos++];
        if (ssidLen > BEACON_MAX_SSID) return false;
        if (pos + ssidLen + 4 > len) return false;

        memcpy(ssid, &buf[pos], ssidLen);
        ssid[ssidLen] = '\0';
        pos += ssidLen;

        imageCount = buf[pos++];
        batteryPct = buf[pos++];
        uptimeMin = buf[pos] | (buf[pos + 1] << 8);
    } else {
        // v2: no SSID — derive from MAC
        if (pos + 4 > len) return false;

        imageCount = buf[pos++];
        batteryPct = buf[pos++];
        uptimeMin = buf[pos] | (buf[pos + 1] << 8);

        // Derive SSID from MAC
        deriveSsid();
    }

    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Derive SSID from MAC
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void BeaconPacket::deriveSsid() {
    macToSsid(nodeId, nodeRole, ssid, sizeof(ssid));
    ssidLen = strlen(ssid);
}

void BeaconPacket::macToSsid(const uint8_t mac[6], uint8_t role, char* out, size_t outLen) {
    if (role == NODE_ROLE_GATEWAY) {
        snprintf(out, outLen, "ForestCam-GW-%02X%02X", mac[4], mac[5]);
    } else {
        snprintf(out, outLen, "ForestCam-%02X%02X", mac[4], mac[5]);
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Utility
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void BeaconPacket::nodeIdToString(char* buf, size_t bufLen) const {
    snprintf(buf, bufLen, "%02X:%02X:%02X:%02X:%02X:%02X",
             nodeId[0], nodeId[1], nodeId[2],
             nodeId[3], nodeId[4], nodeId[5]);
}

bool BeaconPacket::sameNode(const BeaconPacket& other) const {
    return memcmp(nodeId, other.nodeId, 6) == 0;
}

const char* BeaconPacket::roleToString(uint8_t role) {
    switch (role) {
        case NODE_ROLE_LEAF:    return "LEAF";
        case NODE_ROLE_RELAY:   return "RELAY";
        case NODE_ROLE_GATEWAY: return "GATEWAY";
        default:                return "UNKNOWN";
    }
}
