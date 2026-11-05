#include "replication_workload.h"
#include "replication_workload_exec.h"

#include <ydb/public/lib/ydb_cli/common/duration.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/discovery/discovery.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status/status.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/ydb.h>

#include <util/generic/hash_set.h>
#include <util/generic/yexception.h>
#include <util/string/builder.h>
#include <util/string/cast.h>
#include <util/string/printf.h>
#include <util/system/info.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace NYdb::NConsoleClient {
namespace {

using namespace NReplicationWorkload;

TString OneDecimal(double value) {
    return Sprintf("%.1f", value);
}

// Statuses the standard idempotent retry policy keeps retrying.
// NOT_FOUND is retried by default, but a missing table is not a dead node.
bool IsNodeFailure(EStatus status) {
    switch (status) {
        case EStatus::ABORTED:
        case EStatus::UNAVAILABLE:
        case EStatus::OVERLOADED:
        case EStatus::BAD_SESSION:
        case EStatus::SESSION_BUSY:
        case EStatus::UNDETERMINED:
        case EStatus::TRANSPORT_UNAVAILABLE:
        case EStatus::CLIENT_RESOURCE_EXHAUSTED:
            return true;
        default:
            return false;
    }
}

// An endpoint with an empty service list accepts every service.
bool ServesQueries(const NDiscovery::TEndpointInfo& info) {
    if (info.Services.empty()) {
        return true;
    }
    for (const std::string& service : info.Services) {
        if (service == "query_service") {
            return true;
        }
    }
    return false;
}

TString FormatEndpoint(const std::string& address, uint32_t port) {
    TStringBuilder out;
    if (address.find(':') != std::string::npos && (address.empty() || address.front() != '[')) {
        out << "[" << address << "]";
    } else {
        out << address;
    }
    out << ":" << port;
    return out;
}

struct TDiscoveredNode {
    ui32 NodeId = 0;
    TString Endpoint;
    bool Ssl = false;
};

TVector<TDiscoveredNode> CollectNodes(const std::vector<NDiscovery::TEndpointInfo>& infos, bool queriesOnly) {
    THashSet<TString> seen;
    TVector<TDiscoveredNode> nodes;
    for (const NDiscovery::TEndpointInfo& info : infos) {
        if (queriesOnly && !ServesQueries(info)) {
            continue;
        }
        const TString endpoint = FormatEndpoint(info.Address, info.Port);
        const TString key = info.NodeId == 0 ? TString("e:") + endpoint : TString("n:") + ToString(info.NodeId);
        if (!seen.insert(key).second) {
            continue;
        }
        TDiscoveredNode node;
        node.NodeId = info.NodeId;
        node.Endpoint = endpoint;
        node.Ssl = info.Ssl;
        nodes.push_back(std::move(node));
    }
    std::sort(nodes.begin(), nodes.end(), [](const TDiscoveredNode& left, const TDiscoveredNode& right) {
        if (left.NodeId != right.NodeId) {
            return left.NodeId < right.NodeId;
        }
        return left.Endpoint < right.Endpoint;
    });
    return nodes;
}

// EXPLAIN compiles the statement and does not execute it.
// Declared parameter types are enough, so values are not sent.
TStatus CompileQuery(
    NQuery::TQueryClient& client,
    const TString& text,
    const TDuration& timeout,
    const NQuery::TRetryOperationSettings& retry)
{
    const auto settings = NQuery::TExecuteQuerySettings()
        .ExecMode(NQuery::EExecMode::Explain)
        .ClientTimeout(timeout);
    const auto tx = NQuery::TTxControl::BeginTx(NQuery::TTxSettings::SerializableRW()).CommitTx(true);
    return client.RetryQuerySync([text, tx, settings](NQuery::TSession session) {
        auto result = session.ExecuteQuery(std::string(text), tx, settings).GetValueSync();
        return TStatus(std::move(result));
    }, retry);
}

enum class ECompileResult {
    Compiled,
    QueryError,
    Skipped,
    Pessimized,
};

struct TNodeTarget {
    ui32 NodeId = 0;
    TString Endpoint;
    std::atomic<bool> Pessimized{false};
    // True while this node is inside the standard backoff budget.
    // Other workers wait instead of sending another request.
    std::atomic<bool> BackingOff{false};
    std::mutex RetryLock;
    std::condition_variable RetryCv;
    TDriver Driver;
    NQuery::TQueryClient Client;

    TNodeTarget(ui32 nodeId, TString endpoint, TDriver driver, uint32_t sessions)
        : NodeId(nodeId)
        , Endpoint(std::move(endpoint))
        , Driver(std::move(driver))
        , Client(Driver, NQuery::TClientSettings().SessionPoolSettings(
            NQuery::TSessionPoolSettings().MaxActiveSessions(sessions)))
    {}
};

} // namespace

TCommandWorkloadReplicationCompile::TCommandWorkloadReplicationCompile()
    : TYdbCommand("compile", {}, "Compile every UPSERT and UPDATE statement that run can send")
{}

void TCommandWorkloadReplicationCompile::Config(TConfig& config) {
    TYdbCommand::Config(config);
    config.Opts->AddLongOption("path", "Directory of workload tables in the source database")
        .DefaultValue(Path).StoreResult(&Path);
    config.Opts->AddLongOption("threads", "Parallel compile requests. 0 uses the host vCPU count. "
        "The pool is raised to the number of discovered nodes, so every node is compiled at once")
        .DefaultValue(Threads).StoreResult(&Threads);
    config.Opts->AddLongOption("timeout", "Timeout of one compile request. A plain number is milliseconds")
        .DefaultValue(Timeout).StoreResult(&Timeout);
}

int TCommandWorkloadReplicationCompile::Run(TConfig& config) {
    const TDuration timeout = ParseDurationMilliseconds(Timeout);
    const TVector<TRunQuery> queries = RunQueries(WorkloadRoot(config.Database, Path));
    if (queries.empty()) {
        throw TMisuseException() << "No queries to compile";
    }

    TDriver driver(config.CreateDriverConfig());
    NDiscovery::TDiscoveryClient discovery(driver);
    auto listed = discovery.ListEndpoints().GetValueSync();
    NStatusHelpers::ThrowOnError(listed);
    TVector<TDiscoveredNode> discovered = CollectNodes(listed.GetEndpointsInfo(), true);
    if (discovered.empty()) {
        discovered = CollectNodes(listed.GetEndpointsInfo(), false);
    }
    if (discovered.empty()) {
        ythrow yexception() << "Discovery returned no nodes";
    }

    const ui64 cpus = NSystemInfo::CachedNumberOfCpus();
    ui64 threads = Threads == 0 ? (cpus == 0 ? 1 : cpus) : Threads;
    if (threads < discovered.size()) {
        threads = discovered.size();
    }
    const uint32_t sessions = static_cast<uint32_t>(std::min<ui64>(threads, std::numeric_limits<uint32_t>::max()));

    std::vector<std::unique_ptr<TNodeTarget>> nodes;
    nodes.reserve(discovered.size());
    for (const TDiscoveredNode& found : discovered) {
        TDriverConfig nodeConfig = driver.GetConfig();
        nodeConfig.SetEndpoint(std::string(found.Endpoint));
        nodeConfig.SetDiscoveryMode(EDiscoveryMode::Off);
        nodeConfig.SetNetworkThreadsNum(1);
        nodeConfig.SetUsePerChannelTcpConnection(config.UsePerChannelTcpConnection);
        if (found.Ssl) {
            nodeConfig.UseSecureConnection(std::string(config.CaCerts));
        }
        nodes.push_back(std::make_unique<TNodeTarget>(
            found.NodeId,
            found.Endpoint,
            TDriver(nodeConfig),
            sessions));
    }

    const ui64 nodeCount = nodes.size();
    const ui64 queryCount = queries.size();
    const ui64 total = nodeCount * queryCount;
    const bool verbose = config.IsVerbose();

    std::atomic<ui64> compiled{0};
    std::atomic<ui64> errors{0};
    std::atomic<ui64> skipped{0};
    std::atomic<ui64> pessimized{0};
    std::atomic<bool> printedError{false};
    std::mutex printLock;

    auto printProgress = [&](double elapsedSec) {
        std::lock_guard<std::mutex> lock(printLock);
        Cout << "elapsed_s=" << OneDecimal(elapsedSec)
            << " compiled=" << compiled.load()
            << " total=" << total
            << " errors=" << errors.load()
            << " skipped=" << skipped.load()
            << " pessimized=" << pessimized.load()
            << " nodes=" << nodeCount
            << Endl;
    };

    auto printStatus = [&](const char* kind, const TNodeTarget& node, const TRunQuery* query, const TStatus& status) {
        std::lock_guard<std::mutex> lock(printLock);
        Cerr << kind
            << " node=" << node.NodeId
            << " endpoint=" << node.Endpoint;
        if (query) {
            Cerr << " query=\"" << query->Name << "\"";
        }
        Cerr << " status=" << status.GetStatus()
            << " issues=" << status.GetIssues().ToString()
            << Endl;
    };

    Cout << "nodes=" << nodeCount
        << " queries=" << queryCount
        << " total=" << total
        << " threads=" << threads
        << Endl;

    const auto once = NQuery::TRetryOperationSettings()
        .Idempotent(true)
        .MaxRetries(0)
        .RetryNotFound(false)
        .GetSessionClientTimeout(timeout);
    // Default fast and slow backoff, and the default retry limit.
    // A missing table is a query error, so NOT_FOUND is not retried.
    const auto backoff = NQuery::TRetryOperationSettings()
        .Idempotent(true)
        .RetryNotFound(false)
        .GetSessionClientTimeout(timeout);

    const TInstant started = TInstant::Now();
    std::atomic<size_t> nextJob{0};
    std::atomic<bool> stopReport{false};
    std::thread reporter([&] {
        TInstant next = started + TDuration::Seconds(1);
        while (!stopReport.load()) {
            Sleep(TDuration::MilliSeconds(100));
            if (TInstant::Now() < next) {
                continue;
            }
            const double elapsed = static_cast<double>((TInstant::Now() - started).MicroSeconds()) / 1000000.0;
            printProgress(elapsed);
            next += TDuration::Seconds(1);
        }
    });

    auto worker = [&] {
        while (true) {
            const size_t job = nextJob.fetch_add(1);
            if (job >= total) {
                return;
            }
            const size_t nodeIndex = job % nodeCount;
            const size_t queryIndex = job / nodeCount;
            TNodeTarget& node = *nodes[nodeIndex];
            const TRunQuery& query = queries[queryIndex];
            try {
                auto waitUntilSendable = [&](std::unique_lock<std::mutex>& lock) {
                    node.RetryCv.wait(lock, [&] {
                        return !node.BackingOff.load() || node.Pessimized.load();
                    });
                    return !node.Pessimized.load();
                };
                {
                    std::unique_lock<std::mutex> lock(node.RetryLock);
                    if (!waitUntilSendable(lock)) {
                        skipped.fetch_add(1);
                        continue;
                    }
                }
                auto noteQueryError = [&](const TStatus& status) {
                    errors.fetch_add(1);
                    if (verbose || !printedError.exchange(true)) {
                        printStatus("error", node, &query, status);
                    }
                };
                TStatus status = CompileQuery(node.Client, query.Text, timeout, once);
                if (status.IsSuccess()) {
                    compiled.fetch_add(1);
                    continue;
                }
                if (!IsNodeFailure(status.GetStatus())) {
                    noteQueryError(status);
                    continue;
                }
                std::unique_lock<std::mutex> lock(node.RetryLock);
                // Another worker may already be spending this node's backoff budget.
                // Wait for that result instead of sending a parallel retry.
                if (node.BackingOff.load() || node.Pessimized.load()) {
                    if (!waitUntilSendable(lock)) {
                        skipped.fetch_add(1);
                        continue;
                    }
                    status = CompileQuery(node.Client, query.Text, timeout, once);
                    if (status.IsSuccess()) {
                        compiled.fetch_add(1);
                        continue;
                    }
                    if (!IsNodeFailure(status.GetStatus())) {
                        noteQueryError(status);
                        continue;
                    }
                }
                // The node failed. Retry with the standard backoff and, while that
                // runs, do not send anything else to it. Past the budget, drop the node.
                node.BackingOff.store(true);
                struct TStopBackoff {
                    TNodeTarget& Node;
                    ~TStopBackoff() {
                        Node.BackingOff.store(false);
                        Node.RetryCv.notify_all();
                    }
                } stopBackoff{node};
                status = CompileQuery(node.Client, query.Text, timeout, backoff);
                if (status.IsSuccess()) {
                    compiled.fetch_add(1);
                    continue;
                }
                if (!IsNodeFailure(status.GetStatus())) {
                    noteQueryError(status);
                    continue;
                }
                node.Pessimized.store(true);
                pessimized.fetch_add(1);
                printStatus("pessimized", node, &query, status);
                errors.fetch_add(1);
            } catch (const std::exception& ex) {
                errors.fetch_add(1);
                if (verbose || !printedError.exchange(true)) {
                    std::lock_guard<std::mutex> lock(printLock);
                    Cerr << "error node=" << node.NodeId
                        << " endpoint=" << node.Endpoint
                        << " query=\"" << query.Name << "\""
                        << " issues=" << ex.what()
                        << Endl;
                }
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (ui64 i = 0; i < threads; ++i) {
        pool.emplace_back(worker);
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    stopReport.store(true);
    reporter.join();

    const double elapsed = static_cast<double>((TInstant::Now() - started).MicroSeconds()) / 1000000.0;
    printProgress(elapsed);

    for (std::unique_ptr<TNodeTarget>& node : nodes) {
        node->Driver.Stop(true);
    }
    driver.Stop(true);

    if (errors.load() != 0 || pessimized.load() != 0) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace NYdb::NConsoleClient
