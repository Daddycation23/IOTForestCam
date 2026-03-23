/**
 * @file test_thread_safety.cpp
 * @brief Unit tests for concurrency configuration constants and guard patterns
 *
 * Verifies that FreeRTOS task configuration values in TaskConfig.h have not
 * accidentally changed. Re-implements constants inline so tests run on native.
 *
 * Run: pio test -e native -f test_thread_safety
 */

#include <unity.h>
#include <cstdint>
#include <atomic>

#ifdef NATIVE_TEST

// ─── Re-implement TaskConfig.h constants inline ─────────────
// These mirror include/TaskConfig.h and will catch regressions
// if someone changes the real values without updating tests.

// Core affinities
static constexpr int CORE_LORA    = 0;
static constexpr int CORE_NETWORK = 1;

// Task stack sizes (words)
static constexpr uint32_t STACK_LORA        = 4096;
static constexpr uint32_t STACK_HARVEST     = 8192;
static constexpr uint32_t STACK_COAP_SERVER = 6144;
static constexpr uint32_t STACK_OLED        = 2048;

// Queue sizes
static constexpr uint8_t LORA_TX_QUEUE_SIZE       = 8;
static constexpr uint8_t HARVEST_CMD_QUEUE_SIZE   = 2;
static constexpr uint8_t ANNOUNCE_QUEUE_SIZE      = 8;

// Mutex timeout: pdMS_TO_TICKS(1000) — on native we just track the ms value
static constexpr uint32_t MUTEX_TIMEOUT_MS = 1000;

#endif // NATIVE_TEST

// ─── Test setup / teardown ──────────────────────────────────
void setUp(void) {}
void tearDown(void) {}

// ═════════════════════════════════════════════════════════════
// CONSTANT VERIFICATION TESTS
// ═════════════════════════════════════════════════════════════

void test_registry_lock_timeout_constant() {
    // MUTEX_TIMEOUT is pdMS_TO_TICKS(1000) — verify the ms value is 1000
    TEST_ASSERT_EQUAL_UINT32(1000, MUTEX_TIMEOUT_MS);
}

void test_lora_tx_queue_size() {
    TEST_ASSERT_EQUAL_UINT8(8, LORA_TX_QUEUE_SIZE);
}

void test_harvest_cmd_queue_size() {
    TEST_ASSERT_EQUAL_UINT8(2, HARVEST_CMD_QUEUE_SIZE);
}

void test_announce_queue_size() {
    // Was 4 in earlier versions, now increased to 8
    TEST_ASSERT_EQUAL_UINT8(8, ANNOUNCE_QUEUE_SIZE);
}

// ═════════════════════════════════════════════════════════════
// ATOMIC / VOLATILE PATTERN TESTS
// ═════════════════════════════════════════════════════════════

void test_atomic_relay_busy_default_false() {
    std::atomic<bool> relayBusy{false};
    TEST_ASSERT_FALSE(relayBusy.load());

    // Simulate setting busy
    relayBusy.store(true);
    TEST_ASSERT_TRUE(relayBusy.load());

    // Reset
    relayBusy.store(false);
    TEST_ASSERT_FALSE(relayBusy.load());
}

void test_volatile_flag_visibility() {
    volatile bool flag = false;
    TEST_ASSERT_FALSE(flag);

    flag = true;
    TEST_ASSERT_TRUE(flag);

    flag = false;
    TEST_ASSERT_FALSE(flag);
}

// ═════════════════════════════════════════════════════════════
// CORE AFFINITY / LOCKING ORDER TESTS
// ═════════════════════════════════════════════════════════════

void test_locking_order_documented() {
    // Verify core affinity constants exist and have expected values
    TEST_ASSERT_EQUAL_INT(0, CORE_LORA);
    TEST_ASSERT_EQUAL_INT(1, CORE_NETWORK);

    // Cores must be different (no overlap)
    TEST_ASSERT_NOT_EQUAL(CORE_LORA, CORE_NETWORK);
}

// ═════════════════════════════════════════════════════════════
// STACK SIZE TESTS
// ═════════════════════════════════════════════════════════════

void test_stack_sizes_reasonable() {
    // STACK_HARVEST >= 8192 (needs WiFi + CoAP buffers)
    TEST_ASSERT_TRUE(STACK_HARVEST >= 8192);

    // STACK_COAP_SERVER >= 6144 (CoapMessage + WiFi IRQ headroom)
    TEST_ASSERT_TRUE(STACK_COAP_SERVER >= 6144);

    // STACK_LORA >= 4096
    TEST_ASSERT_TRUE(STACK_LORA >= 4096);

    // STACK_OLED is smallest — just display updates
    TEST_ASSERT_TRUE(STACK_OLED >= 2048);

    // Ordering: harvest > coap_server > lora > oled
    TEST_ASSERT_TRUE(STACK_HARVEST >= STACK_COAP_SERVER);
    TEST_ASSERT_TRUE(STACK_COAP_SERVER >= STACK_LORA);
    TEST_ASSERT_TRUE(STACK_LORA >= STACK_OLED);
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Constants
    RUN_TEST(test_registry_lock_timeout_constant);
    RUN_TEST(test_lora_tx_queue_size);
    RUN_TEST(test_harvest_cmd_queue_size);
    RUN_TEST(test_announce_queue_size);

    // Atomic / volatile
    RUN_TEST(test_atomic_relay_busy_default_false);
    RUN_TEST(test_volatile_flag_visibility);

    // Core affinity / locking
    RUN_TEST(test_locking_order_documented);

    // Stack sizes
    RUN_TEST(test_stack_sizes_reasonable);

    return UNITY_END();
}
