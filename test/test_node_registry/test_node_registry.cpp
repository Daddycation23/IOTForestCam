/**
 * @file test_node_registry.cpp
 * @brief Unit tests for NodeRegistry pure logic (weakest slot, expiry)
 *
 * Uses inline copies of data structures for native testing without
 * Arduino/hardware dependencies.
 *
 * Run: pio test -e native -f test_node_registry
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#ifdef NATIVE_TEST

// ─── Configuration (mirrors NodeRegistry.h) ──────────────────
static constexpr uint8_t  REGISTRY_MAX_NODES = 8;
static constexpr uint32_t REGISTRY_EXPIRY_MS = 120000;  // 2 minutes

// ─── Simplified node entry for native testing ────────────────
struct TestNodeEntry {
    uint8_t  nodeId[6];
    float    rssi;
    uint32_t lastSeenMs;
    bool     active;
};

// ─── Simplified registry for native testing ──────────────────
class TestNodeRegistry {
public:
    TestNodeEntry nodes[REGISTRY_MAX_NODES];

    TestNodeRegistry() {
        for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
            nodes[i].active     = false;
            nodes[i].rssi       = -999.0f;
            nodes[i].lastSeenMs = 0;
            memset(nodes[i].nodeId, 0, 6);
        }
    }

    /** Find the slot with the weakest (most negative) RSSI among active nodes. */
    int8_t findWeakestSlot() const {
        int8_t weakestIdx  = -1;
        float  weakestRSSI = 0.0f;

        for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
            if (nodes[i].active) {
                if (weakestIdx < 0 || nodes[i].rssi < weakestRSSI) {
                    weakestRSSI = nodes[i].rssi;
                    weakestIdx  = i;
                }
            }
        }
        return weakestIdx;
    }

    /** Remove entries not heard for REGISTRY_EXPIRY_MS. */
    void expireStale(uint32_t nowMs) {
        for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
            if (nodes[i].active && (nowMs - nodes[i].lastSeenMs) > REGISTRY_EXPIRY_MS) {
                nodes[i].active = false;
            }
        }
    }

    /** Count active slots. */
    uint8_t activeCount() const {
        uint8_t count = 0;
        for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
            if (nodes[i].active) count++;
        }
        return count;
    }
};

#endif // NATIVE_TEST

// ─── Test setup / teardown ───────────────────────────────────
void setUp(void) {}
void tearDown(void) {}

// ═════════════════════════════════════════════════════════════
// WEAKEST SLOT TESTS
// ═════════════════════════════════════════════════════════════

void test_weakest_slot_returns_lowest_rssi() {
    TestNodeRegistry reg;

    // Slot 0: RSSI -60
    reg.nodes[0].active = true;
    reg.nodes[0].rssi   = -60.0f;

    // Slot 1: RSSI -90 (weakest)
    reg.nodes[1].active = true;
    reg.nodes[1].rssi   = -90.0f;

    // Slot 2: RSSI -75
    reg.nodes[2].active = true;
    reg.nodes[2].rssi   = -75.0f;

    int8_t weakest = reg.findWeakestSlot();
    TEST_ASSERT_EQUAL_INT8(1, weakest);
    TEST_ASSERT_EQUAL_FLOAT(-90.0f, reg.nodes[weakest].rssi);
}

void test_weakest_slot_with_all_same_rssi() {
    TestNodeRegistry reg;

    // All three slots have the same RSSI
    for (uint8_t i = 0; i < 3; i++) {
        reg.nodes[i].active = true;
        reg.nodes[i].rssi   = -70.0f;
    }

    // When all equal, the first active slot wins (index 0)
    int8_t weakest = reg.findWeakestSlot();
    TEST_ASSERT_EQUAL_INT8(0, weakest);
}

// ═════════════════════════════════════════════════════════════
// EXPIRY TESTS
// ═════════════════════════════════════════════════════════════

void test_expire_stale_removes_old_entries() {
    TestNodeRegistry reg;

    // Slot 0: seen at time 0 — should expire at time > REGISTRY_EXPIRY_MS
    reg.nodes[0].active     = true;
    reg.nodes[0].rssi       = -50.0f;
    reg.nodes[0].lastSeenMs = 0;

    // Slot 1: seen at time 100000 — should NOT expire at time 150000
    reg.nodes[1].active     = true;
    reg.nodes[1].rssi       = -60.0f;
    reg.nodes[1].lastSeenMs = 100000;

    // Slot 2: seen at time 10000 — should expire at time 150000
    reg.nodes[2].active     = true;
    reg.nodes[2].rssi       = -70.0f;
    reg.nodes[2].lastSeenMs = 10000;

    TEST_ASSERT_EQUAL(3, reg.activeCount());

    // Advance to 150000 ms — entries older than 120000 ms are stale
    reg.expireStale(150000);

    // Slot 0: lastSeen=0, age=150000 > 120000 → expired
    TEST_ASSERT_FALSE(reg.nodes[0].active);

    // Slot 1: lastSeen=100000, age=50000 < 120000 → still active
    TEST_ASSERT_TRUE(reg.nodes[1].active);

    // Slot 2: lastSeen=10000, age=140000 > 120000 → expired
    TEST_ASSERT_FALSE(reg.nodes[2].active);

    TEST_ASSERT_EQUAL(1, reg.activeCount());
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Weakest slot
    RUN_TEST(test_weakest_slot_returns_lowest_rssi);
    RUN_TEST(test_weakest_slot_with_all_same_rssi);

    // Expiry
    RUN_TEST(test_expire_stale_removes_old_entries);

    return UNITY_END();
}
