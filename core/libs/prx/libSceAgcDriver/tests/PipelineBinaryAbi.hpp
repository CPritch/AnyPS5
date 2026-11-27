#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_PIPELINEBINARYABI_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TESTS_PIPELINEBINARYABI_HPP

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

struct PipelineBinaryAbiRegisters {
    std::array<std::uint64_t, 8> general{};
    std::array<std::array<std::uint64_t, 2>, 10> vector{};
    std::uint32_t mxcsr = 0;
    std::uint16_t fpuControl = 0;
    std::uint16_t direction = 0;
};

struct PipelineBinaryAbiObservation {
    PipelineBinaryAbiRegisters before;
    PipelineBinaryAbiRegisters after;
    std::uint64_t result = ~0ull;
};

struct PipelineBinaryAbiCall {
    std::uintptr_t function = 0;
    std::array<std::uint64_t, 7> arguments{};
    std::uint64_t seed = 0;
};

static_assert(sizeof(PipelineBinaryAbiRegisters) == 232);
static_assert(offsetof(PipelineBinaryAbiRegisters, vector) == 64);
static_assert(offsetof(PipelineBinaryAbiRegisters, mxcsr) == 224);
static_assert(offsetof(PipelineBinaryAbiRegisters, fpuControl) == 228);
static_assert(offsetof(PipelineBinaryAbiRegisters, direction) == 230);
static_assert(offsetof(PipelineBinaryAbiObservation, after) == 232);
static_assert(offsetof(PipelineBinaryAbiObservation, result) == 464);
static_assert(offsetof(PipelineBinaryAbiCall, arguments) == 8);
static_assert(offsetof(PipelineBinaryAbiCall, seed) == 64);

extern "C" void PipelineBinaryAbiProbe(const PipelineBinaryAbiCall* call, PipelineBinaryAbiObservation* observation);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL PipelineBinaryAbiConforming(std::uint64_t first, std::uint64_t second, std::uint64_t third, std::uint64_t fourth, std::uint64_t fifth, std::uint32_t* output);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL PipelineBinaryAbiClobbering(std::uint64_t first, std::uint64_t second, std::uint64_t third, std::uint64_t fourth, std::uint64_t fifth, std::uint32_t* output);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL PipelineBinaryAbiClobberingKey(VkDevice device, const VkPipelineCreateInfoKHR* info, VkPipelineBinaryKeyKHR* key);

constexpr unsigned PipelineBinaryAbiGeneralRegisters() {
#ifdef _WIN32
    return 8;
#else
    return 6;
#endif
}

constexpr unsigned PipelineBinaryAbiVectorRegisters() {
#ifdef _WIN32
    return 10;
#else
    return 0;
#endif
}

const char* PipelineBinaryAbiPlatform();
std::string PipelineBinaryAbiDifferences(const PipelineBinaryAbiObservation& observation, unsigned& changed);
int PipelineBinaryAbiSynthetic(const std::string& mode);

#endif
