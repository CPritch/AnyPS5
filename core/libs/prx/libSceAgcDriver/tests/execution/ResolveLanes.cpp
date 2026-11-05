#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;

alignas(256) constexpr std::array<std::uint32_t, 16> TriangleVertexCode{
    0x7e000d05u, 0x080200f4u, 0x10020300u, 0x100202f4u, 0x060202f3u, 0x060400f3u, 0x10040500u, 0x060404f3u,
    0x7e060280u, 0x7e0802f2u, 0xf80008cfu, 0x04030201u, 0xbf810000u,
};
alignas(256) constexpr std::array<std::uint32_t, 11> PixelCode{0x7e040280u, 0xe0301000u, 0x80010102u, 0xbf8c3f70u, 0x7e100301u, 0x7e120280u, 0x7e140280u, 0x7e160280u, 0xf800180fu, 0x0b0a0908u, 0xbf810000u};
constexpr std::size_t BlockBytes = std::size_t{1} << 16u;
struct Block {
    explicit Block(std::size_t bytes) : bytes(bytes), watched(AgcDriver::GuestMemory::WriteWatched()) {
#ifdef _WIN32
        if (watched) {
            data = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, BlockBytes));
            GuestArena::GuestArenaCommit_nid_postfix(data, bytes, PAGE_READWRITE, bytes);
        } else {
            data = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
#else
        if (watched) {
            void* raw = mmap(nullptr, bytes + BlockBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            Require(raw != MAP_FAILED, "cannot map the color targets");
            const auto begin = reinterpret_cast<std::uintptr_t>(raw);
            const auto aligned = (begin + BlockBytes - 1) & ~(static_cast<std::uintptr_t>(BlockBytes) - 1);
            if (aligned != begin) munmap(raw, aligned - begin);
            if (aligned + bytes != begin + bytes + BlockBytes) munmap(reinterpret_cast<void*>(aligned + bytes), begin + BlockBytes - aligned);
            data = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(data, bytes);
        } else {
            data = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, bytes));
        }
#endif
        Require(data != nullptr, "cannot allocate the color targets");
        Require(!watched || AgcDriver::GuestMemory::Watched(Address(), bytes), "the color targets are not write-watched");
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
        if (watched) {
            GuestArena::GuestArenaReset_nid_postfix(data, bytes);
            GuestArena::GuestArenaRelease_nid_postfix(data, bytes);
        } else {
            VirtualFree(data, 0, MEM_RELEASE);
        }
#else
        if (watched) {
            munmap(data, bytes);
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(data, bytes);
        } else {
            std::free(data);
        }
#endif
    }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
    std::uint64_t Address() const { return reinterpret_cast<std::uintptr_t>(data); }
    std::size_t bytes;
    bool watched;
    std::uint8_t* data = nullptr;
};

std::array<std::uint32_t, 4> RawBuffer(std::uint64_t address, std::uint32_t bytes) {
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}


struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header VertexHeader{};
alignas(8) Header PixelHeader{};

void Registered(Header& header, const std::uint32_t* code, std::size_t bytes, std::uint8_t type) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(bytes);
    header.shader.code = code;
    header.shader.type = type;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

std::vector<std::uint32_t> Setup(std::uint64_t constantAddress, std::uintptr_t color) {
    const auto vertex = reinterpret_cast<std::uintptr_t>(TriangleVertexCode.data());
    const auto pixel = reinterpret_cast<std::uintptr_t>(PixelCode.data());
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> context{
        {0x2d5, 0x2000},
        {0x1b6, 0}, {0x207, 0}, {0x200, 0}, {0x203, 0x800},
        {0x2dc, 0xaa00}, {0x2f8, 0}, {0x292, 2}, {0x293, 0},
        {0x80, 0}, {0x8d, 0}, {0x83, 0xffff}, {0x8c, 0xa},
        {0x2f9, 0x2d}, {0x313, 0x6000}, {0x30e, 0xffffffff}, {0x30f, 0xffffffff},
        {0x206, 0x43f}, {0x204, 0x80000}, {0x205, 0x240},
        {0x8e, 0xf}, {0x8f, 0xf}, {0x202, 0xcc0010},
        {0x1c4, 0}, {0x1c5, 9}, {0x1c3, 4}, {0x31c, 0x28028},
        {0x31b, 0}, {0x31d, 0}, {0x3b0, (63u << 14u) | 3u},
        {0x3b8, 0x9000000}, {0x1e0, 0},
        {0xc, 0}, {0xd, 0x40040},
        {0x81, 0x80000000}, {0x82, 0x40040},
        {0x90, 0x80000000}, {0x91, 0x40040},
        {0x94, 0x80000000}, {0x95, 0x40040},
        {0x318, static_cast<std::uint32_t>(color >> 8u)}, {0x390, static_cast<std::uint32_t>(color >> 40u)},
        {0x10f, std::bit_cast<std::uint32_t>(32.0f)}, {0x110, std::bit_cast<std::uint32_t>(32.0f)},
        {0x111, std::bit_cast<std::uint32_t>(-2.0f)}, {0x112, std::bit_cast<std::uint32_t>(2.0f)},
        {0x113, std::bit_cast<std::uint32_t>(1.0f)}, {0x114, 0},
        {0xb4, 0}, {0xb5, std::bit_cast<std::uint32_t>(1.0f)},
        {0x2fe, 0}, {0x302, 0}, {0x306, 0}, {0x30a, 0},
    };
    const std::vector<std::pair<std::uint32_t, std::uint32_t>> shader{
        {0xc8, static_cast<std::uint32_t>(vertex >> 8u)}, {0xc9, static_cast<std::uint32_t>(vertex >> 40u)}, {0x8b, 0}, {0x8c, 0},
        {0x008, static_cast<std::uint32_t>(pixel >> 8u)}, {0x009, static_cast<std::uint32_t>(pixel >> 40u)}, {0x00b, 8u << 1u},
    };
    std::vector<std::uint32_t> commands;
    Append(commands, 0x79, {0x242, 4});
    for (const auto& [offset, value] : context) Append(commands, 0x69, {offset, value});
    for (const auto& [offset, value] : shader) Append(commands, 0x76, {offset, value});
    const auto descriptor = RawBuffer(constantAddress, 16u);
    for (std::uint32_t word = 0; word < 4; ++word) Append(commands, 0x76, {0x00c + word, 0u});
    for (std::uint32_t word = 0; word < 4; ++word) Append(commands, 0x76, {0x010 + word, descriptor[word]});
    return commands;
}

void WriteConstant(std::vector<std::uint32_t>& commands, std::uint64_t address, float value) {
    commands.insert(commands.end(), {0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), std::bit_cast<std::uint32_t>(value)});
}

unsigned Brightest(const std::uint8_t* color) {
    constexpr std::size_t ColorBytes = 1024;
    StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(color), ColorBytes, nullptr, "test read");
    AgcDriver::GuestMemory::FlushGpuWrites(reinterpret_cast<std::uintptr_t>(color), ColorBytes);
    unsigned brightest = 0;
    for (std::size_t at = 0; at < ColorBytes; ++at) brightest = std::max(brightest, static_cast<unsigned>(color[at]));
    return brightest;
}

}

int main() {
    try {
        bool libraries = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            libraries = device->GraphicsPipelineLibraries();
        }
        if (!libraries && std::getenv("APS5_NO_GPL") == nullptr) return VulkanTestSkipped;
        using AgcDriver::DriverDetail::DrawPipeline;
        using AgcDriver::Graphics::ShaderResources;
        using E = DrawPipeline::EarlyEvent;
        Registered(VertexHeader, TriangleVertexCode.data(), sizeof(TriangleVertexCode), 2);
        Registered(PixelHeader, PixelCode.data(), sizeof(PixelCode), 1);
        Block block(BlockBytes * 4);
        auto* colorMemory = block.data + BlockBytes;
        auto* constant = reinterpret_cast<float*>(block.data);
        for (int round = 0; round < 24; ++round) {
            *constant = 0.25f + static_cast<float>(round % 3) * 0.125f;
            std::memset(colorMemory, 0, 1024);
            auto commands = Setup(block.Address(), reinterpret_cast<std::uintptr_t>(colorMemory));
            for (int draw = 0; draw < 96; ++draw) Append(commands, 0x2d, {3, 2});
            WriteConstant(commands, block.Address(), 0.5f);
            for (int draw = 0; draw < 16; ++draw) Append(commands, 0x2d, {3, 2});
            Packet packet{commands.data(), static_cast<std::uint32_t>(commands.size()), 0, {}};
            Require(sceAgcDriverSubmitDcb(&packet) == 0, "the draw submission failed");
            AgcDriverWaitIdle_nid_postfix();
            const auto brightest = Brightest(colorMemory);
            Require(brightest + 2 >= 128 && brightest <= 130, "round " + std::to_string(round) + ": draws after a store over their constant buffer wrote " + std::to_string(brightest));
        }
        const auto twoAhead = DrawPipeline::EarlyEventCount(E::PostedTwoAhead);
        const auto used = DrawPipeline::EarlyEventCount(E::Used);
        const auto verifyDiffered = DrawPipeline::EarlyEventCount(E::VerifyDiffered);
        const auto taken = ShaderResources::JobBindingsTaken();
        const auto bindingsDiffer = ShaderResources::JobBindingsVerifyDiffers();
        const auto report = ShaderResources::JobBindingsText();
        AgcDriverShutdown_nid_postfix();
        std::cout << report << '\n';
        std::cout << "draws resolved two ahead: posted " << twoAhead << ", packets used " << used << ", job bindings taken " << taken << ", verify differences " << verifyDiffered << " and " << bindingsDiffer << '\n';
        Require(verifyDiffered == 0, "an early packet differed from a resolve at its commit");
        Require(bindingsDiffer == 0, "a job's bindings differed from the emit's");
        if (std::getenv("APS5_DRAW_PIPELINE") != nullptr && std::string(std::getenv("APS5_DRAW_PIPELINE")) != "0") {
            Require(used != 0, "no early packet was used");
            if (DrawPipeline::ResolveLanes() && std::getenv("APS5_NO_RESOLVE_LANES") == nullptr) Require(twoAhead != 0, "no job was posted two ahead");
            const bool jobs = DrawPipeline::ResolveLanes() && ShaderResources::JobBindingsMode() != ShaderResources::JobBindings::Off;
            if (jobs && std::getenv("APS5_VERIFY_RESOLVE_SPLIT") == nullptr) Require(ShaderResources::JobBindingsWithSnapshots() != 0, "no job made bindings with snapshots");
            if (jobs && ShaderResources::JobBindingsMode() == ShaderResources::JobBindings::On && std::getenv("APS5_VERIFY_JOB_BINDINGS") != nullptr && std::getenv("APS5_VERIFY_RESOLVE_SPLIT") == nullptr) Require(ShaderResources::JobBindingsVerified() != 0, "no job's bindings were verified");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
