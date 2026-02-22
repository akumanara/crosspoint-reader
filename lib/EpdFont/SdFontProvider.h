#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>

#include "CpfFormat.h"
#include "EpdFontData.h"

/// A cached glyph loaded from SD card, holding both metadata and bitmap data.
struct CachedGlyph {
  uint32_t codepoint;
  uint32_t lastUsed;   ///< Monotonic counter for LRU eviction
  EpdGlyph glyph;
  uint8_t* bitmapData;  ///< Points into the shared bitmap pool
  uint16_t bitmapSize;
  bool occupied;
};

/**
 * Loads extended Unicode glyphs from .cpf font files on SD card.
 *
 * Acts as a fallback for flash-resident fonts: when EpdFont::getGlyph()
 * returns nullptr for a codepoint, the renderer can query this provider.
 * Uses an LRU glyph cache to minimize SD card reads.
 */
class SdFontProvider {
 public:
  static constexpr size_t MAX_CACHE_ENTRIES = 64;
  static constexpr size_t BITMAP_POOL_SIZE = 24 * 1024;  // 24KB
  static constexpr size_t MAX_INTERVALS = 64;

  /// Initialize with a .cpf file path. Returns true on success.
  bool begin(const char* cpfFilePath);

  /// Close the font file and clear the cache.
  void end();

  /// Look up a glyph by Unicode codepoint. Returns nullptr if not in this font.
  const CachedGlyph* getGlyph(uint32_t cp);

  /// Clear the glyph cache (e.g. when switching font sizes).
  void clearCache();

  bool isReady() const { return ready; }
  uint8_t getAdvanceY() const { return advanceY; }
  int getAscender() const { return ascender; }
  int getDescender() const { return descender; }
  bool getIs2Bit() const { return is2Bit; }

  static SdFontProvider& getInstance();

 private:
  bool ready = false;
  uint8_t advanceY = 0;
  int ascender = 0;
  int descender = 0;
  bool is2Bit = false;

  // SD card file handle (kept open)
  FsFile fontFile;
  uint32_t glyphDirOffset = 0;

  // Interval index (loaded into RAM)
  CpfInterval intervals[MAX_INTERVALS];
  uint32_t intervalCount = 0;

  // LRU glyph cache
  CachedGlyph cache[MAX_CACHE_ENTRIES];
  uint8_t bitmapPool[BITMAP_POOL_SIZE];
  size_t bitmapPoolUsed = 0;
  uint32_t accessCounter = 0;

  /// Binary search the interval index for a codepoint. Returns -1 if not found.
  int findInterval(uint32_t cp) const;

  /// Find an existing cache entry for a codepoint, or -1.
  int findCacheEntry(uint32_t cp) const;

  /// Evict the least recently used cache entries and compact the bitmap pool.
  void evictAndCompact();

  /// Read a glyph from the .cpf file and insert it into the cache.
  const CachedGlyph* loadGlyphFromFile(uint32_t cp, int intervalIdx);

  static SdFontProvider instance;
};
