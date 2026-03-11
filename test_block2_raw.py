"""
Raw UDP Block2 test — manually requests image blocks one at a time.
Bypasses aiocoap to isolate Block2 transfer issues.

Usage: python test_block2_raw.py
"""
import socket
import struct
import sys
import hashlib

LEAF_IP   = "192.168.4.1"
LEAF_PORT = 5683
TIMEOUT   = 5
BLOCK_SZX = 5          # 2^(5+4) = 512 bytes
BLOCK_SIZE = 512

def build_coap_request(msg_id, token, uri_segments, block2_num=None, block2_szx=BLOCK_SZX):
    """Build a CoAP CON GET request with optional Block2 option."""
    # Header: Ver=1, Type=CON(0), TKL=len(token)
    ver_type_tkl = (1 << 6) | (0 << 4) | len(token)
    code = 1  # 0.01 = GET
    header = struct.pack('!BBH', ver_type_tkl, code, msg_id)
    packet = header + token

    # Options must be sorted by option number
    # Uri-Path = option 11
    prev_opt_num = 0
    for seg in uri_segments:
        delta = 11 - prev_opt_num
        seg_bytes = seg.encode('utf-8')
        seg_len = len(seg_bytes)

        if delta < 13 and seg_len < 13:
            packet += struct.pack('B', (delta << 4) | seg_len)
        else:
            raise ValueError("Extended option encoding not implemented for this test")

        packet += seg_bytes
        prev_opt_num = 11  # Uri-Path is repeatable, same number

    # Block2 = option 23
    if block2_num is not None:
        delta = 23 - prev_opt_num
        block2_val = (block2_num << 4) | (0 << 3) | (block2_szx & 0x07)

        # Encode block2 value as minimal bytes
        if block2_val <= 0xFF:
            val_bytes = struct.pack('B', block2_val)
        elif block2_val <= 0xFFFF:
            val_bytes = struct.pack('!H', block2_val)
        elif block2_val <= 0xFFFFFF:
            val_bytes = struct.pack('!I', block2_val)[1:]  # 3 bytes
        else:
            val_bytes = struct.pack('!I', block2_val)

        val_len = len(val_bytes)

        if delta < 13 and val_len < 13:
            packet += struct.pack('B', (delta << 4) | val_len)
        elif delta >= 13 and delta < 269:
            packet += struct.pack('BB', (13 << 4) | val_len, delta - 13)
        else:
            raise ValueError("Extended option delta not implemented")

        packet += val_bytes
        prev_opt_num = 23

    return packet

def parse_coap_response(data):
    """Parse a CoAP response, extracting Block2 and payload."""
    if len(data) < 4:
        return None

    ver = (data[0] >> 6) & 0x03
    msg_type = (data[0] >> 4) & 0x03
    tkl = data[0] & 0x0F
    code_raw = data[1]
    code_class = (code_raw >> 5) & 0x07
    code_detail = code_raw & 0x1F
    msg_id = (data[2] << 8) | data[3]

    token = data[4:4+tkl]
    pos = 4 + tkl

    # Parse options
    prev_opt_num = 0
    options = []
    while pos < len(data) and data[pos] != 0xFF:
        delta = (data[pos] >> 4) & 0x0F
        opt_len = data[pos] & 0x0F
        pos += 1

        if delta == 13:
            delta = data[pos] + 13
            pos += 1
        elif delta == 14:
            delta = ((data[pos] << 8) | data[pos+1]) + 269
            pos += 2

        if opt_len == 13:
            opt_len = data[pos] + 13
            pos += 1
        elif opt_len == 14:
            opt_len = ((data[pos] << 8) | data[pos+1]) + 269
            pos += 2

        opt_num = prev_opt_num + delta
        opt_val = data[pos:pos+opt_len]
        options.append((opt_num, opt_val))
        pos += opt_len
        prev_opt_num = opt_num

    # Payload
    payload = b''
    if pos < len(data) and data[pos] == 0xFF:
        pos += 1
        payload = data[pos:]

    # Decode Block2 option (number 23)
    block2 = None
    size2 = None
    for opt_num, opt_val in options:
        if opt_num == 23:
            val = 0
            for b in opt_val:
                val = (val << 8) | b
            block2 = {
                'num': val >> 4,
                'more': bool((val >> 3) & 1),
                'szx': val & 0x07,
                'block_size': 1 << ((val & 0x07) + 4)
            }
        elif opt_num == 28:
            val = 0
            for b in opt_val:
                val = (val << 8) | b
            size2 = val

    return {
        'type': msg_type,
        'code': f"{code_class}.{code_detail:02d}",
        'msg_id': msg_id,
        'token': token,
        'options': options,
        'block2': block2,
        'size2': size2,
        'payload': payload
    }


def main():
    image_index = 0
    if len(sys.argv) > 1:
        image_index = int(sys.argv[1])

    print(f"=== Raw Block2 Image Download Test ===")
    print(f"Target: {LEAF_IP}:{LEAF_PORT}")
    print(f"Image index: {image_index}")
    print(f"Block size: {BLOCK_SIZE} (SZX={BLOCK_SZX})")
    print()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(TIMEOUT)

    token = b'\xCA\xFE'
    msg_id = 1
    block_num = 0
    total_payload = bytearray()
    total_size = None

    try:
        while True:
            # Build request
            if block_num == 0:
                # First request — no Block2 option (let server start from 0)
                packet = build_coap_request(
                    msg_id, token, ["image", str(image_index)]
                )
            else:
                # Subsequent requests — include Block2 with requested block number
                packet = build_coap_request(
                    msg_id, token, ["image", str(image_index)],
                    block2_num=block_num, block2_szx=BLOCK_SZX
                )

            sock.sendto(packet, (LEAF_IP, LEAF_PORT))

            # Wait for response
            data, addr = sock.recvfrom(1024)
            resp = parse_coap_response(data)

            if resp is None:
                print(f"  Block {block_num}: PARSE ERROR")
                break

            if resp['code'] != '2.05':
                print(f"  Block {block_num}: ERROR code {resp['code']}")
                if resp['payload']:
                    print(f"    Diagnostic: {resp['payload'].decode('utf-8', errors='replace')}")
                break

            b2 = resp['block2']
            if b2 is None:
                print(f"  Block {block_num}: No Block2 option in response (single-block response)")
                total_payload.extend(resp['payload'])
                break

            if block_num == 0 and resp['size2']:
                total_size = resp['size2']
                print(f"  Total file size (Size2): {total_size} bytes")
                print(f"  Expected blocks: {(total_size + b2['block_size'] - 1) // b2['block_size']}")
                print()

            total_payload.extend(resp['payload'])

            # Progress
            pct = (len(total_payload) / total_size * 100) if total_size else 0
            print(f"  Block {b2['num']:4d}: {len(resp['payload']):4d} bytes | "
                  f"Total: {len(total_payload):8d} | "
                  f"More: {b2['more']} | SZX: {b2['szx']} | "
                  f"{pct:.1f}%", end='\r')

            if not b2['more']:
                print()  # Newline after progress
                print(f"\n  Transfer complete!")
                break

            block_num = b2['num'] + 1
            msg_id += 1

    except socket.timeout:
        print(f"\n  TIMEOUT on block {block_num} after {TIMEOUT}s")
        print(f"  Received {len(total_payload)} bytes so far")

    except Exception as e:
        print(f"\n  ERROR on block {block_num}: {e}")

    finally:
        sock.close()

    # Save if we got data
    if total_payload:
        out_file = f"test_block2_img{image_index}.jpg"
        with open(out_file, 'wb') as f:
            f.write(total_payload)
        print(f"\n  Saved {len(total_payload)} bytes -> {out_file}")

        # Verify with Fletcher-16
        sum1, sum2 = 0, 0
        for b in total_payload:
            sum1 = (sum1 + b) % 255
            sum2 = (sum2 + sum1) % 255
        fletcher16 = (sum2 << 8) | sum1
        print(f"  Fletcher-16 checksum: {fletcher16}")

        if total_size:
            if len(total_payload) == total_size:
                print(f"  Size match: YES ({total_size} bytes)")
            else:
                print(f"  Size match: NO (got {len(total_payload)}, expected {total_size})")
    else:
        print("\n  No data received.")

if __name__ == "__main__":
    main()
