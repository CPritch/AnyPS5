#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>

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
bool RebaseMovedFields(DispatchVariant& copy, const DispatchVariant& variant, std::uint64_t moved, std::span<const std::pair<std::uint32_t, std::uint32_t>> movedValues) {
    std::size_t offset = 0;
    auto next = variant.movableWords.begin();
    for (std::size_t run = 0; run < copy.runs.size() && next != variant.movableWords.end(); ++run) {
        const auto count = static_cast<std::size_t>((copy.runs[run].second - copy.runs[run].first) / sizeof(std::uint32_t));
        const auto base = run < copy.runBases.size() ? copy.runBases[run] : std::int8_t {-1};
        const bool shifted = base >= 0 && (moved >> base & 1u) != 0;
        for (; next != variant.movableWords.end() && *next < offset + count; ++next) {
            if (*next < offset || !shifted) continue;
            std::uint32_t live = 0;
            const auto address = copy.runs[run].first + (*next - offset) * sizeof(std::uint32_t);
            if (GuestMemory::CopyMapped(address, std::as_writable_bytes(std::span(&live, 1))) != GuestMemory::Compare::Equal) return false;
            const auto kept = variant.words[*next];
            if (live == 0 || kept == 0 || ((live ^ kept) & 3u) != 0) return false;
            copy.words[*next] = live;
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

bool Driver::poolRelocationEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_POOL_RELOCATION");
        return drawRelocationEnabled() && value != nullptr && std::strcmp(value, "1") == 0;
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
    if ((use.asValue >> table & 1u) != 0) return true;
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

std::uint64_t Driver::relocatedContentKey(const RelocatableUserWord& word, std::uint32_t value, bool& readable) {
    std::uint64_t hash = 0x9e3779b97f4a7c15ull;
    const auto mix = [&](std::uint64_t mixed) {
        hash ^= mixed;
        hash *= 0x100000001b3ull;
    };
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
            const auto address = AlignedWord(value) + static_cast<std::uint64_t>(offset) + done * sizeof(std::uint32_t);
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

void Driver::tagVariantRuns(DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> regions, const ShaderRecompiler::SrtUserDataUse& use, bool eligible, std::uint32_t hiddenWords, std::span<const std::shared_ptr<const ShaderRecompiler::SrtGuestWords>> supplied, const PoolRelocation* pool) {
    std::uint64_t relocatable = eligible && drawRelocationEnabled() ? use.asBase & ~use.asValue : 0;
    if (pool != nullptr && eligible && drawRelocationEnabled()) relocatable |= pool->words;
    if (hiddenWords != 0) relocatable &= hiddenWords >= 64 ? 0 : ~((std::uint64_t {1} << hiddenWords) - 1);
    std::vector<std::pair<std::uint64_t, std::int32_t>> reads;
    const auto covered = [&](std::uint64_t address) {
        return std::any_of(regions.begin(), regions.end(), [&](const ShaderRecompiler::MemoryRegion& region) { return address >= region.guestAddress && address - region.guestAddress + sizeof(std::uint32_t) <= region.bytes.size(); });
    };
    if (relocatable != 0) {
        reads = use.reads;
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

void Driver::learnPendingRelocation() {
    const auto pending = drawRelocationPending;
    drawRelocationPending = {};
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
            if ((variant.relocatableWords >> index & 1u) == 0) {
                if (held == shape->words.end()) continue;
                shape->words.erase(held);
                shape->banned.push_back(reg);
                changed = true;
                continue;
            }
            if (held != shape->words.end() || std::find(shape->banned.begin(), shape->banned.end(), reg) != shape->banned.end()) continue;
            RelocatableUserWord word{reg, {}};
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

void Driver::relocateDrawEntry(const QueueState& queue, std::uint64_t alias, std::shared_ptr<DrawEntry>& entry, std::shared_ptr<const DrawDecode>& decode, std::vector<DrawUserPatch>& patches) {
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
    struct Move {
        std::size_t program;
        std::uint32_t index, from, to;
    };
    std::vector<Move> moves;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        for (std::uint32_t index = program.hiddenUserWords; index < program.userData.size(); ++index) {
            const auto current = readUserData(queue.shader, program.userDataBase + index - program.hiddenUserWords);
            if (current != program.userData[index]) moves.push_back({i, index, program.userData[index], current});
        }
    }
    if (moves.empty()) return;
    ++counters.donors;
    auto relocated = std::make_shared<DrawEntry>();
    relocated->decode = donor->decode;
    relocated->stages.resize(donor->stages.size());
    for (std::size_t i = 0; i < donor->stages.size(); ++i) {
        const bool moved = std::any_of(moves.begin(), moves.end(), [&](const Move& move) { return move.program == i; });
        if (!moved) {
            relocated->stages[i] = donor->stages[i];
            continue;
        }
        std::uint64_t movedWords = 0;
        std::vector<std::pair<std::uint32_t, std::uint32_t>> movedValues;
        for (const auto& move : moves) {
            if (move.program != i || move.index >= 64) continue;
            movedWords |= std::uint64_t {1} << move.index;
            movedValues.emplace_back(move.index, move.to);
        }
        for (const auto& variant : donor->stages[i]) {
            if (variant == nullptr || variant->compiled == nullptr) continue;
            const bool allowed = std::all_of(moves.begin(), moves.end(), [&](const Move& move) { return move.program != i || (move.index < 64 && (variant->relocatableWords >> move.index & 1u) != 0 && move.from != 0 && move.to != 0 && ((move.from ^ move.to) & 3u) == 0); });
            if (!allowed) {
                ++counters.refused;
                continue;
            }
            auto copy = std::make_shared<DispatchVariant>();
            copy->runs = variant->runs;
            copy->runBases = variant->runBases;
            copy->relocatableWords = variant->relocatableWords;
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
            }
            if ((!variant->movableWords.empty() || !variant->rebases.empty()) && !RebaseMovedFields(*copy, *variant, movedWords, movedValues)) {
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

void Driver::applyShaderDataPatches(DispatchVariant& variant) {
    if (variant.shaderDataPatches.empty() && variant.rebasedWords.empty()) return;
    auto rebased = std::make_shared<ShaderRecompiler::RecompileResult>(*variant.compiled);
    for (const auto& [slot, value] : variant.shaderDataPatches) {
        const auto at = static_cast<std::size_t>(slot) * sizeof(std::uint32_t);
        if (at + sizeof(std::uint32_t) <= rebased->pushConstants.size()) std::memcpy(rebased->pushConstants.data() + at, &value, sizeof(value));
        for (auto& binding : rebased->bindings) {
            if (binding.role == ShaderRecompiler::DescriptorRole::ShaderData && slot < binding.guestDescriptor.size()) binding.guestDescriptor[slot] = value;
        }
    }
    std::shared_ptr<ShaderRecompiler::ShaderVertexStageInfo> info;
    for (const auto& patch : variant.rebasedWords) {
        if (patch.kind == VariantRebase::Kind::BufferWord) {
            if (patch.target < rebased->bindings.size() && patch.word < rebased->bindings[patch.target].guestDescriptor.size()) rebased->bindings[patch.target].guestDescriptor[patch.word] = patch.value;
            continue;
        }
        if (patch.target >= rebased->vertexAttributes.size() || patch.word >= 4u) continue;
        auto& attribute = rebased->vertexAttributes[patch.target];
        attribute.resource.fields[patch.word] = patch.value;
        if (variant.vertexInfo == nullptr || attribute.location >= variant.vertexInfo->resources.size()) continue;
        if (info == nullptr) info = std::make_shared<ShaderRecompiler::ShaderVertexStageInfo>(*variant.vertexInfo);
        info->resources[attribute.location].fields[patch.word] = patch.value;
    }
    if (info != nullptr) variant.vertexInfo = std::move(info);
    variant.compiled = std::move(rebased);
    variant.shaderDataPatches.clear();
    variant.rebasedWords.clear();
}

void Driver::reportDrawRelocation() {
    auto& counters = drawRelocationCounters;
    AgcDriver::ProfilePrint_nid_no_patch("[draw-relocate] %llu alias lookups (10 s): %llu donors with moved user words, %llu refusals (%llu of them at a V# base read again), %llu hits through a relocated entry (%llu with rebased buffers or vertex inputs); %zu shader sets learned, %zu aliases held (%llu dropped at the limit)\n", static_cast<unsigned long long>(counters.lookups), static_cast<unsigned long long>(counters.donors), static_cast<unsigned long long>(counters.refused + counters.unreadBases), static_cast<unsigned long long>(counters.unreadBases), static_cast<unsigned long long>(counters.hits), static_cast<unsigned long long>(counters.rebasedHits), drawRelocations.size(), drawAliases.size(), static_cast<unsigned long long>(counters.evicted));
    counters = {};
}

}
