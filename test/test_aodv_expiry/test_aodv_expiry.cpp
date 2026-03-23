/**
 * @file test_aodv_expiry.cpp
 * @brief Unit tests for AODV route lifecycle, RERR packets, and dedup table
 *
 * Re-implements RouteEntry, RerrEntry, RerrPacket, and dedup table logic
 * inline so tests can run on the native platform without ESP32 dependencies.
 *
 * Run: pio test -e native -f test_aodv_expiry
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#ifdef NATIVE_TEST

// ─── Protocol constants (mirrors AodvPacket.h) ─────────────
static constexpr uint8_t AODV_MAGIC     = 0xFC;
static constexpr uint8_t AODV_VERSION   = 0x01;
static constexpr uint8_t PKT_TYPE_RERR  = 0x12;
static constexpr uint8_t RERR_MAX_DESTS = 6;

// ─── LE helpers ─────────────────────────────────────────────
static inline void writeU16LE(uint8_t* buf, uint16_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
}

static inline uint16_t readU16LE(const uint8_t* buf) {
    return buf[0] | (buf[1] << 8);
}

// ─── RouteEntry (inline copy) ───────────────────────────────
struct RouteEntry {
    uint8_t  destId[6];
    uint8_t  nextHopId[6];
    uint16_t destSeqNum;
    uint8_t  hopCount;
    uint32_t expiryMs;     // absolute expiry time in millis
    bool     active;
    bool     validSeqNum;

    RouteEntry() : destSeqNum(0), hopCount(0), expiryMs(0),
                   active(false), validSeqNum(false) {
        memset(destId, 0, 6);
        memset(nextHopId, 0, 6);
    }

    bool isExpired(uint32_t nowMs) const {
        return nowMs >= expiryMs;
    }
};

// ─── RerrEntry (inline copy) ────────────────────────────────
struct RerrEntry {
    uint8_t  destId[6];
    uint16_t destSeqNum;
};

// ─── RerrPacket (inline copy) ───────────────────────────────
struct RerrPacket {
    uint8_t   destCount;
    RerrEntry entries[RERR_MAX_DESTS];

    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const {
        if (destCount == 0 || destCount > RERR_MAX_DESTS) return 0;
        uint8_t totalLen = 4 + destCount * 8;
        if (maxLen < totalLen) return 0;

        uint8_t pos = 0;
        buf[pos++] = AODV_MAGIC;
        buf[pos++] = AODV_VERSION;
        buf[pos++] = PKT_TYPE_RERR;
        buf[pos++] = destCount;

        for (uint8_t i = 0; i < destCount; i++) {
            memcpy(&buf[pos], entries[i].destId, 6); pos += 6;
            writeU16LE(&buf[pos], entries[i].destSeqNum); pos += 2;
        }
        return pos;
    }

    bool parse(const uint8_t* buf, uint8_t len) {
        if (len < 4) return false;
        if (buf[0] != AODV_MAGIC || buf[1] != AODV_VERSION) return false;
        if (buf[2] != PKT_TYPE_RERR) return false;

        destCount = buf[3];
        if (destCount == 0 || destCount > RERR_MAX_DESTS) return false;
        if (len < (uint8_t)(4 + destCount * 8)) return false;

        uint8_t pos = 4;
        for (uint8_t i = 0; i < destCount; i++) {
            memcpy(entries[i].destId, &buf[pos], 6); pos += 6;
            entries[i].destSeqNum = readU16LE(&buf[pos]); pos += 2;
        }
        return true;
    }
};

// ─── Dedup table (inline simplified version) ────────────────
static constexpr uint8_t DEDUP_TABLE_SIZE = 16;

struct DedupEntry {
    uint8_t  origId[6];
    uint32_t rreqId;
    bool     valid;
};

struct DedupTable {
    DedupEntry entries[DEDUP_TABLE_SIZE];
    uint8_t    nextSlot;

    DedupTable() : nextSlot(0) {
        for (uint8_t i = 0; i < DEDUP_TABLE_SIZE; i++) {
            entries[i].valid = false;
        }
    }

    // Returns true if this is a duplicate (already seen)
    bool isDuplicate(const uint8_t origId[6], uint32_t rreqId) {
        for (uint8_t i = 0; i < DEDUP_TABLE_SIZE; i++) {
            if (entries[i].valid &&
                entries[i].rreqId == rreqId &&
                memcmp(entries[i].origId, origId, 6) == 0) {
                return true;
            }
        }
        return false;
    }

    void record(const uint8_t origId[6], uint32_t rreqId) {
        entries[nextSlot].valid = true;
        entries[nextSlot].rreqId = rreqId;
        memcpy(entries[nextSlot].origId, origId, 6);
        nextSlot = (nextSlot + 1) % DEDUP_TABLE_SIZE;
    }
};

#endif // NATIVE_TEST

// ─── Test setup / teardown ──────────────────────────────────
void setUp(void) {}
void tearDown(void) {}

// ═════════════════════════════════════════════════════════════
// ROUTE ENTRY TESTS
// ═════════════════════════════════════════════════════════════

void test_route_entry_initial_state() {
    RouteEntry route;
    TEST_ASSERT_FALSE(route.active);
    TEST_ASSERT_EQUAL_UINT8(0, route.hopCount);
    TEST_ASSERT_EQUAL_UINT16(0, route.destSeqNum);
    TEST_ASSERT_EQUAL_UINT32(0, route.expiryMs);
    TEST_ASSERT_FALSE(route.validSeqNum);
}

void test_route_active_within_lifetime() {
    RouteEntry route;
    route.active   = true;
    route.expiryMs = 120000;  // expires at 120s

    // At time 0, route is not expired
    TEST_ASSERT_FALSE(route.isExpired(0));
    // At time 60000, still not expired
    TEST_ASSERT_FALSE(route.isExpired(60000));
    // At time 119999, still not expired
    TEST_ASSERT_FALSE(route.isExpired(119999));
}

void test_route_expired_after_lifetime() {
    RouteEntry route;
    route.active   = true;
    route.expiryMs = 120000;  // expires at 120s

    // At exactly 120000, expired
    TEST_ASSERT_TRUE(route.isExpired(120000));
    // At 130000, definitely expired
    TEST_ASSERT_TRUE(route.isExpired(130000));
}

// ═════════════════════════════════════════════════════════════
// RERR TESTS
// ═════════════════════════════════════════════════════════════

void test_rerr_entry_serialization() {
    // A single RerrEntry is destId[6] + destSeqNum[2] = 8 bytes
    RerrEntry entry;
    memset(entry.destId, 0xAB, 6);
    entry.destSeqNum = 1234;

    uint8_t buf[8];
    memcpy(buf, entry.destId, 6);
    writeU16LE(&buf[6], entry.destSeqNum);

    // Verify size
    TEST_ASSERT_EQUAL_UINT8(8, sizeof(buf));
    // Verify content
    TEST_ASSERT_EQUAL_UINT8(0xAB, buf[0]);
    TEST_ASSERT_EQUAL_UINT16(1234, readU16LE(&buf[6]));
}

void test_rerr_packet_roundtrip() {
    RerrPacket original;
    original.destCount = 3;
    for (uint8_t i = 0; i < 3; i++) {
        memset(original.entries[i].destId, 0x10 + i, 6);
        original.entries[i].destSeqNum = 100 + i;
    }

    uint8_t buf[64];
    uint8_t len = original.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(4 + 3 * 8, len);  // 28 bytes

    RerrPacket parsed;
    bool ok = parsed.parse(buf, len);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(3, parsed.destCount);

    for (uint8_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_UINT8_ARRAY(original.entries[i].destId, parsed.entries[i].destId, 6);
        TEST_ASSERT_EQUAL_UINT16(original.entries[i].destSeqNum, parsed.entries[i].destSeqNum);
    }
}

void test_rerr_max_destinations() {
    RerrPacket pkt;
    pkt.destCount = RERR_MAX_DESTS;  // 6
    for (uint8_t i = 0; i < RERR_MAX_DESTS; i++) {
        memset(pkt.entries[i].destId, i + 1, 6);
        pkt.entries[i].destSeqNum = 200 + i;
    }

    uint8_t buf[64];
    uint8_t len = pkt.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(4 + 6 * 8, len);  // 52 bytes

    RerrPacket parsed;
    bool ok = parsed.parse(buf, len);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT8(6, parsed.destCount);

    // Verify all 6 entries survived roundtrip
    for (uint8_t i = 0; i < RERR_MAX_DESTS; i++) {
        TEST_ASSERT_EQUAL_UINT16(200 + i, parsed.entries[i].destSeqNum);
    }

    // Over the limit should fail
    RerrPacket overLimit;
    overLimit.destCount = RERR_MAX_DESTS + 1;
    len = overLimit.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(0, len);
}

// ═════════════════════════════════════════════════════════════
// DEDUP TABLE TESTS
// ═════════════════════════════════════════════════════════════

void test_dedup_table_prevents_reprocess() {
    DedupTable dedup;

    uint8_t origin[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint32_t rreqId = 42;

    // First time: not a duplicate
    TEST_ASSERT_FALSE(dedup.isDuplicate(origin, rreqId));

    // Record it
    dedup.record(origin, rreqId);

    // Second time: detected as duplicate
    TEST_ASSERT_TRUE(dedup.isDuplicate(origin, rreqId));

    // Different RREQ ID from same origin: not a duplicate
    TEST_ASSERT_FALSE(dedup.isDuplicate(origin, rreqId + 1));

    // Same RREQ ID from different origin: not a duplicate
    uint8_t origin2[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    TEST_ASSERT_FALSE(dedup.isDuplicate(origin2, rreqId));
}

// ═════════════════════════════════════════════════════════════
// TEST RUNNER
// ═════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Route entry
    RUN_TEST(test_route_entry_initial_state);
    RUN_TEST(test_route_active_within_lifetime);
    RUN_TEST(test_route_expired_after_lifetime);

    // RERR
    RUN_TEST(test_rerr_entry_serialization);
    RUN_TEST(test_rerr_packet_roundtrip);
    RUN_TEST(test_rerr_max_destinations);

    // Dedup
    RUN_TEST(test_dedup_table_prevents_reprocess);

    return UNITY_END();
}
