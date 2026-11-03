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
#include <map>
#include <optional>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef __linux__
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif
#include <stdexcept>
#include <string>

namespace AgcDriver::DriverDetail {

namespace {
std::string TakeStaleSubmitText();
thread_local std::uint64_t followedEpoch = 0;
std::atomic<std::uint64_t> committerEpochChanges{0};
std::atomic<std::uint64_t> earlyResolved{0}, earlyDeclined{0}, earlyFailed{0};
std::atomic<bool> earlyStopped{false};
struct PendingEarly {
    DrawPipeline::Early hook;
    const std::vector<DrawPipeline::Range>* writes = nullptr;
    bool late = false;
    const void* target = nullptr;
    std::size_t next = 0;
};
thread_local PendingEarly pendingEarly;
thread_local DrawPipeline* committingPipeline = nullptr;
constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::ItemKind::Count)> kindNames{"other", "draw", "indirect draw", "dispatch", "dispatch miss", "dispatch without recipe", "label", "fill", "copy", "store", "pass", "seal", "completion", "occlusion dump"};
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrawPipeline::ItemKind::Count)> nextNotDrawKinds{};
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrawPipeline::ItemKind::Count) + 1> afterSealKinds{};
thread_local bool skipCommittingReads = false;
enum ResolveJobState : int { ResolveIdle, ResolvePosted, ResolveRunning, ResolveDone };
std::atomic<std::uint64_t> earlyResolveNs{0}, earlyResolveCount{0};
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrawPipeline::CommitterPart::Count)> committerPartNs{}, committerPartCount{};
std::atomic<std::uint64_t> resolvesPosted{0}, resolvesRevoked{0}, resolvesDoneBeforeJoin{0}, resolvesWaited{0}, resolveWaitNs{0};
alignas(64) std::atomic<std::uint64_t> resolveStartNs{0};
std::atomic<std::uint64_t> resolveBusyNs{0};
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrawPipeline::EarlyEvent::Count)> earlyEvents{};
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(GuestMemory::BarrierKind::Count)> earlyAborts{};
thread_local std::uint64_t lastJoinWaitNs = 0;
constexpr std::size_t JobClassCount = 7;
std::array<std::atomic<std::uint64_t>, JobClassCount> jobClassCount{}, jobClassNs{}, jobClassWaited{}, jobClassWaitNs{};
thread_local const void* offerTarget = nullptr;
thread_local std::size_t offerNext = 0;
thread_local std::size_t postedNext = 0;
thread_local bool carriedHold = false;
thread_local const void* carriedTarget = nullptr;
thread_local const void* takenItem = nullptr;
thread_local const void* heldReadsFrom = nullptr;
void (*carryRelease)() = nullptr;
std::atomic<std::uint64_t> carriedHolds{0}, carriesReleasedEarly{0}, heldReadsTargetMissing{0};
thread_local DrawPipeline::OwnCause takenOwnCause = DrawPipeline::OwnCause::Unknown;
thread_local DrawPipeline::OwnCause nextOwnCause = DrawPipeline::OwnCause::Unknown;
std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DrawPipeline::OwnCause::Count)> ownCauseCount{}, ownCauseResolveNs{}, ownCauseEmitNs{};

bool OwnCausesCounted() {
    static const bool counted = std::getenv("APS5_COMMIT_STATS") != nullptr;
    return counted;
}
}

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
    static const bool enabled = std::getenv("APS5_COMMIT_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
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
    text += TakePatchMemoText();
    if (DrawPipeline::ResolveSplit() != 0) {
        const auto take = [](std::atomic<std::uint64_t>& value) { return std::to_string(value.exchange(0, std::memory_order_relaxed)); };
        const auto event = [&](DrawPipeline::EarlyEvent kind) { return take(earlyEvents[static_cast<std::size_t>(kind)]); };
        using E = DrawPipeline::EarlyEvent;
        text += "; early resolves (APS5_RESOLVE_SPLIT" + std::string(DrawPipeline::EarlyStopped() ? ", stopped by verify" : "") + "): resolved " + take(earlyResolved) + ", declined " + take(earlyDeclined) + ", failed " + take(earlyFailed) + ", never offered " + event(E::Unoffered) + "; packets used " + event(E::Used) + ", redone for proof " + event(E::StaleProof) + ", pending " + event(E::StalePending) + ", imports " + event(E::StaleImports) + ", device " + event(E::StaleDevice) + ", submit " + event(E::StaleSubmit) + TakeStaleSubmitText() + ", redo " + event(E::StaleRedo) + ", a later note over its reads " + event(E::StaleNoted) + ", template left the cache " + event(E::StaleTemplate) + "; dropped after resolve for reads overlapping the previous draw's writes " + event(E::OverlapAfter) + ", failed on the resolver " + event(E::ResolverFailed) + "; declined for the resolver's copied writers " + event(E::DeclinedWriters) + "; multisampled draws whose targets were found before resolve " + event(E::MultisampleFound) + ", not found " + event(E::MultisampleMissed) + "; verified same " + event(E::VerifyMatched) + ", different " + event(E::VerifyDiffered) + "; draws not offered, by the next item: none queued " + event(E::NextMissing) + ", not a draw " + event(E::NextNotDraw) + " (" + [&] {
            std::string kinds;
            for (std::size_t kind = 0; kind < kindNames.size(); ++kind) {
                const auto count = nextNotDrawKinds[kind].exchange(0, std::memory_order_relaxed);
                if (count != 0) kinds += (kinds.empty() ? "" : ", ") + std::string(kindNames[kind]) + " " + std::to_string(count);
            }
            return kinds;
        }() + "), after such a seal (" + [&] {
            std::string kinds;
            for (std::size_t kind = 0; kind < afterSealKinds.size(); ++kind) {
                const auto count = afterSealKinds[kind].exchange(0, std::memory_order_relaxed);
                if (count != 0) kinds += (kinds.empty() ? "" : ", ") + std::string(kind < kindNames.size() ? kindNames[kind] : "a draw without an early hook") + " " + std::to_string(count);
            }
            return kinds;
        }() + "), offered once the committer's own resolve was done " + event(E::LateOffered) + ", offered across a seal " + event(E::AcrossSeal) + ", offered across a label run " + event(E::AcrossLabels) + " (used " + event(E::UsedAcrossLabels) + ", redone for a label's bytes " + event(E::StaleScoped) + ", for its template " + event(E::StaleScopedTemplate) + ")" + ", a draw without an early hook " + event(E::NextNoHook) + "; kept by the gate: recipe " + event(E::KeptRecipe) + ", tessellation " + event(E::KeptTessellation) + ", another epoch " + event(E::KeptEpoch) + ", reads the previous draw's writes " + event(E::KeptReadsWritten) + ", depth over colour " + event(E::KeptDepthColour) + ", predictor " + event(E::KeptPredictor) + ", the template being emitted " + event(E::KeptCurrentTemplate) + ", described ranges written " + event(E::KeptDescribed) + "; idle submits moved after the emit " + event(E::IdleSubmitAfterEmit) + "; aborted at a barrier:";
        for (std::size_t kind = 1; kind < earlyAborts.size(); ++kind) {
            const auto count = earlyAborts[kind].exchange(0, std::memory_order_relaxed);
            if (count != 0) text += std::string(" ") + GuestMemory::BarrierKindName(static_cast<GuestMemory::BarrierKind>(kind)) + " " + std::to_string(count) + ",";
        }
        text += " committer draw parts (count, ns each):";
        constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::CommitterPart::Count)> partNames{"early-packet checks", "resolves of its own", "offers", "emits of early packets", "emits of its own packets", "joins", "prologues (claim, lock, labels, draw writers)", "memo keeps"};
        for (std::size_t part = 0; part < partNames.size(); ++part) {
            const auto count = committerPartCount[part].exchange(0, std::memory_order_relaxed);
            const auto ns = committerPartNs[part].exchange(0, std::memory_order_relaxed);
            text += std::string(" ") + partNames[part] + " " + std::to_string(count) + " " + std::to_string(count == 0 ? 0 : ns / count) + ",";
        }
        if (OwnCausesCounted()) {
            constexpr std::array<const char*, static_cast<std::size_t>(DrawPipeline::OwnCause::Count)> causeNames{"none queued", "behind a seal", "behind a seal then a label", "behind a label", "behind a fill", "behind another item", "next without an early hook", "declined by the predictor", "declined for described ranges written", "declined for reads the previous draw writes", "declined otherwise", "revoked unstarted", "aborted at an image refresh", "aborted at another barrier", "failed on the resolver", "dropped after the resolve", "stale after a submit", "stale after a proof break", "stale otherwise", "unknown"};
            text += " committer's own resolves by cause (count, resolve ns, emit ns each):";
            for (std::size_t cause = 0; cause < causeNames.size(); ++cause) {
                const auto count = ownCauseCount[cause].exchange(0, std::memory_order_relaxed);
                const auto resolveNs = ownCauseResolveNs[cause].exchange(0, std::memory_order_relaxed);
                const auto emitNs = ownCauseEmitNs[cause].exchange(0, std::memory_order_relaxed);
                if (count != 0) text += std::string(" ") + causeNames[cause] + " " + std::to_string(count) + " " + std::to_string(resolveNs / count) + " " + std::to_string(emitNs / count) + ",";
            }
        }
        if (DrawPipeline::ResolveSplit() == 1) {
            const auto count = earlyResolveCount.exchange(0, std::memory_order_relaxed);
            text += " early resolves on the committer took " + std::to_string(count == 0 ? 0 : earlyResolveNs.exchange(0, std::memory_order_relaxed) / count) + " ns each;";
        }
        if (DrawPipeline::ResolveSplit() >= 2) {
            const auto posted = resolvesPosted.exchange(0, std::memory_order_relaxed);
            const auto started = posted - std::min(posted, resolvesRevoked.load(std::memory_order_relaxed));
            const auto waited = resolvesWaited.exchange(0, std::memory_order_relaxed);
            const auto per = [](std::uint64_t ns, std::uint64_t count) { return count == 0 ? std::string("0") : std::to_string(ns / count); };
            {
                constexpr std::array<const char*, JobClassCount> classNames{"template reused", "never proved", "new epoch", "proofs broken", "pending images changed", "no template", "aborted"};
                text += " resolver jobs by their template proof (jobs, us each, joins waited, us each):";
                for (std::size_t index = 0; index < classNames.size(); ++index) {
                    const auto jobs = jobClassCount[index].exchange(0, std::memory_order_relaxed);
                    const auto jobNs = jobClassNs[index].exchange(0, std::memory_order_relaxed);
                    const auto waits = jobClassWaited[index].exchange(0, std::memory_order_relaxed);
                    const auto waitNs = jobClassWaitNs[index].exchange(0, std::memory_order_relaxed);
                    if (jobs == 0) continue;
                    char item[160];
                    std::snprintf(item, sizeof(item), " %s %llu %.2f %llu %.2f,", classNames[index], static_cast<unsigned long long>(jobs), static_cast<double>(jobNs) / 1e3 / static_cast<double>(jobs), static_cast<unsigned long long>(waits), waits == 0 ? 0.0 : static_cast<double>(waitNs) / 1e3 / static_cast<double>(waits));
                    text += item;
                }
            }
            text += " holds carried to the next draw " + take(carriedHolds) + " (released before another item " + take(carriesReleasedEarly) + ", held reads whose target was gone " + take(heldReadsTargetMissing) + "),";
            text += " resolver jobs posted " + std::to_string(posted) + ", revoked unstarted " + take(resolvesRevoked) + ", done before the join " + take(resolvesDoneBeforeJoin) + ", waited for " + std::to_string(waited) + " (" + per(resolveWaitNs.exchange(0, std::memory_order_relaxed), waited) + " ns each), start latency " + per(resolveStartNs.exchange(0, std::memory_order_relaxed), started) + " ns, resolving " + per(resolveBusyNs.exchange(0, std::memory_order_relaxed), started) + " ns a job; recorder methods off the resolver's list:" + GuestMemory::TakeResolverRecorderText() + ";";
        }
        const auto [unwinding, handler, completion] = GuestMemory::TakeEarlyBarriersPassed();
        text += " barriers passed while unwinding " + std::to_string(unwinding) + ", in an exception handler " + std::to_string(handler) + ", in a completion " + std::to_string(completion);
    }
    text += "; pending images:" + Graphics::TakePendingCleanText();
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

bool DrawFinisher::WaitPendingKey(std::uint64_t key, std::uint64_t shape) {
    if (!Busy() || !finisherProducer || OnFinisherThread()) return false;
    const bool every = LookupDrainsEvery();
    std::unique_lock lock(mutex);
    std::uint64_t target = 0;
    std::size_t after = 0;
    for (std::size_t i = pending.size(); i-- > 0;) {
        const auto& held = pending[i];
        if (!(every || held.inserts) || (held.key != key && (shape == 0 || held.shape != shape))) continue;
        target = ranJobs + i + 1;
        after = pending.size() - i - 1;
        break;
    }
    if (target == 0) return false;
    if (GuestMemory::GpuMutex().HeldByThisThread()) throw std::runtime_error("draw finisher wait under the GPU lock");
    PerformanceTimer timing("DrawFinisher.WaitKey");
    const auto start = std::chrono::steady_clock::now();
    lock.unlock();
    SpinUntil(HandoffSpinWindow() * 2, [&] { return outstanding.load(std::memory_order_acquire) <= after; });
    lock.lock();
    ++idleWaiters;
    idle.wait(lock, [&] { return ranJobs >= target || failure != nullptr; });
    --idleWaiters;
    QueueWaits::Note(QueueWaits::Site::FinisherLookup, start);
    if (failure != nullptr) std::rethrow_exception(std::exchange(failure, nullptr));
    return true;
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

namespace {
std::size_t FinisherDepth() {
    static const std::size_t depth = [] {
        const char* text = std::getenv("APS5_FINISHER_DEPTH");
        const auto value = text != nullptr ? std::strtoull(text, nullptr, 10) : 16ull;
        return static_cast<std::size_t>(std::clamp<unsigned long long>(value, 2ull, 256ull));
    }();
    return depth;
}
}

void DrawFinisher::Enqueue(Job job) {
    Enqueue(std::move(job), Pending{});
}

void DrawFinisher::Enqueue(Job job, Pending held) {
    finisherProducer = true;
    const std::size_t depth = FinisherDepth();
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
            const std::size_t depth = FinisherDepth();
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


void DrawPipeline::NoteEarly(EarlyEvent event) {
    earlyEvents[static_cast<std::size_t>(event)].fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t DrawPipeline::EarlyEventCount(EarlyEvent event) {
    return earlyEvents[static_cast<std::size_t>(event)].load(std::memory_order_relaxed);
}

void DrawPipeline::NoteCommitterPart(CommitterPart part, std::chrono::steady_clock::time_point since) {
    const auto index = static_cast<std::size_t>(part);
    committerPartNs[index].fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count()), std::memory_order_relaxed);
    committerPartCount[index].fetch_add(1, std::memory_order_relaxed);
}

DrawPipeline::OwnCause DrawPipeline::TakenOwnCause() {
    return takenOwnCause;
}

std::uint64_t DrawPipeline::LastJoinWaitNs() {
    return lastJoinWaitNs;
}

namespace {
std::mutex staleSubmitMutex;
std::map<std::pair<const void*, Graphics::Recorder::SubmitCause>, std::uint64_t> staleSubmits;

std::string TakeStaleSubmitText() {
    std::vector<std::pair<std::pair<const void*, Graphics::Recorder::SubmitCause>, std::uint64_t>> entries;
    {
        std::lock_guard lock(staleSubmitMutex);
        entries.assign(staleSubmits.begin(), staleSubmits.end());
        staleSubmits.clear();
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    constexpr std::array<const char*, static_cast<std::size_t>(Graphics::Recorder::SubmitCause::Count)> causes{"other", "label deadline", "batch cap", "idle"};
    std::string text = " (by the submit:";
    for (std::size_t i = 0; i < entries.size() && i < 8; ++i) {
        const auto& [key, count] = entries[i];
        unsigned long long offset = reinterpret_cast<std::uintptr_t>(key.first);
#ifdef __linux__
        Dl_info info{};
        if (key.first != nullptr && dladdr(key.first, &info) != 0 && info.dli_fbase != nullptr) offset = reinterpret_cast<std::uintptr_t>(key.first) - reinterpret_cast<std::uintptr_t>(info.dli_fbase);
#endif
        char item[96];
        std::snprintf(item, sizeof(item), "%s %s at +0x%llx %llu", i == 0 ? "" : ",", causes[static_cast<std::size_t>(key.second)], offset, static_cast<unsigned long long>(count));
        text += item;
    }
    return text + ")";
}
}

namespace {
std::array<std::atomic<std::uint64_t>, DrawPipeline::LabelPartCount> labelPartNs{};
std::atomic<std::uint64_t> labelPartCommits{0};
std::array<std::atomic<std::uint64_t>, DrawPipeline::LabelStorePartCount> labelStorePartNs{};
std::array<std::atomic<std::uint64_t>, 8> labelStoreReasons{};
std::atomic<std::uint64_t> labelStores{0};
}

void DrawPipeline::NoteLabelStoreParts(const std::array<std::uint64_t, LabelStorePartCount>& parts, int reason) {
    labelStores.fetch_add(1, std::memory_order_relaxed);
    labelStoreReasons[static_cast<std::size_t>(std::clamp(reason, 0, 7))].fetch_add(1, std::memory_order_relaxed);
    for (std::size_t part = 0; part < parts.size(); ++part) labelStorePartNs[part].fetch_add(parts[part], std::memory_order_relaxed);
}

bool DrawPipeline::LabelPartsCounted() {
    static const bool counted = std::getenv("APS5_COMMIT_STATS") != nullptr;
    return counted;
}

void DrawPipeline::NoteLabelParts(const std::array<std::uint64_t, LabelPartCount>& parts) {
    labelPartCommits.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t part = 0; part < parts.size(); ++part) labelPartNs[part].fetch_add(parts[part], std::memory_order_relaxed);
}

void DrawPipeline::NoteStaleSubmit() {
    const auto cause = Graphics::Recorder::LastSubmitCause();
    std::lock_guard lock(staleSubmitMutex);
    ++staleSubmits[cause];
}

void DrawPipeline::NoteJobClass(std::size_t jobClass, std::uint64_t jobNs, std::uint64_t waitNs) {
    const auto index = std::min(jobClass, JobClassCount - 1);
    jobClassCount[index].fetch_add(1, std::memory_order_relaxed);
    jobClassNs[index].fetch_add(jobNs, std::memory_order_relaxed);
    if (waitNs == 0) return;
    jobClassWaited[index].fetch_add(1, std::memory_order_relaxed);
    jobClassWaitNs[index].fetch_add(waitNs, std::memory_order_relaxed);
}

bool DrawPipeline::CarryHold() {
    static const bool enabled = std::getenv("APS5_NO_CARRY_RESOLVE_HOLD") == nullptr;
    const auto next = std::exchange(postedNext, 0);
    if (!enabled || next != 1 || committingPipeline == nullptr || carriedHold || std::uncaught_exceptions() != 0) return false;
    auto& pipeline = Queue0();
    if (pipeline.resolveState.load(std::memory_order_acquire) == ResolveIdle || pipeline.resolveTarget == nullptr) return false;
    GuestMemory::GpuMutex().lock();
    carriedHold = true;
    carriedTarget = pipeline.resolveTarget;
    carriedHolds.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool DrawPipeline::HoldCarried() {
    return carriedHold;
}

void DrawPipeline::ReleaseCarriedHold() noexcept {
    if (!carriedHold) return;
    carriedHold = false;
    carriedTarget = nullptr;
    try {
        GuestMemory::GpuMutex().unlock();
    } catch (...) {
    }
}

void DrawPipeline::SetCarryRelease(void (*release)()) {
    carryRelease = release;
}

void DrawPipeline::NoteOwnResolve(OwnCause cause, std::uint64_t resolveNs, std::uint64_t emitNs) {
    const auto index = static_cast<std::size_t>(cause);
    ownCauseCount[index].fetch_add(1, std::memory_order_relaxed);
    ownCauseResolveNs[index].fetch_add(resolveNs, std::memory_order_relaxed);
    ownCauseEmitNs[index].fetch_add(emitNs, std::memory_order_relaxed);
}

void DrawPipeline::NoteEarlyResolveNs(std::uint64_t ns) {
    earlyResolveNs.fetch_add(ns, std::memory_order_relaxed);
    earlyResolveCount.fetch_add(1, std::memory_order_relaxed);
}

void DrawPipeline::NoteEarlyAbort(std::uint8_t kind) {
    earlyAborts[std::min<std::size_t>(kind, earlyAborts.size() - 1)].fetch_add(1, std::memory_order_relaxed);
}

unsigned DrawPipeline::ResolveSplit() {
    static const unsigned mode = [] {
        const char* text = std::getenv("APS5_RESOLVE_SPLIT");
        return text == nullptr ? 2u : static_cast<unsigned>(std::strtoul(text, nullptr, 10));
    }();
    return mode;
}

void DrawPipeline::PostResolve(void (*body)(void*), void (*finish)(void*, bool), void* context) {
    auto& pipeline = Queue0();
    if (!pipeline.resolver.joinable()) {
        std::lock_guard lock(pipeline.prepareMutex);
        if (!pipeline.resolverClosing.load(std::memory_order_relaxed) && !pipeline.resolver.joinable()) pipeline.resolver = std::thread([&pipeline] { pipeline.resolveAhead(); });
    }
    pipeline.resolveBody = body;
    pipeline.resolveFinish = finish;
    pipeline.resolveContext = context;
    pipeline.resolveTarget = std::exchange(offerTarget, nullptr);
    postedNext = std::exchange(offerNext, 0);
    static const bool timed = std::getenv("APS5_COMMIT_STATS") != nullptr;
    if (timed) pipeline.resolvePosted = std::chrono::steady_clock::now();
    resolvesPosted.fetch_add(1, std::memory_order_relaxed);
    pipeline.resolveState.store(ResolvePosted, std::memory_order_release);
    pipeline.resolverWake.fetch_add(1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (pipeline.resolverSleeping.load(std::memory_order_seq_cst) != 0) pipeline.resolverWake.notify_one();
}

void DrawPipeline::JoinResolve() {
    joinResolve(true);
}

void DrawPipeline::WaitResolve() {
    joinResolve(false);
}

void DrawPipeline::joinResolve(bool revoke) {
    auto& pipeline = Queue0();
    postedNext = 0;
    lastJoinWaitNs = 0;
    if (pipeline.resolveState.load(std::memory_order_acquire) == ResolveIdle) return;
    bool ran = true;
    int expected = ResolvePosted;
    if (revoke && pipeline.resolveState.compare_exchange_strong(expected, ResolveIdle, std::memory_order_acq_rel)) {
        ran = false;
        resolvesRevoked.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (!revoke) expected = pipeline.resolveState.load(std::memory_order_acquire);
        if (expected == ResolveDone) {
            resolvesDoneBeforeJoin.fetch_add(1, std::memory_order_relaxed);
        } else {
            const auto start = std::chrono::steady_clock::now();
            while (pipeline.resolveState.load(std::memory_order_acquire) != ResolveDone) SpinPause();
            resolvesWaited.fetch_add(1, std::memory_order_relaxed);
            lastJoinWaitNs = elapsedNs(start);
            resolveWaitNs.fetch_add(lastJoinWaitNs, std::memory_order_relaxed);
        }
        pipeline.resolveState.store(ResolveIdle, std::memory_order_relaxed);
    }
    const auto finish = std::exchange(pipeline.resolveFinish, nullptr);
    auto* context = std::exchange(pipeline.resolveContext, nullptr);
    pipeline.resolveBody = nullptr;
    if (finish == nullptr) return;
    if (std::uncaught_exceptions() == 0) {
        finish(context, ran);
        return;
    }
    try {
        finish(context, ran);
    } catch (...) {
    }
}

#ifdef __linux__
namespace {
struct sigaction previousSegv {};
struct sigaction previousBus {};

void reportFatalFault(int signal, siginfo_t* info, void* context) {
    const auto& previous = signal == SIGBUS ? previousBus : previousSegv;
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
        previous.sa_sigaction(signal, info, context);
        return;
    }
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN && previous.sa_handler != nullptr) {
        previous.sa_handler(signal);
        return;
    }
    char name[32] = "?";
    pthread_getname_np(pthread_self(), name, sizeof(name));
    const auto* registers = static_cast<const ucontext_t*>(context);
    std::fprintf(stderr, "[crash] signal %d (code %d) at address %p in thread '%s' (tid %ld), rip %p rsp %p\n", signal, info->si_code, info->si_addr, name, static_cast<long>(syscall(SYS_gettid)), reinterpret_cast<void*>(registers->uc_mcontext.gregs[REG_RIP]), reinterpret_cast<void*>(registers->uc_mcontext.gregs[REG_RSP]));
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    if (const int maps = open("/proc/self/maps", O_RDONLY); maps >= 0) {
        std::fprintf(stderr, "[crash] executable mappings:\n");
        char buffer[4096];
        std::string pending;
        for (ssize_t got; (got = read(maps, buffer, sizeof(buffer))) > 0;) {
            pending.append(buffer, static_cast<std::size_t>(got));
            for (std::size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1)) {
                const auto line = pending.substr(0, end);
                if (line.find(" r-xp ") != std::string::npos) std::fprintf(stderr, "%s\n", line.c_str());
            }
        }
        close(maps);
    }
    std::fflush(stderr);
    ::signal(signal, SIG_DFL);
    ::raise(signal);
}

void installFaultReport() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction action {};
        action.sa_sigaction = reportFatalFault;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, &previousSegv);
        sigaction(SIGBUS, &action, &previousBus);
    });
}
}
#endif

void DrawPipeline::resolveAhead() {
#ifdef __linux__
    installFaultReport();
#endif
    GuestMemory::MarkResolverThread();
    PinResolverThread("draw resolver");
    PerformanceTimer::UntraceThisThread();
    constexpr std::chrono::microseconds idleNap{20};
    static const bool spins = std::getenv("APS5_RESOLVER_SPINS") != nullptr;
    static const auto window = [] {
        const char* text = std::getenv("APS5_RESOLVER_SPIN_US");
        return std::chrono::nanoseconds(std::chrono::microseconds(text != nullptr ? std::strtoll(text, nullptr, 10) : 20));
    }();
    for (;;) {
        const auto posted = [&] { return resolveState.load(std::memory_order_acquire) == ResolvePosted || resolverClosing.load(std::memory_order_relaxed); };
        if (spins && !SpinUntil(HandoffSpinWindow(), posted)) {
            std::this_thread::sleep_for(idleNap);
            continue;
        }
        if (!spins && !SpinUntil(window, posted)) {
            const auto wake = resolverWake.load(std::memory_order_acquire);
            resolverSleeping.fetch_add(1, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (!posted()) resolverWake.wait(wake, std::memory_order_acquire);
            resolverSleeping.fetch_sub(1, std::memory_order_relaxed);
            continue;
        }
        if (resolverClosing.load(std::memory_order_acquire)) return;
        int expected = ResolvePosted;
        if (!resolveState.compare_exchange_strong(expected, ResolveRunning, std::memory_order_acq_rel)) continue;
        static const bool timed = std::getenv("APS5_COMMIT_STATS") != nullptr;
        const auto start = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (timed) resolveStartNs.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(start - resolvePosted).count()), std::memory_order_relaxed);
        heldReadsFrom = resolveTarget;
        try {
            resolveBody(resolveContext);
        } catch (...) {
        }
        heldReadsFrom = nullptr;
        if (timed) resolveBusyNs.fetch_add(elapsedNs(start), std::memory_order_relaxed);
        resolveState.store(ResolveDone, std::memory_order_release);
    }
}

bool DrawPipeline::EarlyStopped() {
    return earlyStopped.load(std::memory_order_relaxed);
}

void DrawPipeline::StopEarly() {
    earlyStopped.store(true, std::memory_order_relaxed);
}

bool DrawPipeline::SealsBreakProofs() {
    static const bool breaks = std::getenv("APS5_SEALS_BREAK_PROOFS") != nullptr;
    return breaks;
}

bool DrawPipeline::OfferAcrossLabels() {
    static const bool across = std::getenv("APS5_OFFER_ACROSS_LABELS") != nullptr && GuestMemory::ScopedLabelBreaks();
    return across;
}

namespace {

constexpr std::size_t MaxOfferRun = 4;
constexpr std::size_t MaxOfferLabels = 2;
thread_local std::vector<DrawPipeline::Range> runWrites;

}

bool DrawPipeline::RunMember(const Item& item, std::size_t& labels) {
    if (item.kind == ItemKind::Seal && !SealsBreakProofs()) return true;
    if (!OfferAcrossLabels() || item.kind != ItemKind::Label || item.writes.empty() || labels >= MaxOfferLabels) return false;
    ++labels;
    return true;
}

const std::vector<DrawPipeline::Range>* DrawPipeline::OfferWrites(const std::deque<Item>& items, std::size_t next) {
    if (next < 2 || !OfferAcrossLabels()) return &items.front().writes;
    runWrites.assign(items.front().writes.begin(), items.front().writes.end());
    for (std::size_t k = 1; k < next; ++k) runWrites.insert(runWrites.end(), items[k].writes.begin(), items[k].writes.end());
    return &runWrites;
}

bool DrawPipeline::OnlyRunBehind(const std::deque<Item>& items) {
    std::size_t labels = 0;
    for (std::size_t k = 1; k < items.size(); ++k) {
        if (k > MaxOfferRun || !RunMember(items[k], labels)) return false;
    }
    return true;
}

std::size_t DrawPipeline::EarlyIndex(const std::deque<Item>& items) {
    if (items.size() >= 2 && items[1].draw) return items[1].early ? 1 : 0;
    if (!OfferAcrossLabels()) {
        if (items.size() >= 3 && items[1].kind == ItemKind::Seal && !SealsBreakProofs() && items[2].draw && items[2].early) return 2;
        return 0;
    }
    std::size_t k = 1;
    std::size_t labels = 0;
    while (k < items.size() && k <= MaxOfferRun && RunMember(items[k], labels)) ++k;
    return k >= 2 && k < items.size() && items[k].draw && items[k].early ? k : 0;
}

void DrawPipeline::OfferEarly(std::span<const Range> depth, const void* currentTemplate) {
    if (!pendingEarly.hook && pendingEarly.late && committingPipeline != nullptr) {
        auto& pipeline = *committingPipeline;
        std::lock_guard lock(pipeline.mutex);
        if (const auto next = EarlyIndex(pipeline.items); next != 0) {
            pendingEarly.hook = pipeline.items[next].early;
            pendingEarly.writes = OfferWrites(pipeline.items, next);
            pendingEarly.target = &pipeline.items[next];
            pendingEarly.next = next;
            NoteEarly(EarlyEvent::LateOffered);
            nextOwnCause = OwnCause::Unknown;
            if (next >= 2) NoteEarly(pendingEarly.writes == &runWrites && runWrites.size() > pipeline.items.front().writes.size() ? EarlyEvent::AcrossLabels : EarlyEvent::AcrossSeal);
        }
    }
    if (!pendingEarly.hook) {
        pendingEarly = {};
        return;
    }
    auto hook = std::move(pendingEarly.hook);
    const auto* writes = pendingEarly.writes;
    offerTarget = pendingEarly.target;
    offerNext = pendingEarly.next;
    pendingEarly = {};
    try {
        (hook(EarlyOffer{writes, depth, currentTemplate}) ? earlyResolved : earlyDeclined).fetch_add(1, std::memory_order_relaxed);
    } catch (const ProcessShutdown&) {
        throw;
    } catch (...) {
        earlyFailed.fetch_add(1, std::memory_order_relaxed);
    }
}

std::uint64_t DrawPipeline::FollowedEpoch() {
    return followedEpoch;
}

void DrawPipeline::FollowEpoch(std::uint64_t token) {
    auto& followed = followedEpoch;
    if (followed == token) return;
    followed = token;
    static const bool shared = std::getenv("APS5_NO_SHARED_COMMIT_EPOCH") == nullptr;
    if (GuestMemory::CurrentHotThread() == GuestMemory::HotThread::Committer) committerEpochChanges.fetch_add(1, std::memory_order_relaxed);
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
    prepareWake.fetch_add(1, std::memory_order_seq_cst);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (preparersSleeping.load(std::memory_order_seq_cst) != 0) prepareWake.notify_all();
    while (preparers.size() < PrepareThreads()) preparers.emplace_back([this] { prepareAhead(); });
}

void DrawPipeline::prepareAhead() {
    PinPreparerThread("draw preparer");
    PerformanceTrace::BindThread("draw preparer", std::size_t{1} << 17u);
    constexpr std::chrono::microseconds idleNap{200};
    static const bool naps = std::getenv("APS5_PREPARER_NAPS") != nullptr;
    static const auto window = [] {
        const char* text = std::getenv("APS5_PREPARER_SPIN_US");
        return std::chrono::nanoseconds(std::chrono::microseconds(text != nullptr ? std::strtoll(text, nullptr, 10) : 20));
    }();
    for (;;) {
        const auto queued = [&] { return queuedPrepares.load(std::memory_order_acquire) != 0 || preparersClosing.load(std::memory_order_relaxed); };
        if (naps && !SpinUntil(HandoffSpinWindow(), queued)) {
            std::this_thread::sleep_for(idleNap);
            continue;
        }
        if (!naps && !SpinUntil(window, queued)) {
            const auto wake = prepareWake.load(std::memory_order_acquire);
            preparersSleeping.fetch_add(1, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (!queued()) prepareWake.wait(wake, std::memory_order_acquire);
            preparersSleeping.fetch_sub(1, std::memory_order_relaxed);
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
    static const bool stats = std::getenv("APS5_COMMIT_STATS") != nullptr || std::getenv("APS5_PIPELINE_STATS") != nullptr;
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
    drainText += "; draw runs between other items by length (runs/draws): 1 " + std::to_string(drawRuns[0]) + "/" + std::to_string(drawRunDraws[0]) + ", 2-4 " + std::to_string(drawRuns[1]) + "/" + std::to_string(drawRunDraws[1]) + ", 5-16 " + std::to_string(drawRuns[2]) + "/" + std::to_string(drawRunDraws[2]) + ", 17-64 " + std::to_string(drawRuns[3]) + "/" + std::to_string(drawRunDraws[3]) + ", 65 or more " + std::to_string(drawRuns[4]) + "/" + std::to_string(drawRunDraws[4]) + "; committer collect-epoch changes " + std::to_string(committerEpochChanges.exchange(0, std::memory_order_relaxed));
    drawRuns = {};
    drawRunDraws = {};
    {
        constexpr std::array<const char*, LabelPartCount> names{"lock and deferred labels", "linear stores", "GPU store", "notes", "interrupt", "submit due"};
        const auto labels = labelPartCommits.exchange(0, std::memory_order_relaxed);
        drainText += "; label commits " + std::to_string(labels) + ", parts (ns each):";
        for (std::size_t part = 0; part < names.size(); ++part) drainText += std::string(" ") + names[part] + " " + std::to_string(labels == 0 ? 0 : labelPartNs[part].exchange(0, std::memory_order_relaxed) / labels) + (part + 1 == names.size() ? "" : ",");
        constexpr std::array<const char*, LabelStorePartCount> storeNames{"pending image stores", "reap", "idle check and import lookup", "unit shadows", "copied-writer and completion checks", "recorded store", "note and stamp"};
        const auto stores = labelStores.exchange(0, std::memory_order_relaxed);
        drainText += "; label GPU stores " + std::to_string(stores) + " (by reason 0-7:";
        for (auto& reason : labelStoreReasons) drainText += " " + std::to_string(reason.exchange(0, std::memory_order_relaxed));
        drainText += "), parts (ns each):";
        for (std::size_t part = 0; part < storeNames.size(); ++part) drainText += std::string(" ") + storeNames[part] + " " + std::to_string(stores == 0 ? 0 : labelStorePartNs[part].exchange(0, std::memory_order_relaxed) / stores) + (part + 1 == storeNames.size() ? "" : ",");
    }
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
    AgcDriver::ProfilePrint_nid_no_patch("[draw] pipelined (10 s, depth %zu): %llu commits at %.1f us each on the committer, %llu failed; %llu enqueued behind %.2f on average, waiting %.1f us per draw for room, %.2f us for the lock; %llu committer wakes;%s; drains:%s\n", Depth(), static_cast<unsigned long long>(commits), perCommit, static_cast<unsigned long long>(commitErrors), static_cast<unsigned long long>(enqueued), queued, perDraw, enqueued == 0 ? 0.0 : static_cast<double>(lockWaitNs) / 1e3 / static_cast<double>(enqueued), static_cast<unsigned long long>(wakes), reads, drainText.c_str());
    if (PrepareThreads() != 0) AgcDriver::ProfilePrint_nid_no_patch("%s with %u threads\n", Graphics::DrawAheadReport().c_str(), PrepareThreads());
    commits = commitNs = commitErrors = enqueued = fullWaitNs = depthSum = wakes = lockWaitNs = 0;
    drains = {};
    drainWaitNs = {};
    AgcDriver::ProfilePrint_nid_no_patch("[waits] queue 0 (10 s): %s\n", QueueWaits::Text().c_str());
}


namespace {
void releaseCarriedForRun() {
    struct Drop {
        ~Drop() { DrawPipeline::ReleaseCarriedHold(); }
    } drop;
    if (carryRelease != nullptr) carryRelease();
    else DrawPipeline::JoinResolve();
}
}

void DrawPipeline::run() {
    committingPipeline = this;
    PinCommitterThread("draw committer");
    PerformanceTrace::BindThread("draw committer");
    StartCommitterSampler();
    GuestMemory::CountGpuLockWaits(GuestMemory::HotThread::Committer);
    const bool counted = QueueWaits::Enabled();
    static const bool timed = counted || std::getenv("APS5_COMMIT_STATS") != nullptr || std::getenv("APS5_PROFILE_DRAW") != nullptr;
    const auto clock = [] { return timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}; };
    auto idleStart = clock();
    bool idleBehindWait = false;
    static const bool prefetch = std::getenv("APS5_NO_COMMIT_PREFETCH") == nullptr;
    Commit* commit = nullptr;
    bool isDraw = false;
    std::size_t kind = 0;
    bool taken = false;
    const auto take = [&] {
        takenItem = &items.front();
        commit = stopped ? nullptr : &items.front().commit;
        committingWrites = &items.front().writes;
        isDraw = items.front().draw;
        kind = static_cast<std::size_t>(items.front().kind);
        if (ResolveSplit() == 0 || !isDraw || commit == nullptr || EarlyStopped()) return;
        if (OwnCausesCounted()) {
            takenOwnCause = nextOwnCause;
            nextOwnCause = OwnCause::Unknown;
        }
        if (const auto next = EarlyIndex(items); next != 0) {
            pendingEarly.hook = items[next].early;
            pendingEarly.writes = OfferWrites(items, next);
            pendingEarly.target = &items[next];
            pendingEarly.next = next;
            if (next >= 2) NoteEarly(pendingEarly.writes == &runWrites && runWrites.size() > items.front().writes.size() ? EarlyEvent::AcrossLabels : EarlyEvent::AcrossSeal);
            return;
        }
        const bool sealLast = OfferAcrossLabels() ? items.size() >= 2 && OnlyRunBehind(items) : items.size() == 2 && items[1].kind == ItemKind::Seal && !SealsBreakProofs();
        NoteEarly(items.size() < 2 || sealLast ? EarlyEvent::NextMissing : !items[1].draw ? EarlyEvent::NextNotDraw : EarlyEvent::NextNoHook);
        if (OwnCausesCounted()) {
            using C = OwnCause;
            const auto behind = [&](ItemKind next) { return next == ItemKind::Seal ? (items.size() >= 3 && items[2].kind == ItemKind::Label ? C::BehindSealThenLabel : C::BehindSeal) : next == ItemKind::Label ? C::BehindLabel : next == ItemKind::Fill ? C::BehindFill : C::BehindOther; };
            nextOwnCause = items.size() < 2 || sealLast ? C::NoneQueued : items[1].draw ? C::NextNoHook : behind(items[1].kind);
        }
        if (items.size() >= 2 && !items[1].draw && !sealLast) nextNotDrawKinds[static_cast<std::size_t>(items[1].kind)].fetch_add(1, std::memory_order_relaxed);
        if (items.size() >= 3 && items[1].kind == ItemKind::Seal) afterSealKinds[items[2].draw ? static_cast<std::size_t>(ItemKind::Count) : static_cast<std::size_t>(items[2].kind)].fetch_add(1, std::memory_order_relaxed);
        static const bool late = std::getenv("APS5_NO_LATE_EARLY_OFFER") == nullptr;
        if (late && (items.size() < 2 || sealLast)) {
            pendingEarly.late = true;
            pendingEarly.writes = &items.front().writes;
        }
    };
    for (;;) {
        if (!taken) {
            commit = nullptr;
            isDraw = false;
            kind = 0;
            SpinUntil(HandoffSpinWindow(), [&] { return outstanding.load(std::memory_order_acquire) != 0; });
        }
        const auto spun = counted ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        if (!taken) {
            std::unique_lock lock(mutex);
            committerWaiting = true;
            wake.wait(lock, [&] { return !items.empty() || closing; });
            committerWaiting = false;
            if (items.empty()) break;
            take();
        }
        taken = false;
        const auto start = clock();
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
        try {
            if (carriedHold && (commit == nullptr || takenItem != carriedTarget)) {
                carriesReleasedEarly.fetch_add(1, std::memory_order_relaxed);
                releaseCarriedForRun();
            }
            if (commit != nullptr) {
                struct ClearEarly {
                    ~ClearEarly() {
                        if (!pendingEarly.hook) {
                            pendingEarly = {};
                            return;
                        }
                        pendingEarly = {};
                        DrawPipeline::NoteEarly(DrawPipeline::EarlyEvent::Unoffered);
                    }
                } clearEarly;
                (*commit)();
            }
            const bool scopedLabel = kind == static_cast<std::size_t>(ItemKind::Label) && GuestMemory::TakeScopedLabelCommit();
            if (!isDraw && !scopedLabel && (kind != static_cast<std::size_t>(ItemKind::Seal) || SealsBreakProofs())) GuestMemory::BreakProofs(GuestMemory::ProofBreak::Item);
        } catch (const std::exception&) {
            failed = true;
            fatal = std::current_exception();
        } catch (const ProcessShutdown&) {
            shutdown = true;
            fatal = std::current_exception();
        } catch (...) {
            fatal = std::current_exception();
        }
        if (carriedHold && fatal != nullptr) {
            try {
                releaseCarriedForRun();
            } catch (...) {
            }
        }
        const auto committed = clock();
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
            if (kind == static_cast<std::size_t>(ItemKind::Draw)) {
                ++drawRun;
            } else if (drawRun != 0) {
                const std::size_t bucket = drawRun == 1 ? 0 : drawRun <= 4 ? 1 : drawRun <= 16 ? 2 : drawRun <= 64 ? 3 : 4;
                ++drawRuns[bucket];
                drawRunDraws[bucket] += drawRun;
                drawRun = 0;
            }
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
            if (prefetch && !items.empty()) {
                take();
                taken = true;
            }
        }
        idleStart = clock();
        if (counted) QueueWaits::Note(QueueWaits::Site::CommitterRetire, committed, idleStart);
    }
    if (carriedHold) {
        try {
            releaseCarriedForRun();
        } catch (...) {
        }
    }
    runningCommitters.fetch_sub(1, std::memory_order_acq_rel);
}

void DrawPipeline::Stop() {
    std::thread committer;
    std::vector<std::thread> stopped;
    {
        std::lock_guard lock(prepareMutex);
        preparersClosing.store(true, std::memory_order_release);
        prepareWake.fetch_add(1, std::memory_order_seq_cst);
        prepareWake.notify_all();
        stopped = std::move(preparers);
    }
    for (auto& preparer : stopped) {
        if (preparer.get_id() == std::this_thread::get_id()) preparer.detach();
        else preparer.join();
    }
    std::thread resolving;
    {
        std::lock_guard lock(prepareMutex);
        resolverClosing.store(true, std::memory_order_release);
        resolverWake.fetch_add(1, std::memory_order_release);
        resolverWake.notify_all();
        resolving = std::move(resolver);
    }
    if (resolving.joinable()) {
        if (resolving.get_id() == std::this_thread::get_id()) resolving.detach();
        else resolving.join();
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
    static const bool unordered = std::getenv("APS5_NO_QUEUED_DRAW_READ_ORDER") != nullptr;
    if (bytes == 0 || unordered) return false;
    const auto end = address + bytes;
    auto& pipeline = Queue0();
    std::lock_guard lock(pipeline.mutex);
    heldReadQueries.fetch_add(1, std::memory_order_relaxed);
    auto from = pipeline.items.begin();
    if (skipCommittingReads && !pipeline.items.empty()) {
        if (heldReadsFrom != nullptr) {
            while (from != pipeline.items.end() && &*from != heldReadsFrom) ++from;
            if (from == pipeline.items.end()) {
                heldReadsTargetMissing.fetch_add(1, std::memory_order_relaxed);
                from = pipeline.items.begin() + 1;
            }
        } else {
            ++from;
        }
    }
    for (auto it = from; it != pipeline.items.end(); ++it) {
        for (const auto& [begin, limit] : it->reads) {
            if (address >= limit || begin >= end) continue;
            heldReadHits.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void DrawPipeline::SkipCommittingReads(bool skip) {
    skipCommittingReads = skip;
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
