#include "blobstorage_pdisk_grow.h"
#include "blobstorage_pdisk_impl.h"

#include <ydb/core/blobstorage/crypto/secured_block.h>
#include <ydb/library/pdisk_io/file_params.h>

#include <util/generic/ylimits.h>
#include <util/system/mutex.h>
#include <util/system/unaligned_mem.h>

#include <atomic>
#include <unordered_map>

namespace NKikimr::NPDisk {

namespace {

// On-disk bit in TDiskFormat::FormatFlags. It is not part of the public flag enum:
// a value here means the formatted DiskSize may be ahead of the chunk map stored
// in the syslog, because a previous boot crashed between those two writes.
constexpr ui64 FormatFlagShortChunkMap = 1ull << 8;

std::atomic<bool> GrowEnabled{false};

struct TBootState {
    TMainKey Key;
    bool HasKey = false;
    ui64 PayloadBytes = 0;
    bool MapCoversDisk = true;
};

TMutex BootStateLock;
std::unordered_map<TPDisk*, TBootState> BootStates;

TBootState* FindState(TPDisk* pdisk) {
    auto it = BootStates.find(pdisk);
    return it == BootStates.end() ? nullptr : &it->second;
}

void WipeKey(TMainKey& key) {
    if (!key.Keys.empty()) {
        SecureWipeBuffer(reinterpret_cast<ui8*>(key.Keys.data()), sizeof(TKey) * key.Keys.size());
        key.Keys.clear();
    }
    key.IsInitialized = false;
}

ui32 SectorSetsForPayload(const TDiskFormat& format, ui64 payload) {
    if (format.SectorSize <= CanarySize + sizeof(TDataSectorFooter)) {
        return Max<ui32>();
    }
    const ui32 sectorPayload = format.SectorSize - CanarySize - sizeof(TDataSectorFooter);
    if (sectorPayload <= sizeof(TFirstLogPageHeader) || sectorPayload <= sizeof(TLogPageHeader)) {
        return Max<ui32>();
    }
    const ui32 first = sectorPayload - sizeof(TFirstLogPageHeader);
    const ui32 extra = sectorPayload - sizeof(TLogPageHeader);
    if (payload <= first) {
        return 1;
    }
    const ui64 rest = payload - first;
    const ui64 sets = 1 + (rest + extra - 1) / extra;
    if (sets > Max<ui32>()) {
        return Max<ui32>();
    }
    return static_cast<ui32>(sets);
}

ui64 PayloadBytesForChunkCount(ui32 chunkCount, ui32 compatibilityInfoSize) {
    return sizeof(TSysLogRecord)
        + ui64(sizeof(TChunkInfo)) * chunkCount
        + sizeof(TSysLogFirstNoncesToKeep)
        + sizeof(ui64)
        + TChunkTrimInfo::SizeForChunkCount(chunkCount)
        + sizeof(ui32)
        + sizeof(ui32)
        + compatibilityInfoSize;
}

ui32 CompatibilityInfoBytes(TPDisk& disk) {
    if (!disk.SerializedCompatibilityInfo) {
        TString serialized;
        auto stored = CompatibilityInfo.MakeStored(NKikimrConfig::TCompatibilityRule::PDisk);
        const bool ok = stored.SerializeToString(&serialized);
        Y_ABORT_UNLESS(ok);
        disk.SerializedCompatibilityInfo.emplace(std::move(serialized));
    }
    return disk.SerializedCompatibilityInfo->size();
}

bool OwnersFit(ui64 recordSize, ui32 chunkCount) {
    const ui64 need = sizeof(TSysLogRecord) + ui64(sizeof(TChunkInfo)) * chunkCount;
    return recordSize >= need;
}

// Version 7 record layout written by WriteSysLogRestorePoint, with no bytes past the proto.
bool SysLogV7MatchesChunkCount(const ui8* data, ui64 size, ui32 chunkCount) {
    ui64 offset = sizeof(TSysLogRecord) + ui64(sizeof(TChunkInfo)) * chunkCount + sizeof(TSysLogFirstNoncesToKeep);
    if (size < offset + sizeof(ui64) + sizeof(ui32) + sizeof(ui32)) {
        return false;
    }
    const ui64 trimBytes = ReadUnaligned<ui64>(data + offset);
    if (trimBytes != TChunkTrimInfo::SizeForChunkCount(chunkCount)) {
        return false;
    }
    offset += sizeof(ui64) + trimBytes;
    if (size < offset + sizeof(ui32) + sizeof(ui32)) {
        return false;
    }
    offset += sizeof(ui32); // FirstLogChunkToParseCommits
    const ui32 protoSize = ReadUnaligned<ui32>(data + offset);
    offset += sizeof(ui32);
    return protoSize > 0 && offset + protoSize == size;
}

bool FindShorterChunkCount(const ui8* data, ui64 size, ui32 diskChunkCount, ui32 minChunkCount, ui32& found) {
    if (diskChunkCount <= minChunkCount || size <= sizeof(TSysLogRecord)) {
        return false;
    }
    ui64 hi = diskChunkCount - 1;
    const ui64 maxBySize = (size - sizeof(TSysLogRecord)) / sizeof(TChunkInfo);
    if (hi > maxBySize) {
        hi = maxBySize;
    }
    if (hi < minChunkCount) {
        return false;
    }
    for (ui64 count = hi;; --count) {
        if (SysLogV7MatchesChunkCount(data, size, static_cast<ui32>(count))) {
            found = static_cast<ui32>(count);
            return true;
        }
        if (count == minChunkCount) {
            break;
        }
    }
    return false;
}

bool ReadLiveDeviceBytes(TPDisk& disk, ui64& liveBytes) {
    liveBytes = 0;
    if (disk.Cfg->SectorMap) {
        liveBytes = disk.Cfg->SectorMap->GetDeviceSize();
        return liveBytes > 0;
    }
    if (disk.Cfg->Path.empty()) {
        return false;
    }
    try {
        bool isBlockDevice = false;
        ::NKikimr::DetectFileParameters(disk.Cfg->Path, liveBytes, isBlockDevice);
    } catch (const std::exception& ex) {
        auto& PCtx = disk.PCtx;
        P_LOG(PRI_WARN, BPD01, "PDisk grow skipped, device size is unavailable",
            (Path, disk.Cfg->Path),
            (Error, ex.what()));
        liveBytes = 0;
        return false;
    }
    return liveBytes > 0;
}

TKey CurrentFormatKey(const TMainKey& key) {
    return key.Keys.back();
}

bool RewriteFormat(TPDisk& disk, const TDiskFormat& updated, const TKey& mainKey) {
    const TDiskFormat saved = disk.Format;
    try {
        disk.WriteApplyFormatRecord(updated, mainKey);
        return true;
    } catch (const std::exception& ex) {
        disk.Format = saved;
        auto& PCtx = disk.PCtx;
        P_LOG(PRI_ERROR, BPD01, "PDisk format rewrite failed", (Error, ex.what()));
        return false;
    } catch (...) {
        disk.Format = saved;
        auto& PCtx = disk.PCtx;
        P_LOG(PRI_ERROR, BPD01, "PDisk format rewrite failed");
        return false;
    }
}

} // namespace

void SetGrowToDeviceSizeEnabled(bool enabled) {
    GrowEnabled.store(enabled, std::memory_order_relaxed);
}

bool GrowToDeviceSizeEnabled() {
    return GrowEnabled.load(std::memory_order_relaxed);
}

bool BootMayRewriteFormat(const TPDisk& disk) {
    return GrowToDeviceSizeEnabled() || (disk.Format.FormatFlags & FormatFlagShortChunkMap) != 0;
}

void RememberFormatMainKey(TPDisk* pdisk, const TMainKey& key) {
    TGuard<TMutex> guard(BootStateLock);
    TBootState& state = BootStates[pdisk];
    WipeKey(state.Key);
    state.Key = key;
    state.HasKey = !key.Keys.empty();
}

void ForgetFormatMainKey(TPDisk* pdisk) {
    TGuard<TMutex> guard(BootStateLock);
    auto it = BootStates.find(pdisk);
    if (it == BootStates.end()) {
        return;
    }
    WipeKey(it->second.Key);
    BootStates.erase(it);
}

void NoteSysLogParse(TPDisk* pdisk, ui64 payloadBytes, bool mapCoversDisk) {
    TGuard<TMutex> guard(BootStateLock);
    TBootState& state = BootStates[pdisk];
    state.PayloadBytes = payloadBytes;
    state.MapCoversDisk = mapCoversDisk;
}

bool ResolveSysLogChunkCount(const void* record, ui64 recordSize, ui32 recordVersion,
        ui32 diskChunkCount, ui32 minChunkCount, ui64 formatFlags, ui32& mapChunkCount) {
    if (OwnersFit(recordSize, diskChunkCount)) {
        mapChunkCount = diskChunkCount;
        return true;
    }
    if ((formatFlags & FormatFlagShortChunkMap) == 0 || recordVersion < PDISK_SYS_LOG_RECORD_VERSION_7) {
        return false;
    }
    ui32 found = 0;
    if (!FindShorterChunkCount(static_cast<const ui8*>(record), recordSize, diskChunkCount, minChunkCount, found)) {
        return false;
    }
    mapChunkCount = found;
    return true;
}

void ClearShortChunkMapAllowance(TPDisk& disk) {
    if (disk.Cfg->ReadOnly || (disk.Format.FormatFlags & FormatFlagShortChunkMap) == 0) {
        return;
    }
    TMainKey key;
    {
        TGuard<TMutex> guard(BootStateLock);
        TBootState* state = FindState(&disk);
        if (!state || !state->HasKey || !state->MapCoversDisk) {
            return;
        }
        key = state->Key;
    }
    if (key.Keys.empty()) {
        return;
    }

    TDiskFormat updated = disk.Format;
    updated.FormatFlags &= ~FormatFlagShortChunkMap;
    updated.SetHash();
    auto& PCtx = disk.PCtx;
    if (RewriteFormat(disk, updated, CurrentFormatKey(key))) {
        P_LOG(PRI_NOTICE, BPD01, "PDisk format no longer accepts a short syslog chunk map",
            (DiskChunks, disk.Format.DiskSizeChunks()));
    }
    WipeKey(key);
}

void TryGrowToDeviceSize(TPDisk& disk) {
    auto& PCtx = disk.PCtx;
    if (!GrowToDeviceSizeEnabled() || disk.Cfg->ReadOnly) {
        return;
    }
    if (disk.Format.ChunkSize == 0 || disk.Format.SectorSize == 0) {
        return;
    }

    ui64 liveBytes = 0;
    if (!ReadLiveDeviceBytes(disk, liveBytes)) {
        return;
    }

    const ui32 oldChunks = disk.Format.DiskSizeChunks();
    if (liveBytes / disk.Format.ChunkSize <= oldChunks) {
        return;
    }
    if (liveBytes / disk.Format.ChunkSize > Max<ui32>()) {
        P_LOG(PRI_ERROR, BPD01, "PDisk grow skipped, device chunk count does not fit into 32 bits",
            (LiveBytes, liveBytes),
            (ChunkSize, disk.Format.ChunkSize));
        return;
    }
    const ui32 newChunks = static_cast<ui32>(liveBytes / disk.Format.ChunkSize);

    TMainKey key;
    ui64 oldPayload = 0;
    {
        TGuard<TMutex> guard(BootStateLock);
        TBootState* state = FindState(&disk);
        if (!state || !state->HasKey) {
            P_LOG(PRI_ERROR, BPD01, "PDisk grow skipped, format main key is not available");
            return;
        }
        key = state->Key;
        oldPayload = state->PayloadBytes;
    }
    if (key.Keys.empty()) {
        WipeKey(key);
        P_LOG(PRI_ERROR, BPD01, "PDisk grow skipped, format main key is empty");
        return;
    }

    const ui32 compatBytes = CompatibilityInfoBytes(disk);
    if (compatBytes == 0) {
        WipeKey(key);
        P_LOG(PRI_ERROR, BPD01, "PDisk grow skipped, compatibility record is empty");
        return;
    }
    if (oldPayload == 0) {
        oldPayload = PayloadBytesForChunkCount(oldChunks, compatBytes);
    }
    const ui64 newPayload = PayloadBytesForChunkCount(newChunks, compatBytes);
    const ui32 oldSets = SectorSetsForPayload(disk.Format, oldPayload);
    const ui32 newSets = SectorSetsForPayload(disk.Format, newPayload);
    // Leave the previous syslog record intact so the reader still sees two record starts,
    // and do not wrap onto the record being written.
    if (newSets >= disk.Format.SysLogSectorCount
            || ui64(newSets) + oldSets > disk.Format.SysLogSectorCount) {
        P_LOG(PRI_NOTICE, BPD01, "PDisk grow skipped, larger chunk map does not fit into the syslog ring",
            (OldChunks, oldChunks),
            (NewChunks, newChunks),
            (OldSectorSets, oldSets),
            (NewSectorSets, newSets),
            (SysLogSectorCount, disk.Format.SysLogSectorCount));
        WipeKey(key);
        return;
    }

    TDiskFormat updated = disk.Format;
    updated.DiskSize = ui64(newChunks) * disk.Format.ChunkSize;
    updated.FormatFlags |= FormatFlagShortChunkMap;
    updated.SetHash();
    if (!RewriteFormat(disk, updated, CurrentFormatKey(key))) {
        WipeKey(key);
        return;
    }

    {
        TGuard<TMutex> guard(disk.StateMutex);
        const ui32 previous = disk.ChunkState.size();
        // TChunkState contains atomics, so grow by constructing a new vector.
        TVector<TChunkState> grown(newChunks);
        for (ui32 chunk = 0; chunk < previous && chunk < newChunks; ++chunk) {
            TChunkState& src = disk.ChunkState[chunk];
            TChunkState& dst = grown[chunk];
            dst.Nonce = src.Nonce;
            dst.CurrentNonce = src.CurrentNonce;
            dst.PreviousNonce = src.PreviousNonce;
            dst.OperationsInProgress.store(src.OperationsInProgress.load());
            dst.OwnerId = src.OwnerId;
            dst.CommitState = src.CommitState;
            dst.IsDirty.store(src.IsDirty.load());
            dst.CommitsInProgress = src.CommitsInProgress;
            dst.ShredGeneration = src.ShredGeneration;
        }
        for (ui32 chunk = previous; chunk < newChunks; ++chunk) {
            grown[chunk].OwnerId = OwnerUnallocated;
            grown[chunk].CommitState = TChunkState::FREE;
            if (disk.ShredGeneration != 0) {
                grown[chunk].ShredGeneration = disk.ShredGeneration;
            }
        }
        disk.ChunkState.swap(grown);
        disk.DriveModel.SetTotalChunksCount(newChunks);
        if (disk.Mon.TotalSpaceBytes) {
            *disk.Mon.TotalSpaceBytes = disk.Format.DiskSize;
        }
    }

    P_LOG(PRI_NOTICE, BPD01, "PDisk grew to the device size",
        (OldChunks, oldChunks),
        (NewChunks, newChunks),
        (DiskSize, disk.Format.DiskSize),
        (AddedChunks, newChunks - oldChunks));
    WipeKey(key);
}

} // NKikimr::NPDisk
