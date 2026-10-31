# CPU cost of the binary CDC serializer

Recommendations for the implementation in [PR 56373](https://github.com/ydb-platform/ydb/pull/56373), at `3d78c7dd`.

The binary codec already removes the per-cell text conversions of `FORMAT JSON`: Base64 for `String`, parsing of `Json` and `JsonDocument`, YSON-to-JSON, and decimal/date formatting. The remaining source-side CPU cost is in `TBinaryCdcSerializer`, not in the cell layout. Changing the version-1 cell encoding will not move the needle. The changes below are ordered by expected effect. The first three keep the version-1 framing. The fourth changes only how an autopartitioned topic selects a partition.

`TCdcChangeSenderPartition` is started with `RegisterWithSameMailbox` (`TChangeSender::CreateSendersImpl`). Serialization therefore runs on the source datashard tablet thread. It is not inside the user transaction that produced the change, but a slow serialize blocks every other event on that tablet until it returns.

## Where the time goes

`TBinaryCdcSerializer::FillDataChunk` (`ydb/core/tx/datashard/change_record_cdc_serializer.cpp`) does the following for every data record:

1. `BuildColumnMap` walks every column, sorts non-key names, and builds `TagToIndex`. Erase records pay this too.
2. `CollectValues` parses a `TSerializedCellVec` and `assign`s every non-empty value into its own `TString`.
3. `TBinaryCdcWriter::WriteCell` copies those bytes again. `Start` reserves 64 bytes, so a large value reallocates the output `TString` as it grows.
4. `StoreBinaryPayload` compresses the finished buffer and keeps the compressed bytes only when they are strictly shorter.

Key cells skip step 2. `AddKey` reads them from the `TSerializedCellVec` that is still alive in `FillDataChunk`.

`StoreBinaryPayload` constructs a `TZstdCompress` per record. That constructor calls `ZSTD_createCStream` and `ZSTD_initCStream`. The destructor calls `Finish`, which runs `ZSTD_endStream`. `quality = 0` is not level 0: `zstd.h` defines it as `ZSTD_CLEVEL_DEFAULT` (3). A one-shot buffer does not need the streaming API.

The same function compresses heartbeats, erases, and schema snapshots. A heartbeat written by this serializer is exactly 24 bytes: the 8-byte header plus `step` and `txId`. `LeavesIncompressibleAndTinyRecordsRaw` expects that record to stay `RAW`. The `CStream` is still created and destroyed.

`TBinaryCdcSerializer::Serialize` does not set `TCmdWrite.PartitionKey`. Partition selection is separate:

- Without topic autopartitioning, `ECdcStreamFormatBinary` uses `CreateDefaultPartitionResolver`, which compares raw key cells with `ResolveSchemaBoundaryPartitionId`. No JSON.
- With autopartitioning, `TBoundaryPartitionResolver` calls `TChangeRecord::GetPartitionKey()`. That parses the key again, runs `SerializeJsonKey` (`ToJson` per column, then `WriteJson`), and MD5s the text. The result is cached on the record. `TBoundaryChooser::GetPartition` hashes that string once more to place it against topic bounds.

## 1. Stop compressing every record on the datashard

This is the change that matters. A binary record has no repeated column names, so it has less redundancy than the JSON record it replaces. Small rows, erases, and high-entropy values usually do not come out shorter, but level-3 streaming compression has already run.

Keep the existing wire rule. Set `NPersQueueCommon::ZSTD` only when the compressed size is strictly smaller, and set `TCmdWrite.UncompressedSize` only in that case, and only when the raw size fits in `i32` (the current `Max<i32>()` check). Readers already accept both `RAW` (`codec = 0`) and `ZSTD` (`codec = 3`, mapped to the SDK codec with `+ 1`).

In `StoreBinaryPayload`:

- Write heartbeats as `RAW` and do not call zstd. The payload is 24 bytes; a frame that fails to shrink still costs a `CStream`.
- Below a size cutoff, write data records as `RAW` without calling zstd. Start the cutoff at 512 bytes so `CompressesRepetitivePayload` (1024 identical bytes) still takes the compress path. Treat 512 as a starting value, measure it, and put the chosen constant in the test. Do not leave the cutoff implicit in zstd's frame overhead.
- At or above the cutoff, compress with one `ZSTD_compress2` into a `ZSTD_CCtx` stored on the serializer. The serializer is a member of one `TCdcChangeSenderPartition` and is not shared across threads, so the context does not need a lock.
- Allocate the destination with `TString::Uninitialized(ZSTD_compressBound(n))`. `TString::resize(n)` zero-fills `n` bytes before compression and adds another memcpy-sized cost on large values. Shrink the string to the returned size afterwards.
- Compare with `got < payload.size()`, matching today's strict inequality.
- Do not keep level 3 as the data-record default without a measurement. Context setup dominates small records, and level 3 is substantially more expensive than level 1. Benchmark level 1 and one negative fast level (`ZSTD_c_compressionLevel`) on the cases in the last section, and ship the cheaper one that still shrinks the 1024-byte repetitive row.
- Leave schema snapshots on level 3. They are DDL-rare and their body is the same repetitive JSON document `FORMAT JSON` already produces (`TYdbJsonSerializer::RenderSchemaChangeJson`). Set the context parameter when switching between a data record and a snapshot.

Sketch, omitting the `i32` check and error text for brevity:

```cpp
void StoreBinaryPayload(TDataChunk& data, TString payload, int level) {
    if (payload.size() < MinCompressBytes) {
        data.SetData(std::move(payload));
        return;
    }
    ZSTD_CCtx_setParameter(Ctx_, ZSTD_c_compressionLevel, level);
    const size_t bound = ZSTD_compressBound(payload.size());
    TString compressed(TString::Uninitialized(bound));
    const size_t got = ZSTD_compress2(
        Ctx_, (char*)compressed.data(), bound, payload.data(), payload.size());
    if (ZSTD_isError(got) || got >= payload.size()) {
        data.SetData(std::move(payload));
        return;
    }
    compressed.resize(got);
    data.SetCodec(NPersQueueCommon::ZSTD);
    if (payload.size() <= static_cast<size_t>(Max<i32>())) {
        UncompressedSize = static_cast<ui32>(payload.size());
    }
    data.SetData(std::move(compressed));
}
```

Do not pack several CDC records into one topic message to amortize a context. Offsets, schema barriers, and seqnos are per message. If the remaining goal is disk bytes, compress in the topic partition tablet over a blob that already contains many messages, with one context per blob. That work is not on the datashard thread.

A zstd dictionary is worth trying only if, after the cutoff and the lower level, a large fraction of big records from the same schema still stays `RAW`. It is not part of the first change.

## 2. Cache the column map

After short records stop entering zstd, `BuildColumnMap` is the next schema-sized cost. It sorts every non-key column name on every record, including erases, which never consult the value index.

The map is a pure function of the schema snapshot held by `TChangeRecord::GetSchema()` (`TUserTable::TCPtr`). An alter builds a new `TUserTable` (`TUserTable(const TUserTable&, const TTableDescription&)`), and the record keeps the snapshot it was created with. Cache one `TColumnMap` on the serializer. Rebuild it when either the `TUserTable` pointer or `GetTableSchemaVersion()` changes. The pointer distinguishes successive snapshots. The version catches an in-place bump on an object the sender is still holding.

The cache must store key tags and types in primary-key order, value types and name-sorted indexes, `TagToIndex`, and `Wide`. An erase then only parses its key `TSerializedCellVec` and writes the key.

`TUserTable::GetSchema()` is a different function. It returns the serialized `TTableDescription` string. Do not use it as the cache key.

## 3. Copy each value once

`CollectValues` copies into `TIndexedCell::Storage` so the bytes outlive the `TSerializedCellVec`. In `FillDataChunk` that vector is alive until `writer.Finish()`. The extra `TString` per column is unnecessary. Pass the source `TCell` into `AddValue` / `AddOldImage` / `AddNewImage`, as `AddKey` already does.

The static empty byte in `TIndexedCell::View` exists only because a zero-length value is not copied and `View` still has to return a non-null pointer. The source cell already has the right null bit and size. Keep using it, including a zero-length non-null cell.

Reserve the output once, before the first `Append`. The size is fixed by data already in hand:

- 8 byte header;
- 2 bytes for each image count that is present;
- 16 bytes when `HasTs` is set;
- 1 marker byte per key column when `NullKeys` is set;
- for each non-null cell, `CellFixedSize(type)` or `4 + cell.Size()`;
- 1 byte per value index, or 2 when `Wide` is set;
- `2 + size` for the user SID and the trace id when those flags are set.

`TString` growth is geometric. One `reserve` removes the extra copies of a large value. `CellFixedSize` is a type-id switch already required by `WriteCell`; calling it once more while counting is noise next to a copy.

Sorting the touched value columns by index is cheap and keeps the bytes stable for tests. The reader accepts any index order (`ReadIndexed` does not require them sorted). Leave the sort.

`map.Wide = map.Values.size() > 127` emits a `u16` index for a table of 128 non-key columns, whose largest index is 127 and still fits in 7 bits. That costs one extra byte per index. It is not a CPU item. `WideIndexStartsAfter127ValueColumns` locks the current threshold; update that test if the predicate changes.

## 4. Hash raw key bytes when the topic is autopartitioned

This cost exists only on the autopartitioned path. `GetPartitionKey` re-parses the key and runs `ToJson` on every key column. `String` becomes Base64. `Decimal`, `Pg`, `Json`, `JsonDocument`, and `Yson` go through their text conversions. The MD5 hex is then hashed again inside `TBoundaryChooser::GetPartition`.

For a binary stream, pass the chooser a stable hash of the key's raw cell bytes (the same bytes `AddKey` writes, or `MD5` of `body.GetKey().GetData()` if that blob's encoding is stable). That removes `ToJson` and `WriteJson` from the datashard.

The string is the chooser's input, and the chooser hashes it into the topic's existing bounds. It must stay the same for the life of that topic. Replacing it after splits exist moves a given row to a different partition and breaks per-key order. JSON streams keep `MD5(JSON(key))`. A binary stream uses its own derivation. One topic must not accept both derivations.

Replication turns autopartitioning off for global consistency (`!consistency.HasGlobal()`) and does not advertise it for schema-aware streams. The path still matters for a binary CDC stream whose topic has autopartitioning enabled, and for row-consistent replication when that flag is on.

## Changes that do not buy CPU

Packing fixed-width columns into one region, or dropping indexes on a full-row image, saves a branch and a few bytes per column. Against one zstd call that is invisible, and it breaks version-1 framing.

A `schemaVersion` field in the data header is worth adding because value indexes are positions in the name-sorted column list of a particular snapshot. It does not change encode cost: one integer store.

Special-casing `Pg` through `NScheme::GetFixedSize` does not make the encoder faster. `CellFixedSize` returns 0 for `Pg`, so both sides write `u32` length plus the cell bytes. `GetFixedSize` reports the type's stored width, which is a different number for fixed-width `Pg` types. Both sides must keep using `CellFixedSize`.

## Order

1. Heartbeat bypass, size cutoff, a serializer-owned `ZSTD_CCtx`, and a measured level other than 3 for data records. Version-1 readers stay valid.
2. `TColumnMap` cache keyed by `TUserTable const*` and `GetTableSchemaVersion()`.
3. One cell copy and a counted `reserve`.
4. A raw-key hash for autopartitioned binary topics, before such a topic has production splits.

## How to check

Microbenchmark `FillDataChunk` on one shard, same schemas, before and after each step:

- erase, and an upsert of a few integers, both under the cutoff;
- upsert of 1024 identical bytes, which must remain `ZSTD`;
- upsert of 1 KiB and of 64 KiB of incompressible bytes;
- 100 non-key columns, one column updated, the same snapshot for many records;
- a snapshot change between records, so the cache rebuilds;
- autopartitioned routing with a string key of several tens of kilobytes.

Record serializer CPU time per record and the fraction of records whose compressed size is strictly smaller. The first change is done when records under the cutoff do not enter zstd, an incompressible payload pays for one `ZSTD_compress2` and stays `RAW`, and the repetitive 1024-byte payload is still stored as `ZSTD`.
