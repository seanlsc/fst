require(testthat)

context("concatenation")

source("fst_helpers.R")


concat_dir <- "concat_data"

if (dir.exists(concat_dir)) {
  unlink(concat_dir, recursive = TRUE, force = TRUE)
}

dir.create(concat_dir)


store <- function(table, name, compression = 50L) {
  path <- file.path(concat_dir, name)
  msg <- fstcore:::fststore(path, table, compression, TRUE)

  if ("fst_error" %in% class(msg)) stop(msg)

  path
}


test_that("concatenated chunks are read as a single table", {
  t1 <- data.frame(X = 1:10000, Y = as.raw(1:10000 %% 256), S = as.character(1:10000), stringsAsFactors = FALSE)
  t2 <- data.frame(X = -(1:333), Y = as.raw(1:333 %% 7), S = letters[1 + 1:333 %% 26], stringsAsFactors = FALSE)

  p1 <- store(t1, "a.fst", 0L)
  p2 <- store(t2, "b.fst", 100L)
  res <- file.path(concat_dir, "res.fst")

  expect_null(fstcore:::fstconcat(c(p1, p2, p1), res))

  expected <- rbind(t1, t2, t1)
  rownames(expected) <- NULL

  y <- fstcore:::fstretrieve(res, NULL, 1L, NULL)$resTable
  expect_equal(as.data.frame(y, stringsAsFactors = FALSE), expected)

  y <- fstcore:::fstretrieve(res, c("S", "X"), 9990L, 10400L)$resTable
  expect_equal(y$X, expected$X[9990:10400])
  expect_equal(y$S, expected$S[9990:10400])

  meta <- fstcore:::fstmetadata(res)
  expect_equal(meta$nrOfRows, 20333)
  expect_equal(meta$nrOfChunks, 3)
  expect_equal(meta$fstVersion, 2)

  # single chunk files are unchanged
  expect_equal(fstcore:::fstmetadata(p1)$nrOfChunks, 1)
  expect_equal(fstcore:::fstmetadata(p1)$fstVersion, 1)
})


test_that("factor levels are merged", {
  p1 <- store(data.frame(F = factor(c("b", "a", NA), levels = c("b", "a"))), "f1.fst")
  p2 <- store(data.frame(F = factor(c("c", "a"))), "f2.fst")
  res <- file.path(concat_dir, "f.fst")

  fstcore:::fstconcat(c(p1, p2), res)
  y <- fstcore:::fstretrieve(res, NULL, 1L, NULL)$resTable

  expect_equal(levels(y$F), c("b", "a", "c"))
  expect_equal(as.character(y$F), c("b", "a", NA, "c", "a"))
})


test_that("errors are reported", {
  p1 <- store(data.frame(A = 1:3), "e1.fst")
  p2 <- store(data.frame(A = c(1, 2)), "e2.fst")
  res <- file.path(concat_dir, "e.fst")

  msg <- fstcore:::fstconcat(c(p1, p2), res)
  expect_true(inherits(msg, "fst_error"))
  expect_true(grepl("type 'integer'", msg))
  expect_false(file.exists(res))

  msg <- fstcore:::fstconcat(character(0), res)
  expect_true(inherits(msg, "fst_error"))

  msg <- fstcore:::fstconcat(c(p1, res), res)
  expect_true(inherits(msg, "fst_error"))
})


unlink(concat_dir, recursive = TRUE, force = TRUE)
