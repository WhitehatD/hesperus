/**
 * @file    jpeg_encode.c
 * @brief   Compact baseline JPEG encoder (4:2:0) for RGB565 frames
 * @author  Alexandru-Ionut Cioc
 * @date    2026
 *
 * Why hand-rolled instead of vendoring tiny_jpeg.h / stb_image_write:
 *   every single-header encoder in circulation takes an RGB888 or RGBA8888
 *   source buffer. On this board that would mean 640*480*3 = 921 KB of extra
 *   SRAM we simply do not have (768 KB total, ~70 KB free after the 614 KB
 *   DCMI frame buffer). This encoder reads RGB565 *directly*, one 16x16 MCU at
 *   a time, so the only working memory is a handful of float blocks on the
 *   stack. Everything else is the standard, well-trodden baseline pipeline.
 *
 * Pipeline (ITU-T T.81 / JFIF):
 *   RGB565 -> RGB888 (server-identical unpack) -> YCbCr (JFIF/BT.601)
 *   -> 4:2:0 chroma box-average -> level shift -> AAN float FDCT
 *   -> quantisation (Annex K tables scaled to quality) -> zig-zag
 *   -> run-length + Huffman (Annex K standard tables) -> byte-stuffed bitstream
 *
 * Spec references (all constants below are straight from the standard):
 *   - T.81 Annex K.1  : example luminance/chrominance quantisation tables
 *   - T.81 Annex K.3  : typical Huffman tables for baseline DCT
 *   - T.81 Figure A.6 : zig-zag scan order
 *   - T.81 Table F.1  : DC/AC magnitude categories (SSSS)
 *   - AAN FDCT after Arai/Agui/Nakajima, as used by libjpeg's jfdctflt.c;
 *     the 1/8 and per-row/column scale factors are folded into the
 *     precomputed quantisation divisors (see _build_tables).
 *
 * Colour byte order is dictated by the server (see jpeg_encode.h header
 * comment): byte[0] = LOW byte, bits[15:11]=R, bits[10:5]=G, bits[4:0]=B.
 */

#include "jpeg_encode.h"

#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Standard tables (T.81 Annex K)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* K.1 — luminance quantisation table (natural, row-major order) */
static const uint8_t k_qt_luma[64] = {
    16, 11, 10, 16,  24,  40,  51,  61,
    12, 12, 14, 19,  26,  58,  60,  55,
    14, 13, 16, 24,  40,  57,  69,  56,
    14, 17, 22, 29,  51,  87,  80,  62,
    18, 22, 37, 56,  68, 109, 103,  77,
    24, 35, 55, 64,  81, 104, 113,  92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103,  99
};

/* K.1 — chrominance quantisation table (natural, row-major order) */
static const uint8_t k_qt_chroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99
};

/* Figure A.6 — zig-zag order: k_zigzag[k] = natural index of the k-th
 * coefficient in the zig-zag sequence. */
static const uint8_t k_zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* K.3 — DC luminance: BITS[1..16] then HUFFVAL */
static const uint8_t k_dc_luma_bits[17] = {
    0, 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0
};
static const uint8_t k_dc_luma_vals[12] = {
    0,1,2,3,4,5,6,7,8,9,10,11
};

/* K.3 — DC chrominance */
static const uint8_t k_dc_chroma_bits[17] = {
    0, 0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0
};
static const uint8_t k_dc_chroma_vals[12] = {
    0,1,2,3,4,5,6,7,8,9,10,11
};

/* K.3 — AC luminance */
static const uint8_t k_ac_luma_bits[17] = {
    0, 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7D
};
static const uint8_t k_ac_luma_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
    0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08,
    0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16,
    0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
    0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
    0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5,
    0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4,
    0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2,
    0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA,
    0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA
};

/* K.3 — AC chrominance */
static const uint8_t k_ac_chroma_bits[17] = {
    0, 0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77
};
static const uint8_t k_ac_chroma_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
    0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0,
    0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34,
    0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26,
    0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96,
    0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5,
    0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
    0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3,
    0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2,
    0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
    0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
    0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA
};

/* AAN scale factors: cos(k*pi/16) * sqrt(2) for k=1..7, 1.0 for k=0. */
static const float k_aan_scale[8] = {
    1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
    1.0f, 0.785694958f, 0.541196100f, 0.275899379f
};

/* ═══════════════════════════════════════════════════════════════════════════
 *  Derived tables (built once, ~2.2 KB of .bss)
 * ═══════════════════════════════════════════════════════════════════════════ */

static uint8_t  s_qtab[2][64];       /* quality-scaled quant tables, natural order */
static float    s_qdiv[2][64];       /* 1 / (qtab * aan_row * aan_col * 8) */
static uint16_t s_dc_code[2][12];
static uint8_t  s_dc_size[2][12];
static uint16_t s_ac_code[2][256];
static uint8_t  s_ac_size[2][256];
static uint8_t  s_tables_ready = 0;

/**
 * Build canonical Huffman codes from a BITS/HUFFVAL pair (T.81 Annex C,
 * procedures "Generate_size_table" + "Generate_code_table"), storing them
 * indexed by symbol value so encoding is a direct lookup.
 */
static void _build_huff(const uint8_t *bits, const uint8_t *vals,
                        uint16_t *code_out, uint8_t *size_out, uint32_t table_len)
{
    uint16_t code = 0;
    uint32_t k = 0;

    memset(code_out, 0, table_len * sizeof(uint16_t));
    memset(size_out, 0, table_len * sizeof(uint8_t));

    for (uint32_t len = 1; len <= 16; len++)
    {
        for (uint32_t i = 0; i < bits[len]; i++)
        {
            uint8_t sym = vals[k++];
            code_out[sym] = code;
            size_out[sym] = (uint8_t)len;
            code++;
        }
        code = (uint16_t)(code << 1);
    }
}

/**
 * Scale the Annex K quantisation tables to the requested quality using the
 * classic IJG mapping, then fold the AAN FDCT output scaling into a
 * reciprocal so the inner loop is a multiply instead of a divide.
 */
static void _build_tables(int quality)
{
    int scale;

    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;

    /* IJG jpeg_quality_scaling() */
    scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);

    for (int i = 0; i < 64; i++)
    {
        int lv = ((int)k_qt_luma[i]   * scale + 50) / 100;
        int cv = ((int)k_qt_chroma[i] * scale + 50) / 100;

        if (lv < 1)   lv = 1;
        if (lv > 255) lv = 255;   /* baseline: 8-bit precision only */
        if (cv < 1)   cv = 1;
        if (cv > 255) cv = 255;

        s_qtab[0][i] = (uint8_t)lv;
        s_qtab[1][i] = (uint8_t)cv;
    }

    for (int row = 0; row < 8; row++)
    {
        for (int col = 0; col < 8; col++)
        {
            int i = row * 8 + col;
            float aan = k_aan_scale[row] * k_aan_scale[col] * 8.0f;
            s_qdiv[0][i] = 1.0f / ((float)s_qtab[0][i] * aan);
            s_qdiv[1][i] = 1.0f / ((float)s_qtab[1][i] * aan);
        }
    }

    _build_huff(k_dc_luma_bits,   k_dc_luma_vals,   s_dc_code[0], s_dc_size[0], 12);
    _build_huff(k_dc_chroma_bits, k_dc_chroma_vals, s_dc_code[1], s_dc_size[1], 12);
    _build_huff(k_ac_luma_bits,   k_ac_luma_vals,   s_ac_code[0], s_ac_size[0], 256);
    _build_huff(k_ac_chroma_bits, k_ac_chroma_vals, s_ac_code[1], s_ac_size[1], 256);

    s_tables_ready = 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Bounded output writer — never writes past capacity
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t *out;
    uint32_t cap;
    uint32_t len;
    uint32_t bitbuf;    /* pending bits, MSB-aligned */
    int      bitcnt;    /* number of pending bits (0..31) */
    int      overflow;  /* sticky: set once the output would exceed cap */
} jpeg_writer_t;

static void _put_byte(jpeg_writer_t *w, uint8_t b)
{
    if (w->overflow)
        return;
    if (w->len >= w->cap)
    {
        w->overflow = 1;
        return;
    }
    w->out[w->len++] = b;
}

static void _put_u16(jpeg_writer_t *w, uint16_t v)
{
    _put_byte(w, (uint8_t)(v >> 8));
    _put_byte(w, (uint8_t)(v & 0xFF));
}

static void _put_marker(jpeg_writer_t *w, uint8_t marker)
{
    _put_byte(w, 0xFF);
    _put_byte(w, marker);
}

/** Emit `size` bits (LSB-justified in `code`), MSB first, with 0xFF stuffing. */
static void _put_bits(jpeg_writer_t *w, uint16_t code, uint8_t size)
{
    if (size == 0)
        return;

    w->bitcnt += size;
    w->bitbuf |= ((uint32_t)code) << (32 - w->bitcnt);

    while (w->bitcnt >= 8)
    {
        uint8_t b = (uint8_t)(w->bitbuf >> 24);
        _put_byte(w, b);
        if (b == 0xFF)
            _put_byte(w, 0x00);   /* byte stuffing, T.81 F.1.2.3 */
        w->bitbuf <<= 8;
        w->bitcnt  -= 8;
    }
}

/** Pad the final partial byte with 1-bits (T.81 F.1.2.3).
 *
 * Pad ONCE, by exactly the number of bits missing. The obvious
 * `while (bitcnt > 0) _put_bits(w, 0x7F, 7);` is wrong: each call adds 7
 * bits while the drain removes 8, so bitcnt falls by only 1 per iteration
 * and the loop runs bitcnt times. Iterations after the first emit whole
 * 0xFF bytes, each of which then trips the 0xFF byte-stuffing below and
 * appends a 0x00 — up to 13 junk bytes between the last MCU and EOI
 * (harmless to libjpeg/PIL, but wrong and wasteful).
 *
 * The low (8 - bitcnt) bits of 0x7F are all 1s, which is exactly the
 * required pad, and it lands the buffer on a byte boundary in one step. */
static void _flush_bits(jpeg_writer_t *w)
{
    if (w->bitcnt > 0)
        _put_bits(w, 0x7F, (uint8_t)(8 - w->bitcnt));
    w->bitbuf = 0;
    w->bitcnt = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  AAN float forward DCT (in place, natural order)
 *  After Arai/Agui/Nakajima; identical structure to libjpeg jfdctflt.c.
 *  Output is scaled up by 8 * aan_row * aan_col — undone by s_qdiv.
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _fdct(float *data)
{
    float tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
    float tmp10, tmp11, tmp12, tmp13;
    float z1, z2, z3, z4, z5, z11, z13;
    float *p;

    /* Pass 1: rows */
    p = data;
    for (int ctr = 0; ctr < 8; ctr++, p += 8)
    {
        tmp0 = p[0] + p[7];  tmp7 = p[0] - p[7];
        tmp1 = p[1] + p[6];  tmp6 = p[1] - p[6];
        tmp2 = p[2] + p[5];  tmp5 = p[2] - p[5];
        tmp3 = p[3] + p[4];  tmp4 = p[3] - p[4];

        /* even part */
        tmp10 = tmp0 + tmp3;  tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;  tmp12 = tmp1 - tmp2;

        p[0] = tmp10 + tmp11;
        p[4] = tmp10 - tmp11;

        z1   = (tmp12 + tmp13) * 0.707106781f;
        p[2] = tmp13 + z1;
        p[6] = tmp13 - z1;

        /* odd part */
        tmp10 = tmp4 + tmp5;
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;

        z5 = (tmp10 - tmp12) * 0.382683433f;
        z2 = 0.541196100f * tmp10 + z5;
        z4 = 1.306562965f * tmp12 + z5;
        z3 = tmp11 * 0.707106781f;

        z11 = tmp7 + z3;
        z13 = tmp7 - z3;

        p[5] = z13 + z2;
        p[3] = z13 - z2;
        p[1] = z11 + z4;
        p[7] = z11 - z4;
    }

    /* Pass 2: columns */
    p = data;
    for (int ctr = 0; ctr < 8; ctr++, p++)
    {
        tmp0 = p[0]  + p[56];  tmp7 = p[0]  - p[56];
        tmp1 = p[8]  + p[48];  tmp6 = p[8]  - p[48];
        tmp2 = p[16] + p[40];  tmp5 = p[16] - p[40];
        tmp3 = p[24] + p[32];  tmp4 = p[24] - p[32];

        tmp10 = tmp0 + tmp3;  tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;  tmp12 = tmp1 - tmp2;

        p[0]  = tmp10 + tmp11;
        p[32] = tmp10 - tmp11;

        z1    = (tmp12 + tmp13) * 0.707106781f;
        p[16] = tmp13 + z1;
        p[48] = tmp13 - z1;

        tmp10 = tmp4 + tmp5;
        tmp11 = tmp5 + tmp6;
        tmp12 = tmp6 + tmp7;

        z5 = (tmp10 - tmp12) * 0.382683433f;
        z2 = 0.541196100f * tmp10 + z5;
        z4 = 1.306562965f * tmp12 + z5;
        z3 = tmp11 * 0.707106781f;

        z11 = tmp7 + z3;
        z13 = tmp7 - z3;

        p[40] = z13 + z2;
        p[24] = z13 - z2;
        p[8]  = z11 + z4;
        p[56] = z11 - z4;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Block encoding
 * ═══════════════════════════════════════════════════════════════════════════ */

/** T.81 Table F.1: number of bits needed to encode magnitude of v. */
static int _category(int v)
{
    int a = (v < 0) ? -v : v;
    int n = 0;
    while (a)
    {
        a >>= 1;
        n++;
    }
    return n;
}

/**
 * FDCT + quantise + entropy-code one 8x8 block.
 *
 * @param blk   64 level-shifted samples (natural order), destroyed in place
 * @param tbl   0 = luma tables, 1 = chroma tables
 * @param dc_pred  running DC predictor for this component (in/out)
 */
static void _encode_block(jpeg_writer_t *w, float *blk, int tbl, int *dc_pred)
{
    int coef[64];

    _fdct(blk);

    for (int i = 0; i < 64; i++)
    {
        /* Round-to-nearest without libm: the +16384.5 bias trick keeps the
         * float in a range where truncation equals rounding. */
        float t = blk[i] * s_qdiv[tbl][i] + 16384.5f;
        coef[i] = (int)t - 16384;
    }

    /* ── DC: differential, Huffman category + magnitude bits ── */
    {
        int diff = coef[0] - *dc_pred;
        *dc_pred = coef[0];

        int nbits = _category(diff);
        _put_bits(w, s_dc_code[tbl][nbits], s_dc_size[tbl][nbits]);
        if (nbits)
        {
            if (diff < 0)
                diff += (1 << nbits) - 1;
            _put_bits(w, (uint16_t)(diff & ((1 << nbits) - 1)), (uint8_t)nbits);
        }
    }

    /* ── AC: zig-zag run-length, ZRL for runs of 16, EOB at the end ── */
    {
        int last_nonzero = 0;
        for (int k = 63; k >= 1; k--)
        {
            if (coef[k_zigzag[k]] != 0)
            {
                last_nonzero = k;
                break;
            }
        }

        int run = 0;
        for (int k = 1; k <= last_nonzero; k++)
        {
            int v = coef[k_zigzag[k]];
            if (v == 0)
            {
                run++;
                continue;
            }
            while (run >= 16)
            {
                _put_bits(w, s_ac_code[tbl][0xF0], s_ac_size[tbl][0xF0]);  /* ZRL */
                run -= 16;
            }
            int nbits = _category(v);
            int sym   = (run << 4) | nbits;
            _put_bits(w, s_ac_code[tbl][sym], s_ac_size[tbl][sym]);
            if (v < 0)
                v += (1 << nbits) - 1;
            _put_bits(w, (uint16_t)(v & ((1 << nbits) - 1)), (uint8_t)nbits);
            run = 0;
        }

        if (last_nonzero < 63)
            _put_bits(w, s_ac_code[tbl][0x00], s_ac_size[tbl][0x00]);      /* EOB */
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Headers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void _write_dht(jpeg_writer_t *w, uint8_t class_id,
                       const uint8_t *bits, const uint8_t *vals)
{
    uint16_t nvals = 0;
    for (int i = 1; i <= 16; i++)
        nvals = (uint16_t)(nvals + bits[i]);

    _put_marker(w, 0xC4);                       /* DHT */
    _put_u16(w, (uint16_t)(2 + 1 + 16 + nvals));
    _put_byte(w, class_id);                     /* (class << 4) | id */
    for (int i = 1; i <= 16; i++)
        _put_byte(w, bits[i]);
    for (uint16_t i = 0; i < nvals; i++)
        _put_byte(w, vals[i]);
}

static void _write_headers(jpeg_writer_t *w, uint32_t width, uint32_t height)
{
    /* SOI */
    _put_marker(w, 0xD8);

    /* APP0 / JFIF */
    _put_marker(w, 0xE0);
    _put_u16(w, 16);
    _put_byte(w, 'J'); _put_byte(w, 'F'); _put_byte(w, 'I'); _put_byte(w, 'F');
    _put_byte(w, 0x00);
    _put_byte(w, 1); _put_byte(w, 1);   /* version 1.1 */
    _put_byte(w, 0);                    /* density units: none */
    _put_u16(w, 1); _put_u16(w, 1);     /* X/Y density */
    _put_byte(w, 0); _put_byte(w, 0);   /* no thumbnail */

    /* DQT — both tables, values in zig-zag order */
    _put_marker(w, 0xDB);
    _put_u16(w, 2 + 2 * (1 + 64));
    _put_byte(w, 0x00);                 /* 8-bit precision, table 0 */
    for (int i = 0; i < 64; i++)
        _put_byte(w, s_qtab[0][k_zigzag[i]]);
    _put_byte(w, 0x01);                 /* 8-bit precision, table 1 */
    for (int i = 0; i < 64; i++)
        _put_byte(w, s_qtab[1][k_zigzag[i]]);

    /* SOF0 — baseline, 3 components, 4:2:0 (Y = 2x2, Cb/Cr = 1x1) */
    _put_marker(w, 0xC0);
    _put_u16(w, 8 + 3 * 3);
    _put_byte(w, 8);                    /* sample precision */
    _put_u16(w, (uint16_t)height);
    _put_u16(w, (uint16_t)width);
    _put_byte(w, 3);                    /* component count */
    _put_byte(w, 1); _put_byte(w, 0x22); _put_byte(w, 0);   /* Y  */
    _put_byte(w, 2); _put_byte(w, 0x11); _put_byte(w, 1);   /* Cb */
    _put_byte(w, 3); _put_byte(w, 0x11); _put_byte(w, 1);   /* Cr */

    /* DHT × 4 */
    _write_dht(w, 0x00, k_dc_luma_bits,   k_dc_luma_vals);
    _write_dht(w, 0x10, k_ac_luma_bits,   k_ac_luma_vals);
    _write_dht(w, 0x01, k_dc_chroma_bits, k_dc_chroma_vals);
    _write_dht(w, 0x11, k_ac_chroma_bits, k_ac_chroma_vals);

    /* SOS */
    _put_marker(w, 0xDA);
    _put_u16(w, 6 + 2 * 3);
    _put_byte(w, 3);
    _put_byte(w, 1); _put_byte(w, 0x00);   /* Y  : DC0 / AC0 */
    _put_byte(w, 2); _put_byte(w, 0x11);   /* Cb : DC1 / AC1 */
    _put_byte(w, 3); _put_byte(w, 0x11);   /* Cr : DC1 / AC1 */
    _put_byte(w, 0);                       /* Ss */
    _put_byte(w, 63);                      /* Se */
    _put_byte(w, 0);                       /* Ah/Al */
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  MCU loader — RGB565 -> YCbCr, level shifted, 4:2:0
 * ═══════════════════════════════════════════════════════════════════════════ */

/**
 * Read one 16x16 MCU straight out of the RGB565 frame.
 *
 * Edge pixels are replicated when the frame is not a multiple of 16 (VGA is,
 * so this is defensive only). Chroma is a 2x2 box average, which is what
 * every standard 4:2:0 encoder does.
 */
static void _load_mcu(const uint8_t *src, uint32_t w_pix, uint32_t h_pix,
                      uint32_t mx, uint32_t my,
                      float y[4][64], float cb[64], float cr[64])
{
    for (int i = 0; i < 64; i++)
    {
        cb[i] = 0.0f;
        cr[i] = 0.0f;
    }

    for (uint32_t r = 0; r < 16; r++)
    {
        uint32_t sy = my * 16 + r;
        if (sy >= h_pix)
            sy = h_pix - 1;

        const uint8_t *row = src + (size_t)sy * (size_t)w_pix * 2u;

        for (uint32_t c = 0; c < 16; c++)
        {
            uint32_t sx = mx * 16 + c;
            if (sx >= w_pix)
                sx = w_pix - 1;

            const uint8_t *p = row + (size_t)sx * 2u;

            /* Server-identical unpack: byte[0] = LOW byte of the LE uint16,
             * bits[15:11]=R, bits[10:5]=G, bits[4:0]=B, floor-scaled to 8 bit. */
            uint32_t px = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
            int R = (int)((((px >> 11) & 0x1Fu) * 255u) / 31u);
            int G = (int)((((px >>  5) & 0x3Fu) * 255u) / 63u);
            int B = (int)((( px        & 0x1Fu) * 255u) / 31u);

            float fr = (float)R, fg = (float)G, fb = (float)B;

            /* JFIF / BT.601 full-range YCbCr, already level shifted by -128 */
            float yy  =  0.299000f * fr + 0.587000f * fg + 0.114000f * fb - 128.0f;
            float fcb = -0.168736f * fr - 0.331264f * fg + 0.500000f * fb;
            float fcr =  0.500000f * fr - 0.418688f * fg - 0.081312f * fb;

            int blk = (int)((r >> 3) * 2u + (c >> 3));
            int idx = (int)((r & 7u) * 8u + (c & 7u));
            y[blk][idx] = yy;

            int cidx = (int)((r >> 1) * 8u + (c >> 1));
            cb[cidx] += fcb;
            cr[cidx] += fcr;
        }
    }

    for (int i = 0; i < 64; i++)
    {
        cb[i] *= 0.25f;
        cr[i] *= 0.25f;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

uint32_t JPEG_EncodeRGB565(const uint8_t *rgb565, uint32_t width, uint32_t height,
                           uint8_t *out, uint32_t out_capacity)
{
    jpeg_writer_t w;

    if (rgb565 == NULL || out == NULL)
        return 0;
    if (width == 0 || height == 0 || width > 65535u || height > 65535u)
        return 0;
    /* Headers alone are ~630 bytes; refuse anything that obviously cannot hold
     * even a trivially small image rather than half-filling the buffer. */
    if (out_capacity < 1024u)
        return 0;

    if (!s_tables_ready)
        _build_tables(JPEG_ENCODE_QUALITY);

    w.out      = out;
    w.cap      = out_capacity;
    w.len      = 0;
    w.bitbuf   = 0;
    w.bitcnt   = 0;
    w.overflow = 0;

    _write_headers(&w, width, height);

    int dc_pred[3] = { 0, 0, 0 };

    const uint32_t mcus_x = (width  + 15u) / 16u;
    const uint32_t mcus_y = (height + 15u) / 16u;

    /* ~1.5 KB of stack: 4 luma blocks + 2 chroma blocks of floats. */
    float y[4][64];
    float cb[64];
    float cr[64];

    for (uint32_t my = 0; my < mcus_y; my++)
    {
        for (uint32_t mx = 0; mx < mcus_x; mx++)
        {
            _load_mcu(rgb565, width, height, mx, my, y, cb, cr);

            _encode_block(&w, y[0], 0, &dc_pred[0]);
            _encode_block(&w, y[1], 0, &dc_pred[0]);
            _encode_block(&w, y[2], 0, &dc_pred[0]);
            _encode_block(&w, y[3], 0, &dc_pred[0]);
            _encode_block(&w, cb,   1, &dc_pred[1]);
            _encode_block(&w, cr,   1, &dc_pred[2]);
        }

        /* Bail out early on overflow — no point encoding the rest of a frame
         * that will be discarded, and it keeps the failure cheap. */
        if (w.overflow)
            return 0;
    }

    _flush_bits(&w);
    _put_marker(&w, 0xD9);   /* EOI */

    if (w.overflow)
        return 0;

    return w.len;
}
