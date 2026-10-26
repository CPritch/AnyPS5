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

void PinWorkerThread(const char* role) {
    NameThread(role);
    static std::once_flag summary;
    PinThread(role, WorkerAffinityMask(), summary, "queue workers and presenter");
}

void PinCommitterThread(const char* role) {
    NameThread(role);
    static std::once_flag summary;
    PinThread(role, CommitterAffinityMask(), summary, "draw committer and finisher");
}

}
