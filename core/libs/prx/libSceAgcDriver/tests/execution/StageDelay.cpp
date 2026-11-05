#include "prx/libSceAgcDriver/Execution/include/Driver/Draw/StageDelay.hpp"
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace AgcDriver::DriverDetail;

namespace {

void Require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error(what);
}

std::uint64_t Delay(const StageDelays& delays, DelayedStage stage) {
    return delays[static_cast<std::size_t>(stage)];
}

void accepts() {
    const auto delays = ParseStageDelays("committer:1,worker:2,finisher:3,preparer:4,resolver:5");
    Require(Delay(delays, DelayedStage::Committer) == 1, "committer");
    Require(Delay(delays, DelayedStage::Worker) == 2, "worker");
    Require(Delay(delays, DelayedStage::Finisher) == 3, "finisher");
    Require(Delay(delays, DelayedStage::Preparer) == 4, "preparer");
    Require(Delay(delays, DelayedStage::Resolver) == 5, "resolver");
    Require(Delay(ParseStageDelays("worker:2,worker:7"), DelayedStage::Worker) == 7, "the last pair for a stage wins");
}

void skipsBadInput() {
    Require(ParseStageDelays("") == StageDelays{}, "empty text");
    for (const char* bad : {"committer", "committer:", ":5", "committer:x", "committer:5x", "committer:-5", "committer: 5", " committer:5", "Committer:5", "gpu:5", "committer:99999999999999999999", ",,,", "committer:5:6"}) {
        Require(ParseStageDelays(bad) == StageDelays{}, std::string("accepted ") + bad);
    }
    const auto delays = ParseStageDelays("gpu:9,worker:2,resolver:x,,preparer:4,");
    Require(Delay(delays, DelayedStage::Worker) == 2 && Delay(delays, DelayedStage::Preparer) == 4, "good pairs beside bad ones are kept");
    Require(Delay(delays, DelayedStage::Resolver) == 0 && Delay(delays, DelayedStage::Committer) == 0, "bad pairs set nothing");
}

void spins() {
    constexpr std::uint64_t ns = 2'000'000;
    const auto start = std::chrono::steady_clock::now();
    SpinStageDelay(ns);
    Require(std::chrono::steady_clock::now() - start >= std::chrono::nanoseconds(ns), "the wait lasts its nanoseconds");
}

}

int main() {
    try {
        accepts();
        skipsBadInput();
        spins();
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
    return 0;
}
