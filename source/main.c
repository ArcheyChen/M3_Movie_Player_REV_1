/*
 * GBA GBS Audio Player
 *
 * Plays 2-bit ADPCM audio from GBS files (Mode 3: 22050 Hz, mono)
 * Uses Timer0 for sample rate + Timer1 cascade for buffer swap interrupt
 */

#include <gba_base.h>
#include <gba_dma.h>
#include <gba_input.h>
#include <gba_interrupt.h>
#include <gba_sound.h>
#include <gba_systemcalls.h>
#include <gba_timers.h>
#include <gba_video.h>
#include <gba_console.h>
#include <stdio.h>
#include <string.h>

#include "audio_gbs.h"

// Timer control bits
#ifndef TIMER_CASCADE
#define TIMER_CASCADE 0x0004
#endif
#ifndef TIMER_IRQ
#define TIMER_IRQ 0x0040
#endif

// Sound control
#define SOUNDCNT_X_ENABLE 0x0080

// GBA clock
#define GBA_MASTER_CLOCK 16777216

// Audio parameters for 22050 Hz
// Timer0 reload = 65536 - (16777216 / 22050) = 65536 - 761 = 0xFD07
#define AUDIO_SAMPLE_RATE 22050
#define AUDIO_TIMER_RELOAD (65536 - (GBA_MASTER_CLOCK / AUDIO_SAMPLE_RATE))

// Double buffer configuration
// Using 368 samples per buffer (approximately 16.7ms at 22050Hz)
// This gives us ~60 buffer swaps per second, close to VBlank rate
#define AUDIO_BUFFER_SAMPLES 368
#define AUDIO_BUFFER_COUNT 2

// GBS file header structure
typedef struct {
    char magic[4];      // "GBAL"
    uint32_t file_size;
    char marker[4];     // "MUSI"
    uint32_t reserved1;
    uint32_t mode;      // 0-4, we expect mode 3
    uint32_t reserved2[59];  // Padding to 0x200
} __attribute__((packed)) GBSHeader;

// 2-bit ADPCM delta table (from savemu.dll reverse engineering)
// Table has 356 entries (89 step levels * 4 codes)
static const int16_t adpcm2_delta_table[356] = {
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

// Audio state
static struct {
    const uint8_t* data;        // Pointer to ADPCM data (after header)
    uint32_t data_size;         // Size of ADPCM data in bytes
    uint32_t nibble_offset;     // Current nibble position
    uint32_t total_nibbles;     // Total nibbles available
    int32_t predictor;          // Current predictor value (unsigned 16-bit range)
    int32_t step_index;         // Current step index (0-0x160)
    uint32_t samples_decoded;   // Total samples decoded so far
    uint32_t total_samples;     // Total samples to decode
    volatile uint8_t active_buffer;  // Which buffer is currently playing
    volatile uint8_t finished;  // Playback finished flag
} audio_state;

// Double buffer for decoded PCM (8-bit signed)
EWRAM_DATA static int8_t audio_buffer[AUDIO_BUFFER_COUNT][AUDIO_BUFFER_SAMPLES] __attribute__((aligned(4)));

// Get next 2-bit nibble from data stream
static inline uint8_t get_next_2bit(void) {
    if (audio_state.nibble_offset >= audio_state.total_nibbles) {
        return 0;
    }

    uint32_t byte_index = audio_state.nibble_offset >> 2;  // 4 nibbles per byte
    uint32_t shift = (audio_state.nibble_offset & 3) * 2;  // 0, 2, 4, 6

    audio_state.nibble_offset++;
    return (audio_state.data[byte_index] >> shift) & 0x03;
}

// Decode a single 2-bit ADPCM sample
static inline int8_t decode_2bit_sample(void) {
    uint8_t code = get_next_2bit();

    // Table lookup: table[code + step_index]
    int32_t table_index = code + audio_state.step_index;
    if (table_index > 352) table_index = 352;  // Max index is 352 (step 88 * 4 + 0)

    int16_t delta = adpcm2_delta_table[table_index];
    audio_state.predictor += delta;

    // Clamp to unsigned 16-bit range (0-65535)
    if (audio_state.predictor < 0) {
        audio_state.predictor = 0;
    } else if (audio_state.predictor > 65535) {
        audio_state.predictor = 65535;
    }

    // Update step index: bit0=1 -> +4, bit0=0 -> -4
    if (code & 1) {
        audio_state.step_index += 4;
        if (audio_state.step_index > 0x160) {
            audio_state.step_index = 0x160;
        }
    } else {
        audio_state.step_index -= 4;
        if (audio_state.step_index < 0) {
            audio_state.step_index = 0;
        }
    }

    // Convert to signed 8-bit: (predictor - 0x8000) >> 8
    return (int8_t)((audio_state.predictor - 0x8000) >> 8);
}

// Parse GBS block header and reset decoder state for it
static void parse_block_header(const uint8_t* block) {
    // Block header: 2 bytes predictor + 2 bytes step_index
    uint16_t predictor = block[0] | (block[1] << 8);
    uint16_t step_idx = block[2] | (block[3] << 8);

    audio_state.predictor = predictor;
    audio_state.step_index = step_idx;
    if (audio_state.step_index > 0x160) {
        audio_state.step_index = 0x160;
    }
}

// GBS Mode 3 constants
#define GBS_HEADER_SIZE 0x200
#define GBS_BLOCK_SIZE 0x200
#define GBS_BLOCK_HEADER_SIZE 4
#define GBS_SAMPLES_PER_BLOCK ((GBS_BLOCK_SIZE - GBS_BLOCK_HEADER_SIZE) * 4)  // 4 samples per byte

// Current block tracking
static struct {
    uint32_t block_index;       // Current block being decoded
    uint32_t total_blocks;      // Total number of blocks
    uint32_t sample_in_block;   // Current sample position within block
    uint32_t samples_per_block; // Samples per block (after header)
} block_state;

// Initialize block-based decoding
static void init_block_decoder(void) {
    const GBSHeader* header = (const GBSHeader*)audio_gbs;

    // Verify header
    if (memcmp(header->magic, "GBAL", 4) != 0 ||
        memcmp(header->marker, "MUSI", 4) != 0) {
        audio_state.finished = 1;
        return;
    }

    if (header->mode != 3) {
        // Only mode 3 (2-bit ADPCM, 22050Hz) supported for now
        audio_state.finished = 1;
        return;
    }

    // Calculate total data and blocks
    uint32_t data_start = GBS_HEADER_SIZE;
    uint32_t data_size = audio_gbs_size - data_start;

    block_state.total_blocks = data_size / GBS_BLOCK_SIZE;
    block_state.samples_per_block = GBS_SAMPLES_PER_BLOCK;
    block_state.block_index = 0;
    block_state.sample_in_block = 0;

    // Total samples
    audio_state.total_samples = block_state.total_blocks * block_state.samples_per_block;
    audio_state.samples_decoded = 0;
    audio_state.finished = 0;

    // Initialize first block
    if (block_state.total_blocks > 0) {
        const uint8_t* first_block = audio_gbs + data_start;
        parse_block_header(first_block);
        audio_state.data = first_block + GBS_BLOCK_HEADER_SIZE;
        audio_state.nibble_offset = 0;
        audio_state.total_nibbles = (GBS_BLOCK_SIZE - GBS_BLOCK_HEADER_SIZE) * 4;
    }
}

// Advance to next block if needed
static void advance_block_if_needed(void) {
    if (block_state.sample_in_block >= block_state.samples_per_block) {
        block_state.block_index++;
        block_state.sample_in_block = 0;

        if (block_state.block_index >= block_state.total_blocks) {
            audio_state.finished = 1;
            return;
        }

        // Setup new block
        const uint8_t* block = audio_gbs + GBS_HEADER_SIZE +
                               (block_state.block_index * GBS_BLOCK_SIZE);
        parse_block_header(block);
        audio_state.data = block + GBS_BLOCK_HEADER_SIZE;
        audio_state.nibble_offset = 0;
    }
}

// Decode samples with block boundary handling
static void decode_samples_with_blocks(int8_t* dest, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (audio_state.finished) {
            dest[i] = 0;
            continue;
        }

        advance_block_if_needed();

        if (audio_state.finished) {
            dest[i] = 0;
            continue;
        }

        dest[i] = decode_2bit_sample();
        block_state.sample_in_block++;
        audio_state.samples_decoded++;
    }
}

// Timer1 interrupt handler - called when buffer playback completes
static void audio_timer1_handler(void) {
    // Acknowledge interrupt
    REG_IF = IRQ_TIMER1;

    if (audio_state.finished) {
        REG_DMA1CNT = 0;
        return;
    }

    // Swap buffers
    uint8_t play_buffer = audio_state.active_buffer;
    uint8_t decode_buffer = play_buffer ^ 1;
    audio_state.active_buffer = decode_buffer;

    // Start DMA for new play buffer
    REG_DMA1CNT = 0;
    REG_DMA1SAD = (uint32_t)audio_buffer[decode_buffer];
    REG_DMA1DAD = (uint32_t)&REG_FIFO_A;
    REG_DMA1CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;

    // Decode next block into the buffer that just finished playing
    decode_samples_with_blocks(audio_buffer[play_buffer], AUDIO_BUFFER_SAMPLES);
}

// Initialize audio system
static void audio_init(void) {
    // Initialize block decoder
    init_block_decoder();

    if (audio_state.finished) {
        return;
    }

    // Pre-decode both buffers
    decode_samples_with_blocks(audio_buffer[0], AUDIO_BUFFER_SAMPLES);
    decode_samples_with_blocks(audio_buffer[1], AUDIO_BUFFER_SAMPLES);

    audio_state.active_buffer = 0;

    // Enable sound
    REG_SOUNDCNT_X = SOUNDCNT_X_ENABLE;
    REG_SOUNDCNT_H = DSOUNDCTRL_DMG100 | DSOUNDCTRL_A100 |
                     DSOUNDCTRL_AR | DSOUNDCTRL_AL |
                     DSOUNDCTRL_ATIMER(0) | DSOUNDCTRL_ARESET;
    REG_SOUNDCNT_L = 0;

    // Clear FIFO
    for (int i = 0; i < 16; i++) {
        REG_FIFO_A = 0;
    }

    // Setup Timer0 for sample rate
    REG_TM0CNT_H = 0;
    REG_TM0CNT_L = AUDIO_TIMER_RELOAD;
    REG_TM0CNT_H = TIMER_START;

    // Setup Timer1 cascade for buffer swap interrupt
    REG_TM1CNT_H = 0;
    REG_TM1CNT_L = 65536 - AUDIO_BUFFER_SAMPLES;
    REG_TM1CNT_H = TIMER_IRQ | TIMER_CASCADE | TIMER_START;

    // Setup interrupt
    irqSet(IRQ_TIMER1, audio_timer1_handler);
    irqEnable(IRQ_TIMER1);

    // Start DMA
    REG_DMA1CNT = 0;
    REG_DMA1SAD = (uint32_t)audio_buffer[0];
    REG_DMA1DAD = (uint32_t)&REG_FIFO_A;
    REG_DMA1CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;
}

// Shutdown audio system
static void audio_shutdown(void) {
    REG_DMA1CNT = 0;
    REG_TM0CNT_H = 0;
    REG_TM1CNT_H = 0;
    irqDisable(IRQ_TIMER1);
    REG_SOUNDCNT_X = 0;
}

// Restart audio playback
static void audio_restart(void) {
    audio_shutdown();

    // Reset block state
    block_state.block_index = 0;
    block_state.sample_in_block = 0;

    // Re-initialize
    init_block_decoder();

    if (!audio_state.finished) {
        // Pre-decode buffers
        decode_samples_with_blocks(audio_buffer[0], AUDIO_BUFFER_SAMPLES);
        decode_samples_with_blocks(audio_buffer[1], AUDIO_BUFFER_SAMPLES);
        audio_state.active_buffer = 0;

        // Re-enable audio
        REG_SOUNDCNT_X = SOUNDCNT_X_ENABLE;
        REG_SOUNDCNT_H = DSOUNDCTRL_DMG100 | DSOUNDCTRL_A100 |
                         DSOUNDCTRL_AR | DSOUNDCTRL_AL |
                         DSOUNDCTRL_ATIMER(0) | DSOUNDCTRL_ARESET;
        REG_SOUNDCNT_L = 0;

        REG_TM0CNT_H = 0;
        REG_TM0CNT_L = AUDIO_TIMER_RELOAD;
        REG_TM0CNT_H = TIMER_START;

        REG_TM1CNT_H = 0;
        REG_TM1CNT_L = 65536 - AUDIO_BUFFER_SAMPLES;
        REG_TM1CNT_H = TIMER_IRQ | TIMER_CASCADE | TIMER_START;

        irqEnable(IRQ_TIMER1);

        REG_DMA1CNT = 0;
        REG_DMA1SAD = (uint32_t)audio_buffer[0];
        REG_DMA1DAD = (uint32_t)&REG_FIFO_A;
        REG_DMA1CNT = DMA_DST_FIXED | DMA_SRC_INC | DMA_REPEAT | DMA32 | DMA_SPECIAL | DMA_ENABLE;
    }
}

int main(void) {
    // Initialize interrupts
    irqInit();
    irqEnable(IRQ_VBLANK);

    // Initialize console for debug output
    consoleDemoInit();

    iprintf("\x1b[2J");  // Clear screen
    iprintf("GBS Audio Player\n");
    iprintf("================\n\n");

    // Parse and display GBS info
    const GBSHeader* header = (const GBSHeader*)audio_gbs;
    iprintf("File size: %lu bytes\n", (unsigned long)audio_gbs_size);
    iprintf("Mode: %lu\n", (unsigned long)header->mode);
    iprintf("Sample rate: %d Hz\n", AUDIO_SAMPLE_RATE);
    iprintf("Buffer: %d samples\n\n", AUDIO_BUFFER_SAMPLES);

    // Initialize audio
    audio_init();

    if (audio_state.finished) {
        iprintf("Error: Invalid GBS file\n");
    } else {
        uint32_t blocks = block_state.total_blocks;
        uint32_t samples = audio_state.total_samples;
        uint32_t duration = samples / AUDIO_SAMPLE_RATE;

        iprintf("Blocks: %lu\n", (unsigned long)blocks);
        iprintf("Samples: %lu\n", (unsigned long)samples);
        iprintf("Duration: %lu sec\n\n", (unsigned long)duration);
        iprintf("Playing...\n");
        iprintf("Press START to restart\n");
    }

    // Main loop
    while (1) {
        VBlankIntrWait();

        scanKeys();
        uint16_t keys = keysDown();

        if (keys & KEY_START) {
            audio_restart();
            iprintf("\x1b[12;0HRestarted!        ");
        }

        // Display progress
        if (!audio_state.finished) {
            uint32_t progress = (audio_state.samples_decoded * 100) / audio_state.total_samples;
            iprintf("\x1b[10;0HProgress: %lu%%  ", (unsigned long)progress);
        } else {
            iprintf("\x1b[10;0HFinished!         ");
        }
    }

    return 0;
}
