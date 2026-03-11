/**
 * @file CoapServer.cpp
 * @brief CoAP server implementation — image serving via Block2 transfer
 *
 * Request Flow (Block-Wise Image Transfer):
 *
 *   Client (Gateway)                          Server (Leaf Node)
 *     |                                          |
 *     |  CON GET /image/0                        |
 *     |  (no Block2 or Block2: NUM=0,SZX=5)      |
 *     |----------------------------------------->|
 *     |                                          |
 *     |  ACK 2.05 Content                        |
 *     |  Block2: NUM=0, M=1, SZX=5              |
 *     |  Size2: <total file size>                |
 *     |  Payload: [512 bytes]                    |
 *     |<-----------------------------------------|
 *     |                                          |
 *     |  CON GET /image/0                        |
 *     |  Block2: NUM=1, SZX=5                    |
 *     |----------------------------------------->|
 *     |                                          |
 *     |  ACK 2.05 Content                        |
 *     |  Block2: NUM=1, M=1, SZX=5              |
 *     |  Payload: [512 bytes]                    |
 *     |<-----------------------------------------|
 *     |           ...                            |
 *     |  ACK 2.05 Content                        |
 *     |  Block2: NUM=N, M=0, SZX=5  (last)      |
 *     |  Payload: [≤512 bytes]                   |
 *     |<-----------------------------------------|
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "CoapServer.h"

static const char* TAG = "CoapServer";

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Constructor
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

CoapServer::CoapServer(StorageReader& storage)
    : _storage(storage)
    , _port(COAP_DEFAULT_PORT)
    , _running(false)
    , _nextMsgId(0)
    , _openImageIndex(-1)
    , _requestCount(0)
    , _blocksSent(0)
{}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Lifecycle
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool CoapServer::begin(uint16_t port) {
    _port = port;
    _nextMsgId = (uint16_t)(esp_random() & 0xFFFF);

    if (_udp.begin(port)) {
        _running = true;
        log_i("%s: Listening on UDP port %u", TAG, port);
        return true;
    }

    log_e("%s: Failed to bind UDP port %u", TAG, port);
    return false;
}

void CoapServer::stop() {
    if (_running) {
        _udp.stop();
        _running = false;
        _storage.closeImage();
        _openImageIndex = -1;
        log_i("%s: Server stopped", TAG);
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Main Loop
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void CoapServer::loop() {
    if (!_running) return;

    int packetSize = _udp.parsePacket();
    if (packetSize <= 0) return;

    if (packetSize > (int)sizeof(_rxBuf)) {
        log_w("%s: Packet too large (%d bytes), dropping", TAG, packetSize);
        return;
    }

    int len = _udp.read(_rxBuf, sizeof(_rxBuf));
    if (len <= 0) return;

    IPAddress remoteIP   = _udp.remoteIP();
    uint16_t  remotePort = _udp.remotePort();

    CoapMessage request;
    if (!request.parse(_rxBuf, len)) {
        log_w("%s: Malformed CoAP packet from %s:%u",
              TAG, remoteIP.toString().c_str(), remotePort);
        return;
    }

    _requestCount++;
    _handleRequest(request, remoteIP, remotePort);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Request Router
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void CoapServer::_handleRequest(CoapMessage& req,
                                 IPAddress remoteIP, uint16_t remotePort)
{
    // We only support GET
    if (req.code != COAP_GET) {
        _sendError(req, COAP_METHOD_NOT_ALLOWED, "Only GET supported",
                   remoteIP, remotePort);
        return;
    }

    // Collect Uri-Path segments
    const uint8_t* segs[4];
    uint16_t       lens[4];
    uint8_t segCount = req.getUriSegments(segs, lens, 4);

    // Route: no path → info
    if (segCount == 0) {
        _handleInfoGet(req, remoteIP, remotePort);
        return;
    }

    // Route: /image/{index}
    if (segCount >= 2 && lens[0] == 5 && memcmp(segs[0], "image", 5) == 0) {
        char idxBuf[8] = {0};
        memcpy(idxBuf, segs[1], min((uint16_t)7, lens[1]));
        int idx = atoi(idxBuf);

        if (idx < 0 || idx >= _storage.imageCount()) {
            _sendError(req, COAP_NOT_FOUND, "Image index out of range",
                       remoteIP, remotePort);
            return;
        }
        _handleImageGet(req, (uint8_t)idx, remoteIP, remotePort);
        return;
    }

    // Route: /info
    if (segCount == 1 && lens[0] == 4 && memcmp(segs[0], "info", 4) == 0) {
        _handleInfoGet(req, remoteIP, remotePort);
        return;
    }

    // Route: /checksum/{index}
    if (segCount >= 2 && lens[0] == 8 && memcmp(segs[0], "checksum", 8) == 0) {
        char idxBuf[8] = {0};
        memcpy(idxBuf, segs[1], min((uint16_t)7, lens[1]));
        int idx = atoi(idxBuf);

        if (idx < 0 || idx >= _storage.imageCount()) {
            _sendError(req, COAP_NOT_FOUND, "Image index out of range",
                       remoteIP, remotePort);
            return;
        }
        _handleChecksumGet(req, (uint8_t)idx, remoteIP, remotePort);
        return;
    }

    // Route: /.well-known/core
    if (segCount >= 2
        && lens[0] == 11 && memcmp(segs[0], ".well-known", 11) == 0
        && lens[1] == 4  && memcmp(segs[1], "core", 4) == 0) {
        _handleWellKnownCore(req, remoteIP, remotePort);
        return;
    }

    _sendError(req, COAP_NOT_FOUND, "Unknown resource", remoteIP, remotePort);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// GET /image/{index}  — Block-Wise Image Transfer
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void CoapServer::_handleImageGet(CoapMessage& req, uint8_t imageIndex,
                                  IPAddress remoteIP, uint16_t remotePort)
{
    ImageInfo info;
    if (!_storage.getImageInfo(imageIndex, info)) {
        _sendError(req, COAP_INTERNAL_ERROR, "Cannot read image info",
                   remoteIP, remotePort);
        return;
    }

    // Determine which block the client is requesting
    uint32_t blockNum = 0;
    Block2Info clientBlock2;
    bool hasBlock2 = req.getBlock2(clientBlock2);

    if (hasBlock2) {
        if (clientBlock2.szx != COAP_BLOCK_SZX) {
            // Client requested a different block size — convert to our block number
            // byte_offset = client_num * client_block_size
            // our_block   = byte_offset / our_block_size
            uint32_t byteOffset = clientBlock2.num * clientBlock2.blockSize();
            blockNum = byteOffset / VSENSOR_BLOCK_SIZE;
        } else {
            blockNum = clientBlock2.num;
        }
    }

    if (blockNum >= info.totalBlocks) {
        _sendError(req, COAP_BAD_REQUEST, "Block number exceeds image size",
                   remoteIP, remotePort);
        return;
    }

    // Open image if not already open (or if a different image is requested)
    if (_openImageIndex != (int8_t)imageIndex) {
        _storage.closeImage();
        if (!_storage.openImage(imageIndex)) {
            _sendError(req, COAP_INTERNAL_ERROR, "Cannot open image file",
                       remoteIP, remotePort);
            _openImageIndex = -1;
            return;
        }
        _openImageIndex = imageIndex;
    }

    // Read the requested block from SD card (uses member _blockBuf to
    // avoid ~520 bytes of stack that could cause overflow with WiFi active)
    if (!_storage.readBlock(blockNum, _blockBuf)) {
        _sendError(req, COAP_INTERNAL_ERROR, "Block read failed",
                   remoteIP, remotePort);
        return;
    }

    // Build response
    CoapMessage resp = _makeResponse(req, COAP_CONTENT);

    // Content-Format: application/octet-stream (42)
    resp.addOptionUint(COAP_OPT_CONTENT_FORMAT, COAP_FMT_APP_OCTET_STREAM);

    // Block2 option: our block number, more flag, SZX=5 (512 bytes)
    Block2Info respBlock2;
    respBlock2.num  = blockNum;
    respBlock2.more = !_blockBuf.isLast;
    respBlock2.szx  = COAP_BLOCK_SZX;
    resp.addOptionUint(COAP_OPT_BLOCK2, respBlock2.encode());

    // Size2: total file size (only sent on first block, per RFC 7959 Section 4)
    if (blockNum == 0) {
        resp.addOptionUint(COAP_OPT_SIZE2, info.fileSize);
    }

    // Payload: image block data
    resp.payload       = _blockBuf.data;
    resp.payloadLength = _blockBuf.length;

    _sendResponse(resp, remoteIP, remotePort);
    _blocksSent++;

    // Yield to WiFi stack between block responses — prevents WiFi task
    // starvation during rapid multi-block transfers.
    yield();

    log_d("%s: image/%u block %lu/%lu (%u B)%s → %s:%u",
          TAG, imageIndex, blockNum, info.totalBlocks - 1,
          _blockBuf.length, _blockBuf.isLast ? " [LAST]" : "",
          remoteIP.toString().c_str(), remotePort);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// GET /info  — Image Catalogue (JSON)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void CoapServer::_handleInfoGet(CoapMessage& req,
                                 IPAddress remoteIP, uint16_t remotePort)
{
    CoapMessage resp = _makeResponse(req, COAP_CONTENT);
    resp.addOptionUint(COAP_OPT_CONTENT_FORMAT, COAP_FMT_APP_JSON);

    // Build JSON catalogue
    static char json[512];
    int pos = snprintf(json, sizeof(json),
                       "{\"count\":%u,\"block_size\":%u,\"images\":[",
                       _storage.imageCount(), (unsigned)VSENSOR_BLOCK_SIZE);

    for (uint8_t i = 0; i < _storage.imageCount() && pos < (int)sizeof(json) - 80; i++) {
        ImageInfo info;
        if (_storage.getImageInfo(i, info)) {
            if (i > 0) json[pos++] = ',';
            pos += snprintf(&json[pos], sizeof(json) - pos,
                            "{\"id\":%u,\"name\":\"%s\",\"size\":%lu,\"blocks\":%lu}",
                            i, info.filename, info.fileSize, info.totalBlocks);
        }
    }
    pos += snprintf(&json[pos], sizeof(json) - pos, "]}");

    resp.payload       = (uint8_t*)json;
    resp.payloadLength = pos;

    _sendResponse(resp, remoteIP, remotePort);

    log_d("%s: Sent /info (%d bytes JSON) → %s:%u",
          TAG, pos, remoteIP.toString().c_str(), remotePort);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// GET /checksum/{index}  — Fletcher-16 Checksum (JSON)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void CoapServer::_handleChecksumGet(CoapMessage& req, uint8_t imageIndex,
                                     IPAddress remoteIP, uint16_t remotePort)
{
    ImageInfo info;
    _storage.getImageInfo(imageIndex, info);

    uint16_t checksum = (uint16_t)info.checksum;   // Pre-computed at scan time

    CoapMessage resp = _makeResponse(req, COAP_CONTENT);
    resp.addOptionUint(COAP_OPT_CONTENT_FORMAT, COAP_FMT_APP_JSON);

    static char json[128];
    int len = snprintf(json, sizeof(json),
                       "{\"id\":%u,\"checksum\":%u,\"size\":%lu}",
                       imageIndex, checksum, info.fileSize);

    resp.payload       = (uint8_t*)json;
    resp.payloadLength = len;

    _sendResponse(resp, remoteIP, remotePort);

    log_d("%s: Sent /checksum/%u = %u → %s:%u",
          TAG, imageIndex, checksum, remoteIP.toString().c_str(), remotePort);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// GET /.well-known/core  — CoRE Resource Discovery (RFC 6690)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void CoapServer::_handleWellKnownCore(CoapMessage& req,
                                       IPAddress remoteIP, uint16_t remotePort)
{
    CoapMessage resp = _makeResponse(req, COAP_CONTENT);
    resp.addOptionUint(COAP_OPT_CONTENT_FORMAT, COAP_FMT_APP_LINK_FORMAT);

    static char links[256];
    int len = snprintf(links, sizeof(links),
        "</info>;rt=\"core.info\";ct=50,"
        "</image>;rt=\"core.image\";ct=42;sz=512,"
        "</checksum>;rt=\"core.checksum\";ct=50");

    resp.payload       = (uint8_t*)links;
    resp.payloadLength = len;

    _sendResponse(resp, remoteIP, remotePort);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Response Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

CoapMessage CoapServer::_makeResponse(const CoapMessage& req, CoapCode code) {
    if (req.type == COAP_CON) {
        // Piggybacked ACK: same message ID
        return CoapMessage::createAck(req, code);
    } else {
        // NON response: new message ID
        return CoapMessage::createNon(req, code, _nextMsgId++);
    }
}

void CoapServer::_sendResponse(CoapMessage& resp,
                                IPAddress remoteIP, uint16_t remotePort)
{
    size_t len = resp.serialize(_txBuf, sizeof(_txBuf));
    if (len == 0) {
        log_e("%s: Serialize failed (buffer too small?)", TAG);
        return;
    }

    _udp.beginPacket(remoteIP, remotePort);
    _udp.write(_txBuf, len);
    _udp.endPacket();
}

void CoapServer::_sendError(CoapMessage& req, CoapCode code,
                             const char* diagnostic,
                             IPAddress remoteIP, uint16_t remotePort)
{
    CoapMessage resp = _makeResponse(req, code);

    if (diagnostic) {
        resp.addOptionUint(COAP_OPT_CONTENT_FORMAT, COAP_FMT_TEXT_PLAIN);
        resp.payload       = (uint8_t*)diagnostic;
        resp.payloadLength = strlen(diagnostic);
    }

    _sendResponse(resp, remoteIP, remotePort);

    char codeBuf[8];
    CoapMessage::codeToString(code, codeBuf, sizeof(codeBuf));
    log_w("%s: %s %s → %s:%u", TAG, codeBuf,
          diagnostic ? diagnostic : "",
          remoteIP.toString().c_str(), remotePort);
}
