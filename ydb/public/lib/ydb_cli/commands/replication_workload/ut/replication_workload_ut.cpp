#include <ydb/public/lib/ydb_cli/commands/replication_workload/replication_workload_exec.h>
#include <ydb/public/lib/ydb_cli/commands/replication_workload/replication_workload_model.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/hash_set.h>
#include <util/string/cast.h>

#include <random>

namespace NYdb::NConsoleClient::NReplicationWorkload {
namespace {

const TTable* Find(const TString& name) {
    for (const TTable& table : AllTables()) {
        if (table.Name == name) {
            return &table;
        }
    }
    return nullptr;
}

const TBoundParam* FindParam(const TVector<TBoundParam>& params, const TString& name) {
    for (const TBoundParam& param : params) {
        if (param.Name == name) {
            return &param;
        }
    }
    return nullptr;
}

}

Y_UNIT_TEST_SUITE(ReplicationWorkload) {

Y_UNIT_TEST(TableShapes) {
    const TVector<TTable>& tables = AllTables();
    UNIT_ASSERT_VALUES_EQUAL(tables.size(), 28);
    ui32 ordinary = 0;
    ui32 wide = 0;
    ui32 composite = 0;
    for (const TTable& table : tables) {
        if (table.Wide) {
            ++wide;
            UNIT_ASSERT_VALUES_EQUAL(table.Columns.size(), 32);
            UNIT_ASSERT_VALUES_EQUAL(table.Columns[30].Name, "c_medium");
            UNIT_ASSERT_VALUES_EQUAL(table.Columns[31].Name, "c_wide");
            UNIT_ASSERT(table.Columns[30].Fill == EFill::Medium);
            UNIT_ASSERT(table.Columns[31].Fill == EFill::Wide);
        } else if (table.PkSeqHashPrefix) {
            ++ordinary;
            UNIT_ASSERT_VALUES_EQUAL(table.Columns.size(), 11);
            UNIT_ASSERT_VALUES_EQUAL(table.Columns[0].Name, "pk_seq_hash");
            UNIT_ASSERT(table.Columns[0].Key);
        } else {
            ++ordinary;
            UNIT_ASSERT_VALUES_EQUAL(table.Columns.size(), 10);
        }
        if (table.Composite) {
            ++composite;
            UNIT_ASSERT(table.Columns[table.PkSeqHashPrefix ? 2 : 1].Name == "seq");
        }
        bool hasSeqno = false;
        for (const TColumn& column : table.Columns) {
            if (column.Name == "seqno") {
                hasSeqno = true;
            }
        }
        UNIT_ASSERT(hasSeqno);
    }
    UNIT_ASSERT_VALUES_EQUAL(ordinary, 23);
    UNIT_ASSERT_VALUES_EQUAL(wide, 5);
    UNIT_ASSERT_VALUES_EQUAL(composite, 7);
}

Y_UNIT_TEST(BoundaryCountUsesSeqnoIndex) {
    const TTable* wide = Find("wide_json");
    const TTable* plain = Find("pk_uint64");
    const TTable* composite = Find("pk_bool");
    UNIT_ASSERT(wide && plain && composite);
    UNIT_ASSERT_VALUES_EQUAL(
        BoundaryQuery(*wide, "/db/replication/wide_json"),
        "SELECT COUNT(*) AS pk FROM `/db/replication/wide_json` VIEW seqno_idx;");
    UNIT_ASSERT_VALUES_EQUAL(
        BoundaryQuery(*plain, "/db/replication/pk_uint64"),
        "SELECT COUNT(*) AS pk FROM `/db/replication/pk_uint64` VIEW seqno_idx;");
    const TString compositeQuery = BoundaryQuery(*composite, "/db/replication/pk_bool");
    UNIT_ASSERT(compositeQuery.Contains("ORDER BY seq DESC LIMIT 1"));
    UNIT_ASSERT(!compositeQuery.Contains("COUNT"));
    UNIT_ASSERT(!compositeQuery.Contains("VIEW"));
}

Y_UNIT_TEST(IndexesAndPartitions) {
    const TTable* narrow = Find("pk_bool");
    const TTable* plain = Find("pk_uint64");
    const TTable* wide = Find("wide_json");
    UNIT_ASSERT(narrow && plain && wide);
    const TString narrowDdl = CreateTableQuery("/db/replication/pk_bool", *narrow, 3, 4);
    UNIT_ASSERT(narrowDdl.Contains("INDEX seqno_idx GLOBAL SYNC ON (seqno)"));
    UNIT_ASSERT(narrowDdl.Contains("INDEX seq_idx GLOBAL SYNC ON (seq)"));
    UNIT_ASSERT(narrowDdl.Contains("PRIMARY KEY (pk_seq_hash, pk, seq)"));
    UNIT_ASSERT(narrowDdl.Contains("pk_seq_hash Uint64 NOT NULL"));
    UNIT_ASSERT(narrowDdl.Contains("pk Bool NOT NULL"));
    UNIT_ASSERT(narrowDdl.Contains("seq Uint64 NOT NULL"));
    UNIT_ASSERT(narrowDdl.Contains("seqno Uint64 NOT NULL"));
    UNIT_ASSERT(narrowDdl.Contains("FAMILY default (COMPRESSION='lz4')"));
    UNIT_ASSERT(!narrowDdl.Contains("UNIFORM_PARTITIONS"));
    UNIT_ASSERT(narrowDdl.Contains("AUTO_PARTITIONING_BY_LOAD = ENABLED"));
    UNIT_ASSERT(narrowDdl.Contains("AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 3"));
    UNIT_ASSERT(narrowDdl.Contains("AUTO_PARTITIONING_MAX_PARTITIONS_COUNT = 4"));
    UNIT_ASSERT_VALUES_EQUAL(AlterIndexQueries("/db/replication/pk_bool", *narrow, 3, 4).size(), 2);

    const TString plainDdl = CreateTableQuery("/db/replication/pk_uint64", *plain, 1000, 1100);
    UNIT_ASSERT(!plainDdl.Contains("seq_idx"));
    UNIT_ASSERT(plainDdl.Contains("FAMILY default (COMPRESSION='lz4')"));
    UNIT_ASSERT(!plainDdl.Contains("UNIFORM_PARTITIONS"));
    UNIT_ASSERT_VALUES_EQUAL(AlterIndexQueries("/db/replication/pk_uint64", *plain, 1000, 1100).size(), 1);

    const TString wideDdl = CreateTableQuery("/db/replication/wide_json", *wide, 8, 9);
    UNIT_ASSERT(wideDdl.Contains("c_medium Json"));
    UNIT_ASSERT(wideDdl.Contains("c_wide Json"));
    UNIT_ASSERT(wideDdl.Contains("FAMILY default (COMPRESSION='lz4')"));
    UNIT_ASSERT(!wideDdl.Contains("UNIFORM_PARTITIONS"));
    const TTable* yson = Find("wide_yson");
    UNIT_ASSERT(CreateTableQuery("/db/wide_yson", *yson, 1, 2).Contains("c_medium Yson"));
}

Y_UNIT_TEST(KeysAndText) {
    const TTable* boolean = Find("pk_bool");
    const TTable* bytes = Find("pk_string");
    const TTable* text = Find("pk_utf8");
    const TTable* uuid = Find("pk_uuid");
    const TTable* uint64 = Find("pk_uint64");
    auto boolKey = MakeKeyParams(*boolean, 0, "k");
    UNIT_ASSERT_VALUES_EQUAL(boolKey[0].U, HashPkSeq(0, 0));
    UNIT_ASSERT(!boolKey[1].Bool);
    UNIT_ASSERT_VALUES_EQUAL(boolKey[2].U, 0);
    boolKey = MakeKeyParams(*boolean, 1, "k");
    UNIT_ASSERT_VALUES_EQUAL(boolKey[0].U, HashPkSeq(1, 1));
    UNIT_ASSERT(boolKey[1].Bool);
    UNIT_ASSERT_VALUES_EQUAL(boolKey[2].U, 1);
    const auto sameBoolPk = MakeKeyParams(*boolean, 2, "k");
    UNIT_ASSERT(!sameBoolPk[1].Bool);
    UNIT_ASSERT_VALUES_EQUAL(sameBoolPk[2].U, 2);
    UNIT_ASSERT_VALUES_EQUAL(sameBoolPk[0].U, HashPkSeq(0, 2));
    UNIT_ASSERT(sameBoolPk[0].U != MakeKeyParams(*boolean, 0, "k")[0].U);
    UNIT_ASSERT(HashSeqno(1) != HashSeqno(2));

    const TString digest = HashBytes(42);
    UNIT_ASSERT_VALUES_EQUAL(digest.size(), 32);
    UNIT_ASSERT_VALUES_EQUAL(HashHex(42).size(), 64);
    UNIT_ASSERT(HashBytes(1) != HashBytes(2));
    UNIT_ASSERT_VALUES_EQUAL(MakeKeyParams(*bytes, 7, "k")[0].Text, HashBytes(7));
    UNIT_ASSERT_VALUES_EQUAL(MakeKeyParams(*text, 7, "k")[0].Text, HashHex(7));

    const ui64 firstNumber = MakeKeyParams(*uint64, 1, "k")[0].U;
    const ui64 secondNumber = MakeKeyParams(*uint64, 2, "k")[0].U;
    UNIT_ASSERT((firstNumber >> 63) != (secondNumber >> 63));

    const TString first = MakeKeyParams(*uuid, 1, "k")[0].Text;
    const TString second = MakeKeyParams(*uuid, 2, "k")[0].Text;
    UNIT_ASSERT_VALUES_EQUAL(first.size(), 16);
    UNIT_ASSERT(first.substr(0, 8) != TString(8, '\0'));
    UNIT_ASSERT_VALUES_EQUAL(static_cast<unsigned char>(first[15]), 1);
    UNIT_ASSERT(first != second);

    std::mt19937_64 rng(1);
    const TString json = MakeText(EColType::Json, EFill::Narrow, rng);
    UNIT_ASSERT(json.size() >= 2 && json.size() <= 100);
    rng.seed(2);
    const TString yson = MakeText(EColType::Yson, EFill::Wide, rng);
    UNIT_ASSERT(yson.size() >= 1000 && yson.size() <= 5000);
    UNIT_ASSERT(yson.StartsWith("{v=\""));
    UNIT_ASSERT(yson.find("aaaaaaaa") == TString::npos);

    std::mt19937_64 utf8Rng(7);
    std::mt19937_64 utf8RngAgain(7);
    const TString utf8 = MakeText(EColType::Utf8, EFill::Wide, utf8Rng);
    UNIT_ASSERT(utf8.size() >= 1000 && utf8.size() <= 5000);
    UNIT_ASSERT_VALUES_EQUAL(utf8, MakeText(EColType::Utf8, EFill::Wide, utf8RngAgain));
    UNIT_ASSERT(utf8.find_first_not_of('a') != TString::npos);
    for (unsigned char ch : utf8) {
        UNIT_ASSERT(ch >= 32 && ch < 127);
    }
}

Y_UNIT_TEST(PkSeqHashFollowsPkAndSeq) {
    const TTable* boolean = Find("pk_bool");
    const TTable* decimal = Find("pk_decimal_narrow");
    UNIT_ASSERT(boolean && decimal);

    const auto decimalZero = MakeKeyParams(*decimal, 0, "k");
    UNIT_ASSERT_VALUES_EQUAL(decimalZero[1].Text, "-9");
    UNIT_ASSERT_VALUES_EQUAL(decimalZero[0].U, HashPkSeq(static_cast<ui64>(static_cast<i64>(-9)), 0));
    UNIT_ASSERT_VALUES_EQUAL(decimalZero[2].U, 0);
    const auto decimalAgain = MakeKeyParams(*decimal, 19, "k");
    UNIT_ASSERT_VALUES_EQUAL(decimalAgain[1].Text, "-9");
    UNIT_ASSERT_VALUES_EQUAL(decimalAgain[2].U, 19);
    UNIT_ASSERT_VALUES_EQUAL(decimalAgain[0].U, HashPkSeq(static_cast<ui64>(static_cast<i64>(-9)), 19));
    UNIT_ASSERT(decimalAgain[0].U != decimalZero[0].U);

    std::mt19937_64 leftRng(1);
    std::mt19937_64 rightRng(1);
    const TRowWrite inserted = MakeInsertWrite(*boolean, "/db/replication/pk_bool", 5, 0, leftRng);
    const TRowWrite rewritten = MakeInsertWrite(*boolean, "/db/replication/pk_bool", 5, 42, rightRng);
    const TBoundParam* insertedHash = FindParam(inserted.Params, "pk_seq_hash");
    const TBoundParam* rewrittenHash = FindParam(rewritten.Params, "pk_seq_hash");
    const TBoundParam* insertedSeqno = FindParam(inserted.Params, "seqno");
    const TBoundParam* rewrittenSeqno = FindParam(rewritten.Params, "seqno");
    UNIT_ASSERT(insertedHash && rewrittenHash && insertedSeqno && rewrittenSeqno);
    UNIT_ASSERT_VALUES_EQUAL(insertedHash->U, HashPkSeq(1, 5));
    UNIT_ASSERT_VALUES_EQUAL(rewrittenHash->U, HashPkSeq(1, 5));
    UNIT_ASSERT_VALUES_EQUAL(insertedSeqno->U, 0);
    UNIT_ASSERT_VALUES_EQUAL(rewrittenSeqno->U, 42);

    bool sawBool = false;
    bool sawDecimal = false;
    for (int seed = 0; seed < 40 && !(sawBool && sawDecimal); ++seed) {
        TVector<TTableRuntime> state(AllTables().size());
        for (TTableRuntime& table : state) {
            table.Boundary = 40;
        }
        std::mt19937_64 rng(30 + seed);
        const TTxPlan plan = BuildTransaction(AllTables(), state, "/db/replication", 1000 + seed, 4, rng);
        for (const TRowWrite& write : plan.Writes) {
            if (!write.Update) {
                continue;
            }
            const TBoundParam* hash = FindParam(write.Params, "pk_seq_hash");
            if (hash == nullptr) {
                continue;
            }
            const TBoundParam* pk = FindParam(write.Params, "pk");
            const TBoundParam* seq = FindParam(write.Params, "seq");
            UNIT_ASSERT(pk && seq);
            if (write.Statement.Contains("UPDATE `/db/replication/pk_bool`")) {
                sawBool = true;
                UNIT_ASSERT_VALUES_EQUAL(hash->U, HashPkSeq(pk->Bool ? 1 : 0, seq->U));
            }
            if (write.Statement.Contains("UPDATE `/db/replication/pk_decimal_narrow`")) {
                sawDecimal = true;
                const ui64 pkBits = static_cast<ui64>(FromString<i64>(pk->Text));
                UNIT_ASSERT_VALUES_EQUAL(hash->U, HashPkSeq(pkBits, seq->U));
            }
        }
    }
    UNIT_ASSERT(sawBool);
    UNIT_ASSERT(sawDecimal);
}

Y_UNIT_TEST(ImportBatchUsesAsTable) {
    const TTable* boolean = Find("pk_bool");
    const TTable* wide = Find("wide_utf8");
    UNIT_ASSERT(boolean);
    UNIT_ASSERT(wide);
    const TString query = ImportQuery("/db/replication/pk_bool", *boolean);
    UNIT_ASSERT(query.Contains("DECLARE $input AS List<Struct<"));
    UNIT_ASSERT(query.Contains("pk_seq_hash: Uint64"));
    UNIT_ASSERT(!query.Contains("pk_seq_hash: Uint64?"));
    UNIT_ASSERT(query.Contains("pk: Bool"));
    UNIT_ASSERT(!query.Contains("pk: Bool?"));
    UNIT_ASSERT(query.Contains("seqno: Uint64"));
    UNIT_ASSERT(!query.Contains("seqno: Uint64?"));
    UNIT_ASSERT(query.Contains("c_float: Float?"));
    UNIT_ASSERT(query.Contains("seq: Uint64"));
    UNIT_ASSERT(query.Contains("UPSERT INTO `/db/replication/pk_bool`"));
    UNIT_ASSERT(query.Contains("SELECT * FROM AS_TABLE($input)"));
    UNIT_ASSERT(!query.Contains("VALUES"));

    const TString wideQuery = ImportQuery("/db/replication/wide_utf8", *wide);
    UNIT_ASSERT(wideQuery.Contains("c_medium: Utf8"));
    UNIT_ASSERT(wideQuery.Contains("c_wide: Utf8"));
    UNIT_ASSERT(wideQuery.Contains("SELECT * FROM AS_TABLE($input)"));

    std::mt19937_64 rng(3);
    const TVector<TVector<TBoundParam>> rows = MakeImportRows(*boolean, 5, 2, rng);
    UNIT_ASSERT_VALUES_EQUAL(rows.size(), 2);
    UNIT_ASSERT_VALUES_EQUAL(rows[0].size(), boolean->Columns.size());
    for (size_t i = 0; i < boolean->Columns.size(); ++i) {
        UNIT_ASSERT_VALUES_EQUAL(rows[0][i].Name, boolean->Columns[i].Name);
    }
    UNIT_ASSERT_VALUES_EQUAL(rows[0][0].U, HashPkSeq(1, 5));
    UNIT_ASSERT_VALUES_EQUAL(rows[1][0].U, HashPkSeq(0, 6));
    UNIT_ASSERT_VALUES_EQUAL(rows[0][2].U, 5);
    UNIT_ASSERT_VALUES_EQUAL(rows[1][2].U, 6);
    for (const TVector<TBoundParam>& row : rows) {
        for (const TBoundParam& param : row) {
            if (param.Name == "seqno") {
                UNIT_ASSERT_VALUES_EQUAL(param.U, 0);
            }
        }
    }

    const TParams params = BindImportRows(rows);
    const std::optional<TValue> value = params.GetValue("$input");
    UNIT_ASSERT(value);
    const TString type = FormatType(value->GetType());
    UNIT_ASSERT(type.Contains("List<Struct<"));
    UNIT_ASSERT(type.Contains("pk_seq_hash"));
    TValueParser parser(*value);
    parser.OpenList();
    ui32 items = 0;
    while (parser.TryNextListItem()) {
        ++items;
    }
    UNIT_ASSERT_VALUES_EQUAL(items, 2);
}

ui32 UpdateAssignmentCount(const TString& statement) {
    const size_t setPos = statement.find(" SET ");
    const size_t wherePos = statement.find(" WHERE ");
    UNIT_ASSERT(setPos != TString::npos);
    UNIT_ASSERT(wherePos != TString::npos && wherePos > setPos);
    const TString assignments = statement.substr(setPos + 5, wherePos - (setPos + 5));
    ui32 count = assignments.empty() ? 0 : 1;
    for (char ch : assignments) {
        if (ch == ',') {
            ++count;
        }
    }
    return count;
}

TVector<size_t> ValueIndexes(const TTable& table) {
    TVector<size_t> indexes;
    for (size_t i = 0; i < table.Columns.size(); ++i) {
        if (!table.Columns[i].Key && table.Columns[i].Name != "seqno") {
            indexes.push_back(i);
        }
    }
    return indexes;
}

THashSet<TString> UpdateStatements(const TTable& table, const TString& fullName) {
    const TColumn* seqno = nullptr;
    for (const TColumn& column : table.Columns) {
        if (column.Name == "seqno") {
            seqno = &column;
            break;
        }
    }
    UNIT_ASSERT(seqno);
    THashSet<TString> texts;
    for (const TVector<size_t>& shape : UpdateValueColumnSets(table)) {
        TVector<const TColumn*> assigned;
        assigned.reserve(shape.size() + 1);
        assigned.push_back(seqno);
        for (size_t index : shape) {
            assigned.push_back(&table.Columns[index]);
        }
        const TString text = UpdateQuery(fullName, table, assigned);
        UNIT_ASSERT(texts.insert(text).second);
        const size_t statement = text.find("UPDATE ");
        UNIT_ASSERT(statement != TString::npos);
        UNIT_ASSERT(text.substr(statement).find('\'') == TString::npos);
    }
    UNIT_ASSERT_VALUES_EQUAL(texts.size(), 5);
    return texts;
}

Y_UNIT_TEST(UpdateAssignsSeqnoAndAnotherColumn) {
    const TVector<TTable>& tables = AllTables();
    struct TAllowed {
        TString Quoted;
        THashSet<TString> Texts;
        ui64 ValueColumns = 0;
    };
    TVector<TAllowed> allowed;
    for (const TTable& table : tables) {
        const TString fullName = JoinPath("/db/replication", table.Name);
        allowed.push_back({
            "`" + fullName + "`",
            UpdateStatements(table, fullName),
            ValueIndexes(table).size(),
        });
    }

    ui32 updates = 0;
    ui32 oneColumn = 0;
    ui32 threeColumns = 0;
    ui32 allColumns = 0;
    bool sawBoolKey = false;
    for (int seed = 0; seed < 40; ++seed) {
        TVector<TTableRuntime> state(tables.size());
        for (TTableRuntime& table : state) {
            table.Boundary = 9;
        }
        std::mt19937_64 rng(5 + seed);
        const TTxPlan plan = BuildTransaction(tables, state, "/db/replication", 100 + seed, 4, rng);
        UNIT_ASSERT(!plan.Busy);
        UNIT_ASSERT(!plan.Writes.empty());
        for (const TRowWrite& write : plan.Writes) {
            if (!write.Update) {
                UNIT_ASSERT(write.Statement.Contains("UPSERT INTO"));
                continue;
            }
            ++updates;
            UNIT_ASSERT(write.Statement.Contains("UPDATE "));
            UNIT_ASSERT(write.Statement.Contains("seqno = $seqno"));
            UNIT_ASSERT(write.Statement.Contains(" WHERE "));
            const TAllowed* match = nullptr;
            for (const TAllowed& candidate : allowed) {
                if (write.Statement.Contains(candidate.Quoted)) {
                    match = &candidate;
                    break;
                }
            }
            UNIT_ASSERT(match);
            UNIT_ASSERT(match->Texts.contains(write.Statement));
            const ui32 assigned = UpdateAssignmentCount(write.Statement);
            if (assigned == 2) {
                ++oneColumn;
            } else if (assigned == match->ValueColumns + 1) {
                ++allColumns;
            } else if (assigned == 4 || assigned == 5) {
                ++threeColumns;
            } else {
                UNIT_ASSERT(false);
            }
            ui32 seqnoAssignments = 0;
            for (const TBoundParam& param : write.Params) {
                if (param.Name == "seqno") {
                    ++seqnoAssignments;
                    UNIT_ASSERT(!param.Optional);
                    UNIT_ASSERT(!param.Null);
                }
            }
            UNIT_ASSERT_VALUES_EQUAL(seqnoAssignments, 1);
            if (write.Statement.Contains("UPDATE `/db/replication/pk_bool`")) {
                sawBoolKey = true;
                UNIT_ASSERT(write.Statement.Contains("pk_seq_hash = $"));
                UNIT_ASSERT(write.Statement.Contains("pk = $"));
                UNIT_ASSERT(write.Statement.Contains("seq = $"));
            }
        }
    }
    UNIT_ASSERT(updates > 0);
    UNIT_ASSERT(oneColumn > 0);
    UNIT_ASSERT(threeColumns > 0);
    UNIT_ASSERT(allColumns > 0);
    UNIT_ASSERT(sawBoolKey);
}

Y_UNIT_TEST(NullValueIsOptionalAndEmpty) {
    TBoundParam present;
    present.Name = "seqno";
    present.SqlType = "Uint64";
    present.Kind = EParamKind::Uint64;
    present.Optional = true;
    present.U = 5;
    const std::optional<TValue> filled = BindParams({present}).GetValue("$seqno");
    UNIT_ASSERT(filled);
    {
        TValueParser parser(*filled);
        const std::optional<ui64> value = parser.GetOptionalUint64();
        UNIT_ASSERT(value.has_value());
        UNIT_ASSERT_VALUES_EQUAL(*value, 5);
    }

    present.Null = true;
    const std::optional<TValue> empty = BindParams({present}).GetValue("$seqno");
    UNIT_ASSERT(empty);
    {
        TValueParser parser(*empty);
        UNIT_ASSERT(!parser.GetOptionalUint64().has_value());
    }

    TBoundParam decimal;
    decimal.Name = "c_decimal";
    decimal.SqlType = "Decimal(22, 9)";
    decimal.Kind = EParamKind::Decimal;
    decimal.Precision = 22;
    decimal.Scale = 9;
    decimal.Optional = true;
    decimal.Null = true;
    const std::optional<TValue> nullDecimal = BindParams({decimal}).GetValue("$c_decimal");
    UNIT_ASSERT(nullDecimal);
    {
        TValueParser parser(*nullDecimal);
        parser.OpenOptional();
        UNIT_ASSERT(parser.IsNull());
    }
}

Y_UNIT_TEST(InsertNullsOneOptionalColumn) {
    const TTable* table = Find("pk_uint64");
    UNIT_ASSERT(table);
    std::mt19937_64 rng(11);
    ui32 rowsWithNull = 0;
    const TVector<TVector<TBoundParam>> rows = MakeImportRows(*table, 0, 200, rng);
    for (const TVector<TBoundParam>& row : rows) {
        ui32 nulls = 0;
        for (const TBoundParam& param : row) {
            const bool required = param.Name == "pk" || param.Name == "seqno";
            if (required) {
                UNIT_ASSERT(!param.Optional);
                UNIT_ASSERT(!param.Null);
                continue;
            }
            UNIT_ASSERT(param.Optional);
            if (param.Null) {
                ++nulls;
            }
        }
        UNIT_ASSERT(nulls <= 1);
        rowsWithNull += nulls;
    }
    UNIT_ASSERT(rowsWithNull >= 5 && rowsWithNull <= 50);
}

Y_UNIT_TEST(UpdateNullsOneAssignedValue) {
    const TVector<TTable>& tables = AllTables();
    std::mt19937_64 rng(19);
    ui32 nullUpdates = 0;
    for (int i = 0; i < 400; ++i) {
        TVector<TTableRuntime> state(tables.size());
        for (TTableRuntime& table : state) {
            table.Boundary = 30;
        }
        const TTxPlan plan = BuildTransaction(tables, state, "/db/replication", 1000 + i, 4, rng);
        if (plan.Busy) {
            continue;
        }
        for (const TRowWrite& write : plan.Writes) {
            ui32 nulls = 0;
            for (const TBoundParam& param : write.Params) {
                if (param.Name.EndsWith("seqno")) {
                    UNIT_ASSERT(!param.Optional);
                    UNIT_ASSERT(!param.Null);
                }
                if (param.Null) {
                    UNIT_ASSERT(param.Optional);
                    ++nulls;
                } else if (!param.Optional) {
                    UNIT_ASSERT(!param.Null);
                }
            }
            UNIT_ASSERT(nulls <= 1);
            if (write.Update && nulls == 1) {
                ++nullUpdates;
            }
        }
    }
    UNIT_ASSERT(nullUpdates >= 1);
}

Y_UNIT_TEST(MaybeNullValueHitsRequestedRate) {
    auto trials = [](int percent) {
        std::mt19937_64 rng(percent * 17 + 3);
        ui32 hits = 0;
        for (int i = 0; i < 4000; ++i) {
            TVector<TBoundParam> params(4);
            for (TBoundParam& param : params) {
                param.Optional = true;
            }
            MaybeNullValue(params, rng, percent);
            ui32 nulls = 0;
            for (const TBoundParam& param : params) {
                nulls += param.Null ? 1 : 0;
            }
            UNIT_ASSERT(nulls <= 1);
            hits += nulls;
        }
        return hits;
    };
    const ui32 inserts = trials(10);
    UNIT_ASSERT(inserts >= 320 && inserts <= 480);
    const ui32 updates = trials(1);
    UNIT_ASSERT(updates >= 15 && updates <= 70);
}

Y_UNIT_TEST(StatementsStayStable) {
    const TTable* table = Find("pk_uint64");
    UNIT_ASSERT(table);
    std::mt19937_64 leftRng(1);
    std::mt19937_64 rightRng(2);
    const TRowWrite left = MakeInsertWrite(*table, "/db/replication/pk_uint64", 1, 10, leftRng);
    const TRowWrite right = MakeInsertWrite(*table, "/db/replication/pk_uint64", 2, 20, rightRng);
    UNIT_ASSERT_VALUES_EQUAL(left.Statement, right.Statement);
    UNIT_ASSERT(left.Statement.Contains("DECLARE $pk AS Uint64;\n"));
    UNIT_ASSERT(left.Statement.Contains("DECLARE $seqno AS Uint64;\n"));
    UNIT_ASSERT(!left.Statement.Contains("DECLARE $seqno AS Uint64?;"));
    UNIT_ASSERT(left.Statement.Contains("DECLARE $c_utf8 AS Utf8?;\n"));
    UNIT_ASSERT(left.Statement.Contains("DECLARE $c_decimal AS Decimal(22, 9)?;\n"));
    UNIT_ASSERT(left.Statement.Contains("UPSERT INTO `/db/replication/pk_uint64`"));
    UNIT_ASSERT(left.Statement.EndsWith(");"));
    UNIT_ASSERT_VALUES_EQUAL(left.Params[1].U, 10);
    UNIT_ASSERT_VALUES_EQUAL(right.Params[1].U, 20);
    ui32 upserts = 0;
    for (size_t pos = 0; (pos = left.Statement.find("UPSERT INTO", pos)) != TString::npos; pos += 11) {
        ++upserts;
    }
    UNIT_ASSERT_VALUES_EQUAL(upserts, 1);

    const TVector<TTable>& tables = AllTables();
    THashMap<TString, TString> textByShape;
    ui32 repeated = 0;
    bool sawMultiInsert = false;
    for (int i = 0; i < 40; ++i) {
        TVector<TTableRuntime> state(tables.size());
        for (TTableRuntime& runtime : state) {
            runtime.Boundary = 20;
        }
        std::mt19937_64 rng(100 + i);
        const TTxPlan plan = BuildTransaction(tables, state, "/db/replication", 500 + i, 10, rng);
        if (plan.Busy) {
            continue;
        }
        THashMap<TString, ui32> upsertsByTable;
        for (const TRowWrite& write : plan.Writes) {
            const bool upsert = write.Statement.Contains("UPSERT INTO");
            const bool update = write.Statement.Contains("UPDATE ");
            UNIT_ASSERT(upsert != update);
            UNIT_ASSERT(!write.Statement.Contains("p0_"));
            const size_t pos = upsert ? write.Statement.find("UPSERT INTO") : write.Statement.find("UPDATE ");
            UNIT_ASSERT(pos != TString::npos && pos > 0);
            const TString shape = write.Statement.substr(pos);
            if (const TString* previous = textByShape.FindPtr(shape)) {
                UNIT_ASSERT_VALUES_EQUAL(*previous, write.Statement);
                ++repeated;
            } else {
                textByShape[shape] = write.Statement;
            }
            if (upsert) {
                const size_t path = shape.find('`');
                if (++upsertsByTable[shape.substr(path)] >= 2) {
                    sawMultiInsert = true;
                }
            }
            for (const TBoundParam& param : write.Params) {
                UNIT_ASSERT(write.Statement.Contains("DECLARE $" + param.Name + " AS "));
                UNIT_ASSERT(!param.Name.Contains("p0_"));
            }
        }
    }
    UNIT_ASSERT(sawMultiInsert);
    UNIT_ASSERT(repeated >= 1);
}

void AssertColumnNames(const TTable& table, const TVector<size_t>& indexes, std::initializer_list<const char*> names) {
    UNIT_ASSERT_VALUES_EQUAL(indexes.size(), names.size());
    size_t i = 0;
    for (const char* name : names) {
        UNIT_ASSERT_VALUES_EQUAL(table.Columns[indexes[i]].Name, name);
        ++i;
    }
}

void AssertFill(const TTable& table, const TVector<size_t>& indexes, EFill fill) {
    for (size_t index : indexes) {
        UNIT_ASSERT(table.Columns[index].Fill == fill);
    }
}

Y_UNIT_TEST(FixedUpdateShapes) {
    for (const TTable& table : AllTables()) {
        const TVector<size_t> values = ValueIndexes(table);
        UNIT_ASSERT(values.size() >= 4);
        const TVector<TVector<size_t>> shapes = UpdateValueColumnSets(table);
        UNIT_ASSERT_VALUES_EQUAL(shapes.size(), 5);
        UNIT_ASSERT_VALUES_EQUAL(shapes[0].size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(shapes[1].size(), 1);
        UNIT_ASSERT(shapes[0][0] != shapes[1][0]);
        UNIT_ASSERT(shapes[2] != shapes[3]);
        UNIT_ASSERT_VALUES_EQUAL(shapes[4].size(), values.size());
        for (size_t i = 0; i < values.size(); ++i) {
            UNIT_ASSERT_VALUES_EQUAL(shapes[4][i], values[i]);
        }
        if (table.Wide) {
            UNIT_ASSERT(table.Columns[shapes[0][0]].Fill == EFill::Wide);
            UNIT_ASSERT(table.Columns[shapes[1][0]].Fill == EFill::None);
            UNIT_ASSERT_VALUES_EQUAL(shapes[2].size(), 4);
            UNIT_ASSERT_VALUES_EQUAL(shapes[3].size(), 4);
            UNIT_ASSERT(table.Columns[shapes[2].back()].Fill == EFill::Medium);
            AssertFill(table, {shapes[2][0], shapes[2][1], shapes[2][2]}, EFill::None);
            AssertFill(table, shapes[3], EFill::None);
        } else {
            UNIT_ASSERT_VALUES_EQUAL(shapes[0][0], values[0]);
            UNIT_ASSERT_VALUES_EQUAL(shapes[1][0], values[1]);
            UNIT_ASSERT_VALUES_EQUAL(shapes[2].size(), 3);
            UNIT_ASSERT_VALUES_EQUAL(shapes[3].size(), 3);
            UNIT_ASSERT_VALUES_EQUAL(shapes[2][0], values[0]);
            UNIT_ASSERT_VALUES_EQUAL(shapes[2][1], values[1]);
            UNIT_ASSERT_VALUES_EQUAL(shapes[2][2], values[2]);
            const size_t n = values.size();
            UNIT_ASSERT_VALUES_EQUAL(shapes[3][0], values[n - 3]);
            UNIT_ASSERT_VALUES_EQUAL(shapes[3][1], values[n - 2]);
            UNIT_ASSERT_VALUES_EQUAL(shapes[3][2], values[n - 1]);
        }

        const TString fullName = JoinPath("/db/replication", table.Name);
        const THashSet<TString> texts = UpdateStatements(table, fullName);
        ui32 oneColumn = 0;
        ui32 partial = 0;
        ui32 allColumns = 0;
        const ui32 partialColumns = table.Wide ? 5 : 4;
        for (const TString& text : texts) {
            UNIT_ASSERT(text.Contains("seqno = $seqno"));
            const ui32 assigned = UpdateAssignmentCount(text);
            if (assigned == 2) {
                ++oneColumn;
            } else if (assigned == partialColumns) {
                ++partial;
            } else {
                UNIT_ASSERT_VALUES_EQUAL(assigned, values.size() + 1);
                ++allColumns;
            }
        }
        UNIT_ASSERT_VALUES_EQUAL(oneColumn, 2);
        UNIT_ASSERT_VALUES_EQUAL(partial, 2);
        UNIT_ASSERT_VALUES_EQUAL(allColumns, 1);
    }

    const TTable* plain = Find("pk_uint64");
    const TTable* wide = Find("wide_utf8");
    const TTable* boolean = Find("pk_bool");
    UNIT_ASSERT(plain && wide && boolean);
    const TVector<TVector<size_t>> plainShapes = UpdateValueColumnSets(*plain);
    AssertColumnNames(*plain, plainShapes[0], {"c_bool"});
    AssertColumnNames(*plain, plainShapes[1], {"c_int64"});
    AssertColumnNames(*plain, plainShapes[2], {"c_bool", "c_int64", "c_double"});
    AssertColumnNames(*plain, plainShapes[3], {"c_string", "c_json", "c_timestamp"});
    const TVector<TVector<size_t>> wideShapes = UpdateValueColumnSets(*wide);
    AssertColumnNames(*wide, wideShapes[0], {"c_wide"});
    AssertColumnNames(*wide, wideShapes[1], {"c_bool"});
    AssertColumnNames(*wide, wideShapes[2], {"c_bool", "c_int8", "c_int16", "c_medium"});
    AssertColumnNames(*wide, wideShapes[3], {"c_bool", "c_int8", "c_int16", "c_int32"});
    const TString boolQuery = *UpdateStatements(*boolean, "/db/replication/pk_bool").begin();
    UNIT_ASSERT(boolQuery.Contains("pk_seq_hash = $pk_seq_hash"));
    UNIT_ASSERT(boolQuery.Contains("pk = $pk"));
    UNIT_ASSERT(boolQuery.Contains("seq = $seq"));
}

Y_UNIT_TEST(ReservationKeepsDensePrefix) {
    TVector<TTableRuntime> state(1);
    TVector<TReservation> reserved;
    UNIT_ASSERT(TryReserve(state, {{0, 3}}, reserved));
    UNIT_ASSERT(state[0].InsertBusy);
    UNIT_ASSERT_VALUES_EQUAL(reserved[0].First, 0);
    UNIT_ASSERT(!TryReserve(state, {{0, 1}}, reserved));
    ApplyReservations(state, reserved, false);
    UNIT_ASSERT(!state[0].InsertBusy);
    UNIT_ASSERT_VALUES_EQUAL(state[0].Boundary, -1);
    UNIT_ASSERT(TryReserve(state, {{0, 2}}, reserved));
    ApplyReservations(state, reserved, true);
    UNIT_ASSERT_VALUES_EQUAL(state[0].Boundary, 1);
}

}

} // namespace NYdb::NConsoleClient::NReplicationWorkload
