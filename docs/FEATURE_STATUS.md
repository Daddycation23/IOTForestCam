# Feature Implementation Status

## Project: IOT Forest Cam (CS Group 2)
## Hardware: LILYGO T3-S3 V1.2 (ESP32-S3 + SX1280 LoRa)

---

## Completed Features

### 1. Virtual Sensor Pipeline (Part 1)
- **Branch:** `coap-implemented`
- **Description:** SD card with pre-loaded JPEGs replaces real cameras. Reads images in 1024-byte chunks with Fletcher-16 checksum.
- **Key Files:** `src/StorageReader.cpp`, `include/StorageReader.h`
- **Test Guide:** [docs/PART1_SETUP.md](PART1_SETUP.md)

### 2. CoAP Block-Wise Transfer (Part 3)
- **Branch:** `coap-implemented`
- **Description:** CoAP server (RFC 7252) with Block2 transfer (RFC 7959) using 1024-byte blocks (SZX=6). Gateway downloads images from leaf nodes.
- **Key Files:** `src/CoapServer.cpp`, `src/CoapClient.cpp`, `include/CoapMessage.h`

### 3. AODV Mesh Routing
- **Branch:** `coap-implemented`
- **Description:** Reactive routing with RREQ/RREP/RERR packets. 12-entry route table, 16-entry dedup cache, 120s route expiry.
- **Key Files:** `src/AodvRouter.cpp`, `src/AodvPacket.cpp`, `include/AodvRouter.h`

### 4. Dynamic Role Assignment
- **Branch:** `coap-implemented`
- **Description:** BOOT button (GPIO 0) selects Leaf/Relay/Gateway role at boot. Persisted to NVS flash across power cycles.
- **Key Files:** `src/RoleConfig.cpp`, `include/RoleConfig.h`

### 5. LoRa Beacon Discovery
- **Branch:** `coap-implemented`
- **Description:** 31-byte beacon packets broadcast every 30s with jitter. Contains MAC, role, SSID, image count, battery level.
- **Key Files:** `src/LoRaBeacon.cpp`, `include/LoRaBeacon.h`

### 6. Multi-Hop Harvest via Relay
- **Branch:** `coap-implemented`
- **Description:** Gateway commands relay over LoRa to fetch images from out-of-range leaf. Store-and-forward with HARVEST_CMD/HARVEST_ACK protocol.
- **Key Files:** `src/HarvestLoop.cpp`, `include/AodvPacket.h`

### 7. Data Integrity (Fletcher-16)
- **Branch:** `coap-implemented`
- **Description:** Checksum computed during transfer, verified via CoAP `/checksum` endpoint.
- **Key Files:** `src/StorageReader.cpp`, `src/CoapClient.cpp`, `src/CoapServer.cpp`

### 8. OLED Status Display
- **Branch:** `coap-implemented`
- **Description:** Real-time node status on SSD1306 128x64 OLED (role, IP, image count, network stats).
- **Key Files:** `src/main.cpp`

### 9. Bully Election Algorithm
- **Branch:** `coap-implemented`
- **Description:** Gateway failover via Bully election over LoRa. Relay nodes can promote to acting gateway when original gateway is absent.
- **Key Files:** `src/ElectionManager.cpp`, `include/ElectionManager.h`, `include/ElectionPacket.h`

### 10. FreeRTOS Dual-Core Task Separation
- **Branch:** `feature/freertos`
- **Commit:** `e14776c`
- **Description:** Refactored cooperative loop() into FreeRTOS tasks pinned to dual cores. Core 0: LoRa/AODV/election. Core 1: WiFi/CoAP harvest. Fixes 50-120s LoRa blackout during harvest.
- **Key Files:** `include/TaskConfig.h`, `src/TaskConfig.cpp`, `src/main.cpp`
- **Test Guide:** [docs/FREERTOS_SETUP.md](FREERTOS_SETUP.md)

---

### 11. CoAP Pipelined Block Transfer
- **Branch:** `feature/coap-optimization`
- **Description:** Block size doubled (512→1024B), pipelined requests with window of 3, timeout reduced (5s→2s). Target 2-3x throughput improvement.
- **Key Files:** `include/CoapMessage.h`, `include/CoapClient.h`, `src/CoapClient.cpp`, `include/StorageReader.h`
- **Tests:** `test/test_coap_block/` (15 unit tests)
- **Test Guide:** [docs/COAP_OPTIMIZATION_SETUP.md](COAP_OPTIMIZATION_SETUP.md)

### 11b. Critical Bugfixes (Code Review)
- **Branch:** `feature/coap-optimization`
- **Description:** Fixed 4 bugs found via code review against `ref/code-checker.md`:
  1. **CRITICAL — Relay harvest queue item size mismatch:** `xRelayHarvestQueue` created with `sizeof(uint8_t)` (1 byte) but sent/received `HarvestCmdPacket` (~38 bytes), causing memory corruption. Fixed to `sizeof(HarvestCmdPacket)`.
  2. **HIGH — Buffer overflow in `CoapClient::verifyChecksum()`:** Writing `'\0'` at `buf[128]` when buf was 128 bytes. Fixed by allocating 129 bytes and limiting read to 128.
  3. **HIGH — Blocking `delay()` in FreeRTOS task:** `HarvestLoop::_doConnect()` used `delay(250)` instead of `vTaskDelay()`, starving Core 1 scheduler during WiFi connect. Fixed.
  4. **HIGH — Thread-unsafe shared variables:** `_relayBusy`, `_relayCachedServing`, `_relayCachedStartMs` accessed from both cores without synchronization. Changed to `std::atomic`.
- **Key Files:** `src/TaskConfig.cpp`, `src/CoapClient.cpp`, `src/HarvestLoop.cpp`, `src/main.cpp`

---

### 12. Deep Sleep + Two-Step Wake Protocol
- **Branch:** `feature/deep-sleep`
- **Description:** Leaf/relay nodes enter deep sleep between harvests (<10µA). Gateway sends WAKE_PING over LoRa to trigger DIO1 ext1 wakeup. Fast-path boot from RTC memory skips role menu. New HARVEST_WAKE_NODE state in harvest loop.
- **Key Files:** `include/DeepSleepManager.h`, `src/DeepSleepManager.cpp`, `include/AodvPacket.h`, `src/HarvestLoop.cpp`, `src/main.cpp`
- **Tests:** `test/test_deep_sleep/` (16 unit tests)
- **Test Guide:** [docs/DEEP_SLEEP_SETUP.md](DEEP_SLEEP_SETUP.md)

---

### 13. Auto-Role Negotiation
- **Branch:** `feature/auto-role`
- **Status:** Complete (steps 1-7)
- **Description:** Nodes auto-negotiate roles at boot instead of manual selection. All nodes start as LEAF; a Bully election (with 15s startup grace period) promotes the highest-priority node to gateway. BOOT button override for manual mode.
- **Key Changes (Step 1 — Boot Flow):** `RoleConfig::determineRole()` with auto-negotiate default, `checkBootHeld()` for manual override
- **Key Changes (Step 2 — Election Guard Removal):** ElectionManager now allows LEAF nodes to participate in election, startup grace period before first election, demotion returns to LEAF not RELAY, main.cpp uses `determineRole()`
- **Key Changes (Step 3 — Relay Detection):** AodvRouter tracks relay state via `isRelaying()`/`relayingForCount()`. RREP forwarding sets relay flag; route expiry/RERR/link break clears it. RouteEntry gains `relayed` field.
- **Key Changes (Step 4 — Filename Format):** Harvest filenames include boot count and uptime: `node_AABB_boot005_003672s_img_000.jpg`
- **Key Changes (Step 5 — Serial Commands):** New SerialCmd module for `block`/`unblock`/`list` commands. HarvestLoop skips blocked nodes via callback.
- **Key Changes (Step 6 — Integration):** Beacon nodeRole reflects relay detection, OLED shows correct status.
- **Bugfix — Multi-Promote Race:** LEAF nodes now process beacons for gateway detection, promoted nodes beacon as GATEWAY, MAC-based jitter (0-5s) on grace period prevents simultaneous election, COORDINATOR/GW_RECLAIM set gatewayEverSeen, priority-based stagger on re-election, priority-aware gateway beacon handling (higher-priority node always wins).
- **Key Changes (Promoted GW Harvest):** `taskHarvestGateway` created at boot for all roles (blocks on queue, zero overhead). Promoted gateway triggers harvest via same logic as boot-gateway: 60s listen → route discovery → harvest cycle. `HarvestLoop` handles WiFi switching internally.
- **Bugfix — Code Review Audit (15 issues):**
  1. **CRITICAL — Buffer overflow:** `infoBuf[512]` off-by-one write in `HarvestLoop.cpp` and `main.cpp` (relay harvest). Fixed: `infoBuf[513]`, `infoLen = sizeof(infoBuf) - 1`.
  2. **CRITICAL — Registry mutex missing:** 9/10 `markHarvested()` calls in `HarvestLoop` lacked `registryLock()`. Fixed: all wrapped.
  3. **CRITICAL — JSON buffer overflow:** `_handleInfoGet` in `CoapServer.cpp` could exceed 512-byte static buffer. Fixed: bounds check after each `snprintf`.
  4. **HIGH — Promoted harvest statics not reset on demotion:** Re-promotion triggered immediate harvest. Fixed: edge-detection pattern with `wasPromoted`/`nowPromoted` + guard flag.
  5. **HIGH — Task handle overwrite:** Relay nodes overwrote `hTaskHarvest`. Fixed: separate `hTaskRelayHarvest` handle.
  6. **HIGH — Cross-core data race:** `_relayAckReceived` plain `bool` accessed from both cores. Fixed: `std::atomic<bool>` + `portMUX_TYPE` spinlock for struct copy.
  7. **HIGH — Static `rreqSent` persists across aborts:** Second harvest cycle skipped RREQ. Fixed: promoted to member `_routeDiscRreqSent`, reset in `startCycle()`.
  8. **HIGH — Missing `setAodv()` in `initLeafRelay()`:** Promoted nodes had null AODV/callback. Fixed: added calls.
  9. **MEDIUM — `SPI.end()` on wrong bus:** SD init failure killed LoRa bus. Fixed: `sdSPI.end()`.
  10. **MEDIUM — Directory handle leak:** `_scanDirectory` early return without `dir.close()`. Fixed.
  11. **MEDIUM — `_relayCmdId` data race:** Plain `uint8_t` cross-core. Fixed: `std::atomic<uint8_t>`.
  12. **MEDIUM — Relay harvest loop:** `_relayHarvesting` not reset in `_doCoapInit` failure. Fixed.
  13. **MEDIUM — Test constant mismatch:** Election backoff bounds (200/800) didn't match production (500/3000). Fixed.
- **Bugfix — Core 1 Stack Overflow (CoAP_Srv):** Leaf nodes crash-looped with `Stack canary watchpoint triggered (CoAP_Srv)`. Root cause: `taskHarvestGateway` (32KB stack) created at boot consumed heap, reducing WiFi internal headroom and tipping CoAP_Srv over its 16KB limit. Fix: (1) `STACK_COAP_SERVER` 4096→6144 words (24KB), (2) `taskHarvestGateway` created lazily on first promotion instead of at boot, saving 32KB heap for unpromoted nodes.
- **Bugfix — OLED Demotion Display:** After gateway→leaf demotion, old "Gateway (RTOS+AODV)" header persisted on OLED because leaf path only did partial `fillRect()` updates. Fix: track display mode via `lastDisplayWasGateway`, do full `clearDisplay()` + leaf header redraw on transition.
- **Bugfix — Gateway Self-Copy Missing:** Gateway harvested remote nodes but never copied its own `/images/` to `/received/`. Added `_selfCopyImages()` to `HarvestLoop::_doDone()` — enumerates `/images/*.jpg`, copies each to `/received/node_SELF_boot%03lu_%06lus_img_%03u.jpg`, updates harvest stats.
- **Bugfix — LoRa RX Recovery Noise:** Diagnostic block logged verbose "Radio not in RX" + "RX recovery OK" every 15s. Fix: reduced interval 15s→5s for faster recovery, suppressed all successful recovery logs (only failures are logged).
- **Bugfix — startReceive() Log Flooding:** `log_i()` in `LoRaRadio::startReceive()` fired on every call (21+ call sites), drowning useful output. Fix: demoted to `log_d()` (DEBUG level, silent in release builds).
- **Bugfix — OLED Stays Lit During Deep Sleep:** Display remained on during deep sleep, wasting power. Fix: send `SSD1306_DISPLAYOFF` command before entering sleep. Wake path already calls `display.begin()` which re-enables display.
- **Bugfix — Deep Sleep During Active Harvest:** Leaf nodes entered deep sleep mid-CoAP transfer because `shouldSleep()` only guarded on relay-cached serving, not direct CoAP requests. Fix: poll `coapServer.blocksSent()` in sleep check loop; if blocks increased, call `deepSleepMgr.onActivity()` to reset the sleep timer.
- **Bugfix — WAKE_PING Wait Too Short:** Wake wait (2500ms) + WiFi timeout (15s) = 17.5s total, insufficient for deep-sleep full reboot (9-30s). Fix: wake wait increased to 12000ms, `HARVEST_WIFI_TIMEOUT_MS` increased to 25000ms, giving 37s total window.
- **Key Files:** `include/RoleConfig.h`, `src/RoleConfig.cpp`, `include/ElectionManager.h`, `src/ElectionManager.cpp`, `include/AodvRouter.h`, `src/AodvRouter.cpp`, `include/SerialCmd.h`, `src/SerialCmd.cpp`, `include/HarvestLoop.h`, `src/HarvestLoop.cpp`, `src/main.cpp`, `src/CoapServer.cpp`, `src/StorageReader.cpp`, `include/TaskConfig.h`, `src/TaskConfig.cpp`
- **Tests:** `test/test_auto_role/` (23), `test/test_harvest_trigger/` (8), `test/test_node_registry/` (3) — 91 total across 8 suites
- **Test Guide:** [docs/AUTO_ROLE_SETUP.md](AUTO_ROLE_SETUP.md)
- **Report:** `ref/IOT_Design_Review_Report.tex` (LaTeX, compiles to PDF)
- **Bugfix — WAKE_PING Log Shows Wrong SSID:** `_currentNode.ssid` is empty in WAKE_NODE state (populated later in CONNECT). Fix: changed log to say "broadcast sent" without misleading SSID.
- **Bugfix — CoAP Pipelined Download Timeout Resilience:** Transient UDP loss caused permanent image failure. Fix: (1) detect server error responses and abort early, (2) per-image retry — on overall timeout, reset pipeline state and retry from block 0 once before giving up, (3) server-side `readBlock` failure now logs error and resets `_openImageIndex` so subsequent requests re-open the file.
- **Optimization — Election Packet Size 15→11 bytes:** Removed redundant 4-byte `priority` field from wire format — it's always computable from `senderId` via `macToPriority()`. All senders, receivers, and tests updated. Saves 4 bytes per election/suppress/coordinator/reclaim packet.
- **Bugfix — Deep Sleep During Pending Election:** Leaf node entered deep sleep before election could trigger when gateway disappeared. Root cause: `SLEEP_ACTIVE_TIMEOUT_MS` (120s) could expire before `ELECTION_GW_TIMEOUT_MS` (90s) + stagger + backoff completed, because beacon RX didn't reset the sleep timer. Fix: added `isElectionActive()` and `isGatewayMissing()` guards to the deep sleep check — node stays awake whenever the gateway is missing or an election is in progress. `isGatewayMissing()` uses 60s threshold (2 missed beacons), well before the 90s election timeout, ensuring the sleep guard activates before `shouldSleep()` fires.
- **Config — Harvest Interval 60s→180s:** Changed `HARVEST_LISTEN_PERIOD_MS` from 60s to 180s (3 min) for demo. With 60s cycles, WAKE_PING fired every ~90-150s which reset the leaf sleep timer (120s) before it could expire — leaves never actually slept. With 180s, leaves sleep at 120s, get woken by WAKE_PING at 180s, harvest, then sleep again.

---

## In Progress

### 14. Python Dashboard
- **Planned Branch:** `feature/dashboard`
- **Description:** Laptop Python app using `aiocoap` + `tkinter` to pull and display images from gateway via CoAP.

### 15. AES-128 LoRa Encryption
- **Status:** Not planned for current sprint
- **Description:** Encrypt LoRa control-plane packets. Currently all packets are cleartext.

### 16. Queue-Based / TDMA Scheduling
- **Status:** Not planned for current sprint
- **Description:** Replace sequential harvest with time-slotted or queue-based scheduling.

### 17. ESP-Mesh-Lite Wi-Fi Mesh
- **Status:** Not planned — using simple AP/STA with AODV routing instead

---

## Branch Chain

```
coap-implemented
  └── feature/freertos (e14776c) ................ [Done]
        └── feature/coap-optimization (5458b43) . [Done]
              └── feature/deep-sleep ............ [Done]
                    └── feature/auto-role ....... [Done]
                          └── feature/dashboard . [Planned]
```
