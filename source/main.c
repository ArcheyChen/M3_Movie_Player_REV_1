/*
 * GBS Audio Player - Test Program
 *
 * Simple test/demo for the GBS audio library.
 * Supports loading audio from GBFS or embedded data.
 */

#include <gba_console.h>
#include <gba_interrupt.h>
#include <gba_input.h>
#include <gba_systemcalls.h>
#include <gba_video.h>
#include <stdio.h>

#include "gbs_audio.h"
#include "audio_source.h"

static const char* mode_names[] = {
    "Stereo 4-bit",
    "Mono 3-bit",
    "Mono 4-bit",
    "Mono 2-bit",
    "Mono 2-bit SM",
    "Invalid"
};

static const char* source_names[] = {
    "None",
    "Embedded",
    "GBFS",
    "SD Card"
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

    // Initialize audio source
    if (!audio_source_init()) {
        iprintf("Error: No audio source!\n");
        iprintf("Append GBFS with .gbs file\n");
        while (1) VBlankIntrWait();
    }

    // Find first GBS file
    AudioSourceInfo source_info;
    if (!audio_source_find_gbs(&source_info)) {
        iprintf("Error: No GBS file found!\n");
        while (1) VBlankIntrWait();
    }

    iprintf("Source: %s\n", source_names[source_info.type]);
    iprintf("File: %s\n", source_info.filename);
    iprintf("Size: %lu bytes\n\n", (unsigned long)source_info.size);

    // Initialize audio
    if (!gbs_audio_init(source_info.data, source_info.size)) {
        iprintf("Error: Invalid GBS file!\n");
        while (1) VBlankIntrWait();
    }

    // Display info
    const GbsAudioInfo* info = gbs_audio_get_info();

    iprintf("Mode: %d (%s)\n", info->mode, mode_names[info->mode]);
    iprintf("Rate: %lu Hz\n", (unsigned long)info->sample_rate);
    iprintf("Channels: %d\n", info->channels);

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
            iprintf("\x1b[16;0HStatus: Playing   ");
        }

        if (keys & KEY_SELECT) {
            if (gbs_audio_is_playing()) {
                gbs_audio_stop();
                iprintf("\x1b[16;0HStatus: Stopped   ");
            } else if (!gbs_audio_is_finished()) {
                gbs_audio_start();
                iprintf("\x1b[16;0HStatus: Playing   ");
            }
        }

        // Update progress
        uint32_t progress = gbs_audio_get_progress();
        if (gbs_audio_is_finished()) {
            iprintf("\x1b[17;0HProgress: Done!   ");
        } else {
            iprintf("\x1b[17;0HProgress: %lu%%    ", (unsigned long)progress);
        }
    }

    return 0;
}
