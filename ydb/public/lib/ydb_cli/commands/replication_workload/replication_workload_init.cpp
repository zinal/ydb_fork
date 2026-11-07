#include "replication_workload.h"
#include "replication_workload_exec.h"
#include "replication_workload_text.h"

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/result/result.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/scheme/scheme.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/table/table.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status/status.h>

#include <util/generic/yexception.h>
#include <util/string/builder.h>
#include <util/string/printf.h>
#include <util/system/info.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <random>
#include <thread>

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

TString OneDecimal(double value) {
    return Sprintf("%.1f", value);
}

void PrintImportProgress(double elapsedSec, ui64 rows, ui64 rowsTotal, ui64 tablesDone, ui64 tables) {
    const double rate = elapsedSec > 0 ? static_cast<double>(rows) / elapsedSec : 0;
    Cout << "elapsed_s=" << OneDecimal(elapsedSec)
        << " rows=" << rows
        << " rows_total=" << rowsTotal
        << " rows_per_s=" << OneDecimal(rate)
        << " tables_done=" << tablesDone
        << " tables=" << tables
        << Endl;
}

void EnsureDirectory(const TDriver& driver, const TString& path) {
    NScheme::TSchemeClient scheme(driver);
    auto status = scheme.MakeDirectory(path).GetValueSync();
    if (!status.IsSuccess() && status.GetStatus() != EStatus::ALREADY_EXISTS) {
        NStatusHelpers::ThrowOnError(status);
    }
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
{
    const size_t cpus = NSystemInfo::CachedNumberOfCpus();
    Threads = cpus == 0 ? 1 : static_cast<ui64>(cpus);
}

void TCommandWorkloadReplicationImport::Config(TConfig& config) {
    TYdbCommand::Config(config);
    config.Opts->AddLongOption("path", "Directory of workload tables in the source database")
        .DefaultValue(Path).StoreResult(&Path);
    config.Opts->AddLongOption("rows", "Number of initial rows in every table")
        .DefaultValue(Rows).StoreResult(&Rows);
    config.Opts->AddLongOption("batch-size", "Rows in one UPSERT SELECT AS_TABLE request")
        .DefaultValue(BatchSize).StoreResult(&BatchSize);
    config.Opts->AddLongOption("threads", "Threads that send import batches. The default is the host vCPU count")
        .DefaultValue(Threads).StoreResult(&Threads);
}

int TCommandWorkloadReplicationImport::Run(TConfig& config) {
    if (BatchSize == 0) {
        throw TMisuseException() << "Batch size must be positive";
    }
    if (Threads == 0) {
        throw TMisuseException() << "Thread count must be positive";
    }
    TDriver driver = MakeDriver(config, Threads);
    const TString root = WorkloadRoot(config.Database, Path);
    auto clientSettings = NQuery::TClientSettings().SessionPoolSettings(
        NQuery::TSessionPoolSettings().MaxActiveSessions(Threads + 4));
    NQuery::TQueryClient client(driver, clientSettings);
    const TDuration timeout = TDuration::Seconds(60);
    const TVector<TTable>& tables = AllTables();
    TVector<TString> fullNames;
    TVector<TString> queries;
    fullNames.reserve(tables.size());
    queries.reserve(tables.size());
    for (const TTable& table : tables) {
        const TString fullName = JoinPath(root, table.Name);
        fullNames.push_back(fullName);
        queries.push_back(ImportQuery(fullName, table));
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
    if (Rows == 0) {
        return EXIT_SUCCESS;
    }
    PrepareTpchText();

    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::atomic<ui64> rowsDone{0};
    std::atomic<ui64> tablesDone{0};
    std::atomic<ui64> finishedWorkers{0};
    std::mutex errorMutex;
    std::mutex doneMutex;
    std::condition_variable doneCv;
    TString errorText;
    const ui64 seedBase = TInstant::Now().MicroSeconds();
    const ui64 rowsTotal = Rows * static_cast<ui64>(tables.size());
    auto fail = [&](const TString& text) {
        std::lock_guard<std::mutex> lock(errorMutex);
        if (errorText.empty()) {
            errorText = text;
        }
        failed.store(true);
    };
    auto worker = [&]() {
        while (!failed.load()) {
            const size_t index = next.fetch_add(1);
            if (index >= tables.size()) {
                break;
            }
            std::mt19937_64 rng(seedBase + index);
            ui64 written = 0;
            for (ui64 begin = 0; begin < Rows && !failed.load(); begin += BatchSize) {
                const ui64 count = std::min(BatchSize, Rows - begin);
                try {
                    const TVector<TVector<TBoundParam>> rows = MakeImportRows(tables[index], begin, count, rng);
                    const TStatus status = ExecData(
                        client,
                        queries[index],
                        BindImportRows(rows),
                        timeout,
                        true);
                    if (!status.IsSuccess()) {
                        fail(TStringBuilder() << "table " << fullNames[index]
                            << " error status=" << status.GetStatus()
                            << " issues=" << status.GetIssues().ToString());
                        break;
                    }
                    written += count;
                    rowsDone.fetch_add(count);
                } catch (const std::exception& ex) {
                    fail(TStringBuilder() << "table " << fullNames[index] << " " << ex.what());
                    break;
                } catch (...) {
                    fail(TStringBuilder() << "table " << fullNames[index] << " import failed");
                    break;
                }
            }
            if (written == Rows) {
                tablesDone.fetch_add(1);
            }
        }
        finishedWorkers.fetch_add(1);
        doneCv.notify_all();
    };

    const ui64 workerCount = std::min<ui64>(Threads, tables.size());
    const TInstant start = TInstant::Now();
    TVector<std::thread> pool;
    pool.reserve(workerCount);
    for (ui64 i = 0; i < workerCount; ++i) {
        pool.emplace_back(worker);
    }
    TInstant lastReport;
    bool reported = false;
    auto report = [&] {
        const double elapsed = static_cast<double>((TInstant::Now() - start).MicroSeconds()) / 1000000.0;
        PrintImportProgress(elapsed, rowsDone.load(), rowsTotal, tablesDone.load(), tables.size());
        lastReport = TInstant::Now();
        reported = true;
    };
    while (finishedWorkers.load() < workerCount) {
        bool done = false;
        {
            std::unique_lock lock(doneMutex);
            done = doneCv.wait_for(lock, std::chrono::seconds(30), [&] {
                return finishedWorkers.load() >= workerCount;
            });
        }
        if (done) {
            break;
        }
        report();
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    if (!reported || TInstant::Now() - lastReport >= TDuration::Seconds(1)) {
        report();
    }
    if (failed.load()) {
        throw yexception() << errorText;
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
