/**
 * @file ElectionManager.h
 * @brief Bully election algorithm for gateway failover
 *
 * Monitors gateway liveness via LoRa beacons. When the gateway is
 * absent for ELECTION_GW_TIMEOUT_MS, relays run a Bully election
 * to promote the highest-priority relay to acting gateway.
 *
 * When the original gateway recovers, it broadcasts GW_RECLAIM and
 * the promoted relay steps back down.
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef ELECTION_MANAGER_H
#define ELECTION_MANAGER_H

#include <Arduino.h>
#include "LoRaBeacon.h"
#include "LoRaRadio.h"
#include "ElectionPacket.h"
#include "NodeRegistry.h"
#include "HarvestLoop.h"
#include "AodvRouter.h"

// ─── Election Constants ──────────────────────────────────────
static constexpr uint32_t ELECTION_GW_TIMEOUT_MS          = 90000;  // 3 missed beacons
static constexpr uint32_t ELECTION_BACKOFF_MIN_MS         = 200;
static constexpr uint32_t ELECTION_BACKOFF_MAX_MS         = 800;
static constexpr uint32_t ELECTION_COORDINATOR_TIMEOUT_MS = 5000;
static constexpr uint32_t ELECTION_OVERALL_TIMEOUT_MS     = 10000;
static constexpr uint32_t ELECTION_RECLAIM_COOLDOWN_MS    = 30000;
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

    void begin(const uint8_t mac[6], NodeRole originalRole);
    void tick();
    void onBeacon(const BeaconPacket& beacon);
    void onElectionPacket(const uint8_t* buf, uint8_t len);

    NodeRole activeRole() const { return _activeRole; }
    bool isPromoted() const { return _state == ELECT_ACTING_GATEWAY; }
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
    NodeRole _activeRole;

    ElectionState _state;
    uint32_t _stateEnteredMs;
    uint16_t _electionId;
    uint16_t _currentElectionId;

    uint32_t _lastGatewayBeaconMs;
    bool     _gatewayEverSeen;

    uint8_t  _txRemaining;
    uint32_t _lastTxMs;
    uint32_t _txGapMs;
    uint8_t  _txBuf[ELECTION_PACKET_SIZE];
    uint8_t  _txLen;

    uint32_t _backoffMs;
    uint32_t _cooldownUntilMs;

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
    void _demoteToRelay();
};

#endif // ELECTION_MANAGER_H
