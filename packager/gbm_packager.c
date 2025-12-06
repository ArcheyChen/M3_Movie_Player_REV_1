/*
 * GBM Packager - Standalone tool to create GBA movie ROMs
 *
 * Embeds M3_Movie_Player.gba and packages user-provided
 * .gbm and .gbs files into a playable GBA ROM.
 *
 * Usage:
 *   gbm_packager input.gbm input.gbs              -> generates input.gba
 *   gbm_packager output.gba input.gbm input.gbs   -> generates output.gba
 *
 * Drag & drop: drag both .gbm and .gbs files onto the exe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif

#include "embedded_data.h"

#define GBFS_MAGIC "PinEightGBFS\r\n\x1a\n"
#define GBFS_MAGIC_LEN 16
#define GBFS_NAME_LEN 24

typedef struct {
    char magic[16];
    uint32_t total_len;
    uint16_t dir_off;
    uint16_t dir_nmemb;
    char reserved[8];
} __attribute__((packed)) GBFSHeader;

typedef struct {
    char name[24];
    uint32_t len;
    uint32_t data_offset;
} __attribute__((packed)) GBFSEntry;

static uint32_t align4(uint32_t x) {
    return (x + 3) & ~3;
}

// Check if string ends with suffix (case insensitive)
static int ends_with(const char* str, const char* suffix) {
    size_t str_len = strlen(str);
    size_t suf_len = strlen(suffix);
    if (suf_len > str_len) return 0;
    const char* end = str + str_len - suf_len;
    for (size_t i = 0; i < suf_len; i++) {
        char c1 = end[i], c2 = suffix[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
    }
    return 1;
}

// Generate unique output filename (avoid overwriting)
static void make_unique_path(char* path, size_t max_len) {
    if (access(path, F_OK) != 0) return;  // File doesn't exist, OK

    // Find extension
    char* dot = strrchr(path, '.');
    char base[512], ext[32] = "";
    if (dot) {
        size_t base_len = dot - path;
        strncpy(base, path, base_len);
        base[base_len] = '\0';
        strncpy(ext, dot, sizeof(ext) - 1);
    } else {
        strncpy(base, path, sizeof(base) - 1);
    }

    // Try _1, _2, etc.
    for (int i = 1; i < 1000; i++) {
        snprintf(path, max_len, "%s_%d%s", base, i, ext);
        if (access(path, F_OK) != 0) return;
    }
}

static int create_gbfs(const char* gbm_path, const char* gbs_path,
                       uint8_t** out_data, uint32_t* out_size) {
    FILE* gbm = fopen(gbm_path, "rb");
    FILE* gbs = fopen(gbs_path, "rb");

    if (!gbm || !gbs) {
        if (gbm) fclose(gbm);
        if (gbs) fclose(gbs);
        return -1;
    }

    fseek(gbm, 0, SEEK_END);
    uint32_t gbm_size = ftell(gbm);
    fseek(gbm, 0, SEEK_SET);

    fseek(gbs, 0, SEEK_END);
    uint32_t gbs_size = ftell(gbs);
    fseek(gbs, 0, SEEK_SET);

    uint32_t header_size = sizeof(GBFSHeader);
    uint32_t dir_size = 2 * sizeof(GBFSEntry);
    uint32_t data_start = align4(header_size + dir_size);
    uint32_t gbm_offset = data_start;
    uint32_t gbs_offset = align4(gbm_offset + gbm_size);
    uint32_t total_size = align4(gbs_offset + gbs_size);

    uint8_t* data = calloc(1, total_size);
    if (!data) {
        fclose(gbm);
        fclose(gbs);
        return -1;
    }

    GBFSHeader* hdr = (GBFSHeader*)data;
    memcpy(hdr->magic, GBFS_MAGIC, GBFS_MAGIC_LEN);
    hdr->total_len = total_size;
    hdr->dir_off = header_size;
    hdr->dir_nmemb = 2;

    GBFSEntry* entries = (GBFSEntry*)(data + header_size);
    strncpy(entries[0].name, "movie.gbm", GBFS_NAME_LEN);
    entries[0].len = gbm_size;
    entries[0].data_offset = gbm_offset;
    strncpy(entries[1].name, "movie.gbs", GBFS_NAME_LEN);
    entries[1].len = gbs_size;
    entries[1].data_offset = gbs_offset;

    fread(data + gbm_offset, 1, gbm_size, gbm);
    fread(data + gbs_offset, 1, gbs_size, gbs);

    fclose(gbm);
    fclose(gbs);

    *out_data = data;
    *out_size = total_size;
    return 0;
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Ausar's GBM Packager V0.2 - Create GBA movie ROMs\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s input.gbm input.gbs              (auto-generates input.gba)\n", prog);
    fprintf(stderr, "  %s output.gba input.gbm input.gbs   (explicit output name)\n", prog);
    fprintf(stderr, "\nDrag & drop: drag both .gbm and .gbs files onto this exe\n");
}

int main(int argc, char** argv) {
    const char* output_path = NULL;
    const char* gbm_path = NULL;
    const char* gbs_path = NULL;
    char auto_output[512] = {0};

    if (argc == 3) {
        // Auto mode: two input files, determine which is which
        for (int i = 1; i <= 2; i++) {
            if (ends_with(argv[i], ".gbm")) {
                gbm_path = argv[i];
            } else if (ends_with(argv[i], ".gbs")) {
                gbs_path = argv[i];
            }
        }
        if (!gbm_path || !gbs_path) {
            fprintf(stderr, "Error: Need one .gbm and one .gbs file\n");
            print_usage(argv[0]);
            return 1;
        }
        // Generate output name from gbm file
        strncpy(auto_output, gbm_path, sizeof(auto_output) - 5);
        char* dot = strrchr(auto_output, '.');
        if (dot) *dot = '\0';
        strcat(auto_output, ".gba");
        make_unique_path(auto_output, sizeof(auto_output));
        output_path = auto_output;
    } else if (argc == 4) {
        // Explicit mode: output.gba input.gbm input.gbs
        output_path = argv[1];
        gbm_path = argv[2];
        gbs_path = argv[3];
    } else {
        print_usage(argv[0]);
        return 1;
    }

    uint8_t* gbfs_data = NULL;
    uint32_t gbfs_size = 0;

    if (create_gbfs(gbm_path, gbs_path, &gbfs_data, &gbfs_size) != 0) {
        fprintf(stderr, "Error: Failed to read input files\n");
        fprintf(stderr, "  GBM: %s\n", gbm_path);
        fprintf(stderr, "  GBS: %s\n", gbs_path);
        return 1;
    }

    uint32_t gba_size = embedded_gba_size;
    uint32_t padded_gba = (gba_size + 255) & ~255;
    uint32_t total_size = padded_gba + gbfs_size;

    FILE* out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "Error: Cannot create output file: %s\n", output_path);
        free(gbfs_data);
        return 1;
    }

    fwrite(embedded_gba_data, 1, gba_size, out);

    uint8_t padding[256] = {0};
    uint32_t pad_size = padded_gba - gba_size;
    if (pad_size > 0) {
        fwrite(padding, 1, pad_size, out);
    }

    fwrite(gbfs_data, 1, gbfs_size, out);

    fclose(out);
    free(gbfs_data);

    printf("Created: %s (%u bytes)\n", output_path, total_size);
    return 0;
}
