#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Spans = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

constexpr std::size_t RegionBytes = 0x40000;
constexpr std::size_t Alignment = 0x10000;

void Require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "guest allocation changes: %s\n", message);
    std::exit(1);
}

std::uint64_t Generation() {
    return GuestAllocations::GuestAllocationsGeneration_nid_postfix();
}

GuestAllocations::ChangesAnswer Answer(std::uint64_t since, Spans& spans, std::uint64_t& current) {
    spans.clear();
    return GuestAllocations::GuestAllocationsChangesSince_nid_no_patch(since, spans, current);
}

bool Since(std::uint64_t since, Spans& spans, std::uint64_t& current) {
    return Answer(since, spans, current) == GuestAllocations::ChangesAnswer::Answered;
}

bool Covers(Spans spans, std::uint64_t begin, std::uint64_t end) {
    std::sort(spans.begin(), spans.end());
    auto reached = begin;
    for (const auto& [first, last] : spans) {
        if (first > reached) break;
        reached = std::max(reached, last);
        if (reached >= end) return true;
    }
    return reached >= end;
}

struct Region {
    std::unique_ptr<unsigned char[]> storage{new unsigned char[RegionBytes + Alignment]};
    unsigned char* base = reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(storage.get()) + Alignment - 1) & ~(Alignment - 1));
    std::uint64_t At(std::size_t offset) const { return reinterpret_cast<std::uintptr_t>(base) + offset; }
};

void Logged(const Region& region) {
    Spans spans;
    std::uint64_t current = 0;
    Require(!Since(0, spans, current), "a generation older than the log is answered");
    const auto start = Generation();
    Require(Since(start, spans, current) && spans.empty() && current == start, "the current generation reports changes");
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(region.base, 0x10000, true, true, false);
    }
    Require(Since(start, spans, current) && current == start + 1 && Covers(spans, region.At(0), region.At(0x10000)), "an added range is not logged");
    const auto added = current;
    {
        GuestAllocations::Mutation mutation;
        mutation.Protect(region.base + 0x4000, 0x4000, true, false, false, [] {});
    }
    Require(Since(added, spans, current) && Covers(spans, region.At(0), region.At(0x10000)), "a protection does not log the pieces it re-creates");
    Require(!Covers(spans, region.At(0x10000), region.At(0x10001)), "a protection logs memory outside its ranges");
    const auto protectedAt = current;
    {
        GuestAllocations::Mutation mutation;
        mutation.RequireUnpinned(region.base + 0x9000, 0x1000);
    }
    Require(Since(protectedAt, spans, current) && current == protectedAt + 1 && Covers(spans, region.At(0x8000), region.At(0x10000)), "a renewed backing is not logged over its whole range");
    const auto renewed = current;
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(region.base);
    }
    Require(Since(renewed, spans, current) && Covers(spans, region.At(0), region.At(0x10000)), "a removed allocation is not logged");
    const auto removed = current;
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(region.At(0x20000), 0x1000);
    Require(Since(removed, spans, current) && current == removed + 1 && Covers(spans, region.At(0x20000), region.At(0x21000)), "an invalidation is not logged");
    Require(Since(start, spans, current) && Covers(spans, region.At(0), region.At(0x10000)) && Covers(spans, region.At(0x20000), region.At(0x21000)), "older changes are not reported with newer ones");
}

void Overflow(const Region& region) {
    Spans spans;
    std::uint64_t current = 0;
    const auto old = Generation();
    for (std::size_t index = 0; index < 20000; ++index) GuestAllocations::GuestAllocationsInvalidate_nid_postfix(region.At(0x30000), 0x10);
    Require(Answer(old, spans, current) == GuestAllocations::ChangesAnswer::Dropped, "a generation whose changes were dropped is not refused as dropped");
    const auto recent = Generation();
    GuestAllocations::GuestAllocationsInvalidate_nid_postfix(region.At(0x31000), 0x10);
    Require(Since(recent, spans, current) && spans.size() == 1 && Covers(spans, region.At(0x31000), region.At(0x31010)), "a recent generation is refused after an overflow");
}

void Retiring(const Region& region) {
    Spans spans;
    std::uint64_t current = 0;
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(region.base, 0x10000, true, true, false);
    }
    const auto before = Generation();
    auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
    Require(std::any_of(lease.begin(), lease.end(), [&](const auto& range) { return range->address == region.At(0); }), "the leased registry misses the range");
    std::atomic<bool> done{false};
    std::thread remover([&] {
        GuestAllocations::Mutation mutation;
        mutation.Remove(region.base);
        done.store(true);
    });
    bool waiting = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!waiting && std::chrono::steady_clock::now() < deadline) {
        waiting = Answer(before, spans, current) == GuestAllocations::ChangesAnswer::Waiting;
        if (!waiting) std::this_thread::yield();
    }
    Require(waiting, "changes are answered as complete while a range waits for its lease");
    Require(current == before, "a mutation waiting for its lease moved the generation");
    Require(Covers(spans, region.At(0), region.At(0x10000)), "the range waiting for its lease is not reported as changed");
    Require(!Covers(spans, region.At(0x10000), region.At(0x10001)), "a wait reports memory outside the range waiting for its lease");
    Require(!done.load(), "a leased range was removed");
    lease.clear();
    remover.join();
    Require(Since(before, spans, current) && Covers(spans, region.At(0), region.At(0x10000)), "the removal after the lease is not logged");
}

void WaitingAfterChanges(const Region& region) {
    Spans spans;
    std::uint64_t current = 0;
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(region.base, 0x10000, true, true, false);
    }
    const auto before = Generation();
    auto lease = GuestAllocations::GuestAllocationsAcquire_nid_postfix();
    std::atomic<bool> done{false};
    std::thread mutator([&] {
        GuestAllocations::Mutation mutation;
        mutation.Add(region.base + 0x20000, 0x10000, true, true, false);
        mutation.Remove(region.base);
        done.store(true);
    });
    bool waiting = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!waiting && std::chrono::steady_clock::now() < deadline) {
        waiting = Answer(before, spans, current) == GuestAllocations::ChangesAnswer::Waiting;
        if (!waiting) std::this_thread::yield();
    }
    Require(waiting, "changes are answered as complete while a mutation that already changed a range waits for a lease");
    Require(Covers(spans, region.At(0x20000), region.At(0x30000)), "a range added before the mutation waited for a lease is not reported while it waits");
    Require(Covers(spans, region.At(0), region.At(0x10000)), "the range a mutation waits on is not reported while it waits");
    Require(!Covers(spans, region.At(0x10000), region.At(0x10001)), "a waiting mutation reports memory it did not change");
    Require(!done.load(), "a leased range was removed");
    lease.clear();
    mutator.join();
    Require(Since(before, spans, current) && Covers(spans, region.At(0), region.At(0x10000)) && Covers(spans, region.At(0x20000), region.At(0x30000)), "the mutation that waited for a lease is not logged");
    {
        GuestAllocations::Mutation mutation;
        mutation.Remove(region.base + 0x20000);
    }
}

void Untracked() {
    Spans spans;
    std::uint64_t current = 0;
    const auto before = Generation();
    {
        GuestAllocations::Mutation mutation;
        mutation.RegisterMainImage();
    }
    Require(Answer(before, spans, current) == GuestAllocations::ChangesAnswer::Untracked, "changes are answered across an image registration");
    const auto after = Generation();
    Require(Since(after, spans, current) && spans.empty(), "generations after an image registration are refused");
    {
        GuestAllocations::Mutation mutation;
        mutation.RegisterMainImage();
    }
    Require(Since(after, spans, current) && spans.empty() && current == after + 1, "registering the registered main image again is not answered without changes");
}

}

int main() {
    const Region region;
    Logged(region);
    Overflow(region);
    Retiring(region);
    WaitingAfterChanges(region);
    Untracked();
    return 0;
}
