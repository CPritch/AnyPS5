#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDINGTHREAD_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_RECORDINGTHREAD_HPP

#include <vulkan/vulkan.h>
#include <chrono>
#include <cstdint>
#include <functional>

namespace AgcDriver::Graphics {

// The recording thread: every vkCmd* call into a command buffer of a pool handed to
// RecordPoolOnRecordingThread is queued, in the order it was made, and made by one thread of its
// own instead of the thread that recorded it. The calling thread (the draw committer, mostly)
// keeps the work of deciding what to record; the time inside the Vulkan driver moves to the
// recording thread. Command buffers of other pools are recorded directly, as before.
//
// What has to see a command buffer's commands made waits for its queue first (a flush): a
// vkQueueSubmit that names it, vkAllocateCommandBuffers, vkFreeCommandBuffers, vkResetCommandPool
// and vkDestroyCommandPool on its pool, and the destruction of any object a queued command may
// still name (vkDestroyImage, vkDestroyPipeline, vkResetDescriptorPool and the like). A failure of
// vkBeginCommandBuffer or vkEndCommandBuffer on the recording thread is returned by the
// vkQueueSubmit of that command buffer.
//
// Installed by wrapping the device's vkGetDeviceProcAddr, so it covers every call site that
// resolves its entry points from the device (Context::Function, the DeviceFunctions table and the
// device's own lookups). One device per process records this way.
struct RecordingThreadOptions {
    // Runs first on the recording thread (naming and pinning it).
    std::function<void()> onStart;
    // How long the recording thread spins for more commands, and a flush spins for the thread,
    // before sleeping.
    std::chrono::nanoseconds spin{0};
};

// APS5_RECORDING_THREAD: on unless set to 0.
bool RecordingThreadEnabled();

PFN_vkGetDeviceProcAddr RecordOnRecordingThread(VkDevice device, PFN_vkGetDeviceProcAddr resolver, RecordingThreadOptions options);
void RecordPoolOnRecordingThread(VkDevice device, VkCommandPool pool);
// Waits for every queued command, then stops the thread (the device is being destroyed).
void StopRecordingThread(VkDevice device) noexcept;

struct RecordingThreadCounts {
    std::uint64_t queued = 0;      // commands made by the recording thread
    std::uint64_t direct = 0;      // commands into command buffers of other pools
    std::uint64_t flushes = 0;     // flushes that had to wait
    std::uint64_t flushWaitNs = 0;
    std::uint64_t submitFlushes = 0, poolFlushes = 0, destroyFlushes = 0;
    std::uint64_t replayNs = 0;    // the recording thread's time inside commands
};
RecordingThreadCounts RecordingThreadTotals();

}

#endif
