# IOTForestCam Testing Guide

Reference for running and verifying all test suites in the ForestCam project.

---

## 1. Test Overview

The project has **17 test suites** with a total of **152 test cases**, all running on the `native` platform (host-side, no hardware required).

| # | Suite                       | Tests | Description                          |
|---|-----------------------------|-------|--------------------------------------|
| 1 | test_auto_role              | 23    | Auto role assignment and NVS persistence |
| 2 | test_beacon_v2              | 8     | Beacon v2 serialize/parse, SSID derivation |
| 3 | test_coap_block             | 15    | CoAP Block2 transfer logic           |
| 4 | test_coap_server            | 7     | CoAP server request handling         |
| 5 | test_deep_sleep             | 16    | Deep sleep state save/restore, timeouts |
| 6 | test_deep_sleep_guards      | 5     | Sleep guard conditions (harvest, CoAP busy) |
| 7 | test_election_edge          | 7     | Election edge cases and error paths  |
| 8 | test_election_packet        | 10    | Election packet serialize/parse      |
| 9 | test_election_state         | 7     | Election state machine transitions   |
| 10| test_harvest_fsm            | 8     | Harvest finite state machine         |
| 11| test_harvest_trigger        | 8     | Harvest trigger conditions           |
| 12| test_native                 | 9     | Core native logic tests              |
| 13| test_node_registry          | 3     | Node registry CRUD operations        |
| 14| test_node_registry_announce | 5     | Node registry announce handling      |
| 15| test_relay_harvest          | 6     | Relay harvest forwarding logic       |
| 16| test_thread_safety          | 8     | Thread safety and mutex ordering     |
| 17| test_aodv_expiry            | 7     | AODV route expiry and cleanup        |
|   | **Total**                   | **152** |                                    |

---

## 2. Running Tests

### All Test Suites (Native)

```bash
pio test -e native
```

This runs all 17 suites on the host machine without any hardware.

### Single Test Suite

```bash
pio test -e native -f test_beacon_v2
```

Replace `test_beacon_v2` with any suite name from the table above.

### Verbose Output

```bash
pio test -e native -v
```

### Test Framework

All tests use the **Unity** test framework (`test_framework = unity` in platformio.ini). Each test file follows the pattern:

```
test/<suite_name>/test_main.cpp
```

Each `test_main.cpp` contains a `main()` function that calls `UNITY_BEGIN()`, registers tests with `RUN_TEST()`, and ends with `UNITY_END()`.

---

## 3. Hardware Testing

### Flash and Monitor Script

For on-device testing with serial log capture:

```bash
python flash_and_monitor.py --port COMX
```

This script:
1. Flashes the `esp32s3_unified` firmware to the target board.
2. Opens a serial monitor at 115200 baud.
3. Saves the serial output to a log file for analysis.

### Manual Flash and Monitor

```bash
pio run -e esp32s3_unified -t upload --upload-port COMX
pio device monitor --port COMX --baud 115200
```

---

## 4. Verification Checkpoints

Use these checkpoints to verify correct operation after flashing:

### Boot Sequence

- [ ] Serial output shows "SX1280 ready" (LoRa radio initialized)
- [ ] SD card mounts successfully (no mount failure errors)
- [ ] OLED display shows node role and status
- [ ] Node role is correctly assigned (check serial logs)

### LoRa Communication

- [ ] Beacons are transmitted every 30 s (+/- 2 s jitter)
- [ ] Gateway receives beacons from leaf/relay nodes
- [ ] Beacon packets are 15 bytes with magic 0xFC, version 0x02
- [ ] RSSI and SNR values are reasonable (RSSI > -100 dBm)

### WiFi and CoAP

- [ ] Gateway AP is visible with SSID `ForestCam-GW-XXYY`
- [ ] Leaf connects to gateway AP as STA
- [ ] CoAP GET `/info` returns valid JSON with image count
- [ ] CoAP GET `/image/0` returns image data via Block2
- [ ] Fletcher-16 checksum matches between client and server

### Harvest Cycle

- [ ] Gateway discovers leaf via LoRa beacon
- [ ] Gateway connects to leaf WiFi and downloads images
- [ ] Images are saved to `/received/` on gateway SD card
- [ ] Harvest ACK is sent back via LoRa
- [ ] Gateway copies its own `/images/` to `/received/` locally

### Deep Sleep (Leaf/Relay)

- [ ] Node enters deep sleep after 120 s of inactivity
- [ ] Node wakes on timer after 180 s
- [ ] RTC state is restored correctly (role, SSID, boot count)
- [ ] Node announces via CoAP POST `/announce` after wake
- [ ] Sleep is blocked during active harvest or CoAP serving

### Election

- [ ] Election triggers after 90 s without gateway beacon
- [ ] Highest-priority node promotes to acting gateway
- [ ] Lower-priority nodes stand down on receiving SUPPRESS
- [ ] Original gateway reclaims role via GW_RECLAIM broadcast
- [ ] Cooldown of 120 s prevents immediate re-promotion

### Serial Commands

- [ ] `list` command shows all registered nodes
- [ ] `block` command prevents harvesting from a specific node
- [ ] `unblock` command re-enables a blocked node

---

## 5. Test Development

When adding new tests:

1. Create a directory under `test/` with the `test_` prefix (e.g., `test_new_feature/`).
2. Add a `test_main.cpp` file with Unity test framework boilerplate.
3. Use `RUN_TEST()` to register each test case.
4. Tests run on the `native` environment -- mock any hardware dependencies.
5. Build flags for native tests: `-DNATIVE_TEST -std=c++17 -Iinclude`.

### Existing Test Infrastructure

The `test/` directory also contains:
- `README` -- Test directory documentation
- `coap_test.py` -- Python-based CoAP integration test (requires hardware)
