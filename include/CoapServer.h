/**
 * @file CoapServer.h
 * @brief CoAP server for serving images via Block-Wise Transfer
 *
 * Provides a UDP-based CoAP server that integrates with StorageReader
 * to serve JPEG images using Block2 transfers (RFC 7959).
 *
 * Resources:
 *   GET /image/{index}    — Image data via Block2 (application/octet-stream)
 *   GET /info             — Image catalogue as JSON
 *   GET /checksum/{index} — Fletcher-16 checksum as JSON
 *   GET /.well-known/core — CoRE Link Format resource discovery (RFC 6690)
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef COAP_SERVER_H
#define COAP_SERVER_H

#include <Arduino.h>
#include <WiFiUdp.h>
#include "CoapMessage.h"
#include "StorageReader.h"
#include "TaskConfig.h"  // AnnounceMessage

class CoapServer {
public:
    explicit CoapServer(StorageReader& storage);

    /**
     * Start listening for CoAP requests on the given UDP port.
     * @param port UDP port (default: 5683, the CoAP standard port)
     * @return true if the socket was bound successfully.
     */
    bool begin(uint16_t port = COAP_DEFAULT_PORT);

    /** Stop the server and release resources. */
    void stop();

    /**
     * Process one incoming CoAP packet (if available).
     * Call this from Arduino loop().
     */
    void loop();

    // ── Statistics ───────────────────────────────────────────
    uint32_t requestCount() const { return _requestCount; }
    uint32_t blocksSent()   const { return _blocksSent; }

private:
    StorageReader& _storage;
    WiFiUDP  _udp;
    uint16_t _port;
    bool     _running;

    // Packet buffers
    uint8_t _rxBuf[COAP_MAX_PDU_SIZE];
    uint8_t _txBuf[COAP_MAX_PDU_SIZE];

    // Message ID counter for NON responses
    uint16_t _nextMsgId;

    // Track which image is currently open (avoids repeated open/close)
    int8_t _openImageIndex;

    // Reusable block buffer — kept as a member to avoid ~520 bytes on the
    // stack during _handleImageGet(), preventing stack overflow when WiFi
    // interrupts are also active.
    BlockReadResult _blockBuf;

    // Counters
    uint32_t _requestCount;
    uint32_t _blocksSent;

    // ── Internal handlers ────────────────────────────────────
    void _handleRequest(CoapMessage& req, IPAddress remoteIP, uint16_t remotePort);

    void _handleImageGet(CoapMessage& req, uint8_t imageIndex,
                         IPAddress remoteIP, uint16_t remotePort);

    void _handleInfoGet(CoapMessage& req,
                        IPAddress remoteIP, uint16_t remotePort);

    void _handleChecksumGet(CoapMessage& req, uint8_t imageIndex,
                            IPAddress remoteIP, uint16_t remotePort);

    void _handleWellKnownCore(CoapMessage& req,
                              IPAddress remoteIP, uint16_t remotePort);

    void _handleAnnouncePost(CoapMessage& req,
                             IPAddress remoteIP, uint16_t remotePort);

    // ── Response helpers ─────────────────────────────────────

    /** Build a response message for the given request (handles CON vs NON). */
    CoapMessage _makeResponse(const CoapMessage& req, CoapCode code);

    /** Serialize and send a response via UDP. */
    void _sendResponse(CoapMessage& resp, IPAddress remoteIP, uint16_t remotePort);

    /** Send an error response with an optional diagnostic payload. */
    void _sendError(CoapMessage& req, CoapCode code, const char* diagnostic,
                    IPAddress remoteIP, uint16_t remotePort);
};

#endif // COAP_SERVER_H
