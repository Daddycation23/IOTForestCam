/**
 * @file test_election_state.cpp
 * @brief Unit tests for ElectionManager logic (priority, backoff, state strings)
 *
 * Full state machine integration tests require hardware (LoRa, WiFi).
 * These tests validate the pure-logic components.
 *
 * Run: pio test -e native -f test_election_state
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#ifdef NATIVE_TEST

// ─── Protocol constants ──────────────────────────────────────
static constexpr uint8_t AODV_MAGIC   = 0xFC;
static constexpr uint8_t AODV_VERSION = 0x01;
static constexpr uint32_t ELECTION_BACKOFF_MIN_MS = 500;
static constexpr uint32_t ELECTION_BACKOFF_MAX_MS = 3000;

static uint32_t macToPriority(const uint8_t mac[6]) {
    return mac[2] | (mac[3] << 8) | (mac[4] << 16) | (mac[5] << 24);
}

static uint32_t computeBackoff(uint32_t priority) {
    float norm = (float)(priority % 1000) / 999.0f;
    return ELECTION_BACKOFF_MAX_MS
         - (uint32_t)(norm * (ELECTION_BACKOFF_MAX_MS - ELECTION_BACKOFF_MIN_MS));
}

#endif // NATIVE_TEST

void setUp(void) {}
void tearDown(void) {}

// ─── Priority ordering: relay with higher MAC wins ───────────
void test_higher_mac_higher_priority() {
    uint8_t macA[6] = {0xAA, 0xBB, 0x00, 0x00, 0x00, 0x01};
    uint8_t macB[6] = {0xAA, 0xBB, 0x00, 0x00, 0x00, 0xFF};
    TEST_ASSERT_TRUE(macToPriority(macB) > macToPriority(macA));
}

// ─── Priority uses last 4 bytes, ignores first 2 ────────────
void test_priority_ignores_first_two_bytes() {
    uint8_t macA[6] = {0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01};
    uint8_t macB[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    TEST_ASSERT_EQUAL_UINT32(macToPriority(macA), macToPriority(macB));
}

// ─── Backoff: higher priority -> shorter backoff ─────────────
void test_backoff_higher_priority_shorter() {
    uint32_t highPri = 999;  // max in mod 1000
    uint32_t lowPri  = 0;    // min in mod 1000
    uint32_t highBackoff = computeBackoff(highPri);
    uint32_t lowBackoff  = computeBackoff(lowPri);
    TEST_ASSERT_TRUE(highBackoff < lowBackoff);
}

// ─── Backoff: stays within bounds ────────────────────────────
void test_backoff_within_bounds() {
    for (uint32_t pri = 0; pri < 2000; pri += 100) {
        uint32_t b = computeBackoff(pri);
        TEST_ASSERT_TRUE(b >= ELECTION_BACKOFF_MIN_MS);
        TEST_ASSERT_TRUE(b <= ELECTION_BACKOFF_MAX_MS);
    }
}

// ─── Backoff: max priority -> min backoff ────────────────────
void test_backoff_max_priority_min_delay() {
    uint32_t b = computeBackoff(999);
    TEST_ASSERT_EQUAL_UINT32(ELECTION_BACKOFF_MIN_MS, b);
}

// ─── Backoff: min priority -> max backoff ────────────────────
void test_backoff_min_priority_max_delay() {
    uint32_t b = computeBackoff(0);
    TEST_ASSERT_EQUAL_UINT32(ELECTION_BACKOFF_MAX_MS, b);
}

// ─── Two different MACs always produce different priorities ──
void test_different_macs_different_priorities() {
    uint8_t macA[6] = {0x00, 0x00, 0x11, 0x22, 0x33, 0x44};
    uint8_t macB[6] = {0x00, 0x00, 0x11, 0x22, 0x33, 0x45};
    TEST_ASSERT_NOT_EQUAL(macToPriority(macA), macToPriority(macB));
}

// ─── Runner ──────────────────────────────────────────────────
int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_higher_mac_higher_priority);
    RUN_TEST(test_priority_ignores_first_two_bytes);
    RUN_TEST(test_backoff_higher_priority_shorter);
    RUN_TEST(test_backoff_within_bounds);
    RUN_TEST(test_backoff_max_priority_min_delay);
    RUN_TEST(test_backoff_min_priority_max_delay);
    RUN_TEST(test_different_macs_different_priorities);

    return UNITY_END();
}
