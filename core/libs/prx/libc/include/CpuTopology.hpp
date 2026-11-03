#ifndef CORE_LIBS_PRX_LIBC_INCLUDE_CPUTOPOLOGY_HPP
#define CORE_LIBS_PRX_LIBC_INCLUDE_CPUTOPOLOGY_HPP

#include <bit>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <fstream>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

// Hybrid CPU layout (Intel P/E cores) from the OS: which logical processors belong to the lowest and
// the highest efficiency class. Non-hybrid machines report no layout and every pin is a no-op unless
// a mask is given explicitly. Header-only so libkernel and the AGC driver share it without a link.
namespace CpuTopology {

struct Layout {
    std::uint64_t process = 0;
    std::uint64_t efficient = 0;
    std::uint64_t performant = 0;
    bool hybrid = false;
    std::string classes;
};

#ifdef __linux__
inline std::uint64_t MaskFromCpuList(const char* path) {
    std::ifstream file(path);
    std::string text;
    if (!std::getline(file, text)) return 0;
    std::uint64_t mask = 0;
    const char* cursor = text.c_str();
    while (*cursor != '\0') {
        char* end = nullptr;
        const unsigned long first = std::strtoul(cursor, &end, 10);
        if (end == cursor) return 0;
        unsigned long last = first;
        cursor = end;
        if (*cursor == '-') {
            last = std::strtoul(cursor + 1, &end, 10);
            if (end == cursor + 1 || last < first) return 0;
            cursor = end;
        }
        for (unsigned long cpu = first; cpu <= last && cpu < 64; ++cpu) mask |= std::uint64_t{1} << cpu;
        if (*cursor == ',') ++cursor;
        else if (*cursor != '\0') return 0;
    }
    return mask;
}
#endif

inline Layout LayoutFromClasses(const std::uint64_t* byClass, const bool* seen, std::size_t count) {
    Layout result{};
    int lowest = -1;
    int highest = -1;
    for (std::size_t cls = 0; cls < count; ++cls) {
        if (!seen[cls]) continue;
        if (lowest < 0) lowest = static_cast<int>(cls);
        highest = static_cast<int>(cls);
    }
    if (lowest < 0 || lowest == highest) return result;
    for (int cls = 0; cls < highest; ++cls) {
        if (seen[cls]) result.efficient |= byClass[cls];
    }
    result.performant = byClass[highest];
    result.hybrid = result.efficient != 0 && result.performant != 0;
    return result;
}

inline Layout LayoutFromCpuValues(const std::uint64_t* values, std::size_t count, const char* source) {
    const std::size_t cpus = std::min<std::size_t>(count, 64);
    std::vector<std::uint64_t> distinct;
    for (std::size_t cpu = 0; cpu < cpus; ++cpu)
        if (values[cpu] != 0) distinct.push_back(values[cpu]);
    std::sort(distinct.begin(), distinct.end());
    std::vector<std::uint64_t> floors;
    for (const auto value : distinct)
        if (floors.empty() || value * 10 > floors.back() * 11) floors.push_back(value);
    std::uint64_t byClass[64] = {};
    bool seen[64] = {};
    std::uint64_t highest[64] = {};
    for (std::size_t cpu = 0; cpu < cpus; ++cpu) {
        if (values[cpu] == 0) continue;
        const auto cls = static_cast<std::size_t>(std::upper_bound(floors.begin(), floors.end(), values[cpu]) - floors.begin()) - 1;
        byClass[cls] |= std::uint64_t{1} << cpu;
        seen[cls] = true;
        highest[cls] = std::max(highest[cls], values[cpu]);
    }
    auto result = LayoutFromClasses(byClass, seen, floors.size());
    if (!result.hybrid) return result;
    result.classes = source;
    for (std::size_t cls = floors.size(); cls-- > 0;) {
        char text[64];
        std::snprintf(text, sizeof(text), "%s %llu: 0x%llx", cls + 1 == floors.size() ? "" : ",", static_cast<unsigned long long>(highest[cls]), static_cast<unsigned long long>(byClass[cls]));
        result.classes += text;
    }
    return result;
}

#ifdef __linux__
inline std::uint64_t NumberFromFile(const std::string& path) {
    std::ifstream file(path);
    unsigned long long value = 0;
    if (!(file >> value)) return 0;
    return value;
}

inline Layout LayoutFromSysfs(const std::string& root) {
    const auto online = MaskFromCpuList((root + "/online").c_str());
    if (online == 0) return {};
    for (const char* source : {"cpufreq/cpuinfo_max_freq", "acpi_cppc/highest_perf"}) {
        std::uint64_t values[64] = {};
        bool complete = true;
        for (unsigned cpu = 0; cpu < 64 && complete; ++cpu) {
            if (((online >> cpu) & 1u) == 0) continue;
            values[cpu] = NumberFromFile(root + "/cpu" + std::to_string(cpu) + "/" + source);
            complete = values[cpu] != 0;
        }
        if (complete) return LayoutFromCpuValues(values, 64, source);
    }
    return {};
}
#endif

inline const Layout& Get() {
    static const Layout layout = [] {
        Layout result{};
#ifdef _WIN32
        DWORD_PTR processMask = 0;
        DWORD_PTR systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) result.process = static_cast<std::uint64_t>(processMask);
        DWORD length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) return result;
        std::vector<unsigned char> buffer(length);
        if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &length)) return result;
        std::uint64_t byClass[256] = {};
        bool seen[256] = {};
        for (DWORD offset = 0; offset + 8 <= length;) {
            const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
            if (entry->Size == 0 || offset + entry->Size > length) break;
            if (entry->Relationship == RelationProcessorCore && entry->Processor.GroupCount >= 1 && entry->Processor.GroupMask[0].Group == 0) {
                const unsigned cls = entry->Processor.EfficiencyClass;
                byClass[cls] |= static_cast<std::uint64_t>(entry->Processor.GroupMask[0].Mask);
                seen[cls] = true;
            }
            offset += entry->Size;
        }
        const auto classified = LayoutFromClasses(byClass, seen, 256);
        result.efficient = classified.efficient;
        result.performant = classified.performant;
        result.hybrid = classified.hybrid;
#elif defined(__linux__)
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(set), &set) == 0) {
            for (unsigned cpu = 0; cpu < 64; ++cpu)
                if (CPU_ISSET(cpu, &set)) result.process |= std::uint64_t{1} << cpu;
        }
        result.performant = MaskFromCpuList("/sys/devices/cpu_core/cpus");
        result.efficient = MaskFromCpuList("/sys/devices/cpu_atom/cpus");
        result.hybrid = result.efficient != 0 && result.performant != 0 && (result.efficient & result.performant) == 0;
        if (result.hybrid) {
            result.classes = "cpu_core/cpu_atom";
        } else {
            auto classified = LayoutFromSysfs("/sys/devices/system/cpu");
            if (classified.hybrid) {
                result.efficient = classified.efficient;
                result.performant = classified.performant;
                result.hybrid = true;
                result.classes = std::move(classified.classes);
            }
        }
#endif
        return result;
    }();
    return layout;
}

// A hex mask from the environment, 0 when unset or empty.
inline std::uint64_t MaskFromEnvironment(const char* name) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') return 0;
    return std::strtoull(text, nullptr, 16);
}

inline bool Trace() {
    static const bool trace = std::getenv("APS5_TRACE_AFFINITY") != nullptr;
    return trace;
}

inline unsigned long ThreadId(void* handle) {
#ifdef _WIN32
    return handle != nullptr ? GetThreadId(static_cast<HANDLE>(handle)) : GetCurrentThreadId();
#elif defined(__linux__)
    return handle == nullptr ? static_cast<unsigned long>(gettid()) : 0;
#else
    (void)handle;
    return 0;
#endif
}

// Restricts the thread (`nullptr` is the calling thread) to `mask` clipped to the process affinity;
// returns the mask applied, 0 when nothing was.
inline std::uint64_t Pin(void* handle, std::uint64_t mask) {
#ifdef _WIN32
    const auto& layout = Get();
    if (layout.process != 0) mask &= layout.process;
    if (mask == 0) return 0;
    HANDLE thread = handle != nullptr ? static_cast<HANDLE>(handle) : GetCurrentThread();
    return SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(mask)) != 0 ? mask : 0;
#elif defined(__linux__)
    const auto& layout = Get();
    if (layout.process != 0) mask &= layout.process;
    if (mask == 0 || handle != nullptr) return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned cpu = 0; cpu < 64; ++cpu)
        if ((mask >> cpu) & 1u) CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0 ? mask : 0;
#else
    (void)handle;
    (void)mask;
    return 0;
#endif
}

// Pin plus the APS5_TRACE_AFFINITY=1 line naming the thread.
inline std::uint64_t PinTraced(const char* role, void* handle, std::uint64_t mask) {
    const auto applied = Pin(handle, mask);
    if (Trace()) std::fprintf(stderr, "[affinity] %s tid=%lu mask=0x%llx%s\n", role, ThreadId(handle), static_cast<unsigned long long>(mask), applied != 0 ? "" : " (not applied)");
    return applied;
}

struct Placement {
    std::uint64_t worker = 0;
    std::uint64_t finisher = 0;
    std::uint64_t committer = 0;
    std::uint64_t resolver = 0;
    std::uint64_t shared = 0;
    bool Active() const {
        return shared != 0;
    }
};

inline Placement PlacementFor(const Layout& layout, const std::vector<std::uint64_t>& performantCores, bool resolver = false, bool resolverWholeCore = false, bool resolverOwnCore = false) {
    constexpr std::size_t dedicated = 3;
    if (!layout.hybrid || performantCores.size() <= dedicated) return {};
    Placement result{};
    result.worker = performantCores[0];
    result.finisher = performantCores[1];
    result.committer = performantCores[2];
    std::uint64_t reserved = result.worker | result.finisher | result.committer;
    if (resolver && performantCores.size() > dedicated) {
        const auto core = performantCores[dedicated];
        const bool siblings = std::popcount(core) > 1;
        result.resolver = siblings && !resolverWholeCore && !resolverOwnCore ? std::uint64_t{1} << (63 - std::countl_zero(core)) : core;
        if ((siblings && !resolverWholeCore) || resolverOwnCore) reserved |= result.resolver;
    }
    result.shared = layout.process & ~reserved;
    if (result.shared == 0) return {};
    return result;
}

#ifdef __linux__
inline std::vector<std::uint64_t> CoresFromSysfs(const std::string& root, std::uint64_t mask) {
    std::vector<std::uint64_t> cores;
    std::uint64_t seen = 0;
    for (unsigned cpu = 0; cpu < 64; ++cpu) {
        const std::uint64_t bit = std::uint64_t{1} << cpu;
        if ((mask & bit) == 0 || (seen & bit) != 0) continue;
        auto siblings = MaskFromCpuList((root + "/cpu" + std::to_string(cpu) + "/topology/thread_siblings_list").c_str()) & mask;
        if ((siblings & bit) == 0) siblings = bit;
        seen |= siblings;
        cores.push_back(siblings);
    }
    return cores;
}
#endif

inline bool PlacementRequested() {
    for (const char* name : {"APS5_NO_CORE_PLACEMENT", "APS5_NO_WORKER_AFFINITY"})
        if (std::getenv(name) != nullptr) return false;
    return MaskFromEnvironment("APS5_WORKER_AFFINITY_MASK") == 0 && MaskFromEnvironment("APS5_COMMITTER_AFFINITY_MASK") == 0;
}

inline const Placement& GetPlacement() {
    static const Placement placement = [] {
        if (!PlacementRequested()) return Placement{};
#ifdef __linux__
        const auto& layout = Get();
        const char* split = std::getenv("APS5_RESOLVE_SPLIT");
        const bool resolver = (split == nullptr || std::strtoul(split, nullptr, 10) >= 2) && MaskFromEnvironment("APS5_RESOLVER_AFFINITY_MASK") == 0;
        return PlacementFor(layout, CoresFromSysfs("/sys/devices/system/cpu", layout.performant & layout.process), resolver, std::getenv("APS5_RESOLVER_WHOLE_CORE") != nullptr, std::getenv("APS5_RESOLVER_OWN_CORE") != nullptr);
#else
        return Placement{};
#endif
    }();
    return placement;
}

struct NamedPin {
    std::string name;
    std::uint64_t mask = 0;
};

inline std::vector<NamedPin> ParseNamedPins(const char* text) {
    std::vector<NamedPin> pins;
    if (text == nullptr) return pins;
    const std::string all(text);
    std::size_t start = 0;
    while (start < all.size()) {
        const auto end = all.find(',', start);
        const auto item = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto colon = item.rfind(':');
        if (colon != std::string::npos && colon != 0) {
            const auto mask = std::strtoull(item.c_str() + colon + 1, nullptr, 16);
            if (mask != 0) pins.push_back({item.substr(0, colon), mask});
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return pins;
}

inline const std::vector<NamedPin>& NamedPins() {
    static const std::vector<NamedPin> pins = ParseNamedPins(std::getenv("APS5_PIN_GUEST_THREADS"));
    return pins;
}

inline std::uint64_t NamedPinMask(const char* role) {
    if (role == nullptr) return 0;
    for (const auto& pin : NamedPins())
        if (pin.name == role) return pin.mask;
    return 0;
}

inline std::uint64_t SharedWithoutNamedPins(std::uint64_t shared) {
    std::uint64_t reserved = 0;
    for (const auto& pin : NamedPins()) reserved |= pin.mask;
    const auto rest = shared & ~reserved;
    return rest != 0 ? rest : shared;
}

inline void PinHelperThread(const char* role) {
    static const std::uint64_t mask = [] {
        const auto requested = MaskFromEnvironment("APS5_HELPER_AFFINITY_MASK");
        return requested != 0 ? requested : SharedWithoutNamedPins(GetPlacement().shared);
    }();
    if (const auto named = NamedPinMask(role); named != 0) {
        PinTraced(role, nullptr, named);
        return;
    }
    if (mask != 0) PinTraced(role, nullptr, mask);
}

inline void PinGuestThread(const char* role) {
    if (const auto named = NamedPinMask(role); named != 0) {
        PinTraced(role, nullptr, named);
        return;
    }
    const auto mask = SharedWithoutNamedPins(GetPlacement().shared);
    if (mask != 0) PinTraced(role, nullptr, mask);
}

}

#endif
