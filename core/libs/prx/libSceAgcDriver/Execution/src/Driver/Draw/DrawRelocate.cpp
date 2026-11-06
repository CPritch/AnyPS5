#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "ControlFlow/GraphBuilder.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include <string>
#include <span>
#include <optional>
#include <atomic>
#include <array>
#include <utility>
#include <stdexcept>
#include <memory>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::size_t MaxAliases = 16384;
constexpr std::size_t MaxContentWords = 64;

std::uint64_t AlignedWord(std::uint32_t word) {
    return word & ~std::uint32_t {3};
}

// A relocated copy's V# base words read again at their runs' new addresses, and the result fields they and the
// moved user words feed (VariantRebase). False when a base is unmapped, null or moved off its 4-byte alignment:
// then the bound address could not be what a capture at the new address binds.
bool RebaseMovedFields(DispatchVariant& copy, const DispatchVariant& variant, std::uint64_t moved, std::span<const std::pair<std::uint32_t, std::uint32_t>> movedValues, bool batch) {
    constexpr std::size_t WindowWords = 1024;
    thread_local std::array<std::uint32_t, WindowWords> window;
    std::size_t offset = 0;
    auto next = variant.movableWords.begin();
    const auto rebaseWord = [&](std::uint32_t position, std::uint32_t live) {
        const auto kept = variant.words[position];
        if (live == 0 || kept == 0 || ((live ^ kept) & 3u) != 0) return false;
        copy.words[position] = live;
        return true;
    };
    for (std::size_t run = 0; run < copy.runs.size() && next != variant.movableWords.end(); ++run) {
        const auto count = static_cast<std::size_t>((copy.runs[run].second - copy.runs[run].first) / sizeof(std::uint32_t));
        const auto base = run < copy.runBases.size() ? copy.runBases[run] : std::int8_t {-1};
        const bool shifted = base >= 0 && (moved >> base & 1u) != 0;
        auto first = next;
        while (next != variant.movableWords.end() && *next < offset + count) ++next;
        if (shifted) {
            while (first != next && *first < offset) ++first;
            if (batch && next - first >= 2 && static_cast<std::size_t>(*(next - 1) - *first) < WindowWords) {
                const auto span = static_cast<std::size_t>(*(next - 1) - *first) + 1u;
                const auto address = copy.runs[run].first + (*first - offset) * sizeof(std::uint32_t);
                if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(window).first(span))) == GuestMemory::Compare::Equal) {
                    for (auto at = first; at != next; ++at) {
                        if (!rebaseWord(*at, window[*at - *first])) return false;
                    }
                    first = next;
                }
            }
            for (; first != next; ++first) {
                std::uint32_t live = 0;
                const auto address = copy.runs[run].first + (*first - offset) * sizeof(std::uint32_t);
                if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(&live, 1))) != GuestMemory::Compare::Equal) return false;
                if (!rebaseWord(*first, live)) return false;
            }
        }
        offset += count;
    }
    for (const auto& rebase : variant.rebases) {
        if (rebase.userWord) {
            const auto found = std::find_if(movedValues.begin(), movedValues.end(), [&](const auto& value) { return value.first == rebase.source; });
            if (found != movedValues.end()) copy.rebasedWords.push_back({rebase.kind, rebase.target, rebase.word, found->second});
            continue;
        }
        const auto position = static_cast<std::size_t>(rebase.source);
        if (position >= copy.words.size() || copy.words[position] == variant.words[position]) continue;
        if (rebase.kind == VariantRebase::Kind::BufferWord) {
            copy.rebasedWords.push_back({rebase.kind, rebase.target, rebase.word, copy.words[position]});
            continue;
        }
        if (position + 1u >= copy.words.size() || rebase.target >= variant.compiled->vertexAttributes.size()) return false;
        const auto high = static_cast<std::uint64_t>(variant.words[position + 1u] & 0xffffu) << 32u;
        const auto delta = (copy.words[position] | high) - (variant.words[position] | high);
        const auto& fields = variant.compiled->vertexAttributes[rebase.target].resource.fields;
        const auto rebased = ((fields[0] | (static_cast<std::uint64_t>(fields[1] & 0xffffu) << 32u)) + delta) & 0xffffffffffffull;
        copy.rebasedWords.push_back({rebase.kind, rebase.target, 0, static_cast<std::uint32_t>(rebased)});
        copy.rebasedWords.push_back({rebase.kind, rebase.target, 1, (fields[1] & 0xffff0000u) | static_cast<std::uint32_t>(rebased >> 32u)});
    }
    return true;
}

}

bool Driver::drawRelocationEnabled() {
    static const bool enabled = std::getenv("APS5_NO_DRAW_RELOCATION") == nullptr;
    return enabled;
}

bool Driver::relocationDrain() {
    static const bool enabled = std::getenv("APS5_RELOCATION_DRAIN") != nullptr;
    return enabled;
}

bool Driver::poolRelocationEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_POOL_RELOCATION");
        return drawRelocationEnabled() && (value == nullptr || std::strcmp(value, "0") != 0);
    }();
    return enabled;
}

bool Driver::vertexTableRelocation(const DrawProgram& program, const ShaderRecompiler::ShaderVertexStageInfo& info, std::span<const Graphics::DecodeRead> reads, const ShaderRecompiler::RecompileResult& compiled, const ShaderRecompiler::SrtUserDataUse& use, PoolRelocation& pool) {
    constexpr auto MaxResources = ShaderRecompiler::ShaderVertexStageInfo::MaxResources;
    if (program.binary.stage != ShaderRecompiler::ShaderStage::Vertex || !info.fetchEmbedded || info.resourcesNum == 0 || info.resourcesNum > MaxResources || reads.size() != 2u * info.resourcesNum) return false;
    const auto table = program.hiddenUserWords + info.fetchBufferReg;
    const auto attributes = program.hiddenUserWords + info.fetchAttribReg;
    if (table + 1u >= program.userData.size() || attributes + 1u >= program.userData.size() || table + 1u >= ShaderRecompiler::SrtUserDataUse::TrackedWords) return false;
    const auto address = [&](std::uint32_t word) { return static_cast<std::uint64_t>(program.userData[word]) | (static_cast<std::uint64_t>(program.userData[word + 1u]) << 32u); };
    const auto tableAddress = address(table);
    const auto attributeAddress = address(attributes);
    for (std::uint32_t resource = 0; resource < info.resourcesNum; ++resource) {
        const auto& attribute = reads[2u * resource];
        const auto& sharp = reads[2u * resource + 1u];
        if (attribute.size != sizeof(std::uint32_t) || attribute.address < attributeAddress || (attribute.address - attributeAddress) % sizeof(std::uint32_t) != 0) return false;
        if (sharp.size != 4u * sizeof(std::uint32_t) || sharp.address < tableAddress || (sharp.address - tableAddress) % (4u * sizeof(std::uint32_t)) != 0 || sharp.address - tableAddress >= 4u * sizeof(std::uint32_t) * MaxResources) return false;
    }
    for (const auto& attribute : compiled.vertexAttributes) {
        if (attribute.location >= info.resourcesNum || attribute.resource.fields != info.resources[attribute.location].fields) return false;
    }
    if (((use.asValue | use.asBufferBase) >> table & 1u) != 0) return true;
    for (std::uint32_t resource = 0; resource < info.resourcesNum; ++resource) {
        const auto& sharp = reads[2u * resource + 1u];
        for (std::uint32_t word = 0; word < 4u; ++word) pool.reads.emplace_back(sharp.address + word * sizeof(std::uint32_t), static_cast<std::int32_t>(table));
        if (attributes == table) pool.reads.emplace_back(reads[2u * resource].address, static_cast<std::int32_t>(table));
        pool.movable.push_back(sharp.address);
    }
    for (std::uint32_t index = 0; index < compiled.vertexAttributes.size(); ++index) pool.rebases.push_back({VariantRebase::Kind::VertexBase, false, index, 0, reads[2u * compiled.vertexAttributes[index].location + 1u].address});
    pool.words |= std::uint64_t {1} << table;
    return true;
}

void Driver::bufferBaseRelocation(const ShaderRecompiler::ResourceCapture& capture, const ShaderRecompiler::RecompileResult& compiled, std::uint32_t hiddenWords, PoolRelocation& pool) {
    const auto& use = capture.userDataUse;
    if (use.bufferBases.empty() || capture.plan == nullptr) return;
    const auto& buffers = capture.snapshot.buffers;
    // Where each buffer resource's base dword came from; a resource the walk took it from twice, differently, has
    // none, as has one whose descriptor aliases an image.
    struct Source {
        bool known = false;
        bool mixed = false;
        ShaderRecompiler::SrtBufferBase base{};
    };
    std::vector<Source> sources(buffers.size());
    for (const auto& base : use.bufferBases) {
        if (base.resource >= sources.size()) return;
        auto& source = sources[base.resource];
        const auto same = [&](const ShaderRecompiler::SrtBufferBase& other) { return other.word == base.word && (base.word >= 0 || (other.address == base.address && other.origin == base.origin)); };
        if (source.known && !same(source.base)) source.mixed = true;
        source.known = true;
        source.base = base;
    }
    const auto& planned = capture.plan->info.buffers;
    for (std::size_t resource = 0; resource < sources.size(); ++resource) {
        if (resource >= planned.size() || planned[resource].imageAlias != ShaderRecompiler::BufferResource::NoImageAlias) sources[resource].mixed = true;
    }
    // A bound element is a snapshot buffer's four words; when several buffers hold its words, it is rebased only if
    // they all took their base from the same place, and those places are kept still otherwise.
    std::uint64_t pinnedWords = 0;
    std::vector<std::uint64_t> pinnedReads;
    std::vector<VariantRebase> rebases;
    const auto pin = [&](const Source& source) {
        if (!source.known) return;
        if (source.base.word >= 0) pinnedWords |= std::uint64_t {1} << source.base.word;
        else pinnedReads.push_back(source.base.address);
    };
    for (std::uint32_t index = 0; index < compiled.bindings.size(); ++index) {
        const auto& binding = compiled.bindings[index];
        if (binding.role != ShaderRecompiler::DescriptorRole::GuestBuffers) continue;
        if (binding.guestDescriptor.size() != static_cast<std::size_t>(binding.count) * 4u) return;
        for (std::uint32_t element = 0; element < binding.count; ++element) {
            const auto* words = binding.guestDescriptor.data() + element * 4u;
            const Source* first = nullptr;
            bool uniform = true;
            for (std::size_t resource = 0; resource < buffers.size(); ++resource) {
                const auto& value = buffers[resource];
                if (value.dwordCount != 4u || !std::equal(words, words + 4, value.dwords.begin())) continue;
                const auto& source = sources[resource];
                if (first == nullptr) first = &source;
                uniform = uniform && source.known && !source.mixed && first->known && source.base.word == first->base.word && (source.base.word >= 0 || (source.base.address == first->base.address && source.base.origin == first->base.origin));
            }
            if (first == nullptr) return;
            if (!uniform) {
                for (std::size_t resource = 0; resource < buffers.size(); ++resource) {
                    if (buffers[resource].dwordCount == 4u && std::equal(words, words + 4, buffers[resource].dwords.begin())) pin(sources[resource]);
                }
                continue;
            }
            const auto& base = first->base;
            rebases.push_back({VariantRebase::Kind::BufferWord, base.word >= 0, index, element * 4u, base.word >= 0 ? static_cast<std::uint64_t>(base.word) : base.address});
        }
    }
    const auto flat = std::ranges::find(compiled.bindings, ShaderRecompiler::DescriptorRole::FlattenedSrt, &ShaderRecompiler::DescriptorBinding::role);
    std::vector<std::uint64_t> flatRebased;
    for (const auto& rebase : rebases) {
        if (rebase.userWord) {
            const auto word = static_cast<std::uint32_t>(rebase.source);
            if (word < hiddenWords || word >= 64u || ((use.asValue | pinnedWords) >> word & 1u) != 0) continue;
            pool.words |= std::uint64_t {1} << word;
            pool.rebases.push_back(rebase);
            continue;
        }
        // A table word moves with the table's pointer when the walk read it for nothing but buffer bases and, for
        // a flat SRT slot, the flat copy, which the result's FlattenedSrt binding holds and is rebased with it.
        const auto address = rebase.source;
        const auto base = std::ranges::find(use.bufferBases, address, &ShaderRecompiler::SrtBufferBase::address);
        if (base == use.bufferBases.end() || base->origin < 0 || (address & 3u) != 0 || std::ranges::find(pinnedReads, address) != pinnedReads.end()) continue;
        if (std::ranges::any_of(use.bufferBases, [&](const auto& other) { return other.word < 0 && other.address == address && (other.origin != base->origin || other.slot != base->slot); })) continue;
        const auto reads = std::ranges::count(use.reads, address, &std::pair<std::uint64_t, std::int32_t>::first);
        if (base->slot < 0 ? reads != 0 : reads > 1 || std::ranges::find(use.valueSlots, static_cast<std::uint32_t>(base->slot)) != use.valueSlots.end()) continue;
        if (base->slot >= 0 && flat != compiled.bindings.end() && std::ranges::find(flatRebased, address) == flatRebased.end()) {
            const auto slot = static_cast<std::size_t>(base->slot);
            const auto offset = slot < capture.plan->srtReads.size() ? capture.plan->srtReads[slot].flatOffset : ~std::uint32_t {0};
            if (offset < flat->guestDescriptor.size()) {
                if (flat->guestDescriptor[offset] != buffers[base->resource].dwords[0]) continue;
                pool.rebases.push_back({VariantRebase::Kind::BufferWord, false, static_cast<std::uint32_t>(flat - compiled.bindings.begin()), offset, address});
            }
            flatRebased.push_back(address);
        }
        pool.movable.push_back(address);
        pool.rebases.push_back(rebase);
    }
}

std::uint64_t Driver::relocatedContentKey(const RelocatableUserWord& word, std::uint32_t value, bool& readable) {
    std::uint64_t hash = 0x9e3779b97f4a7c15ull;
    const auto mix = [&](std::uint64_t mixed) {
        hash ^= mixed;
        hash *= 0x100000001b3ull;
    };
    if (word.value) return hash;
    mix(value & 3u);
    if (value == 0) {
        readable = false;
        return hash;
    }
    std::array<std::uint32_t, MaxContentWords> content;
    std::uint32_t index = 0;
    auto masked = word.masked.begin();
    for (const auto& [offset, words] : word.reads) {
        mix(static_cast<std::uint64_t>(offset));
        mix(words);
        for (std::size_t done = 0; done < words; done += MaxContentWords) {
            const auto count = std::min<std::size_t>(words - done, MaxContentWords);
            const auto address = (static_cast<std::uint64_t>(word.high) << 32u) + AlignedWord(value) + static_cast<std::uint64_t>(offset) + done * sizeof(std::uint32_t);
            if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(content).first(count))) != GuestMemory::Compare::Equal) {
                readable = false;
                return hash;
            }
            for (std::size_t i = 0; i < count; ++i, ++index) {
                while (masked != word.masked.end() && *masked < index) ++masked;
                if (masked == word.masked.end() || *masked != index) mix(content[i]);
            }
        }
    }
    return hash;
}

void Driver::tagUserValueWords(DispatchVariant& variant, const DrawProgram& program) {
    if (!drawRelocationEnabled() || variant.compiled == nullptr) return;
    const auto& compiled = *variant.compiled;
    const auto& forwarded = compiled.shaderDataUserWords;
    const auto users = std::min<std::size_t>(program.userData.size(), 64);
    const auto bound = [&](std::uint32_t value) {
        std::size_t count = 0;
        for (const auto& binding : compiled.bindings) count += static_cast<std::size_t>(std::count(binding.guestDescriptor.begin(), binding.guestDescriptor.end(), value));
        return count;
    };
    const auto elsewhere = [&](std::uint32_t index, std::uint32_t value) {
        for (std::size_t slot = 0; (slot + 1) * sizeof(std::uint32_t) <= compiled.pushConstants.size(); ++slot) {
            std::uint32_t word = 0;
            std::memcpy(&word, compiled.pushConstants.data() + slot * sizeof(std::uint32_t), sizeof(word));
            if (word == value && (slot >= forwarded.size() || forwarded[slot] != index)) return true;
        }
        if (std::any_of(compiled.specialization.begin(), compiled.specialization.end(), [&](const auto& constant) { return constant.value == value; })) return true;
        return std::find(variant.words.begin(), variant.words.end(), value) != variant.words.end();
    };
    const auto readThrough = [&](const std::uint32_t* descriptor) {
        const auto base = static_cast<std::uint64_t>(descriptor[0]) | static_cast<std::uint64_t>(descriptor[1] & 0xffffu) << 32u;
        const auto stride = (descriptor[1] >> 16u) & 0x3fffu;
        const auto bytes = std::max<std::uint64_t>(stride != 0 ? static_cast<std::uint64_t>(stride) * descriptor[2] : descriptor[2], 16u);
        return std::any_of(variant.runs.begin(), variant.runs.end(), [&](const auto& run) { return run.first < base + bytes && run.second > base; });
    };
    const auto taken = [&](std::uint32_t index) { return ((variant.relocatableWords | variant.valueWords | variant.calleeWords) >> index & 1u) != 0; };
    if (ShaderRecompiler::GcnGuestShaders() && program.binary.stage == ShaderRecompiler::ShaderStage::Vertex) {
        for (const auto instruction : program.binary.code) {
            if ((instruction & 0xff80ff00u) != 0xbe802100u) continue;
            const auto target = instruction & 0xffu;
            if (target < program.firstUserSgpr) continue;
            const auto index = program.hiddenUserWords + target - program.firstUserSgpr;
            if (index + 1 >= users || taken(index) || taken(index + 1)) continue;
            const auto low = program.userData[index];
            const auto address = static_cast<std::uint64_t>(low) | static_cast<std::uint64_t>(program.userData[index + 1]) << 32u;
            if (address == 0 || address % 4u != 0 || bound(low) != 0 || elsewhere(index, low)) continue;
            std::array<std::uint32_t, 1024> code;
            if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(code))) != GuestMemory::Compare::Equal) continue;
            std::optional<std::uint32_t> length;
            try {
                length = ShaderRecompiler::UserDataCalleeLength(code, (instruction >> 16u) & 0x7fu);
            } catch (const std::exception&) {
                continue;
            }
            if (!length || *length == 0) continue;
            variant.calleeWords |= std::uint64_t {1} << index;
            variant.calleeReads.emplace_back(index, *length);
        }
    }
    for (std::uint32_t index = program.hiddenUserWords; index + 2 < users; ++index) {
        const auto base = program.userData[index];
        if (base == 0 || taken(index) || bound(base) != 1 || elsewhere(index, base)) continue;
        std::uint32_t binding = 0, word = 0;
        for (std::size_t b = 0; b < compiled.bindings.size(); ++b) {
            const auto& descriptor = compiled.bindings[b].guestDescriptor;
            const auto found = std::find(descriptor.begin(), descriptor.end(), base);
            if (found == descriptor.end()) continue;
            binding = static_cast<std::uint32_t>(b);
            word = static_cast<std::uint32_t>(found - descriptor.begin());
        }
        const auto& target = compiled.bindings[binding];
        const auto element = word / 4u;
        const bool readOnly = target.role == ShaderRecompiler::DescriptorRole::GuestBuffers && target.guestDescriptor.size() == static_cast<std::size_t>(target.count) * 4u && word % 4u == 0 && element < target.bufferWritten.size() && !target.bufferWritten[element] && (element >= target.bufferAtomic.size() || !target.bufferAtomic[element]);
        if (!readOnly || target.guestDescriptor[word + 1] != program.userData[index + 1] || target.guestDescriptor[word + 2] != program.userData[index + 2] || readThrough(target.guestDescriptor.data() + word)) continue;
        variant.valueWords |= std::uint64_t {1} << index;
        variant.valueSlots.push_back({index, binding, word});
        const auto records = program.userData[index + 2];
        if (records != 0 && !taken(index + 2) && bound(records) == 1 && !elsewhere(index + 2, records)) {
            variant.valueWords |= std::uint64_t {1} << (index + 2);
            variant.valueSlots.push_back({index + 2, binding, word + 2});
        }
    }
}

void Driver::tagVariantRuns(DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, const ShaderRecompiler::SrtUserDataUse& use, bool eligible, std::uint32_t hiddenWords, std::span<const std::shared_ptr<const ShaderRecompiler::SrtGuestWords>> supplied, const PoolRelocation* pool) {
    std::uint64_t relocatable = eligible && drawRelocationEnabled() ? use.asBase & ~(use.asValue | use.asBufferBase) : 0;
    if (pool != nullptr && eligible && drawRelocationEnabled()) relocatable |= pool->words;
    if (hiddenWords != 0) relocatable &= hiddenWords >= 64 ? 0 : ~((std::uint64_t {1} << hiddenWords) - 1);
    std::vector<std::pair<std::uint64_t, std::int32_t>> reads;
    const auto covered = [&](std::uint64_t address) {
        return std::any_of(regions.begin(), regions.end(), [&](const ShaderRecompiler::MemoryRegion& region) { return address >= region.guestAddress && address - region.guestAddress + sizeof(std::uint32_t) <= region.bytes.size(); });
    };
    if (relocatable != 0) {
        reads = use.reads;
        for (const auto& base : use.bufferBases) {
            if (base.word < 0) reads.emplace_back(base.address, base.origin);
        }
        if (pool != nullptr) reads.insert(reads.end(), pool->reads.begin(), pool->reads.end());
        std::sort(reads.begin(), reads.end());
        reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
        const auto drop = [&](std::int32_t origin) {
            if (origin >= 0) relocatable &= ~(std::uint64_t {1} << origin);
        };
        for (std::size_t i = 0; i < reads.size(); ++i) {
            if (i + 1 < reads.size() && reads[i].first == reads[i + 1].first) {
                drop(reads[i].second);
                drop(reads[i + 1].second);
            }
            if (reads[i].second >= 0 && ((reads[i].first & 3u) != 0 || !covered(reads[i].first))) drop(reads[i].second);
        }
    }
    const auto origin = [&](std::uint64_t address) -> std::int8_t {
        const auto found = std::lower_bound(reads.begin(), reads.end(), std::make_pair(address, std::numeric_limits<std::int32_t>::min()));
        if (found == reads.end() || found->first != address || found->second < 0) return -1;
        return (relocatable >> found->second & 1u) != 0 ? static_cast<std::int8_t>(found->second) : std::int8_t {-1};
    };
    variant.relocatableWords = relocatable;
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const auto& region = regions[index];
        const auto count = region.bytes.size() / sizeof(std::uint32_t);
        const auto offset = variant.words.size();
        variant.words.resize(offset + count);
        std::memcpy(variant.words.data() + offset, region.bytes.data(), count * sizeof(std::uint32_t));
        if (relocatable == 0) {
            AppendSharedWords(variant.shared, variant.runs.size(), offset, region.bytes, supplied);
            variant.runs.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
            continue;
        }
        std::size_t start = 0;
        auto current = count != 0 ? origin(region.guestAddress) : std::int8_t {-1};
        for (std::size_t word = 1; word <= count; ++word) {
            const auto next = word < count ? origin(region.guestAddress + word * sizeof(std::uint32_t)) : std::int8_t {-2};
            if (next == current) continue;
            AppendSharedWords(variant.shared, variant.runs.size(), offset + start, region.bytes.subspan(start * sizeof(std::uint32_t), (word - start) * sizeof(std::uint32_t)), supplied);
            variant.runs.emplace_back(region.guestAddress + start * sizeof(std::uint32_t), region.guestAddress + word * sizeof(std::uint32_t));
            variant.runBases.push_back(current);
            start = word;
            current = next;
        }
    }
    if (pool == nullptr || relocatable == 0) return;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> movable;
    std::size_t offset = 0;
    for (std::size_t run = 0; run < variant.runs.size(); ++run) {
        const auto [begin, end] = variant.runs[run];
        if (run < variant.runBases.size() && variant.runBases[run] >= 0) {
            for (const auto address : pool->movable) {
                if (address >= begin && address < end && (address - begin) % sizeof(std::uint32_t) == 0) movable.emplace_back(address, static_cast<std::uint32_t>(offset + (address - begin) / sizeof(std::uint32_t)));
            }
        }
        offset += static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
    }
    for (const auto& [address, position] : movable) variant.movableWords.push_back(position);
    std::sort(variant.movableWords.begin(), variant.movableWords.end());
    variant.movableWords.erase(std::unique(variant.movableWords.begin(), variant.movableWords.end()), variant.movableWords.end());
    for (const auto& rebase : pool->rebases) {
        if (rebase.userWord) {
            if (rebase.source < 64u && (relocatable >> rebase.source & 1u) != 0) variant.rebases.push_back(rebase);
            continue;
        }
        const auto found = std::find_if(movable.begin(), movable.end(), [&](const auto& entry) { return entry.first == rebase.source; });
        if (found == movable.end()) continue;
        auto kept = rebase;
        kept.source = found->second;
        variant.rebases.push_back(kept);
    }
}

namespace {

template <typename TMap>
std::size_t dropEveryOther(TMap& map) {
    std::size_t dropped = 0;
    bool drop = false;
    for (auto it = map.begin(); it != map.end();) {
        drop = !drop;
        if (!drop) {
            ++it;
            continue;
        }
        it = map.erase(it);
        ++dropped;
    }
    return dropped;
}

}

void Driver::learnPendingRelocation(const DrawRelocationPending& pending) {
    if (pending.key == 0) return;
    const auto inserted = drawCache.find(pending.key);
    if (inserted == drawCache.end() || inserted->second->decode == nullptr) return;
    const auto& entry = *inserted->second;
    const auto& programs = entry.decode->programs;
    if (entry.stages.size() != programs.size()) return;
    const auto known = drawRelocations.find(pending.identity);
    auto shape = std::make_shared<DrawRelocation>();
    if (known != drawRelocations.end()) *shape = *known->second;
    bool changed = known == drawRelocations.end();
    for (std::size_t i = 0; i < programs.size(); ++i) {
        if (entry.stages[i].empty()) continue;
        const auto& variant = *entry.stages[i].front();
        const auto& program = programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size() && index < 64; ++index) {
            const auto reg = program.userDataBase + index - program.hiddenUserWords;
            const auto held = std::find_if(shape->words.begin(), shape->words.end(), [&](const RelocatableUserWord& word) { return word.reg == reg; });
            const bool relocatable = (variant.relocatableWords >> index & 1u) != 0;
            const bool value = !relocatable && (variant.valueWords >> index & 1u) != 0;
            if (!relocatable && !value && (variant.calleeWords >> index & 1u) == 0) {
                if (held == shape->words.end()) continue;
                shape->words.erase(held);
                shape->banned.push_back(reg);
                changed = true;
                continue;
            }
            if (held != shape->words.end() || std::find(shape->banned.begin(), shape->banned.end(), reg) != shape->banned.end()) continue;
            RelocatableUserWord word{reg, {}, value};
            for (const auto& [callee, length] : variant.calleeReads) {
                if (callee != index) continue;
                word.reads.emplace_back(0, length);
                word.high = program.userData[index + 1];
            }
            const auto base = AlignedWord(program.userData[index]);
            std::size_t offset = 0;
            std::uint32_t read = 0;
            for (std::size_t run = 0; run < variant.runs.size() && run < variant.runBases.size(); ++run) {
                const auto count = static_cast<std::uint32_t>((variant.runs[run].second - variant.runs[run].first) / sizeof(std::uint32_t));
                if (variant.runBases[run] == static_cast<std::int8_t>(index)) {
                    word.reads.emplace_back(static_cast<std::int64_t>(variant.runs[run].first - base), count);
                    for (auto position = std::lower_bound(variant.movableWords.begin(), variant.movableWords.end(), offset); position != variant.movableWords.end() && *position < offset + count; ++position) word.masked.push_back(read + static_cast<std::uint32_t>(*position - offset));
                    read += count;
                }
                offset += count;
            }
            shape->words.push_back(std::move(word));
            changed = true;
        }
    }
    if (changed) {
        std::sort(shape->words.begin(), shape->words.end(), [](const RelocatableUserWord& a, const RelocatableUserWord& b) { return a.reg < b.reg; });
        drawRelocations[pending.identity] = std::move(shape);
    }
    if (pending.alias == 0) return;
    if (drawAliases.size() >= std::max(MaxAliases, 2u * drawCacheEntries())) drawRelocationCounters.evicted += dropEveryOther(drawAliases);
    drawAliases[pending.alias] = pending.key;
}

namespace {

struct RelocationMove {
    std::size_t program;
    std::uint32_t index, from, to;
};

struct RelocationMoveSet {
    std::uint64_t words = 0;
    std::array<std::uint64_t, 64> delta;
    std::array<std::uint32_t, 64> to;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> values;
};

bool inPlaceRelocation() {
    static const bool enabled = std::getenv("APS5_NO_INPLACE_RELOCATION") == nullptr;
    return enabled;
}

constexpr std::size_t MaxRecycledVariants = 4096;
constexpr std::size_t MaxRecycledEntries = 1024;
constexpr std::size_t RecycleProbes = 32;

template <typename T, typename Tag>
class RecyclingRing {
public:
    struct Slot {
        std::shared_ptr<T> object;
        Tag tag;
    };

    Slot* Acquire(std::size_t capacity) {
        const auto probes = std::min(slots.size(), RecycleProbes);
        for (std::size_t probe = 0; probe < probes; ++probe) {
            auto& slot = slots[cursor];
            cursor = cursor + 1 == slots.size() ? 0 : cursor + 1;
            if (slot.object.use_count() == 1) return &slot;
        }
        if (slots.size() >= capacity) return nullptr;
        slots.push_back({std::make_shared<T>(), Tag{}});
        return &slots.back();
    }

private:
    std::vector<Slot> slots;
    std::size_t cursor = 0;
};

struct NoTag {};

using RecycledVariants = RecyclingRing<DispatchVariant, std::weak_ptr<const DispatchVariant>>;
using RecycledEntries = RecyclingRing<DrawEntry, NoTag>;

bool SameOwner(const std::weak_ptr<const DispatchVariant>& from, const std::shared_ptr<DispatchVariant>& donor) {
    return !from.owner_before(donor) && !donor.owner_before(from) && !from.expired();
}

void SeedFromDonor(DispatchVariant& copy, const DispatchVariant& variant, bool sameDonor) {
    copy.runs.assign(variant.runs.begin(), variant.runs.end());
    copy.runBases.assign(variant.runBases.begin(), variant.runBases.end());
    copy.relocatableWords = variant.relocatableWords;
    copy.valueWords = variant.valueWords;
    copy.valueSlots.assign(variant.valueSlots.begin(), variant.valueSlots.end());
    copy.calleeWords = variant.calleeWords;
    copy.calleeReads.assign(variant.calleeReads.begin(), variant.calleeReads.end());
    if (sameDonor && copy.words.size() == variant.words.size()) {
        for (const auto position : variant.movableWords) copy.words[position] = variant.words[position];
    } else {
        copy.words.assign(variant.words.begin(), variant.words.end());
    }
    copy.forgetSerial = variant.forgetSerial;
    copy.compiled = variant.compiled;
    copy.meshIndexDescriptor = variant.meshIndexDescriptor;
    copy.shader = variant.shader;
    copy.dataPositions.assign(variant.dataPositions.begin(), variant.dataPositions.end());
    copy.dataSlots.assign(variant.dataSlots.begin(), variant.dataSlots.end());
    copy.flatBinding = variant.flatBinding;
    copy.pushOffset = variant.pushOffset;
    copy.vertexInfo = variant.vertexInfo;
    copy.shared.clear();
    copy.registeredViews.reset();
    copy.recipe.store(nullptr);
    copy.userDataMask = ~std::uint64_t {0};
    copy.memory.reset();
    copy.captured.clear();
    copy.spans.clear();
    copy.generation.store(0);
    copy.shaderDataPatches.clear();
    copy.descriptorPatches.clear();
    copy.movableWords.clear();
    copy.rebases.clear();
    copy.rebasedWords.clear();
}

void ApplyIndexedMoves(DispatchVariant& copy, const DispatchVariant& variant, const RelocationMoveSet& set) {
    for (std::size_t run = 0; run < copy.runs.size() && run < copy.runBases.size(); ++run) {
        const auto base = copy.runBases[run];
        if (base < 0 || base >= 64 || (set.words >> base & 1u) == 0) continue;
        copy.runs[run].first += set.delta[static_cast<std::size_t>(base)];
        copy.runs[run].second += set.delta[static_cast<std::size_t>(base)];
    }
    const auto& forwarded = variant.compiled->shaderDataUserWords;
    const auto patchesBefore = copy.shaderDataPatches.size();
    for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
        const auto word = forwarded[slot];
        if (word < 64 && (set.words >> word & 1u) != 0) copy.shaderDataPatches.emplace_back(slot, set.to[word]);
    }
    std::stable_sort(copy.shaderDataPatches.begin() + static_cast<std::ptrdiff_t>(patchesBefore), copy.shaderDataPatches.end(), [&](const auto& left, const auto& right) { return forwarded[left.first] < forwarded[right.first]; });
    for (const auto& [index, binding, word] : variant.valueSlots) {
        if (index < 64 && (set.words >> index & 1u) != 0) copy.descriptorPatches.push_back({binding, word, set.to[index]});
    }
}

const char* DifferingField(const DispatchVariant& a, const DispatchVariant& b) {
    const auto sameRebased = [](const RebasedWord& x, const RebasedWord& y) { return x.kind == y.kind && x.target == y.target && x.word == y.word && x.value == y.value; };
    if (a.runs != b.runs) return "runs";
    if (a.runBases != b.runBases) return "run bases";
    if (a.words != b.words) return "words";
    if (a.relocatableWords != b.relocatableWords || a.valueWords != b.valueWords || a.calleeWords != b.calleeWords) return "tagged words";
    if (a.valueSlots != b.valueSlots) return "value slots";
    if (a.calleeReads != b.calleeReads) return "callee reads";
    if (a.forgetSerial != b.forgetSerial || a.pushOffset != b.pushOffset || a.flatBinding != b.flatBinding || a.userDataMask != b.userDataMask) return "scalars";
    if (a.compiled != b.compiled || a.shader != b.shader || a.vertexInfo != b.vertexInfo || a.registeredViews != b.registeredViews || a.memory != b.memory) return "shared state";
    if (a.meshIndexDescriptor != b.meshIndexDescriptor) return "mesh index descriptor";
    if (a.dataPositions != b.dataPositions || a.dataSlots != b.dataSlots) return "data positions";
    if (a.shaderDataPatches != b.shaderDataPatches) return "shader data patches";
    if (a.descriptorPatches != b.descriptorPatches) return "descriptor patches";
    if (a.movableWords != b.movableWords || !a.rebases.empty() != !b.rebases.empty()) return "movable words";
    if (!std::equal(a.rebasedWords.begin(), a.rebasedWords.end(), b.rebasedWords.begin(), b.rebasedWords.end(), sameRebased)) return "rebased words";
    if (!a.shared.empty() || !b.shared.empty() || !a.captured.empty() || !b.captured.empty() || !a.spans.empty() || !b.spans.empty() || a.recipe.load() != b.recipe.load() || a.generation.load() != b.generation.load()) return "leftover state";
    return nullptr;
}

}

void Driver::relocateDrawEntry(const QueueState& queue, std::uint64_t alias, std::shared_ptr<DrawEntry>& entry, std::shared_ptr<const DrawDecode>& decode, std::vector<DrawUserPatch>& patches) {
    static const bool indexedMoves = std::getenv("APS5_NO_RELOCATE_INDEX") == nullptr;
    auto& counters = drawRelocationCounters;
    ++counters.lookups;
    const auto found = drawAliases.find(alias);
    if (found == drawAliases.end()) return;
    const auto cached = drawCache.find(found->second);
    if (cached == drawCache.end()) {
        drawAliases.erase(found);
        return;
    }
    if (cached->second->decode == nullptr) return;
    const auto donor = cached->second;
    const auto& programs = donor->decode->programs;
    if (donor->stages.size() != programs.size()) return;
    using Move = RelocationMove;
    thread_local std::vector<Move> moves;
    moves.clear();
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
            const auto current = readUserData(queue.shader, program.userDataBase + index - program.hiddenUserWords);
            if (current != program.userData[index]) moves.push_back({i, index, program.userData[index], current});
        }
    }
    if (moves.empty()) return;
    ++counters.donors;
    const bool inPlace = indexedMoves && inPlaceRelocation();
    const bool verify = inPlace && verifyDrawDataHits();
    thread_local RecycledVariants recycledVariants;
    thread_local RecycledEntries recycledEntries;
    std::shared_ptr<DrawEntry> relocated;
    if (inPlace) {
        if (auto* slot = recycledEntries.Acquire(MaxRecycledEntries)) {
            relocated = slot->object;
            for (auto& stage : relocated->stages) stage.clear();
            relocated->decode = donor->decode;
            relocated->recipes.store(nullptr);
            relocated->touched = 0;
            relocated->shape = 0;
            relocated->order = {};
        }
    }
    if (relocated == nullptr) {
        relocated = std::make_shared<DrawEntry>();
        relocated->decode = donor->decode;
    }
    relocated->stages.resize(donor->stages.size());
    thread_local RelocationMoveSet set;
    for (std::size_t i = 0; i < donor->stages.size(); ++i) {
        const bool moved = std::any_of(moves.begin(), moves.end(), [&](const Move& move) { return move.program == i; });
        if (!moved) {
            relocated->stages[i] = donor->stages[i];
            continue;
        }
        set.words = 0;
        set.values.clear();
        for (const auto& move : moves) {
            if (move.program != i || move.index >= 64) continue;
            set.words |= std::uint64_t {1} << move.index;
            set.values.emplace_back(move.index, move.to);
            set.delta[move.index] = AlignedWord(move.to) - AlignedWord(move.from);
            set.to[move.index] = move.to;
        }
        const auto movedWords = set.words;
        const auto& movedValues = set.values;
        const auto buildFresh = [&](const std::shared_ptr<DispatchVariant>& variant) {
            auto copy = std::make_shared<DispatchVariant>();
            copy->runs = variant->runs;
            copy->runBases = variant->runBases;
            copy->relocatableWords = variant->relocatableWords;
            copy->valueWords = variant->valueWords;
            copy->valueSlots = variant->valueSlots;
            copy->calleeWords = variant->calleeWords;
            copy->calleeReads = variant->calleeReads;
            copy->words = variant->words;
            copy->forgetSerial = variant->forgetSerial;
            copy->compiled = variant->compiled;
            copy->meshIndexDescriptor = variant->meshIndexDescriptor;
            copy->shader = variant->shader;
            copy->dataPositions = variant->dataPositions;
            copy->dataSlots = variant->dataSlots;
            copy->flatBinding = variant->flatBinding;
            copy->pushOffset = variant->pushOffset;
            copy->vertexInfo = variant->vertexInfo;
            if (indexedMoves) {
                ApplyIndexedMoves(*copy, *variant, set);
            } else {
                for (const auto& move : moves) {
                    if (move.program != i) continue;
                    const auto delta = AlignedWord(move.to) - AlignedWord(move.from);
                    for (std::size_t run = 0; run < copy->runs.size() && run < copy->runBases.size(); ++run) {
                        if (copy->runBases[run] != static_cast<std::int8_t>(move.index)) continue;
                        copy->runs[run].first += delta;
                        copy->runs[run].second += delta;
                    }
                    const auto& forwarded = variant->compiled->shaderDataUserWords;
                    for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
                        if (forwarded[slot] == move.index) copy->shaderDataPatches.emplace_back(slot, move.to);
                    }
                    for (const auto& [index, binding, word] : variant->valueSlots) {
                        if (index == move.index) copy->descriptorPatches.push_back({binding, word, move.to});
                    }
                }
            }
            return copy;
        };
        for (const auto& variant : donor->stages[i]) {
            if (variant == nullptr || variant->compiled == nullptr) continue;
            const bool allowed = std::all_of(moves.begin(), moves.end(), [&](const Move& move) { return move.program != i || (move.index < 64 && move.from != 0 && move.to != 0 && ((((variant->relocatableWords | variant->calleeWords) >> move.index & 1u) != 0 && ((move.from ^ move.to) & 3u) == 0) || (variant->valueWords >> move.index & 1u) != 0)); });
            if (!allowed) {
                ++counters.refused;
                continue;
            }
            std::shared_ptr<DispatchVariant> copy;
            if (inPlace) {
                if (auto* slot = recycledVariants.Acquire(MaxRecycledVariants)) {
                    const bool sameDonor = SameOwner(slot->tag, variant);
                    slot->tag.reset();
                    SeedFromDonor(*slot->object, *variant, sameDonor);
                    ApplyIndexedMoves(*slot->object, *variant, set);
                    slot->tag = variant;
                    copy = slot->object;
                }
            }
            const bool recycled = copy != nullptr;
            if (!recycled) copy = buildFresh(variant);
            const bool rebased = !((!variant->movableWords.empty() || !variant->rebases.empty()) && !RebaseMovedFields(*copy, *variant, movedWords, movedValues, recycled));
            if (verify && recycled) {
                auto check = buildFresh(variant);
                const bool checked = !((!variant->movableWords.empty() || !variant->rebases.empty()) && !RebaseMovedFields(*check, *variant, movedWords, movedValues, false));
                const char* differing = checked != rebased ? "rebase outcome" : rebased ? DifferingField(*copy, *check) : nullptr;
                if (differing != nullptr) throw std::runtime_error(std::string("APS5_VERIFY_DRAW_DATA_HITS: an in-place relocated variant differs from a fresh copy in its ") + differing);
            }
            if (!rebased) {
                ++counters.unreadBases;
                continue;
            }
            relocated->stages[i].push_back(std::move(copy));
        }
    }
    patches.clear();
    for (const auto& move : moves) patches.push_back({move.program, move.index, move.to});
    entry = std::move(relocated);
    decode = donor->decode;
}

namespace {

struct PatchMemoEntry {
    std::shared_ptr<const ShaderRecompiler::RecompileResult> base;
    std::shared_ptr<const ShaderRecompiler::ShaderVertexStageInfo> baseInfo;
    std::size_t kind = 0;
    std::vector<std::uint32_t> key;
    std::shared_ptr<const ShaderRecompiler::RecompileResult> result;
    std::shared_ptr<const ShaderRecompiler::ShaderVertexStageInfo> info;
};

std::size_t PatchMemoSlots() {
    static const std::size_t slots = [] {
        const char* text = std::getenv("APS5_PATCH_MEMO_SLOTS");
        const auto requested = text == nullptr ? std::size_t{1024} : static_cast<std::size_t>(std::strtoull(text, nullptr, 10));
        return std::max<std::size_t>(requested, 1);
    }();
    return slots;
}
std::array<std::array<std::atomic<std::uint64_t>, 2>, 2> patchMemoCounts{};

bool patchMemoEnabled() {
    static const bool enabled = std::getenv("APS5_NO_PATCH_MEMO") == nullptr;
    return enabled;
}

std::vector<PatchMemoEntry>& patchMemo() {
    thread_local std::vector<PatchMemoEntry> slots(PatchMemoSlots());
    return slots;
}

std::size_t patchMemoSlot(std::size_t kind, const void* base, const void* baseInfo, std::span<const std::uint32_t> key) {
    std::uint64_t hash = 0xcbf29ce484222325ull ^ kind;
    const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 0x100000001b3ull; };
    mix(reinterpret_cast<std::uintptr_t>(base));
    mix(reinterpret_cast<std::uintptr_t>(baseInfo));
    for (const auto word : key) mix(word);
    return static_cast<std::size_t>((hash ^ (hash >> 29u)) % PatchMemoSlots());
}

thread_local std::vector<std::uint32_t> patchKey;

}

std::optional<Driver::PatchMemoHit> Driver::findPatchMemo(std::size_t kind, const std::shared_ptr<const ShaderRecompiler::RecompileResult>& base, const std::shared_ptr<const ShaderRecompiler::ShaderVertexStageInfo>& baseInfo, std::span<const std::uint32_t> key) {
    if (!patchMemoEnabled()) return std::nullopt;
    patchMemoCounts[kind][0].fetch_add(1, std::memory_order_relaxed);
    const auto& entry = patchMemo()[patchMemoSlot(kind, base.get(), baseInfo.get(), key)];
    if (entry.kind != kind || entry.base != base || entry.baseInfo != baseInfo || !std::equal(entry.key.begin(), entry.key.end(), key.begin(), key.end())) return std::nullopt;
    patchMemoCounts[kind][1].fetch_add(1, std::memory_order_relaxed);
    return PatchMemoHit{entry.result, entry.info};
}

void Driver::storePatchMemo(std::size_t kind, const std::shared_ptr<const ShaderRecompiler::RecompileResult>& base, const std::shared_ptr<const ShaderRecompiler::ShaderVertexStageInfo>& baseInfo, std::span<const std::uint32_t> key, PatchMemoHit hit) {
    if (!patchMemoEnabled()) return;
    auto& entry = patchMemo()[patchMemoSlot(kind, base.get(), baseInfo.get(), key)];
    entry.base = base;
    entry.baseInfo = baseInfo;
    entry.kind = kind;
    entry.key.assign(key.begin(), key.end());
    entry.result = std::move(hit.result);
    entry.info = std::move(hit.info);
}

std::string TakePatchMemoText() {
    const auto take = [](std::atomic<std::uint64_t>& value) { return std::to_string(value.exchange(0, std::memory_order_relaxed)); };
    return "; patched shader results (lookups/hits): rebased " + take(patchMemoCounts[0][0]) + "/" + take(patchMemoCounts[0][1]) + ", data words " + take(patchMemoCounts[1][0]) + "/" + take(patchMemoCounts[1][1]);
}

void Driver::applyShaderDataPatches(DispatchVariant& variant) {
    if (variant.shaderDataPatches.empty() && variant.rebasedWords.empty() && variant.descriptorPatches.empty()) return;
    auto& key = patchKey;
    key.clear();
    key.push_back(static_cast<std::uint32_t>(variant.shaderDataPatches.size()));
    for (const auto& [slot, value] : variant.shaderDataPatches) {
        key.push_back(slot);
        key.push_back(value);
    }
    key.push_back(static_cast<std::uint32_t>(variant.descriptorPatches.size()));
    for (const auto& [binding, word, value] : variant.descriptorPatches) {
        key.push_back(binding);
        key.push_back(word);
        key.push_back(value);
    }
    for (const auto& patch : variant.rebasedWords) {
        key.push_back(static_cast<std::uint32_t>(patch.kind));
        key.push_back(patch.target);
        key.push_back(patch.word);
        key.push_back(patch.value);
    }
    if (auto hit = findPatchMemo(0, variant.compiled, variant.vertexInfo, key)) {
        if (hit->info != nullptr) variant.vertexInfo = std::move(hit->info);
        variant.compiled = std::move(hit->result);
        variant.shaderDataPatches.clear();
        variant.rebasedWords.clear();
        variant.descriptorPatches.clear();
        return;
    }
    const auto base = variant.compiled;
    const auto baseInfo = variant.vertexInfo;
    auto rebased = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    for (const auto& [slot, value] : variant.shaderDataPatches) {
        const auto at = static_cast<std::size_t>(slot) * sizeof(std::uint32_t);
        if (at + sizeof(std::uint32_t) <= rebased->pushConstants.size()) std::memcpy(rebased->pushConstants.data() + at, &value, sizeof(value));
        for (std::size_t index = 0; index < rebased->bindings.size(); ++index) {
            const auto& binding = std::as_const(rebased->bindings)[index];
            if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && slot < binding.guestDescriptor.size()) rebased->bindings.Patch(index).guestDescriptor[slot] = value;
        }
    }
    for (const auto& [binding, word, value] : variant.descriptorPatches) {
        if (binding < rebased->bindings.size() && word < std::as_const(rebased->bindings)[binding].guestDescriptor.size()) rebased->bindings.Patch(binding).guestDescriptor[word] = value;
    }
    std::shared_ptr<ShaderRecompiler::ShaderVertexStageInfo> info;
    for (const auto& patch : variant.rebasedWords) {
        if (patch.kind == VariantRebase::Kind::BufferWord) {
            if (patch.target < rebased->bindings.size() && patch.word < std::as_const(rebased->bindings)[patch.target].guestDescriptor.size()) rebased->bindings.Patch(patch.target).guestDescriptor[patch.word] = patch.value;
            continue;
        }
        if (patch.target >= rebased->vertexAttributes.size() || patch.word >= 4u) continue;
        auto& attribute = rebased->vertexAttributes.Mutable()[patch.target];
        attribute.resource.fields[patch.word] = patch.value;
        if (variant.vertexInfo == nullptr || attribute.location >= variant.vertexInfo->resources.size()) continue;
        if (info == nullptr) info = std::make_shared<ShaderRecompiler::ShaderVertexStageInfo>(*variant.vertexInfo);
        info->resources[attribute.location].fields[patch.word] = patch.value;
    }
    storePatchMemo(0, base, baseInfo, key, {rebased, info});
    if (info != nullptr) variant.vertexInfo = std::move(info);
    variant.compiled = std::move(rebased);
    variant.shaderDataPatches.clear();
    variant.rebasedWords.clear();
    variant.descriptorPatches.clear();
}

void Driver::reportDrawRelocation() {
    auto& counters = drawRelocationCounters;
    AgcDriver::ProfilePrint_nid_no_patch("[draw-relocate] %llu alias lookups (10 s): %llu donors with moved user words, %llu refusals (%llu of them at a V# base read again), %llu hits through a relocated entry (%llu with rebased buffers or vertex inputs); %zu shader sets learned, %zu aliases held (%llu dropped at the limit)\n", static_cast<unsigned long long>(counters.lookups), static_cast<unsigned long long>(counters.donors), static_cast<unsigned long long>(counters.refused + counters.unreadBases), static_cast<unsigned long long>(counters.unreadBases), static_cast<unsigned long long>(counters.hits), static_cast<unsigned long long>(counters.rebasedHits), drawRelocations.size(), drawAliases.size(), static_cast<unsigned long long>(counters.evicted));
    counters = {};
}

}
