#!/usr/bin/env python3
"""
IOT Forest Cam — Harvested Image Viewer

Connects to a leaf node's WiFi AP and downloads images via CoAP,
or reads already-harvested images from a mounted SD card directory.

Usage:
  # Mode 1: Connect to a leaf node via CoAP and download images
  python viewer.py coap --host 192.168.4.1

  # Mode 2: Browse images already on the gateway's SD card
  #          (plug SD card into your Mac, find the mount point)
  python viewer.py sd --path /Volumes/SD_CARD/received

  # Mode 3: List available images on a node (no download)
  python viewer.py coap --host 192.168.4.1 --list

Requirements:
  pip install aiocoap Pillow

Author: CS Group 2
"""

import argparse
import asyncio
import json
import os
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# CoAP helpers (only imported when needed)
# ---------------------------------------------------------------------------

COAP_PORT = 5683
BLOCK_SZX = 6        # SZX=6 → 1024-byte blocks (must match firmware)
BLOCK_SIZE = 1024


async def coap_get(host: str, path: str, port: int = COAP_PORT) -> bytes:
    """Simple CoAP GET (no block-wise, for small responses like /info)."""
    import aiocoap
    ctx = await aiocoap.Context.create_client_context()
    try:
        request = aiocoap.Message(
            code=aiocoap.GET,
            uri=f"coap://{host}:{port}/{path.lstrip('/')}"
        )
        response = await asyncio.wait_for(ctx.request(request).response, timeout=10)
        return response.payload
    finally:
        await ctx.shutdown()


async def coap_get_block2(host: str, path: str, port: int = COAP_PORT) -> bytes:
    """CoAP GET with Block2 transfer — downloads a full image."""
    import aiocoap
    ctx = await aiocoap.Context.create_client_context()
    try:
        data = bytearray()
        block_num = 0

        while True:
            # Encode Block2 option: NUM | MORE(0) | SZX
            block2_value = (block_num << 4) | BLOCK_SZX

            request = aiocoap.Message(
                code=aiocoap.GET,
                uri=f"coap://{host}:{port}/{path.lstrip('/')}"
            )
            request.opt.block2 = aiocoap.optiontypes.BlockOption.BlockwiseTuple(
                block_num, False, BLOCK_SZX
            )

            response = await asyncio.wait_for(
                ctx.request(request).response, timeout=15
            )

            if response.code.is_successful():
                data.extend(response.payload)

                # Check if server says there's more
                if response.opt.block2 and response.opt.block2.more:
                    block_num += 1
                else:
                    break
            else:
                print(f"  Error: CoAP response code {response.code}")
                break

        return bytes(data)
    finally:
        await ctx.shutdown()


async def fetch_info(host: str) -> dict:
    """GET /info — returns image catalogue as JSON."""
    raw = await coap_get(host, "/info")
    return json.loads(raw.decode("utf-8"))


async def fetch_image(host: str, index: int) -> bytes:
    """GET /image/{index} — downloads full image via Block2."""
    return await coap_get_block2(host, f"/image/{index}")


async def fetch_checksum(host: str, index: int) -> dict:
    """GET /checksum/{index} — returns Fletcher-16 checksum."""
    raw = await coap_get(host, f"/checksum/{index}")
    return json.loads(raw.decode("utf-8"))


# ---------------------------------------------------------------------------
# Fletcher-16 (matches firmware implementation)
# ---------------------------------------------------------------------------

def fletcher16(data: bytes) -> int:
    sum1, sum2 = 0, 0
    for b in data:
        sum1 = (sum1 + b) % 255
        sum2 = (sum2 + sum1) % 255
    return (sum2 << 8) | sum1


# ---------------------------------------------------------------------------
# Display helpers
# ---------------------------------------------------------------------------

def show_image(path: str):
    """Open an image using the system viewer (Preview on Mac)."""
    import subprocess as _sp
    if sys.platform == "darwin":
        _sp.run(["open", path])
    elif sys.platform == "linux":
        _sp.run(["xdg-open", path])
    elif sys.platform == "win32":
        os.startfile(path)


def print_table(rows: list[dict], columns: list[tuple[str, str]]):
    """Print a formatted table."""
    # Calculate column widths
    widths = {}
    for key, header in columns:
        widths[key] = max(len(header), *(len(str(row.get(key, ""))) for row in rows))

    # Header
    header_line = " | ".join(h.ljust(widths[k]) for k, h in columns)
    print(header_line)
    print("-+-".join("-" * widths[k] for k, _ in columns))

    # Rows
    for row in rows:
        line = " | ".join(str(row.get(k, "")).ljust(widths[k]) for k, _ in columns)
        print(line)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

async def cmd_coap(args):
    """CoAP mode: connect to a node and list/download images."""
    host = args.host

    print(f"Connecting to coap://{host}:{COAP_PORT} ...")

    # Fetch catalogue
    try:
        info = await fetch_info(host)
    except Exception as e:
        print(f"Error: Could not reach {host} — {e}")
        print("Make sure you're connected to the node's WiFi AP (ForestCam-XXXX)")
        return 1

    image_count = info.get("imageCount", 0)
    block_size = info.get("blockSize", BLOCK_SIZE)
    images = info.get("images", [])

    print(f"\nNode has {image_count} image(s), block size {block_size}B\n")

    if not images:
        print("No images available.")
        return 0

    # Show catalogue
    rows = []
    for img in images:
        rows.append({
            "idx": img.get("index", "?"),
            "name": img.get("filename", "?"),
            "size": f"{img.get('fileSize', 0):,}",
            "blocks": img.get("totalBlocks", "?"),
            "checksum": f"0x{img.get('checksum', 0):04X}",
        })

    print_table(rows, [
        ("idx", "#"),
        ("name", "Filename"),
        ("size", "Size (B)"),
        ("blocks", "Blocks"),
        ("checksum", "Checksum"),
    ])

    if args.list:
        return 0

    # Download images
    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)

    indices = range(image_count) if args.index is None else [args.index]

    for i in indices:
        if i >= image_count:
            print(f"\nSkipping index {i} (only {image_count} images)")
            continue

        img_info = images[i] if i < len(images) else {}
        name = img_info.get("filename", f"image_{i:03d}.jpg")
        # Strip path prefix
        basename = name.rsplit("/", 1)[-1] if "/" in name else name

        print(f"\nDownloading [{i}] {basename} ...", end=" ", flush=True)

        try:
            data = await fetch_image(host, i)
            print(f"{len(data):,} bytes", end=" ")

            # Verify checksum
            expected = img_info.get("checksum")
            actual = fletcher16(data)
            if expected is not None:
                if actual != expected:
                    print(f"CHECKSUM MISMATCH (got 0x{actual:04X}, expected 0x{expected:04X})")
                else:
                    print("OK")
            else:
                print("OK (no checksum in catalogue)")

            out_path = out_dir / basename
            out_path.write_bytes(data)
            print(f"  Saved to {out_path}")

        except Exception as e:
            print(f"FAILED: {e}")

    # Open output folder
    if not args.no_open:
        print(f"\nOpening {out_dir} ...")
        show_image(str(out_dir))

    return 0


def cmd_sd(args):
    """SD mode: browse images already on the SD card."""
    sd_path = Path(args.path)

    if not sd_path.exists():
        print(f"Error: Path not found: {sd_path}")
        print("Insert the SD card and find its mount point (e.g., /Volumes/SD_CARD/received)")
        return 1

    # Find all JPEG files
    jpgs = sorted(
        p for p in sd_path.rglob("*")
        if p.suffix.lower() in (".jpg", ".jpeg") and not p.name.startswith(".")
    )

    if not jpgs:
        print(f"No .jpg files found in {sd_path}")
        return 0

    print(f"Found {len(jpgs)} image(s) in {sd_path}\n")

    rows = []
    for i, p in enumerate(jpgs):
        stat = p.stat()
        rows.append({
            "idx": i,
            "name": p.name,
            "size": f"{stat.st_size:,}",
            "path": str(p.relative_to(sd_path)),
        })

    print_table(rows, [
        ("idx", "#"),
        ("name", "Filename"),
        ("size", "Size (B)"),
        ("path", "Path"),
    ])

    if args.list:
        return 0

    # Copy to output dir if requested
    if args.output:
        out_dir = Path(args.output)
        out_dir.mkdir(parents=True, exist_ok=True)
        for p in jpgs:
            dest = out_dir / p.name
            if not dest.exists():
                dest.write_bytes(p.read_bytes())
                print(f"  Copied {p.name} -> {dest}")
        if not args.no_open:
            show_image(str(out_dir))
    else:
        # Just open the SD path directly
        if not args.no_open:
            show_image(str(sd_path))

    return 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="IOT Forest Cam — Harvested Image Viewer"
    )
    sub = parser.add_subparsers(dest="mode", required=True)

    # CoAP mode
    p_coap = sub.add_parser("coap", help="Download images from a node via CoAP")
    p_coap.add_argument("--host", default="192.168.4.1",
                        help="Node IP address (default: 192.168.4.1)")
    p_coap.add_argument("--index", "-i", type=int, default=None,
                        help="Download only this image index (default: all)")
    p_coap.add_argument("--output", "-o", default="./downloaded_images",
                        help="Output directory (default: ./downloaded_images)")
    p_coap.add_argument("--list", "-l", action="store_true",
                        help="List images only, don't download")
    p_coap.add_argument("--no-open", action="store_true",
                        help="Don't open folder after download")

    # SD mode
    p_sd = sub.add_parser("sd", help="Browse images on a mounted SD card")
    p_sd.add_argument("--path", "-p", required=True,
                      help="Path to SD card's /received directory")
    p_sd.add_argument("--output", "-o", default=None,
                      help="Copy images to this directory")
    p_sd.add_argument("--list", "-l", action="store_true",
                      help="List images only, don't open")
    p_sd.add_argument("--no-open", action="store_true",
                      help="Don't open folder")

    args = parser.parse_args()

    if args.mode == "coap":
        return asyncio.run(cmd_coap(args))
    elif args.mode == "sd":
        return cmd_sd(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
