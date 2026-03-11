/**
 * @file LoRaRadio.cpp
 * @brief LoRa SX1262 radio implementation
 *
 * Uses FSPI (SPI2) bus to avoid conflicts with the SD card on HSPI (SPI3).
 *
 * Contains workarounds for SX1262 clone modules found on some LILYGO T3-S3
 * V1.2 boards.  These clones have broken SPI data reads (all register reads
 * return 0x00), non-functional DIO1/IRQ, and the chip gets stuck in FS mode
 * instead of entering RX.  The workarounds are backward-compatible with
 * genuine SX1262 chips.
 *
 *   TX workaround:  time-based wait (startTransmit + delay + finishTransmit)
 *   RX workaround:  multi-strategy detection (DIO1 ISR, GPIO poll, IRQ
 *                   register, periodic buffer poll) + raw SPI fallback
 *
 * @author  CS Group 2
 * @date    2026
 */

#include "LoRaRadio.h"
#include "LoRaStatus.h"

static const char* TAG = "LoRaRadio";

// ISR flag for DIO1 interrupt detection (catches even sub-µs pulses)
static volatile bool _dio1Fired = false;
static void IRAM_ATTR _dio1ISR() { _dio1Fired = true; }

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
// Raw SPI helpers — bypass RadioLib's status-byte checking
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Some SX1262 clones report stale CMD_TIMEOUT in the SPI status byte,
// causing RadioLib to abort with error -705.  Write commands execute
// correctly despite the stale status, so these helpers bypass the check.

void LoRaRadio::_rawSpiWrite(const uint8_t* cmd, size_t len) {
    _loraSPI.beginTransaction(_loraSPISettings);
    digitalWrite(LORA_CS, LOW);
    for (size_t i = 0; i < len; i++) _loraSPI.transfer(cmd[i]);
    digitalWrite(LORA_CS, HIGH);
    _loraSPI.endTransaction();
    delayMicroseconds(100);
    uint32_t t = millis();
    while (digitalRead(LORA_BUSY)) {
        if (millis() - t > 1000) break;
        delay(1);
    }
}

uint8_t LoRaRadio::_rawGetStatus() {
    _loraSPI.beginTransaction(_loraSPISettings);
    digitalWrite(LORA_CS, LOW);
    _loraSPI.transfer(0xC0);  // GetStatus
    uint8_t st = _loraSPI.transfer(0x00);
    digitalWrite(LORA_CS, HIGH);
    _loraSPI.endTransaction();
    return st;
}

uint8_t LoRaRadio::_rawReadReg(uint16_t addr) {
    _loraSPI.beginTransaction(_loraSPISettings);
    digitalWrite(LORA_CS, LOW);
    _loraSPI.transfer(0x1D);
    _loraSPI.transfer((addr >> 8) & 0xFF);
    _loraSPI.transfer(addr & 0xFF);
    _loraSPI.transfer(0x00);  // NOP (status byte)
    uint8_t val = _loraSPI.transfer(0x00);
    digitalWrite(LORA_CS, HIGH);
    _loraSPI.endTransaction();
    return val;
}

void LoRaRadio::_rawGetRxBufferStatus(uint8_t& payloadLen, uint8_t& rxStartPtr) {
    _loraSPI.beginTransaction(_loraSPISettings);
    digitalWrite(LORA_CS, LOW);
    _loraSPI.transfer(0x13);  // GetRxBufferStatus
    _loraSPI.transfer(0x00);  // Status (ignore)
    payloadLen = _loraSPI.transfer(0x00);
    rxStartPtr = _loraSPI.transfer(0x00);
    digitalWrite(LORA_CS, HIGH);
    _loraSPI.endTransaction();
}

void LoRaRadio::_rawReadBuffer(uint8_t offset, uint8_t* data, uint8_t len) {
    _loraSPI.beginTransaction(_loraSPISettings);
    digitalWrite(LORA_CS, LOW);
    _loraSPI.transfer(0x1E);  // ReadBuffer
    _loraSPI.transfer(offset);
    _loraSPI.transfer(0x00);  // NOP (status byte)
    for (uint8_t i = 0; i < len; i++) {
        data[i] = _loraSPI.transfer(0x00);
    }
    digitalWrite(LORA_CS, HIGH);
    _loraSPI.endTransaction();
}

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
                log_e("%s: SX1262 BUSY timeout after reset (>2s)", TAG);
                break;
            }
            delay(1);
        }
        log_i("%s: SX1262 BUSY released after %lu ms", TAG, millis() - busyStart);

        // ── SPI bus init ──────────────────────────────────────
        _loraSPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI);
        delay(10);

        // ── RadioLib SX1262 init ──────────────────────────────
        log_i("%s: Initializing SX1262 at %.1f MHz, SF%u, BW%.0f kHz...",
              TAG, LORA_FREQUENCY, LORA_SF, LORA_BANDWIDTH);

        int state = _radio.begin(
            LORA_FREQUENCY, LORA_BANDWIDTH, LORA_SF, LORA_CR,
            RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
            LORA_TX_POWER, LORA_PREAMBLE,
            1.6  // tcxoVoltage — T3-S3 V1.2 TCXO via DIO3
        );

        if (state == RADIOLIB_ERR_NONE) {
            _radio.setCurrentLimit(140.0);
            _radio.setDio2AsRfSwitch(true);

            _initialized = true;
            log_i("%s: SX1262 ready (attempt %d)", TAG, attempt);
            return true;
        }

        log_e("%s: SX1262 init FAILED attempt %d/3, error %d", TAG, attempt, state);
        _loraSPI.end();
        delay(200);
    }

    log_e("%s: SX1262 init FAILED after 3 attempts", TAG);
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

    // ── Time-based TX workaround for SX1262 clones ────────────
    // These modules enter TX mode but DIO1 never fires TX_DONE.
    // Use startTransmit() + calculated time-on-air delay instead
    // of RadioLib's transmit() which polls DIO1 and times out.

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

    log_d("%s: TX done (%u bytes, waited %ums)", TAG, length, waitMs);
    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Receive (non-blocking continuous mode)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool LoRaRadio::startReceive() {
    if (!_initialized) return false;

    // ── Try RadioLib (tolerates CMD_TIMEOUT from clone chips) ──
    int state = _radio.startReceive();
    if (state == RADIOLIB_ERR_NONE || state == RADIOLIB_ERR_SPI_CMD_TIMEOUT) {
        _dio1Fired = false;
        attachInterrupt(digitalPinToInterrupt(LORA_DIO1), _dio1ISR, RISING);
        uint8_t st = _rawGetStatus();
        uint8_t mode = (st >> 4) & 0x07;
        log_i("%s: startReceive() RadioLib returned %d, status=0x%02X, mode=%u %s",
              TAG, state, st, mode,
              mode == 2 ? "(STBY_RC)" : mode == 3 ? "(STBY_XOSC)" :
              mode == 4 ? "(FS)" : mode == 5 ? "(RX)" :
              mode == 6 ? "(TX)" : "(UNKNOWN)");
        if (mode == 5) {
            return true;
        }
        // Not in RX (stuck in FS on clone chips) — try raw SPI
        log_i("%s: Not in RX — falling through to raw SPI fallback", TAG);
    } else {
        log_e("%s: startReceive() RadioLib error %d — trying raw SPI", TAG, state);
    }

    // ── Raw SPI fallback (full re-init for RX) ─────────────────
    // The radio is stuck in FS mode — the PLL can't lock into RX.
    // Re-issue TCXO, calibration, and frequency commands, then try
    // multiple RX strategies (continuous, finite timeout, SetFs pre-warm).

    // 1. Go to STANDBY_XOSC (0x01) — ensures TCXO is active and stable
    //    (STBY_RC can leave the TCXO off, causing PLL lock failure on clones)
    uint8_t cmd_stby[] = {0x80, 0x01};
    _rawSpiWrite(cmd_stby, sizeof(cmd_stby));
    delay(5);

    // 2. SetDio3AsTcxoCtrl — T3-S3 V1.2 uses TCXO via DIO3, 1.6V, 10ms timeout
    //    Longer timeout (640 ticks = 10ms) for clone TCXO stabilization
    uint8_t cmd_tcxo[] = {0x97, 0x00, 0x00, 0x02, 0x80};
    _rawSpiWrite(cmd_tcxo, sizeof(cmd_tcxo));
    delay(10);  // Let TCXO fully stabilize

    // 3. Calibrate all blocks (RC64k, RC13M, PLL, ADC pulse, ADC bulk N/P, image)
    uint8_t cmd_cal[] = {0x89, 0x7F};
    _rawSpiWrite(cmd_cal, sizeof(cmd_cal));
    delay(10);

    // 4. CalibrateImage for 902-928 MHz band (ISM Singapore 923 MHz)
    uint8_t cmd_calimg[] = {0x98, 0xE1, 0xE9};
    _rawSpiWrite(cmd_calimg, sizeof(cmd_calimg));
    delay(5);

    // 5. SetRegulatorMode(DC-DC) — T3-S3 uses DC-DC, not LDO
    uint8_t cmd_reg[] = {0x96, 0x01};
    _rawSpiWrite(cmd_reg, sizeof(cmd_reg));

    // 6. SetPacketType(LoRa)
    uint8_t cmd_pkt[] = {0x8A, 0x01};
    _rawSpiWrite(cmd_pkt, sizeof(cmd_pkt));

    // 7. SetRfFrequency — 923.0 MHz = 0x39B00000
    uint8_t cmd_freq[] = {0x86, 0x39, 0xB0, 0x00, 0x00};
    _rawSpiWrite(cmd_freq, sizeof(cmd_freq));

    // 8. SetDio2AsRfSwitch(enable) — needed for T3-S3 RF path
    uint8_t cmd_dio2[] = {0x9D, 0x01};
    _rawSpiWrite(cmd_dio2, sizeof(cmd_dio2));

    // 9. SetModulationParams(SF9, BW125, CR4/7, LDR off)
    uint8_t cmd_mod[] = {0x8B, 0x09, 0x04, 0x03, 0x00};
    _rawSpiWrite(cmd_mod, sizeof(cmd_mod));

    // 10. SetPacketParams(preamble=8, header=explicit, maxPayload=64, CRC on, no invert)
    uint8_t cmd_pktp[] = {0x8C, 0x00, 0x08, 0x00, 0x40, 0x01, 0x00};
    _rawSpiWrite(cmd_pktp, sizeof(cmd_pktp));

    // 11. Set sync word to PRIVATE (0x1424) — registers 0x0740, 0x0741
    uint8_t cmd_sw1[] = {0x0D, 0x07, 0x40, 0x14};
    _rawSpiWrite(cmd_sw1, sizeof(cmd_sw1));
    uint8_t cmd_sw2[] = {0x0D, 0x07, 0x41, 0x24};
    _rawSpiWrite(cmd_sw2, sizeof(cmd_sw2));

    // 12. ClearDeviceErrors + ClearIrqStatus
    uint8_t cmd_clr_err[] = {0x07, 0x00, 0x00};
    _rawSpiWrite(cmd_clr_err, sizeof(cmd_clr_err));
    uint8_t cmd_clr[] = {0x02, 0xFF, 0xFF};
    _rawSpiWrite(cmd_clr, sizeof(cmd_clr));

    // 13. SetBufferBaseAddress(TX=0, RX=0)
    uint8_t cmd_buf[] = {0x8F, 0x00, 0x00};
    _rawSpiWrite(cmd_buf, sizeof(cmd_buf));

    // 14. SetDioIrqParams — RxDone on DIO1
    uint8_t cmd_irq[] = {0x08, 0x02, 0x02, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00};
    _rawSpiWrite(cmd_irq, sizeof(cmd_irq));

    _dio1Fired = false;
    attachInterrupt(digitalPinToInterrupt(LORA_DIO1), _dio1ISR, RISING);

    // Read device errors for diagnostics
    uint8_t errH = _rawReadReg(0x0320);
    uint8_t errL = _rawReadReg(0x0321);
    uint16_t devErr = ((uint16_t)errH << 8) | errL;
    log_i("%s: DeviceErrors=0x%04X before SetRx", TAG, devErr);

    // ── Strategy A: SetFs first to pre-warm PLL, then SetRx ─────
    {
        uint8_t cmd_fs[] = {0xC1};   // SetFs — force PLL lock in FS mode
        _rawSpiWrite(cmd_fs, sizeof(cmd_fs));
        delay(5);  // Let PLL lock in FS

        uint8_t cmd_rx[] = {0x82, 0xFF, 0xFF, 0xFF};  // Continuous RX
        _rawSpiWrite(cmd_rx, sizeof(cmd_rx));
        delay(20);

        uint8_t st = _rawGetStatus();
        uint8_t mode = sx1262ExtractMode(st);
        log_i("%s: Strategy A (SetFs+SetRx continuous), status=0x%02X, mode=%u %s",
              TAG, st, mode, mode == SX1262_CHIPMODE_RX ? "(RX) OK" : "(NOT RX)");
        if (evaluateStartReceiveResult(st)) return true;
    }

    // ── Strategy B: Finite RX timeout (clone workaround) ────────
    // Some SX1262 clones can't handle continuous RX (0xFFFFFF) but
    // accept a finite timeout.  Use ~10 s = 640000 ticks = 0x09C400
    {
        uint8_t cmd_clr2[] = {0x02, 0xFF, 0xFF};
        _rawSpiWrite(cmd_clr2, sizeof(cmd_clr2));

        uint8_t cmd_stby2[] = {0x80, 0x01};
        _rawSpiWrite(cmd_stby2, sizeof(cmd_stby2));
        delay(2);

        uint8_t cmd_rx[] = {0x82, 0x09, 0xC4, 0x00};  // ~10 s timeout
        _rawSpiWrite(cmd_rx, sizeof(cmd_rx));
        delay(20);

        uint8_t st = _rawGetStatus();
        uint8_t mode = sx1262ExtractMode(st);
        log_i("%s: Strategy B (SetRx finite 10s), status=0x%02X, mode=%u %s",
              TAG, st, mode, mode == SX1262_CHIPMODE_RX ? "(RX) OK" : "(NOT RX)");
        if (evaluateStartReceiveResult(st)) return true;
    }

    // ── Strategy C: RxDutyCycle — alternates sleep/RX ───────────
    // SetRxDutyCycle(rxPeriod=0x01F400 ~2s, sleepPeriod=0x003200 ~0.8s)
    {
        uint8_t cmd_clr3[] = {0x02, 0xFF, 0xFF};
        _rawSpiWrite(cmd_clr3, sizeof(cmd_clr3));

        uint8_t cmd_stby3[] = {0x80, 0x01};
        _rawSpiWrite(cmd_stby3, sizeof(cmd_stby3));
        delay(2);

        uint8_t cmd_dc[] = {0x94, 0x01, 0xF4, 0x00, 0x00, 0x32, 0x00};
        _rawSpiWrite(cmd_dc, sizeof(cmd_dc));
        delay(50);

        uint8_t st = _rawGetStatus();
        uint8_t mode = sx1262ExtractMode(st);
        log_i("%s: Strategy C (RxDutyCycle), status=0x%02X, mode=%u %s",
              TAG, st, mode, mode == SX1262_CHIPMODE_RX ? "(RX) OK" : "(NOT RX)");
        if (evaluateStartReceiveResult(st)) return true;
    }

    // ── All strategies failed — hardware reset + reinit ─────────
    log_w("%s: All RX strategies failed — attempting hardware reset", TAG);
    _initialized = false;
    _loraSPI.end();
    delay(100);

    if (begin()) {
        int state = _radio.startReceive();
        if (state == RADIOLIB_ERR_NONE || state == RADIOLIB_ERR_SPI_CMD_TIMEOUT) {
            _dio1Fired = false;
            attachInterrupt(digitalPinToInterrupt(LORA_DIO1), _dio1ISR, RISING);
            uint8_t st = _rawGetStatus();
            log_i("%s: After HW reset, status=0x%02X, mode=%u",
                  TAG, st, sx1262ExtractMode(st));
            if (evaluateStartReceiveResult(st)) {
                return true;
            }
        }
    }

    log_e("%s: startReceive FAILED — radio not in RX mode", TAG);
    return false;
}

uint8_t LoRaRadio::getStatus() {
    if (!_initialized) return 0;
    return _rawGetStatus();
}

uint16_t LoRaRadio::getIrqFlags() {
    if (!_initialized) return 0;
    uint8_t irqH = _rawReadReg(0x0044);
    uint8_t irqL = _rawReadReg(0x0045);
    return ((uint16_t)irqH << 8) | irqL;
}

bool LoRaRadio::checkReceive(LoRaRxResult& result) {
    if (!_initialized) return false;

    bool packetDetected = false;

    // ── Strategy 1: DIO1 ISR ────────────────────────────────
    if (_dio1Fired) {
        _dio1Fired = false;
        packetDetected = true;
    }

    // ── Strategy 2: DIO1 GPIO poll ──────────────────────────
    if (!packetDetected && digitalRead(LORA_DIO1) == HIGH) {
        packetDetected = true;
    }

    // ── Strategy 3: IRQ register read ───────────────────────
    if (!packetDetected) {
        uint8_t irqH = _rawReadReg(0x0044);
        uint8_t irqL = _rawReadReg(0x0045);
        uint16_t irq = ((uint16_t)irqH << 8) | irqL;
        if (irq & 0x0002) {  // RX_DONE
            packetDetected = true;
        }
    }

    // ── Strategy 4: Periodic buffer status poll ─────────────
    static uint32_t lastBufPoll = 0;
    if (!packetDetected && (millis() - lastBufPoll) >= 100) {
        lastBufPoll = millis();
        uint8_t payloadLen = 0, rxStartPtr = 0;
        _rawGetRxBufferStatus(payloadLen, rxStartPtr);
        if (payloadLen > 0 && payloadLen <= sizeof(result.data)) {
            packetDetected = true;
        }
    }

    if (!packetDetected) return false;

    // ── Read packet data ────────────────────────────────────
    uint8_t payloadLen = 0, rxStartPtr = 0;
    _rawGetRxBufferStatus(payloadLen, rxStartPtr);

    if (payloadLen == 0 || payloadLen > sizeof(result.data)) {
        // False positive — clear IRQ and resume
        uint8_t cmd_clr[] = {0x02, 0xFF, 0xFF};
        _rawSpiWrite(cmd_clr, sizeof(cmd_clr));
        return false;
    }

    _rawReadBuffer(rxStartPtr, result.data, payloadLen);
    result.length = payloadLen;
    result.rssi   = -(float)_rawReadReg(0x0936) / 2.0f;
    result.snr    =  (float)((int8_t)_rawReadReg(0x0935)) / 4.0f;
    _lastRSSI     = result.rssi;

    log_d("%s: RX packet (%u bytes, RSSI=%.0f dBm, SNR=%.1f dB)",
          TAG, result.length, result.rssi, result.snr);

    // Clear IRQ and re-enter continuous RX
    {
        uint8_t clr[] = {0x02, 0xFF, 0xFF};
        _rawSpiWrite(clr, sizeof(clr));
        uint8_t rx[] = {0x82, 0xFF, 0xFF, 0xFF};
        _rawSpiWrite(rx, sizeof(rx));
    }

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
