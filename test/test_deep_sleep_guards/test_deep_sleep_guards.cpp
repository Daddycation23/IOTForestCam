/**
 * @file test_deep_sleep_guards.cpp
 * @brief Unit tests for deep sleep decision logic
 *
 * Re-implements DeepSleepManager::shouldSleep and activity tracking
 * as pure functions for native testing without ESP32 dependencies.
 *
 * Run: pio test -e native -f test_deep_sleep_guards
 */

#include <unity.h>
#include <cstdint>

#ifdef NATIVE_TEST

// ─── Configuration constant (mirrors DeepSleepManager.h) ────
static constexpr uint32_t SLEEP_ACTIVE_TIMEOUT_MS = 120000;  // 2 minutes

// ─── Testable sleep manager (mirrors DeepSleepManager logic) ─
struct TestSleepManager {
    uint32_t lastActivityMs;
    bool     harvestInProgress;
    bool     coapBusy;

    void init(uint32_t nowMs) {
        lastActivityMs = nowMs;
        harvestInProgress = false;
        coapBusy = false;
    }

    bool shouldSleep(uint32_t nowMs) const {
        // Never sleep during active operations
        if (harvestInProgress || coapBusy) return false;
        // Sleep if active timeout has expired
        return (nowMs - lastActivityMs) >= SLEEP_ACTIVE_TIMEOUT_MS;
    }

    void onActivity(uint32_t nowMs) {
        lastActivityMs = nowMs;
    }

    void setHarvestInProgress(bool inProgress, uint32_t nowMs) {
        harvestInProgress = inProgress;
        if (inProgress) onActivity(nowMs);
    }

    void setCoapBusy(bool busy, uint32_t nowMs) {
        coapBusy = busy;
        if (busy) onActivity(nowMs);
    }
};

// ─── Test instance ───────────────────────────────────────────
static TestSleepManager mgr;

#endif // NATIVE_TEST

// ─── Test setup / teardown ───────────────────────────────────
void setUp(void) {
    mgr.init(0);
}

void tearDown(void) {}

// ═════════════════════════════════════════════════════════════
// SLEEP GUARD TESTS
// ═════════════════════════════════════════════════════════════

void test_should_not_sleep_during_harvest() {
    mgr.init(0);
    mgr.setHarvestInProgress(true, 0);

    // Even well past the timeout, should NOT sleep during harvest
    TEST_ASSERT_FALSE(mgr.shouldSleep(SLEEP_ACTIVE_TIMEOUT_MS + 50000));
}

void test_should_not_sleep_during_coap() {
    mgr.init(0);
    mgr.setCoapBusy(true, 0);

    // Even well past the timeout, should NOT sleep during CoAP
    TEST_ASSERT_FALSE(mgr.shouldSleep(SLEEP_ACTIVE_TIMEOUT_MS + 50000));
}

void test_should_sleep_after_timeout() {
    mgr.init(0);

    // Exactly at timeout boundary
    TEST_ASSERT_TRUE(mgr.shouldSleep(SLEEP_ACTIVE_TIMEOUT_MS));

    // Well past timeout
    TEST_ASSERT_TRUE(mgr.shouldSleep(SLEEP_ACTIVE_TIMEOUT_MS + 60000));
}

void test_activity_resets_timer() {
    mgr.init(0);

    // At 100s: not yet timed out
    TEST_ASSERT_FALSE(mgr.shouldSleep(100000));

    // Reset activity at 100s
    mgr.onActivity(100000);

    // At 200s: only 100s since last activity (timeout is 120s), should NOT sleep
    TEST_ASSERT_FALSE(mgr.shouldSleep(200000));

    // At 220s: 120s since last activity, SHOULD sleep
    TEST_ASSERT_TRUE(mgr.shouldSleep(220000));
}

void test_should_not_sleep_before_timeout() {
    mgr.init(0);

    // Well before timeout
    TEST_ASSERT_FALSE(mgr.shouldSleep(0));
    TEST_ASSERT_FALSE(mgr.shouldSleep(60000));
    TEST_ASSERT_FALSE(mgr.shouldSleep(SLEEP_ACTIVE_TIMEOUT_MS - 1));
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_should_not_sleep_during_harvest);
    RUN_TEST(test_should_not_sleep_during_coap);
    RUN_TEST(test_should_sleep_after_timeout);
    RUN_TEST(test_activity_resets_timer);
    RUN_TEST(test_should_not_sleep_before_timeout);

    return UNITY_END();
}
