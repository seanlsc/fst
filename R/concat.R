#  fst - R package for ultra fast storage and retrieval of datasets
#
#  Copyright (C) 2017-present, Mark AJ Klik
#
#  This file is part of the fst R package.
#
#  The fst R package is free software: you can redistribute it and/or modify it
#  under the terms of the GNU Affero General Public License version 3 as
#  published by the Free Software Foundation.
#
#  The fst R package is distributed in the hope that it will be useful, but
#  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
#  FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
#  for more details.
#
#  You should have received a copy of the GNU Affero General Public License along
#  with the fst R package. If not, see <http://www.gnu.org/licenses/>.
#
#  You can contact the author at:
#  - fst R package source repository : https://github.com/fstpackage/fst


# absolute path of a (possibly not yet existing) file
full_file_path <- function(path) {
	if (file.exists(path)) {
		return(normalizePath(path, mustWork = TRUE))
	}

	file.path(normalizePath(dirname(path), mustWork = FALSE), basename(path))
}


#' Concatenate fst files
#'
#' Row-binds the tables stored in one or more `fst` files into a new `fst` file, without loading
#' the data into memory. The compressed column data of the input files is copied as-is to the
#' result file, so no data is decompressed or recompressed and memory usage is constant
#' (a few megabytes), irrespective of the size of the input files. The result is identical
#' to reading all files, combining them with `rbind` (or `data.table::rbindlist`) and writing
#' the result with `write_fst`, but much faster and without the memory overhead.
#'
#' All input files must have identical column names (in identical order) and identical column
#' types, including column classes and attributes such as time zones and time units.
#' Factor columns are allowed to have different levels: the levels of the result are the union
#' of the levels of all files, in order of first appearance (like `rbind`).
#'
#' The result file consists of multiple data chunks (one for each non-empty input file),
#' each with the compression settings of the input file it came from. Files with multiple
#' chunks are fully supported by `read_fst`, `fst` and `metadata_fst` (including random access to
#' rows and columns), but can only be read with fst version 0.9.9.9000 (fstcore 0.10.0.9000)
#' or later. Concatenating a single file (or a set of files with a single non-empty file) results
#' in a regular single chunk file that can be read by all fst versions.
#'
#' Keys stored in the input files (for example a `data.table` key) are not retained, as the
#' concatenated data is not guaranteed to be sorted.
#'
#' @param x character vector with the paths of the `fst` files to concatenate (in order).
#' The files can themselves be the result of an earlier concatenation.
#' @param path path of the resulting `fst` file. If the file exists, it will be overwritten.
#' The result file can not be one of the input files.
#' @return invisibly returns `path`.
#' @examples
#' fst_file1 <- tempfile(fileext = ".fst")
#' fst_file2 <- tempfile(fileext = ".fst")
#' fst_result <- tempfile(fileext = ".fst")
#'
#' write_fst(data.frame(A = 1:10, B = letters[1:10]), fst_file1)
#' write_fst(data.frame(A = 11:15, B = letters[11:15]), fst_file2)
#'
#' # concatenate without reading the data
#' concat_fst(c(fst_file1, fst_file2), fst_result)
#'
#' read_fst(fst_result)
#' @export
concat_fst <- function(x, path) {
	if (!is.character(x) || length(x) == 0 || anyNA(x)) {
		stop("Please specify a character vector with the paths of the fst files to concatenate.")
	}

	if (!is.character(path) || length(path) != 1 || is.na(path)) stop("Please specify a correct path.")

	missing_files <- x[!file.exists(x)]

	if (length(missing_files) > 0) {
		stop("File(s) not found: ", paste0("'", missing_files, "'", collapse = ", "))
	}

	input_paths <- normalizePath(x, mustWork = TRUE)
	output_path <- full_file_path(path)

	if (output_path %in% input_paths) {
		stop("The result file can not be one of the input files.")
	}

	res <- fstconcat(input_paths, output_path)

	if (inherits(res, "fst_error")) {
		stop(res)
	}

	invisible(path)
}
