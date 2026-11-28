#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "Optimization/RequestMemoryView.hpp"
#include "Optimization/ResourceProgram.hpp"
#include "VulkanTestDevice.hpp"
#include "ExecutionHelpers.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 32;
constexpr std::uint32_t Markers = 12;
constexpr std::uint32_t Format8888UNorm = 56;
constexpr std::uint32_t Type1D = 8;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t SwizzleXyzw = 0xfacu;
constexpr std::uint32_t SwizzleXyz1 = 0x3acu;
constexpr std::uint32_t Texel = 0xff996633u;
constexpr std::uint32_t Fill = 0xdeadbeefu;
alignas(256) std::array<std::uint32_t, 64> Texels{};
alignas(256) std::array<std::uint32_t, (Lanes + Markers) * 4u> Output{};

constexpr std::array<std::uint32_t, 16> SampleCode(std::uint32_t marker) {
    return {0x7e0202f0u, 0x7e0402f0u, 0xf09c0f08u, 0x00400401u, 0xbf8c3f70u, 0x34060084u, 0xe0781000u + marker * 16u, 0x80030403u,
            0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
}

alignas(256) constexpr std::array Codes{SampleCode(0u), SampleCode(1u), SampleCode(2u), SampleCode(3u), SampleCode(4u), SampleCode(5u),
                                        SampleCode(6u), SampleCode(7u), SampleCode(8u), SampleCode(9u), SampleCode(10u), SampleCode(11u)};

enum class Mode { Direct, Captured, Prepared };

using Words = std::array<std::uint32_t, 8>;
using Values = std::array<std::uint32_t, 4>;

Words Texture(std::uint32_t type, std::uint32_t swizzle) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Texels.data()));
    return {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format8888UNorm << 20u), 0u, swizzle | (type << 28u), 0u, 0u, 0u, 0u};
}

constexpr Words ZeroNull{};
constexpr Words TypedNull{0u, Format8888UNorm << 20u, 0u, Type2D << 28u, 0u, 0u, 0u, 0u};
constexpr Words AddressNull{0u, Format8888UNorm << 20u, 0u, SwizzleXyzw | (Type2D << 28u), 0u, 0u, 0u, 0u};

std::uint32_t Unorm(std::uint32_t shift) {
    return std::bit_cast<std::uint32_t>(static_cast<float>((Texel >> shift) & 0xffu) / 255.0f);
}

const Values BoundValues{Unorm(0u), Unorm(8u), Unorm(16u), Unorm(24u)};
const Values Xyz1Values{Unorm(0u), Unorm(8u), Unorm(16u), 0x3f800000u};
constexpr Values ZeroValues{};

struct Outcome {
    std::uint64_t pipeline = 0;
    std::vector<std::uint32_t> spirv;
    Values values{};
};

bool Close(std::uint32_t actual, std::uint32_t expected) {
    return actual == expected || std::fabs(std::bit_cast<float>(actual) - std::bit_cast<float>(expected)) <= 1e-6f;
}

Outcome Run(AgcDriver::VulkanDevice& device, std::uint32_t marker, const Words& texture, const Values& expected, const std::string& name, Mode mode) {
    const auto& code = Codes.at(marker);
    Texels.fill(Texel);
    Output.fill(Fill);
    std::vector<std::uint32_t> userData(texture.begin(), texture.end());
    for (const auto word : {0u, 0x00fff000u, 0u, 0u}) userData.push_back(word);
    const auto output = BufferDescriptor(Output.data(), sizeof(Output));
    userData.insert(userData.end(), output.begin(), output.end());
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span{code})}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0u, {true, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {Lanes, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = true;
    const auto result = [&] {
        if (mode == Mode::Direct) return ShaderRecompiler::Recompile(request);
        ShaderRecompiler::RequestMemoryView memoryView(request.context.memory);
        const auto runtime = memoryView.MakeRuntime(request.context.userData, request.shader.codeAddress);
        if (mode == Mode::Captured) return *ShaderRecompiler::Recompile(request, *ShaderRecompiler::CaptureResources(request, runtime));
        const auto handle = ShaderRecompiler::PrepareShader(request);
        const auto invocation = ShaderRecompiler::PreparedShaderInvocation::TryCreate(request, handle);
        Require(invocation.has_value(), "the prepared shader refused its own request");
        return *invocation->Materialize(*invocation->Capture(runtime));
    }();
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    Outcome outcome;
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
        for (std::uint32_t component = 0; component < 4u; ++component) {
            const auto actual = Output[(marker + lane) * 4u + component];
            if (lane == 0u) outcome.values[component] = actual;
            Require(Close(actual, expected[component]), name + ": lane " + std::to_string(lane) + " component " + std::to_string(component) + " is " + Hex(actual) + ", expected " + Hex(expected[component]));
        }
    }
    outcome.pipeline = result.PipelineVariantId();
    outcome.spirv = result.spirv.Copy();
    Require(outcome.pipeline != 0u && !outcome.spirv.empty(), name + ": no specialized module");
    return outcome;
}

bool Same(const Outcome& left, const Outcome& right) {
    return left.pipeline == right.pipeline && left.spirv == right.spirv;
}

void Report(const char* what, const Outcome& outcome) {
    std::printf("%s reads %s %s %s %s\n", what, Hex(outcome.values[0]).c_str(), Hex(outcome.values[1]).c_str(), Hex(outcome.values[2]).c_str(), Hex(outcome.values[3]).c_str());
}

void BoundThenNull(AgcDriver::VulkanDevice& device, Mode mode) {
    const auto marker = static_cast<std::uint32_t>(mode) * 4u;
    const auto bound = Run(device, marker, Texture(Type2D, SwizzleXyzw), BoundValues, "bound 8_8_8_8_UNORM T#", mode);
    const auto zero = Run(device, marker, ZeroNull, ZeroValues, "zero T# after the bound slot", mode);
    Report("a zero T# specialized with the bound slot's substitute", zero);
    Require(Same(zero, bound), "a zero T# did not take the substitute learned from the bound slot");
    const auto typed = Run(device, marker, TypedNull, ZeroValues, "addressless 2D T# with SEL_0 selectors", mode);
    Require(Same(typed, bound), "an addressless T# with SEL_0 selectors did not take the substitute");
    const auto probe = Run(device, marker, AddressNull, ZeroValues, "addressless 2D T# with XYZW selectors", mode);
    Report("an addressless T# with XYZW selectors, specialized with its own words,", probe);
    Require(!Same(probe, bound), "an addressless T# whose selectors read texels took the substitute");
    const auto again = Run(device, marker, Texture(Type2D, SwizzleXyzw), BoundValues, "bound again", mode);
    Require(Same(again, bound), "the bound slot changed modules after a null T#");
}

void NullThenBound(AgcDriver::VulkanDevice& device, Mode mode) {
    const auto marker = static_cast<std::uint32_t>(mode) * 4u + 1u;
    const auto first = Run(device, marker, ZeroNull, ZeroValues, "zero T# before any bound slot", mode);
    const auto bound = Run(device, marker, Texture(Type2D, SwizzleXyzw), BoundValues, "bound after the zero T#", mode);
    Require(!Same(first, bound), "a slot never seen bound did not specialize with its own words");
    const auto repeated = Run(device, marker, ZeroNull, ZeroValues, "same zero T# after learning", mode);
    Require(Same(repeated, bound), "the full-result memo retained the null module from before learning");
}

void ConstantOneNotLearned(AgcDriver::VulkanDevice& device, Mode mode) {
    const auto marker = static_cast<std::uint32_t>(mode) * 4u + 2u;
    const auto constant = Run(device, marker, Texture(Type2D, SwizzleXyz1), Xyz1Values, "bound XYZ1 T#", mode);
    const auto zero = Run(device, marker, ZeroNull, ZeroValues, "zero T# after an XYZ1 slot", mode);
    Require(!Same(zero, constant), "a zero T# took the specialization of a T# whose W selector is the constant 1");
    const auto bound = Run(device, marker, Texture(Type2D, SwizzleXyzw), BoundValues, "bound XYZW T# after an XYZ1 slot", mode);
    const auto learned = Run(device, marker, ZeroNull, ZeroValues, "zero T# after the XYZW slot", mode);
    Require(Same(learned, bound), "a zero T# did not take the first XYZW T# of its slot");
}

void OtherDimension(AgcDriver::VulkanDevice& device, Mode mode) {
    const auto marker = static_cast<std::uint32_t>(mode) * 4u + 3u;
    const auto line = Run(device, marker, Texture(Type1D, SwizzleXyzw), BoundValues, "bound 1D T# sampled with 2D coordinates", mode);
    const auto zero = Run(device, marker, ZeroNull, ZeroValues, "zero T# after a 1D slot", mode);
    Report("a zero T# specialized with a 1D slot's substitute", zero);
    Require(Same(zero, line), "a zero T# did not take the substitute learned from a 1D T#");
}

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        std::printf("null image substitutes: binding %s\n", std::getenv("APS5_NO_NULL_DESCRIPTOR") != nullptr ? "placeholder images (APS5_NO_NULL_DESCRIPTOR)" : "the device's null image descriptor path");
        for (const auto mode : {Mode::Direct, Mode::Captured, Mode::Prepared}) {
            if (argc > 1 && ((std::string(argv[1]) == "--captured" && mode != Mode::Captured) || (std::string(argv[1]) == "--prepared" && mode != Mode::Prepared))) continue;
            BoundThenNull(*device, mode);
            NullThenBound(*device, mode);
            ConstantOneNotLearned(*device, mode);
            OtherDimension(*device, mode);
            std::printf("null image substitutes mode=%u passed\n", static_cast<unsigned>(mode));
        }
        std::puts("null image substitute tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
