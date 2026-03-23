/**
 * @file TaskHarvestGateway.cpp
 * @brief FreeRTOS Task: Harvest (Core 1) — Gateway mode
 *
 * Extracted from main.cpp. Contains taskHarvestGateway().
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "Globals.h"

// =============================================================
// FreeRTOS Task: Harvest (Core 1) — Gateway mode
// =============================================================

/**
 * Gateway harvest task: waits for harvest trigger from Core 0,
 * then runs the harvest state machine until completion.
 * WiFi connect + CoAP download happen here without blocking LoRa.
 */
void taskHarvestGateway(void* param) {
    for (;;) {
        // ── Check announce queue (leaf-initiated harvest) ─────
        AnnounceMessage announce;
        bool hasAnnounce = false;
        while (xQueueReceive(xAnnounceQueue, &announce, 0) == pdTRUE) {
            if (registryLock()) {
                registry.updateFromAnnounce(announce.nodeId, announce.ip, announce.imageCount);
                registryUnlock();
            }
            hasAnnounce = true;
        }

        if (hasAnnounce && harvestLoop.state() == HARVEST_IDLE) {
            Serial.println("[Harvest Task] Announce-triggered harvest on Core 1");
            harvestLoop.startCycle();
            while (harvestLoop.state() != HARVEST_IDLE) {
                harvestLoop.tick();
                // Drain any new announces that arrive during harvest
                AnnounceMessage late;
                while (xQueueReceive(xAnnounceQueue, &late, 0) == pdTRUE) {
                    if (registryLock()) {
                        registry.updateFromAnnounce(late.nodeId, late.ip, late.imageCount);
                        registryUnlock();
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            Serial.println("[Harvest Task] Harvest cycle complete");
            continue;
        }

        // ── Legacy: block on harvest command queue ────────────
        uint8_t cmd;
        if (xQueueReceive(xHarvestCmdQueue, &cmd, pdMS_TO_TICKS(500)) == pdTRUE) {
            Serial.println("[Harvest Task] Starting harvest cycle on Core 1");
            harvestLoop.startCycle();
            while (harvestLoop.state() != HARVEST_IDLE) {
                harvestLoop.tick();
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            Serial.println("[Harvest Task] Harvest cycle complete");
        }
    }
}
