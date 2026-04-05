#!/usr/bin/env python3
import json
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
from tkinter import filedialog

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
        self.title("🌲 IOT Forest Cam — Dashboard")
        self.geometry("1200x750")
        self.configure(bg="#f5f5f5")
        self.protocol("WM_DELETE_WINDOW", self.on_close)
        self.current_image_data = None  # Store current image data for download
        self.current_image_info = None  # Store current image info
        # Serialize CoAP operations — ESP32's CoAP server is single-threaded,
        # concurrent requests cause state confusion mid-block-transfer
        self._coap_busy = threading.Lock()
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
            # Skip polling while a download is in progress — ESP32 CoAP server
            # can't interleave /info with an ongoing block-wise /image transfer
            if self._coap_busy.acquire(blocking=False):
                try:
                    info = await self._fetch_info(self.coap_host)
                    self.after(0, self._update_image_list, info)
                except Exception as e:
                    print(f"[CoAP] Poll error: {e}")
                finally:
                    self._coap_busy.release()
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
            # Format: "0 │ image.jpg │ 22.1 KB │ 23 blocks"
            size_kb = img['size'] / 1024
            self.image_listbox.insert(tk.END, 
                f"{img['id']:2d} │ {os.path.basename(img['name']):40s} │ {size_kb:7.1f} KB │ {img['blocks']:3d} blocks")
        # Update image count badge
        if hasattr(self, 'image_count_label'):
            self.image_count_label.config(text=f"{len(self.images_info)} images")
        # Log
        self.log_text.config(state="normal")
        self.log_text.insert("end", f"[CoAP] Fetched info: {len(self.images_info)} images available\n")
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

        # Skip zero-byte / zero-block images (aborted transfers)
        if total_blocks == 0 or img_info.get('size', 0) == 0:
            self.after(0, self._set_transfer_status,
                       f"Skipped: {os.path.basename(img_info['name'])} (0 blocks)")
            self.after(0, self._log_message,
                       f"[Skip] {os.path.basename(img_info['name'])} — empty file")
            return

        # Acquire CoAP lock — wait up to 30s if another operation is running
        got_lock = self._coap_busy.acquire(timeout=30)
        if not got_lock:
            self.after(0, self._set_transfer_status, "Busy: another CoAP op in progress")
            return

        block_size = 1024  # Could use info['block_size']
        ctx = await aiocoap.Context.create_client_context()
        data = bytearray()
        block_num = 0

        # Initialize UI on main thread
        self.after(0, self._init_transfer_ui, total_blocks, os.path.basename(img_info['name']))

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
                    # Update progress on main thread
                    self.after(0, self._update_transfer_progress, block_num+1, total_blocks, len(data))
                    if response.opt.block2 and response.opt.block2.more:
                        block_num += 1
                    else:
                        break
                else:
                    self.after(0, self._set_transfer_status, f"Error: CoAP response {response.code}")
                    return
            # Display image on main thread
            self.after(0, self._set_transfer_status, f"Done: {os.path.basename(img_info['name'])}")
            self.after(0, self._display_image_data, data, img_info)
        except Exception as e:
            self.after(0, self._set_transfer_status, f"Error: {e}")
        finally:
            await ctx.shutdown()
            self._coap_busy.release()

    def _init_transfer_ui(self, total_blocks, filename):
        """Initialize progress bar and status (called on main thread)"""
        self.progress['maximum'] = total_blocks
        self.progress['value'] = 0
        self.harvest_label.config(text=f"Transferring {filename}...")
        self.transfer_label.config(text="Transferred: 0 B")
        self.last_file_label.config(text=f"Last file: 0/{total_blocks} blocks")
    
    def _update_transfer_progress(self, block, total, bytes_transferred):
        """Update progress bar (called on main thread)"""
        self.progress['value'] = block
        self.transfer_label.config(text=f"Transferred: {bytes_transferred} B")
        self.last_file_label.config(text=f"Last file: {block}/{total} blocks")
        # Debug log
        self.log_text.config(state="normal")
        self.log_text.insert("end", f"[Progress] Block {block}/{total} ({bytes_transferred} B)\n")
        self.log_text.see("end")
        self.log_text.config(state="disabled")

    def _set_transfer_status(self, msg):
        """Update status label (called on main thread)"""
        self.harvest_label.config(text=msg)
    
    def _download_current_image(self):
        """Download current displayed image to Downloads folder"""
        if not self.current_image_data:
            messagebox.showwarning("No Image", "No image loaded to download")
            return
        
        # Get Downloads folder path
        downloads_path = Path.home() / "Downloads"
        downloads_path.mkdir(exist_ok=True)
        
        # Determine filename
        if self.current_image_info:
            filename = os.path.basename(self.current_image_info.get('name', 'image.jpg'))
        else:
            filename = f"forestcam_image_{int(time.time())}.jpg"
        
        # Check if file exists, add counter if needed
        save_path = downloads_path / filename
        counter = 1
        base_name = Path(filename).stem
        ext = Path(filename).suffix
        while save_path.exists():
            save_path = downloads_path / f"{base_name}_{counter}{ext}"
            counter += 1
        
        try:
            save_path.write_bytes(self.current_image_data)
            messagebox.showinfo("Success", f"Image saved to:\n{save_path}")
            self.log_text.config(state="normal")
            self.log_text.insert("end", f"[Download] Saved {filename} to {save_path}\n")
            self.log_text.see("end")
            self.log_text.config(state="disabled")
        except Exception as e:
            messagebox.showerror("Download Error", f"Failed to save image: {e}")
    
    def _download_all_images(self):
        """Download all images from the list to Downloads folder"""
        if not hasattr(self, 'images_info') or not self.images_info:
            messagebox.showwarning("No Images", "No images available to download")
            return
        
        # Confirm download
        count = len(self.images_info)
        if not messagebox.askyesno("Download All", f"Download all {count} images to Downloads folder?"):
            return

        # Disable buttons while batch runs; re-enable on completion
        self._set_download_buttons_enabled(False)

        def run():
            try:
                asyncio.run(self._download_all_task())
            finally:
                self.after(0, self._set_download_buttons_enabled, True)
        threading.Thread(target=run, daemon=True).start()

    def _set_download_buttons_enabled(self, enabled: bool):
        """Enable/disable download buttons during CoAP operations."""
        new_state = "normal" if enabled else "disabled"
        if hasattr(self, 'download_btn') and self.current_image_data is not None:
            self.download_btn.config(state=new_state)
        if hasattr(self, 'download_all_btn'):
            self.download_all_btn.config(state=new_state)
    
    async def _download_all_task(self):
        """Download all images asynchronously"""
        # Acquire CoAP lock — hold it for the entire batch so polling and
        # individual image-preview clicks don't interleave
        got_lock = self._coap_busy.acquire(timeout=30)
        if not got_lock:
            self.after(0, self._set_transfer_status, "Busy: another CoAP op in progress")
            return

        try:
            downloads_path = Path.home() / "Downloads"
            downloads_path.mkdir(exist_ok=True)

            total = len(self.images_info)
            success_count = 0
            failed_count = 0
            skipped_count = 0

            self.after(0, self._set_transfer_status, f"Downloading {total} images...")

            import aiocoap

            for idx, img_info in enumerate(self.images_info):
                img_id = img_info['id']
                filename = os.path.basename(img_info.get('name', f'image_{img_id}.jpg'))

                # Skip zero-byte / zero-block images (aborted transfers)
                if img_info.get('blocks', 0) == 0 or img_info.get('size', 0) == 0:
                    skipped_count += 1
                    self.after(0, self._log_message, f"[Skip] {filename} — empty file")
                    continue

                self.after(0, self._set_transfer_status,
                           f"Downloading {idx+1}/{total}: {filename}")

                ctx = None
                try:
                    ctx = await aiocoap.Context.create_client_context()
                    data = bytearray()
                    block_num = 0

                    while True:
                        request = aiocoap.Message(
                            code=aiocoap.GET,
                            uri=f"coap://{self.coap_host}:5683/image/{img_id}"
                        )
                        request.opt.block2 = aiocoap.optiontypes.BlockOption.BlockwiseTuple(
                            block_num, False, 6
                        )
                        response = await asyncio.wait_for(
                            ctx.request(request).response, timeout=15)

                        if not response.code.is_successful():
                            raise Exception(f"CoAP error: {response.code}")

                        data.extend(response.payload)
                        # aiocoap may auto-assemble block-wise transfers; if there
                        # is no block2 option or more=False, we're done
                        if response.opt.block2 and response.opt.block2.more:
                            block_num += 1
                        else:
                            break

                    if len(data) == 0:
                        raise Exception("empty payload")

                    # Save file with collision-safe naming
                    save_path = downloads_path / filename
                    counter = 1
                    base_name = Path(filename).stem
                    ext = Path(filename).suffix
                    while save_path.exists():
                        save_path = downloads_path / f"{base_name}_{counter}{ext}"
                        counter += 1

                    save_path.write_bytes(data)
                    success_count += 1
                    self.after(0, self._log_message,
                               f"[Download] Saved {filename} ({len(data)} B)")

                except Exception as e:
                    failed_count += 1
                    self.after(0, self._log_message,
                               f"[Download] Failed {filename}: {e}")
                finally:
                    if ctx is not None:
                        try:
                            await ctx.shutdown()
                        except Exception:
                            pass

            summary = (f"Done: {success_count} OK, {failed_count} failed, "
                       f"{skipped_count} skipped")
            self.after(0, self._set_transfer_status, summary)
            self.after(0, messagebox.showinfo, "Download Complete",
                       f"{success_count}/{total} downloaded\n"
                       f"{skipped_count} skipped (empty files)\n"
                       f"{failed_count} failed\n\nSaved to: {downloads_path}")
        finally:
            self._coap_busy.release()
    
    def _log_message(self, msg):
        """Add message to event log"""
        self.log_text.config(state="normal")
        self.log_text.insert("end", f"{msg}\n")
        self.log_text.see("end")
        self.log_text.config(state="disabled")

    def _display_image_data(self, data, img_info=None):
        try:
            self.current_image_data = bytes(data)  # Store for download
            self.current_image_info = img_info  # Store metadata
            img = Image.open(io.BytesIO(data))
            img.thumbnail((400, 400))
            self.tk_img = ImageTk.PhotoImage(img)
            self.img_panel.config(image=self.tk_img, text="", bg="#f8f9fa")
            # Enable download button
            if hasattr(self, 'download_btn'):
                self.download_btn.config(state="normal")
        except Exception as e:
            messagebox.showerror("Image Error", f"Failed to open image: {e}")

    def _build_widgets(self):
        # Modern color scheme
        header_bg = "#1a5f7a"
        card_bg = "#ffffff"
        text_primary = "#2c3e50"
        text_secondary = "#7f8c8d"
        accent_green = "#27ae60"
        accent_blue = "#3498db"
        
        # Header with gradient-like effect
        header_frame = tk.Frame(self, bg=header_bg, height=80)
        header_frame.pack(fill=tk.X)
        header_frame.pack_propagate(False)
        
        header_label = tk.Label(header_frame, text="🌲 IOT Forest Cam", 
                               font=("Segoe UI", 24, "bold"), 
                               bg=header_bg, fg="white")
        header_label.pack(side=tk.LEFT, padx=30, pady=20)
        
        subtitle = tk.Label(header_frame, text="Real-time Image Harvesting Dashboard", 
                           font=("Segoe UI", 10), 
                           bg=header_bg, fg="#b3d4e0")
        subtitle.pack(side=tk.LEFT, padx=(0, 30), pady=20)

        # Main container with background
        container = tk.Frame(self, bg="#f5f5f5")
        container.pack(fill=tk.BOTH, expand=True, padx=20, pady=20)

        # Left panel: Harvest Progress & Controls
        left_panel = tk.Frame(container, bg=card_bg, relief=tk.RAISED, bd=1)
        left_panel.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 10))
        
        # Harvest status card
        status_card = tk.Frame(left_panel, bg=card_bg)
        status_card.pack(fill=tk.X, padx=20, pady=20)
        
        tk.Label(status_card, text="📊 Transfer Status", 
                font=("Segoe UI", 14, "bold"), 
                bg=card_bg, fg=text_primary, anchor="w").pack(fill=tk.X, pady=(0, 10))
        
        self.harvest_label = tk.Label(status_card, text="Ready to transfer", 
                                      font=("Segoe UI", 11), 
                                      bg=card_bg, fg=text_secondary, anchor="w")
        self.harvest_label.pack(fill=tk.X, pady=5)
        
        # Modern progress bar with custom style
        style = ttk.Style()
        style.theme_use('clam')
        style.configure("Custom.Horizontal.TProgressbar", 
                       troughcolor='#ecf0f1', 
                       bordercolor=card_bg, 
                       background=accent_green, 
                       lightcolor=accent_green, 
                       darkcolor=accent_green,
                       thickness=25)
        
        self.progress = ttk.Progressbar(status_card, 
                                       orient="horizontal", 
                                       length=500, 
                                       mode="determinate",
                                       style="Custom.Horizontal.TProgressbar")
        self.progress.pack(fill=tk.X, pady=10)
        
        # Stats row
        stats_frame = tk.Frame(status_card, bg=card_bg)
        stats_frame.pack(fill=tk.X, pady=5)
        
        self.transfer_label = tk.Label(stats_frame, text="Transferred: 0 B", 
                                       font=("Segoe UI", 9), 
                                       bg=card_bg, fg=text_secondary, anchor="w")
        self.transfer_label.pack(side=tk.LEFT)
        
        self.last_file_label = tk.Label(stats_frame, text="Blocks: 0/0", 
                                        font=("Segoe UI", 9), 
                                        bg=card_bg, fg=text_secondary, anchor="e")
        self.last_file_label.pack(side=tk.RIGHT)
        
        # Separator
        tk.Frame(left_panel, bg="#e0e0e0", height=1).pack(fill=tk.X, padx=20, pady=10)
        
        # Image list card
        list_card = tk.Frame(left_panel, bg=card_bg)
        list_card.pack(fill=tk.BOTH, expand=True, padx=20, pady=(0, 20))
        
        # Header with count
        list_header = tk.Frame(list_card, bg=card_bg)
        list_header.pack(fill=tk.X, pady=(0, 10))
        
        tk.Label(list_header, text="📁 Available Images", 
                font=("Segoe UI", 14, "bold"), 
                bg=card_bg, fg=text_primary, anchor="w").pack(side=tk.LEFT)
        
        self.image_count_label = tk.Label(list_header, text="0 images", 
                                          font=("Segoe UI", 10, "bold"), 
                                          bg="#e8f5e9", fg=accent_green, 
                                          padx=10, pady=3, relief=tk.FLAT)
        self.image_count_label.pack(side=tk.RIGHT)
        
        # Listbox with scrollbar
        list_container = tk.Frame(list_card, bg=card_bg)
        list_container.pack(fill=tk.BOTH, expand=True)
        
        scrollbar = tk.Scrollbar(list_container)
        scrollbar.pack(side=tk.RIGHT, fill=tk.Y)
        
        self.image_listbox = tk.Listbox(list_container, 
                                        font=("Consolas", 9),
                                        bg="#fafafa", 
                                        fg=text_primary,
                                        selectbackground=accent_blue,
                                        selectforeground="white",
                                        relief=tk.FLAT,
                                        bd=0,
                                        highlightthickness=1,
                                        highlightcolor=accent_blue,
                                        highlightbackground="#e0e0e0",
                                        yscrollcommand=scrollbar.set)
        self.image_listbox.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        scrollbar.config(command=self.image_listbox.yview)
        self.image_listbox.bind('<<ListboxSelect>>', self._on_image_select)
        
        # Download buttons
        button_frame = tk.Frame(list_card, bg=card_bg)
        button_frame.pack(fill=tk.X, pady=(15, 0))
        
        self.download_btn = tk.Button(button_frame, 
                                      text="⬇ Download Selected", 
                                      command=self._download_current_image,
                                      state="disabled", 
                                      bg=accent_green, 
                                      fg="white",
                                      font=("Segoe UI", 10, "bold"), 
                                      relief=tk.FLAT,
                                      padx=20, 
                                      pady=10,
                                      cursor="hand2",
                                      activebackground="#229954",
                                      activeforeground="white")
        self.download_btn.pack(side=tk.LEFT, padx=(0, 10))
        
        self.download_all_btn = tk.Button(button_frame, 
                                          text="⬇ Download All", 
                                          command=self._download_all_images,
                                          bg=accent_blue, 
                                          fg="white",
                                          font=("Segoe UI", 10, "bold"), 
                                          relief=tk.FLAT,
                                          padx=20, 
                                          pady=10,
                                          cursor="hand2",
                                          activebackground="#2874a6",
                                          activeforeground="white")
        self.download_all_btn.pack(side=tk.LEFT)

        # Right panel: Image Preview & Logs
        right_panel = tk.Frame(container, bg=card_bg, relief=tk.RAISED, bd=1)
        right_panel.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        
        # Image preview card
        preview_card = tk.Frame(right_panel, bg=card_bg)
        preview_card.pack(fill=tk.BOTH, expand=True, padx=20, pady=20)
        
        tk.Label(preview_card, text="🖼 Image Preview", 
                font=("Segoe UI", 14, "bold"), 
                bg=card_bg, fg=text_primary, anchor="w").pack(fill=tk.X, pady=(0, 15))
        
        # Image panel with border
        img_container = tk.Frame(preview_card, bg="#f8f9fa", 
                                relief=tk.SUNKEN, bd=1, 
                                highlightthickness=1, highlightbackground="#dee2e6")
        img_container.pack(fill=tk.BOTH, expand=True)
        
        self.img_panel = tk.Label(img_container, 
                                 text="Click an image to preview",
                                 font=("Segoe UI", 11),
                                 bg="#f8f9fa", 
                                 fg=text_secondary)
        self.img_panel.pack(expand=True, padx=40, pady=40)
        
        # Separator
        tk.Frame(right_panel, bg="#e0e0e0", height=1).pack(fill=tk.X, padx=20, pady=10)
        
        # Event log card
        log_card = tk.Frame(right_panel, bg=card_bg)
        log_card.pack(fill=tk.BOTH, expand=True, padx=20, pady=(0, 20))
        
        tk.Label(log_card, text="📝 Event Log", 
                font=("Segoe UI", 12, "bold"), 
                bg=card_bg, fg=text_primary, anchor="w").pack(fill=tk.X, pady=(0, 10))
        
        self.log_text = scrolledtext.ScrolledText(log_card, 
                                                  height=8, 
                                                  state="disabled", 
                                                  font=("Consolas", 8),
                                                  bg="#1e1e1e", 
                                                  fg="#d4d4d4",
                                                  relief=tk.FLAT,
                                                  bd=0,
                                                  padx=10,
                                                  pady=10)
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
