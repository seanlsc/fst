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


#ifndef FST_CHUNKS_H
#define FST_CHUNKS_H


#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include <interface/ifstcolumn.h>


// Chunk index [node D, leaf of C] [size: 32 + 16 * nrOfChunkSlots]
//
//  8                      | unsigned long long | hash value         // hash of chunk index
//  4                      | unsigned int       | FST_VERSION
//  4                      | int                | index flags
//  8                      | unsigned long long | nrOfChunks         // number of used chunk slots (0 means 1, legacy files)
//  2                      | unsigned short int | nrOfChunkSlots     // number of chunk slots (4 for single chunk files)
//  6                      |                    | free bytes         // possible future use
//  8 * nrOfChunkSlots     | unsigned long long | chunkPos           // data chunk header addresses
//  8 * nrOfChunkSlots     | unsigned long long | chunkRows          // data chunk number of rows
//
// Files written by fst versions before v0.2 always have 4 chunk slots of which only the first
// is used and the nrOfChunks field is zero. Each data chunk header (node E) holds the absolute
// file positions of the column data of that chunk.


/**
 * \brief Location and size of a single (vertical) data chunk in a fst file
 */
struct FstChunk
{
  uint64_t headerPos;            // absolute position of the data chunk header [node E]
  uint64_t nrOfRows;             // number of rows in the chunk
  uint64_t firstRow;             // zero-based table row of the first chunk row
  std::vector<uint64_t> colPos;  // absolute position of the column data of each column
};


/**
 * \brief Read the chunk index and all data chunk headers from a fst file
 * \param myfile stream positioned at the start of the chunk index
 * \param nrOfCols number of columns in the table
 * \param chunks resulting chunk information (output)
 * \return the number of chunk slots in the chunk index
 */
unsigned int ReadChunkIndex(std::istream &myfile, int nrOfCols, std::vector<FstChunk> &chunks);


/**
 * \brief Simple IStringColumn implementation that stores strings in std::string's
 *
 * Used for temporary string vectors (column names, factor levels) that are not
 * part of the result table.
 */
class StdStringColumn : public IStringColumn
{
  StringEncoding encoding = StringEncoding::NATIVE;

public:

  std::vector<std::string> values;
  std::vector<bool> isNA;

  ~StdStringColumn() {}

  void AllocateVec(uint64_t vecLength)
  {
    values.assign(vecLength, std::string());
    isNA.assign(vecLength, false);
  }

  void SetEncoding(StringEncoding stringEncoding) { encoding = stringEncoding; }

  StringEncoding GetEncoding() { return encoding; }

  void BufferToVec(uint64_t nrOfElements, uint64_t startElem, uint64_t endElem,
    uint64_t vecOffset, unsigned int* sizeMeta, char* buf)
  {
    const uint64_t nrOfNAInts = 1 + nrOfElements / 32;  // last bit is NA flag
    const unsigned int* bitsNA = &sizeMeta[nrOfElements];
    const bool hasNA = (bitsNA[nrOfNAInts - 1] & (1u << (nrOfElements % 32))) != 0;

    uint64_t pos = startElem == 0 ? 0 : sizeMeta[startElem - 1];

    for (uint64_t blockElem = startElem; blockElem <= endElem; ++blockElem)
    {
      const uint64_t newPos = sizeMeta[blockElem];
      const uint64_t index = vecOffset + blockElem - startElem;

      if (hasNA && (bitsNA[blockElem / 32] & (1u << (blockElem % 32))) != 0)
      {
        values[index].clear();
        isNA[index] = true;
      }
      else
      {
        values[index].assign(buf + pos, newPos - pos);
        isNA[index] = false;
      }

      pos = newPos;
    }
  }

  const char* GetElement(uint64_t elementNr) { return values[elementNr].c_str(); }
};


/**
 * \brief Concatenate fst files into a new fst file without decompressing the column data
 *
 * All input files must have identical column names, column types, column attributes and
 * column annotations (e.g. time zones). The data chunks of the input files are copied
 * verbatim to the output file, which will contain one data chunk per (non-empty) input chunk.
 * Keys are not retained in the result.
 *
 * \param inputFiles paths of the fst files to concatenate (in order)
 * \param outputFile path of the resulting fst file (will be overwritten)
 */
void FstConcat(const std::vector<std::string> &inputFiles, const std::string &outputFile);


#endif  // FST_CHUNKS_H
