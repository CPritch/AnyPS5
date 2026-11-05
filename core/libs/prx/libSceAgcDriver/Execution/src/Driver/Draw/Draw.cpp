#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "prx/libSceAgcDriver/Execution/include/CaptureTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/StageDelay.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DrawWriteRanges.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawProgramTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DrawWriteRanges.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Synchronization/DeferredLabels.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <atomic>
#include <array>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <unordered_set>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <atomic>
#include <exception>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <typeinfo>

namespace AgcDriver::DriverDetail {

namespace {

bool TraceShaderCorrelation() {
    static const bool enabled = std::getenv("APS5_TRACE_SHADER_CORRELATION") != nullptr;
    return enabled && AgcDriver::CaptureTrace::Enabled();
}

std::atomic<std::uint64_t>& NextDrawTraceToken() {
    static std::atomic<std::uint64_t> next{0};
    return next;
}

std::string ExceptionReason(const std::exception_ptr& failure) {
    try {
        if (failure != nullptr) std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
    }
    return "unknown exception";
}

const char* ExceptionType(const std::exception_ptr& failure) {
    try {
        if (failure != nullptr) std::rethrow_exception(failure);
    } catch (const std::exception& error) {
        return typeid(error).name();
    } catch (...) {
    }
    return "unknown";
}

void TraceMaterializationFailure(std::uint64_t token, const Submission& submission, std::size_t packetOffset, const char* phase, std::size_t completedSubdraws, const std::exception_ptr& failure) {
    if (token == 0) return;
    try {
        const auto reason = ExceptionReason(failure);
        AgcDriver::CaptureTrace::Log("draw-materialization-failed token=%llu submission=%llu queue=%x offset=%zu phase=%s exception=%s reason=%.256s completedSubdraws=%zu",
                static_cast<unsigned long long>(token), static_cast<unsigned long long>(submission.serial), submission.queue, packetOffset, phase, ExceptionType(failure), reason.c_str(), completedSubdraws);
    } catch (...) {

    }
}

void TraceSubmissionFailure(std::uint64_t token, const Submission& submission, std::size_t packetOffset, std::size_t completedSubdraws, const std::exception_ptr& failure) {
    if (token == 0) return;
    try {
        const auto reason = ExceptionReason(failure);
        AgcDriver::CaptureTrace::Log("draw-submission-failed token=%llu submission=%llu queue=%x offset=%zu exception=%s reason=%.256s completedSubdraws=%zu",
                static_cast<unsigned long long>(token), static_cast<unsigned long long>(submission.serial), submission.queue, packetOffset, ExceptionType(failure), reason.c_str(), completedSubdraws);
    } catch (...) {

    }
}

void TraceDrawAttemptFailure(std::uint64_t token, const char* phase, const char* outcome, std::uint64_t submission, std::uint32_t queue, std::size_t packetOffset, std::string_view reason, std::string_view exceptionType = "none") {
    if (token == 0) return;
        AgcDriver::CaptureTrace::LogBestEffort("draw-attempt-failed token=%llu phase=%s outcome=%s submission=%llu queue=%x offset=%zu exception=%s reason=%.256s",
            static_cast<unsigned long long>(token), phase, outcome, static_cast<unsigned long long>(submission), queue, packetOffset, exceptionType.data(), reason.data());
}

void TraceDrawAttemptException(std::uint64_t token, const char* phase, std::uint64_t submission, std::uint32_t queue, std::size_t packetOffset, const std::exception_ptr& failure) noexcept {
    try {
        TraceDrawAttemptFailure(token, phase, "exception", submission, queue, packetOffset, ExceptionReason(failure), ExceptionType(failure));
    } catch (...) {

    }
}

const char* DrawVerdictName(DrawVerdict verdict) {
    switch (verdict) {
        case DrawVerdict::Drawn: return "Drawn";
        case DrawVerdict::Nothing: return "Nothing";
        case DrawVerdict::Rejected: return "Rejected";
    }
    return "Unknown";
}

std::string_view TrimmedReason(std::string_view reason) {
    if (reason.size() > 200) reason = reason.substr(0, 200);
    return reason;
    }

}


struct DrawTailStorage {
    std::vector<DrawProgram> writablePrograms;
    std::vector<ShaderRecompiler::MemoryRegion> memory;
    std::vector<ShaderRecompiler::LinkedProgram> linked;
    std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>> vertexInfos;
    std::vector<std::vector<Graphics::DecodeRead>> decodeReads;
    std::vector<ShaderRecompiler::RecompileResult> results;
    std::vector<std::shared_ptr<const ShaderRecompiler::RecompileResult>> compiledStages;
    std::vector<Graphics::CompiledShader> stages;
    std::vector<const ShaderRecompiler::RecompileResult*> programResults;
    std::vector<StageCapture> stageCaptures;
    std::vector<std::shared_ptr<DispatchVariant>> matched;
    std::vector<std::vector<ShaderRecompiler::MemoryRegion>> matchedRegions;
    std::vector<std::shared_ptr<DispatchVariant>> fresh;
    std::vector<bool> recompiled;
    DrawDataCandidates dataCandidates;
    std::vector<bool> reused;
    std::vector<StageDataHit> stageData;
    std::vector<Graphics::GuestMemorySnapshot> snapshots;
    std::vector<std::shared_ptr<DispatchVariant>> recipeStages;

    void Clear() {
        writablePrograms.clear();
        memory.clear();
        linked.clear();
        vertexInfos.clear();
        for (auto& reads : decodeReads) reads.clear();
        results.clear();
        compiledStages.clear();
        stages.clear();
        programResults.clear();
        stageCaptures.clear();
        matched.clear();
        for (auto& regions : matchedRegions) regions.clear();
        fresh.clear();
        recompiled.clear();
        dataCandidates.clear();
        reused.clear();
        stageData.clear();
        snapshots.clear();
        recipeStages.clear();
    }
};

namespace {

struct DrawStoragePool {
    AgcDriver::Mutex mutex;
    std::vector<std::unique_ptr<DrawTailStorage>> free;
};

DrawStoragePool& drawStoragePool() {
    static auto* pool = new DrawStoragePool();
    return *pool;
}

bool DrawStoragePooled() {
    static const bool pooled = std::getenv("APS5_NO_DRAW_STORAGE_POOL") == nullptr;
    return pooled;
}

struct DrawStorageReturn {
    void operator()(DrawTailStorage* storage) const {
        std::unique_ptr<DrawTailStorage> owned(storage);
        if (!DrawStoragePooled()) return;
        constexpr std::size_t MostFree = 1024;
        owned->Clear();
        auto& pool = drawStoragePool();
        std::lock_guard lock(pool.mutex);
        if (pool.free.size() < MostFree) pool.free.push_back(std::move(owned));
    }
};

using DrawStorageHandle = std::unique_ptr<DrawTailStorage, DrawStorageReturn>;

DrawStorageHandle AcquireDrawStorage() {
    if (DrawStoragePooled()) {
        auto& pool = drawStoragePool();
        std::lock_guard lock(pool.mutex);
        if (!pool.free.empty()) {
            auto* storage = pool.free.back().release();
            pool.free.pop_back();
            return DrawStorageHandle(storage);
        }
    }
    return DrawStorageHandle(new DrawTailStorage());
}

}

struct DrawTail {
    using GpuLock = std::unique_lock<std::remove_reference_t<decltype(GuestMemory::GpuMutex())>>;
    DrawStorageHandle storage = AcquireDrawStorage();
    bool profile = false;
    std::array<double, DrawDriverPhaseCount> phaseMs{};
    std::uint64_t captures = 0;
    std::chrono::steady_clock::time_point phaseLap{};
    DrawPhaseTiming phaseTiming{false, phaseMs, phaseLap};
    Pm4::DrawParameters drawParameters{};
    bool pipelined = false;
    bool traceIndirect = false;
    std::uint64_t dumpTarget = 0;
    std::uint64_t dumpSlot1 = 0;
    std::shared_ptr<VulkanDevice> localDevice;
    bool registerKey = false;
    std::uint64_t drawKey = 0;
    std::shared_ptr<DrawEntry> entry;
    std::shared_ptr<const DrawDecode> decode;
    std::shared_ptr<const DrawDecode> relocatedDecode;
    std::shared_ptr<DrawEntry> dataEntry;
    std::uint64_t shapeKey = 0;
    DrawRelocationPending relocationPending{};
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    decltype(DrawTailStorage::writablePrograms)& writablePrograms = storage->writablePrograms;
    const std::vector<DrawProgram>* programs = nullptr;
    decltype(DrawTailStorage::memory)& memory = storage->memory;
    decltype(DrawTailStorage::linked)& linked = storage->linked;
    decltype(DrawTailStorage::vertexInfos)& vertexInfos = storage->vertexInfos;
    decltype(DrawTailStorage::decodeReads)& decodeReads = storage->decodeReads;
    std::unique_ptr<ShaderMemory> shaderMemory;
    decltype(DrawTailStorage::snapshots)& snapshots = storage->snapshots;
    decltype(DrawTailStorage::recipeStages)& recipeStages = storage->recipeStages;
    decltype(DrawTailStorage::results)& results = storage->results;
    decltype(DrawTailStorage::compiledStages)& compiledStages = storage->compiledStages;
    decltype(DrawTailStorage::stages)& stages = storage->stages;
    std::uint32_t pushCursorBytes = 0;
    decltype(DrawTailStorage::programResults)& programResults = storage->programResults;
    decltype(DrawTailStorage::stageCaptures)& stageCaptures = storage->stageCaptures;
    decltype(DrawTailStorage::matched)& matched = storage->matched;
    decltype(DrawTailStorage::matchedRegions)& matchedRegions = storage->matchedRegions;
    decltype(DrawTailStorage::fresh)& fresh = storage->fresh;
    decltype(DrawTailStorage::recompiled)& recompiled = storage->recompiled;
    decltype(DrawTailStorage::dataCandidates)& dataCandidates = storage->dataCandidates;
    decltype(DrawTailStorage::reused)& reused = storage->reused;
    decltype(DrawTailStorage::stageData)& stageData = storage->stageData;
    bool drawHit = false;
    bool verifyHit = false;
    bool stageEntries = false;
    bool patchedStages = false;
    const Submission* submission = nullptr;
    std::shared_ptr<const ShaderRegistry> shaders;
    std::uint32_t submissionQueue = 0;
    std::uint64_t submissionSerial = 0;
    std::size_t packetOffset = 0;
    std::uint32_t packetOpcode = 0;
    std::uint32_t reg207 = 0;
    std::uint64_t epoch = 0;
    QueueState* queue = nullptr;
    std::span<const std::uint32_t> packet;
    std::string* rejected = nullptr;
    std::uint64_t traceToken = 0;
    VulkanDevice::DrawTraceContext trace{};
    std::string ownRejected;
    GpuLock* gpuLock = nullptr;
    GpuLock ownLock{GuestMemory::GpuMutex(), std::defer_lock};
};

struct DrawAheadSlot {
    enum Phase : std::uint8_t { Queued, Running, Ready, Failed, Dropped };
    std::atomic<std::uint8_t> phase{Queued};
    std::shared_ptr<VulkanDevice> device;
    std::shared_ptr<const DrawDecode> decode;
    Pm4::DrawParameters draw{};
    std::uint64_t shape = 0;
    static constexpr std::size_t MaxStages = 8;
    std::array<Graphics::CompiledShader, MaxStages> stages{};
    std::size_t stageCount = 0;
    Graphics::DrawAhead result;

    void Prepare() {
        std::uint8_t expected = Queued;
        if (!phase.compare_exchange_strong(expected, Running, std::memory_order_acq_rel)) return;
        bool prepared = false;
        try {
            device->PrepareDrawAhead(decode->state, draw, std::span<const Graphics::CompiledShader>(stages.data(), stageCount), shape, result);
            prepared = true;
        } catch (...) {
        }
        Graphics::CountDrawAhead(prepared ? Graphics::DrawAheadEvent::Prepared : Graphics::DrawAheadEvent::Thrown);
        expected = Running;
        phase.compare_exchange_strong(expected, prepared ? Ready : Failed, std::memory_order_acq_rel);
    }

    Graphics::DrawAhead* Claim() {
        auto seen = phase.load(std::memory_order_acquire);
        while (seen == Queued || seen == Running) {
            if (phase.compare_exchange_weak(seen, Dropped, std::memory_order_acq_rel)) {
                Graphics::CountDrawAhead(seen == Queued ? Graphics::DrawAheadEvent::NotStarted : Graphics::DrawAheadEvent::Unfinished);
                return nullptr;
            }
        }
        return seen == Ready ? &result : nullptr;
    }
};

struct EarlyDraw {
    Graphics::DrawPacketPtr packet;
    std::shared_ptr<const DrawRecipe> built;
    bool claimed = false;
    std::uint64_t proof = 0;
    std::uint64_t pending = 0;
    std::uint64_t redo = 0;
    VulkanDevice::EarlySerials serials;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    const VulkanDevice* device = nullptr;
    VulkanDevice* resolver = nullptr;
    const Graphics::State* graphics = nullptr;
    const Pm4::DrawParameters* draw = nullptr;
    std::span<const Graphics::CompiledShader> stages;
    std::span<const Graphics::GuestMemorySnapshot> snapshots;
    std::shared_ptr<const DrawRecipe>* recipeOut = nullptr;
    const VulkanDevice::DrawTraceContext* trace = nullptr;
    std::uint64_t shape = 0;
    Graphics::DrawAhead* prepared = nullptr;
    std::uint64_t epoch = 0;
    std::uint64_t collectEpoch = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> writes;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> depth;
    Graphics::Recorder::DeferredEffects abandoned;
    std::vector<Graphics::DrawPacket*> graveyard;
    Graphics::ResolverHandoff handoff;
    std::vector<Graphics::FoundMultisampleTarget> multisample;
    std::uint8_t abort = 0;
    bool failed = false;
    std::uint8_t proofClass = 0;
    std::uint64_t jobNs = 0;
    bool revoked = false;
    bool dropped = false;
    bool twoAhead = false;
    bool reproved = false;
    const void* hintTemplate = nullptr;
    DrawPipeline::EarlyEvent declined = DrawPipeline::EarlyEvent::Count;
};

namespace {

void noteFinisherHandOff(bool handed) {
    static std::atomic<std::uint64_t> handedOff{0}, kept{0};
    static std::atomic<std::int64_t> reportedAt{0};
    (handed ? handedOff : kept).fetch_add(1, std::memory_order_relaxed);
    static const bool stats = std::getenv("APS5_COMMIT_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!stats) return;
    const auto tenSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 10;
    if (reportedAt.exchange(tenSeconds, std::memory_order_relaxed) == tenSeconds) return;
    AgcDriver::ProfilePrint_nid_no_patch("[finisher] draws (10 s): %llu handed to the draw finisher, %llu finished on the worker%s\n", static_cast<unsigned long long>(handedOff.exchange(0)), static_cast<unsigned long long>(kept.exchange(0)), DrawFinisher::DrainText().c_str());
}

}

namespace {

enum class HeldReason : std::size_t { Known, NoDecode, Size, NullVertex, NullFragment, NullOther, Bda, Buffer, Storage, Count };
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(HeldReason::Count)> heldReasons{};

void noteHeld(HeldReason reason) {
    heldReasons[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    static const bool profile = std::getenv("APS5_COMMIT_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    static std::atomic<std::uint64_t> total{0};
    const auto count = ++total;
    if (count % 262144u != 0u && (count > 262144u || (count & (count - 1u)) != 0u)) return;
    constexpr std::array<const char*, static_cast<std::size_t>(HeldReason::Count)> names{"targets only", "no decode", "stage count", "no variant (vertex)", "no variant (fragment)", "no variant (other)", "BDA stores", "written buffer", "storage image"};
    std::string text;
    for (std::size_t i = 0; i < names.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), "%s %s %llu", i == 0 ? "" : ",", names[i], static_cast<unsigned long long>(count >= 262144u ? heldReasons[i].exchange(0) : heldReasons[i].load()));
        text += item;
    }
    std::fprintf(stderr, "[finisher-held] held draws' writes:%s\n", text.c_str());
}

std::optional<HeldReason> writesBeyondTargets(const ShaderRecompiler::RecompileResult& compiled) {
    using Role = ShaderRecompiler::DescriptorRole;
    for (const auto& binding : compiled.bindings) {
        if (binding.role == Role::BdaPagetable && Graphics::ShaderStoresThroughAddresses(compiled.spirv)) return HeldReason::Bda;
        if (binding.role == Role::GuestBuffers) {
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                if (element >= binding.bufferWritten.size() || binding.bufferWritten[element]) return HeldReason::Buffer;
            }
        }
        if (binding.role == Role::GuestImages && binding.kind == ShaderRecompiler::DescriptorKind::StorageImage) {
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                if (element >= binding.imageWritten.size() || binding.imageWritten[element]) return HeldReason::Storage;
            }
        }
    }
    return std::nullopt;
}

DrawFinisher::Pending heldWrites(const DrawTail& tail) {
    DrawFinisher::Pending held;
    held.key = tail.drawKey;
    held.shape = tail.shapeKey;
    held.inserts = tail.stageEntries && !tail.drawHit;
    if (tail.decode == nullptr) {
        noteHeld(HeldReason::NoDecode);
        return held;
    }
    if (tail.matched.size() != tail.programs->size()) {
        noteHeld(HeldReason::Size);
        return held;
    }
    const auto& roles = tail.decode->roles;
    for (std::size_t i = 0; i < tail.matched.size(); ++i) {
        if (i < roles.size() && roles[i] == ShaderRecompiler::ProgramRole::GeometryBack) continue;
        const auto& variant = tail.matched[i];
        if (variant == nullptr || variant->compiled == nullptr) {
            const bool candidates = !tail.drawHit && i < tail.dataCandidates.size() && !tail.dataCandidates[i].empty();
            const auto& program = (*tail.programs)[i];
            if (!candidates && program.snapshot != nullptr && !ProgramWritesMemory(*program.snapshot, program.codeOffset) && (program.tailContinuation == nullptr || !ProgramWritesMemory(*program.tailContinuation, 0))) continue;
            if (!candidates) {
                const auto stage = (*tail.programs)[i].binary.stage;
                noteHeld(stage == ShaderRecompiler::ShaderStage::Fragment ? HeldReason::NullFragment : stage == ShaderRecompiler::ShaderStage::Vertex ? HeldReason::NullVertex : HeldReason::NullOther);
                return held;
            }
            for (const auto& [candidate, regions] : tail.dataCandidates[i]) {
                if (candidate == nullptr || candidate->compiled == nullptr) {
                    noteHeld(HeldReason::NullOther);
                    return held;
                }
                if (const auto reason = writesBeyondTargets(*candidate->compiled)) {
                    noteHeld(*reason);
                    return held;
                }
            }
            continue;
        }
        if (const auto reason = writesBeyondTargets(*variant->compiled)) {
            noteHeld(*reason);
            return held;
        }
        if (i < tail.stageData.size() && tail.stageData[i].compiled != nullptr) {
            if (const auto reason = writesBeyondTargets(*tail.stageData[i].compiled)) {
                noteHeld(*reason);
                return held;
            }
        }
    }
    noteHeld(HeldReason::Known);
    held.writes = DrawWriteRanges(tail.decode->state, {}, ExactDrawWrites());
    held.unknown = false;
    return held;
}

bool EmptyIndirectSkip() {
    static const bool enabled = std::getenv("APS5_NO_EMPTY_INDIRECT_SKIP") == nullptr;
    return enabled;
}

struct CpuIndirectProgramsStorage {};

std::unordered_map<std::uint64_t, bool>& CpuIndirectPrograms() {
    return HostThreadLocal<std::unordered_map<std::uint64_t, bool>, CpuIndirectProgramsStorage>();
}

}

DrawVerdict Driver::draw(QueueState& queue, std::span<const std::uint32_t> packet, const Submission& submission, std::size_t packetOffset, std::string& rejected) {
    DelayStage(DelayedStage::Worker);
    PerformanceTimer timing("Driver.Draw");
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    auto tailHolder = std::make_shared<DrawTail>();
    auto& tail = *tailHolder;
    auto& phaseMs = tail.phaseMs;
    auto& captures = tail.captures;
    auto& phaseLap = tail.phaseLap;
    phaseLap = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    tail.profile = profile;
    tail.phaseTiming.profile = profile;
    auto& phaseTiming = tail.phaseTiming;
    if (profile && packetStartedAt() != std::chrono::steady_clock::time_point{}) phaseMs[DrawRowPrologue] = std::chrono::duration<double, std::milli>(phaseLap - packetStartedAt()).count();

    const auto drawn = [&] {
        phaseTiming.Phase(DrawRowVectors);
        if (!profile) return DrawVerdict::Drawn;
        auto& pending = pendingDrawPhases();
        pending.phases = true;
        pending.captures = captures;
        pending.ms = phaseMs;
        pending.tailAt = phaseLap;
        return DrawVerdict::Drawn;
    };
    const std::uint64_t traceToken = TraceShaderCorrelation() ? NextDrawTraceToken().fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    if (traceToken != 0) {
        AgcDriver::CaptureTrace::LogBestEffort("draw-attempt token=%llu submission=%llu queue=%x offset=%zu",
                static_cast<unsigned long long>(traceToken), static_cast<unsigned long long>(submission.serial), submission.queue, packetOffset);
    }
    auto& drawParameters = tail.drawParameters;
    try {
        drawParameters = Pm4::ResolveDraw(packet, queue);
    } catch (...) {
        TraceDrawAttemptException(traceToken, "resolve-draw", submission.serial, submission.queue, packetOffset, std::current_exception());
        throw;
    }
    tail.pipelined = DrawPipeline::Active();
    const bool pipelined = tail.pipelined;
    static const bool pipelinedIndirect = std::getenv("APS5_NO_PIPELINED_INDIRECT") == nullptr;
    if (pipelined && drawParameters.indirect) {
        const auto& indirect = *drawParameters.indirect;
        auto& pipeline = DrawPipeline::Queue0();
        const std::uint64_t argumentsEnd = indirect.arguments + std::max<std::uint64_t>(indirect.RangeBytes(), indirect.recordBytes);
        const bool overlaps = pipeline.Busy() && (pipeline.Overlaps(indirect.arguments, static_cast<std::size_t>(argumentsEnd - indirect.arguments)) || (indirect.countIndirect && pipeline.Overlaps(indirect.countAddress, 4)));
        if (!pipelinedIndirect) {
            pipeline.Drain(DrawPipeline::DrainReason::Indirect);
        } else if (overlaps) {
            std::array<DrawPipeline::Range, 2> read{DrawPipeline::Range{indirect.arguments, argumentsEnd}, DrawPipeline::Range{indirect.countAddress, indirect.countAddress + 4}};
            pipeline.WaitWrittenAny(std::span(read.data(), indirect.countIndirect ? 2u : 1u), DrawPipeline::DrainReason::Indirect);
        }
    }
    auto& traceIndirect = tail.traceIndirect;
    if (const auto verdict = precheckDraw(queue, submission, packetOffset, packet, drawParameters, rejected, traceIndirect)) {
        TraceDrawAttemptFailure(traceToken, "precheck", "rejected", submission.serial, submission.queue, packetOffset, rejected);
        return *verdict;
    }
    phaseTiming.Phase(DrawRowPrecheck);
    using Stage = ShaderRecompiler::ShaderStage;
    using Role = ShaderRecompiler::ProgramRole;

    static const std::uint64_t dumpTarget = [] { const char* text = std::getenv("APS5_DUMP_DRAW_SHADERS"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();

    static const std::uint64_t dumpSlot1 = [] { const char* text = std::getenv("APS5_DUMP_DRAW_SLOT1"); return text ? std::strtoull(text, nullptr, 16) : 0ull; }();

    static const bool lockedPrepare = std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr;
    std::unique_lock gpuLock(GuestMemory::GpuMutex(), std::defer_lock);
    auto& localDevice = tail.localDevice;
    if (lockedPrepare) {

        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        gpuLock.lock();
        timing.Mark("gpu_mutex_wait");
        phaseTiming.Phase(DrawRowLockWait);
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(DrawRowLabels);
    } else if ((localDevice = device.Load()) == nullptr) {
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        std::lock_guard createLock(GuestMemory::GpuMutex());
        if (device == nullptr) device = std::make_shared<VulkanDevice>();
        localDevice = device;
    }
    timing.Mark("device_setup");
    phaseTiming.Phase(DrawRowVectors);

    const bool useDrawEntries = drawEntries() && !ShaderRecompiler::DebugProbeActive() && dumpTarget == 0 && dumpSlot1 == 0;
    tail.registerKey = useDrawEntries && registerKeyEnabled();
    const bool registerKey = tail.registerKey;
    auto& drawKey = tail.drawKey;
    std::array<std::uint64_t, 5> drawKeyComponents{};
    auto& entry = tail.entry;
    auto& decode = tail.decode;
    auto& dataEntry = tail.dataEntry;
    auto& shapeKey = tail.shapeKey;
    std::uint64_t drawIdentity = 0, drawAlias = 0;
    std::vector<DrawUserPatch> userPatches;
    if (registerKey) {
        const auto keyStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        std::shared_ptr<const DrawRelocation> relocation;
        bool aliasFirst = false;
        if (drawRelocationEnabled()) {
            if (relocationDrain() && relocationAwaitsLearn && DrawFinisher::Busy()) DrawFinisher::Get().Drain(QueueWaits::Site::FinisherRelocation);
            relocationAwaitsLearn = false;
            drawIdentity = drawIdentityKey(queue, *submission.shaders);
            std::lock_guard cacheLock(drawCacheMutex);
            const auto known = drawRelocations.find(drawIdentity);
            if (known != drawRelocations.end()) {
                auto& trial = drawRelocationTrials[drawIdentity];
                if (trial.idle != 0) --trial.idle;
                else relocation = known->second;
                aliasFirst = relocation != nullptr && trial.productive;
            }
        }
        drawKey = drawRegisterKey(queue, *submission.shaders, localDevice->Serial(), &shapeKey, traceDrawCache() ? &drawKeyComponents : nullptr, aliasFirst ? relocation.get() : nullptr, aliasFirst ? &drawAlias : nullptr);
        if (DrawFinisher::Busy()) {
            bool shapeMatters = false;
            bool cached = false;
            if (DrawFinisher::Overlap()) {
                std::lock_guard peekLock(drawCacheMutex);
                cached = !DrawFinisher::LookupDrainsEvery() && drawCache.contains(drawKey);
                if (drawDataHits()) {
                    const auto shape = drawShapes.find(shapeKey);
                    shapeMatters = !drawCache.contains(drawKey) && (shape == drawShapes.end() || !drawCache.contains(shape->second));
                }
            }
            static const bool targeted = std::getenv("APS5_NO_TARGETED_LOOKUP_WAIT") == nullptr;
            bool drain = false;
            if (DrawFinisher::Overlap() && targeted) {
                drain = !cached && DrawFinisher::Get().WaitPendingKey(drawKey, shapeMatters ? shapeKey : 0);
            } else {
                drain = !DrawFinisher::Overlap() || (!cached && DrawFinisher::Get().PendingKey(drawKey, shapeMatters ? shapeKey : 0));
                if (drain) DrawFinisher::Get().Drain(QueueWaits::Site::FinisherLookup);
            }
            if (DrawFinisher::Overlap()) DrawFinisher::NoteDrain(DrawFinisher::DrainSite::Lookup, drain);
        }
        std::unique_lock cacheLock(drawCacheMutex);
        ++drawEntryCounters.lookups;
        ++drawEntryCounters.registerKeyLookups;
        if (profile) drawEntryCounters.keyUs += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - keyStart).count();
        const auto found = drawCache.find(drawKey);
        if (found != drawCache.end()) {
            entry = found->second;
            decode = entry->decode;
        } else {
            ++drawEntryCounters.absent;
            if (relocation != nullptr && !aliasFirst) {
                cacheLock.unlock();
                drawRegisterKey(queue, *submission.shaders, localDevice->Serial(), nullptr, nullptr, relocation.get(), &drawAlias);
                cacheLock.lock();
            }
            if (drawIdentity != 0) {
                tail.relocationPending = {drawKey, drawAlias, drawIdentity};
                relocationAwaitsLearn = true;
            }
            if (drawAlias != 0) relocateDrawEntry(queue, drawAlias, entry, decode, userPatches);
            if (relocation != nullptr) {
                auto& trial = drawRelocationTrials[drawIdentity];
                const auto flips = flipsCounted.load(std::memory_order_relaxed);
                if (trial.lookups++ == 0) trial.windowFlip = flips;
                if (trial.lookups >= DrawRelocationTrial::Window && (!poolRelocationEnabled() || flips - trial.windowFlip >= DrawRelocationTrial::WindowFlips)) {
                    trial.productive = trial.hits * DrawRelocationTrial::MinimumHitShare >= trial.lookups;
                    if (!trial.productive) trial.idle = DrawRelocationTrial::IdleDraws;
                    trial.lookups = trial.hits = 0;
                }
            }
            if (traceDrawCache()) {
                auto& counters = drawEntryCounters;
                if (traceKeysEvicted.contains(drawKey)) ++counters.absentEvicted;
                else if (traceKeysSeen.contains(drawKey)) ++counters.absentNeverInserted;
                else if (traceShapes.contains(shapeKey)) ++counters.absentUserWords;
                else ++counters.absentNewRegisters;
            }
            if (decode == nullptr && drawDataHits() && !drawParameters.indirect) {
                if (const auto shape = drawShapes.find(shapeKey); shape != drawShapes.end()) {
                    const auto kept = drawCache.find(shape->second);
                    if (kept != drawCache.end() && kept->second->decode != nullptr && kept->second->shape == shapeKey) dataEntry = kept->second;
                }
                if (dataEntry != nullptr) ++drawEntryCounters.dataHits;
            }
        }
        if (traceDrawCache()) traceDrawKeyLookup(drawKeyComponents, found == drawCache.end());
        if (traceDrawCache()) {
            traceInsert(traceKeysSeen, drawKey);
            traceInsert(traceShapes, shapeKey);
        }
        maybeReportDrawCache(profile);
    }
    if (decode != nullptr && !DecodeProgramsCurrent(*decode, *submission.shaders)) {
        userPatches.clear();
        std::lock_guard cacheLock(drawCacheMutex);
        eraseDrawEntry(drawKey, entry);
        ++drawEntryCounters.absent;
        entry = nullptr;
        decode = nullptr;
    }
    if (dataEntry != nullptr && !DecodeProgramsCurrent(*dataEntry->decode, *submission.shaders)) dataEntry = nullptr;
    phaseTiming.Phase(DrawRowKeyLookupValidate);

    if (dataEntry != nullptr) {
        auto refreshed = std::make_shared<DrawDecode>(*dataEntry->decode);
        for (auto& program : refreshed->programs) {
            ReadUserWords(queue, program, false);
            if (program.merged) InitializeMerged(queue, program, program.mergedPointer, program.mergedPointerRequired, false);
        }
        if (verifyDrawDataHits()) {
            const auto full = decodeDraw(queue, submission, localDevice->graphicsContext());
            if (!sameDecode(*refreshed, *full)) {
                std::size_t differing = 0;
                while (differing < full->programs.size() && differing < refreshed->programs.size() && full->programs[differing].userData == refreshed->programs[differing].userData) ++differing;
                char message[320];
                std::snprintf(message, sizeof(message), "APS5_VERIFY_DRAW_DATA_HITS: a data-only hit's decode differs from the full decode (key 0x%llx, shape 0x%llx, %zu / %zu programs, first with differing user words %zu, path %u / %u, target 0x%llx / 0x%llx)", static_cast<unsigned long long>(drawKey), static_cast<unsigned long long>(shapeKey), refreshed->programs.size(), full->programs.size(), differing, static_cast<unsigned>(refreshed->state.stages.path), static_cast<unsigned>(full->state.stages.path), static_cast<unsigned long long>(refreshed->state.color.address), static_cast<unsigned long long>(full->state.color.address));
                throw std::runtime_error(message);
            }
        }
        decode = std::move(refreshed);
    }
    if (decode != nullptr && !userPatches.empty() && verifyDrawRecipe()) {
        auto patched = std::make_shared<DrawDecode>(*decode);
        for (const auto& patch : userPatches) patched->programs.at(patch.program).userData.at(patch.index) = patch.value;
        decode = std::move(patched);
    }
    try {
        resolveDrawDecode(queue, submission, localDevice->graphicsContext(), decode, registerKey, drawKey, profile);
    } catch (...) {
        TraceDrawAttemptFailure(traceToken, "decode", "exception", submission.serial, submission.queue, packetOffset, ExceptionReason(std::current_exception()));
        throw;
    }
    const auto& graphics = decode->state;
    auto& pixel = tail.pixel;
    pixel = decode->pixel;
    const bool writesPrograms = graphics.stages.mesh || drawParameters.indirect || !userPatches.empty();
    auto& writablePrograms = tail.writablePrograms;
    if (writesPrograms) writablePrograms = decode->programs;
    for (const auto& patch : userPatches) writablePrograms[patch.program].userData[patch.index] = patch.value;
    tail.programs = writesPrograms ? &writablePrograms : &decode->programs;
    const auto& programs = *tail.programs;
    if (std::any_of(programs.begin(), programs.end(), [](const DrawProgram& program) { return SkippedShader(program.binary.codeAddress); })) return drawn();
    const bool emptyIndirectSkip = EmptyIndirectSkip();
    auto& cpuIndirectPrograms = CpuIndirectPrograms();
    const auto recordDraws = [&](const Pm4::DrawArguments& arguments) { return arguments.count != 0 && arguments.instances != 0 && (!drawParameters.indexed || !drawParameters.indirect->indexLimitKnown || arguments.firstVertexOrIndex < drawParameters.indexCount); };
    if (emptyIndirectSkip && drawParameters.indirect && !programs.empty()) {
        const auto known = cpuIndirectPrograms.find(programs.front().binary.codeAddress);
        if (known != cpuIndirectPrograms.end() && known->second) {
            const auto& indirect = *drawParameters.indirect;
            recordQueuedLabelsBeforeRead(submission.queue);
            const auto readStart = std::chrono::steady_clock::now();
            const auto count = std::min(indirect.countIndirect ? Pm4::ReadDrawCount(indirect) : indirect.count, indirect.count);
            bool any = false;
            for (std::uint32_t record = 0; record < count && !any; ++record) any = recordDraws(Pm4::ReadDrawArguments(indirect, record));
            if (!any) {
                Graphics::CountEmptyIndirectDraw(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count());
                timing.Mark("empty_indirect_skipped");
                return drawn();
            }
        }
    }
    const auto setMeshIndexBuffer = [&](const Pm4::DrawParameters& parameters) {
        if (!graphics.stages.mesh) return;
        auto& words = writablePrograms.front().userData;
        require(programs.front().firstUserSgpr == 0 && words.size() >= ShaderRecompiler::MeshIndexBufferUserWord + 4, "mesh program lacks the hidden user words");
        const auto descriptor = Graphics::MeshIndexBufferDescriptor(parameters);
        std::copy(descriptor.begin(), descriptor.end(), words.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
    };
    if (!drawParameters.indirect) setMeshIndexBuffer(drawParameters);
    else if (graphics.stages.mesh) setMeshIndexBuffer(Pm4::DrawParameters{drawParameters.indexAddress, std::max(drawParameters.indexCount, 1u), drawParameters.indexSize, 1, 0, drawParameters.indexed});
    const std::vector<Role>& roles = decode->roles;
    static const bool tracePrograms = std::getenv("APS5_TRACE_DRAW_PROGRAMS") != nullptr;
    if (tracePrograms) {
        std::vector<DrawProgramTraceEntry> identities;
        identities.reserve(programs.size());
        for (std::size_t i = 0; i < programs.size(); ++i) {
            identities.push_back({programs[i].binary.stage, roles[i], programs[i].snapshot->codeAddress, programs[i].binary.codeAddress});
        }
        const auto line = FormatDrawProgramTrace(submission.serial, submission.queue, identities);
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    phaseTiming.Phase(DrawRowDecode);

    const auto locate = [&](std::uint32_t location) -> std::optional<std::pair<std::size_t, std::size_t>> {
        const auto words = UserDataWords(programs, roles, location);
        if (words.empty()) return std::nullopt;
        return words.front();
    };
    if (drawParameters.indirect) {
        auto& indirect = *drawParameters.indirect;
        const auto sgprOf = [&](std::uint32_t location) -> std::int32_t {
            const auto word = locate(location);
            if (!word || word->first != 0) return -1;
            return static_cast<std::int32_t>(programs.front().firstUserSgpr + word->second);
        };
        indirect.baseVertexSgpr = sgprOf(indirect.baseVertexLocation);
        indirect.startInstanceSgpr = sgprOf(indirect.startInstanceLocation);
        indirect.drawIndexSgpr = sgprOf(indirect.drawIndexLocation);
    }
    if (DrawFinisher::Busy()) {
        const bool drain = !DrawFinisher::Overlap();
        if (drain) DrawFinisher::Get().Drain(QueueWaits::Site::FinisherReads);
        else DrawFinisher::NoteDrain(DrawFinisher::DrainSite::Reads, false);
    }
    auto& memory = tail.memory;
    auto& linked = tail.linked;
    memory.reserve(2 * programs.size() + 32);
    AppendProgramMemory(memory, programs);
    linked.reserve(programs.size());
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        linked.push_back({roles[i], program.binary, program.userDataBase, program.firstUserSgpr, program.userData});
    }
    timing.Mark("prepare");
    phaseTiming.Phase(DrawRowProgramPrepare);

    auto& vertexInfos = tail.vertexInfos;
    vertexInfos.resize(programs.size());
    auto& decodeReads = tail.decodeReads;
    decodeReads.resize(programs.size());
    const auto decodeVertexInfo = [&](std::size_t i) {
        const auto& program = programs[i];
        if (program.binary.stage == Stage::Fragment || roles[i] == Role::GeometryBack) return;
        vertexInfos[i] = DecodeOrderedVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, decodeReads[i], 8u - program.firstUserSgpr);
        if (vertexInfos[i]) {
            vertexInfos[i]->paClVsOutCntl = PositionExportLayout(program.snapshot.get(), program.codeOffset, program.binary.code, readRegister(queue.context, 0x207), program.tailContinuation.get()) | graphics.layerExports;
            vertexInfos[i]->depthRemapMode = graphics.depthRemap.mode;
            Graphics::ApplyPointSize(graphics, *vertexInfos[i]);
        }
    };
    if (!registerKey) {
        for (std::size_t i = 0; i < programs.size(); ++i) decodeVertexInfo(i);
        phaseTiming.Phase(DrawRowDecode);
    }
    auto& shaderMemory = tail.shaderMemory;
    shaderMemory = std::make_unique<ShaderMemory>(memory, &queryPendingWrite, &observePendingWrite, hookWaitCounter(), &QueryPendingSpan);
    auto& results = tail.results;
    auto& compiledStages = tail.compiledStages;
    compiledStages.reserve(programs.size());
    auto& stages = tail.stages;
    stages.reserve(programs.size());
    auto& pushCursorBytes = tail.pushCursorBytes;

    auto& programResults = tail.programResults;
    programResults.assign(programs.size(), nullptr);

    auto& stageCaptures = tail.stageCaptures;
    stageCaptures.resize(programs.size());
    auto& matched = tail.matched;
    matched.resize(programs.size());
    auto& matchedRegions = tail.matchedRegions;
    matchedRegions.resize(programs.size());

    auto& fresh = tail.fresh;
    fresh.resize(programs.size());

    auto& recompiled = tail.recompiled;
    recompiled.assign(programs.size(), false);
    auto& dataCandidates = tail.dataCandidates;
    dataCandidates.resize(programs.size());
    auto& reused = tail.reused;
    reused.assign(programs.size(), false);
    auto& stageData = tail.stageData;
    stageData.resize(programs.size());
    auto& drawHit = tail.drawHit;
    auto& verifyHit = tail.verifyHit;
    tail.stageEntries = useDrawEntries && !(graphics.stages.tessellation && !drawParameters.indirect && drawParameters.instanceCount > 1u);
    const bool stageEntries = tail.stageEntries;
    try {
        lookupDraw(submission, localDevice, graphics, pixel, programs, roles, vertexInfos, stageEntries, registerKey, profile, drawKey, entry, matched, matchedRegions, drawHit, verifyHit, phaseTiming, phaseMs, dataEntry, dataCandidates, stageData);
    } catch (...) {
        TraceDrawAttemptException(traceToken, "lookup", submission.serial, submission.queue, packetOffset, std::current_exception());
        throw;
    }
    bool rebasedStages = false;
    for (std::size_t i = 0; i < matched.size(); ++i) {
        if (matched[i] != nullptr) stageCaptures[i].registeredViews = matched[i]->registeredViews;
        if (matched[i] == nullptr || (matched[i]->shaderDataPatches.empty() && matched[i]->descriptorPatches.empty() && matched[i]->rebasedWords.empty())) continue;
        rebasedStages = rebasedStages || !matched[i]->rebasedWords.empty();
        applyShaderDataPatches(*matched[i]);
        if (stageData[i].compiled != nullptr) stageData[i].compiled = patchDataWords(*matched[i], stageData[i].words);
    }
    const auto relocatedDecode = decode;
    if (!userPatches.empty()) {
        std::lock_guard cacheLock(drawCacheMutex);
        if (drawHit || verifyHit) {
            ++drawRelocationCounters.hits;
            if (rebasedStages) ++drawRelocationCounters.rebasedHits;
            ++drawRelocationTrials[drawIdentity].hits;
            if (const auto alias = drawAliases.find(drawAlias); alias != drawAliases.end()) {
                const auto donor = drawCache.find(alias->second);
                if (donor != drawCache.end() && drawCacheHits - donor->second->touched > drawCacheEntries() / 8) {
                    drawOrder.splice(drawOrder.begin(), drawOrder, donor->second->order);
                    donor->second->touched = drawCacheHits;
                    ++drawEntryCounters.touches;
                }
            }
        }
    }
    if (!userPatches.empty() && !drawHit) {
        auto current = std::make_shared<DrawDecode>(*decode);
        for (const auto& patch : userPatches) current->programs[patch.program].userData[patch.index] = patch.value;
        decode = std::move(current);
    }
    tail.patchedStages = std::any_of(stageData.begin(), stageData.end(), [](const StageDataHit& hit) { return hit.compiled != nullptr; });
    tail.relocatedDecode = relocatedDecode;
    std::uint64_t stageDataVerified = 0;
    const auto target = graphics.hasColorTarget ? graphics.color.address : 0;
    const VulkanDevice::DrawTraceContext trace{traceToken, submission.serial, submission.queue, packetOffset, target};
    const auto* tracePtr = trace.token != 0 ? &trace : nullptr;
    tail.traceToken = traceToken;
    tail.trace = trace;
    if (trace.token != 0) {
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& program = programs[i];
            AgcDriver::CaptureTrace::LogBestEffort("draw-selected token=%llu submission=%llu queue=%x offset=%zu snapshot=0x%llx code=%llx entryOffset=%zu entry=%llx stage=%u role=%u drawCacheHit=%u variantHit=%u target=%llx",
                    static_cast<unsigned long long>(trace.token), static_cast<unsigned long long>(submission.serial), submission.queue, packetOffset,
                    static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(program.snapshot.get())), static_cast<unsigned long long>(program.snapshot->codeAddress), program.codeOffset,
                    static_cast<unsigned long long>(program.snapshot->codeAddress + program.codeOffset), static_cast<unsigned>(program.binary.stage), static_cast<unsigned>(roles[i]),
                    drawHit ? 1u : 0u, matched[i] != nullptr ? 1u : 0u, static_cast<unsigned long long>(target));
        }
    }

    if (registerKey) {

        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (matched[i] != nullptr && drawHit && !verifyDrawRecipe()) {
                if (matched[i]->vertexInfo != nullptr) vertexInfos[i] = *matched[i]->vertexInfo;
                continue;
            }
            if (!dataCandidates[i].empty() && !verifyDrawDataHits()) {
                const auto& info = dataCandidates[i].front().first->vertexInfo;
                if (info != nullptr) vertexInfos[i] = *info;
                continue;
            }
            decodeVertexInfo(i);
            if (matched[i] != nullptr && verifyDrawRecipe() && (vertexInfos[i].has_value() != (matched[i]->vertexInfo != nullptr) || (vertexInfos[i] && !sameVertexInfo(*vertexInfos[i], *matched[i]->vertexInfo)))) {
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: stage %zu (program 0x%llx) of a hit has a vertex stage info unlike its variant's\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress));
                std::lock_guard cacheLock(drawCacheMutex);
                ++drawEntryCounters.verifyDecodeMismatches;
            }
        }
        phaseTiming.Phase(DrawRowDecode);
    }

    tail.submission = &submission;
    tail.submissionQueue = submission.queue;
    tail.packetOpcode = (packet[0] >> 8u) & 0xffu;
    tail.reg207 = readRegister(queue.context, 0x207);
    tail.dumpTarget = dumpTarget;
    tail.dumpSlot1 = dumpSlot1;
    tail.packetOffset = packetOffset;
    const bool handOff = pipelined && !drawParameters.indirect && !profile && !lockedPrepare && dumpTarget == 0 && dumpSlot1 == 0 && !tracePrograms && !verifyDrawDataHits() && deferredLabels().labels.empty() && DrawFinisher::Enabled();
    if (handOff) {
        tail.epoch = DrawPipeline::EpochToken().load(std::memory_order_relaxed);
        tail.submissionSerial = submission.serial;
        tail.shaders = submission.shaders;
        tail.packetOffset = static_cast<std::size_t>(packet.data() - submission.commands.data());
        noteFinisherHandOff(true);
        DrawFinisher::Pending held;
        if (DrawFinisher::Overlap()) held = heldWrites(tail);
        DrawFinisher::Get().EnqueuePrepared([this, tailHolder] {
            DelayStage(DelayedStage::Finisher);
            auto& job = *tailHolder;
            DrawPipeline::FollowEpoch(job.epoch);
            GuestMemory::SetCurrentPacket(job.packetOpcode, job.submissionQueue);
            PerformanceTimer finishTiming("Driver.DrawFinish");
            try {
                if (finishDraw(job, finishTiming) == DrawVerdict::Rejected) throw std::runtime_error(job.ownRejected);
            } catch (const std::exception& error) {
                CaptureTrace::Log("draw-error submission=%llu offset=%zu finisher=1 reason=%.256s", static_cast<unsigned long long>(job.submissionSerial), job.packetOffset, error.what());
                throw std::runtime_error("draw at submission " + std::to_string(job.submissionSerial) + " offset " + std::to_string(job.packetOffset) + " (finished on the draw finisher): " + error.what());
            }
        }, std::move(held));
        timing.Mark("draw_handed_off");
        return drawn();
    }
    noteFinisherHandOff(false);
    if (DrawFinisher::Busy()) {
        DrawFinisher::Get().Drain(QueueWaits::Site::FinisherKept);
        if (DrawFinisher::Overlap()) DrawFinisher::NoteDrain(DrawFinisher::DrainSite::Kept, true);
    }
    tail.queue = &queue;
    tail.packet = packet;
    tail.rejected = &rejected;
    tail.gpuLock = &gpuLock;
    return finishDraw(tail, timing);
}

DrawVerdict Driver::finishDraw(DrawTail& tail, PerformanceTimer& timing) {
    using Stage = ShaderRecompiler::ShaderStage;
    using Role = ShaderRecompiler::ProgramRole;
    const bool profile = tail.profile;
    const auto traceToken = tail.traceToken;
    const auto& trace = tail.trace;
    const auto* tracePtr = trace.token != 0 ? &trace : nullptr;
    const auto packetOffset = tail.packetOffset;
    const auto packet = tail.packet;
    auto& phaseMs = tail.phaseMs;
    auto& phaseLap = tail.phaseLap;
    auto& captures = tail.captures;
    auto& phaseTiming = tail.phaseTiming;
    auto& drawParameters = tail.drawParameters;
    const bool pipelined = tail.pipelined;
    static const bool pipelinedIndirect = std::getenv("APS5_NO_PIPELINED_INDIRECT") == nullptr;
    const auto drainBeforeInline = [&] {
        if (pipelined && DrawPipeline::Queue0().Busy()) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Indirect);
    };
    auto& traceIndirect = tail.traceIndirect;
    const auto dumpTarget = tail.dumpTarget;
    const auto dumpSlot1 = tail.dumpSlot1;
    auto& gpuLock = tail.gpuLock != nullptr ? *tail.gpuLock : tail.ownLock;
    auto& localDevice = tail.localDevice;
    const bool registerKey = tail.registerKey;
    auto& drawKey = tail.drawKey;
    auto& entry = tail.entry;
    auto& decode = tail.decode;
    auto& dataEntry = tail.dataEntry;
    auto& shapeKey = tail.shapeKey;
    const auto& graphics = decode->state;
    auto& pixel = tail.pixel;
    auto& writablePrograms = tail.writablePrograms;
    const auto& programs = *tail.programs;
    const std::vector<Role>& roles = decode->roles;
    auto& memory = tail.memory;
    auto& linked = tail.linked;
    auto& vertexInfos = tail.vertexInfos;
    auto& decodeReads = tail.decodeReads;
    auto& shaderMemory = tail.shaderMemory;
    auto& results = tail.results;
    auto& compiledStages = tail.compiledStages;
    auto& stages = tail.stages;
    auto& pushCursorBytes = tail.pushCursorBytes;
    auto& programResults = tail.programResults;
    auto& stageCaptures = tail.stageCaptures;
    auto& matched = tail.matched;
    auto& matchedRegions = tail.matchedRegions;
    auto& fresh = tail.fresh;
    auto& recompiled = tail.recompiled;
    auto& dataCandidates = tail.dataCandidates;
    auto& reused = tail.reused;
    auto& stageData = tail.stageData;
    std::uint64_t dataStagesReused = 0, dataStagesCompiled = 0, dataVerified = 0;
    auto& drawHit = tail.drawHit;
    auto& verifyHit = tail.verifyHit;
    const bool stageEntries = tail.stageEntries;
    const bool patchedStages = tail.patchedStages;
    std::uint64_t stageDataVerified = 0;
    Submission handedOff{};
    if (tail.queue == nullptr) {
        handedOff.serial = tail.submissionSerial;
        handedOff.queue = tail.submissionQueue;
        handedOff.shaders = tail.shaders;
    }
    const Submission& submission = tail.queue != nullptr ? *tail.submission : handedOff;
    std::string& rejected = tail.rejected != nullptr ? *tail.rejected : tail.ownRejected;
    static const QueueState noQueue{};
    const QueueState& queue = tail.queue != nullptr ? *tail.queue : noQueue;
    const auto drawn = [&] {
        phaseTiming.Phase(DrawRowVectors);
        if (!profile) return DrawVerdict::Drawn;
        auto& pending = pendingDrawPhases();
        pending.phases = true;
        pending.captures = captures;
        pending.ms = phaseMs;
        pending.tailAt = phaseLap;
        return DrawVerdict::Drawn;
    };
    const auto setMeshIndexBuffer = [&](const Pm4::DrawParameters& parameters) {
        if (!graphics.stages.mesh) return;
        auto& words = writablePrograms.front().userData;
        require(programs.front().firstUserSgpr == 0 && words.size() >= ShaderRecompiler::MeshIndexBufferUserWord + 4, "mesh program lacks the hidden user words");
        const auto descriptor = Graphics::MeshIndexBufferDescriptor(parameters);
        std::copy(descriptor.begin(), descriptor.end(), words.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
    };
    const auto locate = [&](std::uint32_t location) -> std::optional<std::pair<std::size_t, std::size_t>> {
        if (location == 0x280u) return std::nullopt;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (roles[i] == Role::Fragment || roles[i] == Role::GeometryBack || location < programs[i].userDataBase) continue;
            const auto word = location - programs[i].userDataBase + (8u - programs[i].firstUserSgpr);
            if (word < programs[i].userData.size()) return std::make_pair(i, static_cast<std::size_t>(word));
        }
        return std::nullopt;
    };
    const auto decodeVertexInfo = [&](std::size_t i) {
        const auto& program = programs[i];
        if (program.binary.stage == Stage::Fragment || roles[i] == Role::GeometryBack) return;
        decodeReads[i].clear();
        vertexInfos[i] = Graphics::DecodeVertexStageInfo(program.binary.header, program.binary.headerAddress, program.userData, &decodeReads[i], false, 8u - program.firstUserSgpr);
        if (vertexInfos[i]) vertexInfos[i]->paClVsOutCntl = PositionExportLayout(program.snapshot.get(), program.codeOffset, program.binary.code, tail.reg207, program.tailContinuation.get()) | graphics.layerExports;
    };
    const auto fold = [&](const ShaderRecompiler::RecompileResult& main, Pm4::DrawParameters& parameters) { FoldDrawOffsets(main, programs.front(), parameters); };

    const auto freshRuns = [&](std::size_t i, std::vector<std::pair<std::uint64_t, std::uint64_t>>& runs, std::vector<std::uint32_t>& words) {
        std::vector<ShaderRecompiler::MemoryRegion> regions(stageCaptures[i].regions.begin(), stageCaptures[i].regions.end());
        for (const auto& read : decodeReads[i]) regions.push_back({read.address, read.Bytes()});
        std::stable_sort(regions.begin(), regions.end(), [](const ShaderRecompiler::MemoryRegion& a, const ShaderRecompiler::MemoryRegion& b) { return a.guestAddress < b.guestAddress; });
        for (const auto& region : regions) {
            runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
            const auto count = region.bytes.size() / sizeof(std::uint32_t);
            const auto offset = words.size();
            words.resize(offset + count);
            std::memcpy(words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        }
    };
    const auto verifyDataStage = [&](std::size_t i, const DispatchVariant& kept) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
        std::vector<std::uint32_t> words;
        freshRuns(i, runs, words);
        const auto& compiled = *stageCaptures[i].compiled;
        const char* what = runs != kept.runs ? "runs" : words != kept.words ? "words" : vertexInfos[i].has_value() != (kept.vertexInfo != nullptr) || (vertexInfos[i] && !sameVertexInfo(*vertexInfos[i], *kept.vertexInfo)) ? "vertex stage info" : compiled.variantId != kept.compiled->variantId ? "variant" : compiled.pushConstants != kept.compiled->pushConstants ? "push constants" : nullptr;
        ++dataVerified;
        if (what == nullptr) return;
        char message[320];
        std::snprintf(message, sizeof(message), "APS5_VERIFY_DRAW_DATA_HITS: stage %zu (program 0x%llx) of a data-only hit differs from its full build in its %s (%zu runs / %zu words kept, %zu / %zu fresh; variant 0x%llx / 0x%llx; key 0x%llx, shape 0x%llx)", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), what, kept.runs.size(), kept.words.size(), runs.size(), words.size(), static_cast<unsigned long long>(kept.compiled->variantId), static_cast<unsigned long long>(compiled.variantId), static_cast<unsigned long long>(drawKey), static_cast<unsigned long long>(shapeKey));
        throw std::runtime_error(message);
    };
    const auto verifyStageData = [&](std::size_t i) {
        const auto& hit = stageData[i];
        const auto& kept = *matched[i];
        std::vector<std::pair<std::uint64_t, std::uint64_t>> runs;
        std::vector<std::uint32_t> words;
        freshRuns(i, runs, words);
        const auto& fresh = *stageCaptures[i].compiled;
        const auto& patched = *hit.compiled;
        const bool sameBindings = fresh.bindings.size() == patched.bindings.size() && std::equal(fresh.bindings.begin(), fresh.bindings.end(), patched.bindings.begin(), [](const auto& a, const auto& b) { return a.kind == b.kind && a.role == b.role && a.binding == b.binding && a.count == b.count && a.guestDescriptor == b.guestDescriptor; });
        const char* what = runs != kept.runs ? "runs" : words != hit.words ? "words" : fresh.variantId != patched.variantId ? "variant" : fresh.pushConstants != patched.pushConstants ? "push constants" : !sameBindings ? "bindings" : nullptr;
        ++stageDataVerified;
        if (what == nullptr) return;
        char message[256];
        std::snprintf(message, sizeof(message), "APS5_VERIFY_DRAW_DATA_HITS: stage %zu (program 0x%llx) of a stage data hit differs from its full build in its %s (%zu data positions refreshed)", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), what, kept.dataPositions.size());
        throw std::runtime_error(message);
    };
    std::optional<Graphics::IndirectDrawPath> indirectCpu;
    std::size_t completedSubdraws = 0;
    std::vector<std::uint32_t> pushOffsets(programs.size(), 0);
    const bool verifyThreads = [] {
        const auto every = DrawFinisher::DefersOrdered() ? DrawFinisher::VerifyThreadsEvery() : 0u;
        static std::atomic<std::uint64_t> seen{0};
        return every != 0 && seen.fetch_add(1, std::memory_order_relaxed) % every == 0;
    }();
    std::vector<ShaderRecompiler::MemoryRegion> memoryBefore;
    Pm4::DrawParameters drawBefore{};
    if (verifyThreads) {
        memoryBefore = memory;
        drawBefore = drawParameters;
    }
    std::optional<ShaderRecompiler::MeshBarycentricLayout> meshBarycentrics;
    std::vector<std::size_t> resultIndex(programs.size(), 0);
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (roles[i] == Role::GeometryBack) continue;
        const auto& program = programs[i];
        pushCursorBytes = Graphics::StagePushOffset(pushCursorBytes, program.binary.stage, localDevice->GraphicsPipelineLibraries());
        pushOffsets[i] = pushCursorBytes;
        if (drawHit) {

            programResults[i] = stageData[i].compiled != nullptr ? stageData[i].compiled.get() : matched[i]->compiled.get();
            memory.insert(memory.end(), matchedRegions[i].begin(), matchedRegions[i].end());
        } else {
            const auto candidate = std::find_if(dataCandidates[i].begin(), dataCandidates[i].end(), [&](const auto& kept) { return kept.first->pushOffset == pushCursorBytes; });
            if (candidate != dataCandidates[i].end() && !verifyDrawDataHits()) {
                matched[i] = candidate->first;
                stageCaptures[i].registeredViews = matched[i]->registeredViews;
                matchedRegions[i] = std::move(candidate->second);
                reused[i] = true;
                programResults[i] = stageData[i].compiled != nullptr ? stageData[i].compiled.get() : matched[i]->compiled.get();
                memory.insert(memory.end(), matchedRegions[i].begin(), matchedRegions[i].end());
            } else {
                if (!dataCandidates[i].empty() && !verifyDrawDataHits()) {
                    DrawFinisher::WaitTurn();
                    decodeVertexInfo(i);
                }
                if (program.binary.stage == ShaderRecompiler::ShaderStage::Fragment) {
                    for (auto previous = i; previous-- > 0;) {
                        if (programResults[previous] == nullptr) continue;
                        pixel = Graphics::DefaultUnexportedInputs(decode->pixel, programResults[previous]->parameterExports);
                        break;
                    }
                }
                compiledStages.push_back(materializeDrawStage(i, pushCursorBytes, queue, submission, packetOffset, trace.token, programs, graphics, pixel, vertexInfos, memory, linked, drawParameters, localDevice, *shaderMemory, stageCaptures, recompiled, drawHit, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs, rejected, meshBarycentrics));
                if (!rejected.empty()) return DrawVerdict::Rejected;
                programResults[i] = compiledStages.back().get();
                if (candidate != dataCandidates[i].end()) verifyDataStage(i, *candidate->first);
                if (stageData[i].compiled != nullptr && verifyDrawDataHits()) verifyStageData(i);
            }
            if (dataEntry != nullptr && reused[i]) ++dataStagesReused;
            else if (dataEntry != nullptr) ++dataStagesCompiled;
        }
        const auto& result = *programResults[i];
        if (i == 0 && drawParameters.indirect) {
            indirectCpu = ClassifyIndirectDraw(result, graphics, programs.front(), localDevice, drawParameters, traceIndirect);
            if (EmptyIndirectSkip()) CpuIndirectPrograms()[programs.front().binary.codeAddress] = indirectCpu.has_value();
        } else if (i == 0) {
            fold(result, drawParameters);
        }
        try {
            const auto slotEnd = (pushCursorBytes / Graphics::PipelinePushSlotBytes + 1u) * Graphics::PipelinePushSlotBytes;
            require(result.pushConstants.size() <= slotEnd - pushCursorBytes, "stage push constants exceed the pipeline push constant block");
            stages.push_back({program.binary.stage, &result, result.pushConstants.empty() ? 0u : pushCursorBytes, stageCaptures[i].registeredViews.get()});
            pushCursorBytes += static_cast<std::uint32_t>(result.pushConstants.size());
        } catch (...) {
            TraceMaterializationFailure(trace.token, submission, packetOffset, "push-constant-validation", completedSubdraws, std::current_exception());
            throw;
        }
    }

    if (verifyThreads && rejected.empty()) {
        DrawFinisher::WaitTurn();
        ShaderMemory ordered(memoryBefore, &queryPendingWrite, &observePendingWrite, hookWaitCounter(), &QueryPendingSpan);
        auto orderedMemory = memoryBefore;
        auto orderedCaptures = stageCaptures;
        auto orderedRecompiled = recompiled;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (roles[i] == Role::GeometryBack) continue;
            if (recompiled[i]) {
                const auto again = materializeDrawStage(i, pushOffsets[i], queue, submission, packetOffset, trace.token, programs, graphics, pixel, vertexInfos, orderedMemory, linked, drawBefore, localDevice, ordered, orderedCaptures, orderedRecompiled, drawHit, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs, rejected, meshBarycentrics);
                const auto& parallel = *stageCaptures[i].compiled;
                const auto sameBindingWords = [&] {
                    for (std::size_t b = 0; b < parallel.bindings.size(); ++b) {
                        if (parallel.bindings[b].guestDescriptor != again->bindings[b].guestDescriptor) return false;
                    }
                    return true;
                };
                const char* what = again == stageCaptures[i].compiled ? nullptr : parallel.variantId != again->variantId ? "variant" : parallel.specializationId != again->specializationId ? "specialization" : parallel.pushConstants != again->pushConstants ? "push constants" : parallel.poisonedSrtReads != again->poisonedSrtReads ? "poisoned reads" : parallel.bindings.size() != again->bindings.size() ? "binding count" : !sameBindingWords() ? "binding words" : nullptr;
                if (what != nullptr) {
                    std::fprintf(stderr, "[finisher-threads] APS5_VERIFY_FINISHER_THREADS: stage %zu (program 0x%llx, key 0x%llx) captured out of order differs from a capture at its turn in its %s\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), static_cast<unsigned long long>(drawKey), what);
                    std::fflush(stderr);
                    std::abort();
                }
            } else if (programResults[i] != nullptr) {
                orderedMemory.insert(orderedMemory.end(), matchedRegions[i].begin(), matchedRegions[i].end());
            }
            if (i == 0) fold(*programResults[0], drawBefore);
        }
    }
    if (stageDataVerified != 0) {
        std::lock_guard cacheLock(drawCacheMutex);
        drawEntryCounters.stageDataVerified += stageDataVerified;
    }
    if (dataEntry != nullptr) {
        std::lock_guard cacheLock(drawCacheMutex);
        drawEntryCounters.dataStagesReused += dataStagesReused;
        drawEntryCounters.dataStagesCompiled += dataStagesCompiled;
        drawEntryCounters.dataVerified += dataVerified;
    }
    const bool shapeRefresh = Graphics::DrawPipelineEnabled();
    const auto refreshSeenTwice = [&] {
        std::uint64_t data = shapeKey;
        bool stored = false;
        const auto mix = [&data](std::uint64_t value) {
            data ^= value + 0x9e3779b97f4a7c15ull + (data << 6u) + (data >> 2u);
        };
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& capture = stageCaptures[i];
            if (reused[i] || capture.compiled == nullptr || !CacheableResult(*capture.compiled)) continue;
            stored = true;
            const auto& words = programs[i].userData;
            for (std::size_t w = 0; w < words.size(); ++w) {
                if (w >= 64u || (capture.userDataMask >> w & 1u) != 0) mix((static_cast<std::uint64_t>(i) << 40u) ^ (static_cast<std::uint64_t>(w) << 32u) ^ words[w]);
            }
        }
        if (!stored) return false;
        static std::array<std::atomic<std::uint64_t>, 4096> lastData{};
        return lastData[shapeKey % lastData.size()].exchange(data, std::memory_order_relaxed) == data;
    };
    const bool shapeNeedsEntry = shapeRefresh && registerKey && shapeKey != 0 && drawDataHits() && !drawParameters.indirect && dataEntry != nullptr && dataStagesCompiled != 0 && refreshSeenTwice();
    cacheDrawStages(stageEntries, drawHit, drawParameters, indirectCpu, programs, stageCaptures, vertexInfos, decodeReads, verifyHit, matched, stageData, fresh, drawKey, registerKey, decode, phaseTiming, reused, entry, shapeKey, tail.relocationPending, shapeNeedsEntry);
    if (tail.relocationPending.key != 0) {
        std::lock_guard cacheLock(drawCacheMutex);
        learnPendingRelocation(tail.relocationPending);
    }
    timing.Mark("shader_compile_and_link");

    for (const auto& reads : decodeReads) {
        for (const auto& read : reads) memory.push_back({read.address, read.Bytes()});
    }

    if (tail.queue != nullptr && pipelined && !deferredLabels().labels.empty()) DrawPipeline::Queue0().Drain(DrawPipeline::DrainReason::Labels);
    if (recordQueuedLabelsAfterCapture(submission.queue, memory)) return draw(*tail.queue, packet, submission, packetOffset, rejected);

    bool rectListBuilt = false;

    std::size_t rectIndex = 0;
    const auto buildRectList = [&] {
        try {
            phaseTiming.Phase(DrawRowVectors);
            require(programs.size() == 2 && programResults[0] != nullptr && programResults[1] != nullptr, "rect-list requires vertex and fragment programs");
            auto rectangle = DrawRectangle(*programs[0].snapshot, programs[1].snapshot, programResults[0]->variantId, programResults[1]->variantId, localDevice->Target());
            auto& control = graphics.provokingVertexMode == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT ? rectangle.lastVertexControl : rectangle.control;
            if (rectListBuilt) {
                results[rectIndex] = std::move(control);
                results[rectIndex + 1] = std::move(rectangle.evaluation);
                phaseTiming.Phase(DrawRowRectList);
                return;
            }
            require(stages.size() == 2, "rect-list requires vertex and fragment programs");
            results.reserve(programs.size() + 2u);
            rectIndex = results.size();
            results.push_back(std::move(control));
            results.push_back(std::move(rectangle.evaluation));
            stages.insert(stages.begin() + 1, {{Stage::TessellationControl, &results[rectIndex], 0}, {Stage::TessellationEvaluation, &results[rectIndex + 1], 0}});
            rectListBuilt = true;
            phaseTiming.Phase(DrawRowRectList);
        } catch (...) {
            TraceMaterializationFailure(trace.token, submission, packetOffset, "rect-list", completedSubdraws, std::current_exception());
            throw;
        }
    };
    if (graphics.rectList) buildRectList();
    if (graphics.stages.tessellationGeometry) {
        static std::mutex captureMutex;
        static std::map<std::pair<std::uint32_t, std::uint32_t>, ShaderRecompiler::RecompileResult> captureShaders;
        const auto& geometry = *graphics.stages.tessellationGeometry;
        {
            std::lock_guard captureLock(captureMutex);
            auto found = captureShaders.find({geometry.esgsItemSize, geometry.primitivesPerGroup});
            if (found == captureShaders.end()) found = captureShaders.emplace(std::pair{geometry.esgsItemSize, geometry.primitivesPerGroup}, ShaderRecompiler::BuildTessellationCaptureGeometryShader(geometry, localDevice->Target(), localDevice->GeometryLimits())).first;
            results.push_back(found->second);
        }
        const auto evaluation = std::ranges::find_if(stages, [](const Graphics::CompiledShader& stage) { return stage.stage == Stage::TessellationEvaluation; });
        require(evaluation != stages.end(), "tessellation followed by a geometry stage has no domain program");
        stages.insert(std::next(evaluation), Graphics::CompiledShader{Stage::Geometry, &results.back(), 0});
    }
    if (!programs.empty() && programResults.back() != nullptr && programResults.back()->barycentricEmulation.active && graphics.stages.path == Graphics::ShaderPath::Geometry) {
        require(localDevice->Target().mesh.has_value(), "a pixel shader on the mesh path reads barycentrics, and this device has neither VK_KHR_fragment_shader_barycentric nor mesh shaders (VK_EXT_mesh_shader), so they cannot be provided or emulated");
        require(!graphics.rectList && graphics.stages.mesh && programs.size() == 2 && programResults[0] != nullptr && programResults[1] != nullptr && stages.size() == 2, "a pixel shader that reads barycentrics needs exactly one mesh program before it to emulate them");
        meshBarycentrics = ShaderRecompiler::MeshBarycentricLayoutOf(*programResults[1]);
        if (programResults[0]->meshBarycentrics != meshBarycentrics) {
            const auto pushBytes = programResults[0]->pushConstants.size();
            const auto exports = programResults[0]->parameterExports;
            compiledStages.push_back(materializeDrawStage(0, pushOffsets[0], queue, submission, packetOffset, traceToken, programs, graphics, pixel, vertexInfos, memory, linked, drawParameters, localDevice, *shaderMemory, stageCaptures, recompiled, drawHit, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs, rejected, meshBarycentrics));
            if (!rejected.empty()) return DrawVerdict::Rejected;
            programResults[0] = compiledStages.back().get();
            stages[0].program = programResults[0];
            stages[0].registeredImageViews = stageCaptures[0].registeredViews.get();
            require(programResults[0]->pushConstants.size() == pushBytes && programResults[0]->parameterExports == exports, "pairing the mesh program with its pixel shader changed the push constants or the parameter exports the pixel shader was built for");
        }
        require(programResults[0]->meshBarycentrics == meshBarycentrics, "the mesh program was compiled for a different barycentric layout than its pixel shader");
    } else if (!graphics.rectList && !programs.empty() && programResults.back() != nullptr && programResults.back()->barycentricEmulation.active) {
        require(graphics.stages.path == Graphics::ShaderPath::Vertex && programs.size() == 2 && programResults[0] != nullptr && stages.size() == 2, "a pixel shader that reads barycentrics without VK_KHR_fragment_shader_barycentric needs a vertex shader before it");
        const auto geometryLimits = localDevice->GeometryLimits();
        results.reserve(programs.size() + 2u);
        if (geometryLimits || !localDevice->Target().tessellation) {
            results.push_back(ShaderRecompiler::BuildBarycentricGeometryShader(*programResults[0], *programResults[1], localDevice->Target(), geometryLimits));
            stages.insert(stages.begin() + 1, Graphics::CompiledShader{Stage::Geometry, &results.back(), 0});
        } else {
            require((graphics.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && (!graphics.primitiveRestart || !localDevice->PrimitiveListRestart())) || (graphics.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP && (!graphics.primitiveRestart || !drawParameters.indexed)), "a pixel shader that reads barycentrics without VK_KHR_fragment_shader_barycentric or geometry shaders needs a triangle list, with primitive restart only on a device without VK_EXT_primitive_topology_list_restart, or a triangle strip, with primitive restart only in a non-indexed draw");
            auto patches = ShaderRecompiler::BuildBarycentricTessellationShaders(*programResults[0], *programResults[1], localDevice->Target(), localDevice->MaxFragmentInputComponents(), graphics.provokingVertexMode == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT ? 2u : 0u);
            const auto first = results.size();
            results.push_back(std::move(patches.control));
            results.push_back(std::move(patches.evaluation));
            stages.insert(stages.begin() + 1, {{Stage::TessellationControl, &results[first], 0}, {Stage::TessellationEvaluation, &results[first + 1], 0}});
        }
    }
    auto& snapshots = tail.snapshots;
    const auto snapshot = [&] {
        snapshots.clear();
        snapshots.reserve(memory.size());
        for (const auto& region : memory) snapshots.push_back({region.guestAddress, region.bytes});
    };
    snapshot();
    timing.Mark("post_compile_prepare");
    const auto lockForDraw = [&] {
        require(tail.queue != nullptr, "the draw finisher reached a draw it cannot commit through the pipeline");
        if (gpuLock.owns_lock()) return;
        phaseTiming.Phase(DrawRowVectors);
        GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
        gpuLock.lock();
        timing.Mark("gpu_mutex_wait");
        phaseTiming.Phase(DrawRowLockWait);

        if (auto current = device.Load(); current != nullptr && current != localDevice) {
            static std::atomic<std::uint64_t> replaced{0};
            std::fprintf(stderr, "[draw] device replaced during unlocked preparation (%llu)\n", static_cast<unsigned long long>(++replaced));
            localDevice = std::move(current);
        }

        recordLabelsForPacket(localDevice.get(), submission.queue);
        phaseTiming.Phase(DrawRowLabels);
    };
    if (drawParameters.indirect && indirectCpu) {

        const auto indirect = *drawParameters.indirect;
        results.reserve(programs.size() + 2u);
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (programResults[i] == nullptr) continue;
            resultIndex[i] = results.size();
            results.push_back(ShaderRecompiler::RecompileResult(*programResults[i]));
            for (auto& stage : stages) {
                if (stage.program == programResults[i]) stage.program = &results[resultIndex[i]];
            }
            programResults[i] = &results[resultIndex[i]];
        }
        recordQueuedLabelsBeforeRead(submission.queue);
        const auto readStart = std::chrono::steady_clock::now();
        const auto count = std::min(indirect.countIndirect ? Pm4::ReadDrawCount(indirect) : indirect.count, indirect.count);
        std::vector<Pm4::DrawArguments> records;
        for (std::uint32_t record = 0; record < count; ++record) records.push_back(Pm4::ReadDrawArguments(indirect, record));
        Graphics::CountIndirectDraw(*indirectCpu, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - readStart).count());
        const auto baseVertexWords = UserDataWords(programs, roles, indirect.baseVertexLocation);
        const auto startInstanceWords = UserDataWords(programs, roles, indirect.startInstanceLocation);
        const auto drawIndexWords = indirect.drawIndexEnabled ? UserDataWords(programs, roles, indirect.drawIndexLocation) : std::vector<std::pair<std::size_t, std::size_t>>{};
        const auto drawable = [&](const Pm4::DrawArguments& arguments) { return arguments.count != 0 && arguments.instances != 0 && (!drawParameters.indexed || !indirect.indexLimitKnown || arguments.firstVertexOrIndex < drawParameters.indexCount); };
        std::size_t lastDrawable = records.size();
        for (std::size_t record = 0; record < records.size(); ++record) {
            if (drawable(records[record])) lastDrawable = record;
        }
        bool recordedOne = false;
        for (std::uint32_t record = 0; record < records.size(); ++record) {
            const auto& arguments = records[record];
            if (traceIndirect) std::fprintf(stderr, "[draw]   record %u: count %u instances %u first %u vertexOffset %u startInstance %u\n", record, arguments.count, arguments.instances, arguments.firstVertexOrIndex, arguments.vertexOffset, arguments.firstInstance);
            if (arguments.count == 0 || arguments.instances == 0) continue;
            std::set<std::size_t> patched;
            const auto patch = [&](const std::vector<std::pair<std::size_t, std::size_t>>& words, std::uint32_t value) {
                for (const auto& [program, word] : words) {
                    writablePrograms[program].userData[word] = value;
                    patched.insert(program);
                }
            };
            patch(baseVertexWords, indirect.recordBytes == 20 ? arguments.vertexOffset : arguments.firstVertexOrIndex);
            patch(startInstanceWords, arguments.firstInstance);
            patch(drawIndexWords, record);
            if (graphics.stages.tessellation) {
                for (std::size_t i = 0; i < programs.size(); ++i) {
                    if (programs[i].binary.stage == Stage::TessellationControl || programs[i].binary.stage == Stage::TessellationEvaluation) patched.insert(i);
                }
            }
            Pm4::DrawParameters direct{0, arguments.count, 0, arguments.instances, drawParameters.flags, drawParameters.indexed, 0, 0};
            if (drawParameters.indexed) {
                if (indirect.indexLimitKnown) {
                    if (arguments.firstVertexOrIndex >= drawParameters.indexCount) continue;
                    direct.indexCount = std::min(arguments.count, drawParameters.indexCount - arguments.firstVertexOrIndex);
                } else {
                    const auto end = static_cast<std::uint64_t>(arguments.firstVertexOrIndex) + arguments.count;
                    require(end <= std::numeric_limits<std::uint32_t>::max(), "indirect draw index range overflow");
                    require(end * drawParameters.indexSize <= std::numeric_limits<std::uint64_t>::max() - drawParameters.indexAddress, "indirect draw index range address overflow");
                    direct.indexCount = arguments.count;
                }
                direct.indexAddress = drawParameters.indexAddress + static_cast<std::uint64_t>(arguments.firstVertexOrIndex) * drawParameters.indexSize;
                direct.indexSize = drawParameters.indexSize;
            } else {
                direct.firstVertex = indirect.indxOffset;
            }
            if (graphics.stages.mesh) {
                setMeshIndexBuffer(direct);
                patched.insert(0);
            }
            for (const auto programIndex : patched) {
                auto& result = results[resultIndex[programIndex]];
                const auto pushBytes = result.pushConstants.size();
                decodeVertexInfo(programIndex);
                result = *materializeDrawStage(programIndex, pushOffsets[programIndex], queue, submission, packetOffset, trace.token, programs, graphics, pixel, vertexInfos, memory, linked, programs[programIndex].binary.stage == Stage::TessellationControl || programs[programIndex].binary.stage == Stage::TessellationEvaluation ? direct : drawParameters, localDevice, *shaderMemory, stageCaptures, recompiled, drawHit, matched, matchedRegions, profile, dumpTarget, dumpSlot1, captures, phaseTiming, phaseMs, rejected, meshBarycentrics);
                for (auto& stage : stages) if (stage.program == &result) stage.registeredImageViews = stageCaptures[programIndex].registeredViews.get();
                if (!rejected.empty()) return DrawVerdict::Rejected;
                require(result.pushConstants.size() == pushBytes, "patched program changed its push constant layout");
            }
            fold(*programResults[0], direct);
            if (graphics.rectList && patched.contains(0)) buildRectList();
            snapshot();
            if (auto known = localDevice->KnownDrawRejection(graphics, stages)) {
                rejected = std::move(*known);
                return DrawVerdict::Rejected;
            }
            if (pipelined && pipelinedIndirect && record == lastDrawable && !gpuLock.owns_lock() && deferredLabels().labels.empty()) {
                runCaptureCheckHook();
                if (tail.queue != nullptr && capturedMemoryChanged(memory)) {
                    const RecaptureScope recapture;
                    return draw(*tail.queue, tail.packet, submission, tail.packetOffset, rejected);
                }
                phaseTiming.Phase(DrawRowVectors);
                auto writes = drawWriteRanges(graphics, stages);
                const auto commitQueue = submission.queue;
                const auto commitPacket = tail.packetOpcode;
                const auto epoch = DrawPipeline::EpochToken().load(std::memory_order_relaxed);
                auto commit = [this, localDevice = std::move(localDevice), decode = std::move(decode), direct, stages = std::span<const Graphics::CompiledShader>(stages), snapshots = std::span<const Graphics::GuestMemorySnapshot>(snapshots), commitQueue, commitPacket, epoch, storage = std::move(tail.storage), shaderMemory = std::move(shaderMemory), entry = std::move(entry), dataEntry = std::move(dataEntry)] {
                    DrawPipeline::FollowEpoch(epoch);
                    GuestMemory::SetCurrentPacket(commitPacket, commitQueue);
                    commitDraw(localDevice, commitQueue, decode->state, direct, stages, snapshots, nullptr, {}, 0);
                };
                auto held = std::make_shared<decltype(commit)>(std::move(commit));
                DrawPipeline::Queue0().Enqueue([held] { (*held)(); }, std::move(writes), 0, {}, {}, false, DrawPipeline::ItemKind::IndirectDraw);
                phaseTiming.Phase(DrawRowLockWait);
                timing.Mark("draw_enqueued");
                return drawn();
            }
            drainBeforeInline();
            lockForDraw();
            if (!recordedOne) {
                runCaptureCheckHook();
                if (tail.queue != nullptr && capturedMemoryChanged(memory, snapshots)) {
                    gpuLock.unlock();
                    const RecaptureScope recapture;
                    return draw(*tail.queue, tail.packet, submission, tail.packetOffset, rejected);
                }
            }
            recordedOne = true;
            noteDrawWriters(stages, submission.queue);
            phaseTiming.Phase(DrawRowVectors);
            localDevice->Draw(graphics, direct, stages, snapshots, nullptr, tracePtr);
            phaseTiming.Phase(DrawRowGraphics);
        }
        timing.Mark("draw_and_resource_release");
        return drawn();
    }

    auto& recipeStages = tail.recipeStages;
    if (registerKey && !drawParameters.indirect && !patchedStages && !graphics.stages.tessellationGeometry && Graphics::DrawRecipes()) {
        recipeStages.reserve(programs.size());
        for (std::size_t i = 0; i < programs.size(); ++i) recipeStages.push_back(drawHit ? matched[i] : fresh[i]);
        if (std::all_of(recipeStages.begin(), recipeStages.end(), [](const std::shared_ptr<DispatchVariant>& variant) { return variant == nullptr; })) recipeStages.clear();
    }
    std::shared_ptr<const DrawRecipe> recipe;
    if (drawHit && !recipeStages.empty()) {
        recipe = findDrawRecipe(drawKey, recipeStages);
        if (recipe == nullptr) VulkanDevice::NoteDrawRecipeMiss(VulkanDevice::DrawRecipePrecheck::NoRecipe);
    }
    if (recipe == nullptr) {
        if (auto known = localDevice->KnownDrawRejection(graphics, stages)) {
            rejected = std::move(*known);
            return DrawVerdict::Rejected;
        }
    }
    if (static const bool nameVariants = std::getenv("APS5_PROFILE_GPU_DRAWS") != nullptr; nameVariants && stages.size() == programs.size()) {
        static std::mutex namedMutex;
        static std::set<std::uint64_t> named;
        std::lock_guard namedLock(namedMutex);
        for (std::size_t i = 0; i < stages.size(); ++i) {
            if (stages[i].program == nullptr || !named.insert(stages[i].program->variantId).second) continue;
            std::fprintf(stderr, "[drawvariant] 0x%llx stage %d code 0x%llx\n", static_cast<unsigned long long>(stages[i].program->variantId), static_cast<int>(stages[i].stage), static_cast<unsigned long long>(programs[i].binary.codeAddress));
        }
    }
    if (pipelined && !drawParameters.indirect && (tail.queue == nullptr || deferredLabels().labels.empty()) && !localDevice->NeedsMeshLayerWindows(graphics, drawParameters)) {
        runCaptureCheckHook();
        if (tail.queue != nullptr && capturedMemoryChanged(memory)) {
            const RecaptureScope recapture;
            return draw(*tail.queue, tail.packet, submission, tail.packetOffset, rejected);
        }
        phaseTiming.Phase(DrawRowVectors);
        auto writes = drawWriteRanges(graphics, stages);
        const auto commitQueue = submission.queue;
        const auto commitPacket = tail.packetOpcode;
        const auto epoch = DrawFinisher::OnFinisherThread() ? tail.epoch : DrawPipeline::EpochToken().load(std::memory_order_relaxed);
        auto reads = QueuedDrawReadRanges(drawParameters, stages);
        std::shared_ptr<DrawAheadSlot> ahead;
        if (recipe == nullptr && stages.size() <= DrawAheadSlot::MaxStages && DrawPipeline::PreparesAhead()) {
            ahead = std::make_shared<DrawAheadSlot>();
            ahead->device = localDevice;
            ahead->decode = decode;
            ahead->draw = drawParameters;
            ahead->shape = tail.shapeKey;
            std::copy(stages.begin(), stages.end(), ahead->stages.begin());
            ahead->stageCount = stages.size();
            ahead->result.walks.epoch = epoch;
            Graphics::CountDrawAhead(Graphics::DrawAheadEvent::Offered);
        }
        auto earlyDraw = DrawPipeline::ResolveSplit() != 0 ? std::make_shared<EarlyDraw>() : std::shared_ptr<EarlyDraw>();
        if (earlyDraw != nullptr) {
            earlyDraw->writes.reserve(8);
            earlyDraw->depth.reserve(4);
            earlyDraw->reads.reserve(32);
        }
        auto commit = [this, localDevice = std::move(localDevice), decode = std::move(decode), drawParameters, stages = std::span<const Graphics::CompiledShader>(stages), snapshots = std::span<const Graphics::GuestMemorySnapshot>(snapshots), recipe = std::move(recipe), recipeStages = &recipeStages, drawKey, shape = tail.shapeKey, commitQueue, commitPacket, epoch, trace = trace, ahead, storage = std::move(tail.storage), shaderMemory = std::move(shaderMemory), entry = std::move(entry), dataEntry = std::move(dataEntry), early = std::move(earlyDraw), itemReads = reads](const DrawPipeline::EarlyOffer* offer) mutable {
            if (offer != nullptr && offer->preProof != nullptr) return preProveDraw(localDevice, stages, ahead.get(), epoch, *offer->preProof);
            if (offer != nullptr && offer->probe != nullptr) {
                thread_local std::vector<std::pair<std::uint64_t, std::uint64_t>> probedDepth;
                DrawDepthRanges(decode->state, probedDepth);
                offer->probe->depth->insert(offer->probe->depth->end(), probedDepth.begin(), probedDepth.end());
                offer->probe->hintTemplate = early->hintTemplate;
                return false;
            }
            if (offer != nullptr) {
                early->twoAhead = offer->twoAhead;
                return offer->writes != nullptr && resolveDrawEarly(localDevice, decode->state, drawParameters, stages, snapshots, recipe, *recipeStages, trace, shape, ahead.get(), epoch, itemReads, *offer->writes, offer->depth, offer->currentTemplate, offer->nextTemplate, *early);
            }
            DrawPipeline::FollowEpoch(epoch);
            GuestMemory::SetCurrentPacket(commitPacket, commitQueue);
            commitDraw(localDevice, commitQueue, decode->state, drawParameters, stages, snapshots, recipe, *recipeStages, drawKey, trace, shape, ahead.get(), early.get());
            return false;
        };
        auto held = std::make_shared<decltype(commit)>(std::move(commit));
        DrawPipeline::Prepare prepare;
        if (ahead != nullptr) prepare = [held, ahead] { ahead->Prepare(); };
        DrawPipeline::Early earlyHook;
        if (DrawPipeline::ResolveSplit() != 0) earlyHook = [held](const DrawPipeline::EarlyOffer& offer) { return (*held)(&offer); };
        if (DrawFinisher::DefersOrdered()) {
            struct Deferred {
                DrawPipeline::Commit commit;
                std::vector<DrawPipeline::Range> writes;
                std::vector<DrawPipeline::Range> reads;
                DrawPipeline::Prepare prepare;
                DrawPipeline::Early early;
            };
            DrawFinisher::DeferOrdered([deferred = std::make_shared<Deferred>(Deferred{[held] { (*held)(nullptr); }, std::move(writes), std::move(reads), std::move(prepare), std::move(earlyHook)})] {
                DrawPipeline::Queue0().Enqueue(std::move(deferred->commit), std::move(deferred->writes), 0, {}, std::move(deferred->reads), true, DrawPipeline::ItemKind::Draw, std::move(deferred->prepare), std::move(deferred->early));
            });
        } else {
            DrawPipeline::Queue0().Enqueue([held] { (*held)(nullptr); }, std::move(writes), 0, {}, std::move(reads), true, DrawPipeline::ItemKind::Draw, std::move(prepare), std::move(earlyHook));
        }
        phaseTiming.Phase(DrawRowLockWait);
        timing.Mark("draw_enqueued");
        return drawn();
    }
    if (drawParameters.indirect) drainBeforeInline();
    lockForDraw();
    runCaptureCheckHook();
    if (tail.queue != nullptr && capturedMemoryChanged(memory, snapshots)) {
        gpuLock.unlock();
        const RecaptureScope recapture;
        return draw(*tail.queue, tail.packet, submission, tail.packetOffset, rejected);
    }
    noteDrawWriters(stages, submission.queue);
    phaseTiming.Phase(DrawRowVectors);
    if (recipe != nullptr) {
        if (localDevice->DrawFromRecipe(graphics, drawParameters, stages, snapshots, recipe, tracePtr) == RecipeOutcome::Recorded) {
            phaseTiming.Phase(DrawRowGraphics);
            timing.Mark("draw_and_resource_release");
            return drawn();
        }

        VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, VulkanDevice::RecipeKind::Draw);
    }
    std::shared_ptr<const DrawRecipe> built;
    localDevice->Draw(graphics, drawParameters, stages, snapshots, recipeStages.empty() ? nullptr : &built, tracePtr, tail.shapeKey);
    phaseTiming.Phase(DrawRowGraphics);
    if (built != nullptr) attachDrawRecipe(drawKey, recipeStages, std::move(built));
    timing.Mark("draw_and_resource_release");
    return drawn();
}

namespace {
bool DrawSplitTimed() {
    static const bool timed = std::getenv("APS5_COMMIT_STATS") != nullptr;
    return timed;
}

thread_local Graphics::DrawPacketPtr carriedPacket;

void releaseCarriedJob() {
    DrawPipeline::JoinResolve();
    carriedPacket.reset();
}

DrawPipeline::OwnCause OwnResolveCause(const EarlyDraw* early, DrawPipeline::EarlyEvent stale) {
    using C = DrawPipeline::OwnCause;
    using E = DrawPipeline::EarlyEvent;
    if (early == nullptr) return C::Unknown;
    if (stale == E::StaleSubmit) return C::StaleSubmit;
    if (stale == E::StaleProof || stale == E::StaleScoped || stale == E::StaleScopedTemplate) return C::StaleProof;
    if (stale != E::Used) return C::StaleOther;
    if (early->claimed) {
        if (early->abort == static_cast<std::uint8_t>(GuestMemory::BarrierKind::ResolverRefresh)) return C::AbortedImageRefresh;
        if (early->abort != 0) return C::AbortedOther;
        if (early->failed) return C::Failed;
        if (early->dropped) return C::Dropped;
        return C::Unknown;
    }
    if (early->revoked) return C::Revoked;
    switch (early->declined) {
        case E::Count: return DrawPipeline::TakenOwnCause();
        case E::KeptPredictor: return C::DeclinedPredictor;
        case E::KeptDescribed: return C::DeclinedDescribed;
        case E::KeptReadsWritten: return C::DeclinedReadsWritten;
        default: return C::DeclinedOther;
    }
}
}

thread_local std::uint64_t lastJobEpoch = 0;

thread_local const void* lastEmittedTemplate = nullptr;

void resolveEarlyJob(void* context) {
    auto& early = *static_cast<EarlyDraw*>(context);
    lastJobEpoch = early.epoch;
    GuestMemory::AdoptCollectEpoch(early.collectEpoch);
    GuestMemory::FollowCollectToken(early.epoch);
    early.handoff.snapshot = Graphics::Recorder::PendingWriteSnapshot();
    const Graphics::ResolverScope scope(early.handoff);
    struct Disarm {
        ~Disarm() {
            GuestMemory::DisarmEarlyResolve();
            GuestMemory::SetEarlyReadLog(nullptr);
            DrawPipeline::SkipCommittingReads(false);
        }
    } disarm;
    GuestMemory::SetEarlyReadLog(&early.reads);
    DrawPipeline::SkipCommittingReads(true);
    GuestMemory::ArmEarlyResolve();
    const bool timed = DrawSplitTimed();
    const auto started = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    Graphics::ShaderResources::ResetLastProofReuse();
    try {
        early.packet = early.resolver->ResolveDraw(*early.graphics, *early.draw, early.stages, early.snapshots, early.recipeOut, early.trace, early.shape, early.prepared);
        const auto proved = Graphics::ShaderResources::LastProofReuse();
        early.reproved = proved != Graphics::ShaderResources::ProofReuse::Reused;
        if (early.packet != nullptr) DrawPipeline::NoteResolvedTemplate(Graphics::DrawPacketTemplate(*early.packet));
        DrawPipeline::EndResolvePhase();
        if (early.packet != nullptr && DrawPipeline::ResolveLanes()) {
            try {
                Graphics::PrepareDrawBindingsAhead(*early.packet);
            } catch (...) {
            }
        }
    } catch (const GuestMemory::EarlyResolveAbort& abort) {
        early.packet.reset();
        early.abort = abort.kind;
    } catch (...) {
        early.packet.reset();
        early.failed = true;
    }
    if (timed) {
        early.proofClass = early.abort != 0 || early.failed ? 6u : static_cast<std::uint8_t>(Graphics::ShaderResources::LastProofReuse());
        early.jobNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
    }
}

void finishEarlyJob(void* context, bool ran) {
    auto& early = *static_cast<EarlyDraw*>(context);
    early.handoff.snapshot.reset();
    if (!early.abandoned.Empty()) early.resolver->ApplyDeferredEffects(early.abandoned);
    Graphics::BuryPackets(early.graveyard);
    if (ran && DrawSplitTimed()) DrawPipeline::NoteJobClass(early.proofClass, early.jobNs, DrawPipeline::LastJoinWaitNs());
    if (!ran) {
        early.claimed = false;
        early.revoked = true;
        return;
    }
    if (early.abort != 0 || early.failed || early.packet == nullptr) {
        early.packet.reset();
        early.built.reset();
        if (early.abort != 0) DrawPipeline::NoteEarlyAbort(early.abort);
        else DrawPipeline::NoteEarly(DrawPipeline::EarlyEvent::ResolverFailed);
        return;
    }
    if (Graphics::DrawPacketReadsOverlap(*early.packet, early.writes) || Graphics::DrawPacketReadsOverlap(*early.packet, early.depth)) {
        early.resolver->DiscardDraw(*early.packet);
        early.packet.reset();
        early.built.reset();
        early.dropped = true;
        DrawPipeline::NoteEarly(DrawPipeline::EarlyEvent::OverlapAfter);
    }
}

bool Driver::preProveDraw(const std::shared_ptr<VulkanDevice>& localDevice, std::span<const Graphics::CompiledShader> stages, DrawAheadSlot* ahead, std::uint64_t epoch, PreProofRequest& request) {
    using O = DrawPipeline::PreProofOutcome;
    const auto refuse = [](O outcome) {
        DrawPipeline::NotePreProof(outcome);
        return false;
    };
    if (ahead == nullptr || ahead->phase.load(std::memory_order_acquire) != DrawAheadSlot::Ready) return refuse(O::NotReady);
    if (epoch != lastJobEpoch || request.token != epoch) return refuse(O::Epoch);
    if (!localDevice->ResolverInputsClear()) return refuse(O::Writers);
    const auto& found = ahead->result.found;
    if (found == nullptr || request.resolvedTemplate == nullptr) return refuse(O::NoTemplate);
    GuestMemory::AdoptCollectEpoch(request.collectEpoch);
    GuestMemory::FollowCollectToken(request.token);
    if (found->EpochProofCurrent()) return refuse(O::Current);
    if (found.get() == request.emittedTemplate) return refuse(O::Emitted);
    if (found.get() == request.resolvedTemplate) return refuse(O::Resolved);
    if (!found->InResourceCache()) return refuse(O::NotCached);
    if (found->DescribedRangesOverlap(request.excluded)) return refuse(O::Overlap);
    const auto proofBefore = GuestMemory::ProofSerial();
    struct Held {
        Graphics::Recorder::DeferredEffects effects;
        std::vector<Graphics::DrawPacket*> graveyard;
    };
    auto held = std::make_shared<Held>();
    Graphics::ResolverHandoff handoff;
    handoff.snapshot = Graphics::Recorder::PendingWriteSnapshot();
    handoff.queuedStoresUnknown = true;
    handoff.abandoned = &held->effects;
    handoff.graveyard = &held->graveyard;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reads;
    bool proved = false;
    {
        const Graphics::ResolverScope scope(handoff);
        struct Disarm {
            ~Disarm() {
                GuestMemory::DisarmEarlyResolve();
                GuestMemory::SetEarlyReadLog(nullptr);
                DrawPipeline::SkipCommittingReads(false);
            }
        } disarm;
        GuestMemory::SetEarlyReadLog(&reads);
        DrawPipeline::SkipCommittingReads(true);
        GuestMemory::ArmEarlyResolve();
        struct PreProving {
            PreProving() { Graphics::ShaderResources::SetPreProving(true); }
            ~PreProving() { Graphics::ShaderResources::SetPreProving(false); }
        } preProving;
        try {
            proved = found->Revalidate(stages);
        } catch (...) {
            proved = false;
        }
    }
    if (proved && found->ProvedProofSerial() != proofBefore) {
        found->ForgetProof();
        return refuse(O::ProofMoved);
    }
    DrawPipeline::NotePreProof(proved ? O::Proved : O::Failed);
    if (!held->effects.Empty() || !held->graveyard.empty()) {
        request.finish = [device = localDevice, held] {
            if (!held->effects.Empty()) device->ApplyDeferredEffects(held->effects);
            Graphics::BuryPackets(held->graveyard);
        };
    }
    return proved;
}

bool Driver::resolveDrawEarly(const std::shared_ptr<VulkanDevice>& localDevice, const Graphics::State& graphics, const Pm4::DrawParameters& drawParameters, std::span<const Graphics::CompiledShader> stages, std::span<const Graphics::GuestMemorySnapshot> snapshots, const std::shared_ptr<const DrawRecipe>& recipe, const std::vector<std::shared_ptr<DispatchVariant>>& recipeStages, const VulkanDevice::DrawTraceContext& trace, std::uint64_t shape, DrawAheadSlot* ahead, std::uint64_t epoch, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& itemReads, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& writes, std::span<const std::pair<std::uint64_t, std::uint64_t>> depth, const void* currentTemplate, const void* nextTemplate, EarlyDraw& early) {
    static const bool all = std::getenv("APS5_RESOLVE_SPLIT_ALL") != nullptr;
    static const bool probe = (GuestMemory::SetResolveCompletionProbe(&Graphics::Recorder::InCompletion), true);
    static_cast<void>(probe);
    using E = DrawPipeline::EarlyEvent;
    if (early.packet != nullptr || early.claimed) return false;
    const auto keep = [&early](E event) {
        early.declined = event;
        DrawPipeline::NoteEarly(event);
        return false;
    };
    if (recipe != nullptr) return keep(E::KeptRecipe);
    if (graphics.stages.tessellationGeometry) return keep(E::KeptTessellation);
    if (DrawPipeline::FollowedEpoch() != epoch) return keep(E::KeptEpoch);
    const auto overlaps = [](std::span<const std::pair<std::uint64_t, std::uint64_t>> first, std::span<const std::pair<std::uint64_t, std::uint64_t>> second) {
        return std::any_of(first.begin(), first.end(), [&](const auto& a) { return std::any_of(second.begin(), second.end(), [&](const auto& b) { return a.first < b.second && b.first < a.second; }); });
    };
    if (overlaps(writes, itemReads) || overlaps(depth, itemReads)) return keep(E::KeptReadsWritten);
    if (!depth.empty() && overlaps(depth, DrawColorRanges(graphics))) return keep(E::KeptDepthColour);
    const auto* result = ahead != nullptr && ahead->phase.load(std::memory_order_acquire) == DrawAheadSlot::Ready ? &ahead->result : nullptr;
    if (early.twoAhead && ahead != nullptr && result == nullptr) return keep(E::KeptPredictor);
    const bool multisampled = std::any_of(graphics.colors.begin(), graphics.colors.end(), [](const Graphics::ColorTarget& color) { return color.samples > 1; }) || (graphics.depth && graphics.depth->samples > 1);
    early.multisample.clear();
    const bool multisampleFound = multisampled && localDevice->FindMultisampleTargets(graphics, early.multisample);
    if (multisampled) DrawPipeline::NoteEarly(multisampleFound ? DrawPipeline::EarlyEvent::MultisampleFound : DrawPipeline::EarlyEvent::MultisampleMissed);
    if (!all) {
        using R = Graphics::PredictReason;
        const auto reason = Graphics::PredictPureReason(graphics, drawParameters, stages, result, multisampleFound);
        static const bool offerUnhinted = std::getenv("APS5_RESOLVE_OFFER_UNHINTED") != nullptr;
        static const bool offerBuiltAhead = std::getenv("APS5_NO_AHEAD_TEMPLATE_LANE") == nullptr && Graphics::AheadTemplateBuilds();
        const bool builtAhead = offerBuiltAhead && reason == R::NoHint && result != nullptr && result->built != nullptr;
        const bool proofOnly = reason == R::ProofNotCurrent || reason == R::NoSlot || builtAhead || (offerUnhinted && (reason == R::NotLooked || reason == R::NoHint));
        if (reason != R::Accepted && !proofOnly) return keep(E::KeptPredictor);
        if (proofOnly && result != nullptr && result->found != nullptr && (result->found.get() == currentTemplate || (nextTemplate != nullptr && result->found.get() == nextTemplate))) return keep(E::KeptCurrentTemplate);
    }

    if (result != nullptr && result->found != nullptr && (result->found->DescribedRangesOverlap(writes) || result->found->DescribedRangesOverlap(depth))) {
        static const bool reported = std::getenv("APS5_COMMIT_STATS") != nullptr;
        static std::atomic<int> reports{0};
        if (reported && writes.size() > 0 && reports.fetch_add(1, std::memory_order_relaxed) % 997 == 0 && reports.load(std::memory_order_relaxed) < 997 * 24) std::fprintf(stderr, "[early-described] kept: %s; %zu write ranges offered\n", result->found->DescribeRangeOverlap(writes).c_str(), writes.size());
        return keep(E::KeptDescribed);
    }
    const auto split = DrawPipeline::ResolveSplit();
    if (split >= 2 && !localDevice->ResolverInputsClear()) {
        return keep(E::DeclinedWriters);
    }
    early.hintTemplate = result != nullptr ? result->found.get() : nullptr;
    auto* prepared = ahead != nullptr ? ahead->Claim() : nullptr;
    early.claimed = true;
    early.proof = GuestMemory::ProofSerial();
    early.pending = Graphics::StorageTexture::PendingSerial();
    early.serials = localDevice->TakeEarlySerials(split >= 2 ? &early.handoff.queuedStores : nullptr);
    early.redo = Graphics::DrawRedoSerial();
    early.device = localDevice.get();
    early.reads.clear();
    if (split >= 2) {
        static const bool hooked = (Graphics::SetRedoHook(&DrawPipeline::JoinResolve), true);
        static_cast<void>(hooked);
        early.resolver = localDevice.get();
        early.graphics = &graphics;
        early.draw = &drawParameters;
        early.stages = stages;
        early.snapshots = snapshots;
        early.recipeOut = recipeStages.empty() ? nullptr : &early.built;
        early.trace = trace.token != 0 ? &trace : nullptr;
        early.shape = shape;
        early.prepared = prepared;
        early.epoch = epoch;
        early.collectEpoch = GuestMemory::ThreadCollectEpoch();
        early.writes.assign(writes.begin(), writes.end());
        early.depth.assign(depth.begin(), depth.end());
        early.abort = 0;
        early.failed = false;
        early.handoff.shapeMemo = Graphics::ShapeMemoHandle();
        early.handoff.abandoned = &early.abandoned;
        early.handoff.graveyard = &early.graveyard;
        early.handoff.multisampleTargets = multisampleFound ? &early.multisample : nullptr;
        DrawPipeline::PostResolve(&resolveEarlyJob, &finishEarlyJob, &early);
        if (split == 3) DrawPipeline::WaitResolve();
        return true;
    }
    {
        struct Disarm {
            ~Disarm() {
                GuestMemory::DisarmEarlyResolve();
                GuestMemory::SetEarlyReadLog(nullptr);
                DrawPipeline::SkipCommittingReads(false);
                Graphics::SetFoundMultisampleTargets(nullptr);
            }
        } disarm;
        GuestMemory::SetEarlyReadLog(&early.reads);
        DrawPipeline::SkipCommittingReads(true);
        Graphics::SetFoundMultisampleTargets(multisampleFound ? &early.multisample : nullptr);
        GuestMemory::ArmEarlyResolve();
        const auto started = DrawSplitTimed() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        try {
            early.packet = localDevice->ResolveDraw(graphics, drawParameters, stages, snapshots, recipeStages.empty() ? nullptr : &early.built, trace.token != 0 ? &trace : nullptr, shape, prepared);
            if (DrawSplitTimed()) DrawPipeline::NoteEarlyResolveNs(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()));
        } catch (const GuestMemory::EarlyResolveAbort& abort) {
            early.packet.reset();
            early.built.reset();
            DrawPipeline::NoteEarlyAbort(abort.kind);
            early.abort = abort.kind;
            return false;
        }
    }
    if (Graphics::DrawPacketReadsOverlap(*early.packet, writes) || Graphics::DrawPacketReadsOverlap(*early.packet, depth)) {
        localDevice->DiscardDraw(*early.packet);
        early.packet.reset();
        early.built.reset();
        early.dropped = true;
        DrawPipeline::NoteEarly(DrawPipeline::EarlyEvent::OverlapAfter);
        return false;
    }
    return true;
}

void Driver::commitDraw(std::shared_ptr<VulkanDevice> localDevice, std::uint32_t queue, const Graphics::State& graphics, const Pm4::DrawParameters& drawParameters, std::span<const Graphics::CompiledShader> stages, std::span<const Graphics::GuestMemorySnapshot> snapshots, const std::shared_ptr<const DrawRecipe>& recipe, const std::vector<std::shared_ptr<DispatchVariant>>& recipeStages, std::uint64_t drawKey, VulkanDevice::DrawTraceContext trace, std::uint64_t shape, DrawAheadSlot* ahead, EarlyDraw* early) {
    DelayStage(DelayedStage::Committer);
    const auto* tracePtr = trace.token != 0 ? &trace : nullptr;
    static const bool carryRegistered = (DrawPipeline::SetCarryRelease(&releaseCarriedJob), true);
    static_cast<void>(carryRegistered);
    bool idleOwed = false;
    const bool lanes = DrawPipeline::ResolveLanes();
    if (lanes && DrawPipeline::HoldCarried()) {
        const auto joining = DrawSplitTimed() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        DrawPipeline::JoinCarriedJob();
        carriedPacket.reset();
        if (DrawSplitTimed()) DrawPipeline::NoteCommitterPart(DrawPipeline::CommitterPart::Join, joining);
        static const bool idleAfterEmit = std::getenv("APS5_NO_IDLE_SUBMIT_AFTER_EMIT") == nullptr;
        idleOwed = submitDueAfterCommit(localDevice.get(), idleAfterEmit && early != nullptr && early->packet != nullptr);
        DrawPipeline::SettleCarriedBoundary();
    } else if (DrawPipeline::HoldCarried()) {
        struct Drop {
            ~Drop() { DrawPipeline::ReleaseCarriedHold(); }
        } drop;
        const auto joining = DrawSplitTimed() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        releaseCarriedJob();
        if (DrawSplitTimed()) DrawPipeline::NoteCommitterPart(DrawPipeline::CommitterPart::Join, joining);
        static const bool idleAfterEmit = std::getenv("APS5_NO_IDLE_SUBMIT_AFTER_EMIT") == nullptr;
        idleOwed = submitDueAfterCommit(localDevice.get(), idleAfterEmit && early != nullptr && early->packet != nullptr);
    }
    const auto prologue = DrawSplitTimed() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    auto* prepared = ahead != nullptr && (early == nullptr || !early->claimed) ? ahead->Claim() : nullptr;
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    if (auto current = device.Load(); current != nullptr && current != localDevice) localDevice = std::move(current);
    struct SubmitDue {
        Driver& driver;
        const std::shared_ptr<VulkanDevice>& device;
        ~SubmitDue() noexcept(false) {
            if (std::uncaught_exceptions() == 0 && !DrawPipeline::HoldCarried()) driver.submitDueAfterCommit(device.get());
        }
    } submitDue{*this, localDevice};
    if (labelsDueForPacket()) {
        const DrawPipeline::ResolvePhaseScope labelsAlone;
        recordLabelsForPacket(localDevice.get(), queue);
    }
    noteDrawWriters(stages, queue);
    if (DrawSplitTimed()) DrawPipeline::NoteCommitterPart(DrawPipeline::CommitterPart::Prologue, prologue);
    if (recipe != nullptr) {
        if (localDevice->DrawFromRecipe(graphics, drawParameters, stages, snapshots, recipe, tracePtr) == RecipeOutcome::Recorded) return;
        VulkanDevice::NoteRecipe(VulkanDevice::RecipeEvent::Restart, VulkanDevice::RecipeKind::Draw);
    }
    std::shared_ptr<const DrawRecipe> built;
    if (graphics.stages.tessellationGeometry) {
        DrawPipeline::JoinResolve();
        localDevice->Draw(graphics, drawParameters, stages, snapshots, recipeStages.empty() ? nullptr : &built, tracePtr, shape, prepared);
    } else {
        Graphics::DrawPacketPtr packet;
        bool fromEarly = false;
        const bool timed = DrawSplitTimed();
        auto staleCause = DrawPipeline::EarlyEvent::Used;
        const auto clock = [timed] { return timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}; };
        if (early != nullptr && early->packet != nullptr) {
            const auto checking = clock();
            struct CheckTimer {
                bool timed;
                std::chrono::steady_clock::time_point since;
                ~CheckTimer() {
                    if (timed) DrawPipeline::NoteCommitterPart(DrawPipeline::CommitterPart::Check, since);
                }
            } checkTimer{timed, checking};
            using E = DrawPipeline::EarlyEvent;
            auto stale = E::Used;
            const auto rangesOverlap = [](const auto& first, const auto& second) {
                return std::any_of(first.begin(), first.end(), [&](const auto& a) { return std::any_of(second.begin(), second.end(), [&](const auto& b) { return a.first < b.second && b.first < a.second; }); });
            };
            bool acrossLabels = false;
            if (early->device != localDevice.get()) {
                stale = E::StaleDevice;
            } else if (early->proof != GuestMemory::ProofSerial()) {
                stale = E::StaleProof;
                if (DrawPipeline::OfferAcrossLabels()) {
                    if (const auto scoped = GuestMemory::ScopedSince(early->proof)) {
                        const auto itemReads = QueuedDrawReadRanges(drawParameters, stages);
                        const auto* targetWrites = DrawPipeline::CommittingWrites();
                        if (rangesOverlap(*scoped, itemReads) || rangesOverlap(*scoped, early->reads) || (targetWrites != nullptr && rangesOverlap(*scoped, *targetWrites)) || Graphics::DrawPacketReadsOverlap(*early->packet, *scoped)) stale = E::StaleScoped;
                        else if (const DrawPipeline::ResolvePhaseScope revalidating; DrawPipeline::QuiescePreProof(), !Graphics::RevalidateDrawPacketTemplate(*early->packet, stages)) stale = E::StaleScopedTemplate;
                        else {
                            stale = E::Used;
                            acrossLabels = true;
                        }
                    }
                }
            }
            if (stale != E::Used) {
            } else if (early->pending != Graphics::StorageTexture::PendingSerial()) stale = E::StalePending;
            else if (early->redo != Graphics::DrawRedoSerial()) stale = E::StaleRedo;
            else if (Graphics::DrawPacketTemplateGone(*early->packet)) stale = E::StaleTemplate;
            else if (early->twoAhead && early->reproved && Graphics::DrawPacketTemplate(*early->packet) == lastEmittedTemplate) stale = E::StaleTemplate;
            else {
                thread_local std::vector<std::pair<std::uint64_t, std::uint64_t>> noted;
                noted.clear();
                bool overrun = false;
                const auto change = localDevice->CheckEarlySerials(early->serials, noted, overrun);
                const auto overlaps = [](const auto& first, const auto& second) {
                    return std::any_of(first.begin(), first.end(), [&](const auto& a) { return std::any_of(second.begin(), second.end(), [&](const auto& b) { return a.first < b.second && b.first < a.second; }); });
                };
                if (change == VulkanDevice::EarlyChange::Imports) stale = E::StaleImports;
                else if (change == VulkanDevice::EarlyChange::Open) {
                    stale = E::StaleSubmit;
                    if (DrawSplitTimed()) DrawPipeline::NoteStaleSubmit();
                }
                else if (change == VulkanDevice::EarlyChange::Noted && (overrun || overlaps(noted, early->reads) || Graphics::DrawPacketReadsOverlap(*early->packet, noted))) stale = E::StaleNoted;
            }
            DrawPipeline::NoteEarly(stale);
            staleCause = stale;
            if (stale == E::Used && acrossLabels) DrawPipeline::NoteEarly(E::UsedAcrossLabels);
            if (stale == E::Used) {
                packet = std::move(early->packet);
                fromEarly = true;
            } else {
                localDevice->DiscardDraw(*early->packet);
                early->packet.reset();
                early->built.reset();
            }
        }
        if (fromEarly) {
            static const unsigned verifyEvery = [] {
                const char* text = std::getenv("APS5_VERIFY_RESOLVE_SPLIT");
                return text == nullptr ? 0u : static_cast<unsigned>(std::strtoul(text, nullptr, 10));
            }();
            static unsigned verifyCount = 0;
            if (verifyEvery != 0 && ++verifyCount % verifyEvery == 0) {
                DrawPipeline::QuiescePreProof();
                const DrawPipeline::ResolvePhaseScope verifying;
                auto verify = localDevice->ResolveDraw(graphics, drawParameters, stages, snapshots, recipeStages.empty() ? nullptr : &built, tracePtr, shape, nullptr);
                const auto differs = Graphics::CompareDrawPackets(*packet, *verify);
                DrawPipeline::NoteEarly(differs == 0 ? DrawPipeline::EarlyEvent::VerifyMatched : DrawPipeline::EarlyEvent::VerifyDiffered);
                if (differs != 0) {
                    std::string fields;
                    for (unsigned field = 0; field < static_cast<unsigned>(Graphics::DrawPacketField::Count); ++field) {
                        if ((differs >> field & 1u) != 0) fields += std::string(" ") + Graphics::DrawPacketFieldName(static_cast<Graphics::DrawPacketField>(field));
                    }
                    if (!DrawPipeline::EarlyStopped()) std::fprintf(stderr, "[draw] early-resolve verify: the early packet differs from a resolve in the old order in:%s; early resolves stopped\n", fields.c_str());
                    DrawPipeline::StopEarly();
                }
                localDevice->DiscardDraw(*packet);
                packet = std::move(verify);
                fromEarly = false;
                early->built.reset();
            }
        }
        std::uint64_t ownResolveNs = 0;
        if (packet == nullptr) {
            DrawPipeline::QuiescePreProof();
            const DrawPipeline::ResolvePhaseScope ownResolve;
            const auto resolving = clock();
            packet = localDevice->ResolveDraw(graphics, drawParameters, stages, snapshots, recipeStages.empty() ? nullptr : &built, tracePtr, shape, prepared);
            if (timed) {
                DrawPipeline::NoteCommitterPart(DrawPipeline::CommitterPart::ResolveWhole, resolving);
                ownResolveNs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - resolving).count());
            }
        }
        if (fromEarly) {
            const auto keeping = clock();
            if (Graphics::DrawPacketHasMemoKeeps(*packet)) {
                const DrawPipeline::ResolvePhaseScope keepingAlone;
                localDevice->ApplyDrawMemoKeeps(*packet);
            }
            if (timed) DrawPipeline::NoteCommitterPart(DrawPipeline::CommitterPart::MemoKeeps, keeping);
        }
        struct JoinGuard {
            bool timed;
            Graphics::DrawPacketPtr& packet;
            ~JoinGuard() noexcept(false) {
                if (DrawPipeline::CarryHold()) {
                    carriedPacket = std::move(packet);
                    return;
                }
                const auto joining = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                DrawPipeline::JoinResolve();
                if (DrawPipeline::ResolveLanes()) DrawPipeline::ReleaseCarriedHold();
                if (timed) DrawPipeline::NoteCommitterPart(DrawPipeline::CommitterPart::Join, joining);
            }
        } joinGuard{timed, packet};
        const auto offer = [&] {
            if (DrawPipeline::ResolveSplit() == 0) return;
            const auto offering = clock();
            static const bool scratchReuse = std::getenv("APS5_NO_DRAW_SCRATCH_REUSE") == nullptr;
            thread_local std::vector<std::pair<std::uint64_t, std::uint64_t>> reusedDepth;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> ownDepth;
            auto& depth = scratchReuse ? reusedDepth : ownDepth;
            DrawDepthRanges(graphics, depth);
            DrawPipeline::OfferEarly(depth, Graphics::DrawPacketTemplate(*packet));
            if (timed) DrawPipeline::NoteCommitterPart(DrawPipeline::CommitterPart::Offer, offering);
        };
        if (!idleOwed) offer();
        const auto emitting = clock();
        localDevice->EmitDraw(*packet);
        lastEmittedTemplate = Graphics::DrawPacketTemplate(*packet);
        if (timed) {
            DrawPipeline::NoteCommitterPart(fromEarly ? DrawPipeline::CommitterPart::EmitEarly : DrawPipeline::CommitterPart::EmitWhole, emitting);
            if (!fromEarly && ownResolveNs != 0) DrawPipeline::NoteOwnResolve(OwnResolveCause(early, staleCause), ownResolveNs, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - emitting).count()));
        }
        if (idleOwed) {
            DrawPipeline::NoteEarly(DrawPipeline::EarlyEvent::IdleSubmitAfterEmit);
            submitDueAfterCommit(localDevice.get());
            offer();
        }
        if (fromEarly) built = std::move(early->built);
    }
    if (built != nullptr) attachDrawRecipe(drawKey, recipeStages, std::move(built));
}

}
