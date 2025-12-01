/*
 * Audio Source Implementation
 *
 * Provides audio data from GBFS or embedded data.
 */

#include "audio_source.h"
#include "../gbfs/gbfs.h"
#include <string.h>

// Try to include embedded audio if available
// This will be defined by bin2o if data/audio.gbs exists
#ifdef USE_EMBEDDED_AUDIO
#include "audio_gbs.h"
#endif

// Internal state
static struct {
    bool initialized;
    AudioSourceType active_type;
    const GBFS_FILE* gbfs;
    uint32_t gbs_count;
} source_state;

// Check if filename has .gbs extension
static bool is_gbs_file(const char* name) {
    size_t len = strlen(name);
    if (len < 4) return false;
    const char* ext = name + len - 4;
    return (ext[0] == '.' &&
            (ext[1] == 'g' || ext[1] == 'G') &&
            (ext[2] == 'b' || ext[2] == 'B') &&
            (ext[3] == 's' || ext[3] == 'S'));
}

bool audio_source_init(void) {
    memset(&source_state, 0, sizeof(source_state));

    // Try to find GBFS archive
    // Search starting from a location likely after our code
    source_state.gbfs = find_first_gbfs_file(find_first_gbfs_file);

    if (source_state.gbfs) {
        // Count GBS files in GBFS
        size_t total = gbfs_count_objs(source_state.gbfs);
        source_state.gbs_count = 0;

        for (size_t i = 0; i < total; i++) {
            char name[32];
            u32 len;
            if (gbfs_get_nth_obj(source_state.gbfs, i, name, &len)) {
                if (is_gbs_file(name)) {
                    source_state.gbs_count++;
                }
            }
        }

        if (source_state.gbs_count > 0) {
            source_state.active_type = AUDIO_SOURCE_GBFS;
        }
    }

    // Fall back to embedded if no GBFS GBS files
    if (source_state.active_type == AUDIO_SOURCE_NONE) {
#ifdef USE_EMBEDDED_AUDIO
        source_state.active_type = AUDIO_SOURCE_EMBEDDED;
#endif
    }

    source_state.initialized = true;
    return (source_state.active_type != AUDIO_SOURCE_NONE);
}

bool audio_source_find_gbs(AudioSourceInfo* info) {
    if (!source_state.initialized || !info) {
        return false;
    }

    memset(info, 0, sizeof(*info));

    // Try GBFS first
    if (source_state.gbfs && source_state.gbs_count > 0) {
        size_t total = gbfs_count_objs(source_state.gbfs);

        for (size_t i = 0; i < total; i++) {
            char name[32];
            u32 len;
            const void* data = gbfs_get_nth_obj(source_state.gbfs, i, name, &len);

            if (data && is_gbs_file(name)) {
                info->type = AUDIO_SOURCE_GBFS;
                info->data = (const uint8_t*)data;
                info->size = len;
                strncpy(info->filename, name, sizeof(info->filename) - 1);
                return true;
            }
        }
    }

    // Fall back to embedded
#ifdef USE_EMBEDDED_AUDIO
    info->type = AUDIO_SOURCE_EMBEDDED;
    info->data = audio_gbs;
    info->size = audio_gbs_size;
    strncpy(info->filename, "embedded", sizeof(info->filename) - 1);
    return true;
#else
    return false;
#endif
}

bool audio_source_load_gbs(const char* filename, AudioSourceInfo* info) {
    if (!source_state.initialized || !info || !filename) {
        return false;
    }

    memset(info, 0, sizeof(*info));

    // Only GBFS supports loading by name
    if (source_state.gbfs) {
        u32 len;
        const void* data = gbfs_get_obj(source_state.gbfs, filename, &len);

        if (data && is_gbs_file(filename)) {
            info->type = AUDIO_SOURCE_GBFS;
            info->data = (const uint8_t*)data;
            info->size = len;
            strncpy(info->filename, filename, sizeof(info->filename) - 1);
            return true;
        }
    }

    return false;
}

uint32_t audio_source_count_gbs(void) {
    if (!source_state.initialized) {
        return 0;
    }

    if (source_state.gbfs) {
        return source_state.gbs_count;
    }

#ifdef USE_EMBEDDED_AUDIO
    return 1;  // Embedded counts as 1 file
#else
    return 0;
#endif
}

bool audio_source_get_gbs_by_index(uint32_t index, AudioSourceInfo* info) {
    if (!source_state.initialized || !info) {
        return false;
    }

    memset(info, 0, sizeof(*info));

    if (source_state.gbfs && source_state.gbs_count > 0) {
        // Find the nth GBS file
        size_t total = gbfs_count_objs(source_state.gbfs);
        uint32_t gbs_index = 0;

        for (size_t i = 0; i < total; i++) {
            char name[32];
            u32 len;
            const void* data = gbfs_get_nth_obj(source_state.gbfs, i, name, &len);

            if (data && is_gbs_file(name)) {
                if (gbs_index == index) {
                    info->type = AUDIO_SOURCE_GBFS;
                    info->data = (const uint8_t*)data;
                    info->size = len;
                    strncpy(info->filename, name, sizeof(info->filename) - 1);
                    return true;
                }
                gbs_index++;
            }
        }
    }

#ifdef USE_EMBEDDED_AUDIO
    // Embedded as index 0 if no GBFS
    if (index == 0 && source_state.gbs_count == 0) {
        info->type = AUDIO_SOURCE_EMBEDDED;
        info->data = audio_gbs;
        info->size = audio_gbs_size;
        strncpy(info->filename, "embedded", sizeof(info->filename) - 1);
        return true;
    }
#endif

    return false;
}

AudioSourceType audio_source_get_type(void) {
    return source_state.active_type;
}
