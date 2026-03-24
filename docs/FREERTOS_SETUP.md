# FreeRTOS Dual-Core Task Separation — Setup & Testing

## Overview

This feature replaces the single-threaded cooperative `loop()` with dedicated
FreeRTOS tasks pinned to the ESP32-S3's dual cores. The critical improvement:
**LoRa reception is no longer blocked during WiFi+CoAP harvest downloads.**

| Core | Task | Source File | What it does |
|------|------|-------------|-------------|
| 0 | `taskLoRaGateway` | `src/TaskLoRaGateway.cpp` | Gateway: LoRa RX/TX, beacon broadcast, AODV routing, election, harvest trigger |
| 0 | `taskLoRaLeafRelay` | `src/TaskLoRaLeafRelay.cpp` | Leaf/Relay: LoRa RX/TX, beacon, election, deep sleep check (120s idle) |
| 1 | `taskHarvestGateway` | `src/TaskHarvestGateway.cpp` | Gateway: polls announce queue, CoAP image download from leaf STA IPs |
| 1 | `taskCoapServer` | `src/TaskCoapServer.cpp` | CoAP Block2 image serving (leaf/relay) + `/announce` endpoint (gateway) |
| 1 | `taskRelayHarvest` | `src/TaskRelayHarvest.cpp` | Relay store-and-forward (relay only) |
| — | `loop()` | `src/main.cpp` | OLED display updates only |

**Note:** The gateway also runs `taskCoapServer` for the `/announce` endpoint — leaves POST their MAC + image count here on wake, which triggers the gateway harvest.

---

## Hardware Requirements

- **3x LILYGO T3-S3 V1.2** boards (ESP32-S3 + SX1280 LoRa)
- **3x MicroSD cards** (FAT32 formatted)
- **3x USB-C cables** (for flashing + serial monitor)
- **1x Laptop/PC** with PlatformIO installed

### SD Card Preparation

| Board Role | SD Card Contents |
|-----------|-----------------|
| **Leaf** | `/images/` folder with 1-5 JPEG files (10-100 KB each) |
| **Gateway** | Empty (creates `/received/` automatically) |
| **Relay** | Empty (creates `/cached/` automatically) |

---

## Flashing

Flash the **same unified firmware** to all 3 boards:

```bash
$env:PATH += ";C:\Users\<USER>\.platformio\penv\Scripts"
pio run -e esp32s3_unified -t upload --upload-port COMx
```

Replace `COMx` with each board's COM port. Check available ports:
```bash
pio device list
```

---

## Role Assignment

Each board shows a 5-second OLED menu at boot:

1. Press **BOOT button** (GPIO 0) to cycle: `Leaf → Relay → Gateway`
2. Wait for the 5-second timer to expire — the displayed role is saved to NVS
3. Role persists across power cycles — no need to re-select

Assign: **1 Gateway, 1 Leaf, 1 Relay**

---

## Monitoring

Open 3 separate terminal windows for serial output:

```bash
# Terminal 1 — Leaf
pio device monitor --port COM3 --baud 115200

# Terminal 2 — Gateway
pio device monitor --port COM9 --baud 115200

# Terminal 3 — Relay
pio device monitor --port COMx --baud 115200
```

---

## Expected Serial Output

### Boot (all roles)

```
[Boot] Reset reason: Power-on
[Role] Booting as: Gateway
[RTOS] All primitives initialized
...
[RTOS] Tasks created — Core 0: LoRa, Core 1: Harvest
```

### Leaf Node

```
Leaf (AODV)
AP: ForestCam-XXXX
IP: 192.168.4.1
CoAP: :5683
[OK] SD card: 3 image(s) found
[OK] LoRa beacon TX + RX enabled (AODV routing)
[LoRa] Beacon TX (31 bytes) — ForestCam-XXXX, 3 images
```

### Gateway Node

```
Gateway (RTOS+AODV)
Listening for beacons...
[LoRa] Beacon from ForestCam-XXXX — Leaf, 3 images, RSSI=-45 dBm
...
[Harvest Task] Starting harvest cycle on Core 1
--- Connecting to ForestCam-XXXX ---
[CoAP] Block 0/55 downloaded (1024 bytes)
...
[Harvest Task] Harvest cycle complete
```

### Relay Node

```
Relay (AODV)
AP: ForestCam-XXXX
[OK] SD card mounted for relay caching
[LoRa] Beacon TX — ForestCam-XXXX, 0 images
[LoRa] RELAYED beacon from ForestCam-YYYY, TTL=1
```

---

## Key Verification: Beacons During Harvest

**This is the primary test for FreeRTOS correctness.**

On the **Gateway** serial output, watch for beacon messages **during** an active
harvest download:

```
[Harvest Task] Starting harvest cycle on Core 1      <-- harvest begins
--- Connecting to ForestCam-XXXX ---
..........
[OK] Connected (IP: 192.168.4.2)
[CoAP] Block 0/55 downloaded (1024 bytes)
[LoRa] Beacon from ForestCam-YYYY — Relay, RSSI=-52  <-- BEACON DURING HARVEST
[CoAP] Block 5/55 downloaded (1024 bytes)
[LoRa] Beacon from ForestCam-YYYY — Relay, RSSI=-50  <-- STILL RECEIVING
...
[Harvest Task] Harvest cycle complete
```

**Before FreeRTOS:** No beacons would appear between "Starting harvest" and
"Harvest complete" (50-120+ seconds of silence).

**After FreeRTOS:** Beacons continue to arrive every ~30 seconds because LoRa
polling runs on Core 0, independent of the WiFi/CoAP download on Core 1.

---

## OLED Stack Monitoring

The gateway OLED shows FreeRTOS stack high-water marks:

```
Gateway (RTOS+AODV)
Nodes:2 Rtes:1
State: DOWNLOAD
Stk L:1024 H:2048
```

- `L:` = LoRa task stack remaining (words)
- `H:` = Harvest task stack remaining (words)

**Warning signs:**
- Values below **200** indicate the stack is nearly full — risk of overflow
- If a value hits **0**, the task will crash

---

## Troubleshooting

| Problem | Likely Cause | Fix |
|---------|-------------|-----|
| No beacons on gateway | Boards too far apart, or LoRa init failed | Check `[LoRa DIAG]` messages; bring boards within 5m for testing |
| Harvest WiFi timeout | Leaf AP not reachable from gateway | Ensure leaf is powered and WiFi AP is running (check OLED) |
| Stack overflow crash | Task stack too small | Increase `STACK_LORA` or `STACK_HARVEST` in `include/TaskConfig.h` |
| Watchdog timeout | Task blocked for too long | Check for deadlocks — ensure no nested mutex acquisition |
| `[RTOS] All primitives initialized` missing | `initRTOS()` not called | Verify `setup()` calls `initRTOS()` before task creation |
| Relay not forwarding beacons | Relay LoRa not ready | Check `_loraReady` flag in serial output |

---

## Modular Task File Split

As of the `feature/gateway-as-ap` branch, `main.cpp` has been refactored from ~1590 lines to ~295 lines. Task logic is now in dedicated source files:

| File | Purpose |
|------|---------|
| `src/TaskLoRaGateway.cpp` | Gateway LoRa task (beacons, election, harvest trigger) |
| `src/TaskLoRaLeafRelay.cpp` | Leaf/relay LoRa task (beacons, election, sleep check) |
| `src/TaskHarvestGateway.cpp` | Gateway harvest task (announce queue, CoAP download) |
| `src/TaskCoapServer.cpp` | CoAP server task (image serving + /announce) |
| `src/TaskRelayHarvest.cpp` | Relay store-and-forward task |
| `include/Globals.h` | Shared globals, RTC variables, extern declarations |

### Leaf-Initiated Announce Flow

Under the gateway-as-AP architecture, the harvest flow is announce-driven:

1. Leaf wakes from 180s timer deep sleep
2. Leaf connects as STA to gateway's persistent WiFi AP (`rtcGatewaySSID`)
3. Leaf sends CoAP POST `/announce` (MAC + image count) to gateway
4. Gateway's `taskCoapServer` receives announce, enqueues to `xAnnounceQueue`
5. `taskHarvestGateway` dequeues announce, downloads images from leaf's STA IP
6. Leaf returns to deep sleep after 120s idle timeout

---

## Files Changed (Original FreeRTOS Split)

| File | Change |
|------|--------|
| `include/TaskConfig.h` | Task config, FreeRTOS handle declarations |
| `src/TaskConfig.cpp` | `initRTOS()`, thread-safe LoRa/registry helpers |
| `src/main.cpp` | Setup + loop (OLED only); task bodies moved to dedicated files |
| `src/HarvestLoop.cpp` | Mutex-guarded LoRa + registry access, `vTaskDelay()` |
| `src/ElectionManager.cpp` | Thread-safe LoRa TX via `loraSendSafe()` |
| `src/AodvRouter.cpp` | Thread-safe `_broadcast()` via `loraSendSafe()` |
| `src/CoapClient.cpp` | `delay(1)` → `vTaskDelay(1)` |
