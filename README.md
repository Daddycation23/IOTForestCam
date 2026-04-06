# IOTForestCam - ESP32-S3 Forest Camera Mesh Network

[![PlatformIO](https://img.shields.io/badge/PlatformIO-Arduino-blue.svg)](https://platformio.org/)
[![Hardware](https://img.shields.io/badge/Hardware-LILYGO%20T3--S3%20V1.2-orange.svg)](https://www.lilygo.cc/)
[![Tests](https://img.shields.io/badge/Tests-181%20passing-brightgreen.svg)](test/)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Autonomous forest-deployed camera mesh network using ESP32-S3 microcontrollers with LoRa mesh control plane and WiFi star data plane for periodic image harvesting. Designed for long-term deployment with deep sleep power management.

**Academic Project**: Singapore Institute of Technology (SIT) - Year 2 Trimester 2 CSC2016 Internet Of Things: Protocols and Networks, Group 02

---

## 📋 Table of Contents

- [Overview](#overview)
- [Key Features](#key-features)
- [Architecture](#architecture)
- [Technology Stack](#technology-stack)
- [Quick Start Guide](#quick-start-guide)
- [Role Assignment](#role-assignment)
- [Project Structure](#project-structure)
- [Documentation](#documentation)
- [Verification Checklist](#verification-checklist)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## Overview

IOTForestCam is a distributed camera system where every node acts as a camera using pre-loaded JPEG images on an SD card as a virtual sensor pipeline. Nodes operate autonomously, staying awake periodically to transmit images to a gateway node that collects and stores them.

**Core Workflow**:
1. **Leaf nodes** boot and **stay awake** for a 5-minute active window
2. During this window: broadcast LoRa beacons every ~30s, participate in elections, serve images via CoAP
3. After at least one harvest completes + 5 min inactivity → enter **deep sleep** (180s timer)
4. On timer wake: reconnect to gateway WiFi, announce availability via CoAP POST, serve images, repeat cycle
5. **Gateway** discovers nodes via LoRa beacons, downloads images over WiFi using CoAP Block2 transfer, saves to SD card
6. **Relay nodes** forward harvest commands for out-of-range leaf nodes using store-and-forward over LoRa

All communication uses standard protocols: CoAP (RFC 7252) with Block2 (RFC 7959) and AODV mesh routing.

**Latest Design Review**: [ref/Group02-Design_Review_Report.pdf](ref/Group02-Design_Review_Report.pdf)

---

## Key Features

- ✅ **Virtual Sensor Pipeline**: SD card JPEG images as camera data (no physical camera required for development)
- ✅ **CoAP Block2 Transfer**: Pipelined image downloads with 1024-byte blocks, Fletcher-16 checksums
- ✅ **AODV Mesh Routing**: Reactive routing enables multi-hop image harvesting (12-entry route table)
- ✅ **Dynamic Role Assignment**: Bully election algorithm for automatic gateway selection and failover
- ✅ **Deep Sleep Power Management**: <10μA consumption during sleep, 180s timer wake cycles with RTC state persistence
- ✅ **Star-Mesh Hybrid Topology**: LoRa mesh for control plane, WiFi star for data plane
- ✅ **FreeRTOS Dual-Core**: Protocol tasks on Core 0 (LoRa/AODV/Election), network tasks on Core 1 (WiFi/CoAP/Harvest)
- ✅ **Comprehensive Testing**: 181 native unit tests across 19 test suites (no hardware required)
- ✅ **Monitoring Tools**: Terminal-based and GUI dashboards with live image viewer via CoAP
- ✅ **Serial Command Interface**: Runtime node blocking for lab testing (`block`, `unblock`, `list`)

---

## Architecture

### Node Roles

| Role | Description | Power | WiFi | LoRa |
|------|-------------|-------|------|------|
| **Gateway** | Image collector, runs persistent AP | Always on | AP mode | Beacon RX, election |
| **Leaf** | Camera node, stores images on SD | Deep sleep cycles | STA (connects to GW) | Beacon TX, election |
| **Relay** | Extends range for distant leaves | Deep sleep cycles | STA (connects to GW) | Beacon TX/RX, forwarding |

### Communication Planes

- **Control Plane (LoRa Mesh)**: SX1280 2.4 GHz radio for beacons, AODV routing, and gateway election
- **Data Plane (WiFi Star)**: Images transferred over WiFi using CoAP Block2 protocol to gateway

### Hardware Platform

- **MCU**: LILYGO T3-S3 V1.2 (ESP32-S3, dual-core, 4MB flash, 2MB QPI PSRAM)
- **Radio**: SX1280 LoRa PA (FSPI bus, 2.4 GHz)
- **Storage**: SD card on HSPI bus (FAT32)
- **Display**: SSD1306 128x64 OLED (I2C)

---

## Technology Stack

| Category | Technology |
|----------|-----------|
| **Hardware** | ESP32-S3, SX1280 LoRa, SD Card, SSD1306 OLED |
| **Framework** | Arduino via PlatformIO |
| **RTOS** | FreeRTOS (dual-core task scheduling) |
| **Protocols** | CoAP (RFC 7252), Block2 (RFC 7959), AODV |
| **Libraries** | RadioLib 6.6.0, Adafruit SSD1306 2.5.7, Adafruit GFX 1.11.5 |
| **Testing** | Unity test framework (181 tests, 19 suites) |
| **Tools** | Python 3.7+ (pyserial, aiocoap, Pillow, tkinter) |

---

## Quick Start Guide

### Prerequisites

- **PlatformIO Core** (CLI) or **PlatformIO IDE** (VS Code extension)
- **Python 3.7+** (for dashboard and monitoring tools)
- **USB-C cable** for T3-S3 boards
- **LILYGO T3-S3 V1.2 hardware** (for deployment; tests run without hardware)

### Step 1: Clone the Repository

```bash
cd IOTForestCam
```

### Step 2: Build Firmware

```bash
# Build unified firmware (recommended - single binary for all boards)
pio run -e esp32s3_unified
```

### Step 3: Flash to Hardware

```bash
# Flash unified firmware to a specific COM port
pio run -e esp32s3_unified -t upload --upload-port COMX

# Or use the convenience script (flashes + monitors serial output)
python tools/flash_and_monitor.py --port COMX
```

> **Note**: Replace `COMX` with your board's actual COM port (e.g., `COM3`, `COM6`, `COM9` on Windows).

### Step 4: Monitor Serial Output

```bash
# Using PlatformIO device monitor
pio device monitor --port COMX --baud 115200

# Or monitor multiple nodes simultaneously (up to 3 nodes)
python tools/flash_and_monitor.py 300
```

The convenience script flashes all configured nodes and monitors their serial output simultaneously, saving logs to `node_XXXX.log` files.

### Step 5: Run Python Dashboard (Optional)

Monitor node status and view harvested images using the Python tools:

```bash
# Terminal-based live dashboard (parses serial output)
python tools/dashboard.py --port COMX

# GUI dashboard with live image viewer via CoAP
python tools/dashboard_gui.py

# Standalone CoAP image downloader
python tools/viewer.py
```

### Step 6: Run Tests (No Hardware Required)

All tests run on the host machine using the `native` platform:

```bash
# Run all 19 test suites (181 tests)
pio test -e native

# Run a specific test suite
pio test -e native -f test_coap_block
pio test -e native -f test_election_state
pio test -e native -f test_harvest_fsm

# Verbose output with detailed test information
pio test -e native -v
```

### Step 7: Deploy Multiple Nodes

For a multi-node deployment:

1. **Flash the same firmware** (`esp32s3_unified`) to all boards
2. **Power on all boards** - auto-negotiation will assign roles via Bully election
3. **Force gateway role** (optional): Hold the BOOT button (GPIO 0) during power-on
4. **Monitor deployment**: Use serial output or Python dashboard to verify operation
5. **Lab testing**: Use `block`/`unblock` serial commands to simulate out-of-range nodes

---

## Role Assignment

### Auto-Negotiate (Default)

All boards run the same `esp32s3_unified` firmware. On boot:

1. Node starts as **LEAF** by default
2. Listens for gateway beacons during startup grace period (15s + jitter)
3. If no gateway is heard, **Bully election** promotes the highest-priority node to gateway
4. Priority is computed from the last 4 bytes of MAC address (higher value wins)

### BOOT Button Override

Hold the **BOOT button** (GPIO 0) during power-on to force **gateway role**. The selected role is persisted in NVS and survives power cycles.

### Election Parameters

| Parameter | Value |
|-----------|-------|
| Startup grace period | 15 s + 0-5 s MAC-based jitter |
| Gateway timeout (missed beacons) | 90 s |
| Election timeout | 10 s |
| Reclaim cooldown | 120 s |

---

## Project Structure

```
IOTForestCam/
├── src/                    # Firmware source code (23 files)
│   ├── main.cpp            # Entry point, setup, role determination
│   ├── TaskLoRaGateway.cpp # Gateway LoRa task (Core 0)
│   ├── TaskLoRaLeafRelay.cpp # Leaf/Relay LoRa task (Core 0)
│   ├── TaskHarvestGateway.cpp # Gateway harvest task (Core 1)
│   ├── TaskCoapServerLoop.cpp # CoAP server task (Core 1)
│   ├── LoRaRadio.cpp       # SX1280 radio wrapper (RadioLib)
│   ├── CoapServer.cpp      # CoAP server with Block2 transfer
│   ├── CoapClient.cpp      # CoAP client for image downloads
│   ├── AodvRouter.cpp      # AODV mesh routing
│   ├── ElectionManager.cpp # Bully election algorithm
│   └── ...                 # Additional modules
├── include/                # Header files (19 files)
├── test/                   # Unit tests (19 suites, 181 tests)
│   ├── test_coap_block/    # CoAP Block2 transfer tests
│   ├── test_election_state/# Election state machine tests
│   ├── test_harvest_fsm/   # Harvest FSM tests
│   └── ...                 # Additional test suites
├── tools/                  # Python utilities
│   ├── dashboard.py        # Terminal-based live dashboard
│   ├── dashboard_gui.py    # GUI dashboard with image viewer
│   ├── viewer.py           # Standalone CoAP image downloader
│   ├── flash_and_monitor.py# Flash + serial monitor automation
│   └── packet_loss_analyzer.py # LoRa packet loss analysis
├── docs/                   # Documentation
│   ├── SETUP_GUIDE.md      # Detailed setup and configuration
│   ├── TESTING_GUIDE.md    # Test running and development
│   ├── FEATURE_STATUS.md   # Implemented features list
│   └── PROTOCOL_SPECS.md   # Protocol specifications
├── ref/                    # Reference documents
│   └── Group02-Design_Review_Report.pdf  # Latest design review
├── platformio.ini          # Build configuration (5 environments)
└── README.md               # This file
```

---

## Documentation

| Document | Description |
|----------|-------------|
| 📄 **[Design Review Report](ref/Group02-Design_Review_Report.pdf)** | Latest design review (PDF) |
| 📖 **[Setup Guide](docs/SETUP_GUIDE.md)** | Hardware pins, build/flash, deep sleep, CoAP, timing constants |
| 🧪 **[Testing Guide](docs/TESTING_GUIDE.md)** | Running tests, verification checkpoints, test development |
| ✅ **[Feature Status](docs/FEATURE_STATUS.md)** | List of 25+ implemented features |
| 📡 **[Protocol Specs](docs/PROTOCOL_SPECS.md)** | CoAP endpoints, Block2 parameters, beacon format |

---

## Verification Checklist

After flashing, verify the following in serial output:

- [ ] **"SX1280 ready"** - LoRa radio initialized successfully
- [ ] **SD card mounted** - No mount failure errors
- [ ] **OLED display active** - Shows node role and status
- [ ] **Role assigned** - Check serial logs for GATEWAY/LEAF/RELAY
- [ ] **Gateway AP visible** - SSID `ForestCam-GW-XXYY` (if gateway)
- [ ] **WiFi connected** - Leaf connects to gateway AP (if leaf/relay)
- [ ] **Images harvested** - Gateway saves to `/received/` on SD card
- [ ] **Deep sleep activates** - Leaf/relay sleep after 5 min idle + harvest complete
- [ ] **Beacons transmitted** - Every 30s (+/- 2s jitter)

---

## Troubleshooting

### Common Issues

| Issue | Solution |
|-------|----------|
| **SX1280 BUSY timeout** | Check GPIO 36 wiring; ensure OPI PSRAM is NOT enabled in `platformio.ini`; power cycle board |
| **SD card mount failure** | Verify SD card is inserted and formatted as FAT32; check pins: CS=13, CLK=14, MOSI=11, MISO=2 |
| **WiFi connection fails** | Ensure gateway AP is active; check leaf's stored SSID matches gateway; cold boot clears RTC state |
| **Election keeps triggering** | Gateway timeout is 90s; ensure gateway never sleeps; check reclaim cooldown (120s) |
| **CoAP transfer timeout** | Verify leaf's CoAP server is running and WiFi is connected; client retries 3x with 2000ms timeout |
| **Node not in registry** | Check LoRa radio init in serial logs; verify beacon magic=0xFC, version=0x02; use `list` command |
| **Deep sleep not activating** | Check no harvest in progress and CoAP server not busy; requires harvest completion + 5 min idle |

### Serial Commands

Type these into the Serial Monitor (115200 baud) for runtime control:

```
block 80E4      # Block node with MAC suffix 80:E4 (drops all LoRa packets)
unblock 80E4    # Remove block on node 80:E4
list            # Show currently blocked nodes
```

> **Note**: Blocks are **volatile** (reset on reboot) and must be **bidirectional** (block on both sides) for correct simulation. Max 8 blocked entries.

### Lab Relay Testing

To force relay formation when all nodes are within range:

1. Identify MAC suffixes from boot logs (e.g., `ForestCam-86CC` → suffix `86CC`)
2. On the gateway, type: `block XXYY` (far leaf's suffix)
3. On the far leaf, type: `block WWZZ` (gateway's suffix)
4. Leave the relay node unblocked — it must hear both sides

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

---

## Team

**Group 02** - Singapore Institute of Technology (SIT), Year 2 Trimester 2 CSC2016 Internet Of Things: Protocols and Networks

---

## Acknowledgments

- **RadioLib** by Jan Gromes for SX1280 support
- **Adafruit** for SSD1306 and GFX libraries
- **Unity** test framework for native testing
- **PlatformIO** for cross-platform build system
