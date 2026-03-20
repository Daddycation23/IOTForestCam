/**
 * @file TaskConfig.cpp
 * @brief FreeRTOS primitive initialization and thread-safe helpers
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "TaskConfig.h"
#include "LoRaRadio.h"
#include "AodvPacket.h"    // HarvestCmdPacket — needed for relay harvest queue item size

// ─── Extern reference to the global LoRaRadio (defined in main.cpp) ─
extern LoRaRadio loraRadio;

// ─── FreeRTOS Handle Definitions ─────────────────────────────
SemaphoreHandle_t  xLoRaMutex       = nullptr;
SemaphoreHandle_t  xRegistryMutex   = nullptr;
QueueHandle_t      xLoRaTxQueue     = nullptr;
QueueHandle_t      xHarvestCmdQueue = nullptr;
QueueHandle_t      xRelayHarvestQueue = nullptr;
EventGroupHandle_t xHarvestEvents   = nullptr;

TaskHandle_t hTaskLoRa        = nullptr;
TaskHandle_t hTaskHarvest     = nullptr;
TaskHandle_t hTaskRelayHarvest = nullptr;
TaskHandle_t hTaskCoapServer  = nullptr;
TaskHandle_t hTaskOLED        = nullptr;

// ─── Initialization ──────────────────────────────────────────

void initRTOS() {
    xLoRaMutex     = xSemaphoreCreateMutex();
    xRegistryMutex = xSemaphoreCreateMutex();

    xLoRaTxQueue       = xQueueCreate(LORA_TX_QUEUE_SIZE, sizeof(LoRaTxRequest));
    xHarvestCmdQueue   = xQueueCreate(HARVEST_CMD_QUEUE_SIZE, sizeof(uint8_t));
    xRelayHarvestQueue = xQueueCreate(RELAY_HARVEST_QUEUE_SIZE, sizeof(HarvestCmdPacket));
    xHarvestEvents     = xEventGroupCreate();

    configASSERT(xLoRaMutex);
    configASSERT(xRegistryMutex);
    configASSERT(xLoRaTxQueue);
    configASSERT(xHarvestCmdQueue);
    configASSERT(xRelayHarvestQueue);
    configASSERT(xHarvestEvents);

    Serial.println("[RTOS] All primitives initialized");
}

// ─── Thread-safe LoRa Helpers ────────────────────────────────

bool loraSendSafe(const uint8_t* data, uint8_t length) {
    if (xSemaphoreTake(xLoRaMutex, MUTEX_TIMEOUT) != pdTRUE) {
        log_w("loraSendSafe: mutex timeout");
        return false;
    }
    bool ok = loraRadio.send(data, length);
    xSemaphoreGive(xLoRaMutex);
    return ok;
}

bool loraTxEnqueue(const uint8_t* data, uint8_t length) {
    if (length > sizeof(LoRaTxRequest::data)) return false;
    LoRaTxRequest req;
    memcpy(req.data, data, length);
    req.length = length;
    return xQueueSend(xLoRaTxQueue, &req, 0) == pdTRUE;
}

bool loraStartReceiveSafe() {
    if (xSemaphoreTake(xLoRaMutex, MUTEX_TIMEOUT) != pdTRUE) {
        log_w("loraStartReceiveSafe: mutex timeout");
        return false;
    }
    bool ok = loraRadio.startReceive();
    xSemaphoreGive(xLoRaMutex);
    return ok;
}

bool loraCheckReceiveSafe(LoRaRxResult& result) {
    if (xSemaphoreTake(xLoRaMutex, MUTEX_TIMEOUT) != pdTRUE) {
        return false;
    }
    bool ok = loraRadio.checkReceive(result);
    xSemaphoreGive(xLoRaMutex);
    return ok;
}

// ─── Thread-safe Registry Helpers ────────────────────────────

bool registryLock() {
    return xSemaphoreTake(xRegistryMutex, MUTEX_TIMEOUT) == pdTRUE;
}

void registryUnlock() {
    xSemaphoreGive(xRegistryMutex);
}
