/**
 * @file ElectionPacket.h
 * @brief Election protocol packet format for gateway failover
 *
 * All four election packet types (ELECTION, SUPPRESS, COORDINATOR,
 * GW_RECLAIM) share the same 11-byte wire format:
 *
 * Offset  Size  Field
 *   0      1    magic (0xFC)
 *   1      1    version (0x01)
 *   2      1    type (0x30-0x33)
 *   3      6    senderId (MAC address)
 *   9      2    electionId (uint16 LE)
 *                TOTAL: 11 bytes
 *
 * Priority is computed from senderId via macToPriority() — no wire field needed.
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef ELECTION_PACKET_H
#define ELECTION_PACKET_H

#include <Arduino.h>
#include <cstring>
#include "AodvPacket.h"   // PKT_TYPE_ELECTION etc. + AODV_MAGIC/VERSION

static constexpr uint8_t ELECTION_PACKET_SIZE = 11;

struct ElectionPacket {
    uint8_t  type;           ///< PKT_TYPE_ELECTION / SUPPRESS / COORDINATOR / GW_RECLAIM
    uint8_t  senderId[6];    ///< Sender's MAC address
    uint16_t electionId;     ///< Monotonic counter, seeded from millis() on boot

    /**
     * Serialize to byte buffer for LoRa TX.
     * @param[out] buf     Output buffer (>= ELECTION_PACKET_SIZE).
     * @param      maxLen  Buffer capacity.
     * @return Bytes written (15), or 0 on error.
     */
    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const;

    /**
     * Parse from received LoRa buffer.
     * @param buf  Input buffer.
     * @param len  Buffer length.
     * @return true if valid election packet.
     */
    bool parse(const uint8_t* buf, uint8_t len);

    /**
     * Compute priority from a 6-byte MAC address.
     * Uses last 4 bytes as a uint32_t (little-endian).
     */
    static uint32_t macToPriority(const uint8_t mac[6]);

    /**
     * Format senderId as "AA:BB:CC:DD:EE:FF" for logging.
     */
    void senderIdToString(char* buf, size_t bufLen) const;
};

#endif // ELECTION_PACKET_H
