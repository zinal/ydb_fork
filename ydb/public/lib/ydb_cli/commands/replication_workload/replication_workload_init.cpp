#include "replication_workload.h"
#include "replication_workload_exec.h"

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/result/result.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/scheme/scheme.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/table/table.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status/status.h>

#include <util/string/cast.h>

#include <random>

namespace NYdb::NConsoleClient {

using namespace NReplicationWorkload;

namespace {

TDriver MakeDriver(TClientCommand::TConfig& config, ui64 threads) {
    auto driverConfig = config.CreateDriverConfig();
    if (threads > 0) {
        driverConfig.SetClientThreadsNum(threads);
    }
    return TDriver(driverConfig);
}

void EnsureDirectory(const TDriver& driver, const TString& path) {
    NScheme::TSchemeClient scheme(driver);
    auto status = scheme.MakeDirectory(path).GetValueSync();
    if (!status.IsSuccess() && status.GetStatus() != EStatus::ALREADY_EXISTS) {
        NStatusHelpers::ThrowOnError(status);
    }
}

TVector<TBoundParam> Flatten(const TVector<TRowWrite>& writes) {
    TVector<TBoundParam> params;
    for (const TRowWrite& write : writes) {
        params.insert(params.end(), write.Params.begin(), write.Params.end());
    }
    return params;
}

} // namespace

TCommandWorkloadReplicationInit::TCommandWorkloadReplicationInit()
    : TYdbCommand("init", {}, "Create replication workload tables")
{}

void TCommandWorkloadReplicationInit::Config(TConfig& config) {
    TYdbCommand::Config(config);
    config.Opts->AddLongOption("path", "Directory of workload tables in the source database")
        .DefaultValue(Path).StoreResult(&Path);
    config.Opts->AddLongOption("min-partitions", "Minimum partition count of each table and secondary index, AUTO_PARTITIONING_MIN_PARTITIONS_COUNT")
        .DefaultValue(MinPartitions).StoreResult(&MinPartitions);
    config.Opts->AddLongOption("max-partitions", "Maximum partition count of each table and secondary index, AUTO_PARTITIONING_MAX_PARTITIONS_COUNT")
        .DefaultValue(MaxPartitions).StoreResult(&MaxPartitions);
}

int TCommandWorkloadReplicationInit::Run(TConfig& config) {
    if (MinPartitions > MaxPartitions) {
        throw TMisuseException() << "Minimum partition count must not exceed the maximum";
    }
    TDriver driver = MakeDriver(config, 0);
    const TString root = WorkloadRoot(config.Database, Path);
    EnsureDirectory(driver, root);
    NTable::TTableClient tableClient(driver);
    for (const TTable& table : AllTables()) {
        const TString fullName = JoinPath(root, table.Name);
        ExecScheme(tableClient, CreateTableQuery(fullName, table, MinPartitions, MaxPartitions));
        for (const TString& query : AlterIndexQueries(fullName, table, MinPartitions, MaxPartitions)) {
            ExecScheme(tableClient, query);
        }
    }
    return EXIT_SUCCESS;
}

TCommandWorkloadReplicationImport::TCommandWorkloadReplicationImport()
    : TYdbCommand("import", {}, "Insert the initial rows of the replication workload")
{}

void TCommandWorkloadReplicationImport::Config(TConfig& config) {
    TYdbCommand::Config(config);
    config.Opts->AddLongOption("path", "Directory of workload tables in the source database")
        .DefaultValue(Path).StoreResult(&Path);
    config.Opts->AddLongOption("rows", "Number of initial rows in every table")
        .DefaultValue(Rows).StoreResult(&Rows);
}

int TCommandWorkloadReplicationImport::Run(TConfig& config) {
    TDriver driver = MakeDriver(config, 0);
    const TString root = WorkloadRoot(config.Database, Path);
    auto clientSettings = NQuery::TClientSettings().SessionPoolSettings(
        NQuery::TSessionPoolSettings().MaxActiveSessions(8));
    NQuery::TQueryClient client(driver, clientSettings);
    const TDuration timeout = TDuration::Seconds(60);
    for (const TTable& table : AllTables()) {
        const TString fullName = JoinPath(root, table.Name);
        const TString key = table.Columns[0].Name;
        const TString probe = "SELECT " + key + " FROM " + QuotePath(fullName) + " LIMIT 1;";
        auto result = client.ExecuteQuery(
            probe,
            NQuery::TTxControl::BeginTx(NQuery::TTxSettings::SerializableRW()).CommitTx(true),
            NQuery::TExecuteQuerySettings().ClientTimeout(timeout)).GetValueSync();
        NStatusHelpers::ThrowOnError(result);
        TResultSetParser parser(result.GetResultSet(0));
        if (parser.TryNextRow()) {
            throw TMisuseException() << "Table " << fullName << " already has rows";
        }
    }
    std::mt19937_64 rng(TInstant::Now().MicroSeconds());
    for (const TTable& table : AllTables()) {
        const TString fullName = JoinPath(root, table.Name);
        const ui64 batch = table.Wide ? 4 : 40;
        for (ui64 begin = 0; begin < Rows;) {
            const ui64 count = std::min(batch, Rows - begin);
            TVector<TRowWrite> writes;
            for (ui64 row = 0; row < count; ++row) {
                writes.push_back(MakeInsertWrite(table, fullName, begin + row, 0, TStringBuilder() << "r" << row << "_", rng));
            }
            auto status = ExecData(client, BuildScript(writes), BindParams(Flatten(writes)), timeout, timeout, timeout, true);
            NStatusHelpers::ThrowOnError(status);
            begin += count;
        }
    }
    return EXIT_SUCCESS;
}

TCommandWorkloadReplicationClean::TCommandWorkloadReplicationClean()
    : TYdbCommand("clean", {}, "Drop replication workload tables")
{}

void TCommandWorkloadReplicationClean::Config(TConfig& config) {
    TYdbCommand::Config(config);
    config.Opts->AddLongOption("path", "Directory of workload tables in the source database")
        .DefaultValue(Path).StoreResult(&Path);
}

int TCommandWorkloadReplicationClean::Run(TConfig& config) {
    TDriver driver = MakeDriver(config, 0);
    const TString root = WorkloadRoot(config.Database, Path);
    NTable::TTableClient tableClient(driver);
    for (const TTable& table : AllTables()) {
        const TString fullName = JoinPath(root, table.Name);
        auto sessionResult = tableClient.CreateSession().GetValueSync();
        NStatusHelpers::ThrowOnError(sessionResult);
        auto result = sessionResult.GetSession().ExecuteSchemeQuery("DROP TABLE " + QuotePath(fullName)).GetValueSync();
        if (!result.IsSuccess() && result.GetStatus() != EStatus::NOT_FOUND) {
            NStatusHelpers::ThrowOnError(result);
        }
    }
    NScheme::TSchemeClient scheme(driver);
    auto removed = scheme.RemoveDirectory(root).GetValueSync();
    if (!removed.IsSuccess() && removed.GetStatus() != EStatus::NOT_FOUND) {
        NStatusHelpers::ThrowOnError(removed);
    }
    return EXIT_SUCCESS;
}

} // namespace NYdb::NConsoleClient
