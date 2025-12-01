/*
 * Media Source Implementation
 *
 * Provides media data (GBS audio, GBM video) from GBFS or embedded data.
 */

#include "media_source.h"
#include "../gbfs/gbfs.h"
#include <string.h>

// Internal state
static struct {
    bool initialized;
    MediaSourceType active_type;
    const GBFS_FILE* gbfs;
    uint32_t gbs_count;
    uint32_t gbm_count;
} source_state;

// Check file extension
static bool has_extension(const char* name, const char* ext) {
    size_t name_len = strlen(name);
    size_t ext_len = strlen(ext);
    if (name_len < ext_len + 1) return false;

    const char* file_ext = name + name_len - ext_len;
    if (*(file_ext - 1) != '.') return false;

    // Case-insensitive compare
    for (size_t i = 0; i < ext_len; i++) {
        char c1 = file_ext[i];
        char c2 = ext[i];
        // Lowercase
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return false;
    }
    return true;
}

static bool is_gbs_file(const char* name) {
    return has_extension(name, "gbs");
}

static bool is_gbm_file(const char* name) {
    return has_extension(name, "gbm");
}

static MediaFileType get_file_type(const char* name) {
    if (is_gbs_file(name)) return MEDIA_TYPE_GBS;
    if (is_gbm_file(name)) return MEDIA_TYPE_GBM;
    return MEDIA_TYPE_UNKNOWN;
}

bool media_source_init(void) {
    memset(&source_state, 0, sizeof(source_state));

    // Try to find GBFS archive
    source_state.gbfs = find_first_gbfs_file(find_first_gbfs_file);

    if (source_state.gbfs) {
        // Count GBS and GBM files in GBFS
        size_t total = gbfs_count_objs(source_state.gbfs);

        for (size_t i = 0; i < total; i++) {
            char name[32];
            u32 len;
            if (gbfs_get_nth_obj(source_state.gbfs, i, name, &len)) {
                if (is_gbs_file(name)) {
                    source_state.gbs_count++;
                } else if (is_gbm_file(name)) {
                    source_state.gbm_count++;
                }
            }
        }

        if (source_state.gbs_count > 0 || source_state.gbm_count > 0) {
            source_state.active_type = MEDIA_SOURCE_GBFS;
        }
    }

    source_state.initialized = true;
    return (source_state.active_type != MEDIA_SOURCE_NONE);
}

bool media_source_find_gbs(MediaSourceInfo* info) {
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
                info->source = MEDIA_SOURCE_GBFS;
                info->type = MEDIA_TYPE_GBS;
                info->data = (const uint8_t*)data;
                info->size = len;
                strncpy(info->filename, name, sizeof(info->filename) - 1);
                return true;
            }
        }
    }

    return false;
}

bool media_source_find_gbm(MediaSourceInfo* info) {
    if (!source_state.initialized || !info) {
        return false;
    }

    memset(info, 0, sizeof(*info));

    // Try GBFS first
    if (source_state.gbfs && source_state.gbm_count > 0) {
        size_t total = gbfs_count_objs(source_state.gbfs);

        for (size_t i = 0; i < total; i++) {
            char name[32];
            u32 len;
            const void* data = gbfs_get_nth_obj(source_state.gbfs, i, name, &len);

            if (data && is_gbm_file(name)) {
                info->source = MEDIA_SOURCE_GBFS;
                info->type = MEDIA_TYPE_GBM;
                info->data = (const uint8_t*)data;
                info->size = len;
                strncpy(info->filename, name, sizeof(info->filename) - 1);
                return true;
            }
        }
    }

    return false;
}

bool media_source_load_file(const char* filename, MediaSourceInfo* info) {
    if (!source_state.initialized || !info || !filename) {
        return false;
    }

    memset(info, 0, sizeof(*info));

    // Only GBFS supports loading by name
    if (source_state.gbfs) {
        u32 len;
        const void* data = gbfs_get_obj(source_state.gbfs, filename, &len);

        if (data) {
            info->source = MEDIA_SOURCE_GBFS;
            info->type = get_file_type(filename);
            info->data = (const uint8_t*)data;
            info->size = len;
            strncpy(info->filename, filename, sizeof(info->filename) - 1);
            return true;
        }
    }

    return false;
}

uint32_t media_source_count(MediaFileType type) {
    if (!source_state.initialized) {
        return 0;
    }

    if (source_state.gbfs) {
        if (type == MEDIA_TYPE_GBS) return source_state.gbs_count;
        if (type == MEDIA_TYPE_GBM) return source_state.gbm_count;
    }

    return 0;
}

MediaSourceType media_source_get_type(void) {
    return source_state.active_type;
}
