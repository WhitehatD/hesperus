/**
 * @file    mx_wifi_hw.c
 * @brief   SPI2 hardware abstraction for the EMW3080 Wi-Fi co-processor
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Provides:
 *   - MXCHIP_SPI handle (extern'd by the ST reference mx_wifi_spi.c)
 *   - HAL_SPI_MspInit / MspDeInit for SPI2 GPIO + NVIC configuration
 *   - MX_SPI2_Init() — peripheral-level SPI2 setup
 *   - HAL SPI transfer callbacks bridged to the reference driver
 *
 * Pin mapping (B-U585I-IOT02A RevC — UM2839 Table 14):
 *   SPI2_SCK   → PD1  (AF5)
 *   SPI2_MISO  → PD3  (AF5)
 *   SPI2_MOSI  → PD4  (AF5)
 *   NSS        → PB12 (software-managed, WRLS.SPI2_NSS)
 *   RESET      → PF15 (active-low, WRLS.WKUP_W)
 *   NOTIFY     → PD14 (EXTI14, data-ready from EMW3080)
 *   FLOW       → PG15 (EXTI15, flow-control from EMW3080)
 */

#include "main.h"
#include "mx_wifi_conf.h"

/* ── Global SPI handle (extern'd by mx_wifi_spi.c) ───────────────────────── */
SPI_HandleTypeDef MXCHIP_SPI;

#if (defined(DMA_ON_USE) && (DMA_ON_USE == 1))
/* SPI2 DMA. GPDMA1 ch0/ch1 — ch12 belongs to the camera (DCMI, linked-list
 * mode). These are plain direct-mode transfers, so HAL_DMA_Init, not
 * HAL_DMAEx_List_Init. Completion: HAL_SPI_TxRxCpltCallback below signals
 * the driver's SpiTransferDoneSem, which mx_wifi_spi.c's DMA path waits on. */
static DMA_HandleTypeDef hdma_spi2_tx;
static DMA_HandleTypeDef hdma_spi2_rx;

static void _spi2_dma_init(SPI_HandleTypeDef *hspi)
{
    __HAL_RCC_GPDMA1_CLK_ENABLE();

    /* ── TX: memory -> SPI2 peripheral ───────────────────────────────────── */
    hdma_spi2_tx.Instance                     = GPDMA1_Channel0;
    hdma_spi2_tx.Init.Request                 = GPDMA1_REQUEST_SPI2_TX;
    hdma_spi2_tx.Init.BlkHWRequest            = DMA_BREQ_SINGLE_BURST;
    hdma_spi2_tx.Init.Direction               = DMA_MEMORY_TO_PERIPH;
    hdma_spi2_tx.Init.SrcInc                  = DMA_SINC_INCREMENTED;
    hdma_spi2_tx.Init.DestInc                 = DMA_DINC_FIXED;
    hdma_spi2_tx.Init.SrcDataWidth            = DMA_SRC_DATAWIDTH_BYTE;
    hdma_spi2_tx.Init.DestDataWidth           = DMA_DEST_DATAWIDTH_BYTE;
    hdma_spi2_tx.Init.Priority                = DMA_LOW_PRIORITY_LOW_WEIGHT;
    hdma_spi2_tx.Init.SrcBurstLength          = 1;
    hdma_spi2_tx.Init.DestBurstLength         = 1;
    hdma_spi2_tx.Init.TransferAllocatedPort   = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT0;
    hdma_spi2_tx.Init.TransferEventMode       = DMA_TCEM_BLOCK_TRANSFER;
    hdma_spi2_tx.Init.Mode                    = DMA_NORMAL;

    if (HAL_DMA_Init(&hdma_spi2_tx) != HAL_OK)
    {
        while (1) {}
    }
    __HAL_LINKDMA(hspi, hdmatx, hdma_spi2_tx);
    (void)HAL_DMA_ConfigChannelAttributes(&hdma_spi2_tx, DMA_CHANNEL_NPRIV);

    /* ── RX: SPI2 peripheral -> memory ───────────────────────────────────── */
    hdma_spi2_rx.Instance                     = GPDMA1_Channel1;
    hdma_spi2_rx.Init.Request                 = GPDMA1_REQUEST_SPI2_RX;
    hdma_spi2_rx.Init.BlkHWRequest            = DMA_BREQ_SINGLE_BURST;
    hdma_spi2_rx.Init.Direction               = DMA_PERIPH_TO_MEMORY;
    hdma_spi2_rx.Init.SrcInc                  = DMA_SINC_FIXED;
    hdma_spi2_rx.Init.DestInc                 = DMA_DINC_INCREMENTED;
    hdma_spi2_rx.Init.SrcDataWidth            = DMA_SRC_DATAWIDTH_BYTE;
    hdma_spi2_rx.Init.DestDataWidth           = DMA_DEST_DATAWIDTH_BYTE;
    hdma_spi2_rx.Init.Priority                = DMA_LOW_PRIORITY_LOW_WEIGHT;
    hdma_spi2_rx.Init.SrcBurstLength          = 1;
    hdma_spi2_rx.Init.DestBurstLength         = 1;
    hdma_spi2_rx.Init.TransferAllocatedPort   = DMA_SRC_ALLOCATED_PORT0 | DMA_DEST_ALLOCATED_PORT0;
    hdma_spi2_rx.Init.TransferEventMode       = DMA_TCEM_BLOCK_TRANSFER;
    hdma_spi2_rx.Init.Mode                    = DMA_NORMAL;

    if (HAL_DMA_Init(&hdma_spi2_rx) != HAL_OK)
    {
        while (1) {}
    }
    __HAL_LINKDMA(hspi, hdmarx, hdma_spi2_rx);
    (void)HAL_DMA_ConfigChannelAttributes(&hdma_spi2_rx, DMA_CHANNEL_NPRIV);

    /* Priority 5 = same as the NOTIFY/FLOW/SPI2 lines below, so DMA
     * completion neither preempts nor is starved by flow control. */
    HAL_NVIC_SetPriority(GPDMA1_Channel0_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
    HAL_NVIC_SetPriority(GPDMA1_Channel1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
}

/* Referenced by stm32u5xx_it.c's GPDMA1_Channel0/1 IRQ handlers. */
DMA_HandleTypeDef *MX_WIFI_GetSpiDmaTx(void) { return &hdma_spi2_tx; }
DMA_HandleTypeDef *MX_WIFI_GetSpiDmaRx(void) { return &hdma_spi2_rx; }
#endif /* DMA_ON_USE == 1 */

/* ═══════════════════════════════════════════════════════════════════════════
 *  HAL_SPI_MspInit  —  GPIO, clock, and NVIC setup for SPI2
 *
 *  Called automatically by HAL_SPI_Init() when Instance == SPI2.
 * ═══════════════════════════════════════════════════════════════════════════ */
void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance != SPI2)
    {
        return;    /* Only handle SPI2 (WiFi module) */
    }

    /* ── Clocks ───────────────────────────────────────────────────────────── */
    __HAL_RCC_SPI2_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOF_CLK_ENABLE();

    /* GPIOG needs VddIO2 enabled first — it's on a separate power domain */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWREx_EnableVddIO2();
    __HAL_RCC_GPIOG_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};

    /* ── SPI2 data lines: SCK=PD1, MISO=PD3, MOSI=PD4 (AF5) ───────────── */
    gpio.Pin       = GPIO_PIN_1 | GPIO_PIN_3 | GPIO_PIN_4;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF5_SPI2;
    HAL_GPIO_Init(GPIOD, &gpio);

#if (defined(DMA_ON_USE) && (DMA_ON_USE == 1))
    _spi2_dma_init(hspi);
#endif

    /* ── NSS (PB12) — software chip-select, active-low ───────────────── */

    gpio.Pin   = MX_WIFI_SPI_CS_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = 0;
    HAL_GPIO_Init(MX_WIFI_SPI_CS_PORT, &gpio);
    HAL_GPIO_WritePin(MX_WIFI_SPI_CS_PORT, MX_WIFI_SPI_CS_PIN, GPIO_PIN_SET);

    /* ── RESET (PF15) — active-low module reset ───────────────────────────── */
    gpio.Pin   = MX_WIFI_RESET_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(MX_WIFI_RESET_PORT, &gpio);
    HAL_GPIO_WritePin(MX_WIFI_RESET_PORT, MX_WIFI_RESET_PIN, GPIO_PIN_SET);

    /* ── NOTIFY (PD14) — data-ready interrupt from EMW3080 (rising edge) ─── */
    gpio.Pin  = MX_WIFI_SPI_IRQ_PIN;
    gpio.Mode = GPIO_MODE_IT_RISING;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(MX_WIFI_SPI_IRQ_PORT, &gpio);
    HAL_NVIC_SetPriority(MX_WIFI_SPI_IRQ, 5, 0);   /* EXTI14 */
    HAL_NVIC_EnableIRQ(MX_WIFI_SPI_IRQ);

    /* ── FLOW (PG15) — flow-control interrupt from EMW3080 (rising edge) ─── */
    gpio.Pin  = MX_WIFI_SPI_FLOW_PIN;
    gpio.Mode = GPIO_MODE_IT_RISING;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(MX_WIFI_SPI_FLOW_PORT, &gpio);
    HAL_NVIC_SetPriority(EXTI15_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(EXTI15_IRQn);

    /* ── SPI2 interrupt (for HAL callbacks) ────────────────────────────────── */
    HAL_NVIC_SetPriority(SPI2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(SPI2_IRQn);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HAL_SPI_MspDeInit  —  Symmetric teardown
 * ═══════════════════════════════════════════════════════════════════════════ */
void HAL_SPI_MspDeInit(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance != SPI2)
    {
        return;
    }

    __HAL_RCC_SPI2_CLK_DISABLE();

    /* De-init SPI data lines */
    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_1 | GPIO_PIN_3 | GPIO_PIN_4);

    /* De-init control pins */
    HAL_GPIO_DeInit(MX_WIFI_SPI_CS_PORT,   MX_WIFI_SPI_CS_PIN);
    HAL_GPIO_DeInit(MX_WIFI_RESET_PORT,    MX_WIFI_RESET_PIN);
    HAL_GPIO_DeInit(MX_WIFI_SPI_IRQ_PORT,  MX_WIFI_SPI_IRQ_PIN);
    HAL_GPIO_DeInit(MX_WIFI_SPI_FLOW_PORT, MX_WIFI_SPI_FLOW_PIN);

    /* Disable interrupts */
    HAL_NVIC_DisableIRQ(MX_WIFI_SPI_IRQ);   /* EXTI14 — NOTIFY */
    HAL_NVIC_DisableIRQ(EXTI15_IRQn);        /* EXTI15 — FLOW   */
    HAL_NVIC_DisableIRQ(SPI2_IRQn);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MX_WIFI_NVIC_Enable  —  Enable EXTI interrupts for NOTIFY + FLOW
 *
 *  MUST be called AFTER mxwifi_probe() which initializes the semaphores
 *  that the ISR signals (SpiTxRxSem, SpiFlowRiseSem).
 * ═══════════════════════════════════════════════════════════════════════════ */
void MX_WIFI_NVIC_Enable(void)
{
    HAL_NVIC_EnableIRQ(MX_WIFI_SPI_IRQ);    /* EXTI14 — NOTIFY */
    HAL_NVIC_EnableIRQ(EXTI15_IRQn);         /* EXTI15 — FLOW   */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MX_SPI2_Init  —  SPI2 peripheral configuration
 *
 *  Must be called before mxwifi_probe().  Triggers HAL_SPI_MspInit above.
 * ═══════════════════════════════════════════════════════════════════════════ */
void MX_SPI2_Init(void)
{
    MXCHIP_SPI.Instance               = SPI2;
    MXCHIP_SPI.Init.Mode              = SPI_MODE_MASTER;
    MXCHIP_SPI.Init.Direction         = SPI_DIRECTION_2LINES;
    MXCHIP_SPI.Init.DataSize          = SPI_DATASIZE_8BIT;
    MXCHIP_SPI.Init.CLKPolarity       = SPI_POLARITY_LOW;
    MXCHIP_SPI.Init.CLKPhase          = SPI_PHASE_1EDGE;
    MXCHIP_SPI.Init.NSS               = SPI_NSS_SOFT;
    MXCHIP_SPI.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_4;
    MXCHIP_SPI.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    MXCHIP_SPI.Init.TIMode            = SPI_TIMODE_DISABLE;
    MXCHIP_SPI.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    MXCHIP_SPI.Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;

    if (HAL_SPI_Init(&MXCHIP_SPI) != HAL_OK)
    {
        /* Fatal — cannot proceed without SPI */
        while (1) {}
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HAL SPI Callbacks — Bridged to the ST reference driver
 *
 *  The reference mx_wifi_spi.c defines HAL_SPI_TransferCallback(void *hspi)
 *  which signals SpiTransferDoneSem.  In blocking mode (DMA_ON_USE == 0)
 *  these are not normally hit, but providing them prevents deadlocks if
 *  the SPI peripheral enters an unexpected state.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Forward declaration — defined in mx_wifi_spi.c */
extern void HAL_SPI_TransferCallback(void *hspi);

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI2)
    {
        HAL_SPI_TransferCallback(hspi);
    }
}

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI2)
    {
        HAL_SPI_TransferCallback(hspi);
    }
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == SPI2)
    {
        HAL_SPI_TransferCallback(hspi);
    }
}
