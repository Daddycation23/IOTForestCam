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
|                                        |   Election runs after 15s
v                                        v
Selected role persisted to NVS          LEAF → (election) → GATEWAY winner
```

### Election Flow

```
All nodes: LEAF (boot)
  |
  |── 15s grace period (listen for beacons) ──|
  |                                           |
  |  Gateway beacon heard?                    |
  |    YES → stay LEAF, no election           |
  |    NO  → Bully election starts            |
  |                                           |
  |── ELECTION packets exchanged ─────────────|
  |── Highest MAC priority wins ──────────────|
  |── Winner → COORDINATOR → GATEWAY ────────|
  |── Losers → stay LEAF ─────────────────────|
```

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

### Unit Tests (23 tests)

```bash
pio test -e native -f test_auto_role
```

Test categories:
- **Boot flow** (3): default role, enum values, auto-negotiate returns LEAF
- **Election** (6): leaf participation, priority ordering, grace period, beacon prevention, demotion to LEAF
- **Relay detection** (4): initial state, RREP sets relaying, multi-route tracking, expiry clears relaying
- **Filename format** (5): complete format, boot count, uptime, node ID, image index
- **Serial commands** (5): hex parsing, lowercase, invalid rejection, block/unblock filtering

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

---

## Key Files

| File | Purpose |
|------|---------|
| `include/RoleConfig.h` | Boot flow: `determineRole()`, `checkBootHeld()` |
| `src/RoleConfig.cpp` | Auto-negotiate vs manual menu logic |
| `include/ElectionManager.h` | Election constants, `ELECTION_STARTUP_GRACE_MS` |
| `src/ElectionManager.cpp` | Bully election state machine (all nodes) |
| `include/AodvRouter.h` | Relay tracking: `isRelaying()`, `relayingForCount()` |
| `src/AodvRouter.cpp` | RREP forwarding sets relay flag |
| `include/SerialCmd.h` | Block/unblock serial command parser |
| `src/SerialCmd.cpp` | Command processing, blocked node list |
| `include/HarvestLoop.h` | Node-blocked callback |
| `src/HarvestLoop.cpp` | Boot count + uptime filename format |
| `src/main.cpp` | Integration: `determineRole()`, beacon role, SerialCmd tick |
