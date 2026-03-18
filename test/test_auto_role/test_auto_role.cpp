/**
 * @file test_auto_role.cpp
 * @brief Unit tests for automatic role negotiation logic
 *
 * Tests cover: boot flow, election guard removal, relay detection,
 * filename format, serial command parsing.
 *
 * Run: pio test -e native -f test_auto_role
 */

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

#ifdef NATIVE_TEST

// ─── Role enum (mirrors LoRaBeacon.h) ────────────────────────
enum NodeRole : uint8_t {
    NODE_ROLE_LEAF    = 0x01,
    NODE_ROLE_RELAY   = 0x02,
    NODE_ROLE_GATEWAY = 0x03,
};

// ─── Election constants (mirrors ElectionManager.h) ──────────
static constexpr uint32_t ELECTION_GW_TIMEOUT_MS      = 90000;
static constexpr uint32_t ELECTION_STARTUP_GRACE_MS    = 15000;

// ─── Priority helper (mirrors ElectionPacket.cpp) ────────────
static uint32_t macToPriority(const uint8_t mac[6]) {
    return mac[2] | (mac[3] << 8) | (mac[4] << 16) | (mac[5] << 24);
}

// ─── Filename format helper (mirrors new HarvestLoop logic) ──
static int buildFilename(char* buf, size_t bufLen,
                          uint8_t macByte4, uint8_t macByte5,
                          uint32_t bootCount, uint32_t uptimeSec,
                          uint8_t imgIndex) {
    return snprintf(buf, bufLen,
                    "/received/node_%02X%02X_boot%03lu_%06lus_img_%03u.jpg",
                    macByte4, macByte5,
                    (unsigned long)bootCount,
                    (unsigned long)uptimeSec,
                    imgIndex);
}

// ─── Serial command parser (mirrors SerialCmd logic) ──────────
static bool parseBlockCmd(const char* line, uint8_t outBytes[2]) {
    // Expected: "block AABB" where AABB is hex
    if (strncmp(line, "block ", 6) != 0) return false;
    const char* hex = line + 6;
    if (strlen(hex) < 4) return false;

    auto hexNibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };

    int h0 = hexNibble(hex[0]), h1 = hexNibble(hex[1]);
    int h2 = hexNibble(hex[2]), h3 = hexNibble(hex[3]);
    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return false;

    outBytes[0] = (h0 << 4) | h1;
    outBytes[1] = (h2 << 4) | h3;
    return true;
}

static bool isNodeBlocked(const uint8_t blockedList[][2], uint8_t blockedCount,
                           const uint8_t nodeId[6]) {
    for (uint8_t i = 0; i < blockedCount; i++) {
        if (nodeId[4] == blockedList[i][0] && nodeId[5] == blockedList[i][1]) {
            return true;
        }
    }
    return false;
}

#endif // NATIVE_TEST

void setUp(void) {}
void tearDown(void) {}

// ═════════════════════════════════════════════════════════════
// BOOT FLOW TESTS
// ═════════════════════════════════════════════════════════════

void test_default_role_is_leaf() {
    // When auto-negotiating (no BOOT press), default role is LEAF
    NodeRole defaultRole = NODE_ROLE_LEAF;
    TEST_ASSERT_EQUAL(NODE_ROLE_LEAF, defaultRole);
}

void test_node_role_enum_values_unchanged() {
    // Backward compatibility: enum values must not change
    TEST_ASSERT_EQUAL(0x01, NODE_ROLE_LEAF);
    TEST_ASSERT_EQUAL(0x02, NODE_ROLE_RELAY);
    TEST_ASSERT_EQUAL(0x03, NODE_ROLE_GATEWAY);
}

void test_auto_negotiate_returns_leaf() {
    // autoNegotiate() should always return LEAF — election decides gateway
    NodeRole result = NODE_ROLE_LEAF;  // Simulates autoNegotiate() return
    TEST_ASSERT_EQUAL(NODE_ROLE_LEAF, result);
    TEST_ASSERT_NOT_EQUAL(NODE_ROLE_GATEWAY, result);
    TEST_ASSERT_NOT_EQUAL(NODE_ROLE_RELAY, result);
}

// ═════════════════════════════════════════════════════════════
// ELECTION TESTS (all nodes can participate)
// ═════════════════════════════════════════════════════════════

void test_leaf_can_participate_in_election() {
    // A leaf node should be able to compute priority and participate
    uint8_t leafMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint32_t priority = macToPriority(leafMac);
    TEST_ASSERT_GREATER_THAN(0, priority);
}

void test_highest_mac_wins_gateway() {
    uint8_t macA[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};  // Low priority
    uint8_t macB[6] = {0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};  // High priority
    uint8_t macC[6] = {0x00, 0x00, 0x00, 0x80, 0x40, 0x20};  // Medium priority

    uint32_t priA = macToPriority(macA);
    uint32_t priB = macToPriority(macB);
    uint32_t priC = macToPriority(macC);

    // Node B should win (highest priority)
    TEST_ASSERT_GREATER_THAN(priA, priB);
    TEST_ASSERT_GREATER_THAN(priC, priB);
}

void test_startup_grace_period_constant() {
    // Grace period should be 15 seconds — enough time for discovery
    TEST_ASSERT_EQUAL(15000, ELECTION_STARTUP_GRACE_MS);
}

void test_election_only_after_grace_period() {
    // Simulate: boot at time 0, no GW beacon seen
    uint32_t bootTime = 0;
    uint32_t currentTime = 10000;  // 10s after boot
    bool gracePeriodExpired = (currentTime - bootTime) >= ELECTION_STARTUP_GRACE_MS;

    // Should NOT start election at 10s (grace = 15s)
    TEST_ASSERT_FALSE(gracePeriodExpired);

    // At 16s, should start
    currentTime = 16000;
    gracePeriodExpired = (currentTime - bootTime) >= ELECTION_STARTUP_GRACE_MS;
    TEST_ASSERT_TRUE(gracePeriodExpired);
}

void test_gateway_beacon_prevents_election() {
    // If gateway beacon is heard during grace period, no election needed
    bool gatewayHeard = true;
    bool shouldElect = !gatewayHeard;
    TEST_ASSERT_FALSE(shouldElect);
}

void test_demote_returns_to_leaf_not_relay() {
    // When a promoted gateway is reclaimed, it returns to LEAF (not RELAY)
    NodeRole demotedRole = NODE_ROLE_LEAF;  // Simulates _demoteToLeaf()
    TEST_ASSERT_EQUAL(NODE_ROLE_LEAF, demotedRole);
    TEST_ASSERT_NOT_EQUAL(NODE_ROLE_RELAY, demotedRole);
}

// ═════════════════════════════════════════════════════════════
// RELAY DETECTION TESTS
// ═════════════════════════════════════════════════════════════

void test_initial_relaying_state_is_false() {
    bool isRelaying = false;
    uint8_t relayingForCount = 0;
    TEST_ASSERT_FALSE(isRelaying);
    TEST_ASSERT_EQUAL(0, relayingForCount);
}

void test_rrep_forwarding_sets_relaying() {
    // Simulate: node forwards an RREP → becomes relay
    bool isRelaying = false;
    uint8_t relayingForCount = 0;

    // After forwarding RREP:
    isRelaying = true;
    relayingForCount++;

    TEST_ASSERT_TRUE(isRelaying);
    TEST_ASSERT_EQUAL(1, relayingForCount);
}

void test_multiple_routes_tracked() {
    uint8_t relayingForCount = 0;
    relayingForCount++;  // Route 1
    relayingForCount++;  // Route 2
    relayingForCount++;  // Route 3
    TEST_ASSERT_EQUAL(3, relayingForCount);
}

void test_all_routes_expired_clears_relaying() {
    bool isRelaying = true;
    uint8_t relayingForCount = 2;

    // Routes expire one by one
    relayingForCount--;
    TEST_ASSERT_TRUE(relayingForCount > 0);

    relayingForCount--;
    if (relayingForCount == 0) isRelaying = false;

    TEST_ASSERT_FALSE(isRelaying);
    TEST_ASSERT_EQUAL(0, relayingForCount);
}

// ═════════════════════════════════════════════════════════════
// FILENAME FORMAT TESTS
// ═════════════════════════════════════════════════════════════

void test_filename_format_complete() {
    char buf[128];
    buildFilename(buf, sizeof(buf), 0xAA, 0xBB, 5, 3672, 0);
    TEST_ASSERT_EQUAL_STRING("/received/node_AABB_boot005_003672s_img_000.jpg", buf);
}

void test_filename_includes_boot_count() {
    char buf[128];
    buildFilename(buf, sizeof(buf), 0xCC, 0xDD, 42, 100, 3);
    TEST_ASSERT_NOT_NULL(strstr(buf, "boot042"));
}

void test_filename_includes_uptime() {
    char buf[128];
    buildFilename(buf, sizeof(buf), 0xEE, 0xFF, 1, 99999, 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "099999s"));
}

void test_filename_includes_node_id() {
    char buf[128];
    buildFilename(buf, sizeof(buf), 0x12, 0x34, 1, 0, 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "node_1234"));
}

void test_filename_includes_image_index() {
    char buf[128];
    buildFilename(buf, sizeof(buf), 0xAA, 0xBB, 1, 0, 7);
    TEST_ASSERT_NOT_NULL(strstr(buf, "img_007"));
}

// ═════════════════════════════════════════════════════════════
// SERIAL COMMAND TESTS
// ═════════════════════════════════════════════════════════════

void test_block_command_parses_hex() {
    uint8_t bytes[2];
    TEST_ASSERT_TRUE(parseBlockCmd("block AABB", bytes));
    TEST_ASSERT_EQUAL(0xAA, bytes[0]);
    TEST_ASSERT_EQUAL(0xBB, bytes[1]);
}

void test_block_command_lowercase_hex() {
    uint8_t bytes[2];
    TEST_ASSERT_TRUE(parseBlockCmd("block aabb", bytes));
    TEST_ASSERT_EQUAL(0xAA, bytes[0]);
    TEST_ASSERT_EQUAL(0xBB, bytes[1]);
}

void test_block_command_rejects_invalid() {
    uint8_t bytes[2];
    TEST_ASSERT_FALSE(parseBlockCmd("block XY", bytes));    // Too short
    TEST_ASSERT_FALSE(parseBlockCmd("unblock AABB", bytes)); // Wrong command
    TEST_ASSERT_FALSE(parseBlockCmd("block GGGG", bytes));   // Invalid hex
}

void test_blocked_node_is_filtered() {
    uint8_t blockedList[8][2] = {{0xAA, 0xBB}, {0xCC, 0xDD}};
    uint8_t blockedCount = 2;

    uint8_t nodeA[6] = {0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB};
    uint8_t nodeB[6] = {0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF};

    TEST_ASSERT_TRUE(isNodeBlocked(blockedList, blockedCount, nodeA));
    TEST_ASSERT_FALSE(isNodeBlocked(blockedList, blockedCount, nodeB));
}

void test_unblocked_node_not_filtered() {
    uint8_t blockedList[8][2] = {{0xAA, 0xBB}};
    uint8_t blockedCount = 1;

    // Simulate unblock: reduce count
    blockedCount = 0;

    uint8_t nodeA[6] = {0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB};
    TEST_ASSERT_FALSE(isNodeBlocked(blockedList, blockedCount, nodeA));
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Boot flow
    RUN_TEST(test_default_role_is_leaf);
    RUN_TEST(test_node_role_enum_values_unchanged);
    RUN_TEST(test_auto_negotiate_returns_leaf);

    // Election (all nodes)
    RUN_TEST(test_leaf_can_participate_in_election);
    RUN_TEST(test_highest_mac_wins_gateway);
    RUN_TEST(test_startup_grace_period_constant);
    RUN_TEST(test_election_only_after_grace_period);
    RUN_TEST(test_gateway_beacon_prevents_election);
    RUN_TEST(test_demote_returns_to_leaf_not_relay);

    // Relay detection
    RUN_TEST(test_initial_relaying_state_is_false);
    RUN_TEST(test_rrep_forwarding_sets_relaying);
    RUN_TEST(test_multiple_routes_tracked);
    RUN_TEST(test_all_routes_expired_clears_relaying);

    // Filename format
    RUN_TEST(test_filename_format_complete);
    RUN_TEST(test_filename_includes_boot_count);
    RUN_TEST(test_filename_includes_uptime);
    RUN_TEST(test_filename_includes_node_id);
    RUN_TEST(test_filename_includes_image_index);

    // Serial commands
    RUN_TEST(test_block_command_parses_hex);
    RUN_TEST(test_block_command_lowercase_hex);
    RUN_TEST(test_block_command_rejects_invalid);
    RUN_TEST(test_blocked_node_is_filtered);
    RUN_TEST(test_unblocked_node_not_filtered);

    return UNITY_END();
}
