#pragma once

#include <util/generic/string.h>
#include <util/generic/vector.h>
#include <util/system/types.h>

#include <optional>
#include <random>

namespace NYdb::NConsoleClient::NReplicationWorkload {

enum class EColType {
    Bool,
    Int8,
    Uint8,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Int64,
    Uint64,
    Float,
    Double,
    Decimal22_9,
    Decimal1_0,
    Decimal5_2,
    Decimal35_0,
    Decimal35_9,
    Decimal35_35,
    Utf8,
    String,
    Json,
    JsonDocument,
    Yson,
    Uuid,
    Date,
    Date32,
    Datetime,
    Datetime64,
    Timestamp,
    Timestamp64,
    Interval,
    Interval64,
};

enum class EFill {
    None,
    Narrow,
    Medium,
    Wide,
};

enum class EKeyKind {
    Bool,
    Int8,
    Uint8,
    Int16,
    Uint16,
    Date,
    Decimal1_0,
    Int32,
    Int64,
    Uint32,
    Uint64,
    Decimal22_9,
    Decimal35_0,
    Date32,
    Datetime,
    Datetime64,
    Timestamp,
    Timestamp64,
    Interval,
    Interval64,
    Uuid,
    String,
    Utf8,
    WideId,
};

enum class ETextType {
    None,
    Utf8,
    String,
    Json,
    JsonDocument,
    Yson,
};

struct TColumn {
    TString Name;
    EColType Type = EColType::Uint64;
    EFill Fill = EFill::None;
    bool Key = false;
};

struct TTable {
    TString Name;
    EKeyKind Key = EKeyKind::Uint64;
    bool Composite = false;
    bool HashKey = false;
    bool Wide = false;
    ETextType WideText = ETextType::None;
    TVector<TColumn> Columns;
};

enum class EParamKind {
    Bool,
    Int8,
    Uint8,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Int64,
    Uint64,
    Float,
    Double,
    Text,
    Decimal,
    Uuid,
    DateDays,
    Date32Days,
    DatetimeSeconds,
    Datetime64Seconds,
    TimestampUs,
    Timestamp64Us,
    IntervalUs,
    Interval64Us,
};

struct TBoundParam {
    TString Name;
    TString SqlType;
    EParamKind Kind = EParamKind::Uint64;
    bool Bool = false;
    i64 I = 0;
    ui64 U = 0;
    double Real = 0;
    TString Text;
    ui8 Precision = 0;
    ui8 Scale = 0;
};

struct TRowWrite {
    bool Update = false;
    TString Statement;
    TVector<TBoundParam> Params;
};

struct TReservation {
    size_t TableIndex = 0;
    ui64 First = 0;
    ui64 Count = 0;
};

struct TTableRuntime {
    i64 Boundary = -1;
    bool InsertBusy = false;
};

struct TTxPlan {
    bool Busy = false;
    ui64 Seqno = 0;
    TVector<TRowWrite> Writes;
    TVector<TReservation> Reservations;
    TVector<size_t> InsertTables;
};

const TVector<TTable>& AllTables();

TString SqlType(EColType type);
TString QuotePath(const TString& path);
TString JoinPath(const TString& left, const TString& right);

TString CreateTableQuery(const TString& fullName, const TTable& table, ui64 minPartitions, ui64 maxPartitions);
TVector<TString> AlterIndexQueries(const TString& fullName, const TTable& table, ui64 minPartitions, ui64 maxPartitions);
bool UsesUniformPartitions(const TTable& table);

TString HashBytes(ui64 n);
TString HashHex(ui64 n);
TString MakeText(EColType type, EFill fill, std::mt19937_64& rng);
TBoundParam MakeValue(const TColumn& column, std::mt19937_64& rng);
TVector<TBoundParam> MakeKeyParams(const TTable& table, ui64 n, const TString& namePrefix);

TRowWrite MakeInsertWrite(
    const TTable& table,
    const TString& fullName,
    ui64 n,
    ui64 seqno,
    const TString& prefix,
    std::mt19937_64& rng);

TString BuildScript(const TVector<TRowWrite>& writes);

bool TryReserve(TVector<TTableRuntime>& state, const TVector<std::pair<size_t, ui64>>& inserts, TVector<TReservation>& out);
void ApplyReservations(TVector<TTableRuntime>& state, const TVector<TReservation>& reservations, bool success);

TTxPlan BuildTransaction(
    const TVector<TTable>& tables,
    TVector<TTableRuntime>& state,
    const TString& pathPrefix,
    ui64 seqno,
    ui64 maxRecords,
    std::mt19937_64& rng);

ui64 ExistingRows(i64 boundary);

} // namespace NYdb::NConsoleClient::NReplicationWorkload
