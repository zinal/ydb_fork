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

EPrimitiveType PrimitiveOf(const TBoundParam& param) {
    switch (param.Kind) {
        case EParamKind::Bool:
            return EPrimitiveType::Bool;
        case EParamKind::Int8:
            return EPrimitiveType::Int8;
        case EParamKind::Uint8:
            return EPrimitiveType::Uint8;
        case EParamKind::Int16:
            return EPrimitiveType::Int16;
        case EParamKind::Uint16:
            return EPrimitiveType::Uint16;
        case EParamKind::Int32:
            return EPrimitiveType::Int32;
        case EParamKind::Uint32:
            return EPrimitiveType::Uint32;
        case EParamKind::Int64:
            return EPrimitiveType::Int64;
        case EParamKind::Uint64:
            return EPrimitiveType::Uint64;
        case EParamKind::Float:
            return EPrimitiveType::Float;
        case EParamKind::Double:
            return EPrimitiveType::Double;
        case EParamKind::Text:
            if (param.SqlType == "String") {
                return EPrimitiveType::String;
            }
            if (param.SqlType == "Json") {
                return EPrimitiveType::Json;
            }
            if (param.SqlType == "JsonDocument") {
                return EPrimitiveType::JsonDocument;
            }
            if (param.SqlType == "Yson") {
                return EPrimitiveType::Yson;
            }
            return EPrimitiveType::Utf8;
        case EParamKind::Decimal:
            return EPrimitiveType::Utf8;
        case EParamKind::Uuid:
            return EPrimitiveType::Uuid;
        case EParamKind::DateDays:
            return EPrimitiveType::Date;
        case EParamKind::Date32Days:
            return EPrimitiveType::Date32;
        case EParamKind::DatetimeSeconds:
            return EPrimitiveType::Datetime;
        case EParamKind::Datetime64Seconds:
            return EPrimitiveType::Datetime64;
        case EParamKind::TimestampUs:
            return EPrimitiveType::Timestamp;
        case EParamKind::Timestamp64Us:
            return EPrimitiveType::Timestamp64;
        case EParamKind::IntervalUs:
            return EPrimitiveType::Interval;
        case EParamKind::Interval64Us:
            return EPrimitiveType::Interval64;
    }
    return EPrimitiveType::Utf8;
}

void FillBound(TValueBuilder& builder, const TBoundParam& param) {
    switch (param.Kind) {
        case EParamKind::Bool:
            builder.Bool(param.Bool);
            break;
        case EParamKind::Int8:
            builder.Int8(static_cast<int8_t>(param.I));
            break;
        case EParamKind::Uint8:
            builder.Uint8(static_cast<uint8_t>(param.U));
            break;
        case EParamKind::Int16:
            builder.Int16(static_cast<int16_t>(param.I));
            break;
        case EParamKind::Uint16:
            builder.Uint16(static_cast<uint16_t>(param.U));
            break;
        case EParamKind::Int32:
            builder.Int32(static_cast<int32_t>(param.I));
            break;
        case EParamKind::Uint32:
            builder.Uint32(static_cast<uint32_t>(param.U));
            break;
        case EParamKind::Int64:
            builder.Int64(param.I);
            break;
        case EParamKind::Uint64:
            builder.Uint64(param.U);
            break;
        case EParamKind::Float:
            builder.Float(static_cast<float>(param.Real));
            break;
        case EParamKind::Double:
            builder.Double(param.Real);
            break;
        case EParamKind::Text:
            if (param.SqlType == "String") {
                builder.String(param.Text);
            } else if (param.SqlType == "Json") {
                builder.Json(param.Text);
            } else if (param.SqlType == "JsonDocument") {
                builder.JsonDocument(param.Text);
            } else if (param.SqlType == "Yson") {
                builder.Yson(param.Text);
            } else {
                builder.Utf8(param.Text);
            }
            break;
        case EParamKind::Decimal:
            builder.Decimal(TDecimalValue(std::string(param.Text), param.Precision, param.Scale));
            break;
        case EParamKind::Uuid: {
            TUuidValue uuid(0, 0);
            const size_t n = std::min<size_t>(param.Text.size(), 16);
            for (size_t i = 0; i < n; ++i) {
                uuid.Buf_.Bytes[i] = param.Text[i];
            }
            builder.Uuid(uuid);
            break;
        }
        case EParamKind::DateDays:
            builder.Date(TInstant::Days(param.U));
            break;
        case EParamKind::Date32Days:
            builder.Date32(std::chrono::sys_time<TWideDays>(TWideDays(static_cast<int32_t>(param.I))));
            break;
        case EParamKind::DatetimeSeconds:
            builder.Datetime(TInstant::Seconds(param.U));
            break;
        case EParamKind::Datetime64Seconds:
            builder.Datetime64(std::chrono::sys_time<TWideSeconds>(TWideSeconds(param.I)));
            break;
        case EParamKind::TimestampUs:
            builder.Timestamp(TInstant::MicroSeconds(param.U));
            break;
        case EParamKind::Timestamp64Us:
            builder.Timestamp64(std::chrono::sys_time<TWideMicroseconds>(TWideMicroseconds(param.I)));
            break;
        case EParamKind::IntervalUs:
            builder.Interval(param.I);
            break;
        case EParamKind::Interval64Us:
            builder.Interval64(TWideMicroseconds(param.I));
            break;
    }
}

TValue NullValue(const TBoundParam& param) {
    TValueBuilder builder;
    if (param.Kind == EParamKind::Decimal) {
        builder.EmptyOptional(TTypeBuilder().Decimal(TDecimalType(param.Precision, param.Scale)).Build());
    } else {
        builder.EmptyOptional(PrimitiveOf(param));
    }
    return builder.Build();
}

TValue MakeBoundValue(const TBoundParam& param) {
    if (param.Null) {
        return NullValue(param);
    }
    TValueBuilder builder;
    if (param.Optional) {
        builder.BeginOptional();
    }
    FillBound(builder, param);
    if (param.Optional) {
        builder.EndOptional();
    }
    return builder.Build();
}

TParams BindParams(const TVector<TBoundParam>& params) {
    TParamsBuilder builder;
    for (const TBoundParam& param : params) {
        builder.AddParam("$" + param.Name, MakeBoundValue(param));
    }
    return builder.Build();
}

TParams BindImportRows(const TVector<TVector<TBoundParam>>& rows) {
    TValueBuilder list;
    list.BeginList();
    for (const TVector<TBoundParam>& row : rows) {
        list.AddListItem().BeginStruct();
        for (const TBoundParam& param : row) {
            list.AddMember(std::string(param.Name), MakeBoundValue(param));
        }
        list.EndStruct();
    }
    list.EndList();
    return TParamsBuilder().AddParam("$input", list.Build()).Build();
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
