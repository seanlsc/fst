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


// Concatenation of fst files.
//
// The column data of a fst file is stored per (vertical) data chunk. Each data chunk has a header
// with the absolute file positions of its column data and all other positions inside the column
// data are relative to the start of the column, with the exception of the factor level code
// position in the factor column header. Concatenation therefore amounts to:
//
// 1. validating that all input files have identical column names, types and annotations
// 2. writing a new file header (from the first input) with the total number of rows
// 3. writing a chunk index with one slot per (non-empty) input data chunk
// 4. copying the data chunks byte-for-byte and patching the absolute positions
//
// No column data is decompressed or recompressed and memory usage is limited to a copy buffer.


#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <interface/fstdefines.h>
#include <interface/fstchunks.h>
#include <interface/ifstcolumn.h>

#include <character/character_v6.h>

#include <xxhash.h>


using namespace std;


#define COL_NAMES_HEADER_SIZE 24
#define FACTOR_HEADER_SIZE    16
#define COPY_BUFFER_SIZE      (4 * 1024 * 1024)


/**
 * \brief Header information of a single input file
 */
struct FstInputFile
{
	string path;
	int nrOfCols = 0;
	int keyLength = 0;
	int tableFlags = 0;
	uint64_t keyIndexHeaderSize = 0;
	uint64_t metaSize = 0;            // size of key index, chunkset header and column names header
	vector<char> meta;                // key index, chunkset header and column names header
	uint64_t colNamesPos = 0;         // absolute position of the column names vector
	uint64_t chunkIndexPos = 0;       // absolute position of the chunk index
	uint64_t fileSize = 0;
	StdStringColumn colNames;
	vector<FstChunk> chunks;
	vector<uint64_t> chunkEnd;        // absolute position one past the last byte of each chunk

	const unsigned short int* ColAttributes() const
	{
		return reinterpret_cast<const unsigned short int*>(&meta[keyIndexHeaderSize + CHUNKSET_HEADER_SIZE]);
	}

	const unsigned short int* ColTypes() const
	{
		return reinterpret_cast<const unsigned short int*>(&meta[keyIndexHeaderSize + CHUNKSET_HEADER_SIZE + 2 * nrOfCols]);
	}

	const unsigned short int* ColBaseTypes() const
	{
		return reinterpret_cast<const unsigned short int*>(&meta[keyIndexHeaderSize + CHUNKSET_HEADER_SIZE + 4 * nrOfCols]);
	}

	const unsigned short int* ColScales() const
	{
		return reinterpret_cast<const unsigned short int*>(&meta[keyIndexHeaderSize + CHUNKSET_HEADER_SIZE + 6 * nrOfCols]);
	}
};


inline string FileError(const string &path, const string &message)
{
	return "File '" + path + "': " + message;
}


inline string ColumnTypeName(const unsigned short int colType)
{
	switch (colType)
	{
		case 6:  return "character";
		case 7:  return "factor";
		case 8:  return "integer";
		case 9:  return "double";
		case 10: return "logical";
		case 11: return "integer64";
		case 12: return "byte";
		case 13: return "byte block";
		default: return "unknown";
	}
}


inline string ColumnAttributeName(const unsigned short int colAttribute)
{
	switch (colAttribute)
	{
		case FstColumnAttribute::NONE:                           return "none";
		case FstColumnAttribute::CHARACTER_BASE:                 return "character";
		case FstColumnAttribute::FACTOR_BASE:                    return "factor";
		case FstColumnAttribute::FACTOR_ORDERED:                 return "ordered factor";
		case FstColumnAttribute::INT_32_BASE:                    return "integer";
		case FstColumnAttribute::INT_32_TIMESTAMP_SECONDS:       return "integer timestamp (POSIXct)";
		case FstColumnAttribute::INT_32_TIMEINTERVAL_SECONDS:    return "integer time interval (difftime)";
		case FstColumnAttribute::INT_32_DATE_DAYS:               return "integer date (Date)";
		case FstColumnAttribute::INT_32_TIME_OF_DAY_SECONDS:     return "integer time of day (ITime)";
		case FstColumnAttribute::DOUBLE_64_BASE:                 return "double";
		case FstColumnAttribute::DOUBLE_64_DATE_DAYS:            return "double date (Date)";
		case FstColumnAttribute::DOUBLE_64_TIMESTAMP_SECONDS:    return "double timestamp (POSIXct)";
		case FstColumnAttribute::DOUBLE_64_TIMEINTERVAL_SECONDS: return "double time interval (difftime)";
		case FstColumnAttribute::DOUBLE_64_TIME_OF_DAY_SECONDS:  return "double time of day";
		case FstColumnAttribute::BOOL_2_BASE:                    return "logical";
		case FstColumnAttribute::INT_64_BASE:                    return "integer64";
		case FstColumnAttribute::INT_64_TIME_SECONDS:            return "integer64 time (nanotime)";
		case FstColumnAttribute::BYTE_BASE:                      return "byte";
		default:                                                 return "unknown";
	}
}


/**
 * \brief Read the annotation (e.g. time zone or time unit) stored at the start of a column
 *
 * Only column types stored with the block streamer (integer, double, logical, integer64 and byte)
 * carry an annotation.
 */
inline string ReadAnnotation(ifstream &myfile, const uint64_t colPos)
{
	myfile.seekg(colPos);

	unsigned int annotationLength;
	myfile.read(reinterpret_cast<char*>(&annotationLength), 4);

	if (!myfile) throw(runtime_error(FSTERROR_DAMAGED_METADATA));

	if ((annotationLength & (1u << 31)) == 0) return string();  // no annotation

	annotationLength &= 0x7fffffff;

	string annotation(annotationLength, '\0');
	if (annotationLength > 0) myfile.read(&annotation[0], annotationLength);

	if (!myfile) throw(runtime_error(FSTERROR_DAMAGED_METADATA));

	// prefix distinguishes an empty annotation from no annotation
	return "+" + annotation;
}


inline bool HasAnnotation(const unsigned short int colType)
{
	return colType >= 8 && colType <= 12;
}


/**
 * \brief Read and verify all header information of an input file
 */
void ReadInputFile(const string &path, FstInputFile &input)
{
	input.path = path;

	ifstream myfile;
	myfile.open(path.c_str(), ios::in | ios::binary);

	if (myfile.fail())
	{
		throw(runtime_error(FileError(path, FSTERROR_ERROR_OPENING_FILE)));
	}

	// Table header [node A] [size: 48]
	char tableMeta[TABLE_META_SIZE];
	myfile.read(tableMeta, TABLE_META_SIZE);

	if (!myfile)
	{
		throw(runtime_error(FileError(path, FSTERROR_ERROR_OPEN_READ)));
	}

	const uint64_t headerHash = *reinterpret_cast<uint64_t*>(tableMeta);

	if (ZSTD_XXH64(&tableMeta[8], TABLE_META_SIZE - 8, FST_HASH_SEED) != headerHash)
	{
		throw(runtime_error(FileError(path, FSTERROR_NON_FST_FILE)));
	}

	const unsigned int tableVersionMax = *reinterpret_cast<unsigned int*>(&tableMeta[24]);

	if (tableVersionMax > FST_VERSION)
	{
		throw(runtime_error(FileError(path, FSTERROR_UPDATE_FST)));
	}

	input.tableFlags = *reinterpret_cast<int*>(&tableMeta[12]);
	input.nrOfCols   = *reinterpret_cast<int*>(&tableMeta[28]);
	input.keyLength  = *reinterpret_cast<int*>(&tableMeta[40]);

	if (input.keyLength != 0)
	{
		input.keyIndexHeaderSize = (input.keyLength % 2) * 4 + 4 * (input.keyLength + 2);
	}

	const uint64_t chunksetHeaderSize = CHUNKSET_HEADER_SIZE + 8 * static_cast<uint64_t>(input.nrOfCols);
	input.metaSize = input.keyIndexHeaderSize + chunksetHeaderSize + COL_NAMES_HEADER_SIZE;

	input.meta.resize(input.metaSize);
	myfile.read(input.meta.data(), input.metaSize);

	if (!myfile)
	{
		throw(runtime_error(FileError(path, FSTERROR_DAMAGED_HEADER)));
	}

	char* meta = input.meta.data();

	// verify hashes
	if (input.keyLength != 0)
	{
		const uint64_t keyIndexHash = *reinterpret_cast<uint64_t*>(meta);
		if (ZSTD_XXH64(&meta[8], input.keyIndexHeaderSize - 8, FST_HASH_SEED) != keyIndexHash)
		{
			throw(runtime_error(FileError(path, FSTERROR_DAMAGED_HEADER)));
		}
	}

	const uint64_t chunksetHash = *reinterpret_cast<uint64_t*>(&meta[input.keyIndexHeaderSize]);
	if (ZSTD_XXH64(&meta[input.keyIndexHeaderSize + 8], chunksetHeaderSize - 8, FST_HASH_SEED) != chunksetHash)
	{
		throw(runtime_error(FileError(path, FSTERROR_DAMAGED_HEADER)));
	}

	const uint64_t colNamesOffset = input.keyIndexHeaderSize + chunksetHeaderSize;
	const uint64_t colNamesHash = *reinterpret_cast<uint64_t*>(&meta[colNamesOffset]);
	if (ZSTD_XXH64(&meta[colNamesOffset + 8], COL_NAMES_HEADER_SIZE - 8, FST_HASH_SEED) != colNamesHash)
	{
		throw(runtime_error(FileError(path, FSTERROR_DAMAGED_HEADER)));
	}

	// column names
	input.colNamesPos = TABLE_META_SIZE + input.metaSize;
	input.colNames.AllocateVec(input.nrOfCols);

	myfile.seekg(input.colNamesPos);
	fdsReadCharVec_v6(myfile, &input.colNames, input.colNamesPos, 0, input.nrOfCols, input.nrOfCols);

	// chunk index directly follows the column names
	input.chunkIndexPos = myfile.tellg();

	try
	{
		ReadChunkIndex(myfile, input.nrOfCols, input.chunks);
	}
	catch (const std::runtime_error &e)
	{
		throw(runtime_error(FileError(path, e.what())));
	}

	myfile.seekg(0, ios_base::end);
	input.fileSize = myfile.tellg();

	// chunk data extends to the start of the next chunk (or the end of the file)
	vector<uint64_t> starts;
	for (const FstChunk &chunk : input.chunks) starts.push_back(chunk.headerPos);
	sort(starts.begin(), starts.end());

	for (const FstChunk &chunk : input.chunks)
	{
		auto next = upper_bound(starts.begin(), starts.end(), chunk.headerPos);
		const uint64_t end = next == starts.end() ? input.fileSize : *next;

		if (end <= chunk.headerPos || end > input.fileSize)
		{
			throw(runtime_error(FileError(path, FSTERROR_DAMAGED_CHUNKINDEX)));
		}

		input.chunkEnd.push_back(end);
	}

	myfile.close();
}


/**
 * \brief Check that an input file has the same table structure as the first input file
 */
void ValidateInputFile(const FstInputFile &first, const FstInputFile &input)
{
	if (input.nrOfCols != first.nrOfCols)
	{
		throw(runtime_error("Files '" + first.path + "' and '" + input.path + "' have a different number of columns ("
			+ to_string(first.nrOfCols) + " and " + to_string(input.nrOfCols) + ")."));
	}

	const unsigned short int* colTypes1 = first.ColTypes();
	const unsigned short int* colTypes2 = input.ColTypes();
	const unsigned short int* colAttr1  = first.ColAttributes();
	const unsigned short int* colAttr2  = input.ColAttributes();
	const unsigned short int* colBase1  = first.ColBaseTypes();
	const unsigned short int* colBase2  = input.ColBaseTypes();
	const unsigned short int* colScale1 = first.ColScales();
	const unsigned short int* colScale2 = input.ColScales();

	for (int colNr = 0; colNr < first.nrOfCols; ++colNr)
	{
		const string &name1 = first.colNames.values[colNr];
		const string &name2 = input.colNames.values[colNr];
		const string position = "Column " + to_string(colNr + 1);

		if (name1 != name2)
		{
			throw(runtime_error(position + " is named '" + name1 + "' in file '" + first.path + "' but '" + name2
				+ "' in file '" + input.path + "'."));
		}

		if (colTypes1[colNr] != colTypes2[colNr] || colBase1[colNr] != colBase2[colNr])
		{
			throw(runtime_error(position + " ('" + name1 + "') has type '" + ColumnTypeName(colTypes1[colNr]) + "' in file '"
				+ first.path + "' but '" + ColumnTypeName(colTypes2[colNr]) + "' in file '" + input.path + "'."));
		}

		if (colAttr1[colNr] != colAttr2[colNr])
		{
			throw(runtime_error(position + " ('" + name1 + "') has type '" + ColumnAttributeName(colAttr1[colNr]) + "' in file '"
				+ first.path + "' but '" + ColumnAttributeName(colAttr2[colNr]) + "' in file '" + input.path + "'."));
		}

		if (colScale1[colNr] != colScale2[colNr])
		{
			throw(runtime_error(position + " ('" + name1 + "') has a different scale or time unit in file '" + first.path
				+ "' and file '" + input.path + "'."));
		}
	}
}


/**
 * \brief Check that column annotations (time zones, time units) are identical for all inputs
 */
void ValidateAnnotations(const vector<FstInputFile> &inputs)
{
	const FstInputFile &first = inputs[0];
	const unsigned short int* colTypes = first.ColTypes();

	vector<int> annotatedCols;
	for (int colNr = 0; colNr < first.nrOfCols; ++colNr)
	{
		if (HasAnnotation(colTypes[colNr])) annotatedCols.push_back(colNr);
	}

	if (annotatedCols.empty()) return;

	vector<string> firstAnnotations;

	for (size_t fileNr = 0; fileNr < inputs.size(); ++fileNr)
	{
		const FstInputFile &input = inputs[fileNr];

		ifstream myfile;
		myfile.open(input.path.c_str(), ios::in | ios::binary);

		if (myfile.fail())
		{
			throw(runtime_error(FileError(input.path, FSTERROR_ERROR_OPENING_FILE)));
		}

		for (const FstChunk &chunk : input.chunks)
		{
			for (size_t index = 0; index < annotatedCols.size(); ++index)
			{
				const int colNr = annotatedCols[index];
				const string annotation = ReadAnnotation(myfile, chunk.colPos[colNr]);

				if (firstAnnotations.size() < annotatedCols.size())
				{
					firstAnnotations.push_back(annotation);
					continue;
				}

				if (annotation != firstAnnotations[index])
				{
					const string name = first.colNames.values[colNr];
					const string a1 = firstAnnotations[index].empty() ? "<none>" : "'" + firstAnnotations[index].substr(1) + "'";
					const string a2 = annotation.empty() ? "<none>" : "'" + annotation.substr(1) + "'";

					throw(runtime_error("Column " + to_string(colNr + 1) + " ('" + name + "') has attribute (time zone or unit) "
						+ a1 + " in file '" + first.path + "' but " + a2 + " in file '" + input.path + "'."));
				}
			}
		}

		myfile.close();
	}
}


/**
 * \brief Copy a byte range from one stream to another
 */
inline void CopyBytes(ifstream &source, ofstream &target, const uint64_t sourcePos, uint64_t size, char* buffer)
{
	source.seekg(sourcePos);

	while (size > 0)
	{
		const uint64_t blockSize = min(size, static_cast<uint64_t>(COPY_BUFFER_SIZE));
		source.read(buffer, blockSize);

		if (!source)
		{
			throw(runtime_error(FSTERROR_ERROR_OPEN_READ));
		}

		target.write(buffer, blockSize);
		size -= blockSize;
	}
}


/**
 * \brief Write the concatenated file
 */
void WriteConcatenation(const vector<FstInputFile> &inputs, const string &outputFile)
{
	const FstInputFile &first = inputs[0];
	const int nrOfCols = first.nrOfCols;
	const unsigned short int* colTypes = first.ColTypes();

	// select chunks to copy (empty chunks are skipped)
	struct SourceChunk
	{
		size_t fileNr;
		size_t chunkNr;
	};

	vector<SourceChunk> sourceChunks;
	uint64_t totalNrOfRows = 0;

	for (size_t fileNr = 0; fileNr < inputs.size(); ++fileNr)
	{
		for (size_t chunkNr = 0; chunkNr < inputs[fileNr].chunks.size(); ++chunkNr)
		{
			const uint64_t chunkRows = inputs[fileNr].chunks[chunkNr].nrOfRows;
			if (chunkRows == 0) continue;

			sourceChunks.push_back(SourceChunk{fileNr, chunkNr});
			totalNrOfRows += chunkRows;
		}
	}

	// all inputs are empty: use the (empty) first chunk of the first file to retain column meta data
	if (sourceChunks.empty())
	{
		sourceChunks.push_back(SourceChunk{0, 0});
	}

	if (sourceChunks.size() > CHUNK_SLOTS_MAX)
	{
		throw(runtime_error(FSTERROR_CONCAT_MAX_CHUNKS));
	}

	const uint64_t nrOfChunks = sourceChunks.size();
	const unsigned short int nrOfChunkSlots = static_cast<unsigned short int>(max(nrOfChunks, static_cast<uint64_t>(CHUNK_SLOTS_DEFAULT)));

	// a single chunk result can be read by older fst versions
	const unsigned int tableVersionMax = nrOfChunks == 1 ? FST_VERSION_SINGLE_CHUNK : FST_VERSION_MULTI_CHUNK;

	// Table header [node A] [size: 48], keys are not retained

	char tableMeta[TABLE_META_SIZE];
	memset(tableMeta, 0, TABLE_META_SIZE);

	*reinterpret_cast<unsigned int*>(&tableMeta[8])   = tableVersionMax;
	*reinterpret_cast<int*>(&tableMeta[12])           = first.tableFlags;
	*reinterpret_cast<unsigned int*>(&tableMeta[24])  = tableVersionMax;
	*reinterpret_cast<int*>(&tableMeta[28])           = nrOfCols;
	*reinterpret_cast<uint64_t*>(&tableMeta[32])      = TABLE_META_SIZE;  // primary chunkset location
	*reinterpret_cast<int*>(&tableMeta[40])           = 0;                // no keys
	*reinterpret_cast<int*>(&tableMeta[44])           = FST_MAGIC_NUMBER;
	*reinterpret_cast<uint64_t*>(tableMeta)           = ZSTD_XXH64(&tableMeta[8], TABLE_META_SIZE - 8, FST_HASH_SEED);

	// Chunkset header [node C] and column names header, copied from the first file

	const uint64_t chunksetHeaderSize = CHUNKSET_HEADER_SIZE + 8 * static_cast<uint64_t>(nrOfCols);
	vector<char> chunksetHeader(first.meta.begin() + first.keyIndexHeaderSize,
		first.meta.begin() + first.keyIndexHeaderSize + chunksetHeaderSize + COL_NAMES_HEADER_SIZE);

	*reinterpret_cast<uint64_t*>(&chunksetHeader[64]) = totalNrOfRows;
	*reinterpret_cast<uint64_t*>(chunksetHeader.data()) =
		ZSTD_XXH64(&chunksetHeader[8], chunksetHeaderSize - 8, FST_HASH_SEED);

	// Chunk index [node D]

	const uint64_t chunkIndexSize = CHUNK_INDEX_HEAD + 16 * static_cast<uint64_t>(nrOfChunkSlots);
	vector<char> chunkIndex(chunkIndexSize, 0);

	*reinterpret_cast<unsigned int*>(&chunkIndex[8])         = tableVersionMax;
	*reinterpret_cast<uint64_t*>(&chunkIndex[16])            = nrOfChunks;
	*reinterpret_cast<unsigned short int*>(&chunkIndex[24])  = nrOfChunkSlots;

	uint64_t* p_chunkPos  = reinterpret_cast<uint64_t*>(&chunkIndex[CHUNK_INDEX_HEAD]);
	uint64_t* p_chunkRows = reinterpret_cast<uint64_t*>(&chunkIndex[CHUNK_INDEX_HEAD + 8 * static_cast<uint64_t>(nrOfChunkSlots)]);

	// Write headers

	ofstream outfile;
	outfile.open(outputFile.c_str(), ios::out | ios::binary | ios::trunc);

	if (outfile.fail())
	{
		throw(runtime_error(FSTERROR_ERROR_OPEN_WRITE));
	}

	outfile.write(tableMeta, TABLE_META_SIZE);
	outfile.write(chunksetHeader.data(), chunksetHeader.size());

	// column names vector, copied verbatim from the first file
	std::unique_ptr<char[]> bufferP(new char[COPY_BUFFER_SIZE]);
	char* buffer = bufferP.get();

	{
		ifstream source;
		source.open(first.path.c_str(), ios::in | ios::binary);

		if (source.fail())
		{
			throw(runtime_error(FileError(first.path, FSTERROR_ERROR_OPENING_FILE)));
		}

		CopyBytes(source, outfile, first.colNamesPos, first.chunkIndexPos - first.colNamesPos, buffer);
		source.close();
	}

	const uint64_t chunkIndexPos = outfile.tellp();
	outfile.write(chunkIndex.data(), chunkIndexSize);  // placeholder, rewritten at the end

	// Copy data chunks

	const uint64_t dataHeaderSize = DATA_INDEX_SIZE + 8 * static_cast<uint64_t>(nrOfCols);
	std::unique_ptr<char[]> dataHeaderP(new char[dataHeaderSize]);
	char* dataHeader = dataHeaderP.get();

	size_t openFileNr = inputs.size();  // no file opened yet
	ifstream source;

	for (size_t index = 0; index < sourceChunks.size(); ++index)
	{
		const FstInputFile &input = inputs[sourceChunks[index].fileNr];
		const FstChunk &chunk = input.chunks[sourceChunks[index].chunkNr];
		const uint64_t chunkEnd = input.chunkEnd[sourceChunks[index].chunkNr];

		if (openFileNr != sourceChunks[index].fileNr)
		{
			if (source.is_open()) source.close();

			source.open(input.path.c_str(), ios::in | ios::binary);

			if (source.fail())
			{
				throw(runtime_error(FileError(input.path, FSTERROR_ERROR_OPENING_FILE)));
			}

			openFileNr = sourceChunks[index].fileNr;
		}

		const uint64_t newHeaderPos = outfile.tellp();
		const uint64_t delta = newHeaderPos - chunk.headerPos;  // modular arithmetic, works for both directions

		p_chunkPos[index] = newHeaderPos;
		p_chunkRows[index] = chunk.nrOfRows;

		// copy data chunk header and column data
		CopyBytes(source, outfile, chunk.headerPos, chunkEnd - chunk.headerPos, buffer);

		// Patch data chunk header [node E] with new column positions
		source.seekg(chunk.headerPos);
		source.read(dataHeader, dataHeaderSize);

		if (!source)
		{
			throw(runtime_error(FileError(input.path, FSTERROR_DAMAGED_CHUNKINDEX)));
		}

		uint64_t* positionData = reinterpret_cast<uint64_t*>(&dataHeader[DATA_INDEX_SIZE]);

		for (int colNr = 0; colNr < nrOfCols; ++colNr)
		{
			positionData[colNr] += delta;
		}

		*reinterpret_cast<uint64_t*>(dataHeader) = ZSTD_XXH64(&dataHeader[8], dataHeaderSize - 8, FST_HASH_SEED);

		outfile.seekp(newHeaderPos);
		outfile.write(dataHeader, dataHeaderSize);

		// Patch factor headers: the position of the level codes is an absolute file position
		for (int colNr = 0; colNr < nrOfCols; ++colNr)
		{
			if (colTypes[colNr] != 7) continue;

			char factorHeader[FACTOR_HEADER_SIZE];
			source.seekg(chunk.colPos[colNr]);
			source.read(factorHeader, FACTOR_HEADER_SIZE);

			if (!source)
			{
				throw(runtime_error(FileError(input.path, FSTERROR_DAMAGED_METADATA)));
			}

			*reinterpret_cast<uint64_t*>(&factorHeader[8]) += delta;

			outfile.seekp(chunk.colPos[colNr] + delta);
			outfile.write(factorHeader, FACTOR_HEADER_SIZE);
		}

		outfile.seekp(0, ios_base::end);
	}

	if (source.is_open()) source.close();

	// Write final chunk index
	*reinterpret_cast<uint64_t*>(chunkIndex.data()) = ZSTD_XXH64(&chunkIndex[8], chunkIndexSize - 8, FST_HASH_SEED);

	outfile.seekp(chunkIndexPos);
	outfile.write(chunkIndex.data(), chunkIndexSize);

	if (outfile.fail())
	{
		outfile.close();
		throw(runtime_error("There was an error during the write operation, fst file might be corrupted. Please check available disk space and access rights."));
	}

	outfile.close();
}


void FstConcat(const vector<string> &inputFiles, const string &outputFile)
{
	if (inputFiles.empty())
	{
		throw(runtime_error(FSTERROR_CONCAT_NO_INPUT));
	}

	for (const string &inputFile : inputFiles)
	{
		if (inputFile == outputFile)
		{
			throw(runtime_error(FSTERROR_CONCAT_SAME_FILE));
		}
	}

	// read and validate all headers before the output file is touched
	vector<FstInputFile> inputs(inputFiles.size());

	for (size_t fileNr = 0; fileNr < inputFiles.size(); ++fileNr)
	{
		ReadInputFile(inputFiles[fileNr], inputs[fileNr]);

		if (fileNr > 0)
		{
			ValidateInputFile(inputs[0], inputs[fileNr]);
		}
	}

	ValidateAnnotations(inputs);

	try
	{
		WriteConcatenation(inputs, outputFile);
	}
	catch (const std::runtime_error&)
	{
		std::remove(outputFile.c_str());  // don't leave an incomplete file
		throw;
	}
}
