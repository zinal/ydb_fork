#pragma once

#include <util/system/types.h>

namespace NKikimr::NPDisk {

class TPDisk;
struct TMainKey;

// Set from the "run --pdisk-grow-to-device-size" command-line flag.
void SetGrowToDeviceSizeEnabled(bool enabled);
bool GrowToDeviceSizeEnabled();

// True when this boot may rewrite the format: the command-line flag is on,
// or a previous grow left the on-disk "short chunk map" bit set.
bool BootMayRewriteFormat(const TPDisk& disk);

// The format main key is wiped on the actor before syslog read. Keep a copy until
// the boot either rewrites the format or gives up.
void RememberFormatMainKey(TPDisk* pdisk, const TMainKey& key);
void ForgetFormatMainKey(TPDisk* pdisk);

void NoteSysLogParse(TPDisk* pdisk, ui64 payloadBytes, bool mapCoversDisk);

// Owners of diskChunkCount fit in the record, or the format explicitly allows a
// shorter consistent map left by a crashed grow. On success mapChunkCount is set.
bool ResolveSysLogChunkCount(const void* record, ui64 recordSize, ui32 recordVersion,
    ui32 diskChunkCount, ui32 minChunkCount, ui64 formatFlags, ui32& mapChunkCount);

// Drop the "short map is allowed" bit once the syslog already describes the whole disk.
void ClearShortChunkMapAllowance(TPDisk& disk);

// If the underlying file or device contains more whole chunks than the format,
// and one new syslog record still fits into the existing syslog ring, append free chunks.
void TryGrowToDeviceSize(TPDisk& disk);

} // NKikimr::NPDisk
