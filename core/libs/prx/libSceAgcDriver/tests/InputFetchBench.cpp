#include "InputFetchBench_frag_spv.h"
#include "InputFetchBench_vert_spv.h"
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t MeshCount = 2000;
constexpr std::uint32_t GridSide = 46;
constexpr std::uint32_t VerticesPerMesh = GridSide * GridSide;
constexpr std::uint32_t IndicesPerMesh = (GridSide - 1) * (GridSide - 1) * 6;
constexpr std::uint32_t VertexBytes = 32;
constexpr std::uint32_t TargetSide = 512;
constexpr std::uint32_t Iterations = 12;

PFN_vkGetInstanceProcAddr getInstanceProcAddr;
PFN_vkGetDeviceProcAddr getDeviceProcAddr;
VkInstance instance;
VkDevice device;

#define VK_FN(name) PFN_##name name
VK_FN(vkCreateInstance);
VK_FN(vkEnumeratePhysicalDevices);
VK_FN(vkGetPhysicalDeviceProperties);
VK_FN(vkGetPhysicalDeviceMemoryProperties);
VK_FN(vkGetPhysicalDeviceQueueFamilyProperties);
VK_FN(vkCreateDevice);
VK_FN(vkGetDeviceQueue);
VK_FN(vkCreateBuffer);
VK_FN(vkGetBufferMemoryRequirements);
VK_FN(vkAllocateMemory);
VK_FN(vkBindBufferMemory);
VK_FN(vkMapMemory);
VK_FN(vkCreateImage);
VK_FN(vkGetImageMemoryRequirements);
VK_FN(vkBindImageMemory);
VK_FN(vkCreateImageView);
VK_FN(vkCreateShaderModule);
VK_FN(vkCreatePipelineLayout);
VK_FN(vkCreateRenderPass);
VK_FN(vkCreateFramebuffer);
VK_FN(vkCreateGraphicsPipelines);
VK_FN(vkCreateCommandPool);
VK_FN(vkAllocateCommandBuffers);
VK_FN(vkBeginCommandBuffer);
VK_FN(vkEndCommandBuffer);
VK_FN(vkResetCommandBuffer);
VK_FN(vkQueueSubmit);
VK_FN(vkQueueWaitIdle);
VK_FN(vkCreateQueryPool);
VK_FN(vkGetQueryPoolResults);
VK_FN(vkCmdResetQueryPool);
VK_FN(vkCmdWriteTimestamp);
VK_FN(vkCmdBeginRenderPass);
VK_FN(vkCmdEndRenderPass);
VK_FN(vkCmdBindPipeline);
VK_FN(vkCmdBindVertexBuffers);
VK_FN(vkCmdBindIndexBuffer);
VK_FN(vkCmdPushConstants);
VK_FN(vkCmdDrawIndexed);
VK_FN(vkCmdCopyBuffer);
VK_FN(vkGetMemoryHostPointerPropertiesEXT);
#undef VK_FN

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed: " + std::to_string(result));
}

template <typename T>
void loadInstance(T& function, const char* name) {
    function = reinterpret_cast<T>(getInstanceProcAddr(instance, name));
    if (function == nullptr) throw std::runtime_error(std::string("missing ") + name);
}

template <typename T>
void loadDevice(T& function, const char* name) {
    function = reinterpret_cast<T>(getDeviceProcAddr(device, name));
    if (function == nullptr) throw std::runtime_error(std::string("missing ") + name);
}

bool nameMatches(std::string_view name, std::string_view request) {
    return std::search(name.begin(), name.end(), request.begin(), request.end(), [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }) != name.end();
}

struct Allocation {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

std::uint32_t memoryType(const VkPhysicalDeviceMemoryProperties& memory, std::uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags avoided) {
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const auto flags = memory.memoryTypes[i].propertyFlags;
        if ((bits & (1u << i)) != 0 && (flags & required) == required && (flags & avoided) == 0) return i;
    }
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0 && (memory.memoryTypes[i].propertyFlags & required) == required) return i;
    }
    throw std::runtime_error("no memory type");
}

VkBuffer makeBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, bool external) {
    VkExternalMemoryBufferCreateInfo externalInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.pNext = external ? &externalInfo : nullptr;
    info.size = bytes;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer;
    check(vkCreateBuffer(device, &info, nullptr, &buffer), "vkCreateBuffer");
    return buffer;
}

}

int main() {
    try {
        void* loader = dlopen("libvulkan.so.1", RTLD_NOW);
        if (loader == nullptr) throw std::runtime_error("libvulkan.so.1 not found");
        getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
        loadInstance(vkCreateInstance, "vkCreateInstance");
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        loadInstance(vkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
        loadInstance(vkGetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
        loadInstance(vkGetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
        loadInstance(vkGetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
        loadInstance(vkCreateDevice, "vkCreateDevice");
        loadInstance(getDeviceProcAddr, "vkGetDeviceProcAddr");

        std::uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> physicals(count);
        vkEnumeratePhysicalDevices(instance, &count, physicals.data());
        const char* request = std::getenv("ANYPS5_GPU");
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties properties{};
        for (auto candidate : physicals) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(candidate, &p);
            const bool wanted = request != nullptr && *request != '\0' ? nameMatches(p.deviceName, request) : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (wanted && physical == VK_NULL_HANDLE) {
                physical = candidate;
                properties = p;
            }
        }
        if (physical == VK_NULL_HANDLE) throw std::runtime_error("no matching GPU (set ANYPS5_GPU)");
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        std::uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
        std::vector<VkQueueFamilyProperties> familyProperties(families);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, familyProperties.data());
        std::uint32_t family = 0;
        while (family < families && (familyProperties[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) ++family;

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        const char* extensions[] = {VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME};
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = 1;
        deviceInfo.ppEnabledExtensionNames = extensions;
        check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
#define LOAD(name) loadDevice(name, #name)
        LOAD(vkGetDeviceQueue); LOAD(vkCreateBuffer); LOAD(vkGetBufferMemoryRequirements); LOAD(vkAllocateMemory); LOAD(vkBindBufferMemory); LOAD(vkMapMemory);
        LOAD(vkCreateImage); LOAD(vkGetImageMemoryRequirements); LOAD(vkBindImageMemory); LOAD(vkCreateImageView); LOAD(vkCreateShaderModule); LOAD(vkCreatePipelineLayout);
        LOAD(vkCreateRenderPass); LOAD(vkCreateFramebuffer); LOAD(vkCreateGraphicsPipelines); LOAD(vkCreateCommandPool); LOAD(vkAllocateCommandBuffers);
        LOAD(vkBeginCommandBuffer); LOAD(vkEndCommandBuffer); LOAD(vkResetCommandBuffer); LOAD(vkQueueSubmit); LOAD(vkQueueWaitIdle); LOAD(vkCreateQueryPool);
        LOAD(vkGetQueryPoolResults); LOAD(vkCmdResetQueryPool); LOAD(vkCmdWriteTimestamp); LOAD(vkCmdBeginRenderPass); LOAD(vkCmdEndRenderPass); LOAD(vkCmdBindPipeline);
        LOAD(vkCmdBindVertexBuffers); LOAD(vkCmdBindIndexBuffer); LOAD(vkCmdPushConstants); LOAD(vkCmdDrawIndexed); LOAD(vkCmdCopyBuffer); LOAD(vkGetMemoryHostPointerPropertiesEXT);
#undef LOAD
        VkQueue queue;
        vkGetDeviceQueue(device, family, 0, &queue);

        const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(VerticesPerMesh) * VertexBytes;
        const VkDeviceSize indexBytes = static_cast<VkDeviceSize>(IndicesPerMesh) * 4;
        const VkDeviceSize meshBytes = (vertexBytes + indexBytes + 255) & ~VkDeviceSize{255};
        const VkDeviceSize totalBytes = (meshBytes * MeshCount + 0xFFFF) & ~VkDeviceSize{0xFFFF};
        void* host = mmap(nullptr, totalBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (host == MAP_FAILED) throw std::runtime_error("mmap failed");
        auto* bytes = static_cast<std::byte*>(host);
        for (std::uint32_t mesh = 0; mesh < MeshCount; ++mesh) {
            auto* vertices = reinterpret_cast<float*>(bytes + mesh * meshBytes);
            for (std::uint32_t y = 0; y < GridSide; ++y) {
                for (std::uint32_t x = 0; x < GridSide; ++x) {
                    float* v = vertices + (y * GridSide + x) * 8;
                    v[0] = static_cast<float>(x) / (GridSide - 1);
                    v[1] = static_cast<float>(y) / (GridSide - 1);
                    v[2] = 0.5f;
                    v[3] = 1.0f;
                    v[4] = static_cast<float>(mesh % 7) / 7.0f;
                    v[5] = v[0];
                    v[6] = v[1];
                    v[7] = 1.0f;
                }
            }
            auto* indices = reinterpret_cast<std::uint32_t*>(bytes + mesh * meshBytes + vertexBytes);
            std::uint32_t i = 0;
            for (std::uint32_t y = 0; y + 1 < GridSide; ++y) {
                for (std::uint32_t x = 0; x + 1 < GridSide; ++x) {
                    const std::uint32_t a = y * GridSide + x;
                    const std::uint32_t quad[6] = {a, a + 1, a + GridSide, a + 1, a + GridSide + 1, a + GridSide};
                    for (auto index : quad) indices[i++] = index;
                }
            }
        }

        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Allocation imported;
        imported.buffer = makeBuffer(totalBytes, usage, true);
        VkMemoryHostPointerPropertiesEXT pointerProperties{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        check(vkGetMemoryHostPointerPropertiesEXT(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &pointerProperties), "vkGetMemoryHostPointerPropertiesEXT");
        VkMemoryRequirements importRequirements;
        vkGetBufferMemoryRequirements(device, imported.buffer, &importRequirements);
        VkImportMemoryHostPointerInfoEXT importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, nullptr, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host};
        VkMemoryAllocateInfo importAllocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &importInfo, totalBytes, memoryType(memory, pointerProperties.memoryTypeBits & importRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0)};
        check(vkAllocateMemory(device, &importAllocate, nullptr, &imported.memory), "import vkAllocateMemory");
        check(vkBindBufferMemory(device, imported.buffer, imported.memory, 0), "import vkBindBufferMemory");

        Allocation local;
        local.buffer = makeBuffer(totalBytes, usage, false);
        VkMemoryRequirements localRequirements;
        vkGetBufferMemoryRequirements(device, local.buffer, &localRequirements);
        VkMemoryAllocateInfo localAllocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, localRequirements.size, memoryType(memory, localRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
        check(vkAllocateMemory(device, &localAllocate, nullptr, &local.memory), "local vkAllocateMemory");
        check(vkBindBufferMemory(device, local.buffer, local.memory, 0), "local vkBindBufferMemory");

        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = {TargetSide, TargetSide, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkImage image;
        check(vkCreateImage(device, &imageInfo, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements imageRequirements;
        vkGetImageMemoryRequirements(device, image, &imageRequirements);
        VkMemoryAllocateInfo imageAllocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, imageRequirements.size, memoryType(memory, imageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
        VkDeviceMemory imageMemory;
        check(vkAllocateMemory(device, &imageAllocate, nullptr, &imageMemory), "image vkAllocateMemory");
        check(vkBindImageMemory(device, image, imageMemory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = imageInfo.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        check(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView");

        VkAttachmentDescription attachment{};
        attachment.format = imageInfo.format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference colorReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        VkRenderPassCreateInfo passInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        passInfo.attachmentCount = 1;
        passInfo.pAttachments = &attachment;
        passInfo.subpassCount = 1;
        passInfo.pSubpasses = &subpass;
        VkRenderPass pass;
        check(vkCreateRenderPass(device, &passInfo, nullptr, &pass), "vkCreateRenderPass");
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.renderPass = pass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &view;
        framebufferInfo.width = TargetSide;
        framebufferInfo.height = TargetSide;
        framebufferInfo.layers = 1;
        VkFramebuffer framebuffer;
        check(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffer), "vkCreateFramebuffer");

        const auto module = [&](const std::uint32_t* code, std::size_t size) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = size;
            info.pCode = code;
            VkShaderModule result;
            check(vkCreateShaderModule(device, &info, nullptr, &result), "vkCreateShaderModule");
            return result;
        };
        const VkShaderModule vertexModule = module(INPUT_FETCH_BENCH_VERT_SPV, sizeof(INPUT_FETCH_BENCH_VERT_SPV));
        const VkShaderModule fragmentModule = module(INPUT_FETCH_BENCH_FRAG_SPV, sizeof(INPUT_FETCH_BENCH_FRAG_SPV));
        VkPushConstantRange pushRange{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        VkPipelineLayout layout;
        check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");

        const auto makePipeline = [&](bool discard) {
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertexModule, "main", nullptr};
            stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule, "main", nullptr};
            VkVertexInputBindingDescription binding{0, VertexBytes, VK_VERTEX_INPUT_RATE_VERTEX};
            VkVertexInputAttributeDescription attributes[2]{{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16}};
            VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            vertexInput.vertexBindingDescriptionCount = 1;
            vertexInput.pVertexBindingDescriptions = &binding;
            vertexInput.vertexAttributeDescriptionCount = 2;
            vertexInput.pVertexAttributeDescriptions = attributes;
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkViewport viewport{0, 0, static_cast<float>(TargetSide), static_cast<float>(TargetSide), 0, 1};
            VkRect2D scissor{{0, 0}, {TargetSide, TargetSide}};
            VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            viewportState.viewportCount = 1;
            viewportState.pViewports = &viewport;
            viewportState.scissorCount = 1;
            viewportState.pScissors = &scissor;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.rasterizerDiscardEnable = discard ? VK_TRUE : VK_FALSE;
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blend{};
            blend.colorWriteMask = 0xF;
            VkPipelineColorBlendStateCreateInfo blendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blendState.attachmentCount = 1;
            blendState.pAttachments = &blend;
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.stageCount = 2;
            info.pStages = stages;
            info.pVertexInputState = &vertexInput;
            info.pInputAssemblyState = &assembly;
            info.pViewportState = &viewportState;
            info.pRasterizationState = &raster;
            info.pMultisampleState = &multisample;
            info.pColorBlendState = &blendState;
            info.layout = layout;
            info.renderPass = pass;
            VkPipeline pipeline;
            check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline), "vkCreateGraphicsPipelines");
            return pipeline;
        };
        const VkPipeline drawn = makePipeline(false);
        const VkPipeline discarded = makePipeline(true);

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
        VkCommandPool pool;
        check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer commands;
        check(vkAllocateCommandBuffers(device, &commandInfo, &commands), "vkAllocateCommandBuffers");
        VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2;
        VkQueryPool queries;
        check(vkCreateQueryPool(device, &queryInfo, nullptr, &queries), "vkCreateQueryPool");

        const auto submit = [&] {
            check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
            VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &commands;
            check(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE), "vkQueueSubmit");
            check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        };
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commands, &begin);
        vkCmdResetQueryPool(commands, queries, 0, 2);
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        VkBufferCopy whole{0, 0, totalBytes};
        vkCmdCopyBuffer(commands, imported.buffer, local.buffer, 1, &whole);
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        submit();
        const double period = properties.limits.timestampPeriod;
        const auto elapsedMs = [&] {
            std::uint64_t stamps[2];
            check(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(stamps), stamps, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults");
            return static_cast<double>(stamps[1] - stamps[0]) * period / 1e6;
        };
        const double uploadMs = elapsedMs();

        const auto run = [&](VkBuffer source, VkPipeline pipeline) {
            std::vector<double> times;
            for (std::uint32_t iteration = 0; iteration < Iterations + 2; ++iteration) {
                vkResetCommandBuffer(commands, 0);
                vkBeginCommandBuffer(commands, &begin);
                vkCmdResetQueryPool(commands, queries, 0, 2);
                VkClearValue clear{};
                VkRenderPassBeginInfo passBegin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                passBegin.renderPass = pass;
                passBegin.framebuffer = framebuffer;
                passBegin.renderArea = {{0, 0}, {TargetSide, TargetSide}};
                passBegin.clearValueCount = 1;
                passBegin.pClearValues = &clear;
                vkCmdBeginRenderPass(commands, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
                vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                for (std::uint32_t mesh = 0; mesh < MeshCount; ++mesh) {
                    const VkDeviceSize offset = mesh * meshBytes;
                    const float cell = 2.0f / 45.0f;
                    const float placement[4] = {-1.0f + cell * static_cast<float>(mesh % 45), -1.0f + cell * static_cast<float>(mesh / 45), cell, cell};
                    vkCmdPushConstants(commands, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(placement), placement);
                    vkCmdBindVertexBuffers(commands, 0, 1, &source, &offset);
                    vkCmdBindIndexBuffer(commands, source, offset + vertexBytes, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(commands, IndicesPerMesh, 1, 0, 0, 0);
                }
                vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
                vkCmdEndRenderPass(commands);
                submit();
                if (iteration >= 2) times.push_back(elapsedMs());
            }
            std::sort(times.begin(), times.end());
            return times[times.size() / 2];
        };
        const double importedDrawn = run(imported.buffer, drawn);
        const double localDrawn = run(local.buffer, drawn);
        const double importedDiscarded = run(imported.buffer, discarded);
        const double localDiscarded = run(local.buffer, discarded);
        std::printf("[input-fetch-bench] gpu=%s meshes=%u triangles/mesh=%u vertex=%uB input=%.1f MiB upload(copy import->local)=%.3f ms\n", properties.deviceName, MeshCount, IndicesPerMesh / 3, VertexBytes, static_cast<double>(totalBytes) / (1024.0 * 1024.0), uploadMs);
        std::printf("[input-fetch-bench] drawn:     imported=%.3f ms device-local=%.3f ms ratio=%.2fx\n", importedDrawn, localDrawn, importedDrawn / localDrawn);
        std::printf("[input-fetch-bench] discarded: imported=%.3f ms device-local=%.3f ms ratio=%.2fx\n", importedDiscarded, localDiscarded, importedDiscarded / localDiscarded);
        std::printf("[input-fetch-bench] verdict: %s\n", importedDrawn / localDrawn >= 1.5 ? "device-local at least 1.5x faster" : "device-local NOT 1.5x faster");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[input-fetch-bench] %s\n", error.what());
        return 1;
    }
}
