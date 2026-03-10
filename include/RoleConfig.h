/**
 * @file RoleConfig.h
 * @brief Dynamic role selection and NVS persistence for IOT Forest Cam
 *
 * Provides a boot-time OLED menu (5-second window) that lets the user
 * cycle through LEAF / RELAY / GATEWAY by pressing the built-in BOOT
 * button (GPIO 0, active-low).  The selected role is persisted to NVS
 * via Arduino Preferences so it survives power cycles.
 *
 * Usage in main.cpp setup():
 *   Wire.begin(OLED_SDA, OLED_SCL);
 *   display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
 *   g_role = RoleConfig::selectRole(display);   // shows 5-second menu
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

// ─── RoleConfig ──────────────────────────────────────────────

class RoleConfig {
public:
    /**
     * Run the boot-time role selection menu.
     *
     * Displays the current role on the OLED for up to ROLE_SELECT_TIMEOUT_MS.
     * Each BOOT button press cycles: LEAF → RELAY → GATEWAY → LEAF.
     * Pressing resets the countdown timer so the user has a fresh 5 s.
     * On timeout, the displayed role is confirmed, saved to NVS, and returned.
     *
     * @param display  Reference to an already-initialised SSD1306 display.
     * @return The confirmed NodeRole.
     */
    static NodeRole selectRole(Adafruit_SSD1306& display);

    /**
     * Load the role stored in NVS.
     * Returns NODE_ROLE_LEAF on first boot (key not yet written).
     */
    static NodeRole loadRole();

    /** Persist role to NVS. */
    static void saveRole(NodeRole role);

    /** Human-readable name: "LEAF", "RELAY", or "GATEWAY". */
    static const char* roleName(NodeRole role);

private:
    /**
     * Render the selection screen.
     * @param remainMs  Milliseconds remaining before auto-confirm.
     */
    static void _drawMenu(Adafruit_SSD1306& display,
                          NodeRole role, uint32_t remainMs);

    /** Advance to next role in the cycle: LEAF → RELAY → GATEWAY → LEAF. */
    static NodeRole _nextRole(NodeRole role);

    /**
     * Read GPIO 0 with debounce and cooldown.
     * Returns true once per clean button press (waits for release).
     */
    static bool _buttonPressed();
};

#endif // ROLE_CONFIG_H
