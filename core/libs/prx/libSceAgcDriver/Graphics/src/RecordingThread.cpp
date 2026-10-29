#include "prx/libSceAgcDriver/Graphics/include/RecordingThread.hpp"
#include "prx/libSceAgcDriver/Execution/include/HandoffSpin.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AgcDriver::Graphics {

namespace {

// The real entry points of the device that records on the recording thread.
struct Real {
#define APS5_RECORDED_COMMANDS(X) \
    X(PFN_vkCmdPipelineBarrier, cmdPipelineBarrier, "vkCmdPipelineBarrier") \
    X(PFN_vkCmdClearColorImage, cmdClearColorImage, "vkCmdClearColorImage") \
    X(PFN_vkCmdClearDepthStencilImage, cmdClearDepthStencilImage, "vkCmdClearDepthStencilImage") \
    X(PFN_vkCmdBindPipeline, cmdBindPipeline, "vkCmdBindPipeline") \
    X(PFN_vkCmdBindDescriptorSets, cmdBindDescriptorSets, "vkCmdBindDescriptorSets") \
    X(PFN_vkCmdPushConstants, cmdPushConstants, "vkCmdPushConstants") \
    X(PFN_vkCmdBindVertexBuffers, cmdBindVertexBuffers, "vkCmdBindVertexBuffers") \
    X(PFN_vkCmdBindIndexBuffer, cmdBindIndexBuffer, "vkCmdBindIndexBuffer") \
    X(PFN_vkCmdCopyBuffer, cmdCopyBuffer, "vkCmdCopyBuffer") \
    X(PFN_vkCmdCopyImage, cmdCopyImage, "vkCmdCopyImage") \
    X(PFN_vkCmdBlitImage, cmdBlitImage, "vkCmdBlitImage") \
    X(PFN_vkCmdResolveImage, cmdResolveImage, "vkCmdResolveImage") \
    X(PFN_vkCmdCopyBufferToImage, cmdCopyBufferToImage, "vkCmdCopyBufferToImage") \
    X(PFN_vkCmdCopyImageToBuffer, cmdCopyImageToBuffer, "vkCmdCopyImageToBuffer") \
    X(PFN_vkCmdUpdateBuffer, cmdUpdateBuffer, "vkCmdUpdateBuffer") \
    X(PFN_vkCmdFillBuffer, cmdFillBuffer, "vkCmdFillBuffer") \
    X(PFN_vkCmdDispatch, cmdDispatch, "vkCmdDispatch") \
    X(PFN_vkCmdDispatchIndirect, cmdDispatchIndirect, "vkCmdDispatchIndirect") \
    X(PFN_vkCmdDraw, cmdDraw, "vkCmdDraw") \
    X(PFN_vkCmdDrawIndexed, cmdDrawIndexed, "vkCmdDrawIndexed") \
    X(PFN_vkCmdDrawIndirect, cmdDrawIndirect, "vkCmdDrawIndirect") \
    X(PFN_vkCmdDrawIndexedIndirect, cmdDrawIndexedIndirect, "vkCmdDrawIndexedIndirect") \
    X(PFN_vkCmdDrawIndirectCountKHR, cmdDrawIndirectCount, "vkCmdDrawIndirectCountKHR") \
    X(PFN_vkCmdDrawIndexedIndirectCountKHR, cmdDrawIndexedIndirectCount, "vkCmdDrawIndexedIndirectCountKHR") \
    X(PFN_vkCmdDrawMeshTasksEXT, cmdDrawMeshTasks, "vkCmdDrawMeshTasksEXT") \
    X(PFN_vkCmdDrawMeshTasksIndirectEXT, cmdDrawMeshTasksIndirect, "vkCmdDrawMeshTasksIndirectEXT") \
    X(PFN_vkCmdBeginRenderPass, cmdBeginRenderPass, "vkCmdBeginRenderPass") \
    X(PFN_vkCmdEndRenderPass, cmdEndRenderPass, "vkCmdEndRenderPass") \
    X(PFN_vkCmdBeginRenderingKHR, cmdBeginRendering, "vkCmdBeginRenderingKHR") \
    X(PFN_vkCmdEndRenderingKHR, cmdEndRendering, "vkCmdEndRenderingKHR") \
    X(PFN_vkCmdSetViewport, cmdSetViewport, "vkCmdSetViewport") \
    X(PFN_vkCmdSetScissor, cmdSetScissor, "vkCmdSetScissor") \
    X(PFN_vkCmdSetDepthBounds, cmdSetDepthBounds, "vkCmdSetDepthBounds") \
    X(PFN_vkCmdSetDepthBias, cmdSetDepthBias, "vkCmdSetDepthBias") \
    X(PFN_vkCmdSetCullModeEXT, cmdSetCullMode, "vkCmdSetCullModeEXT") \
    X(PFN_vkCmdSetFrontFaceEXT, cmdSetFrontFace, "vkCmdSetFrontFaceEXT") \
    X(PFN_vkCmdSetDepthTestEnableEXT, cmdSetDepthTestEnable, "vkCmdSetDepthTestEnableEXT") \
    X(PFN_vkCmdSetDepthWriteEnableEXT, cmdSetDepthWriteEnable, "vkCmdSetDepthWriteEnableEXT") \
    X(PFN_vkCmdSetDepthCompareOpEXT, cmdSetDepthCompareOp, "vkCmdSetDepthCompareOpEXT") \
    X(PFN_vkCmdSetDepthBoundsTestEnableEXT, cmdSetDepthBoundsTestEnable, "vkCmdSetDepthBoundsTestEnableEXT") \
    X(PFN_vkCmdSetStencilTestEnableEXT, cmdSetStencilTestEnable, "vkCmdSetStencilTestEnableEXT") \
    X(PFN_vkCmdSetStencilOpEXT, cmdSetStencilOp, "vkCmdSetStencilOpEXT") \
    X(PFN_vkCmdSetStencilCompareMask, cmdSetStencilCompareMask, "vkCmdSetStencilCompareMask") \
    X(PFN_vkCmdSetStencilWriteMask, cmdSetStencilWriteMask, "vkCmdSetStencilWriteMask") \
    X(PFN_vkCmdSetStencilReference, cmdSetStencilReference, "vkCmdSetStencilReference") \
    X(PFN_vkCmdResetQueryPool, cmdResetQueryPool, "vkCmdResetQueryPool") \
    X(PFN_vkCmdBeginQuery, cmdBeginQuery, "vkCmdBeginQuery") \
    X(PFN_vkCmdEndQuery, cmdEndQuery, "vkCmdEndQuery") \
    X(PFN_vkCmdWriteTimestamp, cmdWriteTimestamp, "vkCmdWriteTimestamp") \
    X(PFN_vkCmdCopyQueryPoolResults, cmdCopyQueryPoolResults, "vkCmdCopyQueryPoolResults") \
    X(PFN_vkCmdWriteBufferMarkerAMD, cmdWriteBufferMarker, "vkCmdWriteBufferMarkerAMD") \
    X(PFN_vkCmdSetCheckpointNV, cmdSetCheckpoint, "vkCmdSetCheckpointNV")
// Calls that wait for every queued command before they run: they end the life of an object a
// queued command may name.
#define APS5_FLUSHING_DESTROYS(X) \
    X(PFN_vkDestroyImage, destroyImage, "vkDestroyImage") \
    X(PFN_vkDestroyImageView, destroyImageView, "vkDestroyImageView") \
    X(PFN_vkDestroyBuffer, destroyBuffer, "vkDestroyBuffer") \
    X(PFN_vkFreeMemory, freeMemory, "vkFreeMemory") \
    X(PFN_vkDestroyFramebuffer, destroyFramebuffer, "vkDestroyFramebuffer") \
    X(PFN_vkDestroyRenderPass, destroyRenderPass, "vkDestroyRenderPass") \
    X(PFN_vkDestroyPipeline, destroyPipeline, "vkDestroyPipeline") \
    X(PFN_vkDestroyPipelineLayout, destroyPipelineLayout, "vkDestroyPipelineLayout") \
    X(PFN_vkDestroySampler, destroySampler, "vkDestroySampler") \
    X(PFN_vkDestroyDescriptorPool, destroyDescriptorPool, "vkDestroyDescriptorPool") \
    X(PFN_vkDestroyDescriptorSetLayout, destroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout") \
    X(PFN_vkResetDescriptorPool, resetDescriptorPool, "vkResetDescriptorPool") \
    X(PFN_vkFreeDescriptorSets, freeDescriptorSets, "vkFreeDescriptorSets") \
    X(PFN_vkDestroyQueryPool, destroyQueryPool, "vkDestroyQueryPool") \
    X(PFN_vkDeviceWaitIdle, deviceWaitIdle, "vkDeviceWaitIdle")
#define APS5_MEMBER(type, member, name) type member = nullptr;
    APS5_RECORDED_COMMANDS(APS5_MEMBER)
    APS5_FLUSHING_DESTROYS(APS5_MEMBER)
#undef APS5_MEMBER
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer resetCommandBuffer = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers freeCommandBuffers = nullptr;
    PFN_vkResetCommandPool resetCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
};

Real& real() {
    static auto& table = *new Real();
    return table;
}

constexpr std::size_t ChunkBytes = 256 * 1024;
constexpr std::size_t OpAlign = 16;

constexpr std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

struct Stream;
using ReplayFn = void (*)(VkCommandBuffer commands, std::byte* payload, Stream& stream);

struct alignas(OpAlign) OpHeader {
    ReplayFn replay = nullptr;  // null: the rest of the chunk is unused
    std::uint64_t bytes = 0;    // the header and its payload, a multiple of OpAlign
};

struct Chunk {
    Chunk* next = nullptr;
    alignas(OpAlign) std::byte data[ChunkBytes];
};

struct Counters {
    std::atomic<std::uint64_t> queued{0}, direct{0}, flushes{0}, flushWaitNs{0}, submitFlushes{0}, poolFlushes{0}, destroyFlushes{0}, replayNs{0};
};

Counters& counters() {
    static auto& state = *new Counters();
    return state;
}

// One command buffer's queue. Its commands are written by whoever records into the command
// buffer (Vulkan already requires that to be one thread at a time) and made by the recording
// thread, in order. Offsets are logical bytes over the chain of chunks.
struct Stream : std::enable_shared_from_this<Stream> {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    Chunk* writeChunk = nullptr;
    std::size_t writeOffset = 0;
    std::uint64_t written = 0;
    std::atomic<std::uint64_t> published{0};
    Chunk* readChunk = nullptr;
    std::size_t readOffset = 0;
    std::uint64_t consumed = 0;
    std::atomic<std::uint64_t> replayed{0};
    std::atomic<bool> scheduled{false};
    std::atomic<std::uint32_t> waiters{0};
    // The first failure of a vkBeginCommandBuffer or vkEndCommandBuffer since the last begin;
    // written by the recording thread, read after a flush.
    VkResult error = VK_SUCCESS;
    ~Stream();
};

struct Registry {
    std::mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr resolver = nullptr;
    RecordingThreadOptions options;
    std::unordered_set<VkCommandPool> pools;
    std::unordered_map<VkCommandBuffer, std::shared_ptr<Stream>> streams;
    std::atomic<std::uint64_t> epoch{1};
    // Bumped when the device stops, so the per-thread entry points kept for it are asked again.
    std::atomic<std::uint64_t> generation{1};
    std::mutex chunkMutex;
    std::vector<Chunk*> spareChunks;
    // The recording thread.
    std::thread thread;
    std::mutex readyMutex;
    std::condition_variable readyChanged;
    std::deque<std::shared_ptr<Stream>> ready;
    std::atomic<std::size_t> readyCount{0};
    bool sleeping = false;
    bool stopping = false;
    std::unordered_map<std::string, PFN_vkVoidFunction> thunks;
};

Registry& registry() {
    static auto& state = *new Registry();
    return state;
}

Chunk* takeChunk() {
    auto& state = registry();
    {
        std::lock_guard lock(state.chunkMutex);
        if (!state.spareChunks.empty()) {
            auto* chunk = state.spareChunks.back();
            state.spareChunks.pop_back();
            chunk->next = nullptr;
            return chunk;
        }
    }
    return new Chunk();
}

void giveChunk(Chunk* chunk) {
    constexpr std::size_t KeptChunks = 64;
    auto& state = registry();
    {
        std::lock_guard lock(state.chunkMutex);
        if (state.spareChunks.size() < KeptChunks) {
            state.spareChunks.push_back(chunk);
            return;
        }
    }
    delete chunk;
}

Stream::~Stream() {
    for (auto* chunk = readChunk; chunk != nullptr;) {
        auto* next = chunk->next;
        giveChunk(chunk);
        chunk = next;
    }
}

std::shared_ptr<Stream> makeStream(VkCommandBuffer commands, VkCommandPool pool) {
    auto stream = std::make_shared<Stream>();
    stream->commands = commands;
    stream->pool = pool;
    stream->writeChunk = stream->readChunk = takeChunk();
    return stream;
}

// The stream of a command buffer the recording thread records, or null. The last answer is kept
// per thread until a command buffer of a recorded pool is allocated or freed.
Stream* streamFor(VkCommandBuffer commands) {
    struct Last {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        Stream* stream = nullptr;
        std::uint64_t epoch = 0;
    };
    thread_local Last last;
    auto& state = registry();
    const auto epoch = state.epoch.load(std::memory_order_acquire);
    if (last.commands == commands && last.epoch == epoch) return last.stream;
    Stream* stream = nullptr;
    {
        std::lock_guard lock(state.mutex);
        const auto found = state.streams.find(commands);
        if (found != state.streams.end()) stream = found->second.get();
    }
    last = {commands, stream, epoch};
    return stream;
}

void pushReady(std::shared_ptr<Stream> stream) {
    auto& state = registry();
    std::lock_guard lock(state.readyMutex);
    state.ready.push_back(std::move(stream));
    state.readyCount.fetch_add(1, std::memory_order_release);
    if (state.sleeping) state.readyChanged.notify_one();
}

void schedule(Stream& stream) {
    if (stream.scheduled.load(std::memory_order_seq_cst) || stream.scheduled.exchange(true, std::memory_order_seq_cst)) return;
    pushReady(stream.shared_from_this());
}

// Counts kept per thread and added to the shared totals in steps, so a command costs no shared write.
void tally(std::atomic<std::uint64_t> Counters::*total) {
    constexpr std::uint64_t Step = 256;
    thread_local std::uint64_t queued = 0, direct = 0;
    auto& pending = total == &Counters::queued ? queued : direct;
    if (++pending < Step) return;
    (counters().*total).fetch_add(pending, std::memory_order_relaxed);
    pending = 0;
}

// Room for one command of `payload` bytes in the stream; the header is filled in.
std::byte* reserve(Stream& stream, ReplayFn replay, std::size_t payload) {
    const auto bytes = AlignUp(sizeof(OpHeader) + payload, OpAlign);
    if (bytes > ChunkBytes) throw std::length_error("AGC graphics: a command is larger than a recording thread chunk");
    if (stream.writeOffset + bytes > ChunkBytes) {
        const auto rest = ChunkBytes - stream.writeOffset;
        auto* fresh = takeChunk();
        if (rest != 0) new (stream.writeChunk->data + stream.writeOffset) OpHeader{nullptr, rest};
        stream.writeChunk->next = fresh;
        stream.writeChunk = fresh;
        stream.writeOffset = 0;
        stream.written += rest;
    }
    auto* at = stream.writeChunk->data + stream.writeOffset;
    new (at) OpHeader{replay, bytes};
    stream.writeOffset += bytes;
    stream.written += bytes;
    return at + sizeof(OpHeader);
}

void publish(Stream& stream) {
    stream.published.store(stream.written, std::memory_order_seq_cst);
    tally(&Counters::queued);
    schedule(stream);
}

std::uint64_t nowNs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Waits until the recording thread has made every command published to the stream so far.
void flush(Stream& stream, std::atomic<std::uint64_t> Counters::*cause) {
    const auto target = stream.published.load(std::memory_order_seq_cst);
    if (stream.replayed.load(std::memory_order_acquire) >= target) return;
    auto& count = counters();
    const auto started = nowNs();
    const auto spin = registry().options.spin;
    if (!SpinUntil(spin, [&] { return stream.replayed.load(std::memory_order_acquire) >= target; })) {
        stream.waiters.fetch_add(1, std::memory_order_seq_cst);
        for (auto seen = stream.replayed.load(std::memory_order_seq_cst); seen < target; seen = stream.replayed.load(std::memory_order_seq_cst)) stream.replayed.wait(seen, std::memory_order_seq_cst);
        stream.waiters.fetch_sub(1, std::memory_order_seq_cst);
    }
    count.flushes.fetch_add(1, std::memory_order_relaxed);
    (count.*cause).fetch_add(1, std::memory_order_relaxed);
    count.flushWaitNs.fetch_add(nowNs() - started, std::memory_order_relaxed);
}

std::vector<std::shared_ptr<Stream>> streamsOf(VkCommandPool pool) {
    auto& state = registry();
    std::vector<std::shared_ptr<Stream>> found;
    std::lock_guard lock(state.mutex);
    for (const auto& [commands, stream] : state.streams) {
        if (pool == VK_NULL_HANDLE || stream->pool == pool) found.push_back(stream);
    }
    return found;
}

void flushAll(std::atomic<std::uint64_t> Counters::*cause) {
    for (const auto& stream : streamsOf(VK_NULL_HANDLE)) flush(*stream, cause);
}

bool recordedPool(VkCommandPool pool) {
    auto& state = registry();
    std::lock_guard lock(state.mutex);
    return state.pools.contains(pool);
}

// The recording thread.

void replayUpTo(Stream& stream, std::uint64_t end) {
    while (stream.consumed < end) {
        if (stream.readOffset == ChunkBytes) {
            auto* done = stream.readChunk;
            stream.readChunk = done->next;
            stream.readOffset = 0;
            giveChunk(done);
            continue;
        }
        auto* header = reinterpret_cast<OpHeader*>(stream.readChunk->data + stream.readOffset);
        const auto bytes = header->bytes;
        if (header->replay != nullptr) header->replay(stream.commands, reinterpret_cast<std::byte*>(header) + sizeof(OpHeader), stream);
        stream.consumed += bytes;
        stream.readOffset += static_cast<std::size_t>(bytes);
    }
}

void replayStream(const std::shared_ptr<Stream>& owner) {
    auto& state = registry();
    auto& stream = *owner;
    for (;;) {
        const auto end = stream.published.load(std::memory_order_seq_cst);
        if (stream.consumed < end) {
            const auto started = nowNs();
            replayUpTo(stream, end);
            counters().replayNs.fetch_add(nowNs() - started, std::memory_order_relaxed);
            stream.replayed.store(end, std::memory_order_seq_cst);
            if (stream.waiters.load(std::memory_order_seq_cst) != 0) stream.replayed.notify_all();
            continue;
        }
        // Caught up: the command buffer's next command is usually a few microseconds away.
        if (SpinUntil(state.options.spin, [&] { return stream.published.load(std::memory_order_acquire) != stream.consumed || state.readyCount.load(std::memory_order_acquire) != 0; }) && stream.published.load(std::memory_order_acquire) != stream.consumed) continue;
        stream.scheduled.store(false, std::memory_order_seq_cst);
        if (stream.published.load(std::memory_order_seq_cst) != stream.consumed && !stream.scheduled.exchange(true, std::memory_order_seq_cst)) continue;
        return;
    }
}

void runRecordingThread() {
    auto& state = registry();
    if (state.options.onStart) state.options.onStart();
    for (;;) {
        SpinUntil(state.options.spin, [&] { return state.readyCount.load(std::memory_order_acquire) != 0; });
        std::shared_ptr<Stream> stream;
        {
            std::unique_lock lock(state.readyMutex);
            state.sleeping = true;
            state.readyChanged.wait(lock, [&] { return !state.ready.empty() || state.stopping; });
            state.sleeping = false;
            if (state.ready.empty()) return;
            stream = std::move(state.ready.front());
            state.ready.pop_front();
            state.readyCount.fetch_sub(1, std::memory_order_release);
        }
        replayStream(stream);
    }
}

// Commands whose arguments are all values.

template<auto Member, typename TFunction = std::remove_cvref_t<decltype(std::declval<Real&>().*Member)>>
struct ValueCommand;

template<auto Member, typename... TArgs>
struct ValueCommand<Member, void (VKAPI_PTR*)(VkCommandBuffer, TArgs...)> {
    using Args = std::tuple<TArgs...>;
    static_assert((std::is_trivially_copyable_v<TArgs> && ...) && std::is_trivially_destructible_v<Args>);
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        std::apply([commands](TArgs... args) { (real().*Member)(commands, args...); }, *std::launder(reinterpret_cast<Args*>(payload)));
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, TArgs... args) {
        auto* stream = streamFor(commands);
        if (stream == nullptr) {
            tally(&Counters::direct);
            return (real().*Member)(commands, args...);
        }
        new (reserve(*stream, &Replay, sizeof(Args))) Args(args...);
        publish(*stream);
    }
};

// Commands with arrays or structures behind pointers: each copies what its pointers name into
// the stream next to its arguments, and the copy's pointers point into the stream (a chunk does
// not move until the recording thread is done with it).

struct Packer {
    std::byte* base;
    std::size_t at;
    template<typename T>
    T* Put(const T* source, std::size_t count) {
        if (source == nullptr || count == 0) return nullptr;
        at = AlignUp(at, alignof(T));
        auto* to = reinterpret_cast<T*>(base + at);
        std::memcpy(static_cast<void*>(to), source, sizeof(T) * count);
        at += sizeof(T) * count;
        return to;
    }
};

template<typename T>
constexpr std::size_t Room(std::size_t count) {
    return count == 0 ? 0 : sizeof(T) * count + alignof(T);
}

void requireNoChain(const void* next, const char* what) {
    if (next != nullptr) throw std::runtime_error(std::string("AGC graphics: the recording thread has no copy for a pNext chain on ") + what);
}

template<typename TCall, typename TFill>
void queue(Stream& stream, std::size_t extra, TFill&& fill) {
    auto* payload = reserve(stream, &TCall::Replay, sizeof(TCall) + extra);
    auto* call = new (payload) TCall{};
    Packer packer{payload, sizeof(TCall)};
    fill(*call, packer);
    publish(stream);
}

#define APS5_DIRECT_OR_STREAM(member, ...) \
    auto* stream = streamFor(commands); \
    if (stream == nullptr) { \
        tally(&Counters::direct); \
        return real().member(commands, __VA_ARGS__); \
    }

struct PipelineBarrierCall {
    VkPipelineStageFlags source, destination;
    VkDependencyFlags dependency;
    std::uint32_t memoryCount, bufferCount, imageCount;
    const VkMemoryBarrier* memory;
    const VkBufferMemoryBarrier* buffers;
    const VkImageMemoryBarrier* images;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<PipelineBarrierCall*>(payload));
        real().cmdPipelineBarrier(commands, c.source, c.destination, c.dependency, c.memoryCount, c.memory, c.bufferCount, c.buffers, c.imageCount, c.images);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, VkPipelineStageFlags source, VkPipelineStageFlags destination, VkDependencyFlags dependency, std::uint32_t memoryCount, const VkMemoryBarrier* memory, std::uint32_t bufferCount, const VkBufferMemoryBarrier* buffers, std::uint32_t imageCount, const VkImageMemoryBarrier* images) {
        APS5_DIRECT_OR_STREAM(cmdPipelineBarrier, source, destination, dependency, memoryCount, memory, bufferCount, buffers, imageCount, images)
        for (std::uint32_t i = 0; i < memoryCount; ++i) requireNoChain(memory[i].pNext, "VkMemoryBarrier");
        for (std::uint32_t i = 0; i < bufferCount; ++i) requireNoChain(buffers[i].pNext, "VkBufferMemoryBarrier");
        for (std::uint32_t i = 0; i < imageCount; ++i) requireNoChain(images[i].pNext, "VkImageMemoryBarrier");
        queue<PipelineBarrierCall>(*stream, Room<VkMemoryBarrier>(memoryCount) + Room<VkBufferMemoryBarrier>(bufferCount) + Room<VkImageMemoryBarrier>(imageCount), [&](PipelineBarrierCall& c, Packer& p) {
            c = {source, destination, dependency, memoryCount, bufferCount, imageCount, p.Put(memory, memoryCount), p.Put(buffers, bufferCount), p.Put(images, imageCount)};
        });
    }
};

struct BindDescriptorSetsCall {
    VkPipelineBindPoint point;
    VkPipelineLayout layout;
    std::uint32_t first, count, dynamicCount;
    const VkDescriptorSet* sets;
    const std::uint32_t* dynamicOffsets;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<BindDescriptorSetsCall*>(payload));
        real().cmdBindDescriptorSets(commands, c.point, c.layout, c.first, c.count, c.sets, c.dynamicCount, c.dynamicOffsets);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, VkPipelineBindPoint point, VkPipelineLayout layout, std::uint32_t first, std::uint32_t count, const VkDescriptorSet* sets, std::uint32_t dynamicCount, const std::uint32_t* dynamicOffsets) {
        APS5_DIRECT_OR_STREAM(cmdBindDescriptorSets, point, layout, first, count, sets, dynamicCount, dynamicOffsets)
        queue<BindDescriptorSetsCall>(*stream, Room<VkDescriptorSet>(count) + Room<std::uint32_t>(dynamicCount), [&](BindDescriptorSetsCall& c, Packer& p) {
            c = {point, layout, first, count, dynamicCount, p.Put(sets, count), p.Put(dynamicOffsets, dynamicCount)};
        });
    }
};

struct PushConstantsCall {
    VkPipelineLayout layout;
    VkShaderStageFlags stages;
    std::uint32_t offset, size;
    const std::byte* values;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<PushConstantsCall*>(payload));
        real().cmdPushConstants(commands, c.layout, c.stages, c.offset, c.size, c.values);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, VkPipelineLayout layout, VkShaderStageFlags stages, std::uint32_t offset, std::uint32_t size, const void* values) {
        APS5_DIRECT_OR_STREAM(cmdPushConstants, layout, stages, offset, size, values)
        queue<PushConstantsCall>(*stream, Room<std::byte>(size), [&](PushConstantsCall& c, Packer& p) {
            c = {layout, stages, offset, size, p.Put(static_cast<const std::byte*>(values), size)};
        });
    }
};

struct UpdateBufferCall {
    VkBuffer buffer;
    VkDeviceSize offset, size;
    const std::byte* data;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<UpdateBufferCall*>(payload));
        real().cmdUpdateBuffer(commands, c.buffer, c.offset, c.size, c.data);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, const void* data) {
        APS5_DIRECT_OR_STREAM(cmdUpdateBuffer, buffer, offset, size, data)
        queue<UpdateBufferCall>(*stream, Room<std::byte>(static_cast<std::size_t>(size)), [&](UpdateBufferCall& c, Packer& p) {
            c = {buffer, offset, size, p.Put(static_cast<const std::byte*>(data), static_cast<std::size_t>(size))};
        });
    }
};

template<auto Member, typename TElement>
struct RangeCommand {  // vkCmdSetViewport, vkCmdSetScissor
    std::uint32_t first, count;
    const TElement* elements;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<RangeCommand*>(payload));
        (real().*Member)(commands, c.first, c.count, c.elements);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, std::uint32_t first, std::uint32_t count, const TElement* elements) {
        auto* stream = streamFor(commands);
        if (stream == nullptr) {
            tally(&Counters::direct);
            return (real().*Member)(commands, first, count, elements);
        }
        queue<RangeCommand>(*stream, Room<TElement>(count), [&](RangeCommand& c, Packer& p) { c = {first, count, p.Put(elements, count)}; });
    }
};

struct BindVertexBuffersCall {
    std::uint32_t first, count;
    const VkBuffer* buffers;
    const VkDeviceSize* offsets;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<BindVertexBuffersCall*>(payload));
        real().cmdBindVertexBuffers(commands, c.first, c.count, c.buffers, c.offsets);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, std::uint32_t first, std::uint32_t count, const VkBuffer* buffers, const VkDeviceSize* offsets) {
        APS5_DIRECT_OR_STREAM(cmdBindVertexBuffers, first, count, buffers, offsets)
        queue<BindVertexBuffersCall>(*stream, Room<VkBuffer>(count) + Room<VkDeviceSize>(count), [&](BindVertexBuffersCall& c, Packer& p) {
            c = {first, count, p.Put(buffers, count), p.Put(offsets, count)};
        });
    }
};

struct CopyBufferCall {
    VkBuffer source, destination;
    std::uint32_t count;
    const VkBufferCopy* regions;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<CopyBufferCall*>(payload));
        real().cmdCopyBuffer(commands, c.source, c.destination, c.count, c.regions);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, VkBuffer source, VkBuffer destination, std::uint32_t count, const VkBufferCopy* regions) {
        APS5_DIRECT_OR_STREAM(cmdCopyBuffer, source, destination, count, regions)
        queue<CopyBufferCall>(*stream, Room<VkBufferCopy>(count), [&](CopyBufferCall& c, Packer& p) { c = {source, destination, count, p.Put(regions, count)}; });
    }
};

template<auto Member, typename TRegion>
struct ImageToImageCommand {  // vkCmdCopyImage, vkCmdResolveImage
    VkImage source;
    VkImageLayout sourceLayout;
    VkImage destination;
    VkImageLayout destinationLayout;
    std::uint32_t count;
    const TRegion* regions;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<ImageToImageCommand*>(payload));
        (real().*Member)(commands, c.source, c.sourceLayout, c.destination, c.destinationLayout, c.count, c.regions);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, std::uint32_t count, const TRegion* regions) {
        auto* stream = streamFor(commands);
        if (stream == nullptr) {
            tally(&Counters::direct);
            return (real().*Member)(commands, source, sourceLayout, destination, destinationLayout, count, regions);
        }
        queue<ImageToImageCommand>(*stream, Room<TRegion>(count), [&](ImageToImageCommand& c, Packer& p) { c = {source, sourceLayout, destination, destinationLayout, count, p.Put(regions, count)}; });
    }
};

struct BlitImageCall {
    VkImage source;
    VkImageLayout sourceLayout;
    VkImage destination;
    VkImageLayout destinationLayout;
    std::uint32_t count;
    const VkImageBlit* regions;
    VkFilter filter;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<BlitImageCall*>(payload));
        real().cmdBlitImage(commands, c.source, c.sourceLayout, c.destination, c.destinationLayout, c.count, c.regions, c.filter);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, std::uint32_t count, const VkImageBlit* regions, VkFilter filter) {
        APS5_DIRECT_OR_STREAM(cmdBlitImage, source, sourceLayout, destination, destinationLayout, count, regions, filter)
        queue<BlitImageCall>(*stream, Room<VkImageBlit>(count), [&](BlitImageCall& c, Packer& p) { c = {source, sourceLayout, destination, destinationLayout, count, p.Put(regions, count), filter}; });
    }
};

struct CopyBufferToImageCall {
    VkBuffer buffer;
    VkImage image;
    VkImageLayout layout;
    std::uint32_t count;
    const VkBufferImageCopy* regions;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<CopyBufferToImageCall*>(payload));
        real().cmdCopyBufferToImage(commands, c.buffer, c.image, c.layout, c.count, c.regions);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, VkBuffer buffer, VkImage image, VkImageLayout layout, std::uint32_t count, const VkBufferImageCopy* regions) {
        APS5_DIRECT_OR_STREAM(cmdCopyBufferToImage, buffer, image, layout, count, regions)
        queue<CopyBufferToImageCall>(*stream, Room<VkBufferImageCopy>(count), [&](CopyBufferToImageCall& c, Packer& p) { c = {buffer, image, layout, count, p.Put(regions, count)}; });
    }
};

struct CopyImageToBufferCall {
    VkImage image;
    VkImageLayout layout;
    VkBuffer buffer;
    std::uint32_t count;
    const VkBufferImageCopy* regions;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<CopyImageToBufferCall*>(payload));
        real().cmdCopyImageToBuffer(commands, c.image, c.layout, c.buffer, c.count, c.regions);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, VkImage image, VkImageLayout layout, VkBuffer buffer, std::uint32_t count, const VkBufferImageCopy* regions) {
        APS5_DIRECT_OR_STREAM(cmdCopyImageToBuffer, image, layout, buffer, count, regions)
        queue<CopyImageToBufferCall>(*stream, Room<VkBufferImageCopy>(count), [&](CopyImageToBufferCall& c, Packer& p) { c = {image, layout, buffer, count, p.Put(regions, count)}; });
    }
};

template<auto Member, typename TValue>
struct ClearImageCommand {  // vkCmdClearColorImage, vkCmdClearDepthStencilImage
    VkImage image;
    VkImageLayout layout;
    const TValue* value;
    std::uint32_t count;
    const VkImageSubresourceRange* ranges;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<ClearImageCommand*>(payload));
        (real().*Member)(commands, c.image, c.layout, c.value, c.count, c.ranges);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, VkImage image, VkImageLayout layout, const TValue* value, std::uint32_t count, const VkImageSubresourceRange* ranges) {
        auto* stream = streamFor(commands);
        if (stream == nullptr) {
            tally(&Counters::direct);
            return (real().*Member)(commands, image, layout, value, count, ranges);
        }
        queue<ClearImageCommand>(*stream, Room<TValue>(1) + Room<VkImageSubresourceRange>(count), [&](ClearImageCommand& c, Packer& p) { c = {image, layout, p.Put(value, 1), count, p.Put(ranges, count)}; });
    }
};

struct BeginRenderPassCall {
    VkRenderPassBeginInfo info;
    VkSubpassContents contents;
    VkRenderPassSampleLocationsBeginInfoEXT locations;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<BeginRenderPassCall*>(payload));
        real().cmdBeginRenderPass(commands, &c.info, c.contents);
    }
    static std::size_t LocationRoom(const VkSampleLocationsInfoEXT& info) {
        return Room<VkSampleLocationEXT>(info.sampleLocationsCount);
    }
    static void CopyLocations(VkSampleLocationsInfoEXT& info, Packer& p) {
        requireNoChain(info.pNext, "VkSampleLocationsInfoEXT");
        info.pSampleLocations = p.Put(info.pSampleLocations, info.sampleLocationsCount);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
        APS5_DIRECT_OR_STREAM(cmdBeginRenderPass, info, contents)
        const VkRenderPassSampleLocationsBeginInfoEXT* locations = nullptr;
        if (info->pNext != nullptr) {
            locations = static_cast<const VkRenderPassSampleLocationsBeginInfoEXT*>(info->pNext);
            if (locations->sType != VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT) requireNoChain(info->pNext, "VkRenderPassBeginInfo");
            requireNoChain(locations->pNext, "VkRenderPassSampleLocationsBeginInfoEXT");
        }
        std::size_t extra = Room<VkClearValue>(info->clearValueCount);
        if (locations != nullptr) {
            extra += Room<VkAttachmentSampleLocationsEXT>(locations->attachmentInitialSampleLocationsCount) + Room<VkSubpassSampleLocationsEXT>(locations->postSubpassSampleLocationsCount);
            for (std::uint32_t i = 0; i < locations->attachmentInitialSampleLocationsCount; ++i) extra += LocationRoom(locations->pAttachmentInitialSampleLocations[i].sampleLocationsInfo);
            for (std::uint32_t i = 0; i < locations->postSubpassSampleLocationsCount; ++i) extra += LocationRoom(locations->pPostSubpassSampleLocations[i].sampleLocationsInfo);
        }
        queue<BeginRenderPassCall>(*stream, extra, [&](BeginRenderPassCall& c, Packer& p) {
            c.info = *info;
            c.contents = contents;
            c.info.pClearValues = p.Put(info->pClearValues, info->clearValueCount);
            if (locations == nullptr) return;
            c.locations = *locations;
            auto* initial = p.Put(locations->pAttachmentInitialSampleLocations, locations->attachmentInitialSampleLocationsCount);
            auto* post = p.Put(locations->pPostSubpassSampleLocations, locations->postSubpassSampleLocationsCount);
            for (std::uint32_t i = 0; i < locations->attachmentInitialSampleLocationsCount; ++i) CopyLocations(initial[i].sampleLocationsInfo, p);
            for (std::uint32_t i = 0; i < locations->postSubpassSampleLocationsCount; ++i) CopyLocations(post[i].sampleLocationsInfo, p);
            c.locations.pAttachmentInitialSampleLocations = initial;
            c.locations.pPostSubpassSampleLocations = post;
            c.info.pNext = &c.locations;
        });
    }
};

struct BeginRenderingCall {
    VkRenderingInfo info;
    VkRenderingAttachmentInfo depth, stencil;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream&) {
        const auto& c = *std::launder(reinterpret_cast<BeginRenderingCall*>(payload));
        real().cmdBeginRendering(commands, &c.info);
    }
    static VKAPI_ATTR void VKAPI_CALL Call(VkCommandBuffer commands, const VkRenderingInfo* info) {
        APS5_DIRECT_OR_STREAM(cmdBeginRendering, info)
        requireNoChain(info->pNext, "VkRenderingInfo");
        for (std::uint32_t i = 0; i < info->colorAttachmentCount; ++i) requireNoChain(info->pColorAttachments[i].pNext, "VkRenderingAttachmentInfo");
        if (info->pDepthAttachment != nullptr) requireNoChain(info->pDepthAttachment->pNext, "VkRenderingAttachmentInfo");
        if (info->pStencilAttachment != nullptr) requireNoChain(info->pStencilAttachment->pNext, "VkRenderingAttachmentInfo");
        queue<BeginRenderingCall>(*stream, Room<VkRenderingAttachmentInfo>(info->colorAttachmentCount), [&](BeginRenderingCall& c, Packer& p) {
            c.info = *info;
            c.info.pColorAttachments = p.Put(info->pColorAttachments, info->colorAttachmentCount);
            if (info->pDepthAttachment != nullptr) {
                c.depth = *info->pDepthAttachment;
                c.info.pDepthAttachment = &c.depth;
            }
            if (info->pStencilAttachment != nullptr) {
                c.stencil = *info->pStencilAttachment;
                c.info.pStencilAttachment = &c.stencil;
            }
        });
    }
};

// Command buffer lifetime.

struct BeginCall {
    VkCommandBufferBeginInfo info;
    VkCommandBufferInheritanceInfo inheritance;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream& stream) {
        const auto& c = *std::launder(reinterpret_cast<BeginCall*>(payload));
        stream.error = real().beginCommandBuffer(commands, &c.info);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Call(VkCommandBuffer commands, const VkCommandBufferBeginInfo* info) {
        APS5_DIRECT_OR_STREAM(beginCommandBuffer, info)
        requireNoChain(info->pNext, "VkCommandBufferBeginInfo");
        if (info->pInheritanceInfo != nullptr) requireNoChain(info->pInheritanceInfo->pNext, "VkCommandBufferInheritanceInfo");
        queue<BeginCall>(*stream, 0, [&](BeginCall& c, Packer&) {
            c.info = *info;
            if (info->pInheritanceInfo != nullptr) {
                c.inheritance = *info->pInheritanceInfo;
                c.info.pInheritanceInfo = &c.inheritance;
            }
        });
        return VK_SUCCESS;
    }
};

struct EndCall {
    static void Replay(VkCommandBuffer commands, std::byte*, Stream& stream) {
        const auto result = real().endCommandBuffer(commands);
        if (stream.error == VK_SUCCESS) stream.error = result;
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Call(VkCommandBuffer commands) {
        auto* stream = streamFor(commands);
        if (stream == nullptr) return real().endCommandBuffer(commands);
        queue<EndCall>(*stream, 0, [](EndCall&, Packer&) {});
        return VK_SUCCESS;
    }
};

struct ResetCall {
    VkCommandBufferResetFlags flags;
    static void Replay(VkCommandBuffer commands, std::byte* payload, Stream& stream) {
        const auto& c = *std::launder(reinterpret_cast<ResetCall*>(payload));
        stream.error = real().resetCommandBuffer(commands, c.flags);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL Call(VkCommandBuffer commands, VkCommandBufferResetFlags flags) {
        auto* stream = streamFor(commands);
        if (stream == nullptr) return real().resetCommandBuffer(commands, flags);
        queue<ResetCall>(*stream, 0, [&](ResetCall& c, Packer&) { c.flags = flags; });
        return VK_SUCCESS;
    }
};

// APS5_PIPELINE_STATS or APS5_PROFILE_DRAW: a [recording] line every 10 s, from the submits.
void report() {
    static const bool enabled = std::getenv("APS5_PIPELINE_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!enabled) return;
    static std::mutex mutex;
    static auto last = std::chrono::steady_clock::now();
    static RecordingThreadCounts before{};
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(10)) return;
    last = now;
    const auto totals = RecordingThreadTotals();
    const auto flushes = totals.flushes - before.flushes;
    std::fprintf(stderr, "[recording] (10 s): %llu commands made on the recording thread, %llu directly; %llu flushes waited (submit %llu, pool %llu, destroy %llu), %.1f us each; the thread spent %.1f ms in commands\n",
        static_cast<unsigned long long>(totals.queued - before.queued), static_cast<unsigned long long>(totals.direct - before.direct), static_cast<unsigned long long>(flushes),
        static_cast<unsigned long long>(totals.submitFlushes - before.submitFlushes), static_cast<unsigned long long>(totals.poolFlushes - before.poolFlushes), static_cast<unsigned long long>(totals.destroyFlushes - before.destroyFlushes),
        flushes == 0 ? 0.0 : static_cast<double>(totals.flushWaitNs - before.flushWaitNs) / 1e3 / static_cast<double>(flushes), static_cast<double>(totals.replayNs - before.replayNs) / 1e6);
    before = totals;
}

VKAPI_ATTR VkResult VKAPI_CALL queueSubmit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    report();
    for (std::uint32_t s = 0; s < count; ++s) {
        for (std::uint32_t i = 0; i < submits[s].commandBufferCount; ++i) {
            auto* stream = streamFor(submits[s].pCommandBuffers[i]);
            if (stream == nullptr) continue;
            flush(*stream, &Counters::submitFlushes);
            if (stream->error != VK_SUCCESS) return stream->error;
        }
    }
    return real().queueSubmit(queue, count, submits, fence);
}

void forget(std::span<const VkCommandBuffer> buffers) {
    std::vector<std::shared_ptr<Stream>> gone;
    auto& state = registry();
    std::lock_guard lock(state.mutex);
    for (const auto commands : buffers) {
        const auto found = state.streams.find(commands);
        if (found == state.streams.end()) continue;
        gone.push_back(std::move(found->second));
        state.streams.erase(found);
    }
    state.epoch.fetch_add(1, std::memory_order_acq_rel);
}

void flushPool(VkCommandPool pool) {
    for (const auto& stream : streamsOf(pool)) flush(*stream, &Counters::poolFlushes);
}

VKAPI_ATTR VkResult VKAPI_CALL allocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers) {
    if (!recordedPool(info->commandPool)) return real().allocateCommandBuffers(device, info, buffers);
    flushPool(info->commandPool);
    const auto result = real().allocateCommandBuffers(device, info, buffers);
    if (result != VK_SUCCESS) return result;
    std::vector<std::shared_ptr<Stream>> made, replaced;
    for (std::uint32_t i = 0; i < info->commandBufferCount; ++i) made.push_back(makeStream(buffers[i], info->commandPool));
    auto& state = registry();
    std::lock_guard lock(state.mutex);
    for (auto& stream : made) {
        auto& slot = state.streams[stream->commands];
        if (slot != nullptr) replaced.push_back(std::move(slot));
        slot = std::move(stream);
    }
    state.epoch.fetch_add(1, std::memory_order_acq_rel);
    return result;
}

VKAPI_ATTR void VKAPI_CALL freeCommandBuffers(VkDevice device, VkCommandPool pool, std::uint32_t count, const VkCommandBuffer* buffers) {
    if (recordedPool(pool)) {
        flushPool(pool);
        forget({buffers, count});
    }
    real().freeCommandBuffers(device, pool, count, buffers);
}

VKAPI_ATTR VkResult VKAPI_CALL resetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    if (recordedPool(pool)) flushPool(pool);
    return real().resetCommandPool(device, pool, flags);
}

VKAPI_ATTR void VKAPI_CALL destroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator) {
    if (recordedPool(pool)) {
        flushPool(pool);
        std::vector<std::shared_ptr<Stream>> gone;
        auto& state = registry();
        std::lock_guard lock(state.mutex);
        for (auto entry = state.streams.begin(); entry != state.streams.end();) {
            if (entry->second->pool != pool) {
                ++entry;
                continue;
            }
            gone.push_back(std::move(entry->second));
            entry = state.streams.erase(entry);
        }
        state.pools.erase(pool);
        state.epoch.fetch_add(1, std::memory_order_acq_rel);
    }
    real().destroyCommandPool(device, pool, allocator);
}

VKAPI_ATTR void VKAPI_CALL destroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    StopRecordingThread(device);
    real().destroyDevice(device, allocator);
}

template<auto Member, typename TFunction = std::remove_cvref_t<decltype(std::declval<Real&>().*Member)>>
struct FlushingCall;

template<auto Member, typename TResult, typename... TArgs>
struct FlushingCall<Member, TResult (VKAPI_PTR*)(VkDevice, TArgs...)> {
    static VKAPI_ATTR TResult VKAPI_CALL Call(VkDevice device, TArgs... args) {
        flushAll(&Counters::destroyFlushes);
        return (real().*Member)(device, args...);
    }
};

template<typename TFunction>
PFN_vkVoidFunction Entry(TFunction function) {
    return reinterpret_cast<PFN_vkVoidFunction>(function);
}

PFN_vkVoidFunction thunkFor(const char* name) {
    auto& state = registry();
    const auto found = state.thunks.find(name);
    return found == state.thunks.end() ? nullptr : found->second;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL recordingDeviceProc(VkDevice device, const char* name) {
    auto& state = registry();
    if (device != state.device) return state.resolver(device, name);
    // Call sites resolve by name on every call (Context::Function), so the answer is kept per
    // thread for each name pointer, checked against the name it was given for.
    struct Kept {
        std::string name;
        VkDevice device = VK_NULL_HANDLE;
        std::uint64_t generation = 0;
        PFN_vkVoidFunction function = nullptr;
    };
    thread_local std::unordered_map<const char*, Kept> kept;
    const auto generation = state.generation.load(std::memory_order_acquire);
    if (const auto found = kept.find(name); found != kept.end() && found->second.device == device && found->second.generation == generation && found->second.name == name) return found->second.function;
    PFN_vkVoidFunction function = nullptr;
    if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) function = reinterpret_cast<PFN_vkVoidFunction>(&recordingDeviceProc);
    else if (const auto thunk = thunkFor(name)) function = thunk;
    else if (std::strncmp(name, "vkCmd", 5) == 0 || std::strncmp(name, "vkQueueSubmit", 13) == 0) {
        // A command the recording thread cannot queue, or a submit that does not wait for it,
        // would run beside the queued commands, out of order.
        std::fprintf(stderr, "[recording] %s has no recording-thread entry and is unavailable while APS5_RECORDING_THREAD is on\n", name);
        function = nullptr;
    } else function = state.resolver(device, name);
    kept.insert_or_assign(name, Kept{name, device, generation, function});
    return function;
}

}

bool RecordingThreadEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_RECORDING_THREAD");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

PFN_vkGetDeviceProcAddr RecordOnRecordingThread(VkDevice device, PFN_vkGetDeviceProcAddr resolver, RecordingThreadOptions options) {
    if (device == VK_NULL_HANDLE || resolver == nullptr) throw std::invalid_argument("AGC graphics: the recording thread needs a device and its resolver");
    auto& state = registry();
    std::lock_guard lock(state.mutex);
    if (state.device != VK_NULL_HANDLE) {
        std::fprintf(stderr, "[recording] another device already records on the recording thread; this one records directly\n");
        return resolver;
    }
    auto& table = real();
    const auto resolve = [&](auto& member, const char* name, bool required) {
        member = reinterpret_cast<std::remove_reference_t<decltype(member)>>(resolver(device, name));
        if (member == nullptr && required) throw std::runtime_error(std::string("AGC graphics: missing Vulkan function: ") + name);
    };
    state.thunks.clear();
    // A command the device does not have stays missing: its thunk is only handed out when the
    // real entry point exists.
    const auto offer = [&](const char* name, bool present, PFN_vkVoidFunction thunk) {
        if (present) state.thunks.emplace(name, thunk);
    };
#define APS5_RESOLVE(type, member, name) resolve(table.member, name, false);
    APS5_RECORDED_COMMANDS(APS5_RESOLVE)
    APS5_FLUSHING_DESTROYS(APS5_RESOLVE)
#undef APS5_RESOLVE
    resolve(table.beginCommandBuffer, "vkBeginCommandBuffer", true);
    resolve(table.endCommandBuffer, "vkEndCommandBuffer", true);
    resolve(table.resetCommandBuffer, "vkResetCommandBuffer", true);
    resolve(table.queueSubmit, "vkQueueSubmit", true);
    resolve(table.allocateCommandBuffers, "vkAllocateCommandBuffers", true);
    resolve(table.freeCommandBuffers, "vkFreeCommandBuffers", true);
    resolve(table.resetCommandPool, "vkResetCommandPool", true);
    resolve(table.destroyCommandPool, "vkDestroyCommandPool", true);
    resolve(table.destroyDevice, "vkDestroyDevice", true);
    offer("vkCmdBindPipeline", table.cmdBindPipeline != nullptr, Entry(&ValueCommand<&Real::cmdBindPipeline>::Call));
    offer("vkCmdBindIndexBuffer", table.cmdBindIndexBuffer != nullptr, Entry(&ValueCommand<&Real::cmdBindIndexBuffer>::Call));
    offer("vkCmdFillBuffer", table.cmdFillBuffer != nullptr, Entry(&ValueCommand<&Real::cmdFillBuffer>::Call));
    offer("vkCmdDispatch", table.cmdDispatch != nullptr, Entry(&ValueCommand<&Real::cmdDispatch>::Call));
    offer("vkCmdDispatchIndirect", table.cmdDispatchIndirect != nullptr, Entry(&ValueCommand<&Real::cmdDispatchIndirect>::Call));
    offer("vkCmdDraw", table.cmdDraw != nullptr, Entry(&ValueCommand<&Real::cmdDraw>::Call));
    offer("vkCmdDrawIndexed", table.cmdDrawIndexed != nullptr, Entry(&ValueCommand<&Real::cmdDrawIndexed>::Call));
    offer("vkCmdDrawIndirect", table.cmdDrawIndirect != nullptr, Entry(&ValueCommand<&Real::cmdDrawIndirect>::Call));
    offer("vkCmdDrawIndexedIndirect", table.cmdDrawIndexedIndirect != nullptr, Entry(&ValueCommand<&Real::cmdDrawIndexedIndirect>::Call));
    offer("vkCmdDrawIndirectCountKHR", table.cmdDrawIndirectCount != nullptr, Entry(&ValueCommand<&Real::cmdDrawIndirectCount>::Call));
    offer("vkCmdDrawIndexedIndirectCountKHR", table.cmdDrawIndexedIndirectCount != nullptr, Entry(&ValueCommand<&Real::cmdDrawIndexedIndirectCount>::Call));
    offer("vkCmdDrawMeshTasksEXT", table.cmdDrawMeshTasks != nullptr, Entry(&ValueCommand<&Real::cmdDrawMeshTasks>::Call));
    offer("vkCmdDrawMeshTasksIndirectEXT", table.cmdDrawMeshTasksIndirect != nullptr, Entry(&ValueCommand<&Real::cmdDrawMeshTasksIndirect>::Call));
    offer("vkCmdEndRenderPass", table.cmdEndRenderPass != nullptr, Entry(&ValueCommand<&Real::cmdEndRenderPass>::Call));
    offer("vkCmdEndRenderingKHR", table.cmdEndRendering != nullptr, Entry(&ValueCommand<&Real::cmdEndRendering>::Call));
    offer("vkCmdSetDepthBounds", table.cmdSetDepthBounds != nullptr, Entry(&ValueCommand<&Real::cmdSetDepthBounds>::Call));
    offer("vkCmdSetDepthBias", table.cmdSetDepthBias != nullptr, Entry(&ValueCommand<&Real::cmdSetDepthBias>::Call));
    offer("vkCmdSetCullModeEXT", table.cmdSetCullMode != nullptr, Entry(&ValueCommand<&Real::cmdSetCullMode>::Call));
    offer("vkCmdSetFrontFaceEXT", table.cmdSetFrontFace != nullptr, Entry(&ValueCommand<&Real::cmdSetFrontFace>::Call));
    offer("vkCmdSetDepthTestEnableEXT", table.cmdSetDepthTestEnable != nullptr, Entry(&ValueCommand<&Real::cmdSetDepthTestEnable>::Call));
    offer("vkCmdSetDepthWriteEnableEXT", table.cmdSetDepthWriteEnable != nullptr, Entry(&ValueCommand<&Real::cmdSetDepthWriteEnable>::Call));
    offer("vkCmdSetDepthCompareOpEXT", table.cmdSetDepthCompareOp != nullptr, Entry(&ValueCommand<&Real::cmdSetDepthCompareOp>::Call));
    offer("vkCmdSetDepthBoundsTestEnableEXT", table.cmdSetDepthBoundsTestEnable != nullptr, Entry(&ValueCommand<&Real::cmdSetDepthBoundsTestEnable>::Call));
    offer("vkCmdSetStencilTestEnableEXT", table.cmdSetStencilTestEnable != nullptr, Entry(&ValueCommand<&Real::cmdSetStencilTestEnable>::Call));
    offer("vkCmdSetStencilOpEXT", table.cmdSetStencilOp != nullptr, Entry(&ValueCommand<&Real::cmdSetStencilOp>::Call));
    offer("vkCmdSetStencilCompareMask", table.cmdSetStencilCompareMask != nullptr, Entry(&ValueCommand<&Real::cmdSetStencilCompareMask>::Call));
    offer("vkCmdSetStencilWriteMask", table.cmdSetStencilWriteMask != nullptr, Entry(&ValueCommand<&Real::cmdSetStencilWriteMask>::Call));
    offer("vkCmdSetStencilReference", table.cmdSetStencilReference != nullptr, Entry(&ValueCommand<&Real::cmdSetStencilReference>::Call));
    offer("vkCmdResetQueryPool", table.cmdResetQueryPool != nullptr, Entry(&ValueCommand<&Real::cmdResetQueryPool>::Call));
    offer("vkCmdBeginQuery", table.cmdBeginQuery != nullptr, Entry(&ValueCommand<&Real::cmdBeginQuery>::Call));
    offer("vkCmdEndQuery", table.cmdEndQuery != nullptr, Entry(&ValueCommand<&Real::cmdEndQuery>::Call));
    offer("vkCmdWriteTimestamp", table.cmdWriteTimestamp != nullptr, Entry(&ValueCommand<&Real::cmdWriteTimestamp>::Call));
    offer("vkCmdCopyQueryPoolResults", table.cmdCopyQueryPoolResults != nullptr, Entry(&ValueCommand<&Real::cmdCopyQueryPoolResults>::Call));
    offer("vkCmdWriteBufferMarkerAMD", table.cmdWriteBufferMarker != nullptr, Entry(&ValueCommand<&Real::cmdWriteBufferMarker>::Call));
    offer("vkCmdSetCheckpointNV", table.cmdSetCheckpoint != nullptr, Entry(&ValueCommand<&Real::cmdSetCheckpoint>::Call));
    offer("vkCmdPipelineBarrier", table.cmdPipelineBarrier != nullptr, Entry(&PipelineBarrierCall::Call));
    offer("vkCmdBindDescriptorSets", table.cmdBindDescriptorSets != nullptr, Entry(&BindDescriptorSetsCall::Call));
    offer("vkCmdPushConstants", table.cmdPushConstants != nullptr, Entry(&PushConstantsCall::Call));
    offer("vkCmdUpdateBuffer", table.cmdUpdateBuffer != nullptr, Entry(&UpdateBufferCall::Call));
    offer("vkCmdSetViewport", table.cmdSetViewport != nullptr, Entry(&RangeCommand<&Real::cmdSetViewport, VkViewport>::Call));
    offer("vkCmdSetScissor", table.cmdSetScissor != nullptr, Entry(&RangeCommand<&Real::cmdSetScissor, VkRect2D>::Call));
    offer("vkCmdBindVertexBuffers", table.cmdBindVertexBuffers != nullptr, Entry(&BindVertexBuffersCall::Call));
    offer("vkCmdCopyBuffer", table.cmdCopyBuffer != nullptr, Entry(&CopyBufferCall::Call));
    offer("vkCmdCopyImage", table.cmdCopyImage != nullptr, Entry(&ImageToImageCommand<&Real::cmdCopyImage, VkImageCopy>::Call));
    offer("vkCmdResolveImage", table.cmdResolveImage != nullptr, Entry(&ImageToImageCommand<&Real::cmdResolveImage, VkImageResolve>::Call));
    offer("vkCmdBlitImage", table.cmdBlitImage != nullptr, Entry(&BlitImageCall::Call));
    offer("vkCmdCopyBufferToImage", table.cmdCopyBufferToImage != nullptr, Entry(&CopyBufferToImageCall::Call));
    offer("vkCmdCopyImageToBuffer", table.cmdCopyImageToBuffer != nullptr, Entry(&CopyImageToBufferCall::Call));
    offer("vkCmdClearColorImage", table.cmdClearColorImage != nullptr, Entry(&ClearImageCommand<&Real::cmdClearColorImage, VkClearColorValue>::Call));
    offer("vkCmdClearDepthStencilImage", table.cmdClearDepthStencilImage != nullptr, Entry(&ClearImageCommand<&Real::cmdClearDepthStencilImage, VkClearDepthStencilValue>::Call));
    offer("vkCmdBeginRenderPass", table.cmdBeginRenderPass != nullptr, Entry(&BeginRenderPassCall::Call));
    offer("vkCmdBeginRenderingKHR", table.cmdBeginRendering != nullptr, Entry(&BeginRenderingCall::Call));
    offer("vkBeginCommandBuffer", true, Entry(&BeginCall::Call));
    offer("vkEndCommandBuffer", true, Entry(&EndCall::Call));
    offer("vkResetCommandBuffer", true, Entry(&ResetCall::Call));
    offer("vkQueueSubmit", true, Entry(&queueSubmit));
    offer("vkAllocateCommandBuffers", true, Entry(&allocateCommandBuffers));
    offer("vkFreeCommandBuffers", true, Entry(&freeCommandBuffers));
    offer("vkResetCommandPool", true, Entry(&resetCommandPool));
    offer("vkDestroyCommandPool", true, Entry(&destroyCommandPool));
    offer("vkDestroyDevice", true, Entry(&destroyDevice));
#define APS5_FLUSHING(type, member, name) offer(name, table.member != nullptr, Entry(&FlushingCall<&Real::member>::Call));
    APS5_FLUSHING_DESTROYS(APS5_FLUSHING)
#undef APS5_FLUSHING
    state.device = device;
    state.resolver = resolver;
    state.options = std::move(options);
    state.stopping = false;
    state.thread = std::thread(runRecordingThread);
    return &recordingDeviceProc;
}

void RecordPoolOnRecordingThread(VkDevice device, VkCommandPool pool) {
    auto& state = registry();
    std::lock_guard lock(state.mutex);
    if (device == state.device) state.pools.insert(pool);
}

void StopRecordingThread(VkDevice device) noexcept {
    auto& state = registry();
    {
        std::lock_guard lock(state.mutex);
        if (device != state.device || !state.thread.joinable()) return;
    }
    flushAll(&Counters::destroyFlushes);
    {
        std::lock_guard lock(state.readyMutex);
        state.stopping = true;
        state.readyChanged.notify_all();
    }
    state.thread.join();
    std::unordered_map<VkCommandBuffer, std::shared_ptr<Stream>> gone;
    std::lock_guard lock(state.mutex);
    gone.swap(state.streams);
    state.pools.clear();
    state.ready.clear();
    state.device = VK_NULL_HANDLE;
    state.epoch.fetch_add(1, std::memory_order_acq_rel);
    state.generation.fetch_add(1, std::memory_order_acq_rel);
}

RecordingThreadCounts RecordingThreadTotals() {
    const auto& count = counters();
    return {count.queued.load(), count.direct.load(), count.flushes.load(), count.flushWaitNs.load(), count.submitFlushes.load(), count.poolFlushes.load(), count.destroyFlushes.load(), count.replayNs.load()};
}

}
