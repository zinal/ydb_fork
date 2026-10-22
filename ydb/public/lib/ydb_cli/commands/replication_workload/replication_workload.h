#pragma once

#include <ydb/public/lib/ydb_cli/commands/ydb_command.h>

namespace NYdb::NConsoleClient {

class TCommandWorkloadReplication : public TClientCommandTree {
public:
    TCommandWorkloadReplication();
};

class TCommandWorkloadReplicationInit : public TYdbCommand {
public:
    TCommandWorkloadReplicationInit();
    void Config(TConfig& config) override;
    int Run(TConfig& config) override;

private:
    TString Path = "replication";
    ui64 MinPartitions = 1000;
    ui64 MaxPartitions = 1100;
};

class TCommandWorkloadReplicationImport : public TYdbCommand {
public:
    TCommandWorkloadReplicationImport();
    void Config(TConfig& config) override;
    int Run(TConfig& config) override;

private:
    TString Path = "replication";
    ui64 Rows = 1000;
};

class TCommandWorkloadReplicationRun : public TYdbCommand {
public:
    TCommandWorkloadReplicationRun();
    void Config(TConfig& config) override;
    int Run(TConfig& config) override;

private:
    TString Path = "replication";
    ui64 Seconds = 60;
    ui64 Users = 1;
    ui64 MaxRecords = 10;
    TString Pause = "100ms";
    ui64 Threads = 0;
    ui64 WindowSec = 1;
    TString Seed;
    TString ClientTimeout = "10s";
    TString OperationTimeout = "5s";
    TString CancelAfter = "5s";
};

class TCommandWorkloadReplicationClean : public TYdbCommand {
public:
    TCommandWorkloadReplicationClean();
    void Config(TConfig& config) override;
    int Run(TConfig& config) override;

private:
    TString Path = "replication";
};

} // namespace NYdb::NConsoleClient
