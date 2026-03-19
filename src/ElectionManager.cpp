/**
 * @file ElectionManager.cpp
 * @brief Bully election state machine implementation
 */

#include "ElectionManager.h"
#include "TaskConfig.h"

static const char* TAG = "Election";

ElectionManager::ElectionManager(LoRaRadio& radio, NodeRegistry& registry,
                                 HarvestLoop& harvest, AodvRouter& aodv)
    : _radio(radio), _registry(registry), _harvest(harvest), _aodv(aodv),
      _myPriority(0), _originalRole(NODE_ROLE_LEAF), _activeRole(NODE_ROLE_LEAF),
      _state(ELECT_IDLE), _stateEnteredMs(0), _electionId(0),
      _currentElectionId(0), _bootMs(0), _graceEndMs(0), _lastGatewayBeaconMs(0), _gatewayEverSeen(false),
      _txRemaining(0), _lastTxMs(0), _txGapMs(0), _txLen(0),
      _backoffMs(0), _cooldownUntilMs(0)
{
    memset(_mac, 0, 6);
    memset(_txBuf, 0, sizeof(_txBuf));
}

void ElectionManager::begin(const uint8_t mac[6], NodeRole originalRole) {
    memcpy(_mac, mac, 6);
    _originalRole = originalRole;
    _activeRole   = originalRole;
    _myPriority   = ElectionPacket::macToPriority(mac);
    _electionId   = (uint16_t)(millis() & 0xFFFF);
    _bootMs       = millis();
    uint32_t jitter = (mac[4] * 256 + mac[5]) % 5000;  // 0–5s based on MAC
    _graceEndMs   = _bootMs + ELECTION_STARTUP_GRACE_MS + jitter;
    Serial.printf("[%s] Grace period jitter: %lu ms (ends at %lu ms)\n", TAG, jitter, _graceEndMs);
    _lastGatewayBeaconMs = millis();
    _state        = ELECT_IDLE;
    _stateEnteredMs = millis();

    Serial.printf("[%s] begin: role=%s priority=0x%08lX electionId=%u\n",
                  TAG, BeaconPacket::roleToString(originalRole),
                  _myPriority, _electionId);
}

void ElectionManager::tick() {
    // Gateway nodes never participate in election (they ARE the gateway)
    if (_originalRole == NODE_ROLE_GATEWAY) return;

    _tickTxRetransmit();

    switch (_state) {
        case ELECT_IDLE:            _tickIdle();           break;
        case ELECT_ELECTION_START:  _tickElectionStart();  break;
        case ELECT_WAITING:         _tickWaiting();        break;
        case ELECT_STOOD_DOWN:      _tickStoodDown();      break;
        case ELECT_ACTING_GATEWAY:  _tickActingGateway();  break;
        case ELECT_RECLAIMED:       _tickReclaimed();      break;
        case ELECT_PROMOTED:
            _promoteToGateway();
            _enterState(ELECT_ACTING_GATEWAY);
            break;
    }
}

void ElectionManager::onBeacon(const BeaconPacket& beacon) {
    if (beacon.nodeRole == NODE_ROLE_GATEWAY) {
        _lastGatewayBeaconMs = millis();
        _gatewayEverSeen = true;

        if (_state == ELECT_ACTING_GATEWAY) {
            uint32_t otherPriority = ElectionPacket::macToPriority(beacon.nodeId);
            if (otherPriority >= _myPriority) {
                Serial.printf("[%s] Higher-priority gateway beacon — yielding\n", TAG);
                _enterState(ELECT_RECLAIMED);
            } else {
                Serial.printf("[%s] Lower-priority gateway beacon — staying (we outrank)\n", TAG);
            }
        }
    }
}

void ElectionManager::onElectionPacket(const uint8_t* buf, uint8_t len) {
    ElectionPacket pkt;
    if (!pkt.parse(buf, len)) return;

    // Gateway nodes ignore election packets (they broadcast GW_RECLAIM instead)
    if (_originalRole == NODE_ROLE_GATEWAY) return;

    if (memcmp(pkt.senderId, _mac, 6) == 0) return;

    char idStr[24];
    pkt.senderIdToString(idStr, sizeof(idStr));
    uint32_t senderPriority = ElectionPacket::macToPriority(pkt.senderId);

    switch (pkt.type) {
        case PKT_TYPE_ELECTION: {
            Serial.printf("[%s] ELECTION from %s (pri=0x%08lX)\n",
                          TAG, idStr, senderPriority);

            if (senderPriority < _myPriority) {
                Serial.printf("[%s] Suppressing lower-priority candidate\n", TAG);
                _sendElectionPacket(PKT_TYPE_SUPPRESS, ELECTION_TX_REPEAT, ELECTION_TX_GAP_MS);

                if (_state == ELECT_IDLE || _state == ELECT_STOOD_DOWN) {
                    _currentElectionId = pkt.electionId;
                    _enterState(ELECT_ELECTION_START);
                }
            } else if (senderPriority > _myPriority) {
                if (_state == ELECT_WAITING || _state == ELECT_ELECTION_START) {
                    _enterState(ELECT_STOOD_DOWN);
                }
            }
            break;
        }

        case PKT_TYPE_SUPPRESS: {
            Serial.printf("[%s] SUPPRESS from %s (pri=0x%08lX)\n",
                          TAG, idStr, senderPriority);

            if (senderPriority > _myPriority) {
                if (_state == ELECT_WAITING || _state == ELECT_ELECTION_START) {
                    _enterState(ELECT_STOOD_DOWN);
                }
            }
            break;
        }

        case PKT_TYPE_COORDINATOR: {
            Serial.printf("[%s] COORDINATOR from %s (pri=0x%08lX)\n",
                          TAG, idStr, senderPriority);

            if (_state == ELECT_ACTING_GATEWAY && senderPriority > _myPriority) {
                Serial.printf("[%s] Yielding to higher-priority coordinator\n", TAG);
                _enterState(ELECT_RECLAIMED);
            } else if (_state == ELECT_STOOD_DOWN || _state == ELECT_WAITING ||
                       _state == ELECT_ELECTION_START) {
                Serial.printf("[%s] Coordinator announced — returning to IDLE\n", TAG);
                _lastGatewayBeaconMs = millis();
                _gatewayEverSeen = true;  // Coordinator IS the gateway now
                _enterState(ELECT_IDLE);
            }
            break;
        }

        case PKT_TYPE_GW_RECLAIM: {
            Serial.printf("[%s] GW_RECLAIM from %s\n", TAG, idStr);

            if (_state == ELECT_ACTING_GATEWAY) {
                _enterState(ELECT_RECLAIMED);
            } else {
                _lastGatewayBeaconMs = millis();
                _gatewayEverSeen = true;  // Original gateway is back
                if (_state != ELECT_IDLE) {
                    _enterState(ELECT_IDLE);
                }
            }
            break;
        }
    }
}

// ─── State handlers ──────────────────────────────────────────

void ElectionManager::_tickIdle() {
    if (millis() < _cooldownUntilMs) return;

    // Startup grace period (with MAC-based jitter): wait before first election
    if (!_gatewayEverSeen && millis() < _graceEndMs) {
        return;
    }

    // After grace period with no gateway seen, OR gateway beacon timed out
    if (!_gatewayEverSeen) {
        Serial.printf("[%s] Grace period expired, no gateway — starting election\n", TAG);
        _electionId++;
        _currentElectionId = _electionId;
        _enterState(ELECT_ELECTION_START);
        return;
    }

    if (millis() - _lastGatewayBeaconMs >= ELECTION_GW_TIMEOUT_MS) {
        // Stagger re-election by inverse priority: highest priority starts first
        // This prevents simultaneous election when all nodes detect timeout together
        uint32_t stagger = 3000 - (_myPriority % 3000);  // 0-3s, higher priority = shorter
        if (millis() - _lastGatewayBeaconMs >= ELECTION_GW_TIMEOUT_MS + stagger) {
            Serial.printf("[%s] Gateway timeout (stagger=%lu ms) — starting election\n",
                          TAG, stagger);
            _electionId++;
            _currentElectionId = _electionId;
            _enterState(ELECT_ELECTION_START);
        }
    }
}

void ElectionManager::_tickElectionStart() {
    _sendElectionPacket(PKT_TYPE_ELECTION, ELECTION_TX_REPEAT, ELECTION_TX_GAP_MS);
    _backoffMs = _computeBackoff();
    Serial.printf("[%s] Backoff: %lu ms\n", TAG, _backoffMs);
    _enterState(ELECT_WAITING);
}

void ElectionManager::_tickWaiting() {
    if (millis() - _stateEnteredMs >= ELECTION_OVERALL_TIMEOUT_MS) {
        Serial.printf("[%s] Election overall timeout — back to IDLE\n", TAG);
        _enterState(ELECT_IDLE);
        return;
    }

    if (millis() - _stateEnteredMs >= _backoffMs) {
        Serial.printf("[%s] Backoff expired — broadcasting COORDINATOR\n", TAG);
        _sendElectionPacket(PKT_TYPE_COORDINATOR, ELECTION_TX_REPEAT, ELECTION_TX_GAP_MS);
        _enterState(ELECT_PROMOTED);
    }
}

void ElectionManager::_tickStoodDown() {
    if (millis() - _stateEnteredMs >= ELECTION_COORDINATOR_TIMEOUT_MS) {
        Serial.printf("[%s] No COORDINATOR heard — restarting election\n", TAG);
        _electionId++;
        _currentElectionId = _electionId;
        _enterState(ELECT_ELECTION_START);
    }
}

void ElectionManager::_tickActingGateway() {
    // Nothing to do — gateway loop runs via activeRole().
}

void ElectionManager::_tickReclaimed() {
    _demoteToLeaf();
    _cooldownUntilMs = millis() + ELECTION_RECLAIM_COOLDOWN_MS;
    _lastGatewayBeaconMs = millis();
    _enterState(ELECT_IDLE);
}

// ─── Helpers ─────────────────────────────────────────────────

void ElectionManager::_enterState(ElectionState newState) {
    _state = newState;
    _stateEnteredMs = millis();
    Serial.printf("[%s] -> %s\n", TAG, stateStr());
}

void ElectionManager::_sendElectionPacket(uint8_t type, uint8_t repeat, uint32_t gap) {
    ElectionPacket pkt;
    pkt.type = type;
    memcpy(pkt.senderId, _mac, 6);
    pkt.electionId = _currentElectionId;

    _txLen = pkt.serialize(_txBuf, sizeof(_txBuf));
    if (_txLen > 0) {
        loraSendSafe(_txBuf, _txLen);
        loraStartReceiveSafe();
        _lastTxMs = millis();
        _txRemaining = repeat - 1;
        _txGapMs = gap;
    }
}

void ElectionManager::_tickTxRetransmit() {
    if (_txRemaining == 0) return;
    if (millis() - _lastTxMs < _txGapMs) return;

    loraSendSafe(_txBuf, _txLen);
    loraStartReceiveSafe();
    _lastTxMs = millis();
    _txRemaining--;
}

uint32_t ElectionManager::_computeBackoff() const {
    float norm = (float)(_myPriority % 1000) / 999.0f;
    uint32_t backoff = ELECTION_BACKOFF_MAX_MS
                     - (uint32_t)(norm * (ELECTION_BACKOFF_MAX_MS - ELECTION_BACKOFF_MIN_MS));
    return backoff;
}

void ElectionManager::_promoteToGateway() {
    Serial.printf("\n[%s] ╔══════════════════════════════════╗\n", TAG);
    Serial.printf("[%s] ║  PROMOTED TO ACTING GATEWAY      ║\n", TAG);
    Serial.printf("[%s] ╚══════════════════════════════════╝\n\n", TAG);

    _activeRole = NODE_ROLE_GATEWAY;
    _registry.reset();
    _harvest.abortCycle();
}

void ElectionManager::_demoteToLeaf() {
    Serial.printf("\n[%s] ╔══════════════════════════════════╗\n", TAG);
    Serial.printf("[%s] ║  DEMOTED BACK TO LEAF            ║\n", TAG);
    Serial.printf("[%s] ╚══════════════════════════════════╝\n\n", TAG);

    _harvest.abortCycle();
    _registry.reset();
    _activeRole = NODE_ROLE_LEAF;
}

const char* ElectionManager::stateStr() const {
    switch (_state) {
        case ELECT_IDLE:            return "IDLE";
        case ELECT_ELECTION_START:  return "ELECTION_START";
        case ELECT_WAITING:         return "WAITING";
        case ELECT_PROMOTED:        return "PROMOTED";
        case ELECT_STOOD_DOWN:      return "STOOD_DOWN";
        case ELECT_ACTING_GATEWAY:  return "ACTING_GATEWAY";
        case ELECT_RECLAIMED:       return "RECLAIMED";
        default:                    return "UNKNOWN";
    }
}
