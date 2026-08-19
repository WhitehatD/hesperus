/**
 * @file    ota_update.h
 * @brief   Over-The-Air Firmware Update — Dual-Bank Flash OTA
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Enterprise OTA update system for the B-U585I-IOT02A Discovery Kit.
 * Uses the STM32U585's dual-bank flash architecture for safe atomic
 * firmware updates with automatic rollback on boot failure.
 *
 * Architecture:
 *   1. Board polls GET /api/firmware/version to check for updates
 *   2. If new version detected → downloads .bin via HTTP GET in chunks
 *   3. Writes chunks to inactive flash bank (page-granular erase)
 *   4. Verifies integrity via CRC32
 *   5. Swaps active bank via Option Bytes → system reset
 *   6. On next boot: validates boot counter → clears on success
 *   7. If 3 consecutive boot failures → reverts to previous bank
 *
 * Safety guarantees:
 *   - Old firmware is NEVER erased — it stays on the other bank
 *   - Bank swap is atomic (single Option Byte write + reset)
 *   - Boot counter survives reset (stored in RTC backup register)
 */

#ifndef __OTA_UPDATE_H
#define __OTA_UPDATE_H

#include <stdint.h>
#include <stdbool.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Status Codes
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    OTA_OK = 0,
    OTA_NO_UPDATE,           /* Server version matches current */
    OTA_ERROR_NETWORK,       /* HTTP connection/download failed */
    OTA_ERROR_PARSE,         /* Version JSON parse error */
    OTA_ERROR_FLASH_ERASE,   /* Flash erase failed */
    OTA_ERROR_FLASH_WRITE,   /* Flash write failed */
    OTA_ERROR_VERIFY,        /* CRC32 mismatch after write */
    OTA_ERROR_TOO_LARGE,     /* Firmware exceeds max size */
    OTA_ERROR_SWAP,          /* Bank swap failed */
    OTA_ERROR_DOWNGRADE,     /* SEC-06: Server version is older than current */
} OTAStatus_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Version Info (from server)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    char     version[16];    /* Semantic version string, e.g. "0.3" */
    uint32_t size;           /* Firmware binary size in bytes */
    uint32_t crc32;          /* CRC32 of the .bin file */
} OTAVersionInfo_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  CRC32 — MPEG-2 variant (poly 0x04C11DB7, MSB-first, no input/output
 *         reflection, no final XOR). Matches the server's _compute_crc32()
 *         in server/app/api/firmware_routes.py. Used for OTA binary
 *         verification and by wifi.c's chunked image upload — one CRC
 *         implementation for the whole firmware, not two that could drift.
 * @param  crc   Running CRC state — pass 0xFFFFFFFF to start a new one.
 * @param  data  Buffer to checksum.
 * @param  len   Buffer length in bytes.
 * @retval Updated CRC32 value.
 */
uint32_t Firmware_CRC32(uint32_t crc, const uint8_t *data, uint32_t len);

/**
 * @brief  Validate boot integrity on startup.
 *         Increments boot counter in RTC backup register. If counter
 *         reaches OTA_MAX_BOOT_FAILURES, triggers automatic rollback
 *         to the previous flash bank.
 *
 *         MUST be called early in main() after RTC init.
 *         Call OTA_MarkBootSuccessful() after all subsystems initialize OK.
 */
void OTA_ValidateBoot(void);

/**
 * @brief  Mark the current boot as successful.
 *         Resets the boot failure counter to 0.
 *         Call this after Wi-Fi + MQTT + Camera all init OK.
 */
void OTA_MarkBootSuccessful(void);

/**
 * @brief  Check the server for a firmware update.
 *         HTTP GET /api/firmware/version → compare with FW_VERSION.
 *
 * @param  info: Output — filled with server version info if update available.
 * @retval OTA_OK if update is available (info is populated).
 *         OTA_NO_UPDATE if current firmware is up-to-date.
 *         OTA_ERROR_* on failure.
 */
OTAStatus_t OTA_CheckForUpdate(OTAVersionInfo_t *info);

/**
 * @brief  Download firmware binary to RAM, then erase + flash inactive bank.
 *         Strategy: download → RAM buffer → erase → write → CRC verify.
 *         This avoids flash erase starving the Wi-Fi SPI pipeline.
 *
 * @param  info:        Version info from OTA_CheckForUpdate (provides size/CRC).
 * @param  ram_buffer:  Pointer to a RAM buffer for firmware download staging.
 * @param  ram_size:    Size of the RAM buffer (must be >= info->size).
 * @retval OTA_OK on success (ready for bank swap).
 *         OTA_ERROR_* on failure.
 */
OTAStatus_t OTA_DownloadAndFlash(const OTAVersionInfo_t *info,
                                 uint8_t *ram_buffer, uint32_t ram_size);

/**
 * @brief  Swap active flash bank and trigger system reset.
 *         The system will reboot into the newly flashed firmware.
 *
 *         WARNING: This function does not return on success.
 *
 * @retval OTA_ERROR_SWAP if the bank swap fails (system continues running).
 */
OTAStatus_t OTA_SwapBankAndReset(void);

/**
 * @brief  Full OTA flow: check → download to RAM → erase+flash → verify → swap.
 *         Convenience function that chains all steps.
 *         Only executes if the server has a newer version.
 *
 * @param  ram_buffer:  Pointer to a RAM buffer for firmware download staging.
 * @param  ram_size:    Size of the RAM buffer.
 * @retval OTA_NO_UPDATE if already up-to-date.
 *         OTA_OK + system reset if update applied.
 *         OTA_ERROR_* on failure.
 */
OTAStatus_t OTA_CheckAndUpdate(uint8_t *ram_buffer, uint32_t ram_size);

#endif /* __OTA_UPDATE_H */
