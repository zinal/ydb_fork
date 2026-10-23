UNITTEST_FOR(ydb/public/lib/ydb_cli/commands/replication_workload)

ADDINCL(
    ydb/public/sdk/cpp
)

SRCS(
    replication_workload_ut.cpp
)

PEERDIR(
    ydb/public/lib/ydb_cli/commands/replication_workload
)

END()
