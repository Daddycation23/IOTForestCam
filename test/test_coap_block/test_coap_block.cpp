/**
 * @file test_coap_block.cpp
 * @brief Tests for CoAP Block2 encoding/decoding and block size configuration
 *
 * Validates:
 *   - Block2 encode/decode round-trips
 *   - SZX=6 (1024-byte blocks) configuration
 *   - Block number calculations for pipelining
 *   - Window-based pipelining logic
 *
 * Run:  pio test -e native -f test_coap_block
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

void setUp(void) {}
void tearDown(void) {}

// ─── Inline Block2Info for native testing ────────────────────
// Mirrors include/CoapMessage.h Block2Info without Arduino deps

struct TestBlock2Info {
    uint32_t num;
    bool     more;
    uint8_t  szx;

    uint32_t blockSize() const { return 1u << (szx + 4); }

    static TestBlock2Info decode(uint32_t optVal) {
        TestBlock2Info b;
        b.szx  = optVal & 0x07;
        b.more = (optVal >> 3) & 0x01;
        b.num  = optVal >> 4;
        return b;
    }

    uint32_t encode() const {
        return (num << 4) | (more ? 0x08 : 0x00) | (szx & 0x07);
    }
};

// =================================================================
// Block2 encode/decode
// =================================================================

void test_block2_encode_szx6_block0() {
    TestBlock2Info b;
    b.num = 0;
    b.more = true;
    b.szx = 6;  // 1024 bytes

    uint32_t encoded = b.encode();
    // Expected: (0 << 4) | (1 << 3) | 6 = 0x0E
    TEST_ASSERT_EQUAL_UINT32(0x0E, encoded);
}

void test_block2_decode_szx6_block0() {
    TestBlock2Info b = TestBlock2Info::decode(0x0E);
    TEST_ASSERT_EQUAL_UINT32(0, b.num);
    TEST_ASSERT_TRUE(b.more);
    TEST_ASSERT_EQUAL_UINT8(6, b.szx);
    TEST_ASSERT_EQUAL_UINT32(1024, b.blockSize());
}

void test_block2_roundtrip_szx6() {
    for (uint32_t blockNum = 0; blockNum < 100; blockNum++) {
        TestBlock2Info orig;
        orig.num = blockNum;
        orig.more = (blockNum < 99);
        orig.szx = 6;

        uint32_t encoded = orig.encode();
        TestBlock2Info decoded = TestBlock2Info::decode(encoded);

        TEST_ASSERT_EQUAL_UINT32(orig.num, decoded.num);
        TEST_ASSERT_EQUAL(orig.more, decoded.more);
        TEST_ASSERT_EQUAL_UINT8(orig.szx, decoded.szx);
    }
}

void test_block2_szx5_gives_512() {
    TestBlock2Info b;
    b.szx = 5;
    TEST_ASSERT_EQUAL_UINT32(512, b.blockSize());
}

void test_block2_szx6_gives_1024() {
    TestBlock2Info b;
    b.szx = 6;
    TEST_ASSERT_EQUAL_UINT32(1024, b.blockSize());
}

// =================================================================
// Block count calculations
// =================================================================

void test_block_count_exact_multiple() {
    // 10240 bytes / 1024 block size = exactly 10 blocks
    uint32_t fileSize = 10240;
    uint32_t blockSize = 1024;
    uint32_t totalBlocks = (fileSize + blockSize - 1) / blockSize;
    TEST_ASSERT_EQUAL_UINT32(10, totalBlocks);
}

void test_block_count_non_exact() {
    // 10000 bytes / 1024 = 9.765 → 10 blocks
    uint32_t fileSize = 10000;
    uint32_t blockSize = 1024;
    uint32_t totalBlocks = (fileSize + blockSize - 1) / blockSize;
    TEST_ASSERT_EQUAL_UINT32(10, totalBlocks);
}

void test_block_count_small_file() {
    // 500 bytes / 1024 = 1 block
    uint32_t fileSize = 500;
    uint32_t blockSize = 1024;
    uint32_t totalBlocks = (fileSize + blockSize - 1) / blockSize;
    TEST_ASSERT_EQUAL_UINT32(1, totalBlocks);
}

void test_block_count_512_vs_1024() {
    // 50KB file: 512-byte blocks = 100, 1024-byte blocks = 50
    uint32_t fileSize = 51200;
    uint32_t blocks512 = (fileSize + 511) / 512;
    uint32_t blocks1024 = (fileSize + 1023) / 1024;
    TEST_ASSERT_EQUAL_UINT32(100, blocks512);
    TEST_ASSERT_EQUAL_UINT32(50, blocks1024);
    // 1024-byte blocks halve the round-trips
    TEST_ASSERT_EQUAL_UINT32(blocks512 / 2, blocks1024);
}

// =================================================================
// Pipeline window logic
// =================================================================

static constexpr uint8_t PIPELINE_WINDOW_SIZE = 3;

struct PipelineState {
    uint32_t nextToSend;      // Next block number to request
    uint32_t nextToReceive;   // Next block number we expect
    uint32_t totalBlocks;     // Total blocks in transfer
    uint8_t  outstanding;     // Currently outstanding requests

    bool canSendMore() const {
        return outstanding < PIPELINE_WINDOW_SIZE &&
               nextToSend < totalBlocks;
    }

    void onSend() {
        nextToSend++;
        outstanding++;
    }

    void onReceive() {
        nextToReceive++;
        outstanding--;
    }

    bool isComplete() const {
        return nextToReceive >= totalBlocks;
    }
};

void test_pipeline_initial_burst() {
    PipelineState state = {0, 0, 50, 0};

    // Should be able to send PIPELINE_WINDOW_SIZE requests initially
    uint8_t sent = 0;
    while (state.canSendMore()) {
        state.onSend();
        sent++;
    }
    TEST_ASSERT_EQUAL_UINT8(PIPELINE_WINDOW_SIZE, sent);
    TEST_ASSERT_EQUAL_UINT8(PIPELINE_WINDOW_SIZE, state.outstanding);
    TEST_ASSERT_EQUAL_UINT32(PIPELINE_WINDOW_SIZE, state.nextToSend);
}

void test_pipeline_receive_opens_window() {
    PipelineState state = {3, 0, 50, 3};  // Window full

    TEST_ASSERT_FALSE(state.canSendMore());

    // Receive one block — opens window
    state.onReceive();
    TEST_ASSERT_TRUE(state.canSendMore());
    TEST_ASSERT_EQUAL_UINT8(2, state.outstanding);
}

void test_pipeline_completes_correctly() {
    PipelineState state = {0, 0, 5, 0};  // 5 blocks total

    // Send initial burst (3)
    while (state.canSendMore()) state.onSend();
    TEST_ASSERT_EQUAL_UINT32(3, state.nextToSend);

    // Receive blocks and send more
    while (!state.isComplete()) {
        state.onReceive();
        if (state.canSendMore()) state.onSend();
    }

    TEST_ASSERT_EQUAL_UINT32(5, state.nextToReceive);
    TEST_ASSERT_TRUE(state.isComplete());
}

void test_pipeline_single_block_file() {
    PipelineState state = {0, 0, 1, 0};

    // Should send 1, then stop
    TEST_ASSERT_TRUE(state.canSendMore());
    state.onSend();
    TEST_ASSERT_FALSE(state.canSendMore());

    state.onReceive();
    TEST_ASSERT_TRUE(state.isComplete());
}

// =================================================================
// Fletcher-16 checksum (must match StorageReader implementation)
// =================================================================

static uint16_t fletcher16(const uint8_t* data, size_t len) {
    uint16_t sum1 = 0, sum2 = 0;
    for (size_t i = 0; i < len; i++) {
        sum1 = (sum1 + data[i]) % 255;
        sum2 = (sum2 + sum1) % 255;
    }
    return (sum2 << 8) | sum1;
}

void test_fletcher16_known_value() {
    const uint8_t data[] = "abcde";
    uint16_t cksum = fletcher16(data, 5);
    // Fletcher-16 for "abcde": sum1=0xF0, sum2=0xC8 → 0xC8F0
    TEST_ASSERT_EQUAL_HEX16(0xC8F0, cksum);
}

void test_fletcher16_incremental_matches_full() {
    // Simulates receiving blocks and computing checksum incrementally
    const uint8_t data[] = "Hello, World! This is a test of incremental Fletcher-16.";
    size_t len = strlen((const char*)data);

    // Full computation
    uint16_t fullChecksum = fletcher16(data, len);

    // Incremental (2 chunks)
    uint16_t s1 = 0, s2 = 0;
    size_t half = len / 2;
    for (size_t i = 0; i < half; i++) {
        s1 = (s1 + data[i]) % 255;
        s2 = (s2 + s1) % 255;
    }
    for (size_t i = half; i < len; i++) {
        s1 = (s1 + data[i]) % 255;
        s2 = (s2 + s1) % 255;
    }
    uint16_t incrementalChecksum = (s2 << 8) | s1;

    TEST_ASSERT_EQUAL_HEX16(fullChecksum, incrementalChecksum);
}

// =================================================================
// Entry point
// =================================================================

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Block2 encode/decode
    RUN_TEST(test_block2_encode_szx6_block0);
    RUN_TEST(test_block2_decode_szx6_block0);
    RUN_TEST(test_block2_roundtrip_szx6);
    RUN_TEST(test_block2_szx5_gives_512);
    RUN_TEST(test_block2_szx6_gives_1024);

    // Block count calculations
    RUN_TEST(test_block_count_exact_multiple);
    RUN_TEST(test_block_count_non_exact);
    RUN_TEST(test_block_count_small_file);
    RUN_TEST(test_block_count_512_vs_1024);

    // Pipeline window logic
    RUN_TEST(test_pipeline_initial_burst);
    RUN_TEST(test_pipeline_receive_opens_window);
    RUN_TEST(test_pipeline_completes_correctly);
    RUN_TEST(test_pipeline_single_block_file);

    // Fletcher-16
    RUN_TEST(test_fletcher16_known_value);
    RUN_TEST(test_fletcher16_incremental_matches_full);

    return UNITY_END();
}
