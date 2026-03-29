"""
Parallel flash + monitor for 3 ESP32 nodes.
Flashes all nodes simultaneously, then monitors serial output.
Retries port open every 5s for nodes in deep sleep.

Usage: python tools/flash_and_monitor.py [duration_seconds]
"""

import subprocess, serial, threading, time, sys, os

PIO = os.path.expanduser("~/.platformio/penv/Scripts/pio.exe")
PROJECT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

NODES = {
    "COM10": {"mac": "8560", "logfile": "node_8560.log"},
    "COM12": {"mac": "7B28", "logfile": "node_7B28.log"},
    "COM13": {"mac": "80E4", "logfile": "node_80E4.log"},
}

DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 300
BAUD = 115200


def flash_node(port):
    """Flash one node. Returns True on success."""
    print(f"[Flash] {port} starting...")
    result = subprocess.run(
        [PIO, "run", "-e", "esp32s3_unified", "-t", "upload", "--upload-port", port],
        cwd=PROJECT_DIR, capture_output=True, text=True, timeout=60
    )
    ok = result.returncode == 0
    print(f"[Flash] {port} {'OK' if ok else 'FAILED'}")
    return ok


def monitor_node(port, logfile, duration):
    """Monitor serial port with retry. Handles deep-sleep disconnects."""
    filepath = os.path.join(PROJECT_DIR, logfile)
    with open(filepath, 'w', encoding='utf-8', errors='replace') as f:
        start = time.time()
        ser = None
        while time.time() - start < duration:
            # Try to open port if not connected
            if ser is None:
                try:
                    ser = serial.Serial(port, BAUD, timeout=1)
                    f.write(f"--- {port} connected at {time.time()-start:.0f}s ---\n")
                    f.flush()
                    print(f"[Monitor] {port} connected")
                except Exception:
                    time.sleep(5)  # Retry in 5s (node might be sleeping)
                    continue

            # Read data
            try:
                line = ser.readline()
                if line:
                    text = line.decode('utf-8', errors='replace').rstrip()
                    f.write(text + '\n')
                    f.flush()
            except Exception:
                # Port disconnected (node went to sleep)
                f.write(f"--- {port} disconnected at {time.time()-start:.0f}s ---\n")
                f.flush()
                print(f"[Monitor] {port} disconnected (deep sleep?)")
                try:
                    ser.close()
                except:
                    pass
                ser = None
                time.sleep(2)

        if ser:
            ser.close()


if __name__ == "__main__":
    print(f"=== Parallel Flash & Monitor ({DURATION}s) ===\n")

    # Phase 1: Flash all in parallel
    print("--- Flashing all nodes in parallel ---")
    flash_threads = []
    flash_results = {}
    for port in NODES:
        def do_flash(p=port):
            flash_results[p] = flash_node(p)
        t = threading.Thread(target=do_flash)
        t.start()
        flash_threads.append(t)

    for t in flash_threads:
        t.join()

    failed = [p for p, ok in flash_results.items() if not ok]
    if failed:
        print(f"\nWARNING: Failed to flash: {failed}")
        print("Continuing with monitoring anyway...\n")

    # Brief pause for nodes to boot
    print("\nWaiting 3s for nodes to boot...\n")
    time.sleep(3)

    # Phase 2: Monitor all in parallel
    print(f"--- Monitoring all nodes for {DURATION}s ---")
    mon_threads = []
    for port, info in NODES.items():
        t = threading.Thread(target=monitor_node, args=(port, info["logfile"], DURATION))
        t.start()
        mon_threads.append(t)
        print(f"[Monitor] {port} ({info['mac']}) -> {info['logfile']}")

    for t in mon_threads:
        t.join()

    print("\n=== Monitoring complete ===")
    for port, info in NODES.items():
        filepath = os.path.join(PROJECT_DIR, info["logfile"])
        lines = 0
        try:
            with open(filepath, 'r', encoding='utf-8') as f:
                lines = sum(1 for _ in f)
        except:
            pass
        print(f"  {info['mac']} ({port}): {lines} lines captured")
