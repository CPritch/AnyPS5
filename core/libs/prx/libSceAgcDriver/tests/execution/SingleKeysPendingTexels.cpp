#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DccMetadata.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#ifndef _WIN32
namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}
#endif

namespace {

using AgcDriver::Graphics::DccKeys;
using AgcDriver::Graphics::LookupOutcomes;
using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Side = 128;
constexpr std::uint32_t Format8888Unorm = 56;
constexpr std::uint32_t TileRenderTarget64KB = 0x1b;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t SurfaceBytes = 65536;
constexpr std::size_t KeyBytes = SurfaceBytes / 256;
constexpr std::size_t KeysOffset = SurfaceBytes;
constexpr std::size_t BlockBytes = 2 * 65536;
constexpr std::size_t FilledBlock = 0x2000;
constexpr std::uint32_t FilledColor = 0xa5c3e1f0u;

alignas(256) constexpr std::array<std::uint32_t, 8> WriteCode{
    0x7e020280, 0x7e0402f2, 0x7e0602f2, 0x7e0802f2, 0x7e0a02f2, 0xf0201f08, 0x00010200, 0xbf810000,
};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

class GuestBlock {
public:
    explicit GuestBlock(bool watch) : watched(watch) {
#ifdef _WIN32
        if (watched) {
            block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
            if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
        } else {
            block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
#else
        if (watched) {
            constexpr std::uintptr_t alignment = 65536;
            void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (mapped != MAP_FAILED) {
                const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
                const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
                if (aligned != begin) munmap(mapped, aligned - begin);
                if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
                block = reinterpret_cast<std::uint8_t*>(aligned);
                GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
            }
        } else {
            block = static_cast<std::uint8_t*>(std::aligned_alloc(65536, BlockBytes));
        }
#endif
        Require(block != nullptr, "comp-to-single keys over pending texels: cannot allocate the guest block");
        Require(!watched || AgcDriver::GuestMemory::Watched(AddressOf(block), BlockBytes), "comp-to-single keys over pending texels: the watched guest block is not write-watched");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        if (watched) {
            GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
            GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
        } else {
            VirtualFree(block, 0, MEM_RELEASE);
        }
#else
        if (watched) {
            munmap(block, BlockBytes);
            GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
        } else {
            std::free(block);
        }
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    bool watched;
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 8> TextureDescriptor(const std::uint8_t* texels, const std::uint8_t* keys) {
    const auto address = AddressOf(texels);
    const auto meta = AddressOf(keys) >> 8u;
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format8888Unorm << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (TileRenderTarget64KB << 20u) | (Type2D << 28u),
        0u,
        0u,
        (1u << 20u) | (1u << 21u) | (static_cast<std::uint32_t>(meta & 0xffu) << 24u),
        static_cast<std::uint32_t>(meta >> 8u),
    };
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = AddressOf(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

void Dispatch(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const std::vector<std::uint32_t>& userData, bool wait = true) {
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    if (wait) device.WaitIdle();
}

void RequireKeys(const std::uint8_t* keys, std::uint8_t expected, const std::string& what) {
    const auto found = static_cast<std::size_t>(std::count(keys, keys + KeyBytes, expected));
    char message[256];
    std::snprintf(message, sizeof(message), "%s: %zu of %zu key bytes are 0x%02x (key 0 is 0x%02x)", what.c_str(), found, KeyBytes, expected, keys[0]);
    Require(found == KeyBytes, message);
}

bool FillTexels(AgcDriver::VulkanDevice& device, std::uint8_t* at, std::uint32_t first) {
    const std::array<std::uint32_t, 4> pattern{first, 0x11111111u, 0x22222222u, 0x33333333u};
    return device.FillBuffer(AddressOf(at), 16, pattern);
}

using Counts = std::array<std::uint64_t, LookupOutcomes::Count>;

Counts Outcomes() {
    Counts counts{};
    std::copy(std::begin(AgcDriver::Graphics::ThreadLookupOutcomes().counts), std::end(AgcDriver::Graphics::ThreadLookupOutcomes().counts), counts.begin());
    return counts;
}

void RequireUpload(const Counts& before, LookupOutcomes::Kind expected, const char* what) {
    const auto after = Outcomes();
    const auto made = [&](LookupOutcomes::Kind kind) { return after[kind] - before[kind]; };
    char message[256];
    std::snprintf(message, sizeof(message), "%s: %llu direct, %llu clear and %llu CPU uploads, expected one %s", what, static_cast<unsigned long long>(made(LookupOutcomes::UploadDirect)), static_cast<unsigned long long>(made(LookupOutcomes::UploadClear)), static_cast<unsigned long long>(made(LookupOutcomes::UploadCpu)), LookupOutcomes::Name(expected));
    Require(made(LookupOutcomes::UploadDirect) + made(LookupOutcomes::UploadClear) + made(LookupOutcomes::UploadCpu) == 1 && made(expected) == 1, message);
}

enum class Texels { Uniform, PerBlock };

void RunUpload(AgcDriver::VulkanDevice& device, std::uint8_t* block, Texels texels, bool pendingFill, const char* what) {
    auto* surface = block;
    auto* keys = block + KeysOffset;
    const auto address = AddressOf(surface);
    std::vector<std::uint32_t> firsts(SurfaceBytes / 256);
    for (std::size_t index = 0; index < firsts.size(); ++index) firsts[index] = texels == Texels::Uniform ? 0x40302010u : 0x01000000u + static_cast<std::uint32_t>(index);
    for (std::size_t offset = 0; offset < SurfaceBytes; offset += 4) {
        const std::uint32_t word = offset % 256 == 0 ? firsts[offset / 256] : 0x5a000000u + static_cast<std::uint32_t>(offset);
        std::memcpy(surface + offset, &word, 4);
    }
    std::memset(keys, 0x10, KeyBytes);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto texture = TextureDescriptor(surface, keys);
    std::vector<std::uint32_t> writeData(16, 0u);
    std::copy(texture.begin(), texture.end(), writeData.begin() + 4);
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    auto* recorder = AgcDriver::Graphics::Recorder::Active();
    Require(recorder != nullptr, "comp-to-single keys over pending texels: no active recorder");
    if (pendingFill) {
        if (!FillTexels(device, surface + FilledBlock, FilledColor)) {
            std::printf("%s: the texels have no host import, so a pending write over them is not tested\n", what);
            return;
        }
        firsts[FilledBlock / 256] = FilledColor;
        Require(recorder->PendingWriteOverlaps(address + FilledBlock, 16), "comp-to-single keys over pending texels: the texel fill is not pending");
    }
    const bool tracked = AgcDriver::GuestMemory::Watched(address, SurfaceBytes);
    if (pendingFill && !tracked) std::printf("%s: the texels are compared, not watched, so their upload keeps a copy of them and waits for their writer\n", what);
    const auto before = Outcomes();
    const auto hookWaits = AgcDriver::Graphics::Recorder::ThreadHookWaits();
    Dispatch(device, WriteCode, writeData, false);
    if (pendingFill && tracked) {
        char message[256];
        std::snprintf(message, sizeof(message), "%s: the upload under comp-to-single keys waited for the recorded write of the texels (%llu hook waits)", what, static_cast<unsigned long long>(AgcDriver::Graphics::Recorder::ThreadHookWaits() - hookWaits));
        Require(recorder->PendingWriteOverlaps(address + FilledBlock, 16) && AgcDriver::Graphics::Recorder::ThreadHookWaits() == hookWaits, message);
    }
    RequireUpload(before, texels == Texels::Uniform && !pendingFill ? LookupOutcomes::UploadClear : LookupOutcomes::UploadDirect, what);
    device.WaitIdle();
    const auto image = StorageTexture::FindPending(address, SurfaceBytes);
    Require(image != nullptr && image->UploadedKeys() == DccKeys::Uncompressed, std::string(what) + ": the write left no image pending under uncompressed keys");
    StorageTexture::FlushPending(address, SurfaceBytes, nullptr, "test comp-to-single keys over pending texels");
    device.WaitIdle();
    RequireKeys(keys, 0xff, std::string(what) + ": the keys after the write-back");
    const AgcDriver::Graphics::ColorTargetLayout layout(Side, Side, AgcDriver::Graphics::ColorTileMode::RenderTarget, 4);
    std::size_t wrong = 0;
    std::uint32_t firstWrong = 0, firstExpected = 0;
    for (std::uint32_t y = 0; y < Side; ++y) {
        for (std::uint32_t x = 0; x < Side; ++x) {
            const auto offset = layout.Offset(x, y);
            std::uint32_t texel = 0;
            std::memcpy(&texel, surface + offset, 4);
            const auto expected = y == 0 && x < Threads ? 0xffffffffu : firsts[offset / 256];
            if (texel != expected && wrong++ == 0) {
                firstWrong = texel;
                firstExpected = expected;
            }
        }
    }
    char message[320];
    std::snprintf(message, sizeof(message), "%s: %zu of %u written-back texels are neither the kernel's write nor the first texel of their 256-byte block (first wrong 0x%08x, expected 0x%08x)", what, wrong, Side * Side, firstWrong, firstExpected);
    Require(wrong == 0, message);
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_PROFILE_DRAW", "1");
        _putenv_s("APS5_NO_DRAW_READBACK_WAITS", "1");
#else
        setenv("APS5_PROFILE_DRAW", "1", 1);
        setenv("APS5_NO_DRAW_READBACK_WAITS", "1", 1);
#endif
        std::vector<std::unique_ptr<GuestBlock>> blocks;
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
#ifndef _WIN32
        const auto context = AgcDriver::VulkanDeviceTestAccess::ContextOf(*device);
        const auto decided = AgcDriver::Graphics::PrepareImportWatch(context);
        AgcDriver::Graphics::SetImportWatch(context, AgcDriver::Graphics::ImportWatch::Watch);
        struct Restore {
            const AgcDriver::Graphics::Context& context;
            AgcDriver::Graphics::ImportWatch decided;
            ~Restore() { AgcDriver::Graphics::SetImportWatch(context, decided); }
        } restore{context, decided};
#endif
        for (const bool watched : {false, true}) {
            if (watched && !AgcDriver::GuestMemory::WriteWatched()) {
                std::puts("guest memory has no write watch: a write-watched surface is not tested");
                continue;
            }
            const char* memory = watched ? "write-watched" : "unwatched";
            const auto run = [&](Texels texels, bool pendingFill, const char* what) {
                blocks.push_back(std::make_unique<GuestBlock>(watched));
                const auto name = std::string(what) + " (" + memory + ")";
                RunUpload(*device, blocks.back()->Data(), texels, pendingFill, name.c_str());
            };
            run(Texels::Uniform, true, "uniform comp-to-single texels under a pending write");
            run(Texels::PerBlock, true, "per-block comp-to-single texels under a pending write");
            run(Texels::PerBlock, false, "per-block comp-to-single texels nothing writes");
            run(Texels::Uniform, false, "uniform comp-to-single texels nothing writes");
        }
        std::puts("comp-to-single keys over pending texels tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
