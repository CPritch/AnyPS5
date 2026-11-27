#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BindlessHeap.hpp"
#include "prx/libSceAgcDriver/Graphics/include/CpuImageTableTrace.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DeviceImageTable.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "RuntimeAbi.hpp"
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
#include <chrono>
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
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr std::uint32_t Entries = 2000;
constexpr std::uint32_t RecordBytes = 32;
constexpr std::uint32_t PairedRecordBytes = 64;
constexpr std::uint32_t Side = 4;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t Textures = 16;
constexpr std::size_t BlockBytes = std::size_t{2} << 20u;
constexpr std::size_t TextureStride = 0x1000;
constexpr std::size_t TableOffset = 0x100000;
constexpr std::size_t PairedTableOffset = 0x140000;
constexpr std::size_t SrtOffset = 0x180000;
constexpr std::array<std::uint8_t, 4> TexelBytes{0x20u, 0x60u, 0xa0u, 0xffu};

alignas(256) constexpr std::array<std::uint32_t, 18> SampledCode{
    0x7e0002f0u, 0x7e0202f0u, 0xf4080200u, 0xfa000000u, 0xf4080300u, 0xfa000010u, 0xbf8cc07fu, 0x8f108502u, 0xf42c0504u, 0x20000000u,
    0xbf8cc07fu, 0xf09c0f08u, 0x00650000u, 0xbf8c0000u, 0xe0780000u, 0x80010000u, 0xbf810000u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 19> TableSamplerCode{
    0x7e040500u, 0x7e0002f0u, 0x7e0202f0u, 0xf4080200u, 0xfa000020u, 0xbf8cc07fu, 0x8f108602u, 0xf42c0504u, 0x20000000u, 0xf4280304u,
    0x20000020u, 0xbf8cc07fu, 0xf09c0f08u, 0x00650000u, 0xbf8c0000u, 0xe0780000u, 0x80010000u, 0xbf810000u, 0xbf810000u,
};

alignas(256) constexpr std::array<std::uint32_t, 18> LateCode{
    0x7e0002f2u, 0x7e0202f2u, 0xf4080200u, 0xfa000000u, 0xf4080300u, 0xfa000010u, 0xbf8cc07fu, 0x8f108502u, 0xf42c0504u, 0x20000000u,
    0xbf8cc07fu, 0xf09c0f08u, 0x00650000u, 0xbf8c0000u, 0xe0780000u, 0x80010000u, 0xbf810000u, 0xbf810000u,
};

alignas(4096) std::array<std::array<float, 4>, 64> Outputs{};

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
    std::uint32_t* PairedTable() const { return reinterpret_cast<std::uint32_t*>(block + PairedTableOffset); }
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
    for (std::uint32_t entry = 0; entry < Entries; ++entry) {
        const auto descriptor = TextureDescriptor(block.Texture(entry % Textures));
        std::copy(descriptor.begin(), descriptor.end(), table + static_cast<std::size_t>(entry) * (RecordBytes / 4u));
    }
    auto* paired = block.PairedTable();
    std::fill_n(paired, static_cast<std::size_t>(Entries) * (PairedRecordBytes / 4u), 0u);
    for (std::uint32_t entry = 0; entry < Entries; ++entry) {
        const auto descriptor = TextureDescriptor(block.Texture(entry % Textures));
        std::copy(descriptor.begin(), descriptor.end(), paired + static_cast<std::size_t>(entry) * (PairedRecordBytes / 4u));
    }
    auto* srt = block.Srt();
    const auto tableWords = BufferDescriptor(table, Entries * RecordBytes);
    std::copy(tableWords.begin(), tableWords.end(), srt);
    std::fill_n(srt + 4, 4, 0u);
    const auto pairedWords = BufferDescriptor(paired, Entries * PairedRecordBytes);
    std::copy(pairedWords.begin(), pairedWords.end(), srt + 8);
}

struct Header {
    Shader shader{};
    ShaderUserData userData{};
};

alignas(8) Header SampledHeader{};
alignas(8) Header TableSamplerHeader{};
alignas(8) Header LateHeader{};

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

std::uint64_t AddressOf(std::span<const std::uint32_t> code) {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(code.data()));
}

void Dispatch(std::vector<std::uint32_t>& words, const GuestBlock& block, std::span<const std::uint32_t> code, std::uint32_t key, std::array<float, 4>& output) {
    const auto address = AddressOf(code);
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

constexpr std::array<std::uint32_t, 10> Keys{3u, 1764u, 37u, 54u, 1495u, 72u, 1657u, 1600u, 33u, 1810u};

std::string RunBatch(const GuestBlock& block, std::span<const std::uint32_t> code, std::size_t dispatches, const std::string& what) {
    for (std::size_t index = 0; index < dispatches; ++index) Outputs[index].fill(-1.0f);
    std::vector<std::uint32_t> words;
    for (std::size_t index = 0; index < dispatches; ++index) Dispatch(words, block, code, Keys[index], Outputs[index]);
    StderrCapture log;
    Packet packet{words.data(), static_cast<std::uint32_t>(words.size()), 0, {}};
    const auto submitted = sceAgcDriverSubmitAcb(0x20, &packet);
    AgcDriverWaitIdle_nid_postfix();
    const auto text = log.Finish();
    Require(submitted == 0, what + ": the compute submission failed");
    std::vector<std::byte> stored(sizeof(Outputs));
    AgcDriver::GuestMemory::Read(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Outputs.data())), stored, 4);
    std::memcpy(Outputs.data(), stored.data(), stored.size());
    for (std::size_t index = 0; index < dispatches; ++index) {
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const auto expected = static_cast<float>(TexelBytes[channel]) / 255.0f;
            Require(std::fabs(Outputs[index][channel] - expected) < 1e-6f, what + ", key " + std::to_string(Keys[index]) + ": channel " + std::to_string(channel) + " is " + std::to_string(Outputs[index][channel]) + ", expected " + std::to_string(expected));
        }
    }
    return text;
}

std::string ProgramPrefix(std::span<const std::uint32_t> code) {
    char prefix[96];
    std::snprintf(prefix, sizeof(prefix), "[cpu-image-tables] program 0x%" PRIx64 " ", AddressOf(code));
    return prefix;
}

std::vector<std::string> LinesStarting(const std::string& text, const std::string& prefix) {
    std::vector<std::string> lines;
    for (std::size_t at = text.find(prefix); at != std::string::npos; at = text.find(prefix, at + 1)) {
        if (at != 0 && text[at - 1] != '\n') continue;
        const auto end = text.find('\n', at);
        lines.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
    }
    return lines;
}

std::string OnlyLine(const std::string& text, std::span<const std::uint32_t> code, const std::string& what) {
    const auto lines = LinesStarting(text, ProgramPrefix(code));
    Require(lines.size() == 1u, what + ": expected one trace line for program " + ProgramPrefix(code) + ", found " + std::to_string(lines.size()));
    return lines.front();
}

void RequireHas(const std::string& line, const std::string& part, const std::string& what) {
    Require(line.find(part) != std::string::npos, what + ": the trace line lacks \"" + part + "\": " + line);
}

void RequireLacks(const std::string& line, const std::string& part, const std::string& what) {
    Require(line.find(part) == std::string::npos, what + ": the trace line has \"" + part + "\": " + line);
}

std::string ReasonsOf(const std::string& line) {
    const auto begin = line.find(" reasons ");
    const auto end = line.find(" | ", begin);
    Require(begin != std::string::npos && end != std::string::npos, "a trace line has no reasons: " + line);
    return line.substr(begin + 9, end - begin - 9);
}

void RequireTable(const std::string& line, bool heap, const char* served, std::uint32_t records, std::uint32_t keys, std::uint32_t stride, const std::string& what) {
    const auto binding = "." + std::to_string(ShaderRecompiler::RuntimeAbi::ImageTableBinding) + " sampled records " + std::to_string(records) + " device-tables 0 heap " + (heap ? "1" : "0") + " served " + served + " plan captured gate ";
    RequireHas(line, " stage compute binding ", what);
    RequireHas(line, binding, what);
    RequireHas(line, " | image 0 table 0 keys=" + std::to_string(keys) + " fault=0 column stride=" + std::to_string(stride) + ",dwords=8", what);
    RequireHas(line, " access class=sampled,", what);
}

enum class Mode { Plain, Heap, Device };

void RunTrace(const GuestBlock& block, Mode mode) {
    const bool heap = mode != Mode::Plain;
    const auto started = std::chrono::steady_clock::now();
    const auto sampled = RunBatch(block, SampledCode, Keys.size(), "direct sampler");
    if (mode == Mode::Device) {
        Require(LinesStarting(sampled, ProgramPrefix(SampledCode)).empty(), "direct sampler: a program whose only image column the device image table serves printed a CPU table trace line");
    } else {
        const auto line = OnlyLine(sampled, SampledCode, "direct sampler");
        RequireTable(line, heap, "-", 1u, 1u, RecordBytes, "direct sampler");
        RequireHas(line, " gate requested=0,gpu-mode=" + std::string(heap ? "1" : "0") + ",", "direct sampler");
        const auto reasons = ReasonsOf(line);
        RequireHas(reasons, "gate-path-off", "direct sampler");
        RequireLacks(reasons, "table-sampler", "direct sampler");
        RequireLacks(reasons, "unseen", "direct sampler");
        RequireHas(line, " samplers s0:direct why -", "direct sampler");
        std::printf("direct sampler: %s\n", line.c_str());
    }
    const auto paired = RunBatch(block, TableSamplerCode, Keys.size(), "table sampler");
    const auto line = OnlyLine(paired, TableSamplerCode, "table sampler");
    RequireTable(line, heap, mode == Mode::Device ? "1" : "-", Textures, Entries, PairedRecordBytes, "table sampler");
    RequireHas(line, ",map-tables=2 | image 0 ", "table sampler");
    RequireHas(line, " samplers s0:table why table-sampler", "table sampler");
    const auto reasons = ReasonsOf(line);
    if (mode == Mode::Device) {
        RequireHas(line, " gate requested=1,gpu-mode=1,int64=1,buffer-addresses=1 ", "table sampler");
        Require(reasons == "table-sampler", "table sampler: the device-path program's only reason should be its table sampler, not " + reasons);
    } else {
        RequireHas(reasons, "gate-path-off", "table sampler");
        RequireHas(reasons, "table-sampler", "table sampler");
    }
    std::printf("table sampler: %s\n", line.c_str());
    const auto again = RunBatch(block, TableSamplerCode, Keys.size(), "table sampler again");
    Require(LinesStarting(again, ProgramPrefix(TableSamplerCode)).empty(), "table sampler again: a program and binding already traced printed a second line");
    if (mode != Mode::Plain) return;
    std::this_thread::sleep_until(started + std::chrono::milliseconds(10500));
    const auto late = RunBatch(block, LateCode, 1u, "late program");
    static_cast<void>(OnlyLine(late, LateCode, "late program"));
    const auto summaries = LinesStarting(late, "[cpu-image-tables] ");
    const auto summary = std::find_if(summaries.begin(), summaries.end(), [](const std::string& text) { return text.find(" records by reason ") != std::string::npos; });
    Require(summary != summaries.end(), "late program: no 10 s summary was printed after the first CPU table resolve");
    RequireHas(*summary, " CPU table resolves, ", "summary");
    RequireHas(*summary, " gate-path-off ", "summary");
    RequireHas(*summary, " table-sampler ", "summary");
    std::printf("summary: %s\n", summary->c_str());
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_EAGER_WRITEBACK", "1");
    _putenv_s("APS5_TRACE_CPU_IMAGE_TABLES", "1");
#else
    setenv("APS5_EAGER_WRITEBACK", "1", 1);
    setenv("APS5_TRACE_CPU_IMAGE_TABLES", "1", 1);
#endif
    try {
        Require(AgcDriver::Graphics::CpuImageTablesTraced(), "APS5_TRACE_CPU_IMAGE_TABLES set by the test is not seen");
        bool served = false;
        {
            const auto device = OpenVulkanTestDevice();
            if (!device) return VulkanTestSkipped;
            served = device->DeviceImagePath();
        }
        const bool gpuHeap = AgcDriver::Graphics::GpuHeapEnabled() && AgcDriver::Graphics::DescriptorHeapEnabled();
        if (AgcDriver::Graphics::GpuPathEnabled() && AgcDriver::Graphics::GpuHeapEnabled() && !served) {
            std::printf("skipped, the device does not serve the device image path\n");
            return VulkanTestSkipped;
        }
        if (AgcDriver::Graphics::GpuHeapEnabled() && !gpuHeap) {
            std::printf("skipped, the device lacks VK_EXT_descriptor_heap, which the GPU heap needs\n");
            return VulkanTestSkipped;
        }
        const auto mode = served ? Mode::Device : gpuHeap ? Mode::Heap : Mode::Plain;
        GuestBlock block;
        FillGuest(block);
        Registered(SampledHeader, SampledCode);
        Registered(TableSamplerHeader, TableSamplerCode);
        Registered(LateHeader, LateCode);
        std::printf("CPU image table trace, %s\n", mode == Mode::Device ? "device image path on" : mode == Mode::Heap ? "GPU heap without the device image path" : "per-dispatch descriptors");
        RunTrace(block, mode);
        AgcDriverShutdown_nid_postfix();
        std::cout << "CPU image table trace tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CPU image table trace tests failed: " << error.what() << '\n';
        try {
            AgcDriverShutdown_nid_postfix();
        } catch (const std::exception& shutdownError) {
            std::cerr << shutdownError.what() << '\n';
        }
        return 1;
    }
}
