/**
 * @file LoRaBeacon.h
 * @brief LoRa beacon packet format for ForestCam node discovery
 *
 * Defines a compact fixed-size binary beacon packet (14 bytes) that nodes
 * broadcast over LoRa. SSID is NOT included — it is derived from MAC:
 *   Leaf/Relay: "ForestCam-XXYY"  (MAC[4], MAC[5])
 *   Gateway:    "ForestCam-GW-XXYY"
 *
 * Packet Wire Format:
 *   Offset  Size  Field
 *     0       1   magic (0xFC)
 *     1       1   version (0x02)
 *     2       1   packetType (BEACON=0x01, RELAYED=0x02)
 *     3       1   ttl (starts at 2, decremented by relay)
 *     4       6   nodeId (WiFi MAC address)
 *    10       1   nodeRole (LEAF=0x01, RELAY=0x02, GATEWAY=0x03)
 *    11       1   imageCount
 *    12       1   batteryPct (0-100 or 0xFF=USB/unknown)
 *    13       2   uptimeMin (uint16 little-endian)
 *
 * Total: 15 bytes fixed.
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
static constexpr uint8_t BEACON_VERSION  = 0x02;  // v2: no SSID field
static constexpr uint8_t BEACON_MAX_SIZE = 48;
static constexpr uint8_t BEACON_MAX_SSID = 20;    // Kept for derived SSID buffer
static constexpr uint8_t BEACON_MIN_SIZE = 15;    // Fixed size (no variable SSID)
static constexpr uint8_t BEACON_V1_MIN_SIZE = 16; // v1 minimum for backwards compat

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
 *
 * v2 beacons have no SSID on the wire. The ssid field is populated
 * by deriveSsid() after parsing, using the MAC-based naming convention.
 */
struct BeaconPacket {
    uint8_t  packetType;                    ///< BeaconPacketType
    uint8_t  ttl;                           ///< Time-to-live (decremented by relays)
    uint8_t  nodeId[6];                     ///< MAC address (globally unique)
    uint8_t  nodeRole;                      ///< NodeRole enum
    char     ssid[BEACON_MAX_SSID + 1];     ///< Derived from MAC (not on wire in v2)
    uint8_t  ssidLen;                       ///< Length of derived SSID
    uint8_t  imageCount;                    ///< Number of images available
    uint8_t  batteryPct;                    ///< Battery % (0-100) or 0xFF=USB
    uint16_t uptimeMin;                     ///< Uptime in minutes

    /**
     * Serialize to a byte buffer for LoRa transmission (v2 format, no SSID).
     * @param[out] buf     Output buffer (must be >= BEACON_MIN_SIZE).
     * @param      maxLen  Buffer capacity.
     * @return Bytes written, or 0 on error.
     */
    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const;

    /**
     * Parse from a received LoRa packet. Supports both v1 (with SSID) and v2 (no SSID).
     * After parsing, call deriveSsid() to populate the ssid field.
     * @param buf  Input buffer.
     * @param len  Number of bytes received.
     * @return true if packet is a valid ForestCam beacon.
     */
    bool parse(const uint8_t* buf, uint8_t len);

    /**
     * Derive SSID from MAC address using naming convention:
     *   Gateway: "ForestCam-GW-XXYY"
     *   Leaf/Relay: "ForestCam-XXYY"
     * Called automatically after parse().
     */
    void deriveSsid();

    /**
     * Format nodeId as "AA:BB:CC:DD:EE:FF" for logging.
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

    /**
     * Derive SSID from a MAC address and role (static utility).
     */
    static void macToSsid(const uint8_t mac[6], uint8_t role, char* out, size_t outLen);
};

#endif // LORA_BEACON_H
