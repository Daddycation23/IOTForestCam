#!/usr/bin/env python3
"""
IOT Forest Cam — Packet Loss Analyzer

Captures [STATS] serial output from all connected nodes simultaneously
for a fixed duration (default 3 minutes), then computes mesh-wide
packet-loss statistics.

How loss rate is computed:
  Beacons are broadcast — every beacon sent by one node SHOULD be heard
  by all (N-1) other nodes in the mesh. So:
      expected_rx_total = sum(beacon_tx) * (N - 1)
      actual_rx_total   = sum(beacon_rx)
      loss_rate         = 1 - actual_rx_total / expected_rx_total

  The same formula applies to LoRa-layer TX/RX for the broadcast
  control plane (beacons + RREQ floods + RREP forwards).

Usage:
  python packet_loss_analyzer.py                    # auto-detect ports, 180s
  python packet_loss_analyzer.py --duration 120     # 2 minutes
  python packet_loss_analyzer.py --ports COM9 COM10 COM12 COM13

Output:
  Saves a timestamped report to tools/reports/ and prints summary
  to stdout. Report includes per-node deltas and mesh-wide loss rates.
"""

import argparse
import re
import sys
import time
import threading
from datetime import datetime
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Install pyserial:  pip install pyserial")
    sys.exit(1)


# ─── Regex patterns for [STATS] lines ───────────────────────

RE_LORA    = re.compile(r'\[STATS\] LoRa tx=(\d+) rx=(\d+) err=(\d+)')
RE_AODV    = re.compile(r'\[STATS\] AODV rreqT=(\d+) rreqR=(\d+) rrepT=(\d+) rrepR=(\d+) rerrT=(\d+) rerrR=(\d+)')
RE_BEACON  = re.compile(r'\[STATS\] Beacon tx=(\d+) rx=(\d+)')
RE_COAP    = re.compile(r'\[STATS\] CoAP reqs=(\d+) blocks=(\d+)')
RE_HARVEST = re.compile(r'\[STATS\] Harvest cycles=(\d+) imgs=(\d+) bytes=(\d+) ok=(\d+) fail=(\d+)')
RE_MAC     = re.compile(r'Initialized, nodeId=([0-9A-Fa-f:]{17})')
RE_ROLE    = re.compile(r'Role changed:\s*\w+\s*->\s*(\w+)|PROMOTED TO ACTING GATEWAY|RSSI-ASSIGNED TO RELAY')


class NodeStats:
    def __init__(self, port):
        self.port = port
        self.mac = "?"
        self.role = "LEAF"
        # Initial snapshot (first [STATS] seen)
        self.lora_tx_start   = None
        self.lora_rx_start   = None
        self.beacon_tx_start = None
        self.beacon_rx_start = None
        self.rreq_t_start    = None
        self.rreq_r_start    = None
        self.rrep_t_start    = None
        self.rrep_r_start    = None
        # Final snapshot (last [STATS] seen)
        self.lora_tx   = 0
        self.lora_rx   = 0
        self.lora_err  = 0
        self.beacon_tx = 0
        self.beacon_rx = 0
        self.rreq_t    = 0
        self.rreq_r    = 0
        self.rrep_t    = 0
        self.rrep_r    = 0
        self.rerr_t    = 0
        self.rerr_r    = 0
        # Harvest / CoAP (gateway or leaf depending on role)
        self.harvest_cycles = 0
        self.harvest_imgs   = 0
        self.harvest_bytes  = 0
        self.coap_reqs      = 0
        self.coap_blocks    = 0
        self.sample_count   = 0

    def _snap_start(self):
        """Record first-seen values as baseline for deltas."""
        self.lora_tx_start   = self.lora_tx
        self.lora_rx_start   = self.lora_rx
        self.beacon_tx_start = self.beacon_tx
        self.beacon_rx_start = self.beacon_rx
        self.rreq_t_start    = self.rreq_t
        self.rreq_r_start    = self.rreq_r
        self.rrep_t_start    = self.rrep_t
        self.rrep_r_start    = self.rrep_r

    def update(self, line: str):
        m = RE_LORA.search(line)
        if m:
            self.lora_tx  = int(m.group(1))
            self.lora_rx  = int(m.group(2))
            self.lora_err = int(m.group(3))
            if self.sample_count == 0:
                self._snap_start()
            self.sample_count += 1
            return

        m = RE_AODV.search(line)
        if m:
            self.rreq_t = int(m.group(1))
            self.rreq_r = int(m.group(2))
            self.rrep_t = int(m.group(3))
            self.rrep_r = int(m.group(4))
            self.rerr_t = int(m.group(5))
            self.rerr_r = int(m.group(6))
            return

        m = RE_BEACON.search(line)
        if m:
            self.beacon_tx = int(m.group(1))
            self.beacon_rx = int(m.group(2))
            return

        m = RE_HARVEST.search(line)
        if m:
            self.role = "GATEWAY"
            self.harvest_cycles = int(m.group(1))
            self.harvest_imgs   = int(m.group(2))
            self.harvest_bytes  = int(m.group(3))
            return

        m = RE_COAP.search(line)
        if m:
            self.coap_reqs   = int(m.group(1))
            self.coap_blocks = int(m.group(2))
            return

        m = RE_MAC.search(line)
        if m:
            self.mac = m.group(1)
            return

        if "PROMOTED TO ACTING GATEWAY" in line:
            self.role = "GATEWAY"
        elif "RSSI-ASSIGNED TO RELAY" in line:
            self.role = "RELAY"

    # ── Deltas (final - start) ──
    @property
    def lora_tx_delta(self):   return self.lora_tx   - (self.lora_tx_start   or 0)
    @property
    def lora_rx_delta(self):   return self.lora_rx   - (self.lora_rx_start   or 0)
    @property
    def beacon_tx_delta(self): return self.beacon_tx - (self.beacon_tx_start or 0)
    @property
    def beacon_rx_delta(self): return self.beacon_rx - (self.beacon_rx_start or 0)
    @property
    def rreq_t_delta(self):    return self.rreq_t    - (self.rreq_t_start    or 0)
    @property
    def rreq_r_delta(self):    return self.rreq_r    - (self.rreq_r_start    or 0)
    @property
    def rrep_t_delta(self):    return self.rrep_t    - (self.rrep_t_start    or 0)
    @property
    def rrep_r_delta(self):    return self.rrep_r    - (self.rrep_r_start    or 0)


def reader_thread(port: str, stats: NodeStats, stop_event: threading.Event):
    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
        ser.reset_input_buffer()
    except Exception as e:
        print(f"[{port}] open failed: {e}")
        return

    try:
        while not stop_event.is_set():
            try:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if line:
                    stats.update(line)
            except Exception:
                continue
    finally:
        try: ser.close()
        except: pass


def auto_detect_ports():
    """Auto-detect ESP32-S3 serial ports (LILYGO T3-S3 V1.2)."""
    detected = []
    for p in serial.tools.list_ports.comports():
        hwid = (p.hwid or "").upper()
        if "303A:1001" in hwid or "USB VID:PID=303A" in hwid:
            detected.append(p.device)
    return sorted(detected)


def format_table(rows):
    """Pretty-print a list of (label, value) tuples as a right-aligned table."""
    label_w = max(len(r[0]) for r in rows)
    return "\n".join(f"  {r[0]:<{label_w}} : {r[1]}" for r in rows)


def compute_loss_stats(nodes):
    N = len(nodes)
    total_beacon_tx = sum(n.beacon_tx_delta for n in nodes)
    total_beacon_rx = sum(n.beacon_rx_delta for n in nodes)
    total_lora_tx   = sum(n.lora_tx_delta   for n in nodes)
    total_lora_rx   = sum(n.lora_rx_delta   for n in nodes)

    expected_beacon_rx = total_beacon_tx * (N - 1) if N > 1 else 0
    expected_lora_rx   = total_lora_tx   * (N - 1) if N > 1 else 0

    beacon_loss_pct = 0.0
    lora_loss_pct   = 0.0
    if expected_beacon_rx > 0:
        beacon_loss_pct = max(0.0, 1.0 - total_beacon_rx / expected_beacon_rx) * 100
    if expected_lora_rx > 0:
        lora_loss_pct = max(0.0, 1.0 - total_lora_rx / expected_lora_rx) * 100

    return {
        'N': N,
        'total_beacon_tx': total_beacon_tx,
        'total_beacon_rx': total_beacon_rx,
        'expected_beacon_rx': expected_beacon_rx,
        'beacon_loss_pct': beacon_loss_pct,
        'total_lora_tx': total_lora_tx,
        'total_lora_rx': total_lora_rx,
        'expected_lora_rx': expected_lora_rx,
        'lora_loss_pct': lora_loss_pct,
    }


def render_report(nodes, stats, duration_s):
    lines = []
    lines.append("=" * 70)
    lines.append("IOT Forest Cam — Packet Loss Analysis Report")
    lines.append(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"Capture duration: {duration_s}s ({duration_s/60:.1f} min)")
    lines.append(f"Nodes observed: {stats['N']}")
    lines.append("=" * 70)

    # ── Per-node table ─────────────────────────────────────────
    lines.append("")
    lines.append("Per-node deltas (final − first [STATS] sample)")
    lines.append("-" * 70)
    header = (f"  {'Port':<8} {'Role':<8} {'MAC':<18} "
              f"{'LoRa TX':>8} {'LoRa RX':>8} "
              f"{'Bcn TX':>7} {'Bcn RX':>7} {'Samples':>8}")
    lines.append(header)
    lines.append("  " + "-" * 68)
    for n in nodes:
        lines.append(
            f"  {n.port:<8} {n.role:<8} {n.mac:<18} "
            f"{n.lora_tx_delta:>8} {n.lora_rx_delta:>8} "
            f"{n.beacon_tx_delta:>7} {n.beacon_rx_delta:>7} "
            f"{n.sample_count:>8}"
        )

    # ── AODV table ─────────────────────────────────────────────
    lines.append("")
    lines.append("AODV control-plane deltas")
    lines.append("-" * 70)
    lines.append(f"  {'Port':<8} {'RREQ T':>8} {'RREQ R':>8} "
                 f"{'RREP T':>8} {'RREP R':>8}")
    lines.append("  " + "-" * 44)
    for n in nodes:
        lines.append(
            f"  {n.port:<8} "
            f"{n.rreq_t_delta:>8} {n.rreq_r_delta:>8} "
            f"{n.rrep_t_delta:>8} {n.rrep_r_delta:>8}"
        )

    # ── Loss summary ───────────────────────────────────────────
    lines.append("")
    lines.append("Packet loss summary (mesh-wide)")
    lines.append("-" * 70)
    N = stats['N']
    lines.append(f"  Formula:        expected_RX = total_TX × (N−1), "
                 f"loss = 1 − actual_RX / expected_RX")
    lines.append(f"  Node count N:   {N}")
    lines.append("")
    lines.append("  BEACON layer (broadcast, every node should hear all):")
    lines.append(f"    Total beacons sent     : {stats['total_beacon_tx']}")
    lines.append(f"    Expected RX across mesh: {stats['expected_beacon_rx']}")
    lines.append(f"    Actual RX across mesh  : {stats['total_beacon_rx']}")
    lines.append(f"    Loss rate              : {stats['beacon_loss_pct']:.2f}%")
    lines.append("")
    lines.append("  LoRa radio layer (includes beacons + AODV + election):")
    lines.append(f"    Total LoRa TX          : {stats['total_lora_tx']}")
    lines.append(f"    Expected RX across mesh: {stats['expected_lora_rx']}")
    lines.append(f"    Actual RX across mesh  : {stats['total_lora_rx']}")
    lines.append(f"    Loss rate              : {stats['lora_loss_pct']:.2f}%")

    # ── CoAP / Harvest ──────────────────────────────────────────
    gw_nodes = [n for n in nodes if n.role == "GATEWAY" and n.harvest_cycles > 0]
    leaf_nodes = [n for n in nodes if n.coap_blocks > 0]
    if gw_nodes or leaf_nodes:
        lines.append("")
        lines.append("Data-plane activity (CoAP over WiFi)")
        lines.append("-" * 70)
        for n in gw_nodes:
            lines.append(f"  Gateway {n.port}: {n.harvest_cycles} cycles, "
                         f"{n.harvest_imgs} imgs, {n.harvest_bytes} bytes")
        for n in leaf_nodes:
            lines.append(f"  Leaf    {n.port}: {n.coap_reqs} requests, "
                         f"{n.coap_blocks} blocks served")

    lines.append("")
    lines.append("=" * 70)
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="IOT Forest Cam packet loss analyzer")
    parser.add_argument("--duration", "-d", type=int, default=180,
                        help="Capture duration in seconds (default: 180)")
    parser.add_argument("--ports", "-p", nargs="+", default=None,
                        help="Serial ports (default: auto-detect ESP32-S3)")
    parser.add_argument("--output", "-o", default=None,
                        help="Report output path (default: tools/reports/TIMESTAMP.txt)")
    args = parser.parse_args()

    ports = args.ports or auto_detect_ports()
    if not ports:
        print("No ESP32-S3 ports detected. Specify with --ports COM9 COM10 ...")
        return 1

    print(f"Capturing from {len(ports)} ports: {', '.join(ports)}")
    print(f"Duration: {args.duration}s")
    print("Waiting for first [STATS] samples... (~5s after boot)")
    print()

    nodes = [NodeStats(p) for p in ports]
    stop_event = threading.Event()
    threads = []
    for i, p in enumerate(ports):
        t = threading.Thread(target=reader_thread,
                             args=(p, nodes[i], stop_event), daemon=True)
        t.start()
        threads.append(t)

    # Progress bar
    start = time.time()
    try:
        while True:
            elapsed = time.time() - start
            if elapsed >= args.duration:
                break
            remaining = args.duration - elapsed
            bar_w = 40
            filled = int(bar_w * elapsed / args.duration)
            bar = "#" * filled + "-" * (bar_w - filled)
            print(f"\r  [{bar}] {elapsed:5.0f}s / {args.duration}s  "
                  f"(samples: {sum(n.sample_count for n in nodes)})",
                  end="", flush=True)
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nInterrupted.")

    stop_event.set()
    for t in threads:
        t.join(timeout=2)
    print()

    # Compute and print report
    stats = compute_loss_stats(nodes)
    report = render_report(nodes, stats, args.duration)
    print()
    print(report)

    # Save report to file
    if args.output:
        out_path = Path(args.output)
    else:
        reports_dir = Path(__file__).parent / "reports"
        reports_dir.mkdir(exist_ok=True)
        out_path = reports_dir / f"packet_loss_{datetime.now().strftime('%Y%m%d_%H%M%S')}.txt"
    out_path.write_text(report, encoding="utf-8")
    print(f"\nReport saved to: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
