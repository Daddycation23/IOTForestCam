# CoAP Pipelined Block Transfer — Setup & Testing

## Overview

This feature optimizes CoAP Block2 image downloads with two changes:

1. **Block size doubled**: 512B (SZX=5) → 1024B (SZX=6) — halves round-trips
2. **Pipelined requests**: Sliding window of 3 outstanding block requests — overlaps server processing with client SD writes
3. **Reduced timeout**: 5000ms → 2000ms — faster retry on local WiFi

Expected throughput improvement: **2-3x faster** image downloads.

---

## What Changed

| Parameter | Before | After |
|-----------|--------|-------|
| Block size | 512 bytes (SZX=5) | 1024 bytes (SZX=6) |
| PDU buffer | 768 bytes | 1280 bytes |
| Timeout | 5000 ms | 2000 ms |
| Pipeline window | 1 (sequential) | 3 outstanding |
| Download method | `downloadImage()` | `downloadImagePipelined()` |

---

## How Pipelining Works

**Before (sequential):**
```
Client          Server
  |-- GET blk 0 -->|
  |<-- ACK blk 0 --|   (wait for response)
  |-- GET blk 1 -->|
  |<-- ACK blk 1 --|   (wait for response)
  ...
```

**After (pipelined, window=3):**
```
Client          Server
  |-- GET blk 0 -->|
  |-- GET blk 1 -->|   (send 3 immediately)
  |-- GET blk 2 -->|
  |<-- ACK blk 0 --|   (receive, write, send blk 3)
  |-- GET blk 3 -->|
  |<-- ACK blk 1 --|   (receive, write, send blk 4)
  ...
```

The client maintains a reorder buffer to handle out-of-order responses and ensures checksums are computed in block order.

---

## Testing

### Unit Tests (no hardware)

```bash
pio test -e native -f test_coap_block
```

Tests validate:
- Block2 encode/decode round-trips at SZX=6
- Block count calculations for 1024-byte blocks
- Pipeline window logic (burst, receive, completion)
- Fletcher-16 checksum incremental computation

### Hardware Test (3 boards)

1. Flash all boards: `pio run -e esp32s3_unified -t upload`
2. Assign roles: 1 Gateway, 1 Leaf, 1 Relay
3. Watch gateway serial for throughput numbers:

```
[CoapClient] Pipelined download — image/0, window=3
[CoapClient] Image 0 — 51200 bytes, ~50 blocks
  Block 0 — 1024 bytes (2.0%)
  Block 49 — 51200 bytes (100.0%)
[CoapClient] Pipelined download complete — 51200 bytes, 50 blocks, 8500 ms, 5.9 KB/s
```

### What to Measure

| Metric | Before (est.) | Target |
|--------|--------------|--------|
| Throughput (KB/s) | ~1-2 KB/s | 3-6 KB/s |
| Blocks per image (50KB) | 100 | 50 |
| Time per 50KB image | 30-60s | 10-20s |

### Verifying Correctness

After download, the gateway verifies the Fletcher-16 checksum against the leaf's `/checksum/{n}` endpoint. Watch for:
```
[CoapClient] Checksum verify — local=12345, server=12345 → PASS
```

If checksum fails, the pipelined reorder buffer may have a bug — check that blocks are being written in order.

---

## Fallback

The original sequential `downloadImage()` method is still available. If pipelining causes issues on specific hardware, switch back by changing the call in `HarvestLoop.cpp` and `main.cpp` (relay harvest).

---

## Files Changed

| File | Change |
|------|--------|
| `include/CoapMessage.h` | `COAP_BLOCK_SZX` 5→6, `COAP_MAX_PDU_SIZE` 768→1280 |
| `include/StorageReader.h` | `VSENSOR_BLOCK_SIZE` 512→1024 |
| `include/CoapClient.h` | Added `COAP_CLIENT_WINDOW_SIZE=3`, `downloadImagePipelined()`, reduced timeout |
| `src/CoapClient.cpp` | Implemented pipelined Block2 download with reorder buffer |
| `src/CoapServer.cpp` | Fixed stale SZX=5 comment |
| `src/HarvestLoop.cpp` | Calls `downloadImagePipelined()` instead of `downloadImage()` |
| `src/main.cpp` | Relay harvest uses `downloadImagePipelined()` |
| `test/test_coap_block/` | NEW — 15 unit tests for Block2, pipeline, and checksum logic |
