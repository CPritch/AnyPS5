#include "prx/libSceAgcDriver/Graphics/include/RecordingThread.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

template<typename THandle>
THandle Handle(std::uintptr_t value) {
    return reinterpret_cast<THandle>(value);
}

std::uintptr_t Value(const void* handle) {
    return reinterpret_cast<std::uintptr_t>(handle);
}

// What the mock device saw, in order: the command, its command buffer and its arguments, and
// whether it ran on the test's own thread.
struct Event {
    std::string what;
    std::uintptr_t commands = 0;
    bool callerThread = false;
};

std::mutex eventMutex;
std::vector<Event> events;
std::thread::id caller;
VkResult endResult = VK_SUCCESS;
std::atomic<std::uintptr_t> nextHandle{0x1000};

void Note(VkCommandBuffer commands, std::string what) {
    std::lock_guard lock(eventMutex);
    events.push_back({std::move(what), Value(commands), std::this_thread::get_id() == caller});
}

std::vector<Event> Taken() {
    std::lock_guard lock(eventMutex);
    auto taken = events;
    events.clear();
    return taken;
}

VKAPI_ATTR void VKAPI_CALL mockBindPipeline(VkCommandBuffer commands, VkPipelineBindPoint point, VkPipeline pipeline) {
    Note(commands, "bind pipeline " + std::to_string(point) + " " + std::to_string(Value(pipeline)));
}

VKAPI_ATTR void VKAPI_CALL mockDraw(VkCommandBuffer commands, std::uint32_t vertices, std::uint32_t instances, std::uint32_t firstVertex, std::uint32_t firstInstance) {
    Note(commands, "draw " + std::to_string(vertices) + " " + std::to_string(instances) + " " + std::to_string(firstVertex) + " " + std::to_string(firstInstance));
}

VKAPI_ATTR void VKAPI_CALL mockPushConstants(VkCommandBuffer commands, VkPipelineLayout layout, VkShaderStageFlags stages, std::uint32_t offset, std::uint32_t size, const void* values) {
    std::string text = "push " + std::to_string(Value(layout)) + " " + std::to_string(stages) + " " + std::to_string(offset) + ":";
    for (std::uint32_t i = 0; i < size; ++i) text += " " + std::to_string(static_cast<const std::uint8_t*>(values)[i]);
    Note(commands, text);
}

VKAPI_ATTR void VKAPI_CALL mockBindDescriptorSets(VkCommandBuffer commands, VkPipelineBindPoint, VkPipelineLayout layout, std::uint32_t first, std::uint32_t count, const VkDescriptorSet* sets, std::uint32_t dynamicCount, const std::uint32_t* offsets) {
    std::string text = "sets " + std::to_string(Value(layout)) + " " + std::to_string(first) + ":";
    for (std::uint32_t i = 0; i < count; ++i) text += " " + std::to_string(Value(sets[i]));
    text += " /";
    for (std::uint32_t i = 0; i < dynamicCount; ++i) text += " " + std::to_string(offsets[i]);
    Note(commands, text);
}

VKAPI_ATTR void VKAPI_CALL mockPipelineBarrier(VkCommandBuffer commands, VkPipelineStageFlags source, VkPipelineStageFlags destination, VkDependencyFlags, std::uint32_t memoryCount, const VkMemoryBarrier* memory, std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t imageCount, const VkImageMemoryBarrier* images) {
    std::string text = "barrier " + std::to_string(source) + "->" + std::to_string(destination) + " memory";
    for (std::uint32_t i = 0; i < memoryCount; ++i) text += " " + std::to_string(memory[i].srcAccessMask) + "/" + std::to_string(memory[i].dstAccessMask);
    text += " images";
    for (std::uint32_t i = 0; i < imageCount; ++i) text += " " + std::to_string(Value(images[i].image)) + ":" + std::to_string(images[i].oldLayout) + "->" + std::to_string(images[i].newLayout);
    Note(commands, text);
}

VKAPI_ATTR void VKAPI_CALL mockSetViewport(VkCommandBuffer commands, std::uint32_t first, std::uint32_t count, const VkViewport* viewports) {
    std::string text = "viewport " + std::to_string(first) + ":";
    for (std::uint32_t i = 0; i < count; ++i) text += " " + std::to_string(static_cast<int>(viewports[i].width)) + "x" + std::to_string(static_cast<int>(viewports[i].height));
    Note(commands, text);
}

VKAPI_ATTR void VKAPI_CALL mockUpdateBuffer(VkCommandBuffer commands, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, const void* data) {
    std::uint64_t sum = 0;
    for (VkDeviceSize i = 0; i < size; ++i) sum += static_cast<const std::uint8_t*>(data)[i];
    Note(commands, "update " + std::to_string(Value(buffer)) + " " + std::to_string(offset) + " " + std::to_string(size) + " sum " + std::to_string(sum));
}

VKAPI_ATTR void VKAPI_CALL mockClearColorImage(VkCommandBuffer commands, VkImage image, VkImageLayout layout, const VkClearColorValue* value, std::uint32_t count, const VkImageSubresourceRange* ranges) {
    std::string text = "clear " + std::to_string(Value(image)) + " " + std::to_string(layout) + " " + std::to_string(value->uint32[0]) + ":";
    for (std::uint32_t i = 0; i < count; ++i) text += " " + std::to_string(ranges[i].baseMipLevel) + "+" + std::to_string(ranges[i].levelCount);
    Note(commands, text);
}

VKAPI_ATTR void VKAPI_CALL mockBeginRenderPass(VkCommandBuffer commands, const VkRenderPassBeginInfo* info, VkSubpassContents) {
    std::string text = "pass " + std::to_string(Value(info->renderPass)) + " clears";
    for (std::uint32_t i = 0; i < info->clearValueCount; ++i) text += " " + std::to_string(info->pClearValues[i].color.uint32[0]);
    if (info->pNext != nullptr) {
        const auto& locations = *static_cast<const VkRenderPassSampleLocationsBeginInfoEXT*>(info->pNext);
        text += " locations";
        for (std::uint32_t i = 0; i < locations.postSubpassSampleLocationsCount; ++i) {
            const auto& sample = locations.pPostSubpassSampleLocations[i].sampleLocationsInfo;
            for (std::uint32_t s = 0; s < sample.sampleLocationsCount; ++s) text += " " + std::to_string(static_cast<int>(sample.pSampleLocations[s].x * 16.0f));
        }
    }
    Note(commands, text);
}

VKAPI_ATTR void VKAPI_CALL mockBeginRendering(VkCommandBuffer commands, const VkRenderingInfo* info) {
    std::string text = "rendering " + std::to_string(info->layerCount) + " colors";
    for (std::uint32_t i = 0; i < info->colorAttachmentCount; ++i) text += " " + std::to_string(Value(info->pColorAttachments[i].imageView));
    text += " depth " + std::to_string(info->pDepthAttachment != nullptr ? Value(info->pDepthAttachment->imageView) : 0);
    Note(commands, text);
}

VKAPI_ATTR VkResult VKAPI_CALL mockBegin(VkCommandBuffer commands, const VkCommandBufferBeginInfo* info) {
    Note(commands, "begin " + std::to_string(info->flags));
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockEnd(VkCommandBuffer commands) {
    Note(commands, "end");
    return endResult;
}

VKAPI_ATTR VkResult VKAPI_CALL mockReset(VkCommandBuffer commands, VkCommandBufferResetFlags) {
    Note(commands, "reset");
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockSubmit(VkQueue, std::uint32_t count, const VkSubmitInfo* submits, VkFence) {
    for (std::uint32_t s = 0; s < count; ++s) {
        for (std::uint32_t i = 0; i < submits[s].commandBufferCount; ++i) Note(submits[s].pCommandBuffers[i], "submit");
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL mockAllocate(VkDevice, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* buffers) {
    for (std::uint32_t i = 0; i < info->commandBufferCount; ++i) buffers[i] = Handle<VkCommandBuffer>(nextHandle.fetch_add(0x10));
    Note(VK_NULL_HANDLE, "allocate " + std::to_string(Value(info->commandPool)));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockFree(VkDevice, VkCommandPool pool, std::uint32_t count, const VkCommandBuffer* buffers) {
    for (std::uint32_t i = 0; i < count; ++i) Note(buffers[i], "free " + std::to_string(Value(pool)));
}

VKAPI_ATTR VkResult VKAPI_CALL mockResetPool(VkDevice, VkCommandPool pool, VkCommandPoolResetFlags) {
    Note(VK_NULL_HANDLE, "reset pool " + std::to_string(Value(pool)));
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL mockDestroyPool(VkDevice, VkCommandPool pool, const VkAllocationCallbacks*) {
    Note(VK_NULL_HANDLE, "destroy pool " + std::to_string(Value(pool)));
}

VKAPI_ATTR void VKAPI_CALL mockDestroyPipeline(VkDevice, VkPipeline pipeline, const VkAllocationCallbacks*) {
    Note(VK_NULL_HANDLE, "destroy pipeline " + std::to_string(Value(pipeline)));
}

VKAPI_ATTR void VKAPI_CALL mockDestroyDevice(VkDevice device, const VkAllocationCallbacks*) {
    Note(VK_NULL_HANDLE, "destroy device " + std::to_string(Value(device)));
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    struct Entry {
        const char* name;
        PFN_vkVoidFunction function;
    };
    static const Entry entries[] = {
        {"vkCmdBindPipeline", reinterpret_cast<PFN_vkVoidFunction>(&mockBindPipeline)},
        {"vkCmdDraw", reinterpret_cast<PFN_vkVoidFunction>(&mockDraw)},
        {"vkCmdPushConstants", reinterpret_cast<PFN_vkVoidFunction>(&mockPushConstants)},
        {"vkCmdBindDescriptorSets", reinterpret_cast<PFN_vkVoidFunction>(&mockBindDescriptorSets)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(&mockPipelineBarrier)},
        {"vkCmdSetViewport", reinterpret_cast<PFN_vkVoidFunction>(&mockSetViewport)},
        {"vkCmdUpdateBuffer", reinterpret_cast<PFN_vkVoidFunction>(&mockUpdateBuffer)},
        {"vkCmdClearColorImage", reinterpret_cast<PFN_vkVoidFunction>(&mockClearColorImage)},
        {"vkCmdBeginRenderPass", reinterpret_cast<PFN_vkVoidFunction>(&mockBeginRenderPass)},
        {"vkCmdBeginRenderingKHR", reinterpret_cast<PFN_vkVoidFunction>(&mockBeginRendering)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(&mockBegin)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(&mockEnd)},
        {"vkResetCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(&mockReset)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(&mockSubmit)},
        {"vkAllocateCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(&mockAllocate)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(&mockFree)},
        {"vkResetCommandPool", reinterpret_cast<PFN_vkVoidFunction>(&mockResetPool)},
        {"vkDestroyCommandPool", reinterpret_cast<PFN_vkVoidFunction>(&mockDestroyPool)},
        {"vkDestroyPipeline", reinterpret_cast<PFN_vkVoidFunction>(&mockDestroyPipeline)},
        {"vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(&mockDestroyDevice)},
    };
    for (const auto& entry : entries) {
        if (std::strcmp(entry.name, name) == 0) return entry.function;
    }
    return nullptr;
}

// One installed recording thread with one recorded pool, torn down through vkDestroyDevice.
struct Device {
    VkDevice device;
    VkCommandPool recorded = Handle<VkCommandPool>(0x501);
    VkCommandPool direct = Handle<VkCommandPool>(0x502);
    PFN_vkGetDeviceProcAddr proc;
    explicit Device(std::uintptr_t handle) : device(Handle<VkDevice>(handle)) {
        caller = std::this_thread::get_id();
        endResult = VK_SUCCESS;
        proc = RecordOnRecordingThread(device, mockProc, {});
        RecordPoolOnRecordingThread(device, recorded);
        Taken();
    }
    ~Device() { Destroy(); }
    void Destroy() {
        if (device == VK_NULL_HANDLE) return;
        Function<PFN_vkDestroyDevice>("vkDestroyDevice")(device, nullptr);
        device = VK_NULL_HANDLE;
    }
    template<typename TFunction>
    TFunction Function(const char* name) const {
        return reinterpret_cast<TFunction>(proc(device, name));
    }
    VkCommandBuffer Allocate(VkCommandPool pool) const {
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        VkCommandBuffer commands = VK_NULL_HANDLE;
        Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device, &info, &commands);
        return commands;
    }
    VkResult Begin(VkCommandBuffer commands) const {
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        return Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &info);
    }
    VkResult Submit(VkCommandBuffer commands) const {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        return Function<PFN_vkQueueSubmit>("vkQueueSubmit")(Handle<VkQueue>(0x600), 1, &submit, VK_NULL_HANDLE);
    }
};

std::vector<std::string> Whats(const std::vector<Event>& seen, std::uintptr_t commands) {
    std::vector<std::string> whats;
    for (const auto& event : seen) {
        if (event.commands == commands) whats.push_back(event.what);
    }
    return whats;
}

void CommandsAreMadeInOrderOnTheRecordingThreadFromCopies() {
    Device device(0x100);
    const auto commands = device.Allocate(device.recorded);
    Expect(device.Begin(commands) == VK_SUCCESS, "begin on a recorded command buffer returns success");
    {
        std::uint8_t push[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        device.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, Handle<VkPipelineLayout>(0x70), VK_SHADER_STAGE_VERTEX_BIT, 4, sizeof(push), push);
        std::memset(push, 0, sizeof(push));
        VkDescriptorSet sets[2] = {Handle<VkDescriptorSet>(0x81), Handle<VkDescriptorSet>(0x82)};
        std::uint32_t offsets[1] = {256};
        device.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, Handle<VkPipelineLayout>(0x70), 1, 2, sets, 1, offsets);
        sets[0] = sets[1] = VK_NULL_HANDLE;
        offsets[0] = 0;
        VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT};
        VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image.image = Handle<VkImage>(0x90);
        image.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        image.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        device.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &memory, 0, nullptr, 1, &image);
        memory.srcAccessMask = 0;
        image.image = VK_NULL_HANDLE;
        VkViewport viewport{0, 0, 1920, 1080, 0, 1};
        device.Function<PFN_vkCmdSetViewport>("vkCmdSetViewport")(commands, 0, 1, &viewport);
        viewport.width = 0;
        std::vector<std::uint8_t> data(1000, 3);
        device.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer")(commands, Handle<VkBuffer>(0xa0), 16, data.size(), data.data());
        std::fill(data.begin(), data.end(), 0);
        VkClearColorValue clear{};
        clear.uint32[0] = 7;
        VkImageSubresourceRange ranges[2] = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 2, 3, 0, 1}};
        device.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(commands, Handle<VkImage>(0x91), VK_IMAGE_LAYOUT_GENERAL, &clear, 2, ranges);
        clear.uint32[0] = 0;
        ranges[1].baseMipLevel = 9;
        device.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, Handle<VkPipeline>(0xb0));
        device.Function<PFN_vkCmdDraw>("vkCmdDraw")(commands, 3, 1, 0, 0);
    }
    Expect(device.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands) == VK_SUCCESS, "end on a recorded command buffer returns success");
    Expect(device.Submit(commands) == VK_SUCCESS, "the submit of a recorded command buffer succeeds");
    const auto seen = Taken();
    const std::vector<std::string> expected = {
        "begin 1",
        "push 112 1 4: 1 2 3 4 5 6 7 8",
        "sets 112 1: 129 130 / 256",
        "barrier 2048->128 memory 64/32 images 144:0->1",
        "viewport 0: 1920x1080",
        "update 160 16 1000 sum 3000",
        "clear 145 1 7: 0+1 2+3",
        "bind pipeline 0 176",
        "draw 3 1 0 0",
        "end",
        "submit",
    };
    const auto whats = Whats(seen, Value(commands));
    Expect(whats == expected, "the recorded commands arrive in order, with the values they were called with, before the submit");
    if (whats != expected) {
        for (const auto& what : whats) std::fprintf(stderr, "  saw: %s\n", what.c_str());
    }
    bool offThread = true;
    for (const auto& event : seen) {
        if (event.commands == Value(commands) && event.what != "submit") offThread = offThread && !event.callerThread;
    }
    Expect(offThread, "every recorded command ran on the recording thread");
    const auto submit = std::find_if(seen.begin(), seen.end(), [](const Event& event) { return event.what == "submit"; });
    Expect(submit != seen.end() && submit->callerThread, "the submit ran on the caller's thread");
}

void CommandBuffersOfOtherPoolsAreRecordedDirectly() {
    Device device(0x101);
    const auto commands = device.Allocate(device.direct);
    device.Begin(commands);
    device.Function<PFN_vkCmdDraw>("vkCmdDraw")(commands, 6, 2, 0, 0);
    const auto seen = Taken();
    const auto whats = Whats(seen, Value(commands));
    Expect(whats == std::vector<std::string>{"begin 1", "draw 6 2 0 0"}, "a command buffer of a pool not handed over is recorded at once");
    Expect(!seen.empty() && seen.back().callerThread, "...on the caller's thread");
}

void AnEndFailureIsReturnedByTheSubmit() {
    Device device(0x102);
    const auto commands = device.Allocate(device.recorded);
    device.Begin(commands);
    endResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    Expect(device.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands) == VK_SUCCESS, "end returns before it is made");
    Expect(device.Submit(commands) == VK_ERROR_OUT_OF_DEVICE_MEMORY, "the submit returns the failure of the end");
    const auto whats = Whats(Taken(), Value(commands));
    Expect(std::find(whats.begin(), whats.end(), "submit") == whats.end(), "a command buffer whose end failed is not submitted");
    endResult = VK_SUCCESS;
    device.Begin(commands);
    device.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands);
    Expect(device.Submit(commands) == VK_SUCCESS, "a new begin clears the failure");
}

void DestroysAndPoolChangesWaitForTheQueuedCommands() {
    Device device(0x103);
    const auto commands = device.Allocate(device.recorded);
    device.Begin(commands);
    device.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(commands, VK_PIPELINE_BIND_POINT_COMPUTE, Handle<VkPipeline>(0xc0));
    device.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(device.device, Handle<VkPipeline>(0xc0), nullptr);
    auto seen = Taken();
    Expect(seen.size() == 4 && seen[2].what == "bind pipeline 1 192" && seen[3].what == "destroy pipeline 192", "a pipeline is destroyed only after the queued command that binds it was made");
    device.Function<PFN_vkCmdDraw>("vkCmdDraw")(commands, 1, 1, 0, 0);
    device.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands);
    device.Function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(device.device, device.recorded, 1, &commands);
    seen = Taken();
    Expect(seen.size() == 3 && seen[0].what == "draw 1 1 0 0" && seen[1].what == "end" && seen[2].what == "free 1281", "a command buffer is freed only after its queued commands were made");
    const auto second = device.Allocate(device.recorded);
    device.Begin(second);
    device.Function<PFN_vkCmdDraw>("vkCmdDraw")(second, 2, 1, 0, 0);
    device.Function<PFN_vkResetCommandPool>("vkResetCommandPool")(device.device, device.recorded, 0);
    seen = Taken();
    Expect(seen.size() == 4 && seen[2].what == "draw 2 1 0 0" && seen[3].what == "reset pool 1281", "a pool is reset only after the queued commands of its command buffers were made");
    device.Function<PFN_vkCmdDraw>("vkCmdDraw")(second, 3, 1, 0, 0);
    device.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device.device, device.recorded, nullptr);
    seen = Taken();
    Expect(seen.size() == 2 && seen[0].what == "draw 3 1 0 0" && seen[1].what == "destroy pool 1281", "a pool is destroyed only after its queued commands were made");
}

void AQueueLongerThanAChunkKeepsItsOrder() {
    Device device(0x104);
    const auto commands = device.Allocate(device.recorded);
    device.Begin(commands);
    constexpr std::uint32_t Count = 40000;  // about 2.5 MB of push constants, ten chunks
    std::uint8_t push[32];
    for (std::uint32_t i = 0; i < Count; ++i) {
        std::memset(push, static_cast<int>(i & 0xff), sizeof(push));
        device.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants")(commands, Handle<VkPipelineLayout>(0x70), VK_SHADER_STAGE_FRAGMENT_BIT, i % 64, sizeof(push), push);
    }
    device.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands);
    device.Submit(commands);
    const auto whats = Whats(Taken(), Value(commands));
    bool ordered = whats.size() == Count + 3;
    for (std::uint32_t i = 0; ordered && i < Count; ++i) {
        std::string expected = "push 112 16 " + std::to_string(i % 64) + ":";
        for (std::size_t b = 0; b < sizeof(push); ++b) expected += " " + std::to_string(i & 0xff);
        ordered = whats[i + 1] == expected;
    }
    Expect(ordered, "40,000 queued commands over several chunks arrive complete and in order");
}

void ThreadsRecordingTheirOwnCommandBuffersKeepEachOrder() {
    Device device(0x105);
    constexpr int Threads = 4;
    constexpr std::uint32_t Count = 20000;
    std::vector<VkCommandBuffer> buffers;
    for (int t = 0; t < Threads; ++t) buffers.push_back(device.Allocate(device.recorded));
    Taken();
    std::vector<std::thread> threads;
    std::vector<VkResult> results(Threads, VK_ERROR_UNKNOWN);
    for (int t = 0; t < Threads; ++t) {
        threads.emplace_back([&, t] {
            const auto commands = buffers[static_cast<std::size_t>(t)];
            device.Begin(commands);
            for (std::uint32_t i = 0; i < Count; ++i) device.Function<PFN_vkCmdDraw>("vkCmdDraw")(commands, i, static_cast<std::uint32_t>(t), 0, 0);
            device.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands);
            results[static_cast<std::size_t>(t)] = device.Submit(commands);
        });
    }
    for (auto& thread : threads) thread.join();
    const auto seen = Taken();
    for (int t = 0; t < Threads; ++t) {
        const auto whats = Whats(seen, Value(buffers[static_cast<std::size_t>(t)]));
        bool ordered = results[static_cast<std::size_t>(t)] == VK_SUCCESS && whats.size() == Count + 3 && whats.back() == "submit";
        for (std::uint32_t i = 0; ordered && i < Count; ++i) ordered = whats[i + 1] == "draw " + std::to_string(i) + " " + std::to_string(t) + " 0 0";
        Expect(ordered, "thread " + std::to_string(t) + "'s 20,000 draws arrive complete and in order before its submit");
    }
}

void RenderPassChainsAreCopiedAndUnknownChainsRefused() {
    Device device(0x106);
    const auto commands = device.Allocate(device.recorded);
    device.Begin(commands);
    {
        VkClearValue clears[2]{};
        clears[0].color.uint32[0] = 11;
        clears[1].color.uint32[0] = 12;
        VkSampleLocationEXT locations[2] = {{0.25f, 0.75f}, {0.5f, 0.5f}};
        VkSampleLocationsInfoEXT info{VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT, nullptr, VK_SAMPLE_COUNT_2_BIT, {1, 1}, 2, locations};
        VkSubpassSampleLocationsEXT post{0, info};
        VkRenderPassSampleLocationsBeginInfoEXT chain{VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT};
        chain.postSubpassSampleLocationsCount = 1;
        chain.pPostSubpassSampleLocations = &post;
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.pNext = &chain;
        begin.renderPass = Handle<VkRenderPass>(0xd0);
        begin.clearValueCount = 2;
        begin.pClearValues = clears;
        device.Function<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass")(commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
        locations[0].x = 0.0f;
        clears[0].color.uint32[0] = 0;
        VkRenderingAttachmentInfo colors[2]{{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
        colors[0].imageView = Handle<VkImageView>(0xe1);
        colors[1].imageView = Handle<VkImageView>(0xe2);
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = Handle<VkImageView>(0xe3);
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 2;
        rendering.pColorAttachments = colors;
        rendering.pDepthAttachment = &depth;
        device.Function<PFN_vkCmdBeginRenderingKHR>("vkCmdBeginRenderingKHR")(commands, &rendering);
        colors[1].imageView = VK_NULL_HANDLE;
        depth.imageView = VK_NULL_HANDLE;
        VkMemoryBarrier2 unknown{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER, &unknown};
        bool refused = false;
        try {
            device.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
        } catch (const std::runtime_error&) {
            refused = true;
        }
        Expect(refused, "a barrier with a pNext chain the recording thread cannot copy is refused");
    }
    device.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands);
    device.Submit(commands);
    const auto whats = Whats(Taken(), Value(commands));
    Expect(whats.size() == 5 && whats[1] == "pass 208 clears 11 12 locations 4 8" && whats[2] == "rendering 1 colors 225 226 depth 227", "a render pass begin with its sample locations, and a rendering begin with its attachments, arrive as they were called");
}

void UnknownCommandsAreUnavailableAndOtherDevicesPassThrough() {
    Device device(0x107);
    Expect(device.proc(device.device, "vkCmdSetLineWidth") == nullptr, "a command without a recording-thread entry is unavailable");
    Expect(device.proc(device.device, "vkQueueSubmit2") == nullptr, "a submit that does not wait for the queue is unavailable");
    Expect(device.proc(device.device, "vkCmdDraw") != reinterpret_cast<PFN_vkVoidFunction>(&mockDraw), "vkCmdDraw resolves to the queueing entry");
    Expect(device.proc(Handle<VkDevice>(0x999), "vkCmdDraw") == reinterpret_cast<PFN_vkVoidFunction>(&mockDraw), "another device's lookups pass through");
    Expect(RecordOnRecordingThread(Handle<VkDevice>(0x998), mockProc, {}) == mockProc, "a second device records directly");
}

void DestroyingTheDeviceMakesEveryQueuedCommandFirst() {
    Device device(0x108);
    const auto commands = device.Allocate(device.recorded);
    device.Begin(commands);
    for (std::uint32_t i = 0; i < 1000; ++i) device.Function<PFN_vkCmdDraw>("vkCmdDraw")(commands, i, 1, 0, 0);
    device.Destroy();
    const auto seen = Taken();
    Expect(seen.size() == 1003 && seen[1001].what == "draw 999 1 0 0" && seen.back().what == "destroy device 264", "the device is destroyed after every queued command was made");
}

}

int main() {
    CommandsAreMadeInOrderOnTheRecordingThreadFromCopies();
    CommandBuffersOfOtherPoolsAreRecordedDirectly();
    AnEndFailureIsReturnedByTheSubmit();
    DestroysAndPoolChangesWaitForTheQueuedCommands();
    AQueueLongerThanAChunkKeepsItsOrder();
    ThreadsRecordingTheirOwnCommandBuffersKeepEachOrder();
    RenderPassChainsAreCopiedAndUnknownChainsRefused();
    UnknownCommandsAreUnavailableAndOtherDevicesPassThrough();
    DestroyingTheDeviceMakesEveryQueuedCommandFirst();
    if (failures != 0) {
        std::fprintf(stderr, "%d recording thread checks failed\n", failures);
        return 1;
    }
    std::printf("recording thread: all checks passed\n");
    return 0;
}
