/**
  ******************************************************************************
  * @file    stm32u5xx_it.c
  * @author  MCD Application Team
  * @brief   Main Interrupt Service Routines.
  *          This file provides template for all exceptions handler and
  *          peripherals interrupt service routine.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "stm32u5xx_hal.h"
#include "stm32u5xx_it.h"
#include "main.h"
#include "b_u585i_iot02a_camera.h"
#include "mx_wifi_io.h"

#if (defined(DMA_ON_USE) && (DMA_ON_USE == 1))
/* Owned by mx_wifi_hw.c */
DMA_HandleTypeDef *MX_WIFI_GetSpiDmaTx(void);
DMA_HandleTypeDef *MX_WIFI_GetSpiDmaRx(void);
#endif

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private functions ---------------------------------------------------------*/

/******************************************************************************/
/*            Cortex-M33 Processor Exceptions Handlers                         */
/******************************************************************************/

/**
  * @brief  This function handles NMI exception.
  */
void NMI_Handler(void)
{
}

/**
  * @brief  This function handles Hard Fault exception.
  */
void HardFault_Handler(void)
{
  while (1) {}
}

/**
  * @brief  This function handles Memory Manage exception.
  */
void MemManage_Handler(void)
{
  while (1) {}
}

/**
  * @brief  This function handles Bus Fault exception.
  */
void BusFault_Handler(void)
{
  while (1) {}
}

/**
  * @brief  This function handles Usage Fault exception.
  */
void UsageFault_Handler(void)
{
  while (1) {}
}

/**
  * @brief  This function handles SVCall exception.
  */
void SVC_Handler(void)
{
}

/**
  * @brief  This function handles Debug Monitor exception.
  */
void DebugMon_Handler(void)
{
  while (1) {}
}

/**
  * @brief  This function handles PendSVC exception.
  */
void PendSV_Handler(void)
{
  while (1) {}
}

/**
  * @brief  This function handles SysTick Handler.
  */
void SysTick_Handler(void)
{
  HAL_IncTick();
}

/******************************************************************************/
/*                 STM32U5xx Peripherals Interrupt Handlers                   */
/******************************************************************************/

/**
  * @brief  RTC global interrupt handler.
  *         Handles Alarm A events for scheduled wake-up from STOP2 mode.
  */
void RTC_IRQHandler(void)
{
  HAL_RTC_AlarmIRQHandler(&hrtc);
}

/**
  * @brief  DCMI/PSSI global interrupt handler.
  *         Handles camera frame-complete events.
  */
void DCMI_PSSI_IRQHandler(void)
{
  BSP_CAMERA_IRQ_HANDLER(0);
}

/**
  * @brief  GPDMA1 Channel 12 interrupt handler.
  *         Handles DMA transfer-complete for camera DCMI data.
  */
void GPDMA1_Channel12_IRQHandler(void)
{
  BSP_CAMERA_DMA_IRQ_HANDLER(0);
}

#if (defined(DMA_ON_USE) && (DMA_ON_USE == 1))
/**
  * @brief  GPDMA1 ch0/ch1 — SPI2 TX/RX for the EMW3080 Wi-Fi module.
  */
void GPDMA1_Channel0_IRQHandler(void)
{
  HAL_DMA_IRQHandler(MX_WIFI_GetSpiDmaTx());
}

void GPDMA1_Channel1_IRQHandler(void)
{
  HAL_DMA_IRQHandler(MX_WIFI_GetSpiDmaRx());
}
#endif /* DMA_ON_USE == 1 */

/**
  * @brief  EXTI13 interrupt handler.
  *         Handles the B3 USER push-button press (PC13).
  */
void EXTI13_IRQHandler(void)
{
  BSP_PB_IRQHandler(BUTTON_USER);
}

/**
  * @brief  EXTI14 interrupt handler.
  *         WiFi NOTIFY pin (PD14) — data-ready from EMW3080.
  */
void EXTI14_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_14);
  mxchip_WIFI_ISR(GPIO_PIN_14);
}

/**
  * @brief  EXTI15 interrupt handler.
  *         WiFi FLOW pin (PG15) — flow control from EMW3080.
  */
void EXTI15_IRQHandler(void)
{
  HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_15);
  mxchip_WIFI_ISR(GPIO_PIN_15);
}

/**
  * @brief  SPI2 interrupt handler.
  *         WiFi SPI transfer complete.
  */
void SPI2_IRQHandler(void)
{
  extern SPI_HandleTypeDef MXCHIP_SPI;
  HAL_SPI_IRQHandler(&MXCHIP_SPI);
}
