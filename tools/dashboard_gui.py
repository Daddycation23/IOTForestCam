import json
#!/usr/bin/env python3
"""
IOT Forest Cam — Full GUI Dashboard

A Tkinter-based GUI dashboard for real-time monitoring and image viewing.
- Reads gateway serial output (pyserial)
- Fetches images via CoAP (aiocoap)
- Displays node status, harvest progress, logs, and images in a single window

Requirements:
  pip install -r requirements.txt

Author: CS Group 2
"""

import tkinter as tk
from tkinter import ttk, scrolledtext, messagebox
from PIL import Image, ImageTk
import threading
import serial
import serial.tools.list_ports
import asyncio
import io
import re
import sys
import time
import queue
import os
from pathlib import Path

# --- State Classes ---
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

class DashboardState:
    def __init__(self):
        self.nodes = {}
        self.harvest = HarvestState()
        self.boot_count = 0
        self.role = "?"
        self.lora_ok = False
        self.sd_ok = False
        self.wifi_ssid = ""
        self.wifi_ip = ""
        self.uptime_s = 0
        self.last_beacon_tx = ""
        self.beacon_tx_count = 0
        self.election_state = ""
        self.log_lines = []
        self.max_log_lines = 15
        self.total_harvested = 0
        self.sd_images = 0
        self.coap_ok = False
        self.boot_time = None
        self.deep_sleep_timer = ""
        self.node_ssid = ""

# --- GUI Skeleton ---
class ForestCamDashboard(tk.Tk):
    def __init__(self, state: DashboardState):
        super().__init__()
        self.state = state
        self.title("IOT Forest Cam — GUI Dashboard")
        self.geometry("1100x700")
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self._build_widgets()

        # Start CoAP polling
        self.coap_host = "192.168.4.1"  # TODO: Make configurable
        self.poll_interval = 5  # seconds
        self._start_coap_polling()

    def _start_coap_polling(self):
        def run_poll():
            asyncio.run(self._poll_coap_loop())
        t = threading.Thread(target=run_poll, daemon=True)
        t.start()

    async def _poll_coap_loop(self):
        while True:
            try:
                info = await self._fetch_info(self.coap_host)
                self.after(0, self._update_image_list, info)
            except Exception as e:
                print(f"[CoAP] Poll error: {e}")
            await asyncio.sleep(self.poll_interval)

    async def _fetch_info(self, host):
        import aiocoap
        ctx = await aiocoap.Context.create_client_context()
        try:
            request = aiocoap.Message(
                code=aiocoap.GET,
                uri=f"coap://{host}:5683/info"
            )
            response = await asyncio.wait_for(ctx.request(request).response, timeout=10)
            return json.loads(response.payload.decode("utf-8"))
        finally:
            await ctx.shutdown()

    def _update_image_list(self, info):
        # Update image list panel
        self.images_info = info.get("images", [])
        self.image_listbox.delete(0, tk.END)
        for img in self.images_info:
            self.image_listbox.insert(tk.END, f"{img['id']}: {os.path.basename(img['name'])} ({img['size']} B, {img['blocks']} blocks)")
        # Update image count label
        if hasattr(self, 'image_count_label'):
            self.image_count_label.config(text=f"Images transferred: {len(self.images_info)}")
        # Log
        self.log_text.config(state="normal")
        self.log_text.insert("end", f"[CoAP] /info: {info}\n")
        self.log_text.see("end")
        self.log_text.config(state="disabled")

    def _on_image_select(self, event):
        if not self.image_listbox.curselection():
            return
        idx = self.image_listbox.curselection()[0]
        img_info = self.images_info[idx]
        self._fetch_and_display_image(img_info)

    def _fetch_and_display_image(self, img_info):
        # Start async image fetch in thread
        def run():
            asyncio.run(self._fetch_image_task(img_info))
        threading.Thread(target=run, daemon=True).start()

    async def _fetch_image_task(self, img_info):
        import aiocoap
        host = self.coap_host
        img_id = img_info['id']
        total_blocks = img_info['blocks']
        block_size = 1024  # Could use info['block_size']
        ctx = await aiocoap.Context.create_client_context()
        data = bytearray()
        block_num = 0
        self._set_transfer_status(f"Transferring {os.path.basename(img_info['name'])}...")
        self.progress['maximum'] = total_blocks
        self.progress['value'] = 0
        try:
            while True:
                block2_value = (block_num << 4) | 6  # SZX=6 (1024)
                request = aiocoap.Message(
                    code=aiocoap.GET,
                    uri=f"coap://{host}:5683/image/{img_id}"
                )
                request.opt.block2 = aiocoap.optiontypes.BlockOption.BlockwiseTuple(
                    block_num, False, 6
                )
                response = await asyncio.wait_for(ctx.request(request).response, timeout=15)
                if response.code.is_successful():
                    data.extend(response.payload)
                    self.after(0, self._update_transfer_progress, block_num+1, total_blocks, len(data))
                    if response.opt.block2 and response.opt.block2.more:
                        block_num += 1
                    else:
                        break
                else:
                    self._set_transfer_status(f"Error: CoAP response {response.code}")
                    return
            # Display image
            self._set_transfer_status(f"Done: {os.path.basename(img_info['name'])}")
            self._display_image_data(data)
        except Exception as e:
            self._set_transfer_status(f"Error: {e}")
        finally:
            await ctx.shutdown()

    def _update_transfer_progress(self, block, total, bytes_transferred):
        self.progress['value'] = block
        self.transfer_label.config(text=f"Transferred: {bytes_transferred} B")
        self.last_file_label.config(text=f"Last file: {block}/{total} blocks")

    def _set_transfer_status(self, msg):
        self.harvest_label.config(text=msg)

    def _display_image_data(self, data):
        try:
            img = Image.open(io.BytesIO(data))
            img.thumbnail((400, 400))
            self.tk_img = ImageTk.PhotoImage(img)
            self.img_panel.config(image=self.tk_img)
        except Exception as e:
            messagebox.showerror("Image Error", f"Failed to open image: {e}")

    def _build_widgets(self):
        # Header
        header = tk.Label(self, text="IOT Forest Cam — Live Dashboard", font=("Arial", 18, "bold"), bg="#1e3a5c", fg="white", pady=10)
        header.pack(fill=tk.X)

        # Main panels
        main_frame = tk.Frame(self)
        main_frame.pack(fill=tk.BOTH, expand=True)

        # (Discovered nodes section removed)

        # Center: Harvest Progress & Image
        center = tk.Frame(main_frame)
        center.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=10, pady=10)
        tk.Label(center, text="Harvest Status", font=("Arial", 12, "bold")).pack(anchor="w")
        self.harvest_label = tk.Label(center, text="Phase: IDLE", font=("Arial", 11))
        self.harvest_label.pack(anchor="w")
        self.progress = ttk.Progressbar(center, orient="horizontal", length=400, mode="determinate")
        self.progress.pack(anchor="w", pady=5)
        self.transfer_label = tk.Label(center, text="Transferred: 0 KB", font=("Arial", 10))
        self.transfer_label.pack(anchor="w")
        self.last_file_label = tk.Label(center, text="Last file: ", font=("Arial", 10))
        self.last_file_label.pack(anchor="w")
        # Image count label
        self.image_count_label = tk.Label(center, text="Images transferred: 0", font=("Arial", 11, "bold"))
        self.image_count_label.pack(anchor="w", pady=(10,0))
        # Image list
        tk.Label(center, text="Available Images", font=("Arial", 11, "bold")).pack(anchor="w", pady=(5,0))
        self.image_listbox = tk.Listbox(center, width=50, height=8)
        self.image_listbox.pack(anchor="w", fill=tk.X, pady=2)
        self.image_listbox.bind('<<ListboxSelect>>', self._on_image_select)
        # ...existing code...
        # ...existing code...
        # Image display area
        self.img_panel = tk.Label(center)
        self.img_panel.pack(anchor="center", pady=10)

        # Right: Log
        right = tk.Frame(main_frame)
        right.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=10, pady=10)
        tk.Label(right, text="Event Log", font=("Arial", 12, "bold")).pack(anchor="w")
        self.log_text = scrolledtext.ScrolledText(right, width=50, height=30, state="disabled", font=("Consolas", 9))
        self.log_text.pack(fill=tk.BOTH, expand=True)

    def on_close(self):
        self.destroy()

# --- Image Folder Panel ---
class ImageFolderPanel(tk.Frame):
    def __init__(self, master, folder_path):
        super().__init__(master)
        self.folder_path = Path(folder_path)
        self.image_list = []
        self.listbox = tk.Listbox(self, width=40, height=25)
        self.listbox.pack(side=tk.LEFT, fill=tk.Y, padx=5, pady=5)
        self.scrollbar = tk.Scrollbar(self, orient="vertical", command=self.listbox.yview)
        self.scrollbar.pack(side=tk.LEFT, fill=tk.Y)
        self.listbox.config(yscrollcommand=self.scrollbar.set)
        self.listbox.bind('<<ListboxSelect>>', self.on_select)
        self.img_panel = tk.Label(self)
        self.img_panel.pack(side=tk.LEFT, padx=10, pady=5)
        self.refresh_button = tk.Button(self, text="Refresh", command=self.refresh)
        self.refresh_button.pack(side=tk.BOTTOM, pady=5)
        self.refresh()

    def refresh(self):
        self.image_list = sorted([f for f in self.folder_path.glob('*.jpg')])
        self.listbox.delete(0, tk.END)
        for img in self.image_list:
            self.listbox.insert(tk.END, img.name)
        self.img_panel.config(image='')

    def on_select(self, event):
        if not self.listbox.curselection():
            return
        idx = self.listbox.curselection()[0]
        img_path = self.image_list[idx]
        try:
            img = Image.open(img_path)
            img.thumbnail((400, 400))
            self.tk_img = ImageTk.PhotoImage(img)
            self.img_panel.config(image=self.tk_img)
        except Exception as e:
            messagebox.showerror("Image Error", f"Failed to open image: {e}")

# --- Main Entry ---
if __name__ == "__main__":
    state = DashboardState()
    app = ForestCamDashboard(state)
    app.mainloop()
