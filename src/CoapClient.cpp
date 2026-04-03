/**
 * @file CoapClient.cpp
 * @brief CoAP client implementation — Block2 image download with integrity check
 *
 * Download Flow:
 *
 *   Gateway (Client)                         Leaf Node (Server)
 *     |                                          |
 *     |  CON GET /image/0                        |
 *     |  Block2: NUM=0, SZX=5                    |
 *     |----------------------------------------->|
 *     |                                          |
 *     |  ACK 2.05 Content                        |
 *     |  Block2: NUM=0, M=1, SZX=5              |
 *     |  Size2: 27822                            |
 *     |  Payload: [512 bytes]                    |
 *     |<-----------------------------------------|
 *     |  → write to SD, update checksum          |
 *     |                                          |
 *     |  CON GET /image/0                        |
 *     |  Block2: NUM=1, SZX=5                    |
 *     |----------------------------------------->|
 *     |          ...                             |
 *     |                                          |
 *     |  ACK 2.05 Content                        |
 *     |  Block2: NUM=N, M=0, SZX=5  [LAST]      |
 *     |<-----------------------------------------|
 *     |  → close file, verify checksum           |
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "CoapClient.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* TAG = "CoapClient";

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Constructor
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

CoapClient::CoapClient()
    : _running(false)
    , _nextMsgId(0)
    , _nextToken(0)
    , _lastCompletedBlock(0)
    , _currentSum1(0)
    , _currentSum2(0)
{}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Lifecycle
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool CoapClient::begin(uint16_t localPort) {
    _nextMsgId = (uint16_t)(esp_random() & 0xFFFF);
    _nextToken = (uint8_t)(esp_random() & 0xFF);

    if (_udp.begin(localPort)) {
        _running = true;
        log_i("%s: Ready (local port %u)", TAG, localPort);
        return true;
    }

    log_e("%s: Failed to bind UDP port %u", TAG, localPort);
    return false;
}

void CoapClient::stop() {
    if (_running) {
        _udp.stop();
        _running = false;
        log_i("%s: Stopped", TAG);
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// GET — Single request/response (no Block2)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

CoapClientError CoapClient::get(IPAddress serverIP, uint16_t serverPort,
                                 const char* uriPath,
                                 uint8_t* responseBuf, size_t& responseLen)
{
    CoapMessage request = _buildGetRequest(uriPath);

    CoapMessage response;
    uint32_t retries = 0;
    CoapClientError err = _sendAndWait(request, serverIP, serverPort,
                                        response, retries);
    if (err != COAP_CLIENT_OK) return err;

    // Check for server error codes
    if ((uint8_t)response.code >= 128) {
        log_w("%s: Server error on GET /%s", TAG, uriPath);
        return COAP_CLIENT_SERVER_ERROR;
    }

    // Copy payload to caller's buffer
    size_t copyLen = min((size_t)response.payloadLength, responseLen);
    if (response.payload && copyLen > 0) {
        memcpy(responseBuf, response.payload, copyLen);
    }
    responseLen = copyLen;

    return COAP_CLIENT_OK;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// POST — Simple Request with Payload
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

CoapClientError CoapClient::post(IPAddress serverIP, uint16_t serverPort,
                                  const char* uriPath,
                                  const uint8_t* payload, size_t payloadLen)
{
    // Build a CON POST request reusing the GET builder for URI setup
    CoapMessage request = _buildGetRequest(uriPath);
    request.code = COAP_POST;  // Override GET → POST

    // Attach payload
    if (payload && payloadLen > 0 && payloadLen <= 1024) {
        request.payload       = const_cast<uint8_t*>(payload);
        request.payloadLength = payloadLen;
    }

    CoapMessage response;
    uint32_t retries = 0;
    CoapClientError err = _sendAndWait(request, serverIP, serverPort,
                                        response, retries);
    if (err != COAP_CLIENT_OK) return err;

    // Accept 2.01 Created or 2.04 Changed as success
    if (response.code != COAP_CREATED && response.code != COAP_CHANGED) {
        log_w("%s: POST /%s — unexpected response code %u.%02u",
              TAG, uriPath, response.code >> 5, response.code & 0x1F);
        return COAP_CLIENT_SERVER_ERROR;
    }

    log_i("%s: POST /%s — OK (retries=%lu)", TAG, uriPath, retries);
    return COAP_CLIENT_OK;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Download Image — Block2 Transfer
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

CoapClientError CoapClient::downloadImage(IPAddress serverIP, uint16_t serverPort,
                                           uint8_t imageIndex,
                                           const char* outputPath,
                                           TransferStats& stats)
{
    stats.reset();
    uint32_t startMs = millis();

    // Build URI path: "image/{n}"
    char uriPath[16];
    snprintf(uriPath, sizeof(uriPath), "image/%u", imageIndex);

    // Open output file if requested
    File outFile;
    bool writeToSD = (outputPath != nullptr);
    if (writeToSD) {
        if (SD.exists(outputPath)) SD.remove(outputPath);  // Overwrite previous
        outFile = SD.open(outputPath, FILE_WRITE);
        if (!outFile) {
            log_e("%s: Failed to open %s for writing", TAG, outputPath);
            return COAP_CLIENT_SD_ERROR;
        }
        log_i("%s: Writing to %s", TAG, outputPath);
    }

    // Fletcher-16 accumulators
    uint16_t sum1 = 0, sum2 = 0;

    uint32_t blockNum = 0;
    uint32_t totalSize = 0;  // From Size2 option on first block
    bool moreBlocks = true;

    while (moreBlocks) {
        // Build CON GET with Block2 option
        CoapMessage request = _buildBlock2Request(uriPath, blockNum);

        // Send and wait for response
        CoapMessage response;
        uint32_t retries = 0;
        CoapClientError err = _sendAndWait(request, serverIP, serverPort,
                                            response, retries);
        stats.retryCount += retries;

        if (err != COAP_CLIENT_OK) {
            if (writeToSD) outFile.close();
            return err;
        }

        // Validate response
        if (!_validateResponse(request, response)) {
            if (writeToSD) outFile.close();
            return COAP_CLIENT_TOKEN_MISMATCH;
        }

        // Must be 2.05 Content
        if (response.code != COAP_CONTENT) {
            log_w("%s: Unexpected response code %u on block %lu",
                  TAG, (uint8_t)response.code, blockNum);
            if (writeToSD) outFile.close();
            return COAP_CLIENT_SERVER_ERROR;
        }

        // Extract Block2 from response
        Block2Info block2;
        if (!response.getBlock2(block2)) {
            log_e("%s: No Block2 option in response for block %lu", TAG, blockNum);
            if (writeToSD) outFile.close();
            return COAP_CLIENT_BLOCK_ERROR;
        }

        // Verify block number matches what we requested
        if (block2.num != blockNum) {
            log_e("%s: Block number mismatch: expected %lu, got %lu",
                  TAG, blockNum, block2.num);
            if (writeToSD) outFile.close();
            return COAP_CLIENT_BLOCK_ERROR;
        }

        // Extract Size2 on first block (total file size hint)
        if (blockNum == 0) {
            const CoapOption* size2Opt = response.findOption(COAP_OPT_SIZE2);
            if (size2Opt) {
                totalSize = size2Opt->asUint();
                log_i("%s: Image %u total size: %lu bytes", TAG, imageIndex, totalSize);
            }
        }

        // Update Fletcher-16 checksum with payload
        if (response.payload && response.payloadLength > 0) {
            _updateFletcher16(sum1, sum2, response.payload, response.payloadLength);

            // Write block to SD card
            if (writeToSD) {
                size_t written = outFile.write(response.payload, response.payloadLength);
                if (written != response.payloadLength) {
                    log_e("%s: SD write error on block %lu", TAG, blockNum);
                    outFile.close();
                    return COAP_CLIENT_SD_ERROR;
                }
            }
        }

        // Update stats
        stats.totalBytes += response.payloadLength;
        stats.totalBlocks++;

        // Progress logging
        if (blockNum % 50 == 0 || !block2.more) {
            if (totalSize > 0) {
                Serial.printf("  Block %lu — %lu/%lu bytes (%.1f%%)\n",
                              blockNum, stats.totalBytes, totalSize,
                              (float)stats.totalBytes / totalSize * 100.0f);
            } else {
                Serial.printf("  Block %lu — %lu bytes\n",
                              blockNum, stats.totalBytes);
            }
        }

        moreBlocks = block2.more;
        blockNum++;
    }

    // Finalize
    if (writeToSD) outFile.close();

    stats.computedChecksum = (sum2 << 8) | sum1;
    stats.elapsedMs = millis() - startMs;
    stats.finalize();
    _lastStats = stats;

    log_i("%s: Download complete — %lu bytes, %lu blocks, %lu ms, %.1f KB/s",
          TAG, stats.totalBytes, stats.totalBlocks,
          stats.elapsedMs, stats.throughputKBps);

    return COAP_CLIENT_OK;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Download Image — Pipelined Block2 Transfer
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

CoapClientError CoapClient::downloadImagePipelined(IPAddress serverIP, uint16_t serverPort,
                                                     uint8_t imageIndex,
                                                     const char* outputPath,
                                                     TransferStats& stats)
{
    return downloadImagePipelined(serverIP, serverPort, imageIndex, outputPath, stats, nullptr);
}

CoapClientError CoapClient::downloadImagePipelined(IPAddress serverIP, uint16_t serverPort,
                                                     uint8_t imageIndex,
                                                     const char* outputPath,
                                                     TransferStats& stats,
                                                     const DownloadResumeState* resume)
{
    stats.reset();
    uint32_t startMs = millis();

    // Initialize tracking members
    _lastCompletedBlock = resume ? resume->startBlock : 0;
    _currentSum1 = resume ? resume->sum1 : 0;
    _currentSum2 = resume ? resume->sum2 : 0;

    char uriPath[16];
    snprintf(uriPath, sizeof(uriPath), "image/%u", imageIndex);

    // Open output file
    File outFile;
    bool writeToSD = (outputPath != nullptr);
    if (writeToSD) {
        if (resume != nullptr && resume->startBlock > 0) {
            // Resume mode: append to existing file
            if (outputPath && SD.exists(outputPath)) {
                outFile = SD.open(outputPath, FILE_APPEND);
                if (!outFile) {
                    log_w("%s: Resume file %s not openable, falling back to fresh download", TAG, outputPath);
                    resume = nullptr;  // Fall back to fresh
                }
            } else {
                log_w("%s: Resume file %s does not exist, falling back to fresh download", TAG, outputPath);
                resume = nullptr;  // Fall back to fresh
            }
        }
        if (resume == nullptr || resume->startBlock == 0) {
            // Fresh download
            if (SD.exists(outputPath)) SD.remove(outputPath);
            outFile = SD.open(outputPath, FILE_WRITE);
        }
        if (!outFile) {
            log_e("%s: Failed to open %s for writing", TAG, outputPath);
            return COAP_CLIENT_SD_ERROR;
        }
    }

    // Fletcher-16 accumulators
    uint16_t sum1 = resume ? resume->sum1 : 0;
    uint16_t sum2 = resume ? resume->sum2 : 0;

    // Pipeline state
    uint32_t nextToSend    = resume ? resume->startBlock : 0;    // Next block number to request
    uint32_t nextToReceive = resume ? resume->startBlock : 0;    // Next block number we expect in-order

    // Pre-populate stats if resuming
    if (resume && resume->startBlock > 0) {
        stats.totalBytes = resume->bytesWritten;
        log_i("%s: Resuming from block %u (sum1=%u, sum2=%u, %u bytes on disk)",
              TAG, resume->startBlock, resume->sum1, resume->sum2, resume->bytesWritten);
    }
    uint32_t totalBlocks   = 0;    // Estimated total (updated from Size2)
    bool     lastBlockSeen = false;
    uint8_t  outstanding   = 0;

    // Track outstanding requests by their message ID and block number
    struct PendingRequest {
        uint16_t msgId;
        uint8_t  token;
        uint32_t blockNum;
        uint32_t sentMs;
        bool     active;
    };
    PendingRequest pending[COAP_CLIENT_WINDOW_SIZE] = {};

    // Use class-member reorder buffer to reduce stack usage
    memset(_reorderBuf, 0, sizeof(_reorderBuf));

    auto sendBlockRequest = [&](uint32_t blockNum) -> bool {
        // Find free slot
        int slot = -1;
        for (uint8_t i = 0; i < COAP_CLIENT_WINDOW_SIZE; i++) {
            if (!pending[i].active) { slot = i; break; }
        }
        if (slot < 0) return false;

        CoapMessage request = _buildBlock2Request(uriPath, blockNum);
        pending[slot].msgId    = request.messageId;
        pending[slot].token    = request.token[0];
        pending[slot].blockNum = blockNum;
        pending[slot].sentMs   = millis();
        pending[slot].active   = true;

        _sendMessage(request, serverIP, serverPort);
        outstanding++;
        return true;
    };

    auto findPendingSlot = [&](uint16_t msgId) -> int {
        for (uint8_t i = 0; i < COAP_CLIENT_WINDOW_SIZE; i++) {
            if (pending[i].active && pending[i].msgId == msgId) return i;
        }
        return -1;
    };

    auto processInOrderBlocks = [&]() -> CoapClientError {
        // Write any buffered blocks that are now in order
        for (;;) {
            int found = -1;
            for (uint8_t i = 0; i < COAP_CLIENT_WINDOW_SIZE; i++) {
                if (_reorderBuf[i].valid && _reorderBuf[i].blockNum == nextToReceive) {
                    found = i;
                    break;
                }
            }
            if (found < 0) break;

            BufferedBlock& blk = _reorderBuf[found];

            // Update checksum
            _updateFletcher16(sum1, sum2, blk.data, blk.length);

            // Write to SD
            if (writeToSD) {
                size_t written = outFile.write(blk.data, blk.length);
                if (written != blk.length) {
                    log_e("%s: SD write error on block %lu", TAG, blk.blockNum);
                    return COAP_CLIENT_SD_ERROR;
                }
            }

            stats.totalBytes += blk.length;
            stats.totalBlocks++;

            // Update resume tracking state
            _lastCompletedBlock = blk.blockNum + 1;  // next block to request on resume
            _currentSum1 = sum1;
            _currentSum2 = sum2;

            if (!blk.more) lastBlockSeen = true;

            // Progress logging
            if (nextToReceive % 50 == 0 || !blk.more) {
                if (totalBlocks > 0) {
                    Serial.printf("  Block %lu — %lu bytes (%.1f%%)\n",
                                  nextToReceive, stats.totalBytes,
                                  (float)stats.totalBytes / (totalBlocks * 1024) * 100.0f);
                } else {
                    Serial.printf("  Block %lu — %lu bytes\n",
                                  nextToReceive, stats.totalBytes);
                }
            }

            blk.valid = false;
            nextToReceive++;
        }
        return COAP_CLIENT_OK;
    };

    // ── Initial burst: send first WINDOW_SIZE requests ───────
    log_i("%s: Pipelined download — image/%u, window=%u", TAG, imageIndex, COAP_CLIENT_WINDOW_SIZE);

    for (uint8_t i = 0; i < COAP_CLIENT_WINDOW_SIZE; i++) {
        sendBlockRequest(nextToSend++);
    }

    // ── Main receive loop ────────────────────────────────────
    uint32_t lastActivityMs = millis();
    uint32_t overallTimeout = 30000;  // 30s overall timeout per image
    bool retried = false;

    while (!lastBlockSeen || nextToReceive < nextToSend) {
        // Check for overall timeout — retry once before giving up
        if (millis() - lastActivityMs > overallTimeout) {
            if (!retried) {
                log_w("%s: Pipelined download timeout — retrying image from block 0", TAG);
                retried = true;

                // Reset pipeline state
                for (uint8_t i = 0; i < COAP_CLIENT_WINDOW_SIZE; i++) {
                    pending[i].active = false;
                    _reorderBuf[i].valid = false;
                }
                outstanding = 0;
                nextToSend = 0;
                nextToReceive = 0;
                lastBlockSeen = false;
                totalBlocks = 0;
                sum1 = 0;
                sum2 = 0;
                stats.reset();

                // Delete partial file and reopen
                if (writeToSD) {
                    outFile.close();
                    if (outputPath && SD.exists(outputPath)) SD.remove(outputPath);
                    outFile = SD.open(outputPath, FILE_WRITE);
                    if (!outFile) {
                        log_e("%s: Failed to reopen %s for retry", TAG, outputPath);
                        return COAP_CLIENT_SD_ERROR;
                    }
                }

                // Re-send initial burst
                for (uint8_t i = 0; i < COAP_CLIENT_WINDOW_SIZE; i++) {
                    sendBlockRequest(nextToSend++);
                }
                lastActivityMs = millis();
                continue;
            }

            log_e("%s: Pipelined download overall timeout (after retry)", TAG);
            if (writeToSD) outFile.close();
            return COAP_CLIENT_TIMEOUT;
        }

        // Check for individual request timeouts and retransmit
        for (uint8_t i = 0; i < COAP_CLIENT_WINDOW_SIZE; i++) {
            if (pending[i].active && millis() - pending[i].sentMs > COAP_CLIENT_TIMEOUT_MS) {
                stats.retryCount++;
                log_d("%s: Retransmitting block %lu", TAG, pending[i].blockNum);
                CoapMessage retry = _buildBlock2Request(uriPath, pending[i].blockNum);
                retry.messageId = pending[i].msgId;  // Keep same msg ID
                retry.token[0]  = pending[i].token;
                _sendMessage(retry, serverIP, serverPort);
                pending[i].sentMs = millis();
            }
        }

        // Poll for responses
        int packetSize = _udp.parsePacket();
        if (packetSize > 0 && packetSize <= (int)sizeof(_rxBuf)) {
            int len = _udp.read(_rxBuf, sizeof(_rxBuf));
            if (len > 0) {
                CoapMessage response;
                if (response.parse(_rxBuf, len)) {
                    int slot = findPendingSlot(response.messageId);
                    if (slot >= 0 && response.type == COAP_ACK &&
                        response.tokenLength == 1 &&
                        response.token[0] == pending[slot].token)
                    {
                        lastActivityMs = millis();
                        uint32_t blockNum = pending[slot].blockNum;
                        pending[slot].active = false;
                        outstanding--;

                        // Detect server error responses
                        if (response.code != COAP_CONTENT) {
                            log_e("%s: Server error on block %lu (code=%u.%02u)",
                                  TAG, blockNum, response.code >> 5, response.code & 0x1F);
                            if (writeToSD) outFile.close();
                            return COAP_CLIENT_SERVER_ERROR;
                        }

                        // Extract Block2 info
                        Block2Info block2;
                        if (response.getBlock2(block2)) {
                            // Get total size from first block
                            if (blockNum == 0) {
                                const CoapOption* size2Opt = response.findOption(COAP_OPT_SIZE2);
                                if (size2Opt) {
                                    uint32_t totalSize = size2Opt->asUint();
                                    totalBlocks = (totalSize + block2.blockSize() - 1) / block2.blockSize();
                                    log_i("%s: Image %u — %lu bytes, ~%lu blocks",
                                          TAG, imageIndex, totalSize, totalBlocks);
                                }
                            }

                            // Buffer the response data
                            int bufSlot = -1;
                            for (uint8_t b = 0; b < COAP_CLIENT_WINDOW_SIZE; b++) {
                                if (!_reorderBuf[b].valid) { bufSlot = b; break; }
                            }
                            if (bufSlot >= 0 && response.payload && response.payloadLength > 0) {
                                _reorderBuf[bufSlot].blockNum = blockNum;
                                memcpy(_reorderBuf[bufSlot].data, response.payload,
                                       min((size_t)response.payloadLength, sizeof(_reorderBuf[bufSlot].data)));
                                _reorderBuf[bufSlot].length = response.payloadLength;
                                _reorderBuf[bufSlot].more = block2.more;
                                _reorderBuf[bufSlot].valid = true;
                            } else if (bufSlot < 0) {
                                // Reorder buffer full — re-activate the pending slot so
                                // the timeout logic will retransmit this block
                                log_w("%s: Reorder buffer full for block %lu — will retry",
                                      TAG, blockNum);
                                pending[slot].active = true;
                                pending[slot].sentMs = millis();  // Reset timeout
                                outstanding++;
                            }

                            // Process in-order blocks
                            CoapClientError procErr = processInOrderBlocks();
                            if (procErr != COAP_CLIENT_OK) {
                                if (writeToSD) outFile.close();
                                return procErr;
                            }

                            // Send next block request if more to go
                            if (!lastBlockSeen && outstanding < COAP_CLIENT_WINDOW_SIZE) {
                                // Only send if we haven't already seen the last block
                                if (totalBlocks == 0 || nextToSend < totalBlocks) {
                                    sendBlockRequest(nextToSend++);
                                }
                            }
                        }
                    }
                }
            }
        }

        vTaskDelay(1);  // Yield
    }

    // Finalize
    if (writeToSD) outFile.close();

    stats.computedChecksum = (sum2 << 8) | sum1;
    stats.elapsedMs = millis() - startMs;
    stats.finalize();
    _lastStats = stats;

    log_i("%s: Pipelined download complete — %lu bytes, %lu blocks, %lu ms, %.1f KB/s",
          TAG, stats.totalBytes, stats.totalBlocks,
          stats.elapsedMs, stats.throughputKBps);

    return COAP_CLIENT_OK;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Verify Checksum
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

CoapClientError CoapClient::verifyChecksum(IPAddress serverIP, uint16_t serverPort,
                                            uint8_t imageIndex,
                                            uint16_t localChecksum, bool& match)
{
    match = false;

    // GET /checksum/{n}
    char uriPath[20];
    snprintf(uriPath, sizeof(uriPath), "checksum/%u", imageIndex);

    uint8_t buf[129];                       // +1 byte for null terminator
    size_t len = sizeof(buf) - 1;           // Reserve space for '\0'
    CoapClientError err = get(serverIP, serverPort, uriPath, buf, len);
    if (err != COAP_CLIENT_OK) return err;

    // Response is JSON: {"id":0,"checksum":14843,"size":27822}
    // Parse the checksum value
    buf[len] = '\0';
    const char* json = (const char*)buf;
    const char* checksumKey = strstr(json, "\"checksum\":");
    if (!checksumKey) {
        log_e("%s: Cannot parse checksum from response", TAG);
        return COAP_CLIENT_BAD_RESPONSE;
    }

    uint16_t serverChecksum = (uint16_t)atoi(checksumKey + 11);

    match = (localChecksum == serverChecksum);

    log_i("%s: Checksum verify — local=%u, server=%u → %s",
          TAG, localChecksum, serverChecksum, match ? "PASS" : "FAIL");

    return COAP_CLIENT_OK;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Request Builders
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

CoapMessage CoapClient::_buildGetRequest(const char* uriPath) {
    CoapMessage msg;
    msg.type      = COAP_CON;
    msg.code      = COAP_GET;
    msg.messageId = _nextMsgId++;

    // Single-byte token for sequential requests
    msg.tokenLength = 1;
    msg.token[0]    = _nextToken++;

    // Parse "path/segments" into separate Uri-Path options
    char pathCopy[64];
    strncpy(pathCopy, uriPath, sizeof(pathCopy) - 1);
    pathCopy[sizeof(pathCopy) - 1] = '\0';

    char* savePtr = nullptr;
    char* segment = strtok_r(pathCopy, "/", &savePtr);
    while (segment != nullptr) {
        msg.addUriPath(segment);
        segment = strtok_r(nullptr, "/", &savePtr);
    }

    return msg;
}

CoapMessage CoapClient::_buildBlock2Request(const char* uriPath, uint32_t blockNum) {
    CoapMessage msg = _buildGetRequest(uriPath);

    // Block2 option: requesting specific block
    // Client sets M=0 in request (RFC 7959 Section 2.3)
    Block2Info block2;
    block2.num  = blockNum;
    block2.more = false;
    block2.szx  = COAP_BLOCK_SZX;
    msg.addOptionUint(COAP_OPT_BLOCK2, block2.encode());

    return msg;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Send and Wait — CON Retransmission
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

CoapClientError CoapClient::_sendAndWait(CoapMessage& request,
                                          IPAddress serverIP, uint16_t serverPort,
                                          CoapMessage& response,
                                          uint32_t& retries)
{
    retries = 0;

    for (uint8_t attempt = 0; attempt <= COAP_CLIENT_MAX_RETRIES; attempt++) {
        if (attempt > 0) {
            retries++;
            log_d("%s: Retry %u for msgId=%u", TAG, attempt, request.messageId);
        }

        // Send the request
        _sendMessage(request, serverIP, serverPort);

        // Wait for matching response
        uint32_t startWait = millis();
        while (millis() - startWait < COAP_CLIENT_TIMEOUT_MS) {
            int packetSize = _udp.parsePacket();
            if (packetSize > 0 && packetSize <= (int)sizeof(_rxBuf)) {
                int len = _udp.read(_rxBuf, sizeof(_rxBuf));
                if (len > 0 && response.parse(_rxBuf, len)) {
                    if (_validateResponse(request, response)) {
                        return COAP_CLIENT_OK;
                    }
                    // Not our response — keep waiting
                }
            }
            vTaskDelay(1);  // Yield to RTOS
        }

        log_w("%s: Timeout attempt %u/%u for msgId=%u",
              TAG, attempt + 1, COAP_CLIENT_MAX_RETRIES + 1, request.messageId);
    }

    return COAP_CLIENT_TIMEOUT;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Response Validation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool CoapClient::_validateResponse(const CoapMessage& request,
                                    const CoapMessage& response)
{
    // CON request → expect ACK with same message ID
    if (request.type == COAP_CON) {
        if (response.type != COAP_ACK) return false;
        if (response.messageId != request.messageId) return false;
    }

    // Token must match
    if (response.tokenLength != request.tokenLength) return false;
    if (memcmp(response.token, request.token, request.tokenLength) != 0) {
        return false;
    }

    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Fletcher-16 — matches StorageReader::computeChecksum() exactly
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void CoapClient::_updateFletcher16(uint16_t& sum1, uint16_t& sum2,
                                    const uint8_t* data, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        sum1 = (sum1 + data[i]) % 255;
        sum2 = (sum2 + sum1) % 255;
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Send Message
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void CoapClient::_sendMessage(CoapMessage& msg, IPAddress ip, uint16_t port) {
    size_t len = msg.serialize(_txBuf, sizeof(_txBuf));
    if (len == 0) {
        log_e("%s: Serialize failed", TAG);
        return;
    }

    _udp.beginPacket(ip, port);
    _udp.write(_txBuf, len);
    _udp.endPacket();
}
