/**
 * @file test_beacon_v2.cpp
 * @brief Unit tests for the v2 beacon format (no SSID on wire)
 *
 * Re-implements beacon serialize/parse/deriveSsid logic inline for
 * native testing without ESP32 headers.
 *
 * Run: pio test -e native -f test_beacon_v2
 */

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

#ifdef NATIVE_TEST

// ─── Protocol constants (mirrors LoRaBeacon.h) ──────────────
static constexpr uint8_t BEACON_MAGIC     = 0xFC;
static constexpr uint8_t BEACON_VERSION   = 0x02;
static constexpr uint8_t BEACON_MAX_SSID  = 20;
static constexpr uint8_t BEACON_MIN_SIZE  = 15;
static constexpr uint8_t BEACON_V1_MIN_SIZE = 16;

enum BeaconPacketType : uint8_t {
    BEACON_TYPE_BEACON       = 0x01,
    BEACON_TYPE_BEACON_RELAY = 0x02,
};

enum NodeRole : uint8_t {
    NODE_ROLE_LEAF    = 0x01,
    NODE_ROLE_RELAY   = 0x02,
    NODE_ROLE_GATEWAY = 0x03,
};

// ─── Inline BeaconPacket (mirrors LoRaBeacon.h/cpp) ──────────
struct BeaconPacket {
    uint8_t  packetType;
    uint8_t  ttl;
    uint8_t  nodeId[6];
    uint8_t  nodeRole;
    char     ssid[BEACON_MAX_SSID + 1];
    uint8_t  ssidLen;
    uint8_t  imageCount;
    uint8_t  batteryPct;
    uint16_t uptimeMin;

    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const {
        if (maxLen < BEACON_MIN_SIZE) return 0;

        uint8_t pos = 0;
        buf[pos++] = BEACON_MAGIC;
        buf[pos++] = BEACON_VERSION;
        buf[pos++] = packetType;
        buf[pos++] = ttl;
        memcpy(&buf[pos], nodeId, 6);
        pos += 6;
        buf[pos++] = nodeRole;
        buf[pos++] = imageCount;
        buf[pos++] = batteryPct;
        buf[pos++] = uptimeMin & 0xFF;
        buf[pos++] = (uptimeMin >> 8) & 0xFF;
        return pos;
    }

    bool parse(const uint8_t* buf, uint8_t len) {
        if (len < 11) return false;

        uint8_t pos = 0;
        if (buf[pos++] != BEACON_MAGIC) return false;

        uint8_t version = buf[pos++];
        if (version != 0x01 && version != BEACON_VERSION) return false;

        packetType = buf[pos++];
        if (packetType != BEACON_TYPE_BEACON &&
            packetType != BEACON_TYPE_BEACON_RELAY) {
            return false;
        }

        ttl = buf[pos++];
        memcpy(nodeId, &buf[pos], 6);
        pos += 6;
        nodeRole = buf[pos++];

        if (version == 0x01) {
            if (pos >= len) return false;
            ssidLen = buf[pos++];
            if (ssidLen > BEACON_MAX_SSID) return false;
            if (pos + ssidLen + 4 > len) return false;
            memcpy(ssid, &buf[pos], ssidLen);
            ssid[ssidLen] = '\0';
            pos += ssidLen;
            imageCount = buf[pos++];
            batteryPct = buf[pos++];
            uptimeMin = buf[pos] | (buf[pos + 1] << 8);
        } else {
            if (pos + 4 > len) return false;
            imageCount = buf[pos++];
            batteryPct = buf[pos++];
            uptimeMin = buf[pos] | (buf[pos + 1] << 8);
            deriveSsid();
        }

        return true;
    }

    void deriveSsid() {
        if (nodeRole == NODE_ROLE_GATEWAY) {
            snprintf(ssid, sizeof(ssid), "ForestCam-GW-%02X%02X", nodeId[4], nodeId[5]);
        } else {
            snprintf(ssid, sizeof(ssid), "ForestCam-%02X%02X", nodeId[4], nodeId[5]);
        }
        ssidLen = strlen(ssid);
    }
};

// ─── Helper: build a default test beacon ─────────────────────
static BeaconPacket makeTestBeacon(uint8_t role = NODE_ROLE_LEAF) {
    BeaconPacket b;
    memset(&b, 0, sizeof(b));
    b.packetType = BEACON_TYPE_BEACON;
    b.ttl = 2;
    b.nodeId[0] = 0xAA; b.nodeId[1] = 0xBB;
    b.nodeId[2] = 0xCC; b.nodeId[3] = 0xDD;
    b.nodeId[4] = 0xEE; b.nodeId[5] = 0xFF;
    b.nodeRole = role;
    b.imageCount = 3;
    b.batteryPct = 85;
    b.uptimeMin = 1234;
    b.deriveSsid();
    return b;
}

#endif // NATIVE_TEST

// ─── Test setup / teardown ───────────────────────────────────
void setUp(void) {}
void tearDown(void) {}

// ═════════════════════════════════════════════════════════════
// SERIALIZE TESTS
// ═════════════════════════════════════════════════════════════

void test_v2_serialize_fixed_size() {
    BeaconPacket b = makeTestBeacon();
    uint8_t buf[48];
    uint8_t len = b.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(BEACON_MIN_SIZE, len);
    TEST_ASSERT_EQUAL_UINT8(15, len);
}

// ═════════════════════════════════════════════════════════════
// PARSE TESTS
// ═════════════════════════════════════════════════════════════

void test_v2_parse_valid() {
    // Build a v2 packet manually
    uint8_t buf[15] = {
        BEACON_MAGIC, BEACON_VERSION,
        BEACON_TYPE_BEACON, 2,             // type, ttl
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,  // nodeId
        NODE_ROLE_LEAF,                    // role
        3,                                  // imageCount
        85,                                 // batteryPct
        0xD2, 0x04                         // uptimeMin = 1234 (LE)
    };

    BeaconPacket parsed;
    TEST_ASSERT_TRUE(parsed.parse(buf, 15));
    TEST_ASSERT_EQUAL_UINT8(BEACON_TYPE_BEACON, parsed.packetType);
    TEST_ASSERT_EQUAL_UINT8(2, parsed.ttl);
    TEST_ASSERT_EQUAL_UINT8(0xAA, parsed.nodeId[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, parsed.nodeId[5]);
    TEST_ASSERT_EQUAL_UINT8(NODE_ROLE_LEAF, parsed.nodeRole);
    TEST_ASSERT_EQUAL_UINT8(3, parsed.imageCount);
    TEST_ASSERT_EQUAL_UINT8(85, parsed.batteryPct);
    TEST_ASSERT_EQUAL_UINT16(1234, parsed.uptimeMin);
}

void test_v2_derive_ssid_leaf() {
    BeaconPacket b = makeTestBeacon(NODE_ROLE_LEAF);
    TEST_ASSERT_EQUAL_STRING("ForestCam-EEFF", b.ssid);
}

void test_v2_derive_ssid_gateway() {
    BeaconPacket b = makeTestBeacon(NODE_ROLE_GATEWAY);
    TEST_ASSERT_EQUAL_STRING("ForestCam-GW-EEFF", b.ssid);
}

void test_v1_backward_compat() {
    // Build a v1 packet: magic, version=0x01, type, ttl, nodeId(6), role,
    //                    ssidLen, ssid bytes, imageCount, batteryPct, uptimeMin(2)
    const char* ssid = "ForestCam-AABB";
    uint8_t ssidLen = strlen(ssid);

    uint8_t buf[48];
    uint8_t pos = 0;
    buf[pos++] = BEACON_MAGIC;
    buf[pos++] = 0x01;  // v1
    buf[pos++] = BEACON_TYPE_BEACON;
    buf[pos++] = 2;
    // nodeId
    buf[pos++] = 0x11; buf[pos++] = 0x22; buf[pos++] = 0x33;
    buf[pos++] = 0x44; buf[pos++] = 0xAA; buf[pos++] = 0xBB;
    buf[pos++] = NODE_ROLE_LEAF;  // role
    buf[pos++] = ssidLen;
    memcpy(&buf[pos], ssid, ssidLen);
    pos += ssidLen;
    buf[pos++] = 7;     // imageCount
    buf[pos++] = 100;   // batteryPct
    buf[pos++] = 0x00;  // uptimeMin low
    buf[pos++] = 0x01;  // uptimeMin high = 256

    BeaconPacket parsed;
    TEST_ASSERT_TRUE(parsed.parse(buf, pos));
    TEST_ASSERT_EQUAL_STRING("ForestCam-AABB", parsed.ssid);
    TEST_ASSERT_EQUAL_UINT8(7, parsed.imageCount);
    TEST_ASSERT_EQUAL_UINT8(100, parsed.batteryPct);
    TEST_ASSERT_EQUAL_UINT16(256, parsed.uptimeMin);
}

void test_v2_roundtrip() {
    BeaconPacket original = makeTestBeacon(NODE_ROLE_RELAY);
    original.imageCount = 42;
    original.batteryPct = 77;
    original.uptimeMin = 5678;

    uint8_t buf[48];
    uint8_t len = original.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);

    BeaconPacket parsed;
    TEST_ASSERT_TRUE(parsed.parse(buf, len));

    TEST_ASSERT_EQUAL_UINT8(original.packetType, parsed.packetType);
    TEST_ASSERT_EQUAL_UINT8(original.ttl, parsed.ttl);
    TEST_ASSERT_EQUAL_MEMORY(original.nodeId, parsed.nodeId, 6);
    TEST_ASSERT_EQUAL_UINT8(original.nodeRole, parsed.nodeRole);
    TEST_ASSERT_EQUAL_UINT8(original.imageCount, parsed.imageCount);
    TEST_ASSERT_EQUAL_UINT8(original.batteryPct, parsed.batteryPct);
    TEST_ASSERT_EQUAL_UINT16(original.uptimeMin, parsed.uptimeMin);
    TEST_ASSERT_EQUAL_STRING(original.ssid, parsed.ssid);
}

void test_v2_magic_check() {
    uint8_t buf[15] = {
        0x00,  // Wrong magic
        BEACON_VERSION,
        BEACON_TYPE_BEACON, 2,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
        NODE_ROLE_LEAF,
        3, 85, 0xD2, 0x04
    };

    BeaconPacket parsed;
    TEST_ASSERT_FALSE(parsed.parse(buf, 15));
}

void test_v2_too_short() {
    // Less than 15 bytes (but >= 11 for header check, then fails at payload)
    uint8_t buf[14] = {
        BEACON_MAGIC, BEACON_VERSION,
        BEACON_TYPE_BEACON, 2,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
        NODE_ROLE_LEAF,
        3, 85, 0xD2  // Missing last byte
    };

    BeaconPacket parsed;
    TEST_ASSERT_FALSE(parsed.parse(buf, 14));

    // Also test very short (< 11 bytes)
    uint8_t tiny[5] = {BEACON_MAGIC, BEACON_VERSION, 0x01, 0x02, 0x03};
    TEST_ASSERT_FALSE(parsed.parse(tiny, 5));
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_v2_serialize_fixed_size);
    RUN_TEST(test_v2_parse_valid);
    RUN_TEST(test_v2_derive_ssid_leaf);
    RUN_TEST(test_v2_derive_ssid_gateway);
    RUN_TEST(test_v1_backward_compat);
    RUN_TEST(test_v2_roundtrip);
    RUN_TEST(test_v2_magic_check);
    RUN_TEST(test_v2_too_short);

    return UNITY_END();
}
