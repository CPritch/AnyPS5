#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/VerifyCounters.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/DrawPipeline.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include <algorithm>
#include "prx/libSceAgcDriver/Execution/include/TracyZone.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace AgcDriver::DriverDetail {

std::uint32_t Driver::ReserveEopFlip(const FlipInfo& info) {
    std::lock_guard lock(mutex);
    require(nextEopToken <= EopFlipTokenLimit, "EOP flip token space exhausted");
    return reserveEopFlip(info, nextEopToken);
}

std::uint32_t Driver::ReserveEopFlipAt(const FlipInfo& info, std::uint32_t token) {
    std::lock_guard lock(mutex);
    return reserveEopFlip(info, token);
}

std::uint32_t Driver::reserveEopFlip(const FlipInfo& info, std::uint32_t token) {
    rethrowFailure();
    checkStopping();
    require(token >= EopFlipTokenBase && token <= EopFlipTokenLimit && !eopFlips.contains(token), "EOP flip token is not free");
    const auto output = outputs.find(info.handle);
    require(output != outputs.end(), "EOP flip references an unregistered video output");
    auto request = output->second->TryReserve(info);
    if (!request) return 0;
    eopFlips.emplace(token, EopFlip{info.handle, std::move(request)});
    nextEopToken = std::max(nextEopToken, token + 1);
    return token;
}

std::vector<std::uint32_t> Driver::PendingEopFlipTokens() {
    std::lock_guard lock(mutex);
    std::vector<std::uint32_t> tokens;
    for (const auto& [token, flip] : eopFlips) tokens.push_back(token);
    return tokens;
}

void Driver::RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    require(output != nullptr, "null video output");
    std::lock_guard lock(mutex);
    rethrowFailure();
    checkStopping();
    require(outputs.emplace(handle, output).second, "video output already registered");
}

void Driver::UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    std::lock_guard lock(mutex);
    const auto it = outputs.find(handle);
    require(it != outputs.end() && it->second == output, "video output registration mismatch");
    std::erase_if(eopFlips, [handle](const auto& entry) { return entry.second.handle == handle; });
    outputs.erase(it);
}

void Driver::AttachWindow(const PresentationWindow& window) {
    CheckFailure();
    std::unique_lock replacing(deviceReplacement);
    GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
    std::lock_guard lock(GuestMemory::GpuMutex());
    if (device) {
        require(device->Window() == nullptr, "a window is already attached to the device");
        device->PrepareForReplacement();
        replacedDevices.push_back(device);
    }
    device = std::make_shared<VulkanDevice>(&window);
}

void Driver::Present(const PresentationWindow& window, const DisplayBuffer* buffer, const DisplayBuffer* plane, bool opaque, void (*gpuReady)(void*), void* context) {
    if (PerformanceTrace::Configured()) PerformanceTrace::Present(window.width, window.height, device.Load().get());
    static const bool logged = std::getenv("APS5_LONG_WAIT_LOG") != nullptr;
    if (AnyVerifyRequested()) {
        static auto lastSummary = std::chrono::steady_clock::now();
        static VerifyCounts previousSummary{};
        const auto now = std::chrono::steady_clock::now();
        if (now - lastSummary >= std::chrono::seconds(10)) {
            lastSummary = now;
            const auto totals = VerifyTotals();
            char summary[1024];
            std::size_t length = static_cast<std::size_t>(std::snprintf(summary, sizeof(summary), "[verify] 10 s:"));
            for (std::size_t index = 0; index < totals.size() && length < sizeof(summary); ++index) {
                if (!VerifyRequested(static_cast<VerifyCount>(index))) continue;
                length += static_cast<std::size_t>(std::snprintf(summary + length, sizeof(summary) - length, " %s %llu", VerifyFieldName(static_cast<VerifyCount>(index)), static_cast<unsigned long long>(totals[index] - previousSummary[index])));
            }
            if (length < sizeof(summary)) length += static_cast<std::size_t>(std::snprintf(summary + length, sizeof(summary) - length, "; total:"));
            for (std::size_t index = 0; index < totals.size() && length < sizeof(summary); ++index) {
                if (!VerifyRequested(static_cast<VerifyCount>(index))) continue;
                length += static_cast<std::size_t>(std::snprintf(summary + length, sizeof(summary) - length, " %s %llu", VerifyFieldName(static_cast<VerifyCount>(index)), static_cast<unsigned long long>(totals[index])));
            }
            std::fprintf(stderr, "%s\n", summary);
            std::fflush(stderr);
            previousSummary = totals;
        }
    }
    if (logged) {
        static VerifyCounts previousVerify{};
        static std::uint64_t previousDraws = 0, previousResolves = 0, previousBuilds = 0;
        static Graphics::ImportChurnCounts previousChurn = Graphics::ImportChurnTotals();
        const auto draws = DrawPipeline::CommittedDraws(), resolves = DrawPipeline::OwnResolves(), builds = Graphics::DrawTemplateBuilds();
        const auto churn = Graphics::ImportChurnTotals();
        const auto delta = [](std::uint64_t now, std::uint64_t before) { return static_cast<unsigned long long>(now - before); };
        const auto micros = [](std::uint64_t now, std::uint64_t before) { return static_cast<unsigned long long>((now - before) / 1000u); };
        char line[2048];
        std::size_t used = static_cast<std::size_t>(std::snprintf(line, sizeof(line), "[present] %lld draws %llu own_resolves %llu template_builds %llu", static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()), static_cast<unsigned long long>(draws - previousDraws), static_cast<unsigned long long>(resolves - previousResolves), static_cast<unsigned long long>(builds - previousBuilds)));
        const auto field = [&](const char* name, unsigned long long value) {
            if (used < sizeof(line)) used += static_cast<std::size_t>(std::snprintf(line + used, sizeof(line) - used, " %s %llu", name, value));
        };
        field("alloc_generations", delta(churn.generations, previousChurn.generations));
        field("import_reconciles", delta(churn.reconciles, previousChurn.reconciles));
        field("import_reconcile_us", micros(churn.reconcileNs, previousChurn.reconcileNs));
        field("import_journal_refreshes", delta(churn.journalRefreshes, previousChurn.journalRefreshes));
        field("import_journal_fallbacks", delta(churn.journalFallbacks, previousChurn.journalFallbacks));
        field("import_full_refreshes", delta(churn.fullRefreshes, previousChurn.fullRefreshes));
        field("import_full_refresh_us", micros(churn.fullRefreshNs, previousChurn.fullRefreshNs));
        field("import_absorbs", delta(churn.absorbs, previousChurn.absorbs));
        field("import_retires", delta(churn.retires, previousChurn.retires));
        field("import_inserts", delta(churn.inserts, previousChurn.inserts));
        field("revalidate_full", delta(churn.revalidations, previousChurn.revalidations));
        field("revalidate_full_us", micros(churn.revalidateNs, previousChurn.revalidateNs));
        field("import_proof_walks", delta(churn.proofWalks, previousChurn.proofWalks));
        field("import_lock_waits", delta(churn.lockWaits, previousChurn.lockWaits));
        field("import_lock_wait_us", micros(churn.lockWaitNs, previousChurn.lockWaitNs));
        field("import_rc_absorbed", delta(churn.rcAbsorbed, previousChurn.rcAbsorbed));
        field("import_rc_full_off", delta(churn.rcOff, previousChurn.rcOff));
        field("import_rc_full_device", delta(churn.rcDevice, previousChurn.rcDevice));
        field("import_rc_full_unseeded", delta(churn.rcUnseeded, previousChurn.rcUnseeded));
        field("import_rc_full_window", delta(churn.rcWindow, previousChurn.rcWindow));
        field("import_rc_full_budget", delta(churn.rcBudget, previousChurn.rcBudget));
        field("import_rc_full_waiting", delta(churn.rcWaiting, previousChurn.rcWaiting));
        field("import_rc_full_unproven", delta(churn.rcUnproven, previousChurn.rcUnproven));
        field("import_rc_aborted", delta(churn.rcAborted, previousChurn.rcAborted));
        field("import_full_outside", delta(churn.fullOutside, previousChurn.fullOutside));
        field("import_full_lease_us", micros(churn.fullLeaseNs, previousChurn.fullLeaseNs));
        field("import_journal_reseeds", delta(churn.reseeded, previousChurn.reseeded));
        field("import_retire_reconcile", delta(churn.retireReconcile, previousChurn.retireReconcile));
        field("import_retire_waiter", delta(churn.retireWaiter, previousChurn.retireWaiter));
        field("import_retire_replace", delta(churn.retireReplace, previousChurn.retireReplace));
        field("import_retire_evict", delta(churn.retireEvict, previousChurn.retireEvict));
        field("import_table_other", delta(churn.tableOther, previousChurn.tableOther));
        field("import_proof_generation_bumps", delta(churn.proofGenerationBumps, previousChurn.proofGenerationBumps));
        field("import_proofs_survived", delta(churn.proofsSurvived, previousChurn.proofsSurvived));
        field("import_map_unlocked", delta(churn.mapUnlocked, previousChurn.mapUnlocked));
        field("import_map_discarded", delta(churn.mapDiscarded, previousChurn.mapDiscarded));
        const auto verifyTotals = VerifyTotals();
        for (std::size_t index = 0; index < verifyTotals.size(); ++index) {
            if (VerifyRequested(static_cast<VerifyCount>(index))) field(VerifyFieldName(static_cast<VerifyCount>(index)), delta(verifyTotals[index], previousVerify[index]));
        }
        previousVerify = verifyTotals;
        std::fprintf(stderr, "%s\n", line);
        previousDraws = draws;
        previousResolves = resolves;
        previousBuilds = builds;
        previousChurn = churn;
    }
    PerformanceContext timingContext(window.timing.get());
    PerformanceTimer timing("Driver.Present");
    static thread_local bool pinned = false;
    if (!pinned) {
        pinned = true;
        PinWorkerThread("presenter");
        GuestMemory::MarkPresenterThread();
    }
    CheckFailure();
    require(gpuReady != nullptr && context != nullptr, "missing GPU completion callback");
    require(window.getDrawableSize != nullptr, "missing window drawable size query");

    static const bool syncFlip = std::getenv("APS5_SYNC_FLIP") != nullptr;
    static const std::size_t inFlight = VulkanDevice::FlipInFlight();
    static const bool profile = std::getenv("APS5_PROFILE_DRAW") != nullptr;
    std::shared_ptr<VulkanDevice> presenting;
    timing.Mark("validate");
    try {
        bool submitted = false;
        bool presentable = false;
        bool trailing = false;
        bool queueUnlocked = false;
        double waitedMs = 0;
        {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
            std::lock_guard lock(GuestMemory::GpuMutex());
            timing.Mark("gpu_mutex_wait");
            require(device != nullptr && device->Window() == window.context, "presentation window is not attached to the device");
            presenting = device;
            timing.Mark("device_setup");
            std::uint32_t drawableWidth = 0;
            std::uint32_t drawableHeight = 0;
            window.getDrawableSize(window.context, &drawableWidth, &drawableHeight);
            presenting->Resize(drawableWidth, drawableHeight);
            presenting->SelectDynamicRange(buffer);
            timing.Mark("resize");
            presentable = presenting->Presentable();
            if (buffer != nullptr) require(buffer->width == window.width && buffer->height == window.height, "display buffer extent differs from output");
        }

        if (presentable && !syncFlip) {
            if (inFlight != 0) {

                waitedMs = presenting->RetirePresents(presenting->PresentWaitsForSlots(buffer) ? 0 : inFlight);
                timing.Mark("inflight_wait");
            }
            presentable = presenting->AcquireImage();
            timing.Mark("acquire_image");
        }
        if (presentable) {
            GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
            std::lock_guard lock(GuestMemory::GpuMutex());
            timing.Mark("gpu_mutex_wait");
            if (buffer != nullptr) {
                if (syncFlip) {
                    presenting->WaitIdle();
                    timing.Mark("device_idle_wait");
                }
                submitted = presenting->PresentDisplayBuffer(*buffer, plane);
                timing.Mark("present_display_buffer");
            } else {
                submitted = presenting->PresentClear(window.width, window.height, opaque, plane);
                timing.Mark("present_clear");
            }
            if (submitted && syncFlip) {
                waitedMs = presenting->FinishPresent();
                timing.Mark("render_fence_wait");
            }
            if (submitted && (syncFlip || inFlight != 0)) {

                if (presenting->PresentsOnOwnQueue()) {
                    queueUnlocked = true;
                } else {
                    presenting->QueuePresent();
                    timing.Mark("queue_present");
                }
                trailing = !syncFlip;
                submitted = false;
            }
        }
        if (queueUnlocked) {
            presenting->QueuePresent();
            timing.Mark("queue_present");
        }
        if (trailing) {
            waitedMs += presenting->FinishPresent();
            timing.Mark("render_fence_wait");
        }
        if (submitted) {
            waitedMs = presenting->FinishPresent();
            timing.Mark("render_fence_wait");

            if (presenting->PresentsOnOwnQueue()) {
                presenting->QueuePresent();
            } else {
                GuestMemory::TagGpuLockSite(GuestMemory::GpuLockSite::Present);
                std::lock_guard lock(GuestMemory::GpuMutex());
                presenting->QueuePresent();
            }
            timing.Mark("queue_present");
        }
        gpuReady(context);
        timing.Mark("release_and_callback");
        TracyZone::MarkFrame();
        if (profile) reportPresents(waitedMs, inFlight);
        CheckFailure();
    } catch (const ProcessShutdown&) {
        throw;
    } catch (...) {
        ReportFailure(std::current_exception());
        throw;
    }
}

void Driver::reportPresents(double waitedMs, std::size_t inFlight) {
    static std::vector<double> waits;
    static VulkanDevice::PresentStatistics previous{};
    static std::uint64_t previousFlips = 0, windowSerial = 0, previousUnsignaled = 0;
    static auto lastReport = std::chrono::steady_clock::now();
    waits.push_back(waitedMs);
    if (waits.size() == 1) windowSerial = flipSerial.load();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport < std::chrono::seconds(10)) return;
    lastReport = now;
    std::sort(waits.begin(), waits.end());
    const auto percentile = [&](double fraction) { return waits.empty() ? 0.0 : waits[std::min(waits.size() - 1, static_cast<std::size_t>(fraction * static_cast<double>(waits.size())))]; };
    const auto counts = VulkanDevice::PresentCounts();
    const auto presents = counts.presents - previous.presents;
    const double perPresent = presents != 0 ? 1.0 / static_cast<double>(presents) : 0.0;
    const auto dropped = counts.acquireDrops - previous.acquireDrops;
    const double perFrame = presents + dropped != 0 ? 1.0 / static_cast<double>(presents + dropped) : 0.0;
    const auto flips = flipsCounted.load() - previousFlips;
    const double perFlip = flips != 0 ? 1.0 / static_cast<double>(flips) : 0.0;
    const auto serial = flipSerial.load();
    AgcDriver::ProfilePrint_nid_no_patch( "[present] %llu presents over 10 s (%llu dropped: no free swapchain image; %zu may trail on the GPU); %s p50 %.2f ms, p90 %.2f ms, max %.2f ms over %zu; per frame, presented or dropped: GPU busy %.1f ms, idle gaps %.1f ms (%llu batches without a completion record); per flip: %.1f batches recorded (%.1f unsignaled at the flip); per present: %.1f batches ahead of the blit, %.1f after it; after the flip packet: last submit %.1f ms, blit submit %.1f ms; in-place reads overwritten before execution: %llu of %llu checked; waits for the display keys' writer outside the GPU mutex: %llu, %.1f ms\n", static_cast<unsigned long long>(presents), static_cast<unsigned long long>(dropped), inFlight, inFlight != 0 ? "inflight_wait" : "render_fence_wait", percentile(0.5), percentile(0.9), waits.empty() ? 0.0 : waits.back(), waits.size(), (counts.gpuBusyMs - previous.gpuBusyMs) * perFrame, (counts.gpuGapMs - previous.gpuGapMs) * perFrame, static_cast<unsigned long long>(counts.gpuUnread - previous.gpuUnread), static_cast<double>(serial - windowSerial) * perFlip, static_cast<double>(flipBatchesUnsignaled.load() - previousUnsignaled) * perFlip, static_cast<double>(counts.batchesAheadOfBlit - previous.batchesAheadOfBlit) * perPresent, static_cast<double>(counts.batchesAfterFlip - previous.batchesAfterFlip) * perPresent, (counts.lastSubmitAfterFlipMs - previous.lastSubmitAfterFlipMs) * perPresent, (counts.blitSubmitAfterFlipMs - previous.blitSubmitAfterFlipMs) * perPresent, static_cast<unsigned long long>(counts.readsOverwritten - previous.readsOverwritten), static_cast<unsigned long long>(counts.readsChecked - previous.readsChecked), static_cast<unsigned long long>(counts.keyWriterWaits - previous.keyWriterWaits), counts.keyWriterWaitMs - previous.keyWriterWaitMs);
    previous = counts;
    previousFlips = flipsCounted.load();
    previousUnsignaled = flipBatchesUnsignaled.load();
    waits.clear();
}

void Driver::ReleaseWindow(void* window) {
    std::lock_guard lock(GuestMemory::GpuMutex());
    if (device && device->Window() == window) device.Reset();
}

}
