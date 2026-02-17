/**
 * @file StorageReader.cpp
 * @brief Part 1 — Implementation of the SD-based Storage Reader.
 *
 * Power Notes:
 *   - SD card is mounted only during active capture cycles.
 *   - SPI bus is explicitly ended in end() so the SD module
 *     draws near-zero current during deep sleep.
 *   - File handles are closed as early as possible.
 *
 * @author  CS Group 19
 * @date    2026
 */

#include "StorageReader.h"

// ─── Logging Tag ─────────────────────────────────────────────
static const char* TAG = "StorageReader";

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Constructor
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
StorageReader::StorageReader()
    : _mounted(false)
    , _imageCount(0)
    , _currentIndex(0xFF)
    , _currentBlock(0)
{}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Lifecycle
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool StorageReader::begin() {
    if (_mounted) {
        log_w("%s: SD already mounted", TAG);
        return true;
    }

    // Initialise SPI bus on the default FSPI pins for ESP32-S3:
    //   MOSI=11, MISO=13, SCK=12
    SPI.begin(12, 13, 11, VSENSOR_SD_CS);

    if (!SD.begin(VSENSOR_SD_CS, SPI, VSENSOR_SPI_FREQ)) {
        log_e("%s: SD mount FAILED — check wiring / card format (FAT32)", TAG);
        SPI.end();
        return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        log_e("%s: No SD card detected", TAG);
        SD.end();
        SPI.end();
        return false;
    }

    log_i("%s: SD mounted — Type: %s, Size: %llu MB",
          TAG,
          (cardType == CARD_MMC)  ? "MMC"  :
          (cardType == CARD_SD)   ? "SDSC" :
          (cardType == CARD_SDHC) ? "SDHC" : "UNKNOWN",
          SD.cardSize() / (1024ULL * 1024ULL));

    _mounted = true;

    if (!_scanDirectory()) {
        log_e("%s: Image scan failed or no images found", TAG);
        // Keep mounted — caller can retry or end()
        return false;
    }

    log_i("%s: Found %u image(s) in %s", TAG, _imageCount, VSENSOR_IMAGE_DIR);
    return true;
}

void StorageReader::end() {
    closeImage();

    if (_mounted) {
        SD.end();
        SPI.end();
        _mounted    = false;
        _imageCount = 0;
        log_i("%s: SD unmounted, SPI released — ready for deep sleep", TAG);
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Image Catalogue
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

uint8_t StorageReader::imageCount() const {
    return _imageCount;
}

bool StorageReader::getImageInfo(uint8_t index, ImageInfo& info) const {
    if (index >= _imageCount) return false;
    info = _catalogue[index];
    return true;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Streaming Read (Sequential)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool StorageReader::openImage(uint8_t index) {
    if (!_mounted || index >= _imageCount) return false;

    // Close any previously open file
    closeImage();

    _currentFile = SD.open(_catalogue[index].filename, FILE_READ);
    if (!_currentFile) {
        log_e("%s: Failed to open %s", TAG, _catalogue[index].filename);
        return false;
    }

    _currentIndex = index;
    _currentBlock = 0;

    log_i("%s: Opened %s (%lu bytes, %lu blocks)",
          TAG,
          _catalogue[index].filename,
          _catalogue[index].fileSize,
          _catalogue[index].totalBlocks);

    return true;
}

bool StorageReader::readNextBlock(BlockReadResult& result) {
    if (!_currentFile) return false;

    uint32_t totalBlocks = _catalogue[_currentIndex].totalBlocks;
    if (_currentBlock >= totalBlocks) return false;

    uint32_t byteOffset = _currentBlock * VSENSOR_BLOCK_SIZE;
    if (!_readAtOffset(byteOffset, _currentBlock, totalBlocks, result)) {
        return false;
    }

    _currentBlock++;
    return true;
}

void StorageReader::closeImage() {
    if (_currentFile) {
        _currentFile.close();
        _currentIndex = 0xFF;
        _currentBlock = 0;
    }
}

bool StorageReader::isImageOpen() const {
    return (_currentFile);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Random-Access Block Read (for CoAP retransmissions)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool StorageReader::readBlock(uint32_t blockIndex, BlockReadResult& result) {
    if (!_currentFile) return false;

    uint32_t totalBlocks = _catalogue[_currentIndex].totalBlocks;
    if (blockIndex >= totalBlocks) return false;

    uint32_t byteOffset = blockIndex * VSENSOR_BLOCK_SIZE;

    // Seek to the requested position
    if (!_currentFile.seek(byteOffset)) {
        log_e("%s: Seek to offset %lu failed", TAG, byteOffset);
        return false;
    }

    return _readAtOffset(byteOffset, blockIndex, totalBlocks, result);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Checksum
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

uint16_t StorageReader::computeChecksum(uint8_t index) {
    if (!_mounted || index >= _imageCount) return 0;

    File f = SD.open(_catalogue[index].filename, FILE_READ);
    if (!f) return 0;

    // Fletcher-16 algorithm — fast, single-pass, good for error detection
    uint16_t sum1 = 0, sum2 = 0;
    uint8_t  buf[VSENSOR_BLOCK_SIZE];

    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        for (size_t i = 0; i < n; i++) {
            sum1 = (sum1 + buf[i]) % 255;
            sum2 = (sum2 + sum1)   % 255;
        }
    }
    f.close();

    return (sum2 << 8) | sum1;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Private Helpers
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

bool StorageReader::_scanDirectory() {
    _imageCount = 0;

    File dir = SD.open(VSENSOR_IMAGE_DIR);
    if (!dir || !dir.isDirectory()) {
        log_e("%s: Cannot open directory %s", TAG, VSENSOR_IMAGE_DIR);
        return false;
    }

    while (_imageCount < VSENSOR_MAX_IMAGES) {
        File entry = dir.openNextFile();
        if (!entry) break;   // No more files

        // Skip directories and hidden files
        const char* name = entry.name();
        if (entry.isDirectory() || name[0] == '.') {
            entry.close();
            continue;
        }

        // Accept only .jpg / .jpeg (case-insensitive)
        String nameStr = String(name);
        nameStr.toLowerCase();
        if (!nameStr.endsWith(".jpg") && !nameStr.endsWith(".jpeg")) {
            entry.close();
            continue;
        }

        // Populate catalogue entry
        ImageInfo& img = _catalogue[_imageCount];
        snprintf(img.filename, sizeof(img.filename), "%s/%s",
                 VSENSOR_IMAGE_DIR, name);
        img.fileSize    = entry.size();
        img.totalBlocks = (img.fileSize + VSENSOR_BLOCK_SIZE - 1) / VSENSOR_BLOCK_SIZE;
        img.checksum    = 0;  // Computed lazily via computeChecksum()

        log_d("%s:  [%u] %s — %lu B, %lu blocks",
              TAG, _imageCount, img.filename, img.fileSize, img.totalBlocks);

        entry.close();
        _imageCount++;
    }

    dir.close();
    return (_imageCount > 0);
}

bool StorageReader::_readAtOffset(uint32_t byteOffset, uint32_t blockIdx,
                                   uint32_t totalBlocks, BlockReadResult& result)
{
    // Ensure correct file position (for sequential reads, already there)
    if (_currentFile.position() != byteOffset) {
        if (!_currentFile.seek(byteOffset)) {
            log_e("%s: Seek to %lu failed", TAG, byteOffset);
            return false;
        }
    }

    size_t bytesRead = _currentFile.read(result.data, VSENSOR_BLOCK_SIZE);
    if (bytesRead == 0) return false;

    result.length     = bytesRead;
    result.blockIndex = blockIdx;
    result.isLast     = (blockIdx == totalBlocks - 1);

    return true;
}
