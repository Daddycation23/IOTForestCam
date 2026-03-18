/**
 * @file SerialCmd.h
 * @brief Serial command parser for runtime node management
 *
 * Supported commands (typed into Serial Monitor):
 *   block AABB   — block node with MAC suffix AA:BB from harvest
 *   unblock AABB — remove block on node AA:BB
 *   list         — show currently blocked nodes
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef SERIAL_CMD_H
#define SERIAL_CMD_H

#include <Arduino.h>

static constexpr uint8_t SERIAL_CMD_MAX_BLOCKED = 8;

class SerialCmd {
public:
    SerialCmd();

    /** Call from loop() — reads serial input and processes complete lines. */
    void tick();

    /** Check if a node (by full 6-byte MAC) is in the block list. */
    bool isNodeBlocked(const uint8_t nodeId[6]) const;

    /** Number of currently blocked nodes. */
    uint8_t blockedCount() const { return _blockedCount; }

    /** Parse a "block AABB" command, extracting the two hex bytes. */
    static bool parseBlockCmd(const char* line, uint8_t outBytes[2]);

private:
    uint8_t _blockedList[SERIAL_CMD_MAX_BLOCKED][2];
    uint8_t _blockedCount;

    char    _lineBuf[64];
    uint8_t _linePos;

    void _processLine(const char* line);
    bool _addBlock(uint8_t byte0, uint8_t byte1);
    bool _removeBlock(uint8_t byte0, uint8_t byte1);
    void _listBlocked();
};

#endif // SERIAL_CMD_H
