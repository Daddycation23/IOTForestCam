/**
 * @file test_node_registry_announce.cpp
 * @brief Unit tests for announce-based registry updates
 *
 * Re-implements a simplified version of NodeRegistry::updateFromAnnounce
 * logic inline for native testing without ESP32 hardware dependencies.
 *
 * Run: pio test -e native -f test_node_registry_announce
 */

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

#ifdef NATIVE_TEST

// ─── Constants (mirrors NodeRegistry.h) ──────────────────────
static constexpr uint8_t REGISTRY_MAX_NODES = 8;
static constexpr uint8_t BEACON_MAX_SSID    = 20;

enum NodeRole : uint8_t {
    NODE_ROLE_LEAF    = 0x01,
    NODE_ROLE_RELAY   = 0x02,
    NODE_ROLE_GATEWAY = 0x03,
};

// ─── Simplified NodeEntry (mirrors NodeRegistry.h) ───────────
struct NodeEntry {
    uint8_t  nodeId[6];
    uint8_t  nodeRole;
    char     ssid[BEACON_MAX_SSID + 1];
    uint8_t  imageCount;
    float    rssi;
    uint32_t lastSeenMs;
    bool     harvested;
    bool     active;
    uint8_t  announcedIP[4];
    uint8_t  hopCount;
    uint8_t  nextHopId[6];
    bool     routeKnown;
};

// ─── Simplified Registry (mirrors NodeRegistry core logic) ───
class TestRegistry {
public:
    NodeEntry nodes[REGISTRY_MAX_NODES];

    TestRegistry() {
        for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
            nodes[i].active = false;
            nodes[i].harvested = false;
            nodes[i].hopCount = 1;
            nodes[i].routeKnown = false;
            memset(nodes[i].nodeId, 0, 6);
            memset(nodes[i].announcedIP, 0, 4);
            memset(nodes[i].nextHopId, 0, 6);
            memset(nodes[i].ssid, 0, sizeof(nodes[i].ssid));
        }
    }

    bool updateFromAnnounce(const uint8_t nodeId[6], const uint8_t ip[4],
                             uint8_t imageCount, uint32_t nowMs) {
        int8_t idx = findNode(nodeId);

        if (idx >= 0) {
            // Existing node: update IP and mark active
            NodeEntry& node = nodes[idx];
            memcpy(node.announcedIP, ip, 4);
            node.imageCount = imageCount;
            node.lastSeenMs = nowMs;
            node.active     = true;
            node.harvested  = false;
            return false;  // Not new
        }

        // New node from announce
        idx = findEmptySlot();
        if (idx < 0) return false;  // Registry full

        NodeEntry& node = nodes[idx];
        memcpy(node.nodeId, nodeId, 6);
        node.nodeRole   = NODE_ROLE_LEAF;
        snprintf(node.ssid, sizeof(node.ssid), "ForestCam-%02X%02X", nodeId[4], nodeId[5]);
        node.imageCount = imageCount;
        node.rssi       = 0;
        node.lastSeenMs = nowMs;
        node.harvested  = false;
        node.active     = true;
        memcpy(node.announcedIP, ip, 4);
        node.hopCount   = 1;
        node.routeKnown = false;
        memset(node.nextHopId, 0, 6);

        return true;
    }

    uint8_t activeCount() const {
        uint8_t count = 0;
        for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
            if (nodes[i].active) count++;
        }
        return count;
    }

private:
    int8_t findNode(const uint8_t nodeId[6]) const {
        for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
            if (nodes[i].active && memcmp(nodes[i].nodeId, nodeId, 6) == 0) {
                return i;
            }
        }
        return -1;
    }

    int8_t findEmptySlot() const {
        for (uint8_t i = 0; i < REGISTRY_MAX_NODES; i++) {
            if (!nodes[i].active) return i;
        }
        return -1;
    }
};

// ─── Test instance ───────────────────────────────────────────
static TestRegistry* registry = nullptr;

#endif // NATIVE_TEST

// ─── Test setup / teardown ───────────────────────────────────
void setUp(void) {
    registry = new TestRegistry();
}

void tearDown(void) {
    delete registry;
    registry = nullptr;
}

// ═════════════════════════════════════════════════════════════
// ANNOUNCE UPDATE TESTS
// ═════════════════════════════════════════════════════════════

void test_update_from_announce_new_node() {
    uint8_t nodeId[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t ip[4] = {192, 168, 4, 100};

    bool isNew = registry->updateFromAnnounce(nodeId, ip, 5, 1000);
    TEST_ASSERT_TRUE(isNew);
    TEST_ASSERT_EQUAL_UINT8(1, registry->activeCount());
}

void test_update_from_announce_existing_node() {
    uint8_t nodeId[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t ip1[4] = {192, 168, 4, 100};
    uint8_t ip2[4] = {192, 168, 4, 200};

    // First announce: new
    bool isNew1 = registry->updateFromAnnounce(nodeId, ip1, 5, 1000);
    TEST_ASSERT_TRUE(isNew1);

    // Second announce: existing (updated)
    bool isNew2 = registry->updateFromAnnounce(nodeId, ip2, 8, 2000);
    TEST_ASSERT_FALSE(isNew2);
    TEST_ASSERT_EQUAL_UINT8(1, registry->activeCount());

    // Verify IP was updated
    TEST_ASSERT_EQUAL_UINT8(192, registry->nodes[0].announcedIP[0]);
    TEST_ASSERT_EQUAL_UINT8(200, registry->nodes[0].announcedIP[3]);
    TEST_ASSERT_EQUAL_UINT8(8, registry->nodes[0].imageCount);
}

void test_update_from_announce_derives_ssid() {
    uint8_t nodeId[6] = {0x11, 0x22, 0x33, 0x44, 0xAB, 0xCD};
    uint8_t ip[4] = {192, 168, 4, 50};

    registry->updateFromAnnounce(nodeId, ip, 3, 1000);
    TEST_ASSERT_EQUAL_STRING("ForestCam-ABCD", registry->nodes[0].ssid);
}

void test_announce_ip_stored_correctly() {
    uint8_t nodeId[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t ip[4] = {10, 0, 1, 42};

    registry->updateFromAnnounce(nodeId, ip, 2, 1000);

    TEST_ASSERT_EQUAL_UINT8(10, registry->nodes[0].announcedIP[0]);
    TEST_ASSERT_EQUAL_UINT8(0,  registry->nodes[0].announcedIP[1]);
    TEST_ASSERT_EQUAL_UINT8(1,  registry->nodes[0].announcedIP[2]);
    TEST_ASSERT_EQUAL_UINT8(42, registry->nodes[0].announcedIP[3]);
}

void test_announce_sets_active() {
    uint8_t nodeId[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t ip[4] = {192, 168, 4, 100};

    TEST_ASSERT_EQUAL_UINT8(0, registry->activeCount());

    registry->updateFromAnnounce(nodeId, ip, 1, 1000);

    TEST_ASSERT_EQUAL_UINT8(1, registry->activeCount());
    TEST_ASSERT_TRUE(registry->nodes[0].active);
    TEST_ASSERT_FALSE(registry->nodes[0].harvested);
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_update_from_announce_new_node);
    RUN_TEST(test_update_from_announce_existing_node);
    RUN_TEST(test_update_from_announce_derives_ssid);
    RUN_TEST(test_announce_ip_stored_correctly);
    RUN_TEST(test_announce_sets_active);

    return UNITY_END();
}
