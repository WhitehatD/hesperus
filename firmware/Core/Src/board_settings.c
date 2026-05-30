/**
 * @file    board_settings.c
 * @brief   Flash-persistent board settings (lp_mode) — page 126, both banks.
 *
 * Mirrors wifi_credentials.c flash pattern. Dedicated page 126 avoids
 * entangling with WiFi credentials (page 127).
 *
 * STM32U585 flash geometry:
 *   Bank 1: 0x08000000 -- 0x080FFFFF  Page size: 8KB (0x2000)
 *   Bank 2: 0x08100000 -- 0x081FFFFF
 *   Page 126 Bank 1: 0x080FC000
 *   Page 126 Bank 2: 0x081FC000
 *
 * On-flash layout (16 bytes, 1 quadword):
 *   Bytes  0- 3: Magic  = 0x42534554 ("BSET")
 *   Byte   4:    lp_mode (0=Active, 1=PS-REST)
 *   Bytes  5-11: Reserved / 0x00
 *   Bytes 12-15: CRC32 over bytes 0-11
 */

#include "board_settings.h"
#include "firmware_config.h"
#include "debug_log.h"
#include "main.h"

#include <string.h>

#define BSET_PAGE_SIZE    0x2000u
#define BSET_PAGE_NUMBER  126
#define BSET_ADDR_BANK1   0x080FC000u
#define BSET_ADDR_BANK2   0x081FC000u
#define BSET_MAGIC        0x42534554u   /* "BSET" */

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  lp_mode;
    uint8_t  _pad[7];
    uint32_t crc32;
} FlashBSetBlock_t;  /* exactly 16 bytes */
#pragma pack(pop)

_Static_assert(sizeof(FlashBSetBlock_t) == 16,
               "board settings block must be exactly 1 quadword (16 bytes)");

static const char TAG_BSET[] = "BSET";

#define BSET_CRC_SIZE  12u   /* bytes 0-11, before crc32 field */

static uint32_t _bset_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
    }
    return ~crc;
}

BSetStatus_t BoardSettings_LoadLPMode(uint8_t *lp_mode_out)
{
    if (lp_mode_out == NULL)
        return BSET_CORRUPT;

    uint32_t addrs[2] = { BSET_ADDR_BANK1, BSET_ADDR_BANK2 };
    for (int i = 0; i < 2; i++) {
        const FlashBSetBlock_t *b = (const FlashBSetBlock_t *)addrs[i];
        if (b->magic != BSET_MAGIC)
            continue;
        if (_bset_crc32((const uint8_t *)b, BSET_CRC_SIZE) != b->crc32)
            continue;
        if (b->lp_mode > 1u)
            continue;
        *lp_mode_out = b->lp_mode;
        LOG_INFO(TAG_BSET, "Loaded lp_mode=%d from flash (bank %d)",
                 (int)b->lp_mode, i);
        return BSET_OK;
    }
    LOG_DEBUG(TAG_BSET, "No valid board settings in flash");
    return BSET_EMPTY;
}

BSetStatus_t BoardSettings_SaveLPMode(uint8_t lp_mode)
{
    FlashBSetBlock_t block;
    memset(&block, 0x00, sizeof(block));
    block.magic   = BSET_MAGIC;
    block.lp_mode = (lp_mode != 0u) ? 1u : 0u;
    block.crc32   = _bset_crc32((const uint8_t *)&block, BSET_CRC_SIZE);

    LOG_INFO(TAG_BSET, "Saving lp_mode=%d to flash page %d",
             (int)block.lp_mode, BSET_PAGE_NUMBER);

    if (HAL_FLASH_Unlock() != HAL_OK) {
        LOG_ERROR(TAG_BSET, "Flash unlock failed");
        return BSET_FLASH_ERROR;
    }

    FLASH_EraseInitTypeDef ec = {0};
    ec.TypeErase = FLASH_TYPEERASE_PAGES;
    ec.Page      = BSET_PAGE_NUMBER;
    ec.NbPages   = 1;
    uint32_t page_error = 0;

    ec.Banks = FLASH_BANK_1;
    if (HAL_FLASHEx_Erase(&ec, &page_error) != HAL_OK)
        LOG_ERROR(TAG_BSET, "Erase Bank 1 failed (err=0x%08lX)",
                  (unsigned long)HAL_FLASH_GetError());

    ec.Banks = FLASH_BANK_2;
    if (HAL_FLASHEx_Erase(&ec, &page_error) != HAL_OK)
        LOG_ERROR(TAG_BSET, "Erase Bank 2 failed (err=0x%08lX)",
                  (unsigned long)HAL_FLASH_GetError());

    uint8_t write_buf[16] __attribute__((aligned(16)));
    memcpy(write_buf, &block, 16);

    uint32_t targets[2] = { BSET_ADDR_BANK1, BSET_ADDR_BANK2 };
    int ok = 0;
    for (int b = 0; b < 2; b++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD,
                              targets[b],
                              (uint32_t)(uintptr_t)write_buf) == HAL_OK)
            ok++;
        else
            LOG_ERROR(TAG_BSET, "Program Bank %d failed (HAL err=0x%08lX)",
                      b + 1, (unsigned long)HAL_FLASH_GetError());
    }

    HAL_FLASH_Lock();

    if (ok == 0) {
        LOG_ERROR(TAG_BSET, "Flash write failed on all banks");
        return BSET_FLASH_ERROR;
    }
    LOG_INFO(TAG_BSET, "Board settings saved OK (%d/2 banks)", ok);
    return BSET_OK;
}
