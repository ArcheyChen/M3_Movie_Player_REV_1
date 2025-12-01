#!/bin/bash
#
# Build GBA ROM with GBFS media archive
#
# Usage: ./build_with_gbfs.sh [file.gbs|file.gbm ...]
#
# If no files specified, uses all .gbs and .gbm files in data/ directory
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
    # Use all .gbs and .gbm files in data/
    MEDIA_FILES=$(find data -name "*.gbs" -o -name "*.gbm" 2>/dev/null | sort)
    if [ -z "$MEDIA_FILES" ]; then
        echo "No media files specified and none found in data/"
        echo "Usage: $0 [file.gbs|file.gbm ...]"
        echo ""
        echo "Supported formats:"
        echo "  .gbs - GBA Sound (audio)"
        echo "  .gbm - GBA Movie (video)"
        exit 1
    fi
else
    MEDIA_FILES="$@"
fi

# Verify files exist
for f in $MEDIA_FILES; do
    if [ ! -f "$f" ]; then
        echo "Error: File not found: $f"
        exit 1
    fi
done

echo "Creating GBFS archive with:"
for f in $MEDIA_FILES; do
    # Show file type
    case "${f##*.}" in
        gbs|GBS) echo "  [Audio] $f" ;;
        gbm|GBM) echo "  [Video] $f" ;;
        *)       echo "  [?????] $f" ;;
    esac
done

# Output filename
OUTPUT_ROM="gba_media_player.gba"

# Copy ROM to output file (preserve original)
echo "Copying ROM to $OUTPUT_ROM..."
cp gba_audio_decoder.gba "$OUTPUT_ROM"

# Pad output ROM to 256-byte boundary (required for GBFS search)
echo "Padding ROM..."
$PADBIN_CMD 256 "$OUTPUT_ROM"

# Create GBFS archive
echo "Creating GBFS archive..."
$GBFS_CMD media_data.gbfs $MEDIA_FILES

# Append GBFS to output ROM
echo "Appending GBFS to ROM..."
cat media_data.gbfs >> "$OUTPUT_ROM"

# Clean up
rm -f media_data.gbfs

# Show result
echo ""
echo "Done! Output: $OUTPUT_ROM"
echo "(Original gba_audio_decoder.gba preserved)"
ls -la "$OUTPUT_ROM"
