/**
 * @file main.cpp
 * @brief IOT Forest Cam — Main Entry Point
 *
 * On wake-up (from deep sleep or power-on):
 *   1. Init Serial
 *   2. Mount SD card via StorageReader
 *   3. "Capture" a simulated image (read from SD)
 *   4. Stream all blocks (ready for Part 3 CoAP hand-off)
 *   5. Tear down SD
 *   6. Enter deep sleep (Part 2 will provide LoRa wake-up)
 *
 * @author  CS Group 19
 * @date    2026
 */

#include <Arduino.h>
#include "StorageReader.h"

// ──────────────── Configuration ──────────────────────────────
/** Which image index to "capture" each cycle. Wraps on reboot. */
static RTC_DATA_ATTR uint8_t captureIndex = 0;

/** Deep-sleep duration in microseconds (used until Part 2 LoRa wake). */
static constexpr uint64_t SLEEP_DURATION_US = 60ULL * 1000000ULL;  // 60 s

// ──────────────── Globals ────────────────────────────────────
StorageReader sensor;

// ──────────────── Forward Declarations ───────────────────────
void simulateCaptureCycle();
void enterDeepSleep();

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
void setup() {
    Serial.begin(115200);
    delay(500);  // Allow USB-CDC to settle on ESP32-S3

    Serial.println();
    Serial.println("========================================");
    Serial.println("  IOT Forest Cam — Storage Reader Demo");
    Serial.println("  CS Group 19 — Part 1");
    Serial.println("========================================");

    // Print wake-up reason
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    switch (cause) {
        case ESP_SLEEP_WAKEUP_TIMER:
            Serial.println("[BOOT] Woke from deep sleep (timer)");
            break;
        case ESP_SLEEP_WAKEUP_EXT0:
        case ESP_SLEEP_WAKEUP_EXT1:
            Serial.println("[BOOT] Woke from deep sleep (external — LoRa?)");
            break;
        default:
            Serial.println("[BOOT] Power-on / reset");
            break;
    }

    simulateCaptureCycle();

    // ── Power Down ──
    enterDeepSleep();
}

void loop() {
    // Never reached — we deep-sleep at end of setup().
    // Kept empty for Arduino framework compliance.
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Capture Cycle
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void simulateCaptureCycle() {
    // 1 ── Mount SD ──────────────────────────────────────────
    Serial.println("\n[SENSOR] Mounting SD card...");
    if (!sensor.begin()) {
        Serial.println("[SENSOR] *** SD init failed — skipping cycle ***");
        return;
    }

    uint8_t count = sensor.imageCount();
    Serial.printf("[SENSOR] %u image(s) available on SD\n", count);

    if (count == 0) {
        Serial.println("[SENSOR] No images found — nothing to send.");
        sensor.end();
        return;
    }

    // 2 ── Select image (round-robin across deep-sleep cycles) ─
    uint8_t idx = captureIndex % count;
    captureIndex++;  // Persists across deep-sleep via RTC memory

    ImageInfo info;
    if (!sensor.getImageInfo(idx, info)) {
        Serial.println("[SENSOR] Failed to read image info");
        sensor.end();
        return;
    }

    Serial.printf("[SENSOR] Capturing: %s (%lu B, %lu blocks)\n",
                  info.filename, info.fileSize, info.totalBlocks);

    // 3 ── Compute integrity checksum ────────────────────────
    uint16_t checksum = sensor.computeChecksum(idx);
    Serial.printf("[SENSOR] Fletcher-16 checksum: 0x%04X\n", checksum);

    // 4 ── Stream all blocks ─────────────────────────────────
    if (!sensor.openImage(idx)) {
        Serial.println("[SENSOR] Failed to open image for streaming");
        sensor.end();
        return;
    }

    uint32_t totalBytesRead = 0;
    BlockReadResult block;

    Serial.println("[SENSOR] Streaming blocks:");
    unsigned long t0 = millis();

    while (sensor.readNextBlock(block)) {
        totalBytesRead += block.length;

        // Print progress every 10 blocks to avoid serial flood
        if (block.blockIndex % 10 == 0 || block.isLast) {
            Serial.printf("  Block %4lu/%lu  [%3u B]%s\n",
                          block.blockIndex,
                          info.totalBlocks,
                          block.length,
                          block.isLast ? "  ◄ LAST" : "");
        }

        // ═══════════════════════════════════════════════════════
        // HAND-OFF POINT for Part 3:
        //   Pass block.data / block.length / block.blockIndex
        //   to CoAP Block2 transfer here.
        // ═══════════════════════════════════════════════════════
    }

    unsigned long elapsed = millis() - t0;
    Serial.printf("[SENSOR] Done — %lu bytes in %lu ms (%.1f KB/s)\n",
                  totalBytesRead, elapsed,
                  (elapsed > 0) ? (totalBytesRead / 1024.0f) / (elapsed / 1000.0f) : 0.0f);

    // 5 ── Demonstrate random-access re-read (retransmit sim) ─
    if (info.totalBlocks > 1) {
        Serial.println("[SENSOR] Retransmit test — re-reading block 0...");
        if (sensor.readBlock(0, block)) {
            Serial.printf("  Block 0 re-read OK (%u bytes)\n", block.length);
        }
    }

    // 6 ── Clean up SD ───────────────────────────────────────
    sensor.closeImage();
    sensor.end();
    Serial.println("[SENSOR] SD released.");
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Deep Sleep
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void enterDeepSleep() {
    Serial.println("\n[POWER] Preparing for deep sleep...");

    // Part 2 will add: enable LoRa Rx + configure EXT1 wake on DIO1
    // For now, use a timer wake-up as placeholder.
    esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);

    Serial.printf("[POWER] Sleeping for %llu seconds. Goodnight.\n",
                  SLEEP_DURATION_US / 1000000ULL);
    Serial.flush();

    esp_deep_sleep_start();
    // ── Execution stops here; resumes at setup() on wake ──
}