# Deep Sleep + Two-Step Wake Protocol — Setup & Testing

## Overview

Leaf and relay nodes enter deep sleep between harvest cycles to conserve
power. The SX1280's DIO1 pin (GPIO 9) triggers ESP32-S3 wakeup when a
LoRa packet is received.

**Gateway nodes never sleep.**

### Power Impact

| State | Current Draw (est.) |
|-------|-------------------|
| Active (WiFi + LoRa + SD) | ~150-250 mA |
| Deep sleep (SX1280 RX) | < 10 µA |

---

## Two-Step Wake Protocol

```
Gateway                              Leaf (sleeping)
  |                                        |
  |── WAKE_PING (0x40) ──────────────────>|  DIO1 HIGH → ESP32 wakes
  |                                        |  Reinit radio, start WiFi AP
  |   (wait 2.5s)                          |
  |                                        |
  |── HARVEST_CMD ────────────────────────>|  Download begins
  |<── images via CoAP ──────────────────  |
  |                                        |
  |                                        |  Timeout → deep sleep
```

1. Gateway sends `WAKE_PING` (3-byte LoRa packet: `0xFC 0x01 0x40`)
2. DIO1 goes HIGH → ESP32-S3 wakes via ext1 wakeup
3. Leaf reinits radio, starts WiFi AP (using RTC-saved SSID)
4. Gateway waits 2.5s, then proceeds with WiFi connect + CoAP download
5. After harvest or 2-minute timeout, leaf returns to deep sleep

---

## Sleep/Wake Lifecycle

### Cold Boot (power-on)
1. 5-second OLED role selection menu (BOOT button)
2. Full hardware init (SD, LoRa, WiFi AP, CoAP server)
3. Active for 2 minutes (allows first harvest)
4. → Deep sleep

### LoRa Wake (fast-path)
1. Skip role menu — restore from RTC memory
2. Init radio + WiFi AP immediately
3. Wait for HARVEST_CMD (3s timeout)
4. Serve images via CoAP
5. → Deep sleep

---

## Testing

### Unit Tests (no hardware)

```bash
pio test -e native -f test_deep_sleep
```

16 tests validate: wake packet types, active timeout logic, RTC persistence, two-step timing, harvest wake state.

### Hardware Test (3 boards)

1. Flash all boards: `pio run -e esp32s3_unified -t upload`
2. Assign roles: 1 Gateway, 1 Leaf, 1 Relay

### What to Watch

**Leaf serial output — going to sleep:**
```
[DeepSleep] Active timeout expired — entering deep sleep
[DeepSleep] Boot count: 1
[DeepSleep] State saved — role=0, ssid=ForestCam-AABB, imgIdx=-1, bootCount=1
[DeepSleep] Radio in continuous RX for DIO1 wakeup
[DeepSleep] Entering deep sleep — DIO1 (GPIO 9) armed for ext1 wakeup, boot #1
```

**Leaf serial output — waking up:**
```
[Boot] Reset reason: Deep sleep (boot #2)
[Wake] Fast-path: restored role=Leaf, SSID=ForestCam-AABB
```

**Gateway serial output — wake + harvest:**
```
[Harvest] -> WAKE_NODE
[Harvest] WAKE_PING sent for ForestCam-AABB — waiting 2500ms for node to boot
[Harvest] -> CONNECT
--- Connecting to ForestCam-AABB ---
..........
[OK] Connected
```

### Verifying Deep Sleep Current

If you have a multimeter:
1. Disconnect USB
2. Power leaf from battery via 3.3V pin
3. Measure current in series
4. Active: ~150-250 mA
5. After 2 min: should drop to < 10 µA

### Timing Test

1. Let leaf go to sleep (wait 2 minutes after boot)
2. Leaf OLED goes blank and serial stops
3. Gateway starts harvest cycle → sends WAKE_PING
4. Leaf should wake within 100ms
5. Leaf WiFi AP should be up within 2.5s
6. Gateway should successfully connect and download

---

## Configuration

| Constant | Default | File | Purpose |
|----------|---------|------|---------|
| `SLEEP_ACTIVE_TIMEOUT_MS` | 120000 (2 min) | `DeepSleepManager.h` | Stay awake after boot |
| `SLEEP_WAKE_CMD_TIMEOUT_MS` | 3000 (3s) | `DeepSleepManager.h` | Wait for cmd after wake |
| `SLEEP_WAKE_SETTLE_DELAY_MS` | 500 (0.5s) | `DeepSleepManager.h` | Sender delay between ping and cmd |
| `LORA_DIO1` | GPIO 9 | `LoRaRadio.h` | DIO1 wakeup pin |

---

## Troubleshooting

| Problem | Likely Cause | Fix |
|---------|-------------|-----|
| Leaf never sleeps | CoAP requests keep resetting timer | Check `onActivity()` calls |
| Leaf doesn't wake | DIO1 not asserted, or GPIO 9 not RTC-capable | Verify `esp_sleep_enable_ext1_wakeup` with GPIO 9 bitmask |
| Leaf wakes but can't connect | WiFi AP not starting fast enough | Increase wait in `_doWakeNode()` (default 2.5s) |
| RTC state lost | Not saved before sleep | Check `saveState()` is called in sleep path |
| Fast-path skipped | `rtcStateValid` is false | First boot always uses normal path; fast-path starts from boot #2 |

---

## New Packet Types

| Type | Value | Purpose |
|------|-------|---------|
| `PKT_TYPE_WAKE_PING` | `0x40` | Triggers DIO1 wakeup (3-byte minimal packet) |
| `PKT_TYPE_WAKE_BEACON_REQ` | `0x41` | Request beacon after wake |

---

## Files Changed

| File | Change |
|------|--------|
| `include/DeepSleepManager.h` | NEW — Sleep/wake management class |
| `src/DeepSleepManager.cpp` | NEW — Implementation with ext1 wakeup, RTC persistence |
| `include/AodvPacket.h` | Added `PKT_TYPE_WAKE_PING` and `PKT_TYPE_WAKE_BEACON_REQ` |
| `include/HarvestLoop.h` | Added `HARVEST_WAKE_NODE` state |
| `src/HarvestLoop.cpp` | Implemented `_doWakeNode()` with two-step wake |
| `src/main.cpp` | Fast-path wake in setup(), sleep check in leaf/relay task |
| `test/test_deep_sleep/` | NEW — 16 unit tests |
