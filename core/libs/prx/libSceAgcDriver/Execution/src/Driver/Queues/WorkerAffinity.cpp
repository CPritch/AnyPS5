#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Queues/WorkerAffinity.hpp"
#include "prx/libSceAgcDriver/Execution/include/TracyZone.hpp"
#include "prx/libc/include/CpuTopology.hpp"
#include "prx/libc/include/general/ThreadName.hpp"
#include <cstdlib>

namespace AgcDriver::DriverDetail {

namespace {

void NameThread(const char* role) {
    TracyZone::NameThread(role);
#ifdef _WIN32
    const HRESULT nameResult = ThreadName::SetCurrent(role);
    if (FAILED(nameResult)) std::fprintf(stderr, "[thread-name] could not name %s (0x%08lx)\n", role, static_cast<unsigned long>(nameResult));
#elif defined(__linux__)
    static_cast<void>(ThreadName::SetCurrent(role));
#endif
}

void PinThread(const char* role, std::uint64_t mask, std::once_flag& summary, const char* threads) {
    if (mask == 0) return;
    std::call_once(summary, [mask, threads] {
        const auto& layout = CpuTopology::Get();
        std::fprintf(stderr, "[affinity] %s -> 0x%llx (hybrid=%d efficient=0x%llx performant=0x%llx process=0x%llx; classes %s)\n", threads, static_cast<unsigned long long>(mask), layout.hybrid ? 1 : 0, static_cast<unsigned long long>(layout.efficient), static_cast<unsigned long long>(layout.performant), static_cast<unsigned long long>(layout.process), layout.classes.empty() ? "none" : layout.classes.c_str());
    });
    CpuTopology::PinTraced(role, nullptr, mask);
}

std::once_flag workerSummary;
std::once_flag committerSummary;

bool PinPlaced(const char* role, std::uint64_t CpuTopology::Placement::*slot) {
    const auto& placement = CpuTopology::GetPlacement();
    if (!placement.Active() || placement.*slot == 0) return false;
    static std::once_flag summary;
    std::call_once(summary, [&placement] {
        const auto& layout = CpuTopology::Get();
        std::fprintf(stderr, "[affinity] placement: queue worker 0x0 -> 0x%llx, draw finisher -> 0x%llx, draw finisher helpers -> 0x%llx, draw committer -> 0x%llx, draw resolver -> 0x%llx, every other thread -> 0x%llx (performant=0x%llx process=0x%llx; classes %s)\n", static_cast<unsigned long long>(placement.worker), static_cast<unsigned long long>(placement.finisher), static_cast<unsigned long long>(placement.finisherHelper), static_cast<unsigned long long>(placement.committer), static_cast<unsigned long long>(placement.resolver), static_cast<unsigned long long>(placement.shared), static_cast<unsigned long long>(layout.performant), static_cast<unsigned long long>(layout.process), layout.classes.empty() ? "none" : layout.classes.c_str());
    });
    CpuTopology::PinTraced(role, nullptr, placement.*slot);
    return true;
}

}

std::uint64_t WorkerAffinityMask() {
    static const std::uint64_t mask = [] {
        if (std::getenv("APS5_NO_WORKER_AFFINITY") != nullptr) return std::uint64_t{0};
        const auto requested = CpuTopology::MaskFromEnvironment("APS5_WORKER_AFFINITY_MASK");
        if (requested != 0) return requested;
#ifndef __linux__
        if (std::getenv("APS5_WORKER_AFFINITY") == nullptr) return std::uint64_t{0};
#endif
        const auto& layout = CpuTopology::Get();
        return layout.hybrid ? layout.performant : std::uint64_t{0};
    }();
    return mask;
}

std::uint64_t CommitterAffinityMask() {
    static const std::uint64_t mask = [] {
        const auto requested = CpuTopology::MaskFromEnvironment("APS5_COMMITTER_AFFINITY_MASK");
        if (requested != 0) return requested;
        return WorkerAffinityMask();
    }();
    return mask;
}

std::uint64_t PreparerAffinityMask() {
    static const std::uint64_t mask = [] {
        const auto requested = CpuTopology::MaskFromEnvironment("APS5_PREPARE_AHEAD_AFFINITY_MASK");
        if (requested != 0) return requested;
        const auto& placement = CpuTopology::GetPlacement();
        if (!placement.Active()) return CpuTopology::Get().process;
        const auto shared = CpuTopology::SharedWithoutNamedPins(placement.shared);
        const auto clear = std::getenv("APS5_NO_ASYNC_COMPILE") == nullptr ? shared & ~CpuTopology::BackgroundCompileMask() : shared;
        return clear != 0 ? clear : shared;
    }();
    return mask;
}

void PinWorkerThread(const char* role) {
    NameThread(role);
    if (PinPlaced(role, &CpuTopology::Placement::shared)) return;
    PinThread(role, WorkerAffinityMask(), workerSummary, "queue workers and presenter");
}

void PinDrawWorkerThread(const char* role) {
    NameThread(role);
    if (PinPlaced(role, &CpuTopology::Placement::worker)) return;
    PinThread(role, WorkerAffinityMask(), workerSummary, "queue workers and presenter");
}

void PinFinisherThread(const char* role) {
    NameThread(role);
    if (PinPlaced(role, &CpuTopology::Placement::finisher)) return;
    PinThread(role, CommitterAffinityMask(), committerSummary, "draw committer and finisher");
}

void PinFinisherHelperThread(const char* role) {
    NameThread(role);
    static std::once_flag summary;
    const auto requested = CpuTopology::MaskFromEnvironment("APS5_FINISHER_HELPER_AFFINITY_MASK");
    if (requested != 0) {
        PinThread(role, requested, summary, "draw finisher helpers");
        return;
    }
    if (PinPlaced(role, &CpuTopology::Placement::finisherHelper)) return;
    PinThread(role, CpuTopology::GetPlacement().Active() ? CpuTopology::GetPlacement().finisher : CommitterAffinityMask(), summary, "draw finisher helpers");
}

void PinCommitterThread(const char* role) {
    NameThread(role);
    if (PinPlaced(role, &CpuTopology::Placement::committer)) return;
    PinThread(role, CommitterAffinityMask(), committerSummary, "draw committer and finisher");
}

void PinResolverThread(const char* role) {
    NameThread(role);
    static std::once_flag summary;
    const auto requested = CpuTopology::MaskFromEnvironment("APS5_RESOLVER_AFFINITY_MASK");
    if (requested != 0) {
        PinThread(role, requested, summary, "draw resolver");
        return;
    }
    if (PinPlaced(role, &CpuTopology::Placement::resolver)) return;
    PinThread(role, PreparerAffinityMask(), summary, "draw resolver");
}

void PinResolverLaneThread(const char* role) {
    NameThread(role);
    static std::once_flag summary;
    const auto requested = CpuTopology::MaskFromEnvironment("APS5_RESOLVER_LANE_AFFINITY_MASK");
    if (requested != 0) {
        PinThread(role, requested, summary, "draw resolver lane");
        return;
    }
    if (PinPlaced(role, &CpuTopology::Placement::resolverSecond)) return;
    PinThread(role, PreparerAffinityMask(), summary, "draw resolver lane");
}

void PinPreparerThread(const char* role) {
    NameThread(role);
    static std::once_flag summary;
    PinThread(role, PreparerAffinityMask(), summary, "draw preparers");
}

}
