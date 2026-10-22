#include "replication_workload.h"
#include "replication_workload_exec.h"

#include <ydb/public/lib/ydb_cli/common/duration.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/result/result.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status/status.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <util/generic/hash_set.h>
#include <util/string/printf.h>
#include <util/string/cast.h>
#include <util/system/thread.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

namespace NYdb::NConsoleClient {
namespace {

using namespace NReplicationWorkload;

TString OneDecimal(double value) {
    return Sprintf("%.1f", value);
}

void PrintStats(double elapsedSec, double windowSec, ui64 tx, ui64 errors) {
    const double txRate = windowSec > 0 ? static_cast<double>(tx) / windowSec : 0;
    const double errorRate = windowSec > 0 ? static_cast<double>(errors) / windowSec : 0;
    Cout << "elapsed_s=" << OneDecimal(elapsedSec)
        << " tx_per_s=" << OneDecimal(txRate)
        << " errors_per_s=" << OneDecimal(errorRate)
        << " tx=" << tx
        << " errors=" << errors
        << Endl;
}

i64 DecodeOrdered(EKeyKind key, TValueParser& column) {
    switch (key) {
        case EKeyKind::Int32:
            return column.GetInt32();
        case EKeyKind::Int64:
            return column.GetInt64();
        case EKeyKind::Uint32:
            return column.GetUint32();
        case EKeyKind::Uint64:
        case EKeyKind::WideId:
            return static_cast<i64>(column.GetUint64());
        case EKeyKind::Decimal22_9:
        case EKeyKind::Decimal35_0: {
            TString text = column.GetDecimal().ToString();
            const size_t dot = text.find('.');
            if (dot != TString::npos) {
                text = text.substr(0, dot);
            }
            if (text.empty() || text == "-") {
                return 0;
            }
            return FromString<i64>(text);
        }
        case EKeyKind::Date32:
            return column.GetDate32().time_since_epoch().count() + 53375809;
        case EKeyKind::Datetime:
            return static_cast<i64>(column.GetDatetime().Seconds());
        case EKeyKind::Datetime64:
            return column.GetDatetime64().time_since_epoch().count() + 4611669897600ll;
        case EKeyKind::Timestamp:
            return static_cast<i64>(column.GetTimestamp().MicroSeconds());
        case EKeyKind::Timestamp64:
            return column.GetTimestamp64().time_since_epoch().count() + 4611669897600000000ll;
        case EKeyKind::Interval:
            return column.GetInterval();
        case EKeyKind::Interval64:
            return column.GetInterval64().count();
        case EKeyKind::Uuid: {
            const TUuidValue uuid = column.GetUuid();
            ui64 n = 0;
            for (int i = 0; i < 8; ++i) {
                n = (n << 8) | static_cast<unsigned char>(uuid.Buf_.Bytes[8 + i]);
            }
            return static_cast<i64>(n);
        }
        default:
            ythrow yexception() << "unexpected key kind for ordered boundary";
    }
}

i64 LoadBoundary(
    NQuery::TQueryClient& client,
    const TTable& table,
    const TString& fullName,
    const TDuration& timeout)
{
    TString query;
    if (table.HashKey) {
        query = "SELECT COUNT(*) AS pk FROM " + QuotePath(fullName) + ";";
    } else if (table.Composite) {
        query = "SELECT seq AS pk FROM " + QuotePath(fullName) + " ORDER BY seq DESC LIMIT 1;";
    } else {
        const TString& key = table.Columns[0].Name;
        query = "SELECT " + key + " AS pk FROM " + QuotePath(fullName) + " ORDER BY " + key + " DESC LIMIT 1;";
    }
    auto result = client.ExecuteQuery(
        query,
        NQuery::TTxControl::BeginTx(NQuery::TTxSettings::SerializableRW()).CommitTx(true),
        MakeQuerySettings(timeout, timeout, timeout)).GetValueSync();
    NStatusHelpers::ThrowOnError(result);
    TResultSetParser parser(result.GetResultSet(0));
    if (!parser.TryNextRow()) {
        return -1;
    }
    if (table.HashKey) {
        const ui64 count = parser.ColumnParser("pk").GetUint64();
        return count == 0 ? -1 : static_cast<i64>(count - 1);
    }
    if (table.Composite) {
        return static_cast<i64>(parser.ColumnParser("pk").GetUint64());
    }
    return DecodeOrdered(table.Key, parser.ColumnParser("pk"));
}

struct TEvent {
    TInstant When;
    bool Finish = false;
    size_t User = 0;
    bool Success = false;
    TString Error;
    std::shared_ptr<TTxPlan> Plan;
};

struct TEventLater {
    bool operator()(const TEvent& left, const TEvent& right) const {
        return left.When > right.When;
    }
};

TVector<TBoundParam> Flatten(const TVector<TRowWrite>& writes) {
    TVector<TBoundParam> params;
    for (const TRowWrite& write : writes) {
        params.insert(params.end(), write.Params.begin(), write.Params.end());
    }
    return params;
}

class TRunner {
public:
    TRunner(
        NQuery::TQueryClient& client,
        TString root,
        ui64 users,
        ui64 maxRecords,
        TDuration pause,
        TDuration clientTimeout,
        TDuration operationTimeout,
        TDuration cancelAfter,
        ui64 seed,
        bool verbose)
        : Client(client)
        , Root(std::move(root))
        , Users(users)
        , MaxRecords(maxRecords)
        , Pause(pause)
        , ClientTimeout(clientTimeout)
        , OperationTimeout(operationTimeout)
        , CancelAfter(cancelAfter)
        , Verbose(verbose)
        , Tables(AllTables())
        , State(Tables.size())
    {
        Rng.reserve(users);
        for (ui64 i = 0; i < users; ++i) {
            Rng.emplace_back(seed + i);
        }
    }

    void Load(const TDuration& timeout) {
        for (size_t i = 0; i < Tables.size(); ++i) {
            State[i].Boundary = LoadBoundary(Client, Tables[i], JoinPath(Root, Tables[i].Name), timeout);
        }
    }

    void SetSeqno(ui64 seqno) {
        NextSeqno.store(seqno);
    }

    void RunUntil(TInstant stop) {
        StopAt = stop;
        for (size_t user = 0; user < Users; ++user) {
            Queue.push(TEvent{TInstant::Now(), false, user, false, {}, nullptr});
        }
        Thread = std::thread([this] { Loop(); });
        Thread.join();
    }

    void Stop() {
        {
            std::lock_guard lock(Mutex);
        Stopping.store(true);
        }
        Cond.notify_all();
    }

    ui64 Success() const {
        return SuccessCount.load();
    }

    ui64 Errors() const {
        return ErrorCount.load();
    }

    ui64 WindowSuccess() {
        return WindowSuccessCount.exchange(0);
    }

    ui64 WindowErrors() {
        return WindowErrorCount.exchange(0);
    }

private:
    void Loop() {
        while (true) {
            TEvent event;
            {
                std::unique_lock lock(Mutex);
                auto ready = [&] {
                    if (Stopping.load() && InFlight == 0) {
                        return true;
                    }
                    return !Queue.empty() && Queue.top().When <= TInstant::Now();
                };
                if (Queue.empty()) {
                    Cond.wait(lock, ready);
                } else {
                    const TDuration delay = Queue.top().When - TInstant::Now();
                    if (delay > TDuration::Zero()) {
                        Cond.wait_for(lock, std::chrono::microseconds(delay.MicroSeconds()), ready);
                    }
                }
                if (Stopping.load() && InFlight == 0) {
                    return;
                }
                event = Queue.top();
                Queue.pop();
            }
            if (!event.Finish && (Stopping.load() || TInstant::Now() >= StopAt)) {
                continue;
            }
            if (event.Finish) {
                OnFinish(event);
            } else {
                OnStart(event.User);
            }
        }
    }

    void OnStart(size_t user) {
        if (Waiting.count(user)) {
            return;
        }
        TTxPlan plan = BuildTransaction(Tables, State, Root, NextSeqno.fetch_add(1), MaxRecords, Rng[user]);
        if (plan.Busy) {
            NextSeqno.fetch_sub(1);
            Waiting.insert(user);
            return;
        }
        if (plan.Writes.empty()) {
            Schedule(user, Pause);
            return;
        }
        auto shared = std::make_shared<TTxPlan>(std::move(plan));
        const TString script = BuildScript(shared->Writes);
        const TParams params = BindParams(Flatten(shared->Writes));
        {
            std::lock_guard lock(Mutex);
            ++InFlight;
        }
        ExecDataAsync(Client, script, params, ClientTimeout, OperationTimeout, CancelAfter, true)
            .Subscribe([this, user, shared](const NThreading::TFuture<TStatus>& future) {
                TStatus status = future.GetValue();
                TEvent event;
                event.When = TInstant::Now();
                event.Finish = true;
                event.User = user;
                event.Success = status.IsSuccess();
                event.Plan = shared;
                if (!status.IsSuccess()) {
                    event.Error = TStringBuilder() << "error status=" << status.GetStatus()
                        << " issues=" << status.GetIssues().ToString();
                }
                {
                    std::lock_guard lock(Mutex);
                    Queue.push(std::move(event));
                }
                Cond.notify_all();
            });
    }

    void OnFinish(const TEvent& event) {
        ApplyReservations(State, event.Plan->Reservations, event.Success);
        if (event.Success) {
            SuccessCount.fetch_add(1);
            WindowSuccessCount.fetch_add(1);
        } else {
            ErrorCount.fetch_add(1);
            WindowErrorCount.fetch_add(1);
            if (Verbose && !event.Error.empty()) {
                Cerr << event.Error << Endl;
            }
        }
        {
            std::lock_guard lock(Mutex);
            --InFlight;
        }
        WakeWaiting();
        if (!Stopping.load() && TInstant::Now() < StopAt) {
            Schedule(event.User, Pause);
        } else {
            Cond.notify_all();
        }
    }

    void WakeWaiting() {
        TVector<size_t> pending(Waiting.begin(), Waiting.end());
        Waiting.clear();
        for (size_t user : pending) {
            OnStart(user);
        }
    }

    void Schedule(size_t user, TDuration delay) {
        std::lock_guard lock(Mutex);
        Queue.push(TEvent{TInstant::Now() + delay, false, user, false, {}, nullptr});
        Cond.notify_all();
    }

    NQuery::TQueryClient& Client;
    const TString Root;
    const ui64 Users = 1;
    const ui64 MaxRecords = 1;
    const TDuration Pause;
    const TDuration ClientTimeout;
    const TDuration OperationTimeout;
    const TDuration CancelAfter;
    const bool Verbose = false;
    const TVector<TTable> Tables;
    TVector<TTableRuntime> State;
    TVector<std::mt19937_64> Rng;
    std::atomic<ui64> NextSeqno{0};
    std::atomic<ui64> SuccessCount{0};
    std::atomic<ui64> ErrorCount{0};
    std::atomic<ui64> WindowSuccessCount{0};
    std::atomic<ui64> WindowErrorCount{0};
    TInstant StopAt;
    std::atomic<bool> Stopping{false};
    int InFlight = 0;
    THashSet<size_t> Waiting;
    std::mutex Mutex;
    std::condition_variable Cond;
    std::priority_queue<TEvent, std::vector<TEvent>, TEventLater> Queue;
    std::thread Thread;
};

} // namespace

TCommandWorkloadReplicationRun::TCommandWorkloadReplicationRun()
    : TYdbCommand("run", {}, "Run idempotent multi-table write transactions")
{}

void TCommandWorkloadReplicationRun::Config(TConfig& config) {
    TYdbCommand::Config(config);
    config.Opts->AddLongOption("path", "Directory of workload tables in the source database")
        .DefaultValue(Path).StoreResult(&Path);
    config.Opts->AddLongOption('s', "seconds", "Write duration, seconds")
        .DefaultValue(Seconds).StoreResult(&Seconds);
    config.Opts->AddLongOption("users", "Number of parallel users")
        .DefaultValue(Users).StoreResult(&Users);
    config.Opts->AddLongOption("max-records", "Upper bound on rows written into one table inside a transaction. The actual count is random from 1 to this value")
        .DefaultValue(MaxRecords).StoreResult(&MaxRecords);
    config.Opts->AddLongOption("pause", "Per-user pause between transactions. A plain number is milliseconds")
        .DefaultValue(Pause).StoreResult(&Pause);
    config.Opts->AddLongOption("threads", "Client pool threads. 0 selects the adaptive pool")
        .DefaultValue(Threads).StoreResult(&Threads);
    config.Opts->AddLongOption("window", "Rate output window, seconds")
        .DefaultValue(WindowSec).StoreResult(&WindowSec);
    config.Opts->AddLongOption("seed", "Random generator seed. Without this option the seed is the current time")
        .StoreResult(&Seed);
    config.Opts->AddLongOption("client-timeout", "Transport timeout. A plain number is milliseconds")
        .DefaultValue(ClientTimeout).StoreResult(&ClientTimeout);
    config.Opts->AddLongOption("operation-timeout", "Operation timeout. A plain number is milliseconds")
        .DefaultValue(OperationTimeout).StoreResult(&OperationTimeout);
    config.Opts->AddLongOption("cancel-after", "Operation cancel timeout. A plain number is milliseconds")
        .DefaultValue(CancelAfter).StoreResult(&CancelAfter);
}

int TCommandWorkloadReplicationRun::Run(TConfig& config) {
    if (Users == 0) {
        throw TMisuseException() << "Number of users must be positive";
    }
    if (MaxRecords == 0) {
        throw TMisuseException() << "Maximum records must be positive";
    }
    if (WindowSec == 0) {
        throw TMisuseException() << "Window must be positive";
    }
    const TDuration pause = ParseDurationMilliseconds(Pause);
    const TDuration clientTimeout = ParseDurationMilliseconds(ClientTimeout);
    const TDuration operationTimeout = ParseDurationMilliseconds(OperationTimeout);
    const TDuration cancelAfter = ParseDurationMilliseconds(CancelAfter);
    const ui64 seed = Seed.empty() ? TInstant::Now().MicroSeconds() : FromString<ui64>(Seed);

    auto driverConfig = config.CreateDriverConfig();
    if (Threads > 0) {
        driverConfig.SetClientThreadsNum(Threads);
    }
    TDriver driver(driverConfig);
    auto clientSettings = NQuery::TClientSettings().SessionPoolSettings(
        NQuery::TSessionPoolSettings().MaxActiveSessions(Users + 4));
    NQuery::TQueryClient client(driver, clientSettings);

    TRunner runner(client, WorkloadRoot(config.Database, Path), Users, MaxRecords, pause, clientTimeout, operationTimeout, cancelAfter, seed, config.IsVerbose());
    runner.Load(clientTimeout);
    runner.SetSeqno(static_cast<ui64>(TInstant::Now().MicroSeconds()));

    const TInstant start = TInstant::Now();
    const TInstant stop = start + TDuration::Seconds(Seconds);
    std::thread worker([&runner, stop] { runner.RunUntil(stop); });
    TInstant next = start + TDuration::Seconds(WindowSec);
    while (TInstant::Now() < stop) {
        const TInstant now = TInstant::Now();
        const TInstant wake = std::min(next, stop);
        if (wake > now) {
            Sleep(wake - now);
        }
        if (TInstant::Now() >= next) {
            const double elapsed = static_cast<double>((TInstant::Now() - start).MicroSeconds()) / 1000000.0;
            PrintStats(elapsed, static_cast<double>(WindowSec), runner.WindowSuccess(), runner.WindowErrors());
            next += TDuration::Seconds(WindowSec);
        }
    }
    runner.Stop();
    worker.join();
    const double elapsed = static_cast<double>((TInstant::Now() - start).MicroSeconds()) / 1000000.0;
    PrintStats(elapsed, elapsed, runner.Success(), runner.Errors());
    return EXIT_SUCCESS;
}

} // namespace NYdb::NConsoleClient
