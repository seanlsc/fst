context("concat")


# separate directory, other test files clean the testdata directory
concat_dir <- "testconcat"

if (file.exists(concat_dir)) {
  unlink(concat_dir, recursive = TRUE)
}

dir.create(concat_dir)


concat_path <- function(name) {
  file.path(concat_dir, paste0(name, ".fst"))
}


sample_table <- function(nr_of_rows, levels = c("a", "b", "c")) {
  data.frame(
    Int = sample(c(1:100, NA), nr_of_rows, replace = TRUE),
    Double = sample(c(runif(10), NA), nr_of_rows, replace = TRUE),
    Logical = sample(c(TRUE, FALSE, NA), nr_of_rows, replace = TRUE),
    Character = sample(c(LETTERS, NA), nr_of_rows, replace = TRUE),
    Factor = factor(sample(c(levels, NA), nr_of_rows, replace = TRUE), levels = levels),
    Ordered = factor(sample(c("low", "high"), nr_of_rows, replace = TRUE), levels = c("low", "high"), ordered = TRUE),
    Date = as.Date("2020-01-01") + sample(0:1000, nr_of_rows, replace = TRUE),
    IntDate = structure(sample(0:1000, nr_of_rows, replace = TRUE), class = c("IDate", "Date")),
    Posix = as.POSIXct("2020-01-01", tz = "UTC") + sample(0:1e6, nr_of_rows, replace = TRUE),
    Difftime = as.difftime(runif(nr_of_rows), units = "mins"),
    Raw = as.raw(sample(0:255, nr_of_rows, replace = TRUE)),
    stringsAsFactors = FALSE
  )
}


rbind_tables <- function(tables) {
  res <- do.call(rbind, tables)
  rownames(res) <- NULL
  res
}


# write tables, concatenate and compare with rbind result
test_concat <- function(tables, compress = 50, name = "result") {
  paths <- vapply(seq_along(tables), function(i) concat_path(paste0(name, "_in", i)), character(1))

  for (i in seq_along(tables)) {
    write_fst(tables[[i]], paths[i], compress)
  }

  result <- concat_path(name)
  testthat::expect_equal(concat_fst(paths, result), result)

  expected <- rbind_tables(tables)
  testthat::expect_equal(read_fst(result), expected)

  invisible(list(path = result, expected = expected))
}


test_that("concatenation equals rbind of the tables", {
  for (compress in c(0, 30, 70, 100)) {
    tables <- list(sample_table(10000), sample_table(7777), sample_table(1), sample_table(4097))
    res <- test_concat(tables, compress, paste0("compress_", compress))

    meta <- metadata_fst(res$path)
    expect_equal(meta$nrOfRows, 10000 + 7777 + 1 + 4097)
    expect_equal(meta$nrOfChunks, 4)
  }
})


test_that("row and column selection on concatenated file", {
  tables <- list(sample_table(5000), sample_table(3), sample_table(9000), sample_table(100))
  res <- test_concat(tables, 60, "selection")

  nr_of_rows <- nrow(res$expected)

  # ranges within and across chunk boundaries
  ranges <- list(
    c(1, 1), c(1, 5000), c(5000, 5001), c(5001, 5003), c(5002, 5004), c(4990, 14010),
    c(14003, 14004), c(14004, nr_of_rows), c(nr_of_rows, nr_of_rows), c(1, nr_of_rows)
  )

  for (k in 1:30) {
    from <- sample(nr_of_rows, 1)
    ranges[[length(ranges) + 1]] <- c(from, sample(from:nr_of_rows, 1))
  }

  for (range in ranges) {
    from <- range[1]
    to <- range[2]
    expected <- res$expected[from:to, , drop = FALSE]
    rownames(expected) <- NULL

    expect_equal(read_fst(res$path, from = from, to = to), expected, info = paste(from, to))
  }

  # column selection
  cols <- c("Factor", "Int", "Character")
  expect_equal(read_fst(res$path, cols, 4000, 6000), res$expected[4000:6000, cols], check.attributes = FALSE)

  # to beyond last row
  expect_equal(nrow(read_fst(res$path, from = nr_of_rows - 1, to = nr_of_rows + 1000)), 2)
})


test_that("fst table interface on concatenated file", {
  tables <- list(sample_table(300), sample_table(200))
  res <- test_concat(tables, 50, "fst_table")

  ft <- fst(res$path)

  expect_equal(nrow(ft), 500)
  expect_equal(ft[250:260, "Int"], res$expected[250:260, "Int"])
  expect_equal(ft$Factor, res$expected$Factor)
  expect_equal(ft[[4]], res$expected[[4]])
})


test_that("factor levels are merged", {
  t1 <- data.frame(F = factor(c("a", "b", "a", NA), levels = c("b", "a")))
  t2 <- data.frame(F = factor(c("c", NA, "a"), levels = c("c", "a", "unused")))
  t3 <- data.frame(F = factor(c(NA, NA), levels = character(0)))
  t4 <- data.frame(F = factor(c("b", "d")))

  res <- test_concat(list(t1, t2, t3, t4), 50, "factor_levels")

  y <- read_fst(res$path)
  expect_equal(levels(y$F), c("b", "a", "c", "unused", "d"))

  # levels are independent of selected rows
  expect_equal(levels(read_fst(res$path, from = 5, to = 7)$F), levels(y$F))
  expect_equal(as.character(read_fst(res$path, from = 5, to = 7)$F), c("c", NA, "a"))

  # many levels (stored with larger codes)
  t5 <- data.frame(F = factor(paste0("x", sample(1:40000, 10000, replace = TRUE))))
  t6 <- data.frame(F = factor(paste0("x", sample(20000:60000, 10000, replace = TRUE))))
  test_concat(list(t5, t6, t1), 50, "factor_many_levels")
})


test_that("identical factor levels", {
  lv <- c("z", "y", "x")
  t1 <- data.frame(F = factor(sample(lv, 100, replace = TRUE), levels = lv))
  t2 <- data.frame(F = factor(sample(lv, 100, replace = TRUE), levels = lv))

  res <- test_concat(list(t1, t2), 50, "factor_identical")
  expect_equal(levels(read_fst(res$path)$F), lv)
})


test_that("character encodings", {
  utf8 <- enc2utf8(c("été", "naïve", "中文", NA))
  latin1 <- iconv(c("café", "olé"), "UTF-8", "latin1")

  t1 <- data.frame(S = utf8, stringsAsFactors = FALSE)
  t2 <- data.frame(S = latin1, stringsAsFactors = FALSE)
  t3 <- data.frame(S = c("abc", "def"), stringsAsFactors = FALSE)

  res <- test_concat(list(t1, t2, t3), 50, "encoding")

  y <- read_fst(res$path)
  expect_equal(Encoding(y$S)[1:3], rep("UTF-8", 3))
  expect_equal(Encoding(y$S)[5:6], rep("latin1", 2))

  f1 <- data.frame(F = factor(utf8))
  f2 <- data.frame(F = factor(latin1))
  res <- test_concat(list(f1, f2), 50, "encoding_factor")
  y <- read_fst(res$path)
  expect_equal(levels(y$F), c(levels(f1$F), levels(f2$F)))
})


test_that("integer64 and nanotime columns", {
  skip_if_not_installed("bit64")

  t1 <- data.frame(I64 = bit64::as.integer64(c(1, 2, NA, 2^40)))
  t2 <- data.frame(I64 = bit64::as.integer64(c(-5, 7)))
  test_concat(list(t1, t2), 50, "int64")

  skip_if_not_installed("nanotime")

  n1 <- data.frame(N = nanotime::nanotime(1:10))
  n2 <- data.frame(N = nanotime::nanotime(100:105))
  test_concat(list(n1, n2), 50, "nanotime")
})


test_that("large tables with misaligned blocks", {
  t1 <- data.frame(X = 1:123457, Y = rnorm(123457), Z = sample(c(TRUE, FALSE), 123457, replace = TRUE))
  t2 <- data.frame(X = 1:65537, Y = rnorm(65537), Z = sample(c(TRUE, FALSE), 65537, replace = TRUE))
  res <- test_concat(list(t1, t2, t1), 40, "large")

  expect_equal(read_fst(res$path, from = 123450, to = 123470), res$expected[123450:123470, ], check.attributes = FALSE)
})


test_that("concatenation of concatenated files", {
  tables <- lapply(c(10, 20, 30, 40), sample_table)
  paths <- vapply(1:4, function(i) concat_path(paste0("nested_in", i)), character(1))
  for (i in 1:4) write_fst(tables[[i]], paths[i])

  concat_fst(paths[1:2], concat_path("nested_a"))
  concat_fst(paths[3:4], concat_path("nested_b"))
  concat_fst(c(concat_path("nested_a"), concat_path("nested_b"), paths[1]), concat_path("nested"))

  expect_equal(read_fst(concat_path("nested")), rbind_tables(c(tables, tables[1])))
  expect_equal(metadata_fst(concat_path("nested"))$nrOfChunks, 5)
})


test_that("many files", {
  tables <- lapply(1:200, function(i) sample_table(sample(1:50, 1)))
  test_concat(tables, 50, "many")
})


test_that("empty tables", {
  t0 <- sample_table(10)[0, ]
  t1 <- sample_table(10)

  res <- test_concat(list(t0, t1, t0), 50, "empty_mixed")
  expect_equal(metadata_fst(res$path)$nrOfChunks, 1)  # empty chunks are skipped

  res <- test_concat(list(t0, t0), 50, "empty_all")
  expect_equal(nrow(read_fst(res$path)), 0)
  expect_equal(metadata_fst(res$path)$nrOfChunks, 1)
})


test_that("single input file", {
  t1 <- sample_table(1000)
  res <- test_concat(list(t1), 50, "single")

  meta <- metadata_fst(res$path)
  expect_equal(meta$nrOfChunks, 1)
})


test_that("keys are dropped", {
  skip_if_not_installed("data.table")

  t1 <- data.table::data.table(A = c(3L, 1L, 2L), B = c("x", "y", "z"))
  t2 <- data.table::data.table(A = c(0L, 5L), B = c("u", "v"))
  data.table::setkey(t1, A)
  data.table::setkey(t2, A)

  write_fst(t1, concat_path("keys1"))
  write_fst(t2, concat_path("keys2"))
  concat_fst(c(concat_path("keys1"), concat_path("keys2")), concat_path("keys"))

  expect_null(metadata_fst(concat_path("keys"))$keys)

  y <- read_fst(concat_path("keys"), as.data.table = TRUE)
  expect_null(data.table::key(y))
  expect_equal(y$A, c(1L, 2L, 3L, 0L, 5L))
})


test_that("print shows number of chunks", {
  tables <- list(sample_table(3), sample_table(4))
  res <- test_concat(tables, 50, "print")

  expect_output(print(metadata_fst(res$path)), "7 rows, 11 columns, 2 data chunks")
})


test_that("existing result file is overwritten", {
  tables <- list(sample_table(3), sample_table(4))
  res <- test_concat(tables, 50, "overwrite")

  write_fst(sample_table(100), res$path)
  concat_fst(c(concat_path("overwrite_in1"), concat_path("overwrite_in2")), res$path)
  expect_equal(read_fst(res$path), res$expected)
})


test_that("column mismatch errors", {
  t1 <- data.frame(A = 1:3, B = c("a", "b", "c"), stringsAsFactors = FALSE)
  write_fst(t1, concat_path("base"))

  check_error <- function(x, regex) {
    write_fst(x, concat_path("other"))
    expect_error(concat_fst(c(concat_path("base"), concat_path("other")), concat_path("err")), regex)
    expect_false(file.exists(concat_path("err")))
  }

  check_error(data.frame(A = 1:3), "different number of columns")
  check_error(data.frame(B = 1:3, A = c("a", "b", "c"), stringsAsFactors = FALSE), "named 'A'.*but 'B'")
  check_error(
    data.frame(A = c(1, 2, 3), B = c("a", "b", "c"), stringsAsFactors = FALSE),
    "'A'.*type 'integer'.*but 'double'"
  )
  check_error(data.frame(A = 1:3, B = factor(c("a", "b", "c"))), "'B'.*type 'character'.*but 'factor'")
  check_error(
    data.frame(A = structure(1:3, class = "Date"), B = c("a", "b", "c"), stringsAsFactors = FALSE),
    "'A'.*type 'integer'.*but 'integer date"
  )

  f1 <- data.frame(F = factor("a"))
  f2 <- data.frame(F = factor("a", ordered = TRUE))
  write_fst(f1, concat_path("f1"))
  write_fst(f2, concat_path("f2"))
  expect_error(concat_fst(c(concat_path("f1"), concat_path("f2")), concat_path("err")), "ordered factor")
})


test_that("attribute mismatch errors", {
  p1 <- data.frame(P = as.POSIXct("2020-01-01", tz = "UTC"))
  p2 <- data.frame(P = as.POSIXct("2020-01-01", tz = "Europe/Amsterdam"))
  write_fst(p1, concat_path("p1"))
  write_fst(p2, concat_path("p2"))
  expect_error(
    concat_fst(c(concat_path("p1"), concat_path("p2")), concat_path("err")),
    "time zone or unit.*'UTC'.*'Europe/Amsterdam'"
  )

  d1 <- data.frame(D = as.difftime(1, units = "secs"))
  d2 <- data.frame(D = as.difftime(1, units = "hours"))
  write_fst(d1, concat_path("d1"))
  write_fst(d2, concat_path("d2"))
  expect_error(concat_fst(c(concat_path("d1"), concat_path("d2")), concat_path("err")), "different scale or time unit")

  # identical attributes are fine
  test_concat(list(p2, p2), 50, "posix_same_tz")
})


test_that("argument errors", {
  write_fst(sample_table(3), concat_path("arg"))

  expect_error(concat_fst(character(0), concat_path("err")), "Please specify a character vector")
  expect_error(concat_fst(NA_character_, concat_path("err")), "Please specify a character vector")
  expect_error(concat_fst(1, concat_path("err")), "Please specify a character vector")
  expect_error(concat_fst(concat_path("arg"), 3), "Please specify a correct path")
  expect_error(concat_fst(concat_path("arg"), c("a", "b")), "Please specify a correct path")
  expect_error(concat_fst(c(concat_path("arg"), concat_path("does_not_exist")), concat_path("err")), "not found")
  expect_error(concat_fst(c(concat_path("arg"), concat_path("arg")), concat_path("arg")), "can not be one of the input")

  # input file that is not a fst file
  writeLines("not a fst file", concat_path("not_fst"))
  expect_error(concat_fst(c(concat_path("arg"), concat_path("not_fst")), concat_path("err")), "not_fst")
  expect_false(file.exists(concat_path("err")))
})


test_that("damaged chunk index is detected", {
  tables <- list(sample_table(30), sample_table(40))
  res <- test_concat(tables, 50, "damaged")

  raw_file <- readBin(res$path, "raw", file.size(res$path))

  # locate the chunk row counts (30 and 40, stored as consecutive 64-bit integers) in the chunk index
  pattern <- c(writeBin(c(30L, 0L, 40L, 0L), raw(), size = 4))
  starts <- seq_len(length(raw_file) - length(pattern) + 1)
  pos <- Filter(function(p) identical(raw_file[p:(p + length(pattern) - 1)], pattern), starts)

  expect_equal(length(pos), 1)

  # corrupt row count of second chunk
  raw_file[pos + 8] <- as.raw(41)
  writeBin(raw_file, concat_path("damaged_copy"))

  expect_error(read_fst(concat_path("damaged_copy")), "chunk index header is damaged")
  expect_error(metadata_fst(concat_path("damaged_copy")), "chunk index header is damaged")
})


# cleanup
unlink(concat_dir, recursive = TRUE)
