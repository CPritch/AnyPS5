#include "prx/libSceAgcDriver/Graphics/include/CpuImageTableTrace.hpp"
#include "ImageTableAbi.hpp"
#include <spirv/unified1/spirv.hpp>
#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <tuple>
#include <utility>

namespace AgcDriver::Graphics {

namespace {

namespace TableAbi = ShaderRecompiler::ImageTableAbi;

constexpr std::uint32_t ForceUnnormalizedBit = 1u << 15u;
constexpr std::size_t PlansPerProgram = 8;
constexpr std::size_t LineLimit = 4096;
constexpr auto SummaryInterval = std::chrono::seconds(10);

struct DescribedPlan {
    const ShaderRecompiler::IrResourcePlan* identity = nullptr;
    std::weak_ptr<const ShaderRecompiler::IrResourcePlan> held;
    bool deviceColumns = false;
    CpuImageTableGate gate;
    std::vector<CpuImageTableImage> images;
};

using BindingKey = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, bool>;

struct TracedBinding {
    bool cpuImages = true;
    std::vector<std::string> reasons;
};

struct Registry {
    std::mutex mutex;
    std::map<std::uint64_t, std::vector<std::shared_ptr<const DescribedPlan>>> plans;
    std::map<BindingKey, TracedBinding> traced;
    std::map<std::string, std::uint64_t> windowRecords;
    std::uint64_t windowResolves = 0;
    std::uint64_t windowTotal = 0;
    std::chrono::steady_clock::time_point windowStart{};
    bool windowOpen = false;
    std::size_t lines = 0;
};

Registry& registry() {
    static Registry kept;
    return kept;
}

void addFlag(std::string& text, const std::string& flag) {
    if (!text.empty()) text += ',';
    text += flag;
}

void addReason(std::vector<std::string>& reasons, const std::string& reason) {
    if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) reasons.push_back(reason);
}

std::string joined(const std::vector<std::string>& reasons) {
    std::string text;
    for (const auto& reason : reasons) addFlag(text, reason);
    return text.empty() ? std::string("-") : text;
}

const char* stageName(ShaderRecompiler::ShaderStage stage) {
    using Stage = ShaderRecompiler::ShaderStage;
    switch (stage) {
        case Stage::Compute: return "compute";
        case Stage::Vertex: return "vertex";
        case Stage::TessellationControl: return "hull";
        case Stage::TessellationEvaluation: return "domain";
        case Stage::Geometry: return "geometry";
        case Stage::Fragment: return "fragment";
        case Stage::Local: return "local";
        case Stage::Mesh: return "mesh";
    }
    return "unknown";
}

std::string columnText(const ShaderRecompiler::IrResourcePlan& plan, const ShaderRecompiler::TableColumn& column) {
    std::string text = "stride=" + std::to_string(column.stride) + ",dwords=" + std::to_string(column.dwordCount);
    if (column.sampler) addFlag(text, "sampler");
    if (column.address) addFlag(text, "address");
    if (column.buffer) addFlag(text, "buffer");
    if (column.indirect) addFlag(text, "indirect");
    if (!column.pointerHops.empty()) addFlag(text, "hops=" + std::to_string(column.pointerHops.size()));
    if (column.heapSource < plan.descriptorSources.size() && plan.descriptorSources[column.heapSource].tableColumn.has_value()) addFlag(text, "nested");
    if (column.pointerKeyed) addFlag(text, "pointer-keyed");
    if (column.dynamic) addFlag(text, "dynamic");
    if (column.selected) addFlag(text, "selected");
    if (column.unkeyed) addFlag(text, "unkeyed");
    if (!column.keyDomains.empty()) addFlag(text, "domains=" + std::to_string(column.keyDomains.size()));
    if (column.maxKey != 0xffffffffu) addFlag(text, "max-key=" + std::to_string(column.maxKey));
    return text;
}

const char* className(ShaderRecompiler::ImageResourceClass resourceClass) {
    switch (resourceClass) {
        case ShaderRecompiler::ImageResourceClass::Sampled: return "sampled";
        case ShaderRecompiler::ImageResourceClass::Storage: return "storage";
        default: return "none";
    }
}

std::string samplerKind(const ShaderRecompiler::IrResourcePlan& plan, const ShaderRecompiler::SamplerResource& sampler, std::vector<std::string>& reasons) {
    const auto* source = sampler.source < plan.descriptorSources.size() ? &plan.descriptorSources[sampler.source] : nullptr;
    if (sampler.table != ShaderRecompiler::NoTable || (source != nullptr && source->tableColumn.has_value())) {
        addReason(reasons, "table-sampler");
        return "table";
    }
    if (source == nullptr || source->dwordCount < 4u || source->dwords[0] == nullptr) {
        addReason(reasons, "untracked-sampler");
        return "untracked";
    }
    const auto* word = source->dwords[0]->Resolve();
    const bool immediate = word->HasImmediate() && word->Type() == ShaderRecompiler::IrType::U32;
    const bool unnormalized = (immediate && (word->ImmediateU32() & ForceUnnormalizedBit) != 0u) || (sampler.constant && (sampler.constantWords[0] & ForceUnnormalizedBit) != 0u);
    if (unnormalized) {
        addReason(reasons, "unnormalized-sampler");
        return "unnormalized";
    }
    return immediate || sampler.constant ? "direct-immediate" : "direct";
}

CpuImageTableImage describeImage(const ShaderRecompiler::IrResourcePlan& plan, std::uint32_t resource) {
    const auto& info = plan.info;
    const auto& image = info.images[resource];
    CpuImageTableImage described;
    described.resource = resource;
    described.table = image.table;
    const auto* source = image.source < plan.descriptorSources.size() ? &plan.descriptorSources[image.source] : nullptr;
    if (source == nullptr || !source->tableColumn.has_value()) {
        described.column = "none";
        addReason(described.reasons, "no-column");
    } else {
        const auto& column = *source->tableColumn;
        described.column = columnText(plan, column);
        if (column.sampler) addReason(described.reasons, "sampler-column");
        if (column.address) addReason(described.reasons, "address-column");
        if (column.indirect) addReason(described.reasons, "indirect-column");
        if (!column.pointerHops.empty()) addReason(described.reasons, "pointer-hop-column");
    }
    std::vector<std::uint32_t> sharing;
    for (std::uint32_t other = 0; other < info.images.size(); ++other) {
        if (info.images[other].source == image.source) sharing.push_back(other);
    }
    std::uint32_t accesses = 0;
    std::uint32_t packedAccesses = 0;
    std::uint32_t elementAccesses = 0;
    std::uint32_t compareAccesses = 0;
    for (const auto& memory : plan.memoryInfo) {
        if (memory.kind != ShaderRecompiler::ResourceKind::Image || std::find(sharing.begin(), sharing.end(), memory.resource) == sharing.end()) continue;
        ++accesses;
        if (memory.imagePacked) ++packedAccesses;
        if (memory.imageByElements != 0u) ++elementAccesses;
        if ((memory.imageSampleFlags & ShaderRecompiler::RdnaImageSampleFlagCompare) != 0u) ++compareAccesses;
    }
    bool storage = false;
    bool packed = packedAccesses != 0u;
    bool elements = elementAccesses != 0u;
    bool compare = compareAccesses != 0u;
    for (const auto other : sharing) {
        const auto& shared = info.images[other];
        storage = storage || shared.resourceClass == ShaderRecompiler::ImageResourceClass::Storage || shared.written || shared.atomic;
        packed = packed || shared.packed;
        elements = elements || shared.byElements != 0u;
        compare = compare || shared.depthCompare;
    }
    described.access = std::string("class=") + className(image.resourceClass) + ",dim=" + std::to_string(static_cast<int>(image.dimension)) + ",accesses=" + std::to_string(accesses);
    if (sharing.size() > 1u) addFlag(described.access, "source-images=" + std::to_string(sharing.size()));
    if (packed) addFlag(described.access, "packed=" + std::to_string(packedAccesses));
    if (elements) addFlag(described.access, "by-elements=" + std::to_string(elementAccesses));
    if (compare) addFlag(described.access, "compare=" + std::to_string(compareAccesses));
    if (image.written) addFlag(described.access, "written");
    if (image.atomic) addFlag(described.access, "atomic");
    if (image.gathered) addFlag(described.access, "gathered");
    if (image.dimensionQueryOnly) addFlag(described.access, "query-only");
    if (image.nestedTable) addFlag(described.access, "nested-table");
    if (image.tableOperation == ShaderRecompiler::TableOperation::Unsupported) addFlag(described.access, "table-operation-unsupported");
    if (packed) addReason(described.reasons, "packed");
    if (elements) addReason(described.reasons, "element-wise");
    if (compare) addReason(described.reasons, "compare");
    if (storage) addReason(described.reasons, "storage");
    std::vector<std::uint32_t> seen;
    for (const auto& pair : info.sampledPairs) {
        if (std::find(sharing.begin(), sharing.end(), pair.image) == sharing.end() || pair.sampler >= info.samplers.size() || std::find(seen.begin(), seen.end(), pair.sampler) != seen.end()) continue;
        seen.push_back(pair.sampler);
        addFlag(described.samplers, "s" + std::to_string(pair.sampler) + ":" + samplerKind(plan, info.samplers[pair.sampler], described.reasons));
    }
    if (described.samplers.empty()) described.samplers = "-";
    return described;
}

std::shared_ptr<const DescribedPlan> describePlan(const std::shared_ptr<const ShaderRecompiler::IrResourcePlan>& plan, const CpuImageTableGate& gate) {
    auto described = std::make_shared<DescribedPlan>();
    described->identity = plan.get();
    described->held = plan;
    described->gate = gate;
    described->deviceColumns = std::ranges::any_of(plan->info.images, [](const ShaderRecompiler::ImageResource& image) { return image.deviceTable; });
    described->images = DescribeCpuImageTables(*plan);
    return described;
}

std::shared_ptr<const DescribedPlan> capturedPlan(Registry& kept, std::uint64_t address, bool deviceColumns, std::size_t& candidates) {
    const auto found = kept.plans.find(address);
    candidates = found == kept.plans.end() ? 0u : found->second.size();
    if (found == kept.plans.end()) return nullptr;
    for (auto entry = found->second.rbegin(); entry != found->second.rend(); ++entry) {
        if ((*entry)->deviceColumns == deviceColumns) return *entry;
    }
    return found->second.back();
}

void addGateReasons(DeviceImageService service, const CpuImageTableGate& gate, std::vector<std::string>& reasons) {
    if (!gate.known) return;
    if (!gate.requested) addReason(reasons, service == DeviceImageService::NoTable ? "gate-path-off" : service == DeviceImageService::FallenBack ? "gate-fallback" : "gate-not-requested");
    if (!gate.gpuModeStage) addReason(reasons, "gate-not-gpu-mode-stage");
    if (!gate.int64) addReason(reasons, "gate-no-int64");
    if (!gate.bufferAddresses) addReason(reasons, "gate-no-buffer-addresses");
}

std::string gateText(const CpuImageTableGate& gate) {
    if (!gate.known) return "unknown";
    return std::string("requested=") + (gate.requested ? "1" : "0") + ",gpu-mode=" + (gate.gpuModeStage ? "1" : "0") + ",int64=" + (gate.int64 ? "1" : "0") + ",buffer-addresses=" + (gate.bufferAddresses ? "1" : "0");
}

std::size_t countFlags(const ShaderRecompiler::ElementFlags& flags) {
    return static_cast<std::size_t>(std::count(flags.begin(), flags.end(), true));
}

std::string bindingText(const ShaderRecompiler::DescriptorBinding& binding, std::span<const std::uint32_t> map) {
    const auto directSamplers = std::count_if(binding.imageSamplers.begin(), binding.imageSamplers.end(), [](std::uint64_t mask) { return mask != 0u; });
    std::string text = "compare=" + std::to_string(countFlags(binding.imageDepthCompare)) + ",unnormalized=" + std::to_string(countFlags(binding.imageUnnormalized)) + ",written=" + std::to_string(countFlags(binding.imageWritten)) + ",atomic=" + std::to_string(countFlags(binding.imageAtomic)) + ",direct-samplers=" + std::to_string(directSamplers);
    const auto tables = map.size() > TableAbi::HeaderTableCount ? map[TableAbi::HeaderTableCount] : 0u;
    addFlag(text, "map-tables=" + std::to_string(tables));
    return text;
}

std::string tableText(std::span<const std::uint32_t> map, std::uint32_t table) {
    const auto header = TableAbi::TableHeader(table);
    if (map.size() < static_cast<std::size_t>(header) + TableAbi::TableWords) return "keys=? fault=?";
    return "keys=" + std::to_string(map[header + TableAbi::TableKeyCount]) + " fault=" + (map[header + TableAbi::TableFault] != 0u ? "1" : "0");
}

}

bool CpuImageTablesTraced() {
    static const bool traced = std::getenv("APS5_TRACE_CPU_IMAGE_TABLES") != nullptr;
    return traced;
}

CpuImageTableGate CpuImageTableGateOf(const ShaderRecompiler::RecompileRequest& request) {
    using Stage = ShaderRecompiler::ShaderStage;
    const auto stage = request.shader.stage;
    const bool heapStage = stage == Stage::Fragment || stage == Stage::Vertex || stage == Stage::Compute;
    const auto supports = [&](spv::Capability capability) { return std::ranges::find(request.target.supportedCapabilities, static_cast<std::uint32_t>(capability)) != request.target.supportedCapabilities.end(); };
    CpuImageTableGate gate;
    gate.known = true;
    gate.requested = request.target.deviceImageTables;
    gate.gpuModeStage = request.target.gpuResolvedImages && request.target.descriptorMemory && heapStage;
    gate.int64 = supports(spv::CapabilityInt64);
    gate.bufferAddresses = supports(spv::CapabilityPhysicalStorageBufferAddresses);
    return gate;
}

std::vector<CpuImageTableImage> DescribeCpuImageTables(const ShaderRecompiler::IrResourcePlan& plan) {
    std::vector<CpuImageTableImage> images;
    for (std::uint32_t resource = 0; resource < plan.info.images.size(); ++resource) {
        const auto& image = plan.info.images[resource];
        if (image.table == ShaderRecompiler::NoTable || image.deviceTable) continue;
        images.push_back(describeImage(plan, resource));
    }
    return images;
}

void NoteCpuImageTablePlan(const ShaderRecompiler::RecompileRequest& request, const std::shared_ptr<const ShaderRecompiler::IrResourcePlan>& plan) {
    if (!CpuImageTablesTraced() || plan == nullptr) return;
    if (std::ranges::none_of(plan->info.images, [](const ShaderRecompiler::ImageResource& image) { return image.table != ShaderRecompiler::NoTable && !image.deviceTable; })) return;
    auto& kept = registry();
    std::lock_guard lock(kept.mutex);
    auto& entries = kept.plans[plan->shaderHash];
    for (const auto& entry : entries) {
        if (entry->identity == plan.get() && !entry->held.expired()) return;
    }
    entries.push_back(describePlan(plan, CpuImageTableGateOf(request)));
    if (entries.size() > PlansPerProgram) entries.erase(entries.begin());
}

void TraceCpuImageTable(const std::function<DeviceImageService(std::uint64_t codeAddress)>& service, ShaderRecompiler::ShaderStage stage, const ShaderRecompiler::RecompileResult& program, const ShaderRecompiler::DescriptorBinding& binding, std::span<const std::uint32_t> map, bool heap) {
    if (!CpuImageTablesTraced()) return;
    const auto address = program.imageTableShader;
    const BindingKey key{address, binding.descriptorSet, binding.binding, static_cast<std::uint32_t>(binding.role), program.deviceImageTables};
    const auto now = std::chrono::steady_clock::now();
    std::string line;
    std::string summary;
    auto& kept = registry();
    {
        std::lock_guard lock(kept.mutex);
        auto traced = kept.traced.find(key);
        if (traced == kept.traced.end()) {
            std::shared_ptr<const DescribedPlan> described;
            const char* origin = "none";
            std::size_t candidates = 0;
            if (program.deviceImagePlan != nullptr) {
                CpuImageTableGate open;
                open.known = true;
                open.requested = open.gpuModeStage = open.int64 = open.bufferAddresses = true;
                described = describePlan(program.deviceImagePlan, open);
                origin = "result";
            } else if ((described = capturedPlan(kept, address, program.deviceImageTables, candidates)) != nullptr) {
                origin = "captured";
            }
            const bool cpuImages = described == nullptr || !described->images.empty();
            const auto served = service(address);
            std::vector<std::string> reasons;
            if (described == nullptr) {
                addReason(reasons, "no-plan");
            } else {
                addGateReasons(served, described->gate, reasons);
                const bool gateClosed = !reasons.empty();
                for (const auto& image : described->images) {
                    for (const auto& reason : image.reasons) addReason(reasons, reason);
                    if (image.reasons.empty() && !gateClosed) addReason(reasons, "unseen");
                }
            }
            if (cpuImages && kept.lines < LineLimit) {
                char head[512];
                std::snprintf(head, sizeof(head), "[cpu-image-tables] program 0x%" PRIx64 " stage %s binding %u.%u %s records %u device-tables %d heap %d served %s plan %s",
                              address, stageName(stage), binding.descriptorSet, binding.binding, binding.role == ShaderRecompiler::DescriptorRole::ImageStorageTable ? "storage" : "sampled", binding.count, program.deviceImageTables ? 1 : 0, heap ? 1 : 0, served == DeviceImageService::NoTable ? "-" : served == DeviceImageService::Serves ? "1" : "0", origin);
                line = head;
                if (described == nullptr) line += " candidates " + std::to_string(candidates);
                else line += " gate " + gateText(described->gate);
                line += " reasons " + joined(reasons) + " | binding " + bindingText(binding, map);
                if (described != nullptr) {
                    for (const auto& image : described->images) {
                        line += " | image " + std::to_string(image.resource) + " table " + std::to_string(image.table) + " " + tableText(map, image.table) + " column " + image.column + " access " + image.access + " samplers " + image.samplers + " why " + joined(image.reasons);
                    }
                }
                line += '\n';
                if (++kept.lines == LineLimit) line += "[cpu-image-tables] line limit of " + std::to_string(LineLimit) + " reached; later programs are counted in the summaries only\n";
            }
            traced = kept.traced.emplace(key, TracedBinding{cpuImages, std::move(reasons)}).first;
        }
        if (!traced->second.cpuImages) return;
        if (!kept.windowOpen) {
            kept.windowOpen = true;
            kept.windowStart = now;
        }
        ++kept.windowResolves;
        kept.windowTotal += binding.count;
        for (const auto& reason : traced->second.reasons) kept.windowRecords[reason] += binding.count;
        if (now - kept.windowStart >= SummaryInterval) {
            std::vector<std::pair<std::string, std::uint64_t>> ranked(kept.windowRecords.begin(), kept.windowRecords.end());
            std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) { return left.second != right.second ? left.second > right.second : left.first < right.first; });
            const auto seconds = std::chrono::duration<double>(now - kept.windowStart).count();
            char head[384];
            std::snprintf(head, sizeof(head), "[cpu-image-tables] %.1f s: %" PRIu64 " CPU table resolves, %" PRIu64 " records, %zu program bindings with CPU image columns seen; records by reason (a binding counts under each of its reasons):", seconds, kept.windowResolves, kept.windowTotal, static_cast<std::size_t>(std::ranges::count_if(kept.traced, [](const auto& entry) { return entry.second.cpuImages; })));
            summary = head;
            for (const auto& [reason, records] : ranked) summary += " " + reason + " " + std::to_string(records);
            summary += '\n';
            kept.windowRecords.clear();
            kept.windowResolves = 0;
            kept.windowTotal = 0;
            kept.windowStart = now;
        }
    }
    if (!line.empty()) std::fputs(line.c_str(), stderr);
    if (!summary.empty()) std::fputs(summary.c_str(), stderr);
}

}
