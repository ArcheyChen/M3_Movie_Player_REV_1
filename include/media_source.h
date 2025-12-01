/*
 * Media Source Abstraction Layer
 *
 * Provides a unified interface for loading media data from different sources:
 * - GBFS (appended to ROM)
 * - Embedded data (compiled into ROM)
 * - SD card (future)
 *
 * Supports both GBS (audio) and GBM (video) files.
 */

#ifndef MEDIA_SOURCE_H
#define MEDIA_SOURCE_H

#include <stdbool.h>
#include <stdint.h>

// Media source types
typedef enum {
    MEDIA_SOURCE_NONE = 0,
    MEDIA_SOURCE_EMBEDDED,  // Compiled into ROM via bin2o
    MEDIA_SOURCE_GBFS,      // GBFS filesystem appended to ROM
    MEDIA_SOURCE_SDCARD     // SD card (future)
} MediaSourceType;

// Media file types
typedef enum {
    MEDIA_TYPE_UNKNOWN = 0,
    MEDIA_TYPE_GBS,         // Audio file
    MEDIA_TYPE_GBM          // Video file
} MediaFileType;

// Media source info
typedef struct {
    MediaSourceType source;
    MediaFileType type;
    const uint8_t* data;    // Pointer to media data (NULL if not loaded)
    uint32_t size;          // Size of media data
    char filename[32];      // Filename (for GBFS/SD)
} MediaSourceInfo;

/*
 * Initialize the media source system.
 * Call this once at startup.
 *
 * @return true if initialization successful
 */
bool media_source_init(void);

/*
 * Find and load the first available GBS audio file.
 * Searches in order: GBFS, then embedded data.
 *
 * @param info  Output: filled with source information
 * @return      true if audio found
 */
bool media_source_find_gbs(MediaSourceInfo* info);

/*
 * Find and load the first available GBM video file.
 * Searches in order: GBFS, then embedded data.
 *
 * @param info  Output: filled with source information
 * @return      true if video found
 */
bool media_source_find_gbm(MediaSourceInfo* info);

/*
 * Load a specific file by name (GBFS/SD only).
 *
 * @param filename  Name of the file to load
 * @param info      Output: filled with source information
 * @return          true if file found
 */
bool media_source_load_file(const char* filename, MediaSourceInfo* info);

/*
 * Get the number of media files available (GBFS/SD).
 *
 * @param type  File type to count (MEDIA_TYPE_GBS or MEDIA_TYPE_GBM)
 * @return      Number of files, 0 if using embedded data
 */
uint32_t media_source_count(MediaFileType type);

/*
 * Get the currently active source type.
 */
MediaSourceType media_source_get_type(void);

#endif // MEDIA_SOURCE_H
