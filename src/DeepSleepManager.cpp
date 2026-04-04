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

// ─── Resume Offset State ────────────────────────────────────
RTC_DATA_ATTR bool     rtcResumeValid     = false;
RTC_DATA_ATTR uint8_t  rtcResumeNodeId[6] = {0};
RTC_DATA_ATTR uint8_t  rtcResumeImageIdx  = 0;
RTC_DATA_ATTR uint32_t rtcResumeBlock     = 0;
RTC_DATA_ATTR uint16_t rtcResumeSum1      = 0;
RTC_DATA_ATTR uint16_t rtcResumeSum2      = 0;
RTC_DATA_ATTR char     rtcResumeFilePath[64] = {0};
RTC_DATA_ATTR uint32_t rtcResumeTotalBytes = 0;

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Constructor
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

DeepSleepManager::DeepSleepManager()
    : _lastActivityMs(millis())
    , _harvestInProgress(false)
    , _coapBusy(false)
    , _harvestEverCompleted(false)
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
    log_i("%s: Entering deep sleep — timer %lus, boot #%lu",
          TAG, (unsigned long)SLEEP_TIMER_WAKEUP_S, rtcBootCount);

    Serial.flush();
    delay(20);

    // Timer-only wakeup (reliable on all boards).
    // DIO1 ext1 wakeup removed — SX1280 LoRa wake is unreliable on LILYGO T3-S3
    // due to PA power and SPI bus float issues during deep sleep (commit 8665061).
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_TIMER_WAKEUP_S * 1000000ULL);

    esp_deep_sleep_start();
    // ── Does not return ──
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Sleep Decision Logic
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool DeepSleepManager::shouldSleep(uint32_t nowMs) const {
    // Never sleep during active operations
    if (_harvestInProgress || _coapBusy) return false;
    // Don't sleep until images have been transferred at least once
    if (!_harvestEverCompleted) return false;
    // Sleep if active timeout has expired since last harvest
    return (nowMs - _lastActivityMs) >= SLEEP_ACTIVE_TIMEOUT_MS;
}

void DeepSleepManager::onActivity() {
    _lastActivityMs = millis();
}

void DeepSleepManager::onHarvestComplete() {
    _harvestEverCompleted = true;
    onActivity();  // Start the sleep countdown from now
}

void DeepSleepManager::setHarvestInProgress(bool inProgress) {
    _harvestInProgress = inProgress;
    if (inProgress) onActivity();
}

void DeepSleepManager::setCoapBusy(bool busy) {
    _coapBusy = busy;
    if (busy) onActivity();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Resume Offset State Persistence
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void DeepSleepManager::saveResumeState(const uint8_t nodeId[6], uint8_t imageIdx,
                                        uint32_t blockNum, uint16_t sum1, uint16_t sum2,
                                        const char* filePath, uint32_t totalBytes) {
    memcpy(rtcResumeNodeId, nodeId, 6);
    rtcResumeImageIdx  = imageIdx;
    rtcResumeBlock     = blockNum;
    rtcResumeSum1      = sum1;
    rtcResumeSum2      = sum2;
    strncpy(rtcResumeFilePath, filePath, sizeof(rtcResumeFilePath) - 1);
    rtcResumeFilePath[sizeof(rtcResumeFilePath) - 1] = '\0';
    rtcResumeTotalBytes = totalBytes;
    rtcResumeValid     = true;
}

void DeepSleepManager::clearResumeState() {
    rtcResumeValid = false;
    rtcResumeBlock = 0;
    rtcResumeSum1  = 0;
    rtcResumeSum2  = 0;
    rtcResumeTotalBytes = 0;
    memset(rtcResumeFilePath, 0, sizeof(rtcResumeFilePath));
}
