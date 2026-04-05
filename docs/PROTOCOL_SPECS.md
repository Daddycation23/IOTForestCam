# IOTForestCam Protocol Specifications

Complete wire-format specifications for all LoRa and CoAP packets used in the ForestCam mesh network.

---

## 1. Dependencies

| Library                | Version  | Purpose                    |
|------------------------|----------|----------------------------|
| RadioLib               | ^6.6.0   | SX1280 LoRa driver         |
| Adafruit SSD1306       | ^2.5.7   | OLED display driver         |
| Adafruit GFX Library   | ^1.11.5  | Graphics primitives         |

---

## 2. LoRa Radio Parameters

| Parameter        | Value          |
|-----------------|----------------|
| Frequency        | 2400.0 MHz     |
| Spreading Factor | SF9            |
| Bandwidth        | 812.5 kHz      |
| Coding Rate      | 4/7            |
| TX Power         | 10 dBm         |
| Preamble Length  | 12 symbols     |
| Sync Word        | Private (RadioLib default) |
| Max Packet Size  | 64 bytes       |
| SPI Bus          | FSPI (SPI2)    |
| SPI Clock        | 2 MHz          |

---

## 3. Protocol Header

All LoRa packets share a 3-byte header:

| Offset | Size | Field   | Value         |
|--------|------|---------|---------------|
| 0      | 1    | magic   | 0xFC          |
| 1      | 1    | version | 0x01 or 0x02  |
| 2      | 1    | type    | Packet type code |

### Packet Type Codes

| Code | Name          | Version |
|------|---------------|---------|
| 0x01 | BEACON        | 0x02    |
| 0x02 | BEACON_RELAY  | 0x02    |
| 0x10 | RREQ          | 0x01    |
| 0x11 | RREP          | 0x01    |
| 0x12 | RERR          | 0x01    |
| 0x20 | HARVEST_CMD   | 0x01    |
| 0x21 | HARVEST_ACK   | 0x01    |
| 0x30 | ELECTION      | 0x01    |
| 0x31 | SUPPRESS      | 0x01    |
| 0x32 | COORDINATOR   | 0x01    |
| 0x33 | GW_RECLAIM    | 0x01    |
| 0x34 | RELAY_ASSIGN  | 0x02    |
| 0x40 | WAKE_PING     | 0x01    |
| 0x41 | WAKE_BEACON_REQ | 0x01  |

> `RELAY_ASSIGN` (0x34) added by Feature 25 (2026-04-04): RSSI-based relay assignment. See `include/ElectionPacket.h` for the 15-byte `RelayAssignPacket` layout (magic + version + type + gatewayId + relayId).

### Node Role Values

| Value | Role    |
|-------|---------|
| 0x01  | LEAF    |
| 0x02  | RELAY   |
| 0x03  | GATEWAY |

---

## 4. Beacon v2 (15 bytes)

SSID is NOT on the wire in v2. It is derived from the MAC address after parsing:
- Gateway: `ForestCam-GW-XXYY`
- Leaf/Relay: `ForestCam-XXYY`

| Offset | Size | Field       | Description                      |
|--------|------|-------------|----------------------------------|
| 0      | 1    | magic       | 0xFC                             |
| 1      | 1    | version     | 0x02                             |
| 2      | 1    | packetType  | 0x01=BEACON, 0x02=RELAYED        |
| 3      | 1    | ttl         | Starts at 2, decremented by relay|
| 4      | 6    | nodeId      | WiFi MAC address                 |
| 10     | 1    | nodeRole    | 0x01=LEAF, 0x02=RELAY, 0x03=GW  |
| 11     | 1    | imageCount  | Number of images available       |
| 12     | 1    | batteryPct  | 0-100 or 0xFF=USB/unknown        |
| 13     | 2    | uptimeMin   | uint16 little-endian, minutes    |

**Total**: 15 bytes fixed. Min size for v2: 15 bytes. Min size for v1 (backwards compat): 16 bytes. Max buffer: 48 bytes.

---

## 5. AODV RREQ -- Route Request (31 bytes)

| Offset | Size | Field       | Description                      |
|--------|------|-------------|----------------------------------|
| 0      | 1    | magic       | 0xFC                             |
| 1      | 1    | version     | 0x01                             |
| 2      | 1    | type        | 0x10                             |
| 3      | 1    | flags       | G=0x01 (gratuitous), D=0x02 (dest only) |
| 4      | 1    | hopCount    | Incremented at each hop          |
| 5      | 4    | rreqId      | uint32 LE, unique request ID     |
| 9      | 6    | destId      | Destination MAC address          |
| 15     | 2    | destSeqNum  | uint16 LE                        |
| 17     | 6    | origId      | Originator MAC address           |
| 23     | 2    | origSeqNum  | uint16 LE                        |
| 25     | 6    | prevHopId   | Overwritten by each forwarder    |

**Total**: 31 bytes

---

## 6. AODV RREP -- Route Reply (27 bytes)

| Offset | Size | Field       | Description                      |
|--------|------|-------------|----------------------------------|
| 0      | 1    | magic       | 0xFC                             |
| 1      | 1    | version     | 0x01                             |
| 2      | 1    | type        | 0x11                             |
| 3      | 1    | flags       | A=0x01 (ACK required)            |
| 4      | 1    | hopCount    | Hops from destination            |
| 5      | 6    | destId      | Destination MAC address          |
| 11     | 2    | destSeqNum  | uint16 LE                        |
| 13     | 6    | origId      | Originator MAC address           |
| 19     | 2    | lifetime    | uint16 LE, seconds               |
| 21     | 6    | prevHopId   | Overwritten by each forwarder    |

**Total**: 27 bytes

---

## 7. AODV RERR -- Route Error (4 + 8N bytes)

| Offset | Size | Field       | Description                      |
|--------|------|-------------|----------------------------------|
| 0      | 1    | magic       | 0xFC                             |
| 1      | 1    | version     | 0x01                             |
| 2      | 1    | type        | 0x12                             |
| 3      | 1    | destCount   | 1..6 (max RERR_MAX_DESTS)        |
| 4      | 8*N  | entries     | N x (destId[6] + destSeqNum[2])  |

**Total**: 4 + 8*N bytes (max 52 bytes when N=6)

---

## 8. HARVEST_CMD -- Gateway Tells Relay to Fetch (17 + N bytes)

| Offset | Size | Field         | Description                    |
|--------|------|---------------|--------------------------------|
| 0      | 1    | magic         | 0xFC                           |
| 1      | 1    | version       | 0x01                           |
| 2      | 1    | type          | 0x20                           |
| 3      | 1    | cmdId         | Unique ID for ACK correlation  |
| 4      | 6    | relayId       | Intended relay MAC             |
| 10     | 6    | targetLeafId  | Leaf MAC to harvest            |
| 16     | 1    | ssidLen       | Length of SSID string          |
| 17     | N    | ssid          | Target leaf SSID (max 20 chars)|

**Total**: 17 + N bytes (max 37 bytes)

---

## 9. HARVEST_ACK -- Relay Acknowledges Harvest (16 bytes)

| Offset | Size | Field       | Description                      |
|--------|------|-------------|----------------------------------|
| 0      | 1    | magic       | 0xFC                             |
| 1      | 1    | version     | 0x01                             |
| 2      | 1    | type        | 0x21                             |
| 3      | 1    | cmdId       | Echoed from HARVEST_CMD          |
| 4      | 6    | relayId     | Relay's own MAC                  |
| 10     | 1    | status      | 0x00=OK, 0x01=wifi_fail, 0x02=coap_fail, 0x03=sd_fail |
| 11     | 1    | imageCount  | Images cached                    |
| 12     | 4    | totalBytes  | uint32 LE                        |

**Total**: 16 bytes

---

## 10. Election Packet (11 bytes)

All four election types share the same wire format:

| Offset | Size | Field       | Description                      |
|--------|------|-------------|----------------------------------|
| 0      | 1    | magic       | 0xFC                             |
| 1      | 1    | version     | 0x01                             |
| 2      | 1    | type        | 0x30=ELECTION, 0x31=SUPPRESS, 0x32=COORDINATOR, 0x33=GW_RECLAIM |
| 3      | 6    | senderId    | Sender's MAC address             |
| 9      | 2    | electionId  | uint16 LE, monotonic counter     |

**Total**: 11 bytes

Priority is computed from senderId via `macToPriority()` -- uses last 4 bytes of MAC as uint32 (little-endian). No wire field needed.

### Election Flow

1. **ELECTION (0x30)**: Node broadcasts to start election.
2. **SUPPRESS (0x31)**: Higher-priority node tells lower-priority nodes to stand down.
3. **COORDINATOR (0x32)**: Winner announces it is the new gateway.
4. **GW_RECLAIM (0x33)**: Original gateway returns and reclaims the role.

---

## 11. CoAP Protocol

### Server Endpoints (Leaf/Relay, port 5683)

| Method | Path                 | Content Format      | Description                    |
|--------|----------------------|--------------------|---------------------------------|
| GET    | `/image/{index}`     | octet-stream (42)  | Image data via Block2 transfer  |
| GET    | `/info`              | JSON (50)          | Image catalogue                 |
| GET    | `/checksum/{index}`  | JSON (50)          | Fletcher-16 checksum            |
| GET    | `/.well-known/core`  | link-format (40)   | CoRE resource discovery (RFC 6690) |
| POST   | `/announce`          | octet-stream (42)  | Leaf-initiated harvest announce |

### Block2 Transfer (RFC 7959)

| Parameter        | Value              |
|-----------------|---------------------|
| Block SZX        | 6                  |
| Block size       | 2^(6+4) = 1024 bytes |
| Max PDU size     | 1280 bytes         |
| Max options      | 16                 |
| Max option length| 64 bytes           |
| Max token length | 8 bytes            |

### Block2 Option Encoding

```
value = (NUM << 4) | (M << 3) | SZX
  NUM = Block number
  M   = More blocks follow (1 bit)
  SZX = Size exponent (block_size = 2^(SZX+4))
```

### Pipelined Transfer

| Parameter        | Value              |
|-----------------|---------------------|
| Window size      | 3 outstanding requests |
| Timeout          | 2000 ms per request|
| Max retries      | 3                  |
| Checksum         | Fletcher-16        |

Falls back to sequential transfer if pipelining causes errors.

### CoAP Message Format (RFC 7252)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Ver| T |  TKL  |      Code     |          Message ID           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Token (if any, TKL bytes) ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   Options (if any) ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|1 1 1 1 1 1 1 1|    Payload (if any) ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **Ver**: 1 (CoAP version)
- **T**: CON=0, NON=1, ACK=2, RST=3
- **TKL**: Token length (0-8)
- **Code**: (class * 32 + detail), displayed as class.detail

### Announce Payload (7 bytes)

Sent by leaf nodes via CoAP POST `/announce` to notify the gateway of available images:

| Offset | Size | Field       | Description              |
|--------|------|-------------|--------------------------|
| 0      | 6    | MAC address | Leaf's WiFi MAC          |
| 6      | 1    | imageCount  | Number of images to harvest |

---

## 12. WiFi Configuration

| Parameter          | Value                              |
|-------------------|------------------------------------|
| Gateway AP SSID    | `ForestCam-GW-XXYY` (from MAC)   |
| Leaf/Relay AP SSID | `ForestCam-XXYY` (from MAC)      |
| SSID Prefix        | `ForestCam-`                      |
| AP Default IP      | 192.168.4.1                       |

Leaf and relay nodes connect as STA to the gateway's AP. The gateway runs softAP mode permanently.

---

## 13. FreeRTOS Parameters

| Parameter                  | Value       |
|---------------------------|-------------|
| Core 0 (Protocol)         | LoRa, AODV, Election |
| Core 1 (Network)          | WiFi, CoAP, Harvest  |
| LoRa task priority        | 3           |
| Harvest task priority     | 2           |
| CoAP server priority      | 2           |
| OLED task priority        | 1           |
| LoRa stack                | 4096 words  |
| Harvest stack             | 8192 words  |
| CoAP server stack         | 6144 words  |
| OLED stack                | 2048 words  |
| LoRa TX queue             | 8 items     |
| Harvest cmd queue         | 2 items     |
| Relay harvest queue       | 2 items     |
| Announce queue            | 8 items     |
| Mutex timeout             | 1000 ms     |
