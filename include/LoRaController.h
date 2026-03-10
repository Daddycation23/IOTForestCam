/**
 * @file LoRaController.h
 * @brief Part 2 — LoRa Control Plane (SX1262)
 *
 * Listens for LoRa commands using RadioLib. The ESP32 sleeps in deep sleep
 * 99% of the time. The SX1262 DIO1 pin fires HIGH on packet reception,
 * which physically wakes the ESP32 via EXT0 GPIO interrupt.
 *
 * Two-Step Wake Protocol
 * ──────────────────────
 * The sender uses two successive packets:
 *   1) Wake packet  (any byte, e.g. 0xA1) → DIO1 asserts HIGH → ESP32 wakes
 *   2) Command packet (sent ~500 ms later) → ESP32 reads and acts on this
 *
 * This guarantees a clean RadioLib re-initialisation between wake and read,
 * avoiding FIFO state issues after deep sleep.
 *
 * Command Byte Table
 * ──────────────────
 *   0xA1        Wake + send next/random image (cycles lastImgIdx)
 *   0xC1        Send image whose filename contains "fire"
 *   0xC2        Send image whose filename contains "animal"
 *   0xD0–0xDF   Send image at catalogue index = low nibble (0–15)
 *
 * Hardware Wiring (LILYGO T3-S3 V1.2 — onboard SX1262, FSPI bus)
 * ────────────────────────────────────────────────────────────────
 *   NSS   → GPIO 7    SCK  → GPIO 5    MOSI → GPIO 6
 *   MISO  → GPIO 3    DIO1 → GPIO 33   RST  → GPIO 8    BUSY → GPIO 34
 *
 * Note: SD card uses the HSPI bus (GPIO 14/2/11/13) — no SPI conflict.
 *
 * @author  CS Group 19
 * @date    2026
 */

#ifndef LORA_CONTROLLER_H
#define LORA_CONTROLLER_H

#include <Arduino.h>
#include <RadioLib.h>
#include <esp_sleep.h>

// ─── Pin Defaults (LILYGO T3-S3 V1.2 onboard SX1262, FSPI) ──
#ifndef LORA_NSS_PIN
#define LORA_NSS_PIN    7
#endif
#ifndef LORA_SCK_PIN
#define LORA_SCK_PIN    5
#endif
#ifndef LORA_MOSI_PIN
#define LORA_MOSI_PIN   6
#endif
#ifndef LORA_MISO_PIN
#define LORA_MISO_PIN   3
#endif
#ifndef LORA_DIO1_PIN
#define LORA_DIO1_PIN   33
#endif
#ifndef LORA_RST_PIN
#define LORA_RST_PIN    8
#endif
#ifndef LORA_BUSY_PIN
#define LORA_BUSY_PIN   34
#endif

// ─── RF Parameters ────────────────────────────────────────────
/** Carrier frequency in MHz. 915.0 = US ISM band; 868.0 = EU. */
#ifndef LORA_FREQUENCY
#define LORA_FREQUENCY  915.0f
#endif
#ifndef LORA_BANDWIDTH
#define LORA_BANDWIDTH  125.0f   // kHz
#endif
#ifndef LORA_SF
#define LORA_SF         7        // Spreading Factor (7–12)
#endif
#ifndef LORA_CR
#define LORA_CR         5        // Coding Rate (5 = 4/5)
#endif
#ifndef LORA_SYNC_WORD
#define LORA_SYNC_WORD  0x12     // Private network sync word
#endif
#ifndef LORA_TX_POWER
#define LORA_TX_POWER   14       // dBm
#endif

// ─── Command Bytes ────────────────────────────────────────────
#define LORA_CMD_WAKE_BYTE    0xA1  ///< Send next/random image
#define LORA_CMD_FIRE_BYTE    0xC1  ///< Send image matching "fire"
#define LORA_CMD_ANIMAL_BYTE  0xC2  ///< Send image matching "animal"
#define LORA_CMD_IDX_PREFIX   0xD0  ///< High nibble; low nibble = image index

// ─── Data Types ───────────────────────────────────────────────

/** Decoded intent from a LoRa command packet. */
enum class LoRaCmd : uint8_t {
    WAKE_RANDOM = 0,  ///< 0xA1 — send next/random image
    SEND_FIRE   = 1,  ///< 0xC1 — find image matching "fire"
    SEND_ANIMAL = 2,  ///< 0xC2 — find image matching "animal"
    SEND_INDEX  = 3,  ///< 0xDN — send catalogue image at index N
    UNKNOWN     = 0xFF
};

/** Result of parsing a received LoRa packet. */
struct ParsedCommand {
    LoRaCmd cmd;    ///< decoded command
    uint8_t arg;    ///< image index (used when cmd == SEND_INDEX)
    float   rssi;   ///< received signal strength (dBm)
    float   snr;    ///< signal-to-noise ratio (dB)
};

// ─── Class ────────────────────────────────────────────────────

class LoRaController {
public:
    LoRaController();
    ~LoRaController();

    // ── Lifecycle ─────────────────────────────────────────────

    /**
     * Initialise the FSPI bus and SX1262 with the configured RF parameters.
     * Performs a hardware reset of the radio.
     * @return true on success.
     */
    bool begin();

    /** Power down the radio and release the SPI bus. */
    void end();

    // ── Receive ───────────────────────────────────────────────

    /**
     * Put the SX1262 into continuous receive mode.
     * DIO1 will assert HIGH when a packet is received.
     * Must be called before waitForCommand() or enterDeepSleepRx().
     * @return true on success.
     */
    bool startReceive();

    /**
     * Block until a valid command packet is received or the timeout expires.
     * @param[out] cmd  populated on success.
     * @param timeoutMs maximum wait time in milliseconds (default 3000).
     * @return true if a valid command was decoded before the timeout.
     */
    bool waitForCommand(ParsedCommand& cmd, uint32_t timeoutMs = 3000);

    // ── Deep Sleep ────────────────────────────────────────────

    /**
     * Arm EXT0 wakeup on DIO1 (GPIO LORA_DIO1_PIN, active HIGH) and
     * enter ESP32 deep sleep.  The SX1262 must already be in receive mode.
     *
     * @note This function does NOT return — the device sleeps until a
     *       LoRa packet drives DIO1 HIGH.
     */
    void enterDeepSleepRx();

    // ── Status ────────────────────────────────────────────────

    /**
     * Check whether the last deep-sleep wakeup was caused by the LoRa
     * DIO1 interrupt (EXT0).
     * @return true if woken by LoRa packet.
     */
    static bool wasWokenByLoRa();

    /** @return true if the radio was successfully initialised. */
    bool isReady() const { return _ready; }

private:
    bool       _ready;
    SPIClass*  _spi;
    SX1262*    _radio;

    /** Parse a single command byte from a received payload. */
    ParsedCommand _parseCommandByte(uint8_t byte, float rssi, float snr) const;
};

#endif // LORA_CONTROLLER_H
