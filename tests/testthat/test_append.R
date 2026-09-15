context("native column append")

test_that("native append validates inputs and preserves values", {
  path <- tempfile()
  on.exit(unlink(path))
  expect_null(fstcore:::fststore(path, data.frame(a = 1:4), 50L, TRUE))
  expect_null(fstcore:::fstappend(path, data.frame(b = 5:8), 100L, TRUE))
  result <- fstcore:::fstretrieve(path, NULL, 1L, NULL)
  expect_identical(result$resTable$a, 1:4)
  expect_identical(result$resTable$b, 5:8)
  for (bad in list(integer(), NA_integer_, c(1L, 2L), 101L, 50)) {
    expect_s3_class(fstcore:::fstappend(path, data.frame(c = 1:4), bad, TRUE), "fst_error")
  }
  for (bad in list(logical(), NA, c(TRUE, FALSE), 1L)) {
    expect_s3_class(fstcore:::fstappend(path, data.frame(c = 1:4), 50L, bad), "fst_error")
  }
  expect_s3_class(fstcore:::fstappend(path, data.frame(a = 1:4), 50L, TRUE), "fst_error")
  expect_s3_class(fstcore:::fstappend(path, data.frame(c = 1:3), 50L, TRUE), "fst_error")
  expect_identical(fstcore:::fstmetadata(path)$colNames, c("a", "b"))
})

test_that("corrupt and truncated files are rejected before mutation", {
  path <- tempfile()
  on.exit(unlink(path))
  fstcore:::fststore(path, data.frame(a = 1:4), 50L, TRUE)
  original <- readBin(path, "raw", file.info(path)$size)
  for (size in c(0, 20, 48, 100)) {
    writeBin(original[seq_len(size)], path)
    expect_s3_class(fstcore:::fstappend(path, data.frame(b = 1:4), 50L, TRUE), "fst_error")
    expect_identical(file.info(path)$size, as.double(size))
  }
  corrupt <- original
  corrupt[50] <- as.raw(bitwXor(as.integer(corrupt[50]), 1L))
  writeBin(corrupt, path)
  expect_s3_class(fstcore:::fstappend(path, data.frame(b = 1:4), 50L, TRUE), "fst_error")
  expect_identical(readBin(path, "raw", length(corrupt)), corrupt)
})
