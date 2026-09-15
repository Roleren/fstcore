// MPL-2.0. Cooperative exclusive lock for append writers.
#ifndef FST_APPEND_LOCK_H
#define FST_APPEND_LOCK_H
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif
class FstAppendLock {
#ifdef _WIN32
  HANDLE handle;
  OVERLAPPED overlap = {};
#else
  int fd;
#endif
public:
  explicit FstAppendLock(const char* path) {
#ifdef _WIN32
    handle = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open fst file for append.");
    // Lock a reserved byte beyond supported file sizes, so ordinary I/O on
    // separate handles remains possible (Windows byte locks are mandatory).
    overlap.Offset = 0xfffffffe;
    overlap.OffsetHigh = 0x7fffffff;
    if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap)) {
      CloseHandle(handle);
      throw std::runtime_error("Cannot lock fst file; another append may be in progress.");
    }
#else
    fd = open(path, O_RDWR);
    if (fd < 0) throw std::runtime_error("Cannot open fst file for append.");
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
      close(fd);
      throw std::runtime_error("Cannot lock fst file; another append may be in progress.");
    }
#endif
  }
  ~FstAppendLock() {
#ifdef _WIN32
    UnlockFileEx(handle, 0, 1, 0, &overlap);
    CloseHandle(handle);
#else
    flock(fd, LOCK_UN);
    close(fd);
#endif
  }
  FstAppendLock(const FstAppendLock&) = delete;
  FstAppendLock& operator=(const FstAppendLock&) = delete;
};
#endif
