#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINEBUILDTRACE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINEBUILDTRACE_HPP

#include "Recompiler.hpp"
#include <vulkan/vulkan.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace AgcDriver::Graphics {

enum class PipelineBuildJournal { Off, Loading, New, Held, Replay, Unrecorded };

struct PipelineBuildSubject {
    const char* kind = "";
    std::uint64_t program = 0;
    ShaderRecompiler::PipelineIdentity identity;
    std::size_t words = 0;
    std::size_t specializationValues = 0;
    std::uint64_t specialization = 0;
    PipelineBuildJournal journal = PipelineBuildJournal::Unrecorded;
    std::uint64_t module = 0;
};

struct PipelineBuildCounters {
    std::uint64_t builds = 0;
    std::uint64_t cacheHits = 0;
    std::uint64_t cacheMisses = 0;
    std::uint64_t unreported = 0;
    std::uint64_t unasked = 0;
    std::uint64_t journalNew = 0;
    std::uint64_t journalHeld = 0;
    std::uint64_t replayed = 0;
    std::uint64_t repeated = 0;
    double ms = 0;
};

[[nodiscard]] bool PipelineBuildsTraced();
[[nodiscard]] PipelineBuildCounters PipelineBuildTotals();
[[nodiscard]] PipelineBuildJournal PipelineBuildJournalOf(std::span<const ShaderRecompiler::PipelineIdentity> modules);
[[nodiscard]] ShaderRecompiler::PipelineIdentity CombinedPipelineIdentity(std::span<const ShaderRecompiler::PipelineIdentity> stages);
[[nodiscard]] std::uint64_t PipelineSpecializationHash(const VkSpecializationInfo* info);

class PipelineBuildTrace {
public:
    explicit PipelineBuildTrace(bool feedback);
    PipelineBuildTrace(const PipelineBuildTrace&) = delete;
    PipelineBuildTrace& operator=(const PipelineBuildTrace&) = delete;
    void Chain(const void*& next);
    void Report(const PipelineBuildSubject& subject);
    void NoteBinary(const char* outcome) { binary = outcome; }
    [[nodiscard]] bool Active() const { return active; }

private:
    bool active;
    bool feedback;
    const char* binary = "none";
    VkPipelineCreationFeedback created{};
    VkPipelineCreationFeedbackCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
    std::chrono::steady_clock::time_point started{};
};

}

#endif
