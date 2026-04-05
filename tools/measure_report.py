#!/usr/bin/env python3
"""
IOT Forest Cam — Measurement Report Generator

Captures serial output from all connected nodes for a fixed duration
(default 300 s / 5 min), then extracts timestamp-based and throughput
metrics from the existing firmware log output. Produces a markdown
report ready to paste into the design review document.

Metrics captured (all derived from existing serial log markers):
  1. Packet Delivery Ratio (mesh-wide, from [STATS] counter deltas)
  2. CoAP Block2 goodput (from "Speed: X.X KB/s" lines)
  3. Image transfer time by size bucket
  4. AODV route discovery latency
  5. Bully election convergence time
  6. Beacon-reactive harvest trigger latency
  7. Fletcher-16 mismatch rate
  8. Full 4-node harvest cycle time

Usage:
  python measure_report.py                    # auto-detect ports, 5 min
  python measure_report.py --duration 120     # 2 min run
  python measure_report.py --ports COM9 COM10 COM12 COM13
"""

import argparse
import re
import sys
import time
import threading
import statistics
from datetime import datetime
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("Install pyserial:  pip install pyserial")
    sys.exit(1)


# ─── Regex patterns (match the firmware's actual log output) ───

# [STATS] lines (every 5 s)
RE_STATS_LORA   = re.compile(r'\[STATS\] LoRa tx=(\d+) rx=(\d+) err=(\d+)')
RE_STATS_AODV   = re.compile(r'\[STATS\] AODV rreqT=(\d+) rreqR=(\d+) rrepT=(\d+) rrepR=(\d+) rerrT=(\d+) rerrR=(\d+)')
RE_STATS_BEACON = re.compile(r'\[STATS\] Beacon tx=(\d+) rx=(\d+)')
RE_MAC          = re.compile(r'Initialized, nodeId=([0-9A-Fa-f:]{17})')

# Throughput (printed after each image download by HarvestLoop)
RE_SPEED = re.compile(
    r'Bytes:\s*(\d+)\s*\|\s*Blocks:\s*(\d+)\s*\|\s*Time:\s*(\d+)\s*ms\s*\|\s*Speed:\s*([\d.]+)\s*KB/s'
)

# AODV
RE_RREQ_TX  = re.compile(r'RREQ broadcast for (\S+)')
RE_ROUTE_OK = re.compile(r'Route DISCOVERED:\s*(\S+)\s*via\s*\S+\s*\((\d+)\s*hops\)')

# Election
RE_PROMOTED = re.compile(r'PROMOTED TO ACTING GATEWAY')
RE_BOOT     = re.compile(r'IOT Forest Cam|\[Boot\]|Role: LEAF|Booting as:')

# Harvest
RE_NEW_NODE = re.compile(r'NEW node\s*\[\d+\]\s*(\S+)')
RE_CYC_BEG  = re.compile(r'HARVEST CYCLE STARTING')
RE_CYC_END  = re.compile(r'HARVEST CYCLE COMPLETE')

# Checksums
RE_CHK_PASS = re.compile(r'Checksum.*PASS', re.IGNORECASE)
RE_CHK_FAIL = re.compile(r'Checksum.*(FAIL|MISMATCH)', re.IGNORECASE)


# ─── Data classes ──────────────────────────────────────────────

class LogEntry:
    __slots__ = ('ts', 'line')
    def __init__(self, ts, line):
        self.ts   = ts
        self.line = line


class NodeCapture:
    def __init__(self, port):
        self.port = port
        self.mac  = "?"
        self.entries = []  # list of LogEntry
        self.first_ts = None
        # [STATS] counter snapshots (first and last seen)
        self.lora_tx0   = None
        self.lora_rx0   = None
        self.beacon_tx0 = None
        self.beacon_rx0 = None
        self.lora_tx_f   = 0
        self.lora_rx_f   = 0
        self.beacon_tx_f = 0
        self.beacon_rx_f = 0

    def append(self, ts, line):
        if self.first_ts is None:
            self.first_ts = ts
        self.entries.append(LogEntry(ts, line))
        # Track MAC if seen
        m = RE_MAC.search(line)
        if m:
            self.mac = m.group(1)
        # [STATS] snapshots
        m = RE_STATS_LORA.search(line)
        if m:
            if self.lora_tx0 is None:
                self.lora_tx0 = int(m.group(1))
                self.lora_rx0 = int(m.group(2))
            self.lora_tx_f = int(m.group(1))
            self.lora_rx_f = int(m.group(2))
        m = RE_STATS_BEACON.search(line)
        if m:
            if self.beacon_tx0 is None:
                self.beacon_tx0 = int(m.group(1))
                self.beacon_rx0 = int(m.group(2))
            self.beacon_tx_f = int(m.group(1))
            self.beacon_rx_f = int(m.group(2))

    @property
    def lora_tx_d(self):   return self.lora_tx_f   - (self.lora_tx0   or 0)
    @property
    def lora_rx_d(self):   return self.lora_rx_f   - (self.lora_rx0   or 0)
    @property
    def beacon_tx_d(self): return self.beacon_tx_f - (self.beacon_tx0 or 0)
    @property
    def beacon_rx_d(self): return self.beacon_rx_f - (self.beacon_rx0 or 0)


# ─── Serial capture ────────────────────────────────────────────

def reader_thread(port, cap: NodeCapture, stop_event):
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
                line = raw.decode("utf-8", errors="replace").rstrip()
                if line:
                    cap.append(time.time(), line)
            except Exception:
                continue
    finally:
        try: ser.close()
        except: pass


def auto_detect_ports():
    detected = []
    for p in serial.tools.list_ports.comports():
        hwid = (p.hwid or "").upper()
        if "303A:1001" in hwid or "USB VID:PID=303A" in hwid:
            detected.append(p.device)
    return sorted(detected)


# ─── Parsers ───────────────────────────────────────────────────

def parse_speed_samples(nodes):
    """Collect (bytes, blocks, time_ms, speed_kbs) tuples from all nodes."""
    samples = []
    for n in nodes:
        for e in n.entries:
            m = RE_SPEED.search(e.line)
            if m:
                samples.append((int(m.group(1)), int(m.group(2)),
                                int(m.group(3)), float(m.group(4))))
    return samples


def parse_route_discovery_latency(nodes):
    """Pair each 'RREQ broadcast for X' with next 'Route DISCOVERED: X via ...'
    on the same node. Return list of (hops, delta_ms)."""
    results = []
    for n in nodes:
        pending = {}  # dest -> timestamp of RREQ
        for e in n.entries:
            m = RE_RREQ_TX.search(e.line)
            if m:
                dest = m.group(1)
                pending[dest] = e.ts
                continue
            m = RE_ROUTE_OK.search(e.line)
            if m:
                dest  = m.group(1)
                hops  = int(m.group(2))
                # Broadcast discovery: match by most recent pending RREQ
                # (either to this dest or to FF:FF:FF:FF:FF:FF broadcast)
                src_ts = None
                if dest in pending:
                    src_ts = pending.pop(dest)
                else:
                    # Broadcast RREQ discovers any node, take FF:FF entry
                    for k in list(pending.keys()):
                        if "FF:FF" in k.upper():
                            src_ts = pending[k]
                            # Don't pop — a single broadcast RREQ discovers multiple
                            break
                if src_ts is not None:
                    delta_ms = (e.ts - src_ts) * 1000
                    results.append((hops, delta_ms))
    return results


def parse_election_convergence(nodes):
    """First boot ts → first PROMOTED ts (on any node)."""
    first_boot = None
    first_promoted = None
    for n in nodes:
        for e in n.entries:
            if first_boot is None and RE_BOOT.search(e.line):
                first_boot = e.ts
            if first_promoted is None and RE_PROMOTED.search(e.line):
                first_promoted = e.ts
            if first_boot and first_promoted:
                break
    if first_boot is not None and first_promoted is not None and first_promoted >= first_boot:
        return (first_promoted - first_boot) * 1000
    return None


def parse_harvest_trigger_latency(nodes):
    """Per 'NEW node X' → next 'HARVEST CYCLE STARTING' on same node."""
    results = []
    for n in nodes:
        new_node_ts = None
        for e in n.entries:
            if RE_NEW_NODE.search(e.line):
                if new_node_ts is None:  # first new node in this window
                    new_node_ts = e.ts
                continue
            if RE_CYC_BEG.search(e.line) and new_node_ts is not None:
                results.append((e.ts - new_node_ts) * 1000)
                new_node_ts = None  # reset for next cycle
    return results


def parse_cycle_times(nodes):
    """HARVEST CYCLE STARTING → HARVEST CYCLE COMPLETE deltas."""
    results = []
    for n in nodes:
        cyc_start = None
        for e in n.entries:
            if RE_CYC_BEG.search(e.line):
                cyc_start = e.ts
            elif RE_CYC_END.search(e.line) and cyc_start is not None:
                results.append((e.ts - cyc_start) * 1000)
                cyc_start = None
    return results


def parse_checksum_stats(nodes):
    passes = fails = 0
    for n in nodes:
        for e in n.entries:
            if RE_CHK_PASS.search(e.line):
                passes += 1
            if RE_CHK_FAIL.search(e.line):
                fails += 1
    return passes, fails


def compute_pdr(nodes):
    """Reuse packet_loss_analyzer logic: PDR from [STATS] counter deltas."""
    N = len(nodes)
    total_beacon_tx = sum(n.beacon_tx_d for n in nodes)
    total_beacon_rx = sum(n.beacon_rx_d for n in nodes)
    total_lora_tx   = sum(n.lora_tx_d   for n in nodes)
    total_lora_rx   = sum(n.lora_rx_d   for n in nodes)
    exp_beacon_rx = total_beacon_tx * (N - 1) if N > 1 else 0
    exp_lora_rx   = total_lora_tx   * (N - 1) if N > 1 else 0
    beacon_loss = 0.0
    lora_loss   = 0.0
    if exp_beacon_rx > 0:
        beacon_loss = max(0.0, 1.0 - total_beacon_rx / exp_beacon_rx) * 100
    if exp_lora_rx > 0:
        lora_loss = max(0.0, 1.0 - total_lora_rx / exp_lora_rx) * 100
    return {
        'N': N,
        'beacon_tx': total_beacon_tx,
        'beacon_rx': total_beacon_rx,
        'beacon_expected': exp_beacon_rx,
        'beacon_loss_pct': beacon_loss,
        'beacon_pdr_pct':  100.0 - beacon_loss,
        'lora_tx':   total_lora_tx,
        'lora_rx':   total_lora_rx,
        'lora_expected': exp_lora_rx,
        'lora_loss_pct': lora_loss,
        'lora_pdr_pct':  100.0 - lora_loss,
    }


# ─── Statistics helpers ────────────────────────────────────────

def stat_summary(values, fmt="{:.1f}"):
    if not values:
        return "no samples"
    mn = min(values)
    mx = max(values)
    mean = sum(values) / len(values)
    med  = statistics.median(values)
    return (f"min={fmt.format(mn)}  mean={fmt.format(mean)}  "
            f"median={fmt.format(med)}  max={fmt.format(mx)}  N={len(values)}")


def bucket_transfers(speed_samples):
    """Group (bytes, blocks, time_ms, speed) by size bucket."""
    buckets = {
        "<30 kB":     [],
        "30-60 kB":   [],
        "60-100 kB":  [],
        ">100 kB":    [],
    }
    for b, _blk, t, s in speed_samples:
        kb = b / 1024
        if kb < 30:
            buckets["<30 kB"].append((b, t, s))
        elif kb < 60:
            buckets["30-60 kB"].append((b, t, s))
        elif kb < 100:
            buckets["60-100 kB"].append((b, t, s))
        else:
            buckets[">100 kB"].append((b, t, s))
    return buckets


# ─── Report rendering ──────────────────────────────────────────

def render_markdown(nodes, duration_s):
    pdr = compute_pdr(nodes)
    speed_samples = parse_speed_samples(nodes)
    rd_latency    = parse_route_discovery_latency(nodes)
    election_ms   = parse_election_convergence(nodes)
    trigger_ms    = parse_harvest_trigger_latency(nodes)
    cycle_ms      = parse_cycle_times(nodes)
    chk_pass, chk_fail = parse_checksum_stats(nodes)

    buckets = bucket_transfers(speed_samples)
    goodput_kbs = [s for _b, _t, s in
                   [(b, t, sp) for b, _blk, t, sp in speed_samples]]
    goodput_kbs = [s for _b, _blk, _t, s in speed_samples]

    lines = []
    lines.append(f"# IOT Forest Cam — Measurement Report")
    lines.append("")
    lines.append(f"- **Capture timestamp:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"- **Capture duration:** {duration_s} s ({duration_s/60:.1f} min)")
    lines.append(f"- **Nodes observed:** {len(nodes)}")
    lines.append("")
    lines.append("All values below are derived from live serial logs of the running "
                 "firmware; no estimates or datasheet figures.")
    lines.append("")

    # ── PDR ─────────────────────────────────────────────
    lines.append("## 1. Packet Delivery Ratio (mesh-wide)")
    lines.append("")
    lines.append("Computed from `[STATS]` TX/RX counter deltas, formula "
                 "`expected_RX = sum(TX) × (N−1)` and `PDR = actual_RX / expected_RX`.")
    lines.append("")
    lines.append("| Layer | Total TX | Expected RX | Actual RX | Loss % | PDR % |")
    lines.append("|-------|---------:|------------:|----------:|-------:|------:|")
    lines.append(f"| Beacon (broadcast) | {pdr['beacon_tx']} | {pdr['beacon_expected']} "
                 f"| {pdr['beacon_rx']} | {pdr['beacon_loss_pct']:.2f}% "
                 f"| **{pdr['beacon_pdr_pct']:.2f}%** |")
    lines.append(f"| LoRa radio (all packet types) | {pdr['lora_tx']} | {pdr['lora_expected']} "
                 f"| {pdr['lora_rx']} | {pdr['lora_loss_pct']:.2f}% "
                 f"| **{pdr['lora_pdr_pct']:.2f}%** |")
    lines.append("")

    # ── Goodput ────────────────────────────────────────
    lines.append("## 2. CoAP Block2 goodput (1-hop, WiFi + CoAP + SD)")
    lines.append("")
    if goodput_kbs:
        lines.append(f"- **Summary (all images):** {stat_summary(goodput_kbs, '{:.2f}')} KB/s")
    else:
        lines.append("- No speed samples captured in this window.")
    lines.append("")

    # ── Transfer time by size ───────────────────────────
    lines.append("## 3. End-to-end image transfer time by size")
    lines.append("")
    lines.append("| Size bucket | N | mean time (ms) | mean goodput (KB/s) |")
    lines.append("|-------------|--:|---------------:|--------------------:|")
    any_bucket = False
    for label, entries in buckets.items():
        if not entries:
            continue
        any_bucket = True
        times  = [t for _b, t, _s in entries]
        speeds = [s for _b, _t, s in entries]
        lines.append(f"| {label} | {len(entries)} "
                     f"| {sum(times)/len(times):.0f} | {sum(speeds)/len(speeds):.2f} |")
    if not any_bucket:
        lines.append("| *no transfers captured* | | | |")
    lines.append("")

    # ── Route discovery latency ─────────────────────────
    lines.append("## 4. AODV route discovery latency")
    lines.append("")
    if rd_latency:
        by_hops = {}
        for hops, ms in rd_latency:
            by_hops.setdefault(hops, []).append(ms)
        lines.append("| Hops | N | latency (ms) |")
        lines.append("|-----:|--:|-------------|")
        for hops in sorted(by_hops.keys()):
            vs = by_hops[hops]
            lines.append(f"| {hops} | {len(vs)} "
                         f"| mean={sum(vs)/len(vs):.0f}, "
                         f"min={min(vs):.0f}, max={max(vs):.0f} |")
    else:
        lines.append("- No RREQ→RREP pairs captured in this window.")
    lines.append("")

    # ── Election ────────────────────────────────────────
    lines.append("## 5. Bully election convergence time")
    lines.append("")
    if election_ms is not None:
        lines.append(f"- **First boot → PROMOTED TO ACTING GATEWAY:** {election_ms:.0f} ms ({election_ms/1000:.1f} s)")
    else:
        lines.append("- No PROMOTED event observed (no election fired in this window).")
    lines.append("")

    # ── Harvest trigger ─────────────────────────────────
    lines.append("## 6. Beacon-reactive harvest trigger latency")
    lines.append("")
    lines.append("From first new-node beacon → `HARVEST CYCLE STARTING`. "
                 "Code target is 15 s via `HARVEST_REACTIVE_DELAY_MS`.")
    lines.append("")
    if trigger_ms:
        lines.append(f"- **Summary:** {stat_summary(trigger_ms, '{:.0f}')} ms")
    else:
        lines.append("- No harvest trigger observed in this window.")
    lines.append("")

    # ── Cycle time ──────────────────────────────────────
    lines.append("## 7. Full harvest cycle time (4 nodes)")
    lines.append("")
    if cycle_ms:
        lines.append(f"- **Summary:** {stat_summary(cycle_ms, '{:.0f}')} ms")
        lines.append(f"- Cycles completed in window: **{len(cycle_ms)}**")
    else:
        lines.append("- No full cycle completed in this window.")
    lines.append("")

    # ── Checksums ───────────────────────────────────────
    lines.append("## 8. Fletcher-16 integrity")
    lines.append("")
    total = chk_pass + chk_fail
    if total > 0:
        mismatch_pct = (chk_fail / total) * 100
        lines.append(f"- **PASS:** {chk_pass}")
        lines.append(f"- **FAIL/MISMATCH:** {chk_fail}")
        lines.append(f"- **Mismatch rate:** {mismatch_pct:.2f}% (N={total})")
    else:
        lines.append("- No checksum verifications observed in this window.")
    lines.append("")

    # ── Per-node raw counters ───────────────────────────
    lines.append("## Appendix A — Per-node raw deltas")
    lines.append("")
    lines.append("| Port | MAC | LoRa TX | LoRa RX | Beacon TX | Beacon RX |")
    lines.append("|------|-----|--------:|--------:|----------:|----------:|")
    for n in nodes:
        lines.append(f"| {n.port} | {n.mac} | {n.lora_tx_d} | {n.lora_rx_d} "
                     f"| {n.beacon_tx_d} | {n.beacon_rx_d} |")
    lines.append("")

    return "\n".join(lines)


# ─── Main ──────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="IOT Forest Cam measurement report generator")
    ap.add_argument("--duration", "-d", type=int, default=300,
                    help="Capture duration in seconds (default: 300)")
    ap.add_argument("--ports", "-p", nargs="+", default=None,
                    help="Serial ports (default: auto-detect ESP32-S3)")
    args = ap.parse_args()

    ports = args.ports or auto_detect_ports()
    if not ports:
        print("No ESP32-S3 ports detected. Specify with --ports COM9 COM10 ...")
        return 1

    print(f"Capturing from {len(ports)} ports: {', '.join(ports)}")
    print(f"Duration: {args.duration}s")
    print("Reset boards NOW if you want clean baselines.")
    print()

    nodes = [NodeCapture(p) for p in ports]
    stop_event = threading.Event()
    threads = []
    for i, p in enumerate(ports):
        t = threading.Thread(target=reader_thread,
                             args=(p, nodes[i], stop_event), daemon=True)
        t.start()
        threads.append(t)

    start = time.time()
    try:
        while True:
            elapsed = time.time() - start
            if elapsed >= args.duration:
                break
            bar_w = 40
            filled = int(bar_w * elapsed / args.duration)
            bar = "#" * filled + "-" * (bar_w - filled)
            total_entries = sum(len(n.entries) for n in nodes)
            print(f"\r  [{bar}] {elapsed:5.0f}s / {args.duration}s  "
                  f"(lines: {total_entries})", end="", flush=True)
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nInterrupted — generating partial report.")

    stop_event.set()
    for t in threads:
        t.join(timeout=2)
    print()
    print()

    # Render markdown
    md = render_markdown(nodes, args.duration)
    print(md)

    # Save
    reports_dir = Path(__file__).parent / "reports"
    reports_dir.mkdir(exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    md_path = reports_dir / f"measurements_{ts}.md"
    md_path.write_text(md, encoding="utf-8")

    # Also save raw per-port logs (useful for manual inspection)
    for n in nodes:
        raw_path = reports_dir / f"raw_{ts}_{n.port}.log"
        with open(raw_path, "w", encoding="utf-8") as f:
            for e in n.entries:
                f.write(f"{e.ts:.3f}\t{e.line}\n")

    print(f"\nMarkdown report: {md_path}")
    print(f"Raw logs:        {reports_dir}/raw_{ts}_*.log")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
