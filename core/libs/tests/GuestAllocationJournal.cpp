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
#include <functional>
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

bool pinWaitJournal(bool journal) {
    return journal && std::getenv("APS5_NO_PIN_WAIT_JOURNAL") == nullptr;
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


void changedLeaseCoversOnlyTheCover(bool journal) {
    auto* low = arena() + 7 * Granule;
    auto* mid = low + 65536;
    auto* far = low + 16 * 65536;
    auto* hidden = low + 32 * 65536;
    {
        Mutation mutation;
        mutation.Add(low, 65536, true, true, true);
        mutation.Add(mid, 65536, true, true, true);
        mutation.Add(far, 65536, true, true, true);
        mutation.Add(hidden, 65536, false, false, true);
    }
    const auto at = [](const void* pointer) { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer)); };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> seen;
    const auto select = [&](const std::vector<std::pair<std::uint64_t, std::uint64_t>>& spans, std::vector<std::pair<std::uint64_t, std::uint64_t>>& cover) {
        seen = spans;
        cover.emplace_back(at(hidden), at(hidden) + 65536);
        cover.emplace_back(at(low), at(mid) + 1);
        cover.emplace_back(at(low) + 4096, at(mid) + 65536);
    };
    const auto stamp = now();
    {
        Mutation mutation;
        mutation.Protect(mid, 65536, true, false, true, [] {});
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> changed;
    std::uint64_t through = 0;
    Lease lease;
    Lease whole;
    const bool answered = GuestAllocationsAcquireChanged_nid_no_patch(stamp, select, &changed, &through, &lease, &whole);
    check(answered == journal, journal ? "the journal did not answer for the changes since a recent generation" : "without the journal a changed lease was handed out");
    if (journal) {
        check(through == now(), "the changed lease names an older generation than the registry's");
        check(changed == seen && changed.size() == 1 && changed.front().first <= at(mid) && changed.front().second >= at(mid) + 65536 && changed.front().second <= at(far), "the changes handed to the selection are not the protected range alone");
        check(lease.size() == 2 && lease[0]->address == at(low) && lease[1]->address == at(mid) && !lease[1]->writable, "the changed lease does not hold exactly the readable ranges over the cover, as they are now");
        const auto full = GuestAllocationsAcquire_nid_postfix();
        check(whole.size() == full.size() && std::equal(whole.begin(), whole.end(), full.begin()), "the whole lease taken with the changes is not the registry's");
        check(rangeAt(whole, low).lock() == lease[0] && rangeAt(whole, mid).lock() == lease[1] && !rangeAt(whole, far).expired(), "the changed lease and the whole lease disagree on a range");
        check(GuestAllocationsAcquireChanged_nid_no_patch(through, select, &changed, &through, &lease, nullptr) && changed.empty(), "the registry changed although no mutation ran");
        check(!GuestAllocationsAcquireChanged_nid_no_patch(0, select, &changed, &through, &lease, nullptr), "a generation of zero produced a changed lease");
    }
    lease.clear();
    whole.clear();
    auto* added = low + 48 * 65536;
    {
        std::uint64_t published = 0;
        const auto current = GuestAllocationsAcquire_nid_postfix();
        check(GuestAllocationsLeaseCurrent_nid_no_patch(current, &published) && published == now(), "a lease taken with no mutation since is not current");
        {
            Mutation mutation;
            mutation.Add(added, 65536, true, true, true);
        }
        check(!GuestAllocationsLeaseCurrent_nid_no_patch(current, &published), "a lease taken before a registration is still current");
        check(!GuestAllocationsLeaseCurrent_nid_no_patch(Lease{}, &published), "an empty lease is current");
        check(GuestAllocationsLeaseCurrent_nid_no_patch(GuestAllocationsAcquire_nid_postfix(), &published) && published == now(), "a lease taken after the registration is not current");
    }
    Mutation mutation;
    mutation.Remove(low);
    mutation.Remove(mid);
    mutation.Remove(far);
    mutation.Remove(hidden);
    mutation.Remove(added);
}

bool spansCover(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& spans, const void* pointer, std::size_t bytes) {
    const auto begin = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(pointer));
    return std::any_of(spans.begin(), spans.end(), [&](const auto& span) { return span.first <= begin && span.second >= begin + bytes; });
}

void unpublishedChangesAreNeverJournalCovered(bool journal) {
    auto* pinned = arena() + 5 * Granule;
    auto* added = pinned + 16 * 65536;
    auto* renewing = pinned + 64 * 65536;
    {
        Mutation mutation;
        mutation.Add(pinned, 65536, true, true, true);
        mutation.Add(renewing, 4 * 65536, true, true, true);
    }
    auto stamp = now();
    {
        Mutation mutation;
        mutation.RequireUnpinned(renewing + 65536, 65536);
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
    std::uint64_t through = 0;
    if (journal) check(GuestAllocationsChangesSince_nid_no_patch(stamp, &spans, &through) && spansCover(spans, renewing, 4 * 65536), "renewing the backing of a range journaled only the unpinned part of it");
    const auto select = [](const std::vector<std::pair<std::uint64_t, std::uint64_t>>& changed, std::vector<std::pair<std::uint64_t, std::uint64_t>>& cover) { cover = changed; };
    stamp = now();
    auto held = std::make_unique<Lease>(GuestAllocationsAcquire_nid_postfix());
    std::thread mutator([&] {
        Mutation mutation;
        mutation.Add(added, 4 * 65536, true, true, true);
        mutation.RequireUnpinned(pinned, 65536);
        mutation.Remove(pinned);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto retiring = [&] { return GuestAllocationsMutationWaiting_nid_postfix() && rangeAt(GuestAllocationsAcquire_nid_postfix(), pinned).expired(); };
    while (!retiring() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(retiring(), "the mutation never waited for the leased range");
    {
        const auto live = GuestAllocationsAcquire_nid_postfix();
        check(!rangeAt(live, added).expired() && rangeAt(live, pinned).expired(), "while a mutation waits on a pinned range, a lease does not show its earlier registration and hide the retiring range");
        if (journal) check(GuestAllocationsChangesSince_nid_no_patch(stamp, &spans, &through) && !spansCover(spans, added, 1), "the journal covers a registration its mutation has not published");
        std::vector<std::pair<std::uint64_t, std::uint64_t>> changed;
        Lease partial;
        Lease whole;
        bool unsettled = false;
        const bool answered = GuestAllocationsAcquireChanged_nid_no_patch(stamp, select, &changed, &through, &partial, &whole, &unsettled);
        if (pinWaitJournal(journal)) {
            check(answered && unsettled, "the journal was switched off while a mutation waits on a pinned range");
            check(through == stamp, "a changed lease during a pin wait claims a generation the waiting mutation has not published");
            check(spansCover(changed, added, 4 * 65536), "a changed lease during a pin wait does not cover the waiting mutation's unpublished registration");
            check(spansCover(changed, pinned, 65536), "a changed lease during a pin wait does not cover the retiring range");
            check(!rangeAt(partial, added).expired() && rangeAt(partial, pinned).expired(), "a changed lease during a pin wait does not show the unpublished registration and hide the retiring range");
            check(rangeAt(whole, pinned).expired(), "the whole lease beside a changed lease during a pin wait shows the retiring range");
        } else {
            check(!answered && !unsettled, "a changed lease was handed out while a mutation waits on a pinned range, so its unpublished changes were not covered");
        }
        std::uint64_t published = 0;
        check(!GuestAllocationsLeaseCurrent_nid_no_patch(live, &published), "a lease taken while a mutation waits on a pinned range is current");
    }
    held.reset();
    mutator.join();
    if (journal) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> changed;
        Lease partial;
        check(GuestAllocationsAcquireChanged_nid_no_patch(stamp, select, &changed, &through, &partial, nullptr) && spansCover(changed, added, 4 * 65536) && spansCover(changed, pinned, 65536), "once the mutation ended, the journal does not cover what it changed");
    }
    Mutation mutation;
    mutation.Remove(added);
    mutation.Remove(renewing);
}

void leasesKnowTheirGeneration(bool journal) {
    auto* pinned = arena() + 5 * Granule;
    auto* later = pinned + 32 * 65536;
    {
        Mutation mutation;
        mutation.Add(pinned, 65536, true, true, true);
    }
    const auto taken = now();
    auto lease = std::make_unique<Lease>(GuestAllocationsAcquire_nid_postfix());
    std::uint64_t published = 0;
    check(GuestAllocationsLeaseGeneration_nid_no_patch(*lease, &published) && published == taken, "a current settled lease does not know its generation");
    {
        Mutation mutation;
        mutation.Add(later, 65536, true, true, true);
    }
    check(now() != taken && GuestAllocationsLeaseGeneration_nid_no_patch(*lease, &published) && published == taken, "a settled lease taken before a registration does not know the generation it was taken at");
    check(!GuestAllocationsLeaseGeneration_nid_no_patch(Lease{}, &published), "an empty lease knows a generation");
    std::thread mutator([&] {
        Mutation mutation;
        mutation.RequireUnpinned(pinned, 65536);
        mutation.Remove(pinned);
    });
    const auto retiring = [&] { return GuestAllocationsMutationWaiting_nid_postfix() && rangeAt(GuestAllocationsAcquire_nid_postfix(), pinned).expired(); };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!retiring() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(retiring(), "the mutation never waited for the leased range");
    {
        const auto waiting = GuestAllocationsAcquire_nid_postfix();
        bool unsettled = false;
        if (pinWaitJournal(journal)) check(GuestAllocationsLeaseGeneration_nid_no_patch(waiting, &published, &unsettled) && unsettled && published == now(), "a lease taken while a mutation waits on a pinned range does not know the last published generation");
        else check(!GuestAllocationsLeaseGeneration_nid_no_patch(waiting, &published), "a lease taken while a mutation waits on a pinned range knows a generation");
    }
    lease.reset();
    mutator.join();
    Mutation mutation;
    mutation.Remove(later);
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

std::shared_ptr<const Range> leasedAtOrBelow(std::uintptr_t address) {
    const auto lease = GuestAllocationsAcquire_nid_postfix();
    auto found = std::upper_bound(lease.begin(), lease.end(), address, [](std::uint64_t value, const auto& range) { return value < range->address; });
    if (found == lease.begin()) return nullptr;
    return *--found;
}

void waitUntil(const std::function<bool()>& condition, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(condition(), what);
}

bool retiringAt(const void* pointer) {
    return GuestAllocationsRetiring_nid_postfix(reinterpret_cast<std::uintptr_t>(pointer), 1);
}

void lookupAgreesWithTheLease() {
    auto* base = arena() + Granule;
    auto* unreadable = base + 2 * 65536;
    auto* upper = base + 4 * 65536;
    auto* pinned = base + 6 * 65536;
    {
        Mutation mutation;
        mutation.Add(base, 65536, true, true, true);
        mutation.Add(unreadable, 65536, false, false, false);
        mutation.Add(upper, 65536, true, false, true);
        mutation.Add(pinned, 65536, true, true, true);
    }
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    std::vector<std::uintptr_t> probes{start - 1, start};
    for (std::uintptr_t offset = 0; offset <= 8 * 65536; offset += 16384) probes.push_back(start + offset + 4095);
    for (std::uintptr_t offset = 65536; offset <= 8 * 65536; offset += 65536) {
        probes.push_back(start + offset - 1);
        probes.push_back(start + offset);
    }
    const auto agree = [&](const char* what) {
        for (const auto probe : probes) check(GuestAllocationsReadableAtOrBelow_nid_postfix(probe) == leasedAtOrBelow(probe), what);
    };
    agree("the registry lookup and a readable lease find different ranges at or below an address");
    check(GuestAllocationsReadableAtOrBelow_nid_postfix(reinterpret_cast<std::uintptr_t>(unreadable) + 100) == GuestAllocationsReadableAtOrBelow_nid_postfix(start), "the lookup inside an unreadable range does not fall back to the readable range below it");
    auto held = std::make_unique<Lease>(GuestAllocationsAcquire_nid_postfix());
    std::thread mutator([&] {
        Mutation mutation;
        mutation.RequireUnpinned(pinned, 65536);
        mutation.Remove(pinned);
    });
    waitUntil([&] { return retiringAt(pinned); }, "the mutation never waited for the leased range");
    agree("the registry lookup and a readable lease find different ranges while a range is retiring");
    check(GuestAllocationsReadableAtOrBelow_nid_postfix(reinterpret_cast<std::uintptr_t>(pinned) + 100) == GuestAllocationsReadableAtOrBelow_nid_postfix(reinterpret_cast<std::uintptr_t>(upper)), "the lookup inside a retiring range does not fall back to the range below it");
    held.reset();
    mutator.join();
    agree("the registry lookup and a readable lease find different ranges after a removal");
    Mutation mutation;
    mutation.Remove(base);
    mutation.Remove(unreadable);
    mutation.Remove(upper);
}

bool holds(const Lease& lease, const void* pointer) {
    return !rangeAt(lease, pointer).expired();
}

void readableLeaseCachedDuringPinWaits() {
    const bool cached = std::getenv("APS5_NO_RETIRING_LEASE_CACHE") == nullptr;
    auto* first = arena() + Granule;
    auto* second = first + 2 * 65536;
    auto* added = first + 4 * 65536;
    auto* kept = first + 6 * 65536;
    {
        Mutation mutation;
        mutation.Add(first, 65536, true, true, true);
        mutation.Add(second, 65536, true, true, true);
        mutation.Add(kept, 65536, true, true, true);
    }
    const auto counts = [] { return GuestAllocationsLeaseCounts_nid_postfix(); };
    const auto ended = [&] { const auto now = counts(); return now.pinWaitsUnderMs + now.pinWaitsUnderTenMs + now.pinWaitsLonger; };
    const auto endedBefore = ended();
    auto firstPin = std::make_unique<Lease>(GuestAllocationsAcquire_nid_postfix());
    std::atomic<bool> firstDone{false};
    const auto pinWaitsBefore = counts().pinWaits;
    std::thread firstMutator([&] {
        Mutation mutation;
        mutation.RequireUnpinned(first, 65536);
        mutation.Remove(first);
        firstDone = true;
    });
    waitUntil([&] { return retiringAt(first); }, "the first mutation never waited for its leased range");
    check(counts().pinWaits == pinWaitsBefore + 1, "a pin wait starting was not counted");
    auto before = counts();
    auto starting = std::make_unique<Lease>(GuestAllocationsAcquire_nid_postfix());
    auto again = std::make_unique<Lease>(GuestAllocationsAcquire_nid_postfix());
    auto after = counts();
    check(!holds(*starting, first) && holds(*starting, second) && holds(*starting, kept), "a lease taken while a pin wait starts does not hide the retiring range and show the others");
    check(after.retiringBuilds == before.retiringBuilds + 1, "the first lease of a pin wait was not built");
    if (cached) check(starting->Snapshot() == again->Snapshot() && after.retiringHits == before.retiringHits + 1, "a second lease during an unchanged pin wait was built again instead of served from the cache");
    else check(starting->Snapshot() != again->Snapshot() && after.retiringHits == before.retiringHits, "with APS5_NO_RETIRING_LEASE_CACHE=1 a lease during a pin wait was served from a cache");
    {
        Mutation mutation;
        mutation.Add(added, 65536, true, true, true);
    }
    auto mapped = std::make_unique<Lease>(GuestAllocationsAcquire_nid_postfix());
    check(mapped->Snapshot() != starting->Snapshot() && holds(*mapped, added) && !holds(*mapped, first), "a lease after a new map during the pin wait does not show the new range");
    starting.reset();
    again.reset();
    mapped.reset();
    auto secondPin = std::make_unique<Lease>(GuestAllocationsAcquire_nid_postfix());
    auto cachedBeforeSecond = std::make_unique<Lease>(GuestAllocationsAcquire_nid_postfix());
    check(holds(*cachedBeforeSecond, second), "the lease cached before the second pin wait does not hold the range it will wait on");
    const auto cachedSnapshot = cachedBeforeSecond->Snapshot().get();
    std::atomic<bool> secondDone{false};
    std::thread secondMutator([&] {
        Mutation mutation;
        mutation.RequireUnpinned(second, 65536);
        mutation.Remove(second);
        secondDone = true;
    });
    waitUntil([&] { return retiringAt(second); }, "the second mutation never waited for its leased range");
    auto retiringTwo = std::make_unique<Lease>(GuestAllocationsAcquire_nid_postfix());
    check(retiringTwo->Snapshot().get() != cachedSnapshot && !holds(*retiringTwo, first) && !holds(*retiringTwo, second) && holds(*retiringTwo, added), "a lease after a second range started retiring still shows it or misses a live range");
    retiringTwo.reset();
    cachedBeforeSecond.reset();
    firstPin.reset();
    waitUntil([&] { return firstDone.load(); }, "the first mutation did not finish once its range was released while the pin-wait cache held other ranges");
    firstMutator.join();
    auto finished = std::make_unique<Lease>(GuestAllocationsAcquire_nid_postfix());
    check(!holds(*finished, first) && !holds(*finished, second) && holds(*finished, added) && holds(*finished, kept), "a lease after a range finished retiring shows a removed or retiring range, or misses a live one");
    finished.reset();
    secondPin.reset();
    waitUntil([&] { return secondDone.load(); }, "the second mutation did not finish: a lease cached during the first pin wait still pins its range");
    secondMutator.join();
    before = counts();
    const auto settled = GuestAllocationsAcquire_nid_postfix();
    const auto settledAgain = GuestAllocationsAcquire_nid_postfix();
    after = counts();
    check(settled.Snapshot() == settledAgain.Snapshot() && after.retiringBuilds == before.retiringBuilds && after.retiringHits == before.retiringHits && !holds(settled, second), "once no range is retiring the settled lease cache does not serve leases again");
    check(ended() == endedBefore + 2, "the two ended pin waits were not counted by length");
    Mutation mutation;
    mutation.Remove(added);
    mutation.Remove(kept);
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
    changedLeaseCoversOnlyTheCover(journal);
    unpublishedChangesAreNeverJournalCovered(journal);
    leasesKnowTheirGeneration(journal);
    settledLeaseNeverShowsHalfAMutation();
    lookupAgreesWithTheLease();
    readableLeaseCachedDuringPinWaits();
    std::puts("guest allocation journal tests passed");
    return 0;
}
