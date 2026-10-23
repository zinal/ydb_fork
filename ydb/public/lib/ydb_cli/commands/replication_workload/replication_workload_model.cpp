#include "replication_workload_model.h"
#include "replication_workload_text.h"

#include <openssl/sha.h>

#include <util/generic/hash_set.h>
#include <util/stream/str.h>
#include <util/string/builder.h>
#include <util/string/cast.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace NYdb::NConsoleClient::NReplicationWorkload {

TString HashBytes(ui64 n);
TString HashHex(ui64 n);
ui64 HashSeqno(ui64 seqno);

namespace {

constexpr ui32 DateDomain = 49673;
constexpr ui64 MaxDatetime = 86400ull * 49673ull;
constexpr ui64 MaxTimestamp = 86400000000ull * 49673ull;
constexpr i32 MinDate32 = -53375809;
constexpr i32 MaxDate32 = 53375807;
constexpr i64 MinDatetime64 = -4611669897600ll;
constexpr i64 MaxDatetime64 = 4611669811199ll;
constexpr i64 MinTimestamp64 = -4611669897600000000ll;
constexpr i64 MaxTimestamp64 = 4611669811199999999ll;

TColumn Col(const char* name, EColType type, EFill fill = EFill::None, bool key = false) {
    return TColumn{name, type, fill, key};
}

TVector<TColumn> SingleColumns(EColType pkType) {
    return {
        Col("pk", pkType, EFill::None, true),
        Col("seqno", EColType::Uint64),
        Col("c_bool", EColType::Bool),
        Col("c_int64", EColType::Int64),
        Col("c_double", EColType::Double),
        Col("c_decimal", EColType::Decimal22_9),
        Col("c_utf8", EColType::Utf8, EFill::Narrow),
        Col("c_string", EColType::String, EFill::Narrow),
        Col("c_json", EColType::Json, EFill::Narrow),
        Col("c_timestamp", EColType::Timestamp),
    };
}

TVector<TColumn> CompositeColumns(EColType pkType, bool seqnoHashPrefix) {
    TVector<TColumn> columns;
    if (seqnoHashPrefix) {
        columns.push_back(Col("seqno_hash", EColType::Uint64, EFill::None, true));
    }
    columns.push_back(Col("pk", pkType, EFill::None, true));
    columns.push_back(Col("seq", EColType::Uint64, EFill::None, true));
    columns.push_back(Col("seqno", EColType::Uint64));
    columns.push_back(Col("c_float", EColType::Float));
    columns.push_back(Col("c_int32", EColType::Int32));
    columns.push_back(Col("c_json_doc", EColType::JsonDocument, EFill::Narrow));
    columns.push_back(Col("c_yson", EColType::Yson, EFill::Narrow));
    columns.push_back(Col("c_uuid", EColType::Uuid));
    columns.push_back(Col("c_date32", EColType::Date32));
    columns.push_back(Col("c_interval64", EColType::Interval64));
    return columns;
}

TVector<TColumn> WideColumns(EColType textType) {
    TVector<TColumn> columns = {
        Col("id", EColType::Uint64, EFill::None, true),
        Col("seqno", EColType::Uint64),
        Col("c_bool", EColType::Bool),
        Col("c_int8", EColType::Int8),
        Col("c_int16", EColType::Int16),
        Col("c_int32", EColType::Int32),
        Col("c_int64", EColType::Int64),
        Col("c_uint8", EColType::Uint8),
        Col("c_uint16", EColType::Uint16),
        Col("c_uint32", EColType::Uint32),
        Col("c_float", EColType::Float),
        Col("c_double", EColType::Double),
        Col("c_decimal", EColType::Decimal22_9),
        Col("c_decimal_narrow", EColType::Decimal1_0),
        Col("c_decimal_short", EColType::Decimal5_2),
        Col("c_decimal_wide", EColType::Decimal35_0),
        Col("c_decimal_wide_scale", EColType::Decimal35_9),
        Col("c_decimal_max_scale", EColType::Decimal35_35),
        Col("c_uuid", EColType::Uuid),
        Col("c_date", EColType::Date),
        Col("c_date32", EColType::Date32),
        Col("c_datetime", EColType::Datetime),
        Col("c_datetime64", EColType::Datetime64),
        Col("c_timestamp", EColType::Timestamp),
        Col("c_timestamp64", EColType::Timestamp64),
        Col("c_interval", EColType::Interval),
        Col("c_interval64", EColType::Interval64),
        Col("c_utf8", EColType::Utf8, EFill::Narrow),
        Col("c_string", EColType::String, EFill::Narrow),
        Col("c_json", EColType::Json, EFill::Narrow),
        Col("c_medium", textType, EFill::Medium),
        Col("c_wide", textType, EFill::Wide),
    };
    return columns;
}

TTable MakeOrdinary(const char* name, EKeyKind key, EColType pkType, bool composite, bool hash, bool seqnoHashPrefix = false) {
    TTable table;
    table.Name = name;
    table.Key = key;
    table.Composite = composite;
    table.HashKey = hash;
    table.SeqnoHashPrefix = seqnoHashPrefix;
    table.Columns = composite ? CompositeColumns(pkType, seqnoHashPrefix) : SingleColumns(pkType);
    return table;
}

TTable MakeWide(const char* name, ETextType text, EColType textType) {
    TTable table;
    table.Name = name;
    table.Key = EKeyKind::WideId;
    table.Wide = true;
    table.WideText = text;
    table.Columns = WideColumns(textType);
    return table;
}

ui64 UniformSize(std::mt19937_64& rng, ui64 lo, ui64 hi) {
    std::uniform_int_distribution<ui64> dist(lo, hi);
    return dist(rng);
}

TString JsonOfSize(ui64 size, std::mt19937_64& rng) {
    if (size < 2) {
        size = 2;
    }
    if (size == 2) {
        return "{}";
    }
    if (size == 3) {
        return "[0]";
    }
    if (size == 4) {
        return "null";
    }
    if (size == 5) {
        return "false";
    }
    if (size == 6) {
        return "\"abcd\"";
    }
    if (size == 7) {
        return "\"abcde\"";
    }
    return TString("{\"v\":\"") + TpchText(size - 8, rng()) + "\"}";
}

TString YsonOfSize(ui64 size, std::mt19937_64& rng) {
    if (size < 1) {
        size = 1;
    }
    if (size == 1) {
        return "#";
    }
    if (size == 2) {
        return "[]";
    }
    if (size == 3) {
        return "[1]";
    }
    if (size == 4) {
        return "[12]";
    }
    if (size == 5) {
        return "{a=1}";
    }
    return TString("{v=\"") + TpchText(size - 6, rng()) + "\"}";
}

i64 RandomI64(std::mt19937_64& rng, i64 lo, i64 hi) {
    std::uniform_int_distribution<i64> dist(lo, hi);
    return dist(rng);
}

TString DecimalString(std::mt19937_64& rng, ui8 precision, ui8 scale) {
    const ui32 integerDigits = precision - scale;
    TString result;
    if (integerDigits == 0) {
        result.push_back('0');
    } else {
        std::uniform_int_distribution<int> first(1, 9);
        std::uniform_int_distribution<int> rest(0, 9);
        result.push_back(static_cast<char>('0' + first(rng)));
        for (ui32 i = 1; i < integerDigits; ++i) {
            result.push_back(static_cast<char>('0' + rest(rng)));
        }
    }
    if (scale == 0) {
        return result;
    }
    result.push_back('.');
    std::uniform_int_distribution<int> digit(0, 9);
    for (ui8 i = 0; i < scale; ++i) {
        result.push_back(static_cast<char>('0' + digit(rng)));
    }
    return result;
}

void SetDecimal(TBoundParam& param, EColType type, const TString& text) {
    param.Kind = EParamKind::Decimal;
    param.Text = text;
    switch (type) {
        case EColType::Decimal22_9:
            param.Precision = 22;
            param.Scale = 9;
            param.SqlType = "Decimal(22, 9)";
            break;
        case EColType::Decimal1_0:
            param.Precision = 1;
            param.Scale = 0;
            param.SqlType = "Decimal(1, 0)";
            break;
        case EColType::Decimal5_2:
            param.Precision = 5;
            param.Scale = 2;
            param.SqlType = "Decimal(5, 2)";
            break;
        case EColType::Decimal35_0:
            param.Precision = 35;
            param.Scale = 0;
            param.SqlType = "Decimal(35, 0)";
            break;
        case EColType::Decimal35_9:
            param.Precision = 35;
            param.Scale = 9;
            param.SqlType = "Decimal(35, 9)";
            break;
        case EColType::Decimal35_35:
            param.Precision = 35;
            param.Scale = 35;
            param.SqlType = "Decimal(35, 35)";
            break;
        default:
            break;
    }
}

constexpr ui64 Decimal22Span = 10000000000000ull;

ui64 ReverseBits64(ui64 x) {
    x = ((x & 0x5555555555555555ull) << 1) | ((x >> 1) & 0x5555555555555555ull);
    x = ((x & 0x3333333333333333ull) << 2) | ((x >> 2) & 0x3333333333333333ull);
    x = ((x & 0x0f0f0f0f0f0f0f0full) << 4) | ((x >> 4) & 0x0f0f0f0f0f0f0f0full);
    x = ((x & 0x00ff00ff00ff00ffull) << 8) | ((x >> 8) & 0x00ff00ff00ff00ffull);
    x = ((x & 0x0000ffff0000ffffull) << 16) | ((x >> 16) & 0x0000ffff0000ffffull);
    return (x << 32) | (x >> 32);
}

ui32 ReverseBits32(ui32 x) {
    return static_cast<ui32>(ReverseBits64(x) >> 32);
}

ui64 Gcd(ui64 a, ui64 b) {
    while (b != 0) {
        const ui64 next = a % b;
        a = b;
        b = next;
    }
    return a;
}

ui64 MulMod(ui64 a, ui64 b, ui64 mod) {
    return static_cast<ui64>((static_cast<unsigned __int128>(a) * b) % mod);
}

ui64 ModInverse(ui64 value, ui64 mod) {
    __int128 t = 0;
    __int128 nextT = 1;
    __int128 r = mod;
    __int128 nextR = value;
    while (nextR != 0) {
        const __int128 quotient = r / nextR;
        const __int128 savedT = t;
        t = nextT;
        nextT = savedT - quotient * nextT;
        const __int128 savedR = r;
        r = nextR;
        nextR = savedR - quotient * nextR;
    }
    if (t < 0) {
        t += mod;
    }
    return static_cast<ui64>(t);
}

struct TPerm {
    i64 Lo = 0;
    ui64 Span = 1;
    ui64 Mul = 1;
    ui64 Inv = 1;
};

TPerm MakePerm(i64 lo, i64 hi) {
    const ui64 span = static_cast<ui64>(static_cast<__int128>(hi) - static_cast<__int128>(lo)) + 1;
    ui64 mul = 3;
    while (Gcd(mul, span) != 1) {
        mul += 2;
    }
    return TPerm{lo, span, mul, ModInverse(mul, span)};
}

i64 Scatter(const TPerm& perm, ui64 n) {
    if (n >= perm.Span) {
        ythrow yexception() << "row number " << n << " does not fit the key range";
    }
    const ui64 mixed = MulMod(n, perm.Mul, perm.Span);
    return static_cast<i64>(static_cast<__int128>(perm.Lo) + static_cast<__int128>(mixed));
}

const TPerm& Date32Perm() {
    static const TPerm perm = MakePerm(MinDate32, MaxDate32);
    return perm;
}

const TPerm& DatetimePerm() {
    static const TPerm perm = MakePerm(0, static_cast<i64>(MaxDatetime) - 1);
    return perm;
}

const TPerm& Datetime64Perm() {
    static const TPerm perm = MakePerm(MinDatetime64, MaxDatetime64);
    return perm;
}

const TPerm& TimestampPerm() {
    static const TPerm perm = MakePerm(0, static_cast<i64>(MaxTimestamp) - 1);
    return perm;
}

const TPerm& Timestamp64Perm() {
    static const TPerm perm = MakePerm(MinTimestamp64, MaxTimestamp64);
    return perm;
}

const TPerm& IntervalPerm() {
    static const TPerm perm = MakePerm(-static_cast<i64>(MaxTimestamp) + 1, static_cast<i64>(MaxTimestamp) - 1);
    return perm;
}

const TPerm& Interval64Perm() {
    constexpr i64 maxInterval64 = 9223339708799999999ll;
    static const TPerm perm = MakePerm(-maxInterval64, maxInterval64);
    return perm;
}

const TPerm& Decimal22Perm() {
    static const TPerm perm = MakePerm(0, static_cast<i64>(Decimal22Span) - 1);
    return perm;
}

const TColumn& PkColumn(const TTable& table) {
    for (const TColumn& column : table.Columns) {
        if (column.Name == "pk" || column.Name == "id") {
            return column;
        }
    }
    return table.Columns[0];
}

TString UuidBytes(ui64 n) {
    const ui64 mixed = HashSeqno(n);
    TString bytes(16, '\0');
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<char>((mixed >> (56 - 8 * i)) & 0xff);
        bytes[8 + i] = static_cast<char>((n >> (56 - 8 * i)) & 0xff);
    }
    return bytes;
}

TBoundParam KeyParam(const TTable& table, ui64 n, const TString& name) {
    TBoundParam param;
    param.Name = name;
    param.SqlType = SqlType(PkColumn(table).Type);
    switch (table.Key) {
        case EKeyKind::Bool:
            param.Kind = EParamKind::Bool;
            param.Bool = (n % 2) == 1;
            break;
        case EKeyKind::Int8:
            param.Kind = EParamKind::Int8;
            param.I = static_cast<i64>(n % 256) - 128;
            break;
        case EKeyKind::Uint8:
            param.Kind = EParamKind::Uint8;
            param.U = n % 256;
            break;
        case EKeyKind::Int16: {
            const ui64 mod = n % 65536;
            param.Kind = EParamKind::Int16;
            param.I = mod >= 32768 ? static_cast<i64>(mod) - 65536 : static_cast<i64>(mod);
            break;
        }
        case EKeyKind::Uint16:
            param.Kind = EParamKind::Uint16;
            param.U = n % 65536;
            break;
        case EKeyKind::Date:
            param.Kind = EParamKind::DateDays;
            param.U = n % DateDomain;
            break;
        case EKeyKind::Decimal1_0:
            SetDecimal(param, EColType::Decimal1_0, ToString(static_cast<i64>(n % 19) - 9));
            break;
        case EKeyKind::Int32:
            param.Kind = EParamKind::Int32;
            param.I = static_cast<i32>(ReverseBits32(static_cast<ui32>(n)));
            break;
        case EKeyKind::Int64:
            param.Kind = EParamKind::Int64;
            param.I = static_cast<i64>(ReverseBits64(n));
            break;
        case EKeyKind::Uint32:
            param.Kind = EParamKind::Uint32;
            param.U = ReverseBits32(static_cast<ui32>(n));
            break;
        case EKeyKind::Uint64:
        case EKeyKind::WideId:
            param.Kind = EParamKind::Uint64;
            param.U = ReverseBits64(n);
            break;
        case EKeyKind::Decimal22_9:
            SetDecimal(param, EColType::Decimal22_9, ToString(Scatter(Decimal22Perm(), n)) + ".000000000");
            break;
        case EKeyKind::Decimal35_0:
            SetDecimal(param, EColType::Decimal35_0, ToString(ReverseBits64(n)));
            break;
        case EKeyKind::Date32:
            param.Kind = EParamKind::Date32Days;
            param.I = Scatter(Date32Perm(), n);
            break;
        case EKeyKind::Datetime:
            param.Kind = EParamKind::DatetimeSeconds;
            param.U = static_cast<ui64>(Scatter(DatetimePerm(), n));
            break;
        case EKeyKind::Datetime64:
            param.Kind = EParamKind::Datetime64Seconds;
            param.I = Scatter(Datetime64Perm(), n);
            break;
        case EKeyKind::Timestamp:
            param.Kind = EParamKind::TimestampUs;
            param.U = static_cast<ui64>(Scatter(TimestampPerm(), n));
            break;
        case EKeyKind::Timestamp64:
            param.Kind = EParamKind::Timestamp64Us;
            param.I = Scatter(Timestamp64Perm(), n);
            break;
        case EKeyKind::Interval:
            param.Kind = EParamKind::IntervalUs;
            param.I = Scatter(IntervalPerm(), n);
            break;
        case EKeyKind::Interval64:
            param.Kind = EParamKind::Interval64Us;
            param.I = Scatter(Interval64Perm(), n);
            break;
        case EKeyKind::Uuid:
            param.Kind = EParamKind::Uuid;
            param.Text = UuidBytes(n);
            break;
        case EKeyKind::String:
            param.Kind = EParamKind::Text;
            param.Text = HashBytes(n);
            break;
        case EKeyKind::Utf8:
            param.Kind = EParamKind::Text;
            param.Text = HashHex(n);
            break;
    }
    return param;
}

TString ColumnList(const TVector<TColumn>& columns) {
    TStringBuilder out;
    for (size_t i = 0; i < columns.size(); ++i) {
        if (i) {
            out << ", ";
        }
        out << columns[i].Name;
    }
    return out;
}

TString PartitionSettings(ui64 minPartitions, ui64 maxPartitions) {
    TStringBuilder out;
    out << "AUTO_PARTITIONING_BY_SIZE = ENABLED, "
        << "AUTO_PARTITIONING_BY_LOAD = ENABLED, "
        << "AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = " << minPartitions << ", "
        << "AUTO_PARTITIONING_MAX_PARTITIONS_COUNT = " << maxPartitions;
    return out;
}

TVector<size_t> ValueColumnsExceptSeqno(const TTable& table) {
    TVector<size_t> indexes;
    for (size_t i = 0; i < table.Columns.size(); ++i) {
        if (!table.Columns[i].Key && table.Columns[i].Name != "seqno") {
            indexes.push_back(i);
        }
    }
    return indexes;
}

} // namespace

TString SqlType(EColType type) {
    switch (type) {
        case EColType::Bool: return "Bool";
        case EColType::Int8: return "Int8";
        case EColType::Uint8: return "Uint8";
        case EColType::Int16: return "Int16";
        case EColType::Uint16: return "Uint16";
        case EColType::Int32: return "Int32";
        case EColType::Uint32: return "Uint32";
        case EColType::Int64: return "Int64";
        case EColType::Uint64: return "Uint64";
        case EColType::Float: return "Float";
        case EColType::Double: return "Double";
        case EColType::Decimal22_9: return "Decimal(22, 9)";
        case EColType::Decimal1_0: return "Decimal(1, 0)";
        case EColType::Decimal5_2: return "Decimal(5, 2)";
        case EColType::Decimal35_0: return "Decimal(35, 0)";
        case EColType::Decimal35_9: return "Decimal(35, 9)";
        case EColType::Decimal35_35: return "Decimal(35, 35)";
        case EColType::Utf8: return "Utf8";
        case EColType::String: return "String";
        case EColType::Json: return "Json";
        case EColType::JsonDocument: return "JsonDocument";
        case EColType::Yson: return "Yson";
        case EColType::Uuid: return "Uuid";
        case EColType::Date: return "Date";
        case EColType::Date32: return "Date32";
        case EColType::Datetime: return "Datetime";
        case EColType::Datetime64: return "Datetime64";
        case EColType::Timestamp: return "Timestamp";
        case EColType::Timestamp64: return "Timestamp64";
        case EColType::Interval: return "Interval";
        case EColType::Interval64: return "Interval64";
    }
    return "Uint64";
}

TString QuotePath(const TString& path) {
    return "`" + path + "`";
}

TString JoinPath(const TString& left, const TString& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    if (left.EndsWith('/')) {
        return left + right;
    }
    return left + "/" + right;
}

const TVector<TTable>& AllTables() {
    static const TVector<TTable> tables = {
        MakeOrdinary("pk_bool", EKeyKind::Bool, EColType::Bool, true, false, true),
        MakeOrdinary("pk_int8", EKeyKind::Int8, EColType::Int8, true, false),
        MakeOrdinary("pk_uint8", EKeyKind::Uint8, EColType::Uint8, true, false),
        MakeOrdinary("pk_int16", EKeyKind::Int16, EColType::Int16, true, false),
        MakeOrdinary("pk_uint16", EKeyKind::Uint16, EColType::Uint16, true, false),
        MakeOrdinary("pk_date", EKeyKind::Date, EColType::Date, true, false),
        MakeOrdinary("pk_decimal_narrow", EKeyKind::Decimal1_0, EColType::Decimal1_0, true, false, true),
        MakeOrdinary("pk_int32", EKeyKind::Int32, EColType::Int32, false, false),
        MakeOrdinary("pk_int64", EKeyKind::Int64, EColType::Int64, false, false),
        MakeOrdinary("pk_uint32", EKeyKind::Uint32, EColType::Uint32, false, false),
        MakeOrdinary("pk_uint64", EKeyKind::Uint64, EColType::Uint64, false, false),
        MakeOrdinary("pk_decimal", EKeyKind::Decimal22_9, EColType::Decimal22_9, false, false),
        MakeOrdinary("pk_decimal_wide", EKeyKind::Decimal35_0, EColType::Decimal35_0, false, false),
        MakeOrdinary("pk_date32", EKeyKind::Date32, EColType::Date32, false, false),
        MakeOrdinary("pk_datetime", EKeyKind::Datetime, EColType::Datetime, false, false),
        MakeOrdinary("pk_datetime64", EKeyKind::Datetime64, EColType::Datetime64, false, false),
        MakeOrdinary("pk_timestamp", EKeyKind::Timestamp, EColType::Timestamp, false, false),
        MakeOrdinary("pk_timestamp64", EKeyKind::Timestamp64, EColType::Timestamp64, false, false),
        MakeOrdinary("pk_interval", EKeyKind::Interval, EColType::Interval, false, false),
        MakeOrdinary("pk_interval64", EKeyKind::Interval64, EColType::Interval64, false, false),
        MakeOrdinary("pk_uuid", EKeyKind::Uuid, EColType::Uuid, false, false),
        MakeOrdinary("pk_string", EKeyKind::String, EColType::String, false, true),
        MakeOrdinary("pk_utf8", EKeyKind::Utf8, EColType::Utf8, false, true),
        MakeWide("wide_utf8", ETextType::Utf8, EColType::Utf8),
        MakeWide("wide_string", ETextType::String, EColType::String),
        MakeWide("wide_json", ETextType::Json, EColType::Json),
        MakeWide("wide_json_doc", ETextType::JsonDocument, EColType::JsonDocument),
        MakeWide("wide_yson", ETextType::Yson, EColType::Yson),
    };
    return tables;
}

TString CreateTableQuery(const TString& fullName, const TTable& table, ui64 minPartitions, ui64 maxPartitions) {
    TStringBuilder out;
    out << "CREATE TABLE " << QuotePath(fullName) << " (";
    for (const TColumn& column : table.Columns) {
        out << column.Name << " " << SqlType(column.Type);
        if (column.Key || column.Name == "seqno") {
            out << " NOT NULL";
        }
        out << ", ";
    }
    out << "PRIMARY KEY (";
    bool firstKey = true;
    for (const TColumn& column : table.Columns) {
        if (!column.Key) {
            continue;
        }
        if (!firstKey) {
            out << ", ";
        }
        out << column.Name;
        firstKey = false;
    }
    out << "), INDEX seqno_idx GLOBAL SYNC ON (seqno)";
    if (table.Composite) {
        out << ", INDEX seq_idx GLOBAL SYNC ON (seq)";
    }
    out << ") WITH (" << PartitionSettings(minPartitions, maxPartitions) << ")";
    return out;
}

TVector<TString> AlterIndexQueries(const TString& fullName, const TTable& table, ui64 minPartitions, ui64 maxPartitions) {
    const TString settings = TStringBuilder()
        << "AUTO_PARTITIONING_BY_SIZE = ENABLED, "
        << "AUTO_PARTITIONING_BY_LOAD = ENABLED, "
        << "AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = " << minPartitions << ", "
        << "AUTO_PARTITIONING_MAX_PARTITIONS_COUNT = " << maxPartitions;
    TVector<TString> queries;
    queries.push_back(TStringBuilder() << "ALTER TABLE " << QuotePath(fullName)
        << " ALTER INDEX `seqno_idx` SET (" << settings << ")");
    if (table.Composite) {
        queries.push_back(TStringBuilder() << "ALTER TABLE " << QuotePath(fullName)
            << " ALTER INDEX `seq_idx` SET (" << settings << ")");
    }
    return queries;
}

ui64 HashSeqno(ui64 seqno) {
    ui64 x = seqno + 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

TString HashBytes(ui64 n) {
    unsigned char input[8];
    for (int i = 0; i < 8; ++i) {
        input[i] = static_cast<unsigned char>((n >> (8 * i)) & 0xff);
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(input, sizeof(input), digest);
    return TString(reinterpret_cast<const char*>(digest), SHA256_DIGEST_LENGTH);
}

TString HashHex(ui64 n) {
    const TString bytes = HashBytes(n);
    static const char* hex = "0123456789abcdef";
    TString out(bytes.size() * 2, '0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(bytes[i]);
        out[i * 2] = hex[byte >> 4];
        out[i * 2 + 1] = hex[byte & 0x0f];
    }
    return out;
}

TString MakeText(EColType type, EFill fill, std::mt19937_64& rng) {
    ui64 size = 0;
    if (fill == EFill::Narrow) {
        if (type == EColType::Json || type == EColType::JsonDocument) {
            size = UniformSize(rng, 2, 100);
        } else if (type == EColType::Yson) {
            size = UniformSize(rng, 1, 100);
        } else {
            size = UniformSize(rng, 0, 100);
        }
    } else if (fill == EFill::Medium) {
        size = UniformSize(rng, 100, 500);
    } else if (fill == EFill::Wide) {
        size = UniformSize(rng, 1000, 5000);
    }
    if (type == EColType::Json || type == EColType::JsonDocument) {
        return JsonOfSize(size, rng);
    }
    if (type == EColType::Yson) {
        return YsonOfSize(size, rng);
    }
    if (type == EColType::Utf8) {
        return TpchText(size, rng());
    }
    TString bytes(size, '\0');
    std::uniform_int_distribution<int> byte(0, 255);
    for (ui64 i = 0; i < size; ++i) {
        bytes[i] = static_cast<char>(byte(rng));
    }
    return bytes;
}

TBoundParam MakeValue(const TColumn& column, std::mt19937_64& rng) {
    TBoundParam param;
    param.SqlType = SqlType(column.Type);
    if (column.Fill != EFill::None) {
        param.Kind = EParamKind::Text;
        param.Text = MakeText(column.Type, column.Fill, rng);
        return param;
    }
    switch (column.Type) {
        case EColType::Bool:
            param.Kind = EParamKind::Bool;
            param.Bool = std::uniform_int_distribution<int>(0, 1)(rng) == 1;
            break;
        case EColType::Int8:
            param.Kind = EParamKind::Int8;
            param.I = RandomI64(rng, -128, 127);
            break;
        case EColType::Uint8:
            param.Kind = EParamKind::Uint8;
            param.U = UniformSize(rng, 0, 255);
            break;
        case EColType::Int16:
            param.Kind = EParamKind::Int16;
            param.I = RandomI64(rng, -32768, 32767);
            break;
        case EColType::Uint16:
            param.Kind = EParamKind::Uint16;
            param.U = UniformSize(rng, 0, 65535);
            break;
        case EColType::Int32:
            param.Kind = EParamKind::Int32;
            param.I = RandomI64(rng, std::numeric_limits<i32>::min(), std::numeric_limits<i32>::max());
            break;
        case EColType::Uint32:
            param.Kind = EParamKind::Uint32;
            param.U = UniformSize(rng, 0, std::numeric_limits<ui32>::max());
            break;
        case EColType::Int64:
            param.Kind = EParamKind::Int64;
            param.I = RandomI64(rng, std::numeric_limits<i64>::min(), std::numeric_limits<i64>::max());
            break;
        case EColType::Uint64:
            param.Kind = EParamKind::Uint64;
            param.U = rng();
            break;
        case EColType::Float:
            param.Kind = EParamKind::Float;
            param.Real = std::uniform_real_distribution<double>(-1e6, 1e6)(rng);
            break;
        case EColType::Double:
            param.Kind = EParamKind::Double;
            param.Real = std::uniform_real_distribution<double>(-1e12, 1e12)(rng);
            break;
        case EColType::Decimal22_9:
            SetDecimal(param, column.Type, DecimalString(rng, 22, 9));
            break;
        case EColType::Decimal1_0:
            SetDecimal(param, column.Type, ToString(RandomI64(rng, -9, 9)));
            break;
        case EColType::Decimal5_2:
            SetDecimal(param, column.Type, DecimalString(rng, 5, 2));
            break;
        case EColType::Decimal35_0:
            SetDecimal(param, column.Type, DecimalString(rng, 35, 0));
            break;
        case EColType::Decimal35_9:
            SetDecimal(param, column.Type, DecimalString(rng, 35, 9));
            break;
        case EColType::Decimal35_35:
            SetDecimal(param, column.Type, DecimalString(rng, 35, 35));
            break;
        case EColType::Uuid: {
            TString bytes(16, '\0');
            for (size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = static_cast<char>(UniformSize(rng, 0, 255));
            }
            param.Kind = EParamKind::Uuid;
            param.Text = bytes;
            break;
        }
        case EColType::Date:
            param.Kind = EParamKind::DateDays;
            param.U = UniformSize(rng, 0, DateDomain - 1);
            break;
        case EColType::Date32:
            param.Kind = EParamKind::Date32Days;
            param.I = RandomI64(rng, MinDate32, MaxDate32);
            break;
        case EColType::Datetime:
            param.Kind = EParamKind::DatetimeSeconds;
            param.U = UniformSize(rng, 0, MaxDatetime - 1);
            break;
        case EColType::Datetime64:
            param.Kind = EParamKind::Datetime64Seconds;
            param.I = RandomI64(rng, MinDatetime64, MaxDatetime64);
            break;
        case EColType::Timestamp:
            param.Kind = EParamKind::TimestampUs;
            param.U = UniformSize(rng, 0, MaxTimestamp - 1);
            break;
        case EColType::Timestamp64:
            param.Kind = EParamKind::Timestamp64Us;
            param.I = RandomI64(rng, MinTimestamp64 / 1000, MaxTimestamp64 / 1000) * 1000;
            break;
        case EColType::Interval:
            param.Kind = EParamKind::IntervalUs;
            param.I = RandomI64(rng, -static_cast<i64>(MaxTimestamp) + 1, static_cast<i64>(MaxTimestamp) - 1);
            break;
        case EColType::Interval64:
            param.Kind = EParamKind::Interval64Us;
            param.I = RandomI64(rng, -86400000000ll, 86400000000ll);
            break;
        default:
            param.Kind = EParamKind::Text;
            param.Text = MakeText(column.Type, EFill::Narrow, rng);
            break;
    }
    return param;
}

TVector<TBoundParam> MakeKeyParams(const TTable& table, ui64 n, ui64 seqnoHash, const TString& namePrefix) {
    TVector<TBoundParam> params;
    if (table.SeqnoHashPrefix) {
        TBoundParam hash;
        hash.Name = namePrefix + "seqno_hash";
        hash.SqlType = "Uint64";
        hash.Kind = EParamKind::Uint64;
        hash.U = seqnoHash;
        params.push_back(std::move(hash));
    }
    params.push_back(KeyParam(table, n, namePrefix + PkColumn(table).Name));
    if (table.Composite) {
        TBoundParam seq;
        seq.Name = namePrefix + "seq";
        seq.SqlType = "Uint64";
        seq.Kind = EParamKind::Uint64;
        seq.U = n;
        params.push_back(std::move(seq));
    }
    return params;
}

void MaybeNullValue(TVector<TBoundParam>& params, std::mt19937_64& rng, int percent) {
    TVector<size_t> candidates;
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i].Optional) {
            candidates.push_back(i);
        }
    }
    if (candidates.empty() || percent <= 0) {
        return;
    }
    if (std::uniform_int_distribution<int>(1, 100)(rng) > percent) {
        return;
    }
    const size_t pick = candidates[std::uniform_int_distribution<size_t>(0, candidates.size() - 1)(rng)];
    params[pick].Null = true;
}

TVector<TBoundParam> MakeRowParams(const TTable& table, ui64 n, ui64 seqno, std::mt19937_64& rng) {
    TVector<TBoundParam> params = MakeKeyParams(table, n, HashSeqno(seqno), "");
    for (const TColumn& column : table.Columns) {
        if (column.Key) {
            continue;
        }
        TBoundParam value;
        if (column.Name == "seqno") {
            value.Name = "seqno";
            value.SqlType = "Uint64";
            value.Kind = EParamKind::Uint64;
            value.U = seqno;
        } else {
            value = MakeValue(column, rng);
            value.Name = column.Name;
            value.Optional = true;
        }
        params.push_back(std::move(value));
    }
    MaybeNullValue(params, rng, 10);
    return params;
}

TString ImportQuery(const TString& fullName, const TTable& table) {
    TStringBuilder sql;
    sql << "DECLARE $input AS List<Struct<\n";
    for (size_t i = 0; i < table.Columns.size(); ++i) {
        if (i != 0) {
            sql << ",\n";
        }
        sql << "    " << table.Columns[i].Name << ": " << SqlType(table.Columns[i].Type);
        if (!table.Columns[i].Key && table.Columns[i].Name != "seqno") {
            sql << "?";
        }
    }
    sql << "\n>>;\n";
    sql << "UPSERT INTO " << QuotePath(fullName) << "\n";
    sql << "SELECT * FROM AS_TABLE($input);\n";
    return sql;
}

TVector<TVector<TBoundParam>> MakeImportRows(
    const TTable& table,
    ui64 begin,
    ui64 count,
    std::mt19937_64& rng)
{
    TVector<TVector<TBoundParam>> rows;
    rows.reserve(count);
    for (ui64 row = 0; row < count; ++row) {
        rows.push_back(MakeRowParams(table, begin + row, 0, rng));
    }
    return rows;
}

TRowWrite MakeInsertWrite(
    const TTable& table,
    const TString& fullName,
    ui64 n,
    ui64 seqno,
    const TString& prefix,
    std::mt19937_64& rng)
{
    TRowWrite write;
    TVector<TBoundParam> params = MakeRowParams(table, n, seqno, rng);
    for (TBoundParam& param : params) {
        param.Name = prefix + param.Name;
    }
    TStringBuilder sql;
    sql << "UPSERT INTO " << QuotePath(fullName) << " (" << ColumnList(table.Columns) << ") VALUES (";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) {
            sql << ", ";
        }
        sql << "$" << params[i].Name;
    }
    sql << ");";
    write.Statement = sql;
    write.Params = std::move(params);
    return write;
}

TString BuildScript(const TVector<TRowWrite>& writes) {
    TStringBuilder declares;
    TStringBuilder body;
    for (const TRowWrite& write : writes) {
        for (const TBoundParam& param : write.Params) {
            declares << "DECLARE $" << param.Name << " AS " << param.SqlType;
            if (param.Optional) {
                declares << "?";
            }
            declares << ";\n";
        }
        body << write.Statement << "\n";
    }
    return TString(declares) + body;
}

bool TryReserve(TVector<TTableRuntime>& state, const TVector<std::pair<size_t, ui64>>& inserts, TVector<TReservation>& out) {
    for (const auto& item : inserts) {
        if (state[item.first].InsertBusy) {
            return false;
        }
    }
    out.clear();
    for (const auto& item : inserts) {
        TTableRuntime& table = state[item.first];
        table.InsertBusy = true;
        TReservation reservation;
        reservation.TableIndex = item.first;
        reservation.First = static_cast<ui64>(table.Boundary + 1);
        reservation.Count = item.second;
        out.push_back(reservation);
    }
    return true;
}

void ApplyReservations(TVector<TTableRuntime>& state, const TVector<TReservation>& reservations, bool success) {
    for (const TReservation& reservation : reservations) {
        TTableRuntime& table = state[reservation.TableIndex];
        table.InsertBusy = false;
        if (success && reservation.Count > 0) {
            table.Boundary = static_cast<i64>(reservation.First + reservation.Count - 1);
        }
    }
}

ui64 ExistingRows(i64 boundary) {
    return boundary < 0 ? 0 : static_cast<ui64>(boundary) + 1;
}

TTxPlan BuildTransaction(
    const TVector<TTable>& tables,
    TVector<TTableRuntime>& state,
    const TString& pathPrefix,
    ui64 seqno,
    ui64 maxRecords,
    std::mt19937_64& rng)
{
    TTxPlan plan;
    plan.Seqno = seqno;
    std::uniform_int_distribution<size_t> tableCount(1, tables.size());
    const size_t selectedCount = tableCount(rng);
    TVector<size_t> indexes(tables.size());
    for (size_t i = 0; i < indexes.size(); ++i) {
        indexes[i] = i;
    }
    std::shuffle(indexes.begin(), indexes.end(), rng);
    indexes.resize(selectedCount);
    std::sort(indexes.begin(), indexes.end(), [&](size_t left, size_t right) {
        return tables[left].Name < tables[right].Name;
    });

    struct TTableWork {
        size_t Index = 0;
        ui64 Inserts = 0;
        TVector<ui64> UpdateNumbers;
    };
    TVector<TTableWork> work;
    TVector<std::pair<size_t, ui64>> insertRequests;
    std::uniform_int_distribution<ui64> recordCount(1, std::max<ui64>(1, maxRecords));
    for (size_t index : indexes) {
        const ui64 records = recordCount(rng);
        const ui64 plannedInserts = records / 2;
        const ui64 plannedUpdatesRaw = records - plannedInserts;
        const ui64 existing = ExistingRows(state[index].Boundary);
        const ui64 plannedUpdates = std::min(plannedUpdatesRaw, existing);
        const ui64 inserts = plannedInserts + (plannedUpdatesRaw - plannedUpdates);
        TTableWork item;
        item.Index = index;
        item.Inserts = inserts;
        if (plannedUpdates > 0) {
            THashSet<ui64> used;
            std::uniform_int_distribution<ui64> pick(0, static_cast<ui64>(state[index].Boundary));
            while (used.size() < plannedUpdates) {
                used.insert(pick(rng));
            }
            item.UpdateNumbers.assign(used.begin(), used.end());
        }
        if (inserts > 0) {
            insertRequests.emplace_back(index, inserts);
        }
        work.push_back(std::move(item));
    }
    if (!TryReserve(state, insertRequests, plan.Reservations)) {
        plan.Busy = true;
        return plan;
    }
    for (const TReservation& reservation : plan.Reservations) {
        plan.InsertTables.push_back(reservation.TableIndex);
    }

    ui64 paramId = 0;
    auto nextPrefix = [&]() {
        return TStringBuilder() << "p" << paramId++ << "_";
    };
    for (const TTableWork& item : work) {
        const TTable& table = tables[item.Index];
        const TString fullName = JoinPath(pathPrefix, table.Name);
        const TReservation* reservation = nullptr;
        for (const TReservation& candidate : plan.Reservations) {
            if (candidate.TableIndex == item.Index) {
                reservation = &candidate;
                break;
            }
        }
        for (ui64 row = 0; row < item.Inserts; ++row) {
            const ui64 n = reservation->First + row;
            const TString prefix = nextPrefix();
            plan.Writes.push_back(MakeInsertWrite(table, fullName, n, seqno, prefix, rng));
        }
        for (ui64 number : item.UpdateNumbers) {
            const TString prefix = nextPrefix();
            TRowWrite write;
            write.Update = true;
            const int columnCount = std::uniform_int_distribution<int>(1, 3)(rng);
            TVector<size_t> eligible = ValueColumnsExceptSeqno(table);
            std::shuffle(eligible.begin(), eligible.end(), rng);
            const int extras = std::min<int>(columnCount - 1, static_cast<int>(eligible.size()));
            TVector<const TColumn*> chosen;
            for (const TColumn& column : table.Columns) {
                if (column.Name == "seqno") {
                    chosen.push_back(&column);
                    break;
                }
            }
            for (int i = 0; i < extras; ++i) {
                chosen.push_back(&table.Columns[eligible[i]]);
            }
            TStringBuilder sql;
            sql << "UPDATE " << QuotePath(fullName) << " SET ";
            TVector<TBoundParam> params;
            for (size_t i = 0; i < chosen.size(); ++i) {
                if (i) {
                    sql << ", ";
                }
                TBoundParam value;
                if (chosen[i]->Name == "seqno") {
                    value.Name = prefix + "seqno";
                    value.SqlType = "Uint64";
                    value.Kind = EParamKind::Uint64;
                    value.U = seqno;
                } else {
                    value = MakeValue(*chosen[i], rng);
                    value.Name = prefix + chosen[i]->Name;
                    value.Optional = true;
                }
                sql << chosen[i]->Name << " = $" << value.Name;
                params.push_back(std::move(value));
            }
            MaybeNullValue(params, rng, 1);
            sql << " WHERE ";
            ui64 seqnoHash = HashSeqno(0);
            if (table.SeqnoHashPrefix) {
                if (const ui64* found = state[item.Index].SeqnoHashByRow.FindPtr(number)) {
                    seqnoHash = *found;
                }
            }
            TVector<TBoundParam> keys = MakeKeyParams(table, number, seqnoHash, prefix);
            size_t keyIndex = 0;
            for (const TColumn& column : table.Columns) {
                if (!column.Key) {
                    continue;
                }
                if (keyIndex != 0) {
                    sql << " AND ";
                }
                sql << column.Name << " = $" << keys[keyIndex].Name;
                params.push_back(std::move(keys[keyIndex]));
                ++keyIndex;
            }
            sql << ";";
            write.Statement = sql;
            write.Params = std::move(params);
            plan.Writes.push_back(std::move(write));
        }
    }
    return plan;
}

} // namespace NYdb::NConsoleClient::NReplicationWorkload
