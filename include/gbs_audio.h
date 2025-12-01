/*
 * GBS Audio Decoder for GBA
 *
 * Public interface for GBS audio playback.
 * Supports all 5 GBS modes from the M3 Movie Player.
 */

#ifndef GBS_AUDIO_H
#define GBS_AUDIO_H

#include <gba_base.h>
#include <stdbool.h>
#include <stdint.h>

// GBS audio modes
typedef enum {
    GBS_MODE_STEREO_4BIT   = 0,  // Stereo 4-bit IMA ADPCM, 22050 Hz, block 0x400
    GBS_MODE_MONO_3BIT     = 1,  // Mono 3-bit ADPCM, 11025 Hz, block 0x400
    GBS_MODE_MONO_4BIT     = 2,  // Mono 4-bit IMA ADPCM, 11025 Hz, block 0x200
    GBS_MODE_MONO_2BIT     = 3,  // Mono 2-bit ADPCM, 22050 Hz, block 0x200
    GBS_MODE_MONO_2BIT_SM  = 4,  // Mono 2-bit ADPCM, 22050 Hz, block 0x100 (small)
    GBS_MODE_INVALID       = 255
} GbsMode;

// Audio state (read-only for external use)
typedef struct {
    GbsMode mode;
    uint32_t sample_rate;
    uint8_t channels;           // 1=mono, 2=stereo
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t total_samples;     // Per channel for stereo
    uint32_t samples_decoded;
    bool is_playing;
    bool is_finished;
} GbsAudioInfo;

/*
 * Initialize the GBS audio system with embedded data.
 *
 * @param gbs_data    Pointer to GBS file data (typically from bin2o)
 * @param gbs_size    Size of GBS data in bytes
 * @return            true if initialization successful
 */
bool gbs_audio_init(const uint8_t* gbs_data, uint32_t gbs_size);

/*
 * Start audio playback.
 * Call this after gbs_audio_init().
 */
void gbs_audio_start(void);

/*
 * Stop audio playback.
 */
void gbs_audio_stop(void);

/*
 * Restart playback from beginning.
 */
void gbs_audio_restart(void);

/*
 * Check if audio is currently playing.
 */
bool gbs_audio_is_playing(void);

/*
 * Check if audio playback has finished.
 */
bool gbs_audio_is_finished(void);

/*
 * Get current playback progress (0-100).
 */
uint32_t gbs_audio_get_progress(void);

/*
 * Get audio information.
 * Returns pointer to internal state (do not modify).
 */
const GbsAudioInfo* gbs_audio_get_info(void);

/*
 * Shutdown audio system and release resources.
 */
void gbs_audio_shutdown(void);

#endif // GBS_AUDIO_H
