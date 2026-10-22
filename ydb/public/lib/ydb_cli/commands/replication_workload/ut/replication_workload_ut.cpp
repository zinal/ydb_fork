#include <ydb/public/lib/ydb_cli/commands/replication_workload/replication_workload_model.h>

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
        } else {
            ++ordinary;
            UNIT_ASSERT_VALUES_EQUAL(table.Columns.size(), 10);
        }
        if (table.Composite) {
            ++composite;
            UNIT_ASSERT_VALUES_EQUAL(table.Columns[1].Name, "seq");
        }
        UNIT_ASSERT_VALUES_EQUAL(table.Columns[table.Composite ? 2 : 1].Name, "seqno");
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
    UNIT_ASSERT(!narrowDdl.Contains("UNIFORM_PARTITIONS"));
    UNIT_ASSERT(narrowDdl.Contains("AUTO_PARTITIONING_BY_LOAD = ENABLED"));
    UNIT_ASSERT(narrowDdl.Contains("AUTO_PARTITIONING_MIN_PARTITIONS_COUNT = 3"));
    UNIT_ASSERT(narrowDdl.Contains("AUTO_PARTITIONING_MAX_PARTITIONS_COUNT = 4"));
    UNIT_ASSERT_VALUES_EQUAL(AlterIndexQueries("/db/replication/pk_bool", *narrow, 3, 4).size(), 2);

    const TString plainDdl = CreateTableQuery("/db/replication/pk_uint64", *plain, 1000, 1100);
    UNIT_ASSERT(!plainDdl.Contains("seq_idx"));
    UNIT_ASSERT(plainDdl.Contains("UNIFORM_PARTITIONS = 1000"));
    UNIT_ASSERT_VALUES_EQUAL(AlterIndexQueries("/db/replication/pk_uint64", *plain, 1000, 1100).size(), 1);

    const TString wideDdl = CreateTableQuery("/db/replication/wide_json", *wide, 8, 9);
    UNIT_ASSERT(wideDdl.Contains("c_medium Json"));
    UNIT_ASSERT(wideDdl.Contains("c_wide Json"));
    UNIT_ASSERT(wideDdl.Contains("UNIFORM_PARTITIONS = 8"));
    const TTable* yson = Find("wide_yson");
    UNIT_ASSERT(CreateTableQuery("/db/wide_yson", *yson, 1, 2).Contains("c_medium Yson"));
}

Y_UNIT_TEST(KeysAndText) {
    const TTable* boolean = Find("pk_bool");
    const TTable* bytes = Find("pk_string");
    const TTable* text = Find("pk_utf8");
    const TTable* uuid = Find("pk_uuid");
    auto boolKey = MakeKeyParams(*boolean, 0, "k");
    UNIT_ASSERT(!boolKey[0].Bool);
    boolKey = MakeKeyParams(*boolean, 1, "k");
    UNIT_ASSERT(boolKey[0].Bool);
    UNIT_ASSERT_VALUES_EQUAL(boolKey[1].U, 1);

    const TString digest = HashBytes(42);
    UNIT_ASSERT_VALUES_EQUAL(digest.size(), 32);
    UNIT_ASSERT_VALUES_EQUAL(HashHex(42).size(), 64);
    UNIT_ASSERT(HashBytes(1) != HashBytes(2));
    UNIT_ASSERT_VALUES_EQUAL(MakeKeyParams(*bytes, 7, "k")[0].Text, HashBytes(7));
    UNIT_ASSERT_VALUES_EQUAL(MakeKeyParams(*text, 7, "k")[0].Text, HashHex(7));

    const TString first = MakeKeyParams(*uuid, 1, "k")[0].Text;
    const TString second = MakeKeyParams(*uuid, 2, "k")[0].Text;
    UNIT_ASSERT_VALUES_EQUAL(first.size(), 16);
    UNIT_ASSERT(first < second);
    UNIT_ASSERT_VALUES_EQUAL(static_cast<unsigned char>(first[8]), 0);
    UNIT_ASSERT_VALUES_EQUAL(static_cast<unsigned char>(first[15]), 1);

    std::mt19937_64 rng(1);
    const TString json = MakeText(EColType::Json, EFill::Narrow, rng);
    UNIT_ASSERT(json.size() >= 2 && json.size() <= 100);
    rng.seed(2);
    const TString yson = MakeText(EColType::Yson, EFill::Wide, rng);
    UNIT_ASSERT(yson.size() >= 1000 && yson.size() <= 5000);
    UNIT_ASSERT(yson.StartsWith("{v=\""));
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
