#pragma once

#include "replication_workload_model.h"

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/table/table.h>

#include <util/datetime/base.h>

namespace NYdb::NConsoleClient::NReplicationWorkload {

TString WorkloadRoot(const TString& database, const TString& path);
TParams BindParams(const TVector<TBoundParam>& params);
TParams BindImportRows(const TVector<TVector<TBoundParam>>& rows);

NQuery::TExecuteQuerySettings MakeQuerySettings(const TDuration& timeout);
void ExecScheme(NTable::TTableClient& client, const TString& query);
TStatus ExecData(
    NQuery::TQueryClient& client,
    const TString& script,
    const TParams& params,
    const TDuration& timeout,
    bool idempotent);
// Runs every write as its own query inside one interactive serializable transaction.
// The whole transaction goes through RetryQuerySync with the idempotent flag:
// the first query opens it, the following queries continue it, then it is committed.
// A retriable status repeats that sequence from the beginning.
TStatus ExecWrites(
    NQuery::TQueryClient& client,
    const TVector<TRowWrite>& writes,
    const TDuration& timeout);

} // namespace NYdb::NConsoleClient::NReplicationWorkload
