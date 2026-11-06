#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 128;
constexpr std::size_t ImageBytes = Width * Height * 4u;
constexpr std::size_t Stride = ImageBytes + 0x10000u;
constexpr std::size_t Images = 256;
constexpr std::size_t BlockBytes = Images * Stride;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Initial = 0x3f800000u;
constexpr std::uint32_t Stored = 0x11111111u;
constexpr std::size_t FarDword = 100u * Width;
constexpr std::size_t Rounds = 2000;

alignas(256) constexpr std::array<std::uint32_t, 11> StoreImageCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Stored, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};

ShaderRecompiler::RecompileResult Compile(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData, const ShaderRecompiler::SpirvTarget& target) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{64u, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {64u, 0, userData, compute, std::nullopt, std::nullopt, memory},
        target,
        {0, 0, 0, 128}
    };
    request.useCache = false;
    return ShaderRecompiler::Recompile(request);
}

std::vector<std::uint32_t> ImageUserData(std::uint64_t base) {
    std::vector<std::uint32_t> userData(12, 0u);
    userData[4] = static_cast<std::uint32_t>(base >> 8u);
    userData[5] = static_cast<std::uint32_t>((base >> 40u) & 0xffu) | (Format32UInt << 20u) | (((Width - 1u) & 3u) << 30u);
    userData[6] = ((Width - 1u) >> 2u) | ((Height - 1u) << 14u);
    userData[7] = 0xfacu | (Type2D << 28u);
    return userData;
}

std::uint8_t* AllocateBlock() {
#ifdef _WIN32
    auto* block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
    if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
    return block;
#else
    constexpr std::uintptr_t alignment = 65536;
    void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) return nullptr;
    const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
    const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
    if (aligned != begin) munmap(mapped, aligned - begin);
    if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
    return reinterpret_cast<std::uint8_t*>(aligned);
#endif
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        auto* block = AllocateBlock();
        Require(block != nullptr, "pending image index: cannot allocate the guest block");
        auto* words = reinterpret_cast<std::uint32_t*>(block);
        std::fill(words, words + BlockBytes / 4u, Initial);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
        const auto address = reinterpret_cast<std::uint64_t>(block);
        AgcDriver::GuestMemory::CollectWritesUncached(address, BlockBytes);
        const auto target = device->Target();
        for (std::size_t image = 0; image < Images; ++image) {
            const auto base = address + image * Stride;
            const auto program = Compile(StoreImageCode, ImageUserData(base), target);
            device->Dispatch(program, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreImageCode.data()));
        }
        std::vector<StorageTexture::PendingQuery> queries;
        for (std::size_t image = 0; image < Images; ++image) queries.push_back({address + image * Stride + 0x1000u, address + image * Stride + 0x2000u, nullptr, nullptr, false});
        std::vector<StorageTexture::PendingQuery> gaps;
        for (std::size_t image = 0; image < Images; ++image) gaps.push_back({address + image * Stride + ImageBytes, address + image * Stride + ImageBytes + 0x1000u, nullptr, nullptr, false});
        const auto started = std::chrono::steady_clock::now();
        std::size_t overlapping = 0, gapsOverlapping = 0;
        for (std::size_t round = 0; round < Rounds; ++round) {
            StorageTexture::ScanPending(queries);
            StorageTexture::ScanPending(gaps);
            overlapping += static_cast<std::size_t>(std::count_if(queries.begin(), queries.end(), [](const auto& query) { return query.overlaps; }));
            gapsOverlapping += static_cast<std::size_t>(std::count_if(gaps.begin(), gaps.end(), [](const auto& query) { return query.overlaps; }));
        }
        const auto scanNs = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started).count() / static_cast<double>(Rounds * Images * 2);
        Require(overlapping == Rounds * Images, "pending image index: a query inside a pending image did not find it");
        Require(gapsOverlapping == 0, "pending image index: a query between pending images found one");
        const auto rangesStarted = std::chrono::steady_clock::now();
        std::size_t found = 0;
        for (std::size_t round = 0; round < Rounds; ++round) {
            for (std::size_t image = 0; image < Images; ++image) {
                const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> range{{{queries[image].begin, queries[image].end}}};
                found += StorageTexture::AnyPendingOverlaps(range) ? 1u : 0u;
            }
        }
        const auto rangeNs = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - rangesStarted).count() / static_cast<double>(Rounds * Images);
        Require(found == Rounds * Images, "pending image index: AnyPendingOverlaps missed a pending image");
        for (std::size_t image = 0; image < Images; image += 2) Require(StorageTexture::FlushPending(address + image * Stride, ImageBytes, nullptr, "test"), "pending image index: an image store left no results pending");
        StorageTexture::FlushAllPending("test");
        device->WaitIdle();
        for (std::size_t image = 0; image < Images; ++image) {
            const auto* imageWords = words + image * Stride / 4u;
            Require(imageWords[0] == Stored && imageWords[FarDword] == Stored, "pending image index: image " + std::to_string(image) + "'s texels were not stored");
            Require(imageWords[ImageBytes / 4u] == Initial, "pending image index: a store reached past image " + std::to_string(image));
        }
        Require(!StorageTexture::AnyPending(), "pending image index: images stayed pending after the stores");
        GuestAllocations::Mutation().Remove(block);
        std::printf("pending image index tests passed: %zu pending images, ScanPending %.0f ns per query, AnyPendingOverlaps %.0f ns per range (%s)\n", Images, scanNs, rangeNs, std::getenv("APS5_NO_LABEL_PENDING_INDEX") != nullptr ? "scan" : "index");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
