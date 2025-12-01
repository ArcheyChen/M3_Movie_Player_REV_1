#ifndef GBM_DECODER_H
#define GBM_DECODER_H

#include <gba_types.h>

#define FRAME_WIDTH 240
#define FRAME_HEIGHT 160
#define GBM_HEADER_SIZE 0x200

#define IWRAM_CODE __attribute__((section(".iwram"), long_call))

// Context for decoding a single frame
typedef struct {
    u32 state;
    const u8 *flag_ptr; // Current position in flag stream (must be 4-byte aligned reads)
    const u8 *flag_end;

    const u8 *palette_ptr; // Current position in palette stream
    const u8 *palette_end;

    const u8 *payload_ptr; // Current position in payload stream
    const u8 *payload_end;

    u16 *dst;       // Destination buffer (current frame)
    const u16 *ref; // Reference buffer (previous frame)

    int row_offset;   // Current macroblock row offset in bytes
    int block_offset; // Current block offset in bytes
} DecodeContext;

// Initialize decoder (copies codebook to IWRAM)
// Call once at startup
void gbm_init(void);

// Decode a frame
// returns the offset of the next frame, or 0 on error
u32 gbm_decode_frame(const u8 *data, u32 offset, u16 *dst, const u16 *ref);

#endif // GBM_DECODER_H
