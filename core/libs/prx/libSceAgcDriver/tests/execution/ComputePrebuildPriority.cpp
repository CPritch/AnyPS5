#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::atomic<bool> Released{false};
std::atomic<int> BudgetedStarted{0};
std::atomic<int> BudgetedFinished{0};
std::atomic<bool> AwaitedRan{false};

bool WaitFor(const std::atomic<int>& value, int target, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (value.load() < target) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}

int main() {
    try {
        constexpr int Budgeted = 4;
        for (int task = 0; task < Budgeted; ++task) {
            AgcDriver::DriverDetail::SubmitComputePrebuild([] {
                BudgetedStarted.fetch_add(1);
                while (!Released.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                BudgetedFinished.fetch_add(1);
            });
        }
        Require(WaitFor(BudgetedStarted, 2, std::chrono::seconds(10)), "the prebuild pool did not start two budgeted compute prebuilds");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Require(BudgetedStarted.load() == 2, "the prebuild pool ran " + std::to_string(BudgetedStarted.load()) + " budgeted compute prebuilds at once with APS5_PREBUILD_THREADS=4, expected its cap of 2");
        AgcDriver::DriverDetail::SubmitComputePrebuild([] { AwaitedRan.store(true); }, true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!AwaitedRan.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const bool awaitedFirst = AwaitedRan.load();
        Released.store(true);
        Require(WaitFor(BudgetedFinished, Budgeted, std::chrono::seconds(10)), "the budgeted compute prebuilds did not finish");
        Require(awaitedFirst, "a compute prebuild a dispatch waits for did not run while budgeted prebuilds held every budgeted slot and others were queued");
        std::cout << "compute prebuild priority tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        Released.store(true);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
