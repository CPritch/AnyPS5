#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/UnitShadow.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#ifdef _WIN32
int main() {
    std::puts("skipped, udmabuf imports are Linux only");
    return VulkanTestSkipped;
}
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace AgcDriver {

struct VulkanDeviceTestAccess {
    static Graphics::Context ContextOf(const VulkanDevice& device) { return device.graphicsContext(); }
};

}

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::size_t BlockBytes = 4u << 20u;
constexpr std::uint32_t Width = 256;
constexpr std::uint32_t Height = 128;
constexpr std::size_t ImageBytes = Width * Height * 4u;
constexpr std::uint32_t Format32UInt = 20;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Initial = 0x3f800000u;
constexpr std::uint32_t Standard256B = 1u;
constexpr std::size_t Frames = 4;
constexpr std::size_t ImageOffset = 1u << 20u;

alignas(256) std::array<std::array<std::uint32_t, 64>, Frames> storeCodes{};

std::uint8_t* mapping = nullptr;
int backing = -1;

bool Resolve(std::uintptr_t address, std::size_t bytes, int* file, std::uint64_t* offset) {
    const auto begin = reinterpret_cast<std::uintptr_t>(mapping);
    if (address < begin || address + bytes > begin + BlockBytes) return false;
    *file = dup(backing);
    *offset = address - begin;
    return *file >= 0;
}

std::uint32_t FrameValue(std::size_t frame) {
    return 0x11110000u + static_cast<std::uint32_t>(frame);
}

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
    userData[7] = 0xfacu | (Standard256B << 20u) | (Type2D << 28u);
    return userData;
}

std::size_t Count(const std::uint32_t* words, std::uint32_t value) {
    return static_cast<std::size_t>(std::count(words, words + ImageBytes / 4u, value));
}

bool DmaBufImport(const AgcDriver::VulkanDevice& device, std::uint64_t address, std::size_t bytes) {
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    const auto* import = AgcDriver::Graphics::HostImportFor(AgcDriver::VulkanDeviceTestAccess::ContextOf(device), address, bytes);
    return import != nullptr && import->dmaBuf;
}

}

int main(int argc, char** argv) {
    try {
        const bool bench = argc > 1 && std::string(argv[1]) == "--bench";
        if (!AgcDriver::Graphics::UnitShadowEnabled()) {
            std::puts("skipped, unit shadows are off");
            return VulkanTestSkipped;
        }
        const int udmabuf = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
        if (udmabuf < 0) {
            std::puts("skipped, /dev/udmabuf is not accessible");
            return VulkanTestSkipped;
        }
        close(udmabuf);
        auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        backing = memfd_create("anyps5-lazy-writeback-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        Require(backing >= 0 && ftruncate(backing, static_cast<off_t>(BlockBytes)) == 0, "lazy write-back: cannot make the backing file");
        Require(fcntl(backing, F_ADD_SEALS, F_SEAL_SHRINK) == 0, "lazy write-back: cannot seal the backing file");
        void* mapped = mmap(nullptr, BlockBytes, PROT_READ | PROT_WRITE, MAP_SHARED, backing, 0);
        Require(mapped != MAP_FAILED, "lazy write-back: cannot map the backing file");
        mapping = static_cast<std::uint8_t*>(mapped);
        Require(reinterpret_cast<std::uintptr_t>(mapping) % 65536u == 0, "lazy write-back: the mapping is not 64 KiB aligned");
        GuestArena::GuestArenaSetSharedBacking_nid_postfix(Resolve);
        auto* words = reinterpret_cast<std::uint32_t*>(mapping);
        std::fill(words, words + BlockBytes / 4u, Initial);
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(mapping, BlockBytes);
        GuestAllocations::Mutation().Add(mapping, BlockBytes, true, true, true);
        const auto address = reinterpret_cast<std::uint64_t>(mapping);
        {
            GuestAllocations::Mapped ranges;
            for (const auto& range : GuestAllocations::GuestAllocationsAcquire_nid_postfix()) {
                if (range->address == address) ranges.push_back(range);
            }
            Require(ranges.size() == 1, "lazy write-back: the allocation is not registered");
            device->ImportGuestMemory(ranges, GuestAllocations::GuestAllocationsGeneration_nid_postfix(), false);
        }
        if (!DmaBufImport(*device, address, BlockBytes)) {
            std::puts("skipped, the device does not import the block as dma-buf memory");
            return VulkanTestSkipped;
        }
        if (!AgcDriver::GuestMemory::Watched(address, BlockBytes)) {
            std::puts("skipped, the imported block is not write-watched");
            return VulkanTestSkipped;
        }
        AgcDriver::GuestMemory::CollectWritesUncached(address, BlockBytes);
        const auto image = address + ImageOffset;
        const auto* imageWords = words + ImageOffset / 4u;
        const bool lazy = AgcDriver::Graphics::LazyWriteBack() && !AgcDriver::Graphics::VerifyLazyWriteBack();
        const auto target = device->Target();
        std::vector<ShaderRecompiler::RecompileResult> programs;
        for (std::size_t frame = 0; frame < Frames; ++frame) {
            auto& code = storeCodes[frame];
            constexpr std::array<std::uint32_t, 11> store{0x7e000280u, 0x7e020280u, 0x7e0402ffu, 0u, 0xf0200108u, 0x00010200u, 0x7e0202ffu, 0x00000064u, 0xf0200108u, 0x00010200u, 0xbf810000u};
            std::copy(store.begin(), store.end(), code.begin());
            code[3] = FrameValue(frame);
            programs.push_back(Compile(std::span<const std::uint32_t>(code.data(), store.size()), ImageUserData(image), target));
        }
        const auto started = std::chrono::steady_clock::now();
        std::size_t iterations = 0;
        for (; bench ? std::chrono::steady_clock::now() - started < std::chrono::seconds(11) : iterations < Frames; ++iterations) {
            const auto frame = iterations % Frames;
            const std::string what = "lazy write-back, frame " + std::to_string(iterations) + ": ";
            device->Dispatch(programs[frame], 1u, 1u, 1u, {}, reinterpret_cast<std::uintptr_t>(storeCodes[frame].data()));
            Require(AgcDriver::Graphics::StorageTexture::FlushPending(image, ImageBytes, nullptr, "stencil plane store"), what + "the plane store found no results pending");
            device->WaitIdle();
            if (!AgcDriver::Graphics::AnyShadowedOverlaps(image, ImageBytes) && iterations == 0 && lazy) {
                std::puts("skipped, the write-back did not retile the image into the unit shadow");
                return VulkanTestSkipped;
            }
            if (lazy) {
                Require(Count(imageWords, FrameValue(frame)) == 0, what + "the plane store published its results into guest memory");
                Require(AgcDriver::Graphics::AnyShadowedOverlaps(image, ImageBytes), what + "the plane store's results are not held by the unit shadow");
            } else {
                Require(Count(imageWords, FrameValue(frame)) == 2, what + "the eager plane store did not publish both texels");
            }
        }
        AgcDriver::GuestMemory::FlushGpuWrites(image, ImageBytes);
        device->WaitIdle();
        const std::string what = "lazy write-back, CPU read after " + std::to_string(iterations) + " frames: ";
        const auto last = (iterations - 1) % Frames;
        Require(Count(imageWords, FrameValue(last)) == 2, what + "the CPU-access hook did not publish the last frame's texels");
        for (std::size_t frame = 0; frame < Frames; ++frame) Require(frame == last || Count(imageWords, FrameValue(frame)) == 0, what + "an earlier frame's texel survived");
        Require(Count(imageWords, Initial) == ImageBytes / 4u - 2u, what + "bytes around the texels changed");
        Require(words[ImageOffset / 4u - 1u] == Initial && words[(ImageOffset + ImageBytes) / 4u] == Initial, what + "the publish reached past the image");
        Require(!AgcDriver::Graphics::AnyShadowedOverlaps(image, ImageBytes), what + "the published units still count as shadowed");
        GuestAllocations::Mutation().Remove(mapping);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(mapping, BlockBytes);
        std::printf("lazy write-back tests passed after %zu frames (%s)\n", iterations, lazy ? "lazy" : AgcDriver::Graphics::VerifyLazyWriteBack() ? "verified" : "eager");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
