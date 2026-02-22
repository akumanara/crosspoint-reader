#!/bin/bash
# Generate .cpf (CrossPoint Font) files for SD card loading.
#
# Usage:
#   ./generate-sd-fonts.sh [output_dir]
#
# Requires: python3, freetype-py (pip install freetype-py)
#
# Font stack: Uses NotoSans-Regular.ttf as base. For CJK coverage,
# place NotoSansCJK-Regular.ttc (or NotoSansSC-Regular.otf) next to
# the NotoSans source fonts before running this script.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FONT_SOURCE_DIR="$SCRIPT_DIR/../builtinFonts/source/NotoSans"
INTERVALS_FILE="$SCRIPT_DIR/sd-font-intervals.conf"
OUTPUT_DIR="${1:-$SCRIPT_DIR/../sd-fonts}"

NOTO_SANS="$FONT_SOURCE_DIR/NotoSans-Regular.ttf"
NOTO_SANS_CJK="$FONT_SOURCE_DIR/NotoSansCJK-Regular.ttc"

# Check required files
if [ ! -f "$NOTO_SANS" ]; then
    echo "ERROR: NotoSans-Regular.ttf not found at $NOTO_SANS" >&2
    exit 1
fi

# Build font stack (NotoSans first, CJK fallback if available)
FONTSTACK="$NOTO_SANS"
if [ -f "$NOTO_SANS_CJK" ]; then
    FONTSTACK="$NOTO_SANS $NOTO_SANS_CJK"
    echo "Using NotoSans + NotoSansCJK font stack"
else
    echo "WARNING: NotoSansCJK not found. CJK characters will be skipped." >&2
    echo "  Place NotoSansCJK-Regular.ttc at: $NOTO_SANS_CJK" >&2
fi

mkdir -p "$OUTPUT_DIR"

SIZES=(8 12 14 16 18)

for sz in "${SIZES[@]}"; do
    echo "Generating notosans_${sz}_regular.cpf..."
    python3 "$SCRIPT_DIR/cpfconvert.py" \
        "notosans_${sz}_regular" \
        "$sz" \
        $FONTSTACK \
        --2bit \
        --intervals-file "$INTERVALS_FILE" \
        --output "$OUTPUT_DIR/notosans_${sz}_regular.cpf"
done

echo ""
echo "Done. Copy the .cpf files to /.crosspoint/fonts/ on the SD card."
ls -lh "$OUTPUT_DIR"/*.cpf
