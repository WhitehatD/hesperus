/**
  ******************************************************************************
  * @file    main.h
  * @brief   Header for main.c — Autonomous IoT Visual Monitoring
  * @author  Alexandru-Ionut Cioc (based on ST MCD template)
  ******************************************************************************
  */

#ifndef MAIN_H
#define MAIN_H

/* ── HAL & BSP ─────────────────────────────────────────── */
#include "stm32u5xx_hal.h"
#include "b_u585i_iot02a.h"

/* ── Firmware Modules ──────────────────────────────────── */
#include "firmware_config.h"
#include "debug_log.h"

/* ── Shared Peripheral Handles (extern) ────────────────── */
extern RTC_HandleTypeDef hrtc;

/* ── System Function Prototypes ────────────────────────── */
void SystemClock_Config(void);
void Watchdog_Refresh(void);

#endif /* MAIN_H */