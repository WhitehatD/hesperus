/**
 * @file    camera.c
 * @brief   OV5640 Camera Capture via BSP DCMI Interface — Optimized
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Wraps the BSP_CAMERA functions for the MB1379 camera module (OV5640 sensor)
 * on the B-U585I-IOT02A Discovery Kit.
 *
 * 2026-08-21: reverted to the OV5640 manufacturer-recommended baseline
 * (Drivers/BSP/Components/ov5640/ov5640.c OV5640_Common[] — ST's complete
 * OmniVision init table: LSC, gamma, color matrix, AWB, AEC, timing). This
 * file used to override ~10 of those registers (AEC ceiling/target band,
 * VTS, PCLK divider, sharpen/denoise, a manual AWB freeze) tuned around an
 * assumption — a fixed, unchanging monitoring scene — that doesn't hold in
 * practice, and it produced measurably wrong images once the camera saw a
 * different scene than whatever it booted in front of. All removed except
 * binning (has a direct datasheet citation). AEC/AWB now run continuously
 * in AUTO (the vendor default) and reconverge before every capture instead
 * of freezing once at boot — see _wait_aec_converge(), called from both
 * capture entry points below.
 *
 * Remaining optimizations kept:
 *   - Adaptive AEC polling (replaces fixed 2s delay)
 *   - Continuous-mode warm-up (replaces 6× snapshot Start/Stop loop)
 *   - Compile-time SRAM budget validation
 *   - Diagnostics gated behind CAMERA_DIAG_ENABLED flag
 *
 * Capture flow:
 *   1. Camera_Init() — power up sensor, configure resolution (once per boot)
 *   2. Camera_CaptureFrame() / Camera_WarmCapture() — reconverge AEC, then
 *      continuous-mode warm-up + final frame (cold) or single snapshot (warm)
 *   3. Camera_DeInit() — power down to save energy before sleep
 */

#include "app_camera.h"
#include "firmware_config.h"
#include "debug_log.h"
#include "jpeg_encode.h"
#include "stm32u5xx_ll_dcache.h"
#include "main.h"

#include "b_u585i_iot02a_camera.h"
#include "b_u585i_iot02a_bus.h"

#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Compile-Time Safety Gate
 *
 *  Enterprise practice: if anyone changes CAMERA_FRAME_BUFFER_SIZE or
 *  resolution to a value that would overflow SRAM, the build fails
 *  immediately with a clear error rather than silently corrupting RAM.
 * ═══════════════════════════════════════════════════════════════════════════ */

_Static_assert(CAMERA_FRAME_BUFFER_SIZE <= CAMERA_FRAME_BUFFER_MAX,
    "FATAL: CAMERA_FRAME_BUFFER_SIZE exceeds safe SRAM budget! "
    "Reduce resolution or increase RAM_SAFETY_MARGIN_BYTES.");

/* ═══════════════════════════════════════════════════════════════════════════
 *  Module State
 * ═══════════════════════════════════════════════════════════════════════════ */

static volatile uint32_t s_frame_count = 0;   /* Counts frames in continuous mode */
static volatile uint32_t s_frame_size  = 0;
static volatile uint8_t  s_initialized = 0;    /* 1 = camera is warm and ready */
static volatile uint8_t  s_continuous_mode = 0; /* 1 = continuous capture active (ISR suspend enabled) */

/* Pointer to the active capture buffer (set by Camera_CaptureFrame) */
static uint8_t *s_active_buffer = NULL;

/* ═══════════════════════════════════════════════════════════════════════════
 *  OV5640 I2C Address
 * ═══════════════════════════════════════════════════════════════════════════ */

#define OV5640_I2C_ADDR  0x78

/* ═══════════════════════════════════════════════════════════════════════════
 *  Resolution Mapping
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Map our CameraResolution_t enum to the BSP CAMERA_RES_xxx constants.
 */
static uint32_t _map_resolution(CameraResolution_t res)
{
    switch (res)
    {
        case CAMERA_RES_QVGA:  return CAMERA_R320x240;
        case CAMERA_RES_VGA:   return CAMERA_R640x480;
        case CAMERA_RES_SVGA:  return CAMERA_R800x480;  /* Closest — BSP has no 800x600 */
        case CAMERA_RES_XGA:   return CAMERA_R800x480;  /* Closest — BSP has no 1024x768 */
        default:               return CAMERA_R640x480;
    }
}

/**
 * Return the expected raw frame size for a given resolution (RGB565).
 * In JPEG mode the actual captured size will be much smaller.
 */
__attribute__((unused)) static uint32_t _raw_frame_size(CameraResolution_t res)
{
    switch (res)
    {
        case CAMERA_RES_QVGA: return 320 * 240 * 2;
        case CAMERA_RES_VGA:  return 640 * 480 * 2;
        case CAMERA_RES_SVGA: return 800 * 600 * 2;
        case CAMERA_RES_XGA:  return 1024 * 768 * 2;
        default:              return 640 * 480 * 2;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  JPEG End-Marker Scanner
 *
 *  OV5640 JPEG frames always end with bytes 0xFF 0xD9.
 *  Scanning for this marker gives the actual JPEG payload size without
 *  needing DMA CNDTR register access.
 * ═══════════════════════════════════════════════════════════════════════════ */

#if CAMERA_JPEG_MODE
/**
 * @brief  Scan buffer for JPEG end-of-image marker (0xFF 0xD9).
 * @param  buf   Buffer containing the JPEG bitstream
 * @param  max   Maximum bytes to scan (buffer_size)
 * @return Actual JPEG size in bytes (includes the 0xFF 0xD9 bytes),
 *         or 0 if the end marker is not found.
 */
static uint32_t _find_jpeg_size(const uint8_t *buf, uint32_t max)
{
    if (!buf || max < 2)
        return 0;
    for (uint32_t i = 0; i + 1 < max; i++)
    {
        if (buf[i] == 0xFF && buf[i + 1] == 0xD9)
            return i + 2;   /* include the 2-byte EOI marker */
    }
    return 0;   /* marker not found — truncated or bad capture */
}
#endif /* CAMERA_JPEG_MODE */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Sensor Stream-State Assertion
 *
 *  OV5640 register 0x3008 (SYSTEM_CTROL0) is the master stream control:
 *    0x02 = normal operation (streaming)   — what OV5640_Start() writes
 *    0x42 = software power down (no video) — what OV5640_Stop() writes
 *
 *  ST's OV5640_Common[] init table ends with 0x3008=0x02, so a full sensor
 *  init is *supposed* to leave it streaming, and this codebase has always
 *  relied on that: BSP_CAMERA_Start() only arms DCMI (HAL_DCMI_Start_DMA),
 *  it performs no I2C at all, so nothing in the capture path ever asserts
 *  stream-on. That's a single point of failure with no recovery — if the
 *  sensor ends up in power-down for any reason, every capture from then on
 *  times out with a perfectly healthy DCMI, and even a full camera
 *  DeInit/Init cycle won't necessarily dig it out.
 *
 *  Measured live 2026-08-21 with a direct pin probe: PIXCLK toggling
 *  (263k edges/250ms) while HSYNC, VSYNC and all eight data lines sat
 *  completely static across multiple frame periods — the exact signature
 *  of a clocked-but-not-streaming sensor. So assert the documented state
 *  explicitly before arming DCMI rather than assuming it.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void _ensure_sensor_streaming(void)
{
    uint8_t ctrl0 = 0xFFu;

    if (BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3008, &ctrl0, 1) != BSP_ERROR_NONE)
    {
        LOG_WARN(TAG_CAM, "Could not read SYSTEM_CTROL0 (0x3008) — I2C read failed");
        return;
    }

    if (ctrl0 != 0x02u)
    {
        LOG_WARN(TAG_CAM, "Sensor NOT streaming: 0x3008=0x%02X (0x42=power-down) — asserting "
                 "stream-on (0x02)", ctrl0);
        uint8_t on = 0x02u;
        (void)BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3008, &on, 1);
    }

    /* ── Manufacturer-validated DVP power-up ─────────────────────────────
     * Source: Linux mainline drivers/media/i2c/ov5640.c,
     * ov5640_set_power_dvp() — the actively maintained encoding of
     * OmniVision's parallel-mode bring-up (the OmniVision app note PDF is
     * scanned images and unusable as a reference; this driver is the same
     * sequence in verifiable form).
     *
     * THE DISCREPANCY THAT MATTERS — 0x300E (IO_MIPI_CTRL00):
     *   Linux DVP power ON  -> 0x18  [4]=1 power down MIPI HS Tx,
     *                                [3]=1 power down MIPI LS Rx,
     *                                [2]=0 DVP enabled
     *   Linux DVP power OFF -> 0x58  (its documented reset default)
     * This sensor reads 0x58 — the powered-OFF value. ST's OV5640_Common[]
     * never writes the DVP-active value, and ST's own EnableDVPMode()
     * writes 0x58 as if it were the "on" value, which contradicts the
     * Linux driver. With the MIPI block left in its default state the core
     * can stream and the pads can read "enabled" while the DVP output
     * block is not actually driving framed video — exactly the measured
     * symptom (PCLK driven, HREF/VSYNC/data idle).
     *
     * Pad enables also differ: Linux uses 0x3017=0x7F (VSYNC/HREF/PCLK +
     * D[9:6]) and 0x3018=0xFC (all of D[5:0]). ST uses 0xFF/0xF3, and 0xF3
     * leaves bits[3:2] — two D[5:0] enables — CLEAR while setting the
     * GPIO bits[1:0] instead. Use the Linux values. */
    /* 0x3103 SCCB_SYSTEM_CTRL1 bit1: 0 = system clock from PAD (XCLK),
     * 1 = system clock from PLL. Both ST's OV5640_Common[] and Linux's
     * init table do 0x3103=0x11 (from pad, so the PLL can be reprogrammed
     * safely) -> software reset -> 0x3103=0x03 (switch to PLL). If the
     * sequence is interrupted or the second write is lost, the sensor is
     * left running off the pad clock with the PLL output not feeding the
     * output stage: registers all read correct, SCCB still works, the
     * output pads still drive their idle levels — and NO PCLK is produced.
     * That is precisely the measured state (PCLK pin floating, VSYNC/HSYNC/
     * data driven but static), so check and assert it. */
    struct { uint16_t reg; uint8_t val; const char *what; } dvp[] = {
        { 0x3103, 0x03, "clock source: PLL (not pad)"             },
        { 0x4740, 0x22, "polarity: HREF active-high, PCLK rising" },
        { 0x300E, 0x18, "MIPI Tx/Rx powered down, DVP enabled"    },
        { 0x3017, 0x7F, "pad enable: VSYNC/HREF/PCLK + D[9:6]"    },
        { 0x3018, 0xFC, "pad enable: D[5:0]"                      },
        { 0x4300, 0x6F, "format: RGB565"                          },
    };

    int changed = 0;
#if CAMERA_ASSERT_DVP_REGS
    for (unsigned i = 0; i < sizeof(dvp) / sizeof(dvp[0]); i++)
    {
        uint8_t cur = 0;
        if (BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, dvp[i].reg, &cur, 1) != BSP_ERROR_NONE)
            continue;
        if (cur == dvp[i].val)
            continue;

        LOG_WARN(TAG_CAM, "DVP setup 0x%04X: 0x%02X -> 0x%02X (%s)",
                 dvp[i].reg, cur, dvp[i].val, dvp[i].what);
        uint8_t v = dvp[i].val;
        (void)BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, dvp[i].reg, &v, 1);
        changed = 1;
    }
#else
    (void)dvp;  /* held back — see CAMERA_ASSERT_DVP_REGS in firmware_config.h */
#endif

    if (changed)
    {
        /* Re-assert stream-on after touching the output block, then give
         * the sensor a couple of frame periods to start framing before
         * DCMI is armed. */
        uint8_t on = 0x02u;
        (void)BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3008, &on, 1);
        HAL_Delay(100);
        LOG_INFO(TAG_CAM, "DVP output block reconfigured per Linux ov5640_set_power_dvp()");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  DVP Signal-Activity Probe
 *
 *  Added 2026-08-21 to settle a hardware-vs-software question decisively
 *  instead of guessing at it. When DCMI reports 0 frames, there are two
 *  fundamentally different causes and the register state alone does not
 *  distinguish them:
 *
 *    (a) the sensor isn't clocking pixels out at all (not streaming, or the
 *        DVP bus isn't physically connected) — nothing software can fix;
 *    (b) the sensor IS clocking, but DCMI/DMA is misconfigured on our side
 *        — entirely a firmware bug.
 *
 *  GPIO IDR still reflects the live pin level while a pin is in alternate-
 *  function mode, so we can sample the three sync/clock lines directly and
 *  count transitions without disturbing DCMI. Pin map from the BSP's
 *  DCMI_MspInit (b_u585i_iot02a_camera.c): PIXCLK=PA6, VSYNC=PB7, HSYNC=PH8.
 *
 *  PIXCLK runs in the MHz range, so even a short sample window sees
 *  thousands of transitions when the sensor is alive. Zero transitions on
 *  PIXCLK is conclusive: case (a).
 * ═══════════════════════════════════════════════════════════════════════════ */
/* ═══════════════════════════════════════════════════════════════════════════
 *  DVP Pin Continuity Test  (driven vs floating — unambiguous, no scope)
 *
 *  Every register on this sensor reads correct while the sync/data lines
 *  stay idle, and edge-count evidence became untrustworthy once PCLK's
 *  count shifted merely from enabling other output pads (a hallmark of
 *  crosstalk on an undriven pin, not of a real clock).
 *
 *  This resolves it definitively. Reconfigure each DVP line as a plain
 *  GPIO input, first with the internal pull-UP, then the pull-DOWN, and
 *  read the level each time:
 *     level follows the pull (1 then 0)  -> pin is FLOATING (undriven)
 *     level holds against the pull       -> pin is actively DRIVEN
 *  The sensor's push-pull output easily overpowers the ~40k internal
 *  pull resistors, so this cannot produce a false "driven" result.
 *
 *  Pins are restored to DCMI alternate function afterwards.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void _probe_pin_continuity(void)
{
    static const struct {
        GPIO_TypeDef *port;
        uint16_t      pin;
        uint8_t       bit;
        const char   *name;
    } lines[] = {
        { GPIOA, GPIO_PIN_6,  6,  "PCLK/PA6"  },
        { GPIOB, GPIO_PIN_7,  7,  "VSYNC/PB7" },
        { GPIOH, GPIO_PIN_8,  8,  "HSYNC/PH8" },
        { GPIOC, GPIO_PIN_6,  6,  "D0/PC6"    },
        { GPIOH, GPIO_PIN_14, 14, "D4/PH14"   },
        { GPIOI, GPIO_PIN_7,  7,  "D7/PI7"    },
    };

    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
    {
        GPIO_InitTypeDef g = {0};
        g.Pin   = lines[i].pin;
        g.Mode  = GPIO_MODE_INPUT;
        g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;

        g.Pull = GPIO_PULLUP;
        HAL_GPIO_Init(lines[i].port, &g);
        HAL_Delay(2);
        uint32_t with_pu = (lines[i].port->IDR >> lines[i].bit) & 1U;

        g.Pull = GPIO_PULLDOWN;
        HAL_GPIO_Init(lines[i].port, &g);
        HAL_Delay(2);
        uint32_t with_pd = (lines[i].port->IDR >> lines[i].bit) & 1U;

        const char *verdict = (with_pu == 1U && with_pd == 0U)
                                ? "FLOATING (follows pull — nothing driving it)"
                                : "DRIVEN (holds against pull)";
        LOG_ERROR(TAG_CAM, "  %-10s pull-up=%lu pull-down=%lu -> %s",
                  lines[i].name, (unsigned long)with_pu, (unsigned long)with_pd, verdict);

        /* Restore DCMI alternate function on this pin. */
        g.Pull      = GPIO_NOPULL;
        g.Mode      = GPIO_MODE_AF_PP;
        g.Alternate = (lines[i].port == GPIOA) ? GPIO_AF4_DCMI : GPIO_AF10_DCMI;
        HAL_GPIO_Init(lines[i].port, &g);
    }
}

static void _probe_dvp_signals(void)
{
    /* Sample for a fixed WALL-CLOCK window, not a fixed iteration count.
     * VSYNC only pulses once per frame (~33-66ms), so a short window can
     * legitimately see zero VSYNC edges even on a perfectly healthy sensor
     * — a trap the first version of this probe fell into. 250ms guarantees
     * several frame periods. HSYNC (~one pulse per line, tens of kHz) and
     * PIXCLK (MHz, heavily aliased by our sample rate but unmistakably
     * active) both show large counts when alive. */
    const uint32_t WINDOW_MS = 250;

    uint32_t pixclk_edges = 0, vsync_edges = 0, hsync_edges = 0, data_edges = 0;
    uint32_t samples = 0;

    /* Data bus: D0=PC6 D1=PC7 D2=PC8, D3=PE1, D4=PH14, D5=PI6 D6=PI4 D7=PI7.
     * Pack the ones we can read cheaply into a signature and watch it move —
     * we only care whether pixel data is changing at all, not its value. */
#define DVP_DATA_SIG()  ( ((GPIOC->IDR >> 6) & 0x7U)        \
                        | (((GPIOE->IDR >> 1) & 1U) << 3)   \
                        | (((GPIOH->IDR >> 14) & 1U) << 4)  \
                        | (((GPIOI->IDR >> 4) & 1U) << 5)   \
                        | (((GPIOI->IDR >> 6) & 1U) << 6)   \
                        | (((GPIOI->IDR >> 7) & 1U) << 7) )

    uint32_t prev_pixclk = (GPIOA->IDR >> 6) & 1U;
    uint32_t prev_vsync  = (GPIOB->IDR >> 7) & 1U;
    uint32_t prev_hsync  = (GPIOH->IDR >> 8) & 1U;
    uint32_t prev_data   = DVP_DATA_SIG();

    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < WINDOW_MS)
    {
        uint32_t pixclk = (GPIOA->IDR >> 6) & 1U;
        uint32_t vsync  = (GPIOB->IDR >> 7) & 1U;
        uint32_t hsync  = (GPIOH->IDR >> 8) & 1U;
        uint32_t data   = DVP_DATA_SIG();

        if (pixclk != prev_pixclk) { pixclk_edges++; prev_pixclk = pixclk; }
        if (vsync  != prev_vsync)  { vsync_edges++;  prev_vsync  = vsync;  }
        if (hsync  != prev_hsync)  { hsync_edges++;  prev_hsync  = hsync;  }
        if (data   != prev_data)   { data_edges++;   prev_data   = data;   }
        samples++;
    }
#undef DVP_DATA_SIG

    LOG_ERROR(TAG_CAM, "DVP probe %lums (%lu samples): PIXCLK=%lu VSYNC=%lu HSYNC=%lu DATA=%lu "
              "(levels: pclk=%lu vs=%lu hs=%lu data=0x%02lX)",
              (unsigned long)WINDOW_MS, (unsigned long)samples,
              (unsigned long)pixclk_edges, (unsigned long)vsync_edges,
              (unsigned long)hsync_edges,  (unsigned long)data_edges,
              (unsigned long)prev_pixclk, (unsigned long)prev_vsync,
              (unsigned long)prev_hsync,  (unsigned long)prev_data);

    if (pixclk_edges == 0)
    {
        LOG_ERROR(TAG_CAM, "DVP verdict: PIXCLK DEAD — sensor not clocking at all.");
    }
    else if (hsync_edges == 0 && data_edges == 0)
    {
        LOG_ERROR(TAG_CAM, "DVP verdict: PIXCLK alive, but NO line sync AND NO pixel data — "
                  "the sensor is clocked but not streaming video. Sensor-side: stream/standby "
                  "or output-format state, NOT a DCMI/DMA fault and NOT the ribbon (PIXCLK and "
                  "I2C both cross it fine).");
    }
    else if (hsync_edges == 0 && data_edges > 0)
    {
        LOG_ERROR(TAG_CAM, "DVP verdict: pixel data IS moving but HSYNC is static — sync lines "
                  "specifically are not reaching the MCU. Points at the HSYNC/VSYNC pins/traces, "
                  "not the sensor.");
    }
    else
    {
        LOG_ERROR(TAG_CAM, "DVP verdict: sensor IS delivering framed video (sync + data active) "
                  "— the fault is ours: DCMI config, DMA, or IRQ wiring.");
    }

    /* Settle driven-vs-floating for each line — see _probe_pin_continuity(). */
    LOG_ERROR(TAG_CAM, "Pin continuity test (driven vs floating):");
    _probe_pin_continuity();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  AEC Convergence Polling (replaces fixed HAL_Delay)
 *
 *  Enterprise practice: poll the ISP's current exposure level register
 *  rather than blindly waiting. Returns as soon as AEC has settled,
 *  saving 500ms–1.5s in well-lit environments.
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Wait for OV5640 AEC to converge, with hard timeout.
 *
 * Polls the AEC average luminance register (0x56A1) and compares against
 * the AEC target brightness bands we configured. If we can't read the
 * register, falls back to a fixed delay.
 */
static void _wait_aec_converge(void)
{
    uint32_t start = HAL_GetTick();
    uint8_t  avg_lum;
    int32_t  ret;

    const uint8_t AEC_TARGET_LOW  = 0x50;
    const uint8_t AEC_TARGET_HIGH = 0x90;

    /* Stability detection: if luminance stops changing, AEC has done
     * all it can — no point waiting further (handles very dark scenes). */
    uint8_t  prev_lum = 0xFF;
    uint8_t  stable_count = 0;
    const uint8_t STABLE_THRESHOLD = 6;  /* 6 × 50ms = 300ms stable → done */

    LOG_DEBUG(TAG_CAM, "Waiting for AEC convergence (timeout=%dms)...",
              CAMERA_AEC_SETTLE_TIMEOUT_MS);

    while ((HAL_GetTick() - start) < (uint32_t)CAMERA_AEC_SETTLE_TIMEOUT_MS)
    {
        ret = BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x56A1, &avg_lum, 1);
        if (ret != 0)
        {
            HAL_Delay(50);
            continue;
        }

        /* In target range — converged */
        if (avg_lum >= AEC_TARGET_LOW && avg_lum <= AEC_TARGET_HIGH)
        {
            LOG_INFO(TAG_CAM, "AEC converged in %lums (lum=0x%02X)",
                     (unsigned long)(HAL_GetTick() - start), avg_lum);
            return;
        }

        /* Stability check: AEC saturated (dark/bright scene) */
        if (avg_lum == prev_lum || (avg_lum > 0 && avg_lum <= prev_lum + 1 && avg_lum + 1 >= prev_lum))
        {
            stable_count++;
            if (stable_count >= STABLE_THRESHOLD)
            {
                LOG_INFO(TAG_CAM, "AEC stable in %lums (lum=0x%02X — scene limit)",
                         (unsigned long)(HAL_GetTick() - start), avg_lum);
                return;
            }
        }
        else
        {
            stable_count = 0;
        }
        prev_lum = avg_lum;

        HAL_Delay(50);
    }

    BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x56A1, &avg_lum, 1);
    LOG_WARN(TAG_CAM, "AEC timeout after %dms (lum=0x%02X — proceeding anyway)",
             CAMERA_AEC_SETTLE_TIMEOUT_MS, avg_lum);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Initialization
 * ═══════════════════════════════════════════════════════════════════════════ */

CameraStatus_t Camera_Init(CameraResolution_t resolution)
{
    /* ── Double-init guard: skip if already warm ── */
    if (s_initialized)
    {
        LOG_INFO(TAG_CAM, "Camera already initialized (warm) — skipping init");
        return CAMERA_OK;
    }

    LOG_INFO(TAG_CAM, "Initializing OV5640 camera (resolution=%d)...", resolution);

    uint32_t bsp_res = _map_resolution(resolution);

    /*
     * BSP_CAMERA_Init:
     *   Instance = 0
     *   Resolution = bsp_res
     *   PixelFormat = CAMERA_PF_RGB565
     *
     * The BSP handles I2C configuration of the OV5640 + DCMI/DMA setup.
     */
    int32_t ret = BSP_CAMERA_Init(0, bsp_res, CAMERA_PF_RGB565);
    if (ret != BSP_ERROR_NONE)
    {
        LOG_ERROR(TAG_CAM, "BSP_CAMERA_Init failed (err=%ld)", (long)ret);
        return CAMERA_ERROR_INIT;
    }

    /*
     * ── OV5640 Register Overrides ────────────────────────────────────────
     *
     * 2026-08-21 cleanup: this used to override ~10 registers (AEC gain
     * ceiling, AEC target band, VTS, PCLK divider, sharpen/denoise, a
     * manual AWB freeze) on top of ST's manufacturer-verified init table
     * (Drivers/BSP/Components/ov5640/ov5640.c OV5640_Common[], which IS
     * OmniVision's complete recommended config: LSC, gamma, color matrix,
     * AWB, AEC). None of those overrides were re-validated against each
     * other on hardware, and they produced measurably wrong images (hazy/
     * washed-out outdoors, blown-out+torn on a bright ceiling) once the
     * camera was pointed at scenes other than the one it happened to boot
     * in front of. Removed all of them except binning, which has a direct
     * datasheet citation below. AEC/AWB now run in AUTO continuously (the
     * vendor default) and reconverge before every capture — see
     * _wait_aec_converge(), now called per-capture instead of once at
     * cold init (Camera_CaptureFrame / Camera_WarmCapture).
     *
     * OV5640 I2C address = 0x78 (confirmed in b_u585i_iot02a_camera.h).
     */
    uint8_t val;

    /* ── 0. ENABLE BINNING — the single biggest image-quality win ────────
     *
     * ST's BSP common table (Drivers/BSP/Components/ov5640/ov5640.c:180-183)
     * programs:
     *     0x3814 X_INC = 0x31   → 2:1 horizontal subsample
     *     0x3815 Y_INC = 0x31   → 2:1 vertical subsample
     *     0x3820 TC_REG20 = 0x06  → bit[0] vertical binning   = 0 (OFF)
     *     0x3821 TC_REG21 = 0x00  → bit[0] horizontal binning = 0 (OFF)
     *
     * Per the OV5640 datasheet: "When the binning function is ON, voltage
     * levels of adjacent pixels are averaged before being sent to the ADC.
     * If the binning function is OFF, the pixels which are not output are
     * merely skipped."
     *
     * So with 2:1 subsampling and binning OFF we were SKIPPING — discarding
     * 3 of every 4 photons and point-sampling a sparse grid with no
     * anti-alias prefilter. Result: aliasing/moiré, false colour, and ~half
     * the SNR the sensor is capable of. ST's OV5640_SetResolution() only
     * writes 0x3808-0x380B (output size) and never touches these, so the
     * defect is invisible from the BSP API — it must be overridden here.
     *
     * Enabling binning averages the 2x2 pixel groups instead. Because
     * 0x3814/0x3815 are left unchanged, the ISP input pixel count, HTS, VTS
     * and PCLK are all identical — only the analog combining changes. This
     * is timing-neutral and costs zero extra bytes on the wire.
     *
     * NOTE: 0x3820 keeps ST's existing bit[1] (vflip) so orientation is
     * unchanged; we only OR in bit[0]. For 0x3821 we set bit[0] ONLY —
     * Linux's 0x07 would also set bits[2:1] which enable mirror. */
#if CAMERA_BASELINE_ONLY
    /* 2026-08-21 bisect: skip binning to run a pure ST-baseline init. */
    (void)val;
#else
    val = 0x07;  /* 0x06 | bit0 — vertical binning ON, orientation preserved */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3820, &val, 1);
    val = 0x01;  /* bit0 only — horizontal binning ON, no mirror */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3821, &val, 1);
#endif

    /* Dump the registers that decide whether framed video reaches DCMI at
     * all. 0x4740 POLARITY_CTRL is the one with history here: the JPEG-mode
     * block further down documents that BSP_CAMERA_Init writes 0x23, whose
     * HREF bit inverts the output in silicon, so DCMI (HSPOL active-high)
     * sees "line never active" and captures nothing. That correction is
     * gated behind CAMERA_JPEG_MODE, which is 0 — so in RGB565 mode nothing
     * ever checks it. Log the real values instead of assuming. */
    {
        uint8_t pol = 0, tc20 = 0, tc21 = 0, fmt = 0, mux = 0;
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x4740, &pol,  1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3820, &tc20, 1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3821, &tc21, 1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x4300, &fmt,  1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x501F, &mux,  1);
        LOG_INFO(TAG_CAM, "Sensor out-cfg: 0x4740(pol)=0x%02X 0x3820=0x%02X 0x3821=0x%02X "
                 "0x4300(fmt)=0x%02X 0x501F(mux)=0x%02X", pol, tc20, tc21, fmt, mux);

        /* Timing generator / output window. If the output size or the total
         * line/frame periods are degenerate, the sensor clocks forever and
         * never emits an active line — which is precisely what the pin
         * probe measures. Proven this session that the sensor DOES drive
         * the sync pins (a polarity write moved the HSYNC idle level), so
         * the remaining explanation is the frame geometry itself.
         *   0x3808/09 = DVP output width   0x380A/0B = output height
         *   0x380C/0D = HTS (total line)   0x380E/0F = VTS (total frame)
         * A zero in any of these is fatal to framing. */
        uint8_t w_h = 0, w_l = 0, h_h = 0, h_l = 0;
        uint8_t hts_h = 0, hts_l = 0, vts_h = 0, vts_l = 0;
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3808, &w_h,   1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3809, &w_l,   1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x380A, &h_h,   1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x380B, &h_l,   1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x380C, &hts_h, 1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x380D, &hts_l, 1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x380E, &vts_h, 1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x380F, &vts_l, 1);
        LOG_INFO(TAG_CAM, "Sensor timing: out=%ux%u HTS=%u VTS=%u",
                 (unsigned)((w_h << 8) | w_l), (unsigned)((h_h << 8) | h_l),
                 (unsigned)((hts_h << 8) | hts_l), (unsigned)((vts_h << 8) | vts_l));

        /* Clock tree + block reset/enable. PCLK is derived from the PLL via
         * its own divider chain (0x3108 bits[5:4]) while the pixel array and
         * timing generator run off SCLK (0x3108 bits[1:0]). That's how the
         * measured state — PCLK driven and responsive, timing generator
         * silent — is even possible, so read the whole chain. Divider map
         * from Linux ov5640.c: 0x3034 bit-div, 0x3035 sysclk/MIPI div,
         * 0x3036 PLL multiplier, 0x3037 pre-div + root-div, 0x3108 SCLK /
         * SCLK2x / PCLK dividers. Also read the block reset (0x3000-0x3002)
         * and clock-enable (0x3004-0x3006) registers: a block held in reset
         * or with its clock gated off produces exactly this signature.
         * ST's OV5640_Common[] sets 0x3000=0x00, 0x3002=0x1c, 0x3004=0xff,
         * 0x3006=0xc3 — identical to Linux's table, so any deviation is
         * something clobbering them at runtime. */
        uint8_t p34 = 0, p35 = 0, p36 = 0, p37 = 0, p108 = 0;
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3034, &p34,  1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3035, &p35,  1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3036, &p36,  1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3037, &p37,  1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3108, &p108, 1);

        uint8_t r00 = 0, r01 = 0, r02 = 0, c00 = 0, c01 = 0, c02 = 0;
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3000, &r00, 1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3001, &r01, 1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3002, &r02, 1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3004, &c00, 1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3005, &c01, 1);
        (void)BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3006, &c02, 1);

        LOG_INFO(TAG_CAM, "Sensor clocks: PLL 0x3034=0x%02X 0x3035=0x%02X 0x3036=0x%02X "
                 "0x3037=0x%02X rootdiv 0x3108=0x%02X", p34, p35, p36, p37, p108);
        LOG_INFO(TAG_CAM, "Sensor blocks: reset 0x3000=0x%02X 0x3001=0x%02X 0x3002=0x%02X "
                 "(want 00/xx/1C) | clken 0x3004=0x%02X 0x3005=0x%02X 0x3006=0x%02X "
                 "(want FF/xx/C3)", r00, r01, r02, c00, c01, c02);

        /* 0x4740 POLARITY_CTRL: per the Linux mainline ov5640 driver
         * (ov5640_set_power_dvp), bit1=1 means HREF active HIGH — the
         * DIRECT sense, not inverted. An earlier fix here cleared bit1
         * based on a comment in this codebase's JPEG block claiming the
         * opposite; that contradicted both the Linux driver's documented
         * semantics and the empirical record (captures worked with 0x22 +
         * DCMI HSPOL active-high). 0x22 = HREF active high + PCLK sample
         * rising is CORRECT for this DCMI config. Do not "fix" it. */
        (void)pol;
    }

    /* 1. Ensure AEC/AGC stay in AUTO mode (matches OV5640 POR default —
     *    this is a defensive assertion, not an override of anything in
     *    OV5640_Common[], which doesn't touch 0x3503 either). Everything
     *    else that used to live here (AEC gain ceiling, AEC target band,
     *    max-exposure lines, VTS, night mode, PCLK divider, sharpen/
     *    denoise trim, AWB freeze) has been removed — those were unverified
     *    deviations from ST's manufacturer-recommended OV5640_Common[]
     *    table, tuned around an assumption (fixed static scene) that
     *    doesn't hold, and they were the direct cause of the hazy/washed/
     *    blown-out captures. AWB and AEC now run continuously in AUTO for
     *    the lifetime of the sensor (the vendor default) and reconverge
     *    before every single capture — see _wait_aec_converge(), called
     *    from Camera_CaptureFrame()/Camera_WarmCapture(), not here. */
    val = 0x00;  /* bit 0=0: AEC auto, bit 1=0: AGC auto */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3503, &val, 1);

#if CAMERA_NIGHT_MODE_ENABLE
    /* ── Night mode: give AEC headroom beyond one frame period ───────────
     *
     * See CAMERA_NIGHT_MODE_ENABLE in firmware_config.h for the full
     * rationale. Short version: ST's OV5640_Common[] caps max exposure to
     * ~984 lines (~1 frame at VTS=1088) and never enables night mode
     * (0x3A00 stays at its POR default, night-mode bit clear), so in a dim
     * room the AEC loop hits the exposure ceiling almost immediately and
     * can only fight darkness with gain — adding noise instead of light.
     * This block is purely additive to the AUTO AEC loop: it does not
     * disable AUTO, does not touch the gain ceiling (already generous at
     * ~15.5x from 0x3A18/0x3A19), and does not touch the AEC target band.
     * A bright scene simply never needs the extra headroom, so it should
     * be a no-op there. UNVALIDATED ON HARDWARE — see the config comment
     * for the required daylight/lit-room check before the BNAIC demo. */
    {
        uint8_t reg3a00 = 0;
        BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, 0x3A00, &reg3a00, 1);
        reg3a00 |= 0x04;  /* bit2 = NightModeOn; preserve banding (bit5) and other bits as read */
        BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A00, &reg3a00, 1);

        /* Max exposure ceiling, both 50Hz and 60Hz pairs kept consistent
         * per datasheet guidance. 984 * 4 = 3936 = 0x0F60. */
        val = 0x0F;
        BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A02, &val, 1);  /* 60Hz max exposure high */
        val = 0x60;
        BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A03, &val, 1);  /* 60Hz max exposure low */
        val = 0x0F;
        BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A14, &val, 1);  /* 50Hz max exposure high */
        val = 0x60;
        BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A15, &val, 1);  /* 50Hz max exposure low */

        LOG_INFO(TAG_CAM, "Night mode enabled: 0x3A00=0x%02X, max-expo ceiling 984->3936 lines (~4x frame)",
                 reg3a00);
    }
#endif

#if CAMERA_JPEG_MODE
    /* ── Enable OV5640 JPEG output + DCMI JPEG mode ──────────────────────
     *
     * Use BSP_CAMERA_SetPixelFormat(OV5640_JPEG) instead of raw I2C writes.
     * The ST OV5640 driver's SetPixelFormat(JPEG) does 5 operations:
     *   1. FORMAT_CTRL00 = 0x30  (JPEG output select)
     *   2. FORMAT_MUX_CTRL = 0x00
     *   3. TIMING_TC_REG21 |= (1<<5)     — enable JPEG timing
     *   4. SYSREM_RESET02  &= ~0x1C      — de-assert JPEG module resets
     *   5. CLOCK_ENABLE02  |= 0x28       — enable JPEG clocks
     *
     * Steps 3–5 were missing from prior raw-register code, leaving the JPEG
     * encoder in reset with no clock — causing all-zero captures (camera
     * outputting RGB565 black pixels = 0x0000 in a dark scene). */
    if (BSP_CAMERA_SetPixelFormat(0, OV5640_JPEG) != BSP_ERROR_NONE)
    {
        LOG_ERROR(TAG_CAM, "OV5640 SetPixelFormat(JPEG) failed");
    }

    /* Quality scale: 0 = best quality / largest file, 0xFF = worst / smallest.
     * BSP does not expose a quality API, so write directly. */
    val = (uint8_t)CAMERA_JPEG_QUALITY;
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, OV5640_JPEG_CTRL07, &val, 1);

    /* CRITICAL: OV5640 POLARITY_CTRL (0x4740) override for JPEG mode.
     *
     * The BSP_CAMERA_Init pipeline writes 0x23 (HREF bit[1]=1) which counter-
     * intuitively INVERTS the HREF output in OV5640 silicon — the SetPolarities
     * macro OV5640_POLARITY_HREF_HIGH=1 maps to bit1=1 but that bit selects
     * "inverted polarity" in the actual register.  Result: sensor outputs
     * HREF=LOW during data-valid periods.  DCMI is configured HSPOL=1 (active
     * HIGH) so it sees the inverted HREF as "line not active" and captures
     * nothing — buffer stays all zeros (proven by buf[0..7]=0000... + CDAR
     * stuck at buffer_start + CBR1_BNDT=0 in v1.0.245 capture log).
     *
     * In RGB565 mode this mismatch is harmless because DCMI uses byte-count
     * DMA and doesn't strictly need HREF.  In JPEG mode HREF MUST be correct
     * because each PCLK byte is gated by HREF=active.
     *
     * Fix: write 0x21 — bit[1]=0 makes HREF "normal polarity" (active HIGH),
     * matching DCMI HSPOL=1.  This is the value confirmed working at commit
     * 4e0db7a (partial JPEG captured, only EOI was lost to FIFO race).
     *   bit[5]=1 → PCLK rising-edge sample (matches DCMI PCKPOL=1)
     *   bit[1]=0 → HREF active HIGH        (matches DCMI HSPOL=1)
     *   bit[0]=1 → VSYNC active HIGH       (matches DCMI VSPOL=1) */
    val = 0x21;
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, OV5640_POLARITY_CTRL, &val, 1);

    /* Allow OV5640 ISP pipeline to flush after format switch (RGB565→JPEG). */
    HAL_Delay(100);

    /* Set DCMI JPEG mode — safe while DCMI is disabled (between Init and Start) */
    DCMI->CR |= DCMI_CR_JPEG;

    LOG_INFO(TAG_CAM, "JPEG mode: OV5640 encoder active (buffer=%lu KB, QS=%d)",
             (unsigned long)(CAMERA_FRAME_BUFFER_SIZE / 1024),
             (int)CAMERA_JPEG_QUALITY);

    /* ── Reg readback: verify JPEG encoder state ────────────────────────
     * Read back 4 registers to confirm I2C writes landed.
     * Expected values:
     *   TC_REG21 bit5 = 1  (JPEG timing enabled)
     *   RESET02  [4:2]= 0  (JPEG/JFIFO/SFIFO out of reset)
     *   CLOCK02  bit5,3=1  (JPEG clocks on)
     *   FMT_CTRL00 = 0x30  (JPEG format selected)
     *   DCMI_CR  bit3  = 1  (DCMI in JPEG mode) */
    {
        uint8_t rv;
        BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, OV5640_TIMING_TC_REG21, &rv, 1);
        LOG_DEBUG(TAG_CAM, "JPEG regs: TC_REG21=0x%02X (expect bit5=1)", (unsigned)rv);
        BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, OV5640_SYSREM_RESET02, &rv, 1);
        LOG_DEBUG(TAG_CAM, "JPEG regs: RESET02  =0x%02X (expect bits[4:2]=0)", (unsigned)rv);
        BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, OV5640_CLOCK_ENABLE02, &rv, 1);
        LOG_DEBUG(TAG_CAM, "JPEG regs: CLOCK02  =0x%02X (expect bits[5,3]=1)", (unsigned)rv);
        BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, OV5640_FORMAT_CTRL00, &rv, 1);
        LOG_DEBUG(TAG_CAM, "JPEG regs: FMT_CTRL =0x%02X DCMI_CR=0x%08lX (expect FMT=0x30 CR.bit3=1)",
                  (unsigned)rv, (unsigned long)DCMI->CR);
        BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, OV5640_FORMAT_MUX_CTRL, &rv, 1);
        LOG_DEBUG(TAG_CAM, "JPEG regs: FMT_MUX  =0x%02X (expect 0x00 = JPEG path)", (unsigned)rv);
    }
#endif /* CAMERA_JPEG_MODE */

    LOG_INFO(TAG_CAM, "Camera initialized OK (raw frame size: %lu bytes, "
             "AEC/AWB auto, manufacturer timing)",
             (unsigned long)_raw_frame_size(resolution));

    s_initialized = 1;
    return CAMERA_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Query: Is Camera Warm?
 * ═══════════════════════════════════════════════════════════════════════════ */

uint8_t Camera_IsInitialized(void)
{
    return s_initialized;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Single Frame Capture — Continuous-Mode Warm-up
 *
 *  Enterprise optimization: instead of 6× Start/Stop in Snapshot mode
 *  (each with ~50ms overhead), we use Continuous mode and count frame
 *  interrupts. This eliminates per-frame DCMI reinit overhead.
 * ═══════════════════════════════════════════════════════════════════════════ */

CameraStatus_t Camera_CaptureFrame(uint8_t *buffer, uint32_t buffer_size,
                                    uint32_t *captured_size)
{
    LOG_INFO(TAG_CAM, "Starting frame capture (%d warm-up + 1 final)...",
             CAMERA_WARMUP_FRAMES);

    if (buffer == NULL || captured_size == NULL)
    {
        LOG_ERROR(TAG_CAM, "Null buffer or size pointer");
        return CAMERA_ERROR_CAPTURE;
    }

    /* Buffer overflow guard */
    if (buffer_size < CAMERA_FRAME_BUFFER_SIZE)
    {
        LOG_ERROR(TAG_CAM, "Buffer too small (%lu < %lu)",
                  (unsigned long)buffer_size,
                  (unsigned long)CAMERA_FRAME_BUFFER_SIZE);
        return CAMERA_ERROR_CAPTURE;
    }

#if CAMERA_JPEG_MODE
    /* In JPEG mode the DCMI JPEG protocol requires snapshot mode (variable frame
     * size with VSYNC-terminated transfer). The continuous-mode warmup path
     * assumes fixed-size frames and does not work reliably with JPEG output.
     * Camera_WarmCapture() reconverges AEC itself before capturing, so no
     * separate warmup is needed here. */
    LOG_INFO(TAG_CAM, "JPEG mode: delegating to Camera_WarmCapture (single snapshot)");
    return Camera_WarmCapture(buffer, buffer_size, captured_size);
#endif

    /* 2026-08-21: reconverge AEC before every capture, not just once at
     * cold init — see the matching comment in Camera_WarmCapture(). This
     * path only runs on a genuinely cold camera (main.c falls back here
     * when Camera_IsInitialized() is false), but the same staleness bug
     * applies: don't trust whatever exposure happened to be in the sensor. */
    _ensure_sensor_streaming();
    _wait_aec_converge();

    const uint32_t TOTAL_FRAMES = CAMERA_WARMUP_FRAMES + 1;  /* warm-up + final */
    int32_t ret;

    /* Reset frame counter */
    s_frame_count = 0;
    s_frame_size  = 0;
    s_active_buffer = buffer;
    s_continuous_mode = 1;  /* Enable ISR suspend logic for continuous capture */

    /* Turn ON tally light (Red LED) directly before capture */
    BSP_LED_On(LED_RED);

    /* Start continuous capture — DMA writes directly into caller's buffer.
     * Each frame overwrites the previous one in-place. After TOTAL_FRAMES,
     * the buffer contains the final (well-exposed) frame. */
    ret = BSP_CAMERA_Start(0, buffer, CAMERA_MODE_CONTINUOUS);
    if (ret != BSP_ERROR_NONE)
    {
        LOG_ERROR(TAG_CAM, "BSP_CAMERA_Start (continuous) failed (err=%ld)", (long)ret);
        s_active_buffer = NULL;
        return CAMERA_ERROR_CAPTURE;
    }

    /* ── DCMI diagnostic: check if sensor is outputting VSYNC/HSYNC ── */
    HAL_Delay(50);  /* Let at least one VSYNC edge pass */
    LOG_DEBUG(TAG_CAM, "DCMI SR=0x%08lX CR=0x%08lX (after start+50ms)",
             (unsigned long)DCMI->SR, (unsigned long)DCMI->CR);

    /* Wait for TOTAL_FRAMES frame-complete interrupts */
    uint32_t start_tick = HAL_GetTick();

    /* Frame timing now comes from ST's stock OV5640_Common[] VTS (0x0440),
     * not a project override. Timeout: generous 3s to handle low-light
     * slow-down regardless of the exact stock frame rate. */
    const uint32_t CAPTURE_TIMEOUT_MS = 3000;

    while (s_frame_count < TOTAL_FRAMES)
    {
        if ((HAL_GetTick() - start_tick) > CAPTURE_TIMEOUT_MS)
        {
            LOG_ERROR(TAG_CAM, "Capture timeout after %lu ms (got %lu/%lu frames)",
                      (unsigned long)CAPTURE_TIMEOUT_MS,
                      (unsigned long)s_frame_count,
                      (unsigned long)TOTAL_FRAMES);
            /* Probe the DVP lines BEFORE stopping DCMI — the sensor is still
             * streaming at this point, so this tells us whether pixels are
             * physically arriving at all. */
            _probe_dvp_signals();
            BSP_CAMERA_Stop(0);
            BSP_LED_Off(LED_RED);
            s_active_buffer = NULL;
            return CAMERA_ERROR_TIMEOUT;
        }
        /* Yield CPU briefly — 1ms poll granularity */
        HAL_Delay(1);
    }

    /* Diagnostic: capture RISR before Stop clears peripheral state.
     * OVR_RIS (bit1) = FIFO overrun — PCLK outpaced DMA → pixel(s) dropped → black line.
     * ERR_RIS (bit2) = sync error   — VSYNC/HSYNC mismatch → corrupt frame. */
    LOG_DEBUG(TAG_CAM, "DCMI RISR=0x%08lX (OVR=%lu ERR=%lu)",
              (unsigned long)DCMI->RISR,
              (unsigned long)((DCMI->RISR >> 1) & 1U),
              (unsigned long)((DCMI->RISR >> 2) & 1U));

    /* Stop continuous capture — the buffer now has the final frame */
    BSP_CAMERA_Stop(0);
    BSP_LED_Off(LED_RED);
    s_active_buffer = NULL;
    s_continuous_mode = 0;

    /* Determine frame size */
    uint32_t copy_size = s_frame_size;
    if (copy_size == 0)
    {
        copy_size = buffer_size;
    }

    if (copy_size > buffer_size)
    {
        LOG_WARN(TAG_CAM, "Frame (%lu) exceeds buffer (%lu), clamping",
                 (unsigned long)copy_size, (unsigned long)buffer_size);
        copy_size = buffer_size;
    }

    /* No memcpy needed — DMA wrote directly into caller's buffer */
    *captured_size = copy_size;

    /* SEC-09: Enterprise Cache Coherency (Fintech Grade)
     * Invalidate the CPU D-Cache for the DMA destination buffer so the CPU
     * reads the actual photo from physical SRAM, not stale zeroed cache lines. */
    LL_DCACHE_SetCommand(DCACHE1, LL_DCACHE_COMMAND_INVALIDATE_BY_ADDR);
    LL_DCACHE_SetStartAddress(DCACHE1, (uint32_t)buffer);
    LL_DCACHE_SetEndAddress(DCACHE1, (uint32_t)buffer + copy_size - 1);
    LL_DCACHE_StartCommand(DCACHE1);
    while (LL_DCACHE_IsActiveFlag_BUSYCMD(DCACHE1));

    /* Diagnostic: log first 8 bytes to confirm DMA wrote real data.
     * RGB565 first pixel typically non-zero — all zeros = DMA didn't write.
     * Black-line artifact: if px[0..639] = 0x0000, first row is black (overrun). */
    LOG_DEBUG(TAG_CAM,
              "buf[0..7]=%02X%02X %02X%02X %02X%02X %02X%02X (px[0]=0x%04X)",
              (unsigned)buffer[0], (unsigned)buffer[1],
              (unsigned)buffer[2], (unsigned)buffer[3],
              (unsigned)buffer[4], (unsigned)buffer[5],
              (unsigned)buffer[6], (unsigned)buffer[7],
              (unsigned)(((uint16_t)buffer[0] << 8) | buffer[1]));

    LOG_INFO(TAG_CAM, "Captured in %lums: %lu bytes (%lu frames)",
             (unsigned long)(HAL_GetTick() - start_tick),
             (unsigned long)copy_size,
             (unsigned long)s_frame_count);

#if CAMERA_DIAG_ENABLED
    /* ── Raw Buffer Diagnostic ──
     * Dump first 32 bytes and count zero vs non-zero pixels
     * to verify the DMA actually transferred real image data. */
    LOG_INFO(TAG_CAM, "RAW[0..31]: %02X %02X %02X %02X %02X %02X %02X %02X "
             "%02X %02X %02X %02X %02X %02X %02X %02X "
             "%02X %02X %02X %02X %02X %02X %02X %02X "
             "%02X %02X %02X %02X %02X %02X %02X %02X",
             buffer[0], buffer[1], buffer[2], buffer[3],
             buffer[4], buffer[5], buffer[6], buffer[7],
             buffer[8], buffer[9], buffer[10], buffer[11],
             buffer[12], buffer[13], buffer[14], buffer[15],
             buffer[16], buffer[17], buffer[18], buffer[19],
             buffer[20], buffer[21], buffer[22], buffer[23],
             buffer[24], buffer[25], buffer[26], buffer[27],
             buffer[28], buffer[29], buffer[30], buffer[31]);

    /* Count zero vs non-zero uint16 pixels in first 1000 pixels */
    uint32_t zero_count = 0, nonzero_count = 0;
    uint16_t *px = (uint16_t *)buffer;
    uint32_t check_pixels = (copy_size / 2 < 1000) ? (copy_size / 2) : 1000;
    for (uint32_t i = 0; i < check_pixels; i++) {
        if (px[i] == 0) zero_count++;
        else nonzero_count++;
    }
    LOG_INFO(TAG_CAM, "Pixel check (first %lu): zero=%lu nonzero=%lu px[0]=0x%04X px[500]=0x%04X",
             (unsigned long)check_pixels,
             (unsigned long)zero_count, (unsigned long)nonzero_count,
             (unsigned)px[0], (unsigned)px[500]);
#endif /* CAMERA_DIAG_ENABLED */

    return CAMERA_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Warm Capture — single snapshot, no DCMI/sensor re-init
 *
 *  Captures exactly 1 frame. Reconverges AEC first (2026-08-21 — the
 *  sensor's own AEC loop runs continuously in the background regardless of
 *  DCMI state, so this is normally a fast poll of an already-settled
 *  register, not a fresh multi-frame convergence), then arms DCMI for one
 *  snapshot. No fixed-VTS latency claim here anymore — actual timing comes
 *  from ST's stock OV5640_Common[] table.
 * ═══════════════════════════════════════════════════════════════════════════ */

CameraStatus_t Camera_WarmCapture(uint8_t *buffer, uint32_t buffer_size,
                                   uint32_t *captured_size)
{
    uint32_t perf_start = HAL_GetTick();
    (void)perf_start;

    LOG_INFO(TAG_CAM, "Warm capture (single frame, no DCMI warmup)...");

    if (buffer == NULL || captured_size == NULL)
    {
        LOG_ERROR(TAG_CAM, "Null buffer or size pointer");
        return CAMERA_ERROR_CAPTURE;
    }

    if (buffer_size < CAMERA_FRAME_BUFFER_SIZE)
    {
        LOG_ERROR(TAG_CAM, "Buffer too small (%lu < %lu)",
                  (unsigned long)buffer_size,
                  (unsigned long)CAMERA_FRAME_BUFFER_SIZE);
        return CAMERA_ERROR_CAPTURE;
    }

    /* 2026-08-21: reconverge AEC before EVERY capture, not just once at
     * cold Camera_Init(). This is the dominant fix for wrong exposure —
     * previously AEC/AWB converged once at boot and every subsequent
     * WarmCapture (the path used for all normal scheduled captures, see
     * main.c) reused whatever settled at that first scene forever, even
     * as lighting changed or the board was repositioned. The sensor's own
     * AEC loop runs continuously in the background regardless of DCMI
     * state (BSP_CAMERA_Init leaves it streaming), so this is usually a
     * fast poll of an already-settled register, not a fresh convergence —
     * see _wait_aec_converge(). */
    _ensure_sensor_streaming();
    _wait_aec_converge();

    /* ── Enterprise Retry Loop ──────────────────────────────────────
     * The DCMI/sensor can miss a snapshot trigger after extended idle.
     * Retry up to CAMERA_WARM_CAPTURE_RETRIES times, stopping and
     * restarting the DCMI between attempts to re-arm the hardware. */

    const uint32_t MAX_ATTEMPTS = CAMERA_WARM_CAPTURE_RETRIES;
    const uint32_t WARM_TIMEOUT_MS = 1000;  /* 1s per attempt */

    for (uint32_t attempt = 1; attempt <= MAX_ATTEMPTS; attempt++)
    {
        /* Reset frame counter for this attempt */
        s_frame_count = 0;
        s_frame_size  = 0;
        s_active_buffer = buffer;
        s_continuous_mode = 0;  /* Snapshot mode — ISR must NOT call suspend */

        /* Turn ON tally light (Red LED) */
        BSP_LED_On(LED_RED);

        /* Defensive: ensure DCMI is fully stopped before starting.
         * Recovers from any previous abnormal suspend/stale DMA state. */
        BSP_CAMERA_Stop(0);

        int32_t ret = BSP_CAMERA_Start(0, buffer, CAMERA_MODE_SNAPSHOT);
        if (ret != BSP_ERROR_NONE)
        {
            LOG_ERROR(TAG_CAM, "BSP_CAMERA_Start failed (err=%ld, attempt %lu/%lu)",
                      (long)ret, (unsigned long)attempt, (unsigned long)MAX_ATTEMPTS);
            BSP_LED_Off(LED_RED);
            s_active_buffer = NULL;

            if (attempt < MAX_ATTEMPTS)
            {
                HAL_Delay(50);
                continue;
            }
            return CAMERA_ERROR_CAPTURE;
        }

        /* Fix: HAL_DCMI_Start_DMA (double-buffer path, Length > 0xFFFF) only
         * enables DCMI_IT_FRAME when XferCount reaches 0 — after ALL DMA nodes
         * are exhausted.  For JPEG snapshot the actual frame (~30-80 KB) is far
         * smaller than the 614 KB DMA window, so XferCount never hits 0 and
         * FRAME IT is never armed.  The VSYNC falling edge that ends the frame
         * is silently discarded → s_frame_count stays 0 → 1-second timeout × 3.
         * Fix: enable DCMI_IT_FRAME explicitly right after DMA is started. */
        DCMI->IER |= DCMI_IT_FRAME;

        /* ── DCMI diagnostic: sample SR/CR after first frame starts ── */
        if (attempt == 1)
        {
            HAL_Delay(50);
            LOG_DEBUG(TAG_CAM, "DCMI SR=0x%08lX CR=0x%08lX (snapshot attempt 1)",
                     (unsigned long)DCMI->SR, (unsigned long)DCMI->CR);
        }

        /* SNAPSHOT mode: DCMI captures exactly ONE VSYNC active period then stops.
         * The OV5640 in JPEG mode streams frames continuously from Camera_Init.
         * BSP_CAMERA_Start (snapshot) arms DCMI; the next JPEG frame from the
         * OV5640 (within one frame period, ~83 ms at 12 fps) fills the buffer
         * via GPDMA.  FRAME ISR fires when VSYNC deasserts → s_frame_count = 1. */
        uint32_t start_tick = HAL_GetTick();
        uint8_t  got_frame = 0;

        while ((HAL_GetTick() - start_tick) <= WARM_TIMEOUT_MS)
        {
            if (s_frame_count >= 1)
            {
                /* Fix: drain DCMI FIFO before stopping DMA.
                 * FRAME ISR fires on VSYNC fall — the FIFO may still hold
                 * the last bytes of the JPEG stream, including the EOI
                 * marker (0xFF 0xD9).  HAL_DMA_Abort called immediately
                 * afterwards drops those bytes, so _find_jpeg_size finds
                 * no EOI and returns 0.
                 *
                 * The GPDMA drains the 16-byte DCMI FIFO in << 1 µs, so
                 * this loop is effectively a memory barrier.  5 ms ceiling
                 * guards against any unexpected hardware stall. */
                uint32_t drain_t0 = HAL_GetTick();
                while ((DCMI->SR & DCMI_SR_FNE) != 0U &&
                       (HAL_GetTick() - drain_t0) < 5U) {}
                /* Capture RISR before Stop clears flags.
                 * OVR_RIS (bit1) = FIFO overrun — means PCLK too fast
                 * ERR_RIS (bit2) = sync error — means VSYNC/HSYNC mismatch */
                LOG_DEBUG(TAG_CAM, "DCMI RISR=0x%08lX (OVR=%lu ERR=%lu)",
                          (unsigned long)DCMI->RISR,
                          (unsigned long)((DCMI->RISR >> 1) & 1U),
                          (unsigned long)((DCMI->RISR >> 2) & 1U));
                got_frame = 1;
                break;
            }
            HAL_Delay(1);
        }

        /* Always stop DCMI after each attempt */
        BSP_CAMERA_Stop(0);

        if (got_frame)
        {
            /* ── Success ── */
            BSP_LED_Off(LED_RED);
            s_active_buffer = NULL;

            uint32_t copy_size = s_frame_size;
            if (copy_size == 0)
                copy_size = buffer_size;
            if (copy_size > buffer_size)
                copy_size = buffer_size;

            /* SEC-09: Enterprise Cache Coherency — MUST happen before ANY CPU read of
             * the buffer. DMA writes directly to physical SRAM, bypassing the D-Cache.
             * Without invalidation the CPU reads stale cached data (zeros or last frame).
             *
             * In JPEG mode this must come before _find_jpeg_size() or the EOI scan
             * reads stale cache and always returns 0, causing spurious retries. */
            LL_DCACHE_SetCommand(DCACHE1, LL_DCACHE_COMMAND_INVALIDATE_BY_ADDR);
            LL_DCACHE_SetStartAddress(DCACHE1, (uint32_t)buffer);
            LL_DCACHE_SetEndAddress(DCACHE1, (uint32_t)buffer + copy_size - 1);
            LL_DCACHE_StartCommand(DCACHE1);
            while (LL_DCACHE_IsActiveFlag_BUSYCMD(DCACHE1));

#if CAMERA_JPEG_MODE
            /* Diagnostic: log first 8 bytes so we can see what DMA actually wrote.
             * Expected: FF D8 FF ... (JPEG SOI).  All-zeros = DMA didn't write. */
            /* GPDMA1_Channel12->CDAR = address DMA last wrote to.
             * If CDAR == &buffer[0], DMA transferred 0 bytes → OV5640 output empty.
             * If CDAR > &buffer[0], CDAR - buffer = bytes written so far. */
            LOG_DEBUG(TAG_CAM,
                      "buf[0..7]=%02X%02X %02X%02X %02X%02X %02X%02X "
                      "frame_cnt=%lu CDAR=0x%08lX CBR1_BNDT=%lu",
                      (unsigned)buffer[0], (unsigned)buffer[1],
                      (unsigned)buffer[2], (unsigned)buffer[3],
                      (unsigned)buffer[4], (unsigned)buffer[5],
                      (unsigned)buffer[6], (unsigned)buffer[7],
                      (unsigned long)s_frame_count,
                      (unsigned long)GPDMA1_Channel12->CDAR,
                      (unsigned long)(GPDMA1_Channel12->CBR1 & 0xFFFFU));

            /* Scan for JPEG End-Of-Image marker (0xFF 0xD9) to find actual size.
             * If not found, the capture is incomplete — retry. */
            uint32_t jpeg_size = _find_jpeg_size(buffer, copy_size);
            if (jpeg_size == 0)
            {
                LOG_ERROR(TAG_CAM,
                          "JPEG EOI marker not found (attempt %lu/%lu) — bad capture",
                          (unsigned long)attempt, (unsigned long)MAX_ATTEMPTS);
                BSP_LED_Off(LED_RED);
                s_active_buffer = NULL;
                if (attempt < MAX_ATTEMPTS)
                {
                    HAL_Delay(50);
                    continue;
                }
                return CAMERA_ERROR_CAPTURE;
            }
            copy_size = jpeg_size;
            LOG_INFO(TAG_CAM, "JPEG size: %lu bytes (EOI at offset %lu)",
                     (unsigned long)jpeg_size, (unsigned long)(jpeg_size - 2));
#endif /* CAMERA_JPEG_MODE */

            *captured_size = copy_size;

            LOG_INFO(TAG_CAM, "[PERF] Warm capture: %lums, %lu bytes (attempt %lu/%lu)",
                     (unsigned long)(HAL_GetTick() - perf_start),
                     (unsigned long)copy_size,
                     (unsigned long)attempt, (unsigned long)MAX_ATTEMPTS);

            return CAMERA_OK;
        }

        /* ── Timeout on this attempt ── */
        BSP_LED_Off(LED_RED);
        s_active_buffer = NULL;

        if (attempt < MAX_ATTEMPTS)
        {
            LOG_WARN(TAG_CAM, "Warm capture timeout (attempt %lu/%lu) — retrying...",
                     (unsigned long)attempt, (unsigned long)MAX_ATTEMPTS);
            HAL_Delay(50);  /* Brief settle before DCMI re-arm */
        }
        else
        {
            LOG_ERROR(TAG_CAM, "Warm capture failed after %lu attempts",
                      (unsigned long)MAX_ATTEMPTS);
        }
    }

    return CAMERA_ERROR_TIMEOUT;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  BSP Callbacks (called from ISR context)
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief  Frame Event callback — called by BSP when DCMI frame is complete.
 *         This is a weak function in the BSP that we override here.
 *
 *         In continuous mode, this fires once per frame. We count frames
 *         to know when warm-up is done.
 */
void BSP_CAMERA_FrameEventCallback(uint32_t Instance)
{
    (void)Instance;
    s_frame_count++;

    /* HAL_DCMI_IRQHandler disables DCMI_IT_FRAME after every FRAME ISR.
     * Re-arm it here so subsequent frames keep firing callbacks.
     * In snapshot mode this is a no-op (we stop immediately after s_frame_count=1).
     * In continuous mode (including the 2-frame JPEG warm path) this is essential
     * to receive the second (and later) frame interrupt. */
    DCMI->IER |= DCMI_IT_FRAME;

    /* Suspend DCMI only during continuous captures, and only after all
     * needed frames (warmup + final) are complete. This prevents DMA
     * overrun into the next unwanted frame.
     *
     * CRITICAL: Must NOT fire during snapshot mode — calling Suspend on
     * a snapshot-mode DCMI corrupts the peripheral state machine. */
    if (s_continuous_mode && s_frame_count == (CAMERA_WARMUP_FRAMES + 1))
    {
        BSP_CAMERA_Suspend(0);
    }

#if CAMERA_JPEG_MODE
    /* JPEG mode: actual frame size is variable; determined post-capture by
     * scanning for the 0xFF 0xD9 EOI marker. Set to 0 so callers fall back
     * to scanning the full buffer. */
    s_frame_size = 0;
#else
    /* RGB565: always a fixed, known size */
    s_frame_size = CAMERA_FRAME_BUFFER_SIZE;
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Shutdown
 * ═══════════════════════════════════════════════════════════════════════════ */

CameraStatus_t Camera_DeInit(void)
{
    LOG_INFO(TAG_CAM, "De-initializing camera");
    int32_t ret = BSP_CAMERA_Stop(0);
    if (ret != BSP_ERROR_NONE) {
        LOG_ERROR(TAG_CAM, "BSP_CAMERA_Stop failed (err=%ld)", (long)ret);
    }
    
    ret = BSP_CAMERA_DeInit(0);
    if (ret != BSP_ERROR_NONE) {
        LOG_ERROR(TAG_CAM, "BSP_CAMERA_DeInit failed (err=%ld)", (long)ret);
        return CAMERA_ERROR_INIT;
    }
    
    s_initialized = 0;
    return CAMERA_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Upload Preparation — software JPEG compression
 *
 *  A VGA RGB565 frame is 614,400 bytes on the wire. Over the 4G uplink that
 *  is the single dominant cost of a capture (and the reason the chunked
 *  resumable upload path exists at all). Compressing it on-board to a few
 *  tens of KB with a baseline JPEG encoder cuts the transfer by ~10-60x.
 *
 *  RAM: one static output buffer, sized so that a frame that would not
 *  compress well (sensor noise, pathological texture) simply fails the
 *  encode and falls back to the raw path rather than growing the buffer.
 *  The server distinguishes the two by exact payload size — 614400 (or
 *  153600 for QVGA) means raw RGB565, anything else is treated as an
 *  already-encoded image — so the fallback needs no server change.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define JPEG_OUT_BUF_SIZE       (56 * 1024)
#define CAMERA_RGB565_VGA_SIZE  (640u * 480u * 2u)   /* 614400 — the server's raw marker */

static uint8_t s_jpeg_buf[JPEG_OUT_BUF_SIZE];

void Camera_PrepareUpload(uint8_t *frame, uint32_t frame_len,
                          const uint8_t **out_data, uint32_t *out_len)
{
    if (out_data == NULL || out_len == NULL)
        return;

    /* Default: raw passthrough. Every early return below leaves this in
     * place, so a failed encode can never lose the image. */
    *out_data = frame;
    *out_len  = frame_len;

    if (frame == NULL || frame_len != CAMERA_RGB565_VGA_SIZE)
        return;   /* QVGA, hardware-JPEG mode or a short capture — send as-is */

    uint32_t jpeg_len = JPEG_EncodeRGB565(frame, 640, 480,
                                          s_jpeg_buf, JPEG_OUT_BUF_SIZE);
    if (jpeg_len == 0)
    {
        LOG_WARN(TAG_CAM, "JPEG encode failed or exceeded %d bytes — "
                 "uploading raw RGB565 (%lu bytes)",
                 (int)JPEG_OUT_BUF_SIZE, (unsigned long)frame_len);
        return;
    }

    LOG_INFO(TAG_CAM, "JPEG encoded: %lu -> %lu bytes (q%d)",
             (unsigned long)frame_len, (unsigned long)jpeg_len,
             (int)JPEG_ENCODE_QUALITY);

    *out_data = s_jpeg_buf;
    *out_len  = jpeg_len;
}

void Camera_ClearUploadBuffer(void)
{
    secure_erase(s_jpeg_buf, sizeof(s_jpeg_buf));
}
