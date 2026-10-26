#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
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
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Recorder;
using AgcDriver::Graphics::Require;
using AgcDriver::Graphics::StorageTexture;
using ShaderRecompiler::ShaderStage;

constexpr std::size_t BlockBytes = 1u << 20u;
constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 128;
constexpr std::size_t ImageBytes = Width * Height * 4u;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Initial = 0x3f800000u;
constexpr std::uint32_t First = 0x11111111u;
constexpr std::uint32_t Second = 0x22222222u;
constexpr std::uint32_t Foreign = 0x33333333u;
constexpr std::uint32_t Cpu = 0x44444444u;
constexpr std::size_t NearDword = 0x100u / 4u;
constexpr std::size_t FarDword = 100u * Width;
constexpr std::size_t FilledDwords = 16;

alignas(256) constexpr std::array<std::uint32_t, 11> StoreFirstCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, First, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};
alignas(256) constexpr std::array<std::uint32_t, 11> StoreSecondCode{0x7e000280u, 0x7e020280u, 0x7e0402ffu, Second, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};

class GuestBlock {
public:
    explicit GuestBlock(bool watched) : watched(watched) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, BlockBytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, BlockBytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + BlockBytes != begin + BlockBytes + alignment) munmap(reinterpret_cast<void*>(aligned + BlockBytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            if (watched) GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "ordered write-backs: cannot allocate the guest block");
        Require(!watched || AgcDriver::GuestMemory::Watched(Address(), BlockBytes), "ordered write-backs: the guest block is not write-watched");
        std::fill(Words(), Words() + BlockBytes / 4u, Initial);
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
        AgcDriver::GuestMemory::CollectWritesUncached(Address(), BlockBytes);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
        if (watched) GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint32_t* Words() const { return reinterpret_cast<std::uint32_t*>(block); }
    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    bool watched;
    std::uint8_t* block = nullptr;
};

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

struct Notes {
    std::size_t stores = 0;
    std::size_t others = 0;
    std::size_t open = 0;
    std::uint64_t openSerial = 0;
};

Notes NotesOver(std::uint64_t image) {
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto* recorder = Recorder::Active();
    Require(recorder != nullptr, "ordered write-backs: no active recorder");
    Notes notes;
    for (const auto& write : recorder->PendingWritesOver(image, ImageBytes)) {
        ++(write.store != 0 ? notes.stores : notes.others);
        if (write.open) ++notes.open;
    }
    notes.openSerial = recorder->OpenSerial();
    return notes;
}

struct Scene {
    AgcDriver::VulkanDevice& device;
    const std::uint32_t* words;
    std::uint64_t image;
    ShaderRecompiler::RecompileResult first;
    ShaderRecompiler::RecompileResult second;

    Scene(AgcDriver::VulkanDevice& device, const GuestBlock& guest, std::size_t offset) : device(device), words(guest.Words() + offset / 4u), image(guest.Address() + offset), first(Compile(StoreFirstCode, ImageUserData(image), device.Target())), second(Compile(StoreSecondCode, ImageUserData(image), device.Target())) {}

    void StoreFirst(const std::string& what) {
        device.Dispatch(first, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreFirstCode.data()));
        Require(StorageTexture::FlushPending(image, ImageBytes, nullptr, "test"), what + "the first dispatch left no results pending");
        const auto notes = NotesOver(image);
        Require(notes.stores == 1 && notes.others == 0 && notes.open == 1, what + "the first store of the image is not one GPU-direct store in the open batch");
    }

    void WriteSecond() {
        device.Dispatch(second, 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(StoreSecondCode.data()));
    }
};

void RunOrdered(AgcDriver::VulkanDevice& device, const GuestBlock& guest) {
    const std::string what = "ordered write-backs, two stores of one image: ";
    Scene scene(device, guest, 0);
    scene.StoreFirst(what);
    scene.WriteSecond();
    const auto before = NotesOver(scene.image);
    Require(before.stores == 1 && before.others == 0 && before.open == 1, what + "the first store is not alone in the open batch at the second store");
    Require(StorageTexture::FlushPending(scene.image, ImageBytes, nullptr, "test"), what + "the second dispatch left no results pending");
    const auto after = NotesOver(scene.image);
    Require(after.openSerial == before.openSerial, what + "the second store waited for the first, still in the open batch");
    Require(after.stores == 2 && after.others == 0 && after.open == 2, what + "the two stores are not both pending in the open batch");
    device.WaitIdle();
    Require(scene.words[0] == Second && scene.words[FarDword] == Second, what + "the first store landed over the second");
    Require(scene.words[NearDword] == Initial, what + "a texel neither dispatch wrote changed");
}

void RunForeign(AgcDriver::VulkanDevice& device, const GuestBlock& guest) {
    const std::string what = "ordered write-backs, a fill between the stores: ";
    Scene scene(device, guest, BlockBytes / 4u);
    scene.StoreFirst(what);
    const std::array<std::uint32_t, 4> pattern{Foreign, Foreign, Foreign, Foreign};
    {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Require(device.FillBuffer(scene.image + (FarDword + 4u) * 4u, FilledDwords * 4u, pattern), what + "the fill was not recorded");
    }
    scene.WriteSecond();
    const auto before = NotesOver(scene.image);
    Require(before.others != 0 && before.open == before.stores + before.others, what + "the fill is not pending in the open batch at the second store");
    Require(StorageTexture::FlushPending(scene.image, ImageBytes, nullptr, "test"), what + "the second dispatch left no results pending");
    Require(NotesOver(scene.image).openSerial != before.openSerial, what + "the second store did not wait for the fill");
    device.WaitIdle();
    Require(scene.words[0] == Second && scene.words[FarDword] == Second, what + "the second dispatch's texels were not stored");
    for (std::size_t dword = FarDword + 4u; dword < FarDword + 4u + FilledDwords; ++dword) Require(scene.words[dword] == Foreign, what + "the second store put its texels over the newer fill");
}

void RunCpuWrite(AgcDriver::VulkanDevice& device, const GuestBlock& guest) {
    const std::string what = "ordered write-backs, a CPU store before the second store: ";
    Scene scene(device, guest, BlockBytes / 2u);
    scene.StoreFirst(what);
    scene.WriteSecond();
    const auto before = NotesOver(scene.image);
    Require(before.stores == 1 && before.others == 0 && before.open == 1, what + "the first store is not alone in the open batch at the second store");
    volatile auto* target = const_cast<std::uint32_t*>(scene.words) + FarDword + 1u;
    *target = Cpu;
    Require(StorageTexture::FlushPending(scene.image, ImageBytes, nullptr, "test"), what + "the second dispatch left no results pending");
    Require(NotesOver(scene.image).openSerial != before.openSerial, what + "the second store judged the CPU-written block before the first store landed");
    device.WaitIdle();
    Require(scene.words[0] == Second, what + "the second store left a block the CPU did not write");
    Require(scene.words[FarDword + 1u] == Cpu, what + "the second store put its texels over the CPU's store");
    Require(scene.words[FarDword] == First, what + "the block the CPU wrote does not hold the first store's texels");
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_ORDERED_WRITEBACKS", "1");
#else
        setenv("APS5_ORDERED_WRITEBACKS", "1", 1);
#endif
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const bool watched = AgcDriver::GuestMemory::WriteWatched();
        GuestBlock guest(watched);
        RunOrdered(*device, guest);
        RunForeign(*device, guest);
        if (watched) RunCpuWrite(*device, guest);
        else std::puts("guest memory has no write watch: the CPU store case is skipped");
        std::puts("ordered write-back tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
