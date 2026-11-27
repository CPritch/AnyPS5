#include "PipelineBinaryAbi.hpp"
#include "prx/libSceAgcDriver/Graphics/include/PreservedCall.hpp"
#include <SDL_loadso.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using AgcDriver::Graphics::PreservedArgument;
using AgcDriver::Graphics::PreservedCall;

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void Check(VkResult result, const char* message) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(message) + ": " + std::to_string(result));
}

struct Unavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Device {
    void* library = nullptr;
    PFN_vkGetInstanceProcAddr instanceProc = nullptr;
    PFN_vkGetDeviceProcAddr deviceProc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};

    template<typename TFunction>
    TFunction Function(const char* name) const {
        const auto result = device != VK_NULL_HANDLE && deviceProc != nullptr ? reinterpret_cast<TFunction>(deviceProc(device, name)) : reinterpret_cast<TFunction>(instanceProc(instance, name));
        Require(result != nullptr, name);
        return result;
    }

    ~Device() {
        if (device != VK_NULL_HANDLE) {
            if (module != VK_NULL_HANDLE) Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(device, module, nullptr);
            if (layout != VK_NULL_HANDLE) Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(device, layout, nullptr);
            Function<PFN_vkDestroyDevice>("vkDestroyDevice")(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) reinterpret_cast<PFN_vkDestroyInstance>(instanceProc(instance, "vkDestroyInstance"))(instance, nullptr);
        if (library != nullptr) SDL_UnloadObject(library);
    }

    bool Open() {
#ifdef _WIN32
        library = SDL_LoadObject("vulkan-1.dll");
#else
        library = SDL_LoadObject("libvulkan.so.1");
#endif
        if (library == nullptr) throw Unavailable("cannot load Vulkan");
        instanceProc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(library, "vkGetInstanceProcAddr"));
        Require(instanceProc != nullptr, "missing Vulkan instance resolver");
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &application;
        const auto created = reinterpret_cast<PFN_vkCreateInstance>(instanceProc(VK_NULL_HANDLE, "vkCreateInstance"))(&info, nullptr, &instance);
        if (created == VK_ERROR_INCOMPATIBLE_DRIVER) throw Unavailable("no compatible Vulkan driver");
        Check(created, "vkCreateInstance");
        std::uint32_t count = 0;
        const auto enumerate = Function<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        Check(enumerate(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        if (count == 0) throw Unavailable("no Vulkan device");
        std::vector<VkPhysicalDevice> devices(count);
        Check(enumerate(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        const auto lower = [](std::string text) {
            std::ranges::transform(text, text.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            return text;
        };
        const auto* requested = std::getenv("ANYPS5_GPU");
        const auto requestedName = lower(requested != nullptr ? requested : "");
        int best = -1;
        for (const auto candidate : devices) {
            VkPhysicalDeviceProperties queried{};
            Function<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(candidate, &queried);
            if (queried.apiVersion < VK_API_VERSION_1_1 || lower(queried.deviceName).find(requestedName) == std::string::npos) continue;
            const int rank = queried.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : queried.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : queried.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 1 : 0;
            if (rank <= best) continue;
            best = rank;
            physical = candidate;
            properties = queried;
        }
        if (physical == VK_NULL_HANDLE) {
            Require(requestedName.empty(), "no Vulkan device matches ANYPS5_GPU");
            throw Unavailable("no Vulkan 1.1 device");
        }
        const auto extensions = Function<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        Check(extensions(physical, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> available(count);
        Check(extensions(physical, nullptr, &count, available.data()), "vkEnumerateDeviceExtensionProperties");
        const auto has = [&](const char* name) { return std::ranges::any_of(available, [&](const auto& extension) { return std::strcmp(extension.extensionName, name) == 0; }); };
        if (properties.apiVersion >= VK_API_VERSION_1_2 || has(VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME)) {
            VkPhysicalDeviceProperties2 queried{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
            Function<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(physical, &queried);
        }
        const auto* layers = std::getenv("VK_INSTANCE_LAYERS");
        std::printf("device=%s; driver=%s; driver_info=%s; driver_id=%d; driver_version=%u; platform/ABI=%s; compiler=%s; VK_INSTANCE_LAYERS=%s\n", properties.deviceName, driver.driverName, driver.driverInfo, static_cast<int>(driver.driverID), properties.driverVersion, PipelineBinaryAbiPlatform(), __VERSION__, layers != nullptr && *layers != 0 ? layers : "(unset)");
        if (!has(VK_KHR_PIPELINE_BINARY_EXTENSION_NAME)) return false;
        constexpr std::array enabled{VK_KHR_PIPELINE_BINARY_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME};
        for (const auto* name : enabled) Require(has(name), name);
        VkPhysicalDevicePipelineBinaryFeaturesKHR binary{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
        VkPhysicalDeviceMaintenance5FeaturesKHR maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &binary};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &maintenance};
        Function<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical, &features);
        Require(binary.pipelineBinaries == VK_TRUE && maintenance.maintenance5 == VK_TRUE, "pipeline binary or maintenance5 feature is unavailable");
        std::uint32_t familyCount = 0;
        const auto queues = Function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        queues(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        queues(physical, &familyCount, families.data());
        std::uint32_t family = 0;
        while (family < families.size() && (families[family].queueCount == 0 || (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0)) ++family;
        Require(family < families.size(), "no compute queue");
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &maintenance};
        create.queueCreateInfoCount = 1;
        create.pQueueCreateInfos = &queue;
        create.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
        create.ppEnabledExtensionNames = enabled.data();
        Check(Function<PFN_vkCreateDevice>("vkCreateDevice")(physical, &create, nullptr, &device), "vkCreateDevice");
        deviceProc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(instanceProc(instance, "vkGetDeviceProcAddr"));
        Require(deviceProc != nullptr, "vkGetDeviceProcAddr");
        constexpr std::array spirv{0x07230203u, 0x00010000u, 0u, 6u, 0u,
            0x00020011u, 1u, 0x0003000eu, 0u, 1u,
            0x0005000fu, 5u, 4u, 0x6e69616du, 0u,
            0x00060010u, 4u, 17u, 1u, 1u, 1u,
            0x00020013u, 1u, 0x00030021u, 2u, 1u,
            0x00050036u, 1u, 4u, 0u, 2u, 0x000200f8u, 5u, 0x000100fdu, 0x00010038u};
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize = sizeof(spirv);
        moduleInfo.pCode = spirv.data();
        Check(Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        Check(Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
        return true;
    }
};

struct Entry {
    unsigned calls = 0;
    unsigned changedCalls = 0;
    std::map<std::int32_t, unsigned> results;
    std::string example;
};

class Prober {
public:
    explicit Prober(bool preserved) : preserved(preserved) {}

    template<typename TResult, typename... TParameters>
    std::uint64_t Call(const std::string& name, TResult (VKAPI_PTR* function)(TParameters...), std::type_identity_t<TParameters>... arguments) {
        const std::array<std::uint64_t, 6> words{PreservedArgument(arguments)...};
        PipelineBinaryAbiCall call;
        call.seed = 0x0f1e2d3c4b5a6978ull * (++sequence);
        const auto target = reinterpret_cast<std::uintptr_t>(function);
        if (preserved) call = {reinterpret_cast<std::uintptr_t>(&PreservedVulkanCall_nid_no_patch), {target, words[0], words[1], words[2], words[3], words[4], words[5]}, call.seed};
        else call = {target, {words[0], words[1], words[2], words[3], words[4], words[5], 0}, call.seed};
        PipelineBinaryAbiObservation observation;
        PipelineBinaryAbiProbe(&call, &observation);
        unsigned changed = 0;
        const auto differences = PipelineBinaryAbiDifferences(observation, changed);
        auto& entry = entries[name];
        if (std::ranges::find(order, name) == order.end()) order.push_back(name);
        ++entry.calls;
        if constexpr (!std::is_void_v<TResult>) ++entry.results[static_cast<std::int32_t>(static_cast<std::uint32_t>(observation.result))];
        if (changed != 0) {
            ++entry.changedCalls;
            if (entry.example.empty()) entry.example = differences;
        }
        return observation.result;
    }

    unsigned Report() const {
        unsigned changedEntries = 0;
        for (const auto& name : order) {
            const auto& entry = entries.at(name);
            std::string results;
            for (const auto& [result, count] : entry.results) results += (results.empty() ? "" : ",") + std::to_string(result) + "x" + std::to_string(count);
            std::printf("entry=%s; mode=%s; calls=%u; results=%s; changed_calls=%u;%s\n", name.c_str(), preserved ? "preserved" : "raw", entry.calls, results.empty() ? "void" : results.c_str(), entry.changedCalls, entry.example.c_str());
            if (entry.changedCalls != 0) ++changedEntries;
        }
        return changedEntries;
    }

private:
    bool preserved;
    std::uint64_t sequence = 0;
    std::map<std::string, Entry> entries;
    std::vector<std::string> order;
};

VkResult Result(std::uint64_t value) {
    return static_cast<VkResult>(static_cast<std::int32_t>(static_cast<std::uint32_t>(value)));
}

void Iterate(const Device& device, Prober& probe) {
    const auto getKey = device.Function<PFN_vkGetPipelineKeyKHR>("vkGetPipelineKeyKHR");
    const auto createBinaries = device.Function<PFN_vkCreatePipelineBinariesKHR>("vkCreatePipelineBinariesKHR");
    const auto getData = device.Function<PFN_vkGetPipelineBinaryDataKHR>("vkGetPipelineBinaryDataKHR");
    const auto releaseData = device.Function<PFN_vkReleaseCapturedPipelineDataKHR>("vkReleaseCapturedPipelineDataKHR");
    const auto destroyBinary = device.Function<PFN_vkDestroyPipelineBinaryKHR>("vkDestroyPipelineBinaryKHR");
    const auto createPipelines = device.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
    const auto destroyPipeline = device.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline");
    struct Output {
        std::array<std::uint8_t, 64> prefix;
        VkPipelineBinaryKeyKHR key{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        std::array<std::uint8_t, 64> suffix;
    };
    const auto intact = [](const Output& output) {
        const auto guard = [](const auto& bytes) { return std::ranges::all_of(bytes, [](auto value) { return value == 0xa5; }); };
        return guard(output.prefix) && guard(output.suffix) && output.key.sType == VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR && output.key.pNext == nullptr && output.key.keySize != 0 && output.key.keySize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
    };
    Output global;
    global.prefix.fill(0xa5);
    global.suffix.fill(0xa5);
    Check(Result(probe.Call("vkGetPipelineKeyKHR(global)", getKey, device.device, nullptr, &global.key)), "vkGetPipelineKeyKHR(global)");
    Require(intact(global), "the global key output or its guards changed");
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.layout = device.layout;
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, device.module, "main", nullptr};
    VkPipelineCreateInfoKHR keyInfo{VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR, &pipelineInfo};
    Output pipelineKey;
    pipelineKey.prefix.fill(0xa5);
    pipelineKey.suffix.fill(0xa5);
    Check(Result(probe.Call("vkGetPipelineKeyKHR(pipeline)", getKey, device.device, &keyInfo, &pipelineKey.key)), "vkGetPipelineKeyKHR(pipeline)");
    Require(intact(pipelineKey), "the pipeline key output or its guards changed");
    VkPipelineCreateFlags2CreateInfoKHR captureFlags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR, nullptr, VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR};
    auto captureInfo = pipelineInfo;
    captureInfo.pNext = &captureFlags;
    VkPipeline captured = VK_NULL_HANDLE;
    Check(Result(probe.Call("vkCreateComputePipelines(capture)", createPipelines, device.device, VK_NULL_HANDLE, 1u, &captureInfo, nullptr, &captured)), "vkCreateComputePipelines(capture)");
    VkPipelineBinaryCreateInfoKHR fromPipeline{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
    fromPipeline.pipeline = captured;
    VkPipelineBinaryHandlesInfoKHR handles{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    Check(Result(probe.Call("vkCreatePipelineBinariesKHR(pipeline, count)", createBinaries, device.device, &fromPipeline, nullptr, &handles)), "vkCreatePipelineBinariesKHR(pipeline, count)");
    Require(handles.pipelineBinaryCount != 0, "the captured pipeline has no binaries");
    std::vector<VkPipelineBinaryKHR> binaries(handles.pipelineBinaryCount);
    handles.pPipelineBinaries = binaries.data();
    Check(Result(probe.Call("vkCreatePipelineBinariesKHR(pipeline)", createBinaries, device.device, &fromPipeline, nullptr, &handles)), "vkCreatePipelineBinariesKHR(pipeline)");
    std::vector<VkPipelineBinaryKeyKHR> keys(binaries.size(), {VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR});
    std::vector<std::vector<std::byte>> data(binaries.size());
    for (std::size_t i = 0; i < binaries.size(); ++i) {
        VkPipelineBinaryDataInfoKHR dataInfo{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, nullptr, binaries[i]};
        std::size_t size = 0;
        Check(Result(probe.Call("vkGetPipelineBinaryDataKHR(size)", getData, device.device, &dataInfo, &keys[i], &size, nullptr)), "vkGetPipelineBinaryDataKHR(size)");
        data[i].resize(size);
        Check(Result(probe.Call("vkGetPipelineBinaryDataKHR(data)", getData, device.device, &dataInfo, &keys[i], &size, data[i].data())), "vkGetPipelineBinaryDataKHR(data)");
        data[i].resize(size);
    }
    VkReleaseCapturedPipelineDataInfoKHR release{VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR, nullptr, captured};
    Check(Result(probe.Call("vkReleaseCapturedPipelineDataKHR", releaseData, device.device, &release, nullptr)), "vkReleaseCapturedPipelineDataKHR");
    PreservedCall(destroyPipeline, device.device, captured, nullptr);
    std::vector<VkPipelineBinaryDataKHR> stored(binaries.size());
    for (std::size_t i = 0; i < binaries.size(); ++i) stored[i] = {data[i].size(), data[i].data()};
    VkPipelineBinaryKeysAndDataKHR keysAndData{static_cast<std::uint32_t>(binaries.size()), keys.data(), stored.data()};
    VkPipelineBinaryCreateInfoKHR fromData{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
    fromData.pKeysAndDataInfo = &keysAndData;
    std::vector<VkPipelineBinaryKHR> imported(binaries.size());
    VkPipelineBinaryHandlesInfoKHR importedHandles{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR, nullptr, static_cast<std::uint32_t>(imported.size()), imported.data()};
    Check(Result(probe.Call("vkCreatePipelineBinariesKHR(data)", createBinaries, device.device, &fromData, nullptr, &importedHandles)), "vkCreatePipelineBinariesKHR(data)");
    VkPipelineBinaryInfoKHR binaryInfo{VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, nullptr, static_cast<std::uint32_t>(imported.size()), imported.data()};
    auto restoreInfo = pipelineInfo;
    restoreInfo.pNext = &binaryInfo;
    VkPipeline restored = VK_NULL_HANDLE;
    Check(Result(probe.Call("vkCreateComputePipelines(VkPipelineBinaryInfoKHR)", createPipelines, device.device, VK_NULL_HANDLE, 1u, &restoreInfo, nullptr, &restored)), "vkCreateComputePipelines(VkPipelineBinaryInfoKHR)");
    PreservedCall(destroyPipeline, device.device, restored, nullptr);
    for (const auto binary : binaries) probe.Call("vkDestroyPipelineBinaryKHR", destroyBinary, device.device, binary, nullptr);
    for (const auto binary : imported) probe.Call("vkDestroyPipelineBinaryKHR", destroyBinary, device.device, binary, nullptr);
}

}

int main(int argc, char** argv) try {
    Require(argc >= 2 && (std::string_view(argv[1]) == "raw" || std::string_view(argv[1]) == "preserved"), "expected raw or preserved, then an optional iteration count");
    const unsigned iterations = argc >= 3 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 3u;
    Require(iterations != 0 && iterations <= 1000, "the iteration count must be from 1 through 1000");
    Device device;
    if (!device.Open()) {
        std::puts("skipped, the device lacks VK_KHR_pipeline_binary");
        return 77;
    }
    Prober probe(std::string_view(argv[1]) == "preserved");
    for (unsigned iteration = 0; iteration < iterations; ++iteration) Iterate(device, probe);
    const auto changed = probe.Report();
    std::printf("iterations=%u; entry_points_with_changed_registers=%u\n", iterations, changed);
    return changed == 0 ? 0 : 1;
} catch (const Unavailable& error) {
    if (std::getenv("ANYPS5_REQUIRE_VULKAN") != nullptr) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::printf("skipped, no usable Vulkan device: %s\n", error.what());
    return 77;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
