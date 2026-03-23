/**
 * @file main.cpp
 * @brief IOT Forest Cam — Unified firmware entry point
 *
 * Contains global state definitions, setup(), and loop() only.
 * Task implementations are in separate files:
 *   - TaskLoRaGateway.cpp    — Gateway LoRa task (Core 0)
 *   - TaskLoRaLeafRelay.cpp  — Leaf/Relay LoRa task (Core 0)
 *   - TaskHarvestGateway.cpp — Harvest orchestration (Core 1)
 *   - TaskCoapServerLoop.cpp — CoAP server task (Core 1)
 *   - TaskRelayHarvest.cpp   — Relay harvest + cached serving (Core 1)
 *   - InitNodes.cpp          — initGateway() + initLeafRelay()
 *
 * Build: pio run -e esp32s3_unified
 *
 * Hardware: LILYGO T3-S3 V1.2
 *   - OLED:    SSD1306 128x64 (I2C: SDA=18, SCL=17)
 *   - SD Card: HSPI (MOSI=11, MISO=2, CLK=14, CS=13)
 *   - LoRa:    SX1280 FSPI (MOSI=6, MISO=3, SCK=5, CS=7, DIO1=9, BUSY=36, RST=8, RXEN=21, TXEN=10)
 *   - BOOT:    GPIO 0 (active-low, used for role selection at boot)
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "Globals.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <driver/gpio.h>

// ─── Global Definitions ──────────────────────────────────────
// These are the actual storage for all extern declarations in Globals.h.

NodeRole g_role = NODE_ROLE_LEAF;
std::atomic<uint8_t> _activeRole{NODE_ROLE_LEAF};

Adafruit_SSD1306 display(128, 64, &Wire, -1);
LoRaRadio        loraRadio;
AodvRouter       aodvRouter(loraRadio);
SPIClass         gwSPI(HSPI);

// IMPORTANT: declaration order is significant — C++ initialises file-scope
// objects in declaration order. HarvestLoop stores references so deps come first.
NodeRegistry     registry;
CoapClient       coapClient;
HarvestLoop      harvestLoop(registry, coapClient);
ElectionManager  electionMgr(loraRadio, registry, harvestLoop, aodvRouter);

SerialCmd        serialCmd;

StorageReader    storage;
CoapServer       coapServer(storage);
StorageReader    cachedStorage("/cached");
CoapServer       cachedCoapServer(cachedStorage);

DeepSleepManager deepSleepMgr;

const char* AP_SSID_PREFIX = "ForestCam";
const char* AP_PASS        = "forestcam123";
char        _apSSID[32];

volatile bool           _loraReady    = false;
volatile bool           _gwLoraReady  = false;
std::atomic<bool>       _relayBusy{false};
std::atomic<uint8_t>    _relayCmdId{0};
std::atomic<bool>       _relayCachedServing{false};
std::atomic<uint32_t>   _relayCachedStartMs{0};
volatile uint32_t       _lastNewBeaconMs = 0;

// =============================================================
// Helpers (used by extracted modules via forward declaration)
// =============================================================

void displayStatus(const char* line) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("IOT Forest Cam");
    display.println(line);
    display.display();
}

static const char* getResetReasonStr() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "Power-on";
        case ESP_RST_SW:       return "Software";
        case ESP_RST_PANIC:    return "Panic/crash";
        case ESP_RST_INT_WDT:  return "Interrupt WDT";
        case ESP_RST_TASK_WDT: return "Task WDT";
        case ESP_RST_WDT:      return "Other WDT";
        case ESP_RST_DEEPSLEEP:return "Deep sleep";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "Unknown";
    }
}

bool wasCleanBoot() {
    esp_reset_reason_t r = esp_reset_reason();
    return (r == ESP_RST_POWERON || r == ESP_RST_SW || r == ESP_RST_DEEPSLEEP);
}

// =============================================================
// Arduino setup()
// =============================================================

void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(1000);

    Serial.printf("\n[Boot] Reset reason: %s (boot #%lu)\n", getResetReasonStr(), rtcBootCount);

    // ── OLED ─────────────────────────────────────────────────
    Wire.begin(OLED_SDA, OLED_SCL);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    // ── Fast-path wake: skip role menu if woken by timer or LoRa ─
    if (DeepSleepManager::wasWokenByTimer() || DeepSleepManager::wasWokenByLoRa()) {
        gpio_hold_dis(GPIO_NUM_21);
        gpio_hold_dis(GPIO_NUM_10);
        gpio_hold_dis(GPIO_NUM_7);
        gpio_hold_dis(GPIO_NUM_5);
        gpio_hold_dis(GPIO_NUM_6);
        gpio_hold_dis(GPIO_NUM_3);
        gpio_deep_sleep_hold_dis();

        char restoredSSID[32];
        NodeRole restoredRole;
        if (deepSleepMgr.restoreState(restoredRole, restoredSSID)) {
            g_role = restoredRole;
            strncpy(_apSSID, restoredSSID, sizeof(_apSSID));
            Serial.printf("[Wake] Fast-path: restored role=%s, SSID=%s\n",
                          RoleConfig::roleName(g_role), _apSSID);

            display.clearDisplay();
            display.setCursor(0, 0);
            display.println("LoRa Wake!");
            display.printf("Role: %s\n", RoleConfig::roleName(g_role));
            display.printf("Boot #%lu\n", rtcBootCount);
            display.display();
        } else {
            Serial.println("[Wake] No valid RTC state — doing normal boot");
            goto normal_boot;
        }
    } else {
normal_boot:
        if (!wasCleanBoot()) {
            Serial.println("[Boot] Non-clean reset detected — adding extra stabilisation delay");
            delay(2000);
        }

        bool manualOverride = false;
        g_role = RoleConfig::determineRole(display, manualOverride);
        Serial.printf("\n[Role] Booting as: %s (%s)\n\n",
                      RoleConfig::roleName(g_role),
                      manualOverride ? "manual" : "auto-negotiate");
    }

    // ── Initialize FreeRTOS primitives ───────────────────────
    initRTOS();

    // ── Role-specific hardware init ──────────────────────────
    if (g_role == NODE_ROLE_GATEWAY) {
        initGateway();
    } else {
        initLeafRelay();
    }

    _activeRole = g_role;

    // ── Create FreeRTOS tasks based on role ──────────────────
    if (g_role == NODE_ROLE_GATEWAY) {
        xTaskCreatePinnedToCore(taskLoRaGateway,    "LoRa_GW",   STACK_LORA,        nullptr, PRIORITY_LORA,        &hTaskLoRa,            CORE_LORA);
        xTaskCreatePinnedToCore(taskHarvestGateway,  "Harvest_GW", STACK_HARVEST,   nullptr, PRIORITY_HARVEST,     &hTaskHarvest,         CORE_NETWORK);
        xTaskCreatePinnedToCore(taskCoapServerLoop,  "CoAP_Srv",  STACK_COAP_SERVER, nullptr, PRIORITY_COAP_SERVER, &hTaskCoapServer,      CORE_NETWORK);
    } else {
        xTaskCreatePinnedToCore(taskLoRaLeafRelay,   "LoRa_LR",   STACK_LORA,        nullptr, PRIORITY_LORA,        &hTaskLoRa,            CORE_LORA);
        xTaskCreatePinnedToCore(taskCoapServerLoop,  "CoAP_Srv",  STACK_COAP_SERVER, nullptr, PRIORITY_COAP_SERVER, &hTaskCoapServer,      CORE_NETWORK);

        if (g_role == NODE_ROLE_RELAY) {
            xTaskCreatePinnedToCore(taskRelayHarvest, "Relay_H",  STACK_HARVEST,    nullptr, PRIORITY_HARVEST,     &hTaskRelayHarvest,    CORE_NETWORK);
        }
    }

    Serial.printf("[RTOS] Tasks created — Core 0: LoRa, Core 1: %s\n",
                  g_role == NODE_ROLE_GATEWAY ? "Harvest+CoAP" : "CoAP Server");
}

// =============================================================
// Arduino loop() — OLED updates + serial commands
// =============================================================

void loop() {
    serialCmd.tick();
    _activeRole = electionMgr.activeRole();

    // ── OLED Update (every 2 s) ──────────────────────────────
    static uint32_t lastDisplayMs = 0;
    if (millis() - lastDisplayMs < 2000) {
        vTaskDelay(pdMS_TO_TICKS(100));
        return;
    }
    lastDisplayMs = millis();

    static bool lastDisplayWasGateway = (g_role == NODE_ROLE_GATEWAY);

    if (_activeRole == NODE_ROLE_GATEWAY) {
        lastDisplayWasGateway = true;

        uint8_t activeNodes = 0;
        if (registryLock()) {
            activeNodes = registry.activeCount();
            registryUnlock();
        }

        HarvestState curState = harvestLoop.state();
        bool firstHarvestDone = (curState != HARVEST_IDLE) ||
                                (harvestLoop.lastCycleStats().nodesAttempted > 0);

        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("Gateway (RTOS+AODV)");
        display.printf("Nodes:%u Rtes:%u\n", activeNodes, aodvRouter.routeCount());
        display.printf("State: %s\n", harvestLoop.stateStr());

        if (curState >= HARVEST_CONNECT && curState <= HARVEST_DOWNLOAD) {
            display.printf("-> %s\n", harvestLoop.currentNodeSSID());
        } else if (firstHarvestDone) {
            const HarvestCycleStats& stats = harvestLoop.lastCycleStats();
            display.printf("Last: %u OK, %u fail\n", stats.nodesSucceeded, stats.nodesFailed);
            display.printf("Imgs: %lu\n", stats.totalImages);
        }

        if (hTaskLoRa) {
            display.printf("Stk L:%u H:%u\n",
                           uxTaskGetStackHighWaterMark(hTaskLoRa),
                           hTaskHarvest ? uxTaskGetStackHighWaterMark(hTaskHarvest) : 0);
        }
        display.display();

    } else {
        if (lastDisplayWasGateway) {
            lastDisplayWasGateway = false;
            display.clearDisplay();
        }

        display.fillRect(0, 0, 128, 48, SSD1306_BLACK);
        display.setCursor(0, 0);
        display.println(_activeRole == NODE_ROLE_RELAY ? "Relay (AODV)" : "Leaf (AODV)");
        display.printf("AP: %s\n", _apSSID);
        display.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());
        display.printf("CoAP: %s\n", coapServer.requestCount() > 0 ? ":5683" : "OFF");
        display.printf("Imgs: %d  LoRa:%s\n", storage.imageCount(), _loraReady ? "OK" : "NO");
        display.println("────────────────────");

        if (_relayCachedServing) {
            display.fillRect(0, 48, 128, 16, SSD1306_BLACK);
            display.setCursor(0, 48);
            uint32_t elapsed = (millis() - _relayCachedStartMs) / 1000;
            display.printf("Serving:%u %lus", cachedStorage.imageCount(), elapsed);
            display.setCursor(0, 56);
            display.printf("CReqs:%-3lu CBlk:%-4lu",
                           cachedCoapServer.requestCount(), cachedCoapServer.blocksSent());
        } else {
            display.fillRect(0, 56, 128, 8, SSD1306_BLACK);
            display.setCursor(0, 56);
            display.printf("Reqs:%-4lu Blks:%-5lu",
                           coapServer.requestCount(), coapServer.blocksSent());
        }

        ElectionState es = electionMgr.state();
        if (es == ELECT_ELECTION_START || es == ELECT_WAITING) {
            display.fillRect(0, 40, 128, 8, SSD1306_BLACK);
            display.setCursor(0, 40);
            display.println("ELECTING...");
        } else if (es == ELECT_ACTING_GATEWAY || es == ELECT_PROMOTED) {
            display.fillRect(0, 0, 128, 8, SSD1306_BLACK);
            display.setCursor(0, 0);
            display.println("ACTING GW");
        } else if (es == ELECT_STOOD_DOWN) {
            display.fillRect(0, 40, 128, 8, SSD1306_BLACK);
            display.setCursor(0, 40);
            display.println("ELECTION LOST");
        } else if (es == ELECT_RECLAIMED) {
            display.fillRect(0, 40, 128, 8, SSD1306_BLACK);
            display.setCursor(0, 40);
            display.println("RECLAIMED -> LEAF");
        }

        display.display();
    }
}
