#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_PENDINGWRITEINDEX_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_PENDINGWRITEINDEX_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

class CountedRanges {
public:
    using Range = std::pair<std::uint64_t, std::uint64_t>;

    void Add(std::span<const Range> writes) {
        for (const auto& range : writes) {
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.first == range; });
            if (found != entries.end()) ++found->second;
            else entries.emplace_back(range, 1u);
        }
    }

    void Remove(std::span<const Range> writes) {
        for (const auto& range : writes) {
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) { return entry.first == range; });
            if (found == entries.end() || --found->second != 0) continue;
            *found = entries.back();
            entries.pop_back();
        }
    }

    [[nodiscard]] bool Overlaps(std::uint64_t begin, std::uint64_t end) const {
        return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) { return begin < entry.first.second && entry.first.first < end; });
    }

    [[nodiscard]] bool OverlapsAny(std::span<const Range> ranges) const {
        return std::any_of(ranges.begin(), ranges.end(), [&](const Range& range) { return Overlaps(range.first, range.second); });
    }

    [[nodiscard]] std::size_t Size() const { return entries.size(); }

private:
    std::vector<std::pair<Range, std::uint32_t>> entries;
};

class NewestSequenceMap {
public:
    void Set(std::uint64_t key, std::uint64_t sequence) {
        auto at = home(key);
        while (slots[at].sequence != 0 && slots[at].key != key) at = (at + 1) & Mask;
        slots[at] = {key, sequence};
    }

    void Erase(std::uint64_t key, std::uint64_t sequence) {
        auto at = home(key);
        while (slots[at].sequence != 0 && slots[at].key != key) at = (at + 1) & Mask;
        if (slots[at].sequence != sequence) return;
        auto hole = at;
        for (auto next = (at + 1) & Mask; slots[next].sequence != 0; next = (next + 1) & Mask) {
            const auto wanted = home(slots[next].key);
            if (((next - wanted) & Mask) < ((next - hole) & Mask)) continue;
            slots[hole] = slots[next];
            hole = next;
        }
        slots[hole] = {};
    }

    [[nodiscard]] std::uint64_t Find(std::uint64_t key) const {
        auto at = home(key);
        while (slots[at].sequence != 0) {
            if (slots[at].key == key) return slots[at].sequence;
            at = (at + 1) & Mask;
        }
        return 0;
    }

private:
    static constexpr std::size_t Capacity = 1024;
    static constexpr std::size_t Mask = Capacity - 1;

    struct Slot {
        std::uint64_t key = 0;
        std::uint64_t sequence = 0;
    };

    static std::size_t home(std::uint64_t key) {
        return static_cast<std::size_t>((key * 0x9e3779b97f4a7c15ull) >> 54u) & Mask;
    }

    std::array<Slot, Capacity> slots{};
};

class RetiredWrites {
public:
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    enum class Answer : std::uint8_t { Clear, Overlap, Overflow };

    void Note(std::uint64_t sequence, std::span<const Range> writes) {
        for (const auto& range : writes) {
            auto& entry = entries[next % entries.size()];
            lost = std::max(lost, entry.sequence);
            entry = {range, sequence};
            ++next;
        }
    }

    [[nodiscard]] Answer Since(std::uint64_t sequence, std::span<const Range> reads) const {
        if (lost > sequence) return Answer::Overflow;
        for (auto at = next; at != 0 && next - at < entries.size(); --at) {
            const auto& entry = entries[(at - 1) % entries.size()];
            if (entry.sequence <= sequence) break;
            for (const auto& [begin, end] : reads) {
                if (begin < entry.range.second && entry.range.first < end) return Answer::Overlap;
            }
        }
        return Answer::Clear;
    }

private:
    struct Entry {
        Range range{};
        std::uint64_t sequence = 0;
    };

    std::array<Entry, 1024> entries{};
    std::uint64_t next = 0;
    std::uint64_t lost = 0;
};

}

#endif
