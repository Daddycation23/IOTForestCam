/**
 * @file test_harvest_trigger.cpp
 * @brief Unit tests for promoted-gateway harvest trigger timing logic
 *
 * Tests cover: fresh promotion init, listen period gating, active node guard,
 * double-trigger guard, timer reset after harvest done, demotion/repromotion.
 *
 * Run: pio test -e native -f test_harvest_trigger
 */

#include <unity.h>
#include <cstdint>

#ifdef NATIVE_TEST

// ─── Inline enum copy (mirrors HarvestLoop.h) ───────────────
enum HarvestState : uint8_t {
    HARVEST_IDLE,
    HARVEST_START,
    HARVEST_ROUTE_DISCOVERY,
    HARVEST_DISCONNECT,
    HARVEST_WAKE_NODE,
    HARVEST_CONNECT,
    HARVEST_COAP_INIT,
    HARVEST_DOWNLOAD,
    HARVEST_NEXT,
    HARVEST_RELAY_CMD,
    HARVEST_RELAY_WAIT,
    HARVEST_DONE,
};

// ─── Configuration constant (mirrors main.cpp) ──────────────
static constexpr uint32_t HARVEST_LISTEN_PERIOD_MS = 60000;

// ─── Testable pure function (mirrors main.cpp logic) ────────
struct HarvestTriggerResult {
    bool shouldInit;
    bool shouldTrigger;
    bool shouldResetTimer;
};

HarvestTriggerResult evaluateHarvestTrigger(
    bool nowPromoted, bool wasPromoted,
    HarvestState curState, HarvestState prevState,
    uint8_t activeNodes, uint32_t elapsedMs,
    bool alreadyTriggered)
{
    HarvestTriggerResult r = {false, false, false};

    if (nowPromoted && !wasPromoted) {
        r.shouldInit = true;
    }

    if (nowPromoted &&
        curState == HARVEST_IDLE &&
        !alreadyTriggered &&
        activeNodes > 0 &&
        elapsedMs >= HARVEST_LISTEN_PERIOD_MS)
    {
        r.shouldTrigger = true;
    }

    if (curState == HARVEST_IDLE && prevState == HARVEST_DONE) {
        r.shouldResetTimer = true;
    }

    return r;
}

#endif // NATIVE_TEST

// ─── Test setup / teardown ───────────────────────────────────
void setUp(void) {}
void tearDown(void) {}

// ═════════════════════════════════════════════════════════════
// PROMOTION INIT TESTS
// ═════════════════════════════════════════════════════════════

void test_fresh_promotion_triggers_init() {
    // First time promoted: wasPromoted=false, nowPromoted=true → shouldInit
    auto r = evaluateHarvestTrigger(
        true, false,                    // nowPromoted, wasPromoted
        HARVEST_IDLE, HARVEST_IDLE,     // curState, prevState
        0, 0,                           // activeNodes, elapsedMs
        false);                         // alreadyTriggered
    TEST_ASSERT_TRUE(r.shouldInit);
}

void test_already_promoted_no_reinit() {
    // Already promoted on previous tick: no re-init
    auto r = evaluateHarvestTrigger(
        true, true,                     // nowPromoted, wasPromoted
        HARVEST_IDLE, HARVEST_IDLE,
        3, 0,
        false);
    TEST_ASSERT_FALSE(r.shouldInit);
}

// ═════════════════════════════════════════════════════════════
// TRIGGER TIMING TESTS
// ═════════════════════════════════════════════════════════════

void test_harvest_not_triggered_before_listen_period() {
    // Elapsed < LISTEN_PERIOD → no trigger even if all other conditions met
    auto r = evaluateHarvestTrigger(
        true, true,
        HARVEST_IDLE, HARVEST_IDLE,
        3, HARVEST_LISTEN_PERIOD_MS - 1, // Just under the threshold
        false);
    TEST_ASSERT_FALSE(r.shouldTrigger);
}

void test_harvest_triggered_after_listen_period() {
    // elapsed >= LISTEN_PERIOD, activeNodes > 0, idle, not triggered → trigger
    auto r = evaluateHarvestTrigger(
        true, true,
        HARVEST_IDLE, HARVEST_IDLE,
        3, HARVEST_LISTEN_PERIOD_MS,
        false);
    TEST_ASSERT_TRUE(r.shouldTrigger);
}

// ═════════════════════════════════════════════════════════════
// GUARD TESTS
// ═════════════════════════════════════════════════════════════

void test_no_trigger_when_no_active_nodes() {
    // activeNodes=0 → no trigger even after listen period
    auto r = evaluateHarvestTrigger(
        true, true,
        HARVEST_IDLE, HARVEST_IDLE,
        0, HARVEST_LISTEN_PERIOD_MS + 5000,
        false);
    TEST_ASSERT_FALSE(r.shouldTrigger);
}

void test_guard_prevents_double_trigger() {
    // alreadyTriggered=true → no trigger
    auto r = evaluateHarvestTrigger(
        true, true,
        HARVEST_IDLE, HARVEST_IDLE,
        3, HARVEST_LISTEN_PERIOD_MS + 5000,
        true);
    TEST_ASSERT_FALSE(r.shouldTrigger);
}

// ═════════════════════════════════════════════════════════════
// TIMER RESET TESTS
// ═════════════════════════════════════════════════════════════

void test_timer_resets_after_harvest_done() {
    // curState=IDLE, prevState=DONE → shouldResetTimer
    auto r = evaluateHarvestTrigger(
        true, true,
        HARVEST_IDLE, HARVEST_DONE,
        3, 0,
        false);
    TEST_ASSERT_TRUE(r.shouldResetTimer);
}

// ═════════════════════════════════════════════════════════════
// DEMOTION / REPROMOTION TEST
// ═════════════════════════════════════════════════════════════

void test_demotion_and_repromotion_resets_state() {
    // Step 1: Fresh promotion → init fires
    auto r1 = evaluateHarvestTrigger(
        true, false,
        HARVEST_IDLE, HARVEST_IDLE,
        0, 0,
        false);
    TEST_ASSERT_TRUE(r1.shouldInit);

    // Step 2: Demoted (nowPromoted=false) → no init, no trigger
    auto r2 = evaluateHarvestTrigger(
        false, true,
        HARVEST_IDLE, HARVEST_IDLE,
        0, 0,
        false);
    TEST_ASSERT_FALSE(r2.shouldInit);
    TEST_ASSERT_FALSE(r2.shouldTrigger);

    // Step 3: Re-promoted → init fires again (wasPromoted=false after demotion)
    auto r3 = evaluateHarvestTrigger(
        true, false,
        HARVEST_IDLE, HARVEST_IDLE,
        0, 0,
        false);
    TEST_ASSERT_TRUE(r3.shouldInit);
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Promotion init
    RUN_TEST(test_fresh_promotion_triggers_init);
    RUN_TEST(test_already_promoted_no_reinit);

    // Trigger timing
    RUN_TEST(test_harvest_not_triggered_before_listen_period);
    RUN_TEST(test_harvest_triggered_after_listen_period);

    // Guards
    RUN_TEST(test_no_trigger_when_no_active_nodes);
    RUN_TEST(test_guard_prevents_double_trigger);

    // Timer reset
    RUN_TEST(test_timer_resets_after_harvest_done);

    // Demotion / repromotion
    RUN_TEST(test_demotion_and_repromotion_resets_state);

    return UNITY_END();
}
