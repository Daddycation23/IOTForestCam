/**
 * @file Globals.h
 * @brief Extern declarations for shared state across task modules
 *
 * All definitions live in main.cpp. Other .cpp files include this
 * header to access shared hardware, objects, and flags.
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef GLOBALS_H
#define GLOBALS_H

#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <atomic>

#include "LoRaRadio.h"
#include "LoRaBeacon.h"
#include "LoRaDispatch.h"
#include "AodvPacket.h"
#include "AodvRouter.h"
#include "CoapClient.h"
#include "NodeRegistry.h"
#include "HarvestLoop.h"
#include "StorageReader.h"
#include "CoapServer.h"
#include "RoleConfig.h"
#include "ElectionManager.h"
#include "TaskConfig.h"
#include "DeepSleepManager.h"
#include "SerialCmd.h"

// ─── Pin Definitions ─────────────────────────────────────────
#define OLED_SDA 18
#define OLED_SCL 17

// ─── SD Card SPI Pins (HSPI) ────────────────────────────────
#define VSENSOR_SD_CS   13
#define VSENSOR_SD_CLK  14
#define VSENSOR_SD_MOSI 11
#define VSENSOR_SD_MISO 2

// ─── Runtime role ────────────────────────────────────────────
extern NodeRole g_role;
extern std::atomic<uint8_t> _activeRole;

// ─── Shared hardware ────────────────────────────────────────
extern Adafruit_SSD1306 display;
extern LoRaRadio        loraRadio;
extern AodvRouter       aodvRouter;
extern SPIClass         gwSPI;

// ─── Gateway objects ────────────────────────────────────────
extern NodeRegistry     registry;
extern CoapClient       coapClient;
extern HarvestLoop      harvestLoop;
extern ElectionManager  electionMgr;

// ─── Serial command handler ─────────────────────────────────
extern SerialCmd        serialCmd;

// ─── Leaf / Relay objects ───────────────────────────────────
extern StorageReader    storage;
extern CoapServer       coapServer;
extern StorageReader    cachedStorage;
extern CoapServer       cachedCoapServer;

// ─── Deep sleep manager ────────────────────────────────────
extern DeepSleepManager deepSleepMgr;

// ─── Shared state flags ────────────────────────────────────
extern const char*             AP_SSID_PREFIX;
extern const char*             AP_PASS;
extern char                    _apSSID[32];
extern std::atomic<bool>       _loraReady;
extern std::atomic<bool>       _gwLoraReady;
extern std::atomic<bool>       _relayBusy;
extern std::atomic<uint8_t>    _relayCmdId;
extern std::atomic<bool>       _relayCachedServing;
extern std::atomic<uint32_t>   _relayCachedStartMs;
extern std::atomic<uint32_t>   _lastNewBeaconMs;

// ─── Per-boot beacon counters ──────────────────────────────
extern uint32_t g_beaconTxCount;
extern uint32_t g_beaconRxCount;

// ─── Timing constants ──────────────────────────────────────
static constexpr uint32_t BEACON_INTERVAL_MS       = 30000;
static constexpr uint32_t BEACON_JITTER_MS         = 2000;
static constexpr uint32_t HARVEST_LISTEN_PERIOD_MS = 180000;
static constexpr uint32_t HARVEST_REACTIVE_DELAY_MS = 15000;
static constexpr uint32_t ROUTE_DISCOVERY_DELAY_MS = 15000;
static constexpr uint32_t RELAY_CACHED_TIMEOUT_MS  = 120000;

// ─── Forward declarations for task functions ────────────────
void taskLoRaGateway(void* param);
void taskLoRaLeafRelay(void* param);
void taskHarvestGateway(void* param);
void taskCoapServerLoop(void* param);
void taskRelayHarvest(void* param);
void initGateway();
void initLeafRelay();

// ─── Helpers used across modules ────────────────────────────
void relayCachedCleanup();
void relayHarvest(const HarvestCmdPacket& cmd);
void onRouteDiscovered(const uint8_t destId[6], uint8_t hopCount);
void displayStatus(const char* line);
bool wasCleanBoot();

#endif // GLOBALS_H
