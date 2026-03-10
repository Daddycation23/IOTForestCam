# Part 2 — LoRa Control Plane Setup

## Overview

Part 2 adds the LoRa "control plane" — the remote trigger that wakes the ESP32
from deep sleep and tells it which image to transmit.

The device spends 99% of its time in deep sleep (< 10 µA).
The onboard SX1262 stays in continuous receive mode.
When a LoRa packet arrives, DIO1 goes HIGH, waking the ESP32 via EXT0 interrupt.

---

## Hardware

The **LILYGO T3-S3 V1.2** board has an **onboard SX1262** LoRa chip — no
external LoRa module or extra wiring is needed for the LoRa radio itself.

### SPI Bus Summary

| SPI Bus | Peripheral | Pins |
|---------|-----------|------|
| HSPI | MicroSD card | CLK=14, MISO=2, MOSI=11, CS=13 |
| FSPI | SX1262 (onboard) | SCK=5, MISO=3, MOSI=6, NSS=7 |

The two buses are independent — no conflict.

### SX1262 Control Pins (onboard, pre-wired)

| Signal | GPIO |
|--------|------|
| NSS (CS) | 7 |
| SCK | 5 |
| MOSI | 6 |
| MISO | 3 |
| DIO1 | 33 |
| RST | 8 |
| BUSY | 34 |

---

## RF Configuration

Defaults (change via `build_flags` in `platformio.ini`):

| Parameter | Default | Notes |
|-----------|---------|-------|
| Frequency | 915.0 MHz | US/AU ISM. EU = 868.0, Asia = 433.0 |
| Bandwidth | 125 kHz | |
| Spreading Factor | SF7 | Fastest; increase for longer range |
| Coding Rate | 4/5 | |
| Sync Word | 0x12 | Private network (not public TTN) |
| TX Power | +14 dBm | |

---

## Command Protocol

Part 2 uses a **two-packet** sequence so that the ESP32 can fully reinitialise
the radio after deep-sleep wakeup before reading the command:

```
Sender                                    ESP32 (sleeping)
  │                                             │
  │── Wake packet (any byte, e.g. 0xA1) ──────►│  DIO1 asserts HIGH
  │                                             │  ESP32 wakes, reinits LoRa
  │   (wait ~500 ms)                            │  startReceive(), waiting...
  │                                             │
  │── Command packet ───────────────────────────►│  ESP32 reads command
  │                                             │  Selects + streams image
  │                                             │  Returns to deep sleep
```

### Command Byte Table

| Byte | Action |
|------|--------|
| `0xA1` | Send next image (cycles through catalogue on each wake) |
| `0xC1` | Send image whose filename contains `fire` |
| `0xC2` | Send image whose filename contains `animal` |
| `0xD0` | Send catalogue image at index 0 |
| `0xD1` | Send catalogue image at index 1 |
| `0xDN` | Send catalogue image at index N (N = 0–15) |

If no command packet arrives within **3 seconds** of wake, the device defaults
to `0xA1` (random/next image) behaviour.

---

## Quick Start

### 1. Flash the firmware

```bash
pio run -t upload
```

### 2. Open serial monitor

```bash
pio device monitor
```

Expected output on first boot:

```
========================================
  IOT Forest Cam  (boot #1)
  Wakeup: Power-on / Reset
========================================
[SENSOR] Mounting SD card...
[SENSOR] 3 image(s) found
[LoRa] Initialising SX1262...
[LoRa] SX1262 ready — 915.0 MHz, SF7, BW125 kHz, +14 dBm
[LoRa] Listening — entering deep sleep. Goodnight.
```

The OLED shows `LoRa Listening / Waiting for / 0xA1 wake cmd`.

### 3. Send a wake + command from a second LoRa node

Example using RadioLib on the sender node:

```cpp
// Step 1: send wake packet
radio.transmit((uint8_t[]){0xA1}, 1);
delay(500);

// Step 2: send command packet
uint8_t cmd[] = {0xC1};  // send "fire" image
radio.transmit(cmd, 1);
```

Expected receiver output:

```
========================================
  IOT Forest Cam  (boot #2)
  Wakeup: LoRa DIO1
========================================
[LoRa] SX1262 ready — 915.0 MHz, SF7, BW125 kHz, +14 dBm
[LoRa] Listening for LoRa packets...
[LoRa] Packet — RSSI -65.3 dBm, SNR 9.2 dB, cmd=0xC1
[LoRa] CMD=1 ARG=0 RSSI=-65.3 dBm SNR=9.2 dB
[STREAM] /images/fire001.jpg — 45320 bytes, 89 blocks
  Block    0/89  [512 B]
  Block   10/89  [512 B]
  ...
  Block   88/89  [ 168 B]  ◄ LAST
[STREAM] Done — 45320 bytes in 84 ms (526.4 KB/s)
[LoRa] Listening — entering deep sleep. Goodnight.
```

---

## Integration Points

### Part 1 (Storage Reader)

`StorageReader` is consumed by `main.cpp` directly via the `streamImageBlocks()`
helper. No changes to Part 1 files are needed.

### Part 3 (CoAP Data Plane)

Inside `streamImageBlocks()` in `src/main.cpp`, look for:

```cpp
// ── PART 3 HAND-OFF ──────────────────────────────────
// coap.sendBlock(block);   ← Part 3 inserts CoAP call here
// ─────────────────────────────────────────────────────
```

Replace the placeholder comment with your `coap.sendBlock(block)` call.
The `BlockReadResult` fields map directly to CoAP Block2 options:

| `BlockReadResult` field | CoAP Block2 usage |
|------------------------|-------------------|
| `block.data` | Payload bytes |
| `block.length` | Payload length |
| `block.blockIndex` | Block NUM |
| `block.isLast` | M (More) flag = `!isLast` |

For CoAP retransmission of a lost block, call:
```cpp
storage.readBlock(blockIndex, block);
```

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| `SX1262 begin() failed` | Wiring issue or wrong pins | Re-check FSPI pins in `platformio.ini` |
| Device wakes but no command received | Sender too slow | Increase sender delay between wake and command packets (try 700 ms) |
| Device wakes repeatedly without command | DIO1 floating HIGH | Confirm SX1262 is in startReceive() before sleep; check BUSY pin wiring |
| Wrong image selected | Filename mismatch | Rename SD card files to include `fire` or `animal` in the name |
| SD fails after LoRa wakeup | SPI conflict | Confirm SD uses HSPI and LoRa uses FSPI (not the same bus) |
