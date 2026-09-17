context("native column replacement")

test_that("native replacement validates arguments before mutation", {
  path <- tempfile(); on.exit(unlink(path))
  x <- data.frame(a = 1:3, b = letters[1:3])
  expect_null(fstcore:::fststore(path, x, 50L, TRUE))
  original <- readBin(path, "raw", file.info(path)$size)
  for (bad in list(integer(), NA_integer_, c(1L, 2L), -1L, 101L, 50))
    expect_s3_class(fstcore:::fstreplacecolumns(path, x, bad, TRUE), "fst_error")
  for (bad in list(logical(), NA, c(TRUE, FALSE), 1L))
    expect_s3_class(fstcore:::fstreplacecolumns(path, x, 50L, bad), "fst_error")
  for (bad in list(NULL, list(), 1:3, list(c = 1:3), list(a = c(1, 2, 3)), list(a = 1L)))
    expect_s3_class(fstcore:::fstreplacecolumns(path, bad, 50L, TRUE), "fst_error")
  expect_identical(readBin(path, "raw", file.info(path)$size), original)
  expect_null(fstcore:::fstreplacecolumns(path, list(b = rev(x$b)), 50L, TRUE))
  expect_identical(fstcore:::fstretrieve(path, NULL, 1, NULL)$resTable,
    list(a = 1:3, b = rev(x$b)))
})

test_that("damaged files are rejected without writing", {
  path <- tempfile(); on.exit(unlink(path))
  fstcore:::fststore(path, data.frame(a = 1:3), 50L, TRUE)
  original <- readBin(path, "raw", file.info(path)$size)
  for (size in c(0, 20, 48, 100)) {
    truncated <- original[seq_len(size)]; writeBin(truncated, path)
    expect_s3_class(fstcore:::fstreplacecolumns(path, list(a = 3:1), 50L, TRUE), "fst_error")
    expect_identical(readBin(path, "raw", file.info(path)$size), truncated)
  }
})
