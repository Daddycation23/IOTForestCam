/**
 * @file LoRaRadio.cpp
 * @brief LoRa SX1280 radio implementation
 *
 * Uses FSPI (SPI2) bus to avoid conflicts with the SD card on HSPI (SPI3).
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "LoRaRadio.h"
#include "LoRaStatus.h"
#include <atomic>

static const char* TAG = "LoRaRadio";

// ISR flag for DIO1 interrupt detection — atomic for dual-core cache coherence
static std::atomic<bool> _dio1Fired{false};
static void IRAM_ATTR _dio1ISR() { _dio1Fired.store(true, std::memory_order_release); }

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

    // ── Pin pre-configuration ─────────────────────────────────
    pinMode(LORA_CS, OUTPUT);
    digitalWrite(LORA_CS, HIGH);
    pinMode(LORA_BUSY, INPUT);

    log_i("%s: Pin check: DIO1(GPIO%d)=%d, BUSY(GPIO%d)=%d, CS(GPIO%d)=%d",
          TAG, LORA_DIO1, digitalRead(LORA_DIO1),
          LORA_BUSY, digitalRead(LORA_BUSY),
          LORA_CS, digitalRead(LORA_CS));

    // ── Retry loop — up to 3 attempts ─────────────────────────
    for (int attempt = 1; attempt <= 3; attempt++) {
        log_i("%s: Init attempt %d/3", TAG, attempt);

        // ── Manual hardware reset ─────────────────────────────
        pinMode(LORA_RST, OUTPUT);
        digitalWrite(LORA_RST, LOW);
        delay(50);
        digitalWrite(LORA_RST, HIGH);

        uint32_t busyStart = millis();
        while (digitalRead(LORA_BUSY) == HIGH) {
            if (millis() - busyStart > 2000) {
                log_e("%s: SX1280 BUSY timeout after reset (>2s)", TAG);
                break;
            }
            delay(1);
        }
        log_i("%s: SX1280 BUSY released after %lu ms", TAG, millis() - busyStart);

        // ── SPI bus init ──────────────────────────────────────
        _loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI);
        delay(10);

        // ── RadioLib SX1280 init ──────────────────────────────
        log_i("%s: Initializing SX1280 at %.1f MHz, SF%u, BW%.1f kHz...",
              TAG, LORA_FREQUENCY, LORA_SF, LORA_BANDWIDTH);

        int state = _radio.begin(
            LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SF, LORA_CR,
            RADIOLIB_SX128X_SYNC_WORD_PRIVATE, LORA_TX_POWER, LORA_PREAMBLE
        );

        if (state == RADIOLIB_ERR_NONE) {
            // Configure PA/LNA RF switch pins
            _radio.setRfSwitchPins(LORA_RXEN, LORA_TXEN);

            _initialized = true;
            log_i("%s: SX1280 ready (attempt %d)", TAG, attempt);
            return true;
        }

        log_e("%s: SX1280 init FAILED attempt %d/3, error %d", TAG, attempt, state);
        _loraSPI.end();
        delay(200);
    }

    log_e("%s: SX1280 init FAILED after 3 attempts", TAG);
    return false;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Transmit
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool LoRaRadio::send(const uint8_t* data, uint8_t length) {
    if (!_initialized) {
        log_e("%s: Not initialized", TAG);
        return false;
    }

    int state = _radio.startTransmit(const_cast<uint8_t*>(data), length);
    if (state != RADIOLIB_ERR_NONE) {
        log_e("%s: TX startTransmit failed, error %d", TAG, state);
        return false;
    }

    // Wait for calculated time-on-air + 50% safety margin
    RadioLibTime_t toaUs = _radio.getTimeOnAir(length);
    uint32_t waitMs = (uint32_t)((toaUs * 1.5) / 1000) + 10;
    delay(waitMs);

    _radio.finishTransmit();

    _txCount++;
    log_d("%s: TX done (%u bytes, waited %ums)", TAG, length, waitMs);
    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Receive (non-blocking continuous mode)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool LoRaRadio::startReceive() {
    if (!_initialized) return false;

    int state = _radio.startReceive();
    _dio1Fired.store(false, std::memory_order_relaxed);
    attachInterrupt(digitalPinToInterrupt(LORA_DIO1), _dio1ISR, RISING);

    if (state == RADIOLIB_ERR_NONE) {
        log_d("%s: startReceive() OK", TAG);
        return true;
    }

    log_e("%s: startReceive() failed, error %d", TAG, state);
    return false;
}

bool LoRaRadio::checkReceive(LoRaRxResult& result) {
    if (!_initialized) return false;

    bool packetDetected = false;

    // ── Strategy 1: DIO1 ISR (atomic exchange for dual-core safety) ──
    if (_dio1Fired.exchange(false, std::memory_order_acquire)) {
        packetDetected = true;
    }

    // ── Strategy 2: DIO1 GPIO poll ──────────────────────────
    if (!packetDetected && digitalRead(LORA_DIO1) == HIGH) {
        packetDetected = true;
    }

    if (!packetDetected) return false;

    // ── Read packet data via RadioLib ───────────────────────
    uint8_t buf[64];
    size_t len = sizeof(buf);
    int state = _radio.readData(buf, len);

    if (state != RADIOLIB_ERR_NONE) {
        _rxErrorCount++;
        log_d("%s: readData returned %d (false positive or CRC error)", TAG, state);
        _radio.startReceive();  // Re-enter RX
        return false;
    }

    // Copy to result
    result.length = (len > sizeof(result.data)) ? sizeof(result.data) : (uint8_t)len;
    memcpy(result.data, buf, result.length);
    result.rssi  = _radio.getRSSI();
    result.snr   = _radio.getSNR();
    _lastRSSI    = result.rssi;
    _rxCount++;

    log_d("%s: RX packet (%u bytes, RSSI=%.0f dBm, SNR=%.1f dB)",
          TAG, result.length, result.rssi, result.snr);

    // Re-enter continuous RX
    _radio.startReceive();

    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Receive (blocking with timeout)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool LoRaRadio::receive(LoRaRxResult& result, uint32_t timeoutMs) {
    if (!_initialized) return false;

    if (!startReceive()) return false;

    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        if (checkReceive(result)) return true;
        delay(10);
    }

    return false;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Diagnostics
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

uint8_t LoRaRadio::getStatus() {
    if (!_initialized) return 0;
    // Minimal raw SPI GetStatus — RadioLib doesn't expose this directly
    _loraSPI.beginTransaction(_loraSPISettings);
    digitalWrite(LORA_CS, LOW);
    _loraSPI.transfer(0xC0);  // GetStatus opcode (same for SX1280)
    uint8_t st = _loraSPI.transfer(0x00);
    digitalWrite(LORA_CS, HIGH);
    _loraSPI.endTransaction();
    return st;
}

uint16_t LoRaRadio::getIrqFlags() {
    if (!_initialized) return 0;
    return _radio.getIrqStatus();
}

void LoRaRadio::standby() {
    if (!_initialized) return;
    detachInterrupt(digitalPinToInterrupt(LORA_DIO1));
    _radio.standby();
    log_i("%s: Radio in standby (low power)", TAG);
}
