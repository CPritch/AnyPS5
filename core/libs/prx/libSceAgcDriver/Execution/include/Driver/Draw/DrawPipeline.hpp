#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_DRAW_DRAWPIPELINE_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

class QueueWaits {
public:
    enum class Site : std::uint8_t {
        FinisherRelocation,
        FinisherLookup,
        FinisherReads,
        FinisherKept,
        FinisherEnqueue,
        FinisherPacket,
        FinisherPipelineDrain,
        FinisherEnd,
        FinisherSettle,
        FinisherDispatch,
        FinisherTargeted,
        FinisherOther,
        FinisherRoom,
        PipelineRoomWorker,
        PipelineRoomFinisher,
        PipelineDrainFinisher,
        FinisherIdleSpin,
        FinisherIdleBlocked,
        CommitterIdleSpin,
        CommitterIdleBlocked,
        CommitterIdleBehindWait,
        CommitterRetire,
        Count
    };
    static bool Enabled();
    static void Note(Site site, std::chrono::steady_clock::time_point since, std::chrono::steady_clock::time_point until);
    static void Note(Site site, std::chrono::steady_clock::time_point since) { Note(site, since, std::chrono::steady_clock::now()); }
    static void NoteShadowCapture(bool overlapped);
    static void NoteQueuedLabelRead();
    static std::string Text();
};

struct PreProofRequest {
    const void* emittedTemplate = nullptr;
    const void* resolvedTemplate = nullptr;
    std::uint64_t token = 0;
    std::uint64_t collectEpoch = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> excluded;
    std::function<void()> finish;
};

class DrawPipeline {
public:
    using Commit = std::function<void()>;
    using Prepare = std::function<void()>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    using PreProofRequest = DriverDetail::PreProofRequest;
    struct EarlyProbe {
        std::vector<Range>* depth = nullptr;
        const void* hintTemplate = nullptr;
    };
    struct EarlyOffer {
        const std::vector<Range>* writes = nullptr;
        std::span<const Range> depth;
        const void* currentTemplate = nullptr;
        PreProofRequest* preProof = nullptr;
        EarlyProbe* probe = nullptr;
        const void* nextTemplate = nullptr;
        bool twoAhead = false;
    };
    using Early = std::function<bool(const EarlyOffer&)>;
    struct Watch {
        std::uint64_t retired = 0;
        std::size_t items = 0;
        std::vector<Range> writes;
    };
    enum class DrainReason : std::uint8_t { Packet, Flush, Labels, Capture, Indirect, Submission, Count };
    enum class ItemKind : std::uint8_t { Other, Draw, IndirectDraw, Dispatch, DispatchMiss, DispatchNoRecipe, Label, Fill, Copy, Store, Pass, Seal, Completion, Dump, Count };

    static DrawPipeline& Queue0();
    static std::size_t Depth();
    static std::uint64_t CommitSequence();
    static const std::vector<std::pair<std::uint64_t, std::uint64_t>>* CommittingWrites();
    static std::size_t QueuedItems();
    static bool& Active();
    static std::atomic<std::uint64_t>& EpochToken();
    static void FollowEpoch(std::uint64_t token);
    static unsigned Committers();
    static unsigned PrepareThreads();
    static bool PreparesAhead();

    void Enqueue(Commit commit, std::vector<Range> writes, std::uint64_t labelAddress = 0, std::vector<std::byte> labelBytes = {}, std::vector<Range> reads = {}, bool draw = false, ItemKind kind = ItemKind::Other, Prepare prepare = {}, Early early = {});
    static unsigned ResolveSplit();
    enum class EarlyEvent : std::uint8_t { Used, StaleProof, StalePending, StaleImports, StaleDevice, StaleSubmit, StaleRedo, StaleNoted, StaleTemplate, OverlapAfter, ResolverFailed, DeclinedWriters, MultisampleFound, MultisampleMissed, VerifyMatched, VerifyDiffered, Unoffered, KeptRecipe, KeptTessellation, KeptEpoch, KeptReadsWritten, KeptDepthColour, KeptPredictor, KeptCurrentTemplate, KeptDescribed, IdleSubmitAfterEmit, NextMissing, NextNotDraw, NextNoHook, LateOffered, AcrossSeal, AcrossLabels, UsedAcrossLabels, StaleScoped, StaleScopedTemplate, PostedTwoAhead, Count };
    static void NoteEarly(EarlyEvent event);
    static std::uint64_t EarlyEventCount(EarlyEvent event);
    static bool OfferAcrossLabels();
    static void OfferEarly(std::span<const Range> depth, const void* currentTemplate);
    static void NoteResolvedTemplate(const void* resolved);
    static bool PreProofEnabled();
    static void QuiescePreProof();
    enum class PreProofOutcome : std::uint8_t { NotReady, Epoch, Writers, NoTemplate, Emitted, Resolved, NotCached, Current, Overlap, Failed, ProofMoved, Proved, Count };
    static void NotePreProof(PreProofOutcome outcome);
    static void PostResolve(void (*body)(void*), void (*finish)(void*, bool), void* context);
    static void JoinResolve();
    static void WaitResolve();
    static bool ResolveLanes();
    static bool LaneJobsInFlight();
    static void JoinCarriedJob();
    static void SettleCarriedBoundary();
    static void EndResolvePhase();
    class ResolvePhaseScope {
    public:
        ResolvePhaseScope();
        ~ResolvePhaseScope();
        ResolvePhaseScope(const ResolvePhaseScope&) = delete;
        ResolvePhaseScope& operator=(const ResolvePhaseScope&) = delete;

    private:
        bool held = false;
    };
    class LaneSerial {
    public:
        LaneSerial();
        ~LaneSerial();
        LaneSerial(const LaneSerial&) = delete;
        LaneSerial& operator=(const LaneSerial&) = delete;

    private:
        bool held = false;
    };
    static bool ParallelResolve();
    static bool EarlyStopped();
    static void StopEarly();
    static void NoteEarlyAbort(std::uint8_t kind);
    static std::uint64_t EarlyAbortCount(std::uint8_t kind);
    static void NoteEarlyResolveNs(std::uint64_t ns);
    enum class CommitterPart : std::uint8_t { Check, ResolveWhole, Offer, EmitEarly, EmitWhole, Join, Prologue, MemoKeeps, Count };
    static void NoteCommitterPart(CommitterPart part, std::chrono::steady_clock::time_point since);
    enum class OwnCause : std::uint8_t { NoneQueued, BehindSeal, BehindSealThenLabel, BehindLabel, BehindFill, BehindOther, NextNoHook, DeclinedPredictor, DeclinedDescribed, DeclinedReadsWritten, DeclinedOther, Revoked, AbortedImageRefresh, AbortedOther, Failed, Dropped, StaleSubmit, StaleProof, StaleOther, Unknown, Count };
    static OwnCause TakenOwnCause();
    static std::uint64_t LastJoinWaitNs();
    static void NoteStaleSubmit();
    static constexpr std::size_t LabelPartCount = 6;
    static bool LabelPartsCounted();
    static void NoteLabelParts(const std::array<std::uint64_t, LabelPartCount>& parts);
    static constexpr std::size_t LabelStorePartCount = 7;
    static void NoteLabelStoreParts(const std::array<std::uint64_t, LabelStorePartCount>& parts, int reason);
    static void NoteJobClass(std::size_t jobClass, std::uint64_t jobNs, std::uint64_t waitNs);
    static bool CarryHold();
    static bool HoldCarried();
    static void ReleaseCarriedHold() noexcept;
    static void SetCarryRelease(void (*release)());
    static void NoteOwnResolve(OwnCause cause, std::uint64_t resolveNs, std::uint64_t emitNs);
    static std::uint64_t FollowedEpoch();
    void Drain(DrainReason reason, std::uint32_t opcode = 0x100);
    void WaitWritten(std::uint64_t address, std::size_t bytes, DrainReason reason, std::uint32_t opcode = 0x100);
    void WaitWrittenAny(std::span<const Range> ranges, DrainReason reason, std::uint32_t opcode = 0x100);
    static bool TargetedWaits();
    void Stop();
    void ReleaseRetired();
    static std::uint64_t DrainRequestsByOpcode(std::uint32_t opcode);
    bool Busy() const;
    std::size_t Outstanding() const { return outstanding.load(std::memory_order_acquire); }
    bool Overlaps(std::uint64_t address, std::size_t bytes);
    bool OverlapsAny(std::span<const std::pair<std::uint64_t, std::uint64_t>> ranges);
    Watch WatchWrites();
    bool WrittenSince(const Watch& watch, const std::vector<Range>& reads);
    std::uint64_t Retired() const { return retiredItems.load(std::memory_order_acquire); }
    void NoteDecodeDrain() { decodeDrains.fetch_add(1, std::memory_order_relaxed); }
    std::uint64_t DecodeDrains() const { return decodeDrains.load(std::memory_order_relaxed); }
    std::optional<std::uint64_t> PendingLabel(std::uint64_t address, std::size_t bytes);
    static bool HeldReads(std::uint64_t address, std::size_t bytes);
    static void SkipCommittingReads(bool skip);
    static std::uint64_t HeldReadQueries();
    void ReleaseRunningReads();

    static bool MetadataPassesPipelined();
    static void CountPipelinedMetadataPass();
    static std::uint64_t PipelinedMetadataPasses();
private:
    struct Item {
        Commit commit;
        std::vector<Range> writes;
        std::uint64_t labelAddress = 0;
        std::vector<std::byte> labelBytes;
        std::vector<Range> reads;
        bool draw = false;
        ItemKind kind = ItemKind::Other;
        Early early;
    };
    DrawPipeline() = default;
    void run();
    void prepareAhead();
    void queuePrepare(Prepare prepare);
    void rethrowFailure();
    void report(std::chrono::steady_clock::time_point now);
    void countWrites(const std::vector<Range>& writes, bool added);

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::condition_variable room;
    std::condition_variable retiredWake;
    std::deque<Item> items;
    static bool SealsBreakProofs();
    static bool RunMember(const Item& item, std::size_t& labels);
    static const std::vector<Range>* OfferWrites(const std::deque<Item>& items, std::size_t next);
    static bool OnlyRunBehind(const std::deque<Item>& items);
    static std::size_t EarlyIndex(const std::deque<Item>& items);
    std::vector<std::pair<Range, std::uint32_t>> queuedWrites;
    std::vector<Item> retired;
    std::exception_ptr failure;
    std::atomic<std::size_t> outstanding{0};
    std::atomic<std::uint64_t> retiredItems{0};
    std::atomic<std::uint64_t> decodeDrains{0};
    std::uint64_t reportedDecodeDrains = 0;
    std::uint32_t idleWaiters = 0;
    std::uint32_t roomWaiters = 0;
    std::uint32_t targetWaiters = 0;
    std::uint64_t targetWake = ~std::uint64_t{0};
    std::array<std::uint64_t, static_cast<std::size_t>(ItemKind::Count)> kindGpuWaitNs{};
    std::uint64_t targetedWaits = 0;
    std::uint64_t targetedWaitNs = 0;
    std::uint64_t targetedBehind = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(ItemKind::Count)> targetedKinds{};
    bool committerWaiting = false;
    bool stopped = false;
    bool closing = false;
    std::uint64_t wakes = 0;
    std::uint64_t lockWaitNs = 0;
    std::thread thread;
    std::uint64_t commits = 0;
    std::uint64_t commitNs = 0;
    std::uint64_t commitErrors = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t fullWaitNs = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drains{};
    std::array<std::uint64_t, static_cast<std::size_t>(DrainReason::Count)> drainWaitNs{};
    std::array<std::uint64_t, 257> drainOpcodes{};
    std::array<std::uint64_t, 257> drainOpcodeNs{};
    std::array<std::uint64_t, static_cast<std::size_t>(ItemKind::Count)> kindCommits{};
    std::array<std::uint64_t, static_cast<std::size_t>(ItemKind::Count)> kindCommitNs{};
    std::array<std::uint64_t, 5> drawRuns{}, drawRunDraws{};
    std::uint64_t drawRun = 0;
    std::array<std::atomic<std::uint64_t>, 257> drainRequests{};
    std::uint64_t depthSum = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();
    std::mutex prepareMutex;
    std::deque<Prepare> prepares;
    std::vector<Prepare> spentPrepares;
    std::vector<std::thread> preparers;
    std::atomic<std::size_t> queuedPrepares{0};
    std::atomic<bool> preparersClosing{false};
    std::atomic<std::uint32_t> prepareWake{0};
    std::atomic<std::uint32_t> preparersSleeping{0};
    struct ResolveLane {
        std::thread thread;
        alignas(64) std::atomic<int> state{0};
        std::atomic<std::uint32_t> wake{0};
        std::atomic<std::uint32_t> sleeping{0};
        void (*body)(void*) = nullptr;
        void (*finish)(void*, bool) = nullptr;
        void* context = nullptr;
        const void* target = nullptr;
        std::uint64_t sequence = 0;
        std::size_t distance = 0;
        std::chrono::steady_clock::time_point posted;
    };
    void resolveAhead(unsigned laneIndex);
    static void WakeResolverForSpent(unsigned lane);
    static void offerSecondEarly(std::span<const Range> depth, const void* currentTemplate);
    static void joinResolve(bool revoke);
    static void joinLane(DrawPipeline& pipeline, ResolveLane& lane, bool revoke);
    void wakeLane(ResolveLane& lane);
    bool laneTargets(const void* item) const;
    void postPreProof(const void* currentTemplate, std::span<const Range> depth, const std::vector<Range>* writes);
    void runPreProof();
    void settlePreProof(const void* taken);
    void consumePreProof();
    alignas(64) std::atomic<int> preProofState{0};
    Item* preProofTarget = nullptr;
    std::atomic<const void*> preProofAfter{nullptr};
    PreProofRequest preProofRequest;
    std::array<ResolveLane, 2> lanes;
    std::atomic<bool> resolverClosing{false};
    alignas(64) std::atomic<std::uint32_t> resolveGate{0};
    alignas(64) std::atomic<bool> laneSerial{false};
    alignas(64) std::uint64_t nextJob = 0;
    unsigned postLane = 0;
    unsigned oldestLane = 0;
    unsigned lanesInFlight = 0;
    std::uint32_t carriedBoundaries = 0;
    alignas(64) std::uint64_t resolveLineEnd = 0;
};

class DrawFinisher {
public:
    using Job = std::function<void()>;
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    struct Pending {
        std::uint64_t key = 0;
        std::uint64_t shape = 0;
        bool inserts = false;
        bool unknown = true;
        std::vector<Range> writes;
        std::uint64_t labelAddress = 0;
        std::vector<std::byte> labelBytes;
    };
    enum class LabelLookup : std::uint8_t { NotHeld, Value, Unknown };
    static bool Overlap();
    bool PendingOverlaps(std::uint64_t begin, std::uint64_t end);
    LabelLookup PendingLabel(std::uint64_t address, std::size_t bytes, std::uint64_t& value);
    bool PendingOverlapsAny(std::span<const Range> ranges);
    bool PendingKey(std::uint64_t key, std::uint64_t shape);
    bool WaitPendingKey(std::uint64_t key, std::uint64_t shape);
    static bool LookupDrainsEvery();
    enum class DrainSite : std::uint32_t { Lookup, Reads, Packet, Kept, Enqueue, Count };
    static void NoteDrain(DrainSite site, bool drained);
    static std::string DrainText();
    static void NoteDrainedOpcode(std::uint32_t opcode, bool besideCommits);
    static DrawFinisher& Get();
    static bool Enabled();
    static bool OnFinisherThread();
    static bool Busy() { return outstanding.load(std::memory_order_acquire) != 0; }
    void Enqueue(Job job);
    void Enqueue(Job job, Pending held);
    void Drain(QueueWaits::Site site = QueueWaits::Site::FinisherOther);
    void WaitPendingWritten(std::span<const Range> ranges);
    void Settle();
    void ReleaseRan();

private:
    DrawFinisher() = default;
    void run();
    inline static std::atomic<std::size_t> outstanding{0};
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::condition_variable room;
    std::uint32_t idleWaiters = 0;
    std::uint32_t roomWaiters = 0;
    std::deque<Job> jobs;
    std::deque<Pending> pending;
    std::vector<Job> ran;
    std::exception_ptr failure;
    std::uint64_t ranJobs = 0;
    bool running = false;
    bool sleeping = false;
    std::thread thread;
};

}

#endif
