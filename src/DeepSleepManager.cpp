/**
 * @file DeepSleepManager.cpp
 * @brief Deep sleep management implementation
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "DeepSleepManager.h"
#include <driver/gpio.h>

static const char* TAG = "DeepSleep";

// ─── RTC-persistent state ────────────────────────────────────
RTC_DATA_ATTR uint32_t rtcBootCount   = 0;
RTC_DATA_ATTR uint8_t  rtcSavedRole   = 0;
RTC_DATA_ATTR int8_t   rtcLastImgIdx  = -1;
RTC_DATA_ATTR char     rtcSavedSSID[32] = {0};
RTC_DATA_ATTR bool     rtcStateValid  = false;
RTC_DATA_ATTR bool     rtcGatewayKnown = false;
RTC_DATA_ATTR char     rtcGatewaySSID[32] = {0};

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Constructor
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

DeepSleepManager::DeepSleepManager()
    : _lastActivityMs(millis())
    , _harvestInProgress(false)
    , _coapBusy(false)
{
    rtcBootCount++;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Wake Cause Detection
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool DeepSleepManager::wasWokenByLoRa() {
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;
}

bool DeepSleepManager::wasWokenByTimer() {
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
}

bool DeepSleepManager::isColdBoot() {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    return (cause == ESP_SLEEP_WAKEUP_UNDEFINED);  // Power-on has no wakeup cause
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// RTC State Persistence
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void DeepSleepManager::saveState(NodeRole role, const char* ssid, int8_t imgIdx) {
    rtcSavedRole  = (uint8_t)role;
    rtcLastImgIdx = imgIdx;
    strncpy(rtcSavedSSID, ssid, sizeof(rtcSavedSSID) - 1);
    rtcSavedSSID[sizeof(rtcSavedSSID) - 1] = '\0';
    rtcStateValid = true;

    log_i("%s: State saved — role=%u, ssid=%s, imgIdx=%d, bootCount=%lu",
          TAG, role, ssid, imgIdx, rtcBootCount);
}

bool DeepSleepManager::restoreState(NodeRole& role, char* ssid) {
    if (!rtcStateValid) {
        log_w("%s: No valid RTC state to restore", TAG);
        return false;
    }

    role = (NodeRole)rtcSavedRole;
    strncpy(ssid, rtcSavedSSID, 32);
    ssid[31] = '\0';

    log_i("%s: State restored — role=%u, ssid=%s, imgIdx=%d, boot #%lu",
          TAG, role, ssid, rtcLastImgIdx, rtcBootCount);
    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Sleep Entry
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void DeepSleepManager::prepareRadioForSleep(LoRaRadio& radio) {
    // Put radio into continuous RX — DIO1 will assert HIGH on packet reception
    if (radio.isReady()) {
        radio.startReceive();
        log_i("%s: Radio in continuous RX for DIO1 wakeup", TAG);
    }

    // Hold PA control + ALL SPI bus pins during deep sleep.
    // Without holds, GPIOs float on sleep entry which can:
    //   1. Disable the PA receive path (RXEN goes LOW → no RF to SX1280)
    //   2. Cause SPI bus glitches that kick SX1280 out of RX mode
    //
    // Pin states held:
    //   RXEN  (GPIO 21) HIGH — PA receive path active
    //   TXEN  (GPIO 10) LOW  — PA transmit path off
    //   CS    (GPIO 7)  HIGH — SPI chip select inactive
    //   SCK   (GPIO 5)  LOW  — SPI clock idle
    //   MOSI  (GPIO 6)  LOW  — SPI data idle
    //   MISO  (GPIO 3)  INPUT — held to prevent floating
    gpio_hold_en(GPIO_NUM_21);   // RXEN
    gpio_hold_en(GPIO_NUM_10);   // TXEN
    gpio_hold_en(GPIO_NUM_7);    // CS
    gpio_hold_en(GPIO_NUM_5);    // SCK
    gpio_hold_en(GPIO_NUM_6);    // MOSI
    gpio_hold_en(GPIO_NUM_3);    // MISO
    gpio_deep_sleep_hold_en();
    log_i("%s: GPIO 21/10/7/5/6/3 held for deep sleep (PA RX + SPI stable)", TAG);
}

void DeepSleepManager::enterSleep() {
    log_i("%s: Entering deep sleep — timer %lus + DIO1 armed, boot #%lu",
          TAG, (unsigned long)SLEEP_TIMER_WAKEUP_S, rtcBootCount);

    Serial.flush();
    delay(20);

    // Primary: timer wakeup (reliable on all boards)
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_TIMER_WAKEUP_S * 1000000ULL);

    // Secondary: DIO1 ext1 wakeup (may not work on LILYGO T3-S3 due to PA power)
    esp_sleep_enable_ext1_wakeup(1ULL << LORA_DIO1, ESP_EXT1_WAKEUP_ANY_HIGH);

    esp_deep_sleep_start();
    // ── Does not return ──
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Sleep Decision Logic
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool DeepSleepManager::shouldSleep(uint32_t nowMs) const {
    // Never sleep during active operations
    if (_harvestInProgress || _coapBusy) return false;
    // Sleep if active timeout has expired
    return (nowMs - _lastActivityMs) >= SLEEP_ACTIVE_TIMEOUT_MS;
}

void DeepSleepManager::onActivity() {
    _lastActivityMs = millis();
}

void DeepSleepManager::setHarvestInProgress(bool inProgress) {
    _harvestInProgress = inProgress;
    if (inProgress) onActivity();
}

void DeepSleepManager::setCoapBusy(bool busy) {
    _coapBusy = busy;
    if (busy) onActivity();
}
