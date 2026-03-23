/**
 * @file test_election_edge.cpp
 * @brief Unit tests for election edge cases (tiebreakers, GW_RECLAIM, cooldown)
 *
 * Builds on test_election_state by testing edge-case behaviors in the
 * election state machine. Logic is re-implemented inline as pure functions.
 *
 * Run: pio test -e native -f test_election_edge
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#ifdef NATIVE_TEST

// ─── Election states (mirrors ElectionManager.h) ──────────────
enum ElectionState : uint8_t {
    ELECT_IDLE,
    ELECT_ELECTION_START,
    ELECT_WAITING,
    ELECT_PROMOTED,
    ELECT_STOOD_DOWN,
    ELECT_ACTING_GATEWAY,
    ELECT_RECLAIMED,
};

// ─── Node roles ──────────────────────────────────────────────
enum NodeRole : uint8_t {
    NODE_ROLE_LEAF    = 0x01,
    NODE_ROLE_RELAY   = 0x02,
    NODE_ROLE_GATEWAY = 0x03,
};

// ─── Election constants (mirrors ElectionManager.h) ──────────
static constexpr uint32_t ELECTION_GW_TIMEOUT_MS       = 90000;
static constexpr uint32_t ELECTION_RECLAIM_COOLDOWN_MS = 30000;

// ─── Priority from MAC (mirrors ElectionPacket) ──────────────
static uint32_t macToPriority(const uint8_t mac[6]) {
    return mac[2] | (mac[3] << 8) | (mac[4] << 16) | (mac[5] << 24);
}

// ─── Tiebreaker logic (mirrors ElectionManager::onElectionPacket) ──
struct TiebreakerResult {
    bool iAmHigher;
    bool theyAreHigher;
};

static TiebreakerResult evaluateTiebreaker(const uint8_t myMac[6], const uint8_t theirMac[6]) {
    uint32_t myPri = macToPriority(myMac);
    uint32_t theirPri = macToPriority(theirMac);

    TiebreakerResult r;
    r.iAmHigher = (theirPri < myPri) ||
                  (theirPri == myPri && memcmp(theirMac, myMac, 6) < 0);
    r.theyAreHigher = (theirPri > myPri) ||
                      (theirPri == myPri && memcmp(theirMac, myMac, 6) > 0);
    return r;
}

// ─── GW_RECLAIM response (mirrors onElectionPacket GW_RECLAIM case) ──
static ElectionState handleGwReclaim(ElectionState currentState) {
    if (currentState == ELECT_ACTING_GATEWAY) {
        return ELECT_RECLAIMED;
    }
    // Non-acting states: return to IDLE (if not already)
    if (currentState != ELECT_IDLE) {
        return ELECT_IDLE;
    }
    return currentState;
}

// ─── Promotion idempotency (mirrors ELECT_PROMOTED tick) ─────
struct PromotionResult {
    NodeRole activeRole;
    ElectionState nextState;
};

static PromotionResult handlePromotion(NodeRole currentRole) {
    PromotionResult r;
    // Guard: only promote once
    if (currentRole != NODE_ROLE_GATEWAY) {
        r.activeRole = NODE_ROLE_GATEWAY;
    } else {
        r.activeRole = currentRole;  // Already gateway — no change
    }
    r.nextState = ELECT_ACTING_GATEWAY;
    return r;
}

// ─── Cooldown check (mirrors _tickIdle cooldown guard) ───────
static bool isCooldownBlocking(uint32_t nowMs, uint32_t cooldownUntilMs) {
    return nowMs < cooldownUntilMs;
}

// ─── Stale gateway detection (mirrors _tickIdle timeout) ─────
static bool isGatewayStale(uint32_t nowMs, uint32_t lastBeaconMs) {
    return (nowMs - lastBeaconMs) >= ELECTION_GW_TIMEOUT_MS;
}

// ─── Election from stood-down (mirrors onElectionPacket) ─────
static ElectionState handleElectionAsHigher(ElectionState currentState,
                                             uint32_t myPriority,
                                             uint32_t senderPriority) {
    if (senderPriority < myPriority) {
        if (currentState == ELECT_IDLE || currentState == ELECT_STOOD_DOWN) {
            return ELECT_ELECTION_START;
        }
    }
    return currentState;
}

#endif // NATIVE_TEST

// ─── Test setup / teardown ───────────────────────────────────
void setUp(void) {}
void tearDown(void) {}

// ═════════════════════════════════════════════════════════════
// TIEBREAKER TESTS (equal priority, MAC decides)
// ═════════════════════════════════════════════════════════════

void test_equal_priority_tiebreaker_higher_mac_wins() {
    // Same priority bytes (mac[2..5]) but different mac[0..1]
    // won't work since priority only uses mac[2..5].
    // Instead: same mac[2..5] but different mac[0..1].
    // Actually, if priority is equal, we compare the full MAC.
    // Higher MAC (lexicographic) wins.

    // Both have same priority (same mac[2..5])
    uint8_t macA[6] = {0x00, 0x01, 0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t macB[6] = {0x00, 0x02, 0xAA, 0xBB, 0xCC, 0xDD};

    // macA < macB lexicographically (at byte 1: 0x01 < 0x02)
    // So macB is the "higher MAC" and should win
    TiebreakerResult r = evaluateTiebreaker(macB, macA);  // I am macB
    TEST_ASSERT_TRUE(r.iAmHigher);
    TEST_ASSERT_FALSE(r.theyAreHigher);
}

void test_equal_priority_tiebreaker_lower_mac_yields() {
    uint8_t macA[6] = {0x00, 0x01, 0xAA, 0xBB, 0xCC, 0xDD};
    uint8_t macB[6] = {0x00, 0x02, 0xAA, 0xBB, 0xCC, 0xDD};

    // I am macA (the lower MAC), sender is macB (higher MAC)
    TiebreakerResult r = evaluateTiebreaker(macA, macB);
    TEST_ASSERT_FALSE(r.iAmHigher);
    TEST_ASSERT_TRUE(r.theyAreHigher);
}

// ═════════════════════════════════════════════════════════════
// GW_RECLAIM TESTS
// ═════════════════════════════════════════════════════════════

void test_gw_reclaim_during_election_returns_to_idle() {
    ElectionState next = handleGwReclaim(ELECT_ELECTION_START);
    TEST_ASSERT_EQUAL(ELECT_IDLE, next);
}

void test_gw_reclaim_during_acting_gateway_demotes() {
    ElectionState next = handleGwReclaim(ELECT_ACTING_GATEWAY);
    TEST_ASSERT_EQUAL(ELECT_RECLAIMED, next);
}

// ═════════════════════════════════════════════════════════════
// PROMOTION IDEMPOTENCY TEST
// ═════════════════════════════════════════════════════════════

void test_promoted_re_entry_guard() {
    // First promotion: LEAF → GATEWAY
    PromotionResult r1 = handlePromotion(NODE_ROLE_LEAF);
    TEST_ASSERT_EQUAL(NODE_ROLE_GATEWAY, r1.activeRole);
    TEST_ASSERT_EQUAL(ELECT_ACTING_GATEWAY, r1.nextState);

    // Re-entry: already GATEWAY → stays GATEWAY (idempotent)
    PromotionResult r2 = handlePromotion(NODE_ROLE_GATEWAY);
    TEST_ASSERT_EQUAL(NODE_ROLE_GATEWAY, r2.activeRole);
    TEST_ASSERT_EQUAL(ELECT_ACTING_GATEWAY, r2.nextState);
}

// ═════════════════════════════════════════════════════════════
// COOLDOWN TEST
// ═════════════════════════════════════════════════════════════

void test_cooldown_blocks_election_start() {
    uint32_t nowMs = 50000;
    uint32_t cooldownUntilMs = nowMs + ELECTION_RECLAIM_COOLDOWN_MS;

    // During cooldown: should block
    TEST_ASSERT_TRUE(isCooldownBlocking(nowMs, cooldownUntilMs));
    TEST_ASSERT_TRUE(isCooldownBlocking(nowMs + 10000, cooldownUntilMs));

    // After cooldown: should not block
    TEST_ASSERT_FALSE(isCooldownBlocking(cooldownUntilMs, cooldownUntilMs));
    TEST_ASSERT_FALSE(isCooldownBlocking(cooldownUntilMs + 1, cooldownUntilMs));
}

// ═════════════════════════════════════════════════════════════
// STALE GATEWAY DETECTION TEST
// ═════════════════════════════════════════════════════════════

void test_stale_gateway_timestamp_detection() {
    uint32_t lastBeaconMs = 10000;

    // Before timeout: not stale
    TEST_ASSERT_FALSE(isGatewayStale(lastBeaconMs + 89999, lastBeaconMs));

    // At timeout: stale
    TEST_ASSERT_TRUE(isGatewayStale(lastBeaconMs + ELECTION_GW_TIMEOUT_MS, lastBeaconMs));

    // After timeout: stale
    TEST_ASSERT_TRUE(isGatewayStale(lastBeaconMs + ELECTION_GW_TIMEOUT_MS + 5000, lastBeaconMs));
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Tiebreaker
    RUN_TEST(test_equal_priority_tiebreaker_higher_mac_wins);
    RUN_TEST(test_equal_priority_tiebreaker_lower_mac_yields);

    // GW_RECLAIM
    RUN_TEST(test_gw_reclaim_during_election_returns_to_idle);
    RUN_TEST(test_gw_reclaim_during_acting_gateway_demotes);

    // Promotion
    RUN_TEST(test_promoted_re_entry_guard);

    // Cooldown
    RUN_TEST(test_cooldown_blocks_election_start);

    // Stale gateway
    RUN_TEST(test_stale_gateway_timestamp_detection);

    return UNITY_END();
}
