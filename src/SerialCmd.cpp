/**
 * @file SerialCmd.cpp
 * @brief Serial command parser implementation
 */

#include "SerialCmd.h"

static const char* TAG = "SerialCmd";

SerialCmd::SerialCmd()
    : _blockedCount(0), _linePos(0)
{
    memset(_blockedList, 0, sizeof(_blockedList));
    memset(_lineBuf, 0, sizeof(_lineBuf));
}

void SerialCmd::tick() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (_linePos > 0) {
                _lineBuf[_linePos] = '\0';
                _processLine(_lineBuf);
                _linePos = 0;
            }
        } else if (_linePos < sizeof(_lineBuf) - 1) {
            _lineBuf[_linePos++] = c;
        }
    }
}

bool SerialCmd::isNodeBlocked(const uint8_t nodeId[6]) const {
    for (uint8_t i = 0; i < _blockedCount; i++) {
        if (nodeId[4] == _blockedList[i][0] && nodeId[5] == _blockedList[i][1]) {
            return true;
        }
    }
    return false;
}

bool SerialCmd::parseBlockCmd(const char* line, uint8_t outBytes[2]) {
    if (strncmp(line, "block ", 6) != 0) return false;
    const char* hex = line + 6;
    if (strlen(hex) < 4) return false;

    auto hexNibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };

    int h0 = hexNibble(hex[0]), h1 = hexNibble(hex[1]);
    int h2 = hexNibble(hex[2]), h3 = hexNibble(hex[3]);
    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return false;

    outBytes[0] = (h0 << 4) | h1;
    outBytes[1] = (h2 << 4) | h3;
    return true;
}

void SerialCmd::_processLine(const char* line) {
    uint8_t bytes[2];

    if (parseBlockCmd(line, bytes)) {
        if (_addBlock(bytes[0], bytes[1])) {
            Serial.printf("[%s] Blocked node *:%02X:%02X\n", TAG, bytes[0], bytes[1]);
        } else {
            Serial.printf("[%s] Block list full or already blocked\n", TAG);
        }
        return;
    }

    if (strncmp(line, "unblock ", 8) == 0) {
        const char* hex = line + 8;
        // Reuse parse logic with "block " prefix
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "block %s", hex);
        if (parseBlockCmd(tmp, bytes)) {
            if (_removeBlock(bytes[0], bytes[1])) {
                Serial.printf("[%s] Unblocked node *:%02X:%02X\n", TAG, bytes[0], bytes[1]);
            } else {
                Serial.printf("[%s] Node *:%02X:%02X not in block list\n", TAG, bytes[0], bytes[1]);
            }
        }
        return;
    }

    if (strcmp(line, "list") == 0) {
        _listBlocked();
        return;
    }
}

bool SerialCmd::_addBlock(uint8_t byte0, uint8_t byte1) {
    // Check if already blocked
    for (uint8_t i = 0; i < _blockedCount; i++) {
        if (_blockedList[i][0] == byte0 && _blockedList[i][1] == byte1) {
            return false;
        }
    }
    if (_blockedCount >= SERIAL_CMD_MAX_BLOCKED) return false;

    _blockedList[_blockedCount][0] = byte0;
    _blockedList[_blockedCount][1] = byte1;
    _blockedCount++;
    return true;
}

bool SerialCmd::_removeBlock(uint8_t byte0, uint8_t byte1) {
    for (uint8_t i = 0; i < _blockedCount; i++) {
        if (_blockedList[i][0] == byte0 && _blockedList[i][1] == byte1) {
            // Shift remaining entries down
            for (uint8_t j = i; j < _blockedCount - 1; j++) {
                _blockedList[j][0] = _blockedList[j + 1][0];
                _blockedList[j][1] = _blockedList[j + 1][1];
            }
            _blockedCount--;
            return true;
        }
    }
    return false;
}

void SerialCmd::_listBlocked() {
    if (_blockedCount == 0) {
        Serial.printf("[%s] No blocked nodes\n", TAG);
        return;
    }
    Serial.printf("[%s] Blocked nodes (%u):\n", TAG, _blockedCount);
    for (uint8_t i = 0; i < _blockedCount; i++) {
        Serial.printf("  [%u] *:%02X:%02X\n", i, _blockedList[i][0], _blockedList[i][1]);
    }
}
