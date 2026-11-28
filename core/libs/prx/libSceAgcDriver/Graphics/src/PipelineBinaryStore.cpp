#include "prx/libSceAgcDriver/Graphics/include/PipelineBinaryStore.hpp"
#include "ShaderCacheDirectory.hpp"
#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace AgcDriver::Graphics {
namespace {

constexpr std::uint64_t FileMagic = 0x3259425035535041ull;
constexpr std::uint64_t KeyFormat = 2;
constexpr std::size_t MaximumFileBytes = 256u * 1024u * 1024u;
constexpr std::uint32_t MaximumBinaries = 64;
constexpr std::uint64_t CaptureFailureMarker = 1;

std::uint64_t configured(const char* name, std::uint64_t fallback) {
    const auto* value = std::getenv(name);
    if (value == nullptr) return fallback;
    std::uint64_t result = 0;
    const auto end = value + std::strlen(value);
    const auto parsed = std::from_chars(value, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end ? result : fallback;
}

void number(std::vector<std::byte>& output, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) output.push_back(static_cast<std::byte>(value >> (i * 8u)));
}

void bytes(std::vector<std::byte>& output, std::span<const std::byte> value) {
    number(output, value.size());
    output.insert(output.end(), value.begin(), value.end());
}

void real(std::vector<std::byte>& output, float value) {
    number(output, std::bit_cast<std::uint32_t>(value));
}

bool validKey(const VkPipelineBinaryKeyKHR& key) {
    return key.keySize != 0 && key.keySize <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
}

const char* queryKey(PFN_vkGetPipelineKeyKHR getKey, VkDevice device, const VkPipelineCreateInfoKHR* info, VkPipelineBinaryKeyKHR& key, VkResult& result) {
    auto output = std::make_unique<PipelineBinaryKeyOutput>();
    output->guard.fill(0xa5);
    result = PreservedCall(getKey, device, info, &output->key);
    if (result != VK_SUCCESS) return "result";
    if (!std::ranges::all_of(output->guard, [](auto byte) { return byte == 0xa5; })) return "output overrun";
    if (output->key.sType != VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR || output->key.pNext != nullptr) return "header";
    if (!validKey(output->key)) return "size";
    key = output->key;
    return nullptr;
}

void binaryKey(std::vector<std::byte>& output, const VkPipelineBinaryKeyKHR& key) {
    bytes(output, std::as_bytes(std::span(key.key, key.keySize)));
}

struct PipelineChain {
    std::optional<VkPipelineCreateFlags2CreateInfoKHR> flags;
    std::optional<VkPipelineCreationFeedbackCreateInfo> feedback;
    bool known = true;
};

PipelineChain pipelineChain(const void* next) {
    PipelineChain chain;
    for (unsigned depth = 0; next != nullptr; ++depth) {
        const auto* header = static_cast<const VkBaseInStructure*>(next);
        if (depth == 8) chain.known = false;
        else if (header->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR && !chain.flags) chain.flags = *static_cast<const VkPipelineCreateFlags2CreateInfoKHR*>(next);
        else if (header->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO && !chain.feedback) chain.feedback = *static_cast<const VkPipelineCreationFeedbackCreateInfo*>(next);
        else chain.known = false;
        if (!chain.known) return chain;
        next = header->pNext;
    }
    return chain;
}

struct RebuiltChain {
    VkPipelineCreateFlags2CreateInfoKHR flags{VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
    VkPipelineCreationFeedbackCreateInfo feedback{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};

    const void* Link(const PipelineChain& chain, bool withFlags, VkPipelineCreateFlags2KHR flagBits, bool withFeedback) {
        const void* head = nullptr;
        if (withFeedback && chain.feedback) {
            feedback = *chain.feedback;
            feedback.pNext = nullptr;
            head = &feedback;
        }
        if (withFlags) {
            flags.flags = flagBits;
            flags.pNext = head;
            head = &flags;
        }
        return head;
    }
};

VkPipelineCreateFlags2KHR pipelineFlags(const VkComputePipelineCreateInfo& info, const PipelineChain& chain) {
    return chain.flags ? chain.flags->flags : static_cast<VkPipelineCreateFlags2KHR>(info.flags);
}

bool sampler(std::vector<std::byte>& key, const VkSamplerCreateInfo* info) {
    number(key, info != nullptr);
    if (info == nullptr) return true;
    if (info->sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO) return false;
    number(key, info->flags);
    number(key, info->magFilter);
    number(key, info->minFilter);
    number(key, info->mipmapMode);
    number(key, info->addressModeU);
    number(key, info->addressModeV);
    number(key, info->addressModeW);
    real(key, info->mipLodBias);
    number(key, info->anisotropyEnable);
    real(key, info->maxAnisotropy);
    number(key, info->compareEnable);
    number(key, info->compareOp);
    real(key, info->minLod);
    real(key, info->maxLod);
    number(key, info->borderColor);
    number(key, info->unnormalizedCoordinates);
    bool reduction = false;
    bool custom = false;
    for (const auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next != nullptr; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO && !reduction) {
            reduction = true;
            number(key, next->sType);
            number(key, reinterpret_cast<const VkSamplerReductionModeCreateInfo*>(next)->reductionMode);
        } else if (next->sType == VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT && !custom) {
            custom = true;
            const auto& border = *reinterpret_cast<const VkSamplerCustomBorderColorCreateInfoEXT*>(next);
            number(key, next->sType);
            bytes(key, std::as_bytes(std::span(border.customBorderColor.uint32)));
            number(key, border.format);
        } else {
            return false;
        }
    }
    number(key, 0);
    return true;
}

bool mapping(std::vector<std::byte>& key, const VkDescriptorSetAndBindingMappingEXT& entry) {
    if (entry.sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT || entry.pNext != nullptr) return false;
    number(key, entry.descriptorSet);
    number(key, entry.firstBinding);
    number(key, entry.bindingCount);
    number(key, entry.resourceMask);
    number(key, entry.source);
    const auto& data = entry.sourceData;
    switch (entry.source) {
    case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT: {
        const auto& value = data.constantOffset;
        for (const auto word : {value.heapOffset, value.heapArrayStride, value.samplerHeapOffset, value.samplerHeapArrayStride}) number(key, word);
        return sampler(key, value.pEmbeddedSampler);
    }
    case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT: {
        const auto& value = data.pushIndex;
        for (const auto word : {value.heapOffset, value.pushOffset, value.heapIndexStride, value.heapArrayStride, value.useCombinedImageSamplerIndex, value.samplerHeapOffset, value.samplerPushOffset, value.samplerHeapIndexStride, value.samplerHeapArrayStride}) number(key, word);
        return sampler(key, value.pEmbeddedSampler);
    }
    case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_EXT: {
        const auto& value = data.indirectIndex;
        for (const auto word : {value.heapOffset, value.pushOffset, value.addressOffset, value.heapIndexStride, value.heapArrayStride, value.useCombinedImageSamplerIndex, value.samplerHeapOffset, value.samplerPushOffset, value.samplerAddressOffset, value.samplerHeapIndexStride, value.samplerHeapArrayStride}) number(key, word);
        return sampler(key, value.pEmbeddedSampler);
    }
    case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_INDIRECT_INDEX_ARRAY_EXT: {
        const auto& value = data.indirectIndexArray;
        for (const auto word : {value.heapOffset, value.pushOffset, value.addressOffset, value.heapIndexStride, value.useCombinedImageSamplerIndex, value.samplerHeapOffset, value.samplerPushOffset, value.samplerAddressOffset, value.samplerHeapIndexStride}) number(key, word);
        return sampler(key, value.pEmbeddedSampler);
    }
    case VK_DESCRIPTOR_MAPPING_SOURCE_RESOURCE_HEAP_DATA_EXT:
        number(key, data.heapData.heapOffset);
        number(key, data.heapData.pushOffset);
        return true;
    case VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_DATA_EXT:
        number(key, data.pushDataOffset);
        return true;
    case VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT:
        number(key, data.pushAddressOffset);
        return true;
    case VK_DESCRIPTOR_MAPPING_SOURCE_INDIRECT_ADDRESS_EXT:
        number(key, data.indirectAddress.pushOffset);
        number(key, data.indirectAddress.addressOffset);
        return true;
    case VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_SHADER_RECORD_INDEX_EXT: {
        const auto& value = data.shaderRecordIndex;
        for (const auto word : {value.heapOffset, value.shaderRecordOffset, value.heapIndexStride, value.heapArrayStride, value.useCombinedImageSamplerIndex, value.samplerHeapOffset, value.samplerShaderRecordOffset, value.samplerHeapIndexStride, value.samplerHeapArrayStride}) number(key, word);
        return sampler(key, value.pEmbeddedSampler);
    }
    case VK_DESCRIPTOR_MAPPING_SOURCE_SHADER_RECORD_DATA_EXT:
        number(key, data.shaderRecordDataOffset);
        return true;
    case VK_DESCRIPTOR_MAPPING_SOURCE_SHADER_RECORD_ADDRESS_EXT:
        number(key, data.shaderRecordAddressOffset);
        return true;
    default:
        return false;
    }
}

bool stageChain(std::vector<std::byte>& key, const void* next) {
    std::uint32_t subgroupSize = 0;
    bool subgroup = false;
    bool mapped = false;
    std::vector<std::byte> mappings;
    for (unsigned depth = 0; next != nullptr; ++depth) {
        const auto* header = static_cast<const VkBaseInStructure*>(next);
        if (depth == 8) return false;
        if (header->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO && !subgroup) {
            subgroup = true;
            subgroupSize = static_cast<const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo*>(next)->requiredSubgroupSize;
        } else if (header->sType == VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT && !mapped) {
            mapped = true;
            const auto& info = *static_cast<const VkShaderDescriptorSetAndBindingMappingInfoEXT*>(next);
            if (info.mappingCount != 0 && info.pMappings == nullptr) return false;
            number(mappings, info.mappingCount);
            for (std::uint32_t i = 0; i < info.mappingCount; ++i) {
                if (!mapping(mappings, info.pMappings[i])) return false;
            }
        } else {
            return false;
        }
        next = header->pNext;
    }
    number(key, subgroup);
    number(key, subgroupSize);
    number(key, mapped);
    key.insert(key.end(), mappings.begin(), mappings.end());
    return true;
}

struct InvalidFile {};

class Reader {
public:
    explicit Reader(std::span<const std::byte> data) : data(data) {}
    std::uint64_t Number() {
        if (data.size() < 8) throw InvalidFile{};
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(data[i]) << (i * 8u);
        data = data.subspan(8);
        return value;
    }
    std::span<const std::byte> Bytes() {
        const auto length = Number();
        if (length > data.size()) throw InvalidFile{};
        const auto result = data.first(static_cast<std::size_t>(length));
        data = data.subspan(static_cast<std::size_t>(length));
        return result;
    }
    bool Empty() const { return data.empty(); }

private:
    std::span<const std::byte> data;
};

struct BinaryHandles {
    VkDevice device;
    PFN_vkDestroyPipelineBinaryKHR destroy;
    std::vector<VkPipelineBinaryKHR> handles;
    ~BinaryHandles() {
        for (const auto handle : handles) if (handle != VK_NULL_HANDLE) PreservedCall(destroy, device, handle, nullptr);
    }
};

}

const char* PipelineBinaryOutcomeName(PipelineBinaryOutcome outcome) {
    switch (outcome) {
    case PipelineBinaryOutcome::Restored: return "restored";
    case PipelineBinaryOutcome::Captured: return "captured";
    case PipelineBinaryOutcome::Uncaptured: return "uncaptured";
    case PipelineBinaryOutcome::Ordinary: return "none";
    }
    return "none";
}

VkDriverId PipelineBinaryStore::DriverIdentity(VkPhysicalDevice physical, std::uint32_t apiVersion, bool driverPropertiesExtension, PFN_vkGetPhysicalDeviceProperties2 query) {
    if ((!driverPropertiesExtension && apiVersion < VK_API_VERSION_1_2) || query == nullptr) return VK_DRIVER_ID_MAX_ENUM;
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    driver.driverID = VK_DRIVER_ID_MAX_ENUM;
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    query(physical, &properties);
    return driver.driverID;
}

PipelineBinaryPolicy PipelineBinaryStore::PolicyForDriver(VkDriverId driverId) {
    return PolicyFor(driverId, PreservedCallsAvailable);
}

PipelineBinaryPolicy PipelineBinaryStore::PolicyFor(VkDriverId driverId, bool preservedCalls) {
    if (std::getenv("APS5_NO_PIPELINE_BINARIES") != nullptr) return PipelineBinaryPolicy::Disabled;
    if (driverId != VK_DRIVER_ID_NVIDIA_PROPRIETARY || preservedCalls) return PipelineBinaryPolicy::Enabled;
    const auto* unsafe = std::getenv("APS5_UNSAFE_PIPELINE_BINARIES");
    return unsafe != nullptr && std::strcmp(unsafe, "1") == 0 ? PipelineBinaryPolicy::Enabled : PipelineBinaryPolicy::DisabledByDriver;
}

void PipelineBinaryStore::ReportPolicy(PipelineBinaryPolicy policy, VkDriverId driverId) {
    if (policy == PipelineBinaryPolicy::DisabledByDriver) {
        std::fprintf(stderr, "[pipeline-binary] disabled: NVIDIA proprietary driver (vkGetPipelineKeyKHR changes callee-saved registers) and no register-preserving call on this platform; set APS5_UNSAFE_PIPELINE_BINARIES=1 to override\n");
    } else if (policy == PipelineBinaryPolicy::Enabled && driverId == VK_DRIVER_ID_NVIDIA_PROPRIETARY && PreservedCallsAvailable) {
        std::fprintf(stderr, "[pipeline-binary] NVIDIA proprietary driver: pipeline binary calls save and restore every callee-saved register\n");
    }
}

bool PipelineBinaryStore::LayersCompatible(VkPhysicalDevice physical, PFN_vkEnumerateDeviceLayerProperties enumerate) {
    if (enumerate == nullptr) {
        std::fprintf(stderr, "[pipeline-binary] disabled: active layer enumeration unavailable; using the driver pipeline cache\n");
        return false;
    }
    VkResult result = VK_INCOMPLETE;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        std::uint32_t count = 0;
        result = enumerate(physical, &count, nullptr);
        if (result == VK_INCOMPLETE) continue;
        if (result != VK_SUCCESS) break;
        if (count == 0) return true;
        std::vector<VkLayerProperties> layers(count);
        result = enumerate(physical, &count, layers.data());
        if (result == VK_INCOMPLETE) continue;
        if (result != VK_SUCCESS) break;
        layers.resize(count);
        for (const auto& layer : layers) {
            if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") != 0 && std::strcmp(layer.layerName, "VK_LAYER_LUNARG_gfxreconstruct") != 0) continue;
            if (layer.specVersion >= VK_MAKE_API_VERSION(0, 1, 3, 294)) continue;
            std::fprintf(stderr, "[pipeline-binary] disabled: active layer %s reports Vulkan %u.%u.%u (< 1.3.294); using the driver pipeline cache\n",
                layer.layerName, VK_API_VERSION_MAJOR(layer.specVersion), VK_API_VERSION_MINOR(layer.specVersion), VK_API_VERSION_PATCH(layer.specVersion));
            return false;
        }
        return true;
    }
    if (result == VK_INCOMPLETE) {
        std::fprintf(stderr, "[pipeline-binary] disabled: active layer enumeration remained VK_INCOMPLETE after 3 attempts; using the driver pipeline cache\n");
    } else {
        std::fprintf(stderr, "[pipeline-binary] disabled: active layer enumeration failed (VkResult %d); using the driver pipeline cache\n", static_cast<int>(result));
    }
    return false;
}

PipelineBinaryStore::PipelineBinaryStore(const Context& context, const VkPhysicalDeviceProperties& properties, VkDriverId driverId, bool preservedCalls) : context(context), policy(PolicyFor(driverId, preservedCalls)), properties(properties) {
    if (policy != PipelineBinaryPolicy::Enabled) return;
    minimumWords = static_cast<std::size_t>(std::min<std::uint64_t>(configured("APS5_PIPELINE_BINARY_MIN_WORDS", minimumWords), std::numeric_limits<std::size_t>::max()));
    minimumMilliseconds = configured("APS5_PIPELINE_BINARY_MIN_MS", minimumMilliseconds);
    const auto root = ShaderRecompiler::ShaderCacheDirectory();
    if (root.empty()) return;
    getKey = reinterpret_cast<PFN_vkGetPipelineKeyKHR>(context.deviceProc(context.device, "vkGetPipelineKeyKHR"));
    createBinaries = reinterpret_cast<PFN_vkCreatePipelineBinariesKHR>(context.deviceProc(context.device, "vkCreatePipelineBinariesKHR"));
    destroyBinary = reinterpret_cast<PFN_vkDestroyPipelineBinaryKHR>(context.deviceProc(context.device, "vkDestroyPipelineBinaryKHR"));
    getData = reinterpret_cast<PFN_vkGetPipelineBinaryDataKHR>(context.deviceProc(context.device, "vkGetPipelineBinaryDataKHR"));
    releaseData = reinterpret_cast<PFN_vkReleaseCapturedPipelineDataKHR>(context.deviceProc(context.device, "vkReleaseCapturedPipelineDataKHR"));
    createPipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(context.deviceProc(context.device, "vkCreateComputePipelines"));
    destroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(context.deviceProc(context.device, "vkDestroyPipeline"));
    if (getKey == nullptr || createBinaries == nullptr || destroyBinary == nullptr || getData == nullptr || releaseData == nullptr || createPipelines == nullptr || destroyPipeline == nullptr) return;
    VkResult result = VK_SUCCESS;
    if (const auto* reason = queryKey(getKey, context.device, nullptr, globalKey, result)) {
        std::fprintf(stderr, "[pipeline-binary] disabled: unusable global key (result=%d, %s); using the driver pipeline cache\n", static_cast<int>(result), reason);
        return;
    }
    directory = root / "driver" / "binaries";
}

std::vector<std::byte> PipelineBinaryStore::Key(const VkComputePipelineCreateInfo& info, std::span<const std::uint32_t> spirv, std::span<const std::uint32_t> layout, std::array<std::uint64_t, 6>* parts) const {
    if (directory.empty() || spirv.empty() || layout.empty() || info.stage.pName == nullptr || info.basePipelineHandle != VK_NULL_HANDLE ||
        (info.flags & (VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT)) != 0) return {};
    const auto chain = pipelineChain(info.pNext);
    if (!chain.known) return {};
    const auto flags = pipelineFlags(info, chain);
    if ((flags & (VK_PIPELINE_CREATE_2_DERIVATIVE_BIT_KHR | VK_PIPELINE_CREATE_2_ALLOW_DERIVATIVES_BIT_KHR | VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR | VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR)) != 0) return {};
    std::vector<std::byte> stage;
    if (!stageChain(stage, info.stage.pNext)) return {};
    RebuiltChain queryChain;
    auto keyInfo = info;
    keyInfo.pNext = queryChain.Link(chain, chain.flags.has_value(), flags, false);
    VkPipelineCreateInfoKHR pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR, &keyInfo};
    VkPipelineBinaryKeyKHR pipelineKey{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
    VkResult result = VK_SUCCESS;
    if (queryKey(getKey, context.device, &pipelineInfo, pipelineKey, result) != nullptr) return {};
    std::vector<std::byte> key;
    number(key, KeyFormat);
    number(key, properties.vendorID);
    number(key, properties.deviceID);
    number(key, properties.driverVersion);
    bytes(key, std::as_bytes(std::span(properties.pipelineCacheUUID)));
    binaryKey(key, globalKey);
    binaryKey(key, pipelineKey);
    bytes(key, std::as_bytes(spirv));
    bytes(key, std::as_bytes(layout));
    number(key, info.flags);
    number(key, chain.flags.has_value());
    number(key, flags);
    number(key, info.layout != VK_NULL_HANDLE);
    number(key, static_cast<std::uint32_t>(info.basePipelineIndex));
    number(key, info.stage.flags);
    number(key, info.stage.stage);
    bytes(key, stage);
    bytes(key, std::as_bytes(std::span(info.stage.pName, std::strlen(info.stage.pName))));
    const auto* specialization = info.stage.pSpecializationInfo;
    number(key, specialization != nullptr);
    if (specialization != nullptr) {
        if ((specialization->mapEntryCount != 0 && specialization->pMapEntries == nullptr) || (specialization->dataSize != 0 && specialization->pData == nullptr)) return {};
        number(key, specialization->mapEntryCount);
        for (std::uint32_t i = 0; i < specialization->mapEntryCount; ++i) {
            const auto& entry = specialization->pMapEntries[i];
            if (entry.offset > specialization->dataSize || entry.size > specialization->dataSize - entry.offset) return {};
            number(key, entry.constantID);
            number(key, entry.offset);
            number(key, entry.size);
        }
        bytes(key, {static_cast<const std::byte*>(specialization->pData), specialization->dataSize});
    }
    if (parts != nullptr) {
        std::vector<std::byte> driverKey;
        binaryKey(driverKey, pipelineKey);
        std::vector<std::byte> flagWords;
        number(flagWords, info.flags);
        number(flagWords, flags);
        number(flagWords, info.stage.flags);
        *parts = {ShaderRecompiler::HashBytes(driverKey), ShaderRecompiler::HashBytes(std::as_bytes(spirv)), ShaderRecompiler::HashBytes(std::as_bytes(layout)), ShaderRecompiler::HashBytes(stage),
            specialization != nullptr ? ShaderRecompiler::HashBytes({static_cast<const std::byte*>(specialization->pData), specialization->dataSize}) : 0u, ShaderRecompiler::HashBytes(flagWords)};
    }
    return key;
}

std::filesystem::path PipelineBinaryStore::filePath(std::span<const std::byte> key) const {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(ShaderRecompiler::HashBytes(key)));
    return directory / name;
}

PipelineBinaryStore::RestoreResult PipelineBinaryStore::restore(const VkComputePipelineCreateInfo& info, std::span<const std::byte> key, VkPipeline* pipeline) const {
    auto path = filePath(key);
    const auto result = restoreFile(info, key, pipeline, path);
    if (result != RestoreResult::Missing) return result;
    path += ".threshold";
    return restoreFile(info, key, pipeline, path);
}

PipelineBinaryStore::RestoreResult PipelineBinaryStore::restoreFile(const VkComputePipelineCreateInfo& info, std::span<const std::byte> key, VkPipeline* pipeline, const std::filesystem::path& path) const try {
    const auto lookupStarted = std::chrono::steady_clock::now();
    std::error_code error;
#ifdef _WIN32
    const auto native = ShaderRecompiler::NativeCachePath_nid_no_patch(path, error);
    if (error) return RestoreResult::Missing;
#else
    const auto& native = path;
#endif
    const auto size = std::filesystem::file_size(native, error);
    if (error || size > MaximumFileBytes) return RestoreResult::Missing;
    std::vector<std::byte> file;
    if (!ShaderRecompiler::ReadWholeFile(path, file) || file.size() < 8) return RestoreResult::Missing;
    const auto payload = std::span(file).first(file.size() - 8);
    Reader checksum{std::span(file).last(8)};
    if (checksum.Number() != ShaderRecompiler::HashBytes(payload)) throw InvalidFile{};
    Reader reader(payload);
    if (reader.Number() != FileMagic || !std::ranges::equal(reader.Bytes(), key)) throw InvalidFile{};
    const auto count = reader.Number();
    if (count == 0) {
        const auto threshold = reader.Number();
        const auto kind = reader.Empty() ? 0 : reader.Number();
        if (!reader.Empty()) throw InvalidFile{};
        if (kind == CaptureFailureMarker) return RestoreResult::CaptureFailed;
        return minimumMilliseconds >= threshold ? RestoreResult::BelowThreshold : RestoreResult::Missing;
    }
    if (count > MaximumBinaries) throw InvalidFile{};
    std::vector<VkPipelineBinaryKeyKHR> keys(static_cast<std::size_t>(count), {VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR});
    std::vector<VkPipelineBinaryDataKHR> data(static_cast<std::size_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const auto storedKey = reader.Bytes();
        if (storedKey.empty() || storedKey.size() > VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR) throw InvalidFile{};
        keys[i].keySize = static_cast<std::uint32_t>(storedKey.size());
        std::memcpy(keys[i].key, storedKey.data(), storedKey.size());
        const auto storedData = reader.Bytes();
        if (storedData.empty()) throw InvalidFile{};
        data[i] = {storedData.size(), const_cast<std::byte*>(storedData.data())};
    }
    if (!reader.Empty()) throw InvalidFile{};
    BinaryHandles binaries{context.device, destroyBinary, std::vector<VkPipelineBinaryKHR>(static_cast<std::size_t>(count))};
    VkPipelineBinaryKeysAndDataKHR keysAndData{static_cast<std::uint32_t>(count), keys.data(), data.data()};
    VkPipelineBinaryCreateInfoKHR binaryInfo{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
    binaryInfo.pKeysAndDataInfo = &keysAndData;
    VkPipelineBinaryHandlesInfoKHR handles{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR, nullptr, static_cast<std::uint32_t>(count), binaries.handles.data()};
    const auto imported = PreservedCall(createBinaries, context.device, &binaryInfo, nullptr, &handles);
    if (imported != VK_SUCCESS || handles.pipelineBinaryCount != count || std::ranges::find(binaries.handles, VK_NULL_HANDLE) != binaries.handles.end()) {
        std::fprintf(stderr, "[pipeline-binary] import rejected key=%016llx result=%d; rebuilding\n", static_cast<unsigned long long>(ShaderRecompiler::HashBytes(key)), static_cast<int>(imported));
        return RestoreResult::Missing;
    }
    const auto chain = pipelineChain(info.pNext);
    RebuiltChain restoredChain;
    VkPipelineBinaryInfoKHR binary{VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, restoredChain.Link(chain, chain.flags.has_value(), pipelineFlags(info, chain), true), static_cast<std::uint32_t>(count), binaries.handles.data()};
    auto restoredInfo = info;
    restoredInfo.pNext = &binary;
    const auto started = std::chrono::steady_clock::now();
    const auto result = PreservedCall(createPipelines, context.device, VK_NULL_HANDLE, 1u, &restoredInfo, nullptr, pipeline);
    if (result != VK_SUCCESS) {
        if (*pipeline != VK_NULL_HANDLE) destroyPipeline(context.device, *pipeline, nullptr);
        *pipeline = VK_NULL_HANDLE;
        std::fprintf(stderr, "[pipeline-binary] restore rejected key=%016llx result=%d; rebuilding\n", static_cast<unsigned long long>(ShaderRecompiler::HashBytes(key)), static_cast<int>(result));
        return RestoreResult::Missing;
    }
    std::fprintf(stderr, "[pipeline-binary] loaded key=%016llx bytes=%zu create_ms=%.3f total_ms=%.3f\n", static_cast<unsigned long long>(ShaderRecompiler::HashBytes(key)), file.size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lookupStarted).count());
    return RestoreResult::Restored;
} catch (const InvalidFile&) {
    std::fprintf(stderr, "[pipeline-binary] invalid store key=%016llx; rebuilding\n", static_cast<unsigned long long>(ShaderRecompiler::HashBytes(key)));
    return RestoreResult::Missing;
}

bool PipelineBinaryStore::capture(VkPipeline pipeline, std::span<const std::byte> key) const {
    VkPipelineBinaryCreateInfoKHR info{VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR};
    info.pipeline = pipeline;
    VkPipelineBinaryHandlesInfoKHR output{VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    if (PreservedCall(createBinaries, context.device, &info, nullptr, &output) != VK_SUCCESS || output.pipelineBinaryCount == 0 || output.pipelineBinaryCount > MaximumBinaries) return false;
    BinaryHandles binaries{context.device, destroyBinary, std::vector<VkPipelineBinaryKHR>(output.pipelineBinaryCount)};
    const auto count = output.pipelineBinaryCount;
    output.pPipelineBinaries = binaries.handles.data();
    if (PreservedCall(createBinaries, context.device, &info, nullptr, &output) != VK_SUCCESS || output.pipelineBinaryCount != count || std::ranges::find(binaries.handles, VK_NULL_HANDLE) != binaries.handles.end()) return false;
    std::vector<std::byte> file;
    number(file, FileMagic);
    bytes(file, key);
    number(file, count);
    for (const auto binary : binaries.handles) {
        VkPipelineBinaryDataInfoKHR dataInfo{VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, nullptr, binary};
        VkPipelineBinaryKeyKHR binaryId{VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        std::size_t size = 0;
        if (PreservedCall(getData, context.device, &dataInfo, &binaryId, &size, nullptr) != VK_SUCCESS || !validKey(binaryId) || size == 0 || size > MaximumFileBytes) return false;
        std::vector<std::byte> data(size);
        if (PreservedCall(getData, context.device, &dataInfo, &binaryId, &size, data.data()) != VK_SUCCESS || !validKey(binaryId) || size == 0 || size > data.size()) return false;
        data.resize(size);
        if (file.size() > MaximumFileBytes - data.size() || MaximumFileBytes - file.size() - data.size() < 64) return false;
        binaryKey(file, binaryId);
        bytes(file, data);
    }
    number(file, ShaderRecompiler::HashBytes(file));
    const auto path = filePath(key);
    const bool stored = ShaderRecompiler::WriteFileAtomically(path, file);
    if (stored) std::fprintf(stderr, "[pipeline-binary] saved key=%016llx bytes=%zu path=%s\n", static_cast<unsigned long long>(ShaderRecompiler::HashBytes(key)), file.size(), path.string().c_str());
    else std::fprintf(stderr, "[pipeline-binary] cannot save key=%016llx path=%s\n", static_cast<unsigned long long>(ShaderRecompiler::HashBytes(key)), path.string().c_str());
    return stored;
}

void PipelineBinaryStore::rememberThreshold(std::span<const std::byte> key, bool captureFailed) const try {
    std::vector<std::byte> file;
    number(file, FileMagic);
    bytes(file, key);
    number(file, 0);
    number(file, minimumMilliseconds);
    if (captureFailed) number(file, CaptureFailureMarker);
    number(file, ShaderRecompiler::HashBytes(file));
    auto path = filePath(key);
    path += ".threshold";
    if (file.size() <= MaximumFileBytes && ShaderRecompiler::WriteFileAtomically(path, file) && captureFailed) {
        std::fprintf(stderr, "[pipeline-binary] capture failed key=%016llx; marked for driver cache builds\n", static_cast<unsigned long long>(ShaderRecompiler::HashBytes(key)));
    }
} catch (const std::bad_alloc&) {
    std::fputs("[pipeline-binary] marker skipped: host memory exhausted\n", stderr);
}

PipelineBinaryOutcome PipelineBinaryStore::CreateComputePipeline(const VkComputePipelineCreateInfo& info, std::span<const std::uint32_t> spirv, std::span<const std::uint32_t> layout, VkPipelineCache cache, PipelineCacheSynchronization* synchronization, VkPipeline* pipeline) const {
    *pipeline = VK_NULL_HANDLE;
    const auto create = context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
    std::vector<std::byte> key;
    std::array<std::uint64_t, 6> parts{};
    try {
        if (spirv.size() >= minimumWords) key = Key(info, spirv, layout, &parts);
        if (!key.empty()) {
            const auto result = restore(info, key, pipeline);
            if (result == RestoreResult::Restored) return PipelineBinaryOutcome::Restored;
            if (result == RestoreResult::BelowThreshold || result == RestoreResult::CaptureFailed) key.clear();
        }
    } catch (const std::bad_alloc&) {
        key.clear();
        std::fputs("[pipeline-binary] lookup skipped: host memory exhausted\n", stderr);
    }
    if (!key.empty()) {
        const auto chain = pipelineChain(info.pNext);
        RebuiltChain captureChain;
        auto captureInfo = info;
        captureInfo.pNext = captureChain.Link(chain, true, pipelineFlags(info, chain) | VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR, true);
        const auto started = std::chrono::steady_clock::now();
        const auto result = PreservedCall(createPipelines, context.device, VK_NULL_HANDLE, 1u, &captureInfo, nullptr, pipeline);
        const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (result == VK_SUCCESS) {
            struct Release {
                const PipelineBinaryStore& store;
                VkPipeline pipeline;
                ~Release() {
                    VkReleaseCapturedPipelineDataInfoKHR release{VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR, nullptr, pipeline};
                    PreservedCall(store.releaseData, store.context.device, &release, nullptr);
                }
            } release{*this, *pipeline};
            std::fprintf(stderr, "[pipeline-binary] built key=%016llx words=%zu build_ms=%.3f driver=%016llx spirv=%016llx layout=%016llx stage=%016llx specialization=%016llx flags=%016llx\n", static_cast<unsigned long long>(ShaderRecompiler::HashBytes(key)), spirv.size(), milliseconds,
                static_cast<unsigned long long>(parts[0]), static_cast<unsigned long long>(parts[1]), static_cast<unsigned long long>(parts[2]), static_cast<unsigned long long>(parts[3]), static_cast<unsigned long long>(parts[4]), static_cast<unsigned long long>(parts[5]));
            try {
                if (milliseconds >= static_cast<double>(minimumMilliseconds)) {
                    if (capture(*pipeline, key)) return PipelineBinaryOutcome::Captured;
                    rememberThreshold(key, true);
                }
                else rememberThreshold(key);
            } catch (const std::bad_alloc&) {
                std::fputs("[pipeline-binary] capture skipped: host memory exhausted\n", stderr);
                if (milliseconds >= static_cast<double>(minimumMilliseconds)) rememberThreshold(key, true);
            }
            return PipelineBinaryOutcome::Uncaptured;
        }
        if (*pipeline != VK_NULL_HANDLE) destroyPipeline(context.device, *pipeline, nullptr);
        *pipeline = VK_NULL_HANDLE;
        std::fprintf(stderr, "[pipeline-binary] capture build rejected key=%016llx result=%d; rebuilding without capture\n", static_cast<unsigned long long>(ShaderRecompiler::HashBytes(key)), static_cast<int>(result));
        rememberThreshold(key, true);
    }
    auto lock = synchronization != nullptr ? synchronization->LockForPipelineUse() : PipelineCacheSynchronization::PipelineUseLock{};
    Check(create(context.device, cache, 1, &info, nullptr, pipeline), "vkCreateComputePipelines");
    return PipelineBinaryOutcome::Ordinary;
}

}
