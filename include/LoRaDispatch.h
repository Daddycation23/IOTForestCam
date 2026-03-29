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

#endif // LORA_DISPATCH_H
