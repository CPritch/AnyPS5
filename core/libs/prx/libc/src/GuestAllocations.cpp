#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <limits>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#else
#include <link.h>
#endif
#include <sys/mman.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include "prx/libc/include/specifics/linux/ElfTypes.hpp"
#endif

namespace GuestAllocations {
namespace {

struct LeaseIndexEntry {
    std::uint64_t address;
    std::uint64_t end;
    const std::pair<const std::uint64_t, std::shared_ptr<const Range>>* node;
    bool readable;
};

constexpr std::size_t LeaseIndexDirtyLimit = 32;

struct LeaseIndex {
    bool valid = false;
    std::vector<LeaseIndexEntry> entries;
    std::array<std::pair<std::uint64_t, std::uint64_t>, LeaseIndexDirtyLimit> dirty{};
    std::size_t dirtyCount = 0;
};

struct Registry {
    std::mutex mutex;
    std::map<std::uint64_t, std::shared_ptr<const Range>> ranges;
    LeaseIndex leaseIndex;
    bool wholeChangePending = false;
    bool mainImageRegistered = false;
    std::map<std::uintptr_t, std::uintptr_t> registeredImages;
    std::shared_ptr<const Lease::Ranges> readableLease;
    std::shared_ptr<const Lease::Ranges> allLease;
    std::uint64_t backingSerials = 0;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> retiring;
    std::size_t unlockedMutations = 0;
};

Registry& registry() {
    static auto* value = new Registry;
    return *value;
}

std::uint64_t freshBacking() {
    return ++registry().backingSerials;
}

bool leasePatchEnabled() {
    static const bool enabled = std::getenv("APS5_NO_LEASE_PATCH") == nullptr;
    return enabled;
}

bool verifyLeasePatch() {
    static const bool enabled = std::getenv("APS5_VERIFY_LEASE_PATCH") != nullptr;
    return enabled;
}

void noteRangeDirty(std::uint64_t address, std::uint64_t bytes) {
    auto& index = registry().leaseIndex;
    if (!index.valid) return;
    if (index.dirtyCount == LeaseIndexDirtyLimit) {
        index.valid = false;
        index.dirtyCount = 0;
        return;
    }
    index.dirty[index.dirtyCount++] = {address, address + std::max<std::uint64_t>(bytes, 1)};
}

void indexRangesFrom(std::vector<LeaseIndexEntry>& entries, std::uint64_t begin, std::uint64_t end) {
    const auto& ranges = registry().ranges;
    auto it = ranges.upper_bound(begin);
    if (it != ranges.begin()) --it;
    for (; it != ranges.end() && it->first < end; ++it) {
        const auto& range = *it->second;
        if (range.bytes == 0 || range.address + range.bytes <= begin) continue;
        entries.push_back({range.address, range.address + range.bytes, &*it, range.readable});
    }
}

const std::vector<LeaseIndexEntry>& currentLeaseIndex() {
    auto& index = registry().leaseIndex;
    if (!leasePatchEnabled() || !index.valid) {
        index.entries.clear();
        index.entries.reserve(registry().ranges.size());
        indexRangesFrom(index.entries, 0, std::numeric_limits<std::uint64_t>::max());
        index.valid = leasePatchEnabled();
        index.dirtyCount = 0;
        return index.entries;
    }
    thread_local std::vector<LeaseIndexEntry> replacement;
    for (std::size_t slot = 0; slot < index.dirtyCount; ++slot) {
        const auto [begin, end] = index.dirty[slot];
        auto first = std::lower_bound(index.entries.begin(), index.entries.end(), begin, [](const LeaseIndexEntry& entry, std::uint64_t value) { return entry.end <= value; });
        auto last = first;
        auto replacedBegin = begin;
        auto replacedEnd = end;
        while (last != index.entries.end() && last->address < end) {
            replacedBegin = std::min(replacedBegin, last->address);
            replacedEnd = std::max(replacedEnd, last->end);
            ++last;
        }
        replacement.clear();
        indexRangesFrom(replacement, replacedBegin, replacedEnd);
        const auto at = index.entries.erase(first, last);
        index.entries.insert(at, replacement.begin(), replacement.end());
    }
    index.dirtyCount = 0;
    if (verifyLeasePatch()) {
        std::vector<LeaseIndexEntry> walked;
        indexRangesFrom(walked, 0, std::numeric_limits<std::uint64_t>::max());
        const bool same = walked.size() == index.entries.size() && std::equal(walked.begin(), walked.end(), index.entries.begin(), [](const LeaseIndexEntry& left, const LeaseIndexEntry& right) { return left.address == right.address && left.end == right.end && left.node == right.node && left.readable == right.readable; });
        if (!same) {
            std::fprintf(stderr, "FATAL: APS5_VERIFY_LEASE_PATCH: the patched lease index (%zu ranges) differs from the registry (%zu ranges)\n", index.entries.size(), walked.size());
            std::abort();
        }
    }
    return index.entries;
}

void renewBacking(std::uintptr_t address, std::size_t bytes) {
    auto& ranges = registry().ranges;
    auto it = ranges.upper_bound(address);
    if (it != ranges.begin()) --it;
    for (; it != ranges.end() && it->first < address + bytes; ++it) {
        if (it->first + it->second->bytes <= address) continue;
        auto renewed = *it->second;
        renewed.backingSerial = freshBacking();
        it->second = std::make_shared<const Range>(renewed);
    }
}

bool isRetiring(const Range& range) {
    for (const auto& [begin, end] : registry().retiring) {
        if (range.address < end && begin < range.address + range.bytes) return true;
    }
    return false;
}

std::atomic<std::uint64_t> generation{1};

constexpr std::uint64_t FineBucketBytes = 1ull << 16;
constexpr std::size_t FineBuckets = 1u << 18;
constexpr std::uint64_t CoarseBucketBytes = 1ull << 24;
constexpr std::size_t CoarseBuckets = 1u << 16;
constexpr std::uint64_t FineSpanLimit = 256;
constexpr std::uint64_t CoarseSpanLimit = 4096;
constexpr std::size_t JournalCapacity = 8192;
constexpr std::size_t VerifySnapshots = 256;

std::atomic<std::uint64_t> fineChanged[FineBuckets]{};
std::atomic<std::uint64_t> coarseChanged[CoarseBuckets]{};
std::atomic<std::uint64_t> wideChanged[CoarseBuckets]{};
std::atomic<std::uint64_t> wholeChanged{0};

struct JournalEntry {
    std::uint64_t generation = 0;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

using SnapshotRanges = std::vector<Range>;

struct ChangeJournal {
    std::mutex mutex;
    std::vector<JournalEntry> entries;
    std::size_t next = 0;
    std::uint64_t oldestComplete = 1;
    std::map<std::uint64_t, std::shared_ptr<const SnapshotRanges>> snapshots;
    std::shared_ptr<const SnapshotRanges> latest;
};

ChangeJournal& changeJournal() {
    static auto* value = [] {
        auto* journal = new ChangeJournal;
        journal->entries.resize(JournalCapacity);
        return journal;
    }();
    return *value;
}

bool journalEnabled() {
    static const bool enabled = std::getenv("APS5_NO_ALLOCATION_JOURNAL") == nullptr;
    return enabled;
}

bool verifyJournal() {
    static const bool enabled = std::getenv("APS5_VERIFY_ALLOCATION_JOURNAL") != nullptr;
    return enabled;
}

bool sameRangeFields(const Range& left, const Range& right) {
    return left.address == right.address && left.bytes == right.bytes && left.readable == right.readable && left.writable == right.writable && left.allocationAddress == right.allocationAddress && left.allocationBytes == right.allocationBytes && left.releasable == right.releasable && left.gpu == right.gpu && left.backingSerial == right.backingSerial;
}

std::vector<JournalEntry> differingEntries(const SnapshotRanges& before, const SnapshotRanges& after) {
    std::vector<JournalEntry> differing;
    const auto add = [&](const Range& range) { differing.push_back({0, range.address, range.address + std::max<std::uint64_t>(range.bytes, 1)}); };
    std::size_t first = 0;
    std::size_t second = 0;
    while (first < before.size() || second < after.size()) {
        if (second == after.size() || (first < before.size() && before[first].address < after[second].address)) {
            add(before[first++]);
        } else if (first == before.size() || after[second].address < before[first].address) {
            add(after[second++]);
        } else {
            if (!sameRangeFields(before[first], after[second])) {
                add(before[first]);
                add(after[second]);
            }
            ++first;
            ++second;
        }
    }
    return differing;
}

[[noreturn]] void journalDisagreement(const char* what, std::uint64_t begin, std::uint64_t end, std::uint64_t since, std::uint64_t now) {
    std::fprintf(stderr, "FATAL: APS5_VERIFY_ALLOCATION_JOURNAL: %s: 0x%llx-0x%llx between generations %llu and %llu\n", what, static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end), static_cast<unsigned long long>(since), static_cast<unsigned long long>(now));
    std::abort();
}

void markBuckets(std::atomic<std::uint64_t>* table, std::size_t count, std::uint64_t granule, std::uint64_t begin, std::uint64_t end, std::uint64_t stamp) {
    for (auto bucket = begin / granule; bucket <= (end - 1) / granule; ++bucket) table[bucket % count].store(stamp, std::memory_order_release);
}

bool anyBucketNewer(const std::atomic<std::uint64_t>* table, std::size_t count, std::uint64_t granule, std::uint64_t begin, std::uint64_t end, std::uint64_t stamp) {
    for (auto bucket = begin / granule; bucket <= (end - 1) / granule; ++bucket) {
        if (table[bucket % count].load(std::memory_order_acquire) > stamp) return true;
    }
    return false;
}

void markChanged(std::uint64_t stamp, std::uint64_t begin, std::uint64_t end) {
    const auto coarse = (end - 1) / CoarseBucketBytes - begin / CoarseBucketBytes + 1;
    if (coarse > CoarseSpanLimit) {
        wholeChanged.store(stamp, std::memory_order_release);
        return;
    }
    markBuckets(coarseChanged, CoarseBuckets, CoarseBucketBytes, begin, end, stamp);
    if ((end - 1) / FineBucketBytes - begin / FineBucketBytes + 1 > FineSpanLimit) markBuckets(wideChanged, CoarseBuckets, CoarseBucketBytes, begin, end, stamp);
    else markBuckets(fineChanged, FineBuckets, FineBucketBytes, begin, end, stamp);
}

std::uint64_t publishChanges(std::span<const JournalEntry> changes, bool whole, std::shared_ptr<const SnapshotRanges> snapshot) {
    auto& journal = changeJournal();
    std::lock_guard lock(journal.mutex);
    const auto stamp = generation.load(std::memory_order_relaxed) + 1;
    if (journalEnabled()) {
        if (whole) {
            wholeChanged.store(stamp, std::memory_order_release);
            journal.oldestComplete = stamp;
        }
        for (const auto& change : changes) {
            if (change.end <= change.begin) continue;
            markChanged(stamp, change.begin, change.end);
            auto& slot = journal.entries[journal.next];
            if (slot.generation != 0) journal.oldestComplete = std::max(journal.oldestComplete, slot.generation);
            slot = {stamp, change.begin, change.end};
            journal.next = (journal.next + 1) % JournalCapacity;
        }
        if (verifyJournal()) {
            if (snapshot != nullptr) journal.latest = std::move(snapshot);
            if (journal.latest != nullptr) {
                journal.snapshots[stamp] = journal.latest;
                if (journal.snapshots.size() > VerifySnapshots) journal.snapshots.erase(journal.snapshots.begin());
            }
        }
    }
    generation.store(stamp, std::memory_order_release);
    return stamp;
}

void verifyUnchangedAnswer(std::uint64_t since, std::uint64_t now, std::uint64_t begin, std::uint64_t end) {
    auto& journal = changeJournal();
    std::lock_guard lock(journal.mutex);
    const auto before = journal.snapshots.find(since);
    const auto after = journal.snapshots.find(now);
    if (before == journal.snapshots.end() || after == journal.snapshots.end()) return;
    for (const auto& differing : differingEntries(*before->second, *after->second)) {
        if (differing.begin < end && begin < differing.end) journalDisagreement("the journal answers unchanged where the registry snapshots differ", begin, end, since, now);
    }
}
std::atomic<void (*)(std::uintptr_t, std::size_t)> invalidator{nullptr};
std::atomic<bool (*)(std::uintptr_t, std::size_t)> pinWaiter{nullptr};
std::atomic<void (*)(const Mapped&, std::uint64_t)> gpuMapObserver{nullptr};
std::atomic<void (*)(std::uintptr_t, std::size_t)> unmapObserver{nullptr};
std::atomic<std::uint32_t> gpuMapObserverCalls{0};

std::chrono::milliseconds pinWait() {
    static const std::chrono::milliseconds value{[] {
        const char* text = std::getenv("APS5_PIN_WAIT_MS");
        return text != nullptr ? std::strtoull(text, nullptr, 10) : 60000ull;
    }()};
    return value;
}

void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

// A mutation holds the registry lock and remembers the ranges it changed, which are reported to the
// invalidator once the generation has moved on, so a cache that observed the old state meanwhile
// notices either way.
struct MutationState {
    std::unique_lock<std::mutex> lock;
    std::vector<std::pair<std::uintptr_t, std::size_t>> changed;
    std::vector<JournalEntry> journaled;
    std::vector<std::pair<std::uintptr_t, std::size_t>> gpuMapped;
    std::vector<std::pair<std::uintptr_t, std::size_t>> unpinned;
};

void recordChange(void* mutation, const void* pointer, std::size_t bytes) {
    if (mutation == nullptr) {
        registry().wholeChangePending = true;
        return;
    }
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    auto* state = static_cast<MutationState*>(mutation);
    state->journaled.push_back({0, address, address + std::max<std::uint64_t>(bytes, 1)});
    if (bytes != 0) state->changed.emplace_back(address, bytes);
}

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
std::vector<std::pair<std::uintptr_t, std::size_t>> fileBackedWritableImage() {
    const auto image = std::filesystem::read_symlink("/proc/self/exe").string();
    std::ifstream maps("/proc/self/maps");
    std::vector<std::pair<std::uintptr_t, std::size_t>> mappings;
    for (std::string line; std::getline(maps, line);) {
        std::istringstream fields(line);
        std::string span, permissions, offset, device, inode, path;
        fields >> span >> permissions >> offset >> device >> inode >> std::ws;
        std::getline(fields, path);
        if (permissions != "rw-p" || path != image) continue;
        const auto dash = span.find('-');
        const auto begin = std::stoull(span.substr(0, dash), nullptr, 16);
        mappings.emplace_back(begin, std::stoull(span.substr(dash + 1), nullptr, 16) - begin);
    }
    return mappings;
}

#ifdef ANYPS5_ENABLE_ASAN
[[gnu::no_sanitize("address")]] void copyImagePages(void* target, const void* source, std::size_t bytes) {
    auto* to = static_cast<volatile std::uintptr_t*>(target);
    const auto* from = static_cast<const volatile std::uintptr_t*>(source);
    for (std::size_t word = 0; word < bytes / sizeof(std::uintptr_t); ++word) to[word] = from[word];
}
#endif

bool backWritableImageAnonymously() {
    for (const auto& [address, bytes] : fileBackedWritableImage()) {
        auto* image = reinterpret_cast<void*>(address);
        auto* copy = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (copy == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "mmap of an anonymous main image copy failed");
#ifdef ANYPS5_ENABLE_ASAN
        copyImagePages(copy, image, bytes);
#else
        std::memcpy(copy, image, bytes);
#endif
        if (mremap(copy, bytes, bytes, MREMAP_MAYMOVE | MREMAP_FIXED, image) == MAP_FAILED) throw std::system_error(errno, std::generic_category(), "mremap of an anonymous main image copy failed");
    }
    return true;
}

[[maybe_unused]] const bool writableImageAnonymous = backWritableImageAnonymously();
#endif

}

void* GuestAllocationsBegin_nid_postfix() {
    changeJournal();
    auto* state = new MutationState{std::unique_lock<std::mutex>(registry().mutex), {}};
    if (!leasePatchEnabled()) {
        registry().readableLease.reset();
        registry().allLease.reset();
    }
    return state;
}

void GuestAllocationsEnd_nid_postfix(void* mutation) noexcept {
    auto* state = static_cast<MutationState*>(mutation);
    for (const auto& [address, bytes] : state->unpinned) renewBacking(address, bytes);
    const bool whole = std::exchange(registry().wholeChangePending, false);
    std::shared_ptr<const SnapshotRanges> snapshot;
    if (journalEnabled() && verifyJournal()) {
        auto ranges = std::make_shared<SnapshotRanges>();
        ranges->reserve(registry().ranges.size());
        for (const auto& [address, range] : registry().ranges) ranges->push_back(*range);
        auto& journal = changeJournal();
        std::shared_ptr<const SnapshotRanges> previous;
        {
            std::lock_guard lock(journal.mutex);
            previous = journal.latest;
        }
        if (previous != nullptr && !whole) {
            for (const auto& differing : differingEntries(*previous, *ranges)) {
                const bool covered = std::any_of(state->journaled.begin(), state->journaled.end(), [&](const JournalEntry& entry) { return differing.begin < entry.end && entry.begin < differing.end; });
                if (!covered) journalDisagreement("a mutation changed a range it did not journal", differing.begin, differing.end, 0, generation.load());
            }
        }
        snapshot = std::move(ranges);
    }
    const bool unchanged = journalEnabled() && !whole && state->journaled.empty();
    const auto ended = publishChanges(state->journaled, whole, std::move(snapshot));
    std::shared_ptr<const Lease::Ranges> retiredReadable;
    std::shared_ptr<const Lease::Ranges> retiredAll;
    if (!unchanged) {
        retiredReadable = std::move(registry().readableLease);
        retiredAll = std::move(registry().allLease);
    }
    if (const auto callback = invalidator.load(std::memory_order_acquire)) {
        for (const auto& [address, bytes] : state->changed) callback(address, bytes);
    }
    if (!state->gpuMapped.empty()) gpuMapObserverCalls.fetch_add(1);
    const auto observer = state->gpuMapped.empty() ? nullptr : gpuMapObserver.load();
    Lease::Ranges mapped;
    if (observer != nullptr) {
        const auto& ranges = registry().ranges;
        for (const auto& [address, bytes] : state->gpuMapped) {
            auto it = ranges.upper_bound(address);
            if (it != ranges.begin()) --it;
            for (; it != ranges.end() && it->first < address + bytes; ++it) {
                const auto& range = it->second;
                if (range->gpu && range->readable && range->bytes != 0 && range->address + range->bytes > address) mapped.push_back(range);
            }
        }
        std::sort(mapped.begin(), mapped.end(), [](const auto& left, const auto& right) { return left->address < right->address; });
        mapped.erase(std::unique(mapped.begin(), mapped.end()), mapped.end());
    }
    const Mapped weak(mapped.begin(), mapped.end());
    mapped.clear();
    const bool noted = !state->gpuMapped.empty();
    delete state;
    retiredReadable.reset();
    retiredAll.reset();
    if (!weak.empty()) observer(weak, ended);
    if (noted) gpuMapObserverCalls.fetch_sub(1);
}

std::uint64_t GuestAllocationsGeneration_nid_postfix() {
    return generation.load(std::memory_order_acquire);
}

bool GuestAllocationsChangedSince_nid_postfix(std::uint64_t since, std::uintptr_t address, std::size_t bytes) {
    const auto now = generation.load(std::memory_order_acquire);
    if (since == now) return false;
    if (!journalEnabled() || since == 0 || since > now) return true;
    const std::uint64_t begin = address;
    const auto span = std::max<std::uint64_t>(bytes, 1);
    if (span > std::numeric_limits<std::uint64_t>::max() - begin) return true;
    const auto end = begin + span;
    if (wholeChanged.load(std::memory_order_acquire) > since) return true;
    bool changed;
    if ((end - 1) / FineBucketBytes - begin / FineBucketBytes + 1 <= FineSpanLimit) {
        changed = anyBucketNewer(fineChanged, FineBuckets, FineBucketBytes, begin, end, since) || anyBucketNewer(wideChanged, CoarseBuckets, CoarseBucketBytes, begin, end, since);
    } else if ((end - 1) / CoarseBucketBytes - begin / CoarseBucketBytes + 1 <= CoarseSpanLimit) {
        changed = anyBucketNewer(coarseChanged, CoarseBuckets, CoarseBucketBytes, begin, end, since);
    } else {
        changed = true;
    }
    if (!changed && verifyJournal()) verifyUnchangedAnswer(since, now, begin, end);
    return changed;
}

bool GuestAllocationsChangesSince_nid_no_patch(std::uint64_t since, std::vector<std::pair<std::uint64_t, std::uint64_t>>* spans, std::uint64_t* through) {
    spans->clear();
    if (!journalEnabled() || since == 0) return false;
    auto& journal = changeJournal();
    std::lock_guard lock(journal.mutex);
    *through = generation.load(std::memory_order_relaxed);
    if (since == *through) return true;
    if (since > *through || since < journal.oldestComplete || wholeChanged.load(std::memory_order_relaxed) > since) return false;
    for (std::size_t walked = 1; walked <= JournalCapacity; ++walked) {
        const auto& entry = journal.entries[(journal.next + JournalCapacity - walked) % JournalCapacity];
        if (entry.generation <= since) break;
        spans->emplace_back(entry.begin, entry.end);
    }
    std::sort(spans->begin(), spans->end());
    std::size_t kept = 0;
    for (const auto& span : *spans) {
        if (kept != 0 && span.first <= (*spans)[kept - 1].second) (*spans)[kept - 1].second = std::max((*spans)[kept - 1].second, span.second);
        else (*spans)[kept++] = span;
    }
    spans->resize(kept);
    return true;
}

bool GuestAllocationsMutationWaiting_nid_postfix() {
    std::lock_guard lock(registry().mutex);
    return !registry().retiring.empty() || registry().unlockedMutations != 0;
}

bool GuestAllocationsAnyGpuRange_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    std::lock_guard lock(registry().mutex);
    const auto& ranges = registry().ranges;
    const std::uint64_t end = address + std::max<std::size_t>(bytes, 1);
    auto it = ranges.upper_bound(address);
    if (it != ranges.begin()) --it;
    for (; it != ranges.end() && it->first < end; ++it) {
        const auto& range = *it->second;
        if (range.gpu && range.readable && range.bytes != 0 && range.address + range.bytes > address) return true;
    }
    return false;
}

void GuestAllocationsSetInvalidator_nid_postfix(void (*callback)(std::uintptr_t, std::size_t)) {
    invalidator.store(callback, std::memory_order_release);
}

void GuestAllocationsInvalidate_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    const JournalEntry change{0, address, address + std::max<std::uint64_t>(bytes, 1)};
    publishChanges(std::span(&change, 1), false, nullptr);
    if (const auto callback = invalidator.load(std::memory_order_acquire)) callback(address, bytes);
}

void GuestAllocationsSetGpuMapObserver_nid_postfix(void (*callback)(const Mapped& ranges, std::uint64_t generation)) {
    gpuMapObserver.store(callback);
    while (gpuMapObserverCalls.load() != 0) std::this_thread::yield();
}

void GuestAllocationsNoteGpuMapping_nid_postfix(void* mutation, const void* pointer, std::size_t bytes) {
    if (mutation == nullptr || bytes == 0) return;
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    auto* state = static_cast<MutationState*>(mutation);
    state->journaled.push_back({0, address, address + bytes});
    state->gpuMapped.emplace_back(address, bytes);
}

void GuestAllocationsSetPinWaiter_nid_postfix(bool (*callback)(std::uintptr_t, std::size_t)) {
    pinWaiter.store(callback, std::memory_order_release);
}

#ifdef _WIN32
namespace {
void registerImage(const void* image) {
    auto& state = registry();
    if (state.registeredImages.contains(reinterpret_cast<std::uintptr_t>(image))) return;
    auto replacement = state.ranges;
    auto cursor = reinterpret_cast<std::uintptr_t>(image);
    bool registered = false;
    for (;;) {
        MEMORY_BASIC_INFORMATION memory{};
        require(VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) == sizeof(memory), "cannot query a guest image");
        if (memory.AllocationBase != image) break;
        require(memory.Type == MEM_IMAGE && (memory.State == MEM_COMMIT || memory.State == MEM_RESERVE), "unsupported guest image mapping");
        require(reinterpret_cast<std::uintptr_t>(memory.BaseAddress) == cursor && memory.RegionSize != 0 && memory.RegionSize <= std::numeric_limits<std::uintptr_t>::max() - cursor, "invalid guest image range");
        if (memory.State == MEM_COMMIT) {
            require((memory.Protect & PAGE_GUARD) == 0, "guarded guest image pages are not supported");
            const auto protection = memory.Protect & 0xffu;
            const bool writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
            const bool readable = writable || protection == PAGE_READONLY || protection == PAGE_EXECUTE_READ;
            require(readable || protection == PAGE_NOACCESS || protection == PAGE_EXECUTE, "unsupported guest image protection");
            const auto next = replacement.lower_bound(cursor);
            require(next == replacement.end() || cursor + memory.RegionSize <= next->first, "guest image overlaps a registered allocation");
            if (next != replacement.begin()) {
                const auto& previous = *std::prev(next)->second;
                require(previous.address + previous.bytes <= cursor, "guest image overlaps a registered allocation");
            }
            replacement.emplace(cursor, std::make_shared<const Range>(Range{cursor, memory.RegionSize, readable, writable, cursor, memory.RegionSize, false, false, freshBacking()}));
            registered = true;
        }
        cursor += memory.RegionSize;
    }
    require(registered, "guest image has no committed pages");
    state.ranges.swap(replacement);
    state.leaseIndex.valid = false;
    state.wholeChangePending = true;
    state.registeredImages.emplace(reinterpret_cast<std::uintptr_t>(image), cursor);
}
}

void GuestAllocationsRegisterMainImage_nid_postfix(void*) {
    auto& state = registry();
    if (state.mainImageRegistered) return;
    const auto image = GetModuleHandleW(nullptr);
    require(image != nullptr, "cannot locate the main guest image");
    registerImage(image);
    state.mainImageRegistered = true;
}

void GuestAllocationsRegisterImage_nid_postfix(void*, const void* image) {
    require(image != nullptr, "cannot register a guest image without a base");
    registerImage(image);
}

#else
namespace {
struct ImagePages {
    struct Page {
        bool readable = false;
        bool writable = false;
    };
    std::uintptr_t base = 0;
    bool main = false;
    bool found = false;
    std::uint64_t pageSize = 0;
    std::map<std::uint64_t, Page> pages;
};

#ifdef __APPLE__
void addSegmentPages(ImagePages& collected, std::uintptr_t address, std::uint64_t bytes, bool readable, bool writable) {
    const auto start = address & ~(collected.pageSize - 1);
    const auto end = (address + bytes + collected.pageSize - 1) & ~(collected.pageSize - 1);
    for (auto page = start; page < end; page += collected.pageSize) {
        auto& entry = collected.pages[page];
        entry.readable = entry.readable || readable;
        entry.writable = entry.writable || writable;
    }
}

void collectMachImagePages(ImagePages& collected) {
    const auto count = _dyld_image_count();
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(index));
        if (header == nullptr || header->magic != MH_MAGIC_64) continue;
        if (collected.main ? header->filetype != MH_EXECUTE : reinterpret_cast<std::uintptr_t>(header) != collected.base) continue;
        collected.found = true;
        const auto slide = static_cast<std::uintptr_t>(_dyld_get_image_vmaddr_slide(index));
        const auto* command = reinterpret_cast<const load_command*>(header + 1);
        for (std::uint32_t loaded = 0; loaded < header->ncmds; ++loaded) {
            if (command->cmd == LC_SEGMENT_64) {
                const auto& segment = *reinterpret_cast<const segment_command_64*>(command);
                if (segment.vmsize != 0 && segment.initprot != VM_PROT_NONE)
                    addSegmentPages(collected, slide + segment.vmaddr, segment.vmsize, (segment.initprot & (VM_PROT_READ | VM_PROT_WRITE)) != 0, (segment.initprot & VM_PROT_WRITE) != 0);
            }
            command = reinterpret_cast<const load_command*>(reinterpret_cast<const std::uint8_t*>(command) + command->cmdsize);
        }
        return;
    }
}
#else
int collectImagePages(dl_phdr_info* image, std::size_t, void* data) {
    auto& collected = *static_cast<ImagePages*>(data);
    if (!collected.main && image->dlpi_addr != collected.base) return 0;
    collected.found = true;
    for (int index = 0; index < image->dlpi_phnum; ++index) {
        const auto& header = image->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || header.p_memsz == 0) continue;
        const auto start = (image->dlpi_addr + header.p_vaddr) & ~(collected.pageSize - 1);
        const auto end = (image->dlpi_addr + header.p_vaddr + header.p_memsz + collected.pageSize - 1) & ~(collected.pageSize - 1);
        for (auto page = start; page < end; page += collected.pageSize) {
            auto& entry = collected.pages[page];
            entry.readable = entry.readable || (header.p_flags & (PF_R | PF_W)) != 0;
            entry.writable = entry.writable || (header.p_flags & PF_W) != 0;
        }
    }
    return 1;
}
#endif

std::uintptr_t registerImagePages(const ImagePages& collected) {
    auto& state = registry();
    const auto pageSize = collected.pageSize;
    const auto& pages = collected.pages;
    auto replacement = state.ranges;
    for (auto page = pages.begin(); page != pages.end();) {
        auto last = page;
        while (std::next(last) != pages.end() && std::next(last)->first == last->first + pageSize && std::next(last)->second.readable == page->second.readable && std::next(last)->second.writable == page->second.writable) ++last;
        const auto address = page->first;
        const auto bytes = static_cast<std::size_t>(last->first + pageSize - address);
        const auto next = replacement.lower_bound(address);
        require(next == replacement.end() || address + bytes <= next->first, "guest image overlaps a registered allocation");
        if (next != replacement.begin()) {
            const auto& previous = *std::prev(next)->second;
            require(previous.address + previous.bytes <= address, "guest image overlaps a registered allocation");
        }
        replacement.emplace(address, std::make_shared<const Range>(Range{address, bytes, page->second.readable, page->second.writable, address, bytes, false, false, freshBacking()}));
        page = std::next(last);
    }
    state.ranges.swap(replacement);
    state.leaseIndex.valid = false;
    state.wholeChangePending = true;
    return static_cast<std::uintptr_t>(std::prev(pages.end())->first + pageSize);
}
}

void GuestAllocationsRegisterMainImage_nid_postfix(void*) {
    auto& state = registry();
    if (state.mainImageRegistered) return;
    ImagePages collected{.main = true, .pageSize = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE))};
#ifdef __APPLE__
    collectMachImagePages(collected);
#else
    dl_iterate_phdr(collectImagePages, &collected);
#endif
    require(!collected.pages.empty(), "main guest image has no loadable segments");
    registerImagePages(collected);
    state.mainImageRegistered = true;
}

void GuestAllocationsRegisterImage_nid_postfix(void*, const void* image) {
    require(image != nullptr, "cannot register a guest image without a base");
    auto& state = registry();
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    if (state.registeredImages.contains(base)) return;
    ImagePages collected{.base = base, .pageSize = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE))};
#ifdef __APPLE__
    collectMachImagePages(collected);
#else
    dl_iterate_phdr(collectImagePages, &collected);
#endif
    require(collected.found, "cannot locate a guest image");
    require(!collected.pages.empty(), "guest image has no loadable segments");
    require(collected.pages.begin()->first == base, "guest image does not start at its base");
    state.registeredImages.emplace(base, registerImagePages(collected));
}
#endif

void GuestAllocationsUnregisterImage_nid_postfix(void* mutation, const void* image) {
    auto& state = registry();
    const auto found = state.registeredImages.find(reinterpret_cast<std::uintptr_t>(image));
    if (found == state.registeredImages.end()) return;
    const auto [begin, end] = *found;
    GuestAllocationsRequireUnpinned_nid_postfix(mutation, image, end - begin);
    recordChange(mutation, image, end - begin);
    std::erase_if(state.ranges, [&](const auto& entry) { return !entry.second->releasable && entry.second->allocationAddress >= begin && entry.second->allocationAddress < end; });
    state.leaseIndex.valid = false;
    state.registeredImages.erase(found);
}

void GuestAllocationsAdd_nid_postfix(void* mutation, void* pointer, std::size_t bytes, bool readable, bool writable, bool gpu) {
    recordChange(mutation, pointer, bytes);
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    require(address != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address, "invalid guest allocation range");
    require(!writable || readable, "writable guest allocation must be readable");
    auto& ranges = registry().ranges;
    const auto next = ranges.lower_bound(address);
    require(next == ranges.end() || (next->first != address && address + bytes <= next->first), "overlapping guest allocation");
    if (next != ranges.begin()) {
        const auto& previous = *std::prev(next)->second;
        require(previous.address + previous.bytes <= address, "overlapping guest allocation");
    }
    ranges.emplace(address, std::make_shared<const Range>(Range{address, bytes, readable, writable, address, bytes, true, gpu, freshBacking()}));
    noteRangeDirty(address, bytes);
}

[[noreturn]] void PinnedFailure(std::uintptr_t address, std::size_t bytes, const char* why) {
    char message[160];
    std::snprintf(message, sizeof(message), "guest allocation 0x%llx+0x%llx is still pinned by GPU work or an import (%s)", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), why);
    throw std::runtime_error(message);
}

void GuestAllocationsRequireUnpinned_nid_postfix(void* mutation, const void* pointer, std::size_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    require(bytes <= std::numeric_limits<std::uint64_t>::max() - address, "guest allocation range overflow");
    const auto end = address + bytes;
    auto& ranges = registry().ranges;
    // GPU work leases allocations (a lease copies the shared_ptr) until the work completed, so wait
    // for it rather than failing the guest. The wait releases the registry lock: the driver's waiter
    // finishes that work under the device lock, under which the driver's workers acquire leases from
    // this registry, so waiting with the lock held would deadlock them. The scan restarts after the
    // lock is retaken, since another mutation may have run meanwhile. Without a waiter the lease is
    // dropped by another thread (a synchronous draw or dispatch), so yielding suffices. A waiter
    auto* state = static_cast<MutationState*>(mutation);
    const auto deadline = std::chrono::steady_clock::now() + pinWait();
    int syncedRounds = 0;
    int fruitlessRounds = 0;
    bool marked = false;
    const auto unmark = [&] {
        if (!marked) return;
        if (!state->lock.owns_lock()) state->lock.lock();
        auto& retiring = registry().retiring;
        retiring.erase(std::find(retiring.begin(), retiring.end(), std::pair<std::uintptr_t, std::uintptr_t>{address, end}));
        registry().readableLease.reset();
        registry().allLease.reset();
        marked = false;
    };
    struct Unmark {
        const decltype(unmark)& release;
        ~Unmark() { release(); }
    } unmarkOnExit{unmark};
    int cacheDrops = 0;
    const auto release = [&] {
        ++registry().unlockedMutations;
        state->lock.unlock();
    };
    const auto retake = [&] {
        state->lock.lock();
        --registry().unlockedMutations;
    };
    for (;;) {
        if (registry().readableLease != nullptr || registry().allLease != nullptr) {
            auto droppedReadable = std::move(registry().readableLease);
            auto droppedAll = std::move(registry().allLease);
            if (leasePatchEnabled() && cacheDrops++ < 2 && state != nullptr && state->lock.owns_lock()) {
                release();
                droppedReadable.reset();
                droppedAll.reset();
                retake();
                continue;
            }
        }
        bool pinned = false;
        auto it = ranges.upper_bound(address);
        if (it != ranges.begin()) --it;
        for (; it != ranges.end(); ++it) {
            const auto& [base, range] = *it;
            if (base >= end && base != address) break;
            if (((address < base + range->bytes && base < end) || base == address) && range.use_count() != 1) {
                pinned = true;
                break;
            }
        }
        if (!pinned) {
            unmark();
            if (state != nullptr && bytes != 0) {
                state->journaled.push_back({0, address, end});
                state->unpinned.emplace_back(address, bytes);
            }
            return;
        }
        if (!marked && state != nullptr && state->lock.owns_lock()) {
            registry().retiring.emplace_back(address, end);
            marked = true;
        }
        if (std::chrono::steady_clock::now() >= deadline) PinnedFailure(address, bytes, ("waited " + std::to_string(pinWait().count()) + " ms for the lease (APS5_PIN_WAIT_MS)").c_str());
        const auto waiter = pinWaiter.load(std::memory_order_acquire);
        bool progressed = false;
        if (state != nullptr && state->lock.owns_lock()) {
            release();
            try {
                if (waiter != nullptr) progressed = waiter(address, bytes);
                else std::this_thread::yield();
            } catch (...) {
                retake();
                throw;
            }
            retake();
        } else {
            std::this_thread::yield();
        }
        if (progressed) {
            // The lease's work finished, yet the range is pinned again: between the waiter's return
            // and the re-lock a driver worker took a new lease (every address-based build pins every
            // registered range). Rare per round, but each round is a GPU wait, so a repeat is reported.
            if (++syncedRounds == 8) std::fprintf(stderr, "[gpu] leased allocation 0x%llx+0x%llx still pinned after %d recorder syncs\n", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), syncedRounds);
            fruitlessRounds = 0;
            continue;
        }
        static const bool backsOff = std::getenv("APS5_NO_LEASE_WAIT_UNLOCKED") == nullptr;
        if (backsOff && ++fruitlessRounds > 8 && state != nullptr && state->lock.owns_lock()) {
            release();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            retake();
        }
    }
}

decltype(registry().ranges)::iterator firstRangeTouching(std::uintptr_t address) {
    auto& ranges = registry().ranges;
    auto it = ranges.lower_bound(address);
    if (it == ranges.begin()) return it;
    const auto& previous = *std::prev(it);
    if (previous.second->bytes != 0 && address < previous.first + previous.second->bytes) return std::prev(it);
    return it;
}

void GuestAllocationsRequireAvailable_nid_postfix(void*, const void* pointer, std::size_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    require(address != 0 && bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address, "invalid fixed guest mapping");
    const auto first = firstRangeTouching(address);
    for (auto it = first; it != registry().ranges.end(); ++it) {
        const auto& [base, range] = *it;
        if (base >= address + bytes) break;
        if (base + range->bytes > address) {
            char message[160];
            std::snprintf(message, sizeof(message), "fixed mapping 0x%llx+0x%llx overlaps a registered guest allocation 0x%llx+0x%llx", static_cast<unsigned long long>(address), static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(base), static_cast<unsigned long long>(range->bytes));
            throw std::runtime_error(message);
        }
    }
}

bool GuestAllocationsOverlaps_nid_postfix(void*, const void* pointer, std::size_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    require(address != 0 && bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address, "invalid guest allocation range");
    const auto first = firstRangeTouching(address);
    for (auto it = first; it != registry().ranges.end(); ++it) {
        const auto& [base, range] = *it;
        if (base >= address + bytes) break;
        if (base + range->bytes > address) return true;
    }
    return false;
}

bool GuestAllocationsForRegistered_nid_postfix(void*, const void* pointer, std::size_t bytes, const std::function<void(const void*, std::size_t)>& visit) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    require(address != 0 && bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address, "invalid guest allocation range");
    const auto end = address + bytes;
    const auto& ranges = registry().ranges;
    auto it = ranges.upper_bound(address);
    if (it != ranges.begin()) --it;
    std::uintptr_t first = 0;
    std::uintptr_t last = 0;
    bool any = false;
    for (; it != ranges.end() && it->first < end; ++it) {
        const auto base = std::max<std::uintptr_t>(it->first, address);
        const auto finish = std::min<std::uintptr_t>(it->first + it->second->bytes, end);
        if (finish <= base) continue;
        if (any && base == last) {
            last = finish;
            continue;
        }
        if (any) visit(reinterpret_cast<const void*>(first), last - first);
        first = base;
        last = finish;
        any = true;
    }
    if (any) visit(reinterpret_cast<const void*>(first), last - first);
    return any;
}

void GuestAllocationsGuestPages_nid_postfix(std::uintptr_t address, std::size_t bytes, std::size_t page, std::uintptr_t* first, std::uintptr_t* end) {
    std::uintptr_t bias = 0;
#ifdef __APPLE__
    struct Search {
        std::uintptr_t address;
        std::uintptr_t hostPage;
        std::uintptr_t bias;
    } search{address, static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE)), 0};
    dl_iterate_phdr([](dl_phdr_info* image, std::size_t, void* data) {
        auto& found = *static_cast<Search*>(data);
        for (unsigned index = 0; index < image->dlpi_phnum; ++index) {
            const auto& header = image->dlpi_phdr[index];
            const auto segment = static_cast<std::uintptr_t>(image->dlpi_addr + header.p_vaddr);
            const auto start = segment & ~(found.hostPage - 1);
            const auto finish = (segment + header.p_memsz + found.hostPage - 1) & ~(found.hostPage - 1);
            if (header.p_type == PT_LOAD && found.address >= start && found.address < finish) {
                found.bias = static_cast<std::uintptr_t>(image->dlpi_addr);
                return 1;
            }
        }
        return 0;
    }, &search);
    bias = search.bias;
#endif
    const auto mask = static_cast<std::uintptr_t>(page - 1);
    *first = bias + ((address - bias) & ~mask);
    *end = bias + ((address + bytes - bias + mask) & ~mask);
}

void GuestAllocationsTrimToRegistered_nid_postfix(void*, std::size_t page, const void** pointer, std::size_t* bytes) {
#ifdef __APPLE__
    auto start = reinterpret_cast<std::uintptr_t>(*pointer);
    const auto end = start + *bytes;
    const auto& ranges = registry().ranges;
    const auto following = ranges.upper_bound(start);
    const bool startCovered = following != ranges.begin() && std::prev(following)->first + std::prev(following)->second->bytes > start;
    if (!startCovered && following != ranges.end() && following->first < end && following->first - start < page) start = following->first;
    *pointer = reinterpret_cast<const void*>(start);
    *bytes = end - start;
#else
    static_cast<void>(page);
    static_cast<void>(pointer);
    static_cast<void>(bytes);
#endif
}

bool GuestAllocationsCovers_nid_postfix(void*, const void* pointer, std::size_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    require(address != 0 && bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address, "invalid guest allocation range");
    const auto end = address + bytes;
    auto cursor = address;
    const auto& ranges = registry().ranges;
    auto it = ranges.upper_bound(address);
    if (it != ranges.begin()) --it;
    for (; it != ranges.end(); ++it) {
        const auto base = it->first;
        const auto finish = base + it->second->bytes;
        if (finish <= cursor) continue;
        if (base > cursor || !it->second->releasable) return false;
        cursor = finish;
        if (cursor >= end) return true;
    }
    return false;
}

Range GuestAllocationsFind_nid_postfix(void*, const void* pointer) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    const auto exact = registry().ranges.find(address);
    if (exact != registry().ranges.end() && exact->second->allocationAddress == address && exact->second->allocationBytes == exact->second->bytes) {
        const auto& range = *exact->second;
        require(range.releasable, "guest image memory is not a releasable allocation");
        return {address, range.allocationBytes, range.readable, range.writable, address, range.allocationBytes, range.releasable, range.gpu};
    }
    for (const auto& [base, range] : registry().ranges) {
        if (range->allocationAddress == address) {
            require(range->releasable, "guest image memory is not a releasable allocation");
            return {address, range->allocationBytes, range->readable, range->writable, address, range->allocationBytes, range->releasable, range->gpu};
        }
    }
    throw std::runtime_error("guest allocation is not registered");
}

void GuestAllocationsRemove_nid_postfix(void* mutation, const void* pointer) {
    const auto range = GuestAllocationsFind_nid_postfix(mutation, pointer);
    GuestAllocationsRequireUnpinned_nid_postfix(mutation, pointer, range.bytes);
    recordChange(mutation, pointer, range.bytes);
    noteRangeDirty(range.address, range.bytes);
    auto& ranges = registry().ranges;
    const auto exact = ranges.find(range.address);
    if (exact != ranges.end() && exact->second->allocationBytes == exact->second->bytes) {
        ranges.erase(exact);
    } else {
        std::erase_if(ranges, [&](const auto& entry) { return entry.second->allocationAddress == range.address; });
    }
    if (const auto observer = unmapObserver.load(std::memory_order_acquire)) observer(range.address, range.bytes);
}

namespace {

struct RangePatch {
    std::vector<std::uint64_t> erased;
    std::vector<std::pair<std::uint64_t, std::shared_ptr<const Range>>> inserted;
};

RangePatch replaceRange(const void* pointer, std::size_t bytes, bool remove, bool readable, bool writable, bool gpu) {
    const std::uint64_t address = reinterpret_cast<std::uintptr_t>(pointer);
    require(bytes != 0 && bytes <= std::numeric_limits<std::uint64_t>::max() - address, "invalid guest protection or unmap range");
    require(!writable || readable, "writable guest allocation must be readable");
    const auto end = address + bytes;
    RangePatch patch;
    auto cursor = address;
    const auto& ranges = registry().ranges;
    auto it = ranges.upper_bound(address);
    if (it != ranges.begin()) --it;
    for (; it != ranges.end(); ++it) {
        const auto base = it->first;
        const auto& range = *it->second;
        const auto finish = base + range.bytes;
        if (finish <= address) continue;
        if (base >= end) break;
        require(base <= cursor, "guest protection or unmap range has a hole");
        patch.erased.push_back(base);
        const auto insert = [&](std::uint64_t first, std::uint64_t last, bool canRead, bool canWrite, bool gpuMapped, std::uint64_t backing) {
            if (first < last) patch.inserted.emplace_back(first, std::make_shared<const Range>(Range{first, static_cast<std::size_t>(last - first), canRead, canWrite, range.allocationAddress, range.allocationBytes, range.releasable, gpuMapped, backing}));
        };
        insert(base, std::max<std::uint64_t>(base, address), range.readable, range.writable, range.gpu, range.backingSerial);
        if (!remove) insert(std::max<std::uint64_t>(base, address), std::min<std::uint64_t>(finish, end), readable, writable, gpu, freshBacking());
        insert(std::min<std::uint64_t>(finish, end), finish, range.readable, range.writable, range.gpu, range.backingSerial);
        cursor = std::min<std::uint64_t>(finish, end);
    }
    require(cursor == end, "guest protection or unmap range is not registered");
    return patch;
}

struct StagedPatch {
    using Ranges = decltype(Registry::ranges);
    const RangePatch& patch;
    Ranges staged;
    explicit StagedPatch(const RangePatch& source) : patch(source) {
        for (const auto& [key, range] : patch.inserted) staged.emplace(key, range);
    }
    void apply() noexcept {
        auto& ranges = registry().ranges;
        for (const auto key : patch.erased) {
            if (const auto found = ranges.find(key); found != ranges.end()) noteRangeDirty(key, found->second->bytes);
            ranges.erase(key);
        }
        for (const auto& [key, range] : patch.inserted) noteRangeDirty(key, range->bytes);
        ranges.merge(staged);
    }
};

void recordReplaced(void* mutation, const RangePatch& patch) {
    for (const auto key : patch.erased) {
        if (const auto found = registry().ranges.find(key); found != registry().ranges.end()) recordChange(mutation, reinterpret_cast<const void*>(key), found->second->bytes);
    }
}

bool allocationRemains(const Range& range, const RangePatch& patch) {
    for (const auto& [key, entry] : patch.inserted) {
        if (entry->allocationAddress == range.allocationAddress) return true;
    }
    const auto& ranges = registry().ranges;
    const auto limit = range.allocationAddress + range.allocationBytes;
    for (auto it = ranges.lower_bound(range.allocationAddress); it != ranges.end() && it->first < limit; ++it) {
        if (it->second->allocationAddress != range.allocationAddress) continue;
        if (std::find(patch.erased.begin(), patch.erased.end(), it->first) == patch.erased.end()) return true;
    }
    return false;
}

}

void GuestAllocationsProtect_nid_postfix(void* mutation, const void* pointer, std::size_t bytes, bool readable, bool writable, bool gpu, const std::function<void()>& apply) {
    GuestAllocationsRequireUnpinned_nid_postfix(mutation, pointer, bytes);
    recordChange(mutation, pointer, bytes);
    const auto patch = replaceRange(pointer, bytes, false, readable, writable, gpu);
    recordReplaced(mutation, patch);
    StagedPatch staged(patch);
    apply();
    staged.apply();
}

void GuestAllocationsUnmap_nid_postfix(void* mutation, const void* pointer, std::size_t bytes, const std::function<void(const void*, std::size_t, const void*, bool)>& apply) {
    GuestAllocationsRequireUnpinned_nid_postfix(mutation, pointer, bytes);
    recordChange(mutation, pointer, bytes);
    const auto end = reinterpret_cast<std::uintptr_t>(pointer) + bytes;
    auto cursor = reinterpret_cast<std::uintptr_t>(pointer);
    bool any = false;
    while (cursor < end) {
        const auto found = registry().ranges.upper_bound(cursor);
        const Range* containing = nullptr;
        if (found != registry().ranges.begin()) {
            const auto& candidate = *std::prev(found)->second;
            if (cursor < candidate.address + candidate.bytes) containing = &candidate;
        }
        if (containing == nullptr) {
            if (found == registry().ranges.end() || found->first >= end) break;
            cursor = found->first;
            continue;
        }
        const auto range = *containing;
        require(range.releasable, "guest image memory cannot be unmapped");
        const auto pieceEnd = std::min<std::uint64_t>(end, range.address + range.bytes);
        const auto patch = replaceRange(reinterpret_cast<const void*>(cursor), pieceEnd - cursor, true, false, false, false);
        const bool last = !allocationRemains(range, patch);
        recordReplaced(mutation, patch);
        StagedPatch staged(patch);
        apply(reinterpret_cast<const void*>(cursor), pieceEnd - cursor, reinterpret_cast<const void*>(range.allocationAddress), last);
        staged.apply();
        if (const auto observer = unmapObserver.load(std::memory_order_acquire)) observer(cursor, pieceEnd - cursor);
        any = true;
        cursor = pieceEnd;
    }
    require(any, "unmap address is not registered");
    if (const auto observer = unmapObserver.load(std::memory_order_acquire)) observer(reinterpret_cast<std::uintptr_t>(pointer), bytes);
}

static void verifyCachedLease(const Lease::Ranges& cached, bool readableOnly) {
    if (!verifyLeasePatch()) return;
    std::size_t at = 0;
    for (const auto& [address, range] : registry().ranges) {
        if (range->bytes == 0 || (readableOnly && !range->readable)) continue;
        if (at >= cached.size() || cached[at] != range) {
            std::fprintf(stderr, "FATAL: APS5_VERIFY_LEASE_PATCH: the cached lease differs from the registry at 0x%llx+0x%llx\n", static_cast<unsigned long long>(range->address), static_cast<unsigned long long>(range->bytes));
            std::abort();
        }
        ++at;
    }
    if (at == cached.size()) return;
    std::fprintf(stderr, "FATAL: APS5_VERIFY_LEASE_PATCH: the cached lease holds %zu ranges past the registry's %zu\n", cached.size() - at, at);
    std::abort();
}

void GuestAllocationsSetUnmapObserver_nid_postfix(void (*callback)(std::uintptr_t, std::size_t)) {
    unmapObserver.store(callback, std::memory_order_release);
}

Lease GuestAllocationsAcquire_nid_postfix() {
    std::lock_guard lock(registry().mutex);
    if (auto shared = registry().readableLease) {
        verifyCachedLease(*shared, true);
        return Lease(std::move(shared));
    }
    const bool retiring = !registry().retiring.empty();
    const auto& index = currentLeaseIndex();
    auto ranges = std::make_shared<Lease::Ranges>();
    ranges->reserve(index.size());
    for (const auto& entry : index) {
        if (entry.readable && (!retiring || !isRetiring(*entry.node->second))) ranges->push_back(entry.node->second);
    }
    if (!retiring) registry().readableLease = ranges;
    return Lease(std::move(ranges));
}

Lease GuestAllocationsAcquireMapped_nid_no_patch(const Mapped& mapped) {
    std::lock_guard lock(registry().mutex);
    auto& registered = registry().ranges;
    std::vector<decltype(registered.begin())> targets;
    targets.reserve(mapped.size());
    for (const auto& weak : mapped) {
        const auto range = weak.lock();
        if (range == nullptr || !range->readable || range->bytes == 0) continue;
        const auto found = registered.find(range->address);
        if (found != registered.end() && found->second == range) targets.push_back(found);
    }
    std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) { return a->first < b->first; });
    auto ranges = std::make_shared<Lease::Ranges>();
    const auto continues = [](const Range& before, const Range& after) {
        return before.address + before.bytes == after.address && before.readable && after.readable && before.writable == after.writable && before.gpu == after.gpu;
    };
    for (const auto target : targets) {
        if (!ranges->empty() && target->first < ranges->back()->address + ranges->back()->bytes) continue;
        auto first = target;
        while (first != registered.begin()) {
            auto previous = std::prev(first);
            while (previous != registered.begin() && previous->second->bytes == 0) --previous;
            if (previous->second->bytes == 0 || !continues(*previous->second, *first->second)) break;
            first = previous;
        }
        for (auto current = first;;) {
            ranges->push_back(current->second);
            auto next = std::next(current);
            while (next != registered.end() && next->second->bytes == 0) ++next;
            if (next == registered.end() || !continues(*current->second, *next->second)) break;
            current = next;
        }
    }
    return Lease(std::move(ranges));
}

static Lease acquireRegisteredLocked() {
    if (registry().retiring.empty()) {
        if (auto shared = registry().readableLease) {
            verifyCachedLease(*shared, true);
            return Lease(std::move(shared));
        }
    }
    const auto& index = currentLeaseIndex();
    auto ranges = std::make_shared<Lease::Ranges>();
    ranges->reserve(index.size());
    for (const auto& entry : index) {
        if (entry.readable) ranges->push_back(entry.node->second);
    }
    if (registry().retiring.empty()) registry().readableLease = ranges;
    return Lease(std::move(ranges));
}

Lease GuestAllocationsAcquireRegistered_nid_postfix() {
    std::lock_guard lock(registry().mutex);
    return acquireRegisteredLocked();
}

bool GuestAllocationsAcquireSettled_nid_no_patch(Lease* lease, std::uint64_t* published) {
    std::lock_guard lock(registry().mutex);
    if (!registry().retiring.empty() || registry().unlockedMutations != 0) return false;
    *published = generation.load(std::memory_order_acquire);
    *lease = acquireRegisteredLocked();
    return true;
}

std::shared_ptr<const Range> GuestAllocationsLookup_nid_postfix(std::uintptr_t address, bool next) {
    std::lock_guard lock(registry().mutex);
    const auto& ranges = registry().ranges;
    auto it = ranges.upper_bound(address);
    if (it != ranges.begin()) {
        const auto& previous = std::prev(it)->second;
        if (previous->bytes != 0 && address < previous->address + previous->bytes) return previous;
    }
    if (next) {
        for (; it != ranges.end(); ++it) {
            if (it->second->bytes != 0) return it->second;
        }
    }
    return nullptr;
}

Lease GuestAllocationsAcquireAll_nid_postfix() {
    std::lock_guard lock(registry().mutex);
    if (auto shared = registry().allLease) {
        verifyCachedLease(*shared, false);
        return Lease(std::move(shared));
    }
    const bool retiring = !registry().retiring.empty();
    const auto& index = currentLeaseIndex();
    auto ranges = std::make_shared<Lease::Ranges>();
    ranges->reserve(index.size());
    for (const auto& entry : index) {
        if (!retiring || !isRetiring(*entry.node->second)) ranges->push_back(entry.node->second);
    }
    if (!retiring) registry().allLease = ranges;
    return Lease(std::move(ranges));
}

}
