#include <ydb/public/lib/ydb_cli/commands/replication_workload/replication_workload_exec.h>
#include <ydb/public/lib/ydb_cli/commands/replication_workload/replication_workload_model.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <library/cpp/testing/unittest/registar.h>

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
        } else if (table.SeqnoHashPrefix) {
            ++ordinary;
            UNIT_ASSERT_VALUES_EQUAL(table.Columns.size(), 11);
            UNIT_ASSERT_VALUES_EQUAL(table.Columns[0].Name, "seqno_hash");
            UNIT_ASSERT(table.Columns[0].Key);
        } else {
            ++ordinary;
            UNIT_ASSERT_VALUES_EQUAL(table.Columns.size(), 10);
        }
        if (table.Composite) {
            ++composite;
            UNIT_ASSERT(table.Columns[table.SeqnoHashPrefix ? 2 : 1].Name == "seq");
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

Y_UNIT_TEST(IndexesAndPartitions) {
    const TTable* narrow = Find("pk_bool");
    const TTable* plain = Find("pk_uint64");
    const TTable* wide = Find("wide_json");
    UNIT_ASSERT(narrow && plain && wide);
    const TString narrowDdl = CreateTableQuery("/db/replication/pk_bool", *narrow, 3, 4);
    UNIT_ASSERT(narrowDdl.Contains("INDEX seqno_idx GLOBAL SYNC ON (seqno)"));
    UNIT_ASSERT(narrowDdl.Contains("INDEX seq_idx GLOBAL SYNC ON (seq)"));
    UNIT_ASSERT(narrowDdl.Contains("PRIMARY KEY (seqno_hash, pk, seq)"));
    UNIT_ASSERT(!narrowDdl.Contains("UNIFORM_PARTITIONS"));
    UNIT_ASSERT(narrowDdl.Contains("AUTO_PARTITIONING_BY_LOAD = ENABLED"));
    UNIT_ASSERT(narrowDdl.Contains("AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 3"));
    UNIT_ASSERT(narrowDdl.Contains("AUTO_PARTITIONING_MAX_PARTITIONS_COUNT = 4"));
    UNIT_ASSERT_VALUES_EQUAL(AlterIndexQueries("/db/replication/pk_bool", *narrow, 3, 4).size(), 2);

    const TString plainDdl = CreateTableQuery("/db/replication/pk_uint64", *plain, 1000, 1100);
    UNIT_ASSERT(!plainDdl.Contains("seq_idx"));
    UNIT_ASSERT(!plainDdl.Contains("UNIFORM_PARTITIONS"));
    UNIT_ASSERT_VALUES_EQUAL(AlterIndexQueries("/db/replication/pk_uint64", *plain, 1000, 1100).size(), 1);

    const TString wideDdl = CreateTableQuery("/db/replication/wide_json", *wide, 8, 9);
    UNIT_ASSERT(wideDdl.Contains("c_medium Json"));
    UNIT_ASSERT(wideDdl.Contains("c_wide Json"));
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
    auto boolKey = MakeKeyParams(*boolean, 0, HashSeqno(0), "k");
    UNIT_ASSERT_VALUES_EQUAL(boolKey[0].U, HashSeqno(0));
    UNIT_ASSERT(!boolKey[1].Bool);
    boolKey = MakeKeyParams(*boolean, 1, HashSeqno(9), "k");
    UNIT_ASSERT_VALUES_EQUAL(boolKey[0].U, HashSeqno(9));
    UNIT_ASSERT(boolKey[1].Bool);
    UNIT_ASSERT_VALUES_EQUAL(boolKey[2].U, 1);
    UNIT_ASSERT(HashSeqno(1) != HashSeqno(2));

    const TString digest = HashBytes(42);
    UNIT_ASSERT_VALUES_EQUAL(digest.size(), 32);
    UNIT_ASSERT_VALUES_EQUAL(HashHex(42).size(), 64);
    UNIT_ASSERT(HashBytes(1) != HashBytes(2));
    UNIT_ASSERT_VALUES_EQUAL(MakeKeyParams(*bytes, 7, 0, "k")[0].Text, HashBytes(7));
    UNIT_ASSERT_VALUES_EQUAL(MakeKeyParams(*text, 7, 0, "k")[0].Text, HashHex(7));

    const ui64 firstNumber = MakeKeyParams(*uint64, 1, 0, "k")[0].U;
    const ui64 secondNumber = MakeKeyParams(*uint64, 2, 0, "k")[0].U;
    UNIT_ASSERT((firstNumber >> 63) != (secondNumber >> 63));

    const TString first = MakeKeyParams(*uuid, 1, 0, "k")[0].Text;
    const TString second = MakeKeyParams(*uuid, 2, 0, "k")[0].Text;
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

Y_UNIT_TEST(ImportBatchUsesAsTable) {
    const TTable* boolean = Find("pk_bool");
    const TTable* wide = Find("wide_utf8");
    UNIT_ASSERT(boolean);
    UNIT_ASSERT(wide);
    const TString query = ImportQuery("/db/replication/pk_bool", *boolean);
    UNIT_ASSERT(query.Contains("DECLARE $input AS List<Struct<"));
    UNIT_ASSERT(query.Contains("seqno_hash: Uint64"));
    UNIT_ASSERT(query.Contains("pk: Bool"));
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
    UNIT_ASSERT_VALUES_EQUAL(rows[0][0].U, HashSeqno(0));
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
    UNIT_ASSERT(type.Contains("seqno_hash"));
    TValueParser parser(*value);
    parser.OpenList();
    ui32 items = 0;
    while (parser.TryNextListItem()) {
        ++items;
    }
    UNIT_ASSERT_VALUES_EQUAL(items, 2);
}

Y_UNIT_TEST(UpdateTouchesOneToThreeColumnsAndFullKey) {
    const TVector<TTable>& tables = AllTables();
    TVector<TTableRuntime> state(tables.size());
    for (TTableRuntime& table : state) {
        table.Boundary = 9;
    }
    std::mt19937_64 rng(5);
    const TTxPlan plan = BuildTransaction(tables, state, "/db/replication", 100, 4, rng);
    UNIT_ASSERT(!plan.Busy);
    UNIT_ASSERT(!plan.Writes.empty());
    bool sawUpdate = false;
    for (const TRowWrite& write : plan.Writes) {
        if (!write.Update) {
            UNIT_ASSERT(write.Statement.Contains("UPSERT INTO"));
            continue;
        }
        sawUpdate = true;
        UNIT_ASSERT(write.Statement.Contains("UPDATE "));
        UNIT_ASSERT(write.Statement.Contains("seqno = $"));
        UNIT_ASSERT(write.Statement.Contains(" WHERE "));
        const size_t setPos = write.Statement.find(" SET ");
        const size_t wherePos = write.Statement.find(" WHERE ");
        const TString assignments = write.Statement.substr(setPos, wherePos - setPos);
        ui32 commas = 0;
        for (char ch : assignments) {
            if (ch == ',') {
                ++commas;
            }
        }
        UNIT_ASSERT(commas <= 2);
        if (write.Statement.Contains("pk_bool")) {
            UNIT_ASSERT(write.Statement.Contains("seqno_hash = $"));
            UNIT_ASSERT(write.Statement.Contains("pk = $"));
            UNIT_ASSERT(write.Statement.Contains("seq = $"));
        }
    }
    UNIT_ASSERT(sawUpdate);
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
