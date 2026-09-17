/*
  fstcore - R bindings to the fstlib library

  Copyright (C) 2017-present, Mark AJ Klik

  This file is part of the fstcore R package.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this file,
  You can obtain one at https://mozilla.org/MPL/2.0/.

  https://www.mozilla.org/en-US/MPL/2.0/FAQ/

  You can contact the author at:
  - fstcore R package source repository : https://github.com/fstpackage/fstcore
*/


#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <memory>
#include <limits>
#include <set>
#include <map>
#include <fst_append_lock.h>
#include <character/character_v6.h>

#include <Rcpp.h>

#include <interface/fstdefines.h>
#include <interface/istringwriter.h>
#include <interface/ifsttable.h>
#include <interface/icolumnfactory.h>
#include <interface/fststore.h>

#include <fst_error.h>
#include <fst_table.h>
#include <fst_column.h>
#include <fst_column_factory.h>
#include <fst_blockrunner_char.h>
#include <fst_string_vector_container.h>

#include <flex_store.h>


using namespace std;
using namespace Rcpp;


inline int FindKey(StringVector colNameList, String item)
{
  int index = -1;
  int found = 0;
  for (Rcpp::StringVector::iterator it = colNameList.begin(); it != colNameList.end(); ++it)
  {
    if (*it == item)
    {
      index = found;
      break;
    }
    ++found;
  }

  return index;
}


int fstlib_version()
{
  return FST_VERSION_MAJOR * 64 * 64 + FST_VERSION_MINOR * 64 + FST_VERSION_RELEASE;
}


SEXP fststore(String fileName, SEXP table, SEXP compression, SEXP uniformEncoding)
{
  if (!Rf_isLogical(uniformEncoding))
  {
    return fst_error("Parameter uniform.encoding should be a logical value");
  }

  if (!Rf_isInteger(compression))
  {
    return fst_error("Parameter compression should be an integer value between 0 and 100");
  }

  int compress = *INTEGER(compression);
  if ((compress < 0) | (compress > 100))
  {
    return fst_error("Parameter compression should be an integer value between 0 and 100");
  }

  // avoid using PROTECT statements in C++ classes (which generate rchk errors)
  // this PROTECTED container can be used to hold any R object safely
  SEXP r_container = PROTECT(Rf_allocVector(VECSXP, 1));

  FstTable fstTable(table, *LOGICAL(uniformEncoding), r_container);
  FstStore fstStore(fileName.get_cstring());

  try
  {
    fstStore.fstWrite(fstTable, compress);
  }
  catch (const std::runtime_error& e)
  {
    UNPROTECT(1);
    return fst_error(e.what());
  }

  UNPROTECT(1);
  return R_NilValue;
}


SEXP fstmetadata(String fileName)
{
  FstStore fstStore(fileName.get_cstring());
  std::unique_ptr<IColumnFactory> columnFactory(new ColumnFactory());

  // to hold the column names
  SEXP list_container = PROTECT(Rf_allocVector(VECSXP, 1));
  StringVectorContainer* str_container = new StringVectorContainer(list_container);
  std::unique_ptr<IStringColumn> col_names(str_container);

  // use fst format >= v0.8.0
  try
  {
    fstStore.fstMeta(columnFactory.get(), col_names.get());
  }
  catch (const std::runtime_error& e)
  {
    UNPROTECT(1); // list_container

    // We may be looking at a fst v0.7.2 file format, this unsafe code will be removed later
    if (std::strcmp(e.what(), FSTERROR_NON_FST_FILE) == 0)
    {
      return fst_error("File header information does not contain the fst format marker. "
        "If this is a fst file generated with package version older than v0.8.0, "
        "please read (and re-write) your file using fst package versions 0.8.0 to 0.8.10.");
    }

    return fst_error(e.what());
  }
  catch(...)
  {
    UNPROTECT(1); // list_container

    return fst_error("An unknown C++ error occured in the fstlib library.");
  }

  // R internals part TODO: speed up this code
  // SEXP colNames = ((BlockReaderChar*) fstStore.blockReader)->StrVector();
  SEXP colNames = str_container->StrVector();

  // Convert column info to integer vector
  // IntegerVector colTypeVec(fstStore.nrOfCols);
  IntegerVector colBaseType(fstStore.nrOfCols);
  IntegerVector colAttributeTypes(fstStore.nrOfCols);

  for (int col = 0; col != fstStore.nrOfCols; ++col)
  {
    // colTypeVec[col] = fstStore.colTypes[col];
    colBaseType[col] = fstStore.colBaseTypes[col];
    colAttributeTypes[col] = fstStore.colAttributeTypes[col];
  }

  List retList;

  if (fstStore.keyLength > 0)
  {
    SEXP keyNames;
    PROTECT(keyNames = Rf_allocVector(STRSXP, fstStore.keyLength));
    for (int i = 0; i < fstStore.keyLength; ++i)
    {
      SET_STRING_ELT(keyNames, i, STRING_ELT(colNames, fstStore.keyColPos[i]));
    }

    IntegerVector keyColIndex(fstStore.keyLength);
    for (int col = 0; col != fstStore.keyLength; ++col)
    {
      keyColIndex[col] = fstStore.keyColPos[col];
    }

    UNPROTECT(2);  // keyNames, list_container

    retList = List::create(
      _["nNofCols"]         = fstStore.nrOfCols,
      _["nrOfRows"]         = *(fstStore.p_nrOfRows),
      _["fstVersion"]       = fstStore.tableVersionMax,
      _["keyLength"]        = fstStore.keyLength,
      _["colBaseType"]      = colBaseType,
      _["colType"]          = colAttributeTypes,
      _["colNames"]         = colNames,
      _["keyColIndex"]      = keyColIndex,
      _["keyNames"]         = keyNames);
  }
  else
  {
    UNPROTECT(1); // list_container

    retList = List::create(
      _["nrOfCols"]        = fstStore.nrOfCols,
      _["nrOfRows"]        = *fstStore.p_nrOfRows,
      _["fstVersion"]      = fstStore.tableVersionMax,
      _["keyLength"]       = fstStore.keyLength,
      _["colBaseType"]     = colBaseType,
      _["colType"]         = colAttributeTypes,
      _["colNames"]        = colNames);
  }

  return retList;
}


SEXP fstretrieve(String fileName, SEXP columnSelection, SEXP startRow, SEXP endRow, SEXP mergeGap)
{
  // Validate before allocating/protecting R containers. Never silently consume
  // just the first element, round fractional indexes, or dereference empties.
  auto numeric = [](SEXP x) { return TYPEOF(x) == INTSXP || TYPEOF(x) == REALSXP; };
  auto number = [](SEXP x, R_xlen_t i) -> double {
    if (TYPEOF(x) == REALSXP) return REAL(x)[i];
    return INTEGER(x)[i] == NA_INTEGER ? NA_REAL : static_cast<double>(INTEGER(x)[i]);
  };
  auto valid = [](double x, double minimum) {
    return R_FINITE(x) && x >= minimum && x <= 9007199254740992.0 && x == std::floor(x);
  };
  if (!numeric(startRow) || (!Rf_isNull(endRow) && (!numeric(endRow) || XLENGTH(endRow) != XLENGTH(startRow))))
    return fst_error("Parameters from and to must be numeric vectors of equal length.");
  if (Rf_isNull(endRow) && XLENGTH(startRow) != 1)
    return fst_error("Parameter to may be NULL only for a single starting row.");
  if (!Rf_isNull(columnSelection) && TYPEOF(columnSelection) != STRSXP)
    return fst_error("Parameter columns must be a character vector.");
  if (!Rf_isNull(columnSelection)) {
    for (R_xlen_t i = 0; i < XLENGTH(columnSelection); ++i)
      if (STRING_ELT(columnSelection, i) == NA_STRING) return fst_error("Column names must not be NA.");
  }
  int64_t merge = -1;
  if (!Rf_isNull(mergeGap)) {
    if (!numeric(mergeGap) || XLENGTH(mergeGap) != 1 || !valid(number(mergeGap, 0), 0))
      return fst_error("Parameter merge_gap must be NULL or a non-negative whole number.");
    merge = static_cast<int64_t>(number(mergeGap, 0));
  }
  std::vector<FstReadRange> ranges;
  const bool multiple = XLENGTH(startRow) != 1;
  if (multiple) ranges.reserve(XLENGTH(startRow));
  int64_t sRow = 1, eRow = -1;
  for (R_xlen_t i = 0; i < XLENGTH(startRow); ++i) {
    const double from = number(startRow, i);
    const double to = Rf_isNull(endRow) ? from : number(endRow, i);
    if (!valid(from, 1) || !valid(to, 1) || to < from)
      return fst_error("Row endpoints must be finite positive whole numbers through 2^53, with from <= to.");
    if (multiple) ranges.push_back({static_cast<uint64_t>(from) - 1, static_cast<uint64_t>(to), 0});
    else { sRow = static_cast<int64_t>(from); if (!Rf_isNull(endRow)) eRow = static_cast<int64_t>(to); }
  }
  // avoid using PROTECT statements in C++ classes (which generate rchk errors)
  // this PROTECTED container can be used to hold any R object safely
  SEXP r_container = PROTECT(Rf_allocVector(VECSXP, 1));

  // to hold the column names
  SEXP list_container = PROTECT(Rf_allocVector(VECSXP, 1));
  StringVectorContainer* str_container = new StringVectorContainer(list_container);
  std::unique_ptr<IStringColumn> col_names(str_container);

  FstTable tableReader(r_container);

  std::unique_ptr<IColumnFactory> columnFactory(new ColumnFactory());
  FstStore fstStore(fileName.get_cstring());

  vector<int> keyIndex;

  // use fst format >= v0.8.0
  std::unique_ptr<StringArray> colSelection;
  std::unique_ptr<StringArray> colNames(new StringArray());

  if (!Rf_isNull(columnSelection))
  {
    colSelection = std::unique_ptr<StringArray>(new StringArray());
    colSelection->SetArray(columnSelection);
  }

  try
  {
    fstStore.fstRead(tableReader, colSelection.get(), sRow, eRow, columnFactory.get(), keyIndex,
      &*colNames, col_names.get(), multiple ? &ranges : nullptr, merge);
  }
  catch (const std::runtime_error& e)
  {
    // We may be looking at a fst v0.7.2 file format, this unsafe code will be removed later
    if (std::strcmp(e.what(), FSTERROR_NON_FST_FILE) == 0)
    {
      UNPROTECT(2);  // r_container, list_container
      return fst_error("File header information does not contain the fst format marker. "
        "If this is a fst file generated with package version older than v0.8.0, "
        "please read (and re-write) your file using fst package versions 0.8.0 to 0.8.10.");
    }

    // re-throw uncatched errors
    UNPROTECT(2);  // r_container, list_container
    return fst_error(e.what());
  }
  catch(...)
  {
    UNPROTECT(2);  // r_container, list_container
    return fst_error("An unknown C++ error occured in the fstlib library");
  }

  SEXP colNameVec = tableReader.GetColNames();

  // SEXP colNameVec = colNames->StrVector();

  // Generalize to full atributes
  SEXP resTable = PROTECT(tableReader.ResTable());
  Rf_setAttrib(resTable, R_NamesSymbol, colNameVec);
  UNPROTECT(1);

  // Convert keyIndex to keyNames
  SEXP keyNames = PROTECT(Rf_allocVector(STRSXP, keyIndex.size()));

  int count = 0;

  for (vector<int>::iterator keyIt = keyIndex.begin(); keyIt != keyIndex.end(); ++keyIt)
  {
    SET_STRING_ELT(keyNames, count++, STRING_ELT(colNameVec, *keyIt));
  }

  UNPROTECT(3);  // r_container, keyNames, list_container

  return List::create(
    _["keyNames"]   = keyNames,
    _["keyIndex"]   = keyIndex,
    _["resTable"]   = tableReader.ResTable());
}

SEXP fstappend(String fileName, SEXP table, SEXP compression, SEXP uniformEncoding)
{
  if (!Rf_inherits(table, "data.frame") || Rf_length(table) == 0)
    return fst_error("Please supply a data frame with at least one new column.");
  if (TYPEOF(compression) != INTSXP || XLENGTH(compression) != 1 ||
      INTEGER(compression)[0] == NA_INTEGER || INTEGER(compression)[0] < 0 || INTEGER(compression)[0] > 100)
    return fst_error("Parameter compress must be a single integer between 0 and 100.");
  if (TYPEOF(uniformEncoding) != LGLSXP || XLENGTH(uniformEncoding) != 1 || LOGICAL(uniformEncoding)[0] == NA_LOGICAL)
    return fst_error("Parameter uniform_encoding must be TRUE or FALSE.");

  try {
    FstAppendLock lock(fileName.get_cstring());
    List namesContainer(1), tableContainer(1);
    StringVectorContainer oldNames(namesContainer);
    ColumnFactory factory;
    FstStore store(fileName.get_cstring());
    store.fstMeta(&factory, &oldNames);
    if (Rf_length(table) > std::numeric_limits<int>::max() - store.nrOfCols)
      return fst_error("Too many columns.");
    SEXP newNames = Rf_getAttrib(table, R_NamesSymbol);
    if (TYPEOF(newNames) != STRSXP || XLENGTH(newNames) != XLENGTH(table))
      return fst_error("New columns must have unique, non-empty names.");
    CharacterVector allNames(store.nrOfCols + Rf_length(table));
    std::set<std::string> names;
    for (int i = 0; i < store.nrOfCols; ++i) {
      SEXP name = STRING_ELT(oldNames.StrVector(), i);
      if (name == NA_STRING || !names.insert(Rf_translateCharUTF8(name)).second || LENGTH(name) == 0)
        return fst_error("Existing columns must have unique, non-empty names.");
      allNames[i] = Rf_mkCharCE(Rf_translateCharUTF8(name), CE_UTF8);
    }
    for (int i = 0; i < Rf_length(table); ++i) {
      SEXP name = STRING_ELT(newNames, i);
      if (name == NA_STRING || LENGTH(name) == 0 || !names.insert(Rf_translateCharUTF8(name)).second)
        return fst_error("New column names must be unique, non-empty, and absent from the fst file.");
      if (static_cast<uint64_t>(XLENGTH(VECTOR_ELT(table, i))) != *store.p_nrOfRows)
        return fst_error("New columns must have the same number of rows as the fst file.");
      allNames[store.nrOfCols + i] = Rf_mkCharCE(Rf_translateCharUTF8(name), CE_UTF8);
    }
    FstTable newTable(table, LOGICAL(uniformEncoding)[0], tableContainer);
    SEXP combinedNames = allNames;
    BlockWriterChar nameWriter(combinedNames, allNames.size(), MAX_CHAR_STACK_SIZE, true);
    store.fstAppend(newTable, INTEGER(compression)[0], &nameWriter);
  } catch (const std::exception& e) {
    return fst_error(e.what());
  }
  return R_NilValue;
}

// Validate schema payloads before a replacement writes any bytes. Full-column
// replacement still requires the same dictionary/timezone: no schema migration.
static void validateReplacementSchema(FstStore& store, FstTable& table, int source, int target,
  SEXP value, ifstream& file, List& levelsContainer)
{
  FstColumnAttribute attribute; short scale = 0; std::string annotation; bool annotated;
  const auto type = table.ColumnType(source, attribute, scale, annotation, annotated);
  if (static_cast<unsigned>(type) != store.colBaseTypes[target] ||
      static_cast<unsigned>(attribute) != store.colAttributeTypes[target] ||
      static_cast<unsigned short>(scale) != store.colScales[target] || type == FstColumnType::BYTE_BLOCK)
    throw std::runtime_error("Replacement columns must have identical stored types, attributes and scales; no coercion is performed.");
  const uint64_t position = store.columnPositions[target];
  if (type == FstColumnType::FACTOR) {
    char header[16] = {}; file.seekg(position); file.read(header, 16);
    uint32_t version, count;
    std::memcpy(&version, header, 4); std::memcpy(&count, header + 4, 4);
    if (!file || version > 1) throw std::runtime_error("Damaged factor header.");
    SEXP incomingLevels = Rf_getAttrib(value, R_LevelsSymbol);
    if (TYPEOF(incomingLevels) != STRSXP || static_cast<uint64_t>(XLENGTH(incomingLevels)) != count)
      throw std::runtime_error("Replacement factors must have identical levels in the same order.");
    StringVectorContainer levels(levelsContainer);
    levels.AllocateVec(count);
    if (count) fdsReadCharVec_v6(file, &levels, position + 16, 0, count, count);
    if (!file || !R_compute_identical(levels.StrVector(), incomingLevels, 16))
      throw std::runtime_error("Replacement factors must have identical levels in the same order.");
  }
  if (attribute == FstColumnAttribute::INT_32_TIMESTAMP_SECONDS ||
      attribute == FstColumnAttribute::DOUBLE_64_TIMESTAMP_SECONDS) {
    uint32_t size = 0; file.seekg(position); file.read(reinterpret_cast<char*>(&size), 4);
    const bool oldAnnotated = (size & 0x80000000U) != 0;
    size &= 0x7fffffffU;
    if (!file || oldAnnotated != annotated || size != annotation.size())
      throw std::runtime_error("Replacement timestamps must have identical timezones.");
    std::string oldAnnotation(size, '\0');
    if (size) file.read(&oldAnnotation[0], size);
    if (!file || oldAnnotation != annotation)
      throw std::runtime_error("Replacement timestamps must have identical timezones.");
  }
}

SEXP fstreplacecolumns(String fileName, SEXP table, SEXP compression, SEXP uniformEncoding)
{
  if (TYPEOF(table) != VECSXP || XLENGTH(table) == 0 || XLENGTH(table) > std::numeric_limits<int>::max())
    return fst_error("Please supply a non-empty data frame or named list of replacement columns.");
  if (TYPEOF(compression) != INTSXP || XLENGTH(compression) != 1 ||
      INTEGER(compression)[0] == NA_INTEGER || INTEGER(compression)[0] < 0 || INTEGER(compression)[0] > 100)
    return fst_error("Parameter compress must be a single integer between 0 and 100.");
  if (TYPEOF(uniformEncoding) != LGLSXP || XLENGTH(uniformEncoding) != 1 || LOGICAL(uniformEncoding)[0] == NA_LOGICAL)
    return fst_error("Parameter uniform_encoding must be TRUE or FALSE.");
  try {
    FstAppendLock lock(fileName.get_cstring());
    List namesContainer(1), tableContainer(1), levelsContainer(1);
    StringVectorContainer oldNames(namesContainer);
    ColumnFactory factory;
    FstStore store(fileName.get_cstring());
    store.fstMeta(&factory, &oldNames);
    SEXP newNames = Rf_getAttrib(table, R_NamesSymbol);
    if (TYPEOF(newNames) != STRSXP || XLENGTH(newNames) != XLENGTH(table))
      return fst_error("Replacement columns must have unique, non-empty names.");
    std::map<std::string, int> existing;
    for (int col = 0; col < store.nrOfCols; ++col) {
      SEXP name = STRING_ELT(oldNames.StrVector(), col);
      if (name == NA_STRING || LENGTH(name) == 0 ||
          !existing.emplace(Rf_translateCharUTF8(name), col).second)
        return fst_error("Existing columns must have unique, non-empty names for replacement.");
    }
    std::vector<int> replacements(store.nrOfCols, -1);
    FstTable newTable(table, LOGICAL(uniformEncoding)[0], tableContainer);
    ifstream file(fileName.get_cstring(), ios::binary);
    for (int source = 0; source < Rf_length(table); ++source) {
      SEXP name = STRING_ELT(newNames, source);
      if (name == NA_STRING || LENGTH(name) == 0)
        return fst_error("Replacement columns must have unique, non-empty names.");
      const std::string utf8Name = Rf_translateCharUTF8(name);
      const auto found = existing.find(utf8Name);
      if (found == existing.end())
        throw std::runtime_error(std::string("Replacement column does not exist: ") + utf8Name);
      const int target = found->second;
      if (replacements[target] >= 0)
        return fst_error("Replacement column names must be unique.");
      replacements[target] = source;
      SEXP value = VECTOR_ELT(table, source);
      if (!Rf_isVectorAtomic(value) || !Rf_isNull(Rf_getAttrib(value, R_DimSymbol)))
        return fst_error("Replacement columns must be atomic vectors without dimensions.");
      if (static_cast<uint64_t>(XLENGTH(value)) != *store.p_nrOfRows)
        return fst_error("Replacement columns must have the same number of rows as the fst file; no recycling is performed.");
      try {
        validateReplacementSchema(store, newTable, source, target, value, file, levelsContainer);
      } catch (const std::runtime_error& e) {
        throw std::runtime_error("Column '" + utf8Name + "': " + e.what());
      }
    }
    SEXP allNames = oldNames.StrVector();
    BlockWriterChar nameWriter(allNames, store.nrOfCols, MAX_CHAR_STACK_SIZE, false);
    store.fstReplaceColumns(newTable, INTEGER(compression)[0], &nameWriter, replacements);
  } catch (const std::exception& e) {
    return fst_error(e.what());
  }
  return R_NilValue;
}

SEXP fstappendrows(String fileName, SEXP table, SEXP compression, SEXP uniformEncoding)
{
  if (TYPEOF(table) != VECSXP || Rf_length(table) == 0)
    return fst_error("Please supply a non-empty data frame or named list of columns.");
  if (TYPEOF(compression) != INTSXP || XLENGTH(compression) != 1 ||
      INTEGER(compression)[0] == NA_INTEGER || INTEGER(compression)[0] < 0 || INTEGER(compression)[0] > 100)
    return fst_error("Parameter compress must be a single integer between 0 and 100.");
  if (TYPEOF(uniformEncoding) != LGLSXP || XLENGTH(uniformEncoding) != 1 || LOGICAL(uniformEncoding)[0] == NA_LOGICAL)
    return fst_error("Parameter uniform_encoding must be TRUE or FALSE.");
  try {
    FstAppendLock lock(fileName.get_cstring());
    List namesContainer(1), tableContainer(1), levelsContainer(1);
    StringVectorContainer oldNames(namesContainer);
    ColumnFactory factory;
    FstStore store(fileName.get_cstring());
    store.fstMeta(&factory, &oldNames);
    SEXP newNames = Rf_getAttrib(table, R_NamesSymbol);
    if (Rf_length(table) != store.nrOfCols || TYPEOF(newNames) != STRSXP || XLENGTH(newNames) != XLENGTH(table))
      return fst_error("Row append requires the same column names in the same order.");
    const R_xlen_t rows = XLENGTH(VECTOR_ELT(table, 0));
    std::set<std::string> names;
    FstTable newTable(table, LOGICAL(uniformEncoding)[0], tableContainer);
    ifstream file(fileName.get_cstring(), ios::binary);
    for (int col = 0; col < store.nrOfCols; ++col) {
      SEXP name = STRING_ELT(newNames, col), oldName = STRING_ELT(oldNames.StrVector(), col);
      if (name == NA_STRING || oldName == NA_STRING || LENGTH(name) == 0 ||
          !names.insert(Rf_translateCharUTF8(name)).second ||
          std::strcmp(Rf_translateCharUTF8(name), Rf_translateCharUTF8(oldName)) != 0)
        return fst_error("Row append requires unique matching column names in the same order.");
      SEXP value = VECTOR_ELT(table, col);
      if (!Rf_isVectorAtomic(value) || !Rf_isNull(Rf_getAttrib(value, R_DimSymbol)) || XLENGTH(value) != rows)
        return fst_error("Row append requires atomic columns of equal length without dimensions.");
      FstColumnAttribute attribute; short scale = 0; std::string annotation; bool hasAnnotation;
      const auto type = newTable.ColumnType(col, attribute, scale, annotation, hasAnnotation);
      if (static_cast<unsigned>(type) != store.colBaseTypes[col] || static_cast<unsigned>(attribute) != store.colAttributeTypes[col] ||
          static_cast<unsigned short>(scale) != store.colScales[col])
        return fst_error("Row append requires identical column types, attributes and scales.");
      const uint64_t position = store.columnPositions[col];
      if (type == FstColumnType::FACTOR) {
        char header[16] = {}; file.seekg(position); file.read(header, 16);
        uint32_t version, count;
        std::memcpy(&version, header, 4); std::memcpy(&count, header + 4, 4);
        if (!file || version > 1) return fst_error("Damaged factor header.");
        StringVectorContainer levels(levelsContainer);
        levels.AllocateVec(count);
        if (count) fdsReadCharVec_v6(file, &levels, position + 16, 0, count, count);
        if (!R_compute_identical(levels.StrVector(), Rf_getAttrib(value, R_LevelsSymbol), 16))
          return fst_error("Row append requires identical factor levels in the same order.");
      }
      if (attribute == FstColumnAttribute::INT_32_TIMESTAMP_SECONDS ||
          attribute == FstColumnAttribute::DOUBLE_64_TIMESTAMP_SECONDS) {
        uint32_t annotationSize = 0; file.seekg(position); file.read(reinterpret_cast<char*>(&annotationSize), 4);
        const bool oldAnnotated = (annotationSize & 0x80000000U) != 0;
        annotationSize &= 0x7fffffffU;
        if (!file || oldAnnotated != hasAnnotation || annotationSize != annotation.size())
          return fst_error("Row append requires identical timestamp timezones.");
        std::string oldAnnotation(annotationSize, '\0');
        if (annotationSize) file.read(&oldAnnotation[0], annotationSize);
        if (!file || oldAnnotation != annotation) return fst_error("Row append requires identical timestamp timezones.");
      }
    }
    store.fstAppendRows(newTable, INTEGER(compression)[0]);
  } catch (const std::exception& e) {
    return fst_error(e.what());
  }
  return R_NilValue;
}
