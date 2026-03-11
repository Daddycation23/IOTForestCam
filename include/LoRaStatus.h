/**
 * @file LoRaStatus.h
 * @brief SX1262 status-byte helpers — shared between firmware and tests.
 *
 * Extracted from LoRaRadio.cpp so that the return-value decision in
 * startReceive() can be unit-tested on the host without hardware.
 */

#ifndef LORA_STATUS_H
#define LORA_STATUS_H

#include <cstdint>

// ─── SX1262 chip modes (datasheet Table 13-1) ─────────────────
static constexpr uint8_t SX1262_CHIPMODE_STBY_RC   = 2;
static constexpr uint8_t SX1262_CHIPMODE_STBY_XOSC = 3;
static constexpr uint8_t SX1262_CHIPMODE_FS         = 4;
static constexpr uint8_t SX1262_CHIPMODE_RX         = 5;
static constexpr uint8_t SX1262_CHIPMODE_TX         = 6;

/**
 * Extract the 3-bit chip-mode field from an SX1262 status byte.
 *
 * Status byte layout: [7] reserved | [6:4] chipMode | [3:1] cmdStatus | [0] reserved
 */
inline uint8_t sx1262ExtractMode(uint8_t statusByte) {
    return (statusByte >> 4) & 0x07;
}

/**
 * Decide whether startReceive() should report success based on the
 * final SX1262 status byte after all fallback attempts.
 *
 * Returns true ONLY when the radio is in RX mode (chipMode == 5).
 * Any other mode (especially FS == 4) means the radio is NOT listening.
 */
inline bool evaluateStartReceiveResult(uint8_t finalStatusByte) {
    return sx1262ExtractMode(finalStatusByte) == SX1262_CHIPMODE_RX;
}

#endif // LORA_STATUS_H
