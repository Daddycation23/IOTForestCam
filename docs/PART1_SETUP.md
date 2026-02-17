# Part 1 — Virtual Sensor: SD Card Setup

## SD Card Preparation

1. **Format** a MicroSD card as **FAT32**.
2. Create a directory at the root called `/images/`.
3. Copy one or more JPEG files into `/images/`:
   ```
   /images/
       img_001.jpg
       img_002.jpg
       img_003.jpg
   ```
   - Any standard `.jpg` or `.jpeg` file works.
   - Recommended size: **10–100 KB** each (realistic for low-res forest cam).
   - Maximum **32 images** will be catalogued.

## Wiring: ESP32-S3 ↔ MicroSD Module (SPI)

| MicroSD Module | ESP32-S3 GPIO | Notes           |
|:--------------:|:-------------:|:----------------|
| VCC            | 3V3           | 3.3 V supply    |
| GND            | GND           |                 |
| MOSI           | GPIO 11       |                 |
| MISO           | GPIO 13       |                 |
| SCK / CLK      | GPIO 12       |                 |
| CS             | GPIO 10       | Configurable    |

> **Tip:** If your wires are longer than 15 cm, reduce `VSENSOR_SPI_FREQ` from 4 MHz to 1 MHz in `VirtualSensor.h`.

## Pin Conflicts

The SPI pins above are on the **FSPI** bus.  
Part 2's LoRa module (SX1262/SX1278) should be on the **HSPI** bus to avoid conflicts:

| SPI Bus | Usage         | Pins (default)           |
|---------|---------------|--------------------------|
| FSPI    | SD Card       | MOSI=11, MISO=13, CLK=12 |
| HSPI    | LoRa Module   | MOSI=35, MISO=37, CLK=36 |

## Quick Test

1. Prepare the SD card as above.
2. Flash the firmware: `pio run -t upload`
3. Open serial monitor: `pio device monitor`
4. Expected output:
   ```
   ========================================
     IOT Forest Cam — Virtual Sensor Demo
     CS Group 19 — Part 1
   ========================================
   [BOOT] Power-on / reset
   [SENSOR] Mounting SD card...
   [SENSOR] 3 image(s) available on SD
   [SENSOR] Capturing: /images/img_001.jpg (45678 B, 90 blocks)
   [SENSOR] Fletcher-16 checksum: 0xA3F1
   [SENSOR] Streaming blocks:
     Block    0/90  [512 B]
     Block   10/90  [512 B]
     ...
     Block   89/90  [134 B]  ◄ LAST
   [SENSOR] Done — 45678 bytes in 82 ms (543.7 KB/s)
   [SENSOR] SD released.
   [POWER] Sleeping for 60 seconds. Goodnight.
   ```

## Integration Points for Other Parts

### Part 2 (LoRa Control Plane)
Replace the timer wake-up in `enterDeepSleep()` with `esp_sleep_enable_ext1_wakeup()` on the LoRa DIO1 pin.

### Part 3 (CoAP Data Plane)
In `main.cpp`, the comment block `HAND-OFF POINT` marks where each `BlockReadResult` should be passed to a CoAP Block2 message builder. The fields map directly:

| BlockReadResult field | CoAP Block2 option      |
|----------------------|--------------------------|
| `block.data`         | Payload                  |
| `block.length`       | Payload length           |
| `block.blockIndex`   | Block NUM                |
| `block.isLast`       | M (More) flag = !isLast  |

Use `readBlock(index)` for retransmitting a specific block on CoAP timeout/NACK.
