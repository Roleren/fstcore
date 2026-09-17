/*
  fstlib - A C++ library for ultra fast storage and retrieval of datasets

  Copyright (C) 2017-present, Mark AJ Klik

  This file is part of fstlib.

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this file,
  You can obtain one at https://mozilla.org/MPL/2.0/.

  https://www.mozilla.org/en-US/MPL/2.0/FAQ/

  You can contact the author at:
  - fstlib source repository : https://github.com/fstpackage/fstlib
*/


#ifndef FST_STORE_H
#define FST_STORE_H


#include <vector>
#include <interface/istringwriter.h>
#include <memory>
#include <fstream>
#include <interface/row_selection.h>

#include <interface/icolumnfactory.h>
#include <interface/ifsttable.h>


class FstStore
{
  std::string fstFile;
  std::unique_ptr<char[]> metaDataBlockP;

  void fstWriteImpl(IFstTable &fstTable, int compress, const FstStore* existing,
    IStringWriter* allNames, bool appendRows = false,
    const std::vector<int>* replacements = nullptr) const;

  void readSegmentedColumn(IFstTable& table, IColumnFactory* factory,
    std::ifstream& file, int col, int outputCol, uint64_t first, uint64_t length);

  void readSelectedColumn(IFstTable& table, IColumnFactory* factory,
    std::ifstream& file, int col, int outputCol, uint64_t length, const FstReadPlan& plan);

  public:
    struct RowSegment { uint64_t start, rows, position; };
    std::vector<std::vector<RowSegment>> columnSegments;
    std::vector<unsigned long long> columnPositions;
    unsigned long long* p_nrOfRows;
    int* keyColPos;

    char* metaDataBlock;

  	// column info
    unsigned short int* colTypes;
    unsigned short int* colBaseTypes;
    unsigned short int* colAttributeTypes;
    unsigned short int* colScales;

    unsigned int tableVersionMax;
    int nrOfCols, keyLength;

    FstStore(std::string fstFile);

    ~FstStore() { }

	/**
     * \brief Stream a data table
     * \param fstTable Table to stream, implementation of IFstTable interface
     * \param compress Compression factor with a value 0-100
     */
    void fstWrite(IFstTable &fstTable, int compress) const;

    // Existing metadata must have been loaded under the caller's exclusive lock.
    void fstAppend(IFstTable &fstTable, int compress, IStringWriter* allNames) const;

    // Caller validates factor dictionaries and annotations and holds the append lock.
    void fstAppendRows(IFstTable &fstTable, int compress) const;

    // replacements maps file columns to input columns; -1 preserves a column.
    // Caller validates names, factor dictionaries and annotations under the lock.
    void fstReplaceColumns(IFstTable &fstTable, int compress, IStringWriter* allNames,
      const std::vector<int>& replacements) const;

    void fstMeta(IColumnFactory* columnFactory, IStringColumn* col_names);

    void fstRead(IFstTable &tableReader, IStringArray* columnSelection, int64_t startRow, int64_t endRow,
      IColumnFactory* columnFactory, std::vector<int> &keyIndex, IStringArray* selectedCols, IStringColumn* col_names,
      const std::vector<FstReadRange>* ranges = nullptr, int64_t mergeGap = -1);
};


#endif  // FST_STORE_H
