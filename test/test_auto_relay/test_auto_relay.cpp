/**
 * @file test_auto_relay.cpp
 * @brief Unit tests for auto-relay promotion logic during election
 *
 * Validates that nodes which sent SUPPRESS but lost the election
 * are automatically promoted to RELAY role, while non-suppressing
 * losers remain LEAF and the winner becomes GATEWAY.
 *
 * Run: pio test -e native -f test_auto_relay
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#ifdef NATIVE_TEST

// ─── Node roles (mirrors LoRaBeacon.h) ──────────────────────
enum NodeRole : uint8_t {
    NODE_ROLE_LEAF    = 0x01,
    NODE_ROLE_RELAY   = 0x02,
    NODE_ROLE_GATEWAY = 0x03,
};

// ─── Priority helper (mirrors ElectionPacket.cpp) ───────────
static uint32_t macToPriority(const uint8_t mac[6]) {
    return mac[2] | (mac[3] << 8) | (mac[4] << 16) | (mac[5] << 24);
}

// ─── Simulated election node state ──────────────────────────
struct SimNode {
    uint8_t  mac[6];
    uint32_t priority;
    NodeRole role;
    bool     sentSuppressDuringElection;   // set true when node sends SUPPRESS
    bool     receivedCoordinator;          // set true when COORDINATOR is received
};

static void initNode(SimNode& n, const uint8_t mac[6]) {
    memcpy(n.mac, mac, 6);
    n.priority = macToPriority(mac);
    n.role = NODE_ROLE_LEAF;
    n.sentSuppressDuringElection = false;
    n.receivedCoordinator = false;
}

// ─── Core auto-relay decision (mirrors ElectionManager logic) ─
// Called on a non-winner node when it receives COORDINATOR.
// If it sent at least one SUPPRESS during this election round,
// it proved it has higher priority than some peer -> promote to RELAY.
static NodeRole resolvePostElectionRole(const SimNode& n, bool isWinner) {
    if (isWinner) {
        return NODE_ROLE_GATEWAY;
    }
    if (n.sentSuppressDuringElection) {
        return NODE_ROLE_RELAY;
    }
    return NODE_ROLE_LEAF;
}

// ─── Simulate sending SUPPRESS to a lower-priority peer ─────
static void simulateSendSuppress(SimNode& sender, const SimNode& /*target*/) {
    sender.sentSuppressDuringElection = true;
}

// ─── Simulate receiving COORDINATOR (election over) ─────────
static void simulateReceiveCoordinator(SimNode& n, bool isWinner) {
    n.receivedCoordinator = true;
    n.role = resolvePostElectionRole(n, isWinner);
}

// ─── Reset flag on new election start ───────────────────────
static void simulateNewElectionStart(SimNode& n) {
    n.sentSuppressDuringElection = false;
    n.receivedCoordinator = false;
}

// ─── Demotion (e.g. GW_RECLAIM or route expiry) ────────────
static void simulateDemoteToLeaf(SimNode& n) {
    n.role = NODE_ROLE_LEAF;
    n.sentSuppressDuringElection = false;
}

#endif // NATIVE_TEST

void setUp(void) {}
void tearDown(void) {}

// ═════════════════════════════════════════════════════════════
// 1. BASIC PROMOTION: sent SUPPRESS + received COORDINATOR -> RELAY
// ═════════════════════════════════════════════════════════════

void test_suppress_sender_becomes_relay() {
    uint8_t macMid[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x50};
    SimNode node;
    initNode(node, macMid);

    // Node suppressed a lower-priority peer during election
    SimNode dummyLow;
    uint8_t macLow[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    initNode(dummyLow, macLow);
    simulateSendSuppress(node, dummyLow);

    // Election ends, this node is NOT the winner
    simulateReceiveCoordinator(node, false);

    TEST_ASSERT_EQUAL(NODE_ROLE_RELAY, node.role);
}

// ═════════════════════════════════════════════════════════════
// 2. NO FALSE PROMOTION: never sent SUPPRESS -> stays LEAF
// ═════════════════════════════════════════════════════════════

void test_no_suppress_stays_leaf() {
    uint8_t macLow[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    SimNode node;
    initNode(node, macLow);

    // Node never sent SUPPRESS (lowest priority, nobody to suppress)
    TEST_ASSERT_FALSE(node.sentSuppressDuringElection);

    // Election ends, not the winner
    simulateReceiveCoordinator(node, false);

    TEST_ASSERT_EQUAL(NODE_ROLE_LEAF, node.role);
}

// ═════════════════════════════════════════════════════════════
// 3. GATEWAY WINS: highest priority -> GATEWAY (not RELAY)
// ═════════════════════════════════════════════════════════════

void test_winner_becomes_gateway_not_relay() {
    uint8_t macHigh[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x90};
    SimNode node;
    initNode(node, macHigh);

    // Winner also sent SUPPRESS to everyone else during election
    SimNode dummyLow;
    uint8_t macLow[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    initNode(dummyLow, macLow);
    simulateSendSuppress(node, dummyLow);

    // This node IS the winner
    simulateReceiveCoordinator(node, true);

    TEST_ASSERT_EQUAL(NODE_ROLE_GATEWAY, node.role);
    TEST_ASSERT_NOT_EQUAL(NODE_ROLE_RELAY, node.role);
}

// ═════════════════════════════════════════════════════════════
// 4. THREE-NODE SCENARIO SIMULATION
//    A (0x10) -> LEAF, B (0x50) -> RELAY, C (0x90) -> GATEWAY
// ═════════════════════════════════════════════════════════════

void test_three_node_scenario() {
    uint8_t macA[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10};  // lowest
    uint8_t macB[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x50};  // middle
    uint8_t macC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x90};  // highest

    SimNode nodeA, nodeB, nodeC;
    initNode(nodeA, macA);
    initNode(nodeB, macB);
    initNode(nodeC, macC);

    // Verify priority ordering
    TEST_ASSERT_TRUE(nodeC.priority > nodeB.priority);
    TEST_ASSERT_TRUE(nodeB.priority > nodeA.priority);

    // Election round: higher-priority nodes suppress lower ones
    // B suppresses A
    simulateSendSuppress(nodeB, nodeA);
    // C suppresses A and B
    simulateSendSuppress(nodeC, nodeA);
    simulateSendSuppress(nodeC, nodeB);
    // A never suppresses anyone (lowest priority)

    // C wins the election
    simulateReceiveCoordinator(nodeC, true);   // winner
    simulateReceiveCoordinator(nodeB, false);   // loser, but suppressed A
    simulateReceiveCoordinator(nodeA, false);   // loser, never suppressed

    // Verify roles
    TEST_ASSERT_EQUAL(NODE_ROLE_GATEWAY, nodeC.role);  // winner -> GATEWAY
    TEST_ASSERT_EQUAL(NODE_ROLE_RELAY,   nodeB.role);  // suppressed A -> RELAY
    TEST_ASSERT_EQUAL(NODE_ROLE_LEAF,    nodeA.role);  // never suppressed -> LEAF
}

// ═════════════════════════════════════════════════════════════
// 5. FLAG RESET ON NEW ELECTION
// ═════════════════════════════════════════════════════════════

void test_flag_resets_on_new_election() {
    uint8_t mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x50};
    SimNode node;
    initNode(node, mac);

    // First election: node sends SUPPRESS
    SimNode dummyLow;
    uint8_t macLow[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    initNode(dummyLow, macLow);
    simulateSendSuppress(node, dummyLow);
    TEST_ASSERT_TRUE(node.sentSuppressDuringElection);

    // New election starts: flag must reset
    simulateNewElectionStart(node);
    TEST_ASSERT_FALSE(node.sentSuppressDuringElection);
    TEST_ASSERT_FALSE(node.receivedCoordinator);
}

// ═════════════════════════════════════════════════════════════
// 6. FLAG RESET ON DEMOTION
// ═════════════════════════════════════════════════════════════

void test_flag_resets_on_demotion() {
    uint8_t mac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x50};
    SimNode node;
    initNode(node, mac);

    // Node was promoted to RELAY
    SimNode dummyLow;
    uint8_t macLow[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    initNode(dummyLow, macLow);
    simulateSendSuppress(node, dummyLow);
    simulateReceiveCoordinator(node, false);
    TEST_ASSERT_EQUAL(NODE_ROLE_RELAY, node.role);
    TEST_ASSERT_TRUE(node.sentSuppressDuringElection);

    // Demotion clears everything
    simulateDemoteToLeaf(node);
    TEST_ASSERT_EQUAL(NODE_ROLE_LEAF, node.role);
    TEST_ASSERT_FALSE(node.sentSuppressDuringElection);
}

// ═════════════════════════════════════════════════════════════
// 7. TWO-NODE SCENARIO: winner -> GATEWAY, loser -> LEAF
// ═════════════════════════════════════════════════════════════

void test_two_node_scenario() {
    uint8_t macHigh[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x90};
    uint8_t macLow[6]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10};

    SimNode winner, loser;
    initNode(winner, macHigh);
    initNode(loser, macLow);

    // Winner suppresses loser; loser never suppresses anyone
    simulateSendSuppress(winner, loser);

    simulateReceiveCoordinator(winner, true);
    simulateReceiveCoordinator(loser, false);

    TEST_ASSERT_EQUAL(NODE_ROLE_GATEWAY, winner.role);
    TEST_ASSERT_EQUAL(NODE_ROLE_LEAF,    loser.role);  // no relay needed
    TEST_ASSERT_FALSE(loser.sentSuppressDuringElection);
}

// ═════════════════════════════════════════════════════════════
// 8. MULTIPLE SUPPRESSIONS: still RELAY, not GATEWAY
// ═════════════════════════════════════════════════════════════

void test_multiple_suppressions_still_relay() {
    uint8_t macMid[6]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x50};
    uint8_t macLo1[6]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    uint8_t macLo2[6]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x20};
    uint8_t macLo3[6]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x30};

    SimNode node, low1, low2, low3;
    initNode(node, macMid);
    initNode(low1, macLo1);
    initNode(low2, macLo2);
    initNode(low3, macLo3);

    // Node suppressed multiple candidates
    simulateSendSuppress(node, low1);
    simulateSendSuppress(node, low2);
    simulateSendSuppress(node, low3);
    TEST_ASSERT_TRUE(node.sentSuppressDuringElection);

    // But it is NOT the winner (someone with higher priority won)
    simulateReceiveCoordinator(node, false);

    TEST_ASSERT_EQUAL(NODE_ROLE_RELAY, node.role);
    TEST_ASSERT_NOT_EQUAL(NODE_ROLE_GATEWAY, node.role);
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Basic promotion
    RUN_TEST(test_suppress_sender_becomes_relay);

    // No false promotion
    RUN_TEST(test_no_suppress_stays_leaf);

    // Gateway wins
    RUN_TEST(test_winner_becomes_gateway_not_relay);

    // 3-node scenario
    RUN_TEST(test_three_node_scenario);

    // Flag reset on new election
    RUN_TEST(test_flag_resets_on_new_election);

    // Flag reset on demotion
    RUN_TEST(test_flag_resets_on_demotion);

    // 2-node scenario
    RUN_TEST(test_two_node_scenario);

    // Multiple suppressions
    RUN_TEST(test_multiple_suppressions_still_relay);

    return UNITY_END();
}
