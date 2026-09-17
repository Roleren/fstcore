// MPL-2.0. Version 3 row-segment manifest. All integers are little-endian.
#ifndef FST_ROW_SEGMENTS_H
#define FST_ROW_SEGMENTS_H
#include <cstring>
#include <limits>
#include <interface/fststore.h>
#include <interface/fstdefines.h>
#include <xxhash.h>

namespace fst_rows {
const uint64_t MAGIC = 0x3130574f52545346ULL; // "FSTROW01"
inline uint64_t get64(const char* p) { uint64_t x; std::memcpy(&x, p, 8); return x; }
inline void put64(char* p, uint64_t x) { std::memcpy(p, &x, 8); }

inline std::vector<char> encode(const std::vector<std::vector<FstStore::RowSegment>>& columns) {
  uint64_t size = 32 + 8ULL * columns.size();
  for (const auto& col : columns) {
    if (col.size() > (std::numeric_limits<size_t>::max() - size) / 24)
      throw std::runtime_error("Row-segment manifest is too large.");
    size += 24ULL * col.size();
  }
  std::vector<char> data(size, 0);
  put64(data.data() + 8, MAGIC);
  put64(data.data() + 16, size);
  put64(data.data() + 24, columns.size());
  size_t at = 32;
  for (const auto& col : columns) {
    put64(data.data() + at, col.size()); at += 8;
    for (const auto& segment : col) {
      put64(data.data() + at, segment.start);
      put64(data.data() + at + 8, segment.rows);
      put64(data.data() + at + 16, segment.position); at += 24;
    }
  }
  put64(data.data(), ZSTD_XXH64(data.data() + 8, data.size() - 8, FST_HASH_SEED));
  return data;
}

inline void read(std::ifstream& file, unsigned version, int columns, uint64_t rows,
  const unsigned long long* positions, std::vector<std::vector<FstStore::RowSegment>>& result) {
  if (columns < 0 || rows > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    throw std::runtime_error("Invalid row-segment table dimensions.");
  result.clear();
  result.resize(columns);
  if (version < FST_ROWS_VERSION) {
    for (int col = 0; col < columns; ++col) result[col].push_back({0, rows, positions[col]});
    return;
  }
  const auto start = file.tellg();
  file.seekg(0, std::ios::end);
  const auto end = file.tellg();
  file.seekg(start);
  char header[32] = {};
  file.read(header, 32);
  const uint64_t size = get64(header + 16);
  if (!file || start < 0 || end < start || get64(header + 8) != MAGIC ||
      get64(header + 24) != static_cast<uint64_t>(columns) ||
      size < 32 + 8ULL * columns || size > static_cast<uint64_t>(end - start) ||
      size > std::numeric_limits<size_t>::max())
    throw std::runtime_error("Damaged row-segment manifest header.");
  std::vector<char> data(size);
  std::memcpy(data.data(), header, 32);
  file.read(data.data() + 32, size - 32);
  if (!file || get64(data.data()) != ZSTD_XXH64(data.data() + 8, size - 8, FST_HASH_SEED))
    throw std::runtime_error("Damaged row-segment manifest checksum.");
  size_t at = 32;
  for (int col = 0; col < columns; ++col) {
    if (size - at < 8) throw std::runtime_error("Truncated row-segment manifest.");
    const uint64_t count = get64(data.data() + at); at += 8;
    if (!count || count > (size - at) / 24)
      throw std::runtime_error("Invalid row-segment count.");
    result[col].reserve(count);
    uint64_t nextRow = 0;
    for (uint64_t i = 0; i < count; ++i, at += 24) {
      FstStore::RowSegment segment = {get64(data.data() + at), get64(data.data() + at + 8),
        get64(data.data() + at + 16)};
      if (segment.start != nextRow || nextRow > rows || segment.rows > rows - nextRow ||
          segment.position < TABLE_META_SIZE || segment.position > static_cast<uint64_t>(end) ||
          (segment.rows && segment.position == static_cast<uint64_t>(end)))
        throw std::runtime_error("Invalid row-segment coverage or payload offset.");
      nextRow += segment.rows;
      result[col].push_back(segment);
    }
    if (nextRow != rows || positions[col] != result[col][0].position)
      throw std::runtime_error("Row-segment manifest disagrees with table metadata.");
  }
  if (at != size) throw std::runtime_error("Unexpected row-segment manifest bytes.");
}
}
#endif
