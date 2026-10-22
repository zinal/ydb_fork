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

The `--path` directory contains only tables of this workload. Directory replication captures every row-oriented table present in it at the moment the instance is created.

{% endnote %}

## Table schema {#schema}

Every table is row-oriented, without secondary indexes and without explicit partitioning settings. Tables are created in the `--path` directory (default `replication`).

Primary key columns are declared `NOT NULL`. The other columns allow `NULL`; every write puts a concrete value into them.

A row-oriented table accepts comparable types that the storage can keep in a column. The workload uses the types from the tables below as keys. `Interval` and `Interval64` are among them: key comparison is defined for both, and a table with `Interval` in its primary key can be created. The types `TzDate`, `TzDatetime`, `TzTimestamp`, `TzDate32`, `TzDatetime64`, and `TzTimestamp64` are not stored in columns of row-oriented tables, so the schema has them neither as keys nor as values. `Float`, `Double`, `Json`, `JsonDocument`, and `Yson` are not used in the primary key; they appear among the value columns. `Uuid` in a primary key requires the server capability of the same name; the default configuration enables it. The storage also accepts `DyNumber` and comparable PostgreSQL types in a key; the schema has no columns of those types.

Wide text, bytes, and JSON live in three separate tables. The other tables hold scalar values.

### Row number {#row-number}

Every row of a table has a number `n`, an integer starting at 0. The primary key is a pure function of `n`, and different numbers produce different keys. After `import` the table holds numbers `0 .. Q-1`. `run` appends the following numbers.

### Composite key of a narrow type {#composite-key}

When a type has fewer than 2^32 distinct values, one column of that type is not enough: once the domain is exhausted, a new row cannot be inserted. The primary key of such a table is `(pk <type>, seq Uint64)`. The second component is always `Uint64` and equals the number `n`. The first column walks the whole domain of the type: `pk` is derived from `n % <domain size>`.

The narrow types are `Bool` (2 values), `Int8` and `Uint8` (256), `Int16` and `Uint16` (65536), `Date` (days from 1970-01-01 through 2105-12-31), and `Decimal(1, 0)` (values from −9 to 9).

The other key types hold at least 2^32 distinct values. Their primary key is a single column `pk`, and it grows monotonically with `n`.

### Primary key tables {#pk-tables}

Each table in this section has one non-key column, `marker Uint64`. Insert and update write a new value there.

| Table | Primary key | Key of number `n` |
| --- | --- | --- |
| `pk_bool` | `(pk Bool, seq Uint64)` | `pk = (n % 2 = 1)`, `seq = n` |
| `pk_int8` | `(pk Int8, seq Uint64)` | `pk = (n % 256) − 128`, `seq = n` |
| `pk_uint8` | `(pk Uint8, seq Uint64)` | `pk = n % 256`, `seq = n` |
| `pk_int16` | `(pk Int16, seq Uint64)` | `pk` is the signed 16-bit representation of `n % 65536`, `seq = n` |
| `pk_uint16` | `(pk Uint16, seq Uint64)` | `pk = n % 65536`, `seq = n` |
| `pk_date` | `(pk Date, seq Uint64)` | `pk` cycles through every valid `Date`, `seq = n` |
| `pk_decimal_narrow` | `(pk Decimal(1, 0), seq Uint64)` | `pk = (n % 19) − 9`, `seq = n` |
| `pk_int32` | `(pk Int32)` | `pk = n` |
| `pk_int64` | `(pk Int64)` | `pk = n` |
| `pk_uint32` | `(pk Uint32)` | `pk = n` |
| `pk_uint64` | `(pk Uint64)` | `pk = n` |
| `pk_decimal` | `(pk Decimal(22, 9))` | the integer part is `n`, nine fractional digits are zero |
| `pk_decimal_wide` | `(pk Decimal(35, 0))` | `pk = n` |
| `pk_date32` | `(pk Date32)` | a shift of `n` days from the start of the range |
| `pk_datetime` | `(pk Datetime)` | `n` seconds from the start of the range |
| `pk_datetime64` | `(pk Datetime64)` | `n` seconds from the start of the range |
| `pk_timestamp` | `(pk Timestamp)` | `n` microseconds from the start of the range |
| `pk_timestamp64` | `(pk Timestamp64)` | `n` microseconds from the start of the range |
| `pk_interval` | `(pk Interval)` | an interval of `n` microseconds |
| `pk_interval64` | `(pk Interval64)` | an interval of `n` microseconds |
| `pk_uuid` | `(pk Uuid)` | 16 bytes: the high 8 bytes are zero, the low 8 bytes are `n` in big-endian |
| `pk_string` | `(pk String)` | SHA-256 of the 8 little-endian bytes of `n`; the key is the 32-byte digest |
| `pk_utf8` | `(pk Utf8)` | the same SHA-256; the key is 64 characters of its lowercase hexadecimal form |

`Decimal(22, 9)` is the standard parameterization. `Decimal(1, 0)` has one decimal digit and scale zero. `Decimal(35, 0)` has precision 35 and scale zero. `String` is the binary key (`Bytes` is the same type). `Utf8` is the text key (`Text` is the same type).

The binary and text keys are a cryptographic hash of the number, not its textual representation. The order of these keys in the table does not follow the order of the numbers.

For a single-column key, `n` has to fit into the type. That is up to 2^31−1 for `Int32`, and up to the type's upper bound for `Datetime`, `Timestamp`, and `Interval`.

### Value type table {#types-table}

Table `types`, primary key `(id Uint64)`, the row number equals `id`. A row carries the value types, including the `Decimal` variants:

| Column | Type |
| --- | --- |
| `id` | `Uint64`, primary key |
| `v_bool` | `Bool` |
| `v_int8` | `Int8` |
| `v_int16` | `Int16` |
| `v_int32` | `Int32` |
| `v_int64` | `Int64` |
| `v_uint8` | `Uint8` |
| `v_uint16` | `Uint16` |
| `v_uint32` | `Uint32` |
| `v_uint64` | `Uint64` |
| `v_float` | `Float` |
| `v_double` | `Double` |
| `v_decimal` | `Decimal(22, 9)` |
| `v_decimal_narrow` | `Decimal(1, 0)` |
| `v_decimal_short` | `Decimal(5, 2)` |
| `v_decimal_wide` | `Decimal(35, 0)` |
| `v_decimal_wide_scale` | `Decimal(35, 9)` |
| `v_decimal_max_scale` | `Decimal(35, 35)` |
| `v_uuid` | `Uuid` |
| `v_date` | `Date` |
| `v_date32` | `Date32` |
| `v_datetime` | `Datetime` |
| `v_datetime64` | `Datetime64` |
| `v_timestamp` | `Timestamp` |
| `v_timestamp64` | `Timestamp64` |
| `v_interval` | `Interval` |
| `v_interval64` | `Interval64` |
| `v_yson` | `Yson` |

`Decimal(5, 2)` is a narrow variant between `Decimal(1, 0)` and the standard `Decimal(22, 9)`. `Decimal(35, 9)` has precision 35 and scale 9. `Decimal(35, 35)` has precision 35 and scale 35.

A `Decimal` value is random, uses every `scale` fractional digit, and stays within `precision`. Values of the other numeric and time columns are random inside the legal range. `v_yson` is valid YSON; its text size is chosen like the medium class below, from 100 to 500 bytes inclusive.

### Value size tables {#payload-tables}

Three tables carry text (`Utf8`), bytes (`String`), `Json`, and `JsonDocument` columns. The primary key is `(id Uint64)`, and the row number equals `id`.

| Table | Class | Value size |
| --- | --- | --- |
| `payload_narrow` | narrow | 0 to 100 bytes inclusive |
| `payload_medium` | medium | 100 to 500 bytes inclusive, length is random |
| `payload_wide` | wide | 1000 to 5000 bytes inclusive, length is random |

Columns in all three tables: `utf8 Utf8`, `bin String`, `json Json`, `json_doc JsonDocument`.

Size is measured as follows:

* `Utf8` is the number of text bytes. The generator writes printable ASCII, so the byte count equals the character count.
* `String` is the number of bytes. The contents are random and may be any byte sequence.
* `Json` and `JsonDocument` are the number of bytes of the JSON text the client passes as a parameter. The document is valid. For the narrow class the lower bound is 2 bytes (`{}`), because a shorter JSON document does not exist. `JsonDocument` receives the same text. The server stores it in binary form, and the on-disk size may differ from the text size.

The narrow class for `String` and `Utf8` includes the empty value.

The directory holds 27 tables: 23 primary-key tables, `types`, and three size tables.

## Initial fill {#import}

`import` inserts rows numbered `0 .. Q−1` into every table. The row key is computed from the table above.

The command finishes after all of these rows are committed. Rows of one table may be written by several transactions; the batch size does not affect the `run` scenario.

A repeated `import` into a non-empty table conflicts on the primary key and fails the command. The working order is one `import` after `init`.

With Q = 0 the tables stay empty. The first `run` transactions then consist of inserts, until the table has at least one committed row.

## Number boundary {#boundary}

An update addresses a row that already exists. For that, every table has a boundary: the maximum number for which that number and every smaller one are committed. The existing numbers are `0, 1, …, boundary`. The next insert receives `boundary + 1`.

### Reading the boundary at the start of `run` {#boundary-read}

Before the first transaction the command reads the boundary of every table from the database once.

For a single-column key that grows with the number, the command reads the maximum value of the key column. In `pk_*` tables that column is named `pk`; in `types` and `payload_*` it is named `id`:

```sql
SELECT <key column> FROM `<table>` ORDER BY <key column> DESC LIMIT 1
```

This covers `pk_int32`, `pk_int64`, `pk_uint32`, `pk_uint64`, `pk_decimal`, `pk_decimal_wide`, `pk_date32`, `pk_datetime`, `pk_datetime64`, `pk_timestamp`, `pk_timestamp64`, `pk_interval`, `pk_interval64`, `pk_uuid`, `types`, `payload_narrow`, `payload_medium`, and `payload_wide`.

The number is recovered from the key by the inverse of the rule in [primary key tables](#pk-tables): for integers, dates, intervals, and `Decimal` it is the numeric component of the key itself, for `Uuid` it is the low 8 bytes as big-endian, and for `types` and `payload_*` the number equals `id`. An empty result means an empty table and no boundary.

For a composite key the monotonic component is `seq`, while `pk` repeats as it cycles through the domain. The maximum of the pair `(pk, seq)` in lexicographic order is not the maximum number, so the boundary is read from `seq`:

```sql
SELECT seq FROM `<table>` ORDER BY seq DESC LIMIT 1
```

The returned `seq` is the maximum number.

For `pk_string` and `pk_utf8` the key is a hash. `ORDER BY pk` shows the maximum hash, not the maximum number. The number of rows already written equals the count of numbers `0 .. boundary`, so the boundary is read as follows:

```sql
SELECT COUNT(*) FROM `<table>`
```

If the query returns `C`, the maximum number is `C − 1` and the next free number is `C`. Zero means an empty table.

This read is valid while the table contains only rows of this workload and their numbers form a contiguous prefix `0 .. boundary`. How that prefix is preserved is described below.

### Maintaining the boundary during `run` {#boundary-maintain}

The boundary is kept in the process memory and moves forward when a transaction with new numbers commits successfully.

At most one transaction at a time reserves and commits new numbers of a given table. A transaction that needs inserts into several tables locks those tables in name order and holds the locks until the commit finishes, so users do not deadlock. A retried transaction uses the numbers it already reserved. A number joins the boundary only after a successful commit. Until the transaction commits, updates do not select these numbers.

Committed numbers of a table therefore always form a contiguous run from 0 through the current boundary. The next `run` reads the same boundary with the same `ORDER BY … DESC LIMIT 1` or `COUNT(*)` query. If the process stops before the commit, the server rolls the unfinished transaction back, and the prefix has no hole.

An update picks a number uniformly from `0 .. boundary` inclusive, using the boundary as it was at the start of the transaction. Numbers chosen for update inside one transaction are distinct when there are no more of them than `boundary + 1`. When more updates are planned than rows already exist, the extra updates become inserts and receive the next numbers.

## Transactions {#transactions}

Every transaction is one atomic write in serializable mode. It inserts new rows and updates existing ones. There are no deletes and no column changes.

The number of affected tables **M** is chosen uniformly from 1 to the number of tables in the directory. A random subset of M distinct tables is then chosen uniformly.

The transaction writes **R** rows into every selected table. R is chosen uniformly from 1 to **K** inclusive. K is the `--max-records` parameter. Of these R rows, `floor(R / 2)` are planned as inserts and the rest as updates. R = 1 yields one update. R = 2 yields one insert and one update. When fewer rows exist than updates were planned, the missing updates are performed as inserts. In an empty table all R rows are inserts.

An insert fills every column. An update rewrites every non-key column and leaves the primary key unchanged.

A transaction counts as successful after the client retries. Retriable failures that the client retries on its own are not included in the error rate when the final attempt succeeds. A retry uses the same reserved numbers. The error rate includes a transaction whose last attempt failed. One such transaction is one error, regardless of how many tables it contains. Its numbers do not join the boundary. After the failure the table locks are released, and the next insert into the same table again takes numbers starting at `boundary + 1`.

## Concurrency {#concurrency}

`--users` sets the number of parallel users. The default is 1. Each user loops through one transaction, waits for `--pause`, and starts the next one. The pause is the same after success and after an error. The pause wait is asynchronous.

The upper bound on simultaneous transactions equals the number of users. The client session pool is sized for that number. Transactions that insert rows into the same table pass through it one after another. Transactions with disjoint sets of tables run at the same time. Updates of already committed numbers do not block each other.

`--threads` sets the number of client pool threads that run asynchronous completions. The default is 0: the pool is adaptive and the client chooses its size. A positive value fixes the pool size. Driver network threads stay at the client default.

`--seconds` limits the run. When the time is over, new transactions are not started, and transactions already sent wait for their timeout. The pause is interrupted together with the shutdown. Table locks are released before exit, including when a transaction never committed.

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

A non-zero `run` exit code is reserved for startup failures: missing tables, invalid parameters, or a failure of the run itself. Errors of individual transactions do not change the exit code; they show up in `errors` and, with `--verbose`, in the error texts.

## Parameters {#options}

Shared by `init`, `import`, `run`, and `clean`:

| Parameter | Description | Default |
| --- | --- | --- |
| `--path` | Directory of workload tables in the source database | `replication` |

`import`:

| Parameter | Description | Default |
| --- | --- | --- |
| `--rows` | Q, the number of initial rows in every table | `1000` |

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
| `--client-timeout` | Transport timeout. A plain number with no suffix is milliseconds | `10s` |
| `--operation-timeout` | Operation timeout | `5s` |
| `--cancel-after` | Operation cancel timeout | `5s` |

The timeouts are sized for a transaction that writes several tables, including rows of several kilobytes.

## Example {#example}

```bash
{{ ydb-cli }} workload replication init --path replication
{{ ydb-cli }} workload replication import --path replication --rows 1000
```

Next, on the destination database, the user creates replication of the directory. Connection and authentication parameters are described in [CREATE ASYNC REPLICATION](../../yql/reference/syntax/create-async-replication.md). The workload command does not execute this statement:

```yql
CREATE ASYNC REPLICATION replication_workload
FOR `replication` AS `replication_replica`
WITH (
    CONNECTION_STRING = 'grpcs://<source>:2135/?database=<source-database>',
    TOKEN_SECRET_PATH = '<secret>'
);
```

After the instance is created, the write phase starts, with error text when needed:

```bash
{{ ydb-cli }} -v workload replication run \
    --path replication \
    --users 4 \
    --max-records 10 \
    --pause 100ms \
    --seconds 300
```

Removing the workload:

```bash
{{ ydb-cli }} workload replication clean --path replication
```

## Left to the user {#scope}

The user creates, describes, finishes, and drops the asynchronous replication instance. The command does not wait for the initial scan to finish and does not read lag. Checking that the replica matches the source is done outside the command.
