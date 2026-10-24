# Replication workload

A workload for exercising [asynchronous replication](../../concepts/async-replication.md) of row-oriented tables. The command creates a set of source tables, fills them, and then writes multi-table transactions. Those writes produce a change stream on the source database, which replication consumes.

The user creates the asynchronous replication instance after the initial fill and before the write phase. The initial rows reach the replica through the [initial scan](../../concepts/async-replication.md#initial-scan), and `run` transactions produce the [replication of changes](../../concepts/async-replication.md#replication-of-changes). The command writes only to the source database.

## Run order {#flow}

```bash
{{ ydb-cli }} workload replication init [options...]
{{ ydb-cli }} workload replication import [options...]
# the user creates an asynchronous replication instance here
{{ ydb-cli }} workload replication run [options...]
{{ ydb-cli }} workload replication clean [options...]
```

1. `init` creates the directory and empty row-oriented tables.
2. `import` writes exactly **Q** rows into every table. Q is the `--rows` parameter.
3. The user creates an asynchronous replication instance for this directory with [CREATE ASYNC REPLICATION](../../yql/reference/syntax/create-async-replication.md).
4. `run` executes write transactions for a given time.
5. `clean` drops the workload tables and their directory. Before `clean`, the user drops the replication instance: while replication holds changefeeds of these tables, the drop can fail with a server error.

{% note info %}

The `--path` directory should contain only tables of this workload. Directory replication takes every row-oriented table present in it at the moment the instance is created.

{% endnote %}

## Table schema {#schema}

Every table is row-oriented. Tables are created in the `--path` directory (default `replication`). Secondary indexes, compression, and partitioning settings are described below.

Primary key columns and `seqno` are declared `NOT NULL`. The other columns allow `NULL`. On insert, in both `import` and `run`, one randomly chosen value field other than `seqno` is written as `NULL` with probability 10%. In an `UPDATE`, one of the assigned values other than `seqno` is written as `NULL` with probability 1%. When the `UPDATE` assigns only `seqno`, no empty value is written.

A row-oriented table accepts comparable types that the storage can keep in a column. The workload uses the types from the tables below as keys. `Interval` and `Interval64` are among them: key comparison is defined for both, and a table with `Interval` in its primary key can be created. The types `TzDate`, `TzDatetime`, `TzTimestamp`, `TzDate32`, `TzDatetime64`, and `TzTimestamp64` are not stored in columns of row-oriented tables, so the schema has them neither as keys nor as values. `Float`, `Double`, `Json`, `JsonDocument`, and `Yson` are not used in the primary key; they appear among the value columns. `Uuid` in a primary key requires the server capability of the same name; the default configuration enables it. The storage also accepts `DyNumber` and comparable PostgreSQL types in a key; the schema has no columns of those types.

Tables are divided into ordinary tables and wide tables. An ordinary table holds narrow values. `pk_bool` and `pk_decimal_narrow` have 11 columns; the other ordinary tables have 10. Columns of types `Utf8`, `String`, `Json`, `JsonDocument`, and `Yson` in an ordinary table are filled narrowly. A wide table has 30 columns of different types, plus one text column with a medium fill and one text column with a wide fill.

### Row number {#row-number}

Every row of a table has a number `n`, an integer starting at 0. After `import` the table holds numbers `0 .. Q-1`. `run` appends the following numbers. The primary key is fixed at insert. On every table except `pk_bool` and `pk_decimal_narrow` it is computed from `n`, and different numbers produce different keys. On those two tables the first component is the hash of the `seqno` written by the insert, and `seq` equals `n`, so different numbers produce different keys there as well.

### seqno column {#seqno}

Every table has a column `seqno Uint64 NOT NULL`. It is not part of the primary key. The `seq` column of a composite key stores the row number `n`. `seqno` is the operation number, shared by every table of one transaction. `seqno` is not among the fields that can be written as `NULL`.

Rows that one transaction inserts or updates store this number in `seqno`. `seqno_hash` is computed from the same operation number.

The number comes from an atomic counter in the command process. The counter advances when the transaction is formed. A retry of that transaction keeps the same number.

On `import` the operation number is 0. The load does not advance the counter.

When `run` starts, the counter is set to the current Unix time in microseconds: the integer number of UTC microseconds since 1970-01-01. The first transaction of that run receives this value, and each following new transaction receives the next integer. A later `run` again sets the counter to the Unix time of its own start, also in microseconds.

### Composite key of a narrow type {#composite-key}

When a type has fewer than 2^32 distinct values, one column of that type is not enough: once the domain is exhausted, a new row cannot be inserted. For `Int8`, `Uint8`, `Int16`, `Uint16`, and `Date` the primary key is `(pk <type>, seq Uint64)`. The second component is always `Uint64` and equals the number `n`. The first column walks the whole domain of the type: `pk` is derived from `n % <domain size>`.

The narrow types are `Bool` (2 values), `Int8` and `Uint8` (256), `Int16` and `Uint16` (65536), `Date` (days from 1970-01-01 through 2105-12-31, 49673 values), and `Decimal(1, 0)` (values from −9 to 9).

For `Bool` and `Decimal(1, 0)` the key is preceded by `seqno_hash Uint64 NOT NULL`. The primary key of these two tables is `(seqno_hash, pk <type>, seq Uint64)`. `seqno_hash` is the hash of the `seqno` of the transaction that inserted the row. `pk` is derived from `n % <domain size>`, and `seq` equals `n`.

The hash is computed in unsigned 64-bit arithmetic. Addition and multiplication wrap modulo 2^64, and the right shift is logical:

```
x = seqno + 0x9e3779b97f4a7c15
x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9
x = (x ^ (x >> 27)) * 0x94d049bb133111eb
seqno_hash = x ^ (x >> 31)
```

`import` writes `seqno` equal to 0, so every initially loaded row of `pk_bool` and `pk_decimal_narrow` has the same `seqno_hash`, the hash of zero. Each new `run` transaction receives its own `seqno`, and inserts of that transaction into these two tables receive the hash of that `seqno`. A later write to the `seqno` column keeps `seqno_hash` equal to the hash from the insert. The `WHERE` clause of an update compares this stored hash. At the start of `run` the command reads `SELECT seq, seqno_hash` and remembers the hash of every number already written. A successful insert of the same run adds the hash of its `seqno` for the new numbers to that memory. The `seqno_hash` column is not part of a secondary index.

The other key types are stored in a single column. In ordinary tables it is named `pk`; in wide tables it is named `id`. The value is a reversible mix of the number `n` over the legal range of the type, so consecutive numbers land in different parts of the key order.

### Ordinary tables {#ordinary-tables}

The ordinary tables are the 23 primary-key tables. `pk_bool` and `pk_decimal_narrow` have 11 columns; the other ordinary tables have 10. The columns `seq` and `seqno` are both `Uint64`: `seq` exists only on a composite key and stores the row number, `seqno` exists on every table and stores the operation number. The other columns of one table have different types. When the key type equals the type of one value column, the table has two columns of that type.

A table with a single key column has these columns:

| Column | Type | Fill |
| --- | --- | --- |
| `pk` | the key type of this table | key of number `n` |
| `seqno` | `Uint64` | operation number |
| `c_bool` | `Bool` | |
| `c_int64` | `Int64` | |
| `c_double` | `Double` | |
| `c_decimal` | `Decimal(22, 9)` | |
| `c_utf8` | `Utf8` | narrow |
| `c_string` | `String` | narrow |
| `c_json` | `Json` | narrow |
| `c_timestamp` | `Timestamp` | |

A table with a composite key has these columns:

| Column | Type | Fill |
| --- | --- | --- |
| `pk` | the key type of this table | first key component |
| `seq` | `Uint64` | row number `n` |
| `seqno` | `Uint64` | operation number |
| `c_float` | `Float` | |
| `c_int32` | `Int32` | |
| `c_json_doc` | `JsonDocument` | narrow |
| `c_yson` | `Yson` | narrow |
| `c_uuid` | `Uuid` | |
| `c_date32` | `Date32` | |
| `c_interval64` | `Interval64` | |

On `pk_bool` and `pk_decimal_narrow` the column `seqno_hash Uint64 NOT NULL` precedes `pk`. It is part of the primary key and is filled with the hash of the insert `seqno`.

The table-to-key mapping:

| Table | Primary key | Key of number `n` |
| --- | --- | --- |
| `pk_bool` | `(seqno_hash Uint64, pk Bool, seq Uint64)` | `seqno_hash` is the hash of the insert `seqno`, `pk = (n % 2 = 1)`, `seq = n` |
| `pk_int8` | `(pk Int8, seq Uint64)` | `pk = (n % 256) − 128`, `seq = n` |
| `pk_uint8` | `(pk Uint8, seq Uint64)` | `pk = n % 256`, `seq = n` |
| `pk_int16` | `(pk Int16, seq Uint64)` | `pk` is the signed 16-bit representation of `n % 65536`, `seq = n` |
| `pk_uint16` | `(pk Uint16, seq Uint64)` | `pk = n % 65536`, `seq = n` |
| `pk_date` | `(pk Date, seq Uint64)` | `pk` cycles through every valid `Date`, `seq = n` |
| `pk_decimal_narrow` | `(seqno_hash Uint64, pk Decimal(1, 0), seq Uint64)` | `seqno_hash` is the hash of the insert `seqno`, `pk = (n % 19) − 9`, `seq = n` |
| `pk_int32` | `(pk Int32)` | the 32-bit reversal of `n`, read as signed |
| `pk_int64` | `(pk Int64)` | the 64-bit reversal of `n`, read as signed |
| `pk_uint32` | `(pk Uint32)` | the 32-bit reversal of `n` |
| `pk_uint64` | `(pk Uint64)` | the 64-bit reversal of `n` |
| `pk_decimal` | `(pk Decimal(22, 9))` | the integer part is `n` mixed into `0 .. 10^13−1`, nine fractional digits are zero |
| `pk_decimal_wide` | `(pk Decimal(35, 0))` | the 64-bit reversal of `n` as a non-negative integer |
| `pk_date32` | `(pk Date32)` | `n` mixed over the `Date32` range |
| `pk_datetime` | `(pk Datetime)` | `n` mixed over the `Datetime` range |
| `pk_datetime64` | `(pk Datetime64)` | `n` mixed over the `Datetime64` range |
| `pk_timestamp` | `(pk Timestamp)` | `n` mixed over the `Timestamp` range |
| `pk_timestamp64` | `(pk Timestamp64)` | `n` mixed over the `Timestamp64` range |
| `pk_interval` | `(pk Interval)` | `n` mixed over the `Interval` range |
| `pk_interval64` | `(pk Interval64)` | `n` mixed over the `Interval64` range |
| `pk_uuid` | `(pk Uuid)` | the high 8 bytes are the hash of `n` in big-endian, the low 8 bytes are `n` in big-endian |
| `pk_string` | `(pk String)` | SHA-256 of the 8 little-endian bytes of `n`; the key is the 32-byte digest |
| `pk_utf8` | `(pk Utf8)` | the same SHA-256; the key is 64 characters of its lowercase hexadecimal form |

`Decimal(22, 9)` is the standard parameterization. `Decimal(1, 0)` has one decimal digit and scale zero. `Decimal(35, 0)` has precision 35 and scale zero. `String` is the binary key (`Bytes` is the same type). `Utf8` is the text key (`Text` is the same type).

The binary and text keys are a cryptographic hash of the number, not its textual representation. The order of these keys in the table does not follow the order of the numbers.

Bit reversal reorders the bits of the number: bit `i` of the result equals bit `w − 1 − i` of the original number. For `Int32` and `Uint32`, `w = 32`, and the number has to be less than 2^32. The `Int32` bit string is read as a signed value. For `Int64`, `Uint64`, the wide-table `id`, and `Decimal(35, 0)`, `w = 64`. For `Decimal(35, 0)` the same string is read as a non-negative integer.

Mixing over an inclusive integer range `[lo, hi]` is defined as follows:

```
span = hi − lo + 1
A is the smallest odd positive integer coprime to span
value = lo + ((n * A) mod span)
```

The number has to be less than `span`. The multipliers for these ranges:

| Type | `lo` | `hi` | `A` |
| --- | --- | --- | --- |
| `Date32` | −53375809 | 53375807 | 3 |
| `Datetime` | 0 | `86400 * 49673 − 1` | 7 |
| `Datetime64` | −4611669897600 | 4611669811199 | 11 |
| `Timestamp` | 0 | `86400000000 * 49673 − 1` | 7 |
| `Timestamp64` | −4611669897600000000 | 4611669811199999999 | 11 |
| `Interval` | `−(86400000000 * 49673 − 1)` | `86400000000 * 49673 − 1` | 3 |
| `Interval64` | −9223339708799999999 | 9223339708799999999 | 3 |
| integer part of `Decimal(22, 9)` | 0 | `10^13 − 1` | 3 |

For `Uuid`, the high 8 bytes are the same hash as `seqno_hash`, computed from the number `n` and stored big-endian. The low 8 bytes are the number `n` in big-endian.

### Wide tables {#wide-tables}

There are five wide tables: `wide_utf8`, `wide_string`, `wide_json`, `wide_json_doc`, and `wide_yson`. The primary key of each is `(id Uint64)`. The row number is `n`, and `id` is the 64-bit reversal of `n`, the same value as in `pk_uint64`.

The thirty columns of different types are the same on all five tables. Text columns among them are filled narrowly. Each table adds two columns of its own: `c_medium` with a medium fill and `c_wide` with a wide fill. The table name sets their type.

| Column | Type |
| --- | --- |
| `id` | `Uint64`, primary key |
| `seqno` | `Uint64` |
| `c_bool` | `Bool` |
| `c_int8` | `Int8` |
| `c_int16` | `Int16` |
| `c_int32` | `Int32` |
| `c_int64` | `Int64` |
| `c_uint8` | `Uint8` |
| `c_uint16` | `Uint16` |
| `c_uint32` | `Uint32` |
| `c_float` | `Float` |
| `c_double` | `Double` |
| `c_decimal` | `Decimal(22, 9)` |
| `c_decimal_narrow` | `Decimal(1, 0)` |
| `c_decimal_short` | `Decimal(5, 2)` |
| `c_decimal_wide` | `Decimal(35, 0)` |
| `c_decimal_wide_scale` | `Decimal(35, 9)` |
| `c_decimal_max_scale` | `Decimal(35, 35)` |
| `c_uuid` | `Uuid` |
| `c_date` | `Date` |
| `c_date32` | `Date32` |
| `c_datetime` | `Datetime` |
| `c_datetime64` | `Datetime64` |
| `c_timestamp` | `Timestamp` |
| `c_timestamp64` | `Timestamp64` |
| `c_interval` | `Interval` |
| `c_interval64` | `Interval64` |
| `c_utf8` | `Utf8`, narrow fill |
| `c_string` | `String`, narrow fill |
| `c_json` | `Json`, narrow fill |

| Table | Type of `c_medium` and `c_wide` |
| --- | --- |
| `wide_utf8` | `Utf8` |
| `wide_string` | `String` |
| `wide_json` | `Json` |
| `wide_json_doc` | `JsonDocument` |
| `wide_yson` | `Yson` |

`Decimal(5, 2)` is a narrow variant between `Decimal(1, 0)` and the standard `Decimal(22, 9)`. `Decimal(35, 9)` has precision 35 and scale 9. `Decimal(35, 35)` has precision 35 and scale 35. A `Decimal` value is random, uses every `scale` fractional digit, and stays within `precision`. Values of the other numeric and time columns are random inside the legal range.

`id` and `seqno` are both `Uint64`. The other columns of the thirty have different types.

### Text column fill {#text-size}

The text types here are `Utf8`, `String`, `Json`, `JsonDocument`, and `Yson`.

| Class | Size |
| --- | --- |
| narrow | 0 to 100 bytes inclusive |
| medium | 100 to 500 bytes inclusive, length is random |
| wide | 1000 to 5000 bytes inclusive, length is random |

Size is measured as follows:

* `Utf8` is the number of text bytes. The generator takes a slice of TPC-H pseudo-text: words and punctuation from the `dists.dss` dictionary. The slice is printable ASCII, so the byte count equals the character count.
* `String` is the number of bytes. The contents are random and may be any byte sequence.
* `Json`, `JsonDocument`, and `Yson` are the number of bytes of the text the client passes as a parameter. The document is valid. For the narrow class the lower bound is the shortest valid value: 2 bytes (`{}`) for `Json` and `JsonDocument`, 1 byte (`#`) for `Yson`. From `{"v":"…"}` for `Json` and `{v="…"}` for `Yson`, the field value is the same TPC-H pseudo-text slice. `JsonDocument` in `c_medium` and `c_wide` receives text of the same size as `Json` of that class. The server stores `JsonDocument` in binary form, and the on-disk size may differ from the text size.

The narrow class for `String` and `Utf8` includes the empty value.

The directory holds 28 tables: 23 ordinary tables and 5 wide tables.

## Indexes, compression, and partitioning {#indexes}

Every created table enables lz4 compression for the `default` column family. All columns of the table belong to this family:

```yql
FAMILY default (COMPRESSION='lz4')
```

A secondary index copies the table's `default` family, so its data is compressed with lz4 as well.

Every table gets a synchronous secondary index `seqno_idx` on `seqno`:

```yql
INDEX seqno_idx GLOBAL SYNC ON (seqno)
```

The column `seq` gets the same kind of index, `seq_idx`. The column `seq` exists only on the composite-key tables: `pk_bool`, `pk_int8`, `pk_uint8`, `pk_int16`, `pk_uint16`, `pk_date`, and `pk_decimal_narrow`. The other tables have no index on `seq`.

Automatic partitioning by size and by load is enabled for every table and for every one of these indexes. The minimum and maximum partition counts are `init` parameters and default to 1000 and 1100:

```yql
AUTO_PARTITIONING_BY_SIZE = ENABLED,
AUTO_PARTITIONING_BY_LOAD = ENABLED,
AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 1000,
AUTO_PARTITIONING_MAX_PARTITIONS_COUNT = 1100
```

On a table these parameters are set in `WITH` at creation. The table is created with one partition. The command does not pre-split it. Automatic partitioning then brings the partition count up to the configured minimum and keeps it at or below the configured maximum.

Index partitioning parameters are not set when the index is created. `init` creates the index and then runs `ALTER TABLE … ALTER INDEX … SET` with the same four parameters. The indexes `seqno_idx` and `seq_idx` are built on the columns `seqno` and `seq` and are created without a pre-split.

If the minimum is greater than the maximum, `init` fails with a parameter error and does not create the tables.

## Initial fill {#import}

`import` inserts rows numbered `0 .. Q−1` into every table. The row key is computed from the table above. Every row has `seqno` equal to 0. With probability 10%, one other value field stays `NULL`. On `pk_bool` and `pk_decimal_narrow` the `seqno_hash` column equals the hash of zero.

Rows are written in batches. One batch holds at most `--batch-size` rows, 100 by default. The last batch of a table is shorter when Q is not divisible by that size. A batch is one query:

```sql
DECLARE $input AS List<Struct<...>>;
UPSERT INTO `<table>`
SELECT * FROM AS_TABLE($input);
```

Struct fields have the same names as the table columns and follow the same order. The primary key and `seqno` are required in the struct; the other value fields are optional (`type?`). A batch is idempotent: a retry sends the same rows with the same values, including the same empty field.

Batches of one table are sent one after another. Different tables are processed at the same time. `--threads` sets the thread count; by default it is the number of vCPUs on the current host. The number of tables in progress at once is the smaller of the thread count and the number of tables in the directory.

The command finishes after every batch is committed. The batch size belongs to `import` only and does not change the size of `run` transactions.

A repeated `import` when rows are already present fails the command and does not rewrite the table. The working order is one `import` after `init`.

With Q = 0 the tables stay empty. The first `run` transactions then consist of inserts, until the table has at least one committed row.

## Number boundary {#boundary}

An update addresses a row that already exists. For that, every table has a boundary: the maximum number for which that number and every smaller one are committed. The existing numbers are `0, 1, …, boundary`. The next insert receives `boundary + 1`.

### Reading the boundary at the start of `run` {#boundary-read}

Before the first transaction the command reads the boundary of every table from the database once. The read is an online read-only query and does not take a serializable lock. Its deadline is at least 10 minutes and at least `--timeout`. Queries to different tables run in parallel.

For a composite key the number is stored in `seq`. These tables are `pk_bool`, `pk_int8`, `pk_uint8`, `pk_int16`, `pk_uint16`, `pk_date`, and `pk_decimal_narrow`. The boundary is read as follows:

```sql
SELECT seq FROM `<table>` ORDER BY seq DESC LIMIT 1
```

The returned `seq` is the maximum number. An empty result means an empty table and no boundary.

For `pk_bool` and `pk_decimal_narrow` the command also reads the insert hash:

```sql
SELECT seq, seqno_hash FROM `<table>`
```

An update of a row uses the `seqno_hash` from this read. A row written by `import` has the hash of zero.

For the other tables the key is a mix or a hash of the number. The number of rows already written equals the count of numbers `0 .. boundary`, so the boundary is read as follows:

```sql
SELECT COUNT(*) FROM `<table>`
```

If the query returns `C`, the maximum number is `C − 1` and the next free number is `C`. Zero means an empty table. This query is used for every single-column ordinary table, including `pk_string` and `pk_utf8`, and for every wide table.

This read is valid while the table contains only rows of this workload and their numbers form a contiguous prefix `0 .. boundary`. How that prefix is preserved is described below.

### Maintaining the boundary during `run` {#boundary-maintain}

The boundary is kept in the process memory and moves forward when a transaction with new numbers commits successfully.

At most one transaction at a time reserves new numbers of a given table. The reservation is client memory: a queue that hands out the next numbers, not a lock on a row or a partition in the server. A transaction that needs inserts into several tables takes the reservations in table-name order and holds them until the transaction finishes. The name order keeps two users from waiting on each other in a circle. A retry uses the numbers it already reserved. A number joins the boundary only after a successful commit. Until then, those numbers are not chosen for update.

Committed numbers of a table therefore always run contiguously from 0 through the current boundary. The next `run` reads the same boundary with the same query: `ORDER BY seq DESC LIMIT 1` for a composite key and `COUNT(*)` for the other tables. If the process stops before the commit, the server rolls the unfinished transaction back, and the number prefix stays contiguous.

An update picks a number uniformly from `0 .. boundary` inclusive, using the boundary as it was at the start of the transaction. Numbers chosen for update inside one transaction are distinct when there are no more of them than `boundary + 1`. When more updates are planned than rows already exist, the extra updates become inserts and receive the next numbers.

## Transactions {#transactions}

Every transaction is one atomic write in serializable mode. It inserts new rows and updates existing ones. There are no deletes and no column changes.

The number of affected tables **M** is chosen uniformly from 1 to the number of tables in the directory. A random subset of M distinct tables is then chosen uniformly.

The transaction writes **R** rows into every selected table. R is chosen uniformly from 1 to **K** inclusive. K is the `--max-records` parameter. Of these R rows, `floor(R / 2)` are planned as inserts and the rest as updates. R = 1 yields one update. R = 2 yields one insert and one update. When fewer rows exist than updates were planned, the missing updates are performed as inserts. In an empty table all R rows are inserts.

An insert fills every column and is performed with `UPSERT`. With probability 10%, one random value field of that row, other than `seqno`, is `NULL`. Each updated row is changed with `UPDATE`, which assigns new values to 1, 2, or 3 columns of that row. The number of columns is chosen uniformly. The set always includes `seqno` with the operation number of this transaction; the remaining places are random value columns that are not part of the primary key. With probability 1%, one of these assigned values, other than `seqno`, is `NULL`. When the set contains only `seqno`, no empty value is written. The `WHERE` clause contains every primary-key column and compares it for equality. For a single-column key this is `pk`, and in a wide table it is `id`. For `pk_int8`, `pk_uint8`, `pk_int16`, `pk_uint16`, and `pk_date` this is `pk` and `seq`. For `pk_bool` and `pk_decimal_narrow` this is `seqno_hash`, `pk`, and `seq`. The `seqno_hash` value is the hash written when the row was inserted.

Each statement is sent as its own query inside one interactive serializable transaction. The first query opens the transaction, the following queries continue it, and the last query commits it by setting the commit flag. The whole attempt runs in the SDK retry handler with the idempotent flag set. The status of the query is returned to that handler. When the status is retriable, the handler runs the whole attempt again.

A query text is the parameter declarations plus one statement. A parameter name is the column name. Every insert into one table therefore has the same text, listing every column. An update text is determined by the table and by the set of columns in `SET`. Columns of that set are listed by name, so one set produces one text. An empty value leaves the text unchanged: every value column other than `seqno` is declared with an optional type. A repeated text lets the server take an already built plan from the cache.

The transaction is idempotent and runs in the client retry handler with the idempotent flag set. Primary keys, the columns chosen for update, their new values, and `seqno` are fixed before the first attempt. Every attempt opens the transaction again and sends the same statements as separate queries, with the same parameters. A repeated `UPSERT` after a commit whose response the client did not see writes the same rows and does not fail with a primary key conflict. A repeated `UPDATE` writes the same columns for the same full primary key. The handler retries statuses that are safe to retry for an idempotent operation, including when the outcome of an attempt is unknown. A retry allocates neither new row numbers, nor a new `seqno`, nor a different set of columns.

A transaction counts as successful after this handler. Retriable failures are not included in the error rate when the final attempt succeeds. The error rate includes a transaction whose last attempt failed. One such transaction is one error, however many tables it contains. Its row numbers do not join the boundary, and its `seqno` is not reused. The number reservation is released, and the next insert into the same table again takes numbers starting at `boundary + 1`.

## Concurrency {#concurrency}

`--users` sets the number of parallel users. The default is 1. Each user runs one transaction, waits for `--pause`, and starts the next one. The pause is the same after success and after an error, and it does not stop the other users.

The upper bound on simultaneous transactions equals the number of users. The client session pool is sized for that number. Transactions that insert rows into the same table pass through its number reservation one after another. Transactions with disjoint sets of tables run at the same time. Updates of already committed numbers do not take the reservation and do not wait in the insert queue.

`--threads` sets the number of client pool threads that run asynchronous completions. The default is 0: the pool is adaptive and the client chooses its size. A positive value fixes the pool size. Driver network threads stay at the client default.

`--seconds` limits the run. When the time is over, new transactions are not started. Transactions already started wait until their requests finish, and each request is bounded by `--timeout`. A user who is waiting out `--pause` does not start another transaction. When a started transaction finishes, by commit or by error, its number reservation is released. The command exits once no such transaction is still outstanding.

`--seed` fixes the random generator: table selection, the row count R, selection of numbers for update, and the lengths and contents of values. With several users the seed fixes each user's decisions, but not the order in which their transactions interleave. If the parameter is omitted, the seed is taken from the current time. Numbers of new rows do not depend on the seed: the table boundary issues them.

## Output {#output}

Once per `--window` seconds (once per second by default) one line is printed to standard output:

```text
elapsed_s=<seconds> tx_per_s=<rate> errors_per_s=<rate> tx=<count> errors=<count>
```

`tx` and `errors` are counters for that window. Rates are those counters divided by the window length, with one digit after the decimal point.

After shutdown, a total for the whole run is printed in the same form: `tx` and `errors` are cumulative, and rates are computed over the full duration.

With `--verbose` (the global `-v` parameter) every error is also printed to standard error:

```text
error status=<status code> issues=<message text>
```

The query text is not included. Without `--verbose`, only rates and counters are printed.

`run` exits with a non-zero code when it cannot start or when the command itself fails: missing tables, invalid parameters, or a failure while reading the boundary. Errors of individual transactions do not change the exit code. They show up in `errors` and, with `--verbose`, in the error texts.

## Parameters {#options}

Shared by `init`, `import`, `run`, and `clean`:

| Parameter | Description | Default |
| --- | --- | --- |
| `--path` | Directory of workload tables in the source database | `replication` |

`init`:

| Parameter | Description | Default |
| --- | --- | --- |
| `--min-partitions` | Minimum partition count of a table and of each secondary index, `AUTO_PARTITIONING_MIN_PARTITIONS_COUNT` | `1000` |
| `--max-partitions` | Maximum partition count of a table and of each secondary index, `AUTO_PARTITIONING_MAX_PARTITIONS_COUNT` | `1100` |

`import`:

| Parameter | Description | Default |
| --- | --- | --- |
| `--rows` | Q, the number of initial rows in every table | `1000` |
| `--batch-size` | Rows in one `UPSERT … SELECT * FROM AS_TABLE` request | `100` |
| `--threads` | Threads that send batches. The default is the number of vCPUs on the current host | host vCPUs |

`run`:

| Parameter | Description | Default |
| --- | --- | --- |
| `--seconds`, `-s` | Write duration, seconds | `60` |
| `--users` | Number of parallel users | `1` |
| `--max-records` | K, the upper bound on rows written into one table inside a transaction. The actual count is random from 1 to K | `10` |
| `--pause` | Per-user pause between transactions. A plain number with no suffix is read as milliseconds | `100ms` |
| `--threads` | Client pool threads. 0 means an adaptive pool | `0` |
| `--window` | Rate output window, seconds | `1` |
| `--seed` | Generator seed. Without the parameter the seed is taken from the current time | — |
| `--timeout` | Timeout of each request. The last statement of a transaction commits it in the same request. A plain number with no suffix is milliseconds | `10s` |

A request to the query service has one client timeout, and `run` sets it with `--timeout`. The limit applies to a single request, including the last statement, which commits the transaction in that same request, and including a statement that writes a row of several kilobytes. A transaction of several statements may take several such limits. The initial boundary read uses a separate, longer deadline: see [reading the boundary](#boundary-read).

## Example {#example}

```bash
{{ ydb-cli }} workload replication init --path replication
{{ ydb-cli }} workload replication import --path replication --rows 1000
```

Next, on the destination database, the user creates the replication instance. The full statement is in the [section below](#create-replication). The workload command does not execute it.

After the instance is created, the write phase starts, with error text when needed:

```bash
{{ ydb-cli }} -v workload replication run \
    --path replication \
    --users 4 \
    --max-records 10 \
    --pause 100ms \
    --seconds 300
```

Dropping the tables:

```bash
{{ ydb-cli }} workload replication clean --path replication
```

## Creating an asynchronous replication instance {#create-replication}

The statements below run in the destination database after `import` and before `run`. The workload command does not execute them.

First, a [secret](../../yql/reference/syntax/create-secret.md) is created in the destination database with the source database user's password:

```yql
CREATE SECRET source_password WITH (value = "<password>");
```

Then a replication instance is created for all 28 tables in the `replication` directory. The path on the destination is the same as the path on the source. Authentication is the `USER` login and the `PASSWORD_SECRET_PATH` secret. The connection uses `grpcs://`, and the source root certificate is passed in `CA_CERT` as a raw `@@…@@` string. `CONSISTENCY_LEVEL = 'ROW'` selects [row-level consistency](../../concepts/async-replication.md#consistency-level-row); that is the default.

```yql
CREATE ASYNC REPLICATION source_repl
FOR
    `replication/pk_bool` AS `replication/pk_bool`,
    `replication/pk_int8` AS `replication/pk_int8`,
    `replication/pk_uint8` AS `replication/pk_uint8`,
    `replication/pk_int16` AS `replication/pk_int16`,
    `replication/pk_uint16` AS `replication/pk_uint16`,
    `replication/pk_date` AS `replication/pk_date`,
    `replication/pk_decimal_narrow` AS `replication/pk_decimal_narrow`,
    `replication/pk_int32` AS `replication/pk_int32`,
    `replication/pk_int64` AS `replication/pk_int64`,
    `replication/pk_uint32` AS `replication/pk_uint32`,
    `replication/pk_uint64` AS `replication/pk_uint64`,
    `replication/pk_decimal` AS `replication/pk_decimal`,
    `replication/pk_decimal_wide` AS `replication/pk_decimal_wide`,
    `replication/pk_date32` AS `replication/pk_date32`,
    `replication/pk_datetime` AS `replication/pk_datetime`,
    `replication/pk_datetime64` AS `replication/pk_datetime64`,
    `replication/pk_timestamp` AS `replication/pk_timestamp`,
    `replication/pk_timestamp64` AS `replication/pk_timestamp64`,
    `replication/pk_interval` AS `replication/pk_interval`,
    `replication/pk_interval64` AS `replication/pk_interval64`,
    `replication/pk_uuid` AS `replication/pk_uuid`,
    `replication/pk_string` AS `replication/pk_string`,
    `replication/pk_utf8` AS `replication/pk_utf8`,
    `replication/wide_utf8` AS `replication/wide_utf8`,
    `replication/wide_string` AS `replication/wide_string`,
    `replication/wide_json` AS `replication/wide_json`,
    `replication/wide_json_doc` AS `replication/wide_json_doc`,
    `replication/wide_yson` AS `replication/wide_yson`
WITH (
    USER = 'test1',
    PASSWORD_SECRET_PATH = 'source_password',
    CONSISTENCY_LEVEL = 'ROW',
    CONNECTION_STRING = 'grpcs://<source>:2135/?database=<source-database>',
    CA_CERT = @@
-----BEGIN CERTIFICATE-----
<certificate body>
-----END CERTIFICATE-----
@@
);
```

The other connection and authentication parameters are described in [CREATE ASYNC REPLICATION](../../yql/reference/syntax/create-async-replication.md).

## Left to the user {#scope}

The user creates, describes, finishes, and drops the asynchronous replication instance. The command does not wait for the initial scan to finish and does not read lag. Checking that the replica matches the source is done outside the command.
