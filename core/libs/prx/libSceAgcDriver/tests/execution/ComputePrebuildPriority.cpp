#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::array<std::atomic<bool>, 5> Released{};
std::atomic<int> Started{0};
std::atomic<int> Finished{0};
std::atomic<bool> LibraryRan{false};
std::mutex OrderMutex;
std::vector<int> Order;

void Blocking(int index) {
    {
        std::lock_guard lock(OrderMutex);
        Order.push_back(index);
    }
    Started.fetch_add(1);
    while (!Released[static_cast<std::size_t>(index)].load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Finished.fetch_add(1);
}

bool WaitFor(const std::atomic<int>& value, int target, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (value.load() < target) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

std::vector<int> OrderNow() {
    std::lock_guard lock(OrderMutex);
    return Order;
}

void ReleaseAll() {
    for (auto& released : Released) released.store(true);
}

}

int main() {
    try {
        using AgcDriver::DriverDetail::SubmitComputePrebuild;
        SubmitComputePrebuild([] { Blocking(0); });
        SubmitComputePrebuild([] { Blocking(1); });
        Require(WaitFor(Started, 2, std::chrono::seconds(10)), "the prebuild pool did not start two budgeted compute prebuilds");
        SubmitComputePrebuild([] { Blocking(4); });
        SubmitComputePrebuild([] { Blocking(2); }, true);
        SubmitComputePrebuild([] { Blocking(3); }, true);
        AgcDriver::DriverDetail::SubmitLibraryPrebuild([] { LibraryRan.store(true); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!LibraryRan.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool libraryRan = LibraryRan.load();
        const auto whileFull = Started.load();
        Released[0].store(true);
        const bool firstFreed = WaitFor(Started, 3, std::chrono::seconds(10));
        Released[1].store(true);
        const bool secondFreed = WaitFor(Started, 4, std::chrono::seconds(10));
        const auto afterFreed = OrderNow();
        Released[2].store(true);
        const bool lastStarted = WaitFor(Started, 5, std::chrono::seconds(10));
        ReleaseAll();
        Require(WaitFor(Finished, 5, std::chrono::seconds(10)), "the compute prebuilds did not finish");
        Require(libraryRan, "a library prebuild did not run while compute prebuilds held every budgeted slot and awaited ones were queued");
        Require(whileFull == 2, "the prebuild pool ran " + std::to_string(whileFull) + " compute prebuilds at once with APS5_PREBUILD_THREADS=4, past its budgeted cap of 2");
        Require(firstFreed && secondFreed && lastStarted, "a freed budgeted slot was not taken by the next queued compute prebuild");
        Require(afterFreed.size() == 4 && afterFreed[2] != 4 && afterFreed[3] != 4, "a compute prebuild queued ahead of the awaited ones took a freed slot before them");
        std::cout << "compute prebuild priority tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        ReleaseAll();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
