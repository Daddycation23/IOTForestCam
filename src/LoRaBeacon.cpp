/**
 * @file LoRaBeacon.cpp
 * @brief LoRa beacon packet serialization and parsing
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "LoRaBeacon.h"

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Serialize
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

uint8_t BeaconPacket::serialize(uint8_t* buf, uint8_t maxLen) const {
    // Calculate total packet size
    uint8_t totalLen = BEACON_MIN_SIZE + ssidLen;

    if (totalLen > maxLen || ssidLen > BEACON_MAX_SSID) {
        return 0;
    }

    uint8_t pos = 0;

    // Fixed header
    buf[pos++] = BEACON_MAGIC;          // offset 0
    buf[pos++] = BEACON_VERSION;        // offset 1
    buf[pos++] = packetType;            // offset 2
    buf[pos++] = ttl;                   // offset 3

    // Node ID (6 bytes MAC)
    memcpy(&buf[pos], nodeId, 6);       // offset 4-9
    pos += 6;

    // Node role
    buf[pos++] = nodeRole;              // offset 10

    // SSID (length-prefixed variable)
    buf[pos++] = ssidLen;               // offset 11
    memcpy(&buf[pos], ssid, ssidLen);   // offset 12 .. 12+ssidLen-1
    pos += ssidLen;

    // Payload fields
    buf[pos++] = imageCount;            // offset 12+N
    buf[pos++] = batteryPct;            // offset 13+N

    // Uptime (uint16 little-endian)
    buf[pos++] = uptimeMin & 0xFF;      // offset 14+N (low byte)
    buf[pos++] = (uptimeMin >> 8) & 0xFF; // offset 15+N (high byte)

    return pos;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Parse
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool BeaconPacket::parse(const uint8_t* buf, uint8_t len) {
    // Minimum check: header (12 bytes) + at least 0 SSID + 4 payload bytes
    if (len < BEACON_MIN_SIZE) return false;

    uint8_t pos = 0;

    // Validate magic and version
    if (buf[pos++] != BEACON_MAGIC)   return false;
    if (buf[pos++] != BEACON_VERSION) return false;

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

    // SSID length
    ssidLen = buf[pos++];
    if (ssidLen > BEACON_MAX_SSID) return false;

    // Check remaining bytes: ssidLen + 4 payload bytes
    if (pos + ssidLen + 4 > len) return false;

    // SSID
    memcpy(ssid, &buf[pos], ssidLen);
    ssid[ssidLen] = '\0';      // Null-terminate for convenience
    pos += ssidLen;

    // Payload fields
    imageCount = buf[pos++];
    batteryPct = buf[pos++];

    // Uptime (uint16 little-endian)
    uptimeMin = buf[pos] | (buf[pos + 1] << 8);
    pos += 2;

    return true;
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
