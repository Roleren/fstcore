# On-disk row appending (fork extension, format 3)

`FstStore::fstAppendRows()` writes new independently compressed row segments.
`fstcore:::fstappendrows(path, table, compression, uniform_encoding)` supplies R
validation and locking; `fst::append_rows_fst()` is the public R interface.
The segment reader supports all column types exposed by the R writer, including
factors, character, raw, integer64 and time attributes. Custom C++ BYTE_BLOCK
columns are not supported by row append.

## Layout and publication

The root header keeps the existing layout. Its minimum reader version at byte
24 becomes 3; its primary chunkset pointer points at the replacement metadata.
The table writer version remains 1. Standard writes still use minimum version 1;
column-only appends use minimum version 2 until rows are appended.

An append writes at EOF:

1. A replacement table/chunkset header, schema and column names.
2. The existing-layout chunk index and column position vector.
3. A versioned, checksummed row-segment manifest.
4. One independently compressed payload per new column or per appended column
   segment, using the existing codecs and block sizes.

The writer fills in the new offsets and checksums, flushes the stream, and
publishes the new 48-byte root header at offset zero. Existing payloads and old
metadata after byte 48 are unchanged. The final partial compression block of an
old segment remains as written; no tail-block recompression is necessary.

## Manifest

The manifest immediately follows the existing `96 + 24 + 8 * columns` byte
chunk index. All fields are little-endian unsigned 64-bit integers:

| Offset | Field |
| --- | --- |
| 0 | XXH64 of all remaining manifest bytes, using `FST_HASH_SEED` |
| 8 | Magic bytes `FSTROW01` |
| 16 | Complete manifest size, including the 32-byte header |
| 24 | Column count |
| 32 onward | For each column: segment count, followed by that many triples |

Each triple is `(first logical row, row count, absolute payload offset)`, using
zero-based rows. Segments must cover the complete table contiguously. A zero-row
original segment is allowed and retains the original schema/dictionary payload.
The old column position vector points to the first segment of each column.
Both metadata and data reads validate checksums, dimensions, coverage, offsets
and agreement with table/chunk row counts before decoding segments.

Versions 1 and 2 are represented in memory as one segment per column. Appending
rows adds one segment to every column. Appending a column adds a single segment
covering the entire current row count. Thus column and row appends can alternate
without forcing a new column to reproduce historical row-batch boundaries.

## Reader

The reader allocates each selected output column once. It locates intersecting
segments by binary search and decodes them directly at the appropriate output
offset. Numeric columns use shifted destination pointers; character columns use
an offset adapter; factor columns load the canonical dictionary once and decode
each segment's integer codes. No R-level `rbind()` or whole-column recopy occurs.

Row coordinates are 64-bit. R selectors retain their existing exact-double
limit of 2^53; validation includes a real 2^31 + 16 row segmented file and a full
read through the `data.table_long` capability path. This is not a claim that
every type has been allocated and tested at every 64-bit size. The legacy
factor writer's row length and empty-level reader loop also now retain 64 bits
instead of narrowing at 2^32.

## Schema and keys

R validation requires equal-length atomic columns with no dimensions, the same
unique names in the same order, and identical stored types/attributes/scales.
Factors require the same levels in the same order. Timestamp timezones must
match. No coercion or level merging is performed. A schema-valid empty batch is
a byte-for-byte no-op. A nonempty row append removes all stored keys because it
does not verify sortedness. Column appending retains the existing key behavior.

## Cost and limitations

Payload work is proportional to new rows, not old rows. Append memory is the
new batch plus metadata and codec buffers. However, the complete manifest is
rewritten each time: its size is `32 + 8*C + 24*sum(segments_per_column)` bytes.
Both metadata and row reads load the complete live manifest, even for a column
subset. Factor/timezone validation may read their small existing schema payloads.

Old manifests remain in the file. With C fixed columns and S row appends, live
metadata is O(C*S) and accumulated obsolete manifests are O(C*S^2). For 4,000
columns and 1,000 row appends, the live manifest is about 96 MB and all manifest
triples written total about 48 GB, excluding other metadata. Batch rows into
substantial appends; frequent tiny updates need compaction or a future paged /
incremental manifest design. Each segment also has its own codec indexes and
partially filled blocks. Reading and rewriting to a new file compacts it when
the table fits the selected export path's memory and row limits.

The R wrapper takes the same cooperative append lock as column append. Callers
must exclude ordinary writers and readers during publication. Stream flush is
not fsync; overwriting the root is not crash-atomic. An injected failure before
publication preserves the committed table but may leave orphaned bytes. A
failure during root publication can corrupt the header. This does not provide
transactions, power-loss recovery or multi-file atomicity. Linux behavior is
validated locally; Windows and the target server/network filesystem still need
deployment validation.

Unmodified upstream readers reject format 3. Where a table fits in a regular R
data frame, `write_fst(read_fst(path), another_path)` exports a compact format-1
file readable upstream. Do not overwrite the active file while it is in use.
