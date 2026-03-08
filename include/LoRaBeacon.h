/**
 * @file LoRaBeacon.h
 * @brief LoRa beacon packet format for ForestCam node discovery
 *
 * Defines a compact binary beacon packet (~31 bytes) that leaf and relay
 * nodes broadcast over LoRa. The gateway listens for these beacons to
 * discover available nodes and their WiFi SSIDs for image harvesting.
 *
 * Packet Wire Format:
 *   Offset  Size  Field
 *     0       1   magic (0xFC)
 *     1       1   version (0x01)
 *     2       1   packetType (BEACON=0x01, RELAYED=0x02)
 *     3       1   ttl (starts at 2, decremented by relay)
 *     4       6   nodeId (WiFi MAC address)
 *    10       1   nodeRole (LEAF=0x01, RELAY=0x02)
 *    11       1   ssidLen (length of SSID, max 20)
 *    12       N   ssid (variable length, NOT null-terminated on wire)
 *    12+N     1   imageCount
 *    13+N     1   batteryPct (0-100 or 0xFF=USB/unknown)
 *    14+N     2   uptimeMin (uint16 little-endian)
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef LORA_BEACON_H
#define LORA_BEACON_H

#include <Arduino.h>
#include <cstring>

// ─── Protocol Constants ──────────────────────────────────────
static constexpr uint8_t BEACON_MAGIC    = 0xFC;
static constexpr uint8_t BEACON_VERSION  = 0x01;
static constexpr uint8_t BEACON_MAX_SIZE = 48;
static constexpr uint8_t BEACON_MAX_SSID = 20;
static constexpr uint8_t BEACON_MIN_SIZE = 16;     // Fixed fields without SSID

// ─── Packet Types ────────────────────────────────────────────
enum BeaconPacketType : uint8_t {
    BEACON_TYPE_BEACON       = 0x01,    ///< Direct beacon from leaf/relay
    BEACON_TYPE_BEACON_RELAY = 0x02,    ///< Relayed beacon (TTL decremented)
};

// ─── Node Roles ──────────────────────────────────────────────
enum NodeRole : uint8_t {
    NODE_ROLE_LEAF    = 0x01,
    NODE_ROLE_RELAY   = 0x02,
    NODE_ROLE_GATEWAY = 0x03,
};

// ─── Beacon Packet ───────────────────────────────────────────

/**
 * @brief Parsed representation of a ForestCam LoRa beacon.
 */
struct BeaconPacket {
    uint8_t  packetType;                    ///< BeaconPacketType
    uint8_t  ttl;                           ///< Time-to-live (decremented by relays)
    uint8_t  nodeId[6];                     ///< MAC address (globally unique)
    uint8_t  nodeRole;                      ///< NodeRole enum
    char     ssid[BEACON_MAX_SSID + 1];     ///< Null-terminated for convenience
    uint8_t  ssidLen;                       ///< Actual SSID length on wire
    uint8_t  imageCount;                    ///< Number of images available
    uint8_t  batteryPct;                    ///< Battery % (0-100) or 0xFF=USB
    uint16_t uptimeMin;                     ///< Uptime in minutes

    /**
     * Serialize to a byte buffer for LoRa transmission.
     * @param[out] buf     Output buffer (must be >= BEACON_MAX_SIZE).
     * @param      maxLen  Buffer capacity.
     * @return Bytes written, or 0 on error.
     */
    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const;

    /**
     * Parse from a received LoRa packet.
     * @param buf  Input buffer.
     * @param len  Number of bytes received.
     * @return true if packet is a valid ForestCam beacon.
     */
    bool parse(const uint8_t* buf, uint8_t len);

    /**
     * Format nodeId as "AA:BB:CC:DD:EE:FF" for logging.
     * @param[out] buf     Output buffer (must be >= 18 bytes).
     * @param      bufLen  Buffer capacity.
     */
    void nodeIdToString(char* buf, size_t bufLen) const;

    /**
     * Check if two beacons are from the same node (compare MAC).
     */
    bool sameNode(const BeaconPacket& other) const;

    /**
     * Get human-readable role name.
     */
    static const char* roleToString(uint8_t role);
};

#endif // LORA_BEACON_H
