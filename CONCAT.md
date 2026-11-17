# Concatenating fst files without loading them

This branch adds `concat_fst()` to fst. It row-binds fst files into a new fst file by copying their
compressed data blocks directly. Nothing is decompressed, and peak memory stays at a small copy buffer.

```r
concat_fst(c("file1.fst", "file2.fst"), "file3.fst")
```

## Repository layout

The work spans two R packages, both in this repository:

| Directory  | Package                          | Change                                                                |
|------------|----------------------------------|-----------------------------------------------------------------------|
| `fstcore/` | fstcore 0.10.0.9000              | format extension (multi-chunk files), multi-chunk reader, `FstConcat()` |
| `.` (root) | fst 0.9.9.9000                   | R function `concat_fst()`, `nrOfChunks` in `metadata_fst()`, tests      |

`fstcore/` is a complete copy of [fstpackage/fstcore](https://github.com/fstpackage/fstcore) (commit `bea65eb`)
with the changes applied on top (see git history). It is excluded from the fst package build via `.Rbuildignore`,
so it can be moved to its own repository without changes.

## Installation

fstcore must be installed first (fst requires fstcore >= 0.10.0.9000):

```bash
R CMD INSTALL fstcore
```

```bash
R CMD INSTALL .
```

If the LZ4 and ZSTD development libraries are not installed on the system, build fstcore with the bundled
libraries:

```bash
BUILD_INTERNAL_LIBS=true R CMD INSTALL fstcore
```

## Format change

The fst format already contained a chunk index with 4 (vertical) chunk slots, of which only the first was ever
used. The chunk index now has a variable number of slots (`32 + 16 * nrOfChunkSlots` bytes, maximum 65535 slots)
and a field with the number of used slots (a previously free field that is zero in existing files). Every data
chunk has its own data chunk header with the absolute positions of its column data. See
`fstcore/src/fstlib/interface/fstchunks.h` for the layout.

Compatibility:

* `write_fst()` output is byte-identical to before (format version 1), so old fst versions can still read new
  files.
* Files with multiple chunks (from `concat_fst()` with more than one non-empty input) have minimum reader version 2.
  Older fst versions refuse them with `Incompatible fst file: file was created by a newer version of fst`
  instead of reading wrong data.
* All existing files (single chunk, version 1) are read as before.

Format version 2 is even, which by the existing convention marks a development version. Use an odd number for a
release.

## How concatenation works

1. Read the header, column names, chunk index and data chunk headers of every input file. Validate column names,
   types, attributes (class), scales (for example difftime units) and annotations (for example time zones). On a
   mismatch, report an error before the output file is created.
2. Write a new header taken from the first file, with the total row count, no keys and a chunk index with one slot
   per non-empty input chunk.
3. Copy each data chunk byte-for-byte using a 4 MB buffer, then patch the only absolute file offsets: the column
   positions in the data chunk header, and the level-code position in each factor column header.

Reading a multi-chunk file reads each requested column chunk by chunk into the result vectors. For factors, the
levels of all chunks are merged in order of first appearance (same as `rbind`), independent of the selected rows.

## Measurements

Two files of 15 million rows each (6 columns: integer, double, factor, logical, character and POSIXct; 380 MB each
at compression 50), files in the page cache:

| Method                                         | Time   | Peak memory (RSS) |
|------------------------------------------------|--------|-------------------|
| `concat_fst()`                                 | 0.48 s | 72 MB (mostly R)  |
| `read_fst()` x 2 + `rbindlist()` + `write_fst()` | 3.4 s  | 2.1 GB            |

Reading the concatenated file is as fast as reading the equivalent single-chunk file, for full reads and for row
ranges.

## Limitations and possible next steps

* Keys are not kept, because the concatenated data is not guaranteed to be sorted.
* Column names, types and order must be identical in all files.
* Each input chunk stays a separate chunk with its own compression settings. Many small chunks work, but if needed a
  "compact" step (read and rewrite) can be added later.
* In-place appending (`append = TRUE`) is not implemented. The format allows it: legacy files have 3 free chunk
  slots, and an index with spare slots can be reserved.
