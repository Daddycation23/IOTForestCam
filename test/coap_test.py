"""
CoAP Image Download Test
Connects to the ForestCam CoAP server and downloads an image via Block2 transfer.

Usage:
    python test/coap_test.py

Prerequisites:
    pip install aiocoap
    Connect to ForestCam WiFi AP first
"""

import asyncio
import aiocoap

SERVER = "coap://192.168.4.1"

async def main():
    ctx = await aiocoap.Context.create_client_context()

    # 1. Get image catalogue
    print("=== GET /info ===")
    req = aiocoap.Message(code=aiocoap.GET, uri=f"{SERVER}/info")
    resp = await ctx.request(req).response
    print(resp.payload.decode())
    print()

    # 2. Download image 0 via Block2 transfer
    print("=== GET /image/0 (Block2 transfer) ===")
    req = aiocoap.Message(code=aiocoap.GET, uri=f"{SERVER}/image/0")
    resp = await ctx.request(req).response

    out_file = "test_download.jpg"
    with open(out_file, "wb") as f:
        f.write(resp.payload)
    print(f"Downloaded {len(resp.payload)} bytes -> {out_file}")
    print()

    # 3. Get checksum for verification
    print("=== GET /checksum/0 ===")
    req = aiocoap.Message(code=aiocoap.GET, uri=f"{SERVER}/checksum/0")
    resp = await ctx.request(req).response
    print(resp.payload.decode())
    print()

    # 4. Verify checksum locally
    with open(out_file, "rb") as f:
        data = f.read()
    sum1, sum2 = 0, 0
    for b in data:
        sum1 = (sum1 + b) % 255
        sum2 = (sum2 + sum1) % 255
    local_checksum = (sum2 << 8) | sum1
    print(f"Local Fletcher-16 checksum: {local_checksum}")
    print(f"Match: {'YES' if str(local_checksum) in resp.payload.decode() else 'NO'}")

if __name__ == "__main__":
    asyncio.run(main())
