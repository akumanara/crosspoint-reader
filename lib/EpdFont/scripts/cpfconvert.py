#!/usr/bin/env python3
"""
Generate a CrossPoint Font (.cpf) binary file from TTF fonts.

Outputs a binary font file for SD card glyph loading, using the same
glyph rasterization as fontconvert.py but in a seekable binary format.

Usage:
    python cpfconvert.py <name> <size> <font_files...> --2bit --intervals-file <path> --output <path>

Example:
    python cpfconvert.py notosans_14_regular 14 NotoSans-Regular.ttf --2bit \
        --intervals-file sd-font-intervals.conf --output notosans_14_regular.cpf
"""
import freetype
import sys
import struct
import math
import argparse
from collections import namedtuple

parser = argparse.ArgumentParser(description="Generate a .cpf binary font file for SD card loading.")
parser.add_argument("name", action="store", help="name of the font.")
parser.add_argument("size", type=int, help="font size to use.")
parser.add_argument("fontstack", action="store", nargs='+', help="list of font files, ordered by descending priority.")
parser.add_argument("--2bit", dest="is2Bit", action="store_true", help="generate 2-bit greyscale bitmap instead of 1-bit.")
parser.add_argument("--intervals-file", dest="intervals_file", required=True, help="path to Unicode intervals config file.")
parser.add_argument("--output", dest="output", required=True, help="output .cpf file path.")
args = parser.parse_args()

GlyphProps = namedtuple("GlyphProps", ["width", "height", "advance_x", "left", "top", "data_length", "data_offset", "code_point"])

font_stack = [freetype.Face(f) for f in args.fontstack]
is2Bit = args.is2Bit
size = args.size
font_name = args.name

# --- Load intervals from config file ---

def load_intervals_file(path):
    intervals = []
    with open(path, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split(',')
            if len(parts) == 2:
                first = int(parts[0].strip(), 0)
                last = int(parts[1].strip(), 0)
                intervals.append((first, last))
    return intervals

intervals = load_intervals_file(args.intervals_file)

# --- Reused helpers from fontconvert.py ---

def norm_floor(val):
    return int(math.floor(val / (1 << 6)))

def norm_ceil(val):
    return int(math.ceil(val / (1 << 6)))

def load_glyph(code_point):
    for face in font_stack:
        glyph_index = face.get_char_index(code_point)
        if glyph_index > 0:
            face.load_glyph(glyph_index, freetype.FT_LOAD_RENDER)
            return face
    return None

# --- Merge overlapping intervals and validate against font ---

unmerged_intervals = sorted(intervals)
intervals = []
unvalidated_intervals = []
for i_start, i_end in unmerged_intervals:
    if len(unvalidated_intervals) > 0 and i_start + 1 <= unvalidated_intervals[-1][1]:
        unvalidated_intervals[-1] = (unvalidated_intervals[-1][0], max(unvalidated_intervals[-1][1], i_end))
        continue
    unvalidated_intervals.append((i_start, i_end))

print(f"Validating {len(unvalidated_intervals)} intervals against font stack...", file=sys.stderr)
for i_start, i_end in unvalidated_intervals:
    start = i_start
    for code_point in range(i_start, i_end + 1):
        face = load_glyph(code_point)
        if face is None:
            if start < code_point:
                intervals.append((start, code_point - 1))
            start = code_point + 1
    if start != i_end + 1:
        intervals.append((start, i_end))

total_codepoints = sum(e - s + 1 for s, e in intervals)
print(f"Validated: {len(intervals)} intervals, {total_codepoints} codepoints", file=sys.stderr)

# --- Set font size ---

for face in font_stack:
    face.set_char_size(size << 6, size << 6, 150, 150)

# --- Rasterize all glyphs (same logic as fontconvert.py) ---

total_size = 0
all_glyphs = []

for i_start, i_end in intervals:
    for code_point in range(i_start, i_end + 1):
        face = load_glyph(code_point)
        bitmap = face.glyph.bitmap

        # Build 4-bit greyscale intermediate
        pixels4g = []
        px = 0
        for i, v in enumerate(bitmap.buffer):
            x = i % bitmap.width
            if x % 2 == 0:
                px = (v >> 4)
            else:
                px = px | (v & 0xF0)
                pixels4g.append(px)
                px = 0
            if x == bitmap.width - 1 and bitmap.width % 2 > 0:
                pixels4g.append(px)
                px = 0

        if is2Bit:
            pixels2b = []
            px = 0
            pitch = (bitmap.width // 2) + (bitmap.width % 2)
            for y in range(bitmap.rows):
                for x in range(bitmap.width):
                    px = px << 2
                    bm = pixels4g[y * pitch + (x // 2)]
                    bm = (bm >> ((x % 2) * 4)) & 0xF
                    if bm >= 12:
                        px += 3
                    elif bm >= 8:
                        px += 2
                    elif bm >= 4:
                        px += 1
                    if (y * bitmap.width + x) % 4 == 3:
                        pixels2b.append(px)
                        px = 0
            if (bitmap.width * bitmap.rows) % 4 != 0:
                px = px << (4 - (bitmap.width * bitmap.rows) % 4) * 2
                pixels2b.append(px)
            pixels = pixels2b
        else:
            pixelsbw = []
            px = 0
            pitch = (bitmap.width // 2) + (bitmap.width % 2)
            for y in range(bitmap.rows):
                for x in range(bitmap.width):
                    px = px << 1
                    bm = pixels4g[y * pitch + (x // 2)]
                    px += 1 if ((x & 1) == 0 and bm & 0xE > 0) or ((x & 1) == 1 and bm & 0xE0 > 0) else 0
                    if (y * bitmap.width + x) % 8 == 7:
                        pixelsbw.append(px)
                        px = 0
            if (bitmap.width * bitmap.rows) % 8 != 0:
                px = px << (8 - (bitmap.width * bitmap.rows) % 8)
                pixelsbw.append(px)
            pixels = pixelsbw

        packed = bytes(pixels)
        glyph = GlyphProps(
            width=bitmap.width,
            height=bitmap.rows,
            advance_x=norm_floor(face.glyph.advance.x),
            left=face.glyph.bitmap_left,
            top=face.glyph.bitmap_top,
            data_length=len(packed),
            data_offset=total_size,
            code_point=code_point,
        )
        total_size += len(packed)
        all_glyphs.append((glyph, packed))

# --- Get font metrics ---

face = load_glyph(ord('|'))
advance_y = norm_ceil(face.size.height)
ascender = norm_ceil(face.size.ascender)
descender = norm_floor(face.size.descender)

# --- Build .cpf binary ---

HEADER_SIZE = 64
interval_count = len(intervals)
glyph_count = len(all_glyphs)

index_offset = HEADER_SIZE
glyph_dir_offset = index_offset + interval_count * 12
bitmap_offset = glyph_dir_offset + glyph_count * 16

# Build header
header = struct.pack('<IBBBB hh II III 32s',
    0x31465043,                          # magic "CPF1" little-endian
    1,                                    # version
    1 if is2Bit else 0,                   # is2Bit
    advance_y & 0xFF,                     # advanceY
    0,                                    # reserved0
    ascender,                             # ascender (int16)
    descender,                            # descender (int16)
    interval_count,                       # intervalCount
    glyph_count,                          # glyphCount
    index_offset,                         # indexOffset
    glyph_dir_offset,                     # glyphDirOffset
    bitmap_offset,                        # bitmapOffset
    b'\x00' * 32,                         # reserved1
)

assert len(header) == 64, f"Header is {len(header)} bytes, expected 64"

# Build interval index
interval_data = b''
glyph_dir_index = 0
for i_start, i_end in intervals:
    interval_data += struct.pack('<III', i_start, i_end, glyph_dir_index)
    glyph_dir_index += i_end - i_start + 1

# Build glyph directory and bitmap data
glyph_dir_data = b''
bitmap_data = b''
current_bitmap_offset = bitmap_offset

for props, packed in all_glyphs:
    glyph_dir_data += struct.pack('<BBBB hh HH I',
        props.width,                       # width
        props.height,                      # height
        props.advance_x & 0xFF,            # advanceX
        0,                                 # reserved0
        props.left,                        # left (int16)
        props.top,                         # top (int16)
        props.data_length,                 # dataLength
        0,                                 # reserved1
        current_bitmap_offset,             # bitmapFileOffset
    )
    bitmap_data += packed
    current_bitmap_offset += len(packed)

# Write to file
with open(args.output, 'wb') as f:
    f.write(header)
    f.write(interval_data)
    f.write(glyph_dir_data)
    f.write(bitmap_data)

total_file_size = len(header) + len(interval_data) + len(glyph_dir_data) + len(bitmap_data)
print(f"Generated {args.output}: {total_file_size} bytes ({total_file_size / 1024:.1f} KB)", file=sys.stderr)
print(f"  {interval_count} intervals, {glyph_count} glyphs, {len(bitmap_data)} bytes bitmap data", file=sys.stderr)
print(f"  Font: {font_name}, size: {size}, mode: {'2-bit' if is2Bit else '1-bit'}", file=sys.stderr)
