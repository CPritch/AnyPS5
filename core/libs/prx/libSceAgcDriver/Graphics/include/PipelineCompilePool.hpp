#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECOMPILEPOOL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECOMPILEPOOL_HPP

#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace AgcDriver::Graphics {

enum class CompileWait : std::uint8_t { GuestWrites, Occlusion, LinearTarget, NotRecorded, Dispatch, Count };
enum class DirectCompile : std::uint8_t { PrivatePipeline, FailedBuild, TessellationCapture, PoolClosed, HotThreadModule, Count };
enum class CompileRequest : std::uint8_t { Queued, QueueFull, Failed, Closed };

struct AsyncCompileTotals {
    std::uint64_t droppedDraws = 0;
    std::uint64_t droppedUnqueued = 0;
    std::uint64_t queued = 0;
    std::uint64_t building = 0;
    std::uint64_t built = 0;
    std::uint64_t failed = 0;
    std::uint64_t longestQueueMs = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(CompileWait::Count)> waits{};
    std::array<std::uint64_t, static_cast<std::size_t>(CompileWait::Count)> waitedMs{};
    std::array<std::uint64_t, static_cast<std::size_t>(DirectCompile::Count)> direct{};
};

bool AsyncCompileEnabled();
void SetAsyncCompileForTest(bool enabled);
bool OnCompilePoolThread();
bool OnHotCompileThread();

CompileRequest RequestPipelineCompile(VkDevice device, std::uint64_t hash, const std::function<std::function<void()>()>& makeJob, bool urgent);
bool AwaitPipelineCompile(VkDevice device, std::uint64_t hash);
void RunOnCompilePool(CompileWait reason, const std::function<void()>& job);
void DrainPipelineCompiles(VkDevice device);
void HoldPipelineCompilesForTest(bool held);
void StopPipelineCompilePool();
unsigned PipelineCompileWorkers();

void NoteDroppedDraw(bool queued);
void NoteCompileWait(CompileWait reason, std::uint64_t waitedMs);
void NoteDirectCompile(DirectCompile reason);

AsyncCompileTotals AsyncCompileTotalsNow();
std::string AsyncCompileSummaryJson();
std::size_t AsyncCompileQueueBound();

}

#endif
