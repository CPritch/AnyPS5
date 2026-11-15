#include "prx/libc/include/CpuTopology.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

static void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "cpu topology test failed: %s\n", what);
    std::exit(1);
}

static void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::trunc);
    file << text;
}

struct FakeCpus {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("anyps5_cpu_topology_test_sysfs-" + std::to_string(std::random_device{}()));
    explicit FakeCpus(const char* online) {
        if (online != nullptr) WriteFile(root / "online", online);
    }
    ~FakeCpus() {
        std::filesystem::remove_all(root);
    }
    void Set(unsigned cpu, const char* source, std::uint64_t value) {
        WriteFile(root / ("cpu" + std::to_string(cpu)) / source, std::to_string(value) + "\n");
    }
    CpuTopology::Layout Read() const {
        return CpuTopology::LayoutFromSysfs(root.string());
    }
};

static constexpr const char* MaxFreq = "cpufreq/cpuinfo_max_freq";
static constexpr const char* HighestPerf = "acpi_cppc/highest_perf";

static bool IsZen5AndZen5c(std::uint64_t cpu) {
    return cpu < 4 || (cpu >= 10 && cpu < 14);
}

static std::uint64_t Parse(const char* text) {
    const auto path = std::filesystem::temp_directory_path() / ("anyps5_cpu_topology_test_cpus-" + std::to_string(std::random_device{}()));
    {
        std::ofstream file(path, std::ios::trunc);
        file << text;
    }
    const auto mask = CpuTopology::MaskFromCpuList(path.c_str());
    std::filesystem::remove(path);
    return mask;
}

static void AddCores(std::vector<CpuTopology::CpuSet>& sets, unsigned cores, unsigned threads, std::uint8_t efficiencyClass, std::uint8_t cache, std::uint16_t group = 0) {
    unsigned used = 0;
    for (const auto& set : sets) used += set.group == group ? 1u : 0u;
    for (unsigned core = 0; core < cores; ++core) {
        const auto first = static_cast<std::uint8_t>(used + core * threads);
        for (unsigned thread = 0; thread < threads; ++thread) {
            const auto logical = static_cast<std::uint8_t>(first + thread);
            sets.push_back({256u + static_cast<std::uint32_t>(sets.size()), group, logical, first, efficiencyClass, cache, false});
        }
    }
}

static std::vector<std::uint32_t> IdsExcept(const std::vector<CpuTopology::CpuSet>& sets, std::initializer_list<unsigned> excluded) {
    std::vector<std::uint32_t> ids;
    for (const auto& set : sets) {
        bool left = false;
        for (const auto logical : excluded) left = left || (set.group == 0 && set.logical == logical);
        if (!left) ids.push_back(set.id);
    }
    return ids;
}

static void ReservedCoreTests() {
    using CpuTopology::ChooseReservedCore;
    std::vector<CpuTopology::CpuSet> eight;
    AddCores(eight, 8, 2, 0, 0);
    const auto committer = ChooseReservedCore(eight, {}, std::nullopt);
    Require(committer.has_value() && committer->chosen.logical == 14 && committer->chosen.core == 14, "the first logical processor of the highest-numbered core is reserved");
    Require(committer->others == IdsExcept(eight, {14, 15}), "the other threads keep the CPU sets of every other core");
    const auto finisher = ChooseReservedCore(eight, {committer->chosen}, std::nullopt);
    Require(finisher.has_value() && finisher->chosen.logical == 12, "a second reservation takes the next core");
    Require(finisher->others == IdsExcept(eight, {12, 13, 14, 15}), "a second reservation keeps the first one out of the other CPU sets");
    std::vector<CpuTopology::CpuSet> everyCore;
    for (const auto& set : eight) {
        if (set.logical == set.core) everyCore.push_back(set);
    }
    Require(!ChooseReservedCore(eight, everyCore, std::nullopt).has_value(), "no core is left once every core is reserved");

    auto parked = eight;
    parked[15].parked = true;
    const auto unparked = ChooseReservedCore(parked, {}, std::nullopt);
    Require(unparked.has_value() && unparked->chosen.logical == 12, "a core with a parked logical processor is skipped");
    Require(unparked->others == IdsExcept(parked, {12, 13}), "a parked core stays in the other CPU sets");

    std::vector<CpuTopology::CpuSet> smtOff;
    AddCores(smtOff, 8, 1, 0, 0);
    const auto single = ChooseReservedCore(smtOff, {}, std::nullopt);
    Require(single.has_value() && single->chosen.logical == 7 && single->chosen.core == 7, "a host without SMT reserves its highest-numbered core");
    Require(single->others == IdsExcept(smtOff, {7}), "a host without SMT leaves every other logical processor to the other threads");

    std::vector<CpuTopology::CpuSet> five;
    AddCores(five, 5, 2, 0, 0);
    Require(!ChooseReservedCore(five, {}, std::nullopt).has_value(), "a host with fewer than six cores reserves none");
    const auto requested = ChooseReservedCore(five, {}, 3u);
    Require(requested.has_value() && requested->chosen.logical == 3 && requested->chosen.core == 2, "a requested logical processor picks its core on any host");
    Require(requested->others == IdsExcept(five, {2, 3}), "a requested core leaves both of its logical processors");
    Require(!ChooseReservedCore(five, {requested->chosen}, 2u).has_value(), "a requested core that is already reserved is refused");
    Require(!ChooseReservedCore(five, {}, 10u).has_value(), "a requested logical processor the host lacks is refused");
    std::vector<CpuTopology::CpuSet> one;
    AddCores(one, 1, 2, 0, 0);
    Require(!ChooseReservedCore(one, {}, 0u).has_value(), "a reservation that leaves the other threads no CPU set is refused");

    std::vector<CpuTopology::CpuSet> hybrid;
    AddCores(hybrid, 6, 2, 1, 0);
    AddCores(hybrid, 8, 1, 0, 0);
    const auto performance = ChooseReservedCore(hybrid, {}, std::nullopt);
    Require(performance.has_value() && performance->chosen.logical == 10 && performance->chosen.efficiencyClass == 1, "a hybrid host reserves its highest-numbered performance core");
    Require(performance->others == IdsExcept(hybrid, {10, 11}), "a hybrid host leaves its efficiency cores to the other threads");
    std::vector<CpuTopology::CpuSet> fewPerformance;
    AddCores(fewPerformance, 4, 2, 1, 0);
    AddCores(fewPerformance, 8, 1, 0, 0);
    Require(!ChooseReservedCore(fewPerformance, {}, std::nullopt).has_value(), "a hybrid host with fewer than six performance cores reserves none");
    auto performanceParked = hybrid;
    for (std::size_t index = 0; index < 12; ++index) performanceParked[index].parked = true;
    Require(!ChooseReservedCore(performanceParked, {}, std::nullopt).has_value(), "a hybrid host with every performance core parked reserves none");

    std::vector<CpuTopology::CpuSet> dual;
    AddCores(dual, 8, 2, 0, 0);
    AddCores(dual, 8, 2, 0, 1);
    const auto firstCache = ChooseReservedCore(dual, {}, std::nullopt);
    Require(firstCache.has_value() && firstCache->chosen.logical == 14, "two last-level caches reserve within the cache of the first logical processor");
    Require(firstCache->others == IdsExcept(dual, {14, 15}), "two last-level caches leave the other cache to the other threads");
    auto secondParked = dual;
    for (std::size_t index = 16; index < secondParked.size(); ++index) secondParked[index].parked = true;
    const auto cacheKept = ChooseReservedCore(secondParked, {}, std::nullopt);
    Require(cacheKept.has_value() && cacheKept->chosen.logical == 14, "a parked second cache leaves the reservation in the first");
    auto firstParked = dual;
    for (std::size_t index = 0; index < 16; ++index) firstParked[index].parked = true;
    const auto secondCache = ChooseReservedCore(firstParked, {}, std::nullopt);
    Require(secondCache.has_value() && secondCache->chosen.logical == 30 && secondCache->chosen.lastLevelCache == 1, "a parked first cache moves the reservation to the unparked one");
    std::vector<CpuTopology::CpuSet> quads;
    AddCores(quads, 4, 2, 0, 0);
    AddCores(quads, 4, 2, 0, 1);
    Require(!ChooseReservedCore(quads, {}, std::nullopt).has_value(), "fewer than six cores sharing the first cache reserve none");

    std::vector<CpuTopology::CpuSet> groupOneFirst;
    AddCores(groupOneFirst, 8, 2, 0, 0, 1);
    AddCores(groupOneFirst, 8, 2, 0, 0);
    const auto firstGroup = ChooseReservedCore(groupOneFirst, {}, std::nullopt);
    Require(firstGroup.has_value() && firstGroup->chosen.group == 0 && firstGroup->chosen.logical == 14, "two processor groups reserve in group 0");
    Require(firstGroup->others == IdsExcept(groupOneFirst, {14, 15}), "group 1 keeps the core numbers of the reserved core in the other CPU sets");
    std::vector<CpuTopology::CpuSet> groupZeroFirst;
    AddCores(groupZeroFirst, 8, 2, 0, 0);
    AddCores(groupZeroFirst, 8, 2, 0, 0, 1);
    const auto requestedGroup = ChooseReservedCore(groupZeroFirst, {}, 3u);
    Require(requestedGroup.has_value() && requestedGroup->chosen.group == 0 && requestedGroup->chosen.logical == 3, "a requested logical processor is one of group 0");
}
#ifdef __linux__
static void WriteNumber(const std::filesystem::path& path, unsigned long value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::trunc) << value << "\n";
}

static std::filesystem::path FakeSysfs(unsigned packages, unsigned cachesPerPackage, unsigned coresPerCache, bool smt) {
    const auto root = std::filesystem::temp_directory_path() / ("anyps5_cpu_topology_sysfs-" + std::to_string(std::random_device{}()));
    const unsigned cores = packages * cachesPerPackage * coresPerCache;
    const unsigned logicals = smt ? cores * 2 : cores;
    for (unsigned cpu = 0; cpu < logicals; ++cpu) {
        const unsigned core = cpu % cores;
        const auto base = root / ("cpu" + std::to_string(cpu));
        WriteNumber(base / "topology" / "core_id", core % (cachesPerPackage * coresPerCache));
        WriteNumber(base / "topology" / "physical_package_id", core / (cachesPerPackage * coresPerCache));
        WriteNumber(base / "cache" / "index0" / "level", 1);
        WriteNumber(base / "cache" / "index0" / "id", core);
        WriteNumber(base / "cache" / "index2" / "level", 2);
        WriteNumber(base / "cache" / "index2" / "id", core);
        WriteNumber(base / "cache" / "index3" / "level", 3);
        WriteNumber(base / "cache" / "index3" / "id", 100 + core / coresPerCache);
    }
    return root;
}

static void SysfsTests() {
    const auto single = FakeSysfs(1, 1, 8, true);
    const auto sets = CpuTopology::SysfsCpuSets(single.string(), 0xffffull, 0);
    Require(sets.size() == 16, "every logical processor of the process is listed");
    Require(sets[3].id == 3 && sets[3].logical == 3 && sets[3].core == 3 && sets[11].core == 3, "SMT siblings share a core");
    Require(sets[0].lastLevelCache == 0 && sets[15].lastLevelCache == 0 && sets[0].efficiencyClass == 1, "one cache, one class");
    const auto committer = CpuTopology::ChooseReservedCore(sets, {}, std::nullopt);
    Require(committer.has_value() && committer->chosen.logical == 7 && committer->others.size() == 14, "the committer takes the highest core and leaves its sibling idle");
    const auto finisher = CpuTopology::ChooseReservedCore(sets, {committer->chosen}, std::nullopt);
    Require(finisher.has_value() && finisher->chosen.logical == 6 && finisher->others.size() == 12, "the finisher takes the next core");
    Require(CpuTopology::PipelineCacheSet(sets, {committer->chosen, finisher->chosen}).empty(), "one last-level cache leaves the queue workers alone");
    const auto masked = CpuTopology::SysfsCpuSets(single.string(), 0x7f7full, 0);
    Require(masked.size() == 14 && masked.back().id == 14, "processors outside the process are left out");
    const auto hybrid = CpuTopology::SysfsCpuSets(single.string(), 0xffffull, 0xf0f0ull);
    Require(hybrid[4].efficiencyClass == 0 && hybrid[3].efficiencyClass == 1, "efficient processors get the lower class");
    std::filesystem::remove(single / "cpu5" / "topology" / "core_id");
    Require(CpuTopology::SysfsCpuSets(single.string(), 0xffffull, 0).empty(), "a processor without a core id lists nothing");
    std::filesystem::remove_all(single);

    const auto dual = FakeSysfs(1, 2, 8, true);
    const auto dualSets = CpuTopology::SysfsCpuSets(dual.string(), 0xffffffffull, 0);
    Require(dualSets.size() == 32 && dualSets[8].lastLevelCache == 1 && dualSets[16].lastLevelCache == 0, "two caches are told apart");
    const auto first = CpuTopology::ChooseReservedCore(dualSets, {}, std::nullopt);
    Require(first.has_value() && first->chosen.logical == 7 && first->chosen.lastLevelCache == 0, "two caches reserve in the cache of processor 0");
    const auto second = CpuTopology::ChooseReservedCore(dualSets, {first->chosen}, std::nullopt);
    Require(second.has_value() && second->chosen.logical == 6, "the finisher stays in the same cache");
    const auto workers = CpuTopology::PipelineCacheSet(dualSets, {first->chosen, second->chosen});
    Require(workers == std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 16, 17, 18, 19, 20, 21}, "queue workers keep the other processors of the reserved cache");
    std::filesystem::remove_all(dual);

    const auto sockets = FakeSysfs(2, 1, 8, false);
    const auto socketSets = CpuTopology::SysfsCpuSets(sockets.string(), 0xffffull, 0);
    Require(socketSets[0].core != socketSets[8].core, "equal core ids in two packages are different cores");
    std::filesystem::remove_all(sockets);
}
#endif

int main() {
    Require(Parse("0-15\n") == 0xffffull, "range");
    Require(Parse("16-31\n") == 0xffff0000ull, "high range");
    Require(Parse("0,2,4-5\n") == 0x35ull, "list of cpus and ranges");
    Require(Parse("3") == 0x8ull, "single cpu without a newline");
    Require(Parse("62-70\n") == 0xc000000000000000ull, "cpus past 63 are left out");
    Require(Parse("") == 0, "empty list");
    Require(Parse("4-2\n") == 0, "reversed range");
    Require(Parse("0-x\n") == 0, "malformed range");
    Require(Parse("0;1\n") == 0, "malformed separator");
    Require(CpuTopology::MaskFromCpuList("/nonexistent/anyps5/cpus") == 0, "missing file");

    {
        std::uint64_t byClass[4] = {};
        bool seen[4] = {};
        const auto empty = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(!empty.hybrid && empty.efficient == 0 && empty.performant == 0, "empty classes");

        byClass[0] = 0xff;
        seen[0] = true;
        const auto single = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(!single.hybrid && single.efficient == 0 && single.performant == 0, "single class");

        byClass[1] = 0xff00;
        seen[1] = true;
        const auto twoTier = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(twoTier.hybrid && twoTier.efficient == 0xff && twoTier.performant == 0xff00, "two-tier classes");

        byClass[2] = 0xff0000;
        seen[2] = true;
        const auto threeTier = CpuTopology::LayoutFromClasses(byClass, seen, 4);
        Require(threeTier.hybrid && threeTier.efficient == 0xffff && threeTier.performant == 0xff0000, "three-tier classes aggregate efficient");
    }

    {
        std::uint64_t values[20] = {};
        for (unsigned cpu = 0; cpu < 20; ++cpu) values[cpu] = IsZen5AndZen5c(cpu) ? 5090910 : 3350650;
        const auto zen5 = CpuTopology::LayoutFromCpuValues(values, 20, MaxFreq);
        Require(zen5.hybrid && zen5.performant == 0x3c0full && zen5.efficient == 0xfc3f0ull, "Zen5 and Zen5c by max frequency");
        Require(zen5.classes == "cpufreq/cpuinfo_max_freq 5090910: 0x3c0f, 3350650: 0xfc3f0", "classes named");

        const std::uint64_t ranking[4] = {196, 202, 208, 208};
        for (unsigned cpu = 0; cpu < 20; ++cpu) values[cpu] = IsZen5AndZen5c(cpu) ? ranking[cpu % 10] : 129;
        const auto ranked = CpuTopology::LayoutFromCpuValues(values, 20, HighestPerf);
        Require(ranked.hybrid && ranked.performant == 0x3c0full && ranked.efficient == 0xfc3f0ull, "preferred-core ranking within a core type is one class");
        Require(ranked.classes == "acpi_cppc/highest_perf 208: 0x3c0f, 129: 0xfc3f0", "class named by its highest value");

        for (unsigned cpu = 0; cpu < 16; ++cpu) values[cpu] = cpu == 2 || cpu == 3 ? 5750000 : 5500000;
        const auto favoured = CpuTopology::LayoutFromCpuValues(values, 16, MaxFreq);
        Require(!favoured.hybrid && favoured.performant == 0 && favoured.efficient == 0 && favoured.classes.empty(), "favoured cores of one type are not a class");

        for (unsigned cpu = 0; cpu < 16; ++cpu) values[cpu] = 4000000;
        Require(!CpuTopology::LayoutFromCpuValues(values, 16, MaxFreq).hybrid, "uniform cpus");
        Require(!CpuTopology::LayoutFromCpuValues(values, 0, MaxFreq).hybrid, "no cpus");

        const std::uint64_t tiers[6] = {3000000, 0, 4000000, 5000000, 5100000, 3000000};
        const auto threeTier = CpuTopology::LayoutFromCpuValues(tiers, 6, MaxFreq);
        Require(threeTier.hybrid && threeTier.performant == 0x18ull && threeTier.efficient == 0x25ull, "three classes, unknown cpus left out");
    }

    {
        FakeCpus cpus("0-3\n");
        for (unsigned cpu = 0; cpu < 4; ++cpu) cpus.Set(cpu, MaxFreq, cpu < 2 ? 5090910 : 3350650);
        cpus.Set(4, MaxFreq, 9000000);
        const auto read = cpus.Read();
        Require(read.hybrid && read.performant == 0x3ull && read.efficient == 0xcull, "sysfs max frequencies of the online cpus");
        Require(read.classes == "cpufreq/cpuinfo_max_freq 5090910: 0x3, 3350650: 0xc", "sysfs classes named");
    }

    Require(!FakeCpus("0-1\n").Read().hybrid, "online cpus without values");

    {
        FakeCpus cpus("0-3\n");
        for (unsigned cpu = 0; cpu < 4; ++cpu) cpus.Set(cpu, HighestPerf, cpu % 2 == 0 ? 208 : 129);
        for (unsigned cpu = 0; cpu < 3; ++cpu) cpus.Set(cpu, MaxFreq, 3350650);
        const auto cppc = cpus.Read();
        Require(cppc.hybrid && cppc.performant == 0x5ull && cppc.efficient == 0xaull && cppc.classes.rfind(HighestPerf, 0) == 0, "highest_perf when a cpu has no max frequency");

        cpus.Set(3, MaxFreq, 3350650);
        Require(!cpus.Read().hybrid, "max frequency decides when every cpu has one");
    }

    Require(!FakeCpus(nullptr).Read().hybrid, "no online list");

    {
        FakeCpus cpus("0-19\n");
        for (unsigned cpu = 0; cpu < 20; ++cpu) {
            const unsigned core = cpu % 10;
            WriteFile(cpus.root / ("cpu" + std::to_string(cpu)) / "topology" / "thread_siblings_list", std::to_string(core) + "," + std::to_string(core + 10) + "\n");
        }
        const auto cores = CpuTopology::CoresFromSysfs(cpus.root.string(), 0x3c0full);
        Require(cores == std::vector<std::uint64_t>{0x401ull, 0x802ull, 0x1004ull, 0x2008ull}, "performant cores grouped by SMT siblings in cpu order");
        Require(CpuTopology::CoresFromSysfs(cpus.root.string(), 0x0c03ull) == std::vector<std::uint64_t>{0x401ull, 0x802ull}, "siblings outside the mask are left out");
        Require(CpuTopology::CoresFromSysfs(cpus.root.string(), 0x3ull) == std::vector<std::uint64_t>{0x1ull, 0x2ull}, "a core with one sibling in the mask");

        CpuTopology::Layout zen5{};
        zen5.process = 0xfffffull;
        zen5.performant = 0x3c0full;
        zen5.efficient = 0xfc3f0ull;
        zen5.hybrid = true;
        const std::vector<std::uint64_t> efficientCores{0x4010ull, 0x8020ull, 0x10040ull, 0x20080ull, 0x40100ull, 0x80200ull};
        Require(CpuTopology::BackgroundCompileMaskFor(efficientCores) == 0xe0380ull, "background compiles on the later half of the efficient cores");
        Require(CpuTopology::BackgroundCompileMaskFor({}) == 0ull, "no efficient cores give no background compile mask");
        const auto placement = CpuTopology::PlacementFor(zen5, cores);
        Require(placement.Active() && placement.worker == 0x401ull && placement.finisher == 0x802ull && placement.committer == 0x1004ull, "a performant core each for the worker, finisher and committer");
        Require(placement.shared == 0xfe3f8ull, "every other thread on the fourth performant core and the efficient cores");
        const auto split = CpuTopology::PlacementFor(zen5, cores, true);
        Require(split.resolver == 0x2000ull && split.committer == 0x1004ull, "the resolver on the fourth performant core's second sibling");
        Require(split.shared == 0xfc3f8ull, "every other thread keeps the fourth performant core's first sibling and the efficient cores");
        const auto whole = CpuTopology::PlacementFor(zen5, cores, true, true);
        Require(whole.resolver == 0x2008ull && whole.shared == 0xfe3f8ull, "a whole-core resolver shares its core with every other thread");
        const auto own = CpuTopology::PlacementFor(zen5, cores, true, false, true);
        Require(own.resolver == 0x2008ull && own.shared == 0xfc3f0ull, "an own-core resolver takes the whole fourth performant core from every other thread");
        const auto lanes = CpuTopology::PlacementFor(zen5, cores, true, false, false, true);
        Require(lanes.resolver == 0x2000ull && lanes.resolverSecond == 0x8ull && lanes.shared == 0xfc3f0ull, "two resolver lanes take the fourth performant core's two siblings from every other thread");
        Require(CpuTopology::PlacementFor(zen5, {0x401ull, 0x802ull, 0x1004ull, 0x8ull}, true, false, false, true).resolverSecond == 0, "a resolver core without a second sibling has no second lane core");
        const auto helped = CpuTopology::PlacementFor(zen5, cores, true, false, false, false, true);
        Require(helped.finisher == 0x2ull && helped.finisherHelper == 0x800ull && helped.resolver == 0x2000ull && (helped.shared & 0x8ull) != 0 && (helped.shared & 0x802ull) == 0, "a finisher helper takes the finisher core's second sibling and leaves the fourth performant core as it was");
        const auto helpless = CpuTopology::PlacementFor(zen5, {0x401ull, 0x2ull, 0x1004ull, 0x2008ull}, true, false, false, false, true);
        Require(helpless.finisher == 0x2ull && helpless.finisherHelper == 0, "a finisher core without a second sibling has no helper core");
        const auto single = CpuTopology::PlacementFor(zen5, {0x401ull, 0x802ull, 0x1004ull, 0x8ull}, true);
        Require(single.resolver == 0x8ull && (single.shared & 0x8ull) != 0, "a resolver core without a second sibling stays shared");
        const auto pins = CpuTopology::ParseNamedPins("RenderThread:2008,bad,:4,Other:0,Main Thread:30");
        Require(pins.size() == 2 && pins[0].name == "RenderThread" && pins[0].mask == 0x2008ull && pins[1].name == "Main Thread" && pins[1].mask == 0x30ull, "named pins parse name:hexmask pairs and skip malformed ones");
        Require(CpuTopology::ParseNamedPins(nullptr).empty() && CpuTopology::ParseNamedPins("").empty(), "no named pins without the switch");

        Require(!CpuTopology::PlacementFor(zen5, {0x401ull, 0x802ull, 0x1004ull}).Active(), "no placement without a performant core left to share");
        auto uniform = zen5;
        uniform.hybrid = false;
        Require(!CpuTopology::PlacementFor(uniform, cores).Active(), "no placement on uniform cores");
        auto narrow = zen5;
        narrow.process = 0x1c07ull;
        Require(!CpuTopology::PlacementFor(narrow, cores).Active(), "no placement when the process has no other cpu");
    }
    {
        FakeCpus cpus("0-3\n");
        Require(CpuTopology::CoresFromSysfs(cpus.root.string(), 0xfull) == std::vector<std::uint64_t>{0x1ull, 0x2ull, 0x4ull, 0x8ull}, "cpus without topology are cores of their own");
    }

    ReservedCoreTests();
#ifdef __linux__
    SysfsTests();
#endif

    const auto& layout = CpuTopology::Get();
    Require(layout.process != 0, "process affinity read");
    Require(!layout.hybrid || (layout.performant & layout.efficient) == 0, "hybrid classes overlap");
    const std::uint64_t first = layout.process & (~layout.process + 1);
    Require(CpuTopology::Pin(nullptr, first) == first, "pin to one cpu of the process");
    cpu_set_t set;
    CPU_ZERO(&set);
    Require(pthread_getaffinity_np(pthread_self(), sizeof(set), &set) == 0, "read thread affinity");
    Require(CPU_COUNT(&set) == 1 && CPU_ISSET(static_cast<unsigned>(__builtin_ctzll(first)), &set), "thread runs on the pinned cpu only");
    Require(CpuTopology::Pin(nullptr, ~layout.process) == 0, "mask outside the process is not applied");
    int dummy = 0;
    Require(CpuTopology::Pin(&dummy, first) == 0, "other threads are not pinned by handle");
    return 0;
}
