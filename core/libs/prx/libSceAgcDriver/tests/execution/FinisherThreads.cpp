#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace AgcDriver::DriverDetail;

namespace {

void Require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

DrawFinisher::Pending Known(std::vector<DrawFinisher::Range> writes = {}, std::uint64_t key = 0, bool inserts = false) {
    DrawFinisher::Pending held;
    held.unknown = false;
    held.writes = std::move(writes);
    held.key = key;
    held.inserts = inserts;
    return held;
}

void spin(std::chrono::microseconds length) {
    const auto until = std::chrono::steady_clock::now() + length;
    while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
}

void orderedHandOff() {
    auto& finisher = DrawFinisher::Get();
    std::mutex mutex;
    std::vector<int> order;
    std::atomic<int> running{0}, widest{0};
    std::mt19937 random(11);
    constexpr int Jobs = 3000;
    for (int job = 0; job < Jobs; ++job) {
        const auto delay = std::chrono::microseconds(random() % 40);
        if (job % 5 == 4) {
            finisher.Enqueue([&, job] {
                std::lock_guard lock(mutex);
                order.push_back(job);
            }, Known());
            continue;
        }
        finisher.EnqueuePrepared([&, job, delay] {
            const auto now = running.fetch_add(1, std::memory_order_relaxed) + 1;
            auto seen = widest.load(std::memory_order_relaxed);
            while (now > seen && !widest.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {}
            spin(delay);
            running.fetch_sub(1, std::memory_order_relaxed);
            const auto ordered = [&, job] {
                std::lock_guard lock(mutex);
                order.push_back(job);
            };
            if (DrawFinisher::DefersOrdered()) DrawFinisher::DeferOrdered(ordered);
            else ordered();
        }, Known());
    }
    finisher.Drain();
    Require(order.size() == Jobs, "every job handed its item off");
    for (int job = 0; job < Jobs; ++job) Require(order[job] == job, "the items left out of order at " + std::to_string(job));
    if (DrawFinisher::Threads() > 1) Require(widest.load() >= 2, "the per-draw work never ran on two threads");
    else Require(widest.load() == 1, "one thread ran two jobs at once");
}

void capturesWaitForEarlierWrites() {
    auto& finisher = DrawFinisher::Get();
    if (DrawFinisher::Threads() < 2) return;
    for (int round = 0; round < 50; ++round) {
        std::atomic<bool> release{false}, firstOrdered{false}, besideDone{false}, sawFirst{false};
        finisher.EnqueuePrepared([&] {
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
            DrawFinisher::DeferOrdered([&] { firstOrdered.store(true, std::memory_order_release); });
        }, Known({{0x1000, 0x2000}}));
        finisher.EnqueuePrepared([&] {
            Require(DrawFinisher::EarlierWrites(0x1800, 4), "an earlier job's writes are not seen");
            Require(!DrawFinisher::EarlierWrites(0x4000, 4), "memory no earlier job writes is seen as written");
            besideDone.store(true, std::memory_order_release);
            release.store(true, std::memory_order_release);
            DrawFinisher::WaitEarlierWrites(0x1800, 4);
            sawFirst.store(firstOrdered.load(std::memory_order_acquire), std::memory_order_release);
        }, Known());
        finisher.Drain();
        Require(besideDone.load(), "the second job did not run beside the first");
        Require(sawFirst.load(), "a capture over an earlier job's writes ran before that job's item left");
    }
}

void unknownWritesWait() {
    auto& finisher = DrawFinisher::Get();
    if (DrawFinisher::Threads() < 2) return;
    std::atomic<bool> release{false}, firstOrdered{false}, sawFirst{false};
    DrawFinisher::Pending unknown;
    finisher.EnqueuePrepared([&] {
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        DrawFinisher::DeferOrdered([&] { firstOrdered.store(true, std::memory_order_release); });
    }, std::move(unknown));
    finisher.EnqueuePrepared([&] {
        Require(DrawFinisher::EarlierWrites(0x9000, 4), "an earlier job with unknown writes overlaps everything");
        release.store(true, std::memory_order_release);
        DrawFinisher::WaitTurn();
        sawFirst.store(firstOrdered.load(std::memory_order_acquire), std::memory_order_release);
    }, Known());
    finisher.Drain();
    Require(sawFirst.load(), "its turn came before the earlier item left");
}

void keysWaitForTheOrderedPart() {
    auto& finisher = DrawFinisher::Get();
    for (int round = 0; round < 200; ++round) {
        std::atomic<bool> ordered{false};
        finisher.EnqueuePrepared([&] {
            spin(std::chrono::microseconds(round % 30));
            const auto mark = [&] { ordered.store(true, std::memory_order_release); };
            if (DrawFinisher::DefersOrdered()) DrawFinisher::DeferOrdered(mark);
            else mark();
        }, Known({}, 77, true));
        finisher.EnqueuePrepared([] { spin(std::chrono::microseconds(5)); }, Known());
        static_cast<void>(finisher.WaitPendingKey(77, 0));
        Require(ordered.load(std::memory_order_acquire), "a key's wait ended before the job's ordered part ran");
        finisher.Drain();
    }
}

void failuresStopTheQueue() {
    auto& finisher = DrawFinisher::Get();
    std::atomic<int> after{0};
    finisher.EnqueuePrepared([] { throw std::logic_error("finisher job failed"); }, Known());
    bool thrown = false;
    try {
        finisher.Drain();
    } catch (const std::logic_error&) {
        thrown = true;
    }
    Require(thrown, "a job's failure reaches the drain");
    finisher.EnqueuePrepared([&] { after.fetch_add(1); }, Known());
    finisher.Drain();
    Require(after.load() == 1, "jobs after a consumed failure run");
}

}

int main(int argc, char**) {
    try {
        orderedHandOff();
        capturesWaitForEarlierWrites();
        unknownWritesWait();
        keysWaitForTheOrderedPart();
        if (argc > 1) failuresStopTheQueue();
        std::cout << "finisher threads ok: " << DrawFinisher::Threads() << " threads\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
