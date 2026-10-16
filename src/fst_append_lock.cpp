// MPL-2.0. Cooperative exclusive lock for append writers (see fst_append_lock.h).
#include <stdexcept>
#include "fst_append_lock.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Lock a reserved byte beyond supported file sizes, so ordinary I/O on separate handles remains possible
// (Windows byte locks are mandatory).
static OVERLAPPED fst_lock_region() {
  OVERLAPPED overlap = {};
  overlap.Offset = 0xfffffffe;
  overlap.OffsetHigh = 0x7fffffff;
  return overlap;
}

FstAppendLock::FstAppendLock(const char* path) : handle(NULL), fd(-1) {
  HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open fst file for append.");
  OVERLAPPED overlap = fst_lock_region();
  if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap)) {
    CloseHandle(h);
    throw std::runtime_error("Cannot lock fst file; another append may be in progress.");
  }
  handle = h;
}

FstAppendLock::~FstAppendLock() {
  OVERLAPPED overlap = fst_lock_region();
  UnlockFileEx((HANDLE) handle, 0, 1, 0, &overlap);
  CloseHandle((HANDLE) handle);
}
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

FstAppendLock::FstAppendLock(const char* path) : handle(nullptr), fd(-1) {
  fd = open(path, O_RDWR);
  if (fd < 0) throw std::runtime_error("Cannot open fst file for append.");
  if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
    close(fd);
    throw std::runtime_error("Cannot lock fst file; another append may be in progress.");
  }
}

FstAppendLock::~FstAppendLock() {
  flock(fd, LOCK_UN);
  close(fd);
}
#endif
