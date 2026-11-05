#include "prx/libSceAgcDriver/Graphics/include/PipelineCompilePool.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libc/include/general/ThreadName.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::array<const char*, static_cast<std::size_t>(CompileWait::Count)> WaitNames{"guest_writes", "occlusion", "linear_target", "not_recorded", "dispatch"};
constexpr std::array<const char*, static_cast<std::size_t>(DirectCompile::Count)> DirectNames{"private_pipeline", "failed_build", "tessellation_capture", "pool_closed", "hot_thread_module"};
constexpr auto ReportEvery = std::chrono::seconds(5);

thread_local bool compilePoolThread = false;

std::atomic<int> testOverride{-1};

struct Entry {
    VkDevice device = VK_NULL_HANDLE;
    std::uint64_t hash = 0;
    std::function<void()> job;
    std::uint64_t waiting = 0;
    bool urgent = false;
    bool running = false;
    bool finished = false;
    bool succeeded = false;
    Clock::time_point queuedAt;
    std::exception_ptr failure;
};

struct Counters {
    std::atomic<std::uint64_t> droppedDraws{0};
    std::atomic<std::uint64_t> droppedUnqueued{0};
    std::atomic<std::uint64_t> built{0};
    std::atomic<std::uint64_t> failed{0};
    std::atomic<std::uint64_t> longestQueueMs{0};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(CompileWait::Count)> waits{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(CompileWait::Count)> waitedMs{};
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DirectCompile::Count)> direct{};
};

Counters& TotalCounters() {
    static auto* counters = new Counters();
    return *counters;
}

void noteLongest(std::atomic<std::uint64_t>& longest, std::uint64_t value) {
    auto seen = longest.load(std::memory_order_relaxed);
    while (seen < value && !longest.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
}

std::uint64_t MillisecondsSince(Clock::time_point start, Clock::time_point now) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
}

std::size_t ConfiguredQueueBound() {
    const char* text = std::getenv("APS5_ASYNC_COMPILE_QUEUE");
    const auto value = text != nullptr ? std::strtoul(text, nullptr, 10) : 0ul;
    return value != 0ul ? static_cast<std::size_t>(value) : std::size_t{512};
}

unsigned ConfiguredThreads() {
    const char* text = std::getenv("APS5_ASYNC_COMPILE_THREADS");
    const auto value = text != nullptr ? std::strtoul(text, nullptr, 10) : 0ul;
    return value != 0ul ? static_cast<unsigned>(std::min(value, 16ul)) : CpuTopology::BackgroundCompileThreads();
}

class Pool {
public:
    CompileRequest Request(VkDevice device, std::uint64_t hash, const std::function<std::function<void()>()>& makeJob, bool urgent) {
        const std::pair key{device, hash};
        {
            std::unique_lock lock(mutex);
            if (closed) return CompileRequest::Closed;
            if (failedBuilds.contains(key)) return CompileRequest::Failed;
            if (const auto found = entries.find(key); found != entries.end()) {
                ++found->second->waiting;
                if (urgent && !found->second->urgent) found->second->urgent = true;
                return CompileRequest::Queued;
            }
            if (!urgent && queuedCount >= AsyncCompileQueueBound()) return CompileRequest::QueueFull;
        }
        auto job = makeJob();
        std::unique_lock lock(mutex);
        if (closed) return CompileRequest::Closed;
        if (const auto found = entries.find(key); found != entries.end()) {
            ++found->second->waiting;
            if (urgent) found->second->urgent = true;
            return CompileRequest::Queued;
        }
        auto entry = std::make_shared<Entry>();
        entry->device = device;
        entry->hash = hash;
        entry->job = std::move(job);
        entry->waiting = 1;
        entry->urgent = urgent;
        entry->queuedAt = Clock::now();
        entries.emplace(key, entry);
        ++queuedCount;
        startWorkers();
        lock.unlock();
        ready.notify_one();
        return CompileRequest::Queued;
    }

    bool Await(VkDevice device, std::uint64_t hash) {
        const std::pair key{device, hash};
        std::unique_lock lock(mutex);
        const auto found = entries.find(key);
        if (found == entries.end()) return !failedBuilds.contains(key);
        auto entry = found->second;
        entry->urgent = true;
        ready.notify_one();
        finished.wait(lock, [&] { return entry->finished; });
        return entry->succeeded;
    }

    void Run(const std::function<void()>& job) {
        std::unique_lock lock(mutex);
        if (closed || heldForTest) {
            lock.unlock();
            job();
            return;
        }
        auto entry = std::make_shared<Entry>();
        entry->hash = ++externalSerial;
        entry->job = job;
        entry->waiting = 1;
        entry->urgent = true;
        entry->queuedAt = Clock::now();
        entries.emplace(std::pair{VkDevice{VK_NULL_HANDLE}, entry->hash}, entry);
        ++queuedCount;
        startWorkers();
        ready.notify_one();
        finished.wait(lock, [&] { return entry->finished; });
        if (entry->failure != nullptr) std::rethrow_exception(entry->failure);
        if (!entry->succeeded) {
            lock.unlock();
            job();
        }
    }

    void Drain(VkDevice device) {
        std::unique_lock lock(mutex);
        for (auto it = entries.begin(); it != entries.end();) {
            auto& entry = it->second;
            if (it->first.first != device || entry->running) {
                ++it;
                continue;
            }
            entry->finished = true;
            --queuedCount;
            it = entries.erase(it);
        }
        for (auto it = failedBuilds.begin(); it != failedBuilds.end();) it = it->first == device ? failedBuilds.erase(it) : std::next(it);
        finished.notify_all();
        finished.wait(lock, [&] { return std::none_of(entries.begin(), entries.end(), [&](const auto& item) { return item.first.first == device; }); });
    }

    void Hold(bool held) {
        {
            std::lock_guard lock(mutex);
            heldForTest = held;
        }
        ready.notify_all();
    }

    void Stop() {
        std::vector<std::thread> joined;
        {
            std::unique_lock lock(mutex);
            closed = true;
            heldForTest = false;
            for (auto it = entries.begin(); it != entries.end();) {
                if (it->second->running) {
                    ++it;
                    continue;
                }
                it->second->finished = true;
                --queuedCount;
                it = entries.erase(it);
            }
            finished.notify_all();
            joined.swap(workers);
        }
        ready.notify_all();
        for (auto& worker : joined) {
            if (worker.get_id() == std::this_thread::get_id()) worker.detach();
            else worker.join();
        }
    }

    unsigned Workers() const {
        return liveWorkers.load(std::memory_order_acquire);
    }

    std::pair<std::uint64_t, std::uint64_t> Occupancy() {
        std::lock_guard lock(mutex);
        return {queuedCount, entries.size() - queuedCount};
    }

private:
    void startWorkers() {
        if (!workers.empty()) return;
        for (unsigned index = 0; index < ConfiguredThreads(); ++index) {
            liveWorkers.fetch_add(1, std::memory_order_acq_rel);
            workers.emplace_back([this] { run(); });
        }
    }

    std::shared_ptr<Entry> pick() {
        std::shared_ptr<Entry> best;
        for (const auto& [key, entry] : entries) {
            if (entry->running) continue;
            if (best == nullptr || std::tuple(entry->urgent, entry->waiting, best->queuedAt) > std::tuple(best->urgent, best->waiting, entry->queuedAt)) best = entry;
        }
        return best;
    }

    void report(std::unique_lock<std::mutex>& lock) {
        const auto now = Clock::now();
        if (now - lastReport < ReportEvery) return;
        lastReport = now;
        auto& counters = TotalCounters();
        const auto dropped = counters.droppedDraws.load(std::memory_order_relaxed);
        const auto unqueued = counters.droppedUnqueued.load(std::memory_order_relaxed);
        auto longest = windowLongestMs;
        windowLongestMs = 0;
        for (const auto& [key, entry] : entries) longest = std::max(longest, MillisecondsSince(entry->queuedAt, now));
        std::array<std::uint64_t, static_cast<std::size_t>(CompileWait::Count)> waits{};
        std::array<std::uint64_t, static_cast<std::size_t>(CompileWait::Count)> waited{};
        std::array<std::uint64_t, static_cast<std::size_t>(DirectCompile::Count)> direct{};
        bool waitsChanged = false;
        for (std::size_t index = 0; index < waits.size(); ++index) {
            waits[index] = counters.waits[index].load(std::memory_order_relaxed);
            waited[index] = counters.waitedMs[index].load(std::memory_order_relaxed);
            waitsChanged = waitsChanged || waits[index] != reportedWaits[index];
        }
        for (std::size_t index = 0; index < direct.size(); ++index) {
            direct[index] = counters.direct[index].load(std::memory_order_relaxed);
            waitsChanged = waitsChanged || direct[index] != reportedDirect[index];
        }
        const auto droppedWindow = dropped - reportedDropped;
        const auto unqueuedWindow = unqueued - reportedUnqueued;
        if (droppedWindow == 0 && !waitsChanged) return;
        std::string line = "[async-compile] dropped " + std::to_string(droppedWindow) + " draws waiting for a shader (" + std::to_string(queuedCount) + " compiles queued, longest wait " + std::to_string(longest) + " ms";
        if (unqueuedWindow != 0) line += ", " + std::to_string(unqueuedWindow) + " found the queue full";
        line += ")";
        for (std::size_t index = 0; index < waits.size(); ++index) {
            if (waits[index] == reportedWaits[index]) continue;
            line += std::string("; waited ") + WaitNames[index] + " " + std::to_string(waits[index] - reportedWaits[index]) + " for " + std::to_string(waited[index] - reportedWaited[index]) + " ms";
        }
        for (std::size_t index = 0; index < direct.size(); ++index) {
            if (direct[index] == reportedDirect[index]) continue;
            line += std::string("; hot thread ") + DirectNames[index] + " " + std::to_string(direct[index] - reportedDirect[index]);
        }
        reportedDropped = dropped;
        reportedUnqueued = unqueued;
        reportedWaits = waits;
        reportedWaited = waited;
        reportedDirect = direct;
        lock.unlock();
        std::fprintf(stderr, "%s\n", line.c_str());
        lock.lock();
    }

    void run() {
        compilePoolThread = true;
        static_cast<void>(ThreadName::SetCurrent("pipeline compile"));
        CpuTopology::PinBackgroundCompileThread("pipeline compile", 0);
        std::unique_lock lock(mutex);
        for (;;) {
            report(lock);
            std::shared_ptr<Entry> entry;
            if (!heldForTest) entry = pick();
            if (entry == nullptr) {
                if (closed) break;
                ready.wait_for(lock, std::chrono::seconds(1));
                continue;
            }
            entry->running = true;
            --queuedCount;
            auto job = std::move(entry->job);
            lock.unlock();
            std::exception_ptr failure;
            try {
                job();
            } catch (...) {
                failure = std::current_exception();
            }
            job = nullptr;
            lock.lock();
            const auto waited = MillisecondsSince(entry->queuedAt, Clock::now());
            windowLongestMs = std::max(windowLongestMs, waited);
            noteLongest(TotalCounters().longestQueueMs, waited);
            entry->failure = failure;
            entry->succeeded = failure == nullptr;
            entry->finished = true;
            if (entry->device != VK_NULL_HANDLE) {
                (failure == nullptr ? TotalCounters().built : TotalCounters().failed).fetch_add(1, std::memory_order_relaxed);
                if (failure != nullptr) {
                    failedBuilds.emplace(entry->device, entry->hash);
                    try {
                        std::rethrow_exception(failure);
                    } catch (const std::exception& error) {
                        std::fprintf(stderr, "[async-compile] a pipeline build failed, the committer compiles it itself to report it: %s\n", error.what());
                    } catch (...) {
                        std::fputs("[async-compile] a pipeline build failed with an unknown exception\n", stderr);
                    }
                }
            }
            entries.erase(std::pair{entry->device, entry->hash});
            finished.notify_all();
        }
        liveWorkers.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable finished;
    std::map<std::pair<VkDevice, std::uint64_t>, std::shared_ptr<Entry>> entries;
    std::set<std::pair<VkDevice, std::uint64_t>> failedBuilds;
    std::vector<std::thread> workers;
    std::atomic<unsigned> liveWorkers{0};
    std::uint64_t queuedCount = 0;
    std::uint64_t externalSerial = 0;
    std::uint64_t windowLongestMs = 0;
    Clock::time_point lastReport = Clock::now();
    std::uint64_t reportedDropped = 0;
    std::uint64_t reportedUnqueued = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(CompileWait::Count)> reportedWaits{};
    std::array<std::uint64_t, static_cast<std::size_t>(CompileWait::Count)> reportedWaited{};
    std::array<std::uint64_t, static_cast<std::size_t>(DirectCompile::Count)> reportedDirect{};
    bool closed = false;
    bool heldForTest = false;
};

Pool& CompilePool() {
    static auto* pool = new Pool();
    return *pool;
}

}

bool AsyncCompileEnabled() {
    static const bool disabled = std::getenv("APS5_NO_ASYNC_COMPILE") != nullptr || std::getenv("APS5_NO_PIPELINE_CACHE") != nullptr;
    const auto forced = testOverride.load(std::memory_order_relaxed);
    return forced >= 0 ? forced != 0 : !disabled;
}

void SetAsyncCompileForTest(bool enabled) {
    testOverride.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool OnCompilePoolThread() {
    return compilePoolThread;
}

bool OnHotCompileThread() {
    return !compilePoolThread && (GuestMemory::CurrentHotThread() != GuestMemory::HotThread::None || GuestMemory::OnResolverThread());
}

CompileRequest RequestPipelineCompile(VkDevice device, std::uint64_t hash, const std::function<std::function<void()>()>& makeJob, bool urgent) {
    return CompilePool().Request(device, hash, makeJob, urgent);
}

bool AwaitPipelineCompile(VkDevice device, std::uint64_t hash) {
    return CompilePool().Await(device, hash);
}

void RunOnCompilePool(CompileWait reason, const std::function<void()>& job) {
    const auto started = Clock::now();
    struct Noted {
        CompileWait reason;
        Clock::time_point started;
        ~Noted() { NoteCompileWait(reason, MillisecondsSince(started, Clock::now())); }
    } noted{reason, started};
    CompilePool().Run(job);
}

void DrainPipelineCompiles(VkDevice device) {
    CompilePool().Drain(device);
}

void HoldPipelineCompilesForTest(bool held) {
    CompilePool().Hold(held);
}

void StopPipelineCompilePool() {
    CompilePool().Stop();
}

unsigned PipelineCompileWorkers() {
    return CompilePool().Workers();
}

void NoteDroppedDraw(bool queued) {
    auto& counters = TotalCounters();
    counters.droppedDraws.fetch_add(1, std::memory_order_relaxed);
    if (!queued) counters.droppedUnqueued.fetch_add(1, std::memory_order_relaxed);
}

void NoteCompileWait(CompileWait reason, std::uint64_t waitedMs) {
    auto& counters = TotalCounters();
    counters.waits[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    counters.waitedMs[static_cast<std::size_t>(reason)].fetch_add(waitedMs, std::memory_order_relaxed);
}

void NoteDirectCompile(DirectCompile reason) {
    TotalCounters().direct[static_cast<std::size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
}

void NoteHotThreadModule() {
    if (OnHotCompileThread()) NoteDirectCompile(DirectCompile::HotThreadModule);
}

AsyncCompileTotals AsyncCompileTotalsNow() {
    auto& counters = TotalCounters();
    AsyncCompileTotals totals;
    totals.droppedDraws = counters.droppedDraws.load(std::memory_order_relaxed);
    totals.droppedUnqueued = counters.droppedUnqueued.load(std::memory_order_relaxed);
    const auto occupancy = CompilePool().Occupancy();
    totals.queued = occupancy.first;
    totals.building = occupancy.second;
    totals.built = counters.built.load(std::memory_order_relaxed);
    totals.failed = counters.failed.load(std::memory_order_relaxed);
    totals.longestQueueMs = counters.longestQueueMs.load(std::memory_order_relaxed);
    for (std::size_t index = 0; index < totals.waits.size(); ++index) {
        totals.waits[index] = counters.waits[index].load(std::memory_order_relaxed);
        totals.waitedMs[index] = counters.waitedMs[index].load(std::memory_order_relaxed);
    }
    for (std::size_t index = 0; index < totals.direct.size(); ++index) totals.direct[index] = counters.direct[index].load(std::memory_order_relaxed);
    return totals;
}

std::string AsyncCompileSummaryJson() {
    const auto totals = AsyncCompileTotalsNow();
    std::string json = "{\"enabled\":";
    json += AsyncCompileEnabled() ? "true" : "false";
    json += ",\"dropped_draws\":" + std::to_string(totals.droppedDraws);
    json += ",\"dropped_queue_full\":" + std::to_string(totals.droppedUnqueued);
    json += ",\"queued\":" + std::to_string(totals.queued);
    json += ",\"building\":" + std::to_string(totals.building);
    json += ",\"built\":" + std::to_string(totals.built);
    json += ",\"failed\":" + std::to_string(totals.failed);
    json += ",\"longest_wait_ms\":" + std::to_string(totals.longestQueueMs);
    json += ",\"waits\":{";
    for (std::size_t index = 0; index < totals.waits.size(); ++index) {
        if (index != 0) json += ',';
        json += std::string("\"") + WaitNames[index] + "\":{\"count\":" + std::to_string(totals.waits[index]) + ",\"ms\":" + std::to_string(totals.waitedMs[index]) + "}";
    }
    json += "},\"hot_thread_compiles\":{";
    for (std::size_t index = 0; index < totals.direct.size(); ++index) {
        if (index != 0) json += ',';
        json += std::string("\"") + DirectNames[index] + "\":" + std::to_string(totals.direct[index]);
    }
    json += "}}";
    return json;
}

std::size_t AsyncCompileQueueBound() {
    static const auto bound = ConfiguredQueueBound();
    return bound;
}

}
