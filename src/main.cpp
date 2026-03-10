/**
 * @file main.cpp
 * @brief IOT Forest Cam — Parts 1 + 2 Integration
 *
 * LILYGO T3-S3 V1.2
 *
 * Boot Flow
 * ─────────
 *  Power-on / Reset
 *    → Scan SD card (demo)
 *    → Init LoRa, start RX
 *    → Deep sleep (wakes on DIO1)
 *
 *  LoRa Wakeup (DIO1 EXT0)
 *    → Init LoRa, start RX
 *    → Wait up to 3 s for command packet (sender sends it ~500 ms after wake)
 *    → Resolve image index from command
 *    → Mount SD, stream image blocks   ← PART 3 HAND-OFF POINT
 *    → Unmount SD
 *    → Re-arm LoRa RX → deep sleep
 *
 * @author  CS Group 19
 * @date    2026
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "StorageReader.h"
#include "LoRaController.h"

// ─── OLED (I2C) ───────────────────────────────────────────────
#define OLED_SDA 18
#define OLED_SCL 17

static Adafruit_SSD1306 display(128, 64, &Wire, -1);

// ─── Module Instances ─────────────────────────────────────────
static StorageReader  storage;
static LoRaController lora;

// ─── RTC Memory (survives deep sleep) ─────────────────────────
RTC_DATA_ATTR static uint32_t bootCount  = 0;
RTC_DATA_ATTR static int8_t   lastImgIdx = -1;  ///< last image sent (for cycling)

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

/** Write up to four lines to the OLED display. */
static void oledMsg(const char* line1,
                    const char* line2 = "",
                    const char* line3 = "",
                    const char* line4 = "") {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(line1);
    if (line2[0]) display.println(line2);
    if (line3[0]) display.println(line3);
    if (line4[0]) display.println(line4);
    display.display();
}

/**
 * Search the SD catalogue for an image whose filename (lowercased)
 * contains the given keyword (also lowercased).
 * @return 0-based catalogue index, or -1 if not found.
 */
static int8_t findImageByKeyword(const char* keyword) {
    String kw = String(keyword);
    kw.toLowerCase();

    for (uint8_t i = 0; i < storage.imageCount(); i++) {
        ImageInfo info;
        if (storage.getImageInfo(i, info)) {
            String fn = String(info.filename);
            fn.toLowerCase();
            if (fn.indexOf(kw) >= 0) return static_cast<int8_t>(i);
        }
    }
    return -1;
}

/**
 * Stream every 512-byte block of the chosen image over Serial.
 *
 * ══════════════════════════════════════════════════════════════
 *  PART 3 HAND-OFF POINT
 *  Replace (or supplement) the Serial.printf() inside the while
 *  loop with your CoAP Block2 sender call, for example:
 *
 *      coap.sendBlock(block);
 *
 *  The BlockReadResult fields map directly to CoAP Block2:
 *    block.data       → CoAP payload
 *    block.length     → payload length
 *    block.blockIndex → Block NUM field
 *    block.isLast     → M (More) flag = !isLast
 *
 *  Use storage.readBlock(index, block) to retransmit a specific
 *  block on CoAP timeout/NACK.
 * ══════════════════════════════════════════════════════════════
 */
static void streamImageBlocks(uint8_t imgIdx) {
    if (!storage.openImage(imgIdx)) {
        log_e("main: openImage(%u) failed", imgIdx);
        return;
    }

    ImageInfo info;
    storage.getImageInfo(imgIdx, info);

    Serial.printf("[STREAM] %s — %lu bytes, %lu blocks\n",
                  info.filename, info.fileSize, info.totalBlocks);

    // Show progress on OLED (strip "/images/" prefix for brevity)
    const char* shortName = (strncmp(info.filename, "/images/", 8) == 0)
                            ? info.filename + 8
                            : info.filename;
    char blkStr[24];
    snprintf(blkStr, sizeof(blkStr), "%lu blocks x 512B", info.totalBlocks);
    oledMsg("Streaming...", shortName, blkStr);

    BlockReadResult block;
    uint32_t bytesSent = 0;
    uint32_t t0 = millis();

    while (storage.readNextBlock(block)) {

        // ── PART 3 HAND-OFF ──────────────────────────────────
        // coap.sendBlock(block);   ← Part 3 inserts CoAP call here
        // ─────────────────────────────────────────────────────

        bytesSent += block.length;

        if (block.blockIndex % 10 == 0 || block.isLast) {
            Serial.printf("  Block %4lu/%lu  [%3zu B]%s\n",
                          block.blockIndex,
                          info.totalBlocks,
                          block.length,
                          block.isLast ? "  ◄ LAST" : "");
        }
    }

    uint32_t elapsed = millis() - t0;
    storage.closeImage();

    Serial.printf("[STREAM] Done — %lu bytes in %lu ms (%.1f KB/s)\n",
                  bytesSent, elapsed,
                  elapsed ? (bytesSent / 1024.0f) / (elapsed / 1000.0f) : 0.0f);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// setup()
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void setup() {
    Serial.begin(115200);
    bootCount++;

    // Init OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    const bool loraWake = LoRaController::wasWokenByLoRa();

    Serial.printf("\n========================================\n");
    Serial.printf("  IOT Forest Cam  (boot #%lu)\n", bootCount);
    Serial.printf("  Wakeup: %s\n", loraWake ? "LoRa DIO1" : "Power-on / Reset");
    Serial.printf("========================================\n");

    // ══════════════════════════════════════════════════════════
    // PATH A — LoRa Wakeup
    // ══════════════════════════════════════════════════════════
    if (loraWake) {
        oledMsg("LoRa Wakeup!", "Init radio...");

        // 1. Init radio and start RX so we can receive the command packet.
        if (!lora.begin()) {
            log_e("main: LoRa begin() failed after wakeup");
            oledMsg("LoRa FAIL", "Sleeping again...");
            delay(1000);
            // Fall through to sleep below without a valid command.
            goto arm_sleep;
        }

        if (!lora.startReceive()) {
            log_e("main: LoRa startReceive() failed");
            oledMsg("LoRa RX FAIL", "Sleeping again...");
            delay(1000);
            goto arm_sleep;
        }

        oledMsg("LoRa Wakeup!", "Waiting for cmd", "(3 s timeout)");

        // 2. Wait for the actual command packet (sender sends ~500 ms after
        //    the initial wake packet that triggered DIO1).
        ParsedCommand cmd;
        bool gotCmd = lora.waitForCommand(cmd, 3000);

        if (!gotCmd) {
            // No command received — default to WAKE_RANDOM behaviour.
            log_w("main: No command received, defaulting to WAKE_RANDOM");
            cmd.cmd  = LoRaCmd::WAKE_RANDOM;
            cmd.arg  = 0;
            cmd.rssi = 0.0f;
            cmd.snr  = 0.0f;
        }

        Serial.printf("[LoRa] CMD=%d ARG=%u RSSI=%.1f dBm SNR=%.1f dB\n",
                      static_cast<int>(cmd.cmd), cmd.arg, cmd.rssi, cmd.snr);

        // 3. Mount SD card.
        char rssiStr[28];
        snprintf(rssiStr, sizeof(rssiStr), "RSSI:%.0f SNR:%.0f Mnt SD...",
                 cmd.rssi, cmd.snr);
        oledMsg("CMD recv!", rssiStr);

        if (!storage.begin()) {
            log_e("main: SD init failed after LoRa wakeup");
            oledMsg("SD FAIL", "Nothing to send");
            delay(2000);
            goto arm_sleep;
        }

        // 4. Resolve catalogue index from the decoded command.
        int8_t imgIdx = -1;

        switch (cmd.cmd) {
            case LoRaCmd::WAKE_RANDOM:
                if (storage.imageCount() > 0) {
                    // Cycle through images in order across sleep cycles.
                    imgIdx = static_cast<int8_t>(
                        (static_cast<uint8_t>(lastImgIdx) + 1)
                        % storage.imageCount());
                }
                break;

            case LoRaCmd::SEND_FIRE:
                imgIdx = findImageByKeyword("fire");
                if (imgIdx < 0) {
                    log_w("main: 'fire' image not found, falling back to index 0");
                    imgIdx = 0;
                }
                break;

            case LoRaCmd::SEND_ANIMAL:
                imgIdx = findImageByKeyword("animal");
                if (imgIdx < 0) {
                    log_w("main: 'animal' image not found, falling back to index 1");
                    imgIdx = (storage.imageCount() > 1) ? 1 : 0;
                }
                break;

            case LoRaCmd::SEND_INDEX:
                imgIdx = (cmd.arg < storage.imageCount())
                         ? static_cast<int8_t>(cmd.arg)
                         : 0;
                break;

            default:
                imgIdx = 0;
                break;
        }

        if (imgIdx < 0 || storage.imageCount() == 0) {
            log_e("main: No images available on SD");
            oledMsg("No images!", "SD empty?");
            storage.end();
            delay(2000);
            goto arm_sleep;
        }

        // 5. Stream the image blocks (→ Part 3 CoAP sender).
        lastImgIdx = imgIdx;

        ImageInfo info;
        storage.getImageInfo(static_cast<uint8_t>(imgIdx), info);
        Serial.printf("[main] Sending image[%d]: %s\n", imgIdx, info.filename);

        streamImageBlocks(static_cast<uint8_t>(imgIdx));

        // 6. Clean up SD before sleeping.
        storage.end();

        oledMsg("Done!", "Returning to", "deep sleep...");
        delay(500);

        goto arm_sleep;
    }

    // ══════════════════════════════════════════════════════════
    // PATH B — Power-on / Reset
    // ══════════════════════════════════════════════════════════
    {
        oledMsg("IOT Forest Cam", "T3-S3 V1.2", "Booting...");

        // Demonstrate that the SD / StorageReader (Part 1) is working.
        Serial.println("[SENSOR] Mounting SD card...");
        if (storage.begin()) {
            Serial.printf("[SENSOR] %u image(s) found\n", storage.imageCount());

            char imgStr[24];
            snprintf(imgStr, sizeof(imgStr), "Images: %u", storage.imageCount());
            oledMsg("SD Card OK!", imgStr, "Starting LoRa...");
            storage.end();
        } else {
            oledMsg("SD Card FAIL", "No images found", "Starting LoRa...");
            Serial.println("[SENSOR] SD init failed (no card or no images)");
        }
    }

    // ══════════════════════════════════════════════════════════
    // Common: Init LoRa, start RX, arm wakeup, sleep
    // ══════════════════════════════════════════════════════════
arm_sleep:
    Serial.println("[LoRa] Initialising SX1262...");

    if (!lora.isReady()) {
        // Not yet initialised (power-on path, or failed LoRa wakeup path
        // before begin() succeeded).
        if (!lora.begin()) {
            oledMsg("LoRa FAIL!", "Check wiring.", "System halted.");
            Serial.println("[LoRa] Init FAILED — check wiring. Halted.");
            while (true) delay(1000);
        }
    }

    if (!lora.startReceive()) {
        oledMsg("LoRa RX FAIL", "Halted.");
        Serial.println("[LoRa] startReceive() failed. Halted.");
        while (true) delay(1000);
    }

    char bootStr[24];
    snprintf(bootStr, sizeof(bootStr), "Boot #%lu", bootCount);
    oledMsg("LoRa Listening", "Waiting for", "0xA1 wake cmd", bootStr);

    Serial.println("[LoRa] Listening — entering deep sleep. Goodnight.");
    lora.enterDeepSleepRx();
    // ── Does not return ──
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// loop() — never reached; device sleeps in setup()
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void loop() {}
