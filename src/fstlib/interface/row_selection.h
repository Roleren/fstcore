// MPL-2.0. Logical selections and bounded physical read windows.
#ifndef FST_ROW_SELECTION_H
#define FST_ROW_SELECTION_H
#include <algorithm>
#include <cstdint>
#include <vector>

// Half-open source interval and its offset in the requested output order.
struct FstReadRange { uint64_t first, end, output; };
struct FstReadPiece { uint64_t segment, first, end, output; };
struct FstReadWindow { size_t begin, end; uint64_t first, stop; };

struct FstReadPlan {
  std::vector<FstReadPiece> pieces;
  std::vector<FstReadWindow> windows;
  std::vector<uint64_t> starts, lengths;
  uint64_t blockSize = 0;
  int64_t gap = -2;
  static constexpr uint64_t MAX_WINDOW_ROWS = 262144;

  template<class Segments> void prepare(const std::vector<FstReadRange>& ranges,
    const Segments& segments, uint64_t block, int64_t mergeGap) {
    bool same = starts.size() == segments.size();
    for (size_t i = 0; same && i < segments.size(); ++i)
      same = starts[i] == segments[i].start && lengths[i] == segments[i].rows;
    if (!same) {
      starts.clear(); lengths.clear(); pieces.clear();
      for (const auto& s : segments) { starts.push_back(s.start); lengths.push_back(s.rows); }
      pieces.reserve(ranges.size());
      for (const auto& r : ranges) {
        auto it = std::lower_bound(segments.begin(), segments.end(), r.first,
          [](const typename Segments::value_type& s, uint64_t row) { return s.start + s.rows <= row; });
        for (; it != segments.end() && it->start < r.end; ++it) {
          const uint64_t first = std::max(r.first, it->start);
          const uint64_t end = std::min(r.end, it->start + it->rows);
          if (first < end) pieces.push_back({static_cast<uint64_t>(it - segments.begin()),
            first - it->start, end - it->start, r.output + first - r.first});
        }
      }
      auto less = [](const FstReadPiece& a, const FstReadPiece& b) {
        return a.segment < b.segment || (a.segment == b.segment && a.first < b.first);
      };
      if (!std::is_sorted(pieces.begin(), pieces.end(), less)) std::stable_sort(pieces.begin(), pieces.end(), less);
    }
    if (same && blockSize == block && gap == mergeGap) return;
    blockSize = block; gap = mergeGap; windows.clear();
    const uint64_t allowedGap = mergeGap < 0 ? block : static_cast<uint64_t>(mergeGap);
    for (size_t i = 0; i < pieces.size(); ++i) {
      const auto& p = pieces[i];
      if (!windows.empty()) {
        auto& w = windows.back();
        const bool sameBlock = p.first / block == (w.stop - 1) / block;
        const bool nearby = p.first <= w.stop || p.first - w.stop <= allowedGap;
        if (p.segment == pieces[w.begin].segment && (sameBlock || nearby) &&
            std::max(w.stop, p.end) - w.first <= MAX_WINDOW_ROWS) {
          w.end = i + 1; w.stop = std::max(w.stop, p.end); continue;
        }
      }
      windows.push_back({i, i + 1, p.first, p.end});
    }
  }
};
#endif
