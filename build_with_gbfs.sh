#!/bin/bash
#
# Build GBA ROM with GBFS audio archive
#
# Usage: ./build_with_gbfs.sh [audio.gbs ...]
#
# If no files specified, uses all .gbs files in data/ directory
#

set -e

GBFS_TOOLS="../gbfs/tools"
GBFS_CMD="$GBFS_TOOLS/gbfs"
PADBIN_CMD="$GBFS_TOOLS/padbin"

# Check if gbfs tools exist
if [ ! -x "$GBFS_CMD" ]; then
    echo "Error: gbfs tool not found at $GBFS_CMD"
    echo "Please build the tools: cd $GBFS_TOOLS && sh mktools.sh"
    exit 1
fi

# Build the ROM first
echo "Building ROM..."
make

# Get input files
if [ $# -eq 0 ]; then
    # Use all .gbs files in data/
    GBS_FILES=$(find data -name "*.gbs" 2>/dev/null)
    if [ -z "$GBS_FILES" ]; then
        echo "No .gbs files specified and none found in data/"
        echo "Usage: $0 [audio.gbs ...]"
        exit 1
    fi
else
    GBS_FILES="$@"
fi

# Verify files exist
for f in $GBS_FILES; do
    if [ ! -f "$f" ]; then
        echo "Error: File not found: $f"
        exit 1
    fi
done

echo "Creating GBFS archive with:"
for f in $GBS_FILES; do
    echo "  - $f"
done

# Pad ROM to 256-byte boundary (required for GBFS search)
echo "Padding ROM..."
$PADBIN_CMD 256 gba_audio_decoder.gba

# Create GBFS archive
echo "Creating GBFS archive..."
$GBFS_CMD audio_data.gbfs $GBS_FILES

# Append GBFS to ROM
echo "Appending GBFS to ROM..."
cat audio_data.gbfs >> gba_audio_decoder.gba

# Clean up
rm -f audio_data.gbfs

# Show result
echo ""
echo "Done! Output: gba_audio_decoder.gba"
ls -la gba_audio_decoder.gba
