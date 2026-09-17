// MPL-2.0. Read coalesced windows, scattering only selected values to output.
#ifndef FST_SELECTED_COLUMN_READER_H
#define FST_SELECTED_COLUMN_READER_H
namespace fst_selection {
template<class T, class Decode> void numeric(T* output, const FstReadPlan& plan, Decode decode) {
  std::vector<T> scratch;
  for (const auto& w : plan.windows) {
    const auto& first = plan.pieces[w.begin];
    if (w.end == w.begin + 1) {
      decode(first.segment, w.first, w.stop - w.first, output + first.output);
    } else {
      scratch.resize(w.stop - w.first);
      decode(first.segment, w.first, w.stop - w.first, scratch.data());
      for (size_t i = w.begin; i < w.end; ++i) {
        const auto& p = plan.pieces[i];
        std::copy_n(scratch.data() + p.first - w.first, p.end - p.first, output + p.output);
      }
    }
  }
}

// The character codec hands us decompressed blocks. Materialize only selected
// strings, preserving NA and per-segment encoding without temporary R vectors.
class Strings : public IStringColumn {
  IStringColumn* target;
  const FstReadPlan& plan;
  const FstReadWindow& window;
  std::vector<uint64_t> prefixEnd;
public:
  Strings(IStringColumn* target, const FstReadPlan& plan, const FstReadWindow& window)
    : target(target), plan(plan), window(window) {
    uint64_t end = 0;
    for (size_t i = window.begin; i < window.end; ++i) {
      end = std::max(end, plan.pieces[i].end); prefixEnd.push_back(end);
    }
  }
  void AllocateVec(uint64_t) override { throw std::runtime_error("Cannot resize a selection adapter."); }
  void SetEncoding(StringEncoding encoding) override { target->SetEncoding(encoding); }
  StringEncoding GetEncoding() override { return target->GetEncoding(); }
  const char* GetElement(uint64_t) override { throw std::runtime_error("Selection adapter is write-only."); }
  void BufferToVec(uint64_t n, uint64_t start, uint64_t end, uint64_t base,
    unsigned int* sizes, char* buffer) override {
    const uint64_t first = window.first + base, stop = first + end - start + 1;
    size_t i = window.begin + (std::upper_bound(prefixEnd.begin(), prefixEnd.end(), first) - prefixEnd.begin());
    for (; i < window.end && plan.pieces[i].first < stop; ++i) {
      const auto& p = plan.pieces[i];
      const uint64_t lo = std::max(first, p.first), hi = std::min(stop, p.end);
      if (lo < hi) target->BufferToVec(n, start + lo - first, start + hi - first - 1,
        p.output + lo - p.first, sizes, buffer);
    }
  }
};
}

void FstStore::readSelectedColumn(IFstTable& table, IColumnFactory* factory,
  std::ifstream& file, int col, int outputCol, uint64_t length, const FstReadPlan& plan) {
  const auto& segments = columnSegments[col];
  const auto attribute = static_cast<FstColumnAttribute>(colAttributeTypes[col]);
  const short scale = colScales[col];
  const auto& original = segments.front();
  std::string annotation; bool annotated = false;
  switch (colTypes[col]) {
  case 6: {
    std::unique_ptr<IStringColumn> column(factory->CreateStringColumn(length, attribute));
    column->AllocateVec(length); table.SetStringColumn(column.get(), outputCol);
    for (const auto& w : plan.windows) {
      const auto& s = segments[plan.pieces[w.begin].segment];
      fst_selection::Strings adapter(column.get(), plan, w);
      fdsReadCharVec_v6(file, &adapter, s.position, w.first, w.stop - w.first, s.rows);
    }
    break;
  }
  case 8: {
    std::unique_ptr<IIntegerColumn> column(factory->CreateIntegerColumn(length, attribute, scale));
    table.SetIntegerColumn(column.get(), outputCol);
    fdsReadIntVec_v8(file, column->Data(), original.position, 0, 0, original.rows, annotation, annotated);
    if (annotated) column->Annotate(annotation);
    fst_selection::numeric(column->Data(), plan, [&](uint64_t segment, uint64_t from, uint64_t n, int* out) {
      const auto& s = segments[segment]; std::string ignored; bool flag;
      fdsReadIntVec_v8(file, out, s.position, from, n, s.rows, ignored, flag);
    }); break;
  }
  case 9: {
    std::unique_ptr<IDoubleColumn> column(factory->CreateDoubleColumn(length, attribute, scale));
    table.SetDoubleColumn(column.get(), outputCol);
    fdsReadRealVec_v9(file, column->Data(), original.position, 0, 0, original.rows, annotation, annotated);
    if (annotated) column->Annotate(annotation);
    fst_selection::numeric(column->Data(), plan, [&](uint64_t segment, uint64_t from, uint64_t n, double* out) {
      const auto& s = segments[segment]; std::string ignored; bool flag;
      fdsReadRealVec_v9(file, out, s.position, from, n, s.rows, ignored, flag);
    }); break;
  }
  case 10: {
    std::unique_ptr<ILogicalColumn> column(factory->CreateLogicalColumn(length, attribute));
    table.SetLogicalColumn(column.get(), outputCol);
    fst_selection::numeric(column->Data(), plan, [&](uint64_t segment, uint64_t from, uint64_t n, int* out) {
      const auto& s = segments[segment]; fdsReadLogicalVec_v10(file, out, s.position, from, n, s.rows);
    }); break;
  }
  case 11: {
    std::unique_ptr<IInt64Column> column(factory->CreateInt64Column(length, attribute, scale));
    table.SetInt64Column(column.get(), outputCol);
    fst_selection::numeric(column->Data(), plan, [&](uint64_t segment, uint64_t from, uint64_t n, long long* out) {
      const auto& s = segments[segment]; fdsReadInt64Vec_v11(file, out, s.position, from, n, s.rows);
    }); break;
  }
  case 12: {
    std::unique_ptr<IByteColumn> column(factory->CreateByteColumn(length, attribute));
    table.SetByteColumn(column.get(), outputCol);
    fst_selection::numeric(column->Data(), plan, [&](uint64_t segment, uint64_t from, uint64_t n, char* out) {
      const auto& s = segments[segment]; fdsReadByteVec_v12(file, out, s.position, from, n, s.rows);
    }); break;
  }
  case 7: {
    const auto header = fst_rows::factorHeader(file, original.position);
    std::unique_ptr<IFactorColumn> column(factory->CreateFactorColumn(length, header.levels, attribute));
    table.SetFactorColumn(column.get(), outputCol);
    if (header.levels) fdsReadCharVec_v6(file, column->Levels(), original.position + 16, 0, header.levels, header.levels);
    fst_selection::numeric(column->LevelData(), plan, [&](uint64_t segment, uint64_t from, uint64_t n, int* out) {
      const auto& s = segments[segment]; const auto part = fst_rows::factorHeader(file, s.position);
      if (part.levels != header.levels) throw std::runtime_error("Factor segment levels disagree.");
      if (!part.levels) std::fill_n(out, n, FST_NA_INT);
      else {
        std::string ignored; bool flag;
        fdsReadColumn_v2(file, reinterpret_cast<char*>(out), part.codes, from, n, s.rows, 4, ignored, BATCH_SIZE_READ_FACTOR, flag);
      }
    }); break;
  }
  default: throw std::runtime_error("Unsupported column type for multiple row intervals.");
  }
  if (!file) throw std::runtime_error("Unable to read selected column data.");
}
#endif
