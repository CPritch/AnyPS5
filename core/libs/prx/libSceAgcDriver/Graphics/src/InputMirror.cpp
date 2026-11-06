#include "prx/libSceAgcDriver/Graphics/include/InputMirror.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Mutex.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>

namespace AgcDriver::Graphics {

namespace {

constexpr std::uint64_t StorageImageFloor = std::uint64_t{2048} << 20u;
constexpr std::size_t CandidateLimit = 16384;
constexpr std::size_t ContainingProbes = 8;
constexpr std::size_t PinnedViews = 32;

struct Entry {
    std::uint64_t address = 0;
    std::size_t bytes = 0;
    std::uint64_t generation = 0;
    std::uint64_t registryGeneration = 0;
    std::shared_ptr<DeviceBuffer> buffer;
    HostImport view{};
    bool dirty = true;
    std::uint32_t hitsSinceCopy = 0;
    std::uint64_t verifiedInBatch = 0;
    std::list<std::pair<std::uint64_t, std::size_t>>::iterator recent;
};

struct Candidate {
    std::uint64_t generation = 0;
    std::uint64_t registryGeneration = 0;
};

struct PairHash {
    std::size_t operator()(const std::pair<std::uint64_t, std::size_t>& key) const noexcept {
        return std::hash<std::uint64_t>{}(key.first * 0x9e3779b97f4a7c15ull ^ key.second);
    }
};

struct Cache {
    AgcDriver::Mutex mutex;
    VkDevice device = VK_NULL_HANDLE;
    std::map<std::pair<std::uint64_t, std::size_t>, std::shared_ptr<Entry>> entries;
    std::list<std::pair<std::uint64_t, std::size_t>> recency;
    std::unordered_map<std::pair<std::uint64_t, std::size_t>, Candidate, PairHash> candidates;
    std::uint64_t liveBytes = 0;
    std::vector<std::shared_ptr<Entry>> abandoned;
    std::vector<std::shared_ptr<Entry>> pendingFills;
};

Cache& Mirrors() {
    static auto* cache = new Cache;
    return *cache;
}

struct Counters {
    std::atomic<std::uint64_t> candidates{0}, created{0}, hits{0}, recopies{0}, demoted{0}, evicted{0}, refusedBudget{0}, refusedAllocation{0}, fills{0}, stagedFills{0}, filledBytes{0}, verified{0}, preludes{0}, preludeFills{0}, inBatchFills{0};
};

Counters& Counts() {
    static Counters counters;
    return counters;
}

std::uint64_t mirrorBudget() {
    static const std::uint64_t budget = [] {
        const char* text = std::getenv("APS5_INPUT_MIRROR_MIB");
        return (text != nullptr ? std::strtoull(text, nullptr, 10) : 1024ull) << 20u;
    }();
    return budget;
}

void erase(Cache& cache, std::map<std::pair<std::uint64_t, std::size_t>, std::shared_ptr<Entry>>::iterator found) {
    cache.liveBytes -= found->second->bytes;
    cache.recency.erase(found->second->recent);
    cache.entries.erase(found);
}

void evictTo(Cache& cache, std::uint64_t bytes) {
    while (cache.liveBytes > bytes && !cache.recency.empty()) {
        erase(cache, cache.entries.find(cache.recency.front()));
        Counts().evicted.fetch_add(1, std::memory_order_relaxed);
    }
}

void adoptDevice(Cache& cache, const Context& context) {
    if (cache.device == context.device) return;
    for (auto& [key, entry] : cache.entries) cache.abandoned.push_back(std::move(entry));
    cache.entries.clear();
    cache.recency.clear();
    cache.candidates.clear();
    cache.pendingFills.clear();
    cache.liveBytes = 0;
    cache.device = context.device;
}

std::shared_ptr<Entry> containing(Cache& cache, std::uint64_t address, std::size_t bytes, std::size_t alignment) {
    auto it = cache.entries.upper_bound({address, std::numeric_limits<std::size_t>::max()});
    for (std::size_t probe = 0; probe < ContainingProbes && it != cache.entries.begin(); ++probe) {
        --it;
        const auto& entry = *it->second;
        if (entry.address + entry.bytes >= address + bytes && (address - entry.address) % alignment == 0) return it->second;
    }
    return {};
}

struct Pins {
    std::array<std::shared_ptr<Entry>, PinnedViews> entries;
    std::size_t next = 0;
};

Pins& ThreadPins() {
    thread_local Pins pins;
    return pins;
}

const HostImport* pin(std::shared_ptr<Entry> entry) {
    auto& pins = ThreadPins();
    const auto* view = &entry->view;
    pins.entries[pins.next++ % PinnedViews] = std::move(entry);
    return view;
}

void reportMaybe() {
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    if (!profile) return;
    static std::atomic<std::int64_t> last{0};
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto previous = last.load(std::memory_order_relaxed);
    if (now - previous < 10 || !last.compare_exchange_strong(previous, now, std::memory_order_relaxed)) return;
    const auto counts = InputMirrorCounters();
    std::fprintf(stderr, "[mirrors] input mirrors: %llu live (%.0f MiB of %llu MiB), %llu hits, %llu created, %llu recopied, %llu demoted, %llu evicted, %llu candidates, %llu refused by budget, %llu fills (%llu staged, %.0f MiB) in %llu prelude submissions, %llu recorded in the batch\n", static_cast<unsigned long long>(counts.liveEntries), static_cast<double>(counts.liveBytes) / 1048576.0, static_cast<unsigned long long>(mirrorBudget() >> 20u), static_cast<unsigned long long>(counts.hits), static_cast<unsigned long long>(counts.created), static_cast<unsigned long long>(counts.recopies), static_cast<unsigned long long>(counts.demoted), static_cast<unsigned long long>(counts.evicted), static_cast<unsigned long long>(counts.candidates), static_cast<unsigned long long>(counts.refusedBudget), static_cast<unsigned long long>(counts.fills), static_cast<unsigned long long>(counts.stagedFills), static_cast<double>(counts.filledBytes) / 1048576.0, static_cast<unsigned long long>(counts.preludes), static_cast<unsigned long long>(counts.inBatchFills));
}

void verifyMirror(const Context& context, Recorder& recorder, const std::shared_ptr<Entry>& entry) {
    const auto readback = std::make_shared<Buffer>(context, entry->bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto expected = std::make_shared<std::vector<std::byte>>(entry->bytes);
    GuestMemory::Read(entry->address, *expected);
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    CopyBuffer(context, commands, entry->buffer->Handle(), 0, readback->Handle(), 0, entry->bytes);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT);
    recorder.Keep(readback, entry->bytes);
    recorder.OnComplete([readback, expected, address = entry->address, bytes = entry->bytes] {
        readback->Invalidate();
        const auto actual = readback->Bytes();
        if (std::memcmp(actual.data(), expected->data(), bytes) == 0) {
            Counts().verified.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::size_t first = 0;
        while (first < bytes && actual[first] == (*expected)[first]) ++first;
        std::fprintf(stderr, "[mirrors] APS5_VERIFY_INPUT_MIRRORS: the device-local mirror of 0x%llx+%zu differs from guest memory at byte %zu (mirror %02x, guest %02x)\n", static_cast<unsigned long long>(address), bytes, first, std::to_integer<unsigned>(actual[first]), std::to_integer<unsigned>((*expected)[first]));
        std::abort();
    });
}

struct FillSource {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    std::shared_ptr<Buffer> staged;
};

FillSource fillSource(const Context& context, const Entry& entry) {
    FillSource source;
    if (const auto* import = ExistingHostImportFor(context, entry.address, entry.bytes)) {
        source.buffer = import->buffer;
        source.offset = HostImportOffset(*import, entry.address);
        return source;
    }
    source.staged = std::make_shared<Buffer>(context, entry.bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    GuestMemory::Read(entry.address, source.staged->Bytes());
    source.buffer = source.staged->Handle();
    Counts().stagedFills.fetch_add(1, std::memory_order_relaxed);
    return source;
}

void recordCopy(const Context& context, VkCommandBuffer commands, const Entry& entry, FillSource& source, std::vector<std::shared_ptr<void>>& kept) {
    CopyBuffer(context, commands, source.buffer, source.offset, entry.buffer->Handle(), 0, entry.bytes);
    if (source.staged != nullptr) kept.push_back(std::move(source.staged));
    kept.push_back(entry.buffer);
    Counts().fills.fetch_add(1, std::memory_order_relaxed);
    Counts().filledBytes.fetch_add(entry.bytes, std::memory_order_relaxed);
}

constexpr VkAccessFlags FillSourceWrites = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
constexpr VkAccessFlags FillReaders = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

struct PreludeBatch {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<std::shared_ptr<void>> kept;
};

struct Preludes {
    std::optional<Context> context;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::deque<PreludeBatch> inFlight;
    std::vector<PreludeBatch> spare;
};

Preludes& PreludeState() {
    static auto* state = new Preludes;
    return *state;
}

void resetPreludes(Preludes& state) {
    for (auto& batch : state.inFlight) static_cast<void>(new std::vector<std::shared_ptr<void>>(std::move(batch.kept)));
    state.context.reset();
    state.pool = VK_NULL_HANDLE;
    state.inFlight.clear();
    state.spare.clear();
}

bool preludesEnabled() {
    static const bool disabled = std::getenv("APS5_NO_INPUT_MIRROR_PRELUDE") != nullptr;
    return !disabled;
}

void reclaimPreludes(Preludes& state, bool wait) {
    if (!state.context) return;
    const auto& context = *state.context;
    while (!state.inFlight.empty()) {
        auto& front = state.inFlight.front();
        if (wait) Check(context.Function<PFN_vkWaitForFences>("vkWaitForFences")(context.device, 1, &front.fence, VK_TRUE, std::numeric_limits<std::uint64_t>::max()), "vkWaitForFences input mirror prelude");
        else if (context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus")(context.device, front.fence) != VK_SUCCESS) break;
        Check(context.Function<PFN_vkResetFences>("vkResetFences")(context.device, 1, &front.fence), "vkResetFences input mirror prelude");
        Check(context.Function<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(front.commands, 0), "vkResetCommandBuffer input mirror prelude");
        front.kept.clear();
        state.spare.push_back(std::move(front));
        state.inFlight.pop_front();
    }
}

PreludeBatch beginPrelude(const Context& context) {
    auto& state = PreludeState();
    if (!state.context || state.context->device != context.device) {
        resetPreludes(state);
        state.context.emplace(context);
        VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        info.queueFamilyIndex = context.queueFamily;
        Check(context.Function<PFN_vkCreateCommandPool>("vkCreateCommandPool")(context.device, &info, nullptr, &state.pool), "vkCreateCommandPool input mirror prelude");
    }
    reclaimPreludes(state, false);
    PreludeBatch batch;
    if (!state.spare.empty()) {
        batch = std::move(state.spare.back());
        state.spare.pop_back();
    } else {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, state.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        Check(context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(context.device, &allocate, &batch.commands), "vkAllocateCommandBuffers input mirror prelude");
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &fence, nullptr, &batch.fence), "vkCreateFence input mirror prelude");
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(batch.commands, &begin), "vkBeginCommandBuffer input mirror prelude");
    return batch;
}

void submitPrelude(const Context& context, PreludeBatch batch) {
    Check(context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(batch.commands), "vkEndCommandBuffer input mirror prelude");
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &batch.commands;
    Check(context.Function<PFN_vkQueueSubmit>("vkQueueSubmit")(context.queue, 1, &submit, batch.fence), "vkQueueSubmit input mirror prelude");
    PreludeState().inFlight.push_back(std::move(batch));
    Counts().preludes.fetch_add(1, std::memory_order_relaxed);
}

bool writtenInBatch(Recorder& recorder, const Entry& entry) {
    return recorder.PendingWriteOverlaps(entry.address, entry.bytes) || recorder.QueuedStoreOverlaps(entry.address, entry.bytes) || recorder.QueuedKeyStoreOverlaps(entry.address, entry.bytes) || recorder.PendingLabelIn(entry.address, entry.bytes);
}

void fillInBatch(const Context& context, Recorder& recorder, const std::shared_ptr<Entry>& entry) {
    recorder.FlushStoresOverlapping(entry->address, entry->bytes);
    recorder.FlushKeyStoresOverlapping(entry->address, entry->bytes);
    auto source = fillSource(context, *entry);
    const bool imported = source.staged == nullptr;
    const auto commands = recorder.Commands();
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, FillSourceWrites, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    std::vector<std::shared_ptr<void>> kept;
    recordCopy(context, commands, *entry, source, kept);
    RecordMemoryBarrier(context, commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, FillReaders);
    for (auto& object : kept) recorder.Keep(std::move(object), entry->bytes);
    if (imported) recorder.NotePendingRead(entry->address, entry->bytes, Recorder::ReadKind::GpuCopy);
    Counts().inBatchFills.fetch_add(1, std::memory_order_relaxed);
}

void fillInPrelude(const Context& context, Recorder& recorder, std::span<const std::shared_ptr<Entry>> entries) {
    std::vector<FillSource> sources;
    sources.reserve(entries.size());
    for (const auto& entry : entries) sources.push_back(fillSource(context, *entry));
    auto batch = beginPrelude(context);
    RecordMemoryBarrier(context, batch.commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, FillSourceWrites, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    std::vector<std::pair<std::uint64_t, std::size_t>> reads;
    for (std::size_t at = 0; at < entries.size(); ++at) {
        const auto& entry = entries[at];
        if (sources[at].staged == nullptr) reads.emplace_back(entry->address, entry->bytes);
        recordCopy(context, batch.commands, *entry, sources[at], batch.kept);
        Counts().preludeFills.fetch_add(1, std::memory_order_relaxed);
    }
    RecordMemoryBarrier(context, batch.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, FillReaders);
    submitPrelude(context, std::move(batch));
    for (const auto& [address, bytes] : reads) recorder.NotePendingRead(address, bytes, Recorder::ReadKind::GpuCopy);
}

void fill(const Context& context, Recorder& recorder, const std::shared_ptr<Entry>& entry) {
    const bool prelude = preludesEnabled() && context.queue != VK_NULL_HANDLE && GuestMemory::GpuMutex().HeldByThisThread() && !Recorder::OnSecondaryRecorder();
    std::vector<std::shared_ptr<Entry>> dirty;
    std::uint64_t generation = 0;
    auto& cache = Mirrors();
    {
        std::lock_guard lock(cache.mutex);
        generation = entry->generation;
        if (prelude) {
            for (auto& pending : cache.pendingFills) {
                if (pending->dirty && pending != entry) {
                    pending->dirty = false;
                    dirty.push_back(std::move(pending));
                }
            }
            cache.pendingFills.clear();
        }
        if (entry->dirty) {
            entry->dirty = false;
            dirty.push_back(entry);
        }
    }
    std::vector<std::shared_ptr<Entry>> early;
    std::vector<std::shared_ptr<Entry>> later;
    bool selfInBatch = false;
    const auto openBatch = recorder.Submissions() + 1;
    for (auto& candidate : dirty) {
        if (!prelude || writtenInBatch(recorder, *candidate) || candidate->verifiedInBatch == openBatch) {
            if (candidate == entry) selfInBatch = true;
            else later.push_back(std::move(candidate));
        } else {
            early.push_back(std::move(candidate));
        }
    }
    if (!later.empty()) {
        std::lock_guard lock(cache.mutex);
        for (auto& pending : later) {
            pending->dirty = true;
            cache.pendingFills.push_back(std::move(pending));
        }
    }
    if (!early.empty()) fillInPrelude(context, recorder, early);
    if (selfInBatch) fillInBatch(context, recorder, entry);
    recorder.Keep(entry->buffer, entry->bytes);
    if (InputMirrorsVerified() && !writtenInBatch(recorder, *entry) && GuestMemory::CollectWrites(entry->address, entry->bytes) != 0 && GuestMemory::UnchangedSince(entry->address, entry->bytes, generation)) {
        entry->verifiedInBatch = openBatch;
        verifyMirror(context, recorder, entry);
    }
}

const HostImport* use(const Context& context, std::shared_ptr<Entry> entry, std::vector<std::function<void(Recorder&)>>* deferred) {
    if (deferred != nullptr) {
        deferred->push_back([context, entry](Recorder& recorder) { fill(context, recorder, entry); });
    } else {
        fill(context, *context.recorder, entry);
    }
    reportMaybe();
    return pin(std::move(entry));
}

std::shared_ptr<Entry> create(const Context& context, std::uint64_t address, std::size_t bytes, std::uint64_t generation, std::uint64_t registryGeneration) {
    if (context.memoryProperties2 != nullptr && context.physical != VK_NULL_HANDLE && StorageTextureCacheBudget(context) < StorageImageFloor + bytes) {
        auto& cache = Mirrors();
        std::lock_guard lock(cache.mutex);
        evictTo(cache, cache.liveBytes / 2);
        Counts().refusedBudget.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    std::shared_ptr<DeviceBuffer> buffer;
    try {
        buffer = std::make_shared<DeviceBuffer>(context, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    } catch (const std::exception&) {
        Counts().refusedAllocation.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    auto entry = std::make_shared<Entry>();
    entry->address = address;
    entry->bytes = bytes;
    entry->generation = generation;
    entry->registryGeneration = registryGeneration;
    entry->buffer = std::move(buffer);
    entry->view.base = address;
    entry->view.bytes = bytes;
    entry->view.buffer = entry->buffer->Handle();
    entry->view.memory = VK_NULL_HANDLE;
    entry->view.address = 0;
    auto& cache = Mirrors();
    std::lock_guard lock(cache.mutex);
    adoptDevice(cache, context);
    const auto key = std::pair{address, bytes};
    if (const auto found = cache.entries.find(key); found != cache.entries.end()) erase(cache, found);
    evictTo(cache, mirrorBudget() - std::min<std::uint64_t>(mirrorBudget(), bytes));
    cache.recency.push_back(key);
    entry->recent = std::prev(cache.recency.end());
    cache.entries.emplace(key, entry);
    cache.liveBytes += bytes;
    cache.candidates.erase(key);
    cache.pendingFills.push_back(entry);
    Counts().created.fetch_add(1, std::memory_order_relaxed);
    return entry;
}

}

bool InputMirrorsEnabled(const Context& context) {
    static const bool disabled = std::getenv("APS5_NO_INPUT_MIRRORS") != nullptr;
    static const bool allGpus = std::getenv("APS5_INPUT_MIRRORS_ALL_GPUS") != nullptr;
    return !disabled && (context.discreteGpu || allGpus) && mirrorBudget() != 0;
}

bool InputMirrorsVerified() {
    static const bool verify = std::getenv("APS5_VERIFY_INPUT_MIRRORS") != nullptr;
    return verify;
}

const HostImport* MirrorDrawInput(const Context& context, std::uint64_t address, std::size_t bytes, std::size_t alignment, std::vector<std::function<void(Recorder&)>>* deferred) {
    if (bytes == 0 || bytes > mirrorBudget() / 4 || !InputMirrorsEnabled(context) || context.recorder == nullptr || Recorder::OnSecondaryRecorder() || (deferred == nullptr && GuestMemory::OnResolverThread())) return nullptr;
    const auto registry = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    auto& cache = Mirrors();
    std::shared_ptr<Entry> entry;
    bool promote = false;
    std::uint64_t collected = 0;
    {
        std::lock_guard lock(cache.mutex);
        adoptDevice(cache, context);
        entry = containing(cache, address, bytes, alignment == 0 ? 1 : alignment);
        if (entry == nullptr && bytes < InputMirrorMinBytes) return nullptr;
        collected = GuestMemory::CollectWrites(address, bytes);
        if (collected == 0) return nullptr;
        if (entry != nullptr) {
            const auto key = std::pair{entry->address, entry->bytes};
            const bool sameRegistry = !GuestAllocations::GuestAllocationsChangedSince_nid_postfix(entry->registryGeneration, entry->address, entry->bytes);
            if (sameRegistry && GuestMemory::UnchangedSince(address, bytes, entry->generation)) {
                ++entry->hitsSinceCopy;
                cache.recency.splice(cache.recency.end(), cache.recency, entry->recent);
                Counts().hits.fetch_add(1, std::memory_order_relaxed);
            } else if (!sameRegistry || entry->hitsSinceCopy == 0) {
                erase(cache, cache.entries.find(key));
                if (sameRegistry) Counts().demoted.fetch_add(1, std::memory_order_relaxed);
                if (bytes >= InputMirrorMinBytes) cache.candidates[std::pair{address, bytes}] = Candidate{collected, registry};
                return nullptr;
            } else {
                entry->generation = GuestMemory::CollectWrites(entry->address, entry->bytes);
                entry->registryGeneration = registry;
                if (!entry->dirty) cache.pendingFills.push_back(entry);
                entry->dirty = true;
                entry->hitsSinceCopy = 0;
                cache.recency.splice(cache.recency.end(), cache.recency, entry->recent);
                Counts().recopies.fetch_add(1, std::memory_order_relaxed);
                if (entry->generation == 0) {
                    erase(cache, cache.entries.find(key));
                    return nullptr;
                }
            }
        } else {
            const auto key = std::pair{address, bytes};
            const auto found = cache.candidates.find(key);
            if (found != cache.candidates.end() && found->second.generation != 0 && !GuestAllocations::GuestAllocationsChangedSince_nid_postfix(found->second.registryGeneration, address, bytes) && GuestMemory::UnchangedSince(address, bytes, found->second.generation)) {
                promote = true;
            } else {
                if (cache.candidates.size() >= CandidateLimit) cache.candidates.clear();
                cache.candidates[key] = Candidate{collected, registry};
                Counts().candidates.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
        }
    }
    if (promote) {
        entry = create(context, address, bytes, collected, registry);
        if (entry == nullptr) return nullptr;
    }
    return use(context, std::move(entry), deferred);
}

InputMirrorCounts InputMirrorCounters() {
    auto& counts = Counts();
    InputMirrorCounts result;
    result.candidates = counts.candidates.load(std::memory_order_relaxed);
    result.created = counts.created.load(std::memory_order_relaxed);
    result.hits = counts.hits.load(std::memory_order_relaxed);
    result.recopies = counts.recopies.load(std::memory_order_relaxed);
    result.demoted = counts.demoted.load(std::memory_order_relaxed);
    result.evicted = counts.evicted.load(std::memory_order_relaxed);
    result.refusedBudget = counts.refusedBudget.load(std::memory_order_relaxed);
    result.refusedAllocation = counts.refusedAllocation.load(std::memory_order_relaxed);
    result.fills = counts.fills.load(std::memory_order_relaxed);
    result.stagedFills = counts.stagedFills.load(std::memory_order_relaxed);
    result.filledBytes = counts.filledBytes.load(std::memory_order_relaxed);
    result.verified = counts.verified.load(std::memory_order_relaxed);
    result.preludes = counts.preludes.load(std::memory_order_relaxed);
    result.preludeFills = counts.preludeFills.load(std::memory_order_relaxed);
    result.inBatchFills = counts.inBatchFills.load(std::memory_order_relaxed);
    auto& cache = Mirrors();
    std::lock_guard lock(cache.mutex);
    result.liveEntries = cache.entries.size();
    result.liveBytes = cache.liveBytes;
    return result;
}

void ClearInputMirrors() {
    auto& state = PreludeState();
    if (state.context) {
        reclaimPreludes(state, true);
        const auto& context = *state.context;
        for (auto& batch : state.spare) {
            context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, batch.fence, nullptr);
            context.Function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(context.device, state.pool, 1, &batch.commands);
        }
        context.Function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(context.device, state.pool, nullptr);
        resetPreludes(state);
    }
    auto& cache = Mirrors();
    std::lock_guard lock(cache.mutex);
    cache.entries.clear();
    cache.recency.clear();
    cache.candidates.clear();
    cache.liveBytes = 0;
    cache.pendingFills.clear();
    ThreadPins() = Pins{};
}

}
