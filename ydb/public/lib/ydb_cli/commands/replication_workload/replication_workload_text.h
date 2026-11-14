#pragma once

#include <util/generic/string.h>
#include <util/system/types.h>

namespace NYdb::NConsoleClient::NReplicationWorkload {

// Load the TPC-H word lists and build the text pool. Later calls are cheap.
void PrepareTpchText();

// A slice of that pool, `size` bytes long. The slice is a function of `seed`.
TString TpchText(ui64 size, ui64 seed);

} // namespace NYdb::NConsoleClient::NReplicationWorkload
