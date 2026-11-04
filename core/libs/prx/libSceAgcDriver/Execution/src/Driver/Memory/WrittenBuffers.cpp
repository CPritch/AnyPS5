#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DrawWriteRanges.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestTextureResource.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#include "prx/libSceAgcDriver/Graphics/include/VertexInput.hpp"
#include "Optimization/ShaderStageInputInfo.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

namespace {

template<typename TVisit>
void visitWrittenBuffers(const ShaderRecompiler::RecompileResult& compiled, TVisit&& visit) {
    for (const auto& binding : compiled.bindings) {
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const bool written = element >= binding.bufferWritten.size() || binding.bufferWritten[element];
            if (!written || binding.guestDescriptor.size() < (static_cast<std::size_t>(element) + 1) * 4) continue;
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 4, 4);
            const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1], words[2], words[3]}};
            const auto base = descriptor.Base48();
            const auto size = descriptor.GetSize();
            if (base == 0 || size == 0) continue;
            visit(element, base, base + size, element < binding.bufferAtomic.size() && binding.bufferAtomic[element]);
        }
    }
}

}

template<typename TVisit>
void Driver::forEachWrittenBuffer(const ShaderRecompiler::RecompileResult& compiled, TVisit&& visit) {
    visitWrittenBuffers(compiled, std::forward<TVisit>(visit));
}

void Driver::appendWrittenRanges(const ShaderRecompiler::RecompileResult& compiled, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) {
    forEachWrittenBuffer(compiled, [&](std::uint32_t, std::uint64_t begin, std::uint64_t end, bool) { ranges.emplace_back(begin, end); });
}

void Driver::noteWrittenBuffers(std::uint64_t program, std::uint32_t queue, const ShaderRecompiler::RecompileResult& compiled) {
    std::lock_guard lock(writtenBuffersMutex);
    const auto serial = ++writtenBufferSerial;
    forEachWrittenBuffer(compiled, [&](std::uint32_t, std::uint64_t begin, std::uint64_t end, bool atomic) {
        writtenBuffers.Push({program, begin, end, serial, queue, atomic});
    });
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> DispatchWriteRanges(const ShaderRecompiler::RecompileResult& compiled, bool& bounded, DispatchUnbounded* reason) {
    static const bool boundReadOnly = std::getenv("APS5_NO_BOUND_READ_ONLY_ADDRESS_DISPATCHES") == nullptr;
    const bool addressBased = compiled.bdaAbiVersion != 0;
    const bool faultBuffer = std::any_of(compiled.bindings.begin(), compiled.bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::FaultBuffer; });
    const bool readOnly = addressBased && !compiled.bdaWrites && !Graphics::ShaderStoresThroughAddresses(compiled.spirv);
    const bool readOnlyBounded = readOnly && boundReadOnly;
    const bool addressesBounded = !addressBased || faultBuffer || readOnlyBounded;
    auto why = !addressBased || faultBuffer ? DispatchUnbounded::None : readOnly ? DispatchUnbounded::AddressReadOnly : DispatchUnbounded::AddressWrites;
    bounded = addressesBounded;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    constexpr std::uint64_t UnknownImageBytes = 64ull * 1024 * 1024;
    for (const auto& binding : compiled.bindings) {
        if (binding.role == ShaderRecompiler::DescriptorRole::Gds) {
            bounded = false;
            why = DispatchUnbounded::Gds;
        }
        if (binding.role == ShaderRecompiler::DescriptorRole::BdaPagetable && !readOnlyBounded) {
            bounded = false;
            if (why == DispatchUnbounded::None) why = readOnly ? DispatchUnbounded::AddressReadOnly : DispatchUnbounded::AddressWrites;
        }
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages || binding.kind != ShaderRecompiler::DescriptorKind::StorageImage) continue;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const bool written = element >= binding.imageWritten.size() || binding.imageWritten[element];
            if (!written) continue;
            if (binding.guestDescriptor.size() < (static_cast<std::size_t>(element) + 1) * 8) {
                bounded = false;
                if (why == DispatchUnbounded::None || why == DispatchUnbounded::AddressReadOnly) why = DispatchUnbounded::ImageDescriptor;
                continue;
            }
            const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8, 8);
            if (Graphics::IsNullTextureResource(words)) continue;
            const auto resource = Graphics::DecodeTextureResource(words);
            const auto surfaceBytes = Graphics::DescribeSurface(resource).guestBytes;
            ranges.emplace_back(resource.baseAddress, resource.baseAddress + (surfaceBytes != 0 ? surfaceBytes : UnknownImageBytes));
            if (resource.dccAddress != 0) ranges.emplace_back(resource.dccAddress, resource.dccAddress + (surfaceBytes != 0 ? surfaceBytes / 256 + 1 : UnknownImageBytes));
        }
    }
    visitWrittenBuffers(compiled, [&](std::uint32_t, std::uint64_t begin, std::uint64_t end, bool) { ranges.emplace_back(begin, end); });
    if (reason != nullptr) *reason = bounded ? DispatchUnbounded::None : why;
    return ranges;
}

void Driver::noteForeignWriter(std::uint64_t begin, std::uint64_t end, std::uint32_t queue) {
    if (!foreignWriters() || !(writeEvidenceEnabled() || traceCapSync()) || end <= begin) return;
    std::lock_guard lock(writtenBuffersMutex);
    writtenBuffers.Push({0, begin, end, ++writtenBufferSerial, queue, false});
}

void Driver::noteDrawWriters(std::span<const Graphics::CompiledShader> stages, std::uint32_t queue) {
    for (const auto& stage : stages) {
        forEachWrittenBuffer(*stage.program, [&](std::uint32_t, std::uint64_t begin, std::uint64_t end, bool) { noteForeignWriter(begin, end, queue); });
    }
}

bool ExactDrawWrites() {
    static const bool exact = [] { const char* text = std::getenv("APS5_EXACT_DRAW_WRITES"); return text == nullptr || std::strcmp(text, "0") != 0; }();
    return exact;
}

namespace {

constexpr std::uint64_t DrawSlack = 64 * 1024;
constexpr std::uint64_t DrawUnknownBytes = 64ull * 1024 * 1024;

void appendColorRanges(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, const Graphics::ColorTarget& color, bool exact) {
    const auto add = [&](std::uint64_t begin, std::uint64_t bytes) {
        if (begin != 0) ranges.emplace_back(begin, begin + bytes);
    };
    if (exact && color.bytes != 0) {
        const auto slices = static_cast<std::uint64_t>(std::max<std::uint32_t>(color.depth, 1));
        const auto bytes = static_cast<std::uint64_t>(color.bytes) * slices;
        add(color.address, bytes);
        add(color.dccAddress, (bytes + 255) / 256);
        add(color.cmaskAddress, static_cast<std::uint64_t>(color.cmaskBytes) * slices);
        return;
    }
    const std::uint64_t bytes = color.bytes != 0 ? static_cast<std::uint64_t>(color.bytes) : DrawUnknownBytes;
    const auto base = color.surfaceAddress != 0 ? std::min(color.surfaceAddress, color.address) : color.address;
    if (base != 0) ranges.emplace_back(base, color.address + 2 * bytes + DrawSlack);
    add(color.dccAddress, bytes / 16 + DrawSlack);
    add(color.cmaskAddress, static_cast<std::uint64_t>(color.cmaskBytes) * std::max<std::uint32_t>(color.depth, 1));
}

void appendDepthRanges(std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges, const Graphics::DepthTarget& depth, bool exact) {
    const auto add = [&](std::uint64_t begin, std::uint64_t bytes) {
        if (begin != 0) ranges.emplace_back(begin, begin + bytes);
    };
    if (exact && depth.extent.width != 0 && depth.extent.height != 0) {
        const bool d16 = depth.format == VK_FORMAT_D16_UNORM || depth.format == VK_FORMAT_D16_UNORM_S8_UINT;
        add(depth.address, depth.samples > 1u ? Graphics::MultisampledDepthBytes_nid_no_patch(depth.extent, d16 ? 2u : 4u, depth.samples) : Graphics::DepthSliceBytes(depth.extent, d16 ? 2u : 4u));
        add(depth.stencilAddress, depth.samples > 1u ? Graphics::MultisampledDepthBytes_nid_no_patch(depth.extent, 1u, depth.samples) : Graphics::DepthSliceBytes(depth.extent, 1u));
    } else {
        const auto align = [](std::uint64_t value) { return (value + 255) & ~255ull; };
        const auto pixels = align(depth.extent.width) * align(depth.extent.height) * std::max<std::uint32_t>(depth.samples, 1u);
        const auto bytes = pixels != 0 ? pixels * 8 + DrawSlack : DrawUnknownBytes;
        add(depth.address, bytes);
        add(depth.stencilAddress, bytes);
    }
}

}

std::vector<std::pair<std::uint64_t, std::uint64_t>> DrawWriteRanges(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages, bool exact) {
    constexpr std::uint64_t UnknownBytes = DrawUnknownBytes;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    const auto add = [&](std::uint64_t begin, std::uint64_t bytes) {
        if (begin != 0) ranges.emplace_back(begin, begin + bytes);
    };
    for (const auto& stage : stages) {
        if (stage.program == nullptr) continue;
        visitWrittenBuffers(*stage.program, [&](std::uint32_t, std::uint64_t begin, std::uint64_t end, bool) { ranges.emplace_back(begin, end); });
        for (const auto& binding : stage.program->bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestImages || binding.kind != ShaderRecompiler::DescriptorKind::StorageImage) continue;
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                const bool written = element >= binding.imageWritten.size() || binding.imageWritten[element];
                if (!written || binding.guestDescriptor.size() < (static_cast<std::size_t>(element) + 1) * 8) continue;
                const auto words = std::span<const std::uint32_t>(binding.guestDescriptor).subspan(static_cast<std::size_t>(element) * 8, 8);
                if (Graphics::IsNullTextureResource(words)) continue;
                const auto resource = Graphics::DecodeTextureResource(words);
                if (resource.baseAddress == 0) continue;
                const auto surfaceBytes = Graphics::DescribeSurface(resource).guestBytes;
                add(resource.baseAddress, surfaceBytes != 0 ? surfaceBytes : UnknownBytes);
                add(resource.dccAddress, surfaceBytes != 0 ? surfaceBytes / 256 + 1 : UnknownBytes);
            }
        }
    }
    if (graphics.hasColorTarget) appendColorRanges(ranges, graphics.color, exact);
    for (const auto& color : graphics.colors) appendColorRanges(ranges, color, exact);
    if (graphics.depth) appendDepthRanges(ranges, *graphics.depth, exact);
    return ranges;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> DrawColorRanges(const Graphics::State& graphics) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    if (graphics.hasColorTarget) appendColorRanges(ranges, graphics.color, ExactDrawWrites());
    for (const auto& color : graphics.colors) appendColorRanges(ranges, color, ExactDrawWrites());
    return ranges;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> DrawDepthRanges(const Graphics::State& graphics) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    DrawDepthRanges(graphics, ranges);
    return ranges;
}

void DrawDepthRanges(const Graphics::State& graphics, std::vector<std::pair<std::uint64_t, std::uint64_t>>& ranges) {
    ranges.clear();
    if (!graphics.depth) return;
    appendDepthRanges(ranges, *graphics.depth, ExactDrawWrites());
    if (graphics.depth->htileAddress != 0) ranges.emplace_back(graphics.depth->htileAddress, graphics.depth->htileAddress + Graphics::HtileMetadataBytes(graphics.depth->extent));
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> Driver::drawWriteRanges(const Graphics::State& graphics, std::span<const Graphics::CompiledShader> stages) {
    return DrawWriteRanges(graphics, stages, ExactDrawWrites());
}

std::vector<std::pair<std::uint64_t, std::uint64_t>> QueuedDrawReadRanges(const Pm4::DrawParameters& draw, std::span<const Graphics::CompiledShader> stages) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    const auto add = [&](std::uint64_t begin, std::uint64_t bytes) {
        if (begin == 0 || bytes == 0 || bytes > std::numeric_limits<std::uint64_t>::max() - begin) return;
        ranges.emplace_back(begin, begin + bytes);
    };
    if (draw.indexed && draw.indexSize != 0 && draw.indexCount != 0) add(draw.indexAddress, static_cast<std::uint64_t>(draw.indexCount) * draw.indexSize);
    if (stages.empty()) return ranges;
    for (const auto& stage : stages) {
        if (stage.program == nullptr) continue;
        for (const auto& binding : stage.program->bindings) {
            if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers || binding.guestDescriptor.size() < static_cast<std::size_t>(binding.count) * 4u) continue;
            for (std::uint32_t element = 0; element < binding.count; ++element) {
                const auto* words = binding.guestDescriptor.data() + static_cast<std::size_t>(element) * 4u;
                const ShaderRecompiler::ShaderBufferResource descriptor{{words[0], words[1], words[2], words[3]}};
                add(descriptor.Base48(), descriptor.GetSize());
            }
        }
    }
    if (stages.empty() || stages.front().program == nullptr) return ranges;
    for (const auto& attribute : stages.front().program->vertexAttributes) {
        if (Graphics::NullVertexDescriptor(attribute)) continue;
        const auto address = attribute.resource.fields[0] | (static_cast<std::uint64_t>(attribute.resource.fields[1] & 0xffffu) << 32u);
        std::uint64_t bytes = 0;
        try {
            if (Graphics::VertexFetchOutOfRange(attribute)) continue;
            bytes = Graphics::VertexBufferExtent(attribute);
        } catch (const std::runtime_error&) {
            continue;
        }
        add(address, bytes);
    }
    return ranges;
}

std::optional<WrittenBuffer> Driver::newestWriterLocked(std::uint64_t begin, std::uint64_t end) const {
    return writtenBuffers.Newest(begin, end);
}

std::optional<WrittenBuffer> Driver::newestWriter(std::uint64_t begin, std::uint64_t end) {
    std::lock_guard lock(writtenBuffersMutex);
    return newestWriterLocked(begin, end);
}

std::string Driver::describeWriters(std::uint64_t begin, std::uint64_t end) {
    std::string text;
    std::lock_guard lock(writtenBuffersMutex);
    int shown = 0;
    for (auto it = writtenBuffers.Entries().rbegin(); it != writtenBuffers.Entries().rend() && shown < 3; ++it) {
        if (begin >= it->end || it->begin >= end) continue;
        char item[128];
        if (it->program == 0) std::snprintf(item, sizeof(item), " [foreign q0x%x 0x%llx+0x%llx age %llu]", it->queue, static_cast<unsigned long long>(it->begin), static_cast<unsigned long long>(it->end - it->begin), static_cast<unsigned long long>(writtenBufferSerial - it->serial));
        else std::snprintf(item, sizeof(item), " [0x%llx q0x%x 0x%llx+0x%llx%s%s age %llu]", static_cast<unsigned long long>(it->program), it->queue, static_cast<unsigned long long>(it->begin), static_cast<unsigned long long>(it->end - it->begin), it->atomic ? " atomic" : "", it->value ? " known" : "", static_cast<unsigned long long>(writtenBufferSerial - it->serial));
        text += item;
        ++shown;
    }
    if (shown == 0) return " none";
    std::uint32_t streakMin = 0xffffffffu, streakMax = 0, changed = 0, unseen = 0;
    for (std::uint64_t dword = begin & ~3ull; dword < end; dword += 4) {
        const auto found = dwordEvidence.find(dword);
        if (found == dwordEvidence.end()) {
            ++unseen;
            continue;
        }
        streakMin = std::min(streakMin, found->second.streak);
        streakMax = std::max(streakMax, found->second.streak);
        changed += found->second.changed;
    }
    char item[96];
    std::snprintf(item, sizeof(item), " dwords: streak %u..%u changed %u unseen %u", streakMin == 0xffffffffu ? 0u : streakMin, streakMax, changed, unseen);
    return text + item;
}

std::string Driver::describeSelf(const ShaderRecompiler::RecompileResult& compiled, std::uint64_t begin, std::uint64_t end) {
    std::string text;
    forEachWrittenBuffer(compiled, [&](std::uint32_t element, std::uint64_t base, std::uint64_t limit, bool atomic) {
        if (begin >= limit || base >= end) return;
        char item[96];
        std::snprintf(item, sizeof(item), " own element %u 0x%llx+0x%llx%s", element, static_cast<unsigned long long>(base), static_cast<unsigned long long>(limit - base), atomic ? " atomic" : "");
        text += item;
    });
    return text;
}

}
