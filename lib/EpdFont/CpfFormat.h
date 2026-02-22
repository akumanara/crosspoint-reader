/**
 * CrossPoint Font (.cpf) binary file format.
 *
 * Designed for on-demand glyph loading from SD card with minimal RAM usage.
 * Glyph bitmaps use the same 1-bit/2-bit format as flash-resident EpdFontData.
 *
 * File layout:
 *   [0x0000]               CpfHeader (64 bytes)
 *   [header.indexOffset]    CpfInterval[header.intervalCount]
 *   [header.glyphDirOffset] CpfGlyphEntry[header.glyphCount]
 *   [header.bitmapOffset]   Raw bitmap data (variable length)
 */
#pragma once
#include <cstdint>

static constexpr uint32_t CPF_MAGIC = 0x31465043;  // "CPF1" in little-endian
static constexpr uint8_t CPF_VERSION = 1;

#pragma pack(push, 1)

struct CpfHeader {
  uint32_t magic;           // Must be CPF_MAGIC
  uint8_t version;          // Must be CPF_VERSION
  uint8_t is2Bit;           // 0 = 1-bit, 1 = 2-bit greyscale
  uint8_t advanceY;         // Line height (y axis)
  uint8_t reserved0;
  int16_t ascender;         // Max height above baseline
  int16_t descender;        // Max height below baseline
  uint32_t intervalCount;   // Number of unicode intervals
  uint32_t glyphCount;      // Total glyph count across all intervals
  uint32_t indexOffset;     // Byte offset to interval index
  uint32_t glyphDirOffset;  // Byte offset to glyph directory
  uint32_t bitmapOffset;    // Byte offset to bitmap data
  uint8_t reserved1[32];    // Pad to 64 bytes
};

static_assert(sizeof(CpfHeader) == 64, "CpfHeader must be 64 bytes");

struct CpfInterval {
  uint32_t first;          // First unicode code point (inclusive)
  uint32_t last;           // Last unicode code point (inclusive)
  uint32_t glyphDirIndex;  // Index of first glyph in glyph directory
};

static_assert(sizeof(CpfInterval) == 12, "CpfInterval must be 12 bytes");

struct CpfGlyphEntry {
  uint8_t width;            // Bitmap width in pixels
  uint8_t height;           // Bitmap height in pixels
  uint8_t advanceX;         // Cursor advance (x axis)
  uint8_t reserved0;
  int16_t left;             // X offset from cursor to upper-left
  int16_t top;              // Y offset from cursor to upper-left
  uint16_t dataLength;      // Bitmap data size in bytes
  uint16_t reserved1;
  uint32_t bitmapFileOffset;  // Absolute file offset to bitmap data
};

static_assert(sizeof(CpfGlyphEntry) == 16, "CpfGlyphEntry must be 16 bytes");

#pragma pack(pop)
