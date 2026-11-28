#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BindlessHeap.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "VulkanTestDevice.hpp"
#include "tests/TemporaryFile.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint32_t Entries = 2000;
constexpr std::uint32_t RecordBytes = 64;
constexpr std::uint32_t Side = 4;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Textures = 16;
constexpr std::size_t BlockBytes = std::size_t{2} << 20u;
constexpr std::size_t TextureStride = 0x1000;
constexpr std::size_t TableOffset = 0x140000;
constexpr std::size_t SrtOffset = 0x180000;
constexpr std::array<std::uint8_t, 4> TexelBytes{0x20u, 0x60u, 0xa0u, 0xffu};
constexpr std::array<std::uint32_t, 10> Keys{3u, 1764u, 37u, 54u, 1495u, 72u, 1657u, 1600u, 33u, 1810u};
constexpr std::size_t MaxBatches = 8;

alignas(256) constexpr std::array<std::uint32_t, 19> TableSamplerCode{
    0x7e040500u, 0x7e0002f0u, 0x7e0202f0u, 0xf4080200u, 0xfa000020u, 0xbf8cc07fu, 0x8f108602u, 0xf42c0504u, 0x20000000u, 0xf4280304u,
    0x20000020u, 0xbf8cc07fu, 0xf09c0f08u, 0x00650000u, 0xbf8c0000u, 0xe0780000u, 0x80010000u, 0xbf810000u, 0xbf810000u,
};

alignas(4096) std::array<std::array<float, 4>, Keys.size()> Outputs{};

class GuestBlock {
public:
    GuestBlock() {
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
            GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, BlockBytes);
        }
#endif
        Require(block != nullptr, "cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        GuestArena::GuestArenaReset_nid_postfix(block, BlockBytes);
        GuestArena::GuestArenaRelease_nid_postfix(block, BlockBytes);
#else
        munmap(block, BlockBytes);
        GuestWriteWatch::GuestWriteWatchUnregister_nid_postfix(block, BlockBytes);
#endif
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Texture(std::uint32_t index) const { return block + static_cast<std::size_t>(index) * TextureStride; }
    std::uint32_t* Table() const { return reinterpret_cast<std::uint32_t*>(block + TableOffset); }
    std::uint32_t* Srt() const { return reinterpret_cast<std::uint32_t*>(block + SrtOffset); }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 8> TextureDescriptor(const void* data) {
    constexpr std::uint32_t Rgba8Unorm = 56u;
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Rgba8Unorm << 20u) | (((Side - 1u) & 3u) << 30u),
        ((Side - 1u) >> 2u) | ((Side - 1u) << 14u),
        0xfacu | (Type2D << 28u),
        0u, 0u, 0u, 0u,
    };
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

void FillGuest(const GuestBlock& block) {
    for (std::uint32_t texture = 0; texture < Textures; ++texture) {
        for (std::size_t offset = 0; offset + TexelBytes.size() <= TextureStride; offset += TexelBytes.size()) std::memcpy(block.Texture(texture) + offset, TexelBytes.data(), TexelBytes.size());
    }
    auto* table = block.Table();
    std::fill_n(table, static_cast<std::size_t>(Entries) * (RecordBytes / 4u), 0u);
    for (std::uint32_t entry = 0; entry < Entries; ++entry) {
        const auto descriptor = TextureDescriptor(block.Texture(entry % Textures));
        std::copy(descriptor.begin(), descriptor.end(), table + static_cast<std::size_t>(entry) * (RecordBytes / 4u));
    }
    auto* srt = block.Srt();
    std::fill_n(srt, 8, 0u);
    const auto tableWords = BufferDescriptor(table, Entries * RecordBytes);
    std::copy(tableWords.begin(), tableWords.end(), srt + 8);
}

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header TableSamplerHeader{};

void Registered(Header& header, std::span<const std::uint32_t> code) {
    header.shader.file_header = 0x34333231u;
    header.shader.version = 0x18u;
    header.shader.header_size = sizeof(Header);
    header.shader.shader_size = static_cast<std::uint32_t>(code.size_bytes());
    header.shader.code = code.data();
    header.shader.type = 0;
    header.shader.user_data = &header.userData;
    AgcDriverRegisterShader_nid_postfix(&header.shader);
}

void Append(std::vector<std::uint32_t>& commands, std::uint32_t opcode, std::initializer_list<std::uint32_t> payload) {
    commands.push_back(0xc0000000u | (static_cast<std::uint32_t>(payload.size() - 1) << 16u) | (opcode << 8u));
    commands.insert(commands.end(), payload);
}

void Dispatch(std::vector<std::uint32_t>& words, const GuestBlock& block, std::uint32_t key, std::array<float, 4>& output) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(TableSamplerCode.data()));
    const auto srt = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block.Srt()));
    const auto set = [&](std::uint32_t offset, std::uint32_t value) { Append(words, 0x76, {offset, value}); };
    set(0x20c, static_cast<std::uint32_t>(address >> 8u));
    set(0x20d, static_cast<std::uint32_t>(address >> 40u) & 0xffu);
    for (const std::uint32_t offset : {0x207u, 0x208u, 0x209u}) set(offset, 1);
    set(0x212, 0);
    set(0x213, 16u << 1u);
    std::array<std::uint32_t, 16> userData{};
    userData[0] = static_cast<std::uint32_t>(srt);
    userData[1] = static_cast<std::uint32_t>(srt >> 32u);
    userData[2] = key;
    const auto outputWords = BufferDescriptor(output.data(), sizeof(output));
    std::copy(outputWords.begin(), outputWords.end(), userData.begin() + 4);
    for (std::uint32_t index = 0; index < userData.size(); ++index) set(0x240u + index, userData[index]);
    Append(words, 0x15, {1, 1, 1, 0x8041u});
}

class StderrCapture {
public:
    StderrCapture() {
        std::fflush(stderr);
        file = TemporaryFile::Open();
        Require(file != nullptr, "cannot open a temporary file for stderr");
#ifdef _WIN32
        saved = _dup(_fileno(stderr));
        _dup2(_fileno(file), _fileno(stderr));
#else
        saved = dup(fileno(stderr));
        dup2(fileno(file), fileno(stderr));
#endif
    }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;
    ~StderrCapture() {
        if (file != nullptr) static_cast<void>(Finish());
    }
    std::string Finish() {
        std::fflush(stderr);
#ifdef _WIN32
        _dup2(saved, _fileno(stderr));
        _close(saved);
#else
        dup2(saved, fileno(stderr));
        close(saved);
#endif
        std::rewind(file);
        std::string text;
        char buffer[4096];
        for (std::size_t read = 0; (read = std::fread(buffer, 1, sizeof(buffer), file)) != 0;) text.append(buffer, read);
        std::fclose(file);
        file = nullptr;
        std::fwrite(text.data(), 1, text.size(), stderr);
        return text;
    }

private:
    std::FILE* file = nullptr;
    int saved = -1;
};

struct Batch {
    std::string log;
    std::size_t correct = 0;
};

bool Expected(const std::array<float, 4>& output) {
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (std::fabs(output[channel] - static_cast<float>(TexelBytes[channel]) / 255.0f) >= 1e-6f) return false;
    }
    return true;
}

Batch RunBatch(const GuestBlock& block, std::size_t index, std::size_t dispatches = Keys.size()) {
    for (auto& output : Outputs) output.fill(-1.0f);
    std::vector<std::uint32_t> words;
    for (std::size_t key = 0; key < dispatches; ++key) Dispatch(words, block, Keys[key], Outputs[key]);
    StderrCapture capture;
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    const auto submitted = sceAgcDriverSubmitAcb(0x20, &packet);
    AgcDriverWaitIdle_nid_postfix();
    Batch batch;
    batch.log = capture.Finish();
    Require(submitted == 0, "batch " + std::to_string(index) + ": the compute submission failed");
    std::vector<std::byte> stored(sizeof(Outputs));
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Outputs.data())), stored, 4);
    std::memcpy(Outputs.data(), stored.data(), stored.size());
    for (std::size_t key = 0; key < dispatches; ++key) {
        if (Expected(Outputs[key])) ++batch.correct;
    }
    return batch;
}

std::string DeferralPrefix() {
    char prefix[96];
    std::snprintf(prefix, sizeof(prefix), "[image-table] program 0x%" PRIx64 " deferred ", static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(TableSamplerCode.data())));
    return prefix;
}

void RunUnbudgeted(const GuestBlock& block) {
    const auto batch = RunBatch(block, 0);
    Require(batch.correct == Keys.size(), "without APS5_TABLE_IMAGE_BUDGET, " + std::to_string(Keys.size() - batch.correct) + " of " + std::to_string(Keys.size()) + " dispatches did not sample their table entry");
    Require(batch.log.find("[image-table] program ") == std::string::npos, "without APS5_TABLE_IMAGE_BUDGET, a resolve deferred table entries");
    std::printf("unbudgeted: every dispatch sampled its entry in the first batch\n");
}

std::vector<std::uint32_t> DeferredCounts(const std::string& log, const std::string& prefix) {
    std::vector<std::uint32_t> counts;
    for (std::size_t at = log.find(prefix); at != std::string::npos; at = log.find(prefix, at + 1)) counts.push_back(static_cast<std::uint32_t>(std::strtoul(log.c_str() + at + prefix.size(), nullptr, 10)));
    return counts;
}

void RunBudgeted(const GuestBlock& block) {
    const auto prefix = DeferralPrefix();
    const auto first = RunBatch(block, 0, 1);
    const auto firstCounts = DeferredCounts(first.log, prefix);
    Require(firstCounts.size() == 1u && firstCounts.front() == Textures - 1u, "with APS5_TABLE_IMAGE_BUDGET=1, the first resolve of a table of " + std::to_string(Textures) + " textures did not create one and defer the rest");
    std::size_t batches = 1;
    auto latest = RunBatch(block, batches++);
    auto counts = DeferredCounts(latest.log, prefix);
    Require(!counts.empty() && counts.front() < firstCounts.front(), "the second resolve deferred as many entries as the first");
    for (std::size_t index = 1; index < counts.size(); ++index) Require(counts[index] < counts[index - 1], "a resolve deferred " + std::to_string(counts[index]) + " entries after one that deferred " + std::to_string(counts[index - 1]) + ", so it created none");
    while (latest.log.find(prefix) != std::string::npos && batches < MaxBatches) latest = RunBatch(block, batches++);
    Require(latest.log.find(prefix) == std::string::npos, "table entries were still deferred after " + std::to_string(batches) + " batches");
    Require(latest.correct == Keys.size(), "after every entry was created, only " + std::to_string(latest.correct) + " of " + std::to_string(Keys.size()) + " dispatches sampled their entry");
    const auto settled = RunBatch(block, batches);
    Require(settled.log.find(prefix) == std::string::npos && settled.correct == Keys.size(), "a batch after every entry was created deferred entries or sampled wrongly");
    std::printf("budgeted: the first resolve created one of %u textures, each later resolve created more, and none was deferred after %zu batches\n", Textures, batches);
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_EAGER_WRITEBACK", "1");
#else
    setenv("APS5_EAGER_WRITEBACK", "1", 1);
#endif
    try {
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
        }
        const bool budgeted = std::getenv("APS5_TABLE_IMAGE_BUDGET") != nullptr;
        const bool heap = AgcDriver::Graphics::GpuHeapEnabled() && AgcDriver::Graphics::DescriptorHeapEnabled();
        if (budgeted && !heap) {
            std::printf("skipped, APS5_TABLE_IMAGE_BUDGET applies to image tables on the GPU heap, which needs APS5_GPU_HEAP and VK_EXT_descriptor_heap\n");
            return VulkanTestSkipped;
        }
        GuestBlock block;
        FillGuest(block);
        Registered(TableSamplerHeader, TableSamplerCode);
        if (budgeted) RunBudgeted(block);
        else RunUnbudgeted(block);
        AgcDriverShutdown_nid_postfix();
        std::cout << "image table budget tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "image table budget tests failed: " << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
