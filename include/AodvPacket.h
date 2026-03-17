/**
 * @file AodvPacket.h
 * @brief AODV routing protocol packet formats for LoRa control plane
 *
 * Defines RREQ, RREP, RERR, HARVEST_CMD, and HARVEST_ACK packet types
 * that share the same 3-byte header (magic=0xFC, version=0x01, type) as
 * the existing BeaconPacket. All packets fit within the 64-byte LoRa MTU.
 *
 * Wire format uses little-endian for multi-byte integers.
 * prevHopId[6] is overwritten by each forwarder since LoRa has no
 * MAC-layer source address.
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef AODV_PACKET_H
#define AODV_PACKET_H

#include <Arduino.h>
#include <cstring>

// ─── Shared Protocol Constants (same as LoRaBeacon) ────────────
static constexpr uint8_t AODV_MAGIC   = 0xFC;
static constexpr uint8_t AODV_VERSION = 0x01;

// ─── AODV Packet Type Codes ───────────────────────────────────
static constexpr uint8_t PKT_TYPE_RREQ        = 0x10;
static constexpr uint8_t PKT_TYPE_RREP        = 0x11;
static constexpr uint8_t PKT_TYPE_RERR        = 0x12;
static constexpr uint8_t PKT_TYPE_HARVEST_CMD = 0x20;
static constexpr uint8_t PKT_TYPE_HARVEST_ACK = 0x21;

// ─── Election Packet Type Codes ──────────────────────────────
static constexpr uint8_t PKT_TYPE_ELECTION    = 0x30;
static constexpr uint8_t PKT_TYPE_SUPPRESS    = 0x31;
static constexpr uint8_t PKT_TYPE_COORDINATOR = 0x32;
static constexpr uint8_t PKT_TYPE_GW_RECLAIM  = 0x33;

// ─── Wake Protocol Packet Types ──────────────────────────────
static constexpr uint8_t PKT_TYPE_WAKE_PING       = 0x40;  // Triggers DIO1 wakeup (minimal 3-byte packet)
static constexpr uint8_t PKT_TYPE_WAKE_BEACON_REQ = 0x41;  // Request beacon after wake

// ─── RERR limits ──────────────────────────────────────────────
static constexpr uint8_t RERR_MAX_DESTS = 6;   // Max unreachable destinations per RERR

// ─── HARVEST_CMD SSID limit ───────────────────────────────────
static constexpr uint8_t HARVEST_CMD_MAX_SSID = 20;

// ─── RREQ Flags ───────────────────────────────────────────────
static constexpr uint8_t RREQ_FLAG_G = 0x01;   // Gratuitous RREP
static constexpr uint8_t RREQ_FLAG_D = 0x02;   // Destination only

// ─── RREP Flags ───────────────────────────────────────────────
static constexpr uint8_t RREP_FLAG_A = 0x01;   // Acknowledgment required

// ─── Harvest Status Codes ─────────────────────────────────────
static constexpr uint8_t HARVEST_STATUS_OK        = 0x00;
static constexpr uint8_t HARVEST_STATUS_WIFI_FAIL = 0x01;
static constexpr uint8_t HARVEST_STATUS_COAP_FAIL = 0x02;
static constexpr uint8_t HARVEST_STATUS_SD_FAIL   = 0x03;


// ═══════════════════════════════════════════════════════════════
// RREQ — Route Request (31 bytes)
// ═══════════════════════════════════════════════════════════════
//
// Offset  Size  Field
//   0      1    magic (0xFC)
//   1      1    version (0x01)
//   2      1    type (0x10)
//   3      1    flags
//   4      1    hopCount
//   5      4    rreqId (uint32 LE)
//   9      6    destId
//  15      2    destSeqNum (uint16 LE)
//  17      6    origId
//  23      2    origSeqNum (uint16 LE)
//  25      6    prevHopId
//                TOTAL: 31 bytes

struct RreqPacket {
    uint8_t  flags;
    uint8_t  hopCount;
    uint32_t rreqId;
    uint8_t  destId[6];
    uint16_t destSeqNum;
    uint8_t  origId[6];
    uint16_t origSeqNum;
    uint8_t  prevHopId[6];

    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const;
    bool parse(const uint8_t* buf, uint8_t len);

    void destIdToString(char* buf, size_t bufLen) const;
    void origIdToString(char* buf, size_t bufLen) const;
};


// ═══════════════════════════════════════════════════════════════
// RREP — Route Reply (27 bytes)
// ═══════════════════════════════════════════════════════════════
//
// Offset  Size  Field
//   0      1    magic (0xFC)
//   1      1    version (0x01)
//   2      1    type (0x11)
//   3      1    flags
//   4      1    hopCount
//   5      6    destId
//  11      2    destSeqNum (uint16 LE)
//  13      6    origId
//  19      2    lifetime (seconds, uint16 LE)
//  21      6    prevHopId
//                TOTAL: 27 bytes

struct RrepPacket {
    uint8_t  flags;
    uint8_t  hopCount;
    uint8_t  destId[6];
    uint16_t destSeqNum;
    uint8_t  origId[6];
    uint16_t lifetime;      // seconds
    uint8_t  prevHopId[6];

    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const;
    bool parse(const uint8_t* buf, uint8_t len);
};


// ═══════════════════════════════════════════════════════════════
// RERR — Route Error (4 + 8*N bytes, N = destCount)
// ═══════════════════════════════════════════════════════════════
//
// Offset  Size  Field
//   0      1    magic (0xFC)
//   1      1    version (0x01)
//   2      1    type (0x12)
//   3      1    destCount (1..6)
//   4      8*N  entries: destId[6] + destSeqNum[2] each
//                TOTAL: 4 + 8*N bytes (max 52)

struct RerrEntry {
    uint8_t  destId[6];
    uint16_t destSeqNum;
};

struct RerrPacket {
    uint8_t   destCount;
    RerrEntry entries[RERR_MAX_DESTS];

    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const;
    bool parse(const uint8_t* buf, uint8_t len);
};


// ═══════════════════════════════════════════════════════════════
// HARVEST_CMD — Gateway tells relay to fetch from a leaf
// ═══════════════════════════════════════════════════════════════
//
// Offset  Size  Field
//   0      1    magic (0xFC)
//   1      1    version (0x01)
//   2      1    type (0x20)
//   3      1    cmdId (unique ID for ACK correlation)
//   4      6    relayId (intended relay MAC)
//  10      6    targetLeafId (leaf MAC to harvest)
//  16      1    ssidLen
//  17      N    ssid (target leaf SSID, max 20)
//                TOTAL: 17 + N bytes (max 37)

struct HarvestCmdPacket {
    uint8_t cmdId;
    uint8_t relayId[6];
    uint8_t targetLeafId[6];
    uint8_t ssidLen;
    char    ssid[HARVEST_CMD_MAX_SSID + 1];

    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const;
    bool parse(const uint8_t* buf, uint8_t len);
};


// ═══════════════════════════════════════════════════════════════
// HARVEST_ACK — Relay acknowledges harvest completion
// ═══════════════════════════════════════════════════════════════
//
// Offset  Size  Field
//   0      1    magic (0xFC)
//   1      1    version (0x01)
//   2      1    type (0x21)
//   3      1    cmdId (echoed from HARVEST_CMD)
//   4      6    relayId (relay's own MAC)
//  10      1    status (0x00=OK, 0x01=wifi_fail, ...)
//  11      1    imageCount (images cached)
//  12      4    totalBytes (uint32 LE)
//                TOTAL: 16 bytes

struct HarvestAckPacket {
    uint8_t  cmdId;
    uint8_t  relayId[6];
    uint8_t  status;
    uint8_t  imageCount;
    uint32_t totalBytes;

    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const;
    bool parse(const uint8_t* buf, uint8_t len);

    static const char* statusToString(uint8_t status);
};

#endif // AODV_PACKET_H
