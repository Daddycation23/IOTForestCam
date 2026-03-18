/**
 * @file RoleConfig.h
 * @brief Role selection: auto-negotiation (default) or manual BOOT button override
 *
 * Boot behavior:
 *   1. If BOOT button is held during first 2 seconds → legacy 5-second menu
 *   2. If not held → auto-negotiate (all nodes start as LEAF, election decides gateway)
 *
 * Usage in main.cpp setup():
 *   Wire.begin(OLED_SDA, OLED_SCL);
 *   display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
 *   bool manual = false;
 *   g_role = RoleConfig::determineRole(display, manual);
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef ROLE_CONFIG_H
#define ROLE_CONFIG_H

#include <Arduino.h>
#include <Preferences.h>
#include <Adafruit_SSD1306.h>
#include "LoRaBeacon.h"      // NodeRole enum: NODE_ROLE_LEAF / RELAY / GATEWAY

// ─── Hardware ────────────────────────────────────────────────
/// GPIO 0 is the built-in BOOT button on all ESP32-S3 DevKit / LILYGO T3-S3 boards.
/// Active-low: LOW = pressed, HIGH = released.
#define ROLE_BOOT_BTN_PIN   0

// ─── NVS Persistence ─────────────────────────────────────────
#define ROLE_NVS_NAMESPACE  "forestcam"
#define ROLE_NVS_KEY        "role"

// ─── Menu Timing ─────────────────────────────────────────────
static constexpr uint32_t ROLE_SELECT_TIMEOUT_MS = 5000;  ///< Auto-confirm after 5 s
static constexpr uint32_t ROLE_DEBOUNCE_MS       = 50;    ///< Min press duration
static constexpr uint32_t ROLE_BTN_COOLDOWN_MS   = 300;   ///< Min time between presses
static constexpr uint32_t ROLE_BOOT_CHECK_MS     = 2000;  ///< Window to hold BOOT for manual

// ─── RoleConfig ──────────────────────────────────────────────

class RoleConfig {
public:
    /**
     * Determine role: auto-negotiate or manual override.
     *
     * Checks if BOOT button is held during first ROLE_BOOT_CHECK_MS.
     *   - If held: shows legacy 5-second menu for manual role selection.
     *   - If not held: returns NODE_ROLE_LEAF (auto-negotiate mode).
     *
     * @param display     Reference to an already-initialised SSD1306 display.
     * @param[out] manual Set to true if user chose manual override.
     * @return The selected NodeRole (LEAF for auto, or user's choice for manual).
     */
    static NodeRole determineRole(Adafruit_SSD1306& display, bool& manual);

    /**
     * Run the boot-time role selection menu (legacy manual mode).
     * Called internally by determineRole when BOOT is held.
     */
    static NodeRole selectRole(Adafruit_SSD1306& display);

    /**
     * Check if BOOT button is held during initial window.
     * Shows a brief OLED prompt while checking.
     * @param display  OLED to show "Hold BOOT for manual..." prompt.
     * @return true if button was held during the check window.
     */
    static bool checkBootHeld(Adafruit_SSD1306& display);

    /**
     * Load the role stored in NVS.
     * Returns NODE_ROLE_LEAF on first boot (key not yet written).
     */
    static NodeRole loadRole();

    /** Persist role to NVS. */
    static void saveRole(NodeRole role);

    /** Human-readable name: "LEAF", "LEAF+RELAY", or "GATEWAY". */
    static const char* roleName(NodeRole role);

private:
    static void _drawMenu(Adafruit_SSD1306& display,
                          NodeRole role, uint32_t remainMs);

    static NodeRole _nextRole(NodeRole role);
    static bool _buttonPressed();
};

#endif // ROLE_CONFIG_H
