#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawTiming.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

struct DriverTestAccess {
    static void Relocate(const QueueState& queue, std::uint64_t alias, std::shared_ptr<DrawEntry>& entry, std::shared_ptr<const DrawDecode>& decode, std::vector<DrawUserPatch>& patches) {
        auto& driver = Driver::Get();
        std::lock_guard lock(driver.drawCacheMutex);
        driver.relocateDrawEntry(queue, alias, entry, decode, patches);
    }

    static void Install(std::uint64_t alias, std::uint64_t key, std::shared_ptr<DrawEntry> entry) {
        auto& driver = Driver::Get();
        std::lock_guard lock(driver.drawCacheMutex);
        driver.drawCache[key] = std::move(entry);
        driver.drawAliases[alias] = key;
    }

    static void Clear() {
        auto& driver = Driver::Get();
        std::lock_guard lock(driver.drawCacheMutex);
        driver.drawCache.clear();
        driver.drawAliases.clear();
    }

    static void Lookup(const std::shared_ptr<VulkanDevice>& device, const std::vector<DrawProgram>& programs, const std::vector<ShaderRecompiler::ProgramRole>& roles, std::uint64_t key, std::shared_ptr<DrawEntry>& entry, std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool& drawHit, std::vector<StageDataHit>& stageData) {
        auto& driver = Driver::Get();
        static const Submission submission{};
        static const Graphics::State graphics{};
        static const ShaderRecompiler::ShaderPixelStageInfo pixel{};
        static const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>> vertexInfos(8);
        static std::array<double, DrawDriverPhaseCount> phaseMs{};
        static std::chrono::steady_clock::time_point lap;
        DrawPhaseTiming timing{false, phaseMs, lap};
        std::shared_ptr<DrawEntry> none;
        DrawDataCandidates candidates(programs.size());
        std::uint64_t drawKey = key;
        bool verifyHit = false;
        driver.lookupDraw(submission, device, graphics, pixel, programs, roles, vertexInfos, true, true, false, drawKey, entry, matched, matchedRegions, drawHit, verifyHit, timing, phaseMs, none, candidates, stageData);
    }
};

}

namespace {

using AgcDriver::Graphics::Require;
using namespace AgcDriver::DriverDetail;

using Clock = std::chrono::steady_clock;

std::size_t Count(const char* name, std::size_t fallback) {
    const char* text = std::getenv(name);
    return text == nullptr ? fallback : static_cast<std::size_t>(std::strtoull(text, nullptr, 10));
}

double NanosecondsPer(Clock::duration elapsed, std::size_t operations) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) / static_cast<double>(operations);
}

alignas(64) std::vector<std::uint32_t> Memory;

std::uint64_t MemoryBase() {
    return reinterpret_cast<std::uint64_t>(Memory.data());
}

std::uint32_t Low(std::uint64_t address) {
    return static_cast<std::uint32_t>(address);
}

std::shared_ptr<DispatchVariant> RelocatableVariant(std::uint64_t first, std::uint64_t second, std::size_t seed) {
    auto compiled = std::make_shared<ShaderRecompiler::RecompileResult>();
    compiled->pushConstants.resize(16);
    compiled->shaderDataUserWords = {2u, 4u};
    ShaderRecompiler::DescriptorBinding binding{ShaderRecompiler::DescriptorKind::StorageBuffer, ShaderRecompiler::DescriptorRole::GuestBuffers, 0, 0, 1, std::vector<std::uint32_t>(4, 0u)};
    binding.bufferWritten = {false};
    binding.bufferAtomic = {false};
    binding.bufferRead = {true};
    compiled->bindings.push_back(binding);
    auto variant = std::make_shared<DispatchVariant>();
    variant->compiled = compiled;
    const auto runBytes = [](std::size_t words) { return static_cast<std::uint64_t>(words * sizeof(std::uint32_t)); };
    variant->runs = {{MemoryBase() + 0x300000 + seed * 64, MemoryBase() + 0x300000 + seed * 64 + runBytes(16)}, {first, first + runBytes(16)}, {second, second + runBytes(8)}};
    variant->runBases = {-1, 2, 4};
    variant->words.assign(40, 0u);
    for (std::size_t i = 0; i < variant->words.size(); ++i) variant->words[i] = static_cast<std::uint32_t>(seed * 977 + i);
    for (const auto position : {16u, 20u, 24u, 28u}) variant->words[position] = 0x2000u;
    variant->relocatableWords = (1u << 2) | (1u << 4);
    variant->movableWords = {16u, 20u, 24u, 28u};
    variant->valueSlots = {{2u, 0u, 0u}};
    return variant;
}

void RelocationBenchmark(std::size_t entries, std::size_t rounds) {
    DriverTestAccess::Clear();
    Memory.assign(1u << 21, 0x1000u);
    constexpr std::uint32_t UserBase = 0x240;
    for (std::size_t i = 0; i < entries; ++i) {
        const auto first = MemoryBase() + (i % 4096) * 64;
        const auto second = MemoryBase() + 0x100000 + (i % 1024) * 64;
        auto decode = std::make_shared<DrawDecode>();
        for (std::size_t program = 0; program < 2; ++program) {
            DrawProgram drawProgram{};
            drawProgram.binary.stage = program == 0 ? ShaderRecompiler::ShaderStage::Vertex : ShaderRecompiler::ShaderStage::Fragment;
            drawProgram.userDataBase = UserBase + static_cast<std::uint32_t>(program) * 16u;
            drawProgram.hiddenUserWords = 0;
            drawProgram.userData.assign(8, 0u);
            drawProgram.userData[2] = Low(first);
            drawProgram.userData[4] = Low(second);
            decode->programs.push_back(std::move(drawProgram));
            decode->roles.push_back(ShaderRecompiler::ProgramRole::Main);
        }
        auto entry = std::make_shared<DrawEntry>();
        entry->decode = decode;
        entry->stages.resize(2);
        entry->stages[0].push_back(RelocatableVariant(first, second, i));
        entry->stages[1].push_back(RelocatableVariant(first, second, i + 1));
        DriverTestAccess::Install(1000 + i, 500000 + i, std::move(entry));
    }
    AgcDriver::QueueState queue;
    std::vector<std::shared_ptr<DrawEntry>> inflight(64);
    std::vector<DrawUserPatch> patches;
    std::shared_ptr<const DrawDecode> decode;
    Clock::duration total{};
    std::size_t operations = 0;
    double best = 1e18;
    for (std::size_t round = 0; round < rounds; ++round) {
        const auto firstTo = MemoryBase() + 0x180000 + round * 64;
        const auto secondTo = MemoryBase() + 0x1c0000 + round * 64;
        for (std::uint32_t program = 0; program < 2; ++program) {
            queue.shader[UserBase + program * 16u + 2u] = Low(firstTo);
            queue.shader[UserBase + program * 16u + 4u] = Low(secondTo);
        }
        const auto begin = Clock::now();
        for (std::size_t i = 0; i < entries; ++i) {
            std::shared_ptr<DrawEntry> relocated;
            DriverTestAccess::Relocate(queue, 1000 + i, relocated, decode, patches);
            if (relocated == nullptr) throw std::runtime_error("a relocation produced no entry");
            inflight[i % inflight.size()] = std::move(relocated);
        }
        const auto elapsed = Clock::now() - begin;
        best = std::min(best, NanosecondsPer(elapsed, entries));
        total += elapsed;
        operations += entries;
        for (std::size_t i = entries - std::min<std::size_t>(entries, inflight.size()); i < entries; ++i) {
            const auto& relocated = inflight[i % inflight.size()];
            for (std::size_t program = 0; program < 2; ++program) {
                Require(relocated->stages[program].size() == 1u, "a relocation dropped a stage's variant");
                const auto& variant = *relocated->stages[program].front();
                Require(variant.runs[0].first == MemoryBase() + 0x300000 + (i + program) * 64, "a run that did not move was changed");
                Require(variant.runs[1].first == firstTo && variant.runs[1].second == firstTo + 64 && variant.runs[2].first == secondTo && variant.runs[2].second == secondTo + 32, "a relocated variant's runs were not moved to the new addresses");
                Require(variant.words[16] == 0x1000u && variant.words[20] == 0x1000u && variant.words[24] == 0x1000u && variant.words[28] == 0x1000u && variant.words[15] == static_cast<std::uint32_t>((i + program) * 977 + 15) && variant.words[17] == static_cast<std::uint32_t>((i + program) * 977 + 17), "a relocated variant's moved word was not read again alone");
                Require(variant.shaderDataPatches.size() == 2u && variant.shaderDataPatches[0].second == Low(firstTo) && variant.shaderDataPatches[1].second == Low(secondTo) && variant.descriptorPatches.size() == 1u && variant.descriptorPatches[0][2] == Low(firstTo), "a relocated variant's patches do not name the moved words");
            }
        }
    }
    std::cout << "relocation: " << NanosecondsPer(total, operations) << " ns per relocated draw, best round " << best << " (" << entries << " entries, " << operations << " relocations)\n";
    DriverTestAccess::Clear();
}

void DataHitBenchmark(std::size_t entries, std::size_t variantCount, std::size_t rounds, const std::shared_ptr<AgcDriver::VulkanDevice>& device) {
    constexpr std::size_t Words = 48, DataWords = 8;
    std::vector<std::uint32_t> memory(variantCount * Words);
    std::vector<std::shared_ptr<DrawEntry>> keptEntries;
    std::vector<std::vector<DrawProgram>> programs;
    for (std::size_t v = 0; v < variantCount; ++v) {
        auto compiled = std::make_shared<ShaderRecompiler::RecompileResult>();
        compiled->variantId = v + 1;
        ShaderRecompiler::DescriptorBinding binding{ShaderRecompiler::DescriptorKind::StorageBuffer, ShaderRecompiler::DescriptorRole::FlattenedSrt, 0, 0, 1, std::vector<std::uint32_t>(DataWords + 4, 0u)};
        compiled->bindings.push_back(binding);
        ShaderRecompiler::DescriptorBinding second{ShaderRecompiler::DescriptorKind::StorageBuffer, ShaderRecompiler::DescriptorRole::GuestBuffers, 0, 1, 1, std::vector<std::uint32_t>(4, 0u)};
        compiled->bindings.push_back(second);
        for (std::uint32_t extra = 0; extra < 6; ++extra) {
            ShaderRecompiler::DescriptorBinding more{ShaderRecompiler::DescriptorKind::StorageBuffer, ShaderRecompiler::DescriptorRole::GuestBuffers, 0, 2 + extra, 4, std::vector<std::uint32_t>(16, extra)};
            more.bufferWritten = {false, false, false, false};
            more.bufferAtomic = {false, false, false, false};
            more.bufferRead = {true, true, true, true};
            compiled->bindings.push_back(std::move(more));
        }
        compiled->pushConstants.resize(256);
        compiled->shaderDataUserWords = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};
        auto variant = std::make_shared<DispatchVariant>();
        variant->compiled = compiled;
        const auto address = reinterpret_cast<std::uint64_t>(memory.data() + v * Words);
        for (std::size_t w = 0; w < Words; ++w) memory[v * Words + w] = static_cast<std::uint32_t>(v * 31 + w);
        variant->runs = {{address, address + Words * sizeof(std::uint32_t)}};
        variant->words.assign(memory.begin() + static_cast<std::ptrdiff_t>(v * Words), memory.begin() + static_cast<std::ptrdiff_t>((v + 1) * Words));
        variant->flatBinding = 0;
        for (std::uint32_t k = 0; k < DataWords; ++k) {
            variant->dataPositions.push_back(k * 2u);
            variant->dataSlots.push_back(k);
        }
        auto entry = std::make_shared<DrawEntry>();
        entry->stages.resize(1);
        entry->stages[0].push_back(variant);
        keptEntries.push_back(std::move(entry));
        std::vector<DrawProgram> one(1);
        one[0].binary.stage = ShaderRecompiler::ShaderStage::Vertex;
        one[0].binary.codeAddress = 0x1000 + v * 16;
        programs.push_back(std::move(one));
    }
    const std::vector<ShaderRecompiler::ProgramRole> roles{ShaderRecompiler::ProgramRole::Main};
    std::vector<std::vector<StageDataHit>> inflight(64);
    Clock::duration total{};
    std::size_t operations = 0, hits = 0, counter = 0;
    double best = 1e18;
    for (std::size_t round = 0; round < rounds; ++round) {
        Clock::duration roundTotal{};
        for (std::size_t i = 0; i < entries; ++i) {
            const auto v = i % variantCount;
            for (std::uint32_t k = 0; k < DataWords; ++k) memory[v * Words + k * 2u] = static_cast<std::uint32_t>(0x10000000u + counter * 16u + k);
            ++counter;
            std::vector<StageDataHit> stageData(1);
            std::vector<std::shared_ptr<DispatchVariant>> matched(1);
            std::vector<std::vector<ShaderRecompiler::MemoryRegion>> matchedRegions(1);
            auto entry = keptEntries[v];
            bool drawHit = false;
            const auto begin = Clock::now();
            DriverTestAccess::Lookup(device, programs[v], roles, 1, entry, matched, matchedRegions, drawHit, stageData);
            const auto spent = Clock::now() - begin;
            total += spent;
            roundTotal += spent;
            ++operations;
            if ((!drawHit && std::getenv("APS5_VERIFY_DRAW_DATA_HITS") == nullptr) || stageData[0].compiled == nullptr) throw std::runtime_error("a synthetic data-only hit missed");
            ++hits;
            const auto& descriptor = stageData[0].compiled->bindings[0].guestDescriptor;
            for (std::uint32_t k = 0; k < DataWords; ++k) Require(descriptor[k] == memory[v * Words + k * 2u], "a data hit's patched descriptor does not hold the live data words");
            inflight[i % inflight.size()] = std::move(stageData);
        }
        best = std::min(best, NanosecondsPer(roundTotal, entries));
    }
    std::cout << "data hit: " << NanosecondsPer(total, operations) << " ns per lookup with a data-only stage, best round " << best << " (" << variantCount << " variants, " << hits << " hits)\n";
}

}

int main() {
    try {
        std::shared_ptr<AgcDriver::VulkanDevice> device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto entries = Count("APS5_BENCH_ENTRIES", 10000);
        const auto rounds = Count("APS5_BENCH_ROUNDS", 3);
        const auto variants = Count("APS5_BENCH_VARIANTS", 1000);
        const char* only = std::getenv("APS5_BENCH_ONLY");
        if (only == nullptr || std::string(only) == "relocation") RelocationBenchmark(entries, rounds);
        if (only == nullptr || std::string(only) == "data") DataHitBenchmark(entries, variants, rounds, device);
        AgcDriverShutdown_nid_postfix();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        AgcDriverShutdown_nid_postfix();
        return 1;
    }
}
