# IOTForestCam Setup Guide

Consolidated setup and configuration reference for the ForestCam mesh camera network.

**Hardware**: LILYGO T3-S3 V1.2 (ESP32-S3 + SX1280 2.4 GHz LoRa PA)
**Framework**: Arduino via PlatformIO
**Architecture**: FreeRTOS dual-core, star-mesh hybrid (LoRa mesh control plane + WiFi star data plane)

---

## 1. Hardware Pin Mappings

### SD Card (HSPI / SPI3)

| Signal | GPIO |
|--------|------|
| CS     | 13   |
| CLK    | 14   |
| MOSI   | 11   |
| MISO   | 2    |

### LoRa SX1280 PA (FSPI / SPI2)

| Signal | GPIO |
|--------|------|
| MOSI   | 6    |
| MISO   | 3    |
| SCK    | 5    |
| CS     | 7    |
| DIO1   | 9    |
| BUSY   | 36   |
| RST    | 8    |
| RXEN   | 21   |
| TXEN   | 10   |

### OLED Display (I2C)

| Signal | GPIO |
|--------|------|
| SDA    | 18   |
| SCL    | 17   |

### Notes

- The T3-S3 V1.2 uses ESP32-S3FH4R2 with 2 MB QPI PSRAM (Quad, not Octal). Do NOT enable OPI PSRAM in platformio.ini -- it steals GPIO 33-37, conflicting with LORA_BUSY (GPIO 36).
- LoRa and SD card use separate SPI buses to avoid bus contention.
- Every node has an SD card attached. All nodes act as cameras.

---

## 2. Build and Flash

### Prerequisites

- PlatformIO Core (CLI) or PlatformIO IDE (VS Code extension)
- USB-C cable for the T3-S3

### Build

```bash
pio run -e esp32s3_unified
```

### Flash

```bash
pio run -e esp32s3_unified -t upload --upload-port COMX
```

Replace `COMX` with the actual COM port of the target board (e.g., `COM3`, `COM9`).

### Monitor Serial Output

```bash
pio device monitor --port COMX --baud 115200
```

### Flash and Monitor (Script)

A convenience script is provided for flashing, monitoring, and saving serial logs:

```bash
python flash_and_monitor.py --port COMX
```

### Build Environments

| Environment          | Description                              |
|---------------------|------------------------------------------|
| `esp32s3_unified`   | **Recommended.** Single binary, role chosen at boot. |
| `esp32s3`           | Legacy: compile-time Leaf node           |
| `esp32s3_gateway`   | Legacy: compile-time Gateway node        |
| `esp32s3_relay`     | Legacy: compile-time Relay node          |
| `native`            | Host-side unit tests (no hardware)       |

### Dependencies (from platformio.ini)

| Library                | Version  |
|------------------------|----------|
| RadioLib               | ^6.6.0   |
| Adafruit SSD1306       | ^2.5.7   |
| Adafruit GFX Library   | ^1.11.5  |

---

## 3. FreeRTOS Task Architecture

The firmware runs on both ESP32-S3 cores with the following task layout:

### Core 0 (Protocol Core: LoRa, AODV, Election)

| Task                  | Priority | Stack (words) | Source File             |
|-----------------------|----------|---------------|-------------------------|
| taskLoRaGateway       | 3        | 4096          | TaskLoRaGateway.cpp     |
| taskLoRaLeafRelay     | 3        | 4096          | TaskLoRaLeafRelay.cpp   |

### Core 1 (Network Core: WiFi, CoAP)

| Task                  | Priority | Stack (words) | Source File             |
|-----------------------|----------|---------------|-------------------------|
| taskHarvestGateway    | 2        | 8192          | TaskHarvestGateway.cpp  |
| taskCoapServerLoop    | 2        | 6144          | TaskCoapServerLoop.cpp  |
| taskRelayHarvest      | 2        | 8192          | TaskRelayHarvest.cpp    |

### Synchronization Primitives

| Primitive           | Type           | Purpose                            |
|---------------------|----------------|------------------------------------|
| xLoRaMutex          | Mutex          | Guards LoRaRadio SPI access        |
| xRegistryMutex      | Mutex          | Guards NodeRegistry R/W             |
| xLoRaTxQueue        | Queue (8)      | LoRa TX requests from any core     |
| xHarvestCmdQueue    | Queue (2)      | Harvest trigger (Core 0 -> Core 1) |
| xRelayHarvestQueue  | Queue (2)      | Relay harvest cmd (Core 0 -> Core 1)|
| xAnnounceQueue      | Queue (8)      | Leaf announce msgs (CoAP -> harvest)|
| xHarvestEvents      | EventGroup     | Harvest lifecycle events            |

**Locking order**: Acquire `xLoRaMutex` FIRST, then `xRegistryMutex`. Never hold both simultaneously if possible. Mutex timeout: 1000 ms.

---

## 4. Role Assignment

### Auto-Negotiate (Default)

All boards run the same `esp32s3_unified` firmware. On boot:

1. Node starts as LEAF by default.
2. Listens for gateway beacons during the startup grace period.
3. If no gateway is heard, Bully election promotes the highest-priority node to gateway.

### BOOT Button Override

Hold the BOOT button (GPIO 0) during power-on to force a role change. The selected role is persisted in NVS across power cycles.

### Bully Election Algorithm

The election system handles gateway failover automatically.

| Parameter                      | Value       |
|-------------------------------|-------------|
| Startup grace period          | 15 s + 0-5 s MAC-based jitter |
| Gateway timeout (missed beacons) | 90 s     |
| Gateway missing threshold     | 60 s (blocks sleep) |
| Backoff range                 | 500-3000 ms |
| Coordinator timeout           | 5000 ms     |
| Overall election timeout      | 10000 ms    |
| Reclaim cooldown              | 120 s       |
| Election TX repeat            | 2           |
| Election TX gap               | 500 ms      |
| GW reclaim TX repeat          | 3           |
| GW reclaim TX gap             | 666 ms      |

**Election states**: IDLE -> ELECTION_START -> WAITING -> PROMOTED/STOOD_DOWN -> ACTING_GATEWAY / RECLAIMED

**Priority**: Computed from last 4 bytes of MAC address as uint32 (little-endian). Higher value wins.

When a manually-assigned gateway recovers, it broadcasts GW_RECLAIM and the promoted node demotes back to LEAF.

---

## 5. Deep Sleep

Leaf and relay nodes enter deep sleep between harvest cycles to conserve power. Gateway nodes never sleep.

| Parameter              | Value      |
|-----------------------|------------|
| Active timeout         | 120 s (stay awake after boot/wake) |
| Timer wake interval    | 180 s (primary wake source)        |

### Wake Cycle

1. ESP32 wakes on timer (180 s).
2. Restores role + gateway SSID from RTC memory.
3. Connects STA to gateway AP, announces via CoAP POST `/announce`.
4. Gateway downloads images, leaf sleeps again after 120 s idle.

### RTC-Persistent Variables

These survive deep sleep (declared with `RTC_DATA_ATTR`):

| Variable           | Type       | Purpose                      |
|--------------------|------------|------------------------------|
| rtcBootCount       | uint32_t   | Boot counter                 |
| rtcSavedRole       | uint8_t    | Persisted node role          |
| rtcLastImgIdx      | int8_t     | Last image index served      |
| rtcSavedSSID       | char[32]   | Node's own AP SSID           |
| rtcStateValid      | bool       | RTC state validity flag      |
| rtcGatewayKnown    | bool       | Whether gateway was discovered|
| rtcGatewaySSID     | char[32]   | Gateway AP SSID to reconnect |

### Sleep Guards

The node will NOT sleep if:
- A harvest is currently in progress (`setHarvestInProgress(true)`)
- CoAP server is busy serving requests (`setCoapBusy(true)`)
- Active timeout has not expired (reset by `onActivity()`)
- Gateway is missing (election may be needed)

GPIO hold is enabled to maintain pin states during deep sleep.

---

## 6. CoAP Protocol

### Server Endpoints (Leaf/Relay)

| Method | Path                 | Description                           |
|--------|----------------------|---------------------------------------|
| GET    | `/image/{index}`     | Image data via Block2 (octet-stream)  |
| GET    | `/info`              | Image catalogue as JSON               |
| GET    | `/checksum/{index}`  | Fletcher-16 checksum as JSON          |
| GET    | `/.well-known/core`  | CoRE Link Format resource discovery   |
| POST   | `/announce`          | Leaf-initiated harvest announcement   |

### Block2 Transfer Parameters

| Parameter        | Value              |
|-----------------|---------------------|
| Block SZX        | 6 (1024 bytes)     |
| Max PDU size     | 1280 bytes         |
| Pipeline window  | 3 outstanding requests |
| Timeout          | 2000 ms            |
| Max retries      | 3                  |
| Checksum         | Fletcher-16        |
| Port             | 5683 (standard)    |

### Announce Payload (7 bytes)

| Offset | Size | Field      |
|--------|------|------------|
| 0      | 6    | MAC address|
| 6      | 1    | imageCount |

---

## 7. WiFi Configuration

- **Gateway**: Runs as AP with SSID `ForestCam-GW-XXYY` (derived from MAC).
- **Leaf/Relay**: Runs as STA, connects to gateway AP. Own AP SSID: `ForestCam-XXYY`.
- **SSID Prefix**: `ForestCam-`
- **AP IP**: 192.168.4.1 (standard ESP32 softAP default)

---

## 8. Serial Commands

The firmware accepts serial commands for runtime control:

| Command    | Description                          |
|-----------|--------------------------------------|
| `block`    | Block a node from the registry       |
| `unblock`  | Unblock a previously blocked node    |
| `list`     | List all known nodes in the registry |

---

## 9. Timing Constants

| Constant                    | Value      | Purpose                         |
|----------------------------|------------|----------------------------------|
| BEACON_INTERVAL_MS         | 30000 ms   | Beacon broadcast interval        |
| BEACON_JITTER_MS           | 2000 ms    | Random jitter on beacon timing   |
| HARVEST_LISTEN_PERIOD_MS   | 180000 ms  | How long to listen for harvests  |
| HARVEST_REACTIVE_DELAY_MS  | 15000 ms   | Delay before reactive harvest    |
| ROUTE_DISCOVERY_DELAY_MS   | 15000 ms   | AODV route discovery delay       |
| RELAY_CACHED_TIMEOUT_MS    | 120000 ms  | Relay cached data timeout        |

---

## 10. Troubleshooting

### SX1280 BUSY Timeout After Reset

The LoRa radio init retries up to 3 times. If BUSY stays HIGH for over 2 seconds after reset, the radio may be unresponsive.

**Fix**: Check wiring on GPIO 36 (BUSY). Ensure OPI PSRAM is NOT enabled in platformio.ini. Power cycle the board.

### SD Card Mount Failure

**Fix**: Verify SD card is inserted and formatted as FAT32. Check pin connections: CS=13, CLK=14, MOSI=11, MISO=2.

### WiFi Connection Fails (Leaf to Gateway)

**Fix**: Ensure gateway is running and AP is active. Check that the leaf's stored SSID in RTC memory matches the gateway's actual SSID. A cold boot clears RTC state, so the leaf will need to discover the gateway via LoRa beacons first.

### Election Keeps Triggering

**Fix**: The reclaim cooldown is 120 s. If the gateway sleeps before the cooldown expires, nodes may re-trigger election. Ensure gateway nodes never sleep (`shouldSleep()` returns false for gateways).

### CoAP Block Transfer Timeout

**Fix**: Check that the leaf's CoAP server is running and WiFi is connected. The client retries 3 times with a 2000 ms timeout. If pipelining fails, it falls back to sequential transfer automatically.

### Node Not Appearing in Registry

**Fix**: Verify LoRa radio initialized successfully (check serial logs for "SX1280 ready"). Ensure beacon magic byte is 0xFC and version is 0x02. Use the `list` serial command to see registered nodes.

### Deep Sleep Not Activating

**Fix**: Check that no harvest is in progress and CoAP server is not busy. The active timeout is 120 s from the last activity. If gateway is missing (60 s threshold), sleep is blocked to allow election.

### Images Not Being Harvested

Images are never deleted from source -- harvesting is a copy operation. The gateway copies its own `/images/` to `/received/` locally. Check that the leaf has images in `/images/` on the SD card and that the announce POST was received by the gateway.
