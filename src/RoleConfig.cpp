/**
 * @file RoleConfig.cpp
 * @brief Role selection: auto-negotiation (default) or manual BOOT button override
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "RoleConfig.h"

static const char* TAG = "RoleConfig";

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Public API
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

NodeRole RoleConfig::determineRole(Adafruit_SSD1306& display, bool& manual) {
    pinMode(ROLE_BOOT_BTN_PIN, INPUT_PULLUP);
    manual = false;

    // Check if BOOT button is held — if so, enter legacy manual menu
    if (checkBootHeld(display)) {
        Serial.printf("[%s] BOOT button held — entering manual role selection\n", TAG);
        manual = true;
        return selectRole(display);
    }

    // Auto-negotiate: all nodes start as LEAF, election decides gateway
    Serial.printf("[%s] Auto-negotiation mode — starting as LEAF\n", TAG);

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("IOT Forest Cam");
    display.println();
    display.println("Auto-Negotiating...");
    display.println();
    display.println("All nodes start as");
    display.println("LEAF. Election will");
    display.println("decide GATEWAY.");
    display.display();

    delay(1500);

    return NODE_ROLE_LEAF;
}

bool RoleConfig::checkBootHeld(Adafruit_SSD1306& display) {
    // Show prompt while checking
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("IOT Forest Cam");
    display.println();
    display.println("Hold BOOT button");
    display.println("for manual role...");
    display.display();

    uint32_t startMs = millis();
    bool buttonSeen = false;

    while (millis() - startMs < ROLE_BOOT_CHECK_MS) {
        if (digitalRead(ROLE_BOOT_BTN_PIN) == LOW) {
            buttonSeen = true;
            break;
        }
        delay(50);
    }

    return buttonSeen;
}

NodeRole RoleConfig::selectRole(Adafruit_SSD1306& display) {
    // Start with whatever role was saved last boot (defaults to LEAF).
    NodeRole currentRole = loadRole();

    Serial.printf("[%s] Entering manual role selection (saved: %s)\n",
                  TAG, roleName(currentRole));

    uint32_t startMs = millis();

    while (millis() - startMs < ROLE_SELECT_TIMEOUT_MS) {
        uint32_t remainMs = ROLE_SELECT_TIMEOUT_MS - (millis() - startMs);
        _drawMenu(display, currentRole, remainMs);

        if (_buttonPressed()) {
            currentRole = _nextRole(currentRole);
            startMs = millis();
            Serial.printf("[%s] Cycled to: %s\n", TAG, roleName(currentRole));
        }

        delay(50);
    }

    saveRole(currentRole);
    Serial.printf("[%s] Manual selection confirmed: %s\n", TAG, roleName(currentRole));

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

    display.setCursor(0, 0);
    display.println("IOT Forest Cam");

    display.setCursor(0, 8);
    display.println("Manual Role Select:");

    display.setCursor(0, 20);
    display.print("> ");
    display.print(roleName(role));
    display.println(" <");

    display.setCursor(0, 30);
    display.println("BOOT = cycle");

    display.setCursor(0, 40);
    uint32_t tenths = (remainMs + 99) / 100;
    display.printf("Auto in: %lu.%lus", tenths / 10, tenths % 10);

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
    if (digitalRead(ROLE_BOOT_BTN_PIN) == HIGH) {
        return false;
    }

    static uint32_t lastPressMs = 0;
    uint32_t now = millis();
    if (now - lastPressMs < ROLE_BTN_COOLDOWN_MS) {
        return false;
    }

    uint32_t pressStart = now;
    while (digitalRead(ROLE_BOOT_BTN_PIN) == LOW) {
        if (millis() - pressStart > 2000) break;
        delay(10);
    }

    if (millis() - pressStart >= ROLE_DEBOUNCE_MS) {
        lastPressMs = millis();
        return true;
    }

    return false;
}
