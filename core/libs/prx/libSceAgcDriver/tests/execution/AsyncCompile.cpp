#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PipelineCompilePool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/mman.h>
#endif

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using namespace AgcDriver::Graphics;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 16;
constexpr std::uint32_t Kept = 0x40404040u;
constexpr std::size_t BlockBytes = 1u << 16u;

alignas(256) constexpr std::array<std::uint32_t, 6> VertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};
alignas(256) constexpr std::array<std::uint32_t, 4> WhiteCode{
    0x7e0e02f2, 0xf800180f, 0x07070707, 0xbf810000,
};
alignas(256) constexpr std::array<std::uint32_t, 6> StoringCode{
    0x7e0e02f2, 0xe0700000, 0x80000700, 0xf800180f, 0x07070707, 0xbf810000,
};
constexpr std::array<std::array<float, 4>, 3> Triangle{{
    {-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}
}};
alignas(256) std::array<std::uint32_t, Width * Height> LinearPixels{};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

struct Block {
    explicit Block(std::size_t bytes) : bytes(bytes) {
        void* raw = nullptr;
#ifdef _WIN32
        raw = _aligned_malloc(bytes, BlockBytes);
#else
        raw = std::aligned_alloc(BlockBytes, bytes);
#endif
        Require(raw != nullptr, "cannot allocate the color target");
        data = static_cast<std::uint32_t*>(raw);
        GuestAllocations::Mutation mutation;
        mutation.Add(data, bytes, true, true, true);
    }
    ~Block() {
        StorageTexture::FlushPending(Address(), bytes, nullptr, "test release");
        {
            GuestAllocations::Mutation mutation;
            mutation.Remove(data);
        }
#ifdef _WIN32
        _aligned_free(data);
#else
        std::free(data);
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    std::size_t bytes;
    std::uint32_t* data = nullptr;
};

struct Shaders {
    ShaderRecompiler::RecompileResult vertex;
    ShaderRecompiler::RecompileResult fragment;
    std::array<CompiledShader, 2> compiled;
};

std::unique_ptr<Shaders> Compile(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> pixelCode, std::span<const std::uint32_t> pixelUserData) {
    constexpr std::uint32_t waveSize = 64;
    const auto target = device.Target();
    const auto vertexBuffer = BufferDescriptor(Triangle.data(), 16u, static_cast<std::uint32_t>(Triangle.size()));
    const std::vector<std::uint32_t> vertexUserData(vertexBuffer.begin(), vertexBuffer.end());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(VertexCode.data()), std::as_bytes(std::span(VertexCode))}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(VertexCode.data()), VertexCode, 0, {}},
        {waveSize, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    auto shaders = std::make_unique<Shaders>();
    shaders->vertex = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(shaders->vertex.pushConstants.size());
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.wave32 = false;
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(pixelCode.data()), std::as_bytes(pixelCode)}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(pixelCode.data()), pixelCode, 0, {}},
        {waveSize, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    shaders->fragment = ShaderRecompiler::Recompile(fragment);
    shaders->compiled = {{{ShaderStage::Vertex, &shaders->vertex, 0}, {ShaderStage::Fragment, &shaders->fragment, PixelPushOffset(vertexPush, target)}}};
    return shaders;
}

State StateFor(const ColorTarget& color) {
    State state{};
    state.stages = {ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    state.color = color;
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    return state;
}

ColorTarget TiledTarget(const Block& block) {
    ColorTarget color{block.Address(), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, block.bytes, 0xe4u};
    color.tileMode = ColorTileMode::RenderTarget;
    color.elementBytes = 4;
    color.surfaceAddress = block.Address();
    color.surfaceExtent = {Width, Height};
    return color;
}

std::vector<std::uint32_t> ReadBack(AgcDriver::VulkanDevice& device, std::uint64_t address, std::size_t bytes) {
    StorageTexture::FlushPending(address, bytes, nullptr, "test read-back");
    device.WaitIdle();
    std::vector<std::uint32_t> stored(bytes / 4u);
    AgcDriver::GuestMemory::Read(address, std::as_writable_bytes(std::span(stored)), 1);
    return stored;
}

void Expect(const std::vector<std::uint32_t>& pixels, std::uint32_t word, const char* what) {
    const auto wrong = std::ranges::count_if(pixels, [&](std::uint32_t pixel) { return pixel != word; });
    Require(wrong == 0, std::string(what) + ": " + std::to_string(wrong) + " of " + std::to_string(pixels.size()) + " words differ from " + std::to_string(word) + ", the first holds " + std::to_string(pixels.front()));
}

void WaitForBuilds(std::uint64_t builds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (;;) {
        const auto totals = AsyncCompileTotalsNow();
        Require(totals.failed == 0, "a queued pipeline build failed");
        if (totals.built >= builds && totals.queued == 0 && totals.building == 0) return;
        Require(std::chrono::steady_clock::now() < deadline, "a queued pipeline was never built");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void DroppedDrawDrawsOnceReady(AgcDriver::VulkanDevice& device) {
    Block block(ColorTargetLayout(Width, Height, ColorTileMode::RenderTarget, 4).Bytes());
    const auto color = TiledTarget(block);
    std::fill_n(block.data, block.bytes / 4u, Kept);
    const auto shaders = Compile(device, WhiteCode, std::vector<std::uint32_t>(8, 0u));
    const auto state = StateFor(color);
    const AgcDriver::Pm4::DrawParameters draw{0, 3, 0, 1, 0, false};
    Require(!DrawCompileWait(state, shaders->compiled, AgcDriver::VulkanDeviceTestAccess::ContextOf(device).recorder, true).has_value(), "a color draw into a tiled target has no reason to wait");
    const auto before = AsyncCompileTotalsNow();
    const auto storeBefore = PipelineStoreTotals();
    HoldPipelineCompilesForTest(true);
    device.Draw(state, draw, shaders->compiled);
    device.Draw(state, draw, shaders->compiled);
    const auto dropped = AsyncCompileTotalsNow();
    Require(dropped.droppedDraws == before.droppedDraws + 2u, "the draws of an unbuilt pipeline were not both dropped");
    Require(dropped.droppedUnqueued == before.droppedUnqueued, "a dropped draw did not queue its pipeline");
    Require(dropped.queued == before.queued + 1u, "two draws of one pipeline did not share one queued build");
    Require(dropped.built == before.built, "the held pool built a pipeline");
    Expect(ReadBack(device, block.Address(), block.bytes), Kept, "a dropped draw reached its target");
    HoldPipelineCompilesForTest(false);
    WaitForBuilds(before.built + 1u);
    Require(PipelineStoreTotals().misses == storeBefore.misses + 1u, "the pool did not build through the pipeline store");
    device.Draw(state, draw, shaders->compiled);
    Require(AsyncCompileTotalsNow().droppedDraws == dropped.droppedDraws, "a draw of a built pipeline was dropped");
    const auto drawn = ReadBack(device, block.Address(), block.bytes);
    Require(std::ranges::count(drawn, 0xffffffffu) == Width * Height, "the draw of a built pipeline did not draw");
}

void LinearTargetWaits(AgcDriver::VulkanDevice& device) {
    LinearPixels.fill(Kept);
    const ColorTarget color{reinterpret_cast<std::uintptr_t>(LinearPixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, LinearPixels.size() * 4u, 0xe4u};
    const auto shaders = Compile(device, WhiteCode, std::vector<std::uint32_t>(8, 0u));
    auto state = StateFor(color);
    state.blends.front().colorWriteMask = 7;
    const auto before = AsyncCompileTotalsNow();
    const auto index = static_cast<std::size_t>(CompileWait::LinearTarget);
    device.Draw(state, AgcDriver::Pm4::DrawParameters{0, 3, 0, 1, 0, false}, shaders->compiled);
    const auto after = AsyncCompileTotalsNow();
    Require(after.droppedDraws == before.droppedDraws, "a draw into a linear target was dropped");
    const auto notRecorded = static_cast<std::size_t>(CompileWait::NotRecorded);
    Require(after.waits[index] + after.waits[notRecorded] == before.waits[index] + before.waits[notRecorded] + 1u, "the wait of a draw into a linear target was not counted");
    const auto reason = DrawCompileWait(state, shaders->compiled, AgcDriver::VulkanDeviceTestAccess::ContextOf(device).recorder, true);
    Require(reason.has_value() && *reason == CompileWait::LinearTarget, "a recorded draw into a linear target may be dropped");
    device.WaitIdle();
    Expect(ReadBack(device, color.address, color.bytes), 0x40ffffffu, "a draw into a linear target did not draw at once");
}

void GuestWriteWaits(AgcDriver::VulkanDevice& device) {
    Block block(ColorTargetLayout(Width, Height, ColorTileMode::RenderTarget, 4).Bytes());
    const auto color = TiledTarget(block);
    std::fill_n(block.data, block.bytes / 4u, Kept);
    Block storedBlock(BlockBytes);
    std::fill_n(storedBlock.data, storedBlock.bytes / 4u, 0u);
    const auto descriptor = BufferDescriptor(storedBlock.data, 0u, 1024u);
    std::vector<std::uint32_t> userData(descriptor.begin(), descriptor.end());
    userData.resize(8u, 0u);
    const auto shaders = Compile(device, StoringCode, userData);
    Require(ShadersWriteMemory(shaders->compiled), "the storing pixel shader does not count as writing guest memory");
    const auto state = StateFor(color);
    const auto reason = DrawCompileWait(state, shaders->compiled, AgcDriver::VulkanDeviceTestAccess::ContextOf(device).recorder, true);
    Require(reason.has_value() && *reason == CompileWait::GuestWrites, "a draw writing guest memory may be dropped");
    const auto before = AsyncCompileTotalsNow();
    const auto index = static_cast<std::size_t>(CompileWait::GuestWrites);
    device.Draw(state, AgcDriver::Pm4::DrawParameters{0, 3, 0, 1, 0, false}, shaders->compiled);
    const auto after = AsyncCompileTotalsNow();
    Require(after.droppedDraws == before.droppedDraws, "a draw writing guest memory was dropped");
    Require(after.waits[index] == before.waits[index] + 1u, "the wait of a draw writing guest memory was not counted");
    const auto drawn = ReadBack(device, block.Address(), block.bytes);
    Require(std::ranges::count(drawn, 0xffffffffu) == Width * Height, "a waited draw that writes guest memory did not draw at once");
}

void OcclusionTestWaits(AgcDriver::VulkanDevice& device) {
    Block block(ColorTargetLayout(Width, Height, ColorTileMode::RenderTarget, 4).Bytes());
    auto* recorder = AgcDriver::VulkanDeviceTestAccess::ContextOf(device).recorder;
    Require(recorder != nullptr, "the test device has no recorder");
    const auto shaders = Compile(device, WhiteCode, std::vector<std::uint32_t>(8, 0u));
    auto state = StateFor(TiledTarget(block));
    state.blends.front().colorWriteMask = 0;
    Require(!DrawCompileWait(state, shaders->compiled, recorder, true).has_value() || recorder->CountingSamples(), "a masked draw waits before any query");
    recorder->CountSamples();
    const auto reason = DrawCompileWait(state, shaders->compiled, recorder, true);
    Require(reason.has_value() && *reason == CompileWait::Occlusion, "a query test draw may be dropped while samples count");
    state.blends.front().colorWriteMask = 15;
    Require(!DrawCompileWait(state, shaders->compiled, recorder, true).has_value(), "a visible draw waits only because samples count");
}

void PrebuiltFragmentServesDepthDraws(AgcDriver::VulkanDevice& device) {
    const auto target = device.Target();
    if (!target.registrationModules) return;
    const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    if (!context.graphicsPipelineLibrary) return;
    const auto base = Compile(device, WhiteCode, std::vector<std::uint32_t>(8, 0u));
    const std::vector<std::uint32_t> pixelUserData(8, 0u);
    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.wave32 = false;
    pixel.inputAddr = ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionX) | ShaderRecompiler::PixelInputBit(ShaderRecompiler::PixelInput::PositionY);
    pixel.posX = true;
    pixel.posY = true;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const auto vertexPush = static_cast<std::uint32_t>(base->vertex.pushConstants.size());
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(WhiteCode.data()), std::as_bytes(std::span(WhiteCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(WhiteCode.data()), WhiteCode, 0, {}},
        {64u, 0, pixelUserData, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        PixelPushLayout(vertexPush, target)
    };
    fragment.useCache = false;
    const auto handle = ShaderRecompiler::PrepareShader(fragment);
    const auto registration = ShaderRecompiler::RegistrationResult(fragment, *handle);
    Require(registration != nullptr, "a plain pixel shader has no registration result");
    const std::array<CompiledShader, 1> prebuilt{{{ShaderStage::Fragment, registration.get(), PixelPushOffset(vertexPush, target)}}};
    const ShaderStages stages{ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    Require(device.PrebuildGraphicsLibraries(stages, prebuilt), "the pixel shader did not prebuild its fragment library");
    Require(device.PrebuildGraphicsLibraries(stages, prebuilt, VK_FORMAT_D32_SFLOAT), "the pixel shader did not prebuild its fragment library for a depth format");
    const auto built = PipelineLibraryCountersOf(context.device).built[2];
    Block block(ColorTargetLayout(Width, Height, ColorTileMode::RenderTarget, 4).Bytes());
    auto state = StateFor(TiledTarget(block));
    const std::array<CompiledShader, 2> shaders{{{ShaderStage::Vertex, &base->vertex, 0}, {ShaderStage::Fragment, registration.get(), PixelPushOffset(vertexPush, target)}}};
    const auto layouts = DescribeSetLayouts(context, shaders);
    static_cast<void>(CachedPipeline(context, state, VertexInputLayout{}, layouts.View(), shaders, VK_IMAGE_LAYOUT_GENERAL));
    Require(PipelineLibraryCountersOf(context.device).built[2] == built, "a draw without depth rebuilt the prebuilt fragment library");
    state.depth = DepthTarget{block.Address(), 0, {Width, Height}, VK_FORMAT_D32_SFLOAT, 0.5f, 0};
    state.depthTest = true;
    state.depthCompare = VK_COMPARE_OP_LESS;
    static_cast<void>(CachedPipeline(context, state, VertexInputLayout{}, layouts.View(), shaders, VK_IMAGE_LAYOUT_GENERAL));
    Require(PipelineLibraryCountersOf(context.device).built[2] == built, "a draw with a depth target rebuilt the prebuilt fragment library");
}

void PoolOrderAndBound() {
    HoldPipelineCompilesForTest(true);
    std::mutex mutex;
    std::vector<int> order;
    const auto job = [&](int name) {
        return [&, name]() -> std::function<void()> {
            return [&, name] {
                std::lock_guard lock(mutex);
                order.push_back(name);
            };
        };
    };
    const auto device = reinterpret_cast<VkDevice>(std::uintptr_t{0x1000});
    const auto bound = AsyncCompileQueueBound();
    Require(bound == 4u, "the pool test expects a queue of four");
    Require(RequestPipelineCompile(device, 1, job(1), false) == CompileRequest::Queued, "the first build was not queued");
    Require(RequestPipelineCompile(device, 2, job(2), false) == CompileRequest::Queued, "the second build was not queued");
    Require(RequestPipelineCompile(device, 3, job(3), false) == CompileRequest::Queued, "the third build was not queued");
    Require(RequestPipelineCompile(device, 2, job(2), false) == CompileRequest::Queued, "a repeated request was refused");
    Require(RequestPipelineCompile(device, 2, job(2), false) == CompileRequest::Queued, "a repeated request was refused");
    Require(RequestPipelineCompile(device, 3, job(3), false) == CompileRequest::Queued, "a repeated request was refused");
    Require(RequestPipelineCompile(device, 4, job(4), false) == CompileRequest::Queued, "the fourth build was not queued");
    Require(RequestPipelineCompile(device, 5, job(5), false) == CompileRequest::QueueFull, "the queue took a build past its bound");
    Require(RequestPipelineCompile(device, 2, job(2), false) == CompileRequest::Queued, "a repeat of a queued build was refused at the bound");
    Require(RequestPipelineCompile(device, 6, job(6), true) == CompileRequest::Queued, "a build a draw waits for was refused at the bound");
    HoldPipelineCompilesForTest(false);
    for (;;) {
        const auto totals = AsyncCompileTotalsNow();
        if (totals.queued == 0u && totals.building == 0u) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    DrainPipelineCompiles(device);
    std::lock_guard lock(mutex);
    Require(order == std::vector<int>{6, 2, 3, 1, 4}, "builds did not run by urgency and then by waiting draws, oldest first");
}

void PoolRunsDispatchWork() {
    const auto index = static_cast<std::size_t>(CompileWait::Dispatch);
    const auto before = AsyncCompileTotalsNow().waits[index];
    bool ran = false;
    RunOnCompilePool(CompileWait::Dispatch, [&] { ran = OnCompilePoolThread(); });
    Require(ran, "the dispatch compile did not run on the pool");
    bool thrown = false;
    try {
        RunOnCompilePool(CompileWait::Dispatch, [] { throw std::runtime_error("build failed"); });
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    Require(thrown, "the failure of a dispatch compile did not reach the waiting thread");
    Require(AsyncCompileTotalsNow().waits[index] == before + 2u, "the waits of dispatches were not counted");
}

}

int main(int argc, char** argv) {
    try {
        Require(std::getenv("APS5_NO_ASYNC_COMPILE") == nullptr, "this test covers asynchronous compiles");
        const std::string_view mode = argc > 1 ? argv[1] : "draws";
        if (mode == "pool") {
            PoolOrderAndBound();
            PoolRunsDispatchWork();
            StopPipelineCompilePool();
            Require(PipelineCompileWorkers() == 0, "stopping the pool left a worker running");
            std::puts("compile pool passed");
            return 0;
        }
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
        DroppedDrawDrawsOnceReady(*device);
        LinearTargetWaits(*device);
        GuestWriteWaits(*device);
        OcclusionTestWaits(*device);
        PrebuiltFragmentServesDepthDraws(*device);
        const auto json = AsyncCompileSummaryJson();
        Require(json.find("\"dropped_draws\":2") != std::string::npos, "the summary lacks the dropped draws: " + json);
        std::printf("async compile passed: %s\n", json.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
