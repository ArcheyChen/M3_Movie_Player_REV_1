#include "gbm_decoder.h"
#include <string.h>

#define ROW_BYTES (FRAME_WIDTH * 2)

// Codebook offsets - place in IWRAM data section for fast access (256 bytes)
__attribute__((section(".iwram.data"))) static s16 CODEBOOK_OFFSETS[256];

// ROM source for codebook (will be copied to IWRAM at init)
static const s16 CODEBOOK_OFFSETS_ROM[] = {
    -3856, -3854, -3852, -3850, -3848, -3846, -3844, -3842,
    -3840, -3838, -3836, -3834, -3832, -3830, -3828, -3826,
    -3376, -3374, -3372, -3370, -3368, -3366, -3364, -3362,
    -3360, -3358, -3356, -3354, -3352, -3350, -3348, -3346,
    -2896, -2894, -2892, -2890, -2888, -2886, -2884, -2882,
    -2880, -2878, -2876, -2874, -2872, -2870, -2868, -2866,
    -2416, -2414, -2412, -2410, -2408, -2406, -2404, -2402,
    -2400, -2398, -2396, -2394, -2392, -2390, -2388, -2386,
    -1936, -1934, -1932, -1930, -1928, -1926, -1924, -1922,
    -1920, -1918, -1916, -1914, -1912, -1910, -1908, -1906,
    -1456, -1454, -1452, -1450, -1448, -1446, -1444, -1442,
    -1440, -1438, -1436, -1434, -1432, -1430, -1428, -1426,
    -976, -974, -972, -970, -968, -966, -964, -962,
    -960, -958, -956, -954, -952, -950, -948, -946,
    -496, -494, -492, -490, -488, -486, -484, -482,
    -480, -478, -476, -474, -472, -470, -468, -466,
    -16, -14, -12, -10, -8, -6, -4, -2,
    0, 2, 4, 6, 8, 10, 12, 14,
    464, 466, 468, 470, 472, 474, 476, 478,
    480, 482, 484, 486, 488, 490, 492, 494,
    944, 946, 948, 950, 952, 954, 956, 958,
    960, 962, 964, 966, 968, 970, 972, 974,
    1424, 1426, 1428, 1430, 1432, 1434, 1436, 1438,
    1440, 1442, 1444, 1446, 1448, 1450, 1452, 1454,
    1904, 1906, 1908, 1910, 1912, 1914, 1916, 1918,
    1920, 1922, 1924, 1926, 1928, 1930, 1932, 1934,
    2384, 2386, 2388, 2390, 2392, 2394, 2396, 2398,
    2400, 2402, 2404, 2406, 2408, 2410, 2412, 2414,
    2864, 2866, 2868, 2870, 2872, 2874, 2876, 2878,
    2880, 2882, 2884, 2886, 2888, 2890, 2892, 2894,
    3344, 3346, 3348, 3350, 3352, 3354, 3356, 3358,
    3360, 3362, 3364, 3366, 3368, 3370, 3372, 3374,
};

// Initialize codebook in IWRAM (call once at startup)
void gbm_init(void) {
    for (int i = 0; i < 256; i++) {
        CODEBOOK_OFFSETS[i] = CODEBOOK_OFFSETS_ROM[i];
    }
}

// Inline helpers
static inline u32 read_u32_unaligned(const u8 *ptr) {
    // GBA supports unaligned loads? NO. ARM7TDMI does NOT support unaligned loads correctly (it rotates).
    // We must construct it.
    return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24);
}

static inline u16 read_u16_unaligned(const u8 *ptr) {
    return ptr[0] | (ptr[1] << 8);
}

// Critical Path: next_bit
// Placing in IWRAM, optimized version
static IWRAM_CODE int next_bit(DecodeContext *ctx) {
    u32 state = ctx->state;

    // Fast path: state has bits remaining
    if (state > 1) {
        int bit = state >> 31;
        ctx->state = state << 1;
        return bit;
    }

    // Need to load new word
    // state is 0 or 1 (1 means prev bit was 1 and we consumed all bits)
    int prev_sign = state;

    const u8 *p = ctx->flag_ptr;
    u32 word = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    ctx->flag_ptr = p + 4;

    int bit = word >> 31;
    ctx->state = (word << 1) | prev_sign;
    return bit;
}

static inline u16 read_palette_color(DecodeContext *ctx) {
    u16 color = read_u16_unaligned(ctx->palette_ptr);
    ctx->palette_ptr += 2;
    return color;
}

static inline s16 to_signed16(u16 val) {
    return (s16)val;
}

static inline u8 read_code(DecodeContext *ctx) {
    u8 code = *ctx->payload_ptr;
    ctx->payload_ptr++;
    return code;
}

// Block operations - Hot path
// Use 32-bit operations where possible for better throughput
static IWRAM_CODE void copy_u32_block(DecodeContext *ctx, int dst_off, int ref_off, int rows, int words) {
    u32 *d = (u32*)((u8*)ctx->dst + dst_off);
    const u32 *s = (const u32*)((const u8*)ctx->ref + ref_off);

    // ROW_BYTES = 480, so stride in u32 = 120
    const int stride = ROW_BYTES >> 2;

    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < words; i++) {
            d[i] = s[i];
        }
        d += stride;
        s += stride;
    }
}

static IWRAM_CODE void fill_u32_block(DecodeContext *ctx, int dst_off, int rows, int words, u16 color) {
    u32 *d = (u32*)((u8*)ctx->dst + dst_off);
    const u32 color32 = color | (color << 16);
    const int stride = ROW_BYTES >> 2;

    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < words; i++) {
            d[i] = color32;
        }
        d += stride;
    }
}

static IWRAM_CODE void delta_u32_block(DecodeContext *ctx, int dst_off, int ref_off, int rows, int words, s16 delta) {
    u16 *d = (u16*)((u8*)ctx->dst + dst_off);
    const u16 *s = (const u16*)((const u8*)ctx->ref + ref_off);
    const int stride = ROW_BYTES >> 1;

    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < words * 2; i++) {
            d[i] = s[i] + delta;
        }
        d += stride;
        s += stride;
    }
}

static IWRAM_CODE void copy_u16_block(DecodeContext *ctx, int dst_off, int ref_off, int rows, int halfwords) {
    u16 *d = (u16*)((u8*)ctx->dst + dst_off);
    const u16 *s = (const u16*)((const u8*)ctx->ref + ref_off);
    const int stride = ROW_BYTES >> 1;

    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < halfwords; i++) {
            d[i] = s[i];
        }
        d += stride;
        s += stride;
    }
}

static IWRAM_CODE void fill_u16_block(DecodeContext *ctx, int dst_off, int rows, int halfwords, u16 color) {
    u16 *d = (u16*)((u8*)ctx->dst + dst_off);
    const int stride = ROW_BYTES >> 1;

    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < halfwords; i++) {
            d[i] = color;
        }
        d += stride;
    }
}

static IWRAM_CODE void delta_u16_block(DecodeContext *ctx, int dst_off, int ref_off, int rows, int halfwords, s16 delta) {
    u16 *d = (u16*)((u8*)ctx->dst + dst_off);
    const u16 *s = (const u16*)((const u8*)ctx->ref + ref_off);
    const int stride = ROW_BYTES >> 1;

    for (int r = 0; r < rows; r++) {
        for (int i = 0; i < halfwords; i++) {
            d[i] = s[i] + delta;
        }
        d += stride;
        s += stride;
    }
}

// Forward declarations
static IWRAM_CODE void decode_block_8x4(DecodeContext *ctx);
static IWRAM_CODE void decode_block_4x8(DecodeContext *ctx);
static IWRAM_CODE void decode_block_4x4(DecodeContext *ctx);
static IWRAM_CODE void decode_block_8x2(DecodeContext *ctx);
static IWRAM_CODE void decode_block_2x8(DecodeContext *ctx);
static IWRAM_CODE void decode_block_2x4(DecodeContext *ctx);
static IWRAM_CODE void decode_block_4x2(DecodeContext *ctx);
static IWRAM_CODE void decode_block_1x8(DecodeContext *ctx);
static IWRAM_CODE void decode_block_8x1(DecodeContext *ctx);
static IWRAM_CODE void decode_block_1x4(DecodeContext *ctx);
static IWRAM_CODE void decode_block_2x2(DecodeContext *ctx);
static IWRAM_CODE void decode_block_4x1(DecodeContext *ctx);
static IWRAM_CODE void decode_block_1x2(DecodeContext *ctx);
static IWRAM_CODE void decode_block_2x1(DecodeContext *ctx);


// Functions
static IWRAM_CODE void decode_block_8x8(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 8, 4);
        } else {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 4);
        }
        return;
    }

    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            decode_block_8x4(ctx);
            decode_block_8x4(ctx);
        } else {
            decode_block_4x8(ctx);
            decode_block_4x8(ctx);
        }
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 4, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u32_block(ctx, ctx->block_offset, 8, 4, color);
    }
}

static IWRAM_CODE void decode_block_8x4(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 4, 4);
        } else {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 4);
        }
        ctx->block_offset += 0x780;
        return;
    }

    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) != 0) {
            decode_block_4x4(ctx);
            decode_block_4x4(ctx);
            ctx->block_offset += 0x770;
            return;
        }
        decode_block_8x2(ctx);
        decode_block_8x2(ctx);
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 4, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u32_block(ctx, ctx->block_offset, 4, 4, color);
    }
    ctx->block_offset += 0x780;
}

static IWRAM_CODE void decode_block_4x8(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 8, 2);
        } else {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 2);
        }
        ctx->block_offset += 8;
        return;
    }

    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) != 0) {
            decode_block_2x8(ctx);
            decode_block_2x8(ctx);
            return;
        }
        decode_block_4x4(ctx);
        ctx->block_offset += 0x778;
        decode_block_4x4(ctx);
        ctx->block_offset -= 0x780;
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 2, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u32_block(ctx, ctx->block_offset, 8, 2, color);
    }
    ctx->block_offset += 8;
}

static IWRAM_CODE void decode_block_2x8(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 8, 1);
        } else {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 1);
        }
        ctx->block_offset += 4;
        return;
    }

    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) != 0) {
            decode_block_1x8(ctx);
            decode_block_1x8(ctx);
            return;
        }
        decode_block_2x4(ctx);
        ctx->block_offset += 0x77C;
        decode_block_2x4(ctx);
        ctx->block_offset -= 0x780;
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 1, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u32_block(ctx, ctx->block_offset, 8, 1, color);
    }
    ctx->block_offset += 4;
}

static IWRAM_CODE void decode_block_1x8(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u16_block(ctx, ctx->block_offset, ctx->block_offset, 8, 1);
        } else {
            u8 code = read_code(ctx);
            copy_u16_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 1);
        }
        ctx->block_offset += 2;
        return;
    }

    if (next_bit(ctx) == 0) {
        decode_block_1x4(ctx);
        ctx->block_offset += 0x77E;
        decode_block_1x4(ctx);
        ctx->block_offset -= 0x780;
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u16_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 8, 1, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u16_block(ctx, ctx->block_offset, 8, 1, color);
    }
    ctx->block_offset += 2;
}

static IWRAM_CODE void decode_block_4x4(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 4, 2);
        } else {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 2);
        }
        ctx->block_offset += 8;
        return;
    }

    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) != 0) {
            decode_block_2x4(ctx);
            decode_block_2x4(ctx);
            return;
        }
        decode_block_4x2(ctx);
        ctx->block_offset += 0x3B8;
        decode_block_4x2(ctx);
        ctx->block_offset -= 0x3C0;
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 2, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u32_block(ctx, ctx->block_offset, 4, 2, color);
    }
    ctx->block_offset += 8;
}

static IWRAM_CODE void decode_block_8x2(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 2, 4);
        } else {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 4);
        }
        ctx->block_offset += 0x3C0;
        return;
    }

    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) != 0) {
            decode_block_4x2(ctx);
            decode_block_4x2(ctx);
            ctx->block_offset += 0x3B0;
            return;
        }
        decode_block_8x1(ctx);
        decode_block_8x1(ctx);
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 4, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u32_block(ctx, ctx->block_offset, 2, 4, color);
    }
    ctx->block_offset += 0x3C0;
}

static IWRAM_CODE void decode_block_2x4(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 4, 1);
        } else {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 1);
        }
        ctx->block_offset += 4;
        return;
    }

    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) != 0) {
            decode_block_1x4(ctx);
            decode_block_1x4(ctx);
            return;
        }
        decode_block_2x2(ctx);
        ctx->block_offset += 0x3BC;
        decode_block_2x2(ctx);
        ctx->block_offset -= 0x3C0;
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 1, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u32_block(ctx, ctx->block_offset, 4, 1, color);
    }
    ctx->block_offset += 4;
}

static IWRAM_CODE void decode_block_4x2(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 2, 2);
        } else {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 2);
        }
        ctx->block_offset += 8;
        return;
    }

    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) != 0) {
            decode_block_2x2(ctx);
            decode_block_2x2(ctx);
            return;
        }
        decode_block_4x1(ctx);
        ctx->block_offset += 0x1D8;
        decode_block_4x1(ctx);
        ctx->block_offset -= 0x1E0;
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 2, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u32_block(ctx, ctx->block_offset, 2, 2, color);
    }
    ctx->block_offset += 8;
}

static IWRAM_CODE void decode_block_8x1(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 1, 4);
        } else {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 1, 4);
        }
        ctx->block_offset += 0x1E0;
        return;
    }

    if (next_bit(ctx) == 0) {
        decode_block_4x1(ctx);
        decode_block_4x1(ctx);
        ctx->block_offset += 0x1D0;
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 1, 4, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u32_block(ctx, ctx->block_offset, 1, 4, color);
    }
    ctx->block_offset += 0x1E0;
}

static IWRAM_CODE void decode_block_1x4(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u16_block(ctx, ctx->block_offset, ctx->block_offset, 4, 1);
        } else {
            u8 code = read_code(ctx);
            copy_u16_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 1);
        }
        ctx->block_offset += 2;
        return;
    }

    if (next_bit(ctx) == 0) {
        decode_block_1x2(ctx);
        ctx->block_offset += 0x3BE;
        decode_block_1x2(ctx);
        ctx->block_offset -= 0x3C0;
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u16_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 4, 1, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u16_block(ctx, ctx->block_offset, 4, 1, color);
    }
    ctx->block_offset += 2;
}

static IWRAM_CODE void decode_block_2x2(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 2, 1);
        } else {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 1);
        }
        ctx->block_offset += 4;
        return;
    }

    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) != 0) {
            decode_block_1x2(ctx);
            decode_block_1x2(ctx);
            return;
        }
        decode_block_2x1(ctx);
        ctx->block_offset += 0x1DC;
        decode_block_2x1(ctx);
        ctx->block_offset -= 0x1E0;
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 1, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u32_block(ctx, ctx->block_offset, 2, 1, color);
    }
    ctx->block_offset += 4;
}

static IWRAM_CODE void decode_block_4x1(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 1, 2);
        } else {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 1, 2);
        }
        ctx->block_offset += 8;
        return;
    }

    if (next_bit(ctx) == 0) {
        decode_block_2x1(ctx);
        decode_block_2x1(ctx);
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 1, 2, color);
    } else {
        u16 color = read_palette_color(ctx);
        fill_u32_block(ctx, ctx->block_offset, 1, 2, color);
    }
    ctx->block_offset += 8;
}

static IWRAM_CODE void decode_block_1x2(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u16_block(ctx, ctx->block_offset, ctx->block_offset, 2, 1);
        } else {
            u8 code = read_code(ctx);
            copy_u16_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 1);
        }
        ctx->block_offset += 2;
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u16_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 2, 1, color);
    } else {
        if (next_bit(ctx) == 0) {
            u16 color0 = read_palette_color(ctx);
            fill_u16_block(ctx, ctx->block_offset, 2, 1, color0);
        } else {
            u16 color0 = read_palette_color(ctx);
            u16 color1 = read_palette_color(ctx);
            ctx->dst[ctx->block_offset >> 1] = color0;
            ctx->dst[(ctx->block_offset + ROW_BYTES) >> 1] = color1;
        }
    }
    ctx->block_offset += 2;
}

static IWRAM_CODE void decode_block_2x1(DecodeContext *ctx) {
    if (next_bit(ctx) == 0) {
        if (next_bit(ctx) == 0) {
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset, 1, 1);
        } else {
            u8 code = read_code(ctx);
            copy_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 1, 1);
        }
        ctx->block_offset += 4;
        return;
    }

    if (next_bit(ctx) == 0) {
        u8 code = read_code(ctx);
        s16 color = to_signed16(read_palette_color(ctx));
        delta_u32_block(ctx, ctx->block_offset, ctx->block_offset + CODEBOOK_OFFSETS[code], 1, 1, color);
    } else {
        if (next_bit(ctx) == 0) {
            u16 color0 = read_palette_color(ctx);
            // Fill 1x1 (2 pixels width) with same color
            ctx->dst[ctx->block_offset >> 1] = color0;
            ctx->dst[(ctx->block_offset >> 1) + 1] = color0;
        } else {
            u16 color0 = read_palette_color(ctx);
            u16 color1 = read_palette_color(ctx);
            ctx->dst[ctx->block_offset >> 1] = color0;
            ctx->dst[(ctx->block_offset >> 1) + 1] = color1;
        }
    }
    ctx->block_offset += 4;
}

// Also put the main decoder loop in IWRAM for good measure?
// It calls many IWRAM functions, so it's less critical, but looping overhead is reduced.
u32 IWRAM_CODE gbm_decode_frame(const u8 *data, u32 offset, u16 *dst, const u16 *ref) {
    u16 frame_len = read_u16_unaligned(data + offset);
    u16 bit_enc = read_u16_unaligned(data + offset + 2);
    u16 palette_bytes = read_u16_unaligned(data + offset + 4);

    u32 next_offset = offset + 2 + frame_len;

    u8 b2 = bit_enc & 0xFF;
    u8 b3 = (bit_enc >> 8) & 0xFF;
    u16 flag_bytes = (b2 ^ 0x69) | ((b3 ^ 0xD6) << 8);

    DecodeContext ctx;
    ctx.state = 0x80000000; // Initial state
    
    u32 flag_start = offset + 6;
    u32 flag_end = flag_start + flag_bytes;
    u32 pal_start = flag_end;
    u32 pal_end = pal_start + palette_bytes;
    
    ctx.flag_ptr = data + flag_start;
    ctx.flag_end = data + flag_end;
    ctx.palette_ptr = data + pal_start;
    ctx.palette_end = data + pal_end;
    ctx.payload_ptr = data + pal_end;
    ctx.payload_end = data + next_offset;
    
    ctx.dst = dst;
    // If ref is null, use dst (intra prediction behavior)
    ctx.ref = ref ? ref : dst;

    // Decode loop
    for (int y_block = 0; y_block < 20; y_block++) {
        ctx.row_offset = y_block * 8 * ROW_BYTES;
        ctx.block_offset = ctx.row_offset;
        for (int x_block = 0; x_block < 30; x_block++) {
            ctx.block_offset = ctx.row_offset + x_block * 8 * 2; // 2 bytes per pixel
            decode_block_8x8(&ctx);
        }
    }

    return next_offset;
}
