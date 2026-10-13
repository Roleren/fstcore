/*
  fstcore fork: write integer columns straight from run-length encoded data.

  RleWindowTable is an IFstTable whose columns are windows [start, end] (1-based, inclusive) of
  run-length encoded vectors (run values + run lengths, one pair per column). Each column is
  decoded on demand into ONE reusable buffer when the writer asks for it, so a table of many
  long columns is never materialised: fstWriteImpl() requests and compresses columns strictly
  one at a time (GetIntWriter(col) -> fdsWriteIntVec_v8 -> next column).

  Values are converted exactly like R's as.integer(): doubles are truncated towards zero,
  NA/NaN and values outside the int range become NA_integer_.
*/

#ifndef RLE_TABLE_H
#define RLE_TABLE_H

#include <vector>
#include <string>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <Rcpp.h>

#include <interface/fstdefines.h>
#include <interface/ifsttable.h>
#include "fst_blockrunner_char.h"
#include <interface/openmphelper.h>


class RleWindowTable : public IFstTable
{
  SEXP values;       // list of numeric or integer vectors (run values), one per column
  SEXP runs;         // list of run lengths, or (runsAreEnds) cumulative run ends, one per column
  SEXP colNames;     // character vector, one per column
  bool runsAreEnds;  // runs hold 1-based end positions (cumsum of lengths): binary search, no scan
  uint32_t nrOfCols;
  uint64_t start;    // 1-based first position of the window
  uint64_t nrOfRows; // window width
  int uniformEncoding;
  std::vector<int> buffer;
  std::vector<double> scratchEnds;

  static int toInt(double v)
  {
    if (std::isnan(v)) return NA_INTEGER;
    const double t = std::trunc(v);
    if (t > 2147483647.0 || t <= -2147483649.0 || t == static_cast<double>(NA_INTEGER)) return NA_INTEGER;
    return static_cast<int>(t);
  }

  static double runEnd(SEXP r, R_xlen_t k)
  {
    return TYPEOF(r) == REALSXP ? REAL(r)[k] : static_cast<double>(INTEGER(r)[k]);
  }

  void unsupported() const
  {
    throw std::runtime_error("RleWindowTable only supports writing integer columns.");
  }

  public:
    RleWindowTable(SEXP values, SEXP runs, SEXP colNames, uint64_t start, uint64_t end, int uniformEncoding,
      bool runsAreEnds = false) :
      values(values), runs(runs), colNames(colNames), runsAreEnds(runsAreEnds), uniformEncoding(uniformEncoding)
    {
      if (TYPEOF(values) != VECSXP || TYPEOF(runs) != VECSXP || TYPEOF(colNames) != STRSXP)
        throw std::runtime_error("values and run lengths/ends must be lists and names a character vector.");
      if (XLENGTH(values) != XLENGTH(runs) || XLENGTH(values) != XLENGTH(colNames) || XLENGTH(values) == 0)
        throw std::runtime_error("values, run lengths/ends and names must have the same, non-zero length.");
      if (start < 1 || end < start) throw std::runtime_error("Window must satisfy 1 <= start <= end.");
      nrOfCols = static_cast<uint32_t>(XLENGTH(values));
      this->start = start;
      nrOfRows = end - start + 1;
      for (uint32_t col = 0; col < nrOfCols; ++col) {
        SEXP v = VECTOR_ELT(values, col), r = VECTOR_ELT(runs, col);
        if ((TYPEOF(v) != REALSXP && TYPEOF(v) != INTSXP) || XLENGTH(v) != XLENGTH(r) || XLENGTH(r) == 0)
          throw std::runtime_error("Each column needs non-empty numeric/integer run values and runs of equal length.");
        if (runsAreEnds) {
          // O(1) check: the full monotonicity is the caller's contract (cumsum of run lengths);
          // the visited part is verified while decoding.
          if ((TYPEOF(r) != REALSXP && TYPEOF(r) != INTSXP) || runEnd(r, XLENGTH(r) - 1) < static_cast<double>(end))
            throw std::runtime_error("Window end exceeds the last run end of a column.");
        } else {
          if (TYPEOF(r) != INTSXP) throw std::runtime_error("Run lengths must be integer vectors.");
          uint64_t total = 0;
          const int* lp = INTEGER(r);
          for (R_xlen_t k = 0; k < XLENGTH(r); ++k) {
            if (lp[k] == NA_INTEGER || lp[k] < 0) throw std::runtime_error("Run lengths must be non-negative integers.");
            total += static_cast<uint64_t>(lp[k]);
          }
          if (total < end) throw std::runtime_error("Window end exceeds the run-length vector of a column.");
        }
      }
    }

    ~RleWindowTable() {}

    FstColumnType ColumnType(uint32_t colNr, FstColumnAttribute &columnAttribute, short int &scale,
      std::string &annotation, bool &hasAnnotation)
    {
      hasAnnotation = false;
      scale = 0;
      columnAttribute = FstColumnAttribute::INT_32_BASE;
      return FstColumnType::INT_32;
    }

    // Decode column colNr's window into the shared buffer; valid until the next call.
    // Run ends are binary searched; the window is filled in parallel chunks (fst's thread count).
    int* GetIntWriter(uint32_t colNr)
    {
      if (colNr >= nrOfCols) throw std::runtime_error("Column index out of range.");
      buffer.resize(nrOfRows);
      SEXP v = VECTOR_ELT(values, colNr);
      SEXP r = VECTOR_ELT(runs, colNr);
      const R_xlen_t nRuns = XLENGTH(v);
      const bool isReal = TYPEOF(v) == REALSXP;
      const double* dv = isReal ? REAL(v) : nullptr;
      const int* iv = isReal ? nullptr : INTEGER(v);

      // ends[k] = last 1-based position of run k
      const double* ends;
      if (runsAreEnds && TYPEOF(r) == REALSXP) {
        ends = REAL(r);
      } else {
        scratchEnds.resize(nRuns);
        if (runsAreEnds) {
          const int* ip = INTEGER(r);
          for (R_xlen_t k = 0; k < nRuns; ++k) scratchEnds[k] = ip[k];
        } else {
          const int* lp = INTEGER(r);
          double acc = 0;
          for (R_xlen_t k = 0; k < nRuns; ++k) { acc += lp[k]; scratchEnds[k] = acc; }
        }
        ends = scratchEnds.data();
      }

      const double first = static_cast<double>(start);
      const double last = first + static_cast<double>(nrOfRows) - 1;
      int* out = buffer.data();
      const int nThreads = std::max(1, std::min(GetFstThreads(), static_cast<int>(nrOfRows / 65536) + 1));
      const uint64_t chunk = (nrOfRows + nThreads - 1) / nThreads;
      int failed = 0;

#pragma omp parallel for num_threads(nThreads) schedule(static) reduction(|:failed)
      for (int t = 0; t < nThreads; ++t) {
        const uint64_t o0 = static_cast<uint64_t>(t) * chunk;
        if (o0 >= nrOfRows) continue;
        const uint64_t o1 = std::min<uint64_t>(nrOfRows, o0 + chunk);   // [o0, o1)
        double pos = first + static_cast<double>(o0);
        // first run whose end >= pos
        R_xlen_t k = std::lower_bound(ends, ends + nRuns, pos) - ends;
        uint64_t o = o0;
        while (o < o1 && k < nRuns) {
          if (k > 0 && ends[k] < ends[k - 1]) { failed = 1; break; }   // runs must be nondecreasing
          const double runLast = std::min(ends[k], last);
          if (runLast >= pos) {
            const uint64_t len = std::min<uint64_t>(o1 - o, static_cast<uint64_t>(runLast - pos) + 1);
            const int value = isReal ? toInt(dv[k]) : iv[k];
            std::fill(out + o, out + o + len, value);
            o += len;
            pos += static_cast<double>(len);
          }
          ++k;
        }
        if (o != o1) failed = 1;
      }
      if (failed) throw std::runtime_error("Run ends are not nondecreasing or end before the window end.");
      return out;
    }

    IStringWriter* GetColNameWriter()
    {
      return new BlockWriterChar(colNames, nrOfCols, MAX_CHAR_STACK_SIZE, uniformEncoding);
    }

    void GetKeyColumns(int* keyColPos) {}
    uint32_t NrOfKeys() { return 0; }
    uint32_t NrOfColumns() { return nrOfCols; }
    uint64_t NrOfRows() { return nrOfRows; }

    // Not used when writing integer columns
    IStringWriter* GetStringWriter(uint32_t colNr) { unsupported(); return nullptr; }
    int* GetLogicalWriter(uint32_t colNr) { unsupported(); return nullptr; }
    long long* GetInt64Writer(uint32_t colNr) { unsupported(); return nullptr; }
    char* GetByteWriter(uint32_t colNr) { unsupported(); return nullptr; }
    double* GetDoubleWriter(uint32_t colNr) { unsupported(); return nullptr; }
    IByteBlockColumn* GetByteBlockWriter(uint32_t col_nr) { unsupported(); return nullptr; }
    IStringWriter* GetLevelWriter(uint32_t colNr) { unsupported(); return nullptr; }
    void InitTable(uint32_t nrOfCols, uint64_t nrOfRows) { unsupported(); }
    IByteBlockColumn* add_byte_block_column(unsigned col_nr) { unsupported(); return nullptr; }
    void SetStringColumn(IStringColumn* stringColumn, int colNr) { unsupported(); }
    void SetLogicalColumn(ILogicalColumn* logicalColumn, int colNr) { unsupported(); }
    void SetIntegerColumn(IIntegerColumn* integerColumn, int colNr) { unsupported(); }
    void SetDoubleColumn(IDoubleColumn* doubleColumn, int colNr) { unsupported(); }
    void SetFactorColumn(IFactorColumn* factorColumn, int colNr) { unsupported(); }
    void SetInt64Column(IInt64Column* int64Column, int colNr) { unsupported(); }
    void SetByteColumn(IByteColumn* byteColumn, int colNr) { unsupported(); }
    void SetColNames(IStringArray* col_names) { unsupported(); }
    void SetKeyColumns(int* keyColPos, uint32_t nrOfKeys) { unsupported(); }
};

#endif  // RLE_TABLE_H
