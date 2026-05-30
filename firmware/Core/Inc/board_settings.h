/**
 * @file    board_settings.h
 * @brief   Flash-persistent board settings — page 126 of both banks.
 */
#ifndef __BOARD_SETTINGS_H
#define __BOARD_SETTINGS_H

#include <stdint.h>

typedef enum {
    BSET_OK = 0,
    BSET_EMPTY,
    BSET_CORRUPT,
    BSET_FLASH_ERROR,
} BSetStatus_t;

BSetStatus_t BoardSettings_LoadLPMode(uint8_t *lp_mode_out);
BSetStatus_t BoardSettings_SaveLPMode(uint8_t lp_mode);

#endif /* __BOARD_SETTINGS_H */
