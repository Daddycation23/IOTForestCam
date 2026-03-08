/**
 * @file LoRaRadio.cpp
 * @brief LoRa SX1262 radio implementation
 *
 * Uses FSPI (SPI2) bus to avoid conflicts with the SD card on HSPI (SPI3).
 * All RadioLib calls are blocking for simplicity — suitable for the
 * beacon-based control plane where latency is not critical.
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "LoRaRadio.h"

static const char* TAG = "LoRaRadio";

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Constructor
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

LoRaRadio::LoRaRadio()
    : _loraSPI(FSPI)
    , _loraSPISettings(2000000, MSBFIRST, SPI_MODE0)
    , _module(LORA_CS, LORA_DIO1, LORA_RST, LORA_BUSY, _loraSPI, _loraSPISettings)
    , _radio(&_module)
    , _initialized(false)
    , _lastRSSI(0.0f)
{}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Initialization
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool LoRaRadio::begin() {
    if (_initialized) {
        log_w("%s: Already initialized", TAG);
        return true;
    }

    // Initialize the dedicated SPI bus for LoRa
    _loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);

    // Initialize SX1262 with RF parameters
    log_i("%s: Initializing SX1262 at %.1f MHz, SF%u, BW%.0f kHz...",
          TAG, LORA_FREQUENCY, LORA_SF, LORA_BANDWIDTH);

    int state = _radio.begin(
        LORA_FREQUENCY,     // Carrier frequency (MHz)
        LORA_BANDWIDTH,     // Bandwidth (kHz)
        LORA_SF,            // Spreading factor
        LORA_CR,            // Coding rate
        RADIOLIB_SX126X_SYNC_WORD_PRIVATE,  // Sync word (private network)
        LORA_TX_POWER,      // Output power (dBm)
        LORA_PREAMBLE       // Preamble length (symbols)
    );

    if (state != RADIOLIB_ERR_NONE) {
        log_e("%s: SX1262 init FAILED, error code %d", TAG, state);
        _loraSPI.end();
        return false;
    }

    // SX1262-specific configuration
    _radio.setCurrentLimit(60.0);        // Over-current protection (mA)
    _radio.setDio2AsRfSwitch(true);      // Use DIO2 to control RF switch (T3-S3 design)

    _initialized = true;

    log_i("%s: SX1262 ready at %.1f MHz, SF%u, BW%.0f kHz, TX %d dBm",
          TAG, LORA_FREQUENCY, LORA_SF, LORA_BANDWIDTH, LORA_TX_POWER);

    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Transmit
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool LoRaRadio::send(const uint8_t* data, uint8_t length) {
    if (!_initialized) {
        log_e("%s: Not initialized", TAG);
        return false;
    }

    int state = _radio.transmit(const_cast<uint8_t*>(data), length);

    if (state == RADIOLIB_ERR_NONE) {
        log_d("%s: TX OK (%u bytes)", TAG, length);
        return true;
    }

    if (state == RADIOLIB_ERR_PACKET_TOO_LONG) {
        log_e("%s: TX packet too long (%u bytes)", TAG, length);
    } else if (state == RADIOLIB_ERR_TX_TIMEOUT) {
        log_e("%s: TX timeout", TAG);
    } else {
        log_e("%s: TX failed, error %d", TAG, state);
    }

    return false;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Receive (non-blocking continuous mode)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool LoRaRadio::startReceive() {
    if (!_initialized) return false;

    int state = _radio.startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        log_e("%s: startReceive failed, error %d", TAG, state);
        return false;
    }

    log_d("%s: Entered continuous RX mode", TAG);
    return true;
}

bool LoRaRadio::checkReceive(LoRaRxResult& result) {
    if (!_initialized) return false;

    // Check if DIO1 flagged a received packet
    // RadioLib uses getIrqStatus() internally via readData()
    uint8_t buf[64];
    size_t len = sizeof(buf);

    int state = _radio.readData(buf, 0);

    if (state == RADIOLIB_ERR_NONE) {
        // Packet received successfully
        size_t actualLen = _radio.getPacketLength();
        if (actualLen > sizeof(result.data)) {
            actualLen = sizeof(result.data);
        }

        // Re-read with correct length
        memcpy(result.data, buf, actualLen);
        result.length = (uint8_t)actualLen;
        result.rssi   = _radio.getRSSI();
        result.snr    = _radio.getSNR();
        _lastRSSI     = result.rssi;

        log_d("%s: RX packet (%u bytes, RSSI=%.0f dBm, SNR=%.1f dB)",
              TAG, result.length, result.rssi, result.snr);

        return true;
    }

    // RADIOLIB_ERR_RX_TIMEOUT or RADIOLIB_ERR_LORA_HEADER_DAMAGED = no packet yet
    return false;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Receive (blocking with timeout)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool LoRaRadio::receive(LoRaRxResult& result, uint32_t timeoutMs) {
    if (!_initialized) return false;

    uint8_t buf[64];
    int state = _radio.receive(buf, sizeof(buf));

    if (state == RADIOLIB_ERR_NONE) {
        size_t actualLen = _radio.getPacketLength();
        if (actualLen > sizeof(result.data)) {
            actualLen = sizeof(result.data);
        }

        memcpy(result.data, buf, actualLen);
        result.length = (uint8_t)actualLen;
        result.rssi   = _radio.getRSSI();
        result.snr    = _radio.getSNR();
        _lastRSSI     = result.rssi;

        return true;
    }

    return false;
}
