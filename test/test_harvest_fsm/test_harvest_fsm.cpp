/**
 * @file test_harvest_fsm.cpp
 * @brief Unit tests for HarvestLoop state machine transitions
 *
 * Re-implements harvest FSM transition logic as pure functions so they
 * can be tested without ESP32 hardware dependencies.
 *
 * Run: pio test -e native -f test_harvest_fsm
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

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

// ─── Pure function: mirrors _doStart() transition logic ──────
static HarvestState doStartTransition(uint8_t activeCount, bool aodvAvailable) {
    if (activeCount == 0) {
        return HARVEST_DONE;
    }
    if (aodvAvailable) {
        return HARVEST_ROUTE_DISCOVERY;
    }
    return HARVEST_DISCONNECT;
}

// ─── Pure function: mirrors abortCycle() logic ───────────────
struct AbortResult {
    uint8_t nodesFailed;
    HarvestState finalState;
};

static AbortResult doAbort(HarvestState currentState, uint8_t unharvestedCount) {
    AbortResult result = {0, currentState};
    if (currentState == HARVEST_IDLE) {
        return result;  // no-op
    }
    result.nodesFailed = unharvestedCount;
    result.finalState = HARVEST_IDLE;
    return result;
}

// ─── Pure function: mirrors _doDisconnect() transition ───────
static HarvestState doDisconnectTransition() {
    // Gateway-as-AP: no WAKE_NODE needed, go directly to CONNECT
    return HARVEST_CONNECT;
}

// ─── Pure function: mirrors _doConnect() "no more nodes" path ──
static HarvestState doConnectNoMoreNodes(bool hasNext) {
    if (!hasNext) {
        return HARVEST_DONE;
    }
    return HARVEST_CONNECT;  // Continue with connection
}

// ─── Pure function: mirrors download stats counting ──────────
struct DownloadStats {
    uint8_t nodesSucceeded;
    uint8_t nodesFailed;
};

static DownloadStats countDownloadResult(uint8_t passCount, uint8_t failCount) {
    DownloadStats stats = {0, 0};
    if (failCount == 0) {
        stats.nodesSucceeded = 1;
    } else {
        stats.nodesFailed = 1;
    }
    return stats;
}

#endif // NATIVE_TEST

// ─── Test setup / teardown ───────────────────────────────────
void setUp(void) {}
void tearDown(void) {}

// ═════════════════════════════════════════════════════════════
// START STATE TESTS
// ═════════════════════════════════════════════════════════════

void test_start_with_empty_registry_goes_to_done() {
    HarvestState next = doStartTransition(0, false);
    TEST_ASSERT_EQUAL(HARVEST_DONE, next);
}

void test_start_with_nodes_goes_to_route_discovery() {
    HarvestState next = doStartTransition(3, true);
    TEST_ASSERT_EQUAL(HARVEST_ROUTE_DISCOVERY, next);
}

void test_start_without_aodv_skips_route_discovery() {
    HarvestState next = doStartTransition(3, false);
    TEST_ASSERT_EQUAL(HARVEST_DISCONNECT, next);
}

// ═════════════════════════════════════════════════════════════
// ABORT TESTS
// ═════════════════════════════════════════════════════════════

void test_abort_marks_all_remaining_failed() {
    AbortResult r = doAbort(HARVEST_DOWNLOAD, 5);
    TEST_ASSERT_EQUAL_UINT8(5, r.nodesFailed);
    TEST_ASSERT_EQUAL(HARVEST_IDLE, r.finalState);
}

void test_abort_from_idle_is_noop() {
    AbortResult r = doAbort(HARVEST_IDLE, 3);
    TEST_ASSERT_EQUAL_UINT8(0, r.nodesFailed);
    TEST_ASSERT_EQUAL(HARVEST_IDLE, r.finalState);
}

// ═════════════════════════════════════════════════════════════
// DISCONNECT / CONNECT TESTS
// ═════════════════════════════════════════════════════════════

void test_disconnect_goes_to_connect() {
    HarvestState next = doDisconnectTransition();
    TEST_ASSERT_EQUAL(HARVEST_CONNECT, next);
}

void test_connect_no_more_nodes_goes_to_done() {
    HarvestState next = doConnectNoMoreNodes(false);
    TEST_ASSERT_EQUAL(HARVEST_DONE, next);
}

// ═════════════════════════════════════════════════════════════
// DOWNLOAD STATS TESTS
// ═════════════════════════════════════════════════════════════

void test_download_partial_failure_counts_correctly() {
    // 3 images pass, 2 fail → node counted as failed
    DownloadStats stats = countDownloadResult(3, 2);
    TEST_ASSERT_EQUAL_UINT8(0, stats.nodesSucceeded);
    TEST_ASSERT_EQUAL_UINT8(1, stats.nodesFailed);

    // All pass → node counted as succeeded
    DownloadStats stats2 = countDownloadResult(5, 0);
    TEST_ASSERT_EQUAL_UINT8(1, stats2.nodesSucceeded);
    TEST_ASSERT_EQUAL_UINT8(0, stats2.nodesFailed);
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Start state
    RUN_TEST(test_start_with_empty_registry_goes_to_done);
    RUN_TEST(test_start_with_nodes_goes_to_route_discovery);
    RUN_TEST(test_start_without_aodv_skips_route_discovery);

    // Abort
    RUN_TEST(test_abort_marks_all_remaining_failed);
    RUN_TEST(test_abort_from_idle_is_noop);

    // Disconnect / Connect
    RUN_TEST(test_disconnect_goes_to_connect);
    RUN_TEST(test_connect_no_more_nodes_goes_to_done);

    // Download stats
    RUN_TEST(test_download_partial_failure_counts_correctly);

    return UNITY_END();
}
