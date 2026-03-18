/**
 * @file HarvestLoop.h
 * @brief Gateway harvest state machine — sequential multi-node image download
 *
 * Orchestrates the process of connecting to each discovered leaf node's
 * WiFi AP and downloading images via CoAP Block2 transfer. Supports both
 * direct (1-hop) and multi-hop (via relay) harvesting using AODV routes.
 *
 * State Flow (direct harvest):
 *   IDLE --> START --> ROUTE_DISCOVERY --> DISCONNECT --> CONNECT --> COAP_INIT
 *        --> DOWNLOAD --> NEXT --> (loop) --> DONE --> IDLE
 *
 * State Flow (multi-hop harvest via relay):
 *   ... --> RELAY_CMD --> RELAY_WAIT --> DISCONNECT --> CONNECT(relay)
 *       --> COAP_INIT --> DOWNLOAD --> NEXT --> ...
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef HARVEST_LOOP_H
#define HARVEST_LOOP_H

#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>
#include "NodeRegistry.h"
#include "CoapClient.h"
#include "CoapMessage.h"
#include "AodvRouter.h"
#include "AodvPacket.h"
#include "LoRaRadio.h"

// ─── Harvest States ──────────────────────────────────────────

enum HarvestState : uint8_t {
    HARVEST_IDLE,               ///< Listening for beacons, not harvesting
    HARVEST_START,              ///< Begin a harvest cycle
    HARVEST_ROUTE_DISCOVERY,    ///< Broadcast RREQ for all nodes (AODV)
    HARVEST_DISCONNECT,         ///< Disconnect from current WiFi network
    HARVEST_WAKE_NODE,          ///< Send LoRa wake ping before WiFi connect
    HARVEST_CONNECT,            ///< Connect to next leaf's WiFi AP (direct) or relay's AP
    HARVEST_COAP_INIT,          ///< Initialize CoAP client on new network
    HARVEST_DOWNLOAD,           ///< Download images from current node
    HARVEST_NEXT,               ///< Move to next node in registry
    HARVEST_RELAY_CMD,          ///< Send HARVEST_CMD to relay via LoRa
    HARVEST_RELAY_WAIT,         ///< Wait for HARVEST_ACK from relay
    HARVEST_DONE,               ///< Cycle complete, return to IDLE
};

// ─── Configuration ───────────────────────────────────────────

static constexpr uint32_t HARVEST_WIFI_TIMEOUT_MS    = 15000;   // WiFi connect timeout
static constexpr uint32_t HARVEST_RELAY_TIMEOUT_MS   = 120000;  // Wait for relay ACK (2 min)
static constexpr uint32_t HARVEST_ROUTE_DISC_WAIT_MS = 6000;    // Wait for AODV route replies
static constexpr const char* HARVEST_WIFI_PASSWORD    = "forestcam123";
static constexpr const char* HARVEST_SAVE_DIR         = "/received";

// ─── Harvest Cycle Statistics ────────────────────────────────

/**
 * @brief Summary statistics for one complete harvest cycle.
 */
struct HarvestCycleStats {
    uint8_t  nodesAttempted;    ///< Nodes we tried to connect to
    uint8_t  nodesSucceeded;    ///< Nodes fully harvested
    uint8_t  nodesFailed;       ///< Nodes that failed (WiFi or CoAP)
    uint32_t totalImages;       ///< Total images downloaded
    uint32_t totalBytes;        ///< Total bytes downloaded
    uint32_t totalTimeMs;       ///< Total elapsed time for the cycle

    void reset() {
        nodesAttempted = nodesSucceeded = nodesFailed = 0;
        totalImages = totalBytes = totalTimeMs = 0;
    }
};

// ─── HarvestLoop Class ──────────────────────────────────────

class HarvestLoop {
public:
    /**
     * @param registry   Reference to the gateway's node registry.
     * @param coapClient Reference to the shared CoAP client instance.
     */
    HarvestLoop(NodeRegistry& registry, CoapClient& coapClient);

    /**
     * Set references to AODV router and LoRa radio for multi-hop support.
     * Must be called before startCycle() if multi-hop is desired.
     */
    void setAodv(AodvRouter* router, LoRaRadio* radio);

    /**
     * Non-blocking state machine tick. Call from loop().
     * Processes one step of the harvest cycle per call.
     */
    void tick();

    /**
     * Trigger a harvest cycle.
     * Call when enough beacons have been collected.
     */
    void startCycle();

    /**
     * Called by main loop when a HARVEST_ACK is received from a relay.
     */
    void onHarvestAck(const HarvestAckPacket& ack);

    /** Get current state. */
    HarvestState state() const { return _state; }

    /** Get human-readable state name for OLED/Serial. */
    const char* stateStr() const;

    /**
     * Abort any in-progress harvest cycle.
     * Marks remaining nodes as failed and transitions to IDLE.
     * Used when a promoted relay is reclaimed by the original gateway.
     */
    void abortCycle();

    /** Get the SSID of the node currently being harvested. */
    const char* currentNodeSSID() const { return _currentNode.ssid; }

    /** Get stats from the last completed cycle. */
    const HarvestCycleStats& lastCycleStats() const { return _stats; }

    /** Set callback to check if a node should be skipped (blocked). */
    typedef bool (*NodeBlockedCb)(const uint8_t nodeId[6]);
    void setNodeBlockedCallback(NodeBlockedCb cb) { _nodeBlockedCb = cb; }

private:
    NodeRegistry&  _registry;
    CoapClient&    _coapClient;
    AodvRouter*    _aodvRouter;         ///< Optional AODV router (null if not set)
    LoRaRadio*     _loraRadio;          ///< Optional LoRa radio (null if not set)
    HarvestState   _state;
    HarvestCycleStats _stats;

    NodeEntry      _currentNode;        ///< Node currently being harvested
    uint32_t       _stateEnteredMs;     ///< Timestamp for timeout tracking
    uint32_t       _cycleStartMs;       ///< When the cycle began
    uint16_t       _globalImageCounter; ///< Running counter for unique filenames

    // ── Multi-hop relay state ───────────────────────────────
    bool           _relayHarvesting;    ///< Currently doing relay-assisted harvest?
    uint8_t        _relayCmdIdCounter;  ///< Monotonic command ID counter
    uint8_t        _pendingCmdId;       ///< Command ID we're waiting for ACK on
    bool           _relayAckReceived;   ///< Have we received the ACK?
    HarvestAckPacket _lastRelayAck;     ///< Last received ACK
    char           _relaySSID[21];      ///< SSID of the relay node to connect to
    NodeBlockedCb  _nodeBlockedCb;      ///< Optional callback to check if node is blocked

    /** Transition to a new state and record entry time. */
    void _enterState(HarvestState newState);

    // ── State Handlers ──────────────────────────────────────
    void _doStart();
    void _doRouteDiscovery();
    void _doDisconnect();
    void _doWakeNode();
    void _doConnect();
    void _doCoapInit();
    void _doDownload();
    void _doNext();
    void _doRelayCmd();
    void _doRelayWait();
    void _doDone();
};

#endif // HARVEST_LOOP_H
