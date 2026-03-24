# Auto-Role Negotiation — Setup & Testing

## Overview

Nodes auto-negotiate their roles at boot instead of requiring manual
selection. All nodes start as **LEAF**; a Bully election (with 15-second
startup grace period) promotes the highest-priority node to **GATEWAY**.
Nodes that forward AODV routes are automatically detected as **RELAY**.

### Boot Behavior

```
Power-on
  |
  v
+-- BOOT button held during first 2s? --+
|                                        |
| YES: Legacy manual menu               | NO: Auto-negotiate
|   (5s timeout, BOOT cycles roles)     |   All nodes start as LEAF
|                                        |   Election runs after 15-20s
v                                        v
Selected role persisted to NVS          LEAF → (election) → GATEWAY winner
```

### Election Flow

```
All nodes: LEAF (boot)
  |
  |── 15-20s grace period (with MAC jitter) ──|
  |                                            |
  |  Gateway beacon heard?                     |
  |    YES → stay LEAF, no election            |
  |    NO  → Bully election starts             |
  |                                            |
  |── ELECTION packets exchanged ──────────────|
  |── Highest MAC priority wins ───────────────|
  |── Winner → COORDINATOR → GATEWAY ─────────|
  |── Winner beacons as GATEWAY ───────────────|
  |── Losers hear GATEWAY beacon → stay LEAF ──|
```

Note: The grace period includes 0-5 seconds of MAC-based jitter
(`(MAC[4]*256 + MAC[5]) % 5000`) to stagger elections when multiple
nodes boot simultaneously.

### Relay Detection

Nodes that forward RREP packets automatically gain relay status:
- `aodvRouter.isRelaying()` returns true
- Beacon reports `NODE_ROLE_RELAY`
- When all relayed routes expire, status returns to LEAF

---

## Serial Commands

Type commands in Serial Monitor (115200 baud, NL terminator):

| Command | Description |
|---------|-------------|
| `block AABB` | Block node with MAC suffix AA:BB from harvest |
| `unblock AABB` | Remove block on node AA:BB |
| `list` | Show currently blocked nodes |

Blocked nodes are skipped during harvest cycles.

---

## Harvest Filename Format

```
/received/node_AABB_boot005_003672s_img_000.jpg
                ^^    ^^^    ^^^^^^      ^^^
                |      |       |          |
            MAC suffix |   uptime(s)  image index
                   boot count
```

Filenames include boot count and uptime to prevent collisions across
deep-sleep wake cycles.

---

## Testing

### Unit Tests (91 tests across 8 suites)

```bash
pio test -e native
```

Test suites:
- **test_auto_role** (23): boot flow, election, relay detection, filename format, serial commands
- **test_harvest_trigger** (8): promoted gateway harvest timing, demotion reset, guard flags
- **test_node_registry** (3): RSSI-based eviction, expiry
- **test_coap_block** (15): Block2 encode/decode, pipeline, Fletcher-16
- **test_deep_sleep** (16): sleep timing, wake protocol, RTC state
- **test_election_packet** (10): serialization round-trip (11-byte format, priority computed from senderId)
- **test_election_state** (7): MAC priority, backoff bounds
- **test_native** (9): LoRa RX mode detection

Auto-role specific tests:
- **Boot flow** (3): default role, enum values, auto-negotiate returns LEAF
- **Election** (6): leaf participation, priority ordering, grace period, beacon prevention, demotion to LEAF
- **Relay detection** (4): initial state, RREP sets relaying, multi-route tracking, expiry clears relaying
- **Filename format** (5): complete format, boot count, uptime, node ID, image index
- **Serial commands** (5): hex parsing, lowercase, invalid rejection, block/unblock filtering

### Promoted Gateway Harvest Lifecycle

When a node wins the election and becomes acting gateway, it gains
harvest capability automatically:

```
Election won → PROMOTED
  |
  v
taskHarvestGateway (already running, blocked on queue)
  |
  v
Harvest trigger enabled in taskLoRaLeafRelay:
  1. SD.mkdir("/received") — ensure save directory exists
  2. Wait 60s listen period (collect beacons, build registry)
  3. AODV route discovery (discoverAll)
  4. Signal xHarvestCmdQueue → taskHarvestGateway starts cycle
  5. HarvestLoop handles WiFi STA connect, CoAP download, disconnect
  6. Cycle complete → reset listen timer → repeat
```

Notes:
- `taskHarvestGateway` is created at boot for all roles (blocks on queue, zero overhead)
- CoAP server task continues running but is harmless (no AP during harvest)
- `HarvestLoop` manages WiFi switching internally (disconnect AP → STA connect → download)

### Hardware Test (3+ nodes)

1. **Auto-negotiate boot**: Flash 3 nodes, power on without holding BOOT.
   All should show "Auto-Negotiating..." on OLED and start as LEAF.

2. **Election**: After 15s grace period, the node with the highest MAC
   priority should promote to GATEWAY. Verify via Serial Monitor:
   ```
   [Election] Grace period expired, no gateway — starting election
   [Election] -> ELECTION_START
   [Election] -> PROMOTED
   [Election] PROMOTED TO ACTING GATEWAY
   ```

3. **Manual override**: Hold BOOT during first 2s to enter legacy menu.
   Verify OLED shows "Manual Role Select:" with cycling roles.

4. **Relay detection**: With 3+ nodes, verify the middle node shows
   `NODE_ROLE_RELAY` in beacons after forwarding RREP.

5. **Gateway reclaim**: Boot a node with manual GATEWAY selection.
   Any promoted (acting) gateway should demote back to LEAF:
   ```
   [Election] GW_RECLAIM from ...
   [Election] DEMOTED BACK TO LEAF
   ```

6. **Serial block**: On gateway, type `block AABB` in Serial Monitor.
   Harvest should skip node *:AA:BB with log message:
   ```
   [Harvest] Node ... is BLOCKED — skipping
   ```

7. **Promoted gateway harvest**: After election (~20s), observe the
   promoted gateway starts harvesting (~80s from boot):
   ```
   [Promoted GW] Harvest capability enabled, listening...
   [Harvest] Starting cycle — 2 node(s)
   [Harvest Task] Starting harvest cycle on Core 1
   ```
   Verify images are saved to `/received/` on the gateway's SD card.

---

## Troubleshooting

**All nodes show "Gateway (RTOS+AODV)":**
Ensure you are running the latest firmware. Earlier versions had a bug where
LEAF nodes could not detect gateway beacons, causing all nodes to self-promote.
Reflash all nodes with the latest build (`pio run -e esp32s3_unified -t upload`).

**Election takes longer than expected:**
The grace period includes 0-5s of MAC-based jitter. Nodes with higher MAC[4:5]
values wait longer. Total grace period is 15-20 seconds.

**Two nodes both show Gateway briefly:**
This can happen if two nodes have different grace jitter values. The lower-
priority node may promote first (shorter jitter), but when the higher-priority
node promotes later and sends GATEWAY beacons, the lower-priority node
detects the higher priority and yields. The highest-priority node always wins.

---

## Leaf-Initiated Announce Flow (Gateway-as-AP)

Under the gateway-as-AP architecture, the harvest flow is reversed — leaves
initiate contact with the gateway instead of vice versa.

### Timer Wake → STA Connect → POST /announce

```
Leaf (timer wake)                    Gateway (persistent AP: ForestCam-GW-XXYY)
  |                                    |
  |  Restore rtcGatewaySSID from RTC   |
  |  WiFi.mode(WIFI_STA)              |
  |── WiFi.begin(rtcGatewaySSID) ──>  |
  |                                    |  (leaf joins as STA client)
  |── POST /announce (MAC+imgCnt) ──> |  → xAnnounceQueue
  |                                    |  → taskHarvestGateway downloads
  |<── GET /info, GET /image ───────  |
  |                                    |
  |  120s idle → deep sleep            |
```

### Gateway SSID Caching

`rtcGatewaySSID` is set from multiple sources, all derived from the sender's MAC:

| Source | When | How |
|--------|------|-----|
| **COORDINATOR packet** | After election, winner broadcasts COORDINATOR | SSID derived from sender MAC: `ForestCam-GW-XXYY` |
| **GW_RECLAIM packet** | Original gateway reclaims role | SSID derived from sender MAC |
| **Gateway beacon** | Periodic LoRa beacon from gateway | SSID derived from beacon sender MAC |

The SSID is **not transmitted on wire** (beacon v2 format). It is always
derived from the sender MAC as `ForestCam-GW-XXYY` where `XX` and `YY`
are the last two bytes of the MAC address.

### Election Changes

- **ACTING_GATEWAY sends COORDINATOR on suppress:** When an acting gateway
  receives a SUPPRESS from a higher-priority node, it broadcasts a
  COORDINATOR packet before yielding, so leaves can cache the new gateway SSID.
- **SUPPRESS yields ACTING_GATEWAY:** A suppressed node transitions to LEAF
  and recognizes the suppressor as the new gateway.
- **Cooldown after demotion:** After being demoted from ACTING_GATEWAY back
  to LEAF, the node enters a cooldown period before it can participate in
  another election, preventing rapid promote/demote oscillation.
- **MAC tiebreaker:** Election uses Bully algorithm with MAC-based priority
  as tiebreaker — highest MAC always wins.

---

## Key Files

| File | Purpose |
|------|---------|
| `include/RoleConfig.h` | Boot flow: `determineRole()`, `checkBootHeld()` |
| `src/RoleConfig.cpp` | Auto-negotiate vs manual menu logic |
| `include/ElectionManager.h` | Election constants, `isGatewayMissing()` sleep guard |
| `include/ElectionPacket.h` | 11-byte election packet format (priority computed from senderId) |
| `src/ElectionManager.cpp` | Bully election state machine (all nodes) |
| `src/ElectionPacket.cpp` | Serialize/parse for 11-byte election packets |
| `include/AodvRouter.h` | Relay tracking: `isRelaying()`, `relayingForCount()` |
| `src/AodvRouter.cpp` | RREP forwarding sets relay flag |
| `include/SerialCmd.h` | Block/unblock serial command parser |
| `src/SerialCmd.cpp` | Command processing, blocked node list |
| `include/HarvestLoop.h` | Node-blocked callback |
| `src/HarvestLoop.cpp` | Boot count + uptime filename format |
| `src/main.cpp` | Integration: `determineRole()`, beacon role, SerialCmd tick |
