LIBRARY(replication_workload)

ADDINCL(
    ydb/public/sdk/cpp
)

SRCS(
    replication_workload.cpp
    replication_workload_exec.cpp
    replication_workload_init.cpp
    replication_workload_model.cpp
    replication_workload_run.cpp
    replication_workload_text.cpp
)

PEERDIR(
    contrib/libs/openssl
    ydb/library/workload/tpch
    ydb/public/lib/ydb_cli/common
    ydb/public/sdk/cpp/src/client/driver
    ydb/public/sdk/cpp/src/client/params
    ydb/public/sdk/cpp/src/client/query
    ydb/public/sdk/cpp/src/client/result
    ydb/public/sdk/cpp/src/client/scheme
    ydb/public/sdk/cpp/src/client/table
    ydb/public/sdk/cpp/src/client/types/operation
    ydb/public/sdk/cpp/src/client/types/status
    ydb/public/sdk/cpp/src/client/value
)

END()

RECURSE_FOR_TESTS(
    ut
)
