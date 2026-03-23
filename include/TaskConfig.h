/**
 * @file TaskConfig.h
 * @brief FreeRTOS task configuration and synchronization primitives
 *
 * Defines task stack sizes, priorities, core affinities, and declares
 * all shared FreeRTOS handles (mutexes, queues, event groups) used
 * across the ForestCam firmware.
 *
 * Call initRTOS() early in setup() before creating any tasks.
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef TASK_CONFIG_H
#define TASK_CONFIG_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

// ─── Core Affinities ─────────────────────────────────────────
static constexpr BaseType_t CORE_LORA    = 0;   // Protocol core: LoRa, AODV, election
static constexpr BaseType_t CORE_NETWORK = 1;   // Network core: WiFi, CoAP

// ─── Task Priorities (higher = more urgent) ──────────────────
static constexpr UBaseType_t PRIORITY_LORA          = 3;
static constexpr UBaseType_t PRIORITY_HARVEST       = 2;
static constexpr UBaseType_t PRIORITY_COAP_SERVER   = 2;
static constexpr UBaseType_t PRIORITY_OLED          = 1;

// ─── Task Stack Sizes (words, not bytes) ─────────────────────
static constexpr uint32_t STACK_LORA          = 4096;
static constexpr uint32_t STACK_HARVEST       = 8192;   // Large: WiFi + CoAP buffers
static constexpr uint32_t STACK_COAP_SERVER   = 6144;   // 24KB: CoapMessage + WiFi IRQ headroom
static constexpr uint32_t STACK_OLED          = 2048;

// ─── Queue Sizes ─────────────────────────────────────────────
static constexpr uint8_t LORA_TX_QUEUE_SIZE       = 8;
static constexpr uint8_t HARVEST_CMD_QUEUE_SIZE   = 2;
static constexpr uint8_t RELAY_HARVEST_QUEUE_SIZE = 2;
static constexpr uint8_t ANNOUNCE_QUEUE_SIZE      = 4;

// ─── Mutex Timeout ───────────────────────────────────────────
static constexpr TickType_t MUTEX_TIMEOUT = pdMS_TO_TICKS(1000);

// ─── LoRa TX Queue Item ─────────────────────────────────────
struct LoRaTxRequest {
    uint8_t data[64];
    uint8_t length;
};

// ─── Announce Queue Item (leaf-initiated harvest) ───────────
struct AnnounceMessage {
    uint8_t  nodeId[6];     // Leaf's MAC address
    uint8_t  ip[4];         // Leaf's STA IP (e.g., 192.168.4.2)
    uint8_t  imageCount;    // Number of images available
};

// ─── Harvest Event Bits ──────────────────────────────────────
static constexpr EventBits_t HARVEST_EVT_START    = (1 << 0);
static constexpr EventBits_t HARVEST_EVT_COMPLETE = (1 << 1);
static constexpr EventBits_t HARVEST_EVT_ABORT    = (1 << 2);

// ─── Shared FreeRTOS Handles ─────────────────────────────────
extern SemaphoreHandle_t  xLoRaMutex;       // Guards LoRaRadio SPI access
extern SemaphoreHandle_t  xRegistryMutex;   // Guards NodeRegistry R/W
extern QueueHandle_t      xLoRaTxQueue;     // LoRa TX requests from any core
extern QueueHandle_t      xHarvestCmdQueue; // Harvest trigger (Core 0 → Core 1)
extern QueueHandle_t      xRelayHarvestQueue; // Relay harvest cmd (Core 0 → Core 1)
extern QueueHandle_t      xAnnounceQueue;    // Leaf announce msgs (CoAP server → harvest task)
extern EventGroupHandle_t xHarvestEvents;   // Harvest lifecycle events

// ─── Task Handles ────────────────────────────────────────────
extern TaskHandle_t hTaskLoRa;
extern TaskHandle_t hTaskHarvest;
extern TaskHandle_t hTaskRelayHarvest;
extern TaskHandle_t hTaskCoapServer;
extern TaskHandle_t hTaskOLED;

// ─── Initialization ──────────────────────────────────────────

/**
 * Create all FreeRTOS synchronization primitives.
 * Must be called once in setup() before creating tasks.
 */
void initRTOS();

// ─── Thread-safe LoRa helpers ────────────────────────────────

/**
 * Acquire the LoRa mutex, send a packet, release the mutex.
 * Safe to call from any task/core.
 * @return true if send succeeded (mutex acquired and TX OK).
 */
bool loraSendSafe(const uint8_t* data, uint8_t length);

/**
 * Enqueue a LoRa TX request for the LoRa task to transmit.
 * Non-blocking — returns false if queue is full.
 * Use this from Core 1 tasks that should not touch the SPI bus directly.
 */
bool loraTxEnqueue(const uint8_t* data, uint8_t length);

/**
 * Acquire the LoRa mutex, call startReceive(), release.
 * @return true if startReceive succeeded.
 */
bool loraStartReceiveSafe();

/**
 * Acquire the LoRa mutex, call checkReceive(), release.
 * @return true if a packet was received.
 */
bool loraCheckReceiveSafe(struct LoRaRxResult& result);

// ─── Thread-safe Registry helpers ────────────────────────────

/** Lock the registry mutex. Returns true if acquired within timeout. */
bool registryLock();

/** Unlock the registry mutex. */
void registryUnlock();

#endif // TASK_CONFIG_H
