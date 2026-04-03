/**
 * @file test_resume_offset.cpp
 * @brief Tests for the download resume offset feature
 *
 * Validates:
 *   - Fletcher-16 partial state correctness (split computation matches full)
 *   - RTC resume state lifecycle (save, restore, clear)
 *   - DownloadResumeState struct defaults and usage
 *   - Resume block offset logic and edge cases
 *   - Node ID mismatch rejection
 *
 * Run:  pio test -e native -f test_resume_offset
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

void setUp(void) {}
void tearDown(void) {}

// ─── Fletcher-16 (mirrors CoapClient::_updateFletcher16) ────────
static void fletcher16_update(uint16_t& sum1, uint16_t& sum2,
                               const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        sum1 = (sum1 + data[i]) % 255;
        sum2 = (sum2 + sum1) % 255;
    }
}

static uint16_t fletcher16_finalize(uint16_t sum1, uint16_t sum2) {
    return (sum2 << 8) | sum1;
}

// ─── Simulated RTC resume state (mirrors DeepSleepManager) ─────
static bool     sim_rtcResumeValid     = false;
static uint8_t  sim_rtcResumeNodeId[6] = {0};
static uint8_t  sim_rtcResumeImageIdx  = 0;
static uint32_t sim_rtcResumeBlock     = 0;
static uint16_t sim_rtcResumeSum1      = 0;
static uint16_t sim_rtcResumeSum2      = 0;
static char     sim_rtcResumeFilePath[64] = {0};
static uint32_t sim_rtcResumeTotalBytes = 0;

static void sim_saveResumeState(const uint8_t nodeId[6], uint8_t imageIdx,
                                 uint32_t blockNum, uint16_t s1, uint16_t s2,
                                 const char* filePath, uint32_t totalBytes) {
    memcpy(sim_rtcResumeNodeId, nodeId, 6);
    sim_rtcResumeImageIdx  = imageIdx;
    sim_rtcResumeBlock     = blockNum;
    sim_rtcResumeSum1      = s1;
    sim_rtcResumeSum2      = s2;
    strncpy(sim_rtcResumeFilePath, filePath, sizeof(sim_rtcResumeFilePath) - 1);
    sim_rtcResumeFilePath[sizeof(sim_rtcResumeFilePath) - 1] = '\0';
    sim_rtcResumeTotalBytes = totalBytes;
    sim_rtcResumeValid     = true;
}

static void sim_clearResumeState() {
    sim_rtcResumeValid = false;
    sim_rtcResumeBlock = 0;
    sim_rtcResumeSum1  = 0;
    sim_rtcResumeSum2  = 0;
    sim_rtcResumeTotalBytes = 0;
    memset(sim_rtcResumeFilePath, 0, sizeof(sim_rtcResumeFilePath));
}

// ─── DownloadResumeState (mirrors CoapClient.h) ────────────────
struct DownloadResumeState {
    uint32_t startBlock;
    uint16_t sum1;
    uint16_t sum2;
    uint32_t bytesWritten;
};

static constexpr size_t BLOCK_SIZE = 1024;

// =================================================================
// Fletcher-16 partial state correctness
// =================================================================

// Generate deterministic test data
static void fill_test_data(uint8_t* buf, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)((seed + i * 7 + 13) & 0xFF);
    }
}

void test_fletcher16_full_matches_split_at_50pct() {
    uint8_t data[2048];
    fill_test_data(data, sizeof(data), 0xAB);

    // Full pass
    uint16_t full_s1 = 0, full_s2 = 0;
    fletcher16_update(full_s1, full_s2, data, sizeof(data));
    uint16_t full_checksum = fletcher16_finalize(full_s1, full_s2);

    // Split at 50%
    uint16_t split_s1 = 0, split_s2 = 0;
    fletcher16_update(split_s1, split_s2, data, 1024);
    // Save partial state
    uint16_t saved_s1 = split_s1, saved_s2 = split_s2;
    // Resume with saved state
    fletcher16_update(saved_s1, saved_s2, data + 1024, 1024);
    uint16_t split_checksum = fletcher16_finalize(saved_s1, saved_s2);

    TEST_ASSERT_EQUAL_UINT16(full_checksum, split_checksum);
}

void test_fletcher16_full_matches_split_at_25pct() {
    uint8_t data[2048];
    fill_test_data(data, sizeof(data), 0xCD);

    uint16_t full_s1 = 0, full_s2 = 0;
    fletcher16_update(full_s1, full_s2, data, sizeof(data));
    uint16_t full_checksum = fletcher16_finalize(full_s1, full_s2);

    uint16_t s1 = 0, s2 = 0;
    fletcher16_update(s1, s2, data, 512);       // 25%
    fletcher16_update(s1, s2, data + 512, 1536); // remaining 75%
    uint16_t split_checksum = fletcher16_finalize(s1, s2);

    TEST_ASSERT_EQUAL_UINT16(full_checksum, split_checksum);
}

void test_fletcher16_full_matches_split_at_75pct() {
    uint8_t data[2048];
    fill_test_data(data, sizeof(data), 0xEF);

    uint16_t full_s1 = 0, full_s2 = 0;
    fletcher16_update(full_s1, full_s2, data, sizeof(data));
    uint16_t full_checksum = fletcher16_finalize(full_s1, full_s2);

    uint16_t s1 = 0, s2 = 0;
    fletcher16_update(s1, s2, data, 1536);       // 75%
    fletcher16_update(s1, s2, data + 1536, 512); // remaining 25%
    uint16_t split_checksum = fletcher16_finalize(s1, s2);

    TEST_ASSERT_EQUAL_UINT16(full_checksum, split_checksum);
}

void test_fletcher16_multi_block_split() {
    // Simulate 5 blocks of 1024 bytes, split after block 3
    uint8_t data[5 * BLOCK_SIZE];
    fill_test_data(data, sizeof(data), 0x42);

    uint16_t full_s1 = 0, full_s2 = 0;
    fletcher16_update(full_s1, full_s2, data, sizeof(data));
    uint16_t full_checksum = fletcher16_finalize(full_s1, full_s2);

    // First 3 blocks
    uint16_t s1 = 0, s2 = 0;
    for (int b = 0; b < 3; b++) {
        fletcher16_update(s1, s2, data + b * BLOCK_SIZE, BLOCK_SIZE);
    }
    // Save state (as if entering deep sleep)
    uint16_t saved_s1 = s1, saved_s2 = s2;
    // Resume: remaining 2 blocks
    for (int b = 3; b < 5; b++) {
        fletcher16_update(saved_s1, saved_s2, data + b * BLOCK_SIZE, BLOCK_SIZE);
    }
    uint16_t resumed_checksum = fletcher16_finalize(saved_s1, saved_s2);

    TEST_ASSERT_EQUAL_UINT16(full_checksum, resumed_checksum);
}

void test_fletcher16_partial_last_block() {
    // Last block is partial (less than 1024 bytes)
    uint8_t data[3 * BLOCK_SIZE + 500];  // 3 full blocks + 500 byte partial
    fill_test_data(data, sizeof(data), 0x99);

    uint16_t full_s1 = 0, full_s2 = 0;
    fletcher16_update(full_s1, full_s2, data, sizeof(data));
    uint16_t full_checksum = fletcher16_finalize(full_s1, full_s2);

    // Split after 2 full blocks
    uint16_t s1 = 0, s2 = 0;
    fletcher16_update(s1, s2, data, 2 * BLOCK_SIZE);
    // Resume: 1 full block + partial
    fletcher16_update(s1, s2, data + 2 * BLOCK_SIZE, BLOCK_SIZE + 500);
    uint16_t split_checksum = fletcher16_finalize(s1, s2);

    TEST_ASSERT_EQUAL_UINT16(full_checksum, split_checksum);
}

// =================================================================
// RTC resume state lifecycle
// =================================================================

void test_rtc_resume_starts_invalid() {
    sim_clearResumeState();
    TEST_ASSERT_FALSE(sim_rtcResumeValid);
    TEST_ASSERT_EQUAL_UINT32(0, sim_rtcResumeBlock);
    TEST_ASSERT_EQUAL_UINT16(0, sim_rtcResumeSum1);
    TEST_ASSERT_EQUAL_UINT16(0, sim_rtcResumeSum2);
}

void test_rtc_save_resume_state() {
    sim_clearResumeState();
    uint8_t nodeId[6] = {0xDC, 0x54, 0x75, 0xE4, 0x7B, 0x28};
    sim_saveResumeState(nodeId, 2, 50, 123, 456, "/received/img_002.jpg", 50 * BLOCK_SIZE);

    TEST_ASSERT_TRUE(sim_rtcResumeValid);
    TEST_ASSERT_EQUAL_MEMORY(nodeId, sim_rtcResumeNodeId, 6);
    TEST_ASSERT_EQUAL_UINT8(2, sim_rtcResumeImageIdx);
    TEST_ASSERT_EQUAL_UINT32(50, sim_rtcResumeBlock);
    TEST_ASSERT_EQUAL_UINT16(123, sim_rtcResumeSum1);
    TEST_ASSERT_EQUAL_UINT16(456, sim_rtcResumeSum2);
    TEST_ASSERT_EQUAL_STRING("/received/img_002.jpg", sim_rtcResumeFilePath);
    TEST_ASSERT_EQUAL_UINT32(50 * BLOCK_SIZE, sim_rtcResumeTotalBytes);
}

void test_rtc_clear_resume_state() {
    uint8_t nodeId[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    sim_saveResumeState(nodeId, 1, 100, 200, 300, "/received/test.jpg", 100 * BLOCK_SIZE);
    TEST_ASSERT_TRUE(sim_rtcResumeValid);

    sim_clearResumeState();
    TEST_ASSERT_FALSE(sim_rtcResumeValid);
    TEST_ASSERT_EQUAL_UINT32(0, sim_rtcResumeBlock);
    TEST_ASSERT_EQUAL_UINT16(0, sim_rtcResumeSum1);
    TEST_ASSERT_EQUAL_UINT16(0, sim_rtcResumeSum2);
    TEST_ASSERT_EQUAL_UINT32(0, sim_rtcResumeTotalBytes);
}

// =================================================================
// DownloadResumeState struct
// =================================================================

void test_resume_state_default_construction() {
    DownloadResumeState state = {};
    TEST_ASSERT_EQUAL_UINT32(0, state.startBlock);
    TEST_ASSERT_EQUAL_UINT16(0, state.sum1);
    TEST_ASSERT_EQUAL_UINT16(0, state.sum2);
    TEST_ASSERT_EQUAL_UINT32(0, state.bytesWritten);
}

void test_resume_state_populated() {
    DownloadResumeState state = {50, 123, 456, 50 * BLOCK_SIZE};
    TEST_ASSERT_EQUAL_UINT32(50, state.startBlock);
    TEST_ASSERT_EQUAL_UINT16(123, state.sum1);
    TEST_ASSERT_EQUAL_UINT16(456, state.sum2);
    TEST_ASSERT_EQUAL_UINT32(51200, state.bytesWritten);
}

// =================================================================
// Resume block offset logic
// =================================================================

void test_resume_from_block_50_starts_at_50() {
    DownloadResumeState state = {50, 0, 0, 50 * BLOCK_SIZE};
    uint32_t nextToSend    = state.startBlock;
    uint32_t nextToReceive = state.startBlock;
    TEST_ASSERT_EQUAL_UINT32(50, nextToSend);
    TEST_ASSERT_EQUAL_UINT32(50, nextToReceive);
}

void test_resume_from_block_0_is_fresh_download() {
    DownloadResumeState state = {0, 0, 0, 0};
    uint32_t nextToSend    = state.startBlock;
    uint32_t nextToReceive = state.startBlock;
    TEST_ASSERT_EQUAL_UINT32(0, nextToSend);
    TEST_ASSERT_EQUAL_UINT32(0, nextToReceive);
}

void test_resume_bytes_written_matches_block_offset() {
    for (uint32_t block = 0; block < 100; block++) {
        DownloadResumeState state = {block, 0, 0, block * BLOCK_SIZE};
        TEST_ASSERT_EQUAL_UINT32(block * BLOCK_SIZE, state.bytesWritten);
    }
}

void test_resume_from_last_block() {
    // Image is 85000 bytes = 84 blocks (83 full + 1 partial)
    uint32_t totalBlocks = (85000 + BLOCK_SIZE - 1) / BLOCK_SIZE;
    TEST_ASSERT_EQUAL_UINT32(84, totalBlocks);

    // Resume from block 83 (only 1 block left — the partial one)
    DownloadResumeState state = {83, 100, 200, 83 * BLOCK_SIZE};
    uint32_t blocksRemaining = totalBlocks - state.startBlock;
    TEST_ASSERT_EQUAL_UINT32(1, blocksRemaining);
}

// =================================================================
// Node ID mismatch rejection
// =================================================================

void test_resume_node_id_match() {
    uint8_t currentNode[6] = {0xDC, 0x54, 0x75, 0xE4, 0x7B, 0x28};
    uint8_t savedNode[6]   = {0xDC, 0x54, 0x75, 0xE4, 0x7B, 0x28};
    TEST_ASSERT_EQUAL_INT(0, memcmp(currentNode, savedNode, 6));
}

void test_resume_node_id_mismatch_rejects() {
    uint8_t currentNode[6] = {0xDC, 0x54, 0x75, 0xE4, 0x7B, 0x28};
    uint8_t savedNode[6]   = {0xDC, 0x54, 0x75, 0xE4, 0x80, 0xE4};

    sim_clearResumeState();
    sim_saveResumeState(savedNode, 0, 50, 100, 200, "/received/img.jpg", 50 * BLOCK_SIZE);

    // Simulate HarvestLoop check: only resume if node IDs match
    bool shouldResume = sim_rtcResumeValid && (memcmp(sim_rtcResumeNodeId, currentNode, 6) == 0);
    TEST_ASSERT_FALSE(shouldResume);
}

void test_resume_node_id_match_accepts() {
    uint8_t currentNode[6] = {0xDC, 0x54, 0x75, 0xE4, 0x7B, 0x28};

    sim_clearResumeState();
    sim_saveResumeState(currentNode, 1, 30, 50, 60, "/received/img_001.jpg", 30 * BLOCK_SIZE);

    bool shouldResume = sim_rtcResumeValid && (memcmp(sim_rtcResumeNodeId, currentNode, 6) == 0);
    TEST_ASSERT_TRUE(shouldResume);
    TEST_ASSERT_EQUAL_UINT32(30, sim_rtcResumeBlock);
}

// =================================================================
// Edge cases
// =================================================================

void test_resume_with_zero_sums_is_valid() {
    // sum1=0, sum2=0 is the initial state — valid for a file that starts with all zeros
    DownloadResumeState state = {10, 0, 0, 10 * BLOCK_SIZE};
    TEST_ASSERT_EQUAL_UINT32(10, state.startBlock);
    TEST_ASSERT_EQUAL_UINT16(0, state.sum1);
    TEST_ASSERT_EQUAL_UINT16(0, state.sum2);
}

void test_resume_file_path_truncation() {
    char longPath[128];
    memset(longPath, 'A', sizeof(longPath) - 1);
    longPath[127] = '\0';

    uint8_t nodeId[6] = {0};
    sim_saveResumeState(nodeId, 0, 1, 0, 0, longPath, BLOCK_SIZE);

    // Path should be truncated to 63 chars + null
    TEST_ASSERT_EQUAL(63, strlen(sim_rtcResumeFilePath));
    TEST_ASSERT_EQUAL('\0', sim_rtcResumeFilePath[63]);
}

void test_fletcher16_empty_data() {
    uint16_t s1 = 0, s2 = 0;
    fletcher16_update(s1, s2, nullptr, 0);
    TEST_ASSERT_EQUAL_UINT16(0, s1);
    TEST_ASSERT_EQUAL_UINT16(0, s2);
    TEST_ASSERT_EQUAL_UINT16(0, fletcher16_finalize(s1, s2));
}

void test_fletcher16_single_byte() {
    uint8_t data[1] = {0x42};
    uint16_t s1 = 0, s2 = 0;
    fletcher16_update(s1, s2, data, 1);
    // sum1 = (0 + 0x42) % 255 = 66
    // sum2 = (0 + 66) % 255 = 66
    TEST_ASSERT_EQUAL_UINT16(66, s1);
    TEST_ASSERT_EQUAL_UINT16(66, s2);
    TEST_ASSERT_EQUAL_UINT16((66 << 8) | 66, fletcher16_finalize(s1, s2));
}

// =================================================================
// Test runner
// =================================================================

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Fletcher-16 partial state
    RUN_TEST(test_fletcher16_full_matches_split_at_50pct);
    RUN_TEST(test_fletcher16_full_matches_split_at_25pct);
    RUN_TEST(test_fletcher16_full_matches_split_at_75pct);
    RUN_TEST(test_fletcher16_multi_block_split);
    RUN_TEST(test_fletcher16_partial_last_block);

    // RTC resume state lifecycle
    RUN_TEST(test_rtc_resume_starts_invalid);
    RUN_TEST(test_rtc_save_resume_state);
    RUN_TEST(test_rtc_clear_resume_state);

    // DownloadResumeState struct
    RUN_TEST(test_resume_state_default_construction);
    RUN_TEST(test_resume_state_populated);

    // Resume block offset logic
    RUN_TEST(test_resume_from_block_50_starts_at_50);
    RUN_TEST(test_resume_from_block_0_is_fresh_download);
    RUN_TEST(test_resume_bytes_written_matches_block_offset);
    RUN_TEST(test_resume_from_last_block);

    // Node ID mismatch
    RUN_TEST(test_resume_node_id_match);
    RUN_TEST(test_resume_node_id_mismatch_rejects);
    RUN_TEST(test_resume_node_id_match_accepts);

    // Edge cases
    RUN_TEST(test_resume_with_zero_sums_is_valid);
    RUN_TEST(test_resume_file_path_truncation);
    RUN_TEST(test_fletcher16_empty_data);
    RUN_TEST(test_fletcher16_single_byte);

    return UNITY_END();
}
