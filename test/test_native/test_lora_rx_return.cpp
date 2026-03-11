/**
 * @file test_lora_rx_return.cpp
 * @brief Reproduces the LoRa SX1262 startReceive() return-value bug.
 *
 * BUG: LoRaRadio::startReceive() always returns `true` (line 321 of
 *      LoRaRadio.cpp) even when the raw SPI fallback fails and the radio
 *      is stuck in FS mode (status=0x41, mode=4).  This causes all callers
 *      in main.cpp to believe the radio is listening when it is NOT.
 *
 * The logic under test is extracted from LoRaRadio::startReceive():
 *   - Parse the SX1262 status byte to extract chip mode
 *   - Return true ONLY when mode == 5 (RX)
 *   - Return false for any other mode (especially mode 4 = FS)
 *
 * Run:  pio test -e native
 */

#include <unity.h>
#include <cstdint>

void setUp(void) {}
void tearDown(void) {}

// ─── SX1262 mode constants (from datasheet Table 13-1) ─────────
static constexpr uint8_t SX1262_MODE_STBY_RC   = 2;
static constexpr uint8_t SX1262_MODE_STBY_XOSC = 3;
static constexpr uint8_t SX1262_MODE_FS         = 4;
static constexpr uint8_t SX1262_MODE_RX         = 5;
static constexpr uint8_t SX1262_MODE_TX         = 6;

// ─── Extract chip mode from SX1262 status byte ─────────────────
// Status byte layout: [7]=reserved, [6:4]=chipMode, [3:1]=cmdStatus, [0]=reserved
static uint8_t extractMode(uint8_t statusByte) {
    return (statusByte >> 4) & 0x07;
}

// ─── Decide whether startReceive() should report success ────────
// This mirrors the decision logic at the END of LoRaRadio::startReceive(),
// after the raw SPI fallback has been attempted.
//
// CURRENT CODE (buggy):  return true;          <-- always true
// CORRECT CODE:          return (mode == 5);   <-- true only if RX
//
// We include the current (buggy) implementation so the test FAILS
// before the fix is applied, proving the bug exists.
//
// After fixing LoRaRadio.cpp, flip USE_FIXED_LOGIC to 1 and re-run.
// (Or better: just run the test — it should pass after the fix.)

#include "LoRaStatus.h"  // shared helper: evaluateStartReceiveResult()

// =================================================================
// Test cases
// =================================================================

void test_extractMode_FS_0x41() {
    // 0x41 = 0b01000001 -> mode bits [6:4] = 100 = 4 (FS)
    TEST_ASSERT_EQUAL_UINT8(SX1262_MODE_FS, extractMode(0x41));
}

void test_extractMode_RX_0x52() {
    // 0x52 = 0b01010010 -> mode bits [6:4] = 101 = 5 (RX)
    TEST_ASSERT_EQUAL_UINT8(SX1262_MODE_RX, extractMode(0x52));
}

void test_extractMode_TX_0x62() {
    // 0x62 = 0b01100010 -> mode bits [6:4] = 110 = 6 (TX)
    TEST_ASSERT_EQUAL_UINT8(SX1262_MODE_TX, extractMode(0x62));
}

void test_extractMode_STBY_RC_0x22() {
    TEST_ASSERT_EQUAL_UINT8(SX1262_MODE_STBY_RC, extractMode(0x22));
}

// ─── THE BUG REPRODUCTION TEST ──────────────────────────────────
// This is the exact scenario from the serial monitor:
//   "Raw SPI fallback done, status=0x41, mode=4 (NOT RX!) PROBLEM"
//   ...but startReceive() returned true.
//
// evaluateStartReceiveResult() should return FALSE for status 0x41.

void test_startReceive_returns_false_when_stuck_in_FS_mode() {
    // Status 0x41 = FS mode — observed on all three boards
    bool result = evaluateStartReceiveResult(0x41);
    TEST_ASSERT_FALSE_MESSAGE(result,
        "BUG: startReceive() returns true when radio is stuck in FS mode (0x41). "
        "Radio is NOT listening — all beacon RX is broken.");
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
    // mode=5 (RX) regardless of cmdStatus in bits [3:1]
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
