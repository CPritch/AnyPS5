#ifndef CORE_SHADER_RECOMPILER_COMPILEDVARIANT_HPP
#define CORE_SHADER_RECOMPILER_COMPILEDVARIANT_HPP

#include "Recompiler.hpp"
#include "IntermediateRepresentation/IrMetadata/CompiledShaderInfo.hpp"
#include "Optimization/include/Optimization/BindingAllocator.hpp"
#include "Optimization/include/Optimization/DescriptorBindingBuilder.hpp"
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

template<typename TSlot>
class SubstituteTable {
public:
    explicit SubstituteTable(std::vector<TSlot> slots, std::vector<std::byte> diskKey = {}) : slots(std::move(slots)), diskKey(std::move(diskKey)) {
        std::size_t count = 0;
        for (const auto& slot : this->slots) count += slot.known ? 1u : 0u;
        learned.store(count, std::memory_order_relaxed);
    }

    [[nodiscard]] std::vector<TSlot> Slots() const {
        std::lock_guard lock(mutex);
        return slots;
    }

    void CopySlots(std::vector<TSlot>& out) const {
        std::lock_guard lock(mutex);
        out.assign(slots.begin(), slots.end());
    }

    [[nodiscard]] std::size_t Learned() const {
        return learned.load(std::memory_order_acquire);
    }

    bool Learn(std::size_t slot, const TSlot& value) {
        std::lock_guard lock(mutex);
        if (slot >= slots.size() || slots[slot].known) return false;
        slots[slot] = value;
        slots[slot].known = true;
        learned.fetch_add(1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] const std::vector<std::byte>& DiskKey() const {
        return diskKey;
    }

private:
    mutable std::mutex mutex;
    std::vector<TSlot> slots;
    std::atomic<std::size_t> learned{0};
    const std::vector<std::byte> diskKey;
};

using BufferSubstituteTable = SubstituteTable<BufferSubstitute>;
using ImageSubstituteTable = SubstituteTable<ImageSubstitute>;

struct CompiledVariant {
    BindingLayout layout;
    CompiledShaderInfo info;
    CompiledBindingLayout bindings;
    CompiledShaderArtifact artifact;
    std::shared_ptr<BufferSubstituteTable> substitutes;
    std::shared_ptr<ImageSubstituteTable> imageSubstitutes;
};

}

#endif
