#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using namespace AgcDriver::Graphics;
using DepthFastClearHarness::Compile;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::Shaders;
using DepthFastClearHarness::Triangles;
using DepthFastClearHarness::Width;
using ShaderRecompiler::ShaderStage;

alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};

struct Draw {
    std::array<CompiledShader, 2> shaders;
    State state{};
    AgcDriver::Pm4::DrawParameters parameters{0, static_cast<std::uint32_t>(Triangles().size()), 0, 1, 0, false};
    std::span<std::byte> pixels;
};

Draw Describe(AgcDriver::VulkanDevice& device, const Shaders& compiled, std::span<std::byte> pixels = Pixels) {
    Draw draw{{{{ShaderStage::Vertex, &compiled.vertex, 0}, {ShaderStage::Fragment, &compiled.pixel, PixelPushOffset(compiled.vertexPush, device.Target())}}}};
    draw.pixels = pixels;
    auto& state = draw.state;
    state.stages = {ShaderPath::Vertex, 0u, 32u, 32u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    return draw;
}

std::uint64_t Total(DrawAheadEvent event) {
    return DrawAheadTotals()[static_cast<std::size_t>(event)];
}

std::uint32_t Commit(AgcDriver::VulkanDevice& device, const Draw& draw, DrawAhead* ahead, std::uint64_t shape = 0) {
    const auto pixels = draw.pixels;
    std::fill(pixels.begin(), pixels.end(), std::byte{0});
    device.Draw(draw.state, draw.parameters, draw.shaders, {}, nullptr, nullptr, shape, ahead);
    device.WaitIdle();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(pixels.data()), pixels.size());
    std::uint32_t covered = 0;
    for (std::size_t pixel = 0; pixel < pixels.size() / 4u; ++pixel) {
        if (std::to_integer<std::uint8_t>(pixels[pixel * 4u]) == 255u) ++covered;
    }
    return covered;
}

void PreparationForTheDeviceIsUsed(AgcDriver::VulkanDevice& device, const Draw& draw) {
    DrawAhead ahead;
    device.PrepareDrawAhead(draw.state, draw.parameters, draw.shaders, 0, ahead);
    Require(ahead.validated && ahead.laidOut && ahead.pipeline != nullptr, "the preparation of a plain draw left out its validation, vertex layout or pipeline");
    const auto used = Total(DrawAheadEvent::Used);
    const auto device0 = Total(DrawAheadEvent::Device);
    const auto inputs = Total(DrawAheadEvent::PipelineInputs);
    const auto covered = Commit(device, draw, &ahead);
    Require(covered == Width * Height, "the draw with its preparation covered " + std::to_string(covered) + " of " + std::to_string(Width * Height) + " pixels");
    Require(Total(DrawAheadEvent::Used) == used + 1 && Total(DrawAheadEvent::Device) == device0, "a preparation made for the committing device was not used");
    Require(Total(DrawAheadEvent::PipelineInputs) == inputs, "the described set layouts of a plain draw differ from its build's");
}

void PreparationForAnotherDeviceIsDiscarded(AgcDriver::VulkanDevice& device, const Draw& draw) {
    auto replaced = AgcDriver::VulkanDeviceTestAccess::ContextOf(device);
    replaced.bufferPool = std::make_shared<BufferPool>(replaced);
    DrawAhead ahead;
    PrepareDrawAhead(replaced, draw.state, draw.parameters, draw.shaders, 0, ahead);
    const auto used = Total(DrawAheadEvent::Used);
    const auto device0 = Total(DrawAheadEvent::Device);
    const auto covered = Commit(device, draw, &ahead);
    Require(covered == Width * Height, "the draw whose preparation was for another device covered " + std::to_string(covered) + " of " + std::to_string(Width * Height) + " pixels");
    Require(Total(DrawAheadEvent::Device) == device0 + 1 && Total(DrawAheadEvent::Used) == used, "a preparation made for a replaced device was used");
}

void ShapedDrawLeavesItsPipelineToTheShapeMemo(AgcDriver::VulkanDevice& device, const Draw& draw) {
    constexpr std::uint64_t Shape = 0x5a4d'0e5a'4d0e'5a4dull;
    for (int pass = 0; pass < 2; ++pass) {
        DrawAhead ahead;
        const auto memo = Total(DrawAheadEvent::ShapeMemo);
        device.PrepareDrawAhead(draw.state, draw.parameters, draw.shaders, Shape, ahead);
        Require(ahead.validated && ahead.laidOut && ahead.pipeline == nullptr, "the preparation of a shaped draw left out its validation or vertex layout, or made the pipeline the shape memo repeats");
        Require(Total(DrawAheadEvent::ShapeMemo) == memo + 1, "a shaped draw's pipeline left to the shape memo was not counted");
        const auto used = Total(DrawAheadEvent::Used);
        const auto covered = Commit(device, draw, &ahead, Shape);
        Require(covered == Width * Height, "the shaped draw with its preparation covered " + std::to_string(covered) + " of " + std::to_string(Width * Height) + " pixels (pass " + std::to_string(pass) + ")");
        Require(Total(DrawAheadEvent::Used) == used + 1, "a shaped draw's preparation was not used");
    }
}

constexpr std::size_t BlockBytes = 65536;

struct GuestBlock {
    GuestBlock() {
        block = static_cast<std::byte*>(std::aligned_alloc(BlockBytes, BlockBytes));
        Require(block != nullptr, "prepare-ahead: cannot allocate the guest block");
        std::memset(block, 0, BlockBytes);
        std::memcpy(block, Triangles().data(), Triangles().size() * sizeof(Triangles().front()));
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }
    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
        std::free(block);
    }
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    std::byte* block = nullptr;
};

void TemplatesFoundAheadServeTheCommit(AgcDriver::VulkanDevice& device) {
    const GuestBlock guest;
    const auto compiled = Compile(device, guest.block);
    const auto draw = Describe(device, compiled, std::span<std::byte>(guest.block + 4096, Width * Height * 4));
    Commit(device, draw, nullptr);
    Commit(device, draw, nullptr);
    Require(SharedResourceCache().Size() != 0, "the recorded draw over imported memory left no template in the resource cache");
    const char* verifying = std::getenv("APS5_VERIFY_PREPARE_AHEAD");
    const bool verify = verifying != nullptr && std::strcmp(verifying, "1") == 0;
    {
        DrawAhead ahead;
        device.PrepareDrawAhead(draw.state, draw.parameters, draw.shaders, 0, ahead);
        Require(ahead.looked && ahead.found != nullptr, "the preparation did not find the cached template");
        const auto hinted = Total(DrawAheadEvent::TemplateHinted);
        const auto covered = Commit(device, draw, &ahead);
        Require(covered == Width * Height, "the draw on the template found ahead covered " + std::to_string(covered) + " of " + std::to_string(Width * Height) + " pixels");
        Require(Total(DrawAheadEvent::TemplateHinted) == hinted + (verify ? 0u : 1u), "the template found ahead was not used at the commit");
    }
    {
        DrawAhead ahead;
        device.PrepareDrawAhead(draw.state, draw.parameters, draw.shaders, 0, ahead);
        SharedResourceCache().Clear();
        const auto hinted = Total(DrawAheadEvent::TemplateHinted);
        const auto gone = Total(DrawAheadEvent::TemplateGone);
        const auto covered = Commit(device, draw, &ahead);
        Require(covered == Width * Height, "the draw whose template the cache let go covered " + std::to_string(covered) + " of " + std::to_string(Width * Height) + " pixels");
        Require(Total(DrawAheadEvent::TemplateHinted) == hinted && Total(DrawAheadEvent::TemplateGone) == gone + (verify ? 0u : 1u), "a template the cache let go between the preparation and the commit was used");
    }
    {
        DrawAhead ahead;
        device.PrepareDrawAhead(draw.state, draw.parameters, draw.shaders, 0, ahead);
        auto* vertices = reinterpret_cast<float*>(guest.block);
        const std::vector<float> saved(vertices, vertices + Triangles().size() * 4);
        for (std::size_t vertex = 0; vertex < Triangles().size(); ++vertex) vertices[vertex * 4] = -1.0f;
        const auto covered = Commit(device, draw, &ahead);
        std::copy(saved.begin(), saved.end(), vertices);
        Require(covered == 0, "the draw whose vertex buffer was written after its preparation covered " + std::to_string(covered) + " pixels, expected none of the collapsed triangle");
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto compiled = Compile(*device);
        const auto draw = Describe(*device, compiled);
        PreparationForTheDeviceIsUsed(*device, draw);
        PreparationForAnotherDeviceIsDiscarded(*device, draw);
        if (std::getenv("APS5_NO_SHAPE_MEMO") == nullptr) ShapedDrawLeavesItsPipelineToTheShapeMemo(*device, draw);
        TemplatesFoundAheadServeTheCommit(*device);
        Require(Total(DrawAheadEvent::Mismatch) == 0, "a verified preparation differed from the commit's");
        std::puts("prepare-ahead device tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
