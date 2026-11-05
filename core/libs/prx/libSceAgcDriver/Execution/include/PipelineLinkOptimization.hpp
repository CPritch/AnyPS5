#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINELINKOPTIMIZATION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_PIPELINELINKOPTIMIZATION_HPP

#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstdlib>

namespace AgcDriver {

constexpr std::uint32_t NvidiaVendorId = 0x10de;

inline bool NvidiaBlackwellDevice(std::uint32_t vendorId, std::uint32_t deviceId) {
    return vendorId == NvidiaVendorId && deviceId >= 0x2900 && deviceId <= 0x2fff;
}

inline bool PipelineLinkTimeOptimizationUsable(VkDriverId driver) {
    return driver != VK_DRIVER_ID_NVIDIA_PROPRIETARY;
}

inline bool PipelineLinkTimeOptimizationUsable(VkDriverId driver, const VkPhysicalDeviceProperties& properties) {
    if (std::getenv("APS5_NO_PIPELINE_LTO") != nullptr) return false;
    if (std::getenv("APS5_PIPELINE_LTO") != nullptr) return true;
    if (driver == VK_DRIVER_ID_NVIDIA_PROPRIETARY) return NvidiaBlackwellDevice(properties.vendorID, properties.deviceID);
    return PipelineLinkTimeOptimizationUsable(driver);
}

}

#endif
