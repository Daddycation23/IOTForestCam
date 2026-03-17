/**
 * @file AodvRouter.cpp
 * @brief AODV routing engine implementation
 *
 * Implements RREQ flooding, RREP reverse-path forwarding, RERR
 * link break notification, and route table management per RFC 3561.
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "AodvRouter.h"
#include "TaskConfig.h"

static const char* TAG = "AODV";

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Constructor / Init
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

AodvRouter::AodvRouter(LoRaRadio& radio)
    : _radio(radio)
    , _mySeqNum(0)
    , _rreqIdCounter(0)
    , _initialized(false)
    , _discoveryPending(false)
    , _discoveryStartMs(0)
    , _routeDiscoveredCb(nullptr)
{
    memset(_myId, 0, 6);
    memset(_routes, 0, sizeof(_routes));
    memset(_rreqCache, 0, sizeof(_rreqCache));
}

void AodvRouter::begin(const uint8_t myId[6]) {
    memcpy(_myId, myId, 6);
    _mySeqNum = 1;
    _rreqIdCounter = 0;
    _initialized = true;

    // Clear route table
    for (uint8_t i = 0; i < AODV_MAX_ROUTES; i++) {
        _routes[i].active = false;
    }
    // Clear RREQ cache
    for (uint8_t i = 0; i < RREQ_CACHE_SIZE; i++) {
        _rreqCache[i].used = false;
    }

    char idStr[24];
    snprintf(idStr, sizeof(idStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             _myId[0], _myId[1], _myId[2], _myId[3], _myId[4], _myId[5]);
    Serial.printf("[%s] Initialized, nodeId=%s, seqNum=%u\n", TAG, idStr, _mySeqNum);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Tick (periodic maintenance)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void AodvRouter::tick() {
    if (!_initialized) return;

    uint32_t now = millis();

    // ── Expire stale routes ─────────────────────────────────
    for (uint8_t i = 0; i < AODV_MAX_ROUTES; i++) {
        if (_routes[i].active && now >= _routes[i].expiryMs) {
            char idStr[24];
            snprintf(idStr, sizeof(idStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                     _routes[i].destId[0], _routes[i].destId[1], _routes[i].destId[2],
                     _routes[i].destId[3], _routes[i].destId[4], _routes[i].destId[5]);
            Serial.printf("[%s] Route to %s expired\n", TAG, idStr);
            _routes[i].active = false;
        }
    }

    // ── Expire old RREQ cache entries ───────────────────────
    _expireRreqCache();

    // ── Check discovery timeout ─────────────────────────────
    if (_discoveryPending && (now - _discoveryStartMs) >= AODV_RREQ_TIMEOUT_MS) {
        char idStr[24];
        snprintf(idStr, sizeof(idStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 _discoveryDestId[0], _discoveryDestId[1], _discoveryDestId[2],
                 _discoveryDestId[3], _discoveryDestId[4], _discoveryDestId[5]);
        Serial.printf("[%s] Route discovery TIMEOUT for %s\n", TAG, idStr);
        _discoveryPending = false;
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Handle RREQ
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void AodvRouter::handleRREQ(const RreqPacket& rreq, float rssi) {
    if (!_initialized) return;

    // Don't process our own RREQs
    if (_isSelf(rreq.origId)) return;

    // ── Deduplication check ─────────────────────────────────
    if (_isRreqSeen(rreq.origId, rreq.rreqId)) {
        log_d("%s: Dropping duplicate RREQ (id=%lu)", TAG, rreq.rreqId);
        return;
    }
    _cacheRreq(rreq.origId, rreq.rreqId);

    // ── Increment hop count for our processing ──────────────
    uint8_t newHopCount = rreq.hopCount + 1;

    char origStr[24], destStr[24], prevStr[24];
    snprintf(origStr, sizeof(origStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             rreq.origId[0], rreq.origId[1], rreq.origId[2],
             rreq.origId[3], rreq.origId[4], rreq.origId[5]);
    snprintf(destStr, sizeof(destStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             rreq.destId[0], rreq.destId[1], rreq.destId[2],
             rreq.destId[3], rreq.destId[4], rreq.destId[5]);
    snprintf(prevStr, sizeof(prevStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             rreq.prevHopId[0], rreq.prevHopId[1], rreq.prevHopId[2],
             rreq.prevHopId[3], rreq.prevHopId[4], rreq.prevHopId[5]);

    Serial.printf("[%s] RREQ rx: orig=%s dest=%s hops=%u prevHop=%s\n",
                  TAG, origStr, destStr, newHopCount, prevStr);

    // ── Create/update reverse route to originator ───────────
    _upsertRoute(rreq.origId, rreq.prevHopId, newHopCount,
                 rreq.origSeqNum, AODV_ROUTE_LIFETIME_S);

    // ── Am I the destination? ───────────────────────────────
    bool iAmDest = _isSelf(rreq.destId) || _isBroadcast(rreq.destId);

    if (iAmDest) {
        // Increment my sequence number
        if (rreq.destSeqNum >= _mySeqNum) {
            _mySeqNum = rreq.destSeqNum + 1;
        } else {
            _mySeqNum++;
        }

        Serial.printf("[%s] I am the destination — sending RREP (seqNum=%u)\n",
                      TAG, _mySeqNum);

        // ── Generate RREP ───────────────────────────────────
        RrepPacket rrep;
        rrep.flags      = 0;
        rrep.hopCount   = 0;
        memcpy(rrep.destId, _myId, 6);
        rrep.destSeqNum = _mySeqNum;
        memcpy(rrep.origId, rreq.origId, 6);
        rrep.lifetime   = AODV_ROUTE_LIFETIME_S;
        memcpy(rrep.prevHopId, _myId, 6);

        uint8_t buf[64];
        uint8_t len = rrep.serialize(buf, sizeof(buf));
        if (len > 0) {
            // Small random delay to avoid collision with other responders
            vTaskDelay(pdMS_TO_TICKS(random(AODV_RREQ_BACKOFF_MIN, AODV_RREQ_BACKOFF_MAX)));
            _broadcast(buf, len);
        }
        return;
    }

    // ── Do I have a fresh route to the destination? ─────────
    int8_t idx = _findRoute(rreq.destId);
    if (idx >= 0 && _routes[idx].validSeqNum &&
        _routes[idx].destSeqNum >= rreq.destSeqNum) {

        Serial.printf("[%s] Intermediate RREP — I have route to dest (hops=%u)\n",
                      TAG, _routes[idx].hopCount);

        RrepPacket rrep;
        rrep.flags      = 0;
        rrep.hopCount   = _routes[idx].hopCount;
        memcpy(rrep.destId, rreq.destId, 6);
        rrep.destSeqNum = _routes[idx].destSeqNum;
        memcpy(rrep.origId, rreq.origId, 6);
        rrep.lifetime   = AODV_ROUTE_LIFETIME_S;
        memcpy(rrep.prevHopId, _myId, 6);

        uint8_t buf[64];
        uint8_t len = rrep.serialize(buf, sizeof(buf));
        if (len > 0) {
            vTaskDelay(pdMS_TO_TICKS(random(AODV_RREQ_BACKOFF_MIN, AODV_RREQ_BACKOFF_MAX)));
            _broadcast(buf, len);
        }
        return;
    }

    // ── TTL check before rebroadcasting ─────────────────────
    if (newHopCount >= AODV_MAX_TTL) {
        log_d("%s: RREQ TTL expired (hops=%u)", TAG, newHopCount);
        return;
    }

    // ── Rebroadcast RREQ with updated fields ────────────────
    RreqPacket fwd = rreq;
    fwd.hopCount = newHopCount;
    memcpy(fwd.prevHopId, _myId, 6);   // Overwrite prevHop with our ID

    uint8_t buf[64];
    uint8_t len = fwd.serialize(buf, sizeof(buf));
    if (len > 0) {
        vTaskDelay(pdMS_TO_TICKS(random(AODV_RREQ_BACKOFF_MIN, AODV_RREQ_BACKOFF_MAX)));
        _broadcast(buf, len);
        Serial.printf("[%s] RREQ rebroadcast (hops=%u)\n", TAG, newHopCount);
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Handle RREP
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void AodvRouter::handleRREP(const RrepPacket& rrep) {
    if (!_initialized) return;

    // Don't process our own RREPs (we are the destination that generated it)
    if (_isSelf(rrep.destId) && rrep.hopCount == 0) return;

    uint8_t newHopCount = rrep.hopCount + 1;

    char destStr[24], origStr[24], prevStr[24];
    snprintf(destStr, sizeof(destStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             rrep.destId[0], rrep.destId[1], rrep.destId[2],
             rrep.destId[3], rrep.destId[4], rrep.destId[5]);
    snprintf(origStr, sizeof(origStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             rrep.origId[0], rrep.origId[1], rrep.origId[2],
             rrep.origId[3], rrep.origId[4], rrep.origId[5]);
    snprintf(prevStr, sizeof(prevStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             rrep.prevHopId[0], rrep.prevHopId[1], rrep.prevHopId[2],
             rrep.prevHopId[3], rrep.prevHopId[4], rrep.prevHopId[5]);

    Serial.printf("[%s] RREP rx: dest=%s orig=%s hops=%u prevHop=%s\n",
                  TAG, destStr, origStr, newHopCount, prevStr);

    // ── Create/update forward route to the destination ──────
    _upsertRoute(rrep.destId, rrep.prevHopId, newHopCount,
                 rrep.destSeqNum, rrep.lifetime);

    // ── Am I the originator of the RREQ? ────────────────────
    if (_isSelf(rrep.origId)) {
        Serial.printf("[%s] Route DISCOVERED: %s via %s (%u hops)\n",
                      TAG, destStr, prevStr, newHopCount);

        // Mark discovery as complete
        if (_discoveryPending && _macEqual(_discoveryDestId, rrep.destId)) {
            _discoveryPending = false;
        }
        // For broadcast discovery, we may get multiple RREPs
        if (_discoveryPending && _isBroadcast(_discoveryDestId)) {
            // Keep listening for more RREPs (don't clear pending)
        }

        // Fire callback
        if (_routeDiscoveredCb) {
            _routeDiscoveredCb(rrep.destId, newHopCount);
        }
        return;
    }

    // ── Forward RREP toward the originator ──────────────────
    // Check if we have a reverse route to the originator
    int8_t revIdx = _findRoute(rrep.origId);
    if (revIdx < 0) {
        Serial.printf("[%s] No reverse route to originator — dropping RREP\n", TAG);
        return;
    }

    // Forward RREP with updated fields
    RrepPacket fwd = rrep;
    fwd.hopCount = newHopCount;
    memcpy(fwd.prevHopId, _myId, 6);

    uint8_t buf[64];
    uint8_t len = fwd.serialize(buf, sizeof(buf));
    if (len > 0) {
        _broadcast(buf, len);
        Serial.printf("[%s] RREP forwarded toward originator (hops=%u)\n", TAG, newHopCount);
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Handle RERR
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void AodvRouter::handleRERR(const RerrPacket& rerr) {
    if (!_initialized) return;

    bool affected = false;

    for (uint8_t i = 0; i < rerr.destCount; i++) {
        int8_t idx = _findRoute(rerr.entries[i].destId);
        if (idx >= 0 && _routes[idx].active) {
            char idStr[24];
            snprintf(idStr, sizeof(idStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                     rerr.entries[i].destId[0], rerr.entries[i].destId[1],
                     rerr.entries[i].destId[2], rerr.entries[i].destId[3],
                     rerr.entries[i].destId[4], rerr.entries[i].destId[5]);
            Serial.printf("[%s] RERR: Route to %s invalidated\n", TAG, idStr);

            _routes[idx].active = false;
            _routes[idx].destSeqNum = rerr.entries[i].destSeqNum;
            affected = true;
        }
    }

    // Propagate RERR if we were affected
    if (affected) {
        uint8_t buf[64];
        uint8_t len = rerr.serialize(buf, sizeof(buf));
        if (len > 0) {
            _broadcast(buf, len);
            Serial.printf("[%s] RERR rebroadcast\n", TAG);
        }
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Route Discovery
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void AodvRouter::discoverRoute(const uint8_t destId[6]) {
    if (!_initialized) return;

    // Check if we already have a valid route
    if (hasRoute(destId)) {
        char idStr[24];
        snprintf(idStr, sizeof(idStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 destId[0], destId[1], destId[2], destId[3], destId[4], destId[5]);
        Serial.printf("[%s] Route to %s already exists\n", TAG, idStr);
        return;
    }

    _mySeqNum++;
    _rreqIdCounter++;

    RreqPacket rreq;
    rreq.flags      = 0;
    rreq.hopCount   = 0;
    rreq.rreqId     = _rreqIdCounter;
    memcpy(rreq.destId, destId, 6);
    rreq.destSeqNum = 0;   // Unknown
    memcpy(rreq.origId, _myId, 6);
    rreq.origSeqNum = _mySeqNum;
    memcpy(rreq.prevHopId, _myId, 6);

    // Record in dedup cache so we don't reprocess our own RREQ
    _cacheRreq(_myId, _rreqIdCounter);

    // Track pending discovery
    _discoveryPending = true;
    memcpy(_discoveryDestId, destId, 6);
    _discoveryStartMs = millis();

    uint8_t buf[64];
    uint8_t len = rreq.serialize(buf, sizeof(buf));
    if (len > 0) {
        _broadcast(buf, len);

        char idStr[24];
        snprintf(idStr, sizeof(idStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 destId[0], destId[1], destId[2], destId[3], destId[4], destId[5]);
        Serial.printf("[%s] RREQ broadcast for %s (rreqId=%lu, seqNum=%u)\n",
                      TAG, idStr, _rreqIdCounter, _mySeqNum);
    }
}

void AodvRouter::discoverAll() {
    // Broadcast RREQ with destination = FF:FF:FF:FF:FF:FF (all nodes)
    uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    discoverRoute(broadcast);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Link Break Notification
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void AodvRouter::notifyLinkBreak(const uint8_t brokenNodeId[6]) {
    if (!_initialized) return;

    RerrPacket rerr;
    rerr.destCount = 0;

    for (uint8_t i = 0; i < AODV_MAX_ROUTES; i++) {
        if (!_routes[i].active) continue;

        // Check if this route uses the broken node as next hop
        if (_macEqual(_routes[i].nextHopId, brokenNodeId) ||
            _macEqual(_routes[i].destId, brokenNodeId)) {
            if (rerr.destCount < RERR_MAX_DESTS) {
                memcpy(rerr.entries[rerr.destCount].destId, _routes[i].destId, 6);
                _routes[i].destSeqNum++;  // Increment per RFC 3561
                rerr.entries[rerr.destCount].destSeqNum = _routes[i].destSeqNum;
                rerr.destCount++;
            }
            _routes[i].active = false;
        }
    }

    if (rerr.destCount > 0) {
        char idStr[24];
        snprintf(idStr, sizeof(idStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 brokenNodeId[0], brokenNodeId[1], brokenNodeId[2],
                 brokenNodeId[3], brokenNodeId[4], brokenNodeId[5]);
        Serial.printf("[%s] Link break detected: %s — %u routes invalidated, broadcasting RERR\n",
                      TAG, idStr, rerr.destCount);

        uint8_t buf[64];
        uint8_t len = rerr.serialize(buf, sizeof(buf));
        if (len > 0) {
            _broadcast(buf, len);
        }
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Route Table Queries
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool AodvRouter::hasRoute(const uint8_t destId[6]) const {
    return _findRoute(destId) >= 0;
}

bool AodvRouter::getRoute(const uint8_t destId[6], RouteEntry& route) const {
    int8_t idx = _findRoute(destId);
    if (idx < 0) return false;
    route = _routes[idx];
    return true;
}

uint8_t AodvRouter::routeCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < AODV_MAX_ROUTES; i++) {
        if (_routes[i].active) count++;
    }
    return count;
}

void AodvRouter::dumpRoutes() const {
    Serial.println("┌──────────────────────────────────────────────────────────────────────────────┐");
    Serial.println("│  AODV Routing Table                                                          │");
    Serial.println("├────┬───────────────────┬───────────────────┬──────┬────────┬─────────┬───────┤");
    Serial.println("│ #  │ Destination       │ Next Hop          │ Hops │ SeqNum │ TTL (s) │ Valid │");
    Serial.println("├────┼───────────────────┼───────────────────┼──────┼────────┼─────────┼───────┤");

    uint32_t now = millis();
    for (uint8_t i = 0; i < AODV_MAX_ROUTES; i++) {
        if (!_routes[i].active) continue;

        char destStr[24], nextStr[24];
        snprintf(destStr, sizeof(destStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 _routes[i].destId[0], _routes[i].destId[1], _routes[i].destId[2],
                 _routes[i].destId[3], _routes[i].destId[4], _routes[i].destId[5]);
        snprintf(nextStr, sizeof(nextStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 _routes[i].nextHopId[0], _routes[i].nextHopId[1], _routes[i].nextHopId[2],
                 _routes[i].nextHopId[3], _routes[i].nextHopId[4], _routes[i].nextHopId[5]);

        uint32_t remainMs = (_routes[i].expiryMs > now) ? (_routes[i].expiryMs - now) / 1000 : 0;

        Serial.printf("│ %u  │ %s │ %s │  %2u  │ %5u  │  %5lu  │  %s  │\n",
                       i, destStr, nextStr,
                       _routes[i].hopCount, _routes[i].destSeqNum,
                       remainMs,
                       _routes[i].validSeqNum ? "YES" : " NO");
    }

    Serial.println("└────┴───────────────────┴───────────────────┴──────┴────────┴─────────┴───────┘");
    Serial.printf("Active routes: %u, My SeqNum: %u\n\n", routeCount(), _mySeqNum);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Private: Route Table Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

int8_t AodvRouter::_findRoute(const uint8_t destId[6]) const {
    for (uint8_t i = 0; i < AODV_MAX_ROUTES; i++) {
        if (_routes[i].active && _macEqual(_routes[i].destId, destId)) {
            return i;
        }
    }
    return -1;
}

int8_t AodvRouter::_findEmptyRouteSlot() const {
    for (uint8_t i = 0; i < AODV_MAX_ROUTES; i++) {
        if (!_routes[i].active) return i;
    }
    return -1;
}

int8_t AodvRouter::_findOldestRouteSlot() const {
    int8_t oldest = -1;
    uint32_t oldestExpiry = UINT32_MAX;
    for (uint8_t i = 0; i < AODV_MAX_ROUTES; i++) {
        if (_routes[i].active && _routes[i].expiryMs < oldestExpiry) {
            oldestExpiry = _routes[i].expiryMs;
            oldest = i;
        }
    }
    return oldest;
}

void AodvRouter::_upsertRoute(const uint8_t destId[6], const uint8_t nextHopId[6],
                               uint8_t hopCount, uint16_t destSeqNum, uint16_t lifetimeSec) {
    int8_t idx = _findRoute(destId);

    if (idx >= 0) {
        RouteEntry& r = _routes[idx];
        // Update only if new info is fresher (higher seq) or same seq with fewer hops
        bool fresher = (destSeqNum > r.destSeqNum) ||
                       (destSeqNum == r.destSeqNum && hopCount < r.hopCount);
        if (fresher || !r.validSeqNum) {
            memcpy(r.nextHopId, nextHopId, 6);
            r.hopCount    = hopCount;
            r.destSeqNum  = destSeqNum;
            r.validSeqNum = true;
        }
        // Always refresh lifetime
        r.expiryMs = millis() + (uint32_t)lifetimeSec * 1000;
        return;
    }

    // New route — find a slot
    idx = _findEmptyRouteSlot();
    if (idx < 0) {
        idx = _findOldestRouteSlot();
        if (idx < 0) return;
        log_w("%s: Route table full — replacing oldest entry", TAG);
    }

    RouteEntry& r = _routes[idx];
    memcpy(r.destId, destId, 6);
    memcpy(r.nextHopId, nextHopId, 6);
    r.hopCount    = hopCount;
    r.destSeqNum  = destSeqNum;
    r.expiryMs    = millis() + (uint32_t)lifetimeSec * 1000;
    r.active      = true;
    r.validSeqNum = true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Private: RREQ Cache Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool AodvRouter::_isRreqSeen(const uint8_t origId[6], uint32_t rreqId) const {
    for (uint8_t i = 0; i < RREQ_CACHE_SIZE; i++) {
        if (_rreqCache[i].used &&
            _macEqual(_rreqCache[i].origId, origId) &&
            _rreqCache[i].rreqId == rreqId) {
            return true;
        }
    }
    return false;
}

void AodvRouter::_cacheRreq(const uint8_t origId[6], uint32_t rreqId) {
    // Find an empty or oldest slot
    int8_t slot = -1;
    uint32_t oldest = UINT32_MAX;
    for (uint8_t i = 0; i < RREQ_CACHE_SIZE; i++) {
        if (!_rreqCache[i].used) {
            slot = i;
            break;
        }
        if (_rreqCache[i].timestampMs < oldest) {
            oldest = _rreqCache[i].timestampMs;
            slot = i;
        }
    }
    if (slot < 0) slot = 0;

    memcpy(_rreqCache[slot].origId, origId, 6);
    _rreqCache[slot].rreqId      = rreqId;
    _rreqCache[slot].timestampMs = millis();
    _rreqCache[slot].used        = true;
}

void AodvRouter::_expireRreqCache() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < RREQ_CACHE_SIZE; i++) {
        if (_rreqCache[i].used &&
            (now - _rreqCache[i].timestampMs) > RREQ_CACHE_TTL_MS) {
            _rreqCache[i].used = false;
        }
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Private: TX / MAC Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void AodvRouter::_broadcast(const uint8_t* data, uint8_t len) {
    loraSendSafe(data, len);
    loraStartReceiveSafe();
}

bool AodvRouter::_isSelf(const uint8_t id[6]) const {
    return memcmp(id, _myId, 6) == 0;
}

bool AodvRouter::_macEqual(const uint8_t a[6], const uint8_t b[6]) {
    return memcmp(a, b, 6) == 0;
}

bool AodvRouter::_isBroadcast(const uint8_t id[6]) {
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return memcmp(id, bcast, 6) == 0;
}
