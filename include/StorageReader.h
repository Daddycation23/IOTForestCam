/**
 * @file StorageReader.h
 * @brief Part 1 — Storage Reader (SD-Card JPEG Reader)
 *
 * Simulates a camera by reading pre-loaded JPEG files from an SD card
 * in configurable block sizes (default 512 B) suitable for CoAP
 * Block-Wise Transfer (RFC 7959).
 *
 * Design Goals:
 *   - Power Efficiency: Mount/unmount SD only when needed.
 *   - Stability: CRC-like integrity via simple checksum over chunks.
 *   - Deep-Sleep Friendly: All state is re-initialised on wake;
 *     no persistent RAM required.
 *
 * Hardware Wiring (LILYGO T3-S3 V1.2 ↔ MicroSD via SPI):
 *   MOSI → GPIO 11
 *   MISO → GPIO 2
 *   CLK  → GPIO 14
 *   CS   → GPIO 13
 *
 * SD Card Layout:
 *   /images/
 *       img_001.jpg
 *       img_002.jpg
 *       ...
 *
 * @author  CS Group 19
 * @date    2026
 */

#ifndef STORAGE_READER_H
#define STORAGE_READER_H

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

// ──────────────────────── Pin Defaults (LILYGO T3-S3 V1.2) ────
#ifndef VSENSOR_SD_CLK
#define VSENSOR_SD_CLK  14      // SPI Clock (SCK)
#endif

#ifndef VSENSOR_SD_MISO
#define VSENSOR_SD_MISO 2       // SPI MISO
#endif

#ifndef VSENSOR_SD_MOSI
#define VSENSOR_SD_MOSI 11      // SPI MOSI
#endif

#ifndef VSENSOR_SD_CS
#define VSENSOR_SD_CS   13      // Chip-Select for MicroSD module
#endif

#ifndef VSENSOR_SPI_FREQ
#define VSENSOR_SPI_FREQ 4000000  // 4 MHz — safe for long jumper wires
#endif

// ──────────────────────── Tunables ────────────────────────────
/** Block size in bytes — aligned with CoAP Block2 SZX=5 (512 B). */
static constexpr size_t VSENSOR_BLOCK_SIZE = 512;

/** Maximum images expected on the SD card. */
static constexpr uint8_t VSENSOR_MAX_IMAGES = 32;

/** Root directory on the SD card that holds the JPEG files. */
static constexpr const char* VSENSOR_IMAGE_DIR = "/images";

// ──────────────────────── Data Types ──────────────────────────

/**
 * @brief Metadata for a single simulated capture.
 */
struct ImageInfo {
    char     filename[64];      ///< Full path, e.g. "/images/img_001.jpg"
    uint32_t fileSize;          ///< Total bytes
    uint32_t totalBlocks;       ///< ceil(fileSize / BLOCK_SIZE)
    uint32_t checksum;          ///< Simple Fletcher-16 over entire file
};

/**
 * @brief Result of a single block read.
 */
struct BlockReadResult {
    uint8_t  data[VSENSOR_BLOCK_SIZE];  ///< Payload bytes
    size_t   length;                     ///< Actual bytes in this block (≤ BLOCK_SIZE)
    uint32_t blockIndex;                 ///< 0-based block number
    bool     isLast;                     ///< true if this is the final block
};

// ──────────────────────── Class ───────────────────────────────

class StorageReader {
public:
    StorageReader();

    // ── Lifecycle ───────────────────────────────────────────
    /**
     * Mount the SD card and scan /images for JPEG files.
     * Call once after wake-up, before any read operations.
     * @return true if mount + scan succeeded.
     */
    bool begin();

    /**
     * Unmount SD card and release SPI bus.
     * Call before entering deep sleep for minimum quiescent current.
     */
    void end();

    // ── Image Catalogue ─────────────────────────────────────
    /** Number of JPEG files discovered on /images. */
    uint8_t imageCount() const;

    /**
     * Get metadata for the image at the given catalogue index.
     * @param index 0-based catalogue index (0 … imageCount()-1)
     * @param[out] info populated on success.
     * @return true on success.
     */
    bool getImageInfo(uint8_t index, ImageInfo& info) const;

    // ── Streaming Read ──────────────────────────────────────
    /**
     * Open a file for block-wise streaming.
     * @param index catalogue index of the image.
     * @return true if file opened successfully.
     */
    bool openImage(uint8_t index);

    /**
     * Read the next sequential block.
     * @param[out] result filled with data + metadata.
     * @return true if a block was read (even a partial final block).
     *         false if no more data or file not open.
     */
    bool readNextBlock(BlockReadResult& result);

    /**
     * Read a specific block by index (random access).
     * Useful for CoAP retransmission of a lost block.
     * @param blockIndex 0-based block number.
     * @param[out] result filled with data + metadata.
     * @return true on success.
     */
    bool readBlock(uint32_t blockIndex, BlockReadResult& result);

    /** Close the currently open file. Idempotent. */
    void closeImage();

    /** @return true if a file is currently open for reading. */
    bool isImageOpen() const;

    // ── Utility ─────────────────────────────────────────────
    /**
     * Compute a Fletcher-16 checksum over the full file.
     * Used by the gateway to verify transfer integrity.
     * @param index catalogue index.
     * @return 16-bit checksum, or 0 on error.
     */
    uint16_t computeChecksum(uint8_t index);

private:
    bool        _mounted;
    uint8_t     _imageCount;
    ImageInfo   _catalogue[VSENSOR_MAX_IMAGES];

    File        _currentFile;
    uint8_t     _currentIndex;
    uint32_t    _currentBlock;

    /** Scan /images and populate _catalogue[]. */
    bool        _scanDirectory();

    /** Internal: fill a BlockReadResult from an open file at a byte offset. */
    bool        _readAtOffset(uint32_t byteOffset, uint32_t blockIdx,
                              uint32_t totalBlocks, BlockReadResult& result);
};

#endif // STORAGE_READER_H
