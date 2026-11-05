#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
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
#include <span>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;
using DepthFastClearHarness::Compile;
using DepthFastClearHarness::Height;
using DepthFastClearHarness::Shaders;
using DepthFastClearHarness::Triangles;
using DepthFastClearHarness::Width;
using ShaderRecompiler::ShaderStage;

constexpr std::size_t BlockBytes = 65536;
constexpr std::size_t TargetOffset = 4096;
constexpr int Frames = 6;

struct GuestBlock {
    GuestBlock() {
        block = static_cast<std::byte*>(std::aligned_alloc(BlockBytes, BlockBytes));
        Require(block != nullptr, "ahead templates: cannot allocate the guest block");
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

struct Draw {
    std::array<CompiledShader, 2> shaders;
    State state{};
    AgcDriver::Pm4::DrawParameters parameters{0, static_cast<std::uint32_t>(Triangles().size()), 0, 1, 0, false};
    std::span<std::byte> pixels;
};

Draw Describe(AgcDriver::VulkanDevice& device, const Shaders& compiled, std::span<std::byte> pixels) {
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

std::vector<std::byte> Commit(AgcDriver::VulkanDevice& device, const Draw& draw, DrawAhead* ahead) {
    std::fill(draw.pixels.begin(), draw.pixels.end(), std::byte{0});
    device.Draw(draw.state, draw.parameters, draw.shaders, {}, nullptr, nullptr, 0, ahead);
    device.WaitIdle();
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(draw.pixels.data()), draw.pixels.size());
    return {draw.pixels.begin(), draw.pixels.end()};
}

std::size_t Covered(std::span<const std::byte> pixels) {
    std::size_t covered = 0;
    for (std::size_t pixel = 0; pixel < pixels.size() / 4u; ++pixel) {
        if (std::to_integer<std::uint8_t>(pixels[pixel * 4u]) == 255u) ++covered;
    }
    return covered;
}

std::uint64_t Ahead(ShaderResources::AheadEvent event) {
    return ShaderResources::AheadCounts()[static_cast<std::size_t>(event)];
}

bool Flag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
}

void NewTemplatesEveryFrameAreBuiltAhead(AgcDriver::VulkanDevice& device) {
    const GuestBlock guest;
    const auto compiled = Compile(device, guest.block);
    const auto draw = Describe(device, compiled, std::span<std::byte>(guest.block + TargetOffset, Width * Height * 4));
    SharedResourceCache().Clear();
    const auto reference = Commit(device, draw, nullptr);
    Require(Covered(reference) == Width * Height, "the reference draw covered " + std::to_string(Covered(reference)) + " of " + std::to_string(Width * Height) + " pixels");
    const bool off = Flag("APS5_NO_AHEAD_TEMPLATE_BUILD");
    const bool verify = Flag("APS5_VERIFY_AHEAD_TEMPLATE_BUILD");
    std::uint64_t committerBuilds = 0;
    const auto completedBefore = Ahead(ShaderResources::AheadEvent::Completed);
    const auto verifiedBefore = Ahead(ShaderResources::AheadEvent::Verified);
    for (int frame = 0; frame < Frames; ++frame) {
        SharedResourceCache().Clear();
        DrawAhead ahead;
        device.PrepareDrawAhead(draw.state, draw.parameters, draw.shaders, 0, ahead);
        Require(ahead.looked && ahead.found == nullptr, "frame " + std::to_string(frame) + ": the preparation found a template in the emptied cache");
        Require(off == (ahead.built == nullptr), std::string("frame ") + std::to_string(frame) + (off ? ": a template was built ahead with APS5_NO_AHEAD_TEMPLATE_BUILD" : ": the preparation built no template for a draw whose lookup missed"));
        const auto before = CommitterTemplateBuilds();
        const auto pixels = Commit(device, draw, &ahead);
        committerBuilds += CommitterTemplateBuilds() - before;
        Require(pixels == reference, "frame " + std::to_string(frame) + ": the draw on a template built ahead is not pixel-exact with the committer's build (" + std::to_string(Covered(pixels)) + " pixels covered)");
    }
    const auto completed = Ahead(ShaderResources::AheadEvent::Completed) - completedBefore;
    std::printf("ahead templates: %d frames, %llu completed on the committer from builds made ahead, %llu built whole on the committer, %llu verified\n", Frames, static_cast<unsigned long long>(completed), static_cast<unsigned long long>(committerBuilds), static_cast<unsigned long long>(Ahead(ShaderResources::AheadEvent::Verified) - verifiedBefore));
    if (off) Require(committerBuilds == Frames, "with APS5_NO_AHEAD_TEMPLATE_BUILD the committer built " + std::to_string(committerBuilds) + " templates, expected one a frame");
    else if (verify) Require(committerBuilds == Frames && Ahead(ShaderResources::AheadEvent::Verified) - verifiedBefore == Frames, "with APS5_VERIFY_AHEAD_TEMPLATE_BUILD every frame is built on the committer and compared");
    else Require(committerBuilds == 0 && completed == Frames, "the committer built " + std::to_string(committerBuilds) + " templates whole for draws the preparer built ahead");
}

void VerticesWrittenAfterTheBuildAreDrawn(AgcDriver::VulkanDevice& device) {
    const GuestBlock guest;
    const auto compiled = Compile(device, guest.block);
    const auto draw = Describe(device, compiled, std::span<std::byte>(guest.block + TargetOffset, Width * Height * 4));
    SharedResourceCache().Clear();
    DrawAhead ahead;
    device.PrepareDrawAhead(draw.state, draw.parameters, draw.shaders, 0, ahead);
    auto* vertices = reinterpret_cast<float*>(guest.block);
    const std::vector<float> saved(vertices, vertices + Triangles().size() * 4);
    for (std::size_t vertex = 0; vertex < Triangles().size(); ++vertex) vertices[vertex * 4] = -1.0f;
    const auto pixels = Commit(device, draw, &ahead);
    std::copy(saved.begin(), saved.end(), vertices);
    Require(Covered(pixels) == 0, "the draw whose vertex buffer was written after its template was built ahead covered " + std::to_string(Covered(pixels)) + " pixels, expected none of the collapsed triangles");
}

void UnusedBuildIsDropped(AgcDriver::VulkanDevice& device) {
    const GuestBlock guest;
    const auto compiled = Compile(device, guest.block);
    const auto draw = Describe(device, compiled, std::span<std::byte>(guest.block + TargetOffset, Width * Height * 4));
    SharedResourceCache().Clear();
    DrawAhead ahead;
    device.PrepareDrawAhead(draw.state, draw.parameters, draw.shaders, 0, ahead);
    const auto reference = Commit(device, draw, nullptr);
    Commit(device, draw, nullptr);
    Require(SharedResourceCache().Size() != 0, "the draws over imported memory left no template in the resource cache");
    const auto dropped = Ahead(ShaderResources::AheadEvent::Dropped);
    const auto pixels = Commit(device, draw, &ahead);
    Require(pixels == reference, "the draw whose template the cache held by its commit is not pixel-exact");
    if (ahead.looked && !Flag("APS5_NO_AHEAD_TEMPLATE_BUILD") && !Flag("APS5_VERIFY_AHEAD_TEMPLATE_BUILD")) Require(Ahead(ShaderResources::AheadEvent::Dropped) == dropped + 1, "a template built ahead was used although the cache held one at the commit");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        NewTemplatesEveryFrameAreBuiltAhead(*device);
        VerticesWrittenAfterTheBuildAreDrawn(*device);
        UnusedBuildIsDropped(*device);
        Require(DrawAheadTotals()[static_cast<std::size_t>(DrawAheadEvent::Mismatch)] == 0, "a verified preparation differed from the commit's");
        std::puts("ahead template tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ahead template tests failed: " << error.what() << '\n';
        return 1;
    }
}
