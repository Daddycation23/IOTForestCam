/**
 * @file LoRaDispatch.h
 * @brief Packet type dispatcher for all ForestCam LoRa packets
 *
 * All LoRa packets share a 3-byte header: magic(0xFC), version(0x01 or 0x02), type.
 * Version 0x01 is used by AODV/Election packets; version 0x02 is used by Beacons.
 * This header reads the type byte so the main loop can route to the
 * correct parser (BeaconPacket, RreqPacket, RrepPacket, etc.).
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef LORA_DISPATCH_H
#define LORA_DISPATCH_H

#include <Arduino.h>

// Shared protocol header constants (same as LoRaBeacon and AodvPacket)
static constexpr uint8_t LORA_PKT_MAGIC   = 0xFC;
static constexpr uint8_t LORA_PKT_VERSION    = 0x01; // AODV / Election packets
static constexpr uint8_t LORA_PKT_VERSION_V2 = 0x02; // Beacon packets

// Invalid / unknown packet type sentinel
static constexpr uint8_t LORA_PKT_UNKNOWN = 0xFF;

/**
 * Read the packet type from a raw LoRa buffer.
 * Validates magic and version bytes before returning the type.
 *
 * @param buf  Raw received bytes.
 * @param len  Number of bytes.
 * @return Packet type byte (0x01=BEACON, 0x10=RREQ, etc.) or 0xFF if invalid.
 */
inline uint8_t getLoRaPacketType(const uint8_t* buf, uint8_t len) {
    if (len < 3) return LORA_PKT_UNKNOWN;
    if (buf[0] != LORA_PKT_MAGIC)   return LORA_PKT_UNKNOWN;
    if (buf[1] != LORA_PKT_VERSION && buf[1] != LORA_PKT_VERSION_V2) return LORA_PKT_UNKNOWN;
    return buf[2];
}

/**
 * Extract the sender MAC address from a raw LoRa packet.
 *
 * Since the SX1280 has no MAC-layer source address, each packet type
 * embeds the sender identity at a different offset:
 *   Beacon  (v2)     → nodeId     at bytes  4–9
 *   RREQ    (0x10)   → prevHopId  at bytes 25–30
 *   RREP    (0x11)   → prevHopId  at bytes 21–26
 *   Election(0x30-33)→ senderId   at bytes  3–8
 *   HARVEST_ACK(0x21)→ relayId    at bytes  4–9
 *
 * @param buf     Raw received bytes.
 * @param len     Number of bytes.
 * @param outMac  Output buffer for 6-byte MAC (untouched if extraction fails).
 * @return true if a sender MAC was successfully extracted.
 */
inline bool extractSenderMac(const uint8_t* buf, uint8_t len, uint8_t outMac[6]) {
    if (len < 3) return false;
    uint8_t version = buf[1];
    uint8_t type    = buf[2];

    if (version == LORA_PKT_VERSION_V2) {
        // Beacon: nodeId at offset 4, need 10 bytes minimum
        if (len >= 10) { memcpy(outMac, &buf[4], 6); return true; }
        return false;
    }

    if (version == LORA_PKT_VERSION) {
        switch (type) {
            case 0x10: // RREQ: prevHopId at offset 25
                if (len >= 31) { memcpy(outMac, &buf[25], 6); return true; }
                return false;
            case 0x11: // RREP: prevHopId at offset 21
                if (len >= 27) { memcpy(outMac, &buf[21], 6); return true; }
                return false;
            case 0x21: // HARVEST_ACK: relayId at offset 4
                if (len >= 10) { memcpy(outMac, &buf[4], 6); return true; }
                return false;
            case 0x30: // ELECTION
            case 0x31: // SUPPRESS
            case 0x32: // COORDINATOR
            case 0x33: // GW_RECLAIM
                if (len >= 9) { memcpy(outMac, &buf[3], 6); return true; }
                return false;
            case 0x34: // RELAY_ASSIGN: gatewayId at offset 3
                if (len >= 15) { memcpy(outMac, &buf[3], 6); return true; }
                return false;
            default:
                return false; // RERR, HARVEST_CMD, etc. — no sender field
        }
    }

    return false;
}

#endif // LORA_DISPATCH_H
