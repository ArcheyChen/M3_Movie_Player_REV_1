/*
 * GBS Audio Player - Test Program
 *
 * Simple test/demo for the GBS audio library.
 */

#include <gba_console.h>
#include <gba_interrupt.h>
#include <gba_input.h>
#include <gba_systemcalls.h>
#include <gba_video.h>
#include <stdio.h>

#include "gbs_audio.h"
#include "audio_gbs.h"

static const char* mode_names[] = {
    "Stereo 4-bit",
    "Mono 3-bit",
    "Mono 4-bit",
    "Mono 2-bit",
    "Mono 2-bit SM",
    "Invalid"
};

int main(void) {
    // Initialize interrupts
    irqInit();
    irqEnable(IRQ_VBLANK);

    // Initialize console
    consoleDemoInit();

    iprintf("\x1b[2J");  // Clear screen
    iprintf("GBS Audio Player\n");
    iprintf("================\n\n");

    // Initialize audio
    if (!gbs_audio_init(audio_gbs, audio_gbs_size)) {
        iprintf("Error: Failed to init GBS\n");
        while (1) VBlankIntrWait();
    }

    // Display info
    const GbsAudioInfo* info = gbs_audio_get_info();

    iprintf("File: %lu bytes\n", (unsigned long)audio_gbs_size);
    iprintf("Mode: %d (%s)\n", info->mode, mode_names[info->mode]);
    iprintf("Rate: %lu Hz\n", (unsigned long)info->sample_rate);
    iprintf("Channels: %d\n", info->channels);
    iprintf("Blocks: %lu\n", (unsigned long)info->total_blocks);

    uint32_t duration = info->total_samples / info->sample_rate;
    iprintf("Duration: %lu sec\n\n", (unsigned long)duration);

    iprintf("Controls:\n");
    iprintf("  START: Restart\n");
    iprintf("  SELECT: Stop/Play\n\n");

    // Start playback
    gbs_audio_start();
    iprintf("Status: Playing\n");

    // Main loop
    while (1) {
        VBlankIntrWait();

        scanKeys();
        uint16_t keys = keysDown();

        if (keys & KEY_START) {
            gbs_audio_restart();
            iprintf("\x1b[14;0HStatus: Playing   ");
        }

        if (keys & KEY_SELECT) {
            if (gbs_audio_is_playing()) {
                gbs_audio_stop();
                iprintf("\x1b[14;0HStatus: Stopped   ");
            } else if (!gbs_audio_is_finished()) {
                gbs_audio_start();
                iprintf("\x1b[14;0HStatus: Playing   ");
            }
        }

        // Update progress
        uint32_t progress = gbs_audio_get_progress();
        if (gbs_audio_is_finished()) {
            iprintf("\x1b[15;0HProgress: Done!   ");
        } else {
            iprintf("\x1b[15;0HProgress: %lu%%    ", (unsigned long)progress);
        }
    }

    return 0;
}
