/**
 * @file LoRaController.cpp
 * @brief Part 2 — LoRa Control Plane implementation.
 *
 * Uses RadioLib to drive the onboard SX1262 on the LILYGO T3-S3 V1.2.
 * The SX1262 runs on the FSPI bus (pins 5/3/6/7) independently of the
 * SD card's HSPI bus (pins 14/2/11/13).
 *
 * @author  CS Group 19
 * @date    2026
 */

#include "LoRaController.h"

static const char* TAG = "LoRaCtrl";

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Constructor / Destructor
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

LoRaController::LoRaController()
    : _ready(false), _spi(nullptr), _radio(nullptr)
{}

LoRaController::~LoRaController() {
    end();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Lifecycle
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool LoRaController::begin() {
    if (_ready) return true;

    // Allocate FSPI bus — SD card already claims HSPI, no conflict.
    _spi = new SPIClass(FSPI);
    _spi->begin(LORA_SCK_PIN, LORA_MISO_PIN, LORA_MOSI_PIN, LORA_NSS_PIN);

    // Module(NSS, DIO1, RST, BUSY, spi)
    _radio = new SX1262(new Module(LORA_NSS_PIN, LORA_DIO1_PIN,
                                   LORA_RST_PIN, LORA_BUSY_PIN, *_spi));

    int16_t state = _radio->begin(
        LORA_FREQUENCY,
        LORA_BANDWIDTH,
        LORA_SF,
        LORA_CR,
        LORA_SYNC_WORD,
        LORA_TX_POWER
    );

    if (state != RADIOLIB_ERR_NONE) {
        log_e("%s: SX1262 begin() failed, code %d", TAG, state);
        delete _radio;  _radio = nullptr;
        _spi->end();
        delete _spi;    _spi = nullptr;
        return false;
    }

    // DIO2 doubles as RF switch on the T3-S3 V1.2 module.
    _radio->setDio2AsRfSwitch(true);

    log_i("%s: SX1262 ready — %.1f MHz, SF%d, BW%.0f kHz, +%d dBm",
          TAG, LORA_FREQUENCY, LORA_SF, LORA_BANDWIDTH, LORA_TX_POWER);

    _ready = true;
    return true;
}

void LoRaController::end() {
    if (_radio) {
        _radio->sleep();
        delete _radio;  _radio = nullptr;
    }
    if (_spi) {
        _spi->end();
        delete _spi;    _spi = nullptr;
    }
    _ready = false;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Receive
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool LoRaController::startReceive() {
    if (!_ready) return false;

    int16_t state = _radio->startReceive();
    if (state != RADIOLIB_ERR_NONE) {
        log_e("%s: startReceive() failed, code %d", TAG, state);
        return false;
    }

    log_i("%s: Listening for LoRa packets (%.1f MHz)...", TAG, LORA_FREQUENCY);
    return true;
}

bool LoRaController::waitForCommand(ParsedCommand& cmd, uint32_t timeoutMs) {
    if (!_ready) return false;

    uint32_t start = millis();

    // Poll DIO1: it goes HIGH when the SX1262 RxDone IRQ fires.
    while ((millis() - start) < timeoutMs) {
        if (digitalRead(LORA_DIO1_PIN) == HIGH) {
            uint8_t buf[16] = {0};
            int16_t state   = _radio->readData(buf, sizeof(buf));

            if (state != RADIOLIB_ERR_NONE) {
                log_e("%s: readData() failed, code %d", TAG, state);
                // Restart RX and keep waiting
                _radio->startReceive();
                continue;
            }

            float rssi = _radio->getRSSI();
            float snr  = _radio->getSNR();

            log_i("%s: Packet — RSSI %.1f dBm, SNR %.1f dB, cmd=0x%02X",
                  TAG, rssi, snr, buf[0]);

            cmd = _parseCommandByte(buf[0], rssi, snr);

            if (cmd.cmd == LoRaCmd::UNKNOWN) {
                log_w("%s: Unknown cmd 0x%02X, keep listening", TAG, buf[0]);
                _radio->startReceive();
                continue;
            }

            return true;
        }
        delay(10);
    }

    log_w("%s: waitForCommand() timed out after %lu ms", TAG, timeoutMs);
    return false;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Deep Sleep
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void LoRaController::enterDeepSleepRx() {
    // The radio must already be in startReceive() so that DIO1 fires
    // when the next packet arrives.  We arm EXT0 on DIO1 (active HIGH)
    // and hand control to the ESP32 power manager.

    log_i("%s: Arming EXT0 on DIO1 (GPIO %d), entering deep sleep...",
          TAG, LORA_DIO1_PIN);

    Serial.flush();
    delay(20);

    esp_sleep_enable_ext0_wakeup(static_cast<gpio_num_t>(LORA_DIO1_PIN), 1);
    esp_deep_sleep_start();
    // ── Does not return ──
}

bool LoRaController::wasWokenByLoRa() {
    return (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Private
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

ParsedCommand LoRaController::_parseCommandByte(uint8_t byte,
                                                  float rssi,
                                                  float snr) const {
    ParsedCommand cmd;
    cmd.rssi = rssi;
    cmd.snr  = snr;
    cmd.arg  = 0;

    switch (byte) {
        case LORA_CMD_WAKE_BYTE:
            cmd.cmd = LoRaCmd::WAKE_RANDOM;
            break;
        case LORA_CMD_FIRE_BYTE:
            cmd.cmd = LoRaCmd::SEND_FIRE;
            break;
        case LORA_CMD_ANIMAL_BYTE:
            cmd.cmd = LoRaCmd::SEND_ANIMAL;
            break;
        default:
            if ((byte & 0xF0) == LORA_CMD_IDX_PREFIX) {
                // 0xD0–0xDF: low nibble encodes the image index (0–15)
                cmd.cmd = LoRaCmd::SEND_INDEX;
                cmd.arg = byte & 0x0F;
            } else {
                cmd.cmd = LoRaCmd::UNKNOWN;
            }
            break;
    }

    return cmd;
}
