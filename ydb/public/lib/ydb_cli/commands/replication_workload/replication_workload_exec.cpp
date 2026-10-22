#include "replication_workload_exec.h"

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/params/params.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status/status.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <algorithm>

namespace NYdb::NConsoleClient::NReplicationWorkload {

TString WorkloadRoot(const TString& database, const TString& path) {
    if (path.StartsWith('/')) {
        return path;
    }
    return JoinPath(database, path);
}

TParams BindParams(const TVector<TBoundParam>& params) {
    TParamsBuilder builder;
    for (const TBoundParam& param : params) {
        const TString name = "$" + param.Name;
        switch (param.Kind) {
            case EParamKind::Bool:
                builder.AddParam(name).Bool(param.Bool).Build();
                break;
            case EParamKind::Int8:
                builder.AddParam(name).Int8(static_cast<int8_t>(param.I)).Build();
                break;
            case EParamKind::Uint8:
                builder.AddParam(name).Uint8(static_cast<uint8_t>(param.U)).Build();
                break;
            case EParamKind::Int16:
                builder.AddParam(name).Int16(static_cast<int16_t>(param.I)).Build();
                break;
            case EParamKind::Uint16:
                builder.AddParam(name).Uint16(static_cast<uint16_t>(param.U)).Build();
                break;
            case EParamKind::Int32:
                builder.AddParam(name).Int32(static_cast<int32_t>(param.I)).Build();
                break;
            case EParamKind::Uint32:
                builder.AddParam(name).Uint32(static_cast<uint32_t>(param.U)).Build();
                break;
            case EParamKind::Int64:
                builder.AddParam(name).Int64(param.I).Build();
                break;
            case EParamKind::Uint64:
                builder.AddParam(name).Uint64(param.U).Build();
                break;
            case EParamKind::Float:
                builder.AddParam(name).Float(static_cast<float>(param.Real)).Build();
                break;
            case EParamKind::Double:
                builder.AddParam(name).Double(param.Real).Build();
                break;
            case EParamKind::Text:
                if (param.SqlType == "String") {
                    builder.AddParam(name).String(param.Text).Build();
                } else if (param.SqlType == "Json") {
                    builder.AddParam(name).Json(param.Text).Build();
                } else if (param.SqlType == "JsonDocument") {
                    builder.AddParam(name).JsonDocument(param.Text).Build();
                } else if (param.SqlType == "Yson") {
                    builder.AddParam(name).Yson(param.Text).Build();
                } else {
                    builder.AddParam(name).Utf8(param.Text).Build();
                }
                break;
            case EParamKind::Decimal:
                builder.AddParam(name).Decimal(TDecimalValue(std::string(param.Text), param.Precision, param.Scale)).Build();
                break;
            case EParamKind::Uuid: {
                TUuidValue uuid(0, 0);
                const size_t n = std::min<size_t>(param.Text.size(), 16);
                for (size_t i = 0; i < n; ++i) {
                    uuid.Buf_.Bytes[i] = param.Text[i];
                }
                builder.AddParam(name).Uuid(uuid).Build();
                break;
            }
            case EParamKind::DateDays:
                builder.AddParam(name).Date(TInstant::Days(param.U)).Build();
                break;
            case EParamKind::Date32Days:
                builder.AddParam(name).Date32(std::chrono::sys_time<TWideDays>(TWideDays(static_cast<int32_t>(param.I)))).Build();
                break;
            case EParamKind::DatetimeSeconds:
                builder.AddParam(name).Datetime(TInstant::Seconds(param.U)).Build();
                break;
            case EParamKind::Datetime64Seconds:
                builder.AddParam(name).Datetime64(std::chrono::sys_time<TWideSeconds>(TWideSeconds(param.I))).Build();
                break;
            case EParamKind::TimestampUs:
                builder.AddParam(name).Timestamp(TInstant::MicroSeconds(param.U)).Build();
                break;
            case EParamKind::Timestamp64Us:
                builder.AddParam(name).Timestamp64(std::chrono::sys_time<TWideMicroseconds>(TWideMicroseconds(param.I))).Build();
                break;
            case EParamKind::IntervalUs:
                builder.AddParam(name).Interval(param.I).Build();
                break;
            case EParamKind::Interval64Us:
                builder.AddParam(name).Interval64(TWideMicroseconds(param.I)).Build();
                break;
        }
    }
    return builder.Build();
}

NQuery::TExecuteQuerySettings MakeQuerySettings(
    const TDuration& clientTimeout,
    const TDuration& operationTimeout,
    const TDuration& cancelAfter)
{
    const TDuration bound = std::min(clientTimeout, std::min(operationTimeout, cancelAfter));
    return NQuery::TExecuteQuerySettings().ClientTimeout(bound);
}

void ExecScheme(NTable::TTableClient& client, const TString& query) {
    auto sessionResult = client.CreateSession().GetValueSync();
    NStatusHelpers::ThrowOnError(sessionResult);
    auto result = sessionResult.GetSession().ExecuteSchemeQuery(query).GetValueSync();
    NStatusHelpers::ThrowOnError(result);
}

NThreading::TFuture<TStatus> ExecDataAsync(
    NQuery::TQueryClient& client,
    const TString& script,
    const TParams& params,
    const TDuration& clientTimeout,
    const TDuration& operationTimeout,
    const TDuration& cancelAfter,
    bool idempotent)
{
    auto settings = MakeQuerySettings(clientTimeout, operationTimeout, cancelAfter);
    auto retry = NQuery::TRetryOperationSettings().Idempotent(idempotent).MaxRetries(10);
    return client.RetryQuery([script, params, settings](NQuery::TSession session) {
        return session.ExecuteQuery(
            script,
            NQuery::TTxControl::BeginTx(NQuery::TTxSettings::SerializableRW()).CommitTx(true),
            params,
            settings).Apply([](const NQuery::TAsyncExecuteQueryResult& future) {
                NQuery::TAsyncExecuteQueryResult copy = future;
                return TStatus(copy.ExtractValue());
            });
    }, retry);
}

TStatus ExecData(
    NQuery::TQueryClient& client,
    const TString& script,
    const TParams& params,
    const TDuration& clientTimeout,
    const TDuration& operationTimeout,
    const TDuration& cancelAfter,
    bool idempotent)
{
    return ExecDataAsync(client, script, params, clientTimeout, operationTimeout, cancelAfter, idempotent).GetValueSync();
}

} // namespace NYdb::NConsoleClient::NReplicationWorkload
