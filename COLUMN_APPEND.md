# Column append extension

This development fork adds `fstcore:::fstappend(path, data.frame, compression,
uniformEncoding)` and exports the equivalent C-callable API for the fst package.
The public R entry point is `fst::append_fst()` in the matching fst fork.

## File layout

Ordinary `fstWrite` still emits upstream format version 1. Appending promotes
only that file to minimum reader version **2**, a private fork extension, not an
upstream format assignment. Upstream fstcore 0.10.0 rejects such files.

An append loads column names, types, keys and compressed payload offsets. It
writes a new metadata region at EOF, including the combined names and offsets,
then serializes only the new columns with the existing type-specific writers.
It flushes the output stream before overwriting the root 48-byte table header.
The root's `primaryChunkSetLoc` now points into the appended metadata region.
Readers follow this pointer and use absolute offsets for all column payloads.
Keys are duplicated into the relocated region and their positions stay fixed.

Every byte of the pre-existing file beyond the root header stays unchanged.
Repeated appends retain obsolete metadata. Cost is O(new column data + total
column metadata), with no decompression or copying of old column payloads.
No external sidecar or chain of library files is required for reading.

## Validation and coordination

The R native entry point validates scalar options, column names, column lengths,
and metadata checksums before writing. Names cannot replace existing columns.
Rows match by position, and row identity/order must be guaranteed by the caller.
The C++ `FstStore::fstAppend` requires metadata loaded on the same store and an
exclusive lock held by its caller for the whole read/modify/write sequence.

The R binding takes a non-blocking cooperative file lock: `flock` on POSIX and a
reserved distant byte lock on Windows. Only cooperating appenders honor it;
ordinary readers, `write_fst`, renames and external tools are not coordinated.
Callers must exclude these operations. Network filesystem locking must be
verified on the deployment server. Windows support is implemented but untested.

The root update is **not crash-atomic** and C++ stream flush is **not fsync**.
This API does not promise power-loss durability. A failed serialization before
publication leaves the old table readable, possibly with unused bytes at EOF.
A process/disk failure during publication can damage the root header. Keep
recoverable source libraries/backups; transaction journaling is future work.

## Compatibility and compaction

Use the matching fst/fstcore forks in every process that accesses appended
files. Existing upstream-format files remain readable without conversion.
To compact or export for upstream readers, use this fork to read the file and
`write_fst` the result to a different path. That operation emits version 1.
At 4,000 x 2,000,000 integer cells the materialized table alone is 32 GB, so
full compaction/export requires suitable memory just like the previous workflow.
