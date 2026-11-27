#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/RuntimeSwitches.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
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
#include <dlfcn.h>
#include <sys/mman.h>
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 4;
constexpr std::uint32_t TextureBytes = Width * Height * 4;
constexpr std::uint32_t Stride = 48;
constexpr std::uint32_t Records = 48;
constexpr std::uint32_t Moved = 64;
constexpr std::uint32_t Viewed = 32;
constexpr std::uint32_t Lanes = 64;
constexpr std::uint32_t Format32Float = 22;
constexpr std::uint32_t Type2D = 9;
constexpr std::size_t TexturesOffset = 65536;
constexpr std::size_t BlockBytes = TexturesOffset + 2 * 65536;
constexpr std::size_t UnimportableBytes = BlockBytes - 256;
constexpr std::uint64_t CpuWriteResolutions = 1;
constexpr std::uint32_t RetakenEntry = 0;
constexpr std::uint32_t LoneEntry = 30;
constexpr std::uint32_t KeptEntry = 12;
constexpr float RetakenClearDepth = 0.0625f;
constexpr float LoadedDepth = 0.75f;
constexpr std::size_t ChangeLogFlood = 20000;
constexpr std::uint32_t LargeRecords = 4096;
constexpr std::uint32_t LargeViewedRecords = 1536;
constexpr std::size_t RememberedTables = 16;

bool chained = false;

alignas(256) std::array<std::uint32_t, Records> Keys = [] { std::array<std::uint32_t, Records> keys{}; for (std::uint32_t key = 0; key < Records; ++key) keys[key] = key; return keys; }();
alignas(256) std::array<std::uint32_t, Lanes * 4> Output{};
alignas(256) std::array<std::uint32_t, LargeRecords> LargeKeys{};
alignas(256) std::array<std::uint32_t, 32> Srt{};

alignas(256) constexpr std::array<std::uint32_t, 28> TableCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080700u, 0xfa000020u, 0x8f108202u, 0xf4200444u,
    0x20000000u, 0x9312b011u, 0xf42c0802u, 0x24000000u, 0x360200bfu, 0x7e020d01u, 0x060202f0u, 0x100202ffu,
    0x3c800000u, 0x7e0402ffu, 0x3e000000u, 0xf09c8f08u, 0x01090401u, 0x7e120202u, 0x34101286u, 0x4a101100u,
    0x34101084u, 0xe0781000u, 0x80070408u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 7> StoreCode{
    0x7e020d00u, 0x082802f7u, 0x7e3c0300u, 0x7e3e0280u, 0xf0201108u, 0x0001141eu, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 7> RestoreCode{
    0x7e020d00u, 0x082802f6u, 0x7e3c0300u, 0x7e3e0280u, 0xf0201108u, 0x0001141eu, 0xbf810000u,
};

constexpr std::array<std::uint32_t, 4> PointClamp{0x92u, (4u * 256u) << 12u, 0u, 0u};

std::uint64_t AddressOf(const void* data) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
}

std::array<std::uint32_t, 4> Image(const void* texels) {
    const auto address = AddressOf(texels);
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format32Float << 20u) | (((Width - 1u) & 3u) << 30u), ((Width - 1u) >> 2u) | ((Height - 1u) << 14u) | (1u << 31u), 0xfacu | (Type2D << 28u)};
}

std::array<std::uint32_t, 4> Buffer(const void* base, std::uint32_t bytes) {
    const auto address = AddressOf(base);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

class GuestBlock {
public:
    explicit GuestBlock(std::size_t registered = BlockBytes, std::size_t bytes = BlockBytes, std::size_t texturesAt = TexturesOffset) : registered(registered), bytes(bytes), texturesAt(texturesAt) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(GuestArena::GuestArenaAllocate_nid_postfix(bytes, 65536));
        if (block != nullptr) GuestArena::GuestArenaCommit_nid_postfix(block, bytes, PAGE_READWRITE, bytes);
#else
        constexpr std::uintptr_t alignment = 65536;
        void* mapped = mmap(nullptr, bytes + alignment, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapped != MAP_FAILED) {
            const auto begin = reinterpret_cast<std::uintptr_t>(mapped);
            const auto aligned = (begin + alignment - 1) & ~(alignment - 1);
            if (aligned != begin) munmap(mapped, aligned - begin);
            if (aligned + bytes != begin + bytes + alignment) munmap(reinterpret_cast<void*>(aligned + bytes), begin + alignment - aligned);
            block = reinterpret_cast<std::uint8_t*>(aligned);
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
        }
#endif
        Require(block != nullptr, "image table reuse: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, registered, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, bytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, bytes);
#else
        munmap(block, bytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, bytes);
#endif
    }

    std::uint32_t* Table() const { return reinterpret_cast<std::uint32_t*>(block); }
    float* Texels(std::uint32_t texture) const { return reinterpret_cast<float*>(block + texturesAt + static_cast<std::size_t>(texture) * TextureBytes); }

    void Remap() const {
        GuestAllocations::Mutation mutation;
        mutation.Remove(block);
        mutation.Add(block, registered, true, true, true);
    }

private:
    std::size_t registered = 0;
    std::size_t bytes = 0;
    std::size_t texturesAt = 0;
    std::uint8_t* block = nullptr;
};

void SetRecord(const GuestBlock& block, std::uint32_t record, std::uint32_t texture, const GuestBlock* texels = nullptr) {
    auto* words = block.Table() + record * (Stride / 4u);
    std::fill(words, words + Stride / 4u, 0u);
    std::copy(PointClamp.begin(), PointClamp.end(), words);
    const auto image = Image((texels != nullptr ? *texels : block).Texels(texture));
    std::copy(image.begin(), image.end(), words + 4);
}

void Fill(const GuestBlock& block, std::uint32_t texture, float bias) {
    for (std::uint32_t texel = 0; texel < Width * Height; ++texel) block.Texels(texture)[texel] = bias + static_cast<float>(texture) * 100.0f + static_cast<float>(texel);
}

ShaderRecompiler::RecompileResult Compile(AgcDriver::VulkanDevice& device, const GuestBlock& block, const std::uint32_t* srt = Srt.data(), const std::uint32_t* keys = Keys.data(), std::uint32_t records = Records, std::size_t keyCount = Keys.size()) {
    const auto srtAddress = AddressOf(srt);
    const std::array<std::uint32_t, 2> userData{static_cast<std::uint32_t>(srtAddress), static_cast<std::uint32_t>(srtAddress >> 32u)};
    const std::span<const std::uint32_t> code(TableCode);
    const std::span<const std::uint32_t> table(block.Table(), records * Stride / 4u);
    const std::array<ShaderRecompiler::MemoryRegion, 4> memory{{
        {AddressOf(code.data()), std::as_bytes(code)},
        {srtAddress, std::as_bytes(std::span(srt, Srt.size()))},
        {AddressOf(table.data()), std::as_bytes(table)},
        {AddressOf(keys), std::as_bytes(std::span(keys, keyCount))},
    }};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {true, false, false}, false, 1};
    const ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, AddressOf(code.data()), code, 0, {}},
        {64, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    auto result = ShaderRecompiler::Recompile(request);
    const auto binding = std::find_if(result.bindings.begin(), result.bindings.end(), [](const ShaderRecompiler::DescriptorBinding& candidate) { return candidate.role == ShaderRecompiler::DescriptorRole::ImageTable; });
    Require(binding != result.bindings.end() && binding->guestDescriptor.size() == static_cast<std::size_t>(records) * 8u, "image table reuse: the shader does not bind the whole table (" + std::to_string(binding != result.bindings.end() ? binding->guestDescriptor.size() / 8u : 0u) + " records)");
    return result;
}

void SetOutput(const void* output) {
    const auto view = Buffer(output, static_cast<std::uint32_t>(sizeof(Output)));
    std::copy(view.begin(), view.end(), Srt.begin() + 8);
}

void Store(AgcDriver::VulkanDevice& device, const GuestBlock& block, std::uint32_t texture, std::span<const std::uint32_t> code = StoreCode) {
    std::vector<std::uint32_t> userData(16, 0u);
    const auto image = Image(block.Texels(texture));
    std::copy(image.begin(), image.end(), userData.begin() + 4);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{AddressOf(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, AddressOf(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, AddressOf(code.data()));
    device.WaitIdle();
}

std::vector<std::uint32_t> Dispatch(AgcDriver::VulkanDevice& device, const ShaderRecompiler::RecompileResult& result, std::uint32_t record, std::uint32_t* keys = Keys.data()) {
    keys[0] = record;
    std::fill(Output.begin(), Output.end(), 0xdeadbeefu);
    device.Dispatch(result, 1, 1, 1, {}, AddressOf(TableCode.data()));
    device.WaitIdle();
    std::vector<std::uint32_t> red(Lanes);
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) red[lane] = Output[lane * 4u];
    return red;
}

bool EntryWritten(const GuestBlock& block, std::uint32_t texture, std::uint64_t generation) {
    return !AgcDriver::GuestMemory::UnchangedSince(AddressOf(block.Texels(texture)), TextureBytes, generation);
}

std::uint64_t WrittenEntries(const GuestBlock& block, std::uint64_t generation, std::uint32_t records = Records) {
    std::uint64_t written = 0;
    for (std::uint32_t texture = 0; texture < records; ++texture) written += EntryWritten(block, texture, generation) ? 1u : 0u;
    return written;
}

std::vector<std::uint32_t> Row(const float* texels) {
    std::vector<std::uint32_t> row(Lanes);
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) row[lane] = std::bit_cast<std::uint32_t>(texels[lane % Width]);
    return row;
}

struct Step {
    std::uint64_t reused;
    std::uint64_t resolved;
    std::uint64_t partial;
    std::uint64_t partialEntries;
};

Step Since(const AgcDriver::Graphics::ImageTableReuseCounts& before) {
    const auto now = AgcDriver::Graphics::ImageTableReuses();
    return {now.reused - before.reused, now.resolved - before.resolved, now.partial - before.partial, now.partialEntries - before.partialEntries};
}

void ExpectWrites(const AgcDriver::Graphics::ImageTableReuseCounts& before, std::uint64_t kept, std::uint64_t made, const std::string& what) {
    static const bool keptSets = std::getenv("APS5_KEPT_DESCRIPTOR_SETS") == nullptr || std::string(std::getenv("APS5_KEPT_DESCRIPTOR_SETS")) != "0";
    const auto now = AgcDriver::Graphics::ImageTableReuses();
    const auto keptNow = now.writesKept - before.writesKept;
    const auto madeNow = now.writesMade - before.writesMade;
    if (chained) {
        Require(keptNow == 0u && madeNow == 0u, "image table reuse: " + what + ": table descriptors were recorded for a set from the shared pool chain");
        return;
    }
    if (!keptSets) {
        Require(keptNow == 0u && madeNow == kept + made, "image table reuse: " + what + ": table descriptors were kept with kept sets turned off");
        return;
    }
    Require(keptNow == kept && madeNow == made, "image table reuse: " + what + ": " + std::to_string(keptNow) + " table descriptor writes kept and " + std::to_string(madeNow) + " made, expected " + std::to_string(kept) + " and " + std::to_string(made));
}

void ExpectPartly(const AgcDriver::Graphics::ImageTableReuseCounts& before, std::uint64_t reused, std::uint64_t partial, std::uint64_t entries, std::uint64_t resolved, const std::string& what) {
    const auto step = Since(before);
    Require(step.reused == reused && step.partial == partial && step.partialEntries == entries && step.resolved == resolved, "image table reuse: " + what + ": " + std::to_string(step.reused) + " reused, " + std::to_string(step.partial) + " partly resolved again with " + std::to_string(step.partialEntries) + " entries looked up again, and " + std::to_string(step.resolved) + " resolved tables, expected " + std::to_string(reused) + ", " + std::to_string(partial) + " with " + std::to_string(entries) + ", and " + std::to_string(resolved));
}

void Expect(const AgcDriver::Graphics::ImageTableReuseCounts& before, std::uint64_t reused, std::uint64_t resolved, const std::string& what) {
    ExpectPartly(before, reused, 0, 0, resolved, what);
}

std::uint64_t Unreused(const AgcDriver::Graphics::ImageTableReuseCounts& before, AgcDriver::Graphics::ImageTableUnreused why) {
    const auto index = static_cast<std::size_t>(why);
    return AgcDriver::Graphics::ImageTableReuses().unreused[index] - before.unreused[index];
}

std::uint64_t Refusals(const AgcDriver::Graphics::ImageTableReuseCounts& before) {
    const auto now = AgcDriver::Graphics::ImageTableReuses();
    std::uint64_t total = 0;
    for (std::size_t reason = 0; reason < now.unreused.size(); ++reason) total += now.unreused[reason] - before.unreused[reason];
    return total;
}

void ExpectUnreused(const AgcDriver::Graphics::ImageTableReuseCounts& before, AgcDriver::Graphics::ImageTableUnreused why, const std::string& what) {
    const auto total = Refusals(before);
    Require(total == 1u && Unreused(before, why) == 1u, "image table reuse: " + what + ": the table was not refused once for the expected reason (" + std::to_string(Unreused(before, why)) + " of " + std::to_string(total) + " refusals)");
}

void ExpectNoRefusal(const AgcDriver::Graphics::ImageTableReuseCounts& before, const std::string& what) {
    const auto total = Refusals(before);
    Require(total == 0u, "image table reuse: " + what + ": the table was refused " + std::to_string(total) + " times");
}

void AwaitWaitingChange(std::uint64_t since) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
    std::uint64_t current = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (GuestAllocations::GuestAllocationsChangesSince_nid_no_patch(since, spans, current) != GuestAllocations::ChangesAnswer::Waiting) {
        Require(std::chrono::steady_clock::now() < deadline, "image table reuse: a guest mapping change never waited for the lease held over it");
        spans.clear();
        std::this_thread::yield();
    }
}

const void* HostImageBase() {
#ifdef _WIN32
    const auto* inside = reinterpret_cast<const void*>(&GetTickCount);
    HMODULE module = nullptr;
    Require(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(inside), &module) != 0 && module != nullptr, "image table reuse: cannot locate a host image");
    return module;
#else
    Dl_info info{};
    Require(dladdr(reinterpret_cast<const void*>(&std::malloc), &info) != 0 && info.dli_fbase != nullptr, "image table reuse: cannot locate a host image");
    return info.dli_fbase;
#endif
}

std::vector<std::uint32_t> DispatchOnThread(AgcDriver::VulkanDevice& device, const ShaderRecompiler::RecompileResult& result, std::uint32_t record) {
    std::vector<std::uint32_t> sampled;
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            AgcDriver::GuestMemory::BumpCollectEpoch();
            sampled = Dispatch(device, result, record);
        } catch (...) {
            failure = std::current_exception();
        }
    });
    worker.join();
    if (failure != nullptr) std::rethrow_exception(failure);
    return sampled;
}

void RunImageStore(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(block, texture, 0.125f);
        SetRecord(block, texture, texture);
    }
    const auto srt = Buffer(block.Table(), Records * Stride);
    std::copy(srt.begin(), srt.end(), Srt.begin());
    const auto result = Compile(device, block);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, result, 25) == Row(block.Texels(25)), "image table reuse: the table before an image store sampled the wrong entry");
    Require(Dispatch(device, result, 27) == Row(block.Texels(27)), "image table reuse: the table resolved again before an image store sampled the wrong entry");
    Require(Dispatch(device, result, 23) == Row(block.Texels(23)), "image table reuse: the table reused before an image store sampled the wrong entry");
    Store(device, block, 23);
    std::vector<float> stored(Width);
    for (std::uint32_t x = 0; x < Width; ++x) stored[x] = -4.0f - static_cast<float>(x);
    Require(Dispatch(device, result, 23) == Row(stored.data()), "image table reuse: an entry an image store left results pending over kept its old texels");
    Expect(before, 3 - CpuWriteResolutions, CpuWriteResolutions + 1, "CPU writes to a new table's 64 KiB block, then an image store leaving results pending over an entry");
    AgcDriver::Graphics::StorageTexture::FlushPending(AddressOf(block.Texels(23)), TextureBytes, nullptr, "test");
    device.WaitIdle();
    Require(Row(block.Texels(23)) == Row(stored.data()), "image table reuse: the image store wrote the wrong values");
}

bool RunEntryMemo(AgcDriver::VulkanDevice& device, const GuestBlock& block, const GuestBlock& plain) {
    using AgcDriver::Graphics::ShaderResources;
    namespace RuntimeSwitches = AgcDriver::Graphics::RuntimeSwitches;
    using AgcDriver::Graphics::RuntimeSwitch;
    const auto& context = device.graphicsContext();
    auto* srt = block.Table() + 1024u;
    auto* keys = block.Table() + 2048u;
    auto* output = block.Table() + 4096u;
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(plain, texture, 0.375f);
        SetRecord(block, texture, texture, &plain);
    }
    for (std::uint32_t key = 0; key < Records; ++key) keys[key] = key;
    const auto table = Buffer(block.Table(), Records * Stride);
    const auto keyView = Buffer(keys, static_cast<std::uint32_t>(sizeof(Keys)));
    const auto outputView = Buffer(output, static_cast<std::uint32_t>(sizeof(Output)));
    std::copy(table.begin(), table.end(), srt);
    std::copy(keyView.begin(), keyView.end(), srt + 4);
    std::copy(outputView.begin(), outputView.end(), srt + 8);
    const auto program = Compile(device, block, srt, keys);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const AgcDriver::Graphics::CompiledShader shader{ShaderStage::Compute, &program, 0};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    struct Built {
        std::shared_ptr<ShaderResources> resources;
        std::uint64_t reused = 0;
        std::uint64_t resolved = 0;
        std::uint64_t memoHits = 0;
    };
    const auto make = [&](const std::string& what) {
        const auto before = AgcDriver::Graphics::ImageTableReuses();
        Built built{std::make_shared<ShaderResources>(context, shader)};
        const auto after = AgcDriver::Graphics::ImageTableReuses();
        built.reused = after.reused - before.reused;
        built.resolved = after.resolved - before.resolved;
        built.memoHits = after.entryMemoHits - before.entryMemoHits;
        Require(built.resources->SampledTextures().size() == Records, "image table entry memo: " + what + ": the build does not hold one texture per table entry");
        Require(built.reused + built.resolved == 1u, "image table entry memo: " + what + ": the build reused " + std::to_string(built.reused) + " and resolved " + std::to_string(built.resolved) + " tables, not one table");
        return built;
    };
    const auto describe = [](const Built& built) { return std::to_string(built.reused) + " reused, " + std::to_string(built.resolved) + " resolved, " + std::to_string(built.memoHits) + " memo hits"; };
    const auto build = [&](std::uint64_t reused, std::uint64_t memoHits, const std::string& what) {
        auto built = make(what);
        Require(built.reused == reused && built.memoHits == memoHits, "image table entry memo: " + what + ": " + describe(built) + "; expected " + std::to_string(reused) + " reused and " + std::to_string(memoHits) + " memo hits");
        return built.resources;
    };
    const auto settle = [&](const std::string& what) {
        for (std::uint64_t attempt = 0; attempt <= CpuWriteResolutions; ++attempt) {
            auto built = make(what);
            Require(built.memoHits == 0u, "image table entry memo: " + what + ": the memo served a build before any reuse checked the cache by key (" + describe(built) + ")");
            if (built.reused == 1u) return built.resources;
        }
        Require(false, "image table entry memo: " + what + ": the table was not reused within " + std::to_string(CpuWriteResolutions + 1u) + " builds after it was resolved");
        return std::shared_ptr<ShaderResources>{};
    };
    const auto first = build(0, 0, "the first build");
    const auto second = settle("the first reuse, which checks the cache by key");
    const auto third = build(1, 1, "the next build, with no cache entry erased since the first reuse");
    Require(third->SampledTextures() == second->SampledTextures(), "image table entry memo: the build through the memo holds other textures than the reuse that checked the cache by key");
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    const auto cleared = build(0, 0, "the build after the sampled-texture cache was cleared");
    for (std::size_t texture = 0; texture < Records; ++texture) Require(cleared->SampledTextures()[texture] != first->SampledTextures()[texture], "image table entry memo: the build after the cache was cleared kept texture " + std::to_string(texture) + " of the erased entries");
    const auto again = settle("the first reuse after the clear, which checks the cache by key");
    const auto memo = build(1, 1, "the next reuse after the clear");
    Require(memo->SampledTextures() == again->SampledTextures(), "image table entry memo: the memo after the clear holds other textures than the reuse before it");
    RuntimeSwitches::Assign(RuntimeSwitch::NoTableEntryMemo, true);
    const auto control = build(1, 0, "under APS5_NO_TABLE_ENTRY_MEMO");
    RuntimeSwitches::Assign(RuntimeSwitch::NoTableEntryMemo, false);
    Require(control->SampledTextures() == again->SampledTextures(), "image table entry memo: under APS5_NO_TABLE_ENTRY_MEMO the reuse holds other textures");
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    return true;
}

bool RunReusedTableRecords(AgcDriver::VulkanDevice& device, const GuestBlock& block, const GuestBlock& plain) {
    using AgcDriver::Graphics::ShaderResources;
    const bool extended = std::getenv("APS5_NO_EXTENDED_FAST_PROOFS") == nullptr;
    const auto& context = device.graphicsContext();
    if (context.hostImportAlignment == 0 || AgcDriver::Graphics::HostImportFor(context, AddressOf(block.Table()), BlockBytes) == nullptr) {
        Require(std::getenv("ANYPS5_REQUIRE_VULKAN") == nullptr, "image table reuse records: the table block needs a host import");
        std::puts("host imports are unavailable: the records of a reused table are not tested");
        return false;
    }
    auto* srt = block.Table() + 1024u;
    auto* keys = block.Table() + 2048u;
    auto* firstOutput = block.Table() + 4096u;
    auto* secondOutput = firstOutput + Lanes * 4u;
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(plain, texture, 0.625f);
        SetRecord(block, texture, texture, &plain);
    }
    for (std::uint32_t key = 0; key < Records; ++key) keys[key] = key;
    const auto table = Buffer(block.Table(), Records * Stride);
    const auto keyView = Buffer(keys, static_cast<std::uint32_t>(sizeof(Keys)));
    std::copy(table.begin(), table.end(), srt);
    std::copy(keyView.begin(), keyView.end(), srt + 4);
    const auto firstView = Buffer(firstOutput, static_cast<std::uint32_t>(sizeof(Output)));
    std::copy(firstView.begin(), firstView.end(), srt + 8);
    const auto first = Compile(device, block, srt, keys);
    const auto secondView = Buffer(secondOutput, static_cast<std::uint32_t>(sizeof(Output)));
    std::copy(secondView.begin(), secondView.end(), srt + 8);
    const auto second = Compile(device, block, srt, keys);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const AgcDriver::Graphics::CompiledShader firstShader{ShaderStage::Compute, &first, 0};
    const AgcDriver::Graphics::CompiledShader secondShader{ShaderStage::Compute, &second, 0};
    std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
    const auto before = AgcDriver::Graphics::ImageTableReuses();
    auto firstResources = std::make_shared<ShaderResources>(context, firstShader);
    auto secondResources = std::make_shared<ShaderResources>(context, secondShader);
    const auto built = Since(before);
    Require(built.resolved == 1u && built.reused == 1u, "image table reuse records: the second build did not reuse the first one's table (" + std::to_string(built.reused) + " reused and " + std::to_string(built.resolved) + " resolved tables)");
    Require(firstResources->Reusable() && secondResources->Reusable(), "image table reuse records: the builds over the table are not reusable");
    ShaderResources::ProofReport report{};
    Require(secondResources->Revalidate(secondShader, &report), "image table reuse records: the build over the reused table did not prove current");
    if (extended) Require(report.path == ShaderResources::ProofPath::Fast, "image table reuse records: the build over the reused table took the full walk instead of the fast proof");
    else Require(report.path == ShaderResources::ProofPath::Full, "image table reuse records: with the extended fast proofs off the build over the reused table took the fast proof, so the test no longer reaches the case");
    Require(firstResources->Revalidate(firstShader, &report) && report.path == ShaderResources::ProofPath::Fast, "image table reuse records: the build that resolved the table did not take the fast proof");
    secondResources.reset();
    firstResources.reset();
    return true;
}

void RunPendingWriteBack(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    if (!AgcDriver::GuestMemory::WriteWatched()) return;
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(block, texture, 0.875f);
        SetRecord(block, texture, texture);
    }
    const auto srt = Buffer(block.Table(), Records * Stride);
    const auto keys = Buffer(Keys.data(), static_cast<std::uint32_t>(sizeof(Keys)));
    std::copy(srt.begin(), srt.end(), Srt.begin());
    std::copy(keys.begin(), keys.end(), Srt.begin() + 4);
    SetOutput(Output.data());
    const auto result = Compile(device, block);
    SetOutput(block.Texels(13));
    const auto writer = Compile(device, block);
    SetOutput(Output.data());
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto before = AgcDriver::Graphics::ImageTableReuses();
    const auto old = Row(block.Texels(13));
    Require(Dispatch(device, result, 25) == Row(block.Texels(25)), "image table reuse: the table before a write-back was left pending sampled the wrong entry");
    Require(Dispatch(device, result, 13) == old, "image table reuse: the table reused before a write-back was left pending sampled the wrong entry");
    Expect(before, 2 - CpuWriteResolutions, CpuWriteResolutions, "two dispatches of a table before a write-back is left pending");
    Keys[0] = 13;
    const auto unwritten = AgcDriver::GuestMemory::TrackerGeneration();
    device.Dispatch(writer, 1, 1, 1, {}, AddressOf(TableCode.data()));
    const auto& context = device.graphicsContext();
    if (AgcDriver::Graphics::HostImportCovers(context, AddressOf(block.Texels(13)), TextureBytes) || !AgcDriver::Graphics::AnyPendingCpuWrites(context) || Row(block.Texels(13)) != old) {
        device.WaitIdle();
        std::puts("a dispatch's write-back over a table entry was not left pending: a table used over a pending write-back is not tested");
        return;
    }
    const auto sampled = Dispatch(device, result, 13);
    const auto written = Row(block.Texels(13));
    Require(written != old && written[4] == old[1], "image table reuse: the dispatch writing into an entry's texels with its write-back left pending stored the wrong values");
    const auto step = Since(before);
    Require(sampled == written, std::string("image table reuse: a table used while a dispatch's write-back was pending over an entry sampled ") + (sampled == old ? "the entry's old texels" : "neither the entry's old texels nor the written ones") + " (" + std::to_string(step.reused) + " reused, " + std::to_string(step.partial) + " partly resolved again and " + std::to_string(step.resolved) + " resolved tables in the step)");
    const auto writtenEntries = WrittenEntries(block, unwritten);
    Require(writtenEntries != 0u && writtenEntries * 2u <= Records, "image table reuse: a write-back over one entry stamped " + std::to_string(writtenEntries) + " of the table's entries");
    ExpectPartly(before, 3 - CpuWriteResolutions, 1, writtenEntries, CpuWriteResolutions, "a write-back pending over an entry when its table is used again, which looks up the entries it landed over again");
}

void RunViewedStore(AgcDriver::VulkanDevice& device, const GuestBlock& block, const GuestBlock& plain) {
    const auto& context = device.graphicsContext();
    if (context.hostImportAlignment == 0 || AgcDriver::Graphics::HostImportFor(context, AddressOf(block.Table()), BlockBytes) == nullptr) {
        Require(std::getenv("ANYPS5_REQUIRE_VULKAN") == nullptr, "image table reuse: storage-viewed entries require host imports");
        std::puts("host imports are unavailable: a table of storage-viewed entries is not tested");
        return;
    }
    if (!AgcDriver::GuestMemory::WriteWatched()) {
        std::puts("guest memory has no write watch: a table of storage-viewed entries is not tested");
        return;
    }
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        const auto& texels = texture < Viewed ? block : plain;
        Fill(texels, texture, 0.375f);
        SetRecord(block, texture, texture, &texels);
    }
    Require(!AgcDriver::Graphics::HostImportCovers(context, AddressOf(plain.Texels(Viewed)), TextureBytes), "image table reuse: the entries outside the storage-viewed block are host-imported");
    const auto srt = Buffer(block.Table(), Records * Stride);
    std::copy(srt.begin(), srt.end(), Srt.begin());
    const auto result = Compile(device, block);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(Dispatch(device, result, 23) == Row(block.Texels(23)), "image table reuse: a table of storage-viewed entries sampled the wrong entry");
    Store(device, block, 23);
    std::vector<float> stored(Width);
    for (std::uint32_t x = 0; x < Width; ++x) stored[x] = -4.0f - static_cast<float>(x);
    const auto before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, result, 23) == Row(stored.data()), "image table reuse: a table reused over a storage-viewed entry an image store left pending sampled the old texels");
    Store(device, block, 23, RestoreCode);
    std::vector<float> restored(Width);
    for (std::uint32_t x = 0; x < Width; ++x) restored[x] = 4.0f - static_cast<float>(x);
    Require(Dispatch(device, result, 23) == Row(restored.data()), "image table reuse: a reused table kept the old texels of a storage-viewed entry a second image store left pending");
    Require(Dispatch(device, result, 21) == Row(block.Texels(21)), "image table reuse: a reused table of storage-viewed entries sampled the wrong entry");
    Require(Dispatch(device, result, 40) == Row(plain.Texels(40)), "image table reuse: a reused table sampled the wrong entry outside the storage-viewed block");
    Expect(before, 4, 0, "a table of storage-viewed and other entries, image stores leaving results pending over one between dispatches");
    AgcDriver::Graphics::StorageTexture::FlushPending(AddressOf(block.Texels(23)), TextureBytes, nullptr, "test");
    device.WaitIdle();
    Require(Row(block.Texels(23)) == Row(restored.data()), "image table reuse: the image stores over a storage-viewed entry wrote the wrong values");

    for (std::uint32_t texture = Viewed; texture < Records; ++texture) {
        Fill(block, texture, 0.625f);
        SetRecord(block, texture, texture);
    }
    const auto viewedOnly = Compile(device, block);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto kept = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, viewedOnly, 40) == Row(block.Texels(40)), "image table reuse: a table of only storage-viewed entries sampled the wrong entry");
    Require(Dispatch(device, viewedOnly, 41) == Row(block.Texels(41)), "image table reuse: a reused table of only storage-viewed entries sampled the wrong entry");
    Expect(kept, 1, 1, "a table of only storage-viewed entries, whose views are proved current");
    const auto views = AgcDriver::Graphics::ImageTableReuses();
    Require(views.viewedKept - kept.viewedKept == Records && views.viewedLookedUp == kept.viewedLookedUp, "image table reuse: the reuse of a table of only storage-viewed entries looked up " + std::to_string(views.viewedLookedUp - kept.viewedLookedUp) + " entries again and kept " + std::to_string(views.viewedKept - kept.viewedKept) + ", expected none and " + std::to_string(Records));

    const auto other = DispatchOnThread(device, viewedOnly, 42);
    Require(other == Row(block.Texels(42)), "image table reuse: a table of storage-viewed entries reused on another thread sampled the wrong entry");
    const auto across = AgcDriver::Graphics::ImageTableReuses();
    Require(across.viewedKept - views.viewedKept == Records && across.viewedLookedUp == views.viewedLookedUp, "image table reuse: a table of storage-viewed entries reused on another thread did not keep its views");
    Expect(views, 1, 0, "a table of only storage-viewed entries reused on another thread");

    Store(device, block, 43);
    const auto storedCounts = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, viewedOnly, 43) == Row(stored.data()), "image table reuse: a storage-viewed entry an image store left pending kept its old texels");
    Expect(storedCounts, 1, 0, "a table of storage-viewed entries after an image store over one");
    AgcDriver::Graphics::StorageTexture::FlushPending(AddressOf(block.Texels(43)), TextureBytes, nullptr, "test");
    device.WaitIdle();

    Fill(block, 44, 0.875f);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto written = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, viewedOnly, 44) == Row(block.Texels(44)), "image table reuse: a storage-viewed entry the CPU wrote kept the old texels of its storage image");
    const auto refreshed = AgcDriver::Graphics::ImageTableReuses();
    Require(refreshed.viewedLookedUp - written.viewedLookedUp != 0u, "image table reuse: a storage-viewed entry whose memory the CPU wrote since its storage image matched it was not looked up again");
    Expect(written, 1, 0, "a table of storage-viewed entries after a CPU write under one");
    Require(Dispatch(device, viewedOnly, 44) == Row(block.Texels(44)), "image table reuse: the table after its storage-viewed entry was looked up again sampled the wrong entry");

    AgcDriver::Graphics::ReleaseTextureCaches(~std::uint64_t{0});
    const auto released = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, viewedOnly, 45) == Row(block.Texels(45)), "image table reuse: a table of storage-viewed entries after the texture caches were released sampled the wrong entry");
    const auto remade = AgcDriver::Graphics::ImageTableReuses();
    Require(remade.viewedKept == released.viewedKept && remade.viewedLookedUp - released.viewedLookedUp == Records, "image table reuse: a table of storage-viewed entries kept views the released texture caches no longer hold");
    Expect(released, 1, 0, "a table of storage-viewed entries after the texture caches were released");
}

std::array<std::uint32_t, 8> EntryWords(const GuestBlock& block, std::uint32_t texture) {
    std::array<std::uint32_t, 8> words{};
    const auto image = Image(block.Texels(texture));
    std::copy(image.begin(), image.end(), words.begin());
    return words;
}

std::vector<std::uint32_t> FilledRow(float value) {
    const std::vector<float> texels(Width, value);
    return Row(texels.data());
}

ShaderRecompiler::RecompileResult CompileTable(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(block, texture, 0.625f);
        SetRecord(block, texture, texture);
    }
    const auto srt = Buffer(block.Table(), Records * Stride);
    const auto keys = Buffer(Keys.data(), static_cast<std::uint32_t>(sizeof(Keys)));
    std::copy(srt.begin(), srt.end(), Srt.begin());
    std::copy(keys.begin(), keys.end(), Srt.begin() + 4);
    SetOutput(Output.data());
    return Compile(device, block);
}

bool RunRetakenDepthSurface(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    const auto context = device.graphicsContext();
    const auto address = AddressOf(block.Texels(RetakenEntry));
    const auto planeBytes = static_cast<std::size_t>(AgcDriver::Graphics::DepthSliceBytes({Width, Height}, 4u));
    if (!AgcDriver::GuestMemory::WriteWatched() || !AgcDriver::GuestMemory::Watched(address, planeBytes)) {
        std::puts("guest memory has no write watch: a depth surface taken again under a kept image table is not tested");
        return false;
    }
    const auto result = CompileTable(device, block);
    const auto words = EntryWords(block, RetakenEntry);
    const auto resource = AgcDriver::Graphics::DecodeTextureResource(words);
    const AgcDriver::Graphics::DepthTarget target{address, 0, {Width, Height}, VK_FORMAT_D32_SFLOAT, RetakenClearDepth, 0};
    Require(AgcDriver::Graphics::DepthSurfaceView(context, target, false) != VK_NULL_HANDLE, "image table reuse: the depth surface at an entry was not created");
    AgcDriver::Graphics::RetireDepthSurfaces(context.device, address, TextureBytes);

    AgcDriver::GuestMemory::BumpCollectEpoch();
    auto before = AgcDriver::Graphics::ImageTableReuses();
    std::vector<std::uint32_t> remembered;
    for (std::uint64_t dispatch = 0; dispatch <= CpuWriteResolutions; ++dispatch) {
        remembered = Dispatch(device, result, RetakenEntry);
        Require(remembered == Row(block.Texels(RetakenEntry)), "image table reuse: the table over a retired depth surface sampled the wrong entry");
    }
    if (AgcDriver::Graphics::HostImportCovers(context, address, TextureBytes)) {
        std::puts("the entry over the retired depth surface is host-imported: a depth surface taken again under a kept image table is not tested");
        return false;
    }
    Expect(before, 1, CpuWriteResolutions, "a new table's dispatches over a retired depth surface");

    const auto stamp = AgcDriver::GuestMemory::NewestStamp(address, planeBytes);
    const auto serial = AgcDriver::Graphics::DepthSurfacesSerial();
    Require(AgcDriver::Graphics::DepthSurfaceTexture(context, words, resource, {}) == nullptr, "image table reuse: the retired depth surface at an entry serves it before it is taken again");
    Require(AgcDriver::Graphics::DepthSurfaceView(context, target, false) != VK_NULL_HANDLE, "image table reuse: the retired depth surface at an entry was not taken again");
    const auto retakenSerial = AgcDriver::Graphics::DepthSurfacesSerial();
    const auto served = AgcDriver::Graphics::DepthSurfaceTexture(context, words, resource, {});
    Require(served != nullptr && AgcDriver::Graphics::DepthSurfaceHolds(context, resource, served.get()), "image table reuse: the depth surface taken again at an entry does not serve it, so the step tests nothing");
    const auto retakenStamp = AgcDriver::GuestMemory::NewestStamp(address, planeBytes);

    before = AgcDriver::Graphics::ImageTableReuses();
    const auto kept = Dispatch(device, result, RetakenEntry);
    const auto step = Since(before);
    std::printf("a depth surface taken again at a kept table's entry: depth surfaces serial %llu before the take and %llu after, newest stamp over its plane %llu before and %llu after, %llu reused and %llu resolved tables in the dispatch after the take\n", static_cast<unsigned long long>(serial), static_cast<unsigned long long>(retakenSerial), static_cast<unsigned long long>(stamp), static_cast<unsigned long long>(retakenStamp), static_cast<unsigned long long>(step.reused), static_cast<unsigned long long>(step.resolved));

    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto walked = Dispatch(device, result, RetakenEntry);
    Require(walked != remembered, "image table reuse: the walk after the depth surface at an entry was taken again sampled what the kept table holds, so the step tests nothing");
    Require(walked == FilledRow(RetakenClearDepth), "image table reuse: the walk after the depth surface at an entry was taken again did not sample its cleared depth plane");
    Require(kept == walked, std::string("image table reuse: a table kept while the depth surface at an entry was retired sampled ") + (kept == remembered ? "the entry's guest memory" : "neither the entry's guest memory nor the depth plane") + " after the surface was taken again, where a walk samples the depth plane (" + std::to_string(step.reused) + " reused and " + std::to_string(step.resolved) + " resolved tables in that dispatch)");
    return true;
}

bool RunMirroredDepthSurface(AgcDriver::VulkanDevice& device, const GuestBlock& block, bool cleared) {
    const auto context = device.graphicsContext();
    const auto address = AddressOf(block.Texels(RetakenEntry));
    const VkExtent2D extent{Width, Height};
    const auto planeBytes = static_cast<std::size_t>(AgcDriver::Graphics::DepthSliceBytes(extent, 4u));
    const auto htileBytes = static_cast<std::size_t>(AgcDriver::Graphics::HtileMetadataBytes(extent));
    if (!AgcDriver::GuestMemory::WriteWatched() || !AgcDriver::GuestMemory::Watched(address, planeBytes + htileBytes)) {
        std::puts("guest memory has no write watch: a Z64KBX depth surface taken again under a kept image table is not tested");
        return false;
    }
    const auto result = CompileTable(device, block);
    const auto words = EntryWords(block, RetakenEntry);
    const auto resource = AgcDriver::Graphics::DecodeTextureResource(words);
    AgcDriver::Graphics::DepthTarget target{address, 0, extent, VK_FORMAT_D32_SFLOAT, RetakenClearDepth, 0};
    target.htileAddress = address + planeBytes;
    target.depthSwizzle = AgcDriver::Graphics::XorSwizzleMode(AgcDriver::Graphics::TextureTileMode::kZ64KBX);
    Require(AgcDriver::Graphics::DepthSurfaceView(context, target, false) != VK_NULL_HANDLE, "image table reuse: the Z64KBX depth surface at an entry was not created");
    std::fill_n(block.Texels(RetakenEntry), planeBytes / sizeof(float), LoadedDepth);
    Require(Dispatch(device, result, RetakenEntry) == FilledRow(LoadedDepth), "image table reuse: the Z64KBX depth surface at an entry did not load the depth the CPU wrote over its plane");
    const auto loaded = AgcDriver::Graphics::CachedSampledTexture(context, words);
    Require(loaded != nullptr && AgcDriver::Graphics::DepthSurfaceHolds(context, resource, loaded.get()), "image table reuse: the walk that loaded the Z64KBX depth surface's plane at an entry did not bind the surface's own view");
    AgcDriver::Graphics::RetireDepthSurfaces(context.device, address, TextureBytes);

    AgcDriver::GuestMemory::BumpCollectEpoch();
    auto before = AgcDriver::Graphics::ImageTableReuses();
    std::vector<std::uint32_t> remembered;
    for (std::uint64_t dispatch = 0; dispatch <= CpuWriteResolutions; ++dispatch) {
        remembered = Dispatch(device, result, RetakenEntry);
        Require(remembered == FilledRow(LoadedDepth), "image table reuse: the table over a retired Z64KBX depth surface sampled the wrong entry");
    }
    if (AgcDriver::Graphics::HostImportCovers(context, address, TextureBytes)) {
        std::puts("the entry over the retired depth surface is host-imported: a Z64KBX depth surface taken again under a kept image table is not tested");
        return false;
    }
    Expect(before, 1, CpuWriteResolutions, "a new table's dispatches over a retired Z64KBX depth surface");
    const auto snapshot = AgcDriver::Graphics::CachedSampledTexture(context, words);

    const auto stamp = AgcDriver::GuestMemory::NewestStamp(address, planeBytes);
    const auto serial = AgcDriver::Graphics::DepthSurfacesSerial();
    Require(!AgcDriver::Graphics::DepthPlanesStale(), "image table reuse: a depth plane is stale before the Z64KBX depth surface at an entry is taken again");
    Require(AgcDriver::Graphics::DepthSurfaceTexture(context, words, resource, {}) == nullptr, "image table reuse: the retired Z64KBX depth surface at an entry serves it before it is taken again");
    if (cleared) AgcDriver::Graphics::NoteHtileDepthClear(target.htileAddress, htileBytes, htileBytes);
    Require(AgcDriver::Graphics::DepthSurfaceView(context, target, cleared) != VK_NULL_HANDLE, "image table reuse: the retired Z64KBX depth surface at an entry was not taken again");
    const auto retakenSerial = AgcDriver::Graphics::DepthSurfacesSerial();
    Require(AgcDriver::Graphics::DepthPlanesStale() == cleared, cleared ? "image table reuse: the Z64KBX depth surface taken again after an HTILE clear with depth written left its plane current" : "image table reuse: the Z64KBX depth surface taken again with no depth work left its plane stale");
    const auto served = AgcDriver::Graphics::DepthSurfaceTexture(context, words, resource, {});
    Require(served != nullptr && AgcDriver::Graphics::DepthSurfaceHolds(context, resource, served.get()), "image table reuse: the Z64KBX depth surface taken again at an entry does not serve it, so the step tests nothing");
    const auto retakenStamp = AgcDriver::GuestMemory::NewestStamp(address, planeBytes);

    const auto storesBefore = AgcDriver::Graphics::DepthPlaneStoreCounts().stores;
    before = AgcDriver::Graphics::ImageTableReuses();
    const auto kept = Dispatch(device, result, RetakenEntry);
    const auto step = Since(before);
    const auto storesKept = AgcDriver::Graphics::DepthPlaneStoreCounts().stores;
    std::printf("%s at a kept table's entry: depth surfaces serial %llu before the take and %llu after, newest stamp over its plane %llu before and %llu after, %llu reused and %llu resolved tables in the dispatch after the take, depth plane stores %llu before that dispatch and %llu after it\n", cleared ? "a Z64KBX depth surface taken again after an HTILE clear with depth written" : "a Z64KBX depth surface taken again with no depth work", static_cast<unsigned long long>(serial), static_cast<unsigned long long>(retakenSerial), static_cast<unsigned long long>(stamp), static_cast<unsigned long long>(retakenStamp), static_cast<unsigned long long>(step.reused), static_cast<unsigned long long>(step.resolved), static_cast<unsigned long long>(storesBefore), static_cast<unsigned long long>(storesKept));

    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto walked = Dispatch(device, result, RetakenEntry);
    std::printf("depth plane stores after the walk: %llu\n", static_cast<unsigned long long>(AgcDriver::Graphics::DepthPlaneStoreCounts().stores));
    const auto walkedTexture = AgcDriver::Graphics::CachedSampledTexture(context, words);
    Require(walkedTexture != nullptr && walkedTexture != snapshot && AgcDriver::Graphics::DepthSurfaceHolds(context, resource, walkedTexture.get()), "image table reuse: the walk after the Z64KBX depth surface at an entry was taken again did not bind the surface's own view");
    if (!cleared) {
        Require(walked == remembered && kept == walked, "image table reuse: a Z64KBX depth surface whose plane was loaded from memory, taken again with no depth work, sampled other texels than its kept snapshot");
        return true;
    }
    Require(walked != remembered, "image table reuse: the walk after the HTILE-cleared Z64KBX depth surface at an entry was taken again sampled what the kept table holds, so the step tests nothing");
    Require(walked == FilledRow(RetakenClearDepth), "image table reuse: the walk after the Z64KBX depth surface at an entry was taken again did not sample the depth its HTILE clear left");
    Require(kept == walked, std::string("image table reuse: a table kept while the Z64KBX depth surface at an entry was retired sampled ") + (kept == remembered ? "the entry's guest memory" : "neither the entry's guest memory nor the depth plane") + " after the surface was taken again with an HTILE clear, where a walk samples the cleared depth plane (" + std::to_string(step.reused) + " reused and " + std::to_string(step.resolved) + " resolved tables in that dispatch)");
    return true;
}

bool RunRetiredDepthSurface(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    const auto context = device.graphicsContext();
    const auto address = AddressOf(block.Texels(RetakenEntry));
    const VkExtent2D extent{Width, 2u * Height};
    const auto planeBytes = static_cast<std::size_t>(AgcDriver::Graphics::DepthSliceBytes(extent, 4u));
    if (!AgcDriver::GuestMemory::WriteWatched() || !AgcDriver::GuestMemory::Watched(address, planeBytes)) {
        std::puts("guest memory has no write watch: a depth surface of another extent retired under a kept image table is not tested");
        return false;
    }
    const auto result = CompileTable(device, block);
    const auto words = EntryWords(block, RetakenEntry);
    const auto resource = AgcDriver::Graphics::DecodeTextureResource(words);
    const AgcDriver::Graphics::DepthTarget target{address, 0, extent, VK_FORMAT_D32_SFLOAT, RetakenClearDepth, 0};
    Require(AgcDriver::Graphics::DepthSurfaceView(context, target, false) != VK_NULL_HANDLE, "image table reuse: the depth surface of another extent at an entry was not created");

    AgcDriver::GuestMemory::BumpCollectEpoch();
    auto before = AgcDriver::Graphics::ImageTableReuses();
    std::vector<std::uint32_t> remembered;
    for (std::uint64_t dispatch = 0; dispatch <= CpuWriteResolutions; ++dispatch) {
        remembered = Dispatch(device, result, RetakenEntry);
        Require(remembered == Row(block.Texels(RetakenEntry)), "image table reuse: the table over a depth surface of another extent sampled the wrong entry");
    }
    if (AgcDriver::Graphics::HostImportCovers(context, address, TextureBytes)) {
        std::puts("the entry over the depth surface of another extent is host-imported: a depth surface retired under a kept image table is not tested");
        return false;
    }
    Expect(before, 1, CpuWriteResolutions, "a new table's dispatches over a depth surface of another extent");
    const auto snapshot = AgcDriver::Graphics::CachedSampledTexture(context, words);

    const auto stamp = AgcDriver::GuestMemory::NewestStamp(address, planeBytes);
    const auto serial = AgcDriver::Graphics::DepthSurfacesSerial();
    Require(AgcDriver::Graphics::DepthSurfaceAt(address) && AgcDriver::Graphics::DepthSurfaceTexture(context, words, resource, {}) == nullptr, "image table reuse: the depth surface of another extent at an entry is not live, or serves the entry, before its retire");
    AgcDriver::Graphics::RetireDepthSurfaces(context.device, address, TextureBytes);
    const auto retiredSerial = AgcDriver::Graphics::DepthSurfacesSerial();
    Require(!AgcDriver::Graphics::DepthSurfaceAt(address) && AgcDriver::Graphics::DepthSurfaceTexture(context, words, resource, {}) == nullptr, "image table reuse: the depth surface of another extent at an entry is still live, or serves the entry, after its retire");
    const auto retiredStamp = AgcDriver::GuestMemory::NewestStamp(address, planeBytes);

    before = AgcDriver::Graphics::ImageTableReuses();
    const auto kept = Dispatch(device, result, RetakenEntry);
    const auto step = Since(before);
    std::printf("a depth surface of another extent retired at a kept table's entry: depth surfaces serial %llu before the retire and %llu after, newest stamp over its plane %llu before and %llu after, %llu reused and %llu resolved tables in the dispatch after the retire\n", static_cast<unsigned long long>(serial), static_cast<unsigned long long>(retiredSerial), static_cast<unsigned long long>(stamp), static_cast<unsigned long long>(retiredStamp), static_cast<unsigned long long>(step.reused), static_cast<unsigned long long>(step.resolved));

    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto walked = Dispatch(device, result, RetakenEntry);
    Require(kept == remembered && walked == remembered, "image table reuse: a table kept while a depth surface of another extent was live at an entry sampled other texels after the surface was retired");
    Require(AgcDriver::Graphics::CachedSampledTexture(context, words) == snapshot, "image table reuse: the walk after the depth surface of another extent at an entry was retired made another texture object for it");
    return true;
}

bool RunAcrossThreads(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    using AgcDriver::Graphics::ImageTableUnreused;
    if (!AgcDriver::GuestMemory::WriteWatched()) {
        std::puts("guest memory has no write watch: image table reuse across threads is not tested");
        return false;
    }
    const auto result = CompileTable(device, block);
    Fill(block, Moved, 0.5f);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    auto before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, result, 5) == Row(block.Texels(5)), "image table reuse across threads: the first dispatch sampled the wrong entry");
    Require(AgcDriver::Graphics::ImageTableReuses().remembered - before.remembered == 1u, "image table reuse across threads: the table resolved on the first thread was not remembered");
    Require(DispatchOnThread(device, result, 9) == Row(block.Texels(9)), "image table reuse across threads: a second thread reusing the table sampled the wrong entry");
    Expect(before, 1, 1, "a table resolved on one thread and dispatched on another, in that thread's own collect epoch");
    Require(DispatchOnThread(device, result, 11) == Row(block.Texels(11)), "image table reuse across threads: a third thread reusing the table sampled the wrong entry");
    Require(Dispatch(device, result, 13) == Row(block.Texels(13)), "image table reuse across threads: the first thread reusing the table after the others sampled the wrong entry");
    Expect(before, 3, 1, "a table dispatched on three threads");

    SetRecord(block, 9, Moved);
    const auto moved = Compile(device, block);
    before = AgcDriver::Graphics::ImageTableReuses();
    Require(DispatchOnThread(device, moved, 9) == Row(block.Texels(Moved)), "image table reuse across threads: a record pointed at another texture sampled the old one on another thread");
    Expect(before, 0, 1, "a table whose record's words changed, dispatched on another thread");
    ExpectUnreused(before, ImageTableUnreused::NotRemembered, "a table whose record's words changed");
    before = AgcDriver::Graphics::ImageTableReuses();
    Require(DispatchOnThread(device, result, 9) == Row(block.Texels(9)), "image table reuse across threads: the table with the record's old words sampled the moved texture");
    Require(DispatchOnThread(device, moved, 9) == Row(block.Texels(Moved)), "image table reuse across threads: the table with the record's new words sampled the wrong entry when reused");
    Expect(before, 2, 0, "the tables with a record's old and new words, each reused on another thread");

    const auto unwritten = AgcDriver::GuestMemory::TrackerGeneration();
    Fill(block, 7, 0.875f);
    before = AgcDriver::Graphics::ImageTableReuses();
    Require(DispatchOnThread(device, result, 7) == Row(block.Texels(7)), "image table reuse across threads: an entry the CPU wrote kept its old texels on another thread");
    const auto writtenEntries = WrittenEntries(block, unwritten);
    Require(writtenEntries != 0u && writtenEntries * 2u <= Records, "image table reuse across threads: a CPU write over one entry stamped " + std::to_string(writtenEntries) + " of the table's entries");
    ExpectPartly(before, 0, 1, writtenEntries, 0, "a CPU write under an entry, then a dispatch on another thread, which looks up the written entries again");
    ExpectNoRefusal(before, "a CPU write under an entry");

    before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, result, 15) == Row(block.Texels(15)), "image table reuse across threads: the table resolved after a CPU write sampled the wrong entry");
    {
        const GuestBlock unrelated(UnimportableBytes);
        Fill(unrelated, 0, 0.0f);
    }
    Require(Dispatch(device, result, 17) == Row(block.Texels(17)), "image table reuse across threads: the table after an unrelated mapping change sampled the wrong entry");
    Expect(before, 2, 0, "a guest mapping changed away from every entry");

    block.Remap();
    before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, result, 19) == Row(block.Texels(19)), "image table reuse across threads: the table after its entries' mapping changed sampled the wrong entry");
    Expect(before, 0, 1, "the guest mapping under the entries changed");
    ExpectUnreused(before, ImageTableUnreused::MappingOverHalf, "the guest mapping under the entries changed");
    const auto largest = AgcDriver::Graphics::ImageTableReuses();
    Require(largest.largestMappingAffected == Records && largest.largestMappingEntries == Records, "image table reuse across threads: the largest refusal under a mapping change reports " + std::to_string(largest.largestMappingAffected) + " of " + std::to_string(largest.largestMappingEntries) + " entries, expected every entry of the table");
    Require(DispatchOnThread(device, result, 21) == Row(block.Texels(21)), "image table reuse across threads: the table remembered after a mapping change sampled the wrong entry on another thread");
    Expect(before, 1, 1, "the table remembered again after its mapping changed, reused on another thread");
    return true;
}

bool RunPartialResolve(AgcDriver::VulkanDevice& device, const GuestBlock& block, const GuestBlock& lone, const GuestBlock& importable) {
    using AgcDriver::Graphics::ImageTableUnreused;
    using AgcDriver::Graphics::ShaderResources;
    namespace RuntimeSwitches = AgcDriver::Graphics::RuntimeSwitches;
    using AgcDriver::Graphics::RuntimeSwitch;
    if (!AgcDriver::GuestMemory::WriteWatched()) {
        std::puts("guest memory has no write watch: partly resolving a remembered image table again is not tested");
        return false;
    }
    const auto& context = device.graphicsContext();
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(block, texture, 0.25f);
        SetRecord(block, texture, texture);
    }
    Fill(block, Moved, 0.5f);
    Fill(lone, 0, 0.375f);
    SetRecord(block, LoneEntry, 0, &lone);
    const auto srt = Buffer(block.Table(), Records * Stride);
    const auto keys = Buffer(Keys.data(), static_cast<std::uint32_t>(sizeof(Keys)));
    std::copy(srt.begin(), srt.end(), Srt.begin());
    std::copy(keys.begin(), keys.end(), Srt.begin() + 4);
    SetOutput(Output.data());
    const auto program = Compile(device, block);
    const AgcDriver::Graphics::CompiledShader shader{ShaderStage::Compute, &program, 0};
    using Textures = std::vector<std::shared_ptr<AgcDriver::Graphics::Texture>>;
    struct Built {
        Textures textures;
        Step step;
        std::uint64_t refusals = 0;
        AgcDriver::Graphics::ImageTableReuseCounts before;
    };
    const auto build = [&](const AgcDriver::Graphics::CompiledShader& compiled) {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Built built;
        built.before = AgcDriver::Graphics::ImageTableReuses();
        const auto resources = std::make_shared<ShaderResources>(context, compiled);
        built.textures = resources->SampledTextures();
        built.step = Since(built.before);
        built.refusals = Refusals(built.before);
        return built;
    };
    const auto resolveFully = [&](const AgcDriver::Graphics::CompiledShader& compiled, const std::string& what) {
        Built built;
        std::exception_ptr failure;
        std::thread worker([&] {
            try {
                built = build(compiled);
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure != nullptr) std::rethrow_exception(failure);
        Require(built.step.resolved == 1u && built.step.reused == 0u && built.step.partial == 0u && Unreused(built.before, ImageTableUnreused::NoEpoch) == 1u, "image table partial resolve: " + what + ": the build on a thread without a collect epoch did not resolve the whole table");
        return built.textures;
    };
    const auto describe = [](const Step& step) { return std::to_string(step.reused) + " reused, " + std::to_string(step.partial) + " partly resolved again with " + std::to_string(step.partialEntries) + " entries looked up again, " + std::to_string(step.resolved) + " resolved"; };
    const auto partly = [&](const AgcDriver::Graphics::CompiledShader& compiled, std::uint64_t entries, const std::string& what) {
        const auto built = build(compiled);
        Require(built.step.reused == 0u && built.step.partial == 1u && built.step.partialEntries == entries && built.step.resolved == 0u, "image table partial resolve: " + what + ": " + describe(built.step) + "; expected one table partly resolved again with " + std::to_string(entries) + " entries looked up again");
        Require(built.refusals == 0u, "image table partial resolve: " + what + ": the table partly resolved again was also counted as refused");
        Require(built.textures == resolveFully(compiled, what), "image table partial resolve: " + what + ": the table partly resolved again binds other textures than a whole resolve of it");
        const auto reused = build(compiled);
        Require(reused.step.reused == 1u && reused.step.partial == 0u && reused.step.resolved == 0u && reused.textures == built.textures, "image table partial resolve: " + what + ": the table remembered after it was partly resolved again was not reused whole (" + describe(reused.step) + ")");
        return built.textures;
    };
    const auto keptExcept = [&](const Textures& before, const Textures& after, std::uint32_t changed, const std::string& what) {
        Require(before.size() == Records && after.size() == Records, "image table partial resolve: " + what + ": the builds do not hold one texture per entry");
        for (std::uint32_t entry = 0; entry < Records; ++entry) {
            if (entry != changed) Require(after[entry] == before[entry], "image table partial resolve: " + what + ": entry " + std::to_string(entry) + " outside the change was bound to another texture");
        }
    };
    const auto sampledBy = [&](const ShaderRecompiler::RecompileResult& compiled, const std::string& what, const float* lonely) {
        Require(Dispatch(device, compiled, LoneEntry) == Row(lonely), "image table partial resolve: " + what + ": the entry looked up again sampled the wrong texels");
        Require(Dispatch(device, compiled, KeptEntry) == Row(block.Texels(KeptEntry)), "image table partial resolve: " + what + ": an entry kept from the remembered table sampled the wrong texels");
    };
    const auto sampled = [&](const std::string& what, const float* lonely) { sampledBy(program, what, lonely); };
    const auto settle = [&](const AgcDriver::Graphics::CompiledShader& compiled, const std::string& what) {
        AgcDriver::GuestMemory::BumpCollectEpoch();
        for (std::uint64_t attempt = 0; attempt <= CpuWriteResolutions + 1u; ++attempt) {
            const auto built = build(compiled);
            if (built.step.reused == 1u) return built;
        }
        Require(false, "image table partial resolve: " + what + ": the table was not reused whole after it was resolved");
        return Built{};
    };

    const auto baseline = settle(shader, "a table of one block and one lone entry").textures;
    sampled("the table before any change", lone.Texels(0));

    lone.Remap();
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto remapped = partly(shader, 1, "the guest mapping changed under one entry");
    keptExcept(baseline, remapped, LoneEntry, "the guest mapping changed under one entry");
    sampled("the guest mapping changed under one entry", lone.Texels(0));

    Fill(lone, 0, 0.875f);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto written = partly(shader, 1, "the CPU wrote under one entry");
    keptExcept(remapped, written, LoneEntry, "the CPU wrote under one entry");
    Require(written[LoneEntry] != remapped[LoneEntry], "image table partial resolve: the entry the CPU wrote under kept its texture");
    sampled("the CPU wrote under one entry", lone.Texels(0));

    Fill(lone, 0, 0.125f);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    RuntimeSwitches::Assign(RuntimeSwitch::NoPartialTableResolve, true);
    const auto control = build(shader);
    RuntimeSwitches::Assign(RuntimeSwitch::NoPartialTableResolve, false);
    Require(control.step.resolved == 1u && control.step.partial == 0u, "image table partial resolve: under APS5_NO_PARTIAL_TABLE_RESOLVE a CPU write under one entry did not resolve the whole table (" + describe(control.step) + ")");
    ExpectUnreused(control.before, ImageTableUnreused::Changed, "a CPU write under one entry under APS5_NO_PARTIAL_TABLE_RESOLVE");
    keptExcept(written, control.textures, LoneEntry, "a CPU write under one entry under APS5_NO_PARTIAL_TABLE_RESOLVE");
    sampled("a CPU write under one entry under APS5_NO_PARTIAL_TABLE_RESOLVE", lone.Texels(0));

    const auto unwritten = AgcDriver::GuestMemory::TrackerGeneration();
    for (std::uint32_t texture = 0; texture <= Records / 2u; ++texture) Fill(block, texture, 0.625f);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto wide = build(shader);
    Require(WrittenEntries(block, unwritten) * 2u > Records, "image table partial resolve: the CPU writes over more than half the entries stamped only " + std::to_string(WrittenEntries(block, unwritten)) + " of them");
    Require(wide.step.resolved == 1u && wide.step.partial == 0u && wide.step.reused == 0u, "image table partial resolve: CPU writes over more than half the entries did not resolve the whole table (" + describe(wide.step) + ")");
    ExpectUnreused(wide.before, ImageTableUnreused::Changed, "CPU writes over more than half the entries");
    Require(wide.textures == resolveFully(shader, "CPU writes over more than half the entries"), "image table partial resolve: the whole resolve after CPU writes over more than half the entries binds other textures than another whole resolve");
    Require(Dispatch(device, program, 3) == Row(block.Texels(3)), "image table partial resolve: an entry the CPU wrote with more than half the others sampled the wrong texels");
    sampled("CPU writes over more than half the entries", lone.Texels(0));

    SetRecord(block, LoneEntry + 1u, Moved);
    const auto movedProgram = Compile(device, block);
    const AgcDriver::Graphics::CompiledShader movedShader{ShaderStage::Compute, &movedProgram, 0};
    const auto moved = build(movedShader);
    Require(moved.step.resolved == 1u && moved.step.partial == 0u && moved.step.reused == 0u, "image table partial resolve: a table whose record was pointed at another texture was not resolved whole (" + describe(moved.step) + ")");
    ExpectUnreused(moved.before, ImageTableUnreused::NotRemembered, "a table whose record was pointed at another texture");
    Require(Dispatch(device, movedProgram, LoneEntry + 1u) == Row(block.Texels(Moved)), "image table partial resolve: a record pointed at another texture sampled the old one");

    static_cast<void>(settle(shader, "a table before the block of most entries was remapped"));
    block.Remap();
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto remappedWide = build(shader);
    Require(remappedWide.step.resolved == 1u && remappedWide.step.partial == 0u && remappedWide.step.reused == 0u, "image table partial resolve: a guest mapping change under more than half the entries did not resolve the whole table (" + describe(remappedWide.step) + ")");
    ExpectUnreused(remappedWide.before, ImageTableUnreused::MappingOverHalf, "a guest mapping change under more than half the entries");
    const auto largest = AgcDriver::Graphics::ImageTableReuses();
    Require(largest.largestMappingAffected == Records - 1u && largest.largestMappingEntries == Records, "image table partial resolve: the largest refusal under a mapping change reports " + std::to_string(largest.largestMappingAffected) + " of " + std::to_string(largest.largestMappingEntries) + " entries, expected " + std::to_string(Records - 1u) + " of " + std::to_string(Records));
    Require(remappedWide.textures == resolveFully(shader, "a guest mapping change under more than half the entries"), "image table partial resolve: the whole resolve after a guest mapping change under more than half the entries binds other textures than another whole resolve");
    sampled("a guest mapping change under more than half the entries", lone.Texels(0));

    const auto beforeFlood = settle(shader, "a table before the guest mapping change log overflowed");
    {
        const GuestBlock unrelated(UnimportableBytes);
        for (std::size_t change = 0; change < ChangeLogFlood; ++change) GuestAllocations::GuestAllocationsInvalidate_nid_postfix(AddressOf(unrelated.Texels(0)), 16);
    }
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto flooded = build(shader);
    Require(flooded.step.resolved == 1u && flooded.step.partial == 0u && flooded.step.reused == 0u, "image table partial resolve: a table proved before the guest mapping change log overflowed was not resolved whole (" + describe(flooded.step) + ")");
    ExpectUnreused(flooded.before, ImageTableUnreused::MappingDropped, "a table proved before the guest mapping change log overflowed");
    Require(flooded.textures == beforeFlood.textures && flooded.textures == resolveFully(shader, "a table proved before the guest mapping change log overflowed"), "image table partial resolve: the whole resolve after the guest mapping change log overflowed binds other textures than the unchanged table");
    const auto afterFlood = build(shader);
    Require(afterFlood.step.reused == 1u && afterFlood.refusals == 0u && afterFlood.textures == flooded.textures, "image table partial resolve: the table resolved after the guest mapping change log overflowed was not reused (" + describe(afterFlood.step) + ")");

    const auto* image = HostImageBase();
    const auto beforeImage = settle(shader, "a table before a host image was registered");
    {
        GuestAllocations::Mutation mutation;
        mutation.RegisterImage(image);
    }
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto registered = build(shader);
    Require(registered.step.resolved == 1u && registered.step.partial == 0u && registered.step.reused == 0u, "image table partial resolve: a table proved before an image registration the change log does not record was not resolved whole (" + describe(registered.step) + ")");
    ExpectUnreused(registered.before, ImageTableUnreused::MappingUntracked, "a table proved before an image registration the change log does not record");
    Require(registered.textures == beforeImage.textures, "image table partial resolve: the whole resolve after an image registration away from every entry binds other textures");
    {
        GuestAllocations::Mutation mutation;
        mutation.RegisterImage(image);
    }
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto repeated = build(shader);
    {
        GuestAllocations::Mutation mutation;
        mutation.UnregisterImage(image);
    }
    Require(repeated.step.reused == 1u && repeated.step.partial == 0u && repeated.step.resolved == 0u && repeated.refusals == 0u && repeated.textures == registered.textures, "image table partial resolve: registering a registered image again refused the table (" + describe(repeated.step) + ")");

    const auto beforeWait = settle(shader, "a table before a guest mapping change elsewhere waited for a lease");
    {
        std::optional<GuestBlock> retired(std::in_place, UnimportableBytes);
        auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
        const auto since = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        std::thread remover([&] { retired.reset(); });
        Built waited;
        std::uint64_t proofs = 0;
        std::exception_ptr failure;
        try {
            AwaitWaitingChange(since);
            AgcDriver::GuestMemory::BumpCollectEpoch();
            waited = build(shader);
            proofs = AgcDriver::Graphics::ImageTableReuses().mappingWaited - waited.before.mappingWaited;
        } catch (...) {
            failure = std::current_exception();
        }
        lease.clear();
        remover.join();
        if (failure != nullptr) std::rethrow_exception(failure);
        Require(waited.step.reused == 1u && waited.step.partial == 0u && waited.step.resolved == 0u && waited.refusals == 0u && proofs == 1u && waited.textures == beforeWait.textures, "image table partial resolve: a guest mapping change away from every entry, waiting for a lease, refused the table (" + describe(waited.step) + ", " + std::to_string(proofs) + " proofs made while it waited)");
    }
    const auto afterWait = build(shader);
    Require(afterWait.step.reused == 1u && afterWait.refusals == 0u && afterWait.textures == beforeWait.textures, "image table partial resolve: the table was not reused after a guest mapping change elsewhere finished waiting for its lease (" + describe(afterWait.step) + ")");
    {
        {
            const GuestBlock unrelated(UnimportableBytes);
        }
        auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
        const auto since = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
        std::thread remapper([&] { lone.Remap(); });
        Built under;
        std::exception_ptr failure;
        try {
            AwaitWaitingChange(since);
            AgcDriver::GuestMemory::BumpCollectEpoch();
            under = build(shader);
        } catch (...) {
            failure = std::current_exception();
        }
        lease.clear();
        remapper.join();
        if (failure != nullptr) std::rethrow_exception(failure);
        Require(under.step.reused == 0u && under.step.partial == 1u && under.step.partialEntries == 1u && under.step.resolved == 0u && under.refusals == 0u, "image table partial resolve: a guest mapping change under one entry, waiting for a lease, did not look that entry up again alone (" + describe(under.step) + ")");
    }
    const auto unwaited = settle(shader, "a table after a guest mapping change under one entry finished waiting for its lease");
    keptExcept(beforeWait.textures, unwaited.textures, LoneEntry, "a guest mapping change under one entry that waited for a lease");
    sampled("a guest mapping change under one entry that waited for a lease", lone.Texels(0));

    if (context.hostImportAlignment == 0 || AgcDriver::Graphics::HostImportFor(context, AddressOf(importable.Table()), BlockBytes) == nullptr) {
        Require(std::getenv("ANYPS5_REQUIRE_VULKAN") == nullptr, "image table partial resolve: a storage-viewed entry requires host imports");
        std::puts("host imports are unavailable: partly resolving a table again over a storage-viewed entry is not tested");
        return true;
    }
    SetRecord(block, LoneEntry + 1u, LoneEntry + 1u);
    Fill(importable, 0, 0.75f);
    SetRecord(block, LoneEntry, 0, &importable);
    const auto viewProgram = Compile(device, block);
    const AgcDriver::Graphics::CompiledShader viewShader{ShaderStage::Compute, &viewProgram, 0};
    const auto views = settle(viewShader, "a table with one storage-viewed entry");
    Require(AgcDriver::Graphics::ImageTableReuses().viewedKept - views.before.viewedKept == 1u, "image table partial resolve: the lone entry in host-imported memory was not kept as a storage-viewed entry");
    sampledBy(viewProgram, "a table with one storage-viewed entry", importable.Texels(0));
    importable.Remap();
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto viewRemapped = partly(viewShader, 1, "the guest mapping changed under a storage-viewed entry");
    keptExcept(views.textures, viewRemapped, LoneEntry, "the guest mapping changed under a storage-viewed entry");
    sampledBy(viewProgram, "the guest mapping changed under a storage-viewed entry", importable.Texels(0));
    return true;
}

constexpr std::size_t LargeTexturesOffset(std::uint32_t records) {
    return (static_cast<std::size_t>(records) * Stride + 65535u) & ~std::size_t{65535u};
}

constexpr std::size_t LargeBlockBytes(std::uint32_t records) {
    return LargeTexturesOffset(records) + static_cast<std::size_t>(records) * TextureBytes;
}

bool RunLargeTable(AgcDriver::VulkanDevice& device, const GuestBlock& block, std::uint32_t records, bool viewed) {
    using AgcDriver::Graphics::ShaderResources;
    using Textures = std::vector<std::shared_ptr<AgcDriver::Graphics::Texture>>;
    const std::string kind = viewed ? "image table reuse of a large storage-viewed table" : "image table reuse of a large table";
    if (!AgcDriver::GuestMemory::WriteWatched()) {
        std::puts(("guest memory has no write watch: " + kind + " is not tested").c_str());
        return false;
    }
    const auto& context = device.graphicsContext();
    if (viewed && (context.hostImportAlignment == 0 || AgcDriver::Graphics::HostImportFor(context, AddressOf(block.Table()), LargeBlockBytes(records)) == nullptr)) {
        Require(std::getenv("ANYPS5_REQUIRE_VULKAN") == nullptr, kind + ": storage-viewed entries require host imports");
        std::puts(("host imports are unavailable: " + kind + " is not tested").c_str());
        return false;
    }
    const auto changedAt = records / 2u + 1u;
    const auto keptAt = records - records / 4u + 1u;
    for (std::uint32_t texture = 0; texture < LargeRecords; ++texture) {
        if (texture < records) {
            Fill(block, texture, 0.25f);
            SetRecord(block, texture, texture);
        }
        LargeKeys[texture] = texture % records;
    }
    const auto srt = Buffer(block.Table(), records * Stride);
    const auto keys = Buffer(LargeKeys.data(), static_cast<std::uint32_t>(sizeof(LargeKeys)));
    std::copy(srt.begin(), srt.end(), Srt.begin());
    std::copy(keys.begin(), keys.end(), Srt.begin() + 4);
    SetOutput(Output.data());
    const auto program = Compile(device, block, Srt.data(), LargeKeys.data(), records, LargeKeys.size());
    const AgcDriver::Graphics::CompiledShader shader{ShaderStage::Compute, &program, 0};
    struct Built {
        Textures textures;
        Step step;
        std::uint64_t refusals = 0;
        std::uint64_t spans = 0;
        std::uint64_t entries = 0;
        std::uint64_t viewedKept = 0;
        std::uint64_t viewedLookedUp = 0;
    };
    const auto build = [&] {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        Built built;
        const auto before = AgcDriver::Graphics::ImageTableReuses();
        const auto resources = std::make_shared<ShaderResources>(context, shader);
        built.textures = resources->SampledTextures();
        built.step = Since(before);
        built.refusals = Refusals(before);
        const auto after = AgcDriver::Graphics::ImageTableReuses();
        built.spans = after.proofSpans - before.proofSpans;
        built.entries = after.proofEntries - before.proofEntries;
        built.viewedKept = after.viewedKept - before.viewedKept;
        built.viewedLookedUp = after.viewedLookedUp - before.viewedLookedUp;
        return built;
    };
    const auto onThread = [&] {
        Built built;
        std::exception_ptr failure;
        std::thread worker([&] {
            try {
                AgcDriver::GuestMemory::BumpCollectEpoch();
                built = build();
            } catch (...) {
                failure = std::current_exception();
            }
        });
        worker.join();
        if (failure != nullptr) std::rethrow_exception(failure);
        return built;
    };
    const auto describe = [](const Built& built) { return std::to_string(built.step.reused) + " reused, " + std::to_string(built.step.partial) + " partly resolved again with " + std::to_string(built.step.partialEntries) + " entries looked up again, " + std::to_string(built.step.resolved) + " resolved, " + std::to_string(built.refusals) + " refused; the proof collected or checked " + std::to_string(built.spans) + " merged ranges whole and " + std::to_string(built.entries) + " entries one by one"; };
    const auto settle = [&](const std::string& what) {
        AgcDriver::GuestMemory::BumpCollectEpoch();
        for (std::uint64_t attempt = 0; attempt <= CpuWriteResolutions + 1u; ++attempt) {
            auto built = build();
            if (built.step.reused == 1u) return built;
        }
        Require(false, kind + ": " + what + ": the table was not reused after it was resolved");
        return Built{};
    };
    const auto whole = [&](const Built& built, const Textures& textures, const std::string& what) {
        Require(built.step.reused == 1u && built.step.partial == 0u && built.step.resolved == 0u && built.refusals == 0u, kind + ": " + what + ": " + describe(built) + "; expected one table reused whole");
        Require(built.entries == 0u && built.spans == 1u, kind + ": " + what + ": " + describe(built) + "; expected the one range the entries' adjacent texels merge into and no entry on its own, whatever the number of entries");
        Require(built.textures == textures, kind + ": " + what + ": the reuse binds other textures than the table it reused");
        if (viewed) Require(built.viewedKept == records && built.viewedLookedUp == 0u, kind + ": " + what + ": " + std::to_string(built.viewedKept) + " storage-viewed entries kept and " + std::to_string(built.viewedLookedUp) + " looked up again, expected every entry kept");
    };
    const auto sampled = [&](const std::string& what) {
        Require(Dispatch(device, program, changedAt, LargeKeys.data()) == Row(block.Texels(changedAt)), kind + ": " + what + ": the entry under the CPU write sampled the wrong texels");
        Require(Dispatch(device, program, keptAt, LargeKeys.data()) == Row(block.Texels(keptAt)), kind + ": " + what + ": an entry away from the CPU write sampled the wrong texels");
    };

    const auto first = settle("a table of " + std::to_string(records) + " entries");
    Require(first.textures.size() == records, kind + ": the build does not hold one texture per entry");
    AgcDriver::GuestMemory::BumpCollectEpoch();
    whole(build(), first.textures, "a reuse in a new collect epoch");
    whole(build(), first.textures, "a reuse in the same collect epoch");
    whole(onThread(), first.textures, "a reuse on another thread in its own collect epoch");
    sampled("before any change");

    const auto unwritten = AgcDriver::GuestMemory::TrackerGeneration();
    Fill(block, changedAt, 0.875f);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto written = build();
    const auto writtenEntries = WrittenEntries(block, unwritten, records);
    Require(writtenEntries != 0u && writtenEntries * 2u <= records && EntryWritten(block, changedAt, unwritten) && !EntryWritten(block, keptAt, unwritten), kind + ": a CPU write over one entry stamped " + std::to_string(writtenEntries) + " of the table's entries");
    Require(written.refusals == 0u && written.step.resolved == 0u && (viewed || written.entries != 0u), kind + ": a CPU write under one entry: " + describe(written) + "; expected the changed range proved entry by entry and the table kept");
    if (viewed) Require(written.step.reused == 1u && written.step.partial == 0u && written.viewedLookedUp == writtenEntries && written.viewedKept == records - writtenEntries, kind + ": a CPU write under one entry: " + describe(written) + ", " + std::to_string(written.viewedKept) + " storage-viewed entries kept and " + std::to_string(written.viewedLookedUp) + " looked up again; expected the " + std::to_string(writtenEntries) + " written entries looked up again");
    else Require(written.step.reused == 0u && written.step.partial == 1u && written.step.partialEntries == writtenEntries, kind + ": a CPU write under one entry: " + describe(written) + "; expected the " + std::to_string(writtenEntries) + " written entries looked up again");
    Require(written.textures.size() == records && (viewed || written.textures[changedAt] != first.textures[changedAt]), kind + ": the entry under the CPU write kept its texture");
    for (std::uint32_t entry = 0; entry < records; ++entry) {
        if (!EntryWritten(block, entry, unwritten)) Require(written.textures[entry] == first.textures[entry], kind + ": entry " + std::to_string(entry) + " away from the CPU write was bound to another texture");
    }
    sampled("after a CPU write under one entry");

    const auto rewritten = settle("the table after a CPU write under one entry");
    AgcDriver::GuestMemory::BumpCollectEpoch();
    whole(build(), rewritten.textures, "a reuse in a new collect epoch after the entry under a CPU write was looked up again");
    return true;
}

bool RunRememberedReasons(AgcDriver::VulkanDevice& device, const GuestBlock& block) {
    using AgcDriver::Graphics::ImageTableUnreused;
    using AgcDriver::Graphics::ShaderResources;
    if (!AgcDriver::GuestMemory::WriteWatched()) {
        std::puts("guest memory has no write watch: why an image table was not remembered is not tested");
        return false;
    }
    const auto& context = device.graphicsContext();
    const auto program = CompileTable(device, block);
    const auto build = [&](const ShaderRecompiler::RecompileResult& compiled) {
        std::lock_guard gpu(AgcDriver::GuestMemory::GpuMutex());
        const AgcDriver::Graphics::CompiledShader shader{ShaderStage::Compute, &compiled, 0};
        const auto before = AgcDriver::Graphics::ImageTableReuses();
        static_cast<void>(std::make_shared<ShaderResources>(context, shader));
        return before;
    };
    const auto resolved = [&](const AgcDriver::Graphics::ImageTableReuseCounts& before, ImageTableUnreused why, const std::string& what) {
        Expect(before, 0, 1, what);
        ExpectUnreused(before, why, what);
    };
    AgcDriver::GuestMemory::BumpCollectEpoch();
    resolved(build(program), ImageTableUnreused::NotRemembered, "a table whose records were never remembered");
    std::vector<ShaderRecompiler::RecompileResult> others;
    others.reserve(RememberedTables);
    for (std::uint32_t table = 0; table < RememberedTables; ++table) {
        Fill(block, Records + table, 0.5f);
        SetRecord(block, KeptEntry, Records + table);
        others.push_back(Compile(device, block));
        resolved(build(others.back()), ImageTableUnreused::NotRemembered, "table " + std::to_string(table) + " of the " + std::to_string(RememberedTables) + " tables that differ in one record");
    }
    SetRecord(block, KeptEntry, KeptEntry);
    resolved(build(program), ImageTableUnreused::NotRememberedEvicted, "a table looked up again after " + std::to_string(RememberedTables) + " other tables were remembered");
    auto reshaped = program;
    const auto table = std::find_if(reshaped.bindings.begin(), reshaped.bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ImageTable; });
    Require(table != reshaped.bindings.end() && table->imageShapes.size() == Records, "image table reuse: the table binding has no image shape per entry");
    table->imageShapes[KeptEntry] = ShaderRecompiler::DescriptorImageShape::Image3D;
    resolved(build(reshaped), ImageTableUnreused::NotRememberedShapes, "a remembered table's records bound with another image shape");
    const auto again = build(program);
    Expect(again, 1, 0, "the remembered table's records bound with their own image shapes again");
    return true;
}

void RunTests(AgcDriver::VulkanDevice& device, const GuestBlock& block, const GuestBlock& stores) {
    for (std::uint32_t texture = 0; texture < Records; ++texture) {
        Fill(block, texture, 0.25f);
        SetRecord(block, texture, texture);
    }
    Fill(block, Moved, 0.5f);
    for (std::uint32_t key = 0; key < Records; ++key) Keys[key] = key;
    const auto srt = Buffer(block.Table(), Records * Stride);
    const auto keys = Buffer(Keys.data(), static_cast<std::uint32_t>(sizeof(Keys)));
    std::copy(srt.begin(), srt.end(), Srt.begin());
    std::copy(keys.begin(), keys.end(), Srt.begin() + 4);
    SetOutput(Output.data());
    const auto result = Compile(device, block);
    const bool watched = AgcDriver::GuestMemory::WriteWatched();

    std::vector<std::uint32_t> unbumped[2];
    auto before = AgcDriver::Graphics::ImageTableReuses();
    const auto pools = device.graphicsContext().descriptorCache->Counters();
    std::thread thread([&] {
        unbumped[0] = Dispatch(device, result, 3);
        unbumped[1] = Dispatch(device, result, 3);
    });
    thread.join();
    const auto poolsNow = device.graphicsContext().descriptorCache->Counters();
    chained = poolsNow.dedicatedPools + poolsNow.reusedSets == pools.dedicatedPools + pools.reusedSets;
    if (chained) std::puts("the table's descriptor set fits the shared pool chain: kept table sets are not tested");
    Require(unbumped[0] == Row(block.Texels(3)) && unbumped[1] == Row(block.Texels(3)), "image table reuse: a thread without a collect epoch sampled the wrong entry");
    Expect(before, 0, 2, "a thread without a collect epoch");

    AgcDriver::GuestMemory::BumpCollectEpoch();
    before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, result, 5) == Row(block.Texels(5)), "image table reuse: the first dispatch of the epoch sampled the wrong entry");
    Require(Dispatch(device, result, 9) == Row(block.Texels(9)), "image table reuse: a dispatch reusing the table sampled the wrong entry");
    Require(Dispatch(device, result, 5) == Row(block.Texels(5)), "image table reuse: the third dispatch of the epoch sampled the wrong entry");
    if (!watched) {
        Expect(before, 0, 3, "guest memory without a write watch");
        std::puts("guest memory has no write watch: image table reuse is not tested");
        return;
    }
    Expect(before, 2, 1, "three dispatches of one table in one epoch");
    if (std::getenv("APS5_NO_TABLE_REPLAY") == nullptr) Require(AgcDriver::Graphics::ImageTableReuses().replayed - before.replayed == 2u, "image table reuse: three dispatches of one table in one epoch did not replay the bound table twice");
    ExpectWrites(before, 3, 0, "three dispatches of one table in one epoch");

    before = AgcDriver::Graphics::ImageTableReuses();
    AgcDriver::Graphics::ClearCachedTextures(device.Device());
    Require(Dispatch(device, result, 7) == Row(block.Texels(7)), "image table reuse: the table after the texture cache was cleared sampled the wrong entry");
    Require(Dispatch(device, result, 9) == Row(block.Texels(9)), "image table reuse: the table resolved after the texture cache was cleared sampled the wrong entry");
    Expect(before, 1, 1, "a kept table whose textures the texture cache dropped");
    ExpectWrites(before, 1, 1, "a kept table whose textures the texture cache dropped");

    before = AgcDriver::Graphics::ImageTableReuses();
    std::vector<float> stored(Width * Height);
    for (std::uint32_t texel = 0; texel < Width * Height; ++texel) stored[texel] = -1.0f - static_cast<float>(texel);
    const auto unstored = AgcDriver::GuestMemory::TrackerGeneration();
    AgcDriver::GuestMemory::Write(AddressOf(block.Texels(11)), std::as_bytes(std::span(stored)));
    const auto storedEntries = WrittenEntries(block, unstored);
    Require(storedEntries != 0u && storedEntries * 2u <= Records, "image table reuse: a driver store over one entry stamped " + std::to_string(storedEntries) + " of the table's entries");
    Require(Dispatch(device, result, 11) == Row(stored.data()), "image table reuse: an entry the driver stored over in the epoch kept its old texels");
    Require(Dispatch(device, result, 11) == Row(stored.data()), "image table reuse: the table resolved after a driver store sampled the wrong entry");
    ExpectPartly(before, 1, 1, storedEntries, 0, "a driver store over an entry");

    before = AgcDriver::Graphics::ImageTableReuses();
    const auto unwritten = AgcDriver::GuestMemory::TrackerGeneration();
    Fill(block, 17, 0.75f);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(Dispatch(device, result, 17) == Row(block.Texels(17)), "image table reuse: an entry the CPU wrote before the epoch kept its old texels");
    const auto writtenEntries = WrittenEntries(block, unwritten);
    Require(writtenEntries != 0u && writtenEntries * 2u <= Records, "image table reuse: a CPU write over one entry stamped " + std::to_string(writtenEntries) + " of the table's entries");
    Require(Dispatch(device, result, 3) == Row(block.Texels(3)), "image table reuse: an entry stamped by another entry's collect sampled the wrong texels");
    Require(Dispatch(device, result, 17) == Row(block.Texels(17)), "image table reuse: the table of the new epoch sampled the wrong entry");
    ExpectPartly(before, 3 - CpuWriteResolutions, CpuWriteResolutions, CpuWriteResolutions * writtenEntries, 0, "a CPU write collected in a new epoch, then two dispatches");

    before = AgcDriver::Graphics::ImageTableReuses();
    SetRecord(block, 19, Moved);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    const auto moved = Compile(device, block);
    Require(Dispatch(device, moved, 19) == Row(block.Texels(Moved)), "image table reuse: a record pointed at another texture sampled the old one");
    Require(Dispatch(device, result, 19) == Row(block.Texels(19)), "image table reuse: the original table sampled the moved record");
    Require(Dispatch(device, moved, 19) == Row(block.Texels(Moved)), "image table reuse: the moved table sampled the wrong entry when reused");
    Require(Dispatch(device, result, 21) == Row(block.Texels(21)), "image table reuse: the original table sampled the wrong entry when reused");
    Expect(before, 3, 1, "two tables that differ in one record, the original one unchanged across the epoch");
    ExpectWrites(before, 0, 4, "two tables that differ in one record, alternating");

    SetOutput(block.Texels(13));
    const auto writer = Compile(device, block);
    SetOutput(Output.data());
    before = AgcDriver::Graphics::ImageTableReuses();
    static_cast<void>(Dispatch(device, writer, 7));
    const auto sampled = Dispatch(device, result, 13);
    const std::vector<float> written(block.Texels(13), block.Texels(13) + Width * Height);
    Require(std::bit_cast<std::uint32_t>(written[0]) == Row(block.Texels(7))[0], "image table reuse: the dispatch writing into an entry's texels stored the wrong values");
    Require(sampled == Row(written.data()), "image table reuse: an entry a dispatch wrote in the epoch kept its old texels");
    Expect(before, 1, 1, "a dispatch writing an entry's texels");
    Require(Dispatch(device, result, 15) == Row(block.Texels(15)), "image table reuse: the table after a dispatch wrote an entry sampled the wrong entry");

    RunImageStore(device, stores);

    AgcDriver::Graphics::ReleaseTextureCaches(~std::uint64_t{0});
    Require(AgcDriver::Graphics::SampledTextureMemory() == 0u, "image table reuse: " + std::to_string(AgcDriver::Graphics::SampledTextureMemory()) + " bytes of sampled textures outlived a full release of the texture caches after the kept-table steps");

    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(Dispatch(device, result, 29) == Row(block.Texels(29)), "image table reuse: the table before its textures were destroyed sampled the wrong entry");
    before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, result, 29) == Row(block.Texels(29)), "image table reuse: the table built again before its textures were destroyed sampled the wrong entry");
    ExpectWrites(before, 1, 0, "a kept table built again before its textures were destroyed");
    AgcDriver::Graphics::ReleaseTextureCaches(~std::uint64_t{0});
    Require(AgcDriver::Graphics::SampledTextureMemory() == 0u, "image table reuse: " + std::to_string(AgcDriver::Graphics::SampledTextureMemory()) + " bytes of sampled textures outlived a full release of the texture caches after a table's set was kept");
    before = AgcDriver::Graphics::ImageTableReuses();
    Require(Dispatch(device, result, 29) == Row(block.Texels(29)), "image table reuse: a kept table whose textures were destroyed sampled the wrong entry");
    ExpectWrites(before, 0, 1, "a kept table whose textures were destroyed");
}

}

int main(int argc, char** argv) {
    try {
        const std::string selected = argc > 1 ? argv[1] : "";
        if (selected == "--retaken-depth-surface" || selected == "--retaken-cleared-depth-surface" || selected == "--retaken-mirrored-depth-surface" || selected == "--retired-depth-surface") {
            GuestBlock block(UnimportableBytes);
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            const bool tested = selected == "--retaken-depth-surface" ? RunRetakenDepthSurface(*device, block) : selected == "--retired-depth-surface" ? RunRetiredDepthSurface(*device, block) : RunMirroredDepthSurface(*device, block, selected == "--retaken-cleared-depth-surface");
            if (!tested) return VulkanTestSkipped;
            auto name = selected.substr(2);
            std::replace(name.begin(), name.end(), '-', ' ');
            std::puts(("image table reuse: the " + name + " test passed").c_str());
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "entry-memo") {
            GuestBlock shared;
            GuestBlock plain;
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            if (!RunEntryMemo(*device, shared, plain)) return VulkanTestSkipped;
            std::puts("image table entry memo tests passed");
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "partial") {
            GuestBlock block(UnimportableBytes);
            GuestBlock lone(UnimportableBytes);
            GuestBlock importable;
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            if (!RunPartialResolve(*device, block, lone, importable)) return VulkanTestSkipped;
            std::puts("image table partial resolve tests passed");
            return 0;
        }
        if (argc > 1 && (std::string(argv[1]) == "large" || std::string(argv[1]) == "large-viewed")) {
            const bool viewed = std::string(argv[1]) == "large-viewed";
            const auto records = viewed ? LargeViewedRecords : LargeRecords;
            GuestBlock block(viewed ? LargeBlockBytes(records) : LargeBlockBytes(records) - 256u, LargeBlockBytes(records), LargeTexturesOffset(records));
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            if (!RunLargeTable(*device, block, records, viewed)) return VulkanTestSkipped;
            std::puts(viewed ? "image table reuse tests of a large storage-viewed table passed" : "image table reuse tests of a large table passed");
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "remembered") {
            GuestBlock block(UnimportableBytes);
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            if (!RunRememberedReasons(*device, block)) return VulkanTestSkipped;
            std::puts("image table remembered-table tests passed");
            return 0;
        }
        if (argc > 1 && std::string(argv[1]) == "records") {
            GuestBlock shared;
            GuestBlock plain;
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            if (!RunReusedTableRecords(*device, shared, plain)) return VulkanTestSkipped;
            std::puts("image table reuse record tests passed");
            return 0;
        }
        GuestBlock block;
        GuestBlock stores(UnimportableBytes);
        GuestBlock pending(UnimportableBytes);
        GuestBlock viewed;
        GuestBlock plain;
        GuestBlock threads(UnimportableBytes);
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        RunTests(*device, block, stores);
        RunPendingWriteBack(*device, pending);
        RunViewedStore(*device, viewed, plain);
        RunAcrossThreads(*device, threads);
        std::puts("image table reuse tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
