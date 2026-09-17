// MPL-2.0. Decode segment intersections directly into the final output vectors.
#ifndef FST_ROW_SEGMENT_READER_H
#define FST_ROW_SEGMENT_READER_H
#include <blockstreamer/blockstreamer_v2.h>

namespace fst_rows {
class StringSlice : public IStringColumn {
  IStringColumn* target;
  uint64_t offset;
public:
  StringSlice(IStringColumn* target, uint64_t offset) : target(target), offset(offset) {}
  void AllocateVec(uint64_t) override { throw std::runtime_error("Cannot resize a string slice."); }
  void SetEncoding(StringEncoding encoding) override { target->SetEncoding(encoding); }
  StringEncoding GetEncoding() override { return target->GetEncoding(); }
  const char* GetElement(uint64_t i) override { return target->GetElement(offset + i); }
  void BufferToVec(uint64_t n, uint64_t start, uint64_t end, uint64_t base,
    unsigned int* sizes, char* buffer) override {
    target->BufferToVec(n, start, end, base + offset, sizes, buffer);
  }
};

template<class Decode> void intersections(const std::vector<FstStore::RowSegment>& segments,
  uint64_t first, uint64_t length, Decode decode) {
  const uint64_t end = first + length;
  auto it = std::lower_bound(segments.begin(), segments.end(), first,
    [](const FstStore::RowSegment& s, uint64_t row) { return s.start + s.rows <= row; });
  for (; it != segments.end() && it->start < end; ++it) {
    const uint64_t begin = std::max(first, it->start);
    const uint64_t stop = std::min(end, it->start + it->rows);
    if (stop > begin) decode(*it, begin - it->start, stop - begin, begin - first);
  }
}

struct FactorHeader { uint32_t levels; uint64_t codes; };
inline FactorHeader factorHeader(std::ifstream& file, uint64_t position) {
  char header[16] = {};
  file.seekg(position); file.read(header, 16);
  uint32_t version, levels;
  std::memcpy(&version, header, 4); std::memcpy(&levels, header + 4, 4);
  if (!file || version > 1) throw std::runtime_error("Damaged factor segment header.");
  return {levels, get64(header + 8)};
}
}

void FstStore::readSegmentedColumn(IFstTable& table, IColumnFactory* factory,
  std::ifstream& file, int col, int outputCol, uint64_t first, uint64_t length) {
  const auto& segments = columnSegments[col];
  const auto attribute = static_cast<FstColumnAttribute>(colAttributeTypes[col]);
  const short scale = colScales[col];
  std::string annotation;
  bool annotated = false;
  const auto& original = segments.front();

  switch (colTypes[col]) {
  case 6: {
    std::unique_ptr<IStringColumn> column(factory->CreateStringColumn(length, attribute));
    column->AllocateVec(length); table.SetStringColumn(column.get(), outputCol);
    fst_rows::intersections(segments, first, length, [&](const RowSegment& s, uint64_t from, uint64_t n, uint64_t dest) {
      fst_rows::StringSlice slice(column.get(), dest);
      fdsReadCharVec_v6(file, &slice, s.position, from, n, s.rows);
    });
    break;
  }
  case 8: {
    std::unique_ptr<IIntegerColumn> column(factory->CreateIntegerColumn(length, attribute, scale));
    table.SetIntegerColumn(column.get(), outputCol);
    fdsReadIntVec_v8(file, column->Data(), original.position, 0, 0, original.rows, annotation, annotated);
    if (annotated) column->Annotate(annotation);
    fst_rows::intersections(segments, first, length, [&](const RowSegment& s, uint64_t from, uint64_t n, uint64_t dest) {
      std::string ignored; bool flag;
      fdsReadIntVec_v8(file, column->Data() + dest, s.position, from, n, s.rows, ignored, flag);
    });
    break;
  }
  case 9: {
    std::unique_ptr<IDoubleColumn> column(factory->CreateDoubleColumn(length, attribute, scale));
    table.SetDoubleColumn(column.get(), outputCol);
    fdsReadRealVec_v9(file, column->Data(), original.position, 0, 0, original.rows, annotation, annotated);
    if (annotated) column->Annotate(annotation);
    fst_rows::intersections(segments, first, length, [&](const RowSegment& s, uint64_t from, uint64_t n, uint64_t dest) {
      std::string ignored; bool flag;
      fdsReadRealVec_v9(file, column->Data() + dest, s.position, from, n, s.rows, ignored, flag);
    });
    break;
  }
  case 10: {
    std::unique_ptr<ILogicalColumn> column(factory->CreateLogicalColumn(length, attribute));
    table.SetLogicalColumn(column.get(), outputCol);
    fst_rows::intersections(segments, first, length, [&](const RowSegment& s, uint64_t from, uint64_t n, uint64_t dest) {
      fdsReadLogicalVec_v10(file, column->Data() + dest, s.position, from, n, s.rows);
    });
    break;
  }
  case 11: {
    std::unique_ptr<IInt64Column> column(factory->CreateInt64Column(length, attribute, scale));
    table.SetInt64Column(column.get(), outputCol);
    fst_rows::intersections(segments, first, length, [&](const RowSegment& s, uint64_t from, uint64_t n, uint64_t dest) {
      fdsReadInt64Vec_v11(file, column->Data() + dest, s.position, from, n, s.rows);
    });
    break;
  }
  case 12: {
    std::unique_ptr<IByteColumn> column(factory->CreateByteColumn(length, attribute));
    table.SetByteColumn(column.get(), outputCol);
    fst_rows::intersections(segments, first, length, [&](const RowSegment& s, uint64_t from, uint64_t n, uint64_t dest) {
      fdsReadByteVec_v12(file, column->Data() + dest, s.position, from, n, s.rows);
    });
    break;
  }
  case 7: {
    const auto header = fst_rows::factorHeader(file, original.position);
    std::unique_ptr<IFactorColumn> column(factory->CreateFactorColumn(length, header.levels, attribute));
    table.SetFactorColumn(column.get(), outputCol);
    if (header.levels)
      fdsReadCharVec_v6(file, column->Levels(), original.position + 16, 0, header.levels, header.levels);
    fst_rows::intersections(segments, first, length, [&](const RowSegment& s, uint64_t from, uint64_t n, uint64_t dest) {
      const auto part = fst_rows::factorHeader(file, s.position);
      if (part.levels != header.levels) throw std::runtime_error("Factor segment levels disagree.");
      if (!part.levels) std::fill_n(column->LevelData() + dest, n, FST_NA_INT);
      else {
        std::string ignored; bool flag;
        fdsReadColumn_v2(file, reinterpret_cast<char*>(column->LevelData() + dest), part.codes,
          from, n, s.rows, 4, ignored, BATCH_SIZE_READ_FACTOR, flag);
      }
    });
    break;
  }
  default:
    throw std::runtime_error("Unsupported column type in row-segment file.");
  }
  if (!file) throw std::runtime_error("Unable to read row-segment data.");
}
#endif
