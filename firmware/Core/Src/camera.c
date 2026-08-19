/**
 * @file    camera.c
 * @brief   OV5640 Camera Capture via BSP DCMI Interface — Optimized
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Wraps the BSP_CAMERA functions for the MB1379 camera module (OV5640 sensor)
 * on the B-U585I-IOT02A Discovery Kit.
 *
 * Enterprise optimizations applied:
 *   - Adaptive AEC polling (replaces fixed 2s delay)
 *   - Continuous-mode warm-up (replaces 6× snapshot Start/Stop loop)
 *   - Lower VTS for ~12fps frame rate (83ms/frame vs 330ms)
 *   - PCLK boost for faster pixel throughput
 *   - Compile-time SRAM budget validation
 *   - Diagnostics gated behind CAMERA_DIAG_ENABLED flag
 *
 * Capture flow:
 *   1. Camera_Init() — power up sensor, configure resolution + register tuning
 *   2. Camera_CaptureFrame() — continuous-mode warm-up + final frame capture
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
     * Strategy: Keep AEC in AUTO mode (let the ISP converge naturally),
     * but raise every ceiling it's allowed to reach + optimize frame rate.
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
    val = 0x07;  /* 0x06 | bit0 — vertical binning ON, orientation preserved */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3820, &val, 1);
    val = 0x01;  /* bit0 only — horizontal binning ON, no mirror */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3821, &val, 1);

    /* 1. AEC Gain Ceiling — 4x, NOT 64x.
     *
     * Previously 0x3A18/0x3A19 = 0x03/0xFF = a 64x gain ceiling. For a
     * STATIC monitoring scene that's actively harmful: the AEC loop raises
     * exposure toward max_expo FIRST and only reaches for gain once that's
     * maxed, so a high ceiling just licenses the ISP to multiply noise.
     * Analog gain is pure noise amplification; a static scene can afford
     * long exposure instead, which is nearly free.
     *
     * With VTS=0x07D0 (83ms base) and night mode extending 4x (~333ms), a
     * 4x gain ceiling is ample. Raise to 0x78 (7.5x) only if real scenes
     * come out underexposed. (Linux's general-purpose VGA default is
     * 0x3A19=0xF8 = 15.5x — still 4x below what we had.) */
    val = 0x00;
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A18, &val, 1);
    val = 0x40;  /* 0x040/16 = 4x ceiling (was 0xFF = 64x) */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A19, &val, 1);

    /* 2. AEC target brightness — narrowed stable band.
     *
     * Previous values (0x78/0x68/0xD0/0x78) were aggressive vs OmniVision
     * defaults (0x30/0x28/0x30/0x26), and the stable-out band was very wide
     * (0x3A1B=0xD0 vs 0x3A1E=0x78) — AEC could settle far from target and
     * stay there, giving inconsistent exposure across a monitoring series.
     * Narrowed so it converges closer to target and holds it. */
    val = 0x50;  /* Stable-in high (was 0x78) */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A0F, &val, 1);
    val = 0x40;  /* Stable-in low  (was 0x68) */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A10, &val, 1);
    val = 0x58;  /* Stable-out high (was 0xD0) */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A1B, &val, 1);
    val = 0x38;  /* Stable-out low  (was 0x78) */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A1E, &val, 1);
    val = 0x80;  /* Max gain for stable range (default 0x60) */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A11, &val, 1);

    /* 3. Raise max exposure lines for both 50/60Hz bands */
    val = (uint8_t)((CAMERA_VTS_DEFAULT >> 8) & 0xFF);
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A02, &val, 1);  /* MAX_EXPO 60Hz HIGH */
    val = (uint8_t)(CAMERA_VTS_DEFAULT & 0xFF);
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A03, &val, 1);  /* MAX_EXPO 60Hz LOW  */
    val = (uint8_t)((CAMERA_VTS_DEFAULT >> 8) & 0xFF);
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A14, &val, 1);  /* MAX_EXPO 50Hz HIGH */
    val = (uint8_t)(CAMERA_VTS_DEFAULT & 0xFF);
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A15, &val, 1);  /* MAX_EXPO 50Hz LOW  */

    /* 4. Set VTS (Vertical Total Size) for balanced frame rate + exposure.
     *    VTS=0x07D0 (2000 lines) → ~12fps at VGA, 83ms max base exposure.
     *    Night mode extends this to 4× (8000 lines, ~300ms) in low light. */
    val = (uint8_t)((CAMERA_VTS_DEFAULT >> 8) & 0xFF);
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x380E, &val, 1);  /* VTS HIGH */
    val = (uint8_t)(CAMERA_VTS_DEFAULT & 0xFF);
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x380F, &val, 1);  /* VTS LOW  */

    /* 5. Ensure AEC stays in AUTO mode (no manual exposure override) */
    val = 0x00;  /* bit 0=0: AEC auto, bit 1=0: AGC auto */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3503, &val, 1);

    /* 6. Enable AEC night mode with 4x frame insertion ceiling.
     *    Night mode lets the ISP dynamically increase VTS (lower fps) when
     *    the scene is dark, extending max exposure up to 4× the base VTS.
     *    With VTS=2000 and 4x ceiling: max exposure ≈ 8000 lines ≈ 300ms. */
    val = 0x7C;  /* AEC_CTRL00: enable night mode + band filter */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A00, &val, 1);
    val = 0x03;  /* AEC_MAX_EXPO_INSERT: 11 = 4x frame insertion ceiling */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3A05, &val, 1);

    /* 7. PCLK boost — set PLL pre-divider for higher pixel clock.
     *    Register 0x3824 (PCLK divider): lower value = faster PCLK.
     *    Default from BSP init table is usually 0x04 → set to 0x02 for 2× boost. */
    val = 0x02;
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3824, &val, 1);

    /* 8. Sharpen/denoise trim — optimize for genuine detail, not for a
     *    punchy consumer look. ST's CIP block sharpens with thresholds
     *    (0x5300=0x08, 0x5301=0x30, 0x5302=0x10, 0x5303=0x00) but ships NO
     *    denoise override (0x5304-0x5307 sit at silicon defaults).
     *
     *    Oversharpening creates halo/ringing artifacts around edges that a
     *    vision-LLM can misread as real texture or structure — the opposite
     *    of what we want for scene analysis. Halve the sharpen offset and
     *    add mild denoise (kept mild, not disabled: at a 4x gain ceiling
     *    there's little noise to begin with, and killing denoise entirely
     *    lets chroma speckle straight through the JPEG encode). */
    val = 0x08;  /* Sharpen offset1 (was implicit default via CIP enable) */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x5302, &val, 1);
    val = 0x00;
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x5303, &val, 1);
    val = 0x08;  /* Denoise threshold (mild) */
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x5306, &val, 1);
    val = 0x16;
    BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x5307, &val, 1);

    /* ── Adaptive AEC convergence (replaces fixed 2s delay) ── */
    _wait_aec_converge();

    /* 9. Lock AWB after convergence.
     *
     *    For a monitoring SERIES, frame-to-frame colour drift is worse than
     *    a small constant colour error — it makes consecutive captures of
     *    the same static scene look like different lighting to the VLM.
     *    _wait_aec_converge() already waits for AEC (luma) to settle; AWB
     *    needs longer (~10-20 frames from cold vs ~5-10 for AEC), so read
     *    back the converged gains AFTER waiting and freeze them by writing
     *    them back with the manual-AWB bit set. Read-modify-write (not a
     *    fixed table) because correct gains are scene- and light-dependent
     *    and there is no calibrated reference to hardcode. */
    {
        uint8_t awb_gain[6];
        int32_t rd_ok = BSP_ERROR_NONE;
        for (int i = 0; i < 6; i++)
        {
            if (BSP_I2C1_ReadReg16(OV5640_I2C_ADDR, (uint16_t)(0x3400 + i),
                                    &awb_gain[i], 1) != BSP_ERROR_NONE)
            {
                rd_ok = BSP_ERROR_COMPONENT_FAILURE;
                break;
            }
        }
        if (rd_ok == BSP_ERROR_NONE)
        {
            for (int i = 0; i < 6; i++)
            {
                BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, (uint16_t)(0x3400 + i),
                                     &awb_gain[i], 1);
            }
            val = 0x01;  /* 0x3406 bit0 = manual AWB (use frozen gains above) */
            BSP_I2C1_WriteReg16(OV5640_I2C_ADDR, 0x3406, &val, 1);
        }
        /* If the read failed, leave AWB in auto mode rather than lock a
         * garbage/partial gain set. */
    }

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

    LOG_INFO(TAG_CAM, "Camera initialized OK (raw frame size: %lu bytes, VTS=0x%04X)",
             (unsigned long)_raw_frame_size(resolution),
             (unsigned)CAMERA_VTS_DEFAULT);

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
     * AEC is already settled from Camera_Init, so no warmup is needed. */
    LOG_INFO(TAG_CAM, "JPEG mode: delegating to Camera_WarmCapture (single snapshot)");
    return Camera_WarmCapture(buffer, buffer_size, captured_size);
#endif

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

    /* At VTS=0x07D0 (~12fps), each frame takes ~83ms.
     * Total expected: ~83ms × 4 = ~330ms.
     * Timeout: generous 3s to handle low-light slow-down. */
    const uint32_t CAPTURE_TIMEOUT_MS = 3000;

    while (s_frame_count < TOTAL_FRAMES)
    {
        if ((HAL_GetTick() - start_tick) > CAPTURE_TIMEOUT_MS)
        {
            LOG_ERROR(TAG_CAM, "Capture timeout after %lu ms (got %lu/%lu frames)",
                      (unsigned long)CAPTURE_TIMEOUT_MS,
                      (unsigned long)s_frame_count,
                      (unsigned long)TOTAL_FRAMES);
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
 *  Zero-Overhead Warm Capture — Enterprise Fast Path
 *
 *  Captures exactly 1 frame with zero warmup. The sensor is already
 *  AEC-converged from the persistent init, so the first frame is usable.
 *  Expected latency: ~83ms at VTS=0x07D0 (~12fps).
 * ═══════════════════════════════════════════════════════════════════════════ */

CameraStatus_t Camera_WarmCapture(uint8_t *buffer, uint32_t buffer_size,
                                   uint32_t *captured_size)
{
    uint32_t perf_start = HAL_GetTick();
    (void)perf_start;

    LOG_INFO(TAG_CAM, "Warm capture (single frame, zero warmup)...");

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
