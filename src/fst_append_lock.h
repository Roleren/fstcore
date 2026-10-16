// MPL-2.0. Cooperative exclusive lock for append writers.
// Declaration only: the platform code lives in fst_append_lock.cpp so that <windows.h> (typedef BYTE) is never
// included next to fstlib headers (enum FstColumnType::BYTE), which failed to compile on Windows.
#ifndef FST_APPEND_LOCK_H
#define FST_APPEND_LOCK_H
class FstAppendLock {
  void* handle;  // Windows HANDLE
  int fd;        // POSIX file descriptor
public:
  explicit FstAppendLock(const char* path);
  ~FstAppendLock();
  FstAppendLock(const FstAppendLock&) = delete;
  FstAppendLock& operator=(const FstAppendLock&) = delete;
};
#endif
