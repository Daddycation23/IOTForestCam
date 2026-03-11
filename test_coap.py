"""
Quick CoAP test script for ForestCam leaf node.
Connect your laptop WiFi to the leaf's AP first (ForestCam-XXXX / forestcam123),
then run: python test_coap.py
"""
import asyncio
import sys
from aiocoap import Context, Message, GET

LEAF_IP   = "192.168.4.1"
LEAF_PORT = 5683

async def coap_get(uri_path):
    """Send a CoAP GET request and return the response."""
    ctx = await Context.create_client_context()
    uri = f"coap://{LEAF_IP}:{LEAF_PORT}/{uri_path}"
    print(f"\n{'='*50}")
    print(f"GET {uri}")
    print(f"{'='*50}")

    request = Message(code=GET, uri=uri)
    try:
        response = await asyncio.wait_for(ctx.request(request).response, timeout=10)
        print(f"Response Code: {response.code}")
        print(f"Payload ({len(response.payload)} bytes):")
        # Try to decode as text, fall back to hex summary
        try:
            text = response.payload.decode('utf-8')
            print(text[:500])
        except:
            print(f"  [Binary data: {len(response.payload)} bytes]")
            print(f"  First 32 bytes: {response.payload[:32].hex()}")
        return response
    except asyncio.TimeoutError:
        print("ERROR: Request timed out (10s)")
        return None
    except Exception as e:
        print(f"ERROR: {e}")
        return None
    finally:
        await ctx.shutdown()

async def download_image(index, output_path):
    """Download an image using CoAP Block2 transfer."""
    ctx = await Context.create_client_context()
    uri = f"coap://{LEAF_IP}:{LEAF_PORT}/image/{index}"
    print(f"\n{'='*50}")
    print(f"DOWNLOAD {uri} -> {output_path}")
    print(f"{'='*50}")

    request = Message(code=GET, uri=uri)
    try:
        response = await asyncio.wait_for(ctx.request(request).response, timeout=60)
        print(f"Response Code: {response.code}")
        print(f"Received: {len(response.payload)} bytes")

        with open(output_path, 'wb') as f:
            f.write(response.payload)
        print(f"Saved to: {output_path}")
        return True
    except asyncio.TimeoutError:
        print("ERROR: Download timed out (60s)")
        return False
    except Exception as e:
        print(f"ERROR: {e}")
        return False
    finally:
        await ctx.shutdown()

async def main():
    print("ForestCam CoAP Test")
    print("=" * 50)
    print(f"Target: {LEAF_IP}:{LEAF_PORT}")
    print()

    # Test 1: Get image catalogue
    print("[TEST 1] Image catalogue (/info)")
    await coap_get("info")

    # Test 2: Resource discovery
    print("\n[TEST 2] Resource discovery (/.well-known/core)")
    await coap_get(".well-known/core")

    # Test 3: Checksum for image 0
    print("\n[TEST 3] Checksum for image 0 (/checksum/0)")
    await coap_get("checksum/0")

    # Test 4: Download image 0
    print("\n[TEST 4] Download image 0 (/image/0)")
    ok = await download_image(0, "test_download_0.jpg")
    if ok:
        import os
        size = os.path.getsize("test_download_0.jpg")
        print(f"File size on disk: {size} bytes")

    print("\n" + "=" * 50)
    print("Tests complete!")

if __name__ == "__main__":
    asyncio.run(main())
