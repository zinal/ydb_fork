#include "replication_workload_text.h"

#ifndef LINUX
#define LINUX
#endif

#include <ydb/library/workload/tpch/driver.h>

#include <mutex>
#include <vector>

namespace NYdb::NConsoleClient::NReplicationWorkload {
namespace {

// Stream 5 is the dbgen pregeneration stream. Slices use another stream so
// the pool build does not consume the seed of the requested slice.
constexpr int TextStream = 40;
constexpr ui64 RngModulus = 2147483646ull;

std::mutex TextMutex;
bool TextReady = false;

extern "C" seed_t Seed[];

void EnsureTextReady() {
    if (TextReady) {
        return;
    }
    InitTpchGen(1);
    char scratch[2];
    dbg_text(scratch, 1, 1, TextStream);
    TextReady = true;
}

} // namespace

void PrepareTpchText() {
    std::lock_guard<std::mutex> lock(TextMutex);
    EnsureTextReady();
}

TString TpchText(ui64 size, ui64 seed) {
    std::lock_guard<std::mutex> lock(TextMutex);
    EnsureTextReady();
    if (size == 0) {
        return {};
    }
    Seed[TextStream].value = static_cast<DSS_HUGE>(seed % RngModulus) + 1;
    std::vector<char> buffer(static_cast<size_t>(size) + 1, '\0');
    dbg_text(buffer.data(), static_cast<int>(size), static_cast<int>(size), TextStream);
    return TString(buffer.data(), static_cast<size_t>(size));
}

} // namespace NYdb::NConsoleClient::NReplicationWorkload
