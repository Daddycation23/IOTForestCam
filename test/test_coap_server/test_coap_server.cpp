/**
 * @file test_coap_server.cpp
 * @brief Unit tests for CoAP message parsing and response building
 *
 * Tests announce payload parsing (binary format) and /info JSON count parsing.
 * Logic is re-implemented inline to avoid ESP32 header dependencies.
 *
 * Run: pio test -e native -f test_coap_server
 */

#include <unity.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#ifdef NATIVE_TEST

// ─── Announce payload parser (mirrors CoapServer::_handleAnnouncePost) ──
struct AnnounceParseResult {
    bool     valid;
    uint8_t  nodeId[6];
    uint8_t  imageCount;
};

static AnnounceParseResult parseAnnouncePayload(const uint8_t* payload, uint8_t len) {
    AnnounceParseResult result;
    memset(&result, 0, sizeof(result));

    if (!payload || len < 7) {
        result.valid = false;
        return result;
    }

    result.valid = true;
    memcpy(result.nodeId, payload, 6);
    result.imageCount = payload[6];
    return result;
}

// ─── /info JSON count parser (mirrors HarvestLoop::_doDownload) ──────
static uint8_t parseInfoJsonCount(const char* json) {
    if (!json) return 0;

    const char* countKey = strstr(json, "\"count\":");
    if (!countKey) return 0;

    int parsed = atoi(countKey + 8);
    if (parsed > 0 && parsed <= 255) {
        return (uint8_t)parsed;
    }
    return 0;
}

#endif // NATIVE_TEST

// ─── Test setup / teardown ───────────────────────────────────
void setUp(void) {}
void tearDown(void) {}

// ═════════════════════════════════════════════════════════════
// ANNOUNCE PAYLOAD TESTS
// ═════════════════════════════════════════════════════════════

void test_announce_payload_parse_valid() {
    // 7 bytes: 6-byte MAC + 1-byte image count
    uint8_t payload[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 5};
    AnnounceParseResult r = parseAnnouncePayload(payload, 7);

    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0xAA, r.nodeId[0]);
    TEST_ASSERT_EQUAL_UINT8(0xBB, r.nodeId[1]);
    TEST_ASSERT_EQUAL_UINT8(0xCC, r.nodeId[2]);
    TEST_ASSERT_EQUAL_UINT8(0xDD, r.nodeId[3]);
    TEST_ASSERT_EQUAL_UINT8(0xEE, r.nodeId[4]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, r.nodeId[5]);
    TEST_ASSERT_EQUAL_UINT8(5, r.imageCount);
}

void test_announce_payload_too_short() {
    uint8_t payload[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    AnnounceParseResult r = parseAnnouncePayload(payload, 5);
    TEST_ASSERT_FALSE(r.valid);

    // Also test with null payload
    AnnounceParseResult r2 = parseAnnouncePayload(nullptr, 0);
    TEST_ASSERT_FALSE(r2.valid);
}

void test_announce_payload_zero_images() {
    uint8_t payload[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0};
    AnnounceParseResult r = parseAnnouncePayload(payload, 7);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(0, r.imageCount);
}

void test_announce_payload_max_images() {
    uint8_t payload[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 255};
    AnnounceParseResult r = parseAnnouncePayload(payload, 7);
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_UINT8(255, r.imageCount);
}

// ═════════════════════════════════════════════════════════════
// INFO JSON PARSING TESTS
// ═════════════════════════════════════════════════════════════

void test_info_json_count_parse() {
    uint8_t count = parseInfoJsonCount("{\"count\":5,\"block_size\":512}");
    TEST_ASSERT_EQUAL_UINT8(5, count);
}

void test_info_json_missing_count() {
    uint8_t count = parseInfoJsonCount("{\"block_size\":512}");
    TEST_ASSERT_EQUAL_UINT8(0, count);

    // Also test null input
    uint8_t count2 = parseInfoJsonCount(nullptr);
    TEST_ASSERT_EQUAL_UINT8(0, count2);
}

void test_info_json_invalid_count() {
    // Non-numeric: atoi returns 0, which fails the > 0 check
    uint8_t count = parseInfoJsonCount("{\"count\":\"abc\"}");
    TEST_ASSERT_EQUAL_UINT8(0, count);

    // Negative value: fails the > 0 check
    uint8_t count2 = parseInfoJsonCount("{\"count\":-1}");
    TEST_ASSERT_EQUAL_UINT8(0, count2);

    // Zero: fails the > 0 check
    uint8_t count3 = parseInfoJsonCount("{\"count\":0}");
    TEST_ASSERT_EQUAL_UINT8(0, count3);
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Announce payload
    RUN_TEST(test_announce_payload_parse_valid);
    RUN_TEST(test_announce_payload_too_short);
    RUN_TEST(test_announce_payload_zero_images);
    RUN_TEST(test_announce_payload_max_images);

    // Info JSON
    RUN_TEST(test_info_json_count_parse);
    RUN_TEST(test_info_json_missing_count);
    RUN_TEST(test_info_json_invalid_count);

    return UNITY_END();
}
