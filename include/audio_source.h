/*
 * Audio Source Abstraction Layer
 *
 * Provides a unified interface for loading audio data from different sources:
 * - Embedded data (compiled into ROM)
 * - GBFS (appended to ROM)
 * - SD card (future)
 *
 * This abstraction allows easy switching between backends without changing
 * the audio player code.
 */

#ifndef AUDIO_SOURCE_H
#define AUDIO_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

// Audio source types
typedef enum {
    AUDIO_SOURCE_NONE = 0,
    AUDIO_SOURCE_EMBEDDED,  // Compiled into ROM via bin2o
    AUDIO_SOURCE_GBFS,      // GBFS filesystem appended to ROM
    AUDIO_SOURCE_SDCARD     // SD card (future)
} AudioSourceType;

// Audio source info
typedef struct {
    AudioSourceType type;
    const uint8_t* data;    // Pointer to audio data (NULL if not loaded)
    uint32_t size;          // Size of audio data
    char filename[32];      // Filename (for GBFS/SD)
} AudioSourceInfo;

/*
 * Initialize the audio source system.
 * Call this once at startup.
 *
 * @return true if initialization successful
 */
bool audio_source_init(void);

/*
 * Find and load the first available GBS audio file.
 * Searches in order: GBFS, then embedded data.
 *
 * @param info  Output: filled with source information
 * @return      true if audio found
 */
bool audio_source_find_gbs(AudioSourceInfo* info);

/*
 * Load a specific GBS file by name (GBFS/SD only).
 *
 * @param filename  Name of the file to load
 * @param info      Output: filled with source information
 * @return          true if file found
 */
bool audio_source_load_gbs(const char* filename, AudioSourceInfo* info);

/*
 * Get the number of GBS files available (GBFS/SD).
 *
 * @return Number of GBS files, 0 if using embedded data
 */
uint32_t audio_source_count_gbs(void);

/*
 * Get info about the nth GBS file (GBFS/SD).
 *
 * @param index     File index (0-based)
 * @param info      Output: filled with source information
 * @return          true if index valid
 */
bool audio_source_get_gbs_by_index(uint32_t index, AudioSourceInfo* info);

/*
 * Get the currently active source type.
 */
AudioSourceType audio_source_get_type(void);

#endif // AUDIO_SOURCE_H
