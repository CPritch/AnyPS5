#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t Block = 65536;
constexpr std::uint64_t Page = 4096;
constexpr std::uint64_t Base = std::uint64_t{1} << 32;
constexpr std::chrono::seconds Patience{10};

struct FakeWatch {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> written;
    bool holdNextWalk = false;
    bool holding = false;
    bool released = false;
    bool timedOut = false;
};

FakeWatch& Fake() {
    static FakeWatch fake;
    return fake;
}

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

}

namespace GuestWriteWatch {

extern "C" {

bool GuestWriteWatchAvailable_nid_postfix() {
    return true;
}

bool GuestWriteWatchCovers_nid_postfix(std::uintptr_t address, std::size_t bytes) {
    return address >= Base && bytes <= 2 * Block && address - Base <= 2 * Block - bytes;
}

bool GuestWriteWatchCollect_nid_postfix(std::uintptr_t address, std::size_t bytes, void (*written)(void* context, std::uintptr_t begin, std::uintptr_t end), void* context) {
    auto& fake = Fake();
    const auto end = address + bytes;
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> runs;
    {
        std::unique_lock lock(fake.mutex);
        if (fake.holdNextWalk) {
            fake.holdNextWalk = false;
            fake.holding = true;
            fake.changed.notify_all();
            fake.timedOut = !fake.changed.wait_for(lock, Patience, [&] { return fake.released; });
        }
        for (auto it = fake.written.begin(); it != fake.written.end();) {
            if (it->first < end && address < it->second) {
                runs.emplace_back(std::max(it->first, address), std::min(it->second, end));
                it = fake.written.erase(it);
            } else ++it;
        }
    }
    for (const auto& [from, to] : runs) written(context, from, to);
    return true;
}

}

}

namespace {

using namespace AgcDriver::GuestMemory;

void CheckMemoHitDuringWalk() {
    auto& fake = Fake();
    BumpCollectEpoch();
    const auto collected = CollectWrites(Base, Block);
    Require(collected != 0 && UnchangedSince(Base, Block, collected), "the block was not collected");
    {
        const std::lock_guard lock(fake.mutex);
        fake.written.emplace_back(Base + Page, Base + 2 * Page);
        fake.holdNextWalk = true;
    }
    std::thread walker([] { CollectWritesUncached(Base, 2 * Block); });
    bool holding = false;
    {
        std::unique_lock lock(fake.mutex);
        holding = fake.changed.wait_for(lock, Patience, [&] { return fake.holding; });
    }
    const auto memoized = holding ? CollectWrites(Base, Block) : 0;
    {
        const std::lock_guard lock(fake.mutex);
        fake.released = true;
    }
    fake.changed.notify_all();
    walker.join();
    Require(holding, "the second walk did not reach the write watch");
    Require(!fake.timedOut, "the memoized collect waited for the walk in progress");
    Require(!UnchangedSince(Base + Page, Page, memoized), "a memoized collect returned the generation a walk in progress then stamped a write with");
    BumpCollectEpoch();
    Require(CollectWrites(Base, Block) > memoized && !UnchangedSince(Base, Block, memoized), "a later epoch trusts the write stamped by the walk in progress");
}

void CheckRememberedQueries() {
    using namespace AgcDriver::GuestMemory;
    BumpCollectEpoch();
    const auto proof = CollectWrites(Base, Block);
    Require(proof != 0, "the range was not collected");
    const std::array<UnchangedQuery, 1> queries{UnchangedQuery{Base + Page, Page, proof}};
    for (int round = 0; round < 3; ++round) {
        Require(UnchangedSince(Base + Page, Page, proof), "an unchanged range read as changed");
        Require(UnchangedSinceAll(queries), "an unchanged bulk query read as changed");
        Require(!WrittenSinceAny(queries), "an unchanged range read as written");
    }
    MarkWritten(Base + Block + Page, 4);
    Require(UnchangedSince(Base + Page, Page, proof), "a store elsewhere changed the answer");
    MarkWritten(Base + Page, 4);
    for (int round = 0; round < 2; ++round) {
        Require(!UnchangedSince(Base + Page, Page, proof), "a remembered answer hid a stamp over the range");
        Require(!UnchangedSinceAll(queries), "a remembered bulk answer hid a stamp over the range");
        Require(WrittenSinceAny(queries), "a remembered written answer hid a stamp over the range");
    }
    Require(!UnchangedSince(Base + Page, Page, 0), "generation zero proved a range");
    Require(!UnchangedSince(Base + 3 * Block, Page, proof), "an uncovered range was proved");
    const auto later = CollectWrites(Base + Page, Page);
    Require(UnchangedSince(Base + Page, Page, later) && UnchangedSince(Base + Page, Page, later), "a fresh generation did not prove its range");
    BumpCollectEpoch();
    MarkWritten(Base + Page, 4);
    Require(!UnchangedSince(Base + Page, Page, later), "a later epoch's stamp was hidden");
}

}

int main() {
    try {
        CheckMemoHitDuringWalk();
        CheckRememberedQueries();
    } catch (const std::exception& error) {
        std::cerr << "collect memo generation test failed: " << error.what() << "\n";
        return 1;
    }
    std::cout << "collect memo generation test passed\n";
    return 0;
}
