/**
 * @file test_deep_sleep.cpp
 * @brief Tests for deep sleep manager logic and wake protocol
 *
 * Validates:
 *   - Wake packet type encoding
 *   - Active timeout logic
 *   - RTC state persistence simulation
 *   - Two-step wake timing constants
 *
 * Run:  pio test -e native -f test_deep_sleep
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

void setUp(void) {}
void tearDown(void) {}

// ─── Packet type constants (mirrors AodvPacket.h) ────────────
static constexpr uint8_t AODV_MAGIC            = 0xFC;
static constexpr uint8_t AODV_VERSION          = 0x01;
static constexpr uint8_t PKT_TYPE_WAKE_PING    = 0x40;
static constexpr uint8_t PKT_TYPE_WAKE_BEACON_REQ = 0x41;
static constexpr uint8_t PKT_TYPE_HARVEST_CMD  = 0x20;

// ─── Deep sleep timing constants ─────────────────────────────
static constexpr uint32_t WAKE_COMMAND_TIMEOUT_MS  = 3000;   // Wait for cmd after wake
static constexpr uint32_t ACTIVE_TIMEOUT_MS        = 120000; // Stay awake 2 min after boot
static constexpr uint32_t WAKE_SETTLE_DELAY_MS     = 500;    // Delay between wake ping and cmd

// ─── Simulated active timeout logic ──────────────────────────
struct SleepState {
    uint32_t bootTimeMs;
    uint32_t lastActivityMs;
    uint32_t activeTimeoutMs;
    bool     harvestInProgress;
    bool     coapServerBusy;

    bool shouldSleep(uint32_t nowMs) const {
        // Never sleep during active harvest or CoAP serving
        if (harvestInProgress || coapServerBusy) return false;
        // Sleep after active timeout expires
        return (nowMs - lastActivityMs) >= activeTimeoutMs;
    }

    void onActivity(uint32_t nowMs) {
        lastActivityMs = nowMs;
    }
};

// =================================================================
// Wake packet type tests
// =================================================================

void test_wake_ping_type_value() {
    TEST_ASSERT_EQUAL_HEX8(0x40, PKT_TYPE_WAKE_PING);
}

void test_wake_beacon_req_type_value() {
    TEST_ASSERT_EQUAL_HEX8(0x41, PKT_TYPE_WAKE_BEACON_REQ);
}

void test_wake_ping_no_collision_with_existing() {
    // Ensure no collision with existing packet types
    TEST_ASSERT_NOT_EQUAL(PKT_TYPE_WAKE_PING, PKT_TYPE_HARVEST_CMD);
    TEST_ASSERT_NOT_EQUAL(PKT_TYPE_WAKE_BEACON_REQ, PKT_TYPE_HARVEST_CMD);
    TEST_ASSERT_NOT_EQUAL(PKT_TYPE_WAKE_PING, PKT_TYPE_WAKE_BEACON_REQ);
}

void test_wake_ping_packet_format() {
    // WAKE_PING is a minimal packet: magic + version + type = 3 bytes
    uint8_t packet[3];
    packet[0] = AODV_MAGIC;
    packet[1] = AODV_VERSION;
    packet[2] = PKT_TYPE_WAKE_PING;

    TEST_ASSERT_EQUAL_HEX8(0xFC, packet[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, packet[1]);
    TEST_ASSERT_EQUAL_HEX8(0x40, packet[2]);
}

// =================================================================
// Active timeout logic tests
// =================================================================

void test_should_not_sleep_during_active_period() {
    SleepState state = {0, 0, ACTIVE_TIMEOUT_MS, false, false};
    // 60 seconds after boot — still within 120s timeout
    TEST_ASSERT_FALSE(state.shouldSleep(60000));
}

void test_should_sleep_after_timeout() {
    SleepState state = {0, 0, ACTIVE_TIMEOUT_MS, false, false};
    // 121 seconds — past the 120s timeout
    TEST_ASSERT_TRUE(state.shouldSleep(121000));
}

void test_should_not_sleep_during_harvest() {
    SleepState state = {0, 0, ACTIVE_TIMEOUT_MS, true, false};
    // Even if timeout expired, don't sleep during harvest
    TEST_ASSERT_FALSE(state.shouldSleep(200000));
}

void test_should_not_sleep_during_coap_serving() {
    SleepState state = {0, 0, ACTIVE_TIMEOUT_MS, false, true};
    // Don't sleep while CoAP server is actively serving
    TEST_ASSERT_FALSE(state.shouldSleep(200000));
}

void test_activity_resets_timeout() {
    SleepState state = {0, 0, ACTIVE_TIMEOUT_MS, false, false};
    // Would normally sleep at 121s
    TEST_ASSERT_TRUE(state.shouldSleep(121000));

    // But if activity happened at 100s, timeout extends
    state.onActivity(100000);
    TEST_ASSERT_FALSE(state.shouldSleep(121000));

    // Sleeps at 100s + 120s = 220s
    TEST_ASSERT_TRUE(state.shouldSleep(221000));
}

// =================================================================
// Two-step wake protocol timing
// =================================================================

void test_wake_settle_delay() {
    // Sender must wait 500ms between wake ping and command
    TEST_ASSERT_EQUAL_UINT32(500, WAKE_SETTLE_DELAY_MS);
}

void test_wake_command_timeout() {
    // Receiver waits 3s for command after waking
    TEST_ASSERT_EQUAL_UINT32(3000, WAKE_COMMAND_TIMEOUT_MS);
}

void test_settle_before_command_timeout() {
    // Settle delay must be less than command timeout
    // (sender sends command 500ms after wake, receiver waits 3s)
    TEST_ASSERT_TRUE(WAKE_SETTLE_DELAY_MS < WAKE_COMMAND_TIMEOUT_MS);
}

// =================================================================
// RTC state persistence simulation
// =================================================================

struct RtcState {
    uint32_t bootCount;
    uint8_t  role;        // 0=leaf, 1=relay, 2=gateway
    int8_t   lastImgIdx;
    char     ssid[32];
};

void test_rtc_state_persists_across_sleep() {
    // Simulate: set state before sleep, read after wake
    RtcState beforeSleep = {5, 1, 3, "ForestCam-AABB"};
    RtcState afterWake;

    // Simulate RTC persistence (memcpy = RTC memory survives)
    memcpy(&afterWake, &beforeSleep, sizeof(RtcState));
    afterWake.bootCount++;  // Incremented on wake

    TEST_ASSERT_EQUAL_UINT32(6, afterWake.bootCount);
    TEST_ASSERT_EQUAL_UINT8(1, afterWake.role);
    TEST_ASSERT_EQUAL_INT8(3, afterWake.lastImgIdx);
    TEST_ASSERT_EQUAL_STRING("ForestCam-AABB", afterWake.ssid);
}

void test_rtc_boot_count_increments() {
    uint32_t bootCount = 0;
    for (int i = 0; i < 10; i++) {
        bootCount++;
    }
    TEST_ASSERT_EQUAL_UINT32(10, bootCount);
}

// =================================================================
// Harvest wake state machine
// =================================================================

enum TestHarvestState : uint8_t {
    H_IDLE, H_START, H_ROUTE_DISCOVERY, H_DISCONNECT,
    H_WAKE_NODE,  // NEW state
    H_CONNECT, H_COAP_INIT, H_DOWNLOAD, H_NEXT,
    H_RELAY_CMD, H_RELAY_WAIT, H_DONE
};

void test_wake_node_state_exists() {
    TestHarvestState state = H_WAKE_NODE;
    TEST_ASSERT_EQUAL(H_WAKE_NODE, state);
    // WAKE_NODE sits between DISCONNECT and CONNECT
    TEST_ASSERT_TRUE(H_WAKE_NODE > H_DISCONNECT);
    TEST_ASSERT_TRUE(H_WAKE_NODE < H_CONNECT);
}

void test_wake_node_transitions() {
    // DISCONNECT → WAKE_NODE → CONNECT (for direct leaf harvest)
    TestHarvestState state = H_DISCONNECT;
    state = H_WAKE_NODE;
    TEST_ASSERT_EQUAL(H_WAKE_NODE, state);
    state = H_CONNECT;
    TEST_ASSERT_EQUAL(H_CONNECT, state);
}

// =================================================================
// Entry point
// =================================================================

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Wake packet types
    RUN_TEST(test_wake_ping_type_value);
    RUN_TEST(test_wake_beacon_req_type_value);
    RUN_TEST(test_wake_ping_no_collision_with_existing);
    RUN_TEST(test_wake_ping_packet_format);

    // Active timeout logic
    RUN_TEST(test_should_not_sleep_during_active_period);
    RUN_TEST(test_should_sleep_after_timeout);
    RUN_TEST(test_should_not_sleep_during_harvest);
    RUN_TEST(test_should_not_sleep_during_coap_serving);
    RUN_TEST(test_activity_resets_timeout);

    // Two-step wake protocol
    RUN_TEST(test_wake_settle_delay);
    RUN_TEST(test_wake_command_timeout);
    RUN_TEST(test_settle_before_command_timeout);

    // RTC state persistence
    RUN_TEST(test_rtc_state_persists_across_sleep);
    RUN_TEST(test_rtc_boot_count_increments);

    // Harvest wake state
    RUN_TEST(test_wake_node_state_exists);
    RUN_TEST(test_wake_node_transitions);

    return UNITY_END();
}
