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

---

### 12. Deep Sleep + Two-Step Wake Protocol
- **Branch:** `feature/deep-sleep`
- **Description:** Leaf/relay nodes enter deep sleep between harvests (<10µA). Gateway sends WAKE_PING over LoRa to trigger DIO1 ext1 wakeup. Fast-path boot from RTC memory skips role menu. New HARVEST_WAKE_NODE state in harvest loop.
- **Key Files:** `include/DeepSleepManager.h`, `src/DeepSleepManager.cpp`, `include/AodvPacket.h`, `src/HarvestLoop.cpp`, `src/main.cpp`
- **Tests:** `test/test_deep_sleep/` (16 unit tests)
- **Test Guide:** [docs/DEEP_SLEEP_SETUP.md](DEEP_SLEEP_SETUP.md)

---

## In Progress

### 13. Python Dashboard
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
                    └── feature/dashboard ....... [Planned]
```
