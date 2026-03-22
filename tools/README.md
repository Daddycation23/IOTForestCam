# IOT Forest Cam — Tools

Python utilities for monitoring and downloading images from the mesh network.

## Prerequisites

```bash
pip install pyserial aiocoap
```

- **Python 3.8+** required
- **macOS / Linux / Windows** supported

---

## dashboard.py — Live Harvest Dashboard

Real-time terminal dashboard that reads the gateway's serial output and displays:

- **Node discovery** — all leaf/relay nodes found via LoRa beacons
- **Harvest progress** — block-by-block download progress bar
- **Transfer status** — images completed, bytes transferred, checksums
- **Event log** — timestamped feed of key events

No ESP32 code changes needed — it parses the existing serial log output.

### Usage

```bash
# Close pio monitor first (only one program can use the serial port at a time)

# Auto-detect serial port
python tools/dashboard.py

# Specify port manually
python tools/dashboard.py --port /dev/cu.usbmodem101    # macOS
python tools/dashboard.py --port COM9                    # Windows

# Also print raw serial lines to stderr (for debugging)
python tools/dashboard.py --raw
```

### What You'll See

```
  IOT Forest Cam — Live Dashboard

  Role: ACTING_GW  |  Boot: #3  |  SD: OK  |  LoRa: OK  |  Total harvested: 5

  Discovered Nodes
  ID                   SSID                 Images   RSSI Status       Last Seen
  -------------------- -------------------- ------ ------ ------------ ----------
  DC:54:75:E4:3A:10    ForestCam-3A10            3  -62dBm harvesting   2s ago
  DC:54:75:E4:5B:44    ForestCam-5B44            2  -78dBm done         45s ago

  Harvest Status
  Phase: DOWNLOADING  |  Target: DC:54:75:E4:3A:10 (ForestCam-3A10)
  Image: 2/3  Block: 34/89
  [███████████████░░░░░░░░░░░░░░░] 38%
  Transferred: 45.2 KB  |  Images: 1 OK, 0 failed

  Event Log
  14:23:01 New node: DC:54:75:E4:3A:10 (ForestCam-3A10, 3 imgs)
  14:23:15 Harvest starting: DC:54:75:E4:3A:10
  14:23:18 WiFi connected to ForestCam-3A10
  14:23:22 Saved: node_3A10_boot001_000123s_img_000.jpg (34,210B)
```

### Options

| Flag | Default | Description |
|------|---------|-------------|
| `--port`, `-p` | auto-detect | Serial port path |
| `--baud`, `-b` | 115200 | Baud rate |
| `--raw` | off | Print raw serial lines to stderr |

---

## viewer.py — Image Viewer / Downloader

Download and browse images from the mesh network. Two modes:

### Mode 1: CoAP — Download from a leaf node over WiFi

Connect your computer to the leaf node's WiFi AP, then pull images via CoAP.

```bash
# 1. Connect to the node's WiFi:
#    SSID: ForestCam-XXXX   Password: forestcam123

# 2. List available images
python tools/viewer.py coap --list

# 3. Download all images
python tools/viewer.py coap

# 4. Download a specific image by index
python tools/viewer.py coap -i 0

# 5. Custom output directory
python tools/viewer.py coap -o ./my_images
```

Downloaded images are saved to `./downloaded_images/` by default and the folder opens automatically.

### Mode 2: SD — Browse gateway's harvested images

Plug the gateway's SD card into your computer and browse the `/received/` folder.

```bash
# macOS (SD card mounts as a volume)
python tools/viewer.py sd --path /Volumes/SD_CARD/received

# List only (don't open Finder)
python tools/viewer.py sd --path /Volumes/SD_CARD/received --list

# Copy images to a local folder
python tools/viewer.py sd --path /Volumes/SD_CARD/received -o ./local_copy
```

### Options

**CoAP mode:**

| Flag | Default | Description |
|------|---------|-------------|
| `--host` | 192.168.4.1 | Node IP address |
| `--index`, `-i` | all | Download only this image index |
| `--output`, `-o` | ./downloaded_images | Output directory |
| `--list`, `-l` | off | List images without downloading |
| `--no-open` | off | Don't open folder after download |

**SD mode:**

| Flag | Default | Description |
|------|---------|-------------|
| `--path`, `-p` | (required) | Path to SD card's /received directory |
| `--output`, `-o` | (none) | Copy images to this directory |
| `--list`, `-l` | off | List images without opening |
| `--no-open` | off | Don't open folder |

---

## Architecture Reference

```
Gateway (ESP32)                         Leaf Node (ESP32)
  │                                         │
  │◄──── LoRa beacon (node discovery) ──────│
  │                                         │
  │──── LoRa WAKE_PING ───────────────────►│  (wakes from deep sleep)
  │──── LoRa HARVEST_CMD ────────────────►│
  │                                         │
  │◄──── WiFi AP (ForestCam-XXXX) ─────────│  (leaf starts WiFi AP)
  │                                         │
  │──── CoAP GET /info ───────────────────►│  (catalogue)
  │──── CoAP GET /image/0 (Block2) ──────►│  (1024B blocks)
  │──── CoAP GET /checksum/0 ─────────────►│  (Fletcher-16)
  │                                         │
  ├─ Saves to SD: /received/               │
  │                                         │
  ▼                                         ▼
dashboard.py ◄── Serial ──── Gateway    viewer.py ◄── WiFi ──── Leaf
(live monitoring)                        (image download)
```
