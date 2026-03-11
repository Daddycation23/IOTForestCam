"""
Raw UDP CoAP test — bypasses aiocoap to test basic connectivity.
Sends a minimal CoAP GET /info request and prints raw response.

Usage: python test_udp_raw.py
"""
import socket
import struct
import time

LEAF_IP   = "192.168.4.1"
LEAF_PORT = 5683
TIMEOUT   = 5  # seconds

def build_coap_get_info():
    """Build a minimal CoAP CON GET /info request."""
    # Header: Ver=1, Type=CON(0), TKL=1, Code=GET(0.01), MsgID=0x0001
    ver_type_tkl = (1 << 6) | (0 << 4) | 1   # Ver=1, T=CON, TKL=1
    code = 1  # 0.01 = GET
    msg_id = 1
    token = b'\xAB'  # 1-byte token

    header = struct.pack('!BBH', ver_type_tkl, code, msg_id)

    # Option: Uri-Path = "info" (option number 11, delta=11, length=4)
    opt_byte = (11 << 4) | 4  # delta=11, length=4
    option = struct.pack('B', opt_byte) + b'info'

    return header + token + option

def main():
    print(f"=== Raw UDP CoAP Test ===")
    print(f"Target: {LEAF_IP}:{LEAF_PORT}\n")

    # Step 1: Basic ping via UDP
    print("[1] Testing UDP reachability...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(TIMEOUT)

    # Build CoAP GET /info
    packet = build_coap_get_info()
    print(f"    Sending {len(packet)} byte CoAP GET /info...")
    print(f"    Hex: {packet.hex()}")

    try:
        sock.sendto(packet, (LEAF_IP, LEAF_PORT))
        print(f"    Sent! Waiting for response (timeout={TIMEOUT}s)...")

        data, addr = sock.recvfrom(1024)
        print(f"\n    RESPONSE from {addr}:")
        print(f"    Length: {len(data)} bytes")
        print(f"    Hex:   {data[:64].hex()}")

        # Try to decode payload
        # Find payload marker 0xFF
        marker_idx = data.find(b'\xff')
        if marker_idx >= 0:
            payload = data[marker_idx + 1:]
            print(f"    Payload ({len(payload)} bytes): {payload.decode('utf-8', errors='replace')}")
        else:
            print(f"    No payload marker found")

        print("\n    SUCCESS - CoAP server is responding!")

    except socket.timeout:
        print(f"\n    TIMEOUT - No response after {TIMEOUT}s")
        print(f"\n    Possible causes:")
        print(f"    - Windows Firewall blocking UDP port {LEAF_PORT}")
        print(f"    - Not connected to ForestCam WiFi AP")
        print(f"    - Leaf CoAP server not actually running")
        print(f"\n    Try: Disable Windows Firewall temporarily and retry")

    except Exception as e:
        print(f"\n    ERROR: {e}")

    finally:
        sock.close()

    # Step 2: Can we even reach the IP?
    print(f"\n[2] Checking if {LEAF_IP} is reachable (ICMP may be blocked)...")
    import subprocess
    result = subprocess.run(
        ['ping', '-n', '2', '-w', '2000', LEAF_IP],
        capture_output=True, text=True, timeout=10
    )
    # Print just the summary lines
    for line in result.stdout.strip().split('\n'):
        line = line.strip()
        if line and ('Reply' in line or 'Request' in line or 'Packets' in line
                     or 'packets' in line or 'Lost' in line or 'loss' in line):
            print(f"    {line}")

if __name__ == "__main__":
    main()
