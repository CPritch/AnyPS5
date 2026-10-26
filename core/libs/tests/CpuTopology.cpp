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
