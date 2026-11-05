#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestHeap.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace {

using namespace GuestAllocations;

constexpr std::size_t Granule = std::size_t{1} << 26;

void check(bool condition, const char* what) {
    if (condition) return;
    std::fprintf(stderr, "guest allocation journal test failed: %s\n", what);
    std::exit(1);
}

std::byte* arena() {
    static auto* base = static_cast<std::byte*>(std::aligned_alloc(Granule, 8 * Granule));
    return base;
}

bool changed(std::uint64_t since, const void* pointer, std::size_t bytes) {
    return GuestAllocationsChangedSince_nid_postfix(since, reinterpret_cast<std::uintptr_t>(pointer), bytes);
}

std::uint64_t now() {
    return GuestAllocationsGeneration_nid_postfix();
}

void precision(bool journal) {
    auto* first = arena();
    auto* second = arena() + 2 * Granule;
    {
        Mutation mutation;
        mutation.Add(first, 4096, true, true, true);
        mutation.Add(second, 4096, true, true, true);
    }
    auto stamp = now();
    check(!changed(stamp, first, 4096), "nothing changed, yet the range reads as changed");
    {
        Mutation mutation;
        mutation.Remove(first);
    }
    check(changed(stamp, first, 4096), "removing a range must invalidate a proof over it");
    check(changed(stamp, first + 4095, 1), "removing a range must invalidate a proof over its last byte");
    check(changed(stamp, second, 4096) != journal, journal ? "removing a range invalidated a proof elsewhere" : "without the journal any change invalidates");
    check(changed(0, second, 4096), "a generation of zero is no proof");
    check(changed(now() + 1, second, 4096), "a generation from the future is no proof");
    stamp = now();
    {
        Mutation mutation;
        mutation.Add(first, 4096, true, true, true);
    }
    check(changed(stamp, first, 4096), "mapping a range must invalidate a proof over it");
    stamp = now();
    {
        Mutation mutation;
        mutation.Remove(first);
    }
    {
        Mutation mutation;
        mutation.Remove(second);
    }
}

void unmapAndRemap(bool journal) {
    auto* base = arena() + 4 * Granule;
    auto* far = arena() + 6 * Granule;
    {
        Mutation mutation;
        mutation.Add(base, 3 * 65536, true, true, true);
        mutation.Add(far, 65536, true, true, true);
    }
    auto stamp = now();
    {
        Mutation mutation;
        mutation.Unmap(base + 65536, 65536, [](const void*, std::size_t, const void*, bool) {});
    }
    check(changed(stamp, base + 65536, 65536), "an overlapping unmap must invalidate");
    check(changed(stamp, base + 65536 + 65535, 1), "an unmap must invalidate a proof over its last byte");
    check(changed(stamp, far, 65536) != journal, journal ? "an unmap invalidated a proof far away" : "without the journal any change invalidates");
    stamp = now();
    {
        Mutation mutation;
        mutation.Add(base + 65536, 65536, true, false, true);
    }
    check(changed(stamp, base + 65536, 4096), "a remap at the same address must invalidate");
    stamp = now();
    {
        Mutation mutation;
        mutation.Protect(base + 65536, 65536, true, true, true, [] {});
    }
    check(changed(stamp, base + 65536, 4096), "a protection change must invalidate");
    stamp = now();
    {
        Mutation mutation;
        mutation.RequireUnpinned(base, 65536);
    }
    check(changed(stamp, base, 65536), "a backing renewal must invalidate");
    stamp = now();
    GuestAllocationsInvalidate_nid_postfix(reinterpret_cast<std::uintptr_t>(far), 65536);
    check(changed(stamp, far, 65536), "an invalidated range must read as changed");
    {
        Mutation mutation;
        mutation.Unmap(base, 65536, [](const void*, std::size_t, const void*, bool) {});
        mutation.Unmap(base + 65536, 65536, [](const void*, std::size_t, const void*, bool) {});
        mutation.Unmap(base + 2 * 65536, 65536, [](const void*, std::size_t, const void*, bool) {});
        mutation.Remove(far);
    }
}

void noopMutation(bool journal) {
    auto* base = arena();
    {
        Mutation mutation;
        mutation.Add(base, 65536, true, true, true);
    }
    const auto stamp = now();
    auto lease = GuestAllocationsAcquire_nid_postfix();
    const auto* snapshot = lease.Snapshot().get();
    {
        Mutation mutation;
    }
    check(now() == stamp + 1, "every mutation moves the generation");
    check(changed(stamp, base, 65536) != journal, "a mutation that changed nothing invalidates no range");
    const bool kept = GuestAllocationsAcquire_nid_postfix().Snapshot().get() == snapshot;
    check(kept == (journal && std::getenv("APS5_NO_LEASE_PATCH") == nullptr), "a mutation that changed nothing keeps the cached lease");
    lease.clear();
    {
        Mutation mutation;
        mutation.Remove(base);
    }
}

void spansAndScale(bool journal) {
    auto* base = arena();
    const auto stamp = now();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
    std::uint64_t through = 0;
    {
        Mutation mutation;
        mutation.Add(base, 65536, true, true, false);
    }
    {
        Mutation mutation;
        mutation.Add(base + 65536, 65536, true, true, false);
    }
    const bool answered = GuestAllocationsChangesSince_nid_no_patch(stamp, &spans, &through);
    check(answered == journal, "the spans are available exactly when the journal is");
    if (journal) {
        const auto address = reinterpret_cast<std::uint64_t>(base);
        check(through == now(), "the spans run through the current generation");
        check(spans.size() == 1 && spans[0].first == address && spans[0].second == address + 2 * 65536, "adjacent changes merge into one sorted span");
    }
    const auto wide = now();
    check(changed(wide, base, 65536) == false, "a current stamp reads as unchanged");
    {
        Mutation mutation;
        mutation.Remove(base);
    }
    check(changed(wide, base, std::size_t{1} << 30), "a query wider than the fine table must see a small change inside it");
    const auto stampSmall = now();
    {
        Mutation mutation;
        mutation.Remove(base + 65536);
    }
    check(changed(stampSmall, base + 65536, 16), "a small change at the query");
    auto* large = arena() + 2 * Granule;
    const auto beforeLarge = now();
    {
        Mutation mutation;
        mutation.Add(large, 4 * Granule, true, true, false);
    }
    check(changed(beforeLarge, large + 3 * Granule + 4096, 4096), "a small query must see a change wider than the fine table");
    {
        Mutation mutation;
        mutation.Remove(large);
    }
    if (journal) {
        const auto old = now();
        for (int round = 0; round < 8200; ++round) {
            Mutation mutation;
            mutation.Add(base, 65536, true, true, false);
            mutation.Remove(base);
        }
        check(!GuestAllocationsChangesSince_nid_no_patch(old, &spans, &through), "spans older than the journal holds must not answer");
    }
}

void leaseIndex() {
    auto* base = arena();
    std::vector<const void*> added;
    for (int index = 0; index < 64; index += 2) {
        added.push_back(base + index * 65536);
        Mutation mutation;
        mutation.Add(base + index * 65536, 65536, index % 4 == 0, index % 8 == 0, true);
    }
    const auto expect = [&] {
        const auto all = GuestAllocationsAcquireAll_nid_postfix();
        const auto readable = GuestAllocationsAcquire_nid_postfix();
        const auto registered = GuestAllocationsAcquireRegistered_nid_postfix();
        check(std::is_sorted(all.begin(), all.end(), [](const auto& left, const auto& right) { return left->address < right->address; }), "a lease is sorted");
        std::size_t wanted = 0;
        for (const auto& range : all) wanted += range->readable ? 1 : 0;
        check(readable.size() == wanted && registered.size() == wanted, "the readable leases hold the readable ranges of the registry");
        std::size_t at = 0;
        for (const auto& range : all) {
            if (!range->readable) continue;
            check(readable[at] == range, "the readable lease holds the registry's range");
            ++at;
        }
        return all.size();
    };
    const auto count = expect();
    check(count == added.size(), "every added range is leased");
    {
        Mutation mutation;
        mutation.Remove(added[3]);
    }
    check(expect() == count - 1, "a removed range leaves the lease");
    {
        Mutation mutation;
        mutation.Add(base + 62 * 65536 + 65536, 65536, true, true, true);
    }
    check(expect() == count, "an added range joins the lease");
    {
        Mutation mutation;
        mutation.Protect(added[4], 4096, false, false, true, [] {});
    }
    expect();
    for (int round = 0; round < 40; ++round) {
        Mutation mutation;
        mutation.Add(base + (70 + round) * 65536, 4096, true, true, false);
    }
    expect();
    static_cast<void>(GuestAllocationsAcquire_nid_postfix());
    static_cast<void>(GuestAllocationsAcquireAll_nid_postfix());
    {
        Mutation mutation;
        mutation.RequireUnpinned(added[0], 65536);
        mutation.Unmap(added[0], 65536, [](const void*, std::size_t, const void*, bool) {});
    }
    for (int round = 0; round < 40; ++round) {
        Mutation mutation;
        mutation.Remove(base + (70 + round) * 65536);
    }
    expect();
}

struct RemovalPath {
    const char* name;
    void (*change)(std::byte*);
};

std::weak_ptr<const Range> rangeAt(const Lease& lease, const void* pointer) {
    for (const auto& range : lease) {
        if (range->address == reinterpret_cast<std::uint64_t>(pointer)) return range;
    }
    return {};
}

void cachedSnapshotsFollowRemovals() {
    auto* base = arena() + 3 * Granule;
    const std::vector<RemovalPath> paths{
        {"Remove", [](std::byte* at) { Mutation mutation; mutation.Remove(at); }},
        {"Unmap", [](std::byte* at) { Mutation mutation; mutation.Unmap(at, 65536, [](const void*, std::size_t, const void*, bool) {}); }},
        {"a partial Unmap", [](std::byte* at) { Mutation mutation; mutation.Unmap(at + 4096, 4096, [](const void*, std::size_t, const void*, bool) {}); }},
        {"Protect", [](std::byte* at) { Mutation mutation; mutation.Protect(at, 65536, false, false, true, [] {}); }},
        {"a partial Protect", [](std::byte* at) { Mutation mutation; mutation.Protect(at + 4096, 4096, true, false, true, [] {}); }},
    };
    for (const auto& path : paths) {
        {
            Mutation mutation;
            mutation.Add(base, 65536, true, true, true);
        }
        std::weak_ptr<const Range> old;
        {
            const auto readable = GuestAllocationsAcquire_nid_postfix();
            const auto all = GuestAllocationsAcquireAll_nid_postfix();
            old = rangeAt(readable, base);
            check(!old.expired(), "the snapshot holds the added range");
            check(old.use_count() >= 3, "the cached snapshots hold the range");
        }
        path.change(base);
        if (!old.expired()) {
            std::fprintf(stderr, "guest allocation journal test failed: a cached snapshot outlived %s\n", path.name);
            std::exit(1);
        }
        const auto end = reinterpret_cast<std::uint64_t>(base) + 65536;
        std::vector<std::pair<const void*, std::size_t>> pieces;
        for (const auto& range : GuestAllocationsAcquireAll_nid_postfix()) {
            if (range->address >= reinterpret_cast<std::uint64_t>(base) && range->address < end) pieces.emplace_back(reinterpret_cast<const void*>(range->address), range->bytes);
        }
        Mutation mutation;
        for (const auto& [pointer, bytes] : pieces) mutation.Unmap(pointer, bytes, [](const void*, std::size_t, const void*, bool) {});
    }
}

std::atomic<int> pinWaits{0};
Lease* pinnedLease = nullptr;

bool releasePinnedLease(std::uintptr_t, std::size_t) {
    ++pinWaits;
    if (pinnedLease != nullptr) pinnedLease->clear();
    return true;
}

void aHeldLeaseBlocksRemoval() {
    auto* base = arena() + 3 * Granule;
    {
        Mutation mutation;
        mutation.Add(base, 65536, true, true, true);
    }
    GuestAllocationsSetPinWaiter_nid_postfix(&releasePinnedLease);
    Lease held = GuestAllocationsAcquire_nid_postfix();
    pinnedLease = &held;
    const auto old = rangeAt(held, base);
    check(!old.expired(), "the held lease covers the range");
    pinWaits = 0;
    {
        Mutation mutation;
        mutation.Remove(base);
    }
    check(pinWaits >= 1 && held.empty() && old.expired(), "a removal finished while a lease still held the range");
    pinnedLease = nullptr;
    GuestAllocationsSetPinWaiter_nid_postfix(nullptr);
}

void heapFreeDropsSnapshots() {
    auto* pointer = GuestHeap::GuestHeapAllocate_nid_postfix(4096);
    std::weak_ptr<const Range> old;
    {
        const auto lease = GuestAllocationsAcquire_nid_postfix();
        old = rangeAt(lease, pointer);
        check(!old.expired(), "the heap block is leased");
    }
    const auto stamp = now();
    GuestHeap::GuestHeapFree_nid_postfix(pointer);
    check(old.expired(), "a cached snapshot outlived a heap free");
    check(changed(stamp, pointer, 4096), "a heap free must invalidate a proof over the block");
    check(rangeAt(GuestAllocationsAcquire_nid_postfix(), pointer).expired(), "a freed heap block is still leased");
}


void settledLeaseNeverShowsHalfAMutation() {
    auto* first = arena() + 4 * Granule;
    auto* second = first + 4096;
    {
        Mutation mutation;
        mutation.Add(first, 4096, true, true, true);
        mutation.Add(second, 4096, true, true, true);
    }
    const auto halfDone = [&](const Lease& lease) {
        const auto low = rangeAt(lease, first).lock();
        const auto high = rangeAt(lease, second).lock();
        return low != nullptr && high != nullptr && low->writable != high->writable;
    };
    std::atomic<bool> stop{false};
    std::uint64_t half = 0;
    std::uint64_t settledHalf = 0;
    std::uint64_t settled = 0;
    std::thread reader([&] {
        while (!stop.load()) {
            half += halfDone(GuestAllocationsAcquireRegistered_nid_postfix()) ? 1 : 0;
            Lease lease;
            std::uint64_t published = 0;
            if (!GuestAllocationsAcquireSettled_nid_no_patch(&lease, &published)) continue;
            ++settled;
            settledHalf += halfDone(lease) ? 1 : 0;
        }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (bool writable = false; std::chrono::steady_clock::now() < deadline; writable = !writable) {
        Mutation mutation;
        mutation.Protect(first, 4096, true, writable, true, [] {});
        mutation.Protect(second, 4096, true, writable, true, [] {});
    }
    stop.store(true);
    reader.join();
    std::printf("half-done mutations seen by plain leases: %llu; settled leases taken: %llu\n", static_cast<unsigned long long>(half), static_cast<unsigned long long>(settled));
    check(settled != 0, "no settled lease was ever available");
    check(settledHalf == 0, "a settled lease showed a mutation half done");
    Mutation mutation;
    mutation.Remove(first);
    mutation.Remove(second);
}

}

int main() {
    const bool journal = std::getenv("APS5_NO_ALLOCATION_JOURNAL") == nullptr;
    std::memset(arena(), 0, 4096);
    precision(journal);
    unmapAndRemap(journal);
    noopMutation(journal);
    spansAndScale(journal);
    leaseIndex();
    cachedSnapshotsFollowRemovals();
    aHeldLeaseBlocksRemoval();
    heapFreeDropsSnapshots();
    settledLeaseNeverShowsHalfAMutation();
    std::puts("guest allocation journal tests passed");
    return 0;
}
