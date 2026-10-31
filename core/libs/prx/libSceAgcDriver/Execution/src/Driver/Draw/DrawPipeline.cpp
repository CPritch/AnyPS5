#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/HandoffSpin.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTrace.hpp"
#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/WorkerSampler.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace AgcDriver::DriverDetail {

namespace {

constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::DrainReason::Count)> DrainNames{"packet", "flush", "labels", "capture", "indirect", "submission"};

std::atomic<unsigned> runningCommitters{0};

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
}

std::atomic<std::uint64_t> heldReadRanges{0};
std::atomic<std::uint64_t> heldReadQueries{0};
std::atomic<std::uint64_t> heldReadHits{0};

}

namespace {
thread_local bool finisherThread = false;
thread_local bool finisherProducer = false;
std::size_t RoomBatch(std::size_t depth);

constexpr std::size_t QueueWaitSites = static_cast<std::size_t>(QueueWaits::Site::Count);
constexpr std::array<const char*, QueueWaitSites> QueueWaitNames{"relocation", "lookup", "reads", "kept draw", "enqueue", "packet", "pipeline drain", "end of submission", "settle", "special dispatch", "targeted", "other", "room"};
std::array<std::atomic<std::uint64_t>, QueueWaitSites> queueWaitCounts{}, queueWaitNs{};
std::atomic<std::uint64_t> shadowCaptures{0}, shadowOverlaps{0};
std::atomic<std::uint64_t> heldWaitLabels{0}, heldWaitUnknown{0}, heldWaitOther{0};
std::atomic<std::uint64_t> commitSequence{0};
thread_local const std::vector<DrawPipeline::Range>* committingWrites = nullptr;
std::atomic<std::uint64_t> queuedLabelReads{0};

std::string QueueWaitText(QueueWaits::Site site) {
    const auto index = static_cast<std::size_t>(site);
    char text[64];
    std::snprintf(text, sizeof(text), "%llu %.1f ms", static_cast<unsigned long long>(queueWaitCounts[index].exchange(0, std::memory_order_relaxed)), static_cast<double>(queueWaitNs[index].exchange(0, std::memory_order_relaxed)) / 1e6);
    return text;
}

std::string QueueWaitMs(QueueWaits::Site site) {
    const auto index = static_cast<std::size_t>(site);
    queueWaitCounts[index].store(0, std::memory_order_relaxed);
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f ms", static_cast<double>(queueWaitNs[index].exchange(0, std::memory_order_relaxed)) / 1e6);
    return text;
}

}

bool QueueWaits::Enabled() {
    static const bool enabled = std::getenv("APS5_PIPELINE_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    return enabled;
}

void QueueWaits::Note(Site site, std::chrono::steady_clock::time_point since, std::chrono::steady_clock::time_point until) {
    const auto index = static_cast<std::size_t>(site);
    queueWaitCounts[index].fetch_add(1, std::memory_order_relaxed);
    queueWaitNs[index].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(until - since).count()), std::memory_order_relaxed);
}

void QueueWaits::NoteQueuedLabelRead() {
    queuedLabelReads.fetch_add(1, std::memory_order_relaxed);
}

void QueueWaits::NoteShadowCapture(bool overlapped) {
    shadowCaptures.fetch_add(1, std::memory_order_relaxed);
    if (overlapped) shadowOverlaps.fetch_add(1, std::memory_order_relaxed);
}

std::string QueueWaits::Text() {
    using enum Site;
    std::string text = "worker drains of the finisher (count, waited):";
    for (const auto site : {FinisherRelocation, FinisherLookup, FinisherReads, FinisherKept, FinisherEnqueue, FinisherPacket, FinisherPipelineDrain, FinisherEnd, FinisherSettle, FinisherDispatch, FinisherTargeted, FinisherOther}) {
        text += " ";
        text += QueueWaitNames[static_cast<std::size_t>(site)];
        text += " " + QueueWaitText(site);
        text += site == FinisherOther ? "" : ",";
    }
    text += "; room waits: worker for the finisher " + QueueWaitText(FinisherRoom) + ", worker for the committer " + QueueWaitText(PipelineRoomWorker) + ", finisher for the committer " + QueueWaitText(PipelineRoomFinisher);
    text += "; finisher drains of the pipeline " + QueueWaitText(PipelineDrainFinisher);
    text += "; idle: finisher spinning " + QueueWaitMs(FinisherIdleSpin) + ", blocked " + QueueWaitMs(FinisherIdleBlocked) + "; committer spinning " + QueueWaitMs(CommitterIdleSpin) + ", blocked " + QueueWaitMs(CommitterIdleBlocked) + ", of that behind a drain or targeted wait " + QueueWaitText(CommitterIdleBehindWait) + "; committer retiring items " + QueueWaitMs(CommitterRetire);
    text += "; proof breaks:" + GuestMemory::TakeProofBreakText();
    text += "; CPU-access GPU waits of the hot threads (largest):" + Graphics::Recorder::TakeHotHookSyncText();
    text += "; GPU waits of the hot threads by source and caller (largest):" + Graphics::Recorder::TakeHotWaitSiteText();
    text += "; GPU lock waits (count, waited):" + GuestMemory::TakeHotLockWaitText();
    const auto [importedLayers, uploadedLayers] = Graphics::TakeDepthPlaneLayerCounts();
    char planes[128];
    std::snprintf(planes, sizeof(planes), "; depth plane copy layers detiled from their host import %llu, read on the CPU %llu", static_cast<unsigned long long>(importedLayers), static_cast<unsigned long long>(uploadedLayers));
    text += planes;
    char held[128];
    std::snprintf(held, sizeof(held), "; targeted finisher waits for a held label %llu, a held job with unknown writes %llu, another held job %llu", static_cast<unsigned long long>(heldWaitLabels.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(heldWaitUnknown.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(heldWaitOther.exchange(0, std::memory_order_relaxed)));
    text += held;
    char labelReads[96];
    std::snprintf(labelReads, sizeof(labelReads), "; capture words read from queued labels %llu", static_cast<unsigned long long>(queuedLabelReads.exchange(0, std::memory_order_relaxed)));
    text += labelReads;
    char shadow[160];
    std::snprintf(shadow, sizeof(shadow), "; finisher capture queries %llu, over writes still queued in the pipeline %llu (waited for unless APS5_NO_FINISHER_CAPTURE_ORDER)", static_cast<unsigned long long>(shadowCaptures.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(shadowOverlaps.exchange(0, std::memory_order_relaxed)));
    text += shadow;
    return text;
}

DrawFinisher& DrawFinisher::Get() {
    static DrawFinisher* finisher = new DrawFinisher();
    return *finisher;
}

bool DrawFinisher::Enabled() {
    return Graphics::DrawPipelineEnabled();
}

bool DrawPipeline::MetadataPassesPipelined() {
    return Graphics::DrawPipelineEnabled();
}

namespace {
std::atomic<std::uint64_t> pipelinedMetadataPasses{0};
}

std::uint64_t DrawPipeline::PipelinedMetadataPasses() {
    return pipelinedMetadataPasses.load(std::memory_order_relaxed);
}

void DrawPipeline::CountPipelinedMetadataPass() {
    pipelinedMetadataPasses.fetch_add(1, std::memory_order_relaxed);
}

bool DrawFinisher::Overlap() {
    return Graphics::DrawPipelineEnabled();
}

bool DrawFinisher::PendingOverlaps(std::uint64_t begin, std::uint64_t end) {
    if (!Busy() || !finisherProducer) return false;
    std::lock_guard lock(mutex);
    for (const auto& held : pending) {
        if (held.unknown) return true;
        for (const auto& [first, last] : held.writes) {
            if (begin < last && first < end) return true;
        }
    }
    return false;
}

DrawFinisher::LabelLookup DrawFinisher::PendingLabel(std::uint64_t address, std::size_t bytes, std::uint64_t& value) {
    if (!Busy() || !finisherProducer) return LabelLookup::NotHeld;
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    for (auto held = pending.rbegin(); held != pending.rend(); ++held) {
        if (held->unknown) return LabelLookup::Unknown;
        const bool overlaps = std::any_of(held->writes.begin(), held->writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (!overlaps) continue;
        if (held->labelBytes.empty() || address < held->labelAddress || end > held->labelAddress + held->labelBytes.size()) return LabelLookup::Unknown;
        value = 0;
        std::memcpy(&value, held->labelBytes.data() + (address - held->labelAddress), bytes);
        return LabelLookup::Value;
    }
    return LabelLookup::NotHeld;
}

bool DrawFinisher::PendingOverlapsAny(std::span<const Range> ranges) {
    if (!Busy() || !finisherProducer) return false;
    std::lock_guard lock(mutex);
    for (const auto& held : pending) {
        if (held.unknown) return true;
        for (const auto& [first, last] : held.writes) {
            for (const auto& [begin, end] : ranges) {
                if (begin < last && first < end) return true;
            }
        }
    }
    return false;
}

bool DrawFinisher::LookupDrainsEvery() {
    static const bool every = std::getenv("APS5_LOOKUP_DRAIN") != nullptr;
    return every;
}

bool DrawFinisher::PendingKey(std::uint64_t key, std::uint64_t shape) {
    if (!Busy() || !finisherProducer) return false;
    const bool every = LookupDrainsEvery();
    std::lock_guard lock(mutex);
    return std::any_of(pending.begin(), pending.end(), [&](const Pending& held) { return (every || held.inserts) && (held.key == key || (shape != 0 && held.shape == shape)); });
}

namespace {
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrawFinisher::DrainSite::Count)> finisherDrains{}, finisherSkips{};
}

void DrawFinisher::NoteDrain(DrainSite site, bool drained) {
    (drained ? finisherDrains : finisherSkips)[static_cast<std::size_t>(site)].fetch_add(1, std::memory_order_relaxed);
}

namespace {
std::array<std::atomic<std::uint64_t>, 257> drainedOpcodes{}, drainedBeside{};
}

void DrawFinisher::NoteDrainedOpcode(std::uint32_t opcode, bool besideCommits) {
    drainedOpcodes[std::min<std::uint32_t>(opcode, 256u)].fetch_add(1, std::memory_order_relaxed);
    if (besideCommits) drainedBeside[std::min<std::uint32_t>(opcode, 256u)].fetch_add(1, std::memory_order_relaxed);
}

std::string DrawFinisher::DrainText() {
    constexpr std::array<const char*, static_cast<std::size_t>(DrainSite::Count)> names{"lookup", "reads", "packet", "kept draw", "item enqueue"};
    std::string text = "; worker drains of the finisher (drained/skipped):";
    for (std::size_t i = 0; i < names.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu/%llu", names[i], static_cast<unsigned long long>(finisherDrains[i].exchange(0)), static_cast<unsigned long long>(finisherSkips[i].exchange(0)));
        text += item;
    }
    text += "; drained by opcode (of them beside commits):";
    for (int shown = 0; shown < 6; ++shown) {
        std::size_t top = 0;
        for (std::size_t i = 1; i < drainedOpcodes.size(); ++i) {
            if (drainedOpcodes[i].load() > drainedOpcodes[top].load()) top = i;
        }
        const auto count = drainedOpcodes[top].exchange(0);
        if (count == 0) break;
        char item[48];
        std::snprintf(item, sizeof(item), " 0x%02zx x%llu (%llu)", top, static_cast<unsigned long long>(count), static_cast<unsigned long long>(drainedBeside[top].exchange(0)));
        text += item;
    }
    return text;
}

bool DrawFinisher::OnFinisherThread() {
    return finisherThread;
}

void DrawFinisher::Enqueue(Job job) {
    Enqueue(std::move(job), Pending{});
}

void DrawFinisher::Enqueue(Job job, Pending held) {
    finisherProducer = true;
    constexpr std::size_t depth = 16;
    struct ReclaimStorage {};
    auto& reclaim = HostThreadLocal<std::vector<Job>, ReclaimStorage>();
    reclaim.clear();
    std::unique_lock lock(mutex);
    if (!ran.empty()) std::swap(ran, reclaim);
    if (failure != nullptr) std::rethrow_exception(std::exchange(failure, nullptr));
    if (jobs.size() >= depth && GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw finisher full while the enqueuing thread holds the GPU lock");
    if (jobs.size() >= depth) {
        PerformanceTimer timing("DrawFinisher.EnqueueWait");
        const auto start = std::chrono::steady_clock::now();
        const auto resume = depth - std::min(RoomBatch(depth) / 2 + 1, depth);
        ++roomWaiters;
        room.wait(lock, [&] { return jobs.size() <= resume; });
        --roomWaiters;
        QueueWaits::Note(QueueWaits::Site::FinisherRoom, start);
    }
    jobs.push_back(std::move(job));
    pending.push_back(std::move(held));
    outstanding.fetch_add(1, std::memory_order_release);
    if (!running) {
        running = true;
        thread = std::thread([this] {
            finisherThread = true;
            run();
        });
    }
    if (sleeping) wake.notify_one();
}

void DrawFinisher::run() {
    PinFinisherThread("draw finisher");
    PerformanceTrace::BindThread("draw finisher");
    GuestMemory::CountGpuLockWaits(GuestMemory::HotThread::Finisher);
    const bool counted = QueueWaits::Enabled();
    for (;;) {
        Job* job = nullptr;
        bool failed = false;
        const auto idleStart = counted ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        SpinUntil(HandoffSpinWindow(), [&] { return outstanding.load(std::memory_order_acquire) != 0; });
        const auto spun = counted ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        {
            std::unique_lock lock(mutex);
            sleeping = true;
            wake.wait(lock, [&] { return !jobs.empty(); });
            sleeping = false;
            job = &jobs.front();
            failed = failure != nullptr;
        }
        if (counted) {
            QueueWaits::Note(QueueWaits::Site::FinisherIdleSpin, idleStart, spun);
            QueueWaits::Note(QueueWaits::Site::FinisherIdleBlocked, spun);
        }
        std::exception_ptr error;
        bool shutdown = false;
        try {
            if (!failed) (*job)();
        } catch (const ProcessShutdown&) {
            error = std::current_exception();
            shutdown = true;
        } catch (...) {
            error = std::current_exception();
        }
        bool report = false;
        {
            std::lock_guard lock(mutex);
            if (error != nullptr && failure == nullptr) {
                failure = error;
                report = !shutdown;
            }
            ran.push_back(std::move(jobs.front()));
            jobs.pop_front();
            pending.pop_front();
            ++ranJobs;
            outstanding.fetch_sub(1, std::memory_order_release);
            constexpr std::size_t depth = 16;
            if (idleWaiters != 0) idle.notify_all();
            if (roomWaiters != 0 && jobs.size() <= depth - std::min(RoomBatch(depth) / 2 + 1, depth)) room.notify_all();
        }
        if (report) Driver::Get().ReportFailure(error);
    }
}

void DrawFinisher::Settle() {
    if (!Busy() || OnFinisherThread()) return;
    if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw finisher settle under the GPU lock");
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock(mutex);
    ++idleWaiters;
    idle.wait(lock, [&] { return jobs.empty(); });
    --idleWaiters;
    QueueWaits::Note(QueueWaits::Site::FinisherSettle, start);
}

void DrawFinisher::Drain(QueueWaits::Site site) {
    if (OnFinisherThread()) return;
    const bool busy = Busy();
    if (busy && GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw finisher drain under the GPU lock");
    std::optional<PerformanceTimer> timing;
    if (busy) timing.emplace("DrawFinisher.Drain");
    const auto start = busy ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    SpinUntil(HandoffSpinWindow() * 2, [&] { return outstanding.load(std::memory_order_acquire) == 0; });
    std::unique_lock lock(mutex);
    if (!jobs.empty()) {
        ++idleWaiters;
        idle.wait(lock, [&] { return jobs.empty(); });
        --idleWaiters;
    }
    if (busy) QueueWaits::Note(site, start);
    if (failure != nullptr) std::rethrow_exception(std::exchange(failure, nullptr));
}

void DrawFinisher::WaitPendingWritten(std::span<const Range> ranges) {
    if (!Busy() || OnFinisherThread()) return;
    const auto overlapping = [&](const Range& write) {
        return std::any_of(ranges.begin(), ranges.end(), [&](const Range& range) { return range.first < write.second && write.first < range.second; });
    };
    std::unique_lock lock(mutex);
    std::uint64_t target = 0;
    for (std::size_t i = pending.size(); i-- > 0;) {
        const auto& held = pending[i];
        if (!held.unknown && std::none_of(held.writes.begin(), held.writes.end(), overlapping)) continue;
        target = ranJobs + i + 1;
        (held.unknown ? heldWaitUnknown : held.labelAddress != 0 ? heldWaitLabels : heldWaitOther).fetch_add(1, std::memory_order_relaxed);
        break;
    }
    if (target == 0) return;
    if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw finisher wait under the GPU lock");
    PerformanceTimer timing("DrawFinisher.WaitPending");
    const auto start = std::chrono::steady_clock::now();
    ++idleWaiters;
    idle.wait(lock, [&] { return ranJobs >= target || failure != nullptr; });
    --idleWaiters;
    QueueWaits::Note(QueueWaits::Site::FinisherTargeted, start);
    if (failure != nullptr) std::rethrow_exception(std::exchange(failure, nullptr));
}

void DrawFinisher::ReleaseRan() {
    std::vector<Job> done;
    std::lock_guard lock(mutex);
    std::swap(done, ran);
}

DrawPipeline& DrawPipeline::Queue0() {
    static DrawPipeline* pipeline = new DrawPipeline();
    return *pipeline;
}

std::size_t DrawPipeline::Depth() {
    static const std::size_t depth = [] {
        const char* text = std::getenv("APS5_PIPELINED_DRAWS");
        if (std::getenv("APS5_LOCKED_DRAW_PREPARE") != nullptr || std::getenv("APS5_DRAW_DRAIN") != nullptr || std::getenv("APS5_DRAIN_ALL") != nullptr || std::getenv("APS5_NO_WORDWISE_CAPTURE") != nullptr) return std::size_t{0};
        if (text == nullptr) return std::size_t{32};
        const auto value = std::strtoull(text, nullptr, 10);
        return value == 0 ? std::size_t{0} : value == 1 ? std::size_t{32} : static_cast<std::size_t>(std::min<unsigned long long>(value, 256));
    }();
    return depth;
}

std::size_t DrawPipeline::QueuedItems() {
    auto& pipeline = Queue0();
    std::lock_guard lock(pipeline.mutex);
    return pipeline.items.size();
}

bool& DrawPipeline::Active() {
    static thread_local bool active = false;
    return active;
}

std::atomic<std::uint64_t>& DrawPipeline::EpochToken() {
    static std::atomic<std::uint64_t> token{1};
    return token;
}

namespace {
thread_local std::uint64_t followedEpoch = 0;
std::atomic<std::uint64_t> earlyResolved{0}, earlyDeclined{0}, earlyFailed{0};
}

unsigned DrawPipeline::ResolveSplit() {
    static const unsigned mode = [] {
        const char* text = std::getenv("APS5_RESOLVE_SPLIT");
        return text == nullptr ? 0u : static_cast<unsigned>(std::strtoul(text, nullptr, 10));
    }();
    return mode;
}

std::uint64_t DrawPipeline::FollowedEpoch() {
    return followedEpoch;
}

void DrawPipeline::FollowEpoch(std::uint64_t token) {
    auto& followed = followedEpoch;
    if (followed == token) return;
    followed = token;
    static const bool shared = std::getenv("APS5_NO_SHARED_COMMIT_EPOCH") == nullptr;
    if (shared && GuestMemory::CurrentHotThread() == GuestMemory::HotThread::Committer) {
        static AgcDriver::Mutex mutex;
        static std::array<std::pair<std::uint64_t, std::uint64_t>, 32> epochs{};
        static std::size_t next = 0;
        std::lock_guard lock(mutex);
        const auto found = std::find_if(epochs.begin(), epochs.end(), [&](const auto& entry) { return entry.first == token; });
        if (found != epochs.end() && found->second != 0) {
            GuestMemory::AdoptCollectEpoch(found->second);
        } else {
            GuestMemory::BumpCollectEpoch();
            epochs[next] = {token, GuestMemory::ThreadCollectEpoch()};
            next = (next + 1) % epochs.size();
        }
    } else {
        GuestMemory::BumpCollectEpoch();
    }
    GuestMemory::FollowCollectToken(token);
}

unsigned DrawPipeline::Committers() {
    return runningCommitters.load(std::memory_order_acquire);
}

const std::vector<std::pair<std::uint64_t, std::uint64_t>>* DrawPipeline::CommittingWrites() {
    return committingWrites;
}

std::uint64_t DrawPipeline::CommitSequence() {
    return commitSequence.load(std::memory_order_relaxed);
}

unsigned DrawPipeline::PrepareThreads() {
    static const unsigned threads = [] {
        const char* text = std::getenv("APS5_PREPARE_AHEAD");
        if (Depth() == 0) return 0u;
        if (text == nullptr) return 1u;
        return static_cast<unsigned>(std::min<unsigned long long>(std::strtoull(text, nullptr, 10), 8));
    }();
    return threads;
}

bool DrawPipeline::PreparesAhead() {
    return PrepareThreads() != 0 && !Graphics::DrawAheadDisabled();
}

void DrawPipeline::queuePrepare(Prepare prepare) {
    struct ReclaimStorage {};
    auto& reclaim = HostThreadLocal<std::vector<Prepare>, ReclaimStorage>();
    reclaim.clear();
    std::lock_guard lock(prepareMutex);
    if (!spentPrepares.empty()) std::swap(spentPrepares, reclaim);
    if (preparersClosing.load(std::memory_order_relaxed)) return;
    prepares.push_back(std::move(prepare));
    queuedPrepares.fetch_add(1, std::memory_order_release);
    while (preparers.size() < PrepareThreads()) preparers.emplace_back([this] { prepareAhead(); });
}

void DrawPipeline::prepareAhead() {
    PinPreparerThread("draw preparer");
    PerformanceTrace::BindThread("draw preparer", std::size_t{1} << 17u);
    constexpr std::chrono::microseconds idleNap{200};
    for (;;) {
        const auto queued = [&] { return queuedPrepares.load(std::memory_order_acquire) != 0 || preparersClosing.load(std::memory_order_relaxed); };
        if (!SpinUntil(HandoffSpinWindow(), queued)) {
            std::this_thread::sleep_for(idleNap);
            continue;
        }
        if (preparersClosing.load(std::memory_order_acquire)) return;
        Prepare prepare;
        {
            std::lock_guard lock(prepareMutex);
            if (prepares.empty()) continue;
            prepare = std::move(prepares.front());
            prepares.pop_front();
            queuedPrepares.fetch_sub(1, std::memory_order_release);
        }
        try {
            prepare();
        } catch (...) {
        }
        std::lock_guard lock(prepareMutex);
        spentPrepares.push_back(std::move(prepare));
    }
}

void DrawPipeline::rethrowFailure() {
    if (failure == nullptr) return;
    auto error = std::exchange(failure, nullptr);
    std::rethrow_exception(error);
}

namespace {

std::size_t RoomBatch(std::size_t depth) {
    static const std::size_t batch = [] {
        const char* text = std::getenv("APS5_ROOM_BATCH");
        return text == nullptr ? std::size_t{8} : static_cast<std::size_t>(std::max<long long>(1, std::strtoll(text, nullptr, 10)));
    }();
    return std::clamp<std::size_t>(batch, 1, std::max<std::size_t>(depth, 1));
}

bool ItemsThroughFinisher() {
    static const bool enabled = [] {
        const char* text = std::getenv("APS5_FINISHER_ITEMS");
        return text == nullptr || text[0] != '0';
    }();
    return enabled;
}

}

void DrawPipeline::Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress, std::vector<std::byte> labelBytes, std::vector<Range> reads, bool draw, ItemKind kind, Prepare prepare, Early early) {
    if (DrawFinisher::Busy() && !DrawFinisher::OnFinisherThread()) {
        const bool forward = ItemsThroughFinisher() && DrawFinisher::Overlap() && !draw && reads.empty();
        DrawFinisher::NoteDrain(DrawFinisher::DrainSite::Enqueue, !forward);
        if (forward) {
            DrawFinisher::Pending held;
            held.unknown = false;
            held.writes = writes;
            if (labelAddress != 0 && !labelBytes.empty()) held.writes.emplace_back(labelAddress, labelAddress + labelBytes.size());
            held.labelAddress = labelAddress;
            held.labelBytes = labelBytes;
            DrawFinisher::Get().Enqueue([this, commit = std::move(commit), writes = std::move(writes), labelAddress, labelBytes = std::move(labelBytes), kind]() mutable {
                Enqueue(std::move(commit), std::move(writes), labelAddress, std::move(labelBytes), {}, false, kind);
            }, std::move(held));
            return;
        }
        DrawFinisher::Get().Drain(QueueWaits::Site::FinisherEnqueue);
    }
    if (prepare) queuePrepare(std::move(prepare));
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    struct ReclaimStorage {};
    auto& reclaim = HostThreadLocal<std::vector<Item>, ReclaimStorage>();
    reclaim.clear();
    const auto lockStart = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::unique_lock lock(mutex);
    if (!retired.empty()) std::swap(retired, reclaim);
    if (profile) lockWaitNs += elapsedNs(lockStart);
    rethrowFailure();
    if (closing) {
        lock.unlock();
        commit();
        return;
    }
    if (items.size() >= Depth()) {
        if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline full while the enqueuing thread holds the GPU lock");
        PerformanceTimer timing("DrawPipeline.EnqueueWait");
        const auto start = std::chrono::steady_clock::now();
        const auto resume = Depth() - RoomBatch(Depth());
        ++roomWaiters;
        room.wait(lock, [&] { return items.size() <= resume || failure != nullptr; });
        --roomWaiters;
        fullWaitNs += elapsedNs(start);
        QueueWaits::Note(DrawFinisher::OnFinisherThread() ? QueueWaits::Site::PipelineRoomFinisher : QueueWaits::Site::PipelineRoomWorker, start);
        rethrowFailure();
    }
    depthSum += items.size();
    ++enqueued;
    heldReadRanges.fetch_add(reads.size(), std::memory_order_relaxed);
    countWrites(writes, true);
    items.push_back({std::move(commit), std::move(writes), labelAddress, std::move(labelBytes), std::move(reads), draw, draw && kind == ItemKind::Other ? ItemKind::Draw : kind, std::move(early)});
    outstanding.fetch_add(1, std::memory_order_release);
    if (!thread.joinable()) {
        thread = std::thread([this] {
            GuestMemory::MarkProofThread();
            run();
        });
        runningCommitters.fetch_add(1, std::memory_order_acq_rel);
    }
    if (committerWaiting) {
        ++wakes;
        wake.notify_one();
    }
    static const bool stats = std::getenv("APS5_PIPELINE_STATS") != nullptr;
    if (!profile && !stats) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(10)) report(now);
}

void DrawPipeline::report(std::chrono::steady_clock::time_point now) {
    lastReport = now;
    std::string drainText;
    for (std::size_t i = 0; i < drains.size(); ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu (%.1f ms)", DrainNames[i], static_cast<unsigned long long>(drains[i]), static_cast<double>(drainWaitNs[i]) / 1e6);
        drainText += item;
    }
    drainText += "; packet drains by opcode:";
    for (int shown = 0; shown < 5; ++shown) {
        const auto top = std::max_element(drainOpcodes.begin(), drainOpcodes.begin() + 0x100);
        if (*top == 0) break;
        const auto opcode = static_cast<std::size_t>(top - drainOpcodes.begin());
        char item[48];
        std::snprintf(item, sizeof(item), " 0x%02zx x%llu (%.1f ms)", opcode, static_cast<unsigned long long>(*top), static_cast<double>(drainOpcodeNs[opcode]) / 1e6);
        drainText += item;
        *top = 0;
    }
    drainOpcodes = {};
    drainOpcodeNs = {};
    constexpr std::array<const char*, static_cast<std::size_t>(ItemKind::Count)> kindNames{"other", "draw", "indirect draw", "dispatch", "dispatch miss", "dispatch without recipe", "label", "fill", "copy", "store", "pass", "seal", "completion", "occlusion dump"};
    drainText += "; committer by item kind (items, ms):";
    for (std::size_t kind = 0; kind < kindNames.size(); ++kind) {
        if (kindCommits[kind] == 0) continue;
        char item[64];
        std::snprintf(item, sizeof(item), " %s %llu %.1f", kindNames[kind], static_cast<unsigned long long>(kindCommits[kind]), static_cast<double>(kindCommitNs[kind]) / 1e6);
        drainText += item;
    }
    drainText += "; draws in Graphics:" + Graphics::TakeDrawSplitText();
    drainText += "; committer GPU waits by item kind (ms):";
    for (std::size_t kind = 0; kind < kindNames.size(); ++kind) {
        if (kindGpuWaitNs[kind] == 0) continue;
        char item[64];
        std::snprintf(item, sizeof(item), " %s %.1f", kindNames[kind], static_cast<double>(kindGpuWaitNs[kind]) / 1e6);
        drainText += item;
    }
    kindGpuWaitNs = {};
    kindCommits = {};
    kindCommitNs = {};
    char targetedText[160];
    std::snprintf(targetedText, sizeof(targetedText), "; targeted waits %llu (%.1f ms), %.1f items left queued behind each", static_cast<unsigned long long>(targetedWaits), static_cast<double>(targetedWaitNs) / 1e6, targetedWaits == 0 ? 0.0 : static_cast<double>(targetedBehind) / static_cast<double>(targetedWaits));
    drainText += targetedText;
    drainText += ", waiting for:";
    for (std::size_t kind = 0; kind < kindNames.size(); ++kind) {
        if (targetedKinds[kind] == 0) continue;
        char item[48];
        std::snprintf(item, sizeof(item), " %s %llu", kindNames[kind], static_cast<unsigned long long>(targetedKinds[kind]));
        drainText += item;
    }
    targetedKinds = {};
    targetedWaits = targetedWaitNs = targetedBehind = 0;
    const auto perCommit = commits == 0 ? 0.0 : static_cast<double>(commitNs) / 1e3 / static_cast<double>(commits);
    const auto perDraw = enqueued == 0 ? 0.0 : static_cast<double>(fullWaitNs) / 1e3 / static_cast<double>(enqueued);
    const auto queued = enqueued == 0 ? 0.0 : static_cast<double>(depthSum) / static_cast<double>(enqueued);
    const auto decodeDrainCount = decodeDrains.load(std::memory_order_relaxed);
    char reads[96];
    std::snprintf(reads, sizeof(reads), " %llu read ranges, %llu stores held, %llu decode drains", static_cast<unsigned long long>(heldReadRanges.load(std::memory_order_relaxed)), static_cast<unsigned long long>(heldReadHits.exchange(0, std::memory_order_relaxed)), static_cast<unsigned long long>(decodeDrainCount - reportedDecodeDrains));
    reportedDecodeDrains = decodeDrainCount;
    drainText += reads;
    AgcDriver::ProfilePrint_nid_no_patch("[draw] pipelined (10 s, depth %zu): %llu commits at %.1f us each on the committer, %llu failed; %llu enqueued behind %.2f on average, waiting %.1f us per draw for room, %.2f us for the lock; %llu committer wakes;%s; drains:%s\n", Depth(), static_cast<unsigned long long>(commits), perCommit, static_cast<unsigned long long>(commitErrors), static_cast<unsigned long long>(enqueued), queued, perDraw, enqueued == 0 ? 0.0 : static_cast<double>(lockWaitNs) / 1e3 / static_cast<double>(enqueued), static_cast<unsigned long long>(wakes), reads, drainText.c_str());
    if (PrepareThreads() != 0) AgcDriver::ProfilePrint_nid_no_patch("%s with %u threads\n", Graphics::DrawAheadReport().c_str(), PrepareThreads());
    commits = commitNs = commitErrors = enqueued = fullWaitNs = depthSum = wakes = lockWaitNs = 0;
    drains = {};
    drainWaitNs = {};
    AgcDriver::ProfilePrint_nid_no_patch("[waits] queue 0 (10 s): %s\n", QueueWaits::Text().c_str());
}


void DrawPipeline::run() {
    PinCommitterThread("draw committer");
    PerformanceTrace::BindThread("draw committer");
    StartCommitterSampler();
    GuestMemory::CountGpuLockWaits(GuestMemory::HotThread::Committer);
    const bool counted = QueueWaits::Enabled();
    auto idleStart = std::chrono::steady_clock::now();
    bool idleBehindWait = false;
    for (;;) {
        Commit* commit = nullptr;
        bool isDraw = false;
        std::size_t kind = 0;
        SpinUntil(HandoffSpinWindow(), [&] { return outstanding.load(std::memory_order_acquire) != 0; });
        const auto spun = counted ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        {
            std::unique_lock lock(mutex);
            committerWaiting = true;
            wake.wait(lock, [&] { return !items.empty() || closing; });
            committerWaiting = false;
            if (items.empty()) break;
            commit = stopped ? nullptr : &items.front().commit;
            committingWrites = &items.front().writes;
            isDraw = items.front().draw;
            kind = static_cast<std::size_t>(items.front().kind);
        }
        const auto start = std::chrono::steady_clock::now();
        if (counted) {
            QueueWaits::Note(QueueWaits::Site::CommitterIdleSpin, idleStart, spun);
            QueueWaits::Note(QueueWaits::Site::CommitterIdleBlocked, spun, start);
            if (idleBehindWait) QueueWaits::Note(QueueWaits::Site::CommitterIdleBehindWait, idleStart, start);
        }
        commitSequence.fetch_add(1, std::memory_order_relaxed);
        const double gpuWaitedBefore = counted ? Graphics::Recorder::HotGpuWaitedMs() : 0.0;
        std::exception_ptr fatal;
        bool failed = false;
        bool shutdown = false;
        Early nextEarly;
        const std::vector<Range>* currentWrites = nullptr;
        if (ResolveSplit() == 1 && isDraw && commit != nullptr) {
            std::lock_guard lock(mutex);
            if (items.size() >= 2 && items[1].draw && items[1].early) {
                nextEarly = items[1].early;
                currentWrites = &items.front().writes;
            }
        }
        try {
            if (nextEarly) {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Draw);
                std::lock_guard gpu(GuestMemory::GpuMutex());
                try {
                    (nextEarly(*currentWrites) ? earlyResolved : earlyDeclined).fetch_add(1, std::memory_order_relaxed);
                } catch (const ProcessShutdown&) {
                    throw;
                } catch (...) {
                    earlyFailed.fetch_add(1, std::memory_order_relaxed);
                }
                (*commit)();
            } else if (commit != nullptr) {
                (*commit)();
            }
            if (!isDraw) GuestMemory::BreakProofs(GuestMemory::ProofBreak::Item);
        } catch (const std::exception&) {
            failed = true;
            fatal = std::current_exception();
        } catch (const ProcessShutdown&) {
            shutdown = true;
            fatal = std::current_exception();
        } catch (...) {
            fatal = std::current_exception();
        }
        const auto committed = std::chrono::steady_clock::now();
        const auto spent = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(committed - start).count());
        const auto gpuWaitedNs = counted ? static_cast<std::uint64_t>((Graphics::Recorder::HotGpuWaitedMs() - gpuWaitedBefore) * 1e6) : 0;
        if (fatal != nullptr && !stopped) {
            stopped = true;
            if (!shutdown) Driver::Get().ReportFailure(fatal);
        }
        {
            std::lock_guard lock(mutex);
            countWrites(items.front().writes, false);
            retired.push_back(std::move(items.front()));
            items.pop_front();
            const auto retiredNow = retiredItems.fetch_add(1, std::memory_order_release) + 1;
            ++commits;
            commitNs += spent;
            ++kindCommits[kind];
            kindCommitNs[kind] += spent;
            kindGpuWaitNs[kind] += gpuWaitedNs;
            if (failed) ++commitErrors;
            if (fatal != nullptr && failure == nullptr) failure = fatal;
            outstanding.fetch_sub(1, std::memory_order_release);
            idleBehindWait = items.empty() && (idleWaiters != 0 || targetWaiters != 0);
            if (idleWaiters != 0 && items.empty()) idle.notify_all();
            if (roomWaiters != 0 && (items.size() <= Depth() - RoomBatch(Depth()) || failure != nullptr)) room.notify_all();
            if (targetWaiters != 0 && (retiredNow >= targetWake || failure != nullptr)) {
                targetWake = ~std::uint64_t{0};
                retiredWake.notify_all();
            }
        }
        idleStart = std::chrono::steady_clock::now();
        if (counted) QueueWaits::Note(QueueWaits::Site::CommitterRetire, committed, idleStart);
    }
    runningCommitters.fetch_sub(1, std::memory_order_acq_rel);
}

void DrawPipeline::Stop() {
    std::thread committer;
    std::vector<std::thread> stopped;
    {
        std::lock_guard lock(prepareMutex);
        preparersClosing.store(true, std::memory_order_release);
        stopped = std::move(preparers);
    }
    for (auto& preparer : stopped) {
        if (preparer.get_id() == std::this_thread::get_id()) preparer.detach();
        else preparer.join();
    }
    {
        std::lock_guard lock(mutex);
        closing = true;
        committer = std::move(thread);
    }
    wake.notify_all();
    if (!committer.joinable()) return;
    if (committer.get_id() == std::this_thread::get_id()) committer.detach();
    else committer.join();
}

void DrawPipeline::ReleaseRetired() {
    std::vector<Item> done;
    std::lock_guard lock(mutex);
    std::swap(done, retired);
}

void DrawPipeline::Drain(DrainReason reason, std::uint32_t opcode) {
    drainRequests[std::min<std::uint32_t>(opcode, 0x100)].fetch_add(1, std::memory_order_relaxed);
    const bool finisherBusy = DrawFinisher::Busy() && !DrawFinisher::OnFinisherThread();
    if ((finisherBusy || Busy()) && GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline drain under the GPU lock (reason " + std::to_string(static_cast<unsigned>(reason)) + ", opcode " + std::to_string(opcode) + ")");
    if (finisherBusy) DrawFinisher::Get().Drain(QueueWaits::Site::FinisherPipelineDrain);
    if (!Busy()) return;
    PerformanceTimer timing("DrawPipeline.Drain");
    const auto start = std::chrono::steady_clock::now();
    SpinUntil(HandoffSpinWindow() * 2, [&] { return !Busy(); });
    std::unique_lock lock(mutex);
    ++idleWaiters;
    idle.wait(lock, [&] { return items.empty(); });
    --idleWaiters;
    const auto index = static_cast<std::size_t>(reason);
    const auto waited = elapsedNs(start);
    ++drains[index];
    ++drainOpcodes[std::min<std::uint32_t>(opcode, 0x100)];
    drainOpcodeNs[std::min<std::uint32_t>(opcode, 0x100)] += waited;
    drainWaitNs[index] += waited;
    if (DrawFinisher::OnFinisherThread()) QueueWaits::Note(QueueWaits::Site::PipelineDrainFinisher, start);
    rethrowFailure();
}

void DrawPipeline::WaitWritten(std::uint64_t address, std::size_t bytes, DrainReason reason, std::uint32_t opcode) {
    const Range range{address, address + bytes};
    WaitWrittenAny(std::span(&range, 1), reason, opcode);
}

bool DrawPipeline::TargetedWaits() {
    static const bool targeted = [] {
        const char* text = std::getenv("APS5_TARGETED_WAITS");
        return text == nullptr || text[0] != '0';
    }();
    return targeted;
}

void DrawPipeline::WaitWrittenAny(std::span<const Range> ranges, DrainReason reason, std::uint32_t opcode) {
    const bool finisherBusy = DrawFinisher::Busy() && !DrawFinisher::OnFinisherThread();
    if (!TargetedWaits() || (finisherBusy && !DrawFinisher::Overlap())) {
        Drain(reason, opcode);
        return;
    }
    if (finisherBusy) DrawFinisher::Get().WaitPendingWritten(ranges);
    drainRequests[std::min<std::uint32_t>(opcode, 0x100)].fetch_add(1, std::memory_order_relaxed);
    const auto overlapping = [&](const Range& write) {
        return std::any_of(ranges.begin(), ranges.end(), [&](const Range& range) { return range.first < write.second && write.first < range.second; });
    };
    std::uint64_t target = 0;
    {
        std::lock_guard lock(mutex);
        for (std::size_t i = items.size(); i-- > 0;) {
            const auto& writes = items[i].writes;
            if (std::none_of(writes.begin(), writes.end(), overlapping)) continue;
            target = retiredItems.load(std::memory_order_relaxed) + i + 1;
            targetedBehind += items.size() - i - 1;
            ++targetedKinds[static_cast<std::size_t>(items[i].kind)];
            break;
        }
    }
    if (target == 0) return;
    if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw pipeline wait under the GPU lock (reason " + std::to_string(static_cast<unsigned>(reason)) + ", opcode " + std::to_string(opcode) + ")");
    PerformanceTimer timing("DrawPipeline.WaitWritten");
    const auto start = std::chrono::steady_clock::now();
    SpinUntil(HandoffSpinWindow() * 2, [&] { return retiredItems.load(std::memory_order_acquire) >= target; });
    std::unique_lock lock(mutex);
    ++targetWaiters;
    retiredWake.wait(lock, [&] {
        if (retiredItems.load(std::memory_order_acquire) >= target || failure != nullptr || stopped) return true;
        targetWake = std::min(targetWake, target);
        return false;
    });
    --targetWaiters;
    ++targetedWaits;
    targetedWaitNs += elapsedNs(start);
    if (DrawFinisher::OnFinisherThread()) QueueWaits::Note(QueueWaits::Site::PipelineDrainFinisher, start);
    rethrowFailure();
}

std::uint64_t DrawPipeline::DrainRequestsByOpcode(std::uint32_t opcode) {
    return Queue0().drainRequests[std::min<std::uint32_t>(opcode, 0x100)].load(std::memory_order_relaxed);
}

bool DrawPipeline::HeldReads(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || std::getenv("APS5_NO_QUEUED_DRAW_READ_ORDER") != nullptr) return false;
    const auto end = address + bytes;
    auto& pipeline = Queue0();
    std::lock_guard lock(pipeline.mutex);
    heldReadQueries.fetch_add(1, std::memory_order_relaxed);
    for (const auto& item : pipeline.items) {
        for (const auto& [begin, limit] : item.reads) {
            if (address >= limit || begin >= end) continue;
            heldReadHits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

std::uint64_t DrawPipeline::HeldReadQueries() {
    return heldReadQueries.load(std::memory_order_relaxed);
}

void DrawPipeline::ReleaseRunningReads() {
    std::lock_guard lock(mutex);
    if (!items.empty()) items.front().reads.clear();
}

std::optional<std::uint64_t> DrawPipeline::PendingLabel(std::uint64_t address, std::size_t bytes) {
    if (bytes == 0 || bytes > 8) return std::nullopt;
    if (DrawFinisher::Overlap()) {
        std::uint64_t held = 0;
        const auto lookup = DrawFinisher::Get().PendingLabel(address, bytes, held);
        if (lookup == DrawFinisher::LabelLookup::Value) return held;
        if (lookup == DrawFinisher::LabelLookup::Unknown) return std::nullopt;
    }
    const auto end = address + bytes;
    std::lock_guard lock(mutex);
    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        const bool overlaps = std::any_of(item->writes.begin(), item->writes.end(), [&](const Range& range) { return address < range.second && range.first < end; });
        if (!overlaps) continue;
        if (item->labelBytes.empty() || address < item->labelAddress || end > item->labelAddress + item->labelBytes.size()) return std::nullopt;
        std::uint64_t value = 0;
        std::memcpy(&value, item->labelBytes.data() + (address - item->labelAddress), bytes);
        return value;
    }
    return std::nullopt;
}

void DrawPipeline::countWrites(const std::vector<Range>& writes, bool added) {
    for (const auto& range : writes) {
        const auto found = std::find_if(queuedWrites.begin(), queuedWrites.end(), [&](const auto& entry) { return entry.first == range; });
        if (added) {
            if (found != queuedWrites.end()) ++found->second;
            else queuedWrites.emplace_back(range, 1u);
        } else if (found != queuedWrites.end() && --found->second == 0) {
            *found = queuedWrites.back();
            queuedWrites.pop_back();
        }
    }
}

DrawPipeline::Watch DrawPipeline::WatchWrites() {
    if (!Busy()) return {Retired(), 0, {}};
    std::lock_guard lock(mutex);
    Watch watch{retiredItems.load(std::memory_order_relaxed), items.size(), {}};
    for (const auto& item : items) watch.writes.insert(watch.writes.end(), item.writes.begin(), item.writes.end());
    return watch;
}

bool DrawPipeline::WrittenSince(const Watch& watch, const std::vector<Range>& reads) {
    const auto overlaps = [&reads](const std::vector<Range>& writes) {
        return std::any_of(reads.begin(), reads.end(), [&writes](const Range& read) {
            return std::any_of(writes.begin(), writes.end(), [&read](const Range& write) { return read.first < write.second && write.first < read.second; });
        });
    };
    if (DrawFinisher::Overlap() && DrawFinisher::Get().PendingOverlapsAny(reads)) return true;
    std::lock_guard lock(mutex);
    if (std::any_of(items.begin(), items.end(), [&](const Item& item) { return overlaps(item.writes); })) return true;
    const auto retiredNow = retiredItems.load(std::memory_order_relaxed);
    if (retiredNow == watch.retired) return false;
    return retiredNow - watch.retired > watch.items || overlaps(watch.writes);
}

bool DrawPipeline::Busy() const {
    return outstanding.load(std::memory_order_acquire) != 0 || (finisherProducer && DrawFinisher::Busy() && DrawFinisher::Overlap());
}

bool DrawPipeline::OverlapsAny(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges) {
    if (DrawFinisher::Overlap() && DrawFinisher::Get().PendingOverlapsAny(ranges)) return true;
    std::lock_guard lock(mutex);
    for (const auto& [range, count] : queuedWrites) {
        for (const auto& [address, end] : ranges) {
            if (address < range.second && range.first < end) return true;
        }
    }
    return false;
}

bool DrawPipeline::Overlaps(std::uint64_t address, std::size_t bytes) {
    const auto end = address + bytes;
    if (DrawFinisher::Overlap() && DrawFinisher::Get().PendingOverlaps(address, end)) return true;
    std::lock_guard lock(mutex);
    for (const auto& [range, count] : queuedWrites) {
        if (address < range.second && range.first < end) return true;
    }
    return false;
}

}
