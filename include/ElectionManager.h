/**
 * @file ElectionManager.h
 * @brief Bully election algorithm for gateway selection and failover
 *
 * All non-gateway nodes (LEAF and RELAY) participate in election.
 * On boot, a startup grace period (ELECTION_STARTUP_GRACE_MS + 0-5s
 * MAC-based jitter) allows beacon discovery before triggering election.
 * If no gateway is heard after the grace period, the highest-priority
 * node promotes to gateway and beacons as NODE_ROLE_GATEWAY.
 *
 * When a manually-assigned gateway recovers, it broadcasts GW_RECLAIM
 * and the promoted node demotes back to LEAF.
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef ELECTION_MANAGER_H
#define ELECTION_MANAGER_H

#include <Arduino.h>
#include <atomic>
#include "LoRaBeacon.h"
#include "LoRaRadio.h"
#include "ElectionPacket.h"
#include "NodeRegistry.h"
#include "HarvestLoop.h"
#include "AodvRouter.h"

// ─── Election Constants ──────────────────────────────────────
static constexpr uint32_t ELECTION_GW_TIMEOUT_MS          = 90000;  // 3 missed beacons
static constexpr uint32_t ELECTION_STARTUP_GRACE_MS       = 15000;  // Wait before first election
static constexpr uint32_t ELECTION_BACKOFF_MIN_MS         = 500;
static constexpr uint32_t ELECTION_BACKOFF_MAX_MS         = 3000;
static constexpr uint32_t ELECTION_COORDINATOR_TIMEOUT_MS = 5000;
static constexpr uint32_t ELECTION_OVERALL_TIMEOUT_MS     = 10000;
static constexpr uint32_t ELECTION_RECLAIM_COOLDOWN_MS    = 120000;  // Match sleep timeout — prevents re-promote before sleep
static constexpr uint8_t  ELECTION_TX_REPEAT              = 2;
static constexpr uint32_t ELECTION_TX_GAP_MS              = 500;
static constexpr uint8_t  GW_RECLAIM_TX_REPEAT            = 3;
static constexpr uint32_t GW_RECLAIM_TX_GAP_MS            = 666;

// ─── Election States ─────────────────────────────────────────
enum ElectionState : uint8_t {
    ELECT_IDLE,             ///< Normal operation, monitoring gateway beacons
    ELECT_ELECTION_START,   ///< Broadcasting ELECTION, entering backoff
    ELECT_WAITING,          ///< Waiting for SUPPRESS or backoff expiry
    ELECT_PROMOTED,         ///< Won — transitioning to acting gateway
    ELECT_STOOD_DOWN,       ///< Lost — waiting for COORDINATOR
    ELECT_ACTING_GATEWAY,   ///< Running as gateway
    ELECT_RECLAIMED,        ///< Original GW returned — tearing down
};

// ─── ElectionManager Class ───────────────────────────────────
class ElectionManager {
public:
    ElectionManager(LoRaRadio& radio, NodeRegistry& registry,
                    HarvestLoop& harvest, AodvRouter& aodv);

    void begin(const uint8_t mac[6], NodeRole originalRole, bool gatewayKnownFromRtc = false);
    void tick();
    void onBeacon(const BeaconPacket& beacon);
    void onElectionPacket(const uint8_t* buf, uint8_t len);

    NodeRole activeRole() const { return static_cast<NodeRole>(_activeRole.load()); }
    bool isPromoted() const { return _state == ELECT_ACTING_GATEWAY; }
    bool isElectionActive() const {
        return _state == ELECT_ELECTION_START || _state == ELECT_WAITING ||
               _state == ELECT_STOOD_DOWN    || _state == ELECT_PROMOTED;
    }
    /** True when gateway beacon has been missing long enough that sleep should be blocked.
     *  Uses 60s (2 missed beacons) — well before the 90s election timeout,
     *  ensuring the node stays awake for election to trigger and complete. */
    bool isGatewayMissing() const {
        if (_originalRole == NODE_ROLE_GATEWAY) return false;
        if (!_gatewayEverSeen) return true;  // Still in grace period, no GW
        return (millis() - _lastGatewayBeaconMs) >= 60000;
    }
    ElectionState state() const { return _state; }
    const char* stateStr() const;

private:
    LoRaRadio&    _radio;
    NodeRegistry& _registry;
    HarvestLoop&  _harvest;
    AodvRouter&   _aodv;

    uint8_t  _mac[6];
    uint32_t _myPriority;
    NodeRole _originalRole;
    std::atomic<uint8_t> _activeRole;

    ElectionState _state;
    uint32_t _stateEnteredMs;
    uint16_t _electionId;
    uint16_t _currentElectionId;

    uint32_t _bootMs;
    uint32_t _graceEndMs;
    uint32_t _lastGatewayBeaconMs;
    bool     _gatewayEverSeen;

    uint8_t  _txRemaining;
    uint32_t _lastTxMs;
    uint32_t _txGapMs;
    uint8_t  _txBuf[ELECTION_PACKET_SIZE];
    uint8_t  _txLen;

    uint32_t _backoffMs;
    uint32_t _cooldownUntilMs;
    bool     _sentSuppressDuringElection;

    void _enterState(ElectionState newState);
    void _tickIdle();
    void _tickElectionStart();
    void _tickWaiting();
    void _tickStoodDown();
    void _tickActingGateway();
    void _tickReclaimed();

    void _sendElectionPacket(uint8_t type, uint8_t repeat, uint32_t gap);
    void _tickTxRetransmit();
    uint32_t _computeBackoff() const;
    void _promoteToGateway();
    void _promoteToRelay();
    void _demoteToLeaf();
};

#endif // ELECTION_MANAGER_H
