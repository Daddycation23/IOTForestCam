/**
 * @file ElectionPacket.cpp
 * @brief Serialize/parse for election protocol packets
 */

#include "ElectionPacket.h"

// ─── Helpers (same pattern as AodvPacket.cpp) ────────────────
static void writeU16LE(uint8_t* buf, uint16_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
}
static uint16_t readU16LE(const uint8_t* buf) {
    return buf[0] | (buf[1] << 8);
}

uint8_t ElectionPacket::serialize(uint8_t* buf, uint8_t maxLen) const {
    if (maxLen < ELECTION_PACKET_SIZE) return 0;

    uint8_t pos = 0;
    buf[pos++] = AODV_MAGIC;       // 0xFC
    buf[pos++] = AODV_VERSION;     // 0x01
    buf[pos++] = type;             // 0x30-0x33
    memcpy(&buf[pos], senderId, 6);
    pos += 6;
    writeU16LE(&buf[pos], electionId);
    pos += 2;
    return pos;  // 11
}

bool ElectionPacket::parse(const uint8_t* buf, uint8_t len) {
    if (len < ELECTION_PACKET_SIZE) return false;
    if (buf[0] != AODV_MAGIC || buf[1] != AODV_VERSION) return false;

    uint8_t t = buf[2];
    if (t != PKT_TYPE_ELECTION && t != PKT_TYPE_SUPPRESS &&
        t != PKT_TYPE_COORDINATOR && t != PKT_TYPE_GW_RECLAIM) return false;

    uint8_t pos = 2;
    type = buf[pos++];
    memcpy(senderId, &buf[pos], 6);
    pos += 6;
    electionId = readU16LE(&buf[pos]);
    pos += 2;
    return true;
}

uint32_t ElectionPacket::macToPriority(const uint8_t mac[6]) {
    return mac[2] | (mac[3] << 8) | (mac[4] << 16) | (mac[5] << 24);
}

void ElectionPacket::senderIdToString(char* buf, size_t bufLen) const {
    snprintf(buf, bufLen, "%02X:%02X:%02X:%02X:%02X:%02X",
             senderId[0], senderId[1], senderId[2],
             senderId[3], senderId[4], senderId[5]);
}

// ─── RelayAssignPacket ──────────────────────────────────────────

uint8_t RelayAssignPacket::serialize(uint8_t* buf, uint8_t maxLen) const {
    if (maxLen < RELAY_ASSIGN_PACKET_SIZE) return 0;
    uint8_t pos = 0;
    buf[pos++] = AODV_MAGIC;
    buf[pos++] = AODV_VERSION;
    buf[pos++] = PKT_TYPE_RELAY_ASSIGN;
    memcpy(&buf[pos], gatewayId, 6); pos += 6;
    memcpy(&buf[pos], relayId, 6);   pos += 6;
    return pos;  // 15
}

bool RelayAssignPacket::parse(const uint8_t* buf, uint8_t len) {
    if (len < RELAY_ASSIGN_PACKET_SIZE) return false;
    if (buf[0] != AODV_MAGIC || buf[1] != AODV_VERSION) return false;
    if (buf[2] != PKT_TYPE_RELAY_ASSIGN) return false;
    uint8_t pos = 3;
    memcpy(gatewayId, &buf[pos], 6); pos += 6;
    memcpy(relayId,   &buf[pos], 6); pos += 6;
    return true;
}

void RelayAssignPacket::relayIdToString(char* buf, size_t bufLen) const {
    snprintf(buf, bufLen, "%02X:%02X:%02X:%02X:%02X:%02X",
             relayId[0], relayId[1], relayId[2],
             relayId[3], relayId[4], relayId[5]);
}
