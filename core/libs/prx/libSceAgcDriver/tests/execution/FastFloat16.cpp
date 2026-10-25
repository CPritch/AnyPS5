#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <spirv/unified1/GLSL.std.450.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Inputs = 4;
constexpr std::uint32_t Results = 28;
constexpr std::uint32_t Stride = 32;
constexpr std::uint32_t Batches = 16;
alignas(256) std::array<std::uint32_t, Threads * Inputs> Input{};
alignas(256) std::array<std::uint32_t, Threads * Stride> Output{};

alignas(256) constexpr std::array<std::uint32_t, 73> Code{
    0x34020084, 0x34060087, 0xe0381000, 0x80000401, 0xbf8c3f70,
    0x64140b04,
    0x66160b04,
    0x68180b04,
    0x6a1a0b04,
    0xd74b000e, 0x041a0b04,
    0xd74b980f, 0x041a0b04,
    0xd74b3110, 0xc4160d04,
    0x7e220307, 0x6c220b04,
    0x6e240b04, 0x00003d55,
    0x70260b04, 0x00003801,
    0xd74ba214, 0x241a0b04,
    0x662a08f2,
    0xd5328116, 0x22020b04,
    0xd5350217, 0x42020d04,
    0xd5338018, 0x02020d04,
    0xd5340019, 0x22020b04,
    0xcc0f401a, 0x1a020b04,
    0xcc10401b, 0x1a020b04,
    0xcc0e401c, 0x1c1a0b04,
    0xcc0f4a1d, 0x32020b04,
    0xcc10c01e, 0x1a020d04,
    0xcc0e331f, 0x8c1a0b04,
    0x7e400307, 0x78400b04,
    0xcc0ec021, 0x3c1a0f04,
    0x7e440307, 0xd74b4022, 0x04160d04,
    0x7e460306, 0xd74bfc23, 0x241a0b04,
    0xcc0e4024, 0x1c19e904,
    0xd74b0025, 0x0415e704,
    0xe0781000, 0x80010a03, 0xe0781010, 0x80010e03, 0xe0781020, 0x80011203, 0xe0781030, 0x80011603,
    0xe0781040, 0x80011a03, 0xe0781050, 0x80011e03, 0xe0781060, 0x80012203, 0xbf810000,
};

struct Result {
    const char* name;
    bool fma;
};

constexpr std::array<Result, Results> Names{{
    {"v_add_f16 v10, v4, v5", false},
    {"v_sub_f16 v11, v4, v5", false},
    {"v_subrev_f16 v12, v4, v5", false},
    {"v_mul_f16 v13, v4, v5", false},
    {"v_fma_f16 v14, v4, v5, v6", true},
    {"v_fma_f16 v15, v4, v5, v6 op_sel:[1,1,0,0] clamp", true},
    {"v_fma_f16 v16, |v4|, -v6, -v5 op_sel:[0,1,1,0]", true},
    {"v_fmac_f16 v17, v4, v5 (v17 = v7)", true},
    {"v_fmamk_f16 v18, v4, 0x3d55, v5", true},
    {"v_fmaak_f16 v19, v4, v5, 0x3801", true},
    {"v_fma_f16 v20, -v4, |v5|, v6 op_sel:[0,0,1,0] clamp", true},
    {"v_sub_f16 v21, 1.0, v4", false},
    {"v_add_f16 v22, -|v4|, v5 clamp", false},
    {"v_mul_f16 v23, v4, -|v6|", false},
    {"v_sub_f16 v24, v4, v6 clamp", false},
    {"v_subrev_f16 v25, -v4, v5", false},
    {"v_pk_add_f16 v26, v4, v5", false},
    {"v_pk_mul_f16 v27, v4, v5", false},
    {"v_pk_fma_f16 v28, v4, v5, v6", true},
    {"v_pk_add_f16 v29, v4, v5 op_sel:[1,0] op_sel_hi:[0,1] neg_lo:[1,0] neg_hi:[0,1]", false},
    {"v_pk_mul_f16 v30, v4, v6 clamp", false},
    {"v_pk_fma_f16 v31, v4, v5, v6 op_sel:[0,1,1] op_sel_hi:[1,0,0] neg_lo:[0,0,1] neg_hi:[1,1,0]", true},
    {"v_pk_fmac_f16 v32, v4, v5 (v32 = v7)", true},
    {"v_pk_fma_f16 v33, v4, v7, v6 neg_lo:[1,0,0] clamp", true},
    {"v_fma_f16 v34, v4, v6, v5 op_sel:[0,0,0,1] (v34 = v7)", true},
    {"v_fma_f16 v35, -v4, v5, |v6| op_sel:[1,1,1,1] clamp (v35 = v6)", true},
    {"v_pk_fma_f16 v36, v4, 2.0, v6", true},
    {"v_fma_f16 v37, v4, -1.0, v5", true},
}};

constexpr std::uint32_t FmaCount = 15;
constexpr std::uint32_t AddCount = 9;
constexpr std::uint32_t MulCount = 4;

struct Mode {
    const char* name;
    std::optional<ShaderRecompiler::ShaderFloatMode> floatMode;
    bool compared;
};

const std::array<Mode, 7> Modes{{
    {"no FLOAT_MODE", std::nullopt, true},
    {"dx10 ieee ovfl", ShaderRecompiler::ShaderFloatMode{0xc0u, true, true, true}, true},
    {"none", ShaderRecompiler::ShaderFloatMode{0xc0u, false, false, false}, true},
    {"f16 denormals flushed", ShaderRecompiler::ShaderFloatMode{0x30u, true, false, true}, false},
    {"f16 inputs flushed", ShaderRecompiler::ShaderFloatMode{0x80u, true, false, false}, false},
    {"all denormals flushed", ShaderRecompiler::ShaderFloatMode{0x00u, true, true, false}, false},
    {"f16 round toward zero", ShaderRecompiler::ShaderFloatMode{0xccu, true, false, false}, false},
}};

std::uint64_t Seed = 0x2545f4914f6cdd1dull;

std::uint32_t Next() {
    Seed += 0x9e3779b97f4a7c15ull;
    std::uint64_t z = Seed;
    z = (z ^ (z >> 30u)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27u)) * 0x94d049bb133111ebull;
    return static_cast<std::uint32_t>(z ^ (z >> 31u));
}

std::uint32_t OrdinaryHalf() {
    const std::uint32_t bits = Next();
    return ((bits >> 31u) << 15u) | ((0x0du + (bits >> 16u) % 5u) << 10u) | (bits & 0x3ffu);
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x31016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

struct Arithmetic {
    std::uint32_t f16x2Add = 0;
    std::uint32_t f16x2Mul = 0;
    std::uint32_t f16x2Fma = 0;
    std::uint32_t f32 = 0;
};

Arithmetic CountArithmetic(std::span<const std::uint32_t> spirv) {
    std::optional<std::uint32_t> half;
    std::optional<std::uint32_t> single;
    std::optional<std::uint32_t> pair;
    Arithmetic counts;
    for (std::size_t offset = 5u; offset < spirv.size();) {
        const std::uint32_t words = spirv[offset] >> 16u;
        const std::uint32_t opcode = spirv[offset] & 0xffffu;
        Require(words != 0u && offset + words <= spirv.size(), "fast f16: malformed SPIR-V");
        if (opcode == spv::OpTypeFloat && words >= 3u) {
            if (spirv[offset + 2u] == 16u) half = spirv[offset + 1u];
            if (spirv[offset + 2u] == 32u) single = spirv[offset + 1u];
        }
        if (opcode == spv::OpTypeVector && words >= 4u && half && spirv[offset + 2u] == *half && spirv[offset + 3u] == 2u) pair = spirv[offset + 1u];
        const bool arithmetic = opcode == spv::OpFAdd || opcode == spv::OpFSub || opcode == spv::OpFMul || (opcode == spv::OpExtInst && words >= 5u && spirv[offset + 4u] == GLSLstd450Fma);
        if (arithmetic && words >= 3u) {
            const std::uint32_t type = spirv[offset + 1u];
            if (pair && type == *pair) {
                if (opcode == spv::OpFAdd) ++counts.f16x2Add;
                if (opcode == spv::OpFMul) ++counts.f16x2Mul;
                if (opcode == spv::OpExtInst) ++counts.f16x2Fma;
            }
            if (single && type == *single) ++counts.f32;
        }
        offset += words;
    }
    return counts;
}

std::string Describe(const Arithmetic& counts) {
    return std::to_string(counts.f16x2Add) + " f16vec2 adds, " + std::to_string(counts.f16x2Mul) + " multiplies, " + std::to_string(counts.f16x2Fma) + " Fma and " + std::to_string(counts.f32) + " f32 arithmetic instructions";
}

bool Neighbours(std::uint32_t actual, std::uint32_t expected) {
    if (((actual ^ expected) & 0x8000u) != 0u) return false;
    const std::uint32_t a = actual & 0x7fffu;
    const std::uint32_t b = expected & 0x7fffu;
    if (a >= 0x7c00u || b >= 0x7c00u) return false;
    return (a > b ? a - b : b - a) <= 1u;
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto target = device->Target();
        const auto supports = [&](spv::Capability capability) {
            return std::ranges::find(target.supportedCapabilities, static_cast<std::uint32_t>(capability)) != target.supportedCapabilities.end();
        };
        const bool nativeFloat16 = supports(spv::CapabilityFloat16) && supports(spv::CapabilityRoundingModeRTE) && supports(spv::CapabilityDenormPreserve);
        const char* switchText = std::getenv("APS5_FAST_F16");
        const bool switchOn = switchText == nullptr || std::strcmp(switchText, "0") != 0;
        Require(target.fastFloat16 == (switchOn && nativeFloat16), std::string("fast f16: the device target has the switch ") + (target.fastFloat16 ? "on" : "off") + " with APS5_FAST_F16 " + (switchText == nullptr ? "unset" : switchText));
        if (!nativeFloat16) {
            std::puts("fast f16: skipped, the device has no native f16 with RTE rounding and kept denormals");
            return VulkanTestSkipped;
        }
        auto fastTarget = target;
        fastTarget.fastFloat16 = true;
        auto exactTarget = target;
        exactTarget.fastFloat16 = false;

        const std::span<const std::uint32_t> code(Code);
        std::vector<std::uint32_t> userData(8, 0u);
        const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
        const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
        std::copy(input.begin(), input.end(), userData.begin());
        std::copy(output.begin(), output.end(), userData.begin() + 4);
        const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
        const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
        const auto compile = [&](const ShaderRecompiler::SpirvTarget& shaderTarget, const Mode& mode, bool cached) {
            ShaderRecompiler::RecompileRequest request{
                {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
                {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
                shaderTarget,
                {0, 0, 0, 128}
            };
            request.useCache = cached;
            request.context.floatMode = mode.floatMode;
            return ShaderRecompiler::Recompile(request);
        };
        const auto run = [&](const auto& result) {
            Output.fill(0xdeadbeefu);
            device->Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
            device->WaitIdle();
            return Output;
        };

        const Arithmetic cachedFast = CountArithmetic(compile(fastTarget, Modes[0], true).spirv.Words());
        const Arithmetic cachedExact = CountArithmetic(compile(exactTarget, Modes[0], true).spirv.Words());
        Require(cachedFast.f16x2Fma != 0u && cachedExact.f16x2Fma == 0u, "fast f16: the cached builds with and without the switch share a module (" + Describe(cachedFast) + "; " + Describe(cachedExact) + ")");

        std::uint64_t compared = 0u;
        std::uint64_t neighbours = 0u;
        for (const auto& mode : Modes) {
            const auto fast = compile(fastTarget, mode, false);
            const Arithmetic fastCounts = CountArithmetic(fast.spirv.Words());
            Require(fastCounts.f16x2Add == AddCount && fastCounts.f16x2Mul == MulCount && fastCounts.f16x2Fma == FmaCount && fastCounts.f32 == 0u,
                std::string("fast f16: ") + mode.name + " has " + Describe(fastCounts) + ", expected " + std::to_string(AddCount) + ", " + std::to_string(MulCount) + ", " + std::to_string(FmaCount) + " and 0");
            if (!mode.compared) continue;
            const auto exact = compile(exactTarget, mode, false);
            const Arithmetic exactCounts = CountArithmetic(exact.spirv.Words());
            Require(exactCounts.f16x2Add == AddCount && exactCounts.f16x2Mul == MulCount && exactCounts.f16x2Fma == 0u && exactCounts.f32 != 0u,
                std::string("fast f16: ") + mode.name + " without the switch has " + Describe(exactCounts));
            for (std::uint32_t batch = 0; batch < Batches; ++batch) {
                for (auto& word : Input) word = OrdinaryHalf() | (OrdinaryHalf() << 16u);
                const auto fastOutput = run(fast);
                const auto exactOutput = run(exact);
                for (std::uint32_t tid = 0; tid < Threads; ++tid) {
                    for (std::uint32_t index = 0; index < Results; ++index) {
                        const std::uint32_t actual = fastOutput[tid * Stride + index];
                        const std::uint32_t expected = exactOutput[tid * Stride + index];
                        const std::uint32_t* row = &Input[tid * Inputs];
                        for (const std::uint32_t shift : {0u, 16u}) {
                            const std::uint32_t a = (actual >> shift) & 0xffffu;
                            const std::uint32_t e = (expected >> shift) & 0xffffu;
                            const bool near = a != e && Names[index].fma && Neighbours(a, e);
                            if (near) ++neighbours;
                            Require(a == e || near, std::string("fast f16: ") + mode.name + ", " + Names[index].name + " with v4=" + Hex(row[0]) + " v5=" + Hex(row[1]) + " v6=" + Hex(row[2]) + " v7=" + Hex(row[3]) + " is " + Hex(actual) + ", the exact path gives " + Hex(expected));
                        }
                    }
                }
                compared += Threads * Results;
            }
        }
        std::printf("fast f16 tests passed (%llu results, %llu fma halves one ULP from the exact path)\n", static_cast<unsigned long long>(compared), static_cast<unsigned long long>(neighbours));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
