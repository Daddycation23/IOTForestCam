/**
 * @file test_relay_harvest.cpp
 * @brief Unit tests for relay harvest packet logic (HarvestAckPacket, HarvestCmdPacket)
 *
 * Re-implements packet structs and serialize/parse logic inline so tests
 * can run on the native platform without ESP32 dependencies.
 *
 * Run: pio test -e native -f test_relay_harvest
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#ifdef NATIVE_TEST

// ─── Protocol constants (mirrors AodvPacket.h) ─────────────
static constexpr uint8_t AODV_MAGIC   = 0xFC;
static constexpr uint8_t AODV_VERSION = 0x01;
static constexpr uint8_t PKT_TYPE_HARVEST_CMD = 0x20;
static constexpr uint8_t PKT_TYPE_HARVEST_ACK = 0x21;
static constexpr uint8_t HARVEST_CMD_MAX_SSID = 20;

static constexpr uint8_t HARVEST_STATUS_OK        = 0x00;
static constexpr uint8_t HARVEST_STATUS_WIFI_FAIL = 0x01;
static constexpr uint8_t HARVEST_STATUS_COAP_FAIL = 0x02;

// ─── LE helpers ─────────────────────────────────────────────
static inline void writeU32LE(uint8_t* buf, uint32_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
    buf[2] = (val >> 16) & 0xFF;
    buf[3] = (val >> 24) & 0xFF;
}

static inline uint32_t readU32LE(const uint8_t* buf) {
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

// ─── HarvestAckPacket (inline copy) ─────────────────────────
struct HarvestAckPacket {
    uint8_t  cmdId;
    uint8_t  relayId[6];
    uint8_t  status;
    uint8_t  imageCount;
    uint32_t totalBytes;

    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const {
        static constexpr uint8_t ACK_SIZE = 16;
        if (maxLen < ACK_SIZE) return 0;

        uint8_t pos = 0;
        buf[pos++] = AODV_MAGIC;
        buf[pos++] = AODV_VERSION;
        buf[pos++] = PKT_TYPE_HARVEST_ACK;
        buf[pos++] = cmdId;
        memcpy(&buf[pos], relayId, 6);       pos += 6;
        buf[pos++] = status;
        buf[pos++] = imageCount;
        writeU32LE(&buf[pos], totalBytes);   pos += 4;
        return pos;  // 16
    }

    bool parse(const uint8_t* buf, uint8_t len) {
        static constexpr uint8_t ACK_SIZE = 16;
        if (len < ACK_SIZE) return false;
        if (buf[0] != AODV_MAGIC || buf[1] != AODV_VERSION) return false;
        if (buf[2] != PKT_TYPE_HARVEST_ACK) return false;

        uint8_t pos = 3;
        cmdId      = buf[pos++];
        memcpy(relayId, &buf[pos], 6);       pos += 6;
        status     = buf[pos++];
        imageCount = buf[pos++];
        totalBytes = readU32LE(&buf[pos]);   pos += 4;
        return true;
    }
};

// ─── HarvestCmdPacket (inline copy) ─────────────────────────
struct HarvestCmdPacket {
    uint8_t cmdId;
    uint8_t relayId[6];
    uint8_t targetLeafId[6];
    uint8_t ssidLen;
    char    ssid[HARVEST_CMD_MAX_SSID + 1];

    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const {
        if (ssidLen > HARVEST_CMD_MAX_SSID) return 0;
        uint8_t totalLen = 17 + ssidLen;
        if (maxLen < totalLen) return 0;

        uint8_t pos = 0;
        buf[pos++] = AODV_MAGIC;
        buf[pos++] = AODV_VERSION;
        buf[pos++] = PKT_TYPE_HARVEST_CMD;
        buf[pos++] = cmdId;
        memcpy(&buf[pos], relayId, 6);       pos += 6;
        memcpy(&buf[pos], targetLeafId, 6);  pos += 6;
        buf[pos++] = ssidLen;
        memcpy(&buf[pos], ssid, ssidLen);    pos += ssidLen;
        return pos;
    }

    bool parse(const uint8_t* buf, uint8_t len) {
        if (len < 17) return false;
        if (buf[0] != AODV_MAGIC || buf[1] != AODV_VERSION) return false;
        if (buf[2] != PKT_TYPE_HARVEST_CMD) return false;

        uint8_t pos = 3;
        cmdId = buf[pos++];
        memcpy(relayId, &buf[pos], 6);       pos += 6;
        memcpy(targetLeafId, &buf[pos], 6);  pos += 6;
        ssidLen = buf[pos++];

        if (ssidLen > HARVEST_CMD_MAX_SSID) return false;
        if (len < (uint8_t)(17 + ssidLen)) return false;

        memcpy(ssid, &buf[pos], ssidLen);
        ssid[ssidLen] = '\0';
        return true;
    }
};

#endif // NATIVE_TEST

// ─── Test setup / teardown ──────────────────────────────────
void setUp(void) {}
void tearDown(void) {}

// ═════════════════════════════════════════════════════════════
// HARVEST_ACK TESTS
// ═════════════════════════════════════════════════════════════

void test_harvest_ack_ok_with_images() {
    HarvestAckPacket ack;
    ack.cmdId = 42;
    memset(ack.relayId, 0xAA, 6);
    ack.status     = HARVEST_STATUS_OK;
    ack.imageCount = 3;
    ack.totalBytes = 30000;

    TEST_ASSERT_EQUAL_UINT8(42, ack.cmdId);
    TEST_ASSERT_EQUAL_UINT8(HARVEST_STATUS_OK, ack.status);
    TEST_ASSERT_EQUAL_UINT8(3, ack.imageCount);
    TEST_ASSERT_EQUAL_UINT32(30000, ack.totalBytes);

    // Verify it serializes correctly
    uint8_t buf[64];
    uint8_t len = ack.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(16, len);
    TEST_ASSERT_EQUAL_UINT8(AODV_MAGIC, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(PKT_TYPE_HARVEST_ACK, buf[2]);
}

void test_harvest_ack_wifi_fail() {
    HarvestAckPacket ack;
    ack.cmdId = 7;
    memset(ack.relayId, 0xBB, 6);
    ack.status     = HARVEST_STATUS_WIFI_FAIL;
    ack.imageCount = 0;
    ack.totalBytes = 0;

    TEST_ASSERT_EQUAL_UINT8(HARVEST_STATUS_WIFI_FAIL, ack.status);
    TEST_ASSERT_EQUAL_UINT8(0, ack.imageCount);

    uint8_t buf[64];
    uint8_t len = ack.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(16, len);
}

void test_harvest_ack_coap_fail() {
    HarvestAckPacket ack;
    ack.cmdId = 12;
    memset(ack.relayId, 0xCC, 6);
    ack.status     = HARVEST_STATUS_COAP_FAIL;
    ack.imageCount = 0;
    ack.totalBytes = 0;

    TEST_ASSERT_EQUAL_UINT8(HARVEST_STATUS_COAP_FAIL, ack.status);
    TEST_ASSERT_EQUAL_UINT8(0, ack.imageCount);

    uint8_t buf[64];
    uint8_t len = ack.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(16, len);
}

void test_harvest_ack_serialization_roundtrip() {
    HarvestAckPacket original;
    original.cmdId = 99;
    uint8_t relay[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    memcpy(original.relayId, relay, 6);
    original.status     = HARVEST_STATUS_OK;
    original.imageCount = 5;
    original.totalBytes = 123456;

    uint8_t buf[64];
    uint8_t len = original.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(16, len);

    HarvestAckPacket parsed;
    bool ok = parsed.parse(buf, len);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(original.cmdId, parsed.cmdId);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(original.relayId, parsed.relayId, 6);
    TEST_ASSERT_EQUAL_UINT8(original.status, parsed.status);
    TEST_ASSERT_EQUAL_UINT8(original.imageCount, parsed.imageCount);
    TEST_ASSERT_EQUAL_UINT32(original.totalBytes, parsed.totalBytes);
}

// ═════════════════════════════════════════════════════════════
// HARVEST_CMD TESTS
// ═════════════════════════════════════════════════════════════

void test_harvest_cmd_parse_valid() {
    HarvestCmdPacket cmd;
    cmd.cmdId = 10;
    uint8_t relay[6]  = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
    uint8_t leaf[6]   = {0x11, 0x22, 0x33, 0x44, 0x55, 0x02};
    memcpy(cmd.relayId, relay, 6);
    memcpy(cmd.targetLeafId, leaf, 6);
    const char* testSsid = "ForestCam_02";
    cmd.ssidLen = (uint8_t)strlen(testSsid);
    memcpy(cmd.ssid, testSsid, cmd.ssidLen);
    cmd.ssid[cmd.ssidLen] = '\0';

    uint8_t buf[64];
    uint8_t len = cmd.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);

    HarvestCmdPacket parsed;
    bool ok = parsed.parse(buf, len);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(10, parsed.cmdId);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(relay, parsed.relayId, 6);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(leaf, parsed.targetLeafId, 6);
    TEST_ASSERT_EQUAL_STRING("ForestCam_02", parsed.ssid);
    TEST_ASSERT_EQUAL_UINT8(cmd.ssidLen, parsed.ssidLen);
}

void test_harvest_cmd_ssid_too_long_truncated() {
    // SSID > 20 chars: serialize should reject (return 0)
    HarvestCmdPacket cmd;
    cmd.cmdId = 1;
    memset(cmd.relayId, 0x01, 6);
    memset(cmd.targetLeafId, 0x02, 6);
    const char* longSsid = "ThisSSIDIsWayTooLong!!";  // 21 chars
    cmd.ssidLen = (uint8_t)strlen(longSsid);
    memcpy(cmd.ssid, longSsid, cmd.ssidLen);
    cmd.ssid[cmd.ssidLen] = '\0';

    TEST_ASSERT_TRUE(cmd.ssidLen > HARVEST_CMD_MAX_SSID);

    uint8_t buf[64];
    uint8_t len = cmd.serialize(buf, sizeof(buf));
    // Serialize rejects SSID that exceeds max
    TEST_ASSERT_EQUAL_UINT8(0, len);

    // Truncated to 20 chars should work
    cmd.ssidLen = HARVEST_CMD_MAX_SSID;
    cmd.ssid[HARVEST_CMD_MAX_SSID] = '\0';
    len = cmd.serialize(buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_EQUAL_UINT8(17 + HARVEST_CMD_MAX_SSID, len);
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Harvest ACK
    RUN_TEST(test_harvest_ack_ok_with_images);
    RUN_TEST(test_harvest_ack_wifi_fail);
    RUN_TEST(test_harvest_ack_coap_fail);
    RUN_TEST(test_harvest_ack_serialization_roundtrip);

    // Harvest CMD
    RUN_TEST(test_harvest_cmd_parse_valid);
    RUN_TEST(test_harvest_cmd_ssid_too_long_truncated);

    return UNITY_END();
}
