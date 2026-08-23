/**
 * @file    jpeg_encode.h
 * @brief   Baseline JPEG encoder for RGB565 frames (no heap, no libm)
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Compresses a raw RGB565 frame (as produced by the DCMI/OV5640 path) into a
 * baseline JFIF JPEG, 4:2:0 chroma subsampled, so the board uploads ~40-60 KB
 * instead of 614 KB per VGA capture.
 *
 * Design constraints (STM32U585, 768 KB SRAM, ~70 KB free):
 *   - No malloc, no heap, no libm, no float printf.
 *   - Encoder static state is ~2.2 KB (derived Huffman + scaled quant tables).
 *   - Stack usage of JPEG_EncodeRGB565() is < 2.5 KB (one 16x16 MCU worth of
 *     float working blocks) — well inside the 4 KB budget.
 *   - Output is written strictly sequentially into the caller's buffer and the
 *     writer refuses to ever step past out_capacity (returns 0 instead).
 *
 * Colour: the RGB565 unpacking here MUST stay bit-identical to the server's
 * fallback path in server/app/api/routes.py (_process_uploaded_image), which
 * does `np.frombuffer(content, dtype=np.uint16)` on a little-endian host:
 *   - byte[0] is the LOW byte of the pixel, byte[1] the HIGH byte
 *   - bits[15:11] = Red, bits[10:5] = Green, bits[4:0] = Blue
 *   - 8-bit expansion by integer floor: r8 = r5*255/31, g8 = g6*255/63,
 *     b8 = b5*255/31
 */

#ifndef __JPEG_ENCODE_H
#define __JPEG_ENCODE_H

#include <stdint.h>

/* IJG-style quality (1..100). 87 ≈ visually lossless for VLM analysis while
 * keeping a VGA frame in the 40-60 KB range. */
/* 2026-08-23: 87 -> 62. Total bytes is the other half of the upload-speed
 * problem: at q87 a capture is 26-41KB = 15-20 chunks, and on this link
 * most chunks stall at least once. Fewer bytes = fewer chunks = fewer
 * chances to hit the loss. q62 is still comfortably good enough for VLM
 * scene analysis. Does NOT affect any thesis benchmark: those score the
 * pre-curated MIT Indoor Scenes corpus, never board captures. */
#define JPEG_ENCODE_QUALITY   62

/**
 * @brief  Encode an RGB565 frame as a baseline 4:2:0 JPEG.
 *
 * @param  rgb565        Source frame, 2 bytes/pixel, low byte first.
 * @param  width         Frame width in pixels (>0).
 * @param  height        Frame height in pixels (>0).
 * @param  out           Destination buffer for the JPEG bitstream.
 * @param  out_capacity  Size of @p out in bytes.
 *
 * @retval JPEG size in bytes written to @p out, or 0 if the encode failed or
 *         would not fit (the buffer is never overrun; on failure its contents
 *         are undefined but bounded by out_capacity).
 */
uint32_t JPEG_EncodeRGB565(const uint8_t *rgb565, uint32_t width, uint32_t height,
                           uint8_t *out, uint32_t out_capacity);

#endif /* __JPEG_ENCODE_H */
