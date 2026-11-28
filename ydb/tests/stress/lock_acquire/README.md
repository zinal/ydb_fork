# Concurrent lock acquisition test

This standalone test checks the read/classify/conditional-`UPSERT` lock
acquisition query under contention. Each process selects one of 100 lock IDs
using a truncated normal distribution, holds an acquired lock for a random
interval, and releases only a row owned by that acquisition attempt.

The test exits with an error if a process loses ownership before release or if
the returned `INSERTED`/`ALREADY_EXISTS` status is inconsistent.

## Start local YDB

The command follows the
[local Docker quick start](../../../docs/ru/core/quickstart.md):

```bash
sudo docker volume create ydb-lock-test-data
sudo docker volume create ydb-lock-test-certs

sudo docker run -d --rm --name ydb-lock-test -h localhost \
  --platform linux/amd64 \
  -p 2135:2135 -p 2136:2136 -p 8765:8765 -p 9092:9092 \
  -v ydb-lock-test-certs:/ydb_certs \
  -v ydb-lock-test-data:/ydb_data \
  -e GRPC_TLS_PORT=2135 -e GRPC_PORT=2136 -e MON_PORT=8765 \
  -e YDB_KAFKA_PROXY_PORT=9092 \
  ydbplatform/local-ydb:latest
```

Wait until the database reports `GOOD`:

```bash
curl --fail 'http://localhost:8765/healthcheck?format=prometheus'
```

The current `latest` image creates the `/local` database. Check the container
logs if a pinned image uses a different database path.

## Prepare Python

```bash
python3 -m venv .venv
.venv/bin/python -m pip install \
  -r ydb/tests/stress/lock_acquire/requirements.txt
```

## Run concurrent instances

All processes must use the same table path:

```bash
for i in $(seq 1 16); do
  .venv/bin/python ydb/tests/stress/lock_acquire/__main__.py \
    --endpoint grpc://localhost:2136 \
    --database /local \
    --anonymous \
    --duration-seconds 20 \
    --hold-min-ms 20 \
    --hold-max-ms 100 \
    --normal-stddev 3 \
    --instance-id worker-$i \
    >"/tmp/lock-worker-$i.log" 2>&1 &
done
wait
```

A smaller standard deviation increases contention around the distribution
mean. Use the default standard deviation to spread requests more broadly.

## Monitor server-side errors

Read the cumulative KQP counters before, during, and after the run:

```bash
curl --fail --silent \
  'http://localhost:8765/counters/counters=kqp/prometheus' |
  rg '^(YdbResponses_(Aborted|PreconditionFailed)|Transactions_Aborted) '
```

`YdbResponses_PreconditionFailed` is expected not to increase. Concurrent
read-before-write transactions can increase `YdbResponses_Aborted`; the Python
SDK retries these transient responses up to `--max-retries`.

Every successful worker prints a final JSON summary. Verify that every log has
one summary, no traceback, and that `inserted` equals `released`.
