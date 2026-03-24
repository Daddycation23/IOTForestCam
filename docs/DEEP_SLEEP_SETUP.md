# Timer-Based Deep Sleep — Setup & Testing

## Overview

Leaf and relay nodes enter deep sleep between harvest cycles to conserve
power. A 180-second timer wakeup is the sole wake source — LoRa DIO1 ext1
wake is **not used** (unreliable on SX1280 PA hardware).

**Gateway nodes never sleep.**

### Power Impact

| State | Current Draw (est.) |
|-------|-------------------|
| Active (WiFi + LoRa + SD) | ~150-250 mA |
| Deep sleep | < 10 µA |

---

## Wake Cycle

```
Leaf (sleeping)                          Gateway (persistent AP)
  |                                        |
  |  180s timer fires → ESP32 wakes        |
  |  Restore RTC state (role, SSID, etc.)  |
  |  WiFi.mode(WIFI_STA)                   |
  |  WiFi.begin(rtcGatewaySSID)            |
  |── STA connect to gateway AP ────────>  |  (ForestCam-GW-XXYY)
  |── POST /announce (MAC + imgCount) ──>  |  Gateway enqueues harvest
  |                                        |── GET /info, GET /image ──>|
  |<── CoAP Block2 pipelined download ──   |  (downloads from leaf STA IP)
  |                                        |
  |  120s idle timeout → deep sleep        |
```

1. Leaf enters deep sleep with 180-second timer wakeup armed
2. Timer fires → ESP32-S3 wakes, fast-path boot from RTC memory
3. Leaf connects as STA to gateway's persistent WiFi AP (`rtcGatewaySSID`)
4. Leaf sends CoAP POST `/announce` with MAC + image count
5. Gateway downloads images from leaf via CoAP pipelined Block2
6. After harvest or 120s idle timeout, leaf returns to deep sleep

On **cold boot** (first power-on), the leaf has no cached gateway SSID.
It starts in AP mode, participates in election, and caches the gateway
SSID from COORDINATOR/GW_RECLAIM/beacon packets. Subsequent timer wakes
use the cached SSID for STA connect.

---

## Sleep/Wake Lifecycle

### Cold Boot (power-on)
1. 5-second OLED role selection menu (BOOT button)
2. Full hardware init (SD, LoRa, WiFi AP, CoAP server)
3. Election runs, gateway SSID cached to RTC
4. Active for 2 minutes (allows first harvest)
5. → Deep sleep

### Timer Wake (fast-path)
1. Skip role menu — restore from RTC memory
2. Connect as STA to gateway AP (`rtcGatewaySSID`)
3. POST `/announce` to gateway
4. Serve images via CoAP when gateway downloads
5. 120s idle timeout → deep sleep

### Fallback (no cached SSID)
1. If `rtcGatewaySSID` is empty, fall back to AP mode
2. Behave like cold boot (start AP, wait for gateway-initiated harvest)

---

## GPIO Hold (SPI Bus Stability)

During deep sleep, certain GPIOs must be held to keep the SPI bus stable
and prevent floating pins from causing issues on wake:

- `gpio_hold_en(GPIO_NUM_21)` — RXEN (PA receive enable), held HIGH
- `gpio_deep_sleep_hold_en()` — enables hold across deep sleep

On wake, holds are released:
- `gpio_hold_dis(GPIO_NUM_21)`
- `gpio_deep_sleep_hold_dis()`

This prevents SPI bus contention between the SX1280 and SD card on wake.

---

## RTC State Persistence

The following state is preserved in RTC memory across deep sleep:

| Variable | Purpose |
|----------|---------|
| `rtcStateValid` | Guard flag — true after first successful save |
| `rtcRole` | Node role (LEAF/RELAY/GATEWAY) |
| `rtcGatewaySSID` | Cached gateway AP SSID (for STA connect on wake) |
| `rtcGatewayKnown` | Whether a gateway was known before sleep |
| `rtcBootCount` | Incremented each wake cycle |
| `rtcImageIndex` | Last image index served |

---

## Sleep Guard Logic

The node will **not** enter deep sleep if any of these conditions are true:

| Guard | Reason |
|-------|--------|
| `isElectionActive()` | Election in progress — must participate |
| `isGatewayMissing()` | Gateway absent >60s — election may be needed |
| CoAP `blocksSent()` increasing | Active image transfer in progress |
| CoAP `requestCount()` increasing | Gateway polling (keep-alive) |

Sleep is checked in `TaskLoRaLeafRelay.cpp` with a 120s idle timeout
(`SLEEP_ACTIVE_TIMEOUT_MS`). Any CoAP activity resets the timer.

---

## Testing

### Unit Tests (no hardware)

```bash
pio test -e native -f test_deep_sleep
```

16 tests validate: wake types, active timeout logic, RTC persistence, harvest wake state.

### Hardware Test (3 boards)

1. Flash all boards: `pio run -e esp32s3_unified -t upload`
2. Assign roles: 1 Gateway, 2 Leaves (or auto-negotiate)

### What to Watch

**Leaf serial output — going to sleep:**
```
[DeepSleep] Active timeout expired — entering deep sleep
[DeepSleep] Boot count: 1
[DeepSleep] State saved — role=0, ssid=ForestCam-GW-AABB, imgIdx=-1, bootCount=1
[DeepSleep] Entering deep sleep — timer 180s, boot #1
```

**Leaf serial output — waking up:**
```
[Boot] Reset reason: Deep sleep (boot #2)
[Wake] Fast-path: restored role=Leaf, gateway SSID=ForestCam-GW-AABB
[WiFi] STA connecting to ForestCam-GW-AABB...
[WiFi] STA connected, IP=192.168.4.x
[CoAP] POST /announce sent — MAC=AABBCCDDEEFF, images=3
```

**Gateway serial output — announce + harvest:**
```
[CoAP] POST /announce from 192.168.4.x — MAC=...EEFF, 3 images
[Harvest] Announce-triggered harvest for node EEFF
[CoAP] Block 0/55 downloaded (1024 bytes)
...
[Harvest] Node EEFF complete — 3 images, 156 KB
```

### Verifying Deep Sleep Current

If you have a multimeter:
1. Disconnect USB
2. Power leaf from battery via 3.3V pin
3. Measure current in series
4. Active: ~150-250 mA
5. After 2 min: should drop to < 10 µA

---

## Configuration

| Constant | Default | File | Purpose |
|----------|---------|------|---------|
| `SLEEP_ACTIVE_TIMEOUT_MS` | 120000 (2 min) | `DeepSleepManager.h` | Stay awake after boot/wake |
| `SLEEP_TIMER_US` | 180000000 (180s) | `DeepSleepManager.h` | Deep sleep timer duration |

---

## Troubleshooting

| Problem | Likely Cause | Fix |
|---------|-------------|-----|
| Leaf never sleeps | CoAP requests keep resetting timer | Check `onActivity()` calls |
| Leaf wakes but can't connect to gateway | Gateway AP not running or SSID mismatch | Check `rtcGatewaySSID` matches gateway's actual SSID |
| RTC state lost | Not saved before sleep | Check `saveState()` is called in sleep path |
| Fast-path skipped | `rtcStateValid` is false | First boot always uses normal path; fast-path starts from boot #2 |
| Leaf sleeps during election | Sleep timer fired before election started | `isGatewayMissing()` guard blocks sleep when GW beacon missing >60s. Also `isElectionActive()` blocks sleep during active election. Check `ElectionManager.h` |
| Leaf sleeps during harvest | `blocksSent()` not polled | Ensure CoAP activity resets sleep timer via `onActivity()` |
| Gateway SSID not cached | No COORDINATOR/beacon received before first sleep | First cycle uses AP mode; SSID is cached for subsequent wakes |

---

## Files Changed

| File | Change |
|------|--------|
| `include/DeepSleepManager.h` | Sleep/wake management class with timer wake |
| `src/DeepSleepManager.cpp` | Implementation with timer wakeup, RTC persistence |
| `include/HarvestLoop.h` | `HARVEST_WAKE_NODE` state (legacy, bypassed for announced nodes) |
| `src/HarvestLoop.cpp` | Announce-driven harvest flow |
| `src/TaskLoRaLeafRelay.cpp` | Sleep check with election + CoAP activity guards |
| `include/ElectionManager.h` | `isGatewayMissing()` + `isElectionActive()` sleep guards |
| `test/test_deep_sleep/` | 16 unit tests |
