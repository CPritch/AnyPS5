#include "prx/libSceAgcDriver/Graphics/include/PipelineBuildTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineJournal.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "ShaderCacheDirectory.hpp"
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#ifndef _WIN32
#include <pthread.h>
#endif

namespace AgcDriver::Graphics {

namespace {

struct Totals {
    std::atomic<std::uint64_t> builds{0};
    std::atomic<std::uint64_t> cacheHits{0};
    std::atomic<std::uint64_t> cacheMisses{0};
    std::atomic<std::uint64_t> unreported{0};
    std::atomic<std::uint64_t> unasked{0};
    std::atomic<std::uint64_t> journalNew{0};
    std::atomic<std::uint64_t> journalHeld{0};
    std::atomic<std::uint64_t> replayed{0};
    std::atomic<std::uint64_t> repeated{0};
    std::atomic<std::uint64_t> microseconds{0};
};

Totals& totals() {
    static Totals counters;
    return counters;
}

struct Seen {
    std::mutex mutex;
    std::unordered_map<std::uint64_t, std::uint32_t> subjects;
    std::unordered_map<std::uint64_t, std::uint32_t> programs;
};

Seen& seen() {
    static auto* table = new Seen;
    return *table;
}

const char* JournalName(PipelineBuildJournal journal) {
    switch (journal) {
    case PipelineBuildJournal::Off: return "off";
    case PipelineBuildJournal::Loading: return "loading";
    case PipelineBuildJournal::New: return "new";
    case PipelineBuildJournal::Held: return "held";
    case PipelineBuildJournal::Replay: return "replay";
    case PipelineBuildJournal::Unrecorded: return "unrecorded";
    }
    return "unrecorded";
}

std::string ThreadName() {
#ifdef _WIN32
    return {};
#else
    std::array<char, 64> name{};
    if (pthread_getname_np(pthread_self(), name.data(), name.size()) != 0) return {};
    return name.data();
#endif
}

std::uint64_t SubjectKey(const PipelineBuildSubject& subject) {
    const std::array<std::uint64_t, 5> words{ShaderRecompiler::HashBytes(std::as_bytes(std::span(subject.kind, std::strlen(subject.kind)))), subject.program, subject.identity.low, subject.identity.high, subject.specialization};
    return ShaderRecompiler::HashBytes(std::as_bytes(std::span(words)));
}

}

bool PipelineBuildsTraced() {
    static const bool traced = std::getenv("APS5_TRACE_PIPELINE_BUILDS") != nullptr;
    return traced;
}

PipelineBuildCounters PipelineBuildTotals() {
    const auto& counters = totals();
    PipelineBuildCounters result;
    result.builds = counters.builds.load(std::memory_order_relaxed);
    result.cacheHits = counters.cacheHits.load(std::memory_order_relaxed);
    result.cacheMisses = counters.cacheMisses.load(std::memory_order_relaxed);
    result.unreported = counters.unreported.load(std::memory_order_relaxed);
    result.unasked = counters.unasked.load(std::memory_order_relaxed);
    result.journalNew = counters.journalNew.load(std::memory_order_relaxed);
    result.journalHeld = counters.journalHeld.load(std::memory_order_relaxed);
    result.replayed = counters.replayed.load(std::memory_order_relaxed);
    result.repeated = counters.repeated.load(std::memory_order_relaxed);
    result.ms = static_cast<double>(counters.microseconds.load(std::memory_order_relaxed)) / 1000.0;
    return result;
}

PipelineBuildJournal PipelineBuildJournalOf(std::span<const ShaderRecompiler::PipelineIdentity> modules) {
    if (PipelineJournalReplaying()) return PipelineBuildJournal::Replay;
    if (modules.empty()) return PipelineBuildJournal::Unrecorded;
    if (!PipelineJournalEnabled()) return PipelineBuildJournal::Off;
    bool held = true;
    for (const auto& identity : modules) {
        const auto loaded = PipelineJournalLoaded(identity);
        if (!loaded.has_value()) return PipelineBuildJournal::Loading;
        held = held && *loaded;
    }
    return held ? PipelineBuildJournal::Held : PipelineBuildJournal::New;
}

ShaderRecompiler::PipelineIdentity CombinedPipelineIdentity(std::span<const ShaderRecompiler::PipelineIdentity> stages) {
    std::vector<std::uint64_t> words;
    words.reserve(stages.size() * 2u);
    for (const auto& stage : stages) {
        words.push_back(stage.low);
        words.push_back(stage.high);
    }
    const auto bytes = std::as_bytes(std::span(words));
    return {ShaderRecompiler::HashBytes(bytes, 1), ShaderRecompiler::HashBytes(bytes, 2)};
}

std::uint64_t PipelineSpecializationHash(const VkSpecializationInfo* info) {
    if (info == nullptr || info->mapEntryCount == 0) return 0;
    std::vector<std::uint64_t> words;
    words.reserve(info->mapEntryCount * 2u);
    for (std::uint32_t index = 0; index < info->mapEntryCount; ++index) {
        const auto& entry = info->pMapEntries[index];
        std::uint64_t value = 0;
        if (info->pData != nullptr && entry.offset + entry.size <= info->dataSize) std::memcpy(&value, static_cast<const std::byte*>(info->pData) + entry.offset, entry.size < sizeof(value) ? entry.size : sizeof(value));
        words.push_back(entry.constantID);
        words.push_back(value);
    }
    return ShaderRecompiler::HashBytes(std::as_bytes(std::span(words)));
}

PipelineBuildTrace::PipelineBuildTrace(bool feedback) : active(PipelineBuildsTraced()), feedback(feedback) {}

void PipelineBuildTrace::Chain(const void*& next) {
    if (!active) return;
    created = {};
    if (feedback && next != &info) {
        info.pNext = next;
        info.pPipelineCreationFeedback = &created;
        info.pipelineStageCreationFeedbackCount = 0;
        info.pPipelineStageCreationFeedbacks = nullptr;
        next = &info;
    }
    started = std::chrono::steady_clock::now();
}

void PipelineBuildTrace::Report(const PipelineBuildSubject& subject) {
    if (!active) return;
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto ms = std::chrono::duration<double, std::milli>(elapsed).count();
    auto& counters = totals();
    const char* cache = "unasked";
    if (feedback && (created.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) == 0) {
        cache = "unreported";
        counters.unreported.fetch_add(1, std::memory_order_relaxed);
    } else if (feedback && (created.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0) {
        cache = "hit";
        counters.cacheHits.fetch_add(1, std::memory_order_relaxed);
    } else if (feedback) {
        cache = "miss";
        counters.cacheMisses.fetch_add(1, std::memory_order_relaxed);
    } else {
        counters.unasked.fetch_add(1, std::memory_order_relaxed);
    }
    if (subject.journal == PipelineBuildJournal::New) counters.journalNew.fetch_add(1, std::memory_order_relaxed);
    if (subject.journal == PipelineBuildJournal::Held) counters.journalHeld.fetch_add(1, std::memory_order_relaxed);
    if (subject.journal == PipelineBuildJournal::Replay) counters.replayed.fetch_add(1, std::memory_order_relaxed);
    std::uint32_t repeat = 0;
    std::uint32_t programBuilds = 0;
    {
        auto& table = seen();
        std::lock_guard lock(table.mutex);
        repeat = ++table.subjects[SubjectKey(subject)];
        if (subject.program != 0) programBuilds = ++table.programs[subject.program];
    }
    if (repeat > 1) counters.repeated.fetch_add(1, std::memory_order_relaxed);
    const auto builds = counters.builds.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto spent = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    const auto microseconds = counters.microseconds.fetch_add(spent, std::memory_order_relaxed) + spent;
    const auto at = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto thread = ThreadName();
    std::fprintf(stderr, "[pipeline-build] at=%.3f thread=\"%s\" kind=%s ms=%.1f cache=%s journal=%s gpu-mutex=%s program=0x%" PRIx64 " identity=%016" PRIx64 "%016" PRIx64 " words=%zu spec=%zu:%016" PRIx64 " repeat=%u program-builds=%u total=%" PRIu64 ":%.0fms\n", at, thread.c_str(), subject.kind, ms, cache, JournalName(subject.journal), GuestMemory::GpuMutex().HeldByThisThread() ? "held" : "free", subject.program, subject.identity.high, subject.identity.low, subject.words, subject.specializationValues, subject.specialization, repeat, programBuilds, builds, static_cast<double>(microseconds) / 1000.0);
}

}
