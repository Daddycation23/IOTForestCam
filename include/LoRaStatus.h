/**
 * @file LoRaStatus.h
 * @brief SX12xx status-byte helpers — shared between firmware and tests.
 *
 * Works with both SX1262 and SX1280 chips (identical status byte layout).
 * SX1280 datasheet Table 11-3 / SX1262 datasheet Table 13-1:
 *   [7] reserved | [6:4] chipMode | [3:1] cmdStatus | [0] reserved
 */

#ifndef LORA_STATUS_H
#define LORA_STATUS_H

#include <cstdint>

// ─── SX12xx chip modes ──────────────────────────────────────
static constexpr uint8_t SX12XX_CHIPMODE_STBY_RC   = 2;
static constexpr uint8_t SX12XX_CHIPMODE_STBY_XOSC = 3;
static constexpr uint8_t SX12XX_CHIPMODE_FS         = 4;
static constexpr uint8_t SX12XX_CHIPMODE_RX         = 5;
static constexpr uint8_t SX12XX_CHIPMODE_TX         = 6;

/**
 * Extract the 3-bit chip-mode field from an SX12xx status byte.
 */
inline uint8_t sx12xxExtractMode(uint8_t statusByte) {
    return (statusByte >> 4) & 0x07;
}

/**
 * Decide whether startReceive() should report success.
 * Returns true ONLY when the radio is in RX mode (chipMode == 5).
 */
inline bool evaluateStartReceiveResult(uint8_t finalStatusByte) {
    return sx12xxExtractMode(finalStatusByte) == SX12XX_CHIPMODE_RX;
}

#endif // LORA_STATUS_H
