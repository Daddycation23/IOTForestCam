/**
 * @file TaskCoapServerLoop.cpp
 * @brief FreeRTOS Task: CoAP Server (Core 1) — Leaf/Relay mode
 *
 * Extracted from main.cpp. Contains taskCoapServerLoop().
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "Globals.h"

// =============================================================
// FreeRTOS Task: CoAP Server (Core 1) — Leaf/Relay mode
// =============================================================

/**
 * Leaf/Relay CoAP server task: serves images via CoAP Block2 transfer.
 * Also handles relay cached-image serving and cleanup.
 */
void taskCoapServerLoop(void* param) {
    for (;;) {
        // ── CoAP Server (leaf serves /images/) ───────────────
        coapServer.loop();

        // ── Relay: serve cached images to gateway ────────────
        if (_relayCachedServing) {
            if (millis() - _relayCachedStartMs >= RELAY_CACHED_TIMEOUT_MS) {
                Serial.println("[Relay] Cached serving timeout — cleaning up");
                relayCachedCleanup();
            } else {
                cachedCoapServer.loop();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
