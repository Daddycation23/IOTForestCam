/**
 * @file CoapMessage.h
 * @brief CoAP protocol core — message format, parsing, and Block2 support
 *
 * Implements the essential parts of:
 *   - RFC 7252: The Constrained Application Protocol (CoAP)
 *   - RFC 7959: Block-Wise Transfers in CoAP
 *
 * CoAP Message Format (RFC 7252, Section 3):
 *    0                   1                   2                   3
 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |Ver| T |  TKL  |      Code     |          Message ID           |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |   Token (if any, TKL bytes) ...
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |   Options (if any) ...
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |1 1 1 1 1 1 1 1|    Payload (if any) ...
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef COAP_MESSAGE_H
#define COAP_MESSAGE_H

#include <Arduino.h>
#include <cstring>

// ─── CoAP Constants ──────────────────────────────────────────
static constexpr uint16_t COAP_DEFAULT_PORT   = 5683;
static constexpr uint8_t  COAP_VERSION        = 1;
static constexpr size_t   COAP_MAX_PDU_SIZE   = 1024;
static constexpr uint8_t  COAP_PAYLOAD_MARKER = 0xFF;

/** Block size exponent: 2^(5+4) = 512 bytes, matching StorageReader */
static constexpr uint8_t  COAP_BLOCK_SZX      = 5;

// ─── Message Types (RFC 7252, Section 3) ─────────────────────
enum CoapType : uint8_t {
    COAP_CON = 0,   // Confirmable — requires ACK
    COAP_NON = 1,   // Non-confirmable — fire-and-forget
    COAP_ACK = 2,   // Acknowledgment
    COAP_RST = 3    // Reset — reject a message
};

// ─── Method & Response Codes (RFC 7252, Section 5.9) ─────────
// Encoded as (class * 32 + detail), displayed as class.detail
enum CoapCode : uint8_t {
    // Empty
    COAP_EMPTY  = 0,          // 0.00

    // Methods (class 0)
    COAP_GET    = 1,          // 0.01
    COAP_POST   = 2,          // 0.02
    COAP_PUT    = 3,          // 0.03
    COAP_DELETE = 4,          // 0.04

    // Success (class 2)
    COAP_CREATED  = 65,       // 2.01
    COAP_DELETED  = 66,       // 2.02
    COAP_VALID    = 67,       // 2.03
    COAP_CHANGED  = 68,       // 2.04
    COAP_CONTENT  = 69,       // 2.05

    // Client Error (class 4)
    COAP_BAD_REQUEST        = 128, // 4.00
    COAP_UNAUTHORIZED       = 129, // 4.01
    COAP_BAD_OPTION         = 130, // 4.02
    COAP_FORBIDDEN          = 131, // 4.03
    COAP_NOT_FOUND          = 132, // 4.04
    COAP_METHOD_NOT_ALLOWED = 133, // 4.05

    // Server Error (class 5)
    COAP_INTERNAL_ERROR     = 160, // 5.00
    COAP_NOT_IMPLEMENTED    = 161, // 5.01
};

// ─── Option Numbers (RFC 7252, Section 5.10) ─────────────────
enum CoapOptionNumber : uint16_t {
    COAP_OPT_URI_HOST       = 3,
    COAP_OPT_URI_PORT       = 7,
    COAP_OPT_URI_PATH       = 11,
    COAP_OPT_CONTENT_FORMAT = 12,
    COAP_OPT_MAX_AGE        = 14,
    COAP_OPT_URI_QUERY      = 15,
    COAP_OPT_ACCEPT         = 17,
    COAP_OPT_BLOCK2         = 23,   // RFC 7959
    COAP_OPT_BLOCK1         = 27,   // RFC 7959
    COAP_OPT_SIZE2          = 28,   // RFC 7959
    COAP_OPT_SIZE1          = 60,   // RFC 7959
};

// ─── Content Formats (RFC 7252, Section 12.3) ────────────────
enum CoapContentFormat : uint16_t {
    COAP_FMT_TEXT_PLAIN       = 0,
    COAP_FMT_APP_LINK_FORMAT  = 40,
    COAP_FMT_APP_XML          = 41,
    COAP_FMT_APP_OCTET_STREAM = 42,
    COAP_FMT_APP_JSON         = 50,
};

// ─── Limits ──────────────────────────────────────────────────
static constexpr uint8_t  COAP_MAX_OPTIONS    = 16;
static constexpr uint16_t COAP_MAX_OPTION_LEN = 64;
static constexpr uint8_t  COAP_MAX_TOKEN_LEN  = 8;

// ─── CoAP Option ─────────────────────────────────────────────

struct CoapOption {
    uint16_t number;
    uint16_t length;
    uint8_t  value[COAP_MAX_OPTION_LEN];

    /** Interpret option value as an unsigned integer (big-endian). */
    uint32_t asUint() const {
        uint32_t val = 0;
        for (uint16_t i = 0; i < length && i < 4; i++) {
            val = (val << 8) | value[i];
        }
        return val;
    }
};

// ─── Block2 Helper (RFC 7959, Section 2.2) ───────────────────
//
// Block option encoding:
//   value = (NUM << 4) | (M << 3) | SZX
//   Block size = 2^(SZX + 4) bytes
//   SZX=5 → 512 bytes (matches our StorageReader block size)
//

struct Block2Info {
    uint32_t num;    // Block number
    bool     more;   // More blocks follow (M flag)
    uint8_t  szx;    // Size exponent: block_size = 2^(szx+4)

    uint32_t blockSize() const { return 1u << (szx + 4); }

    /** Decode from a CoAP option uint value. */
    static Block2Info decode(uint32_t optVal) {
        Block2Info b;
        b.szx  = optVal & 0x07;
        b.more = (optVal >> 3) & 0x01;
        b.num  = optVal >> 4;
        return b;
    }

    /** Encode to a CoAP option uint value. */
    uint32_t encode() const {
        return (num << 4) | (more ? 0x08 : 0x00) | (szx & 0x07);
    }
};

// ─── CoAP Message ────────────────────────────────────────────

class CoapMessage {
public:
    CoapType    type;
    CoapCode    code;
    uint16_t    messageId;

    uint8_t     token[COAP_MAX_TOKEN_LEN];
    uint8_t     tokenLength;

    CoapOption  options[COAP_MAX_OPTIONS];
    uint8_t     optionCount;

    uint8_t*    payload;         // Points into parsed buffer or external data
    uint16_t    payloadLength;

    CoapMessage() { reset(); }

    void reset() {
        type          = COAP_CON;
        code          = COAP_EMPTY;
        messageId     = 0;
        tokenLength   = 0;
        optionCount   = 0;
        payload       = nullptr;
        payloadLength = 0;
        memset(token, 0, sizeof(token));
    }

    // ── Parse from raw UDP buffer (RFC 7252, Section 3) ──────

    bool parse(uint8_t* buf, size_t len) {
        if (len < 4) return false;

        uint8_t ver = (buf[0] >> 6) & 0x03;
        if (ver != COAP_VERSION) return false;

        type        = (CoapType)((buf[0] >> 4) & 0x03);
        tokenLength = buf[0] & 0x0F;
        code        = (CoapCode)buf[1];
        messageId   = (buf[2] << 8) | buf[3];

        if (tokenLength > COAP_MAX_TOKEN_LEN) return false;
        if (4 + tokenLength > len) return false;

        memcpy(token, &buf[4], tokenLength);

        // Parse options (delta-encoded, sorted by number)
        size_t pos = 4 + tokenLength;
        uint16_t prevOptNum = 0;
        optionCount = 0;

        while (pos < len && buf[pos] != COAP_PAYLOAD_MARKER) {
            if (optionCount >= COAP_MAX_OPTIONS) return false;

            uint16_t delta  = (buf[pos] >> 4) & 0x0F;
            uint16_t optLen = buf[pos] & 0x0F;
            pos++;

            // Extended delta (RFC 7252, Section 3.1)
            if (delta == 13) {
                if (pos >= len) return false;
                delta = buf[pos++] + 13;
            } else if (delta == 14) {
                if (pos + 1 >= len) return false;
                delta = ((buf[pos] << 8) | buf[pos + 1]) + 269;
                pos += 2;
            } else if (delta == 15) {
                return false;  // Reserved
            }

            // Extended length
            if (optLen == 13) {
                if (pos >= len) return false;
                optLen = buf[pos++] + 13;
            } else if (optLen == 14) {
                if (pos + 1 >= len) return false;
                optLen = ((buf[pos] << 8) | buf[pos + 1]) + 269;
                pos += 2;
            } else if (optLen == 15) {
                return false;  // Reserved
            }

            if (pos + optLen > len) return false;
            if (optLen > COAP_MAX_OPTION_LEN) return false;

            CoapOption& opt = options[optionCount];
            opt.number = prevOptNum + delta;
            opt.length = optLen;
            memcpy(opt.value, &buf[pos], optLen);

            prevOptNum = opt.number;
            pos += optLen;
            optionCount++;
        }

        // Payload (after 0xFF marker)
        if (pos < len && buf[pos] == COAP_PAYLOAD_MARKER) {
            pos++;
            payload       = &buf[pos];
            payloadLength = len - pos;
        } else {
            payload       = nullptr;
            payloadLength = 0;
        }

        return true;
    }

    // ── Serialize to buffer ──────────────────────────────────
    // Returns bytes written, or 0 on error.

    size_t serialize(uint8_t* buf, size_t maxLen) const {
        if (maxLen < 4u + tokenLength) return 0;

        // Header
        buf[0] = (COAP_VERSION << 6) | ((type & 0x03) << 4) | (tokenLength & 0x0F);
        buf[1] = (uint8_t)code;
        buf[2] = (messageId >> 8) & 0xFF;
        buf[3] = messageId & 0xFF;

        memcpy(&buf[4], token, tokenLength);
        size_t pos = 4 + tokenLength;

        // Options (must already be sorted by number)
        uint16_t prevOptNum = 0;
        for (uint8_t i = 0; i < optionCount; i++) {
            const CoapOption& opt = options[i];
            uint16_t delta  = opt.number - prevOptNum;
            uint16_t optLen = opt.length;

            // Calculate space needed for this option
            size_t needed = 1 + optLen;
            if (delta >= 269)  needed += 2;
            else if (delta >= 13) needed += 1;
            if (optLen >= 269) needed += 2;
            else if (optLen >= 13) needed += 1;

            if (pos + needed > maxLen) return 0;

            // Delta nibble
            uint8_t deltaNibble = (delta < 13) ? delta : (delta < 269) ? 13 : 14;
            uint8_t lenNibble   = (optLen < 13) ? optLen : (optLen < 269) ? 13 : 14;

            buf[pos++] = (deltaNibble << 4) | lenNibble;

            // Extended delta bytes
            if (delta >= 269) {
                uint16_t ext = delta - 269;
                buf[pos++] = (ext >> 8) & 0xFF;
                buf[pos++] = ext & 0xFF;
            } else if (delta >= 13) {
                buf[pos++] = delta - 13;
            }

            // Extended length bytes
            if (optLen >= 269) {
                uint16_t ext = optLen - 269;
                buf[pos++] = (ext >> 8) & 0xFF;
                buf[pos++] = ext & 0xFF;
            } else if (optLen >= 13) {
                buf[pos++] = optLen - 13;
            }

            memcpy(&buf[pos], opt.value, optLen);
            pos += optLen;
            prevOptNum = opt.number;
        }

        // Payload
        if (payloadLength > 0 && payload != nullptr) {
            if (pos + 1 + payloadLength > maxLen) return 0;
            buf[pos++] = COAP_PAYLOAD_MARKER;
            memcpy(&buf[pos], payload, payloadLength);
            pos += payloadLength;
        }

        return pos;
    }

    // ── Option Helpers ───────────────────────────────────────

    /** Add an option (inserted in sorted order by option number). */
    bool addOption(uint16_t number, const uint8_t* value, uint16_t length) {
        if (optionCount >= COAP_MAX_OPTIONS) return false;
        if (length > COAP_MAX_OPTION_LEN) return false;

        // Find insertion point to keep options sorted
        uint8_t insertAt = optionCount;
        for (uint8_t i = 0; i < optionCount; i++) {
            if (options[i].number > number) {
                insertAt = i;
                break;
            }
        }

        // Shift options to make room
        for (uint8_t i = optionCount; i > insertAt; i--) {
            options[i] = options[i - 1];
        }

        options[insertAt].number = number;
        options[insertAt].length = length;
        memcpy(options[insertAt].value, value, length);
        optionCount++;
        return true;
    }

    /** Add a numeric option (encoded as big-endian, minimal bytes). */
    bool addOptionUint(uint16_t number, uint32_t value) {
        uint8_t buf[4];
        uint8_t len;

        if (value == 0) {
            len = 0;
        } else if (value <= 0xFF) {
            buf[0] = value & 0xFF;
            len = 1;
        } else if (value <= 0xFFFF) {
            buf[0] = (value >> 8) & 0xFF;
            buf[1] = value & 0xFF;
            len = 2;
        } else if (value <= 0xFFFFFF) {
            buf[0] = (value >> 16) & 0xFF;
            buf[1] = (value >> 8) & 0xFF;
            buf[2] = value & 0xFF;
            len = 3;
        } else {
            buf[0] = (value >> 24) & 0xFF;
            buf[1] = (value >> 16) & 0xFF;
            buf[2] = (value >> 8) & 0xFF;
            buf[3] = value & 0xFF;
            len = 4;
        }

        return addOption(number, buf, len);
    }

    /** Add a Uri-Path segment (e.g., "image" or "0"). */
    bool addUriPath(const char* segment) {
        return addOption(COAP_OPT_URI_PATH,
                         (const uint8_t*)segment, strlen(segment));
    }

    /** Find the first option with the given number. Returns nullptr if not found. */
    const CoapOption* findOption(uint16_t number) const {
        for (uint8_t i = 0; i < optionCount; i++) {
            if (options[i].number == number) return &options[i];
        }
        return nullptr;
    }

    /** Extract Block2 info from the request. Returns false if no Block2 option. */
    bool getBlock2(Block2Info& info) const {
        const CoapOption* opt = findOption(COAP_OPT_BLOCK2);
        if (!opt) return false;
        info = Block2Info::decode(opt->asUint());
        return true;
    }

    /** Collect all Uri-Path segments. Returns segment count. */
    uint8_t getUriSegments(const uint8_t* segs[], uint16_t lens[],
                           uint8_t maxSegs) const {
        uint8_t count = 0;
        for (uint8_t i = 0; i < optionCount && count < maxSegs; i++) {
            if (options[i].number == COAP_OPT_URI_PATH) {
                segs[count]  = options[i].value;
                lens[count]  = options[i].length;
                count++;
            }
        }
        return count;
    }

    // ── Factory Methods ──────────────────────────────────────

    /**
     * Create a piggybacked ACK response for a CON request.
     * Copies the message ID and token from the request.
     */
    static CoapMessage createAck(const CoapMessage& request, CoapCode responseCode) {
        CoapMessage ack;
        ack.type        = COAP_ACK;
        ack.code        = responseCode;
        ack.messageId   = request.messageId;
        ack.tokenLength = request.tokenLength;
        memcpy(ack.token, request.token, request.tokenLength);
        return ack;
    }

    /**
     * Create a NON response (for NON requests).
     * Uses a new message ID; copies the token from the request.
     */
    static CoapMessage createNon(const CoapMessage& request,
                                  CoapCode responseCode, uint16_t newMsgId) {
        CoapMessage non;
        non.type        = COAP_NON;
        non.code        = responseCode;
        non.messageId   = newMsgId;
        non.tokenLength = request.tokenLength;
        memcpy(non.token, request.token, request.tokenLength);
        return non;
    }

    /** Format a code as "c.dd" for logging. */
    static void codeToString(CoapCode c, char* buf, size_t bufLen) {
        snprintf(buf, bufLen, "%u.%02u", ((uint8_t)c >> 5) & 0x07, (uint8_t)c & 0x1F);
    }
};

#endif // COAP_MESSAGE_H
