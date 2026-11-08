#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

namespace Graphics = AgcDriver::Graphics;
using Graphics::Require;

constexpr std::uint64_t MiB = 1ull << 20u;
constexpr std::uint64_t Window = 8 * MiB;
constexpr std::uint64_t Windows = 2;
constexpr std::uint64_t Mapping = 256 * 1024;
constexpr std::uint64_t ProbeBytes = 4096;

std::atomic<bool> inWindow{false};
std::atomic<bool> released{false};
std::atomic<bool> timedOut{false};

void WaitInWindow() {
    inWindow.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!released.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!released.load(std::memory_order_acquire)) timedOut.store(true, std::memory_order_release);
}

std::string Hex(std::uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

class Arena {
public:
    Arena() : base(static_cast<std::uint8_t*>(std::aligned_alloc(Window, Windows * Window))) {
        Require(base != nullptr, "map import outside the lock: cannot allocate the guest arena");
        std::memset(base, 0, Windows * Window);
    }
    ~Arena() { std::free(base); }
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    std::uint8_t* At(std::uint64_t offset) const { return base + offset; }
    std::uint64_t Address(std::uint64_t offset) const { return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base)) + offset; }

private:
    std::uint8_t* base;
};

bool Fill(AgcDriver::VulkanDevice& device, std::uint64_t address) {
    const std::array<std::uint32_t, 4> pattern{0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
    std::lock_guard lock(AgcDriver::GuestMemory::GpuMutex());
    const bool filled = device.FillBuffer(address, ProbeBytes, pattern);
    device.WaitIdle();
    return filled;
}

GuestAllocations::Mapped MappedAt(std::uint64_t address) {
    GuestAllocations::Mapped mapped;
    for (const auto& range : GuestAllocations::GuestAllocationsAcquireRegistered_nid_postfix()) {
        if (range->address == address) mapped.push_back(range);
    }
    return mapped;
}

enum class Outcome : std::uint8_t { NoDevice, NoImport, Unwatched, Passed };

struct MapRun {
    bool reachedWindow = false;
    bool timedOut = false;
};

template<typename During>
MapRun ImportAtMap(const Graphics::Context& context, std::uint64_t address, const During& during) {
    inWindow.store(false);
    released.store(false);
    timedOut.store(false);
    const auto mapped = MappedAt(address);
    Require(mapped.size() == 1, "map import outside the lock: the mapping at " + Hex(address) + " is not registered");
    const auto generation = GuestAllocations::GuestAllocationsGeneration_nid_postfix();
    Graphics::SetMapImportHookForTest(&WaitInWindow);
    std::thread mapper([&] { static_cast<void>(Graphics::ImportMappedRanges(context, mapped, generation, false)); });
    const bool unlocked = std::getenv("APS5_NO_UNLOCKED_MAP_IMPORT") == nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(unlocked ? 10 : 1);
    while (!inWindow.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    MapRun run;
    run.reachedWindow = inWindow.load(std::memory_order_acquire);
    if (run.reachedWindow) during();
    released.store(true, std::memory_order_release);
    mapper.join();
    Graphics::SetMapImportHookForTest(nullptr);
    run.timedOut = timedOut.load(std::memory_order_acquire);
    return run;
}

Outcome MapImportsCreateOutsideTheLock() {
    Arena arena;
    auto device = OpenVulkanTestDevice();
    if (!device) return Outcome::NoDevice;
    const auto context = device->graphicsContext();
    const auto other = arena.Address(Window);
    const auto serial = [&](std::uint64_t address) { return Graphics::HostImportSerial(context, address, ProbeBytes, false); };
    const bool unlocked = std::getenv("APS5_NO_UNLOCKED_MAP_IMPORT") == nullptr;
    GuestAllocations::Mutation().Add(arena.At(Window), Window / 2, true, true, true);
    if (!Fill(*device, other) || serial(other) == 0) return Outcome::NoImport;

    const auto first = arena.Address(0);
    GuestAllocations::Mutation().Add(arena.At(0), Mapping, true, true, true);
    auto before = Graphics::ImportChurnTotals();
    std::uint64_t probedSerial = 0;
    auto run = ImportAtMap(context, first, [&] { probedSerial = serial(other); });
    auto after = Graphics::ImportChurnTotals();
    if (!unlocked) {
        Require(!run.reachedWindow && after.mapUnlocked == before.mapUnlocked, "map import outside the lock: with APS5_NO_UNLOCKED_MAP_IMPORT the import at map still released the lock");
    } else {
        if (!run.reachedWindow && after.mapUnlocked == before.mapUnlocked) {
            GuestAllocations::Mutation().Remove(arena.At(0));
            GuestAllocations::Mutation().Remove(arena.At(Window));
            return Outcome::Unwatched;
        }
        Require(run.reachedWindow && !run.timedOut && probedSerial != 0, "map import outside the lock: another thread could not take the host-import lock while the import at map created its import");
        Require(after.mapUnlocked > before.mapUnlocked && after.mapDiscarded == before.mapDiscarded, "map import outside the lock: the import at map was not counted as made outside the lock, or was discarded");
    }
    Require(serial(first) != 0, "map import outside the lock: the mapping at " + Hex(first) + " was not imported");

    const auto second = arena.Address(2 * MiB);
    GuestAllocations::Mutation().Add(arena.At(2 * MiB), Mapping, true, true, true);
    before = Graphics::ImportChurnTotals();
    std::uint64_t madeSerial = 0;
    run = ImportAtMap(context, second, [&] {
        static_cast<void>(Graphics::HostImportFor(context, second, ProbeBytes, Graphics::ImportRequester::Build));
        madeSerial = serial(second);
    });
    after = Graphics::ImportChurnTotals();
    if (unlocked) {
        Require(run.reachedWindow && !run.timedOut && madeSerial != 0, "map import outside the lock: another thread could not import the mapping while the import at map was outside the lock");
        Require(after.mapDiscarded > before.mapDiscarded && serial(second) == madeSerial, "map import outside the lock: the import at map did not give way to the import made meanwhile over its bytes");
    }
    Require(serial(second) != 0, "map import outside the lock: the mapping at " + Hex(second) + " was not imported");

    GuestAllocations::Mutation().Remove(arena.At(0));
    GuestAllocations::Mutation().Remove(arena.At(2 * MiB));
    GuestAllocations::Mutation().Remove(arena.At(Window));
    Require(Graphics::HostImportSerial(context, first, ProbeBytes, true) == 0 && serial(second) == 0, "map import outside the lock: imports outlived their mappings");
    return Outcome::Passed;
}

}

int main() {
    try {
        const auto outcome = MapImportsCreateOutsideTheLock();
        if (outcome == Outcome::NoDevice) return VulkanTestSkipped;
        if (outcome == Outcome::NoImport) {
            std::printf("skipped, the device does not import guest memory\n");
            return VulkanTestSkipped;
        }
        if (outcome == Outcome::Unwatched) {
            std::printf("skipped, imports at map do not keep their pages watched on this device, so they stay under the lock\n");
            return VulkanTestSkipped;
        }
        std::puts("map import outside the lock tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
