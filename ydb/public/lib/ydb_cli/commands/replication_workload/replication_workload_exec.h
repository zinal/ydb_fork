#pragma once

#include "replication_workload_model.h"

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/table/table.h>

#include <library/cpp/threading/future/future.h>
#include <util/datetime/base.h>

namespace NYdb::NConsoleClient::NReplicationWorkload {

TString WorkloadRoot(const TString& database, const TString& path);
TParams BindParams(const TVector<TBoundParam>& params);

NQuery::TExecuteQuerySettings MakeQuerySettings(
    const TDuration& clientTimeout,
    const TDuration& operationTimeout,
    const TDuration& cancelAfter);
void ExecScheme(NTable::TTableClient& client, const TString& query);
NThreading::TFuture<TStatus> ExecDataAsync(
    NQuery::TQueryClient& client,
    const TString& script,
    const TParams& params,
    const TDuration& clientTimeout,
    const TDuration& operationTimeout,
    const TDuration& cancelAfter,
    bool idempotent);
TStatus ExecData(
    NQuery::TQueryClient& client,
    const TString& script,
    const TParams& params,
    const TDuration& clientTimeout,
    const TDuration& operationTimeout,
    const TDuration& cancelAfter,
    bool idempotent);

} // namespace NYdb::NConsoleClient::NReplicationWorkload
