# Replication workload

A workload for exercising [asynchronous replication](../../concepts/async-replication.md) of row-oriented tables. The command creates a set of source tables, fills them, and then writes multi-table transactions. Those writes produce a change stream on the source database, which replication consumes.

The command does not create, alter, or drop an asynchronous replication instance. The user creates it after the initial fill and before the write phase. The initial rows then reach the replica through the [initial scan](../../concepts/async-replication.md#initial-scan), and `run` transactions exercise [replication of changes](../../concepts/async-replication.md#replication-of-changes).

The command writes only to the source database. Reading replicas, lag, and comparing contents stay on the user side.

## Scenario {#scenario}

The workload uses a fixed set of **30 row-oriented tables** in one directory. Thirty tables satisfy the requirement of at least ten distinct tables and, at the same time, give a separate table for every type that can be a primary key, separate `Decimal` variants, and three size classes of text and JSON columns.

One launch has four steps:

1. `init` creates the directory and 30 empty row-oriented tables.
2. `import` writes exactly **Q** rows into every table. Q is the `--rows` parameter.
3. The user creates an asynchronous replication instance for this directory (or for the listed tables) with [CREATE ASYNC REPLICATION](../../yql/reference/syntax/create-async-replication.md).
4. `run` executes write transactions for a given time.

A transaction is shaped as follows:

* the number of affected tables **M** is chosen uniformly from 1 to **N**, where N is the number of tables in the set (30);
* a random subset of M distinct tables is then chosen uniformly;
* the transaction writes **R** rows into every selected table, where R is chosen uniformly from 1 to **K** inclusive; K is the `--max-records` parameter;
* `floor(R / 2)` of these R rows are inserted as new rows, and the other `R - floor(R / 2)` update rows that already exist from the initial fill;
* R = 1 yields one update of an existing row; R = 2 yields one insert and one update.

Several users run these transactions in parallel. Each user has at most one transaction in flight. After a transaction finishes, the user waits for a pause and starts the next one. The default pause is 100 milliseconds, parameter `--pause`.

Writes go through the asynchronous client interface. Waiting for the server and waiting out the pause do not block a thread. The client pool size is chosen automatically and can be set with `--threads`.

While `run` is in progress, the standard output shows the rate of successful transactions and the error rate. In verbose mode (`-v`, `--verbose`), the standard error output also shows the text of every error.

## Run order {#flow}

```bash
{{ ydb-cli }} workload replication init [options...]
{{ ydb-cli }} workload replication import [options...]
# the user creates an asynchronous replication instance here
{{ ydb-cli }} workload replication run [options...]
{{ ydb-cli }} workload replication clean [options...]
```

`clean` drops only the workload tables and their directory. Before `clean`, the user drops the replication instance: while replication holds changefeeds of these tables, the drop can fail with a server error.

{% note info %}

The `--path` directory must contain only tables of this workload. Directory replication captures every row-oriented table present in it at the moment the instance is created.

{% endnote %}

## Table schema {#schema}

Every table is an ordinary row-oriented table without secondary indexes and without explicit partitioning settings. Tables are created in the `--path` directory (default `replication`).

Primary key columns are declared `NOT NULL`. The other columns allow `NULL`, but the generator always writes a concrete value into them: this workload does not exercise replication of `NULL`.

Types that cannot be stored in a table column or cannot be used in a primary key are not part of the schema. That is YQL containers, `Float`, `Double`, `Json`, `JsonDocument`, and `Yson` used as a key, the `TzDate`, `TzDatetime`, and `TzTimestamp` types, the `DyNumber` type, and PostgreSQL types.

### Primary key tables {#pk-tables}

Each allowed primary key type has its own table. A domain too small for a long series of new keys is extended with a second key column, `seq Uint64`. The type under test stays the first key column: row order and partition borders follow it.

Such a table has one non-key column, `marker Uint64`. Insert and update write a new value there, so the change stream carries a new row image.

| Table | Primary key | Key derived from row number `i` |
| --- | --- | --- |
| `pk_bool` | `(pk Bool, seq Uint64)` | `pk = (i % 2 = 1)`, `seq = i` |
| `pk_int8` | `(pk Int8, seq Uint64)` | `pk` walks the whole `Int8` range, `seq = i` |
| `pk_int16` | `(pk Int16, seq Uint64)` | `pk` walks the whole `Int16` range, `seq = i` |
| `pk_int32` | `(pk Int32)` | `pk = i` |
| `pk_int64` | `(pk Int64)` | `pk = i` |
| `pk_uint8` | `(pk Uint8, seq Uint64)` | `pk = i % 256`, `seq = i` |
| `pk_uint16` | `(pk Uint16, seq Uint64)` | `pk = i % 65536`, `seq = i` |
| `pk_uint32` | `(pk Uint32)` | `pk = i` |
| `pk_uint64` | `(pk Uint64)` | `pk = i` |
| `pk_decimal` | `(pk Decimal(22, 9))` | standard `Decimal`: the integer part is `i`, nine fractional digits are zero |
| `pk_decimal_narrow` | `(pk Decimal(1, 0), seq Uint64)` | `pk` walks the whole `Decimal(1, 0)` range, `seq = i` |
| `pk_decimal_wide` | `(pk Decimal(35, 0))` | widest integer `Decimal`: `pk = i` |
| `pk_string` | `(pk String)` | byte string of the decimal representation of `i` |
| `pk_utf8` | `(pk Utf8)` | text decimal representation of `i` |
| `pk_uuid` | `(pk Uuid)` | a UUID with valid version and variant bits and `i` in the trailing bytes |
| `pk_date` | `(pk Date, seq Uint64)` | `pk` cycles through the legal `Date` range, `seq = i` |
| `pk_date32` | `(pk Date32)` | a shift of `i` days from the start of the range |
| `pk_datetime` | `(pk Datetime)` | `i` seconds from the start of the range |
| `pk_datetime64` | `(pk Datetime64)` | `i` seconds from the start of the range |
| `pk_timestamp` | `(pk Timestamp)` | `i` microseconds from the start of the range |
| `pk_timestamp64` | `(pk Timestamp64)` | `i` microseconds from the start of the range |
| `pk_interval` | `(pk Interval)` | an interval of `i` microseconds |
| `pk_interval64` | `(pk Interval64)` | an interval of `i` microseconds |
| `pk_tz_date32` | `(pk TzDate32)` | the same date as `Date32`, time zone `Europe/Moscow` |
| `pk_tz_datetime64` | `(pk TzDatetime64)` | the same instant as `Datetime64`, time zone `Europe/Moscow` |
| `pk_tz_timestamp64` | `(pk TzTimestamp64)` | the same instant as `Timestamp64`, time zone `Europe/Moscow` |

`Decimal(22, 9)` is the standard parameterization. `Decimal(1, 0)` is the narrowest: one decimal digit and no fractional digits. `Decimal(35, 0)` is the maximum precision of 35 with scale zero. Row numbers produced by this workload fit into precision 22 and 35. For a single-column key, `i` has to fit into the type itself: up to 2^31−1 for `Int32`, and up to the type's upper bound for `Datetime` and `Timestamp`. In practice Q and the number of inserts in a run stay well inside those bounds.

### Value type table {#types-table}

Table `types`, primary key `(id Uint64)`. A row carries every supported value type, including `Decimal` variants that are not used as a key:

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
| `v_tz_date32` | `TzDate32` |
| `v_tz_datetime64` | `TzDatetime64` |
| `v_tz_timestamp64` | `TzTimestamp64` |
| `v_yson` | `Yson` |

`Decimal(5, 2)` is a narrow variant between the minimum and the standard `(22, 9)`. `Decimal(35, 9)` is the maximum precision with the standard scale. `Decimal(35, 35)` is the maximum precision and the maximum scale together.

A `Decimal` value is random, uses the full allowed scale, and stays within precision. Values of the other numeric and time columns are random inside the legal range. `v_yson` is valid YSON; its text size is chosen like the medium class below (100 to 500 bytes inclusive).

Wide text, bytes, and JSON are not stored in this table: one transaction may touch all 30 tables at once, and a wide image on every row would inflate the request.

### Value size tables {#payload-tables}

Three tables carry text (`Utf8`), bytes (`String`), `Json`, and `JsonDocument` columns. Each table has its own size class. The primary key is `(id Uint64)`.

| Table | Class | Value size |
| --- | --- | --- |
| `payload_narrow` | narrow | 0 to 100 bytes inclusive |
| `payload_medium` | medium | 100 to 500 bytes inclusive, length is random |
| `payload_wide` | wide | 1000 to 5000 bytes inclusive, length is random |

Columns in all three tables: `utf8 Utf8`, `bin String`, `json Json`, `json_doc JsonDocument`.

Size is measured as follows:

* `Utf8` is the number of text bytes. The generator writes printable ASCII, so the byte count equals the character count.
* `String` is the number of bytes. The contents are random and may be any byte sequence.
* `Json` and `JsonDocument` are the number of bytes of the JSON text the client passes as a parameter. The document is always valid. For the narrow class the lower bound is raised to 2 bytes (`{}`), because a shorter JSON document does not exist. `JsonDocument` receives the same text: the server stores it in binary form, and the on-disk size may differ from the text size.

The narrow class for `String` and `Utf8` includes the empty value.

## Initial fill {#import}

`import` inserts rows numbered `i` from 0 to Q−1 inclusive into each of the 30 tables. The row key is fully determined by the number, using the table in the section above.

The command finishes after all of these rows are committed. Rows of one table may be written by several transactions; the batch size is an implementation detail and does not affect the `run` scenario.

A repeated `import` into non-empty tables conflicts on the primary key and fails the command. The working order is one `import` after `init`.

With Q = 0 the tables stay empty. Then `run` has no update operations: all R rows of a transaction become inserts.

## Transactions in run {#transactions}

Every transaction is one atomic write in serializable mode. It only inserts new rows and updates existing ones. There are no deletes and no column changes.

For every selected table:

* new rows take the next numbers after those already issued: the per-table counter shared by all users starts at Q;
* a number issued to a failed transaction is not reused, so gaps in the key sequence are allowed;
* updated rows are chosen uniformly among the initial keys `i` from 0 to Q−1;
* an insert fills every column;
* an update rewrites every non-key column and does not change the primary key.

The initial keys do not depend on races between users: `import` wrote them, and `run` does not delete them. New rows of other transactions are not update targets.

A transaction counts as successful after the client retries. Retriable failures (`OVERLOADED`, a transport drop, and other statuses the client retries on its own) are not included in the error rate when the final attempt succeeds. The error rate includes a transaction whose last attempt failed. One such transaction is one error, regardless of how many tables it contains.

## Concurrency {#concurrency}

`--users` sets the number of parallel users. The default is 1. Each user loops through one transaction, waits for `--pause`, and starts the next one. The pause is the same after success and after an error, so a series of errors does not become a tight loop. The pause wait is asynchronous.

The upper bound on simultaneous transactions equals the number of users. The client session pool is sized for that number.

`--threads` sets the number of client pool threads that run asynchronous completions. The default is 0: the pool is adaptive and the client chooses its size. A positive value fixes the pool size. Driver network threads stay at the client default.

`--seconds` limits the run. When the time is over, new transactions are not started, and transactions already sent wait for their timeout. The pause is interrupted together with the shutdown.

`--seed` fixes the random generator: table selection, the row count R, selection of updated keys, and the lengths and contents of values. With several users the seed fixes each user's decisions, but not the order in which their transactions interleave. If the parameter is omitted, the seed is taken from the current time.

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

The timeouts are higher than for short single-table workloads: a transaction may write all 30 tables, and some of those rows are several kilobytes.

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

## Outside the command {#scope}

The user separately creates, describes, finishes, and drops the asynchronous replication instance. The command does not wait for the initial scan to finish and does not read lag. Checking that the replica matches the source is done outside the command.
