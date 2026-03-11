/**
 * @file LoRaRadio.h
 * @brief LoRa SX1280 radio abstraction for the ForestCam mesh network
 *
 * Wraps RadioLib's SX1280 module with project-specific initialization
 * on a dedicated SPI bus (FSPI / SPI2), independent from the SD card
 * which uses HSPI (SPI3).
 *
 * Hardware Wiring (LILYGO T3-S3 V1.2 SX1280 PA):
 *   MOSI -> GPIO 6
 *   MISO -> GPIO 3
 *   SCK  -> GPIO 5
 *   CS   -> GPIO 7
 *   DIO1 -> GPIO 9
 *   BUSY -> GPIO 36
 *   RST  -> GPIO 8
 *   RXEN -> GPIO 21  (PA/LNA RF switch RX enable)
 *   TXEN -> GPIO 10  (PA/LNA RF switch TX enable)
 *
 * @author  CS Group 2
 * @date    2026
 */

#ifndef LORA_RADIO_H
#define LORA_RADIO_H

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// ─── LILYGO T3-S3 V1.2 SX1280 PA Pin Definitions ──────────
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
#define LORA_DIO1   9
#endif
#ifndef LORA_BUSY
#define LORA_BUSY   36
#endif
#ifndef LORA_RST
#define LORA_RST    8
#endif
#ifndef LORA_RXEN
#define LORA_RXEN   21
#endif
#ifndef LORA_TXEN
#define LORA_TXEN   10
#endif

// ─── LoRa RF Parameters (2.4 GHz ISM) ──────────────────────
static constexpr float   LORA_FREQUENCY   = 2400.0;   // MHz
static constexpr float   LORA_BANDWIDTH   = 812.5;    // kHz
static constexpr uint8_t LORA_SF          = 9;         // Spreading factor (5-12)
static constexpr uint8_t LORA_CR          = 7;         // Coding rate 4/7
static constexpr int8_t  LORA_TX_POWER    = 10;        // dBm (max 12.5 for SX1280)
static constexpr uint16_t LORA_PREAMBLE   = 12;        // Preamble symbols

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
     * Initialize the SX1280 on a dedicated FSPI bus.
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

    /** Get raw SX1280 status byte (for diagnostics). */
    uint8_t getStatus();

    /** Get current IRQ flags (for diagnostics). */
    uint16_t getIrqFlags();

    /** Put radio into low-power standby (RC oscillator). Call before WiFi. */
    void standby();

private:
    SPIClass      _loraSPI;
    SPISettings   _loraSPISettings;
    Module        _module;
    SX1280        _radio;
    bool          _initialized;
    float         _lastRSSI;
};

#endif // LORA_RADIO_H
