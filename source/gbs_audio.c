/*
 * GBS Audio Decoder for GBA
 *
 * Implementation of GBS audio playback supporting all 5 modes.
 * Based on reverse engineering of savemu.dll from M3 Movie Player.
 */

#include "gbs_audio.h"

#include <gba_dma.h>
#include <gba_interrupt.h>
#include <gba_sound.h>
#include <gba_timers.h>
#include <string.h>

// IWRAM placement for performance-critical code
#define IWRAM_CODE __attribute__((section(".iwram"), long_call))

// ============================================================================
// Constants
// ============================================================================

#define GBA_MASTER_CLOCK    16777216
#define GBS_HEADER_SIZE     0x200

#ifndef TIMER_CASCADE
#define TIMER_CASCADE       0x0004
#endif
#ifndef TIMER_IRQ
#define TIMER_IRQ           0x0040
#endif
#define SOUNDCNT_X_ENABLE   0x0080

// Buffer configuration
// Use 368 samples for ~60Hz buffer swap rate at 22050Hz
#define AUDIO_BUFFER_SAMPLES    368
#define AUDIO_BUFFER_COUNT      2

// ============================================================================
// ADPCM Tables
// ============================================================================

// Standard IMA ADPCM step table (89 entries)
__attribute__((section(".iwram.rodata"))) static const int16_t ima_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

// Standard IMA ADPCM index adjustment table (4-bit)
__attribute__((section(".iwram.rodata"))) static const int8_t ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

// 3-bit ADPCM index adjustment table (from savemu.dll)
__attribute__((section(".iwram.rodata"))) static const int8_t adpcm3_index_table[8] = {
    -1, -1, 2, 6, -1, -1, 2, 6
};

// 2-bit ADPCM delta table (from savemu.dll 0x1000e388)
// 356 entries: 89 step levels * 4 codes
__attribute__((section(".iwram.rodata"))) static const int16_t adpcm2_delta_table[356] = {
    3, 10, -3, -10, 4, 12, -4, -12,
    4, 13, -4, -13, 5, 15, -5, -15,
    5, 16, -5, -16, 6, 18, -6, -18,
    6, 19, -6, -19, 7, 21, -7, -21,
    8, 24, -8, -24, 8, 25, -8, -25,
    9, 28, -9, -28, 10, 31, -10, -31,
    11, 34, -11, -34, 12, 37, -12, -37,
    14, 42, -14, -42, 15, 46, -15, -46,
    17, 51, -17, -51, 18, 55, -18, -55,
    20, 61, -20, -61, 22, 67, -22, -67,
    25, 75, -25, -75, 27, 82, -27, -82,
    30, 90, -30, -90, 33, 99, -33, -99,
    36, 109, -36, -109, 40, 120, -40, -120,
    44, 132, -44, -132, 48, 145, -48, -145,
    53, 160, -53, -160, 59, 177, -59, -177,
    65, 195, -65, -195, 71, 214, -71, -214,
    78, 235, -78, -235, 86, 259, -86, -259,
    95, 285, -95, -285, 104, 313, -104, -313,
    115, 345, -115, -345, 126, 379, -126, -379,
    139, 418, -139, -418, 153, 460, -153, -460,
    168, 505, -168, -505, 185, 556, -185, -556,
    204, 612, -204, -612, 224, 673, -224, -673,
    247, 741, -247, -741, 272, 816, -272, -816,
    299, 897, -299, -897, 329, 987, -329, -987,
    362, 1086, -362, -1086, 398, 1194, -398, -1194,
    438, 1314, -438, -1314, 481, 1444, -481, -1444,
    530, 1590, -530, -1590, 583, 1749, -583, -1749,
    641, 1923, -641, -1923, 705, 2116, -705, -2116,
    776, 2328, -776, -2328, 853, 2560, -853, -2560,
    939, 2817, -939, -2817, 1033, 3099, -1033, -3099,
    1136, 3408, -1136, -3408, 1249, 3748, -1249, -3748,
    1374, 4123, -1374, -4123, 1512, 4536, -1512, -4536,
    1663, 4990, -1663, -4990, 1830, 5490, -1830, -5490,
    2013, 6039, -2013, -6039, 2214, 6642, -2214, -6642,
    2435, 7306, -2435, -7306, 2679, 8037, -2679, -8037,
    2947, 8841, -2947, -8841, 3242, 9726, -3242, -9726,
    3566, 10698, -3566, -10698, 3922, 11767, -3922, -11767,
    4315, 12945, -4315, -12945, 4746, 14239, -4746, -14239,
    5221, 15663, -5221, -15663, 5743, 17230, -5743, -17230,
    6317, 18952, -6317, -18952, 6949, 20848, -6949, -20848,
    7644, 22933, -7644, -22933, 8409, 25227, -8409, -25227,
    9250, 27750, -9250, -27750, 10175, 30525, -10175, -30525,
    11179, -31999, -11179, 31999, 12316, -28587, -12316, 28587,
    13543, -24907, -13543, 24907, 14897, -20845, -14897, 20845
};

// ============================================================================
// Internal State
// ============================================================================

// GBS file header structure
typedef struct {
    char magic[4];          // "GBAL"
    uint32_t file_size;
    char marker[4];         // "MUSI"
    uint32_t reserved1;
    uint32_t mode;
    uint32_t reserved2[59]; // Padding to 0x200
} __attribute__((packed)) GbsHeader;

// Per-channel decoder state
typedef struct {
    int32_t predictor;      // Current predictor (unsigned 16-bit range for 2/3-bit)
    int32_t step_index;     // Current step index
} ChannelState;

// Internal audio state
static struct {
    // GBS file info
    const uint8_t* gbs_data;
    uint32_t gbs_size;
    GbsAudioInfo info;

    // Decoder state
    ChannelState left;
    ChannelState right;     // Only used for stereo

    // Block tracking - current_block_ptr caches gbs_data + header + block_index * block_size
    const uint8_t* current_block_ptr;
    uint32_t block_index;
    uint32_t byte_in_block;
    uint32_t block_header_size;

    // Buffered samples for multi-sample decoders
    // Mode 1 (3-bit): 8 samples per 3 bytes
    int16_t buffered_samples[8];
    uint8_t samples_buffered;

    // Mode 2 (4-bit mono): high nibble buffering
    int16_t high_nibble_sample;
    bool have_high_nibble;

    // Playback state
    volatile uint8_t active_buffer;
} state;

// Double buffers for decoded PCM (8-bit signed)
// For stereo: left channel in buffer_left, right in buffer_right
EWRAM_DATA static int8_t audio_buffer_left[AUDIO_BUFFER_COUNT][AUDIO_BUFFER_SAMPLES] __attribute__((aligned(4)));
EWRAM_DATA static int8_t audio_buffer_right[AUDIO_BUFFER_COUNT][AUDIO_BUFFER_SAMPLES] __attribute__((aligned(4)));

// ============================================================================
// ADPCM Decoding Functions
// ============================================================================

// Decode single 4-bit IMA ADPCM sample
static IWRAM_CODE int16_t decode_ima_4bit(uint8_t nibble, ChannelState* ch) {
    int step = ima_step_table[ch->step_index];

    int diff = step >> 3;
    if (nibble & 4) diff += step;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 1) diff += step >> 2;

    if (nibble & 8) {
        ch->predictor -= diff;
    } else {
        ch->predictor += diff;
    }

    // Clamp to signed 16-bit
    if (ch->predictor > 32767) ch->predictor = 32767;
    else if (ch->predictor < -32768) ch->predictor = -32768;

    // Update step index
    ch->step_index += ima_index_table[nibble & 0x0F];
    if (ch->step_index < 0) ch->step_index = 0;
    else if (ch->step_index > 88) ch->step_index = 88;

    return (int16_t)ch->predictor;
}

// Decode single 3-bit ADPCM sample
static IWRAM_CODE int16_t decode_adpcm_3bit(uint8_t code, ChannelState* ch) {
    int step = ima_step_table[ch->step_index];

    int diff = step >> 2;
    if (code & 2) diff += step;
    if (code & 1) diff += step >> 1;

    if (code & 4) {
        ch->predictor -= diff;
    } else {
        ch->predictor += diff;
    }

    // Clamp to unsigned 16-bit (0-65535)
    if (ch->predictor < 0) ch->predictor = 0;
    else if (ch->predictor > 65535) ch->predictor = 65535;

    // Update step index
    ch->step_index += adpcm3_index_table[code & 7];
    if (ch->step_index < 0) ch->step_index = 0;
    else if (ch->step_index > 88) ch->step_index = 88;

    // Return as signed (centered at 0x8000)
    return (int16_t)(ch->predictor - 0x8000);
}

// Decode single 2-bit ADPCM sample
static IWRAM_CODE int16_t decode_adpcm_2bit(uint8_t code, ChannelState* ch) {
    int32_t table_index = code + ch->step_index;
    if (table_index > 352) table_index = 352;

    int16_t delta = adpcm2_delta_table[table_index];
    ch->predictor += delta;

    // Clamp to unsigned 16-bit
    if (ch->predictor < 0) ch->predictor = 0;
    else if (ch->predictor > 65535) ch->predictor = 65535;

    // Update step index: bit0=1 -> +4, bit0=0 -> -4
    if (code & 1) {
        ch->step_index += 4;
        if (ch->step_index > 0x160) ch->step_index = 0x160;
    } else {
        ch->step_index -= 4;
        if (ch->step_index < 0) ch->step_index = 0;
    }

    return (int16_t)(ch->predictor - 0x8000);
}

// ============================================================================
// Block Management
// ============================================================================

static inline const uint8_t* get_current_block(void) {
    return state.current_block_ptr;
}

static IWRAM_CODE void parse_block_header_mono(const uint8_t* block, ChannelState* ch) {
    uint16_t predictor = block[0] | (block[1] << 8);
    uint16_t step_idx = block[2] | (block[3] << 8);

    ch->predictor = predictor;
    ch->step_index = step_idx;

    // Clamp step index based on mode
    if (state.info.mode == GBS_MODE_MONO_2BIT ||
        state.info.mode == GBS_MODE_MONO_2BIT_SM) {
        if (ch->step_index > 0x160) ch->step_index = 0x160;
    } else {
        if (ch->step_index > 88) ch->step_index = 88;
    }
}

static IWRAM_CODE void parse_block_header_stereo(const uint8_t* block) {
    // Left channel: bytes 0-3
    uint16_t pred_l = block[0] | (block[1] << 8);
    uint16_t step_l = block[2] | (block[3] << 8);
    state.left.predictor = (int16_t)(pred_l - 0x8000);
    state.left.step_index = (step_l > 88) ? 88 : step_l;

    // Right channel: bytes 4-7
    uint16_t pred_r = block[4] | (block[5] << 8);
    uint16_t step_r = block[6] | (block[7] << 8);
    state.right.predictor = (int16_t)(pred_r - 0x8000);
    state.right.step_index = (step_r > 88) ? 88 : step_r;
}

static IWRAM_CODE void advance_to_next_block(void) {
    state.block_index++;
    state.byte_in_block = 0;
    state.current_block_ptr += state.info.block_size;  // Just add block_size instead of multiply

    if (state.block_index >= state.info.total_blocks) {
        state.info.is_finished = true;
        return;
    }

    const uint8_t* block = state.current_block_ptr;

    if (state.info.channels == 2) {
        parse_block_header_stereo(block);
    } else {
        parse_block_header_mono(block, &state.left);
    }
}

// ============================================================================
// Buffer Decoding Functions
// ============================================================================

// Mode 0: Stereo 4-bit IMA ADPCM
static IWRAM_CODE void decode_buffer_stereo_4bit(int8_t* left, int8_t* right, uint32_t count) {
    // Cache data pointer (past header) and state fields for faster access
    const uint8_t* data = state.current_block_ptr + state.block_header_size;
    uint32_t data_per_block = state.info.block_size - state.block_header_size;
    uint32_t byte_pos = state.byte_in_block;
    uint32_t decoded = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (state.info.is_finished) {
            left[i] = 0;
            right[i] = 0;
            continue;
        }

        // Check if we need next block
        if (byte_pos >= data_per_block) {
            state.byte_in_block = byte_pos;  // Save before advancing
            advance_to_next_block();
            if (state.info.is_finished) {
                left[i] = 0;
                right[i] = 0;
                continue;
            }
            data = state.current_block_ptr + state.block_header_size;
            byte_pos = 0;
        }

        uint32_t byte = data[byte_pos++];

        // Low nibble = left, high nibble = right
        int16_t sample_l = decode_ima_4bit(byte & 0x0F, &state.left);
        int16_t sample_r = decode_ima_4bit(byte >> 4, &state.right);

        left[i] = (int8_t)(sample_l >> 8);
        right[i] = (int8_t)(sample_r >> 8);

        decoded++;
    }
    state.byte_in_block = byte_pos;
    state.info.samples_decoded += decoded;
}

// Mode 1: Mono 3-bit ADPCM (8 samples per 3 bytes)
static IWRAM_CODE void decode_buffer_mono_3bit(int8_t* dest, uint32_t count) {
    const uint8_t* data = state.current_block_ptr + state.block_header_size;
    uint32_t data_per_block = state.info.block_size - state.block_header_size;
    uint32_t byte_pos = state.byte_in_block;
    uint32_t decoded = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (state.info.is_finished) {
            dest[i] = 0;
            continue;
        }

        // If we have buffered samples from previous group, use them
        if (state.samples_buffered > 0) {
            dest[i] = (int8_t)(state.buffered_samples[8 - state.samples_buffered] >> 8);
            state.samples_buffered--;
            decoded++;
            continue;
        }

        // Check if we need next block (need 3 bytes)
        if (byte_pos + 3 > data_per_block) {
            state.byte_in_block = byte_pos;
            advance_to_next_block();
            if (state.info.is_finished) {
                dest[i] = 0;
                continue;
            }
            data = state.current_block_ptr + state.block_header_size;
            byte_pos = 0;
        }

        // Read 3 bytes = 24 bits = 8 samples
        uint32_t packed = data[byte_pos] | (data[byte_pos + 1] << 8) | (data[byte_pos + 2] << 16);
        byte_pos += 3;

        // Decode 8 samples (unrolled, shift by 3 each time)
        state.buffered_samples[0] = decode_adpcm_3bit(packed & 0x07, &state.left);
        packed >>= 3;
        state.buffered_samples[1] = decode_adpcm_3bit(packed & 0x07, &state.left);
        packed >>= 3;
        state.buffered_samples[2] = decode_adpcm_3bit(packed & 0x07, &state.left);
        packed >>= 3;
        state.buffered_samples[3] = decode_adpcm_3bit(packed & 0x07, &state.left);
        packed >>= 3;
        state.buffered_samples[4] = decode_adpcm_3bit(packed & 0x07, &state.left);
        packed >>= 3;
        state.buffered_samples[5] = decode_adpcm_3bit(packed & 0x07, &state.left);
        packed >>= 3;
        state.buffered_samples[6] = decode_adpcm_3bit(packed & 0x07, &state.left);
        packed >>= 3;
        state.buffered_samples[7] = decode_adpcm_3bit(packed & 0x07, &state.left);

        // Output first sample, buffer the rest
        dest[i] = (int8_t)(state.buffered_samples[0] >> 8);
        state.samples_buffered = 7;
        decoded++;
    }
    state.byte_in_block = byte_pos;
    state.info.samples_decoded += decoded;
}

// Mode 2: Mono 4-bit IMA ADPCM
static IWRAM_CODE void decode_buffer_mono_4bit(int8_t* dest, uint32_t count) {
    const uint8_t* data = state.current_block_ptr + state.block_header_size;
    uint32_t data_per_block = state.info.block_size - state.block_header_size;
    uint32_t byte_pos = state.byte_in_block;
    uint32_t decoded = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (state.info.is_finished) {
            dest[i] = 0;
            continue;
        }

        // Check for buffered high nibble
        if (state.have_high_nibble) {
            dest[i] = (int8_t)(state.high_nibble_sample >> 8);
            state.have_high_nibble = false;
            decoded++;
            continue;
        }

        // Check if we need next block
        if (byte_pos >= data_per_block) {
            state.byte_in_block = byte_pos;
            advance_to_next_block();
            if (state.info.is_finished) {
                dest[i] = 0;
                continue;
            }
            data = state.current_block_ptr + state.block_header_size;
            byte_pos = 0;
        }

        uint32_t byte = data[byte_pos++];

        // Low nibble first
        int16_t sample_lo = decode_ima_4bit(byte & 0x0F, &state.left);
        dest[i] = (int8_t)(sample_lo >> 8);
        decoded++;

        // Buffer high nibble for next iteration
        state.high_nibble_sample = decode_ima_4bit(byte >> 4, &state.left);
        state.have_high_nibble = true;
    }
    state.byte_in_block = byte_pos;
    state.info.samples_decoded += decoded;
}

// Mode 3/4: Mono 2-bit ADPCM (4 samples per byte)
static IWRAM_CODE void decode_buffer_mono_2bit(int8_t* dest, uint32_t count) {
    const uint8_t* data = state.current_block_ptr + state.block_header_size;
    uint32_t data_per_block = state.info.block_size - state.block_header_size;
    uint32_t byte_pos = state.byte_in_block;
    uint32_t decoded = 0;

    for (uint32_t i = 0; i < count; i++) {
        if (state.info.is_finished) {
            dest[i] = 0;
            continue;
        }

        // Check for buffered samples
        if (state.samples_buffered > 0) {
            dest[i] = (int8_t)(state.buffered_samples[4 - state.samples_buffered] >> 8);
            state.samples_buffered--;
            decoded++;
            continue;
        }

        // Check if we need next block
        if (byte_pos >= data_per_block) {
            state.byte_in_block = byte_pos;
            advance_to_next_block();
            if (state.info.is_finished) {
                dest[i] = 0;
                continue;
            }
            data = state.current_block_ptr + state.block_header_size;
            byte_pos = 0;
        }

        uint32_t byte = data[byte_pos++];

        // Decode 4 samples from byte (unrolled, shift instead of multiply)
        state.buffered_samples[0] = decode_adpcm_2bit(byte & 0x03, &state.left);
        byte >>= 2;
        state.buffered_samples[1] = decode_adpcm_2bit(byte & 0x03, &state.left);
        byte >>= 2;
        state.buffered_samples[2] = decode_adpcm_2bit(byte & 0x03, &state.left);
        byte >>= 2;
        state.buffered_samples[3] = decode_adpcm_2bit(byte & 0x03, &state.left);

        // Output first sample, buffer the rest
        dest[i] = (int8_t)(state.buffered_samples[0] >> 8);
        state.samples_buffered = 3;
        decoded++;
    }
    state.byte_in_block = byte_pos;
    state.info.samples_decoded += decoded;
}

// Dispatch to appropriate decoder
// Note: For mono modes, right is always NULL (DMA2 not used), so no need to clear it
static IWRAM_CODE void decode_buffer(int8_t* left, int8_t* right, uint32_t count) {
    switch (state.info.mode) {
        case GBS_MODE_STEREO_4BIT:
            decode_buffer_stereo_4bit(left, right, count);
            break;
        case GBS_MODE_MONO_3BIT:
            decode_buffer_mono_3bit(left, count);
            break;
        case GBS_MODE_MONO_4BIT:
            decode_buffer_mono_4bit(left, count);
            break;
        case GBS_MODE_MONO_2BIT:
        case GBS_MODE_MONO_2BIT_SM:
            decode_buffer_mono_2bit(left, count);
            break;
        default:
            memset(left, 0, count);
            break;
    }
}

// ============================================================================
// Interrupt Handler
// ============================================================================

static IWRAM_CODE void audio_timer1_handler(void) {
    REG_IF = IRQ_TIMER1;

    if (state.info.is_finished) {
        REG_DMA1CNT = 0;
        REG_DMA2CNT = 0;
        state.info.is_playing = false;
        return;
    }

    // Swap buffers
    uint8_t play_buffer = state.active_buffer;
    uint8_t decode_buffer_idx = play_buffer ^ 1;
    state.active_buffer = decode_buffer_idx;

    // Restart DMA for new buffer
    REG_DMA1CNT = 0;
    REG_DMA1SAD = (uint32_t)audio_buffer_left[decode_buffer_idx];
    REG_DMA1DAD = (uint32_t)&REG_FIFO_A;
    REG_DMA1CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;

    if (state.info.channels == 2) {
        REG_DMA2CNT = 0;
        REG_DMA2SAD = (uint32_t)audio_buffer_right[decode_buffer_idx];
        REG_DMA2DAD = (uint32_t)&REG_FIFO_B;
        REG_DMA2CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;
    }

    // Decode into buffer that just finished playing
    decode_buffer(audio_buffer_left[play_buffer],
                  state.info.channels == 2 ? audio_buffer_right[play_buffer] : NULL,
                  AUDIO_BUFFER_SAMPLES);
}

// ============================================================================
// Public API
// ============================================================================

bool gbs_audio_init(const uint8_t* gbs_data, uint32_t gbs_size) {
    // Clear state
    memset(&state, 0, sizeof(state));

    state.gbs_data = gbs_data;
    state.gbs_size = gbs_size;
    state.info.mode = GBS_MODE_INVALID;

    // Validate header
    if (gbs_size < GBS_HEADER_SIZE) {
        return false;
    }

    const GbsHeader* header = (const GbsHeader*)gbs_data;

    if (memcmp(header->magic, "GBAL", 4) != 0 ||
        memcmp(header->marker, "MUSI", 4) != 0) {
        return false;
    }

    if (header->mode > 4) {
        return false;
    }

    // Configure based on mode
    state.info.mode = (GbsMode)header->mode;

    switch (state.info.mode) {
        case GBS_MODE_STEREO_4BIT:
            state.info.sample_rate = 22050;
            state.info.channels = 2;
            state.info.block_size = 0x400;
            state.block_header_size = 8;  // 4 bytes per channel
            break;
        case GBS_MODE_MONO_3BIT:
            state.info.sample_rate = 11025;
            state.info.channels = 1;
            state.info.block_size = 0x400;
            state.block_header_size = 4;
            break;
        case GBS_MODE_MONO_4BIT:
            state.info.sample_rate = 11025;
            state.info.channels = 1;
            state.info.block_size = 0x200;
            state.block_header_size = 4;
            break;
        case GBS_MODE_MONO_2BIT:
            state.info.sample_rate = 22050;
            state.info.channels = 1;
            state.info.block_size = 0x200;
            state.block_header_size = 4;
            break;
        case GBS_MODE_MONO_2BIT_SM:
            state.info.sample_rate = 22050;
            state.info.channels = 1;
            state.info.block_size = 0x100;
            state.block_header_size = 4;
            break;
        default:
            return false;
    }

    // Calculate totals
    uint32_t data_size = gbs_size - GBS_HEADER_SIZE;
    state.info.total_blocks = data_size / state.info.block_size;

    // Calculate samples per block based on mode
    uint32_t data_per_block = state.info.block_size - state.block_header_size;
    uint32_t samples_per_block;

    switch (state.info.mode) {
        case GBS_MODE_STEREO_4BIT:
            samples_per_block = data_per_block;  // 1 sample pair per byte
            break;
        case GBS_MODE_MONO_3BIT:
            samples_per_block = (data_per_block / 3) * 8;  // 8 samples per 3 bytes
            break;
        case GBS_MODE_MONO_4BIT:
            samples_per_block = data_per_block * 2;  // 2 samples per byte
            break;
        case GBS_MODE_MONO_2BIT:
        case GBS_MODE_MONO_2BIT_SM:
            samples_per_block = data_per_block * 4;  // 4 samples per byte
            break;
        default:
            samples_per_block = 0;
    }

    state.info.total_samples = state.info.total_blocks * samples_per_block;

    // Initialize first block pointer
    state.current_block_ptr = gbs_data + GBS_HEADER_SIZE;

    // Initialize first block
    if (state.info.total_blocks > 0) {
        if (state.info.channels == 2) {
            parse_block_header_stereo(state.current_block_ptr);
        } else {
            parse_block_header_mono(state.current_block_ptr, &state.left);
        }
    }

    state.info.is_finished = (state.info.total_blocks == 0);

    return true;
}

void gbs_audio_start(void) {
    if (state.info.mode == GBS_MODE_INVALID || state.info.is_finished) {
        return;
    }

    // Pre-decode both buffers
    decode_buffer(audio_buffer_left[0],
                  state.info.channels == 2 ? audio_buffer_right[0] : NULL,
                  AUDIO_BUFFER_SAMPLES);
    decode_buffer(audio_buffer_left[1],
                  state.info.channels == 2 ? audio_buffer_right[1] : NULL,
                  AUDIO_BUFFER_SAMPLES);

    state.active_buffer = 0;

    // Calculate timer reload
    uint16_t timer_reload = 65536 - (GBA_MASTER_CLOCK / state.info.sample_rate);

    // Enable sound
    REG_SOUNDCNT_X = SOUNDCNT_X_ENABLE;

    if (state.info.channels == 2) {
        // Stereo: Channel A = left, Channel B = right
        REG_SOUNDCNT_H = DSOUNDCTRL_DMG100 |
                         DSOUNDCTRL_A100 | DSOUNDCTRL_AL | DSOUNDCTRL_ATIMER(0) | DSOUNDCTRL_ARESET |
                         DSOUNDCTRL_B100 | DSOUNDCTRL_BR | DSOUNDCTRL_BTIMER(0) | DSOUNDCTRL_BRESET;
    } else {
        // Mono: Channel A only, output to both speakers
        REG_SOUNDCNT_H = DSOUNDCTRL_DMG100 |
                         DSOUNDCTRL_A100 | DSOUNDCTRL_AR | DSOUNDCTRL_AL |
                         DSOUNDCTRL_ATIMER(0) | DSOUNDCTRL_ARESET;
    }
    REG_SOUNDCNT_L = 0;

    // Clear FIFOs
    for (int i = 0; i < 16; i++) {
        REG_FIFO_A = 0;
        REG_FIFO_B = 0;
    }

    // Setup Timer0 for sample rate
    REG_TM0CNT_H = 0;
    REG_TM0CNT_L = timer_reload;
    REG_TM0CNT_H = TIMER_START;

    // Setup Timer1 cascade for buffer swap
    REG_TM1CNT_H = 0;
    REG_TM1CNT_L = 65536 - AUDIO_BUFFER_SAMPLES;
    REG_TM1CNT_H = TIMER_IRQ | TIMER_CASCADE | TIMER_START;

    // Setup interrupt
    irqSet(IRQ_TIMER1, audio_timer1_handler);
    irqEnable(IRQ_TIMER1);

    // Start DMA
    REG_DMA1CNT = 0;
    REG_DMA1SAD = (uint32_t)audio_buffer_left[0];
    REG_DMA1DAD = (uint32_t)&REG_FIFO_A;
    REG_DMA1CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;

    if (state.info.channels == 2) {
        REG_DMA2CNT = 0;
        REG_DMA2SAD = (uint32_t)audio_buffer_right[0];
        REG_DMA2DAD = (uint32_t)&REG_FIFO_B;
        REG_DMA2CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;
    }

    state.info.is_playing = true;
}

void gbs_audio_stop(void) {
    REG_DMA1CNT = 0;
    REG_DMA2CNT = 0;
    REG_TM0CNT_H = 0;
    REG_TM1CNT_H = 0;
    irqDisable(IRQ_TIMER1);
    REG_SOUNDCNT_X = 0;
    state.info.is_playing = false;
}

void gbs_audio_restart(void) {
    gbs_audio_stop();

    // Reset decoder state
    state.block_index = 0;
    state.byte_in_block = 0;
    state.info.samples_decoded = 0;
    state.info.is_finished = false;
    state.current_block_ptr = state.gbs_data + GBS_HEADER_SIZE;

    // Re-parse first block header
    if (state.info.total_blocks > 0) {
        if (state.info.channels == 2) {
            parse_block_header_stereo(state.current_block_ptr);
        } else {
            parse_block_header_mono(state.current_block_ptr, &state.left);
        }
    }

    gbs_audio_start();
}

bool gbs_audio_is_playing(void) {
    return state.info.is_playing;
}

bool gbs_audio_is_finished(void) {
    return state.info.is_finished;
}

uint32_t gbs_audio_get_progress(void) {
    if (state.info.total_samples == 0) return 0;
    return (state.info.samples_decoded * 100) / state.info.total_samples;
}

const GbsAudioInfo* gbs_audio_get_info(void) {
    return &state.info;
}

void gbs_audio_shutdown(void) {
    gbs_audio_stop();
    memset(&state, 0, sizeof(state));
    state.info.mode = GBS_MODE_INVALID;
}
