#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PRESERVEDCALL_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PRESERVEDCALL_HPP

#include <vulkan/vulkan.h>
#include <array>
#include <cstdint>
#include <type_traits>

extern "C" std::uint64_t PreservedVulkanCall_nid_no_patch(std::uintptr_t function, std::uint64_t first, std::uint64_t second, std::uint64_t third, std::uint64_t fourth, std::uint64_t fifth, std::uint64_t sixth);

namespace AgcDriver::Graphics {

#if defined(__x86_64__) && (defined(__linux__) || defined(_WIN32))
inline constexpr bool PreservedCallsAvailable = true;
#else
inline constexpr bool PreservedCallsAvailable = false;
#endif

template<typename TValue>
std::uint64_t PreservedArgument(TValue value) {
    static_assert(std::is_pointer_v<TValue> || std::is_integral_v<TValue> || std::is_enum_v<TValue>);
    if constexpr (std::is_pointer_v<TValue>) return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value));
    else return static_cast<std::uint64_t>(value);
}

template<typename TResult, typename... TParameters>
TResult PreservedCall(TResult (VKAPI_PTR* function)(TParameters...), std::type_identity_t<TParameters>... arguments) {
    static_assert(sizeof...(TParameters) <= 6);
    static_assert(std::is_void_v<TResult> || std::is_same_v<TResult, VkResult>);
    if constexpr (!PreservedCallsAvailable) {
        return function(arguments...);
    } else {
        const std::array<std::uint64_t, 6> words{PreservedArgument(arguments)...};
        const auto result = PreservedVulkanCall_nid_no_patch(reinterpret_cast<std::uintptr_t>(function), words[0], words[1], words[2], words[3], words[4], words[5]);
        if constexpr (std::is_same_v<TResult, VkResult>) return static_cast<VkResult>(static_cast<std::int32_t>(static_cast<std::uint32_t>(result)));
    }
}

}

#endif
