/**
 * @file test_lora_rx_return.cpp
 * @brief Tests for LoRa status-byte parsing and startReceive() return logic.
 *
 * Originally written to reproduce a bug where startReceive() always returned
 * true even when the radio was stuck in FS mode. Root cause: the firmware was
 * using the SX1262 driver on SX1280 hardware.
 *
 * These tests validate the shared status-byte parsing logic in LoRaStatus.h,
 * which works for both SX1262 and SX1280 (identical status byte layout).
 *
 * Run:  pio test -e native
 */

#include <unity.h>
#include <cstdint>

void setUp(void) {}
void tearDown(void) {}

// ─── SX12xx mode constants (shared status byte layout) ──────
static constexpr uint8_t SX12XX_MODE_STBY_RC   = 2;
static constexpr uint8_t SX12XX_MODE_STBY_XOSC = 3;
static constexpr uint8_t SX12XX_MODE_FS         = 4;
static constexpr uint8_t SX12XX_MODE_RX         = 5;
static constexpr uint8_t SX12XX_MODE_TX         = 6;

// ─── Extract chip mode from SX12xx status byte ──────────────
// Status byte layout: [7]=reserved, [6:4]=chipMode, [3:1]=cmdStatus, [0]=reserved
static uint8_t extractMode(uint8_t statusByte) {
    return (statusByte >> 4) & 0x07;
}

#include "LoRaStatus.h"  // shared helper: evaluateStartReceiveResult()

// =================================================================
// Test cases
// =================================================================

void test_extractMode_FS_0x41() {
    // 0x41 = 0b01000001 -> mode bits [6:4] = 100 = 4 (FS)
    TEST_ASSERT_EQUAL_UINT8(SX12XX_MODE_FS, extractMode(0x41));
}

void test_extractMode_RX_0x52() {
    // 0x52 = 0b01010010 -> mode bits [6:4] = 101 = 5 (RX)
    TEST_ASSERT_EQUAL_UINT8(SX12XX_MODE_RX, extractMode(0x52));
}

void test_extractMode_TX_0x62() {
    // 0x62 = 0b01100010 -> mode bits [6:4] = 110 = 6 (TX)
    TEST_ASSERT_EQUAL_UINT8(SX12XX_MODE_TX, extractMode(0x62));
}

void test_extractMode_STBY_RC_0x22() {
    TEST_ASSERT_EQUAL_UINT8(SX12XX_MODE_STBY_RC, extractMode(0x22));
}

void test_startReceive_returns_false_when_stuck_in_FS_mode() {
    // Status 0x41 = FS mode — observed when using wrong chip driver
    bool result = evaluateStartReceiveResult(0x41);
    TEST_ASSERT_FALSE_MESSAGE(result,
        "startReceive() must return false when radio is stuck in FS mode (0x41).");
}

void test_startReceive_returns_true_when_in_RX_mode() {
    // Status 0x52 = RX mode — radio is correctly listening
    bool result = evaluateStartReceiveResult(0x52);
    TEST_ASSERT_TRUE_MESSAGE(result,
        "startReceive() should return true when radio is in RX mode.");
}

void test_startReceive_returns_false_for_STBY_RC() {
    bool result = evaluateStartReceiveResult(0x22);
    TEST_ASSERT_FALSE_MESSAGE(result,
        "startReceive() should return false when radio is in STBY_RC mode.");
}

void test_startReceive_returns_false_for_TX_mode() {
    bool result = evaluateStartReceiveResult(0x62);
    TEST_ASSERT_FALSE_MESSAGE(result,
        "startReceive() should return false when radio is in TX mode.");
}

void test_startReceive_accepts_all_RX_status_variants() {
    // RX mode with different command-status bits
    TEST_ASSERT_TRUE(evaluateStartReceiveResult(0x50));  // cmdStatus=0
    TEST_ASSERT_TRUE(evaluateStartReceiveResult(0x52));  // cmdStatus=1
    TEST_ASSERT_TRUE(evaluateStartReceiveResult(0x54));  // cmdStatus=2 (data available)
    TEST_ASSERT_TRUE(evaluateStartReceiveResult(0x56));  // cmdStatus=3 (cmd timeout)
}

// =================================================================
// Runner
// =================================================================

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_extractMode_FS_0x41);
    RUN_TEST(test_extractMode_RX_0x52);
    RUN_TEST(test_extractMode_TX_0x62);
    RUN_TEST(test_extractMode_STBY_RC_0x22);

    RUN_TEST(test_startReceive_returns_false_when_stuck_in_FS_mode);
    RUN_TEST(test_startReceive_returns_true_when_in_RX_mode);
    RUN_TEST(test_startReceive_returns_false_for_STBY_RC);
    RUN_TEST(test_startReceive_returns_false_for_TX_mode);
    RUN_TEST(test_startReceive_accepts_all_RX_status_variants);

    return UNITY_END();
}
