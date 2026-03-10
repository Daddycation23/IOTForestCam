/**
 * @file RoleConfig.cpp
 * @brief Dynamic role selection via BOOT button (GPIO 0) + NVS persistence
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "RoleConfig.h"

static const char* TAG = "RoleConfig";

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Public API
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

NodeRole RoleConfig::selectRole(Adafruit_SSD1306& display) {
    // Configure BOOT button: GPIO 0, active-low.
    // INPUT_PULLUP ensures a defined HIGH when the button is not pressed.
    // After the bootloader hands off to the app, GPIO 0 is safe to use.
    pinMode(ROLE_BOOT_BTN_PIN, INPUT_PULLUP);

    // Start with whatever role was saved last boot (defaults to LEAF).
    NodeRole currentRole = loadRole();

    Serial.printf("[%s] Entering role selection (saved: %s)\n",
                  TAG, roleName(currentRole));

    uint32_t startMs = millis();

    while (millis() - startMs < ROLE_SELECT_TIMEOUT_MS) {
        uint32_t remainMs = ROLE_SELECT_TIMEOUT_MS - (millis() - startMs);
        _drawMenu(display, currentRole, remainMs);

        if (_buttonPressed()) {
            currentRole = _nextRole(currentRole);
            // Reset the countdown so the user has a full 5 s to review.
            startMs = millis();
            Serial.printf("[%s] Cycled to: %s\n", TAG, roleName(currentRole));
        }

        delay(50);  // 20 Hz refresh — smooth countdown, low CPU load
    }

    // Auto-confirm: save and show confirmation screen.
    saveRole(currentRole);
    Serial.printf("[%s] Confirmed: %s\n", TAG, roleName(currentRole));

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("IOT Forest Cam");
    display.println();
    display.println("Role confirmed:");
    display.setTextSize(2);
    display.println(roleName(currentRole));
    display.setTextSize(1);
    display.display();

    delay(800);

    return currentRole;
}

NodeRole RoleConfig::loadRole() {
    Preferences prefs;
    prefs.begin(ROLE_NVS_NAMESPACE, /*readOnly=*/true);
    uint8_t val = prefs.getUChar(ROLE_NVS_KEY, (uint8_t)NODE_ROLE_LEAF);
    prefs.end();

    // Validate: only accept known roles; default to LEAF on corruption.
    if (val == (uint8_t)NODE_ROLE_LEAF  ||
        val == (uint8_t)NODE_ROLE_RELAY ||
        val == (uint8_t)NODE_ROLE_GATEWAY) {
        return (NodeRole)val;
    }
    return NODE_ROLE_LEAF;
}

void RoleConfig::saveRole(NodeRole role) {
    Preferences prefs;
    prefs.begin(ROLE_NVS_NAMESPACE, /*readOnly=*/false);
    prefs.putUChar(ROLE_NVS_KEY, (uint8_t)role);
    prefs.end();
}

const char* RoleConfig::roleName(NodeRole role) {
    switch (role) {
        case NODE_ROLE_LEAF:    return "LEAF";
        case NODE_ROLE_RELAY:   return "RELAY";
        case NODE_ROLE_GATEWAY: return "GATEWAY";
        default:                return "UNKNOWN";
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Private Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void RoleConfig::_drawMenu(Adafruit_SSD1306& display,
                            NodeRole role, uint32_t remainMs) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    // ── Line 0 (y=0): header ─────────────────────────────────
    display.setCursor(0, 0);
    display.println("IOT Forest Cam");

    // ── Line 1 (y=8): label ──────────────────────────────────
    display.setCursor(0, 8);
    display.println("Select Role:");

    // ── Line 2 (y=16): selected role with arrows ─────────────
    display.setCursor(0, 20);
    display.print("> ");
    display.print(roleName(role));
    display.println(" <");

    // ── Line 3 (y=28): button hint ───────────────────────────
    display.setCursor(0, 30);
    display.println("BOOT = cycle");

    // ── Line 4 (y=38): countdown ─────────────────────────────
    display.setCursor(0, 40);
    // Display as X.Xs (one decimal place, rounded up to nearest 0.1s)
    uint32_t tenths = (remainMs + 99) / 100;
    display.printf("Auto in: %lu.%lus", tenths / 10, tenths % 10);

    // ── Progress bar (y=52, h=6px): shrinks right→left ───────
    display.drawRect(0, 52, 128, 7, SSD1306_WHITE);
    int16_t filled = (int16_t)((uint32_t)(remainMs) * 126 / ROLE_SELECT_TIMEOUT_MS);
    if (filled > 0) {
        display.fillRect(1, 53, filled, 5, SSD1306_WHITE);
    }

    display.display();
}

NodeRole RoleConfig::_nextRole(NodeRole role) {
    switch (role) {
        case NODE_ROLE_LEAF:    return NODE_ROLE_RELAY;
        case NODE_ROLE_RELAY:   return NODE_ROLE_GATEWAY;
        case NODE_ROLE_GATEWAY: return NODE_ROLE_LEAF;
        default:                return NODE_ROLE_LEAF;
    }
}

bool RoleConfig::_buttonPressed() {
    // Active-low: LOW = pressed.
    if (digitalRead(ROLE_BOOT_BTN_PIN) == HIGH) {
        return false;
    }

    // Enforce cooldown between presses to prevent bounced repeats.
    static uint32_t lastPressMs = 0;
    uint32_t now = millis();
    if (now - lastPressMs < ROLE_BTN_COOLDOWN_MS) {
        return false;
    }

    // Wait for button release to confirm a complete press.
    // 2-second safety cap avoids blocking forever on a stuck button.
    uint32_t pressStart = now;
    while (digitalRead(ROLE_BOOT_BTN_PIN) == LOW) {
        if (millis() - pressStart > 2000) break;
        delay(10);
    }

    // Accept only if the press lasted at least ROLE_DEBOUNCE_MS.
    if (millis() - pressStart >= ROLE_DEBOUNCE_MS) {
        lastPressMs = millis();
        return true;
    }

    return false;  // Too short — likely noise
}
