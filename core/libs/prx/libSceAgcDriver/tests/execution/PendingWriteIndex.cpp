#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/PendingWriteIndex.hpp"
#include <atomic>
#include <cstdint>
#include <iostream>
#include <map>
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

void countedRanges() {
    CountedRanges ranges;
    const std::vector<CountedRanges::Range> writes{{0x1000, 0x2000}, {0x1000, 0x2000}, {0x4000, 0x4010}};
    ranges.Add(writes);
    Require(ranges.Size() == 2, "equal ranges are counted, not repeated");
    Require(ranges.Overlaps(0x1fff, 0x2001) && !ranges.Overlaps(0x2000, 0x3000), "an overlap is half open");
    ranges.Remove(std::span(writes).first(1));
    Require(ranges.Overlaps(0x1000, 0x1001), "a range stays while another job writes it");
    ranges.Remove(std::span(writes).subspan(1));
    Require(ranges.Size() == 0 && !ranges.Overlaps(0, ~std::uint64_t{0}), "the last removal drops the ranges");
}

void newestSequences() {
    NewestSequenceMap map;
    std::map<std::uint64_t, std::uint64_t> model;
    std::mt19937_64 random(7);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> live;
    std::uint64_t sequence = 0;
    for (int step = 0; step < 200000; ++step) {
        if (live.size() < 256 && (live.empty() || random() % 2 == 0)) {
            const auto key = random() % 64 == 0 ? 0 : (random() % 300) << (random() % 3 * 20);
            ++sequence;
            live.emplace_back(key, sequence);
            model[key] = sequence;
            map.Set(key, sequence);
        } else {
            const auto [key, oldest] = live.front();
            live.erase(live.begin());
            map.Erase(key, oldest);
            if (model[key] == oldest) model.erase(key);
        }
        const auto probe = (random() % 300) << (random() % 3 * 20);
        const auto expected = model.contains(probe) ? model[probe] : 0;
        Require(map.Find(probe) == expected, "the map's newest sequence differs from the model's");
    }
}

void retiredRing() {
    RetiredWrites ring;
    using Answer = RetiredWrites::Answer;
    const std::vector<RetiredWrites::Range> first{{0x1000, 0x2000}}, second{{0x3000, 0x4000}}, none{};
    ring.Note(1, first);
    ring.Note(2, none);
    ring.Note(3, second);
    const std::vector<RetiredWrites::Range> readFirst{{0x1800, 0x1810}}, readSecond{{0x3800, 0x3810}};
    Require(ring.Since(0, readFirst) == Answer::Overlap, "an item retired after the stamp is seen");
    Require(ring.Since(1, readFirst) == Answer::Clear, "an item retired before the stamp is not seen");
    Require(ring.Since(2, readSecond) == Answer::Overlap && ring.Since(3, readSecond) == Answer::Clear, "the stamp bounds the walk");
    for (std::uint64_t sequence = 4; sequence < 2000; ++sequence) ring.Note(sequence, second);
    Require(ring.Since(1, readFirst) == Answer::Overflow, "writes lost to the ring count as written");
    Require(ring.Since(1990, readFirst) == Answer::Clear && ring.Since(1990, readSecond) == Answer::Overlap, "stamps the ring still covers answer exactly");
}

void pendingQueries() {
    auto& finisher = DrawFinisher::Get();
    std::atomic<bool> release{false};
    const auto job = [&release] {
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    };
    const auto held = [](std::uint64_t key, std::uint64_t shape, bool inserts, std::vector<DrawFinisher::Range> writes, bool unknown = false) {
        DrawFinisher::Pending pending;
        pending.key = key;
        pending.shape = shape;
        pending.inserts = inserts;
        pending.unknown = unknown;
        pending.writes = std::move(writes);
        return pending;
    };
    finisher.Enqueue(job, held(11, 21, true, {{0x1000, 0x2000}}));
    finisher.Enqueue([] {}, held(12, 22, false, {{0x3000, 0x4000}}));
    Require(finisher.PendingOverlaps(0x1fff, 0x2000) && finisher.PendingOverlaps(0x3000, 0x3001), "a held job's writes are found");
    Require(!finisher.PendingOverlaps(0x2000, 0x3000), "nothing held writes between the jobs' ranges");
    const std::vector<DrawFinisher::Range> any{{0x2000, 0x2800}, {0x3fff, 0x4100}};
    Require(finisher.PendingOverlapsAny(any), "one range of several is enough");
    Require(finisher.PendingKey(11, 0) && finisher.PendingKey(1, 21), "an inserting job's key and shape are found");
    Require(!finisher.PendingKey(12, 22), "a job that does not insert is no pending key");
    finisher.Enqueue([] {}, held(13, 23, true, {}, true));
    Require(finisher.PendingOverlaps(0x9000, 0x9001), "unknown writes overlap everything");
    release.store(true, std::memory_order_release);
    Require(finisher.WaitPendingKey(13, 0) || !DrawFinisher::Busy(), "the key's job is waited for");
    finisher.Drain();
    Require(!DrawFinisher::Busy(), "drained");
    Require(!finisher.PendingOverlaps(0x1000, 0x2000) && !finisher.PendingKey(11, 0), "the index empties with the queue");
}

void watches() {
    auto& pipeline = DrawPipeline::Queue0();
    std::atomic<bool> release{false};
    const std::vector<DrawPipeline::Range> writesA{{0x10000, 0x11000}}, writesB{{0x20000, 0x21000}};
    pipeline.Enqueue([&release] {
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    }, writesA);
    pipeline.Enqueue([] {}, writesB);
    const auto watch = pipeline.WatchWrites();
    const std::vector<DrawPipeline::Range> readA{{0x10800, 0x10810}}, readB{{0x20800, 0x20810}}, readNone{{0x30000, 0x30010}};
    Require(pipeline.WrittenSince(watch, readA) && pipeline.WrittenSince(watch, readB), "queued writes count");
    Require(!pipeline.WrittenSince(watch, readNone), "other memory is not written");
    release.store(true, std::memory_order_release);
    pipeline.Drain(DrawPipeline::DrainReason::Flush);
    Require(pipeline.WrittenSince(watch, readA) && pipeline.WrittenSince(watch, readB), "writes retired after the watch count");
    Require(!pipeline.WrittenSince(watch, readNone), "other memory stays unwritten after the items retired");
    pipeline.Enqueue([] {}, writesA);
    pipeline.Enqueue([] {}, writesA);
    pipeline.Enqueue([] {}, writesA);
    pipeline.Drain(DrawPipeline::DrainReason::Flush);
    Require(pipeline.WrittenSince(watch, readNone), "more items retired than the watch saw counts as written");
    const auto fresh = pipeline.WatchWrites();
    Require(!pipeline.WrittenSince(fresh, readA), "a watch after the items retired sees nothing");
}

}

int main() {
    try {
        countedRanges();
        newestSequences();
        retiredRing();
        pendingQueries();
        watches();
        DrawPipeline::Queue0().Stop();
        std::cout << "pending write index ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
