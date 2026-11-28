#include "prx/libSceAgcDriver/Execution/include/DeviceFault.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int faultCalls = 0;
bool faultFails = false;
VkResult faultDataResult = VK_SUCCESS;
std::uint32_t faultDataAddressCount = 2;
std::uint64_t faultDataVendorBinarySize = 0;

VKAPI_ATTR VkResult VKAPI_CALL MockFaultInfo(VkDevice, VkDeviceFaultCountsEXT* counts, VkDeviceFaultInfoEXT* info) {
    ++faultCalls;
    if (faultFails) return VK_ERROR_DEVICE_LOST;
    if (info == nullptr) {
        counts->addressInfoCount = 2;
        counts->vendorInfoCount = 1;
        counts->vendorBinarySize = 4096;
        return VK_SUCCESS;
    }
    Require(counts->vendorBinarySize == 0, "the vendor binary was requested");
    Require(counts->addressInfoCount == 2 && info->pAddressInfos != nullptr, "the address array was not passed");
    Require(counts->vendorInfoCount == 1 && info->pVendorInfos != nullptr, "the vendor array was not passed");
    std::memset(info->description, 'd', sizeof(info->description));
    std::memcpy(info->description, "page fault", 10);
    info->description[10] = '\0';
    info->pAddressInfos[0] = {VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x1234000ull, 0x1000ull};
    info->pAddressInfos[1] = {VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT, 0xabcdull, 0x3ull};
    std::memset(info->pVendorInfos[0].description, 'v', sizeof(info->pVendorInfos[0].description));
    info->pVendorInfos[0].vendorFaultCode = 0x77ull;
    info->pVendorInfos[0].vendorFaultData = 0x88ull;
    counts->addressInfoCount = faultDataAddressCount;
    counts->vendorBinarySize = faultDataVendorBinarySize;
    return faultDataResult;
}

std::uint32_t checkpointCount = 0;

VKAPI_ATTR void VKAPI_CALL MockCheckpointData(VkQueue, std::uint32_t* count, VkCheckpointDataNV* data) {
    if (data == nullptr) {
        *count = checkpointCount;
        return;
    }
    Require(*count == checkpointCount, "the checkpoint array has the wrong size");
    for (std::uint32_t i = 0; i < *count; ++i) {
        Require(data[i].sType == VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV, "checkpoint entries were not initialised");
        data[i].stage = i == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        data[i].pCheckpointMarker = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x10000ull + i * 0x100ull));
    }
}

AgcDriver::FaultToken checkpointToken = 0;

VkCommandBuffer emitCmd = VK_NULL_HANDLE;
const void* emitMarker = nullptr;
std::uint32_t emitCalls = 0;

VKAPI_ATTR void VKAPI_CALL MockEmitCheckpoint(VkCommandBuffer commands, const void* marker) {
    ++emitCalls;
    emitCmd = commands;
    emitMarker = marker;
}

VKAPI_ATTR void VKAPI_CALL MockTokenCheckpointData(VkQueue, std::uint32_t* count, VkCheckpointDataNV* data) {
    if (data == nullptr) {
        *count = 1;
        return;
    }
    Require(*count == 1, "the token checkpoint array has the wrong size");
    Require(data[0].sType == VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV, "token checkpoint entries were not initialised");
    data[0].stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    data[0].pCheckpointMarker = reinterpret_cast<void*>(static_cast<std::uintptr_t>(checkpointToken));
}

void ClaimBracketToken(AgcDriver::FaultTokenRing& ring, AgcDriver::FaultToken token, std::uint64_t serial, std::uint64_t program, std::uint64_t variant, const char* desc) {
    AgcDriver::FaultTokenEntry* slot = ring.Mutable(token);
    Require(slot != nullptr, "the claimed token has no slot");
    slot->submitSerial = serial;
    slot->programAddress = program;
    slot->variantId = variant;
    std::snprintf(slot->desc, sizeof(slot->desc), "%s", desc);
    slot->stage = 0;
    slot->argBuffer = 0;
    slot->argOffset = 0;
    slot->argStride = 0;
    for (std::uint32_t i = 0; i < 4; ++i) slot->argWords[i] = 0;
    slot->argCount = 0;
    std::atomic_ref<AgcDriver::FaultToken>(slot->token).store(token, std::memory_order_release);
}

void ClaimBracketTokenWithArgs(AgcDriver::FaultTokenRing& ring, AgcDriver::FaultToken token, std::uint64_t serial, std::uint64_t program, std::uint64_t variant, const char* desc, std::uint32_t stage, std::uint64_t argBuffer, std::uint32_t argOffset, std::uint32_t argStride, const std::uint32_t* argWords, std::uint32_t argCount) {
    AgcDriver::FaultTokenEntry* slot = ring.Mutable(token);
    Require(slot != nullptr, "the claimed token has no slot");
    slot->submitSerial = serial;
    slot->programAddress = program;
    slot->variantId = variant;
    std::snprintf(slot->desc, sizeof(slot->desc), "%s", desc);
    slot->stage = stage;
    slot->argBuffer = argBuffer;
    slot->argOffset = argOffset;
    slot->argStride = argStride;
    for (std::uint32_t i = 0; i < 4; ++i) slot->argWords[i] = 0;
    const std::uint32_t words = argCount < 4 ? argCount : 4;
    for (std::uint32_t i = 0; i < words; ++i) slot->argWords[i] = argWords[i];
    slot->argCount = words;
    std::atomic_ref<AgcDriver::FaultToken>(slot->token).store(token, std::memory_order_release);
}

AgcDriver::DeviceFaultResidencyProbe ResidencyProbeUnboundNow(std::uint64_t va) {
    static_cast<void>(va);
    AgcDriver::DeviceFaultResidencyProbe probe{};
    probe.boundNow = false;
    return probe;
}

VkDevice FakeDevice() {
    return reinterpret_cast<VkDevice>(static_cast<std::uintptr_t>(0x1000));
}

VkQueue FakeQueue() {
    return reinterpret_cast<VkQueue>(static_cast<std::uintptr_t>(0x2000));
}

void Diagnostics() {
    using AgcDriver::ChooseDeviceDiagnostics;
    constexpr VkDeviceDiagnosticsConfigFlagsNV all = VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV | VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_ERROR_REPORTING_BIT_NV;
    for (const bool configSupported : {false, true}) {
        for (const bool checkpointsSupported : {false, true}) {
            const auto off = ChooseDeviceDiagnostics(false, configSupported, checkpointsSupported);
            Require(!off.config && !off.checkpoints && off.flags == 0, "diagnostics were chosen without APS5_NV_DIAGNOSTICS");
        }
    }
    for (const bool checkpointsSupported : {false, true}) {
        const auto unsupported = ChooseDeviceDiagnostics(true, false, checkpointsSupported);
        Require(!unsupported.config && !unsupported.checkpoints && unsupported.flags == 0, "diagnostics were chosen on a device without VK_NV_device_diagnostics_config");
    }
    const auto alone = ChooseDeviceDiagnostics(true, true, false);
    Require(alone.config && !alone.checkpoints && alone.flags == all, "the diagnostics config without checkpoint support did not enable every crash-dump flag alone");
    const auto both = ChooseDeviceDiagnostics(true, true, true);
    Require(both.config && both.checkpoints && both.flags == all, "the diagnostics config did not add the diagnostic checkpoints the device supports");
}

void FaultInfo() {
    faultCalls = 0;
    faultFails = false;
    faultDataResult = VK_SUCCESS;
    faultDataAddressCount = 2;
    faultDataVendorBinarySize = 0;
    const auto lines = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr);
    Require(faultCalls == 2, "the fault info was not queried for counts and then for data");
    Require(lines.size() == 4, "the report does not have a result, two addresses and a vendor line");
    Require(lines[0] == "[device-fault] result 0: page fault", "the description line changed");
    Require(lines[1] == "[device-fault] read invalid at 0x1234000 (precision 0x1000)", "the read fault line changed");
    Require(lines[2] == "[device-fault] instruction pointer fault at 0xabcd (precision 0x3)", "the instruction pointer line changed");
    Require(lines[3] == "[device-fault] vendor: " + std::string(VK_MAX_DESCRIPTION_SIZE, 'v') + " (code 0x77, data 0x88)", "an unterminated vendor description was not bounded");
}

void FaultInfoFails() {
    faultCalls = 0;
    faultFails = true;
    const auto lines = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr);
    Require(faultCalls == 1, "a failed count query was followed by a data query");
    Require(lines.size() == 1 && lines[0] == "[device-fault] vkGetDeviceFaultInfoEXT reported nothing", "a failed query was not reported");
}

void Checkpoints() {
    checkpointCount = 2;
    auto lines = AgcDriver::DeviceFaultReport(VK_NULL_HANDLE, nullptr, FakeQueue(), MockCheckpointData);
    Require(lines.size() == 2, "each checkpoint gets one line");
    Require(lines[0] == "[device-fault] checkpoint: program 0x10000 at stage 0x1", "the first checkpoint line changed");
    Require(lines[1] == "[device-fault] checkpoint: program 0x10100 at stage 0x2000", "the second checkpoint line changed");
    checkpointCount = 0;
    lines = AgcDriver::DeviceFaultReport(VK_NULL_HANDLE, nullptr, FakeQueue(), MockCheckpointData);
    Require(lines.size() == 1 && lines[0] == "[device-fault] no checkpoint reached", "an empty checkpoint list was not reported");
}

void Nothing() {
    faultCalls = 0;
    Require(AgcDriver::DeviceFaultReport(FakeDevice(), nullptr, FakeQueue(), nullptr).empty(), "a device without either extension reported something");
    Require(faultCalls == 0, "the fault info was queried without the extension");
}

std::vector<ShaderRecompiler::BdaAbi::Range> SnapshotRanges() {
    return {
        {0x10000ull, 0x11000ull, 0x10000ull, ShaderRecompiler::BdaAbi::Read, 0},
        {0x12000ull, 0x13000ull, 0x12000ull, ShaderRecompiler::BdaAbi::Read | ShaderRecompiler::BdaAbi::Write, 0},
    };
}

void SubmissionSnapshots() {
    AgcDriver::SubmissionSnapshots snapshots;
    Require(!snapshots.Find(0).has_value(), "serial zero found a snapshot");
    Require(!snapshots.Find(7).has_value(), "a missing serial found a snapshot");
    const auto ranges = std::make_shared<const std::vector<ShaderRecompiler::BdaAbi::Range>>(SnapshotRanges());
    snapshots.NoteSubmit(0, 9, ranges);
    Require(!snapshots.Find(0).has_value(), "serial zero kept a snapshot");
    snapshots.NoteSubmit(7, 3, ranges);
    const auto found = snapshots.Find(7);
    Require(found.has_value() && found->submitSerial == 7 && found->spaceSerial == 3 && found->ranges == ranges, "the noted snapshot was not found");
    snapshots.NoteSubmit(7 + AgcDriver::SubmissionSnapshots::Capacity, 4, ranges);
    Require(!snapshots.Find(7).has_value(), "an evicted serial kept its snapshot");
    const auto evictor = snapshots.Find(7 + AgcDriver::SubmissionSnapshots::Capacity);
    Require(evictor.has_value() && evictor->spaceSerial == 4 && evictor->ranges == ranges, "the evicting snapshot was not found");
    snapshots.NoteSubmit(0x100000005ull, 5, ranges);
    Require(!snapshots.Find(5).has_value(), "a truncated marker matched a wider serial");
    Require(snapshots.Find(0x100000005ull).has_value(), "a wide serial was not found exactly");
}

void SubmissionSnapshotsConcurrent() {
    AgcDriver::SubmissionSnapshots snapshots;
    std::atomic<bool> stop{false};
    std::atomic<bool> readersReady{false};
    std::atomic<std::uint64_t> published{0};
    std::thread writer([&] {
        while (!readersReady.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t serial = 1; serial <= 500; ++serial) {
            auto owned = std::make_shared<std::vector<ShaderRecompiler::BdaAbi::Range>>();
            owned->push_back({serial * 0x1000ull, serial * 0x1000ull + 0x100ull, serial * 0x1000ull, ShaderRecompiler::BdaAbi::Read, 0});
            snapshots.NoteSubmit(serial, serial + 1, owned);
            published.store(serial, std::memory_order_release);
        }
        stop.store(true, std::memory_order_release);
    });
    auto reader = [&] {
        readersReady.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_acquire)) {
            const std::uint64_t serial = published.load(std::memory_order_acquire);
            if (serial == 0) continue;
            const auto found = snapshots.Find(serial);
            if (!found.has_value()) continue;
            Require(found->submitSerial == serial && found->spaceSerial == serial + 1 && found->ranges != nullptr && found->ranges->size() == 1 && (*found->ranges)[0].begin == serial * 0x1000ull, "a concurrent snapshot read mixed serials or ranges");
        }
    };
    std::thread first(reader);
    std::thread second(reader);
    writer.join();
    first.join();
    second.join();
    for (std::uint64_t serial = 500 - AgcDriver::SubmissionSnapshots::Capacity + 1; serial <= 500; ++serial) {
        const auto found = snapshots.Find(serial);
        Require(found.has_value() && found->submitSerial == serial && found->ranges != nullptr && (*found->ranges)[0].begin == serial * 0x1000ull, "a published snapshot was lost or mixed");
    }
    Require(!snapshots.Find(500 - AgcDriver::SubmissionSnapshots::Capacity).has_value(), "an evicted snapshot was still found");
}

void FaultRangeHit() {
    const auto snapshot = SnapshotRanges();
    const auto lines = AgcDriver::DeviceFaultRangeLines(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0x12abcull, 0x100ull, snapshot, 7, 3);
    Require(lines.size() == 1, "a contained fault range reports more than its owner");
    Require(lines[0] == "[device-fault] write invalid at 0x12abc (precision 0x100) in guest range [0x12000, 0x13000) read write (batch 7, space 3)", "the owner line changed");
}

void FaultRangeStraddle() {
    const auto snapshot = SnapshotRanges();
    const auto lines = AgcDriver::DeviceFaultRangeLines(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0x10fffull, 0x2000ull, snapshot, 7, 3);
    Require(lines.size() == 2, "a fault range escaping its point owner does not report the miss and the next range");
    Require(lines[0] == "[device-fault] write invalid at 0x10fff (precision 0x2000) has no guest owner in batch 7 (internal or unmapped)", "the miss line changed");
    Require(lines[1] == "[device-fault] next guest range [0x12000, 0x13000) read write starts 0x1 after the fault range", "the next range line changed");
}

void FaultRangeMiss() {
    const auto snapshot = SnapshotRanges();
    auto gap = AgcDriver::DeviceFaultRangeLines(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x11800ull, 0, snapshot, 7, 3);
    Require(gap.size() == 3, "a fault in a gap does not report its neighbours");
    Require(gap[0] == "[device-fault] read invalid at 0x11800 (precision 0x0) has no guest owner in batch 7 (internal or unmapped)", "the gap miss line changed");
    Require(gap[1] == "[device-fault] previous guest range [0x10000, 0x11000) read ends 0x800 before the fault range", "the previous range line changed");
    Require(gap[2] == "[device-fault] next guest range [0x12000, 0x13000) read write starts 0x800 after the fault range", "the gap next line changed");
    auto low = AgcDriver::DeviceFaultRangeLines(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x500ull, 0x100ull, snapshot, 7, 3);
    Require(low.size() == 2 && low[1] == "[device-fault] next guest range [0x10000, 0x11000) read starts 0xfa01 after the fault range", "a fault below every range misses its next range");
    auto high = AgcDriver::DeviceFaultRangeLines(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x20000ull, 0, snapshot, 7, 3);
    Require(high.size() == 2 && high[1] == "[device-fault] previous guest range [0x12000, 0x13000) read write ends 0xd000 before the fault range", "a fault above every range misses its previous range");
    const std::vector<ShaderRecompiler::BdaAbi::Range> empty;
    auto none = AgcDriver::DeviceFaultRangeLines(VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT, 0x12000ull, 0, empty, 7, 3);
    Require(none.size() == 1 && none[0] == "[device-fault] read invalid at 0x12000 (precision 0x0) has no guest owner in batch 7 (internal or unmapped)", "an empty snapshot reports a neighbour");
}

void BatchSerialMismatch() {
    AgcDriver::SubmissionSnapshots snapshots;
    snapshots.NoteSubmit(7, 3, std::make_shared<const std::vector<ShaderRecompiler::BdaAbi::Range>>(SnapshotRanges()));
    const auto missing = AgcDriver::DeviceFaultBatchLines(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0x12abcull, 0x100ull, 8, snapshots);
    Require(missing.size() == 1 && missing[0] == "[device-fault] no guest snapshot for batch 8", "a serial without a snapshot was correlated");
    const auto hit = AgcDriver::DeviceFaultBatchLines(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0x12abcull, 0x100ull, 7, snapshots);
    Require(hit.size() == 1 && hit[0] == "[device-fault] write invalid at 0x12abc (precision 0x100) in guest range [0x12000, 0x13000) read write (batch 7, space 3)", "the batch snapshot was not correlated");
}

void BracketWindow() {
    AgcDriver::SubmissionSnapshots snapshots;
    snapshots.NoteSubmit(7, 3, std::make_shared<const std::vector<ShaderRecompiler::BdaAbi::Range>>(SnapshotRanges()));
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::FaultToken token = ring.Claim();
    Require(token != 0, "the claimed token is invalid");
    ClaimBracketToken(ring, token, 7, 0x12000ull, 9, "draw main");
    const AgcDriver::SubmitBracket bracket{7, 5, token, token};
    const auto lines = AgcDriver::DeviceFaultBatchLines(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0x12abcull, 0x100ull, 7, snapshots, bracket, ring);
    Require(lines.size() == 3, "a bracket window does not report the owner, the window and the token");
    Require(lines[0] == "[device-fault] write invalid at 0x12abc (precision 0x100) in guest range [0x12000, 0x13000) read write (batch 7, space 3)", "the batch snapshot was not correlated");
    Require(lines[1] == "[device-fault] bracket: last completed 5, in flight 7, first not started 8", "the serial window changed");
    char expected[256];
    std::snprintf(expected, sizeof(expected), "[device-fault] token %u in batch 7: program 0x12000 variant 9 draw main", static_cast<unsigned>(token));
    Require(lines[2] == expected, "the token line changed");
}

void BracketTokenMiss() {
    AgcDriver::SubmissionSnapshots snapshots;
    snapshots.NoteSubmit(7, 3, std::make_shared<const std::vector<ShaderRecompiler::BdaAbi::Range>>(SnapshotRanges()));
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::SubmitBracket bracket{7, 5, 0x0FFFFFFFu, 0x0FFFFFFFu};
    const auto lines = AgcDriver::DeviceFaultBatchLines(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0x12abcull, 0x100ull, 7, snapshots, bracket, ring);
    Require(lines.size() == 3, "a token miss does not report the owner, the window and the miss");
    Require(lines[0] == "[device-fault] write invalid at 0x12abc (precision 0x100) in guest range [0x12000, 0x13000) read write (batch 7, space 3)", "the batch snapshot was not correlated");
    Require(lines[1] == "[device-fault] bracket: last completed 5, in flight 7, first not started 8", "the serial window changed");
    char expected[256];
    std::snprintf(expected, sizeof(expected), "[device-fault] unknown token %u in batch 7", 0x0FFFFFFFu);
    Require(lines[2] == expected, "a token miss was not reported");
}

void BracketSerialMismatch() {
    AgcDriver::SubmissionSnapshots snapshots;
    snapshots.NoteSubmit(7, 3, std::make_shared<const std::vector<ShaderRecompiler::BdaAbi::Range>>(SnapshotRanges()));
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::SubmitBracket bracket{8, 5, 0, 0};
    const auto lines = AgcDriver::DeviceFaultBatchLines(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT, 0x12abcull, 0x100ull, 8, snapshots, bracket, ring);
    Require(lines.size() == 2, "a serial mismatch does not report the miss and the window");
    Require(lines[0] == "[device-fault] no guest snapshot for batch 8", "a serial without a snapshot was correlated");
    Require(lines[1] == "[device-fault] bracket: last completed 5, in flight 8, first not started 9", "the mismatch window changed");
}

void BracketEmptyTokens() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::SubmitBracket bracket{7, 5, 0, 0};
    const auto lines = AgcDriver::DeviceFaultBracketLines(bracket, ring);
    Require(lines.size() == 1, "an empty token range does not report only the window");
    Require(lines[0] == "[device-fault] bracket: last completed 5, in flight 7, first not started 8", "the empty token window changed");
}

void FaultResultZero() {
    faultCalls = 0;
    faultFails = false;
    faultDataResult = VK_SUCCESS;
    faultDataAddressCount = 0;
    faultDataVendorBinarySize = 0;
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::SubmitBracket bracket{7, 5, 0, 0};
    const auto lines = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr, 0, nullptr, bracket, ring);
    Require(faultCalls == 2, "a result-zero fault info was not queried for counts and then for data");
    Require(lines.size() == 5, "a result-zero fault does not report the window, the no-marker, the result, the vendor and the missing device data");
    Require(lines[0] == "[device-fault] bracket: last completed 5, in flight 7, first not started 8", "the result-zero window changed");
    Require(lines[1] == "[device-fault] no per-submit marker (submit serial unknown; markers need APS5_GPU_CHECKPOINTS on the main queue)", "a zero effective serial missed its no-marker line");
    Require(lines[2] == "[device-fault] result 0: page fault", "a zero result was not reported");
    Require(lines[3] == "[device-fault] vendor: " + std::string(VK_MAX_DESCRIPTION_SIZE, 'v') + " (code 0x77, data 0x88)", "the vendor line changed");
    Require(lines[4] == "[device-fault] no device-side fault data; CPU-side bracket only", "missing device-side data was not reported");
    faultDataAddressCount = 2;
}

void TokenCheckpoint() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::FaultToken token = ring.Claim();
    Require(token != 0, "the checkpoint token is invalid");
    ClaimBracketToken(ring, token, 7, 0x12000ull, 9, "draw main");
    checkpointToken = token;
    const AgcDriver::SubmitBracket bracket{7, 5, 0, 0};
    const auto lines = AgcDriver::DeviceFaultReport(VK_NULL_HANDLE, nullptr, FakeQueue(), MockTokenCheckpointData, 0, nullptr, bracket, ring);
    Require(lines.size() == 2, "a token checkpoint does not report the window and the checkpoint");
    Require(lines[0] == "[device-fault] bracket: last completed 5, in flight 7, first not started 8, queue 0x2000", "the checkpoint window changed");
    char expected[256];
    std::snprintf(expected, sizeof(expected), "[device-fault] checkpoint: token %u batch 7 queue 0x2000 program 0x12000 variant 9 draw main observed at bottom-of-pipe", static_cast<unsigned>(token));
    Require(lines[1] == expected, "the token checkpoint line changed");
    checkpointToken = 0;
}

void BatchBeginCheckpoint() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::FaultToken stale = ring.Claim();
    Require(stale != 0, "the stale token is invalid");
    ClaimBracketToken(ring, stale, 3, 0x9000ull, 4, "stale text of an earlier draw");
    AgcDriver::FaultTokenEntry* staleSlot = ring.Mutable(stale);
    staleSlot->argCount = 4;
    for (std::size_t i = 1; i < AgcDriver::FaultTokenRing::Capacity; ++i) ring.Claim();
    const AgcDriver::FaultToken token = AgcDriver::ClaimBatchBeginToken(ring, 7);
    Require(token != 0, "the batch-begin token is invalid");
    Require(static_cast<std::size_t>(token) % AgcDriver::FaultTokenRing::Capacity == static_cast<std::size_t>(stale) % AgcDriver::FaultTokenRing::Capacity, "the batch-begin token did not reuse the stale slot");
    const auto found = ring.Find(token);
    Require(found.has_value(), "the batch-begin token was not published");
    Require(found->submitSerial == 7 && found->programAddress == 0 && found->variantId == 0 && found->argCount == 0 && found->argBuffer == 0, "the batch-begin token kept fields of the earlier occupant of its slot");
    Require(std::strcmp(found->desc, "batch begin") == 0, "the batch-begin token description changed");
    checkpointToken = token;
    const AgcDriver::SubmitBracket bracket{7, 5, token, token};
    const auto lines = AgcDriver::DeviceFaultReport(VK_NULL_HANDLE, nullptr, FakeQueue(), MockTokenCheckpointData, 0, nullptr, bracket, ring);
    char expected[256];
    std::snprintf(expected, sizeof(expected), "[device-fault] checkpoint: token %u batch 7 queue 0x2000 program 0x0 variant 0 batch begin observed at bottom-of-pipe", static_cast<unsigned>(token));
    bool named = false;
    for (const auto& line : lines) named = named || line == expected;
    Require(named, "a device loss before the first draw does not name the batch by its begin token");
    checkpointToken = 0;
}

void ReportBracketOnly() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::SubmitBracket bracket{7, 5, 0, 0};
    const auto lines = AgcDriver::DeviceFaultReport(VK_NULL_HANDLE, nullptr, VK_NULL_HANDLE, nullptr, 0, nullptr, bracket, ring);
    Require(lines.size() == 2, "a report without device data does not report the window and the missing data");
    Require(lines[0] == "[device-fault] bracket: last completed 5, in flight 7, first not started 8", "the bracket-only window changed");
    Require(lines[1] == "[device-fault] no device-side fault data; CPU-side bracket only", "missing device-side data was not reported");
}

void ExtraQueueEmptyBracket() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::FaultToken token = ring.Claim();
    Require(token != 0, "the extra-queue token is invalid");
    ClaimBracketToken(ring, token, 7, 0x12000ull, 9, "draw main");
    checkpointToken = token;
    const AgcDriver::SubmitBracket bracket{};
    const auto lines = AgcDriver::DeviceFaultReport(VK_NULL_HANDLE, nullptr, FakeQueue(), MockTokenCheckpointData, 0, nullptr, bracket, ring);
    Require(lines.size() == 1, "an extra-queue report does not report only the checkpoint");
    char expected[256];
    std::snprintf(expected, sizeof(expected), "[device-fault] checkpoint: token %u batch 7 queue 0x2000 program 0x12000 variant 9 draw main observed at bottom-of-pipe", static_cast<unsigned>(token));
    Require(lines[0] == expected, "the extra-queue checkpoint line changed");
    checkpointToken = 0;
}

void TokenReuse() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    AgcDriver::FaultToken first = 0;
    for (std::size_t i = 0; i < AgcDriver::FaultTokenRing::Capacity + 5; ++i) {
        const AgcDriver::FaultToken token = ring.Claim();
        Require(token != 0, "the reuse token is invalid");
        if (i == 0) first = token;
        ClaimBracketToken(ring, token, 7, 0x12000ull, 9, "draw main");
    }
    Require(!ring.Find(first).has_value(), "an overwritten token still resolves");
}

void TokenEmission() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::FaultToken token = ring.Claim();
    Require(token != 0, "the emission token is invalid");
    ClaimBracketToken(ring, token, 7, 0x12000ull, 9, "draw main");
    emitCmd = VK_NULL_HANDLE;
    emitMarker = nullptr;
    emitCalls = 0;
    static VkCommandBuffer fakeCmd = reinterpret_cast<VkCommandBuffer>(static_cast<std::uintptr_t>(0xC0FFEE1));
    AgcDriver::Graphics::EmitNvCheckpointToken(fakeCmd, token, MockEmitCheckpoint);
    Require(emitCalls == 1, "the checkpoint backend was not called once");
    Require(emitCmd == fakeCmd, "the checkpoint backend got the wrong command buffer");
    Require(emitMarker == reinterpret_cast<const void*>(static_cast<std::uintptr_t>(token)), "the checkpoint backend got the wrong token");
    const auto found = ring.Find(token);
    Require(found.has_value() && found->token == token && found->submitSerial == 7, "an emitted token missed its ring entry");
}

void LiveResourceEmpty() {
    const auto lines = AgcDriver::LiveResourceRing::Instance().LinesFor(0x50000ull);
    Require(lines.empty(), "an empty live ring reports resources");
}

void LiveResourceHit() {
    AgcDriver::LiveResourceRing::Instance().Note(0x100000ull, 0x1000ull, "texture");
    AgcDriver::LiveResourceRing::Instance().Note(0x200000ull, 0x2000ull, "buffer");
    const auto lines = AgcDriver::LiveResourceRing::Instance().LinesFor(0x180000ull);
    Require(lines.size() == 2, "the nearest live resources were not listed");
    Require(lines[0] == "[device-fault] live resource 0x100000 size 0x1000 texture", "the nearest live resource changed");
    Require(lines[1] == "[device-fault] live resource 0x200000 size 0x2000 buffer", "the farther live resource changed");
}

void LiveResourceInReport() {
    AgcDriver::LiveResourceRing::Instance().Note(0x1230000ull, 0x10000ull, "texture");
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::SubmitBracket bracket{};
    const auto lines = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr, 0, nullptr, bracket, ring);
    bool found = false;
    for (const auto& line : lines) {
        if (line == "[device-fault] live resource 0x1230000 size 0x10000 texture") {
            found = true;
            break;
        }
    }
    Require(found, "a fault report missed its covering live resource");
}

void BracketTrimsToLastEight() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    AgcDriver::FaultToken first = 0;
    for (int i = 0; i < 12; ++i) {
        const AgcDriver::FaultToken token = ring.Claim();
        Require(token != 0, "the trimming token is invalid");
        if (i == 0) first = token;
        ClaimBracketToken(ring, token, 7, 0x12000ull, 9, "draw main");
    }
    const AgcDriver::SubmitBracket bracket{7, 5, first, static_cast<AgcDriver::FaultToken>(static_cast<std::uint64_t>(first) + 11)};
    const auto lines = AgcDriver::DeviceFaultBracketLines(bracket, ring);
    Require(lines.size() == 10, "a twelve-token bracket does not report the window, eight tokens and the trim count");
    Require(lines[0] == "[device-fault] bracket: last completed 5, in flight 7, first not started 8", "the trimming window changed");
    for (int i = 0; i < 8; ++i) {
        char expected[256];
        std::snprintf(expected, sizeof(expected), "[device-fault] token %u in batch 7: program 0x12000 variant 9 draw main", static_cast<unsigned>(static_cast<std::uint64_t>(first) + 4 + static_cast<std::uint64_t>(i)));
        Require(lines[1 + i] == expected, "a trimmed bracket missed its last eight tokens");
    }
    Require(lines[9] == "[device-fault] 4 more bracket tokens", "a trimmed bracket missed its remainder count");
}

void BracketDerivedViaFindBracket() {
    AgcDriver::SubmissionSnapshots snapshots;
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::FaultToken token = ring.Claim();
    Require(token != 0, "the derived bracket token is invalid");
    ClaimBracketToken(ring, token, 7, 0x12000ull, 9, "draw main");
    checkpointToken = token;
    const auto ranges = std::make_shared<const std::vector<ShaderRecompiler::BdaAbi::Range>>(SnapshotRanges());
    const AgcDriver::SubmitBracket stored{7, 5, 0, 0};
    snapshots.NoteSubmit(7, 3, ranges, stored);
    const AgcDriver::SubmitBracket empty{};
    const auto lines = AgcDriver::DeviceFaultReport(VK_NULL_HANDLE, nullptr, FakeQueue(), MockTokenCheckpointData, 0, &snapshots, empty, ring);
    Require(lines.size() == 2, "a derived bracket does not report the window and the checkpoint");
    Require(lines[0] == "[device-fault] bracket: last completed 5, in flight 7, first not started 8, queue 0x2000", "the derived window changed");
    char expected[256];
    std::snprintf(expected, sizeof(expected), "[device-fault] checkpoint: token %u batch 7 queue 0x2000 program 0x12000 variant 9 draw main observed at bottom-of-pipe", static_cast<unsigned>(token));
    Require(lines[1] == expected, "the derived checkpoint line changed");
    checkpointToken = 0;
}

void EffectiveSerialZeroHasNoMarker() {
    faultCalls = 0;
    faultFails = false;
    faultDataResult = VK_SUCCESS;
    faultDataAddressCount = 0;
    faultDataVendorBinarySize = 0;
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::SubmitBracket bracket{};
    const auto lines = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr, 0, nullptr, bracket, ring);
    bool found = false;
    for (const auto& line : lines) {
        if (line == "[device-fault] no per-submit marker (submit serial unknown; markers need APS5_GPU_CHECKPOINTS on the main queue)") {
            found = true;
            break;
        }
    }
    Require(found, "an effective-zero report missed its no-marker line");
    faultDataAddressCount = 2;
}

void FaultIncomplete() {
    faultCalls = 0;
    faultFails = false;
    faultDataResult = VK_INCOMPLETE;
    faultDataAddressCount = 5;
    faultDataVendorBinarySize = 0;
    const auto lines = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr);
    Require(faultCalls == 2, "an incomplete fault info was not queried for counts and then for data");
    Require(lines.size() == 4, "an incomplete fault info does not report the addresses it returned");
    Require(lines[0] == "[device-fault] result 5: page fault", "an incomplete result was not reported");
    faultDataResult = VK_ERROR_DEVICE_LOST;
    const auto failed = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr);
    Require(failed.size() == 1 && failed[0] == "[device-fault] vkGetDeviceFaultInfoEXT reported nothing", "a failed data query was not reported");
    faultDataResult = VK_SUCCESS;
    faultDataAddressCount = 2;
}

void VendorBinaryUntouched() {
    faultCalls = 0;
    faultFails = false;
    faultDataResult = VK_SUCCESS;
    faultDataAddressCount = 2;
    faultDataVendorBinarySize = 4096;
    const auto lines = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr);
    Require(faultCalls == 2, "the fault info was not queried for counts and then for data");
    Require(lines.size() == 4, "a reported vendor binary changed the address report");
    Require(lines[1] == "[device-fault] read invalid at 0x1234000 (precision 0x1000)", "the read fault line changed");
    faultDataVendorBinarySize = 0;
}

void AddressKinds() {
    Require(std::strcmp(AgcDriver::DeviceFaultAddressKind(VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT), "none") == 0, "none");
    Require(std::strcmp(AgcDriver::DeviceFaultAddressKind(VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT), "write invalid") == 0, "write invalid");
    Require(std::strcmp(AgcDriver::DeviceFaultAddressKind(VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT), "execute invalid") == 0, "execute invalid");
    Require(std::strcmp(AgcDriver::DeviceFaultAddressKind(VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT), "instruction pointer unknown") == 0, "instruction pointer unknown");
    Require(std::strcmp(AgcDriver::DeviceFaultAddressKind(VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT), "instruction pointer invalid") == 0, "instruction pointer invalid");
    Require(std::strcmp(AgcDriver::DeviceFaultAddressKind(static_cast<VkDeviceFaultAddressTypeEXT>(99)), "unknown") == 0, "an unknown kind");
}

void IndirectRoundTrip() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::FaultToken token = ring.Claim();
    Require(token != 0, "the indirect token is invalid");
    const std::uint32_t words[4] = {3, 1, 1, 0};
    ClaimBracketTokenWithArgs(ring, token, 7, 0x12000ull, 9, "dispatch indirect", static_cast<std::uint32_t>(AgcDriver::FaultStage::compute), 0x80000ull, 16, 12, words, 3);
    emitCmd = VK_NULL_HANDLE;
    emitMarker = nullptr;
    emitCalls = 0;
    static VkCommandBuffer fakeCmd = reinterpret_cast<VkCommandBuffer>(static_cast<std::uintptr_t>(0xC0FFEE2));
    AgcDriver::Graphics::EmitNvCheckpointToken(fakeCmd, token, MockEmitCheckpoint);
    Require(emitCalls == 1, "the indirect checkpoint backend was not called once");
    Require(emitCmd == fakeCmd, "the indirect checkpoint backend got the wrong command buffer");
    Require(emitMarker == reinterpret_cast<const void*>(static_cast<std::uintptr_t>(token)), "the indirect checkpoint backend got the wrong token");
    const auto found = ring.Find(token);
    Require(found.has_value() && found->token == token, "an indirect token missed its ring entry");
    Require(found->stage == static_cast<std::uint32_t>(AgcDriver::FaultStage::compute), "an indirect token lost its stage");
    Require(found->argBuffer == 0x80000ull && found->argOffset == 16 && found->argStride == 12 && found->argCount == 3, "an indirect token lost its args summary");
    Require(found->argWords[0] == 3 && found->argWords[1] == 1 && found->argWords[2] == 1 && found->argWords[3] == 0, "an indirect token lost its arg words");
    const AgcDriver::SubmitBracket bracket{7, 5, token, token};
    const auto lines = AgcDriver::DeviceFaultBracketLines(bracket, ring, VK_NULL_HANDLE, AgcDriver::DeviceFaultStageLinesTag{});
    Require(lines.size() == 3, "an indirect bracket does not report the window, the token and the args");
    char expectedToken[320];
    std::snprintf(expectedToken, sizeof(expectedToken), "[device-fault] token %u in batch 7: program 0x12000 variant 9 stage compute dispatch indirect", static_cast<unsigned>(token));
    Require(lines[1] == expectedToken, "the indirect token stage line changed");
    char expectedIndirect[192];
    std::snprintf(expectedIndirect, sizeof(expectedIndirect), "[device-fault] indirect token %u args 3 words from 0x80000+16 stride 12: 00000003 00000001 00000001 (record-time)", static_cast<unsigned>(token));
    Require(lines[2] == expectedIndirect, "the indirect args line changed");
}

void HungBatchListsItsCommands() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const std::uint32_t direct[3] = {64, 2, 1};
    const std::uint32_t indirect[3] = {4096, 1, 1};
    const AgcDriver::FaultToken first = ring.Claim();
    ClaimBracketTokenWithArgs(ring, first, 41, 0x106fd68a00ull, 11, "dispatch 64x2x1", static_cast<std::uint32_t>(AgcDriver::FaultStage::compute), 0x5000ull, 0, 12, direct, 3);
    const AgcDriver::FaultToken other = ring.Claim();
    ClaimBracketToken(ring, other, 42, 0x7000ull, 12, "draw");
    const AgcDriver::FaultToken second = ring.Claim();
    ClaimBracketTokenWithArgs(ring, second, 41, 0x106fadb000ull, 13, "dispatch indirect", static_cast<std::uint32_t>(AgcDriver::FaultStage::compute), 0x9000ull, 8, 12, indirect, 3);
    Require(ring.Newest() == second, "the ring's newest token is not the last one claimed");
    const auto lines = AgcDriver::HungBatchLines(41, ring);
    Require(lines.size() == 6, "a hung batch does not report its count, its two programs and both of its commands");
    Require(lines[0] == "[gpu-hang] batch 41 holds 2 guest commands in the fault token ring", "the hung batch summary changed");
    char expected[320];
    std::snprintf(expected, sizeof(expected), "[gpu-hang]   compute program 0x106fd68a00 variant 11: 1 command, tokens %u-%u", static_cast<unsigned>(first), static_cast<unsigned>(first));
    Require(lines[1] == expected, "the first program of the hung batch changed");
    std::snprintf(expected, sizeof(expected), "[gpu-hang]   compute program 0x106fadb000 variant 13: 1 command, tokens %u-%u", static_cast<unsigned>(second), static_cast<unsigned>(second));
    Require(lines[2] == expected, "the second program of the hung batch changed");
    Require(lines[3] == "[gpu-hang] the last 2:", "the hung batch listing header changed");
    std::snprintf(expected, sizeof(expected), "[gpu-hang] token %u compute program 0x106fd68a00 variant 11 dispatch 64x2x1 args from 0x5000+0: 64 2 1", static_cast<unsigned>(first));
    Require(lines[4] == expected, "the first command of the hung batch changed");
    std::snprintf(expected, sizeof(expected), "[gpu-hang] token %u compute program 0x106fadb000 variant 13 dispatch indirect args from 0x9000+8: 4096 1 1", static_cast<unsigned>(second));
    Require(lines[5] == expected, "the second command of the hung batch changed");
    const auto none = AgcDriver::HungBatchLines(4141, ring);
    Require(none.size() == 1 && none[0] == "[gpu-hang] batch 4141: none of its guest commands is in the fault token ring (APS5_GPU_CHECKPOINTS records them)", "a batch without ring entries was not reported as such");
}

void HungBatchListsOnlyTheLastCommands() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    AgcDriver::FaultToken first = 0;
    AgcDriver::FaultToken last = 0;
    for (std::size_t index = 0; index < AgcDriver::HungBatchTokensListed + 5; ++index) {
        last = ring.Claim();
        if (first == 0) first = last;
        ClaimBracketToken(ring, last, 43, 0x1000ull, 7, "draw");
    }
    const auto lines = AgcDriver::HungBatchLines(43, ring);
    Require(lines.size() == AgcDriver::HungBatchTokensListed + 3, "a long hung batch was not trimmed to its last commands");
    char expected[160];
    std::snprintf(expected, sizeof(expected), "[gpu-hang] batch 43 holds %zu guest commands in the fault token ring", AgcDriver::HungBatchTokensListed + 5);
    Require(lines[0] == expected, "the trimmed hung batch summary changed");
    std::snprintf(expected, sizeof(expected), "[gpu-hang]   unknown program 0x1000 variant 7: %zu commands, tokens %u-%u", AgcDriver::HungBatchTokensListed + 5, static_cast<unsigned>(first), static_cast<unsigned>(last));
    Require(lines[1] == expected, "a repeated program was not counted once");
    std::snprintf(expected, sizeof(expected), "[gpu-hang] the last %zu:", AgcDriver::HungBatchTokensListed);
    Require(lines[2] == expected, "the trimmed listing header changed");
    std::snprintf(expected, sizeof(expected), "[gpu-hang] token %u unknown program 0x1000 variant 7 draw", static_cast<unsigned>(last));
    Require(lines.back() == expected, "the trimmed hung batch did not end with its newest command");
}

void HungBatchNamesTheUnfinishedDispatch() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const std::uint32_t groups[3] = {5, 1, 1};
    const AgcDriver::FaultToken done = ring.Claim();
    ClaimBracketTokenWithArgs(ring, done, 44, 0xa936000ull, 21, "dispatch 1x1x1", static_cast<std::uint32_t>(AgcDriver::FaultStage::compute), 0, 0, 12, groups, 0);
    const AgcDriver::FaultToken stuck = ring.Claim();
    ClaimBracketTokenWithArgs(ring, stuck, 44, 0xa937b00ull, 22, "dispatch 5x1x1", static_cast<std::uint32_t>(AgcDriver::FaultStage::compute), 0xc000ull, 0, 12, groups, 3);
    auto* slot = ring.Mutable(stuck);
    Require(slot != nullptr, "the stuck token has no slot");
    slot->rangeCount = 2;
    slot->rangeWritten = 2;
    slot->rangeBegin[0] = 0x10a4623fa0ull;
    slot->rangeEnd[0] = 0x10a4623fb0ull;
    slot->rangeBegin[1] = 0x10a4623fc0ull;
    slot->rangeEnd[1] = 0x10a4623fd8ull;
    const AgcDriver::FaultToken after = ring.Claim();
    ClaimBracketToken(ring, after, 44, 0xa82b600ull, 23, "dispatch 1x1x1");
    const auto lines = AgcDriver::HungBatchLines(44, ring, done);
    Require(lines.size() == 9, "a hung batch with a finished dispatch does not report its programs, the finished one and what follows");
    char expected[320];
    std::snprintf(expected, sizeof(expected), "[gpu-hang] the GPU finished its guest dispatches up to token %u compute program 0xa936000 variant 21 dispatch 1x1x1", static_cast<unsigned>(done));
    Require(lines[4] == expected, "the last finished dispatch was not named");
    std::snprintf(expected, sizeof(expected), "[gpu-hang] not finished: token %u compute program 0xa937b00 variant 22 dispatch 5x1x1 args from 0xc000+0: 5 1 1", static_cast<unsigned>(stuck));
    Require(lines[5] == expected, "the first unfinished dispatch was not named");
    Require(lines[6] == "[gpu-hang]   guest 0x10a4623fa0-0x10a4623fb0 read", "the unfinished dispatch's read range changed");
    Require(lines[7] == "[gpu-hang]   guest 0x10a4623fc0-0x10a4623fd8 written", "the unfinished dispatch's written range changed");
    std::snprintf(expected, sizeof(expected), "[gpu-hang] not finished: token %u unknown program 0xa82b600 variant 23 dispatch 1x1x1", static_cast<unsigned>(after));
    Require(lines[8] == expected, "the command after the unfinished dispatch was not listed");
    const auto unfinished = AgcDriver::HungBatchUnfinished(AgcDriver::HungBatchEntries(44, ring), done, 1);
    Require(unfinished.size() == 1 && unfinished[0].token == stuck && unfinished[0].rangeCount == 2, "the first unfinished dispatch was not returned with its ranges");
    const AgcDriver::FaultToken otherBatchToken = ring.Claim();
    ClaimBracketToken(ring, otherBatchToken, 45, 0x2000ull, 24, "dispatch 1x1x1");
    const auto none = AgcDriver::HungBatchLines(44, ring, otherBatchToken);
    Require(none.size() == 10 && none[4] == "[gpu-hang] the GPU finished none of its checkpointed guest dispatches", "a batch none of whose dispatches finished was not reported as such");
    std::snprintf(expected, sizeof(expected), "[gpu-hang] not finished: token %u compute program 0xa936000 variant 21 dispatch 1x1x1", static_cast<unsigned>(done));
    Require(none[5] == expected, "a batch none of whose dispatches finished did not start with its first command");
}

void CompletionMarkerSlots() {
    Require(AgcDriver::LastFinishedToken(7) == 0, "a token was read without completion markers");
    std::array<std::uint32_t, AgcDriver::CompletionMarkerSlots> slots{};
    slots[7] = 1234;
    slots[(7 + AgcDriver::CompletionMarkerSlots) % AgcDriver::CompletionMarkerSlots] = 1234;
    AgcDriver::CompletionMarkers().store(slots.data(), std::memory_order_release);
    const auto read = AgcDriver::LastFinishedToken(7 + AgcDriver::CompletionMarkerSlots);
    AgcDriver::CompletionMarkers().store(nullptr, std::memory_order_release);
    Require(read == 1234, "a batch serial did not select its completion marker slot");
}

void MeshStageLine() {
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::FaultToken token = ring.Claim();
    Require(token != 0, "the mesh token is invalid");
    const std::uint32_t words[4] = {};
    ClaimBracketTokenWithArgs(ring, token, 7, 0x13000ull, 11, "mesh main", static_cast<std::uint32_t>(AgcDriver::FaultStage::mesh), 0, 0, 0, words, 0);
    const AgcDriver::SubmitBracket bracket{7, 5, token, token};
    const auto lines = AgcDriver::DeviceFaultBracketLines(bracket, ring, VK_NULL_HANDLE, AgcDriver::DeviceFaultStageLinesTag{});
    Require(lines.size() == 2, "a mesh bracket does not report the window and the token");
    char expected[320];
    std::snprintf(expected, sizeof(expected), "[device-fault] token %u in batch 7: program 0x13000 variant 11 stage mesh mesh main", static_cast<unsigned>(token));
    Require(lines[1] == expected, "the mesh stage line changed");
    Require(std::strcmp(AgcDriver::DeviceFaultStageName(static_cast<std::uint32_t>(AgcDriver::FaultStage::mesh)), "mesh") == 0, "mesh stage name changed");
    Require(std::strcmp(AgcDriver::DeviceFaultStageName(static_cast<std::uint32_t>(AgcDriver::FaultStage::task)), "task") == 0, "task stage name changed");
    Require(std::strcmp(AgcDriver::DeviceFaultStageName(static_cast<std::uint32_t>(AgcDriver::FaultStage::compute)), "compute") == 0, "compute stage name changed");
    Require(std::strcmp(AgcDriver::DeviceFaultStageName(static_cast<std::uint32_t>(AgcDriver::FaultStage::graphics)), "graphics") == 0, "graphics stage name changed");
    Require(std::strcmp(AgcDriver::DeviceFaultStageName(static_cast<std::uint32_t>(AgcDriver::FaultStage::copy)), "copy") == 0, "copy stage name changed");
    Require(std::strcmp(AgcDriver::DeviceFaultStageName(static_cast<std::uint32_t>(AgcDriver::FaultStage::unknown)), "unknown") == 0, "unknown stage name changed");
}

void ResidencyNeverBound() {
    AgcDriver::SubmissionSnapshots snapshots;
    snapshots.NoteSubmit(7, 3, std::make_shared<const std::vector<ShaderRecompiler::BdaAbi::Range>>(SnapshotRanges()));
    AgcDriver::DeviceFaultResidencyProbe probe{};
    probe.boundNow = false;
    const auto lines = AgcDriver::DeviceFaultResidencyLines(0x50000ull, 7, &snapshots, probe);
    Require(lines.size() == 1 && lines[0] == "[device-fault] residency 0x50000 never bound", "a never-bound address missed its verdict");
}

void ResidencyNoHistory() {
    AgcDriver::SubmissionSnapshots snapshots;
    snapshots.NoteSubmit(7, 3, std::make_shared<const std::vector<ShaderRecompiler::BdaAbi::Range>>(SnapshotRanges()));
    AgcDriver::DeviceFaultResidencyProbe probe{};
    probe.boundNow = false;
    const auto lines = AgcDriver::DeviceFaultResidencyLines(0x50000ull, 0, &snapshots, probe);
    Require(lines.size() == 1 && lines[0] == "[device-fault] residency 0x50000 unknown", "an address without history claimed never bound");
}

void ResidencyEvictedWithSerial() {
    AgcDriver::DeviceFaultResidencyProbe probe{};
    probe.boundNow = true;
    probe.evicted = true;
    probe.evictedAtSerial = 7;
    const auto lines = AgcDriver::DeviceFaultResidencyLines(0x12000ull, 7, nullptr, probe);
    Require(lines.size() == 1 && lines[0] == "[device-fault] residency 0x12000 evicted at serial 7", "an evicted address missed its serial");
}

void ResidencyEvictedFallback() {
    AgcDriver::DeviceFaultResidencyProbe probe{};
    probe.boundNow = true;
    probe.evicted = true;
    probe.evictedAtSerial = 0;
    const auto lines = AgcDriver::DeviceFaultResidencyLines(0x12000ull, 7, nullptr, probe);
    Require(lines.size() == 1 && lines[0] == "[device-fault] residency 0x12000 evicted", "an evicted address without a serial missed its fallback");
}

void ResidencyImportInFlight() {
    AgcDriver::DeviceFaultResidencyProbe probe{};
    probe.boundNow = true;
    probe.importInFlight = true;
    const auto lines = AgcDriver::DeviceFaultResidencyLines(0x12000ull, 7, nullptr, probe);
    Require(lines.size() == 1 && lines[0] == "[device-fault] residency 0x12000 import in flight", "an import in flight missed its verdict");
}

void ResidencyWriteWatchRefused() {
    AgcDriver::DeviceFaultResidencyProbe probe{};
    probe.boundNow = true;
    probe.writeWatchRefused = true;
    const auto lines = AgcDriver::DeviceFaultResidencyLines(0x12000ull, 7, nullptr, probe);
    Require(lines.size() == 1 && lines[0] == "[device-fault] residency 0x12000 write-watch refused", "a write-watch refusal missed its verdict");
}

void ResidencyUnknown() {
    AgcDriver::SubmissionSnapshots snapshots;
    snapshots.NoteSubmit(7, 3, std::make_shared<const std::vector<ShaderRecompiler::BdaAbi::Range>>(SnapshotRanges()));
    AgcDriver::DeviceFaultResidencyProbe probe{};
    probe.boundNow = true;
    const auto lines = AgcDriver::DeviceFaultResidencyLines(0x50000ull, 7, &snapshots, probe);
    Require(lines.size() == 1 && lines[0] == "[device-fault] residency 0x50000 unknown", "an unknown address missed its verdict");
}

void ReportResidencyIntegration() {
    faultCalls = 0;
    faultFails = false;
    faultDataResult = VK_SUCCESS;
    faultDataAddressCount = 2;
    faultDataVendorBinarySize = 0;
    AgcDriver::FaultTokenRing& ring = AgcDriver::FaultTokenRing::Instance();
    const AgcDriver::SubmitBracket bracket{};
    const auto lines = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr, 0, nullptr, bracket, ring, static_cast<AgcDriver::DeviceFaultResidencyProbe (*)(std::uint64_t)>(nullptr));
    bool first = false;
    bool second = false;
    for (const auto& line : lines) {
        if (line == "[device-fault] residency 0x1234000 unknown") first = true;
        if (line == "[device-fault] residency 0xabcd unknown") second = true;
    }
    Require(first && second, "a residency report missed one verdict per fault address");
    const auto routed = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr, 0, nullptr, bracket, ring, ResidencyProbeUnboundNow);
    bool routedFirst = false;
    bool routedSecond = false;
    for (const auto& line : routed) {
        if (line == "[device-fault] residency 0x1234000 unknown") routedFirst = true;
        if (line == "[device-fault] residency 0xabcd unknown") routedSecond = true;
    }
    Require(routedFirst && routedSecond, "a snapshot-less residency report missed its unknown verdicts");
    AgcDriver::SubmissionSnapshots boundSnapshots;
    boundSnapshots.NoteSubmit(9, 3, std::make_shared<const std::vector<ShaderRecompiler::BdaAbi::Range>>(SnapshotRanges()));
    const auto found = AgcDriver::DeviceFaultReport(FakeDevice(), MockFaultInfo, VK_NULL_HANDLE, nullptr, 9, &boundSnapshots, bracket, ring, ResidencyProbeUnboundNow);
    bool foundFirst = false;
    bool foundSecond = false;
    for (const auto& line : found) {
        if (line == "[device-fault] residency 0x1234000 never bound") foundFirst = true;
        if (line == "[device-fault] residency 0xabcd never bound") foundSecond = true;
    }
    Require(foundFirst && foundSecond, "a found-but-uncovering snapshot missed its never-bound verdicts");
    faultDataAddressCount = 2;
}

}

int main() {
    try {
        Diagnostics();
        FaultInfo();
        FaultInfoFails();
        FaultIncomplete();
        VendorBinaryUntouched();
        Checkpoints();
        Nothing();
        AddressKinds();
        SubmissionSnapshots();
        SubmissionSnapshotsConcurrent();
        FaultRangeHit();
        FaultRangeStraddle();
        FaultRangeMiss();
        BatchSerialMismatch();
        BracketWindow();
        BracketTokenMiss();
        BracketSerialMismatch();
        BracketEmptyTokens();
        BracketTrimsToLastEight();
        BracketDerivedViaFindBracket();
        FaultResultZero();
        EffectiveSerialZeroHasNoMarker();
        TokenCheckpoint();
        BatchBeginCheckpoint();
        ReportBracketOnly();
        ExtraQueueEmptyBracket();
        TokenReuse();
        TokenEmission();
        IndirectRoundTrip();
        HungBatchListsItsCommands();
        HungBatchListsOnlyTheLastCommands();
        HungBatchNamesTheUnfinishedDispatch();
        CompletionMarkerSlots();
        MeshStageLine();
        ResidencyNeverBound();
        ResidencyNoHistory();
        ResidencyEvictedWithSerial();
        ResidencyEvictedFallback();
        ResidencyImportInFlight();
        ResidencyWriteWatchRefused();
        ResidencyUnknown();
        ReportResidencyIntegration();
        LiveResourceEmpty();
        LiveResourceHit();
        LiveResourceInReport();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_device_fault: %s\n", error.what());
        return 1;
    }
    std::puts("agc_device_fault: passed");
    return 0;
}
