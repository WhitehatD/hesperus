/**
  ******************************************************************************
  * @file    mx_wifi_conf.h
  * @brief   MX Wi-Fi (EMW3080) Configuration for B-U585I-IOT02A
  *
  * Customized for the thesis IoT firmware:
  *   - SPI transport (not UART)
  *   - Bare-metal mode (no RTOS)
  *   - AT command mode (TCP/IP on the EMW3080 co-processor)
  ******************************************************************************
  */

#ifndef MX_WIFI_CONF_H
#define MX_WIFI_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <inttypes.h>
#include "main.h"

/* ─── Transport: SPI (not UART) ──────────────────────── */
#define MX_WIFI_USE_SPI                             (1)

/* 2026-08-23: back to 0 (polled SPI). DMA was enabled in 6ccb0ea, a commit
 * that self-flagged "NOT YET VALIDATED ON HARDWARE", and this project ran
 * polled SPI reliably for its entire life before it. Measured tonight with
 * the STOCK driver + DMA=1: a 2-byte MQTT PINGREQ/PINGRESP costs a
 * metronomic ~1s (it was ~150ms on the patched build) and a 2KB HTTP chunk
 * costs ~20s — a fixed per-transfer tax, not radio latency. In bare metal
 * (MX_WIFI_USE_CMSIS_OS=0) the DMA path blocks on SpiTransferDoneSem with no
 * RTOS to schedule the wake, which is exactly what the removed
 * "ENTERPRISE FIX" hspi->State forcing was papering over. Polled SPI has
 * none of that machinery: 2KB at 40MHz SCK is microseconds of real work.
 * Channels for the DMA path (unused while this is 0) are still set up in
 * mx_wifi_hw.c (_spi2_dma_init). */
#define DMA_ON_USE                                  (0)

/* ─── No RTOS — bare-metal ────────────────────────────── */
#define MX_WIFI_USE_CMSIS_OS                        (0)

/* ─── AT command mode (TCP/IP on module) ──────────────── */
#define MX_WIFI_NETWORK_BYPASS_MODE                 (0)

/* ─── Do not copy TX buffer ───────────────────────────── */
#define MX_WIFI_TX_BUFFER_NO_COPY                   (1)

/* ─── OS abstraction (bare-metal) ─────────────────────── */
#include "mx_wifi_bare_os.h"

/* ─── Module identity ─────────────────────────────────── */
#define MX_WIFI_PRODUCT_NAME                        ("MXCHIP-WIFI")
#define MX_WIFI_PRODUCT_ID                          ("EMW3080B")

/* ─── Communication parameters ────────────────────────── */
#define MX_WIFI_UART_BAUDRATE                       (230400)
#define MX_WIFI_MTU_SIZE                            (1500)
#define MX_WIFI_BYPASS_HEADER_SIZE                  (28)
#define MX_WIFI_PBUF_LINK_HLEN                      (14)
#define MX_WIFI_BUFFER_SIZE                         (2500)
#define MX_WIFI_IPC_PAYLOAD_SIZE                    (MX_WIFI_BUFFER_SIZE - 6)
#define MX_WIFI_SOCKET_DATA_SIZE                    (MX_WIFI_IPC_PAYLOAD_SIZE - 12)

/* ─── Timeouts ────────────────────────────────────────── */
/* HARDENED: 10s timeout — default was 30s which caused silent 30s hangs per recv call.
 * 10s is sufficient for any single MIPC SPI round-trip while avoiding silent stalls. */
#define MX_WIFI_CMD_TIMEOUT                         (10000)

/* ─── Limits ──────────────────────────────────────────── */
#define MX_WIFI_MAX_SOCKET_NBR                      (8)
#define MX_WIFI_MAX_DETECTED_AP                     (10)
#define MX_WIFI_MAX_SSID_NAME_SIZE                  (32)
#define MX_WIFI_MAX_PSWD_NAME_SIZE                  (64)
#define MX_WIFI_PRODUCT_NAME_SIZE                   (32)
#define MX_WIFI_PRODUCT_ID_SIZE                     (32)
#define MX_WIFI_FW_REV_SIZE                         (24)

/* ─── Buffers ─────────────────────────────────────────── */
#define MX_WIFI_MAX_RX_BUFFER_COUNT                 (2)
#define MX_WIFI_MAX_TX_BUFFER_COUNT                 (4)
#define MX_WIFI_MIN_TX_HEADER_SIZE                  (28)
#define MX_CIRCULAR_UART_RX_BUFFER_SIZE             (400)

/* ─── Stats (disabled in production) ──────────────────── */
#define MX_STAT_ON                                  0
#define MX_STAT_INIT()
#define MX_STAT(A)
#define MX_STAT_LOG()
#define MX_STAT_DECLARE()

/* ─── Hardware pin definitions for mx_wifi_spi.c ──────── */
/* These override the MXCHIP_* defaults in the ST reference code */

/* Reset / Chip Enable (PF15, active-low) */
#define MX_WIFI_RESET_PIN              GPIO_PIN_15
#define MX_WIFI_RESET_PORT             GPIOF

/* SPI chip-select (NSS) — PB12 per UM2839 (WRLS.SPI2_NSS) */
#define MX_WIFI_SPI_CS_PIN             GPIO_PIN_12
#define MX_WIFI_SPI_CS_PORT            GPIOB

/* Data-ready interrupt from EMW3080 (NOTIFY) — PD14 / EXTI14 per UM2839 */
#define MX_WIFI_SPI_IRQ_PIN            GPIO_PIN_14
#define MX_WIFI_SPI_IRQ_PORT           GPIOD
#define MX_WIFI_SPI_IRQ                EXTI14_IRQn

/* Flow control — PG15 / EXTI15 per UM2839 (WRLS.FLOW) */
#define MX_WIFI_SPI_FLOW_PIN           GPIO_PIN_15
#define MX_WIFI_SPI_FLOW_PORT          GPIOG




/* SPI thread (no-ops in bare-metal, needed for RTOS) */
#define MX_WIFI_SPI_THREAD_STACK_SIZE  (1024)
#define MX_WIFI_SPI_THREAD_PRIORITY    OSPRIORITYNORMAL

/* Receive thread (no-ops in bare-metal, needed for RTOS) */
#ifndef MX_WIFI_RECEIVED_THREAD_STACK_SIZE
#define MX_WIFI_RECEIVED_THREAD_STACK_SIZE  (1024)
#endif
#ifndef MX_WIFI_RECEIVED_THREAD_PRIORITY
#define MX_WIFI_RECEIVED_THREAD_PRIORITY    OSPRIORITYNORMAL
#endif

#ifdef __cplusplus
}
#endif

#endif /* MX_WIFI_CONF_H */
