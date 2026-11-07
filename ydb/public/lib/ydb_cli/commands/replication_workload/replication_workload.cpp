#include "replication_workload.h"

namespace NYdb::NConsoleClient {

TCommandWorkloadReplication::TCommandWorkloadReplication()
    : TClientCommandTree("replication", {}, "Workload for testing asynchronous replication of row tables")
{
    AddCommand(std::make_unique<TCommandWorkloadReplicationInit>());
    AddCommand(std::make_unique<TCommandWorkloadReplicationImport>());
    AddCommand(std::make_unique<TCommandWorkloadReplicationRun>());
    AddCommand(std::make_unique<TCommandWorkloadReplicationClean>());
}

} // namespace NYdb::NConsoleClient
