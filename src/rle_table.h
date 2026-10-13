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


class RleWindowTable : public IFstTable
{
  SEXP values;       // list of numeric or integer vectors (run values), one per column
  SEXP lengths;      // list of integer vectors (run lengths), one per column
  SEXP colNames;     // character vector, one per column
  uint32_t nrOfCols;
  uint64_t start;    // 1-based first position of the window
  uint64_t nrOfRows; // window width
  int uniformEncoding;
  std::vector<int> buffer;

  static int toInt(double v)
  {
    if (std::isnan(v)) return NA_INTEGER;
    const double t = std::trunc(v);
    if (t > 2147483647.0 || t <= -2147483649.0 || t == static_cast<double>(NA_INTEGER)) return NA_INTEGER;
    return static_cast<int>(t);
  }

  void unsupported() const
  {
    throw std::runtime_error("RleWindowTable only supports writing integer columns.");
  }

  public:
    RleWindowTable(SEXP values, SEXP lengths, SEXP colNames, uint64_t start, uint64_t end, int uniformEncoding) :
      values(values), lengths(lengths), colNames(colNames), uniformEncoding(uniformEncoding)
    {
      if (TYPEOF(values) != VECSXP || TYPEOF(lengths) != VECSXP || TYPEOF(colNames) != STRSXP)
        throw std::runtime_error("values and lengths must be lists and names a character vector.");
      if (XLENGTH(values) != XLENGTH(lengths) || XLENGTH(values) != XLENGTH(colNames) || XLENGTH(values) == 0)
        throw std::runtime_error("values, lengths and names must have the same, non-zero length.");
      if (start < 1 || end < start) throw std::runtime_error("Window must satisfy 1 <= start <= end.");
      nrOfCols = static_cast<uint32_t>(XLENGTH(values));
      this->start = start;
      nrOfRows = end - start + 1;
      for (uint32_t col = 0; col < nrOfCols; ++col) {
        SEXP v = VECTOR_ELT(values, col), l = VECTOR_ELT(lengths, col);
        if ((TYPEOF(v) != REALSXP && TYPEOF(v) != INTSXP) || TYPEOF(l) != INTSXP || XLENGTH(v) != XLENGTH(l))
          throw std::runtime_error("Each column needs numeric/integer run values and integer run lengths of equal length.");
        uint64_t total = 0;
        const int* lp = INTEGER(l);
        for (R_xlen_t k = 0; k < XLENGTH(l); ++k) {
          if (lp[k] == NA_INTEGER || lp[k] < 0) throw std::runtime_error("Run lengths must be non-negative integers.");
          total += static_cast<uint64_t>(lp[k]);
        }
        if (total < end) throw std::runtime_error("Window end exceeds the run-length vector of a column.");
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
    int* GetIntWriter(uint32_t colNr)
    {
      if (colNr >= nrOfCols) throw std::runtime_error("Column index out of range.");
      buffer.resize(nrOfRows);
      SEXP v = VECTOR_ELT(values, colNr);
      const int* lp = INTEGER(VECTOR_ELT(lengths, colNr));
      const R_xlen_t nRuns = XLENGTH(v);
      const bool isReal = TYPEOF(v) == REALSXP;
      const double* dv = isReal ? REAL(v) : nullptr;
      const int* iv = isReal ? nullptr : INTEGER(v);
      const uint64_t end = start + nrOfRows - 1;

      uint64_t pos = 1;  // first position of run k
      R_xlen_t k = 0;
      while (k < nRuns && pos + static_cast<uint64_t>(lp[k]) <= start) { pos += lp[k]; ++k; }
      int* out = buffer.data();
      uint64_t o = 0;
      while (o < nrOfRows && k < nRuns) {
        const uint64_t runEnd = pos + static_cast<uint64_t>(lp[k]) - 1;
        const uint64_t s = std::max(pos, start), e = std::min(runEnd, end);
        if (lp[k] > 0 && e >= s) {
          const int value = isReal ? toInt(dv[k]) : iv[k];
          std::fill(out + o, out + o + (e - s + 1), value);
          o += e - s + 1;
        }
        pos = runEnd + 1;
        ++k;
      }
      if (o != nrOfRows) throw std::runtime_error("Run-length data ended before the window end.");
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
