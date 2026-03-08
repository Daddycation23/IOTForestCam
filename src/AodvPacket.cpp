/**
 * @file AodvPacket.cpp
 * @brief AODV packet serialization and parsing
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "AodvPacket.h"

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Helper: write/read uint16 LE
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static inline void writeU16LE(uint8_t* buf, uint16_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
}

static inline uint16_t readU16LE(const uint8_t* buf) {
    return buf[0] | (buf[1] << 8);
}

static inline void writeU32LE(uint8_t* buf, uint32_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
    buf[2] = (val >> 16) & 0xFF;
    buf[3] = (val >> 24) & 0xFF;
}

static inline uint32_t readU32LE(const uint8_t* buf) {
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

static void macToString(const uint8_t mac[6], char* buf, size_t bufLen) {
    snprintf(buf, bufLen, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}


// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// RREQ — Route Request (31 bytes)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

uint8_t RreqPacket::serialize(uint8_t* buf, uint8_t maxLen) const {
    static constexpr uint8_t RREQ_SIZE = 31;
    if (maxLen < RREQ_SIZE) return 0;

    uint8_t pos = 0;
    buf[pos++] = AODV_MAGIC;
    buf[pos++] = AODV_VERSION;
    buf[pos++] = PKT_TYPE_RREQ;
    buf[pos++] = flags;
    buf[pos++] = hopCount;
    writeU32LE(&buf[pos], rreqId);   pos += 4;
    memcpy(&buf[pos], destId, 6);    pos += 6;
    writeU16LE(&buf[pos], destSeqNum); pos += 2;
    memcpy(&buf[pos], origId, 6);    pos += 6;
    writeU16LE(&buf[pos], origSeqNum); pos += 2;
    memcpy(&buf[pos], prevHopId, 6); pos += 6;

    return pos;  // 31
}

bool RreqPacket::parse(const uint8_t* buf, uint8_t len) {
    static constexpr uint8_t RREQ_SIZE = 31;
    if (len < RREQ_SIZE) return false;
    if (buf[0] != AODV_MAGIC || buf[1] != AODV_VERSION) return false;
    if (buf[2] != PKT_TYPE_RREQ) return false;

    uint8_t pos = 3;
    flags    = buf[pos++];
    hopCount = buf[pos++];
    rreqId   = readU32LE(&buf[pos]); pos += 4;
    memcpy(destId, &buf[pos], 6);    pos += 6;
    destSeqNum = readU16LE(&buf[pos]); pos += 2;
    memcpy(origId, &buf[pos], 6);    pos += 6;
    origSeqNum = readU16LE(&buf[pos]); pos += 2;
    memcpy(prevHopId, &buf[pos], 6); pos += 6;

    return true;
}

void RreqPacket::destIdToString(char* buf, size_t bufLen) const {
    macToString(destId, buf, bufLen);
}

void RreqPacket::origIdToString(char* buf, size_t bufLen) const {
    macToString(origId, buf, bufLen);
}


// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// RREP — Route Reply (27 bytes)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

uint8_t RrepPacket::serialize(uint8_t* buf, uint8_t maxLen) const {
    static constexpr uint8_t RREP_SIZE = 27;
    if (maxLen < RREP_SIZE) return 0;

    uint8_t pos = 0;
    buf[pos++] = AODV_MAGIC;
    buf[pos++] = AODV_VERSION;
    buf[pos++] = PKT_TYPE_RREP;
    buf[pos++] = flags;
    buf[pos++] = hopCount;
    memcpy(&buf[pos], destId, 6);      pos += 6;
    writeU16LE(&buf[pos], destSeqNum); pos += 2;
    memcpy(&buf[pos], origId, 6);      pos += 6;
    writeU16LE(&buf[pos], lifetime);   pos += 2;
    memcpy(&buf[pos], prevHopId, 6);   pos += 6;

    return pos;  // 27
}

bool RrepPacket::parse(const uint8_t* buf, uint8_t len) {
    static constexpr uint8_t RREP_SIZE = 27;
    if (len < RREP_SIZE) return false;
    if (buf[0] != AODV_MAGIC || buf[1] != AODV_VERSION) return false;
    if (buf[2] != PKT_TYPE_RREP) return false;

    uint8_t pos = 3;
    flags      = buf[pos++];
    hopCount   = buf[pos++];
    memcpy(destId, &buf[pos], 6);      pos += 6;
    destSeqNum = readU16LE(&buf[pos]); pos += 2;
    memcpy(origId, &buf[pos], 6);      pos += 6;
    lifetime   = readU16LE(&buf[pos]); pos += 2;
    memcpy(prevHopId, &buf[pos], 6);   pos += 6;

    return true;
}


// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// RERR — Route Error (4 + 8*N bytes)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

uint8_t RerrPacket::serialize(uint8_t* buf, uint8_t maxLen) const {
    if (destCount == 0 || destCount > RERR_MAX_DESTS) return 0;
    uint8_t totalLen = 4 + destCount * 8;
    if (maxLen < totalLen) return 0;

    uint8_t pos = 0;
    buf[pos++] = AODV_MAGIC;
    buf[pos++] = AODV_VERSION;
    buf[pos++] = PKT_TYPE_RERR;
    buf[pos++] = destCount;

    for (uint8_t i = 0; i < destCount; i++) {
        memcpy(&buf[pos], entries[i].destId, 6); pos += 6;
        writeU16LE(&buf[pos], entries[i].destSeqNum); pos += 2;
    }

    return pos;
}

bool RerrPacket::parse(const uint8_t* buf, uint8_t len) {
    if (len < 4) return false;
    if (buf[0] != AODV_MAGIC || buf[1] != AODV_VERSION) return false;
    if (buf[2] != PKT_TYPE_RERR) return false;

    destCount = buf[3];
    if (destCount == 0 || destCount > RERR_MAX_DESTS) return false;
    if (len < (uint8_t)(4 + destCount * 8)) return false;

    uint8_t pos = 4;
    for (uint8_t i = 0; i < destCount; i++) {
        memcpy(entries[i].destId, &buf[pos], 6); pos += 6;
        entries[i].destSeqNum = readU16LE(&buf[pos]); pos += 2;
    }

    return true;
}


// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// HARVEST_CMD — Gateway to Relay (17 + ssidLen bytes)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

uint8_t HarvestCmdPacket::serialize(uint8_t* buf, uint8_t maxLen) const {
    if (ssidLen > HARVEST_CMD_MAX_SSID) return 0;
    uint8_t totalLen = 17 + ssidLen;
    if (maxLen < totalLen) return 0;

    uint8_t pos = 0;
    buf[pos++] = AODV_MAGIC;
    buf[pos++] = AODV_VERSION;
    buf[pos++] = PKT_TYPE_HARVEST_CMD;
    buf[pos++] = cmdId;
    memcpy(&buf[pos], relayId, 6);       pos += 6;
    memcpy(&buf[pos], targetLeafId, 6);  pos += 6;
    buf[pos++] = ssidLen;
    memcpy(&buf[pos], ssid, ssidLen);    pos += ssidLen;

    return pos;
}

bool HarvestCmdPacket::parse(const uint8_t* buf, uint8_t len) {
    if (len < 17) return false;
    if (buf[0] != AODV_MAGIC || buf[1] != AODV_VERSION) return false;
    if (buf[2] != PKT_TYPE_HARVEST_CMD) return false;

    uint8_t pos = 3;
    cmdId = buf[pos++];
    memcpy(relayId, &buf[pos], 6);       pos += 6;
    memcpy(targetLeafId, &buf[pos], 6);  pos += 6;
    ssidLen = buf[pos++];

    if (ssidLen > HARVEST_CMD_MAX_SSID) return false;
    if (len < (uint8_t)(17 + ssidLen)) return false;

    memcpy(ssid, &buf[pos], ssidLen);
    ssid[ssidLen] = '\0';

    return true;
}


// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// HARVEST_ACK — Relay to Gateway (16 bytes)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

uint8_t HarvestAckPacket::serialize(uint8_t* buf, uint8_t maxLen) const {
    static constexpr uint8_t ACK_SIZE = 16;
    if (maxLen < ACK_SIZE) return 0;

    uint8_t pos = 0;
    buf[pos++] = AODV_MAGIC;
    buf[pos++] = AODV_VERSION;
    buf[pos++] = PKT_TYPE_HARVEST_ACK;
    buf[pos++] = cmdId;
    memcpy(&buf[pos], relayId, 6);       pos += 6;
    buf[pos++] = status;
    buf[pos++] = imageCount;
    writeU32LE(&buf[pos], totalBytes);   pos += 4;

    return pos;  // 16
}

bool HarvestAckPacket::parse(const uint8_t* buf, uint8_t len) {
    static constexpr uint8_t ACK_SIZE = 16;
    if (len < ACK_SIZE) return false;
    if (buf[0] != AODV_MAGIC || buf[1] != AODV_VERSION) return false;
    if (buf[2] != PKT_TYPE_HARVEST_ACK) return false;

    uint8_t pos = 3;
    cmdId      = buf[pos++];
    memcpy(relayId, &buf[pos], 6);       pos += 6;
    status     = buf[pos++];
    imageCount = buf[pos++];
    totalBytes = readU32LE(&buf[pos]);   pos += 4;

    return true;
}

const char* HarvestAckPacket::statusToString(uint8_t status) {
    switch (status) {
        case HARVEST_STATUS_OK:        return "OK";
        case HARVEST_STATUS_WIFI_FAIL: return "WIFI_FAIL";
        case HARVEST_STATUS_COAP_FAIL: return "COAP_FAIL";
        case HARVEST_STATUS_SD_FAIL:   return "SD_FAIL";
        default:                       return "UNKNOWN";
    }
}
