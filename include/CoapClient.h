/**
 * @file CoapClient.h
 * @brief CoAP client for downloading images via Block-Wise Transfer
 *
 * Runs on the gateway node. Connects to leaf nodes and pulls JPEG images
 * using CoAP Block2 (RFC 7959) over UDP. Streams blocks directly to SD
 * card and computes Fletcher-16 checksum on-the-fly.
 *
 * Usage:
 *   CoapClient client;
 *   client.begin();
 *
 *   // Fetch image catalogue
 *   uint8_t buf[512]; size_t len = sizeof(buf);
 *   client.get(leafIP, 5683, "info", buf, len);
 *
 *   // Download image with Block2 transfer
 *   TransferStats stats;
 *   client.downloadImage(leafIP, 5683, 0, "/received/img_000.jpg", stats);
 *
 *   // Verify integrity
 *   bool match;
 *   client.verifyChecksum(leafIP, 5683, 0, stats.computedChecksum, match);
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef COAP_CLIENT_H
#define COAP_CLIENT_H

#include <Arduino.h>
#include <WiFiUdp.h>
#include <SD.h>
#include "CoapMessage.h"

// ─── Transfer Result Codes ──────────────────────────────────

enum CoapClientError : uint8_t {
    COAP_CLIENT_OK = 0,
    COAP_CLIENT_TIMEOUT,           // No response after all retries
    COAP_CLIENT_BAD_RESPONSE,      // Unparseable response
    COAP_CLIENT_TOKEN_MISMATCH,    // Response token doesn't match request
    COAP_CLIENT_BLOCK_ERROR,       // Block2 sequence error
    COAP_CLIENT_SD_ERROR,          // File open/write failed
    COAP_CLIENT_CHECKSUM_MISMATCH, // Fletcher-16 mismatch
    COAP_CLIENT_SERVER_ERROR,      // Server returned 4.xx or 5.xx
};

/** Human-readable error names for logging. */
inline const char* coapClientErrorStr(CoapClientError err) {
    switch (err) {
        case COAP_CLIENT_OK:                return "OK";
        case COAP_CLIENT_TIMEOUT:           return "TIMEOUT";
        case COAP_CLIENT_BAD_RESPONSE:      return "BAD_RESPONSE";
        case COAP_CLIENT_TOKEN_MISMATCH:    return "TOKEN_MISMATCH";
        case COAP_CLIENT_BLOCK_ERROR:       return "BLOCK_ERROR";
        case COAP_CLIENT_SD_ERROR:          return "SD_ERROR";
        case COAP_CLIENT_CHECKSUM_MISMATCH: return "CHECKSUM_MISMATCH";
        case COAP_CLIENT_SERVER_ERROR:      return "SERVER_ERROR";
        default:                            return "UNKNOWN";
    }
}

// ─── Transfer Statistics ────────────────────────────────────

struct TransferStats {
    uint32_t totalBytes;        // Payload bytes received
    uint32_t totalBlocks;       // Blocks received
    uint32_t elapsedMs;         // Wall-clock time for transfer
    uint32_t retryCount;        // Total retransmissions
    uint16_t computedChecksum;  // Fletcher-16 computed on-the-fly
    float    throughputKBps;    // KB/s

    void reset() {
        totalBytes = totalBlocks = elapsedMs = retryCount = 0;
        computedChecksum = 0;
        throughputKBps = 0.0f;
    }

    void finalize() {
        if (elapsedMs > 0) {
            throughputKBps = (float)totalBytes / (float)elapsedMs * 1000.0f / 1024.0f;
        }
    }
};

// ─── Tuning Constants ───────────────────────────────────────

static constexpr uint8_t  COAP_CLIENT_MAX_RETRIES = 3;
static constexpr uint32_t COAP_CLIENT_TIMEOUT_MS  = 2000;    // Reduced: local WiFi is fast
static constexpr uint8_t  COAP_CLIENT_WINDOW_SIZE = 3;       // Pipeline: outstanding requests

// ─── Download Resume State ─────────────────────────────────

/** State needed to resume a partially completed Block2 download. */
struct DownloadResumeState {
    uint32_t startBlock;     // Block number to resume from
    uint16_t sum1;           // Fletcher-16 partial accumulator (low)
    uint16_t sum2;           // Fletcher-16 partial accumulator (high)
    uint32_t bytesWritten;   // Bytes already on disk
};

// ─── CoapClient Class ──────────────────────────────────────

class CoapClient {
public:
    CoapClient();

    /**
     * Bind the UDP socket. Call after WiFi is connected.
     * @param localPort  Local UDP port (0 = ephemeral, assigned by OS)
     */
    bool begin(uint16_t localPort = 0);

    /** Release the UDP socket. */
    void stop();

    // ── High-Level Operations ───────────────────────────────

    /**
     * Send a single CON GET request and receive the response payload.
     * For small resources like /info, /checksum/{n}.
     *
     * @param serverIP     Leaf node IP
     * @param serverPort   CoAP port (5683)
     * @param uriPath      Path segments separated by "/" (e.g., "info" or "checksum/0")
     * @param[out] responseBuf  Buffer for response payload
     * @param[in,out] responseLen  In: buffer capacity. Out: bytes received.
     */
    CoapClientError get(IPAddress serverIP, uint16_t serverPort,
                        const char* uriPath,
                        uint8_t* responseBuf, size_t& responseLen);

    /**
     * Send a single CON POST request with a payload.
     * Used for leaf-initiated announce (POST /announce).
     *
     * @param serverIP     Gateway IP
     * @param serverPort   CoAP port (5683)
     * @param uriPath      Path (e.g., "announce")
     * @param payload      Request body
     * @param payloadLen   Payload size in bytes
     */
    CoapClientError post(IPAddress serverIP, uint16_t serverPort,
                         const char* uriPath,
                         const uint8_t* payload, size_t payloadLen);

    /**
     * Download an image via Block2 transfer.
     * Streams blocks to SD card file and computes Fletcher-16 on-the-fly.
     *
     * @param serverIP     Leaf node IP
     * @param serverPort   CoAP port
     * @param imageIndex   Image index on the leaf (/image/{n})
     * @param outputPath   SD card path to write (e.g., "/received/img_000.jpg").
     *                     Pass nullptr for checksum-only mode (no file write).
     * @param[out] stats   Populated during download.
     */
    CoapClientError downloadImage(IPAddress serverIP, uint16_t serverPort,
                                   uint8_t imageIndex,
                                   const char* outputPath,
                                   TransferStats& stats);

    /**
     * Fetch the server's checksum and compare with a local value.
     *
     * @param serverIP       Leaf node IP
     * @param serverPort     CoAP port
     * @param imageIndex     Image index
     * @param localChecksum  Fletcher-16 from downloadImage stats
     * @param[out] match     True if checksums agree
     */
    CoapClientError verifyChecksum(IPAddress serverIP, uint16_t serverPort,
                                    uint8_t imageIndex,
                                    uint16_t localChecksum, bool& match);

    /**
     * Download an image using pipelined Block2 requests.
     * Sends up to COAP_CLIENT_WINDOW_SIZE requests ahead, overlapping
     * server processing with client SD writes for 2-3x throughput.
     *
     * Falls back to sequential download if pipelining causes errors.
     */
    CoapClientError downloadImagePipelined(IPAddress serverIP, uint16_t serverPort,
                                            uint8_t imageIndex,
                                            const char* outputPath,
                                            TransferStats& stats);

    /**
     * Download an image with optional resume from a previous partial transfer.
     * If resume is non-null, opens file in append mode and starts from resume->startBlock.
     */
    CoapClientError downloadImagePipelined(IPAddress serverIP, uint16_t serverPort,
                                            uint8_t imageIndex,
                                            const char* outputPath,
                                            TransferStats& stats,
                                            const DownloadResumeState* resume);

    // ── Accessors ───────────────────────────────────────────
    const TransferStats& lastStats() const { return _lastStats; }

    /** Get current pipeline progress (for resume state persistence). */
    uint32_t lastCompletedBlock() const { return _lastCompletedBlock; }
    uint16_t currentSum1() const { return _currentSum1; }
    uint16_t currentSum2() const { return _currentSum2; }

private:
    WiFiUDP  _udp;
    bool     _running;
    uint16_t _nextMsgId;
    uint8_t  _nextToken;

    uint8_t _txBuf[COAP_MAX_PDU_SIZE];
    uint8_t _rxBuf[COAP_MAX_PDU_SIZE];

    TransferStats _lastStats;

    uint32_t _lastCompletedBlock;
    uint16_t _currentSum1;
    uint16_t _currentSum2;

    // ── Pipelined download reorder buffer (heap-allocated to reduce stack usage)
    struct BufferedBlock {
        uint32_t blockNum;
        uint8_t  data[1024];
        uint16_t length;
        bool     more;
        bool     valid;
    };
    BufferedBlock _reorderBuf[COAP_CLIENT_WINDOW_SIZE];

    // ── Internal Helpers ────────────────────────────────────

    /** Build a CON GET request with Uri-Path options from a "/" delimited path. */
    CoapMessage _buildGetRequest(const char* uriPath);

    /** Build a CON GET request with Block2 option for a specific block number. */
    CoapMessage _buildBlock2Request(const char* uriPath, uint32_t blockNum);

    /**
     * Send a CON request and wait for matching ACK response.
     * Retransmits up to COAP_CLIENT_MAX_RETRIES times with timeout.
     */
    CoapClientError _sendAndWait(CoapMessage& request,
                                  IPAddress serverIP, uint16_t serverPort,
                                  CoapMessage& response, uint32_t& retries);

    /** Check that response matches request (type, message ID, token). */
    bool _validateResponse(const CoapMessage& request, const CoapMessage& response);

    /** Update Fletcher-16 accumulators with new data. */
    static void _updateFletcher16(uint16_t& sum1, uint16_t& sum2,
                                   const uint8_t* data, size_t length);

    /** Serialize and send a message via UDP. */
    void _sendMessage(CoapMessage& msg, IPAddress ip, uint16_t port);
};

#endif // COAP_CLIENT_H
