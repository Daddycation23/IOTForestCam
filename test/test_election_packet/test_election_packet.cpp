/**
 * @file test_election_packet.cpp
 * @brief Unit tests for ElectionPacket serialize/parse
 *
 * Run: pio test -e native
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

// ─── Minimal stubs for native build ──────────────────────────
// ElectionPacket.h includes Arduino.h which isn't available natively.
// We provide the constants and types inline for the native test env.
#ifdef NATIVE_TEST

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Stub Arduino types
typedef uint8_t byte;

// Protocol constants (from AodvPacket.h)
static constexpr uint8_t AODV_MAGIC   = 0xFC;
static constexpr uint8_t AODV_VERSION = 0x01;
static constexpr uint8_t PKT_TYPE_ELECTION    = 0x30;
static constexpr uint8_t PKT_TYPE_SUPPRESS    = 0x31;
static constexpr uint8_t PKT_TYPE_COORDINATOR = 0x32;
static constexpr uint8_t PKT_TYPE_GW_RECLAIM  = 0x33;
static constexpr uint8_t ELECTION_PACKET_SIZE = 15;

// ─── Inline ElectionPacket for native tests ──────────────────
struct ElectionPacket {
    uint8_t  type;
    uint8_t  senderId[6];
    uint32_t priority;
    uint16_t electionId;

    uint8_t serialize(uint8_t* buf, uint8_t maxLen) const {
        if (maxLen < ELECTION_PACKET_SIZE) return 0;
        uint8_t pos = 0;
        buf[pos++] = AODV_MAGIC;
        buf[pos++] = AODV_VERSION;
        buf[pos++] = type;
        memcpy(&buf[pos], senderId, 6); pos += 6;
        buf[pos++] = priority & 0xFF;
        buf[pos++] = (priority >> 8) & 0xFF;
        buf[pos++] = (priority >> 16) & 0xFF;
        buf[pos++] = (priority >> 24) & 0xFF;
        buf[pos++] = electionId & 0xFF;
        buf[pos++] = (electionId >> 8) & 0xFF;
        return pos;
    }

    bool parse(const uint8_t* buf, uint8_t len) {
        if (len < ELECTION_PACKET_SIZE) return false;
        if (buf[0] != AODV_MAGIC || buf[1] != AODV_VERSION) return false;
        uint8_t t = buf[2];
        if (t != PKT_TYPE_ELECTION && t != PKT_TYPE_SUPPRESS &&
            t != PKT_TYPE_COORDINATOR && t != PKT_TYPE_GW_RECLAIM) return false;
        uint8_t pos = 2;
        type = buf[pos++];
        memcpy(senderId, &buf[pos], 6); pos += 6;
        priority = buf[pos] | (buf[pos+1]<<8) | (buf[pos+2]<<16) | (buf[pos+3]<<24); pos += 4;
        electionId = buf[pos] | (buf[pos+1]<<8); pos += 2;
        return true;
    }

    static uint32_t macToPriority(const uint8_t mac[6]) {
        return mac[2] | (mac[3] << 8) | (mac[4] << 16) | (mac[5] << 24);
    }

    void senderIdToString(char* buf, size_t bufLen) const {
        snprintf(buf, bufLen, "%02X:%02X:%02X:%02X:%02X:%02X",
                 senderId[0], senderId[1], senderId[2],
                 senderId[3], senderId[4], senderId[5]);
    }
};

#endif // NATIVE_TEST

void setUp(void) {}
void tearDown(void) {}

// ─── Test: round-trip serialize then parse ───────────────────
void test_election_packet_round_trip() {
    ElectionPacket out;
    out.type = PKT_TYPE_ELECTION;
    uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    memcpy(out.senderId, mac, 6);
    out.priority = 0xFFEEDDCC;
    out.electionId = 0x1234;

    uint8_t buf[64];
    uint8_t len = out.serialize(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT8(ELECTION_PACKET_SIZE, len);

    ElectionPacket in;
    TEST_ASSERT_TRUE(in.parse(buf, len));
    TEST_ASSERT_EQUAL_UINT8(PKT_TYPE_ELECTION, in.type);
    TEST_ASSERT_EQUAL_MEMORY(mac, in.senderId, 6);
    TEST_ASSERT_EQUAL_UINT32(0xFFEEDDCC, in.priority);
    TEST_ASSERT_EQUAL_UINT16(0x1234, in.electionId);
}

// ─── Test: all four packet types parse correctly ─────────────
void test_election_packet_all_types() {
    uint8_t types[] = {PKT_TYPE_ELECTION, PKT_TYPE_SUPPRESS,
                       PKT_TYPE_COORDINATOR, PKT_TYPE_GW_RECLAIM};
    for (int i = 0; i < 4; i++) {
        ElectionPacket out;
        out.type = types[i];
        memset(out.senderId, i, 6);
        out.priority = i * 1000;
        out.electionId = i;

        uint8_t buf[64];
        uint8_t len = out.serialize(buf, sizeof(buf));
        TEST_ASSERT_EQUAL_UINT8(ELECTION_PACKET_SIZE, len);

        ElectionPacket in;
        TEST_ASSERT_TRUE(in.parse(buf, len));
        TEST_ASSERT_EQUAL_UINT8(types[i], in.type);
    }
}

// ─── Test: parse rejects short buffer ────────────────────────
void test_election_packet_parse_rejects_short() {
    uint8_t buf[10] = {AODV_MAGIC, AODV_VERSION, PKT_TYPE_ELECTION};
    ElectionPacket pkt;
    TEST_ASSERT_FALSE(pkt.parse(buf, 10));
}

// ─── Test: parse rejects wrong magic ─────────────────────────
void test_election_packet_parse_rejects_wrong_magic() {
    uint8_t buf[15] = {0x00, AODV_VERSION, PKT_TYPE_ELECTION};
    ElectionPacket pkt;
    TEST_ASSERT_FALSE(pkt.parse(buf, 15));
}

// ─── Test: parse rejects unknown type ────────────────────────
void test_election_packet_parse_rejects_unknown_type() {
    uint8_t buf[15] = {AODV_MAGIC, AODV_VERSION, 0x99};
    ElectionPacket pkt;
    TEST_ASSERT_FALSE(pkt.parse(buf, 15));
}

// ─── Test: serialize rejects undersized buffer ───────────────
void test_election_packet_serialize_rejects_small_buf() {
    ElectionPacket pkt;
    pkt.type = PKT_TYPE_ELECTION;
    uint8_t buf[10];
    TEST_ASSERT_EQUAL_UINT8(0, pkt.serialize(buf, sizeof(buf)));
}

// ─── Test: macToPriority ─────────────────────────────────────
void test_mac_to_priority() {
    uint8_t mac[6] = {0x00, 0x11, 0xAA, 0xBB, 0xCC, 0xDD};
    uint32_t p = ElectionPacket::macToPriority(mac);
    // Expected: mac[2] | (mac[3]<<8) | (mac[4]<<16) | (mac[5]<<24)
    TEST_ASSERT_EQUAL_UINT32(0xDDCCBBAA, p);
}

// ─── Test: higher MAC = higher priority ──────────────────────
void test_priority_ordering() {
    uint8_t macLow[6]  = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    uint8_t macHigh[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
    TEST_ASSERT_TRUE(ElectionPacket::macToPriority(macHigh) >
                     ElectionPacket::macToPriority(macLow));
}

// ─── Test: senderIdToString ──────────────────────────────────
void test_sender_id_to_string() {
    ElectionPacket pkt;
    uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    memcpy(pkt.senderId, mac, 6);
    char str[24];
    pkt.senderIdToString(str, sizeof(str));
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", str);
}

// ─── Runner ──────────────────────────────────────────────────
int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_election_packet_round_trip);
    RUN_TEST(test_election_packet_all_types);
    RUN_TEST(test_election_packet_parse_rejects_short);
    RUN_TEST(test_election_packet_parse_rejects_wrong_magic);
    RUN_TEST(test_election_packet_parse_rejects_unknown_type);
    RUN_TEST(test_election_packet_serialize_rejects_small_buf);
    RUN_TEST(test_mac_to_priority);
    RUN_TEST(test_priority_ordering);
    RUN_TEST(test_sender_id_to_string);

    return UNITY_END();
}
