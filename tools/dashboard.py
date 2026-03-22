#!/usr/bin/env python3
"""
IOT Forest Cam — Live Harvest Dashboard

Reads the gateway's serial output and displays a real-time terminal
dashboard showing node discovery, harvest progress, and image transfers.

Usage:
  python dashboard.py                          # auto-detect serial port
  python dashboard.py --port /dev/cu.usbmodem101
  python dashboard.py --port COM9              # Windows

Requirements:
  pip install pyserial

Author: CS Group 2
"""

import argparse
import re
import sys
import time
from datetime import datetime

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Install pyserial:  pip install pyserial")
    sys.exit(1)


# ─── ANSI Colors ──────────────────────────────────────────────

RESET  = "\033[0m"
BOLD   = "\033[1m"
DIM    = "\033[2m"
RED    = "\033[31m"
GREEN  = "\033[32m"
YELLOW = "\033[33m"
BLUE   = "\033[34m"
CYAN   = "\033[36m"
WHITE  = "\033[37m"
BG_BLUE = "\033[44m"


def clear_screen():
    print("\033[2J\033[H", end="")


# ─── State ────────────────────────────────────────────────────

class Node:
    def __init__(self, node_id: str):
        self.node_id = node_id
        self.ssid = ""
        self.role = ""
        self.images = 0
        self.rssi = 0
        self.last_seen = time.time()
        self.status = "discovered"  # discovered, harvesting, done, failed


class HarvestState:
    def __init__(self):
        self.phase = "IDLE"
        self.target_node = ""
        self.target_ssid = ""
        self.current_image = 0
        self.total_images = 0
        self.current_block = 0
        self.total_blocks = 0
        self.bytes_transferred = 0
        self.images_completed = 0
        self.images_failed = 0
        self.start_time = None
        self.last_filename = ""


class Dashboard:
    def __init__(self):
        self.nodes: dict[str, Node] = {}
        self.harvest = HarvestState()
        self.boot_count = 0
        self.role = "?"
        self.lora_ok = False
        self.sd_ok = False
        self.wifi_ssid = ""
        self.uptime_s = 0
        self.last_beacon_tx = ""
        self.election_state = ""
        self.log_lines: list[str] = []
        self.max_log_lines = 15
        self.total_harvested = 0

    def add_log(self, line: str):
        timestamp = datetime.now().strftime("%H:%M:%S")
        self.log_lines.append(f"{DIM}{timestamp}{RESET} {line}")
        if len(self.log_lines) > self.max_log_lines:
            self.log_lines.pop(0)


# ─── Serial Line Parsers ─────────────────────────────────────

def parse_line(line: str, dash: Dashboard):
    """Parse a serial log line and update dashboard state."""

    # Boot info
    m = re.search(r'\[Boot\] Reset reason: .+ \(boot #(\d+)\)', line)
    if m:
        dash.boot_count = int(m.group(1))
        dash.add_log(f"{GREEN}Boot #{dash.boot_count}{RESET}")

    # Role
    m = re.search(r'\[Role\] Booting as: (\w+)', line)
    if m:
        dash.role = m.group(1)
        dash.add_log(f"Role: {BOLD}{dash.role}{RESET}")

    # SD card
    if "SD Card OK" in line or "SD mounted" in line:
        dash.sd_ok = True
    if "SD mount FAILED" in line or "SD card mount failed" in line:
        dash.sd_ok = False
        dash.add_log(f"{RED}SD card mount failed{RESET}")

    # LoRa init
    if "SX1280 ready" in line:
        dash.lora_ok = True
        dash.add_log(f"{GREEN}LoRa SX1280 initialized{RESET}")
    if "SX1280" in line and "failed" in line.lower():
        dash.lora_ok = False
        dash.add_log(f"{RED}LoRa init failed{RESET}")

    # WiFi AP
    m = re.search(r'\[OK\] WiFi AP: (\S+)', line)
    if m:
        dash.wifi_ssid = m.group(1)

    # Election
    if "[Election]" in line:
        m = re.search(r'\[Election\] -> (\w+)', line)
        if m:
            dash.election_state = m.group(1)
            dash.add_log(f"{YELLOW}Election: {m.group(1)}{RESET}")
        if "PROMOTED" in line and "GATEWAY" in line:
            dash.role = "ACTING_GW"
            dash.add_log(f"{GREEN}{BOLD}Promoted to ACTING GATEWAY{RESET}")

    # Node discovery via beacon
    m = re.search(r'\[Registry\] (?:New|Updated) node (\S+).+ssid=(\S+).+images=(\d+).+rssi=(-?\d+)', line)
    if m:
        nid = m.group(1)
        node = dash.nodes.get(nid, Node(nid))
        node.ssid = m.group(2)
        node.images = int(m.group(3))
        node.rssi = int(m.group(4))
        node.last_seen = time.time()
        if node.status not in ("harvesting",):
            node.status = "discovered"
        dash.nodes[nid] = node
        if "New node" in line:
            dash.add_log(f"{CYAN}New node: {nid} ({node.ssid}, {node.images} imgs){RESET}")

    # Beacon RX (alternative format)
    m = re.search(r'\[LoRa\] Beacon RX.+from (\S+).+RSSI (-?\d+)', line)
    if m:
        nid = m.group(1)
        if nid in dash.nodes:
            dash.nodes[nid].rssi = int(m.group(2))
            dash.nodes[nid].last_seen = time.time()

    # Beacon TX
    if "[LoRa] Beacon TX" in line:
        dash.last_beacon_tx = datetime.now().strftime("%H:%M:%S")

    # ── Harvest state machine ──

    # Harvest start
    m = re.search(r'Harvesting node (\S+)', line)
    if not m:
        m = re.search(r'harvest.+target.+?(\w{2}:\w{2}:\w{2}:\w{2}:\w{2}:\w{2})', line, re.I)
    if m:
        nid = m.group(1)
        dash.harvest.target_node = nid
        dash.harvest.phase = "STARTING"
        dash.harvest.start_time = time.time()
        dash.harvest.current_image = 0
        dash.harvest.bytes_transferred = 0
        if nid in dash.nodes:
            dash.nodes[nid].status = "harvesting"
            dash.harvest.target_ssid = dash.nodes[nid].ssid
        dash.add_log(f"{BLUE}Harvest starting: {nid}{RESET}")

    # Harvest phase transitions
    m = re.search(r'\[Harvest\] (?:Phase|State|->)\s*(\w+)', line)
    if m:
        dash.harvest.phase = m.group(1).upper()

    # WiFi connecting
    if "Connecting to" in line and "WiFi" in line:
        dash.harvest.phase = "WIFI_CONNECT"
        m = re.search(r'Connecting to (\S+)', line)
        if m:
            dash.harvest.target_ssid = m.group(1)

    if "WiFi connected" in line or "WiFi STA connected" in line:
        dash.harvest.phase = "CONNECTED"
        dash.add_log(f"{GREEN}WiFi connected to {dash.harvest.target_ssid}{RESET}")

    # CoAP download progress
    m = re.search(r'Block\s+(\d+)/(\d+)', line)
    if m:
        dash.harvest.current_block = int(m.group(1))
        dash.harvest.total_blocks = int(m.group(2))
        dash.harvest.phase = "DOWNLOADING"

    # Image download info
    m = re.search(r'Downloading image (\d+)/(\d+)', line)
    if not m:
        m = re.search(r'image\[(\d+)\].*?(\d+) images', line, re.I)
    if m:
        dash.harvest.current_image = int(m.group(1))
        dash.harvest.total_images = int(m.group(2))
        dash.harvest.current_block = 0
        dash.harvest.phase = "DOWNLOADING"

    # Bytes/file saved
    m = re.search(r'Saved.+?(\d+) bytes.+?(\S+\.jpg)', line, re.I)
    if m:
        dash.harvest.bytes_transferred += int(m.group(1))
        dash.harvest.last_filename = m.group(2)
        dash.harvest.images_completed += 1
        dash.add_log(f"{GREEN}Saved: {m.group(2)} ({int(m.group(1)):,}B){RESET}")

    # Image saved (alternative)
    m = re.search(r'Image saved.*?(\S+\.jpg)', line, re.I)
    if m:
        dash.harvest.last_filename = m.group(1)
        dash.harvest.images_completed += 1

    # Checksum verification
    if "checksum" in line.lower() and ("match" in line.lower() or "verified" in line.lower()):
        dash.add_log(f"{GREEN}Checksum verified{RESET}")
    if "checksum" in line.lower() and "mismatch" in line.lower():
        dash.add_log(f"{RED}Checksum MISMATCH{RESET}")
        dash.harvest.images_failed += 1

    # Harvest complete
    if "Harvest complete" in line or "harvest done" in line.lower():
        dash.harvest.phase = "DONE"
        nid = dash.harvest.target_node
        if nid in dash.nodes:
            dash.nodes[nid].status = "done"
        dash.total_harvested += dash.harvest.images_completed
        dash.add_log(f"{GREEN}{BOLD}Harvest complete: {dash.harvest.images_completed} images{RESET}")

    # Harvest failed
    if "harvest" in line.lower() and "fail" in line.lower():
        if dash.harvest.phase not in ("IDLE", "DONE"):
            dash.harvest.phase = "FAILED"
            nid = dash.harvest.target_node
            if nid in dash.nodes:
                dash.nodes[nid].status = "failed"
            dash.add_log(f"{RED}Harvest failed: {nid}{RESET}")

    # Deep sleep
    if "Entering deep sleep" in line:
        dash.add_log(f"{DIM}Node entering deep sleep{RESET}")

    # Wake
    m = re.search(r'Wakeup: (.+)', line)
    if m:
        dash.add_log(f"{YELLOW}Wakeup: {m.group(1)}{RESET}")

    # Self-copy
    if "self-copy" in line.lower() or "SELF" in line:
        m = re.search(r'Copied.*?(\S+\.jpg)', line)
        if m:
            dash.add_log(f"{BLUE}Self-copy: {m.group(1)}{RESET}")


# ─── Rendering ────────────────────────────────────────────────

def render(dash: Dashboard):
    """Render the dashboard to the terminal."""
    clear_screen()

    # Header
    print(f"{BG_BLUE}{WHITE}{BOLD}  IOT Forest Cam — Live Dashboard  {RESET}")
    print()

    # Status bar
    role_color = GREEN if "GATEWAY" in dash.role.upper() else CYAN
    sd_status = f"{GREEN}OK{RESET}" if dash.sd_ok else f"{RED}FAIL{RESET}"
    lora_status = f"{GREEN}OK{RESET}" if dash.lora_ok else f"{RED}FAIL{RESET}"

    print(f"  Role: {role_color}{BOLD}{dash.role}{RESET}  |  "
          f"Boot: #{dash.boot_count}  |  "
          f"SD: {sd_status}  |  "
          f"LoRa: {lora_status}  |  "
          f"SSID: {dash.wifi_ssid}  |  "
          f"Total harvested: {dash.total_harvested}")
    print()

    # ── Discovered Nodes ──
    print(f"  {BOLD}Discovered Nodes{RESET}")
    print(f"  {'ID':<20} {'SSID':<20} {'Images':>6} {'RSSI':>6} {'Status':<12} {'Last Seen':<10}")
    print(f"  {'-'*20} {'-'*20} {'-'*6} {'-'*6} {'-'*12} {'-'*10}")

    if not dash.nodes:
        print(f"  {DIM}(no nodes discovered yet){RESET}")
    else:
        for nid, node in sorted(dash.nodes.items()):
            age = time.time() - node.last_seen
            age_str = f"{int(age)}s ago" if age < 120 else f"{int(age/60)}m ago"

            status_colors = {
                "discovered": CYAN,
                "harvesting": YELLOW,
                "done": GREEN,
                "failed": RED,
            }
            sc = status_colors.get(node.status, WHITE)

            print(f"  {node.node_id:<20} {node.ssid:<20} {node.images:>6} "
                  f"{node.rssi:>4}dBm {sc}{node.status:<12}{RESET} {age_str:<10}")

    print()

    # ── Harvest Progress ──
    h = dash.harvest
    print(f"  {BOLD}Harvest Status{RESET}")

    phase_colors = {
        "IDLE": DIM,
        "STARTING": YELLOW,
        "WIFI_CONNECT": YELLOW,
        "CONNECTED": CYAN,
        "DOWNLOADING": BLUE,
        "DONE": GREEN,
        "FAILED": RED,
    }
    pc = phase_colors.get(h.phase, WHITE)

    print(f"  Phase: {pc}{BOLD}{h.phase}{RESET}", end="")
    if h.target_node:
        print(f"  |  Target: {h.target_node}", end="")
    if h.target_ssid:
        print(f" ({h.target_ssid})", end="")
    print()

    if h.phase == "DOWNLOADING" and h.total_blocks > 0:
        pct = (h.current_block / h.total_blocks) * 100
        bar_width = 30
        filled = int(bar_width * h.current_block / h.total_blocks)
        bar = f"{'█' * filled}{'░' * (bar_width - filled)}"

        print(f"  Image: {h.current_image}/{h.total_images}  "
              f"Block: {h.current_block}/{h.total_blocks}")
        print(f"  [{bar}] {pct:.0f}%")

    if h.bytes_transferred > 0:
        kb = h.bytes_transferred / 1024
        print(f"  Transferred: {kb:.1f} KB  |  "
              f"Images: {h.images_completed} OK, {h.images_failed} failed")

    if h.last_filename:
        print(f"  Last file: {h.last_filename}")

    if h.start_time and h.phase not in ("IDLE", "DONE"):
        elapsed = time.time() - h.start_time
        print(f"  Elapsed: {int(elapsed)}s")

    print()

    # ── Log ──
    print(f"  {BOLD}Event Log{RESET}")
    if not dash.log_lines:
        print(f"  {DIM}(waiting for events...){RESET}")
    else:
        for line in dash.log_lines:
            print(f"  {line}")

    print()
    print(f"  {DIM}Press Ctrl+C to exit{RESET}")


# ─── Serial Port Detection ────────────────────────────────────

def find_port() -> str:
    """Auto-detect the ESP32 serial port."""
    ports = serial.tools.list_ports.comports()
    for p in ports:
        desc = (p.description or "").lower()
        if any(kw in desc for kw in ("usb", "uart", "serial", "esp", "cp210", "ch340")):
            return p.device
        # macOS: cu.usbmodem or cu.usbserial
        if "usbmodem" in p.device or "usbserial" in p.device:
            return p.device
    # Fallback: return first port
    if ports:
        return ports[0].device
    return None


# ─── Main ─────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="IOT Forest Cam — Live Harvest Dashboard"
    )
    parser.add_argument("--port", "-p", default=None,
                        help="Serial port (default: auto-detect)")
    parser.add_argument("--baud", "-b", type=int, default=115200,
                        help="Baud rate (default: 115200)")
    parser.add_argument("--raw", action="store_true",
                        help="Also print raw serial lines to stderr")
    args = parser.parse_args()

    port = args.port or find_port()
    if not port:
        print("Error: No serial port found. Connect the ESP32 and retry.")
        print("       Or specify manually: --port /dev/cu.usbmodem101")
        return 1

    print(f"Connecting to {port} at {args.baud} baud...")

    try:
        ser = serial.Serial(port, args.baud, timeout=0.1)
    except serial.SerialException as e:
        print(f"Error: {e}")
        print("Is another program (pio monitor) using the port? Close it first.")
        return 1

    dash = Dashboard()
    last_render = 0
    render_interval = 0.5  # seconds

    print("Connected. Waiting for data...\n")

    try:
        while True:
            # Read available lines
            while ser.in_waiting:
                try:
                    raw = ser.readline()
                    line = raw.decode("utf-8", errors="replace").strip()
                except Exception:
                    continue

                if not line:
                    continue

                if args.raw:
                    print(line, file=sys.stderr)

                parse_line(line, dash)

            # Render periodically
            now = time.time()
            if now - last_render >= render_interval:
                render(dash)
                last_render = now

            time.sleep(0.05)

    except KeyboardInterrupt:
        print("\n\nDashboard stopped.")
    finally:
        ser.close()

    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
