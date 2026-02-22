#include "SdFontProvider.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>

static const char* TAG = "SDFONT";

SdFontProvider SdFontProvider::instance;

SdFontProvider& SdFontProvider::getInstance() { return instance; }

bool SdFontProvider::begin(const char* cpfFilePath) {
  end();

  if (!Storage.openFileForRead(TAG, cpfFilePath, fontFile)) {
    LOG_DBG(TAG, "No SD font file: %s", cpfFilePath);
    return false;
  }

  // Read header
  CpfHeader header;
  if (fontFile.read(&header, sizeof(header)) != sizeof(header)) {
    LOG_ERR(TAG, "Failed to read CPF header");
    fontFile.close();
    return false;
  }

  if (header.magic != CPF_MAGIC || header.version != CPF_VERSION) {
    LOG_ERR(TAG, "Invalid CPF file (magic=0x%08X, version=%d)", header.magic, header.version);
    fontFile.close();
    return false;
  }

  advanceY = header.advanceY;
  ascender = header.ascender;
  descender = header.descender;
  is2Bit = header.is2Bit != 0;
  glyphDirOffset = header.glyphDirOffset;

  // Load interval index into RAM
  intervalCount = std::min(header.intervalCount, static_cast<uint32_t>(MAX_INTERVALS));
  if (!fontFile.seek(header.indexOffset)) {
    LOG_ERR(TAG, "Failed to seek to interval index");
    fontFile.close();
    return false;
  }

  const size_t intervalBytes = intervalCount * sizeof(CpfInterval);
  if (fontFile.read(intervals, intervalBytes) != static_cast<int>(intervalBytes)) {
    LOG_ERR(TAG, "Failed to read interval index");
    fontFile.close();
    return false;
  }

  clearCache();
  ready = true;

  uint32_t totalCodepoints = 0;
  for (uint32_t i = 0; i < intervalCount; i++) {
    totalCodepoints += intervals[i].last - intervals[i].first + 1;
  }
  LOG_DBG(TAG, "Loaded %s: %lu intervals, %lu codepoints", cpfFilePath, intervalCount, totalCodepoints);

  return true;
}

void SdFontProvider::end() {
  if (fontFile.isOpen()) {
    fontFile.close();
  }
  clearCache();
  ready = false;
  intervalCount = 0;
}

void SdFontProvider::clearCache() {
  for (size_t i = 0; i < MAX_CACHE_ENTRIES; i++) {
    cache[i].occupied = false;
    cache[i].bitmapData = nullptr;
  }
  bitmapPoolUsed = 0;
  accessCounter = 0;
}

int SdFontProvider::findInterval(const uint32_t cp) const {
  int left = 0;
  int right = static_cast<int>(intervalCount) - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    if (cp < intervals[mid].first) {
      right = mid - 1;
    } else if (cp > intervals[mid].last) {
      left = mid + 1;
    } else {
      return mid;
    }
  }
  return -1;
}

int SdFontProvider::findCacheEntry(const uint32_t cp) const {
  for (size_t i = 0; i < MAX_CACHE_ENTRIES; i++) {
    if (cache[i].occupied && cache[i].codepoint == cp) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

const CachedGlyph* SdFontProvider::getGlyph(const uint32_t cp) {
  if (!ready) return nullptr;

  // Check cache first
  const int cacheIdx = findCacheEntry(cp);
  if (cacheIdx >= 0) {
    cache[cacheIdx].lastUsed = ++accessCounter;
    return &cache[cacheIdx];
  }

  // Check if codepoint is in our interval index
  const int intervalIdx = findInterval(cp);
  if (intervalIdx < 0) return nullptr;

  // Load from SD card
  return loadGlyphFromFile(cp, intervalIdx);
}

void SdFontProvider::evictAndCompact() {
  // Find the oldest 25% of entries to evict
  constexpr size_t EVICT_COUNT = MAX_CACHE_ENTRIES / 4;

  // Collect lastUsed values of occupied entries
  uint32_t usedValues[MAX_CACHE_ENTRIES];
  size_t usedCount = 0;
  for (size_t i = 0; i < MAX_CACHE_ENTRIES; i++) {
    if (cache[i].occupied) {
      usedValues[usedCount++] = cache[i].lastUsed;
    }
  }

  if (usedCount == 0) return;

  // Simple selection: find the EVICT_COUNT-th smallest lastUsed value
  // Sort the values to find the threshold
  std::sort(usedValues, usedValues + usedCount);
  const size_t evictTarget = std::min(EVICT_COUNT, usedCount);
  const uint32_t threshold = usedValues[evictTarget - 1];

  // Mark entries for eviction
  for (size_t i = 0; i < MAX_CACHE_ENTRIES; i++) {
    if (cache[i].occupied && cache[i].lastUsed <= threshold) {
      cache[i].occupied = false;
      cache[i].bitmapData = nullptr;
    }
  }

  // Compact bitmap pool: move surviving entries' bitmaps to the front
  size_t newPoolUsed = 0;
  for (size_t i = 0; i < MAX_CACHE_ENTRIES; i++) {
    if (!cache[i].occupied) continue;
    if (cache[i].bitmapData == nullptr || cache[i].bitmapSize == 0) continue;

    uint8_t* oldPtr = cache[i].bitmapData;
    uint8_t* newPtr = bitmapPool + newPoolUsed;

    if (newPtr != oldPtr) {
      memmove(newPtr, oldPtr, cache[i].bitmapSize);
    }
    cache[i].bitmapData = newPtr;
    newPoolUsed += cache[i].bitmapSize;
  }

  bitmapPoolUsed = newPoolUsed;
  LOG_DBG(TAG, "Cache compacted: evicted %zu, pool %zu/%zu bytes", evictTarget, bitmapPoolUsed, BITMAP_POOL_SIZE);
}

const CachedGlyph* SdFontProvider::loadGlyphFromFile(const uint32_t cp, const int intervalIdx) {
  // Calculate glyph directory entry position
  const uint32_t glyphIndex = intervals[intervalIdx].glyphDirIndex + (cp - intervals[intervalIdx].first);
  const uint32_t entryOffset = glyphDirOffset + glyphIndex * sizeof(CpfGlyphEntry);

  // Read glyph directory entry
  CpfGlyphEntry entry;
  if (!fontFile.seek(entryOffset)) {
    LOG_ERR(TAG, "Failed to seek to glyph entry at 0x%X", entryOffset);
    return nullptr;
  }
  if (fontFile.read(&entry, sizeof(entry)) != sizeof(entry)) {
    LOG_ERR(TAG, "Failed to read glyph entry for U+%04X", cp);
    return nullptr;
  }

  // Check if bitmap fits in pool
  if (entry.dataLength > 0) {
    while (bitmapPoolUsed + entry.dataLength > BITMAP_POOL_SIZE) {
      evictAndCompact();
      // If still not enough after compaction, the glyph is too large or cache is thrashing
      if (bitmapPoolUsed + entry.dataLength > BITMAP_POOL_SIZE) {
        // Last resort: clear entire cache
        clearCache();
        if (entry.dataLength > BITMAP_POOL_SIZE) {
          LOG_ERR(TAG, "Glyph U+%04X bitmap too large: %u > %zu", cp, entry.dataLength, BITMAP_POOL_SIZE);
          return nullptr;
        }
      }
    }
  }

  // Find a free cache slot
  int slot = -1;
  for (size_t i = 0; i < MAX_CACHE_ENTRIES; i++) {
    if (!cache[i].occupied) {
      slot = static_cast<int>(i);
      break;
    }
  }
  if (slot < 0) {
    // All slots occupied — evict and try again
    evictAndCompact();
    for (size_t i = 0; i < MAX_CACHE_ENTRIES; i++) {
      if (!cache[i].occupied) {
        slot = static_cast<int>(i);
        break;
      }
    }
    if (slot < 0) {
      LOG_ERR(TAG, "Cache full, cannot load U+%04X", cp);
      return nullptr;
    }
  }

  // Read bitmap data from file
  uint8_t* bitmapDst = bitmapPool + bitmapPoolUsed;
  if (entry.dataLength > 0) {
    if (!fontFile.seek(entry.bitmapFileOffset)) {
      LOG_ERR(TAG, "Failed to seek to bitmap for U+%04X", cp);
      return nullptr;
    }
    if (fontFile.read(bitmapDst, entry.dataLength) != static_cast<int>(entry.dataLength)) {
      LOG_ERR(TAG, "Failed to read bitmap for U+%04X", cp);
      return nullptr;
    }
  }

  // Populate cache entry
  CachedGlyph& cached = cache[slot];
  cached.codepoint = cp;
  cached.lastUsed = ++accessCounter;
  cached.glyph.width = entry.width;
  cached.glyph.height = entry.height;
  cached.glyph.advanceX = entry.advanceX;
  cached.glyph.left = entry.left;
  cached.glyph.top = entry.top;
  cached.glyph.dataLength = entry.dataLength;
  cached.glyph.dataOffset = 0;  // Not used for cached glyphs
  cached.bitmapData = entry.dataLength > 0 ? bitmapDst : nullptr;
  cached.bitmapSize = entry.dataLength;
  cached.occupied = true;

  bitmapPoolUsed += entry.dataLength;

  return &cached;
}
