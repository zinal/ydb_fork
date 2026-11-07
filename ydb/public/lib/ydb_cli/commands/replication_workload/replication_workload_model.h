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
    bool PkSeqHashPrefix = false;
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
    bool Optional = false;
    bool Null = false;
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
// Row count for a single-column key reads the seqno index. A composite key
// reads the greatest seq instead, because the boundary is that value.
TString BoundaryQuery(const TTable& table, const TString& fullName);

ui64 HashSeqno(ui64 seqno);
// Hash of the key columns (pk, seq) for pk_bool and pk_decimal_narrow.
// pk is 0 or 1 for Bool, and the two's complement of the Decimal(1, 0) value.
ui64 HashPkSeq(ui64 pk, ui64 seq);
TString HashBytes(ui64 n);
TString HashHex(ui64 n);
TString MakeText(EColType type, EFill fill, std::mt19937_64& rng);

// Among params with Optional set, with probability percent/100 mark exactly one Null.
void MaybeNullValue(TVector<TBoundParam>& params, std::mt19937_64& rng, int percent);
TBoundParam MakeValue(const TColumn& column, std::mt19937_64& rng);
TVector<TBoundParam> MakeKeyParams(const TTable& table, ui64 n, const TString& namePrefix);

TRowWrite MakeInsertWrite(
    const TTable& table,
    const TString& fullName,
    ui64 n,
    ui64 seqno,
    std::mt19937_64& rng);

TString InsertQuery(const TString& fullName, const TTable& table);
// `assigned` includes seqno and the other value columns of one update shape.
TString UpdateQuery(const TString& fullName, const TTable& table, const TVector<const TColumn*>& assigned);
// Five fixed sets of value columns for UPDATE, not including seqno.
// seqno is always assigned as well.
// Ordinary tables use value-column order: the first column, the second,
// the first three, the last three, and every value column.
// Wide tables: c_wide; the first ordinary column; c_medium plus the first
// three ordinary columns; the first four ordinary columns; every value column.
// An ordinary column has no text fill. The two middle wide shapes therefore
// assign five columns once seqno is included.
TVector<TVector<size_t>> UpdateValueColumnSets(const TTable& table);

TString ImportQuery(const TString& fullName, const TTable& table);
TVector<TVector<TBoundParam>> MakeImportRows(
    const TTable& table,
    ui64 begin,
    ui64 count,
    std::mt19937_64& rng);

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
