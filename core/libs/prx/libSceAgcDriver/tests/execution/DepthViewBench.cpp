#include "prx/libSceAgcDriver/Execution/include/Driver/Memory/DrawWriteRanges.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include <sys/mman.h>
#include "prx/libc/include/Shutdown.hpp"
#include "VulkanTestDevice.hpp"
#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

constexpr VkExtent2D Extent{64, 32};
constexpr std::size_t HtileBytes = 32768;

class GuestBlock {
public:
    GuestBlock() {
        constexpr std::size_t bytes = 65536;
        void* raw = mmap(nullptr, 2 * bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(raw != MAP_FAILED, "depth view bench: cannot map a guest block");
        const auto begin = reinterpret_cast<std::uintptr_t>(raw);
        const auto aligned = (begin + bytes - 1) & ~static_cast<std::uintptr_t>(bytes - 1);
        block = reinterpret_cast<std::uint8_t*>(aligned);
        GuestWriteWatch::GuestWriteWatchRegister_nid_postfix(block, bytes);
        Require(AgcDriver::GuestMemory::Watched(aligned, bytes), "depth view bench: the guest block is not watched");
        GuestAllocations::Mutation().Add(block, bytes, true, true, true);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint64_t Address() const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(block)); }

private:
    std::uint8_t* block = nullptr;
};

GuestBlock& NewBlock() {
    static auto* blocks = new std::deque<GuestBlock>();
    return blocks->emplace_back();
}

class InstructionCounter {
public:
    InstructionCounter() {
#ifdef __linux__
        perf_event_attr attributes{};
        attributes.type = PERF_TYPE_HARDWARE;
        attributes.size = sizeof(attributes);
        attributes.config = PERF_COUNT_HW_INSTRUCTIONS;
        attributes.disabled = 1;
        attributes.exclude_kernel = 1;
        attributes.exclude_hv = 1;
        descriptor = static_cast<int>(syscall(SYS_perf_event_open, &attributes, 0, -1, -1, 0));
#endif
    }

    ~InstructionCounter() {
#ifdef __linux__
        if (descriptor >= 0) close(descriptor);
#endif
    }

    void Start() {
#ifdef __linux__
        if (descriptor < 0) return;
        ioctl(descriptor, PERF_EVENT_IOC_RESET, 0);
        ioctl(descriptor, PERF_EVENT_IOC_ENABLE, 0);
#endif
    }

    void Resume() {
#ifdef __linux__
        if (descriptor >= 0) ioctl(descriptor, PERF_EVENT_IOC_ENABLE, 0);
#endif
    }

    void Pause() {
#ifdef __linux__
        if (descriptor >= 0) ioctl(descriptor, PERF_EVENT_IOC_DISABLE, 0);
#endif
    }

    double Total(std::size_t iterations) {
#ifdef __linux__
        if (descriptor < 0) return -1.0;
        std::uint64_t count = 0;
        if (read(descriptor, &count, sizeof(count)) != static_cast<ssize_t>(sizeof(count))) return -1.0;
        return static_cast<double>(count) / static_cast<double>(iterations);
#else
        static_cast<void>(iterations);
        return -1.0;
#endif
    }

    double Stop(std::size_t iterations) {
#ifdef __linux__
        if (descriptor < 0) return -1.0;
        ioctl(descriptor, PERF_EVENT_IOC_DISABLE, 0);
        std::uint64_t count = 0;
        if (read(descriptor, &count, sizeof(count)) != static_cast<ssize_t>(sizeof(count))) return -1.0;
        return static_cast<double>(count) / static_cast<double>(iterations);
#else
        static_cast<void>(iterations);
        return -1.0;
#endif
    }

private:
    int descriptor = -1;
};

struct Result {
    double nanoseconds = 0;
    double instructions = 0;
};

template<typename Body>
Result Measure(std::size_t iterations, Body&& body) {
    for (std::size_t warm = 0; warm < 64; ++warm) body();
    InstructionCounter counter;
    counter.Start();
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) body();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto instructions = counter.Stop(iterations);
    return {static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) / static_cast<double>(iterations), instructions};
}

template<typename Prepare, typename Body>
Result MeasureInner(std::size_t iterations, Prepare&& prepare, Body&& body) {
    for (std::size_t warm = 0; warm < 64; ++warm) {
        prepare();
        body();
    }
    InstructionCounter counter;
    counter.Start();
    counter.Pause();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        prepare();
        counter.Resume();
        body();
        counter.Pause();
    }
    double instructions = counter.Total(iterations);
    InstructionCounter empty;
    empty.Start();
    empty.Pause();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        empty.Resume();
        empty.Pause();
    }
    instructions -= empty.Total(iterations);
    std::chrono::nanoseconds spent{0};
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        prepare();
        const auto started = std::chrono::steady_clock::now();
        body();
        spent += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
    }
    return {static_cast<double>(spent.count()) / static_cast<double>(iterations), instructions};
}

void Print(const char* label, const Result& result) {
    std::printf("%-58s %9.1f ns %10.1f instructions\n", label, result.nanoseconds, result.instructions);
    if (std::getenv("DEPTH_VIEW_BENCH_TEXT") != nullptr) std::printf("%s\n", TakeDepthViewLapText().c_str());
}

struct Fixture {
    AgcDriver::VulkanDevice& device;
    Context context;
    GuestBlock& htile;
    DepthTarget target;
    std::vector<GuestBlock*> others;
    GuestBlock& colour;

    explicit Fixture(AgcDriver::VulkanDevice& openDevice, std::size_t pending)
        : device(openDevice), context(openDevice.graphicsContext()), htile(NewBlock()), target{NewBlock().Address(), 0, Extent, VK_FORMAT_D32_SFLOAT, 1.0f, 0, htile.Address(), false}, colour(NewBlock()) {
        for (std::size_t index = 0; index < pending; ++index) others.push_back(&NewBlock());
    }
};

std::uint64_t Hits() {
    return DepthViewMemoHits();
}

void PendingFillBench(AgcDriver::VulkanDevice& device, std::size_t iterations, std::size_t pendingWrites) {
    device.WaitIdle();
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    Fixture fixture(device, pendingWrites);
    auto* recorder = Recorder::Active();
    Require(recorder != nullptr, "depth view bench: no active recorder");
    std::array<std::uint32_t, 4> expanded{0xfffc000fu, 0xfffc000fu, 0xfffc000fu, 0xfffc000fu};
    Require(fixture.device.FillBuffer(fixture.htile.Address(), HtileBytes, expanded), "depth view bench: the HTILE fill was not recorded");
    for (auto* other : fixture.others) recorder->NotePendingWrite(other->Address(), 4096);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(DepthSurfaceView(fixture.context, fixture.target) != VK_NULL_HANDLE, "depth view bench: the first bind failed");
    const auto before = Hits();
    const auto result = MeasureInner(iterations, [&] { recorder->NotePendingWrite(fixture.colour.Address(), 4096); }, [&] { DepthSurfaceView(fixture.context, fixture.target); });
    const auto hits = Hits() - before;
    char label[160];
    std::snprintf(label, sizeof(label), "depth view, fill pending over HTILE, %zu other pending writes", pendingWrites);
    Print(label, result);
    Require(hits + 64 >= iterations, "depth view bench: the pending-HTILE calls did not take the memo");
    recorder->Sync();
}

void RewrittenHtile(AgcDriver::VulkanDevice& device, std::size_t iterations) {
    device.WaitIdle();
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    Fixture fixture(device, 0);
    auto* data = reinterpret_cast<std::uint32_t*>(fixture.htile.Address());
    std::fill(data, data + HtileBytes / 4u, 0xfffc000fu);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(DepthSurfaceView(fixture.context, fixture.target) != VK_NULL_HANDLE, "depth view bench: the first bind failed");
    DepthSurfaceView(fixture.context, fixture.target);
    AgcDriver::GuestMemory::MarkWritten(fixture.htile.Address(), HtileBytes);
    const auto before = Hits();
    const auto result = Measure(iterations, [&] { DepthSurfaceView(fixture.context, fixture.target); });
    const auto hits = Hits() - before;
    Print("depth view, HTILE rewritten without a clear", result);
    Require(hits + 64 >= iterations, "depth view bench: the rewritten-HTILE calls did not take the memo");
}

void SteadyHtile(AgcDriver::VulkanDevice& device, std::size_t iterations) {
    device.WaitIdle();
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    Fixture fixture(device, 0);
    auto* data = reinterpret_cast<std::uint32_t*>(fixture.htile.Address());
    std::fill(data, data + HtileBytes / 4u, 0xfffc000fu);
    AgcDriver::GuestMemory::BumpCollectEpoch();
    Require(DepthSurfaceView(fixture.context, fixture.target) != VK_NULL_HANDLE, "depth view bench: the first bind failed");
    DepthSurfaceView(fixture.context, fixture.target);
    const auto result = Measure(iterations, [&] { DepthSurfaceView(fixture.context, fixture.target); });
    Print("depth view, HTILE untouched", result);
}

void OfferRanges(std::size_t iterations) {
    State state{};
    constexpr VkExtent2D extent{1920, 1080};
    state.depth = DepthTarget{0x7f0000000000ull, 0x7f0010000000ull, extent, VK_FORMAT_D32_SFLOAT_S8_UINT, 1.0f, 0, 0x7f0020000000ull, true};
    state.color = {0x7f0030000000ull, extent, VK_FORMAT_R8G8B8A8_UNORM, std::size_t{1920} * 1080 * 4, 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> scratch;
    std::uint64_t sink = 0;
    Print("offer ranges, two computes per draw (depth + colour)", Measure(iterations, [&] {
        AgcDriver::DriverDetail::DrawDepthRanges(state, scratch);
        sink += scratch.size();
        AgcDriver::DriverDetail::DrawDepthRanges(state, scratch);
        sink += scratch.size();
        const auto colour = AgcDriver::DriverDetail::DrawColorRanges(state);
        sink += colour.size();
        const auto again = AgcDriver::DriverDetail::DrawColorRanges(state);
        sink += again.size();
    }));
    std::array<std::pair<std::uint64_t, std::uint64_t>, 3> kept{};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> keptColour;
    Print("offer ranges, one compute per draw and a read of the kept", Measure(iterations, [&] {
        AgcDriver::DriverDetail::DrawDepthRanges(state, scratch);
        std::copy(scratch.begin(), scratch.end(), kept.begin());
        scratch.assign(kept.begin(), kept.begin() + 3);
        sink += scratch.size();
        keptColour = AgcDriver::DriverDetail::DrawColorRanges(state);
        sink += keptColour.size();
    }));
    if (sink == 0) std::puts("unreachable");
}

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const std::size_t iterations = argc > 1 ? static_cast<std::size_t>(std::strtoull(argv[1], nullptr, 10)) : 20000;
        {
            std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
            std::array<std::uint32_t, 4> pattern{};
            GuestBlock& probe = NewBlock();
            if (!device->FillBuffer(probe.Address(), 4096, pattern)) {
                std::puts("skipped depth view bench: the Vulkan device cannot fill imported guest memory");
                return VulkanTestSkipped;
            }
            device->WaitIdle();
        }
        SteadyHtile(*device, iterations);
        RewrittenHtile(*device, iterations);
        PendingFillBench(*device, iterations, 0);
        PendingFillBench(*device, iterations, 256);
        OfferRanges(iterations);
        LibcRunShutdown_nid_postfix();
        std::puts("depth view bench passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { LibcRunShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
