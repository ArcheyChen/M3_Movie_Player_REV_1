/*
 * GBA Media Player
 *
 * Combined video (GBM) and audio (GBS) player.
 * Loads media files from GBFS filesystem.
 *
 * Note: Currently no A/V sync - just plays both independently.
 */

#include <gba.h>
#include <gba_console.h>
#include <gba_interrupt.h>
#include <gba_input.h>
#include <gba_systemcalls.h>
#include <gba_video.h>
#include <stdio.h>
#include <string.h>

#include "media_source.h"
#include "gbs_audio.h"
#include "gbm_decoder.h"

// Fast frame copy using ARM ldmia/stmia
// Unlike DMA, CPU memory access doesn't monopolize the bus and won't
// starve audio FIFO DMA. Audio DMA has higher priority and can interleave.
//
// Copies 128 bytes per iteration (4x unrolled, 32 bytes each).
// Total: 76800 bytes = 600 iterations.
__attribute__((target("arm"), noinline))
static void copy_frame_to_vram(const void* src, void* dst, u32 size) {
    asm volatile(
        "1:                         \n"
        "   ldmia %[src]!, {r2-r9}  \n"
        "   stmia %[dst]!, {r2-r9}  \n"
        "   ldmia %[src]!, {r2-r9}  \n"
        "   stmia %[dst]!, {r2-r9}  \n"
        "   ldmia %[src]!, {r2-r9}  \n"
        "   stmia %[dst]!, {r2-r9}  \n"
        "   ldmia %[src]!, {r2-r9}  \n"
        "   stmia %[dst]!, {r2-r9}  \n"
        "   subs  %[size], %[size], #128 \n"
        "   bgt   1b                \n"
        : [src] "+r" (src), [dst] "+r" (dst), [size] "+r" (size)
        :
        : "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "memory", "cc"
    );
}

// EWRAM buffer for video frame (240 * 160 = 38400 pixels)
__attribute__((section(".ewram"))) u16 frame_buffer[38400];

// State
static bool has_video = false;
static bool has_audio = false;
static const uint8_t* video_data = NULL;
static uint32_t video_offset = GBM_HEADER_SIZE;
static uint32_t video_size = 0;

// Frame rate control
// Video is 10 FPS, VBlank is 60 Hz, so 1 frame = 6 VBlanks
#define VBLANKS_PER_FRAME 6

// target_frame: incremented by VBlank ISR, represents "should have displayed this many frames"
// current_frame: maintained by main loop, represents "have decoded this many frames"
static volatile u32 target_frame = 0;
static u32 current_frame = 0;

static void vblank_handler(void) {
    // Called at 60 Hz, increment target_frame every 6 VBlanks (10 FPS)
    static u8 vblank_counter = 0;
    vblank_counter++;
    if (vblank_counter >= VBLANKS_PER_FRAME) {
        vblank_counter = 0;
        target_frame++;
    }
}

static void show_error(const char* msg) {
    consoleDemoInit();
    iprintf("\x1b[2J");
    iprintf("GBA Media Player\n");
    iprintf("================\n\n");
    iprintf("Error: %s\n", msg);
    while (1) VBlankIntrWait();
}

static void show_info(void) {
    iprintf("\x1b[2J");
    iprintf("GBA Media Player\n");
    iprintf("================\n\n");

    if (has_video) {
        iprintf("Video: Yes (%lu KB)\n", (unsigned long)(video_size / 1024));
    } else {
        iprintf("Video: Not found\n");
    }

    if (has_audio) {
        const GbsAudioInfo* info = gbs_audio_get_info();
        uint32_t duration = info->total_samples / info->sample_rate;
        iprintf("Audio: Mode %d, %lu sec\n", info->mode, (unsigned long)duration);
    } else {
        iprintf("Audio: Not found\n");
    }

    iprintf("\nStarting playback...\n");
}

static void init_video_display(void) {
    // Mode 3: 240x160, 15-bit color
    SetMode(MODE_3 | BG2_ENABLE);

    // Clear buffers
    memset(frame_buffer, 0, sizeof(frame_buffer));

    // Clear VRAM
    u16* vram = (u16*)0x06000000;
    for (int i = 0; i < 38400; i++) {
        vram[i] = 0;
    }
}

// Decode next frame into frame_buffer (does not display)
static void decode_next_frame(void) {
    if (!has_video || !video_data) return;

    // Check for end of video
    if (video_offset + 2 >= video_size) {
        // Loop video
        video_offset = GBM_HEADER_SIZE;
    }

    // Read frame length
    uint16_t frame_len = video_data[video_offset] | (video_data[video_offset + 1] << 8);

    // Check for invalid frame
    if (frame_len == 0 || frame_len == 0xFFFF) {
        video_offset = GBM_HEADER_SIZE;
        frame_len = video_data[video_offset] | (video_data[video_offset + 1] << 8);
    }

    // Decode frame (dst = EWRAM buffer, ref = VRAM for delta)
    video_offset = gbm_decode_frame(video_data, video_offset, frame_buffer, (const u16*)0x06000000);
}

// Process video frames with frame rate control
// Flow: decode -> wait for timing -> display -> repeat
static void process_video(void) {
    // Decode next frame first (into frame_buffer)
    decode_next_frame();

    // Wait until it's time to display
    while (current_frame >= target_frame) {
        VBlankIntrWait();
    }

    // Display the pre-decoded frame
    copy_frame_to_vram(frame_buffer, (void*)0x06000000, 240 * 160 * 2);
    current_frame++;
}

int main(void) {
    // Initialize interrupts
    irqInit();
    irqSet(IRQ_VBLANK, vblank_handler);
    irqEnable(IRQ_VBLANK);

    // Initialize media source
    if (!media_source_init()) {
        show_error("No GBFS found!\nAppend media with GBFS.");
    }

    // Try to load video
    MediaSourceInfo video_info;
    if (media_source_find_gbm(&video_info)) {
        // Validate GBM header
        if (video_info.size >= GBM_HEADER_SIZE &&
            video_info.data[0] == 'G' && video_info.data[1] == 'B' &&
            video_info.data[2] == 'A' && video_info.data[3] == 'M') {
            has_video = true;
            video_data = video_info.data;
            video_size = video_info.size;
        }
    }

    // Try to load audio
    MediaSourceInfo audio_info;
    if (media_source_find_gbs(&audio_info)) {
        if (gbs_audio_init(audio_info.data, audio_info.size)) {
            has_audio = true;
        }
    }

    // Must have at least one media type
    if (!has_video && !has_audio) {
        show_error("No media files found!\nAdd .gbm or .gbs files.");
    }

    // Show info briefly
    consoleDemoInit();
    show_info();

    // Wait a moment to show info
    for (int i = 0; i < 30; i++) {
        VBlankIntrWait();
    }

    // Start playback
    if (has_video) {
        init_video_display();
    }

    if (has_audio) {
        gbs_audio_start();
    }

    // Reset frame counters
    target_frame = 0;
    current_frame = 0;

    // Main loop
    while (1) {
        if (has_video) {
            process_video();
        } else {
            // Audio only - just wait for VBlank
            VBlankIntrWait();
        }

        // Handle audio looping
        if (has_audio && gbs_audio_is_finished()) {
            gbs_audio_restart();
        }

        // Check for restart (START button)
        scanKeys();
        if (keysDown() & KEY_START) {
            if (has_video) {
                video_offset = GBM_HEADER_SIZE;
                target_frame = 0;
                current_frame = 0;
            }
            if (has_audio) {
                gbs_audio_restart();
            }
        }
    }

    return 0;
}
