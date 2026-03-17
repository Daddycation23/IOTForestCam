/**
 * @file DeepSleepManager.cpp
 * @brief Deep sleep management implementation
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "DeepSleepManager.h"

static const char* TAG = "DeepSleep";

// ─── RTC-persistent state ────────────────────────────────────
RTC_DATA_ATTR uint32_t rtcBootCount   = 0;
RTC_DATA_ATTR uint8_t  rtcSavedRole   = 0;
RTC_DATA_ATTR int8_t   rtcLastImgIdx  = -1;
RTC_DATA_ATTR char     rtcSavedSSID[32] = {0};
RTC_DATA_ATTR bool     rtcStateValid  = false;

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
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    return (cause == ESP_SLEEP_WAKEUP_EXT1);
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
}

void DeepSleepManager::enterSleep() {
    log_i("%s: Entering deep sleep — DIO1 (GPIO %d) armed for ext1 wakeup, boot #%lu",
          TAG, LORA_DIO1, rtcBootCount);

    Serial.flush();
    delay(20);

    // Use ext1 wakeup (ext0 is deprecated on ESP32-S3)
    // Bit mask for GPIO 9 (DIO1): 1ULL << 9
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
