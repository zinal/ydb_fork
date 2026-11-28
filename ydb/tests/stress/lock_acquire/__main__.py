#!/usr/bin/env python3

import argparse
import collections
import json
import logging
import os
import random
import socket
import time
import uuid

import ydb


LOCK_COUNT = 100


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Concurrent check-and-UPSERT test for a lock table. Run several "
            "instances with the same endpoint, database, and table path."
        ),
        epilog="""
Example:
  for i in $(seq 1 8); do
    ./lock_acquire_test --database /Root --instance-id worker-$i &
  done
  wait
""",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--endpoint", default="localhost:2135", help="YDB endpoint")
    parser.add_argument("--database", required=True, help="YDB database")
    parser.add_argument(
        "--table-path",
        default="lock_acquire_test",
        help="Absolute table path or path relative to the database",
    )
    parser.add_argument(
        "--duration-seconds",
        type=float,
        default=60.0,
        help="Test duration",
    )
    parser.add_argument(
        "--hold-min-ms",
        type=int,
        default=100,
        help="Minimum random lock hold time",
    )
    parser.add_argument(
        "--hold-max-ms",
        type=int,
        default=500,
        help="Maximum random lock hold time",
    )
    parser.add_argument(
        "--normal-mean",
        type=float,
        default=(LOCK_COUNT - 1) / 2,
        help="Mean of the truncated normal lock-id distribution",
    )
    parser.add_argument(
        "--normal-stddev",
        type=float,
        default=LOCK_COUNT / 6,
        help="Standard deviation of the truncated normal lock-id distribution",
    )
    parser.add_argument(
        "--busy-backoff-max-ms",
        type=int,
        default=10,
        help="Maximum random delay after an ALREADY_EXISTS result",
    )
    parser.add_argument(
        "--max-retries",
        type=int,
        default=20,
        help="Maximum SDK retries for transient errors such as ABORTED",
    )
    parser.add_argument(
        "--report-every-seconds",
        type=float,
        default=5.0,
        help="Progress reporting interval; zero disables progress reports",
    )
    parser.add_argument(
        "--instance-id",
        default=None,
        help="Instance label; defaults to host, process id, and a random suffix",
    )
    parser.add_argument("--seed", type=int, default=None, help="Random seed")
    return parser.parse_args()


def validate_args(args):
    if args.duration_seconds <= 0:
        raise ValueError("--duration-seconds must be positive")
    if args.hold_min_ms < 0 or args.hold_max_ms < args.hold_min_ms:
        raise ValueError(
            "--hold-max-ms must be greater than or equal to --hold-min-ms"
        )
    if not 0 <= args.normal_mean < LOCK_COUNT:
        raise ValueError(f"--normal-mean must be in [0, {LOCK_COUNT})")
    if args.normal_stddev <= 0:
        raise ValueError("--normal-stddev must be positive")
    if args.busy_backoff_max_ms < 0:
        raise ValueError("--busy-backoff-max-ms must be non-negative")
    if args.max_retries < 0:
        raise ValueError("--max-retries must be non-negative")
    if args.report_every_seconds < 0:
        raise ValueError("--report-every-seconds must be non-negative")


def make_table_path(database, table_path):
    if not table_path:
        raise ValueError("--table-path must not be empty")
    if "`" in table_path or any(ord(char) < 32 for char in table_path):
        raise ValueError("--table-path contains unsupported characters")
    if table_path.startswith("/"):
        return table_path.rstrip("/")
    return f"{database.rstrip('/')}/{table_path.strip('/')}"


def choose_lock_id(rng, mean, stddev):
    # Rejection sampling produces a truncated normal distribution without
    # accumulating out-of-range values at ids 0 and 99.
    while True:
        lock_id = round(rng.gauss(mean, stddev))
        if 0 <= lock_id < LOCK_COUNT:
            return lock_id


class LockAcquireTest:
    def __init__(self, pool, table_path, retry_settings, instance_id, rng, args):
        self.pool = pool
        self.table_path = table_path
        self.retry_settings = retry_settings
        self.instance_id = instance_id
        self.rng = rng
        self.args = args
        self.stats = collections.Counter()
        self.selected_ids = collections.Counter()
        self.batch_type = ydb.ListType(
            ydb.StructType()
            .add_member("id", ydb.PrimitiveType.Uint32)
            .add_member("owner", ydb.PrimitiveType.Utf8)
            .add_member("acquired_at", ydb.PrimitiveType.Uint64)
            .add_member("hold_ms", ydb.PrimitiveType.Uint32)
        )

        self.acquire_query = f"""
            DECLARE $batch AS List<Struct<
                id:Uint32,
                owner:Utf8,
                acquired_at:Uint64,
                hold_ms:Uint32
            >>;

            $input = (
                SELECT id, owner, acquired_at, hold_ms
                FROM AS_TABLE($batch)
            );

            $classified = (
                SELECT
                    b.id AS id,
                    b.owner AS owner,
                    b.acquired_at AS acquired_at,
                    b.hold_ms AS hold_ms,
                    s.id IS NULL AS inserted,
                    s.owner AS existing_owner
                FROM $input AS b
                LEFT JOIN `{self.table_path}` AS s
                    ON s.id = b.id
            );

            $to_insert = (
                SELECT id, owner, acquired_at, hold_ms
                FROM $classified
                WHERE inserted
            );

            UPSERT INTO `{self.table_path}` (id, owner, acquired_at, hold_ms)
            SELECT id, owner, acquired_at, hold_ms
            FROM $to_insert;

            SELECT
                id,
                inserted,
                existing_owner,
                IF(inserted, "INSERTED"u, "ALREADY_EXISTS"u) AS status
            FROM $classified;
        """

        self.release_query = f"""
            DECLARE $id AS Uint32;
            DECLARE $owner AS Utf8;

            DELETE FROM `{self.table_path}`
            WHERE id = $id AND owner = $owner
            RETURNING id, owner;
        """

    def create_table(self):
        query = f"""
            CREATE TABLE IF NOT EXISTS `{self.table_path}` (
                id Uint32 NOT NULL,
                owner Utf8 NOT NULL,
                acquired_at Uint64 NOT NULL,
                hold_ms Uint32 NOT NULL,
                PRIMARY KEY (id)
            );
        """
        self.pool.execute_with_retries(
            query,
            retry_settings=self.retry_settings,
        )

    def acquire(self, lock_id, owner, hold_ms):
        result_sets = self.pool.execute_with_retries(
            self.acquire_query,
            parameters={
                "$batch": (
                    [
                        {
                            "id": lock_id,
                            "owner": owner,
                            "acquired_at": time.time_ns() // 1_000,
                            "hold_ms": hold_ms,
                        }
                    ],
                    self.batch_type,
                ),
            },
            retry_settings=self.retry_settings,
        )
        if len(result_sets) != 1 or len(result_sets[0].rows) != 1:
            raise AssertionError(
                f"acquire returned an unexpected result shape: "
                f"{len(result_sets)} result sets"
            )

        row = result_sets[0].rows[0]
        if row["id"] != lock_id:
            raise AssertionError(
                f"acquire returned id {row['id']}, expected {lock_id}"
            )

        inserted = row["inserted"]
        existing_owner = row["existing_owner"]
        expected_status = "INSERTED" if inserted else "ALREADY_EXISTS"
        if row["status"] != expected_status:
            raise AssertionError(
                f"lock {lock_id} returned status={row['status']!r}, "
                f"expected {expected_status!r}"
            )
        if inserted:
            if existing_owner is not None:
                raise AssertionError(
                    f"lock {lock_id} was inserted but existing_owner={existing_owner!r}"
                )
            return True

        # A retry after an ambiguous response can observe a write committed by
        # the same attempt. Treat that case as an idempotently acquired lock.
        if existing_owner == owner:
            self.stats["recovered_own_write"] += 1
            return True

        return False

    def release(self, lock_id, owner):
        result_sets = self.pool.execute_with_retries(
            self.release_query,
            parameters={
                "$id": (lock_id, ydb.PrimitiveType.Uint32),
                "$owner": owner,
            },
            retry_settings=self.retry_settings,
        )
        if len(result_sets) != 1:
            raise AssertionError(
                f"release returned {len(result_sets)} result sets, expected one"
            )
        rows = result_sets[0].rows
        if (
            len(rows) != 1
            or rows[0]["id"] != lock_id
            or rows[0]["owner"] != owner
        ):
            raise AssertionError(
                f"lock {lock_id} ownership was lost before release; owner={owner!r}"
            )

    def run(self):
        deadline = time.monotonic() + self.args.duration_seconds
        next_report = time.monotonic() + self.args.report_every_seconds

        while time.monotonic() < deadline:
            self.stats["attempts"] += 1
            attempt = self.stats["attempts"]
            lock_id = choose_lock_id(
                self.rng,
                self.args.normal_mean,
                self.args.normal_stddev,
            )
            self.selected_ids[lock_id] += 1
            hold_ms = self.rng.randint(
                self.args.hold_min_ms,
                self.args.hold_max_ms,
            )
            owner = f"{self.instance_id}:{attempt}:{uuid.uuid4().hex}"

            acquired = self.acquire(lock_id, owner, hold_ms)
            if acquired:
                self.stats["inserted"] += 1
                try:
                    time.sleep(hold_ms / 1_000)
                finally:
                    self.release(lock_id, owner)
                    self.stats["released"] += 1
            else:
                self.stats["already_exists"] += 1
                if self.args.busy_backoff_max_ms:
                    time.sleep(
                        self.rng.uniform(0, self.args.busy_backoff_max_ms) / 1_000
                    )

            if (
                self.args.report_every_seconds
                and time.monotonic() >= next_report
            ):
                logging.info("progress: %s", self.summary())
                next_report = time.monotonic() + self.args.report_every_seconds

    def summary(self):
        return {
            "instance_id": self.instance_id,
            "attempts": self.stats["attempts"],
            "inserted": self.stats["inserted"],
            "already_exists": self.stats["already_exists"],
            "released": self.stats["released"],
            "recovered_own_write": self.stats["recovered_own_write"],
            "distinct_lock_ids": len(self.selected_ids),
            "most_selected_lock_ids": self.selected_ids.most_common(10),
        }


def main():
    args = parse_args()
    validate_args(args)
    table_path = make_table_path(args.database, args.table_path)
    instance_id = args.instance_id or (
        f"{socket.gethostname()}-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    )
    rng = random.Random(args.seed)

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )
    logging.info(
        "starting instance=%s table=%s lock_ids=%d",
        instance_id,
        table_path,
        LOCK_COUNT,
    )

    driver = ydb.Driver(
        ydb.DriverConfig(
            args.endpoint,
            args.database,
            credentials=ydb.credentials_from_env_variables(),
        )
    )
    driver.wait(timeout=10, fail_fast=True)
    retry_settings = ydb.RetrySettings(max_retries=args.max_retries)

    try:
        with ydb.QuerySessionPool(driver, size=1) as pool:
            test = LockAcquireTest(
                pool,
                table_path,
                retry_settings,
                instance_id,
                rng,
                args,
            )
            test.create_table()
            test.run()
            print(json.dumps(test.summary(), sort_keys=True))
    finally:
        driver.stop()


if __name__ == "__main__":
    main()
