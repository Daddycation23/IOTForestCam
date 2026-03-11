/**
 * @file LoRaRadio.h
 * @brief LoRa SX1262 radio abstraction for the ForestCam mesh network
 *
 * Wraps RadioLib's SX1262 module with project-specific initialization
 * on a dedicated SPI bus (FSPI / SPI2), independent from the SD card
 * which uses HSPI (SPI3).
 *
 * Hardware Wiring (LILYGO T3-S3 V1.2 -> SX1262):
 *   MOSI -> GPIO 6
 *   MISO -> GPIO 3
 *   SCK  -> GPIO 5
 *   CS   -> GPIO 7
 *   DIO1 -> GPIO 33
 *   BUSY -> GPIO 34
 *   RST  -> GPIO 8
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef LORA_RADIO_H
#define LORA_RADIO_H

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// ─── LILYGO T3-S3 V1.2 LoRa Pin Definitions ────────────────
#ifndef LORA_MOSI
#define LORA_MOSI   6
#endif
#ifndef LORA_MISO
#define LORA_MISO   3
#endif
#ifndef LORA_SCK
#define LORA_SCK    5
#endif
#ifndef LORA_CS
#define LORA_CS     7
#endif
#ifndef LORA_DIO1
#define LORA_DIO1   33
#endif
#ifndef LORA_BUSY
#define LORA_BUSY   34
#endif
#ifndef LORA_RST
#define LORA_RST    8
#endif

// ─── LoRa RF Parameters (923 MHz, Singapore) ────────────────
static constexpr float   LORA_FREQUENCY   = 923.0;    // MHz
static constexpr float   LORA_BANDWIDTH   = 125.0;    // kHz
static constexpr uint8_t LORA_SF          = 9;         // Spreading factor (7-12)
static constexpr uint8_t LORA_CR          = 7;         // Coding rate 4/7
static constexpr int8_t  LORA_TX_POWER    = 14;        // dBm (max 22 for SX1262)
static constexpr uint16_t LORA_PREAMBLE   = 8;         // Preamble symbols

// ─── Receive Result ──────────────────────────────────────────

/**
 * @brief Result of a LoRa packet reception.
 */
struct LoRaRxResult {
    uint8_t  data[64];      ///< Received packet data
    uint8_t  length;        ///< Bytes received
    float    rssi;          ///< Signal strength (dBm)
    float    snr;           ///< Signal-to-noise ratio (dB)
};

// ─── LoRa Radio Class ───────────────────────────────────────

class LoRaRadio {
public:
    LoRaRadio();

    /**
     * Initialize the SX1262 on a dedicated FSPI bus.
     * Must be called after Serial.begin() for error logging.
     * @return true if radio initialized successfully.
     */
    bool begin();

    /**
     * Send a raw packet (blocking, waits for TX complete).
     * @param data   Pointer to packet bytes.
     * @param length Number of bytes to send (max 255).
     * @return true if transmission succeeded.
     */
    bool send(const uint8_t* data, uint8_t length);

    /**
     * Put radio into continuous receive mode (non-blocking).
     * Call checkReceive() in loop() to poll for packets.
     * @return true if radio entered RX mode successfully.
     */
    bool startReceive();

    /**
     * Check if a packet has been received (after startReceive).
     * Non-blocking -- returns false immediately if nothing pending.
     * @param[out] result  Populated if a packet was received.
     * @return true if a packet was available.
     */
    bool checkReceive(LoRaRxResult& result);

    /**
     * Try to receive a packet within the given timeout.
     * Blocking call -- polls until packet arrives or timeout.
     * @param[out] result     Populated on successful receive.
     * @param      timeoutMs  Maximum wait time in milliseconds.
     * @return true if a packet was received.
     */
    bool receive(LoRaRxResult& result, uint32_t timeoutMs = 1000);

    /** @return true if radio has been initialized. */
    bool isReady() const { return _initialized; }

    /** Get last RSSI reading. */
    float lastRSSI() const { return _lastRSSI; }

private:
    SPIClass      _loraSPI;
    SPISettings   _loraSPISettings;
    Module        _module;
    SX1262        _radio;
    bool          _initialized;
    float         _lastRSSI;

    // ─── Raw SPI helpers (SX1262 clone workaround) ──────────
    // These bypass RadioLib's SPI status-byte checking which
    // returns CMD_TIMEOUT on some SX1262 clones.
    void    _rawSpiWrite(const uint8_t* cmd, size_t len);
    uint8_t _rawGetStatus();
    uint8_t _rawReadReg(uint16_t addr);
    void    _rawGetRxBufferStatus(uint8_t& payloadLen, uint8_t& rxStartPtr);
    void    _rawReadBuffer(uint8_t offset, uint8_t* data, uint8_t len);
};

#endif // LORA_RADIO_H
