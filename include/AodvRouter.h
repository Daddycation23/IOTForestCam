/**
 * @file AodvRouter.h
 * @brief AODV routing engine for ForestCam LoRa mesh network
 *
 * Implements Ad-hoc On-Demand Distance Vector (AODV) routing per RFC 3561,
 * adapted for the LoRa control plane. Runs on ALL node roles (leaf, relay,
 * gateway). Uses the LoRaRadio abstraction for packet TX.
 *
 * Features:
 *   - RREQ flooding with deduplication cache
 *   - RREP generation and forwarding via reverse path
 *   - RERR generation on link breakage
 *   - Route table with per-entry lifetime expiry
 *   - Sequence number management (per-node monotonic counter)
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef AODV_ROUTER_H
#define AODV_ROUTER_H

#include <Arduino.h>
#include <freertos/semphr.h>
#include "LoRaRadio.h"
#include "AodvPacket.h"

// ─── Configuration ──────────────────────────────────────────
static constexpr uint8_t  AODV_MAX_ROUTES       = 12;
static constexpr uint16_t AODV_ROUTE_LIFETIME_S  = 120;    // Default route lifetime (seconds)
static constexpr uint32_t AODV_RREQ_TIMEOUT_MS   = 5000;   // Wait for RREP before giving up
static constexpr uint8_t  AODV_MAX_TTL           = 5;      // Max RREQ hops
static constexpr uint16_t AODV_RREQ_BACKOFF_MIN  = 50;     // Random rebroadcast backoff (ms)
static constexpr uint16_t AODV_RREQ_BACKOFF_MAX  = 300;

// ─── RREQ Dedup Cache ──────────────────────────────────────
static constexpr uint8_t  RREQ_CACHE_SIZE  = 16;
static constexpr uint32_t RREQ_CACHE_TTL_MS = 10000;  // Forget old RREQ IDs after 10s

// ─── Route Entry ────────────────────────────────────────────

struct RouteEntry {
    uint8_t  destId[6];         ///< Destination MAC
    uint8_t  nextHopId[6];      ///< Next hop MAC toward destination
    uint16_t destSeqNum;        ///< Destination sequence number
    uint8_t  hopCount;          ///< Number of hops to destination
    uint32_t createdMs;         ///< millis() when this route was created/refreshed
    uint32_t lifetimeMs;        ///< Route lifetime in milliseconds
    bool     active;            ///< Slot in use?
    bool     validSeqNum;       ///< Have a valid sequence number?
    bool     relayed;           ///< We forwarded RREP for this route (we're an intermediate hop)
};

// ─── Route Discovery Callback ──────────────────────────────

/**
 * Callback fired when a route to a destination is discovered (RREP received).
 * @param destId   The destination MAC that was found.
 * @param hopCount Number of hops.
 */
typedef void (*RouteDiscoveredCb)(const uint8_t destId[6], uint8_t hopCount);

// ─── AodvRouter Class ──────────────────────────────────────

class AodvRouter {
public:
    explicit AodvRouter(LoRaRadio& radio);

    /**
     * Initialize with this node's MAC address.
     * Must be called after WiFi.macAddress() is available.
     */
    void begin(const uint8_t myId[6]);

    /**
     * Periodic maintenance — call from loop().
     * Expires stale routes and checks for discovery timeouts.
     */
    void tick();

    // ── Packet Handlers (called from main loop dispatch) ────

    void handleRREQ(const RreqPacket& rreq, float rssi);
    void handleRREP(const RrepPacket& rrep);
    void handleRERR(const RerrPacket& rerr);

    // ── Route Discovery (gateway typically) ─────────────────

    /**
     * Initiate RREQ flood to discover a route to destId.
     * Non-blocking — check hasRoute() or use the callback.
     */
    void discoverRoute(const uint8_t destId[6]);

    /**
     * Broadcast RREQ for all nodes (destId = broadcast FF:FF:FF:FF:FF:FF).
     * Used by gateway to discover the full topology.
     */
    void discoverAll();

    /** Check if a valid route exists. */
    bool hasRoute(const uint8_t destId[6]);

    /** Get route details. Returns false if no route. */
    bool getRoute(const uint8_t destId[6], RouteEntry& route);

    // ── Link Break Notification ─────────────────────────────

    /**
     * Called when a neighbor is detected as unreachable
     * (e.g., beacon expiry). Generates RERR for affected routes.
     */
    void notifyLinkBreak(const uint8_t brokenNodeId[6]);

    // ── Accessors / Debug ───────────────────────────────────

    uint8_t routeCount();
    void dumpRoutes();
    uint16_t mySeqNum() const { return _mySeqNum; }

    /** True if this node is forwarding traffic for at least one route. */
    bool isRelaying() const { return _isRelaying; }

    /** Number of active routes this node is relaying for. */
    uint8_t relayingForCount() const { return _relayingForCount; }

    /** Set callback for route discovery events. */
    void setRouteDiscoveredCallback(RouteDiscoveredCb cb) { _routeDiscoveredCb = cb; }

    /** Check if a route discovery is currently pending. */
    bool isDiscoveryPending() const { return _discoveryPending; }

private:
    LoRaRadio& _radio;
    uint8_t    _myId[6];
    uint16_t   _mySeqNum;
    uint32_t   _rreqIdCounter;
    bool       _initialized;

    // ── Route Table (guarded by _routeMutex for cross-core access) ──
    SemaphoreHandle_t _routeMutex;
    RouteEntry _routes[AODV_MAX_ROUTES];

    // ── Deferred Broadcast (replaces vTaskDelay in handlers) ──
    bool     _pendingBroadcast;
    uint8_t  _pendingBuf[64];
    uint8_t  _pendingLen;
    uint32_t _pendingSendMs;

    // ── RREQ Dedup Cache ────────────────────────────────────
    struct RreqCacheEntry {
        uint8_t  origId[6];
        uint32_t rreqId;
        uint32_t timestampMs;
        bool     used;
    };
    RreqCacheEntry _rreqCache[RREQ_CACHE_SIZE];

    // ── Pending Discovery State ─────────────────────────────
    bool     _discoveryPending;
    uint8_t  _discoveryDestId[6];
    uint32_t _discoveryStartMs;

    // ── Callback ────────────────────────────────────────────
    RouteDiscoveredCb _routeDiscoveredCb;

    // ── Relay Detection ─────────────────────────────────────
    bool    _isRelaying;        ///< True if forwarding at least one route
    uint8_t _relayingForCount;  ///< Number of active relayed routes

    // ── Route Table Helpers ─────────────────────────────────
    int8_t _findRoute(const uint8_t destId[6]) const;
    int8_t _findEmptyRouteSlot() const;
    int8_t _findOldestRouteSlot() const;
    void   _upsertRoute(const uint8_t destId[6], const uint8_t nextHopId[6],
                         uint8_t hopCount, uint16_t destSeqNum, uint16_t lifetimeSec);

    // ── RREQ Cache Helpers ──────────────────────────────────
    bool _isRreqSeen(const uint8_t origId[6], uint32_t rreqId) const;
    void _cacheRreq(const uint8_t origId[6], uint32_t rreqId);
    void _expireRreqCache();

    // ── TX Helper ───────────────────────────────────────────
    void _broadcast(const uint8_t* data, uint8_t len);
    void _deferBroadcast(const uint8_t* data, uint8_t len, uint32_t delayMs);

    // ── MAC comparison ──────────────────────────────────────
    bool _isSelf(const uint8_t id[6]) const;
    static bool _macEqual(const uint8_t a[6], const uint8_t b[6]);
    static bool _isBroadcast(const uint8_t id[6]);
};

#endif // AODV_ROUTER_H
