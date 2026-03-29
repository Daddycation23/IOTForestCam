/**
 * @file DeepSleepManager.h
 * @brief Deep sleep management with timer-based wake for leaf/relay nodes
 *
 * Leaf and relay nodes enter deep sleep between harvest cycles to conserve
 * power. Wake is timer-based only (180s intervals). LoRa DIO1 ext1 wakeup
 * was removed — SX1280 LoRa wake is unreliable on LILYGO T3-S3 hardware.
 *
 * Wake Cycle:
 *   1. ESP32 wakes on timer (180s)
 *   2. Restores role + gateway SSID from RTC memory
 *   3. Connects STA to gateway AP, announces via CoAP POST /announce
 *   4. Gateway downloads images, leaf sleeps again after 120s idle
 *
 * Gateway nodes never sleep.
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef DEEP_SLEEP_MANAGER_H
#define DEEP_SLEEP_MANAGER_H

#include <Arduino.h>
#include <esp_sleep.h>
#include "RoleConfig.h"
#include "LoRaRadio.h"

// ─── Deep Sleep Configuration ────────────────────────────────
static constexpr uint32_t SLEEP_ACTIVE_TIMEOUT_MS     = 120000;  // Stay awake 2 min after boot/wake
static constexpr uint32_t SLEEP_TIMER_WAKEUP_S        = 180;     // 3 min timer wake (primary wake source)
// SLEEP_WAKE_CMD_TIMEOUT_MS removed (WAKE_PING no longer used)
// SLEEP_WAKE_SETTLE_DELAY_MS removed (WAKE_PING no longer used)

// ─── RTC-persistent state (survives deep sleep) ──────────────
// These are declared in DeepSleepManager.cpp with RTC_DATA_ATTR

extern RTC_DATA_ATTR uint32_t rtcBootCount;
extern RTC_DATA_ATTR uint8_t  rtcSavedRole;
extern RTC_DATA_ATTR int8_t   rtcLastImgIdx;
extern RTC_DATA_ATTR char     rtcSavedSSID[32];
extern RTC_DATA_ATTR bool     rtcStateValid;
extern RTC_DATA_ATTR bool     rtcGatewayKnown;
extern RTC_DATA_ATTR char     rtcGatewaySSID[32];

// ─── Resume Offset State (persists across deep sleep) ──────
extern RTC_DATA_ATTR bool     rtcResumeValid;       // Is there a resumable transfer?
extern RTC_DATA_ATTR uint8_t  rtcResumeNodeId[6];   // MAC of the node we were downloading from
extern RTC_DATA_ATTR uint8_t  rtcResumeImageIdx;    // Image index we were downloading
extern RTC_DATA_ATTR uint32_t rtcResumeBlock;        // Block number to resume from
extern RTC_DATA_ATTR uint16_t rtcResumeSum1;         // Fletcher-16 partial state (low byte accumulator)
extern RTC_DATA_ATTR uint16_t rtcResumeSum2;         // Fletcher-16 partial state (high byte accumulator)
extern RTC_DATA_ATTR char     rtcResumeFilePath[64]; // Partial file path on SD
extern RTC_DATA_ATTR uint32_t rtcResumeTotalBytes;   // Bytes written so far

// ─── DeepSleepManager Class ─────────────────────────────────

class DeepSleepManager {
public:
    DeepSleepManager();

    /**
     * Check if the ESP32 was woken by a LoRa packet (DIO1 ext1 wakeup).
     * Call early in setup() to determine boot path.
     */
    static bool wasWokenByLoRa();

    /**
     * Check if woken by timer (primary wake source for periodic harvest).
     */
    static bool wasWokenByTimer();

    /**
     * Check if this is a cold boot (power-on or software reset).
     */
    static bool isColdBoot();

    /**
     * Save current state to RTC memory before entering deep sleep.
     * @param role      Current node role (LEAF or RELAY)
     * @param ssid      WiFi AP SSID to restore on wake
     * @param imgIdx    Last image index served
     */
    void saveState(NodeRole role, const char* ssid, int8_t imgIdx = -1);

    /**
     * Restore state from RTC memory after LoRa wake.
     * @param[out] role   Restored role
     * @param[out] ssid   Restored SSID (buffer must be >= 32 bytes)
     * @return true if valid RTC state was restored
     */
    bool restoreState(NodeRole& role, char* ssid);

    /**
     * Enter deep sleep with DIO1 (GPIO 9) ext1 wakeup armed.
     * The SX1280 must be in RX mode before calling this.
     * Does not return.
     */
    void enterSleep();

    /**
     * Prepare the LoRa radio for deep sleep.
     * Puts radio into continuous RX mode so DIO1 fires on packet reception.
     */
    void prepareRadioForSleep(LoRaRadio& radio);

    /**
     * Check if the node should enter deep sleep.
     * Returns true if: active timeout expired AND no harvest/CoAP in progress.
     */
    bool shouldSleep(uint32_t nowMs) const;

    /**
     * Record activity to reset the active timeout.
     * Call when CoAP request is served, harvest completes, etc.
     */
    void onActivity();

    /**
     * Mark that a harvest is currently in progress (prevents sleeping).
     */
    void setHarvestInProgress(bool inProgress);

    /**
     * Mark that CoAP server is busy serving (prevents sleeping).
     */
    void setCoapBusy(bool busy);

    /** Save transfer resume state before entering deep sleep. */
    static void saveResumeState(const uint8_t nodeId[6], uint8_t imageIdx,
                                uint32_t blockNum, uint16_t sum1, uint16_t sum2,
                                const char* filePath, uint32_t totalBytes);

    /** Clear resume state (call after successful transfer completion). */
    static void clearResumeState();

    /** Get boot count from RTC memory. */
    uint32_t bootCount() const { return rtcBootCount; }

private:
    uint32_t _lastActivityMs;
    bool     _harvestInProgress;
    bool     _coapBusy;
};

#endif // DEEP_SLEEP_MANAGER_H
