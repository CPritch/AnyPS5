#include "PipelineBinaryAbi.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PreservedCall.hpp"
#include <xmmintrin.h>
#include <cstdio>
#include <limits>

namespace {

constexpr std::uint32_t MxcsrControl = 0xffc0u;

PipelineBinaryAbiObservation Probe(std::uintptr_t function, std::array<std::uint64_t, 7> arguments, std::uint64_t seed = 0) {
    PipelineBinaryAbiCall call;
    call.function = function;
    call.arguments = arguments;
    call.seed = seed;
    PipelineBinaryAbiObservation observation;
    PipelineBinaryAbiProbe(&call, &observation);
    return observation;
}

bool Seeded(const PipelineBinaryAbiObservation& observation) {
    bool seeded = true;
    for (unsigned i = 0; i < PipelineBinaryAbiGeneralRegisters(); ++i) {
        const auto value = observation.before.general[i];
        seeded = seeded && (value >> 32u) != 0 && static_cast<std::uint32_t>(value) != 0;
        for (unsigned j = 0; j < i; ++j) seeded = seeded && value != observation.before.general[j];
    }
    for (unsigned i = 0; i < PipelineBinaryAbiVectorRegisters(); ++i) {
        for (const auto half : observation.before.vector[i]) seeded = seeded && (half >> 32u) != 0 && static_cast<std::uint32_t>(half) != 0;
    }
    return seeded;
}

bool ClobberedAsExpected(const PipelineBinaryAbiObservation& observation) {
    bool expected = true;
    for (unsigned i = 0; i < PipelineBinaryAbiGeneralRegisters(); ++i) expected = expected && observation.after.general[i] == (observation.before.general[i] & 0xffffffff00000000ull);
    for (unsigned i = 0; i < PipelineBinaryAbiVectorRegisters(); ++i) {
        for (const auto half : observation.after.vector[i]) expected = expected && half == std::numeric_limits<std::uint64_t>::max();
    }
    expected = expected && (observation.after.mxcsr & 0x8040u) == 0x8040u && (observation.before.mxcsr & 0x8040u) != 0x8040u;
    expected = expected && (observation.after.fpuControl ^ observation.before.fpuControl) == 0x0c00u;
    return expected && observation.after.direction == 1 && observation.before.direction == 0;
}

std::array<std::uint64_t, 7> Arguments(std::uint32_t* output) {
    return {1, 2, 3, 4, 5, reinterpret_cast<std::uintptr_t>(output), 0};
}

std::array<std::uint64_t, 7> PreservedArguments(std::uintptr_t function, std::uint32_t* output) {
    return {function, 1, 2, 3, 4, 5, reinterpret_cast<std::uintptr_t>(output)};
}

bool PreservedFromCompiledCode() {
    const auto mxcsr = _mm_getcsr();
    std::uint64_t accumulator = 0;
    std::uint64_t expected = 0;
    for (std::uint64_t i = 0; i < 64; ++i) {
        std::uint32_t output = 0;
        const auto result = AgcDriver::Graphics::PreservedCall(&PipelineBinaryAbiClobbering, 1, 2, 3, 4, 5, &output);
        accumulator = accumulator * 31u + i + output + static_cast<std::uint32_t>(result);
        expected = expected * 31u + i + 0x5au;
    }
    return accumulator == expected && _mm_getcsr() == mxcsr;
}

}

const char* PipelineBinaryAbiPlatform() {
#ifdef _WIN32
    return "Windows x86-64 native Win64";
#else
    return "Linux x86-64 native SysV";
#endif
}

std::string PipelineBinaryAbiDifferences(const PipelineBinaryAbiObservation& observation, unsigned& changed) {
    constexpr std::array names{"rbx", "rbp", "r12", "r13", "r14", "r15", "rdi", "rsi"};
    std::string text;
    changed = 0;
    char line[192];
    for (unsigned i = 0; i < PipelineBinaryAbiGeneralRegisters(); ++i) {
        if (observation.before.general[i] == observation.after.general[i]) continue;
        ++changed;
        std::snprintf(line, sizeof(line), " %s %016llx->%016llx", names[i], static_cast<unsigned long long>(observation.before.general[i]), static_cast<unsigned long long>(observation.after.general[i]));
        text += line;
    }
    for (unsigned i = 0; i < PipelineBinaryAbiVectorRegisters(); ++i) {
        const auto& before = observation.before.vector[i];
        const auto& after = observation.after.vector[i];
        if (before == after) continue;
        ++changed;
        std::snprintf(line, sizeof(line), " xmm%u %016llx%016llx->%016llx%016llx", i + 6, static_cast<unsigned long long>(before[1]), static_cast<unsigned long long>(before[0]), static_cast<unsigned long long>(after[1]), static_cast<unsigned long long>(after[0]));
        text += line;
    }
    if ((observation.before.mxcsr & MxcsrControl) != (observation.after.mxcsr & MxcsrControl)) {
        ++changed;
        std::snprintf(line, sizeof(line), " mxcsr %08x->%08x", observation.before.mxcsr, observation.after.mxcsr);
        text += line;
    }
    if (observation.before.fpuControl != observation.after.fpuControl) {
        ++changed;
        std::snprintf(line, sizeof(line), " x87cw %04x->%04x", observation.before.fpuControl, observation.after.fpuControl);
        text += line;
    }
    if (observation.after.direction != 0) {
        ++changed;
        text += " direction-flag set";
    }
    return text;
}

int PipelineBinaryAbiSynthetic(const std::string& mode) {
    std::uint32_t output = 0;
    unsigned changed = 0;
    bool passed = false;
    std::string differences;
    const auto conforming = reinterpret_cast<std::uintptr_t>(&PipelineBinaryAbiConforming);
    const auto clobbering = reinterpret_cast<std::uintptr_t>(&PipelineBinaryAbiClobbering);
    const auto preserved = reinterpret_cast<std::uintptr_t>(&PreservedVulkanCall_nid_no_patch);
    if (mode == "conforming") {
        const auto observation = Probe(conforming, Arguments(&output), 0x0123456789abcdefull);
        differences = PipelineBinaryAbiDifferences(observation, changed);
        passed = Seeded(observation) && changed == 0 && observation.result == 0 && output == 0x5a;
    } else if (mode == "clobbering") {
        const auto observation = Probe(clobbering, Arguments(&output));
        differences = PipelineBinaryAbiDifferences(observation, changed);
        passed = Seeded(observation) && ClobberedAsExpected(observation) && changed == PipelineBinaryAbiGeneralRegisters() + PipelineBinaryAbiVectorRegisters() + 3u && observation.result == 0 && output == 0x5a;
    } else if (mode == "preserved") {
        const auto observation = Probe(preserved, PreservedArguments(clobbering, &output));
        differences = PipelineBinaryAbiDifferences(observation, changed);
        std::uint32_t conformingOutput = 0;
        unsigned conformingChanged = 0;
        const auto direct = Probe(preserved, PreservedArguments(conforming, &conformingOutput));
        static_cast<void>(PipelineBinaryAbiDifferences(direct, conformingChanged));
        const bool compiled = PreservedFromCompiledCode();
        passed = Seeded(observation) && changed == 0 && observation.result == 0 && output == 0x5a && conformingChanged == 0 && direct.result == 0 && conformingOutput == 0x5a && compiled;
        std::printf("preserved call from compiled code: %s\n", compiled ? "intact" : "changed");
    } else if (mode == "nested") {
        PipelineBinaryAbiCall inner;
        inner.function = conforming;
        inner.arguments = Arguments(&output);
        PipelineBinaryAbiObservation innerObservation;
        const auto outer = Probe(reinterpret_cast<std::uintptr_t>(&PipelineBinaryAbiProbe), {reinterpret_cast<std::uintptr_t>(&inner), reinterpret_cast<std::uintptr_t>(&innerObservation), 0, 0, 0, 0, 0}, 0x5555aaaa5555aaaaull);
        unsigned innerChanged = 0;
        static_cast<void>(PipelineBinaryAbiDifferences(innerObservation, innerChanged));
        differences = PipelineBinaryAbiDifferences(outer, changed);
        bool distinct = true;
        for (unsigned i = 0; i < PipelineBinaryAbiGeneralRegisters(); ++i) distinct = distinct && innerObservation.before.general[i] != outer.before.general[i];
        passed = Seeded(outer) && Seeded(innerObservation) && distinct && changed == 0 && innerChanged == 0 && innerObservation.result == 0 && output == 0x5a;
    } else {
        std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
        return 1;
    }
    std::printf("%s; compiler=%s; synthetic=%s; changed=%u;%s %s\n", PipelineBinaryAbiPlatform(), __VERSION__, mode.c_str(), changed, differences.c_str(), passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
