/**
 * @file NodeRegistry.h
 * @brief Gateway-side registry of discovered ForestCam nodes
 *
 * Maintains a table of nodes discovered via LoRa beacons. Each entry
 * tracks the node's WiFi SSID, image count, RSSI, and harvest status.
 * Entries expire if no beacon is heard for REGISTRY_EXPIRY_MS.
 *
 * Used by HarvestLoop to iterate through nodes and download images.
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef NODE_REGISTRY_H
#define NODE_REGISTRY_H

#include <Arduino.h>
#include "LoRaBeacon.h"

// ─── Configuration ───────────────────────────────────────────
static constexpr uint8_t  REGISTRY_MAX_NODES = 8;
static constexpr uint32_t REGISTRY_EXPIRY_MS = 120000;  // 2 minutes

// ─── Registry Entry ──────────────────────────────────────────

/**
 * @brief A single node entry in the gateway's discovery table.
 */
struct NodeEntry {
    uint8_t  nodeId[6];                     ///< MAC address
    uint8_t  nodeRole;                      ///< NodeRole enum
    char     ssid[BEACON_MAX_SSID + 1];     ///< WiFi SSID (null-terminated)
    uint8_t  imageCount;                    ///< Images available
    float    rssi;                          ///< Best recent RSSI (dBm)
    uint32_t lastSeenMs;                    ///< millis() timestamp of last beacon
    bool     harvested;                     ///< Already downloaded this cycle?
    bool     active;                        ///< Slot in use?

    // ── Announce fields (leaf-initiated harvest) ────────────
    uint8_t  announcedIP[4];                ///< STA IP from announce (0.0.0.0 if not announced)

    // ── AODV routing fields ─────────────────────────────────
    uint8_t  hopCount;                      ///< 1 = direct, 2+ = multi-hop
    uint8_t  nextHopId[6];                  ///< Relay MAC for multi-hop nodes
    bool     routeKnown;                    ///< AODV route has been established?
};

// ─── NodeRegistry Class ──────────────────────────────────────

class NodeRegistry {
public:
    NodeRegistry();

    /**
     * Update or insert a node based on a received beacon.
     * If the node already exists, refreshes its fields and lastSeen.
     * If new and space available, inserts it.
     * If full, replaces the weakest-RSSI entry.
     *
     * @param beacon  Parsed beacon packet.
     * @param rssi    RSSI of the LoRa reception.
     * @return true if node was added or updated.
     */
    bool update(const BeaconPacket& beacon, float rssi);

    /**
     * Update or create a node entry from an announce message.
     * Used for leaf-initiated harvest: leaf sends its MAC, IP, and image count.
     * @return true if a new node was inserted.
     */
    bool updateFromAnnounce(const uint8_t nodeId[6], const uint8_t ip[4], uint8_t imageCount);

    /**
     * Remove entries not heard for REGISTRY_EXPIRY_MS.
     * Call periodically from loop().
     */
    void expireStale();

    /**
     * Reset all harvested flags.
     * Call at the start of each harvest cycle.
     */
    void resetHarvestFlags();

    /**
     * Get the next unharvested node (strongest RSSI first).
     * @param[out] entry  Populated with node details.
     * @return true if an unharvested node was found.
     */
    bool getNextToHarvest(NodeEntry& entry);

    /**
     * Mark a node as harvested by its nodeId.
     * @param nodeId  6-byte MAC address.
     */
    void markHarvested(const uint8_t nodeId[6]);

    /**
     * Number of active (non-expired) nodes.
     */
    uint8_t activeCount() const;

    /**
     * Reset the entire registry — marks all slots inactive and zeroes fields.
     * Used when a promoted relay steps back down to relay role.
     */
    void reset();

    /**
     * Get node entry by index (for OLED display).
     * @param index  0-based slot index (0 .. REGISTRY_MAX_NODES-1).
     * @param[out] entry  Populated if slot is active.
     * @return true if slot is active.
     */
    bool getNode(uint8_t index, NodeEntry& entry) const;

    /**
     * Update routing information for a node from AODV route discovery.
     * Sets hopCount, nextHopId, and routeKnown on the matching entry.
     *
     * @param nodeId     6-byte MAC of the destination node.
     * @param nextHopId  6-byte MAC of the next hop toward that node.
     * @param hopCount   Number of hops to reach the node.
     * @return true if node was found and updated.
     */
    bool updateFromRoute(const uint8_t nodeId[6], const uint8_t nextHopId[6], uint8_t hopCount);

    /**
     * Check if a node requires multi-hop (relay) harvesting.
     * @param nodeId  6-byte MAC address.
     * @return true if hopCount > 1 and routeKnown.
     */
    bool isMultiHop(const uint8_t nodeId[6]) const;

    /**
     * Find the active leaf node with the strongest RSSI (for relay assignment).
     * Only considers nodes with nodeRole == NODE_ROLE_LEAF.
     * On RSSI tie, higher MAC priority wins.
     *
     * @param[out] entry  Populated with the strongest leaf's details.
     * @return true if at least one active leaf was found.
     */
    bool getStrongestLeaf(NodeEntry& entry) const;

    /**
     * Print registry contents to Serial for debugging.
     */
    void dump() const;

private:
    NodeEntry _nodes[REGISTRY_MAX_NODES];

    /** Find existing entry by nodeId, or return -1. */
    int8_t _findNode(const uint8_t nodeId[6]) const;

    /** Find an empty (inactive) slot, or return -1. */
    int8_t _findEmptySlot() const;

    /** Find the slot with the weakest RSSI (for replacement). */
    int8_t _findWeakestSlot() const;
};

#endif // NODE_REGISTRY_H
