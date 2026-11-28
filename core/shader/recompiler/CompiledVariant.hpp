#ifndef CORE_SHADER_RECOMPILER_COMPILEDVARIANT_HPP
#define CORE_SHADER_RECOMPILER_COMPILEDVARIANT_HPP

#include "Recompiler.hpp"
#include "IntermediateRepresentation/IrMetadata/CompiledShaderInfo.hpp"
#include "Optimization/include/Optimization/BindingAllocator.hpp"
#include "Optimization/include/Optimization/DescriptorBindingBuilder.hpp"
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

class BufferSubstituteTable {
public:
    explicit BufferSubstituteTable(std::vector<BufferSubstitute> slots, std::vector<std::byte> diskKey = {}) : slots(std::move(slots)), diskKey(std::move(diskKey)) {}

    [[nodiscard]] std::vector<BufferSubstitute> Slots() const {
        std::lock_guard lock(mutex);
        return slots;
    }

    void CopySlots(std::vector<BufferSubstitute>& out) const {
        std::lock_guard lock(mutex);
        out.assign(slots.begin(), slots.end());
    }

    bool Learn(std::size_t slot, const BufferSubstitute& value) {
        std::lock_guard lock(mutex);
        if (slot >= slots.size() || slots[slot].known) return false;
        slots[slot] = value;
        slots[slot].known = true;
        return true;
    }

    [[nodiscard]] const std::vector<std::byte>& DiskKey() const {
        return diskKey;
    }

private:
    mutable std::mutex mutex;
    std::vector<BufferSubstitute> slots;
    const std::vector<std::byte> diskKey;
};

struct CompiledVariant {
    BindingLayout layout;
    CompiledShaderInfo info;
    CompiledBindingLayout bindings;
    CompiledShaderArtifact artifact;
    std::shared_ptr<BufferSubstituteTable> substitutes;
};

}

#endif
